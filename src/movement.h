// ============================================================================
//  movement.h — player physics/movement (separate from the camera)
// ----------------------------------------------------------------------------
//  camera.c is responsible for the VIEW (yaw/pitch, mouse, matrices), while
//  ALL physics and movement live here: walking (gravity, ground, object
//  collision, water), flying (fly) and phase-through (noclip), plus
//  footstep/jump/landing sounds.
//  This way movement isn't baked into the camera and can be reused/replaced.
// ============================================================================
#ifndef MOVEMENT_H
#define MOVEMENT_H

#include "camera.h"   // Camera
#include "world.h"    // WorldState

// Counter of played footsteps (for snd_debug in renderer.c)
extern int g_step_calls;

// Player physics/movement for a dt tick. Reads actions (actions.h: +forward
// etc.), moves cam->pos / vx / vy / vz, honors cam->fly/noclip modes.
void movement_update(Camera *cam, const WorldState *w, float dt);

// Fall speed (m/s, >0) at the moment of the last landing from a height —
// one-shot: returns and resets. The game layer (game.c) converts it into
// fall damage. 0 — no landing from a noticeable height this frame.
float movement_take_fall_impact(void);

#endif