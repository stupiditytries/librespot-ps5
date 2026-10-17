// EGL display and stencil canvas adapted from StremiBrew.
#pragma once
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <ps5_opengl_display_modes.h>
void log_line(const char*,...);
struct Display
{
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    int width = 0;
    int height = 0;

    bool open()
    {
        const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                            EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                            EGL_NONE};
        const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
                                             EGL_CONTEXT_MINOR_VERSION_KHR, 3,
                                             EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
                                             EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE};
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        // The UI is drawn at 4K; the console scales it for a display that shows less. The
        // mode can only be chosen before EGL starts.
        if (!eglSetDisplayModePS5(display, 3840, 2160) || !eglSetDisplayRefreshPS5(display, 60))
            log_line("display: 4K mode refused (0x%x), staying at the default", eglGetError());
        EGLConfig config = nullptr;
        EGLint count = 0;
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr) ||
            !eglBindAPI(EGL_OPENGL_API) ||
            !eglChooseConfig(display, config_attributes, &config, 1, &count) || count != 1)
        {
            log_line("display: EGL initialisation failed (0x%x)", eglGetError());
            return false;
        }
        surface = eglCreateWindowSurface(display, config, static_cast<EGLNativeWindowType>(0),
                                         nullptr);
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
        if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
            !eglMakeCurrent(display, surface, surface, context))
        {
            log_line("display: no surface or context (0x%x)", eglGetError());
            return false;
        }
        EGLint surface_width = 0, surface_height = 0;
        eglQuerySurface(display, surface, EGL_WIDTH, &surface_width);
        eglQuerySurface(display, surface, EGL_HEIGHT, &surface_height);
        width = surface_width;
        height = surface_height;
        eglSwapInterval(display, 1);
        log_line("display: %dx%d, %s, %s", width, height,
                 reinterpret_cast<const char *>(glGetString(GL_VERSION)),
                 reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
        return width > 0 && height > 0;
    }

    bool present() const
    {
        return eglSwapBuffers(display, surface) == EGL_TRUE;
    }
};

// The UI is drawn into a texture that has a stencil buffer (which the drawing library
// needs and the screen's own surface is not promised to have), and that texture is then
// drawn to the screen with one triangle. Copying it with glBlitFramebuffer instead would
// take the driver's slow path.
class Canvas
{
  public:
    bool create(int width, int height)
    {
        width_ = width;
        height_ = height;
        glGenFramebuffers(1, &framebuffer_);
        glGenTextures(1, &color_);
        glGenRenderbuffers(1, &stencil_);
        glBindTexture(GL_TEXTURE_2D, color_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindRenderbuffer(GL_RENDERBUFFER, stencil_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                  stencil_);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE)
        {
            log_line("canvas: framebuffer incomplete (0x%x)", status);
            return false;
        }
        return create_program();
    }

    void begin() const
    {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        glViewport(0, 0, width_, height_);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }

    // Draws the canvas over the whole screen.
    void show(int screen_width, int screen_height) const
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, screen_width, screen_height);
        glDisable(GL_BLEND);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glUseProgram(program_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, color_);
        glBindVertexArray(vertex_array_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        glUseProgram(0);
    }

  private:
    static GLuint compile(GLenum type, const char *source)
    {
        const GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint compiled = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled != GL_TRUE)
        {
            char text[512] = {};
            glGetShaderInfoLog(shader, sizeof text - 1, nullptr, text);
            log_line("canvas: shader failed: %s", text);
        }
        return shader;
    }

    bool create_program()
    {
        // One triangle that covers the screen; its corners are derived from the vertex
        // number, so no vertex data is needed.
        static const char kVertex[] = "#version 330 core\n"
                                      "out vec2 uv;\n"
                                      "void main() {\n"
                                      "  uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
                                      "  gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);\n"
                                      "}\n";
        static const char kFragment[] = "#version 330 core\n"
                                        "in vec2 uv;\n"
                                        "out vec4 color;\n"
                                        "uniform sampler2D canvas;\n"
                                        "void main() { color = vec4(texture(canvas, uv).rgb, 1.0); }\n";
        const GLuint vertex = compile(GL_VERTEX_SHADER, kVertex);
        const GLuint fragment = compile(GL_FRAGMENT_SHADER, kFragment);
        program_ = glCreateProgram();
        glAttachShader(program_, vertex);
        glAttachShader(program_, fragment);
        glLinkProgram(program_);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        GLint linked = GL_FALSE;
        glGetProgramiv(program_, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE)
        {
            log_line("canvas: program did not link");
            return false;
        }
        glUseProgram(program_);
        glUniform1i(glGetUniformLocation(program_, "canvas"), 0);
        glUseProgram(0);
        glGenVertexArrays(1, &vertex_array_);
        return true;
    }

    int width_ = 0, height_ = 0;
    GLuint framebuffer_ = 0, color_ = 0, stencil_ = 0, program_ = 0, vertex_array_ = 0;
};

