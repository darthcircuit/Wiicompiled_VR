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
 */
class QuestSurface(context: Context) : SDLSurface(context) {

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
}
