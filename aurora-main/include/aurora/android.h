#ifndef AURORA_ANDROID_H
#define AURORA_ANDROID_H

#ifdef __cplusplus
extern "C" {
#else
#include "stdbool.h"
#endif

/*
 * Android surface lifecycle bridge.
 *
 * Aurora only presents while the Android surface is known to be usable, and
 * must never touch an ANativeWindow the Java side is tearing down. Stock SDL3
 * keeps its own activity mutex internal to libSDL3.so, so the host app brackets
 * every surface change instead: an SDLSurface subclass calls begin before
 * delegating surfaceChanged/surfaceDestroyed to SDL and end afterwards, with
 * `ready` saying whether SDL left the surface usable. Between the two calls
 * Aurora's surface lock is held and presentation is paused.
 *
 * Call from the Java UI thread only. The pair must always balance.
 */
void aurora_android_begin_surface_mutation(void);
void aurora_android_end_surface_mutation(bool ready);

#ifdef __cplusplus
}
#endif

#endif
