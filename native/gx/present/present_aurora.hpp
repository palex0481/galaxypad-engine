#pragma once
#include <functional>
// Aurora side of XFB presentation, used by the patched aurora_end_frame
// (patch_aurora_present.py). Included with Aurora's lib/ on the quote path.

#include "gfx/texture.hpp"
#include <webgpu/webgpu_cpp.h>

#include <cstdint>

namespace petari_present {

struct Frame {
    bool active = false;           // presentation enabled: show the XFB, not the EFB
    bool black = true;             // nothing to show: VI black, no render mode, or no copy for the XFB
    float dimOpacity = 0.0f;       // black drawn over the image while dimmed
    uint32_t aspectWidth = 4;      // display aspect for the present viewport
    uint32_t aspectHeight = 3;
    aurora::gfx::TextureHandle xfb;  // the display copy VI latched; kept alive until presented
    // Stereoscopic window (stereo_bridge.cpp): 0 flat, 1 left eye (rendered to the eye image only; the
    // window is not presented), 2 right eye (presented, and rendered to the eye image), 3 mono (both eyes).
    int stereoEye = 0;
};

// Main thread, in aurora_end_frame after the FIFO is drained (the command
// processor is idle, so its copy textures can be read).
Frame take() noexcept;

// Render worker: the bind group drawing the XFB with Aurora's copy pipeline.
// False if nothing is drawn (black).
bool bind_image(const Frame& frame, wgpu::BindGroup& out) noexcept;

// Opt-in PETARI_SURFACE_DUMP=<dir> (present_aurora.cpp): copies the final window image (the presented
// frame plus the Dear ImGui overlay: Home menu, badges) to a PNG. Render worker only. prepare() makes the
// surface a copy source once; encode() returns the work to run after the frame was submitted, or an empty
// function when this frame is not dumped.
void surface_dump_prepare() noexcept;
std::function<void()> surface_dump_encode(const wgpu::CommandEncoder& encoder, const wgpu::Texture& texture) noexcept;

// Render worker, every frame: creates the eye images when the host asked for them, and for a stereo frame
// (Frame::stereoEye) encodes the XFB into the eye image(s). Returns the work to run after the frame was
// submitted (ends the shared-texture access, publishes the images), or an empty function.
std::function<void()> stereo_encode(const Frame& frame, const wgpu::CommandEncoder& encoder) noexcept;

// Render worker: darkens the image drawn in the pass.
void draw_dim(const wgpu::RenderPassEncoder& pass, const Frame& frame) noexcept;

}  // namespace petari_present
