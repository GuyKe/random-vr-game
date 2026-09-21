// Minimal "Hello World" OpenXR app for Meta Quest.
//
// Opens an OpenXR session, creates one swapchain per eye, and clears each
// eye's view to a solid color every frame. That's enough to prove the whole
// pipeline works end to end (instance/session/swapchain lifecycle, frame
// timing, stereo submission) without betting on unverified 3D math for a
// build that can't be tested on real hardware from here.

#include <android_native_app_glue.h>
#include <android/log.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

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

void CreateSwapchains() {
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(g.instance, g.systemId,
                                       XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount,
                                       nullptr);
    g.viewConfigViews.resize(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(g.instance, g.systemId,
                                       XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount,
                                       &viewCount, g.viewConfigViews.data());

    for (uint32_t i = 0; i < viewCount; i++) {
        const XrViewConfigurationView& vp = g.viewConfigViews[i];

        Swapchain swapchain;
        swapchain.width = static_cast<int32_t>(vp.recommendedImageRectWidth);
        swapchain.height = static_cast<int32_t>(vp.recommendedImageRectHeight);

        XrSwapchainCreateInfo swapchainCreateInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swapchainCreateInfo.usageFlags =
            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        swapchainCreateInfo.format = GL_SRGB8_ALPHA8;
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

void RenderFrame() {
    XrFrameWaitInfo frameWaitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    CHK_XR(xrWaitFrame(g.session, &frameWaitInfo, &frameState));

    XrFrameBeginInfo frameBeginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    CHK_XR(xrBeginFrame(g.session, &frameBeginInfo));

    std::vector<XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::vector<XrCompositionLayerProjectionView> projectionViews;

    if (frameState.shouldRender) {
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
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                    colorTexture, 0);
            glViewport(0, 0, swapchain.width, swapchain.height);

            // Distinct colors per eye make stereo separation obvious even
            // without any 3D geometry.
            if (i == 0) {
                glClearColor(0.10f, 0.35f, 0.65f, 1.0f);
            } else {
                glClearColor(0.65f, 0.35f, 0.10f, 1.0f);
            }
            glClear(GL_COLOR_BUFFER_BIT);

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
    }

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
