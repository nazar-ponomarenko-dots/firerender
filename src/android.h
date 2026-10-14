// ============================================================================
//  android.h — environment setup for Android (see android.c)
// ----------------------------------------------------------------------------
//  On Android resources live inside the APK and fopen() cannot see them. At
//  startup we unpack them into the app's private storage (SDL_GetPrefPath) and
//  chdir there — after that the whole engine works as usual: relative paths,
//  config.cfg nearby, miniz map packs.
//  On other platforms — a no-op.
// ============================================================================
#ifndef ANDROID_H
#define ANDROID_H

// Unpack the APK assets into storage and make it the working folder.
// Called once at the start of main(), before base_dir is determined.
// Returns 1 if the environment was prepared (Android), 0 — regular platform.
int fr_android_prepare(void);

#endif
