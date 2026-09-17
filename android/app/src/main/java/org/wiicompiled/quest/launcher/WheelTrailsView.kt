package org.wiicompiled.quest.launcher

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.util.AttributeSet
import android.view.View
import android.view.animation.AccelerateInterpolator
import android.view.animation.DecelerateInterpolator
import org.wiicompiled.quest.R

/**
 * The Home page's backdrop: WheelWizard's wheel trails, broad diagonal bands
 * that each end in a wheel. They roll in when the page opens and roll away
 * when Play is pressed.
 *
 * A trail is anchored at its wheel and its band runs from there off the edge.
 * Rolling moves the wheel along the band, so an entering or leaving trail
 * always comes from or goes to the edge it is attached to.
 */
class WheelTrailsView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : View(context, attrs) {

    private class Trail(
        val fromRight: Boolean,
        val xDp: Float,
        val fromBottom: Boolean,
        val yDp: Float,
        /** Clockwise from straight down, the direction the band runs. */
        val angle: Float,
        val band: Int,
        val wheel: Int,
    )

    private val trails = listOf(
        Trail(false, 30f, false, -20f, 45f, R.color.primary_400, R.color.primary_700),
        Trail(false, 210f, false, 10f, 45f, R.color.primary_600, R.color.primary_800),
        Trail(false, 230f, false, 210f, 45f, R.color.primary_200, R.color.primary_600),
        Trail(true, 230f, true, 60f, -35f, R.color.primary_400, R.color.primary_700),
        Trail(true, 40f, true, 10f, -35f, R.color.primary_600, R.color.primary_800),
    )

    private val density = resources.displayMetrics.density
    private val bandWidth = 137f * density
    private val bandLength = 3000f * density
    private val bandPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val wheelPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val rect = RectF()

    /** How far every wheel sits from its anchor along its band, in pixels. */
    private var offset = 0f
    private var rotationDegrees = 0f
    private var animator: ValueAnimator? = null

    /** Rolls the trails in from the edges. */
    fun enter() {
        animate(from = 320f * density, to = 0f, spin = 560f, durationMs = 650L, leaving = false, done = null)
    }

    /** Rolls the trails off the edges, then runs [done]. */
    fun leave(done: () -> Unit) {
        animate(from = offset, to = 1400f * density, spin = 360f, durationMs = 450L, leaving = true, done = done)
    }

    private fun animate(from: Float, to: Float, spin: Float, durationMs: Long, leaving: Boolean, done: (() -> Unit)?) {
        animator?.cancel()
        val startRotation = if (leaving) rotationDegrees else -200f
        animator = ValueAnimator.ofFloat(0f, 1f).apply {
            duration = durationMs
            interpolator = if (leaving) AccelerateInterpolator(1.6f) else DecelerateInterpolator(2f)
            addUpdateListener {
                val t = it.animatedValue as Float
                offset = from + (to - from) * t
                rotationDegrees = startRotation + spin * t
                invalidate()
            }
            if (done != null) {
                addListener(object : android.animation.AnimatorListenerAdapter() {
                    private var cancelled = false
                    override fun onAnimationCancel(animation: android.animation.Animator) {
                        cancelled = true
                    }

                    override fun onAnimationEnd(animation: android.animation.Animator) {
                        if (!cancelled) done()
                    }
                })
            }
            start()
        }
    }

    override fun onDetachedFromWindow() {
        animator?.cancel()
        super.onDetachedFromWindow()
    }

    override fun onDraw(canvas: Canvas) {
        for (trail in trails) {
            val x = if (trail.fromRight) width - trail.xDp * density else trail.xDp * density
            val y = if (trail.fromBottom) height - trail.yDp * density else trail.yDp * density
            canvas.save()
            canvas.translate(x, y)
            canvas.rotate(trail.angle)
            canvas.translate(0f, offset)
            drawTrail(canvas, trail)
            canvas.restore()
        }
    }

    private fun drawTrail(canvas: Canvas, trail: Trail) {
        val half = bandWidth / 2f
        bandPaint.color = context.getColor(trail.band)
        wheelPaint.color = context.getColor(trail.wheel)

        rect.set(-half, -half, half, bandLength)
        canvas.drawRoundRect(rect, half, half, bandPaint)

        // Tyre, rim and hub, the same wheel as ic_wheel at band scale.
        canvas.rotate(rotationDegrees)
        val unit = bandWidth / 24f * 0.85f
        canvas.drawCircle(0f, 0f, 11f * unit, wheelPaint)
        canvas.drawCircle(0f, 0f, 8f * unit, bandPaint)
        canvas.drawCircle(0f, 0f, 6.5f * unit, wheelPaint)
        for (spoke in 0 until 5) {
            val radians = Math.toRadians(-90.0 + 72.0 * spoke)
            canvas.drawCircle(
                (3.9f * unit * Math.cos(radians)).toFloat(),
                (3.9f * unit * Math.sin(radians)).toFloat(),
                1.45f * unit,
                bandPaint,
            )
        }
        canvas.drawCircle(0f, 0f, 0.9f * unit, bandPaint)
    }
}
