// Stereoscopic window bridge (BETA): the renderer's side of the host's RealityKit view.
//
// The host app asks for the bridge (petari_stereo_bridge_request). On the next frame the render worker
// creates a ring of kSlots 1920x1080 BGRA IOSurfaces per eye and imports each into Dawn as shared texture
// memory (SharedTextureMemoryIOSurface). A stereo frame (Frame::stereoEye, Game/System/Stereo.cpp) then
// draws its XFB with Aurora's copy pipeline into the eye's next slot; after the frame's queue submit the
// access ends and, once the GPU finished the work (Queue::OnSubmittedWorkDone), the slot is published for
// the host (petari_stereo_bridge_latest). The host copies the published slot into its RealityKit textures.
//
// If the shared texture memory cannot be imported the bridge fails (petari_stereo_bridge_error) and the
// game stays flat.
#include "present_aurora.hpp"

#include "webgpu/gpu.hpp"

#include <IOSurface/IOSurfaceRef.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

#include <petari/host_allocation.hpp>

#include "present.h"

namespace petari_present {
namespace {

constexpr uint32_t kWidth = 1920;
constexpr uint32_t kHeight = 1080;
constexpr int kSlots = 4;
constexpr int kEyes = 2;

struct Slot {
  IOSurfaceRef surface = nullptr;
  wgpu::SharedTextureMemory memory;
  wgpu::Texture texture;
  wgpu::TextureView view;
  // The size of the picture drawn into the slot (the top-left rectangle of the image), written before the slot is
  // published.
  std::atomic<uint32_t> width{kWidth};
  std::atomic<uint32_t> height{kHeight};
};

struct Eye {
  std::array<Slot, kSlots> slots;
  int next = 0;
  std::atomic<int> latest{-1};
  std::atomic<uint64_t> sequence{0};
};

std::array<Eye, kEyes> gEyes;
// GPU latency statistics: queue submit -> the GPU finished the frame (every eye frame's whole render + copy, behind
// whatever the GPU still had queued). Printed every 300 eye frames.
struct LatencyStats {
  std::atomic<uint64_t> totalUs{0}, maxUs{0}, count{0};
};
std::array<LatencyStats, 4> gLatency;  // index: stereoEye mode (1 left, 2 right, 3 mono)
void noteLatency(int mode, uint64_t us) {
  LatencyStats& stats = gLatency[mode];
  stats.totalUs.fetch_add(us, std::memory_order_relaxed);
  uint64_t max = stats.maxUs.load(std::memory_order_relaxed);
  while (us > max && !stats.maxUs.compare_exchange_weak(max, us, std::memory_order_relaxed)) {
  }
  if (stats.count.fetch_add(1, std::memory_order_relaxed) + 1 == 300) {
    const uint64_t total = stats.totalUs.exchange(0), peak = stats.maxUs.exchange(0);
    stats.count.store(0);
    std::fprintf(stderr, "Petari Stereo GPU: %s frames, submit to GPU done: mean %.2f ms, max %.1f ms (300 frames)\n",
                 mode == 1 ? "left-eye" : (mode == 2 ? "right-eye" : "mono"), static_cast<double>(total) / 300.0 / 1000.0,
                 static_cast<double>(peak) / 1000.0);
  }
}
std::atomic<bool> gRequested{false};
// Eye picture height in lines when the host upscales (MetalFX): the XFB is then drawn 1:1 into the top-left
// 16:9 rectangle of that height and the host scales it up to the full image. 0: fill the whole image.
std::atomic<int> gRectHeight{0};
std::atomic<int> gStatus{0};  // 0 not open / pending, 1 ready, -1 failed
std::string gError;           // written before gStatus becomes -1
bool gCreated = false;        // render worker only

void fail(const std::string& message) {
  gError = message;
  std::fprintf(stderr, "Petari Stereo: bridge failed: %s\n", message.c_str());
  gStatus.store(-1);
}

IOSurfaceRef createSurface() {
  auto number = [](int32_t value) { return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value); };
  const int32_t bgra = 0x42475241;  // 'BGRA' (kCVPixelFormatType_32BGRA)
  CFNumberRef width = number(kWidth), height = number(kHeight), bytes = number(4), format = number(bgra);
  const void* keys[] = {kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat};
  const void* values[] = {width, height, bytes, format};
  CFDictionaryRef properties = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 4, &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
  IOSurfaceRef surface = IOSurfaceCreate(properties);
  CFRelease(properties);
  CFRelease(width);
  CFRelease(height);
  CFRelease(bytes);
  CFRelease(format);
  return surface;
}

void destroy() {
  for (auto& eye : gEyes) {
    eye.latest.store(-1);
    for (auto& slot : eye.slots) {
      slot.view = {};
      slot.texture = {};
      slot.memory = {};
      if (slot.surface != nullptr) {
        CFRelease(slot.surface);
        slot.surface = nullptr;
      }
    }
    eye.next = 0;
  }
  gCreated = false;
}

bool create() {
  using namespace aurora::webgpu;
  for (int e = 0; e < kEyes; ++e) {
    for (int s = 0; s < kSlots; ++s) {
      Slot& slot = gEyes[e].slots[s];
      slot.surface = createSurface();
      if (slot.surface == nullptr) {
        fail("IOSurfaceCreate failed");
        return false;
      }
      wgpu::SharedTextureMemoryIOSurfaceDescriptor surfaceDescriptor;
      surfaceDescriptor.ioSurface = slot.surface;
      surfaceDescriptor.allowStorageBinding = false;
      wgpu::SharedTextureMemoryDescriptor descriptor{.nextInChain = &surfaceDescriptor, .label = "Petari stereo eye"};
      slot.memory = g_device.ImportSharedTextureMemory(&descriptor);
      if (!slot.memory) {
        fail("Dawn could not import the IOSurface (SharedTextureMemoryIOSurface unsupported?)");
        return false;
      }
      slot.texture = slot.memory.CreateTexture();
      if (!slot.texture) {
        fail("Dawn could not create a texture from the shared memory");
        return false;
      }
      if (slot.texture.GetFormat() != wgpu::TextureFormat::BGRA8Unorm ||
          !(slot.texture.GetUsage() & wgpu::TextureUsage::RenderAttachment) ||
          slot.texture.GetWidth() != kWidth || slot.texture.GetHeight() != kHeight) {
        fail("the imported eye texture has an unexpected format or usage");
        return false;
      }
      slot.view = slot.texture.CreateView();
    }
  }
  if (g_graphicsConfig.surfaceConfiguration.format != wgpu::TextureFormat::BGRA8Unorm) {
    fail("the copy pipeline's format is not BGRA8Unorm");
    return false;
  }
  gCreated = true;
  gStatus.store(1);
  std::fprintf(stderr, "Petari Stereo: bridge open (%dx%d, %d IOSurfaces per eye, Dawn shared texture memory)\n", kWidth,
               kHeight, kSlots);
  return true;
}

// Draws the frame into the eye's next slot and begins the shared access. Returns the slot used, or -1.
int encodeEye(const Frame& frame, const wgpu::CommandEncoder& encoder, int eye) {
  using namespace aurora::webgpu;
  Eye& state = gEyes[eye];
  const int index = state.next;
  state.next = (state.next + 1) % kSlots;
  Slot& slot = state.slots[index];
  // The picture's rectangle: the whole image, or (host upscaling) the top-left 16:9 rectangle of gRectHeight lines.
  uint32_t rectWidth = kWidth, rectHeight = kHeight;
  if (const int lines = gRectHeight.load(); lines > 0 && lines < static_cast<int>(kHeight)) {
    rectHeight = static_cast<uint32_t>(lines);
    rectWidth = std::min<uint32_t>(kWidth, (rectHeight * 16 + 4) / 9);
  }
  slot.width.store(rectWidth, std::memory_order_relaxed);
  slot.height.store(rectHeight, std::memory_order_relaxed);
  wgpu::SharedTextureMemoryBeginAccessDescriptor access{};
  access.initialized = true;
  access.concurrentRead = false;
  if (!slot.memory.BeginAccess(slot.texture, &access)) {
    std::fprintf(stderr, "Petari Stereo: BeginAccess failed (eye %d slot %d)\n", eye, index);
    return -1;
  }
  const wgpu::RenderPassColorAttachment attachment{
      .view = slot.view,
      .loadOp = wgpu::LoadOp::Clear,
      .storeOp = wgpu::StoreOp::Store,
      .clearValue = {0.0, 0.0, 0.0, 1.0},
  };
  const wgpu::RenderPassDescriptor passDescriptor{
      .label = "Petari stereo eye",
      .colorAttachmentCount = 1,
      .colorAttachments = &attachment,
  };
  const auto pass = encoder.BeginRenderPass(&passDescriptor);
  wgpu::BindGroup bindGroup;
  if (bind_image(frame, bindGroup)) {
    const auto viewport = calculate_present_viewport(rectWidth, rectHeight, frame.aspectWidth, frame.aspectHeight);
    pass.SetViewport(viewport.left, viewport.top, viewport.width, viewport.height, 0.f, 1.f);
    const auto x = static_cast<uint32_t>(viewport.left);
    const auto y = static_cast<uint32_t>(viewport.top);
    const auto w = std::min<uint32_t>(static_cast<uint32_t>(viewport.width), rectWidth - x);
    const auto h = std::min<uint32_t>(static_cast<uint32_t>(viewport.height), rectHeight - y);
    pass.SetScissorRect(x, y, w, h);
    pass.SetPipeline(g_CopyPipeline);
    pass.SetBindGroup(0, bindGroup, 0, nullptr);
    pass.Draw(3);
    if (frame.dimOpacity > 0.f) {
      draw_dim(pass, frame);
    }
  }
  pass.End();
  return index;
}

}  // namespace

std::function<void()> stereo_encode(const Frame& frame, const wgpu::CommandEncoder& encoder) noexcept {
  PetariNative::HostAllocationScope host;
  const bool requested = gRequested.load();
  if (!requested) {
    if (gCreated) {
      destroy();
      gStatus.store(0);
    }
    return {};
  }
  if (!gCreated) {
    if (gStatus.load() == -1) {
      return {};
    }
    if (!create()) {
      destroy();
      return {};
    }
  }
  if (frame.stereoEye == 0) {
    return {};
  }
  struct Pending {
    int eye;
    int slot;
  };
  std::array<Pending, kEyes> pending{};
  int count = 0;
  // 1: the left eye only, 2: the right eye only, 3: both (mono).
  const int from = frame.stereoEye == 2 ? 1 : 0;
  const int to = frame.stereoEye == 1 ? 0 : (frame.stereoEye == 2 ? 1 : 1);
  for (int eye = from; eye <= to; ++eye) {
    const int slot = encodeEye(frame, encoder, eye);
    if (slot >= 0) {
      pending[count++] = {eye, slot};
    }
  }
  if (count == 0) {
    return {};
  }
  const int mode = frame.stereoEye;
  return [pending, count, mode] {
    PetariNative::HostAllocationScope scope;
    using namespace aurora::webgpu;
    const auto submitted = std::chrono::steady_clock::now();
    for (int i = 0; i < count; ++i) {
      Slot& slot = gEyes[pending[i].eye].slots[pending[i].slot];
      wgpu::SharedTextureMemoryEndAccessState end{};
      slot.memory.EndAccess(slot.texture, &end);
    }
    // Published once the GPU finished this frame's work (the callbacks run in ProcessEvents).
    g_queue.OnSubmittedWorkDone(wgpu::CallbackMode::AllowSpontaneous,
                                [pending, count, mode, submitted](wgpu::QueueWorkDoneStatus status, wgpu::StringView) {
                                  if (status != wgpu::QueueWorkDoneStatus::Success) {
                                    return;
                                  }
                                  noteLatency(mode, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                                              std::chrono::steady_clock::now() - submitted)
                                                                              .count()));
                                  for (int i = 0; i < count; ++i) {
                                    Eye& eye = gEyes[pending[i].eye];
                                    eye.latest.store(pending[i].slot, std::memory_order_release);
                                    eye.sequence.fetch_add(1, std::memory_order_release);
                                  }
                                });
  };
}

}  // namespace petari_present

extern "C" {

void petari_stereo_bridge_request(int open) {
  petari_present::gRequested.store(open != 0);
  if (open != 0 && petari_present::gStatus.load() == -1) {
    // A new request retries after a failure (render worker resets on the next frame).
    petari_present::gStatus.store(0);
  }
}

int petari_stereo_bridge_status(void) {
  return petari_present::gStatus.load();
}

const char* petari_stereo_bridge_error(void) {
  return petari_present::gStatus.load() == -1 ? petari_present::gError.c_str() : "";
}

void* petari_stereo_bridge_surface(int eye, int slot) {
  if (eye < 0 || eye >= petari_present::kEyes || slot < 0 || slot >= petari_present::kSlots ||
      petari_present::gStatus.load() != 1) {
    return nullptr;
  }
  return petari_present::gEyes[eye].slots[slot].surface;
}

// Host upscaling: the eye pictures are drawn at `lines` (16:9) in the top-left of the eye images and the host scales
// them up; 0 draws them across the whole image. Each published slot reports its own size (slot_size).
void petari_stereo_bridge_set_rect_height(int lines) {
  petari_present::gRectHeight.store(lines);
}

// The size of the picture in the slot (the top-left rectangle of the image). False if there is no such slot.
int petari_stereo_bridge_slot_size(int eye, int slot, int* width, int* height) {
  if (eye < 0 || eye >= petari_present::kEyes || slot < 0 || slot >= petari_present::kSlots ||
      petari_present::gStatus.load() != 1) {
    return 0;
  }
  const auto& s = petari_present::gEyes[eye].slots[slot];
  if (width != nullptr) *width = static_cast<int>(s.width.load(std::memory_order_relaxed));
  if (height != nullptr) *height = static_cast<int>(s.height.load(std::memory_order_relaxed));
  return 1;
}

int petari_stereo_bridge_latest(int eye, uint64_t* sequence) {
  if (eye < 0 || eye >= petari_present::kEyes) {
    return -1;
  }
  auto& state = petari_present::gEyes[eye];
  // Sequence first: a slot published after this read is at least as new as the sequence.
  const uint64_t seq = state.sequence.load(std::memory_order_acquire);
  const int slot = state.latest.load(std::memory_order_acquire);
  if (sequence != nullptr) {
    *sequence = seq;
  }
  return slot;
}

}  // extern "C"
