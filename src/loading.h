// ============================================================================
//  loading.h — level loading screen
// ----------------------------------------------------------------------------
//  While the world loads (game_load_world), the screen shows the img/loading.png
//  background (cover over the whole screen) and a progress bar with a caption at
//  the bottom right. Loading is synchronous, so the bars are drawn and PRESENTED
//  right between loading phases (the main loop is not running at that time).
// ============================================================================
#ifndef LOADING_H
#define LOADING_H

// Screen setup: background texture and quad (requires a live GL context).
void loading_begin(void);

// Draw progress t [0..1] with a caption and present the frame to the window.
void loading_progress(float t, const char *label);

// Free the loading screen's resources.
void loading_end(void);

#endif
