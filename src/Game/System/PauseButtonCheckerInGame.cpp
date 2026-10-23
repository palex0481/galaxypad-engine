#include "Game/System/PauseButtonCheckerInGame.hpp"
#include "Game/Util/GamePadUtil.hpp"
#ifdef PETARI_NATIVE
#include <chrono>
#include <cstdio>
#endif

namespace {
    static const s32 sHoldCounterMax = 12;
#ifdef PETARI_NATIVE
    // The game waits for Plus / Minus to be held for 12 steps: 0.2 s at the Wii's 60 steps per second. The
    // Stereoscopic window runs far fewer steps per second (each step is drawn twice), where 12 steps is half a
    // second or more, so a normal press never opened the pause menu. The hold also counts as complete after the
    // same 0.2 s of real time (the count still reaches 12 exactly once, so the menu opens once).
    constexpr std::chrono::milliseconds kHoldTime{200};
    using Clock = std::chrono::steady_clock;
    Clock::time_point sPlusSince, sMinusSince;
    void trackHold(bool down, s32& frames, Clock::time_point& since, const char* name) {
        if (!down) {
            frames = 0;
            since = Clock::time_point{};
            return;
        }
        const Clock::time_point now = Clock::now();
        if (frames == 0) {
            since = now;
        }
        frames++;
        if (frames < sHoldCounterMax && now - since >= kHoldTime) {
            std::fprintf(stderr, "Petari pause: %s held %lld ms (%d steps, slow frame rate): counted as held\n", name,
                         static_cast< long long >(std::chrono::duration_cast< std::chrono::milliseconds >(now - since).count()),
                         static_cast< int >(frames));
            frames = sHoldCounterMax;
        }
    }
#endif
};  // namespace

PauseButtonCheckerInGame::PauseButtonCheckerInGame() : mPlusHoldFrame(0), mMinusHoldFrame(0) {
}

void PauseButtonCheckerInGame::update() {
#ifdef PETARI_NATIVE
    trackHold(MR::testCorePadButtonPlus(WPAD_CHAN0), mPlusHoldFrame, sPlusSince, "Plus");
    trackHold(MR::testCorePadButtonMinus(WPAD_CHAN0), mMinusHoldFrame, sMinusSince, "Minus");
    return;
#endif
    if (MR::testCorePadButtonPlus(WPAD_CHAN0)) {
        mPlusHoldFrame++;
    } else {
        mPlusHoldFrame = 0;
    }

    if (MR::testCorePadButtonMinus(WPAD_CHAN0)) {
        mMinusHoldFrame++;
    } else {
        mMinusHoldFrame = 0;
    }
}

bool PauseButtonCheckerInGame::isPermitToPlusPause() const {
    return isPermitToPause() && mPlusHoldFrame == ::sHoldCounterMax;
}

bool PauseButtonCheckerInGame::isPermitToMinusPause() const {
    return isPermitToPause() && mMinusHoldFrame == ::sHoldCounterMax;
}

bool PauseButtonCheckerInGame::isPermitToPause() const {
    return !MR::isOperatingWPad(WPAD_CHAN0);
}
