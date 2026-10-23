#include <revolution/gx.h>
#include <revolution/os.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include "sync_backend.h"

extern "C" bool petari_gx_read_snapshot(uint64_t ticket, uint16_t x, uint16_t y,
                                       uint32_t* depth, uint32_t* argb);
extern "C" bool petari_gx_read_snapshot_clamped(uint64_t ticket, uint16_t x, uint16_t y,
                                               uint32_t* depth, uint32_t* argb);
namespace {
std::atomic<GXAlphaReadMode> alphaRead{GX_READ_FF};
std::atomic<unsigned long long> badPeeks{0};
std::atomic<long long> lastBadPeekLogNs{0};

// A peek the capture cannot answer exactly. The game's bright-object test can ask for the row just
// past the bottom edge (y == height) when a sample lands exactly on the screen edge, and a missing
// capture is possible too. Real hardware never aborts here, so answer instead of panicking:
// an out-of-range pixel takes the nearest edge pixel; no capture at all reads as "not bright"
// (alpha 0, far depth). Counted, and logged at most once every 5 s.
void badPeek(u16 x, u16 y, unsigned long long ticket, const char* answer) {
    const auto count = badPeeks.fetch_add(1, std::memory_order_relaxed) + 1;
    const long long now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    long long last = lastBadPeekLogNs.load(std::memory_order_relaxed);
    if ((count == 1 || now - last >= 5000000000LL) &&
        lastBadPeekLogNs.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        std::fprintf(stderr, "Petari peek warning: GX EFB peek at (%u,%u) outside the token-time capture or none "
                     "(ticket %llu); answered with %s (%llu such peeks so far)\n",
                     x, y, ticket, answer, count);
        std::fflush(stderr);
    }
}

void pixel(u16 x, u16 y, u32& depth, u32& color) {
    const auto ticket = petari_gx_sync_delivering_ticket();
    if (ticket && petari_gx_read_snapshot(ticket, x, y, &depth, &color)) return;
    if (ticket && petari_gx_read_snapshot_clamped(ticket, x, y, &depth, &color)) {
        badPeek(x, y, static_cast<unsigned long long>(ticket), "the nearest edge pixel");
        return;
    }
    depth = 0xffffff;
    color = 0;
    badPeek(x, y, static_cast<unsigned long long>(ticket), "not bright (no capture)");
}
}
void GXPeekZ(u16 x, u16 y, u32* value) {
    u32 color;
    if (!value) OSPanic(__FILE__, __LINE__, "GXPeekZ: null output");
    pixel(x, y, *value, color);
}
void GXPeekARGB(u16 x, u16 y, u32* value) {
    u32 depth;
    if (!value) OSPanic(__FILE__, __LINE__, "GXPeekARGB: null output");
    pixel(x, y, depth, *value);
    switch (alphaRead.load(std::memory_order_relaxed)) {
    case GX_READ_00: *value &= 0x00ffffff; break;
    case GX_READ_FF: *value |= 0xff000000; break;
    case GX_READ_NONE: break;
    }
}
void GXPokeAlphaRead(GXAlphaReadMode mode) {
    if (mode != GX_READ_00 && mode != GX_READ_FF && mode != GX_READ_NONE)
        OSPanic(__FILE__, __LINE__, "GXPokeAlphaRead: invalid mode");
    alphaRead.store(mode, std::memory_order_relaxed);
}
