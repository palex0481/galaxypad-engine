#pragma once
// Stereoscopic window (native only, BETA): the game is drawn once per eye each step.
//
// GameSystem::frameLoop draws the step twice from two cameras half the eye separation to the
// left and right of the game camera, each with an off-axis (sheared) projection so the
// convergence plane has no parallax; 2D layouts (HUD) are drawn identically in both eyes, at
// screen depth. The left frame is presented eye-only (not to the window); the right frame
// presents as usual and both frames are also rendered into the shared IOSurfaces the host app
// shows with RealityKit (native/gx/present/stereo_bridge.cpp).
//
// Like Smooth Motion, only the 3D view stage is re-run for the second eye (J3DModel::viewCalc);
// game state, animation and particles are those of the step. Smooth Motion is off while stereo
// is on. Whenever the view stage is not ready (loading, some scenes) the step is presented as
// a mono frame in both eyes.
#ifdef PETARI_NATIVE

namespace Stereo {
// Requested (host setter / PETARI_STEREO=1) and the host bridge is open.
bool active();
// GameSystem::frameLoop: a new game step begins.
void beginStep();
// GameScene::calcAnim ran its 3D view stage this step.
void noteViewStage();
// True if the step is drawn as two eye frames (else a mono frame). Called after calcAnim (previousStep false: this
// step's view stage must be ready) or, in the Wii order, before the update (previousStep true: the previous step's).
bool beginEyes(bool previousStep = false);
// True while the left eye is being drawn: the token-time EFB captures (star pointer, lens flare, talk Z peek) are
// skipped there; the right eye's are the step's result either way, and each costs a render pass split and a GPU round trip.
bool skipDrawSync();
// Sets the camera for the eye (0 left, 1 right) and re-runs the 3D view stage.
void beginEye(int eye);
// After the right eye was drawn: restores the step's own camera.
void endEyes();
// Presents the left eye frame (eye-only) and opens the next one.
void presentLeftEye();

// Per-phase wall-clock timing of a stereo step (printed with the step statistics every 600 steps).
enum Phase { PhaseUpdate, PhaseCalcAnim, PhaseDrawLeft, PhasePresentLeft, PhaseDrawRight, PhaseEndFrame, PhaseSeam, PhaseRetrace, PhaseCount };
// Adds the time since the previous mark (or the step's start) to the phase.
void mark(Phase phase);
}  // namespace Stereo

#endif
