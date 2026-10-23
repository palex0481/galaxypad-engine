// Stereoscopic window: see Game/System/Stereo.hpp.
#ifdef PETARI_NATIVE

#include "Game/System/Stereo.hpp"
#include "Game/Camera/CameraContext.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include <petari/app.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

// native/gx/present/stereo_bridge.cpp (Aurora side).
extern "C" void petari_present_set_stereo(int mode);  // 0 off, 1 left eye only, 2 right eye (presents), 3 mono
extern "C" int petari_stereo_bridge_status(void);
extern "C" unsigned long long petari_gx_processor_busy_total(void);  // native/gx/sync_backend.cpp: FIFO processor thread busy ns     // 1 once the host's IOSurfaces are imported

namespace {

// Eye separation as a fraction of the convergence distance at depth 1.0: about the ratio of a
// 65 mm eye separation to a 2.5 m viewing distance, which suits a plane seen at a couple of metres.
constexpr float kSeparationRatio = 0.026f;

std::atomic< bool > gRequested{false};
std::atomic< float > gDepth{1.0f};
std::atomic< float > gConvergence{1200.0f};
std::atomic< bool > gEnvChecked{false};
// Eye render height in lines (the width follows at 16:9): the game renders at height / 456 times its own
// 640x456 framebuffer, which is what costs GPU time twice per step. 540 / 720 / 1080 are Low / Medium / High.
constexpr int kGameFramebufferHeight = 456;
std::atomic< int > gRenderHeight{720};
int gAppliedHeight = 0;

bool gOn = false;       // this step's state
bool gViewStage = false;
bool gViewStagePrev = false;  // the previous step's, for the Wii order (draw first, then update)
bool gLeftEye = false;
bool gEyes = false;     // between beginEyes and endEyes
TPos3f gSavedView, gSavedInv;
TProj3f gSavedProjection;
CameraContext* gContext = nullptr;
unsigned long gSteps = 0, gEyeSteps = 0, gMonoSteps = 0;
using Clock = std::chrono::steady_clock;
Clock::time_point gLastMark;
double gPhaseMs[Stereo::PhaseCount] = {};
Clock::time_point gStepStart;
double gStepMs = 0.0, gStepMsMax = 0.0;
unsigned long long gBusyAtReset = 0;
unsigned long gSlowSteps = 0;  // steps over 16.7 ms

void checkEnv() {
    if (!gEnvChecked.exchange(true)) {
        const char* value = std::getenv("PETARI_STEREO");
        if (value != nullptr && value[0] == '1') {
            gRequested.store(true);
        }
        const char* depth = std::getenv("PETARI_STEREO_DEPTH");
        if (depth != nullptr) {
            gDepth.store(static_cast< float >(std::atof(depth)));
        }
        const char* height = std::getenv("PETARI_STEREO_HEIGHT");
        if (height != nullptr && std::atoi(height) > 0) {
            gRenderHeight.store(std::atoi(height));
        }
        const char* convergence = std::getenv("PETARI_STEREO_CONVERGENCE");
        if (convergence != nullptr) {
            gConvergence.store(static_cast< float >(std::atof(convergence)));
        }
    }
}

void reView() {
    MR::loadViewMtx();
    CategoryList::entryDrawBuffer3D();
}

}  // namespace

// Host setter (native/app/engine_entry.mm).
extern "C" void petari_stereo_configure(int enabled, float depth, float convergence) {
    gEnvChecked.store(true);
    gDepth.store(depth);
    if (convergence > 1.0f) {
        gConvergence.store(convergence);
    }
    gRequested.store(enabled != 0);
}

// Host setter: the eye render height in lines (clamped to 360...1080), applied live.
extern "C" void petari_stereo_set_resolution(int height) {
    gEnvChecked.store(true);
    gRenderHeight.store(height < 360 ? 360 : (height > 1080 ? 1080 : height));
}

namespace Stereo {

bool active() {
    checkEnv();
    return gRequested.load() && petari_stereo_bridge_status() == 1;
}

void beginStep() {
    const bool on = active();
    if (on != gOn) {
        gOn = on;
        gAppliedHeight = 0;
        if (!on) {
            petari_host_smooth_motion(false);  // back to the screen's pixels
        }
        petari_present_set_stereo(on ? 3 : 0);
        std::fprintf(stderr, "Petari Stereo: %s\n", on ? "on (each step is drawn once per eye)" : "off");
        gSteps = gEyeSteps = gMonoSteps = 0;
    }
    if (!on) {
        return;
    }
    gViewStagePrev = gViewStage;
    gViewStage = false;
    {
        const Clock::time_point now = Clock::now();
        if (gSteps > 0) {
            const double ms = std::chrono::duration< double, std::milli >(now - gStepStart).count();
            gStepMs += ms;
            gStepMsMax = ms > gStepMsMax ? ms : gStepMsMax;
            gSlowSteps += ms > 16.7 ? 1 : 0;
        }
        gStepStart = gLastMark = now;
    }
    petari_present_set_stereo(3);
    // The game renders at the eye resolution (not the 1920x1080 eye images' 3x of the game framebuffer it used
    // to, 2432x1368 per eye); the bridge scales the picture to the eye images. Applied when it changes.
    if (const int height = gRenderHeight.load(); height != gAppliedHeight) {
        gAppliedHeight = height;
        petari_host_set_render_scale(static_cast< float >(height) / static_cast< float >(kGameFramebufferHeight));
        std::fprintf(stderr, "Petari Stereo: eye render size %dx%d\n", (height * 16 + 4) / 9, height);
    }
    if (++gSteps == 600) {
        std::fprintf(stderr, "Petari Stereo: %lu steps, %lu with two eyes, %lu mono\n", gSteps, gEyeSteps, gMonoSteps);
        const double n = static_cast< double >(gSteps);
        std::fprintf(stderr,
                     "Petari Stereo timing (ms per step, mean of %lu): step %.2f (max %.1f, %lu over 16.7) = update %.2f + calcAnim %.2f + "
                     "draw left %.2f + present left %.2f + draw right %.2f + end frame %.2f + seam %.2f + retrace wait %.2f\n",
                     gSteps, gStepMs / n, gStepMsMax, gSlowSteps, gPhaseMs[PhaseUpdate] / n, gPhaseMs[PhaseCalcAnim] / n,
                     gPhaseMs[PhaseDrawLeft] / n, gPhaseMs[PhasePresentLeft] / n, gPhaseMs[PhaseDrawRight] / n,
                     gPhaseMs[PhaseEndFrame] / n, gPhaseMs[PhaseSeam] / n, gPhaseMs[PhaseRetrace] / n);
        const unsigned long long busy = petari_gx_processor_busy_total();
        std::fprintf(stderr, "Petari Stereo GX processor thread: %.2f ms busy per step (decoding GX commands into draws; runs alongside the game thread)\n",
                     static_cast< double >(busy - gBusyAtReset) / 1e6 / n);
        gBusyAtReset = busy;
        for (double& ms : gPhaseMs) {
            ms = 0.0;
        }
        gStepMs = gStepMsMax = 0.0;
        gSlowSteps = 0;
        gSteps = gEyeSteps = gMonoSteps = 0;
    }
}

void noteViewStage() {
    gViewStage = true;
}

bool beginEyes(bool previousStep) {
    if (!gOn || !(previousStep ? gViewStagePrev : gViewStage) || !MR::isExistSceneObj(SceneObj_CameraContext)) {
        if (gOn) {
            gMonoSteps++;
        }
        return false;
    }
    gContext = MR::getSceneObj< CameraContext >(SceneObj_CameraContext);
    gSavedView = gContext->mView;
    gSavedInv = gContext->mViewInv;
    gSavedProjection = gContext->mProjection;
    gEyes = true;
    gEyeSteps++;
    return true;
}

void beginEye(int eye) {
    if (!gEyes) {
        return;
    }
    const float convergence = gConvergence.load();
    // Full separation between the eyes; each eye is half of it from the game camera.
    const float half = 0.5f * kSeparationRatio * gDepth.load() * convergence;
    const float offset = eye == 0 ? -half : half;  // the eye's position along the camera's x axis
    gContext->mView = gSavedView;
    // View space: the eye moves by +offset along x, so the world moves by -offset.
    gContext->mView.mMtx[0][3] -= offset;
    PSMTXInverse(gContext->mView.mMtx, gContext->mViewInv.mMtx);
    gContext->mProjection = gSavedProjection;
    // Off-axis: the shear puts the convergence plane's points back where the centered camera saw them.
    gContext->mProjection.mMtx[0][2] -= gContext->mProjection.mMtx[0][0] * offset / convergence;
    reView();
    petari_present_set_stereo(eye == 0 ? 1 : 2);
    gLeftEye = eye == 0;
}

bool skipDrawSync() {
    return gEyes && gLeftEye;
}

void endEyes() {
    if (!gEyes) {
        return;
    }
    gContext->mView = gSavedView;
    gContext->mViewInv = gSavedInv;
    gContext->mProjection = gSavedProjection;
    gEyes = false;
    gLeftEye = false;
    // The next step's calcAnim re-runs the 3D view stage; only the view matrix the game reads is restored.
    MR::loadViewMtx();
}

void mark(Phase phase) {
    if (!gOn) {
        return;
    }
    const Clock::time_point now = Clock::now();
    gPhaseMs[phase] += std::chrono::duration< double, std::milli >(now - gLastMark).count();
    gLastMark = now;
}

void presentLeftEye() {
    petari_host_present_between();
}

}  // namespace Stereo

#endif
