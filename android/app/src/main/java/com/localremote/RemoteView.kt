package com.localremote

import android.annotation.SuppressLint
import android.content.Context
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import java.nio.ByteBuffer
import java.nio.ByteOrder

class RemoteView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : SurfaceView(context, attrs), SurfaceHolder.Callback {
    private var mediaClient: MediaClient? = null
    private var seqNum = 0
    private var lastX = 0.5f
    private var lastY = 0.5f

    init {
        holder.addCallback(this)
        isFocusable = true
    }

    fun setMediaClient(client: MediaClient?) {
        mediaClient = client
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        mediaClient?.setRenderSurface(holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) = Unit

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        mediaClient?.setRenderSurface(null)
    }

    fun sendRightClick() {
        sendMouse(0, 1, lastX, lastY)
        sendMouse(1, 1, lastX, lastY)
    }

    fun sendScroll(direction: Int) {
        val client = mediaClient ?: return
        if (!client.isConnected) return
        client.sendInput(inputEvent(2, 2, 0, lastX, lastY, 0, direction.coerceIn(-1, 1)))
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        val client = mediaClient ?: return false
        if (!client.isConnected || width <= 0 || height <= 0) return false

        val action = when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> 0
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> 1
            MotionEvent.ACTION_MOVE -> 2
            else -> return false
        }
        lastX = (event.x / width.toFloat()).coerceIn(0f, 1f)
        lastY = (event.y / height.toFloat()).coerceIn(0f, 1f)
        client.sendInput(inputEvent(0, action, 0, lastX, lastY, 0, 0))
        if (event.actionMasked == MotionEvent.ACTION_UP) performClick()
        return true
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }

    private fun sendMouse(action: Int, button: Int, x: Float, y: Float) {
        val client = mediaClient ?: return
        if (!client.isConnected) return
        client.sendInput(inputEvent(0, action, button, x, y, 0, 0))
    }

    private fun inputEvent(
        eventType: Int,
        action: Int,
        button: Int,
        x: Float,
        y: Float,
        scrollX: Int,
        scrollY: Int
    ): ByteArray {
        return ByteBuffer.allocate(40).order(ByteOrder.LITTLE_ENDIAN)
            .putLong(System.currentTimeMillis() * 1000)
            .putInt(seqNum++)
            .put(eventType.toByte())
            .putFloat(x.coerceIn(0f, 1f))
            .putFloat(y.coerceIn(0f, 1f))
            .putInt(width)
            .putInt(height)
            .put(action.toByte())
            .put(button.toByte())
            .putShort(scrollX.toShort())
            .putShort(scrollY.toShort())
            .putInt(0)
            .put(0)
            .array()
    }
}
