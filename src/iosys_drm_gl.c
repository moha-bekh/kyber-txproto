#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

#include <gbm.h>
#include <libdrm/drm_fourcc.h>

#include "libtxproto/log.h"
#include "iosys_drm_gl.h"

#define EGL_API_FUNCS(X)                             \
    X(PFNEGLCREATEIMAGEKHRPROC, eglCreateImageKHR)   \
    X(PFNEGLDESTROYIMAGEKHRPROC, eglDestroyImageKHR) \
    X(PFNEGLGETERRORPROC, eglGetError) \
    \
    X(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC, glEGLImageTargetTexture2DOES) \
    X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLDELETESHADERPROC, glDeleteShader) \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers) \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLBINDBUFFERARBPROC, glBindBuffer) \
    X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLUNIFORM1IPROC, glUniform1i) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLGENTEXTURESEXTPROC, glGenTextures) \
    X(PFNGLTEXTURESUBIMAGE2DPROC, glTexImage2D) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)

#define LOAD_GL_FUNTION(type, name)         \
    gl->name = (type)eglGetProcAddress(#name);  \
    if (!gl->name) {                        \
        sp_log(gl->log, SP_LOG_ERROR, "failed to load OpenGL function %s\n", #name); \
        return -1;                                        \
    }

#define DRMGL_POOL_SIZE 2

typedef struct {
    struct gbm_bo* bo;
    EGLImageKHR egl_image;
    GLuint texture;
    int dma_fd;
    uint32_t stride;
    bool busy;
} DrmGLOutputBuffer;

struct DrmGL {
    void* log;

    struct gbm_device* gbm_device;

    EGLDisplay dpy;
    EGLContext ctx;
    EGLSurface surface;
    EGLConfig conf;

    EGLint major;
    EGLint minor;

    GLuint shaderProgram;
    GLuint VBO, VAO, EBO;

    EGLImageKHR inEglImage;
    GLuint inTexture;

    /* Single output texture (non-pool mode) */
    GLuint outTexture;
    GLuint framebuffer;

    /* GBM BO pool for DMA-BUF export */
    DrmGLOutputBuffer pool[DRMGL_POOL_SIZE];
    int pool_count;     /* 0 = single texture mode, >0 = pool mode */
    int pool_current;   /* index of currently bound output buffer */
    unsigned int out_width, out_height;
    bool swizzle_bgra;  /* true = swap R/B, false = pass through */
    GLint swizzle_loc;  /* uniform location for u_swizzle_bgra */

#define DECLARE_GL_FUNC_PTR(type, name) type name;
    EGL_API_FUNCS(DECLARE_GL_FUNC_PTR)
};


// Vertex shader source
const char *vertexShaderSource =
"#version 330 core\n"
"layout (location = 0) in vec2 position;"
"layout (location = 1) in vec2 texCoord;"
"out vec2 v_texCoord;"
"void main() {"
"    gl_Position = vec4(position, 0.0, 1.0);"
"    v_texCoord = texCoord;"
"}";

// Fragment shader source
const char *fragmentShaderSource =
"#version 330 core\n"
"out vec4 FragColor;"
"in vec2 v_texCoord;"
"uniform sampler2D u_texture;"
"uniform int u_swizzle_bgra;"
"void main() {"
"    vec4 color = texture2D(u_texture, v_texCoord);"
"    FragColor = (u_swizzle_bgra != 0) ? color.bgra : color;"
"}";

#define CASE_ENUM_TO_STR(value) case value: return #value;

static const char *eglGetErrorString(EGLint error)
{
    switch (error) {
        CASE_ENUM_TO_STR(EGL_SUCCESS)
        CASE_ENUM_TO_STR(EGL_NOT_INITIALIZED)
        CASE_ENUM_TO_STR(EGL_BAD_ACCESS)
        CASE_ENUM_TO_STR(EGL_BAD_ALLOC)
        CASE_ENUM_TO_STR(EGL_BAD_ATTRIBUTE)
        CASE_ENUM_TO_STR(EGL_BAD_CONTEXT)
        CASE_ENUM_TO_STR(EGL_BAD_CONFIG)
        CASE_ENUM_TO_STR(EGL_BAD_CURRENT_SURFACE)
        CASE_ENUM_TO_STR(EGL_BAD_DISPLAY)
        CASE_ENUM_TO_STR(EGL_BAD_SURFACE)
        CASE_ENUM_TO_STR(EGL_BAD_MATCH)
        CASE_ENUM_TO_STR(EGL_BAD_PARAMETER)
        CASE_ENUM_TO_STR(EGL_BAD_NATIVE_PIXMAP)
        CASE_ENUM_TO_STR(EGL_BAD_NATIVE_WINDOW)
        CASE_ENUM_TO_STR(EGL_CONTEXT_LOST)
    default:
        return "Unknown";
    }
}


static GLuint compileShader(DrmGL* gl, GLenum type, const char *source)
{

    GLuint shader = gl->glCreateShader(type);

    gl->glShaderSource(shader, 1, &source, NULL);
    gl->glCompileShader(shader);

    GLint success;

    gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        char infoLog[512];
        gl->glGetShaderInfoLog(shader, 512, NULL, infoLog);
        sp_log(gl->log, SP_LOG_ERROR, "Shader compilation failed: %s\n", infoLog);
        gl->glDeleteShader(shader);
        return 0;
    }

    return shader;
}

static GLuint createShaderProgram(DrmGL* gl, const char* vertex, const char* fragment)
{
    GLuint vertexShader = compileShader(gl, GL_VERTEX_SHADER, vertex);
    if (!vertexShader)
        return 0;

    GLuint fragmentShader = compileShader(gl, GL_FRAGMENT_SHADER, fragment);
    if (!fragmentShader) {
        gl->glDeleteShader(vertexShader);
        return 0;
    }

    GLuint shaderProgram = gl->glCreateProgram();
    gl->glAttachShader(shaderProgram, vertexShader);
    gl->glAttachShader(shaderProgram, fragmentShader);
    gl->glLinkProgram(shaderProgram);

    GLint success;
    gl->glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);
    if (!success)
    {
        char infoLog[512];
        gl->glGetProgramInfoLog(shaderProgram, 512, NULL, infoLog);
        sp_log(gl->log, SP_LOG_ERROR, "Shader program linking failed: %s\n", infoLog);
        gl->glDeleteShader(vertexShader);
        gl->glDeleteShader(fragmentShader);
        gl->glDeleteProgram(shaderProgram);
        return 0;
    }

    gl->glDeleteShader(vertexShader);
    gl->glDeleteShader(fragmentShader);

    return shaderProgram;
}

static int init_drawing(DrmGL* gl) {
    gl->shaderProgram = createShaderProgram(gl, vertexShaderSource, fragmentShaderSource);
    if (!gl->shaderProgram)
        return -1;
    float vertices[] = {
                        // positions  // texture coords
                        -1.0f, -1.0f, 0.0f, 0.0f,
                         1.0f, -1.0f, 1.0f, 0.0f,
                         1.0f,  1.0f, 1.0f, 1.0f,
                        -1.0f,  1.0f, 0.0f, 1.0f};

    unsigned int indices[] = {
        0, 1, 2,
        2, 3, 0
    };

    gl->glGenVertexArrays(1, &gl->VAO);
    gl->glGenBuffers(1, &gl->VBO);
    gl->glGenBuffers(1, &gl->EBO);

    gl->glBindVertexArray(gl->VAO);

    gl->glBindBuffer(GL_ARRAY_BUFFER, gl->VBO);
    gl->glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

    gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl->EBO);
    gl->glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    // Position attribute
    gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    gl->glEnableVertexAttribArray(0);

    // Texture coordinate attribute
    gl->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
    gl->glEnableVertexAttribArray(1);

    // Uniforms
    gl->glUseProgram(gl->shaderProgram);
    gl->glUniform1i(gl->glGetUniformLocation(gl->shaderProgram, "u_texture"), 0);
    gl->swizzle_loc = gl->glGetUniformLocation(gl->shaderProgram, "u_swizzle_bgra");

    // create framebuffer
    gl->glGenFramebuffers(1, &gl->framebuffer);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, gl->framebuffer);


    return 0;
}

DrmGL* drmgl_alloc(void) {
    DrmGL* gl = calloc(1, sizeof(*gl));
    return gl;
}

int drmgl_init(DrmGL* gl, void* log, int card_fd) {
    gl->log = log;

    gl->gbm_device = gbm_create_device(card_fd);
    if (!gl->gbm_device) {
        sp_log(gl->log, SP_LOG_ERROR, "fail to create gbm device on fd %i\n", card_fd);
        return -1;
    }

    gl->dpy = eglGetDisplay(gl->gbm_device);

    if (gl->dpy == EGL_NO_DISPLAY) {
        sp_log(gl->log, SP_LOG_ERROR, "eglGetDisplay failed\n");
        return -1;
    }

    EGLBoolean eglRet;
    eglRet = eglInitialize(gl->dpy, &gl->major, &gl->minor);
    if (eglRet != EGL_TRUE) {
        sp_log(gl->log, SP_LOG_ERROR, "eglInitialize failed\n");
        return -1;
    }

    sp_log(gl->log, SP_LOG_INFO, "egl initialized with version %i.%i\n", gl->major, gl->minor);

    eglRet = eglBindAPI(EGL_OPENGL_API);
    if (eglRet != EGL_TRUE) {
        sp_log(gl->log, SP_LOG_ERROR, "eglBindAPI failed\n");
        return -1;
    }

    EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_DEPTH_SIZE, EGL_DONT_CARE,
        EGL_STENCIL_SIZE, EGL_DONT_CARE,
        EGL_BUFFER_SIZE, 32,
        EGL_NONE
    };

    EGLint numConfigs;
    eglRet = eglChooseConfig(gl->dpy, configAttribs, NULL, 0, &numConfigs);
    if (eglRet != EGL_TRUE || numConfigs == 0) {
        sp_log(gl->log, SP_LOG_ERROR, "no egl configuration found");
        return -1;
    }

    EGLConfig* configs = (EGLConfig*)malloc(numConfigs * sizeof(EGLConfig));
    eglRet = eglChooseConfig(gl->dpy, configAttribs, configs, numConfigs, &numConfigs);
    if (eglRet != EGL_TRUE) {
        sp_log(gl->log, SP_LOG_ERROR, "no egl configuration found");
        free(configs);
        return -1;
    }

    //FIXME: do we need to care? we're not drawing in the regular frambuffer
    //try to find a compatible format
    for (int i = 0; i < numConfigs; ++i) {
        EGLint gbm_format;
        eglGetConfigAttrib(gl->dpy, configs[i], EGL_NATIVE_VISUAL_ID, &gbm_format);

        bool formatOk = false;
        switch (gbm_format) {
        case GBM_FORMAT_ARGB8888:
        case GBM_FORMAT_XRGB8888:
            formatOk = true;
            break;
        default:
            break;
        }
        if (formatOk) {
            gl->conf = configs[i];
            break;
        }
    }
    free(configs);
    if (gl->conf == NULL)
        return -1;

    const char *extensions = eglQueryString(gl->dpy, EGL_EXTENSIONS);
    if (!strstr(extensions, "EGL_KHR_image"))
    {
        sp_log(gl->log, SP_LOG_ERROR, "EGL_KHR_image extension not available\n");
        return -1;
    }
    if (!strstr(extensions, "EGL_EXT_image_dma_buf_import"))
    {
        sp_log(gl->log, SP_LOG_ERROR, "EGL_EXT_image_dma_buf_import extension not available\n");
        return -1;
    }

    EGL_API_FUNCS(LOAD_GL_FUNTION)

    EGLint contextAttribs[] = {
       EGL_CONTEXT_MAJOR_VERSION, 3,
       EGL_CONTEXT_MINOR_VERSION, 3,
       EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
       EGL_NONE
    };
    gl->ctx = eglCreateContext(gl->dpy, gl->conf, EGL_NO_CONTEXT, contextAttribs);
    if (!gl->ctx) {
        sp_log(gl->log, SP_LOG_ERROR, "failed to create egl context\n");
        return false;
    }

    eglMakeCurrent(gl->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, gl->ctx);

    int ret = init_drawing(gl);
    if (ret != 0)
        return ret;

    return 0;
}

static void destroy_pool(DrmGL* gl) {
    for (int i = 0; i < gl->pool_count; i++) {
        DrmGLOutputBuffer* buf = &gl->pool[i];
        if (buf->texture) {
            glDeleteTextures(1, &buf->texture);
            buf->texture = 0;
        }
        if (buf->egl_image && gl->eglDestroyImageKHR) {
            gl->eglDestroyImageKHR(gl->dpy, buf->egl_image);
            buf->egl_image = 0;
        }
        if (buf->dma_fd >= 0) {
            close(buf->dma_fd);
            buf->dma_fd = -1;
        }
        if (buf->bo) {
            gbm_bo_destroy(buf->bo);
            buf->bo = NULL;
        }
        buf->busy = false;
    }
    gl->pool_count = 0;
    gl->pool_current = -1;
}

void drmgl_release(DrmGL* gl) {
    if (!gl)
        return;

    // Destroy pool
    destroy_pool(gl);

    // Destroy input texture and EGL image
    if (gl->inTexture) {
        glDeleteTextures(1, &gl->inTexture);
        gl->inTexture = 0;
    }
    if (gl->inEglImage && gl->eglDestroyImageKHR) {
        gl->eglDestroyImageKHR(gl->dpy, gl->inEglImage);
        gl->inEglImage = 0;
    }

    // Destroy output texture and framebuffer
    if (gl->outTexture) {
        glDeleteTextures(1, &gl->outTexture);
        gl->outTexture = 0;
    }
    if (gl->framebuffer && gl->glDeleteFramebuffers) {
        if (gl->glBindFramebuffer)
            gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
        gl->glDeleteFramebuffers(1, &gl->framebuffer);
        gl->framebuffer = 0;
    }

    // Destroy VAO, VBO, EBO
    if (gl->VAO && gl->glDeleteVertexArrays) {
        if (gl->glBindVertexArray)
            gl->glBindVertexArray(0);
        gl->glDeleteVertexArrays(1, &gl->VAO);
        gl->VAO = 0;
    }
    if (gl->VBO && gl->glDeleteBuffers) {
        gl->glDeleteBuffers(1, &gl->VBO);
        gl->VBO = 0;
    }
    if (gl->EBO && gl->glDeleteBuffers) {
        gl->glDeleteBuffers(1, &gl->EBO);
        gl->EBO = 0;
    }

    // Destroy shader program
    if (gl->shaderProgram && gl->glDeleteProgram) {
        gl->glDeleteProgram(gl->shaderProgram);
        gl->shaderProgram = 0;
    }

    // Destroy EGL context and display
    if (gl->dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(gl->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (gl->ctx != EGL_NO_CONTEXT) {
            eglDestroyContext(gl->dpy, gl->ctx);
            gl->ctx = EGL_NO_CONTEXT;
        }
        eglTerminate(gl->dpy);
        gl->dpy = EGL_NO_DISPLAY;
    }

    // Destroy GBM device
    if (gl->gbm_device) {
        gbm_device_destroy(gl->gbm_device);
        gl->gbm_device = NULL;
    }

    free(gl);
}

void drmgl_set_swizzle(DrmGL* gl, bool bgra_swizzle)
{
    gl->swizzle_bgra = bgra_swizzle;
}

void drmgl_set_crop(DrmGL* gl, unsigned int fb_w, unsigned int fb_h,
                    unsigned int crop_x, unsigned int crop_y,
                    unsigned int crop_w, unsigned int crop_h)
{
    float tx0 = 0.0f, ty0 = 0.0f, tx1 = 1.0f, ty1 = 1.0f;

    if (crop_w > 0 && crop_h > 0 && fb_w > 0 && fb_h > 0) {
        tx0 = (float)crop_x / fb_w;
        ty0 = (float)crop_y / fb_h;
        tx1 = (float)(crop_x + crop_w) / fb_w;
        ty1 = (float)(crop_y + crop_h) / fb_h;
    }

    float vertices[] = {
        // positions    // texture coords
        -1.0f, -1.0f,  tx0, ty0,
         1.0f, -1.0f,  tx1, ty0,
         1.0f,  1.0f,  tx1, ty1,
        -1.0f,  1.0f,  tx0, ty1,
    };

    gl->glBindVertexArray(gl->VAO);
    gl->glBindBuffer(GL_ARRAY_BUFFER, gl->VBO);
    gl->glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    gl->glBindVertexArray(0);
}

#define ADD_GL_ATTR(k, v) \
    img_attrs[i++] = k; \
    img_attrs[i++] = v

#define MAP_DMA_PLANE(p) do { \
    ADD_GL_ATTR(EGL_DMA_BUF_PLANE##p##_FD_EXT, buffer->dmafd[p]); \
    ADD_GL_ATTR(EGL_DMA_BUF_PLANE##p##_OFFSET_EXT, buffer->offsets[p]); \
    ADD_GL_ATTR(EGL_DMA_BUF_PLANE##p##_PITCH_EXT, buffer->pitches[p]); \
    if (buffer->modifier != DRM_FORMAT_MOD_INVALID) {   \
        EGLint lo = buffer->modifier & 0xFFFFFFFF; \
        EGLint hi = (buffer->modifier >> 32) & 0xFFFFFFFF; \
        ADD_GL_ATTR(EGL_DMA_BUF_PLANE##p##_MODIFIER_LO_EXT, lo); \
        ADD_GL_ATTR(EGL_DMA_BUF_PLANE##p##_MODIFIER_HI_EXT, hi); \
    } \
} while (0)



int drmgl_bind_dmatexture(DrmGL* gl, const FbWrapper* buffer) {
    if (gl->inTexture) {
        glDeleteTextures(1, &gl->inTexture);
        gl->inTexture = 0;
    }
    if (gl->inEglImage) {
        gl->eglDestroyImageKHR(gl->dpy, gl->inEglImage);
        gl->inEglImage = 0;
    }

    if (buffer->planes == 0 || buffer->planes > 4) {
        sp_log(gl->log, SP_LOG_ERROR, "bad number of dma planes %i\n", buffer->planes);
        return -1;
    }

    const size_t max_attrs = (10 + buffer->planes * 10);
    EGLint* img_attrs = malloc(sizeof(EGLint) * max_attrs);
    int i = 0;
    ADD_GL_ATTR(EGL_IMAGE_PRESERVED_KHR, EGL_TRUE);
    ADD_GL_ATTR(EGL_WIDTH, (EGLint)buffer->width);
    ADD_GL_ATTR(EGL_HEIGHT, (EGLint)buffer->height);
    ADD_GL_ATTR(EGL_LINUX_DRM_FOURCC_EXT, (EGLint)buffer->pixel_format);

    switch (buffer->planes)
    {
    case 4:
        MAP_DMA_PLANE(3);
        // fall through
    case 3:
        MAP_DMA_PLANE(2);
        // fall through
    case 2:
        MAP_DMA_PLANE(1);
        // fall through
    case 1:
        MAP_DMA_PLANE(0);
        break;
    default:
        assert(false); //unreachable;
        return -1;
    }
    img_attrs[i++] = EGL_NONE;
    assert(i <= max_attrs);

    gl->inEglImage = gl->eglCreateImageKHR(gl->dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, 0, img_attrs);
    EGLint glError = eglGetError();
    if (!gl->inEglImage || glError != EGL_SUCCESS) {
        EGLint eglError = gl->eglGetError();
        sp_log(gl->log, SP_LOG_ERROR, "failed to create image KHR image: %s (%X)\n",  eglGetErrorString(eglError), eglError);
        free(img_attrs);
        return -1;
    }

    gl->glGenTextures(1, &gl->inTexture);
    glBindTexture(GL_TEXTURE_2D, gl->inTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, gl->inEglImage);
    glBindTexture(GL_TEXTURE_2D, 0);

    free(img_attrs);

    return 0;
}

static int init_pool_buffer(DrmGL* gl, DrmGLOutputBuffer* buf,
                            unsigned int width, unsigned int height)
{
    buf->bo = gbm_bo_create(gl->gbm_device, width, height,
                            GBM_FORMAT_ARGB8888,
                            GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!buf->bo) {
        sp_log(gl->log, SP_LOG_ERROR, "gbm_bo_create(%ux%u) failed\n", width, height);
        return -1;
    }

    buf->dma_fd = gbm_bo_get_fd(buf->bo);
    buf->stride = gbm_bo_get_stride(buf->bo);

    if (buf->dma_fd < 0) {
        sp_log(gl->log, SP_LOG_ERROR, "gbm_bo_get_fd failed\n");
        gbm_bo_destroy(buf->bo);
        buf->bo = NULL;
        return -1;
    }

    int offset = gbm_bo_get_offset(buf->bo, 0);

    EGLint img_attrs[] = {
        EGL_WIDTH, (EGLint)width,
        EGL_HEIGHT, (EGLint)height,
        EGL_LINUX_DRM_FOURCC_EXT, GBM_FORMAT_ARGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, buf->dma_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)buf->stride,
        EGL_NONE,
    };

    buf->egl_image = gl->eglCreateImageKHR(gl->dpy, EGL_NO_CONTEXT,
                                            EGL_LINUX_DMA_BUF_EXT, NULL, img_attrs);
    if (!buf->egl_image) {
        EGLint eglError = gl->eglGetError();
        sp_log(gl->log, SP_LOG_ERROR, "eglCreateImageKHR for pool buffer failed: %s (%X)\n",
               eglGetErrorString(eglError), eglError);
        close(buf->dma_fd);
        buf->dma_fd = -1;
        gbm_bo_destroy(buf->bo);
        buf->bo = NULL;
        return -1;
    }

    gl->glGenTextures(1, &buf->texture);
    glBindTexture(GL_TEXTURE_2D, buf->texture);
    gl->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, buf->egl_image);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    buf->busy = false;

    return 0;
}

int drmgl_resize_output(DrmGL* gl, unsigned int width, unsigned int height,
                        bool dmabuf_pool)
{
    /* Clean up previous state */
    destroy_pool(gl);
    if (gl->outTexture) {
        glDeleteTextures(1, &gl->outTexture);
        gl->outTexture = 0;
    }

    gl->out_width = width;
    gl->out_height = height;

    if (dmabuf_pool) {
        /* Allocate GBM BO pool for DMA-BUF export */
        for (int i = 0; i < DRMGL_POOL_SIZE; i++) {
            gl->pool[i].dma_fd = -1;
            int ret = init_pool_buffer(gl, &gl->pool[i], width, height);
            if (ret < 0) {
                destroy_pool(gl);
                return ret;
            }
            gl->pool_count++;
        }
        gl->pool_current = -1;
        sp_log(gl->log, SP_LOG_INFO, "GL output pool: %d x %ux%u GBM buffers\n",
               DRMGL_POOL_SIZE, width, height);
    } else {
        /* Single GL texture mode (no pool) */
        gl->glGenTextures(1, &gl->outTexture);
        if (!gl->outTexture) {
            sp_log(gl->log, SP_LOG_ERROR, "failed to glGenTextures\n");
            return -1;
        }
        glBindTexture(GL_TEXTURE_2D, gl->outTexture);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        gl->glBindFramebuffer(GL_FRAMEBUFFER, gl->framebuffer);
        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl->outTexture, 0);
        if (gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            sp_log(gl->log, SP_LOG_ERROR, "Framebuffer not complete\n");
            return -1;
        }
    }

    glViewport(0, 0, width, height);
    return 0;
}

int drmgl_acquire_output(DrmGL* gl)
{
    if (gl->pool_count == 0) {
        /* Single texture mode - always available */
        gl->pool_current = 0;
        return 0;
    }

    /* Find a free buffer in the pool */
    for (int i = 0; i < gl->pool_count; i++) {
        if (!gl->pool[i].busy) {
            gl->pool[i].busy = true;
            gl->pool_current = i;

            /* Bind this buffer's texture as the framebuffer render target */
            gl->glBindFramebuffer(GL_FRAMEBUFFER, gl->framebuffer);
            gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                        GL_TEXTURE_2D, gl->pool[i].texture, 0);
            if (gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                sp_log(gl->log, SP_LOG_ERROR, "Framebuffer not complete for pool buffer %d\n", i);
                gl->pool[i].busy = false;
                return -1;
            }
            glViewport(0, 0, gl->out_width, gl->out_height);
            return i;
        }
    }

    sp_log(gl->log, SP_LOG_WARN, "All %d output buffers are busy, dropping frame\n", gl->pool_count);
    return -1;
}

void drmgl_release_output(DrmGL* gl, int handle)
{
    if (gl->pool_count == 0 || handle < 0 || handle >= gl->pool_count)
        return;
    gl->pool[handle].busy = false;
}

int drmgl_get_output_fd(DrmGL* gl, int handle)
{
    if (handle < 0 || handle >= gl->pool_count)
        return -1;
    return dup(gl->pool[handle].dma_fd);
}

uint32_t drmgl_get_output_stride(DrmGL* gl, int handle)
{
    if (handle < 0 || handle >= gl->pool_count)
        return 0;
    return gl->pool[handle].stride;
}

int drmgl_draw(DrmGL* gl)
{
    GLuint target_texture = gl->outTexture;
    if (gl->pool_count > 0 && gl->pool_current >= 0)
        target_texture = gl->pool[gl->pool_current].texture;

    assert(gl->inTexture != 0);
    assert(target_texture != 0);

    glClearColor(1.0f, 0.4f, 0.4f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    gl->glUseProgram(gl->shaderProgram);
    gl->glUniform1i(gl->swizzle_loc, gl->swizzle_bgra ? 1 : 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl->inTexture);

    gl->glBindVertexArray(gl->VAO);

    gl->glBindFramebuffer(GL_FRAMEBUFFER, gl->framebuffer);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

    glBindTexture(GL_TEXTURE_2D, 0);
    gl->glBindVertexArray(0);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);

    //wait for completion
    glFinish();

    return 0;
}

GLuint drmgl_get_backtexture(const DrmGL* gl)
{
    if (gl->pool_count > 0 && gl->pool_current >= 0)
        return gl->pool[gl->pool_current].texture;
    return gl->outTexture;
}

EGLDisplay drmgl_get_egl_display(const DrmGL* gl)
{
    return gl->dpy;
}

EGLContext drmgl_get_egl_context(const DrmGL* gl)
{
    return gl->ctx;
}
