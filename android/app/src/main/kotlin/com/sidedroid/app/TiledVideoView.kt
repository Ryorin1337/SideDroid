package com.sidedroid.app

import android.content.Context
import android.graphics.SurfaceTexture
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.opengl.GLSurfaceView
import android.view.Surface
import android.view.SurfaceHolder
import com.sidedroid.design.tileSize
import java.nio.ByteBuffer
import java.nio.ByteOrder
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * The picture at its actual size, repeated from the top left corner of the screen.
 *
 * A decoder can only output to one surface, so it outputs to a texture, which is then drawn once per
 * copy. That costs a frame or so of delay, which is why only this mode goes through here.
 *
 * @param onSurface what the decoder should output to, or null when there is nothing to show it on.
 *   Called on the main thread.
 */
class TiledVideoView(context: Context, private val onSurface: (Surface?) -> Unit) : GLSurfaceView(context) {
    @Volatile private var pictureW = 0
    @Volatile private var pictureH = 0
    @Volatile private var decoderSurface: Surface? = null

    private val renderer = object : Renderer, SurfaceTexture.OnFrameAvailableListener {
        private var texture: SurfaceTexture? = null
        private var program = 0
        private var transformAt = 0
        private var viewW = 0
        private var viewH = 0
        private val transform = FloatArray(16)
        private val quad = ByteBuffer.allocateDirect(8 * 4).order(ByteOrder.nativeOrder()).asFloatBuffer().apply {
            put(floatArrayOf(-1f, -1f, 1f, -1f, -1f, 1f, 1f, 1f))
            position(0)
        }

        // A new context means the old texture has gone, along with what the decoder was writing to.
        override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
            program = buildProgram()
            transformAt = GLES20.glGetUniformLocation(program, "transform")
            val names = IntArray(1)
            GLES20.glGenTextures(1, names, 0)
            GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, names[0])
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
            val previous = decoderSurface
            val st = SurfaceTexture(names[0])
            st.setOnFrameAvailableListener(this)
            texture?.release()
            texture = st
            val surface = Surface(st)
            decoderSurface = surface
            post {
                onSurface(surface)
                previous?.release()
            }
        }

        override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
            viewW = width
            viewH = height
        }

        override fun onDrawFrame(gl: GL10?) {
            val st = texture ?: return
            st.updateTexImage()
            st.getTransformMatrix(transform)
            GLES20.glViewport(0, 0, viewW, viewH)
            GLES20.glClearColor(0f, 0f, 0f, 1f)
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
            if (pictureW <= 0 || pictureH <= 0 || viewW <= 0 || viewH <= 0) return
            GLES20.glUseProgram(program)
            GLES20.glUniformMatrix4fv(transformAt, 1, false, transform, 0)
            GLES20.glEnableVertexAttribArray(0)
            GLES20.glVertexAttribPointer(0, 2, GLES20.GL_FLOAT, false, 0, quad)
            val (tileW, tileH) = tileSize(viewW, viewH, pictureW, pictureH)
            // GL counts from the bottom. Copies start at the top left, as the touch mapping expects.
            var y = 0
            while (y < viewH) {
                var x = 0
                while (x < viewW) {
                    GLES20.glViewport(x, viewH - y - tileH, tileW, tileH)
                    GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
                    x += tileW
                }
                y += tileH
            }
        }

        override fun onFrameAvailable(surfaceTexture: SurfaceTexture?) = requestRender()

        private fun buildProgram(): Int {
            val p = GLES20.glCreateProgram()
            GLES20.glAttachShader(p, shader(GLES20.GL_VERTEX_SHADER, VERTEX))
            GLES20.glAttachShader(p, shader(GLES20.GL_FRAGMENT_SHADER, FRAGMENT))
            GLES20.glBindAttribLocation(p, 0, "corner")
            GLES20.glLinkProgram(p)
            return p
        }

        private fun shader(kind: Int, source: String): Int {
            val s = GLES20.glCreateShader(kind)
            GLES20.glShaderSource(s, source)
            GLES20.glCompileShader(s)
            return s
        }
    }

    init {
        setEGLContextClientVersion(2)
        setRenderer(renderer)
        renderMode = RENDERMODE_WHEN_DIRTY
    }

    fun setPicture(width: Int, height: Int) {
        pictureW = width
        pictureH = height
        requestRender()
    }

    // Back from the background the context is still there, so the surface it made is too.
    override fun surfaceCreated(holder: SurfaceHolder) {
        super.surfaceCreated(holder)
        decoderSurface?.let(onSurface)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        onSurface(null)
        super.surfaceDestroyed(holder)
    }

    private companion object {
        const val VERTEX = """
            attribute vec2 corner;
            uniform mat4 transform;
            varying vec2 place;
            void main() {
                gl_Position = vec4(corner, 0.0, 1.0);
                place = (transform * vec4((corner + 1.0) / 2.0, 0.0, 1.0)).xy;
            }
        """
        const val FRAGMENT = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            varying vec2 place;
            uniform samplerExternalOES picture;
            void main() { gl_FragColor = texture2D(picture, place); }
        """
    }
}
