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
#include <cstdio>
#include <cstdlib>

// native/gx/present/stereo_bridge.cpp (Aurora side).
extern "C" void petari_present_set_stereo(int mode);  // 0 off, 1 left eye only, 2 right eye (presents), 3 mono
extern "C" int petari_stereo_bridge_status(void);     // 1 once the host's IOSurfaces are imported

namespace {

// Eye separation as a fraction of the convergence distance at depth 1.0: about the ratio of a
// 65 mm eye separation to a 2.5 m viewing distance, which suits a plane seen at a couple of metres.
constexpr float kSeparationRatio = 0.026f;

std::atomic< bool > gRequested{false};
std::atomic< float > gDepth{1.0f};
std::atomic< float > gConvergence{1200.0f};
std::atomic< bool > gEnvChecked{false};

bool gOn = false;       // this step's state
bool gViewStage = false;
bool gEyes = false;     // between beginEyes and endEyes
TPos3f gSavedView, gSavedInv;
TProj3f gSavedProjection;
CameraContext* gContext = nullptr;
unsigned long gSteps = 0, gEyeSteps = 0, gMonoSteps = 0;

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

namespace Stereo {

bool active() {
    checkEnv();
    return gRequested.load() && petari_stereo_bridge_status() == 1;
}

void beginStep() {
    const bool on = active();
    if (on != gOn) {
        gOn = on;
        // The eye images are 1920x1080: render at 3x the game framebuffer (unless PETARI_RENDER_SCALE chose).
        petari_host_smooth_motion(on);
        petari_present_set_stereo(on ? 3 : 0);
        std::fprintf(stderr, "Petari Stereo: %s\n", on ? "on (each step is drawn once per eye)" : "off");
        gSteps = gEyeSteps = gMonoSteps = 0;
    }
    if (!on) {
        return;
    }
    gViewStage = false;
    petari_present_set_stereo(3);
    if (++gSteps == 600) {
        std::fprintf(stderr, "Petari Stereo: %lu steps, %lu with two eyes, %lu mono\n", gSteps, gEyeSteps, gMonoSteps);
        gSteps = gEyeSteps = gMonoSteps = 0;
    }
}

void noteViewStage() {
    gViewStage = true;
}

bool beginEyes() {
    if (!gOn || !gViewStage || !MR::isExistSceneObj(SceneObj_CameraContext)) {
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
}

void endEyes() {
    if (!gEyes) {
        return;
    }
    gContext->mView = gSavedView;
    gContext->mViewInv = gSavedInv;
    gContext->mProjection = gSavedProjection;
    gEyes = false;
    // The next step's calcAnim re-runs the 3D view stage; only the view matrix the game reads is restored.
    MR::loadViewMtx();
}

void presentLeftEye() {
    petari_host_present_between();
}

}  // namespace Stereo

#endif
