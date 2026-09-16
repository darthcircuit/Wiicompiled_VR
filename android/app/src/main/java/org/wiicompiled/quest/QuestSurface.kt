package org.wiicompiled.quest

import android.content.Context
import android.view.SurfaceHolder
import org.libsdl.app.SDLSurface

/**
 * Stock SDLSurface plus the two calls Aurora needs around every surface change.
 *
 * Aurora only presents while the surface is known to be usable and must never
 * touch an ANativeWindow that SDL is replacing or destroying. SDL keeps its own
 * activity mutex internal to libSDL3.so, so this subclass brackets SDL's
 * handling instead: begin takes Aurora's surface lock and pauses presentation,
 * end releases it and reports whether SDL left the surface ready.
 *
 * The surface buffer is also pinned to [BUFFER_WIDTH] x [BUFFER_HEIGHT]. An
 * immersive app's Android surface is never shown in the headset, yet SDL sizes
 * it to the whole display (4128x2208 on a Quest 3). Aurora sizes its
 * presentation snapshot from it, and in menus that snapshot is the image the
 * virtual screen shows in each eye. The screen spans about 900 eye pixels at
 * the default HUD size, so 1280x720 keeps menus sharp at a small fraction of
 * the cost.
 */
class QuestSurface(context: Context) : SDLSurface(context) {

    init {
        holder.setFixedSize(BUFFER_WIDTH, BUFFER_HEIGHT)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        nativeBeginSurfaceMutation()
        try {
            super.surfaceChanged(holder, format, width, height)
        } finally {
            nativeEndSurfaceMutation(mIsSurfaceReady)
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        nativeBeginSurfaceMutation()
        try {
            super.surfaceDestroyed(holder)
        } finally {
            nativeEndSurfaceMutation(false)
        }
    }

    private external fun nativeBeginSurfaceMutation()
    private external fun nativeEndSurfaceMutation(ready: Boolean)

    private companion object {
        const val BUFFER_WIDTH = 1280
        const val BUFFER_HEIGHT = 720
    }
}
