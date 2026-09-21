// "Hello World" OpenXR app for Meta Quest.
//
// Opens an OpenXR session, creates one swapchain per eye, and renders a big
// textured sign reading "SKELETON" floating in front of where the app
// started. The sign's pixels are generated at startup from a small
// hand-drawn bitmap font (no font file or Android Canvas/JNI needed) and
// drawn as a single textured quad using the view/projection matrices OpenXR
// reports for each eye.

#include <android_native_app_glue.h>
#include <android/log.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cmath>
#include <cstring>
#include <vector>

#define LOG_TAG "HelloVR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define CHK_XR(x)                                                          \
    do {                                                                   \
        XrResult chk_xr_result = (x);                                      \
        if (XR_FAILED(chk_xr_result)) {                                    \
            LOGE("OpenXR call failed: %s (%d) at %s:%d", #x, chk_xr_result, \
                 __FILE__, __LINE__);                                      \
        }                                                                   \
    } while (0)

namespace {

// ---------------------------------------------------------------------------
// Minimal 4x4 matrix math (column-major, matching GL's expected layout).
// ---------------------------------------------------------------------------

struct Mat4 {
    float m[16] = {0};
};

Mat4 Mat4Identity() {
    Mat4 r;
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 Mat4Multiply(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a.m[k * 4 + row] * b.m[col * 4 + k];
            }
            r.m[col * 4 + row] = sum;
        }
    }
    return r;
}

Mat4 Mat4Translation(float x, float y, float z) {
    Mat4 r = Mat4Identity();
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
}

Mat4 Mat4Scale(float sx, float sy, float sz) {
    Mat4 r = Mat4Identity();
    r.m[0] = sx;
    r.m[5] = sy;
    r.m[10] = sz;
    return r;
}

Mat4 Mat4FromQuaternion(const XrQuaternionf& q) {
    Mat4 r = Mat4Identity();
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float x2 = x + x, y2 = y + y, z2 = z + z;
    float xx = x * x2, xy = x * y2, xz = x * z2;
    float yy = y * y2, yz = y * z2, zz = z * z2;
    float wx = w * x2, wy = w * y2, wz = w * z2;

    r.m[0] = 1 - (yy + zz);
    r.m[1] = xy + wz;
    r.m[2] = xz - wy;
    r.m[4] = xy - wz;
    r.m[5] = 1 - (xx + zz);
    r.m[6] = yz + wx;
    r.m[8] = xz + wy;
    r.m[9] = yz - wx;
    r.m[10] = 1 - (xx + yy);
    return r;
}

Mat4 Mat4FromPose(const XrPosef& pose) {
    return Mat4Multiply(Mat4Translation(pose.position.x, pose.position.y, pose.position.z),
                         Mat4FromQuaternion(pose.orientation));
}

// Inverts a rigid-body transform (rotation + translation only): the
// rotation part is its own transpose, and the new translation is
// -R^T * originalTranslation.
Mat4 Mat4InvertRigidBody(const Mat4& m) {
    Mat4 r = Mat4Identity();
    r.m[0] = m.m[0];
    r.m[1] = m.m[4];
    r.m[2] = m.m[8];
    r.m[4] = m.m[1];
    r.m[5] = m.m[5];
    r.m[6] = m.m[9];
    r.m[8] = m.m[2];
    r.m[9] = m.m[6];
    r.m[10] = m.m[10];

    float px = m.m[12], py = m.m[13], pz = m.m[14];
    r.m[12] = -(r.m[0] * px + r.m[4] * py + r.m[8] * pz);
    r.m[13] = -(r.m[1] * px + r.m[5] * py + r.m[9] * pz);
    r.m[14] = -(r.m[2] * px + r.m[6] * py + r.m[10] * pz);
    return r;
}

// Standard OpenXR asymmetric-frustum projection matrix for OpenGL/GLES
// (positive-Y-up, [-1,1] NDC Z range), per the OpenXR spec's reference
// formula.
Mat4 Mat4ProjectionFromFov(const XrFovf& fov, float nearZ, float farZ) {
    const float tanLeft = tanf(fov.angleLeft);
    const float tanRight = tanf(fov.angleRight);
    const float tanDown = tanf(fov.angleDown);
    const float tanUp = tanf(fov.angleUp);

    const float tanWidth = tanRight - tanLeft;
    const float tanHeight = tanUp - tanDown;

    Mat4 r;
    r.m[0] = 2.0f / tanWidth;
    r.m[8] = (tanRight + tanLeft) / tanWidth;
    r.m[5] = 2.0f / tanHeight;
    r.m[9] = (tanUp + tanDown) / tanHeight;
    r.m[10] = -(farZ + nearZ) / (farZ - nearZ);
    r.m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
    r.m[11] = -1.0f;
    return r;
}

// ---------------------------------------------------------------------------
// A tiny hand-drawn 5x7 bitmap font, just enough to spell "SKELETON". Each
// row is a 5-bit mask, MSB = leftmost column.
// ---------------------------------------------------------------------------

const uint8_t* GlyphRows(char c) {
    static const uint8_t kS[7] = {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110};
    static const uint8_t kK[7] = {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001};
    static const uint8_t kE[7] = {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111};
    static const uint8_t kL[7] = {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111};
    static const uint8_t kT[7] = {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100};
    static const uint8_t kO[7] = {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110};
    static const uint8_t kN[7] = {0b10001, 0b11001, 0b10101, 0b10101, 0b10011, 0b10001, 0b10001};

    switch (c) {
        case 'S': return kS;
        case 'K': return kK;
        case 'E': return kE;
        case 'L': return kL;
        case 'T': return kT;
        case 'O': return kO;
        case 'N': return kN;
        default: return nullptr;
    }
}

constexpr int kGlyphCols = 5;
constexpr int kGlyphRows = 7;
constexpr int kGlyphScale = 14;
constexpr int kLetterGapCols = 1;
constexpr int kMarginCols = 3;
constexpr int kMarginRows = 3;

std::vector<uint8_t> BuildSignTexture(const char* text, int& outWidth, int& outHeight) {
    int len = static_cast<int>(std::strlen(text));
    int gridCols = len * kGlyphCols + (len - 1) * kLetterGapCols + kMarginCols * 2;
    int gridRows = kGlyphRows + kMarginRows * 2;
    outWidth = gridCols * kGlyphScale;
    outHeight = gridRows * kGlyphScale;

    std::vector<uint8_t> pixels(static_cast<size_t>(outWidth) * outHeight * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = 20;
        pixels[i + 1] = 20;
        pixels[i + 2] = 24;
        pixels[i + 3] = 255;
    }

    int cursorCol = kMarginCols;
    for (int li = 0; li < len; li++) {
        const uint8_t* glyph = GlyphRows(text[li]);
        if (glyph != nullptr) {
            for (int gr = 0; gr < kGlyphRows; gr++) {
                uint8_t rowBits = glyph[gr];
                for (int gc = 0; gc < kGlyphCols; gc++) {
                    bool on = (rowBits >> (kGlyphCols - 1 - gc)) & 1;
                    if (!on) continue;
                    int px0 = (cursorCol + gc) * kGlyphScale;
                    int py0 = (kMarginRows + gr) * kGlyphScale;
                    for (int dy = 0; dy < kGlyphScale; dy++) {
                        for (int dx = 0; dx < kGlyphScale; dx++) {
                            int idx = ((py0 + dy) * outWidth + (px0 + dx)) * 4;
                            pixels[idx + 0] = 235;
                            pixels[idx + 1] = 230;
                            pixels[idx + 2] = 215;
                            pixels[idx + 3] = 255;
                        }
                    }
                }
            }
        }
        cursorCol += kGlyphCols + kLetterGapCols;
    }
    return pixels;
}

// ---------------------------------------------------------------------------

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int32_t width = 0;
    int32_t height = 0;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
};

struct AppState {
    android_app* app = nullptr;

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace appSpace = XR_NULL_HANDLE;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning = false;
    bool exitRequested = false;

    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    EGLConfig eglConfig = nullptr;
    EGLContext eglContext = EGL_NO_CONTEXT;
    EGLSurface eglSurface = EGL_NO_SURFACE;

    GLuint fbo = 0;
    GLuint program = 0;
    GLint mvpLoc = -1;
    GLint textureLoc = -1;
    GLuint quadVbo = 0;
    GLuint quadIbo = 0;
    GLuint signTexture = 0;
    Mat4 signModel;

    std::vector<XrViewConfigurationView> viewConfigViews;
    std::vector<Swapchain> swapchains;
};

AppState g;

void InitializeLoader(android_app* app) {
    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                           reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader));
    if (initializeLoader == nullptr) {
        LOGE("xrInitializeLoaderKHR not available");
        return;
    }

    XrLoaderInitInfoAndroidKHR loaderInitInfoAndroid{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loaderInitInfoAndroid.applicationVM = app->activity->vm;
    loaderInitInfoAndroid.applicationContext = app->activity->clazz;
    CHK_XR(initializeLoader(
        reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInitInfoAndroid)));
}

void CreateInstance() {
    const char* extensions[] = {
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
    };

    XrInstanceCreateInfoAndroidKHR instanceCreateInfoAndroid{
        XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    instanceCreateInfoAndroid.applicationVM = g.app->activity->vm;
    instanceCreateInfoAndroid.applicationActivity = g.app->activity->clazz;

    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.next = &instanceCreateInfoAndroid;
    createInfo.enabledExtensionCount = 2;
    createInfo.enabledExtensionNames = extensions;
    std::strncpy(createInfo.applicationInfo.applicationName, "HelloVR",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

    CHK_XR(xrCreateInstance(&createInfo, &g.instance));
}

void InitializeSystem() {
    XrSystemGetInfo systemGetInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemGetInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    CHK_XR(xrGetSystem(g.instance, &systemGetInfo, &g.systemId));
}

void InitializeGL() {
    PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = nullptr;
    xrGetInstanceProcAddr(g.instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                           reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements));
    XrGraphicsRequirementsOpenGLESKHR requirements{
        XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    if (getRequirements != nullptr) {
        CHK_XR(getRequirements(g.instance, g.systemId, &requirements));
    }

    g.eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(g.eglDisplay, nullptr, nullptr);
    eglBindAPI(EGL_OPENGL_ES_API);

    const EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLint numConfigs = 0;
    eglChooseConfig(g.eglDisplay, configAttribs, &g.eglConfig, 1, &numConfigs);

    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    g.eglContext = eglCreateContext(g.eglDisplay, g.eglConfig, EGL_NO_CONTEXT, contextAttribs);

    const EGLint pbufferAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    g.eglSurface = eglCreatePbufferSurface(g.eglDisplay, g.eglConfig, pbufferAttribs);

    eglMakeCurrent(g.eglDisplay, g.eglSurface, g.eglSurface, g.eglContext);

    glGenFramebuffers(1, &g.fbo);
}

GLuint CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOGE("Shader compile failed: %s", log);
    }
    return shader;
}

void CreateSignRenderResources() {
    static const char* kVertexSrc = R"(#version 300 es
        layout(location = 0) in vec3 aPosition;
        layout(location = 1) in vec2 aTexCoord;
        uniform mat4 uMvp;
        out vec2 vTexCoord;
        void main() {
            gl_Position = uMvp * vec4(aPosition, 1.0);
            vTexCoord = aTexCoord;
        }
    )";
    static const char* kFragmentSrc = R"(#version 300 es
        precision mediump float;
        in vec2 vTexCoord;
        out vec4 fragColor;
        uniform sampler2D uTexture;
        void main() {
            fragColor = texture(uTexture, vTexCoord);
        }
    )";

    GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertexSrc);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragmentSrc);
    g.program = glCreateProgram();
    glAttachShader(g.program, vs);
    glAttachShader(g.program, fs);
    glLinkProgram(g.program);
    GLint linked = 0;
    glGetProgramiv(g.program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(g.program, sizeof(log), nullptr, log);
        LOGE("Program link failed: %s", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    g.mvpLoc = glGetUniformLocation(g.program, "uMvp");
    g.textureLoc = glGetUniformLocation(g.program, "uTexture");

    int texWidth = 0, texHeight = 0;
    std::vector<uint8_t> pixels = BuildSignTexture("SKELETON", texWidth, texHeight);
    glGenTextures(1, &g.signTexture);
    glBindTexture(GL_TEXTURE_2D, g.signTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texWidth, texHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // A 2m-wide sign, height chosen to keep the texture's aspect ratio.
    float width = 2.0f;
    float height = width * static_cast<float>(texHeight) / static_cast<float>(texWidth);
    float hw = width * 0.5f;
    float hh = height * 0.5f;

    // clang-format off
    const float vertices[] = {
        // x,    y,   z,    u,    v
        -hw, -hh, 0.0f, 0.0f, 1.0f,
         hw, -hh, 0.0f, 1.0f, 1.0f,
         hw,  hh, 0.0f, 1.0f, 0.0f,
        -hw,  hh, 0.0f, 0.0f, 0.0f,
    };
    const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
    // clang-format on

    glGenBuffers(1, &g.quadVbo);
    glBindBuffer(GL_ARRAY_BUFFER, g.quadVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glGenBuffers(1, &g.quadIbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.quadIbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    // Float directly in front of where the app started, at head height.
    g.signModel = Mat4Translation(0.0f, 0.0f, -2.5f);
}

void CreateSession() {
    XrGraphicsBindingOpenGLESAndroidKHR graphicsBinding{
        XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    graphicsBinding.display = g.eglDisplay;
    graphicsBinding.config = g.eglConfig;
    graphicsBinding.context = g.eglContext;

    XrSessionCreateInfo sessionCreateInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionCreateInfo.next = &graphicsBinding;
    sessionCreateInfo.systemId = g.systemId;
    CHK_XR(xrCreateSession(g.instance, &sessionCreateInfo, &g.session));

    XrReferenceSpaceCreateInfo spaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceCreateInfo.poseInReferenceSpace.orientation.w = 1.0f;
    CHK_XR(xrCreateReferenceSpace(g.session, &spaceCreateInfo, &g.appSpace));
}

// The set of swapchain formats a runtime supports varies, so the format we
// request must come from xrEnumerateSwapchainFormats rather than being
// assumed - requesting an unsupported format fails swapchain creation and
// silently leaves nothing to render.
int64_t SelectSwapchainFormat() {
    uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(g.session, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    xrEnumerateSwapchainFormats(g.session, formatCount, &formatCount, formats.data());

    for (int64_t f : formats) {
        if (f == GL_SRGB8_ALPHA8) return f;
    }
    for (int64_t f : formats) {
        if (f == GL_RGBA8) return f;
    }
    return formats.empty() ? GL_RGBA8 : formats[0];
}

void CreateSwapchains() {
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(g.instance, g.systemId,
                                       XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount,
                                       nullptr);
    g.viewConfigViews.resize(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(g.instance, g.systemId,
                                       XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount,
                                       &viewCount, g.viewConfigViews.data());

    int64_t format = SelectSwapchainFormat();

    for (uint32_t i = 0; i < viewCount; i++) {
        const XrViewConfigurationView& vp = g.viewConfigViews[i];

        Swapchain swapchain;
        swapchain.width = static_cast<int32_t>(vp.recommendedImageRectWidth);
        swapchain.height = static_cast<int32_t>(vp.recommendedImageRectHeight);

        XrSwapchainCreateInfo swapchainCreateInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swapchainCreateInfo.usageFlags =
            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        swapchainCreateInfo.format = format;
        swapchainCreateInfo.sampleCount = 1;
        swapchainCreateInfo.width = swapchain.width;
        swapchainCreateInfo.height = swapchain.height;
        swapchainCreateInfo.faceCount = 1;
        swapchainCreateInfo.arraySize = 1;
        swapchainCreateInfo.mipCount = 1;
        CHK_XR(xrCreateSwapchain(g.session, &swapchainCreateInfo, &swapchain.handle));

        uint32_t imageCount = 0;
        xrEnumerateSwapchainImages(swapchain.handle, 0, &imageCount, nullptr);
        swapchain.images.resize(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        xrEnumerateSwapchainImages(
            swapchain.handle, imageCount, &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data()));

        g.swapchains.push_back(swapchain);
    }
}

void PollXrEvents() {
    XrEventDataBuffer eventBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g.instance, &eventBuffer) == XR_SUCCESS) {
        switch (eventBuffer.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                const auto* stateEvent =
                    reinterpret_cast<const XrEventDataSessionStateChanged*>(&eventBuffer);
                g.sessionState = stateEvent->state;
                LOGI("OpenXR session state changed: %d", g.sessionState);
                if (g.sessionState == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                    beginInfo.primaryViewConfigurationType =
                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    CHK_XR(xrBeginSession(g.session, &beginInfo));
                    g.sessionRunning = true;
                } else if (g.sessionState == XR_SESSION_STATE_STOPPING) {
                    CHK_XR(xrEndSession(g.session));
                    g.sessionRunning = false;
                } else if (g.sessionState == XR_SESSION_STATE_EXITING ||
                           g.sessionState == XR_SESSION_STATE_LOSS_PENDING) {
                    g.exitRequested = true;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                g.exitRequested = true;
                break;
            default:
                break;
        }
        eventBuffer.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

void DrawSign(const Mat4& viewProj) {
    Mat4 mvp = Mat4Multiply(viewProj, g.signModel);

    glUseProgram(g.program);
    glUniformMatrix4fv(g.mvpLoc, 1, GL_FALSE, mvp.m);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g.signTexture);
    glUniform1i(g.textureLoc, 0);

    glBindBuffer(GL_ARRAY_BUFFER, g.quadVbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.quadIbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                           (void*)(3 * sizeof(float)));

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
}

void RenderFrame() {
    XrFrameWaitInfo frameWaitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    CHK_XR(xrWaitFrame(g.session, &frameWaitInfo, &frameState));

    XrFrameBeginInfo frameBeginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    CHK_XR(xrBeginFrame(g.session, &frameBeginInfo));

    std::vector<XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::vector<XrCompositionLayerProjectionView> projectionViews;

    // Render (and submit) every frame regardless of frameState.shouldRender:
    // that flag is only a hint the compositor may not honor consistently,
    // and always submitting a layer avoids ever presenting nothing.
    uint32_t viewCount = static_cast<uint32_t>(g.viewConfigViews.size());
    std::vector<XrView> views(viewCount, {XR_TYPE_VIEW});

    XrViewLocateInfo viewLocateInfo{XR_TYPE_VIEW_LOCATE_INFO};
    viewLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    viewLocateInfo.displayTime = frameState.predictedDisplayTime;
    viewLocateInfo.space = g.appSpace;

    XrViewState viewState{XR_TYPE_VIEW_STATE};
    uint32_t outViewCount = 0;
    CHK_XR(xrLocateViews(g.session, &viewLocateInfo, &viewState, viewCount, &outViewCount,
                          views.data()));

    projectionViews.resize(viewCount);

    for (uint32_t i = 0; i < viewCount; i++) {
        Swapchain& swapchain = g.swapchains[i];

        uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        CHK_XR(xrAcquireSwapchainImage(swapchain.handle, &acquireInfo, &imageIndex));

        XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = XR_INFINITE_DURATION;
        CHK_XR(xrWaitSwapchainImage(swapchain.handle, &waitInfo));

        GLuint colorTexture = swapchain.images[imageIndex].image;

        glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTexture,
                                0);
        glViewport(0, 0, swapchain.width, swapchain.height);

        glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        Mat4 view = Mat4InvertRigidBody(Mat4FromPose(views[i].pose));
        Mat4 proj = Mat4ProjectionFromFov(views[i].fov, 0.05f, 100.0f);
        DrawSign(Mat4Multiply(proj, view));

        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        CHK_XR(xrReleaseSwapchainImage(swapchain.handle, &releaseInfo));

        projectionViews[i] = XrCompositionLayerProjectionView{
            XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        projectionViews[i].pose = views[i].pose;
        projectionViews[i].fov = views[i].fov;
        projectionViews[i].subImage.swapchain = swapchain.handle;
        projectionViews[i].subImage.imageRect.offset = {0, 0};
        projectionViews[i].subImage.imageRect.extent = {swapchain.width, swapchain.height};
    }

    layer.space = g.appSpace;
    layer.viewCount = viewCount;
    layer.views = projectionViews.data();
    layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer));

    XrFrameEndInfo frameEndInfo{XR_TYPE_FRAME_END_INFO};
    frameEndInfo.displayTime = frameState.predictedDisplayTime;
    frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    frameEndInfo.layerCount = static_cast<uint32_t>(layers.size());
    frameEndInfo.layers = layers.data();
    CHK_XR(xrEndFrame(g.session, &frameEndInfo));
}

void HandleAppCmd(android_app* /*app*/, int32_t cmd) {
    LOGI("android_app cmd: %d", cmd);
}

}  // namespace

void android_main(android_app* app) {
    g.app = app;
    app->onAppCmd = HandleAppCmd;

    InitializeLoader(app);
    CreateInstance();
    if (g.instance == XR_NULL_HANDLE) {
        LOGE("Failed to create OpenXR instance, exiting");
        return;
    }
    InitializeSystem();
    InitializeGL();
    CreateSession();
    CreateSwapchains();
    CreateSignRenderResources();

    while (app->destroyRequested == 0 && !g.exitRequested) {
        int events = 0;
        android_poll_source* source = nullptr;
        while (ALooper_pollAll(g.sessionRunning ? 0 : -1, nullptr, &events,
                                reinterpret_cast<void**>(&source)) >= 0) {
            if (source != nullptr) {
                source->process(app, source);
            }
            if (app->destroyRequested != 0) {
                break;
            }
        }

        PollXrEvents();

        if (g.sessionRunning) {
            RenderFrame();
        }
    }
}
