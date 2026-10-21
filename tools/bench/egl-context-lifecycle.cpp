// Probe the native EGL prerequisites for renderer recovery (#100).
// This destroys only contexts owned by this process; it does not reset a GPU.
// Build on Haiku: g++ -std=c++17 egl-context-lifecycle.cpp -o egl-context-lifecycle -lepoxy -lbe
#include <Application.h>
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <cstdio>
#include <cstring>
#include <vector>

static bool hasExtension(const char* list, const char* name)
{
    if (!list)
        return false;
    const auto length = std::strlen(name);
    for (const char* found = list; (found = std::strstr(found, name)); found += length) {
        if ((found == list || found[-1] == ' ') && (!found[length] || found[length] == ' '))
            return true;
    }
    return false;
}

int main()
{
    BApplication app("application/x-vnd.Summit-EGLContextLifecycle");
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (!eglInitialize(display, &major, &minor)) {
        std::fprintf(stderr, "eglInitialize failed: 0x%x\n", eglGetError());
        return 2;
    }
    bool passed = true;
    auto check = [&](const char* name, bool value) {
        std::printf("%s: %s\n", name, value ? "PASS" : "FAIL");
        std::fflush(stdout);
        passed &= value;
        return value;
    };
    std::printf("EGL: %d.%d\n", major, minor);
    const bool robust = hasExtension(eglQueryString(display, EGL_EXTENSIONS), "EGL_EXT_create_context_robustness");
    std::printf("EGL_EXT_create_context_robustness: %s\n", robust ? "yes" : "no");
    EGLint configAttributes[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig config;
    EGLint count = 0;
    if (!eglBindAPI(EGL_OPENGL_ES_API) || !eglChooseConfig(display, configAttributes, &config, 1, &count) || !count)
        return 3;
    EGLint surfaceAttributes[] = { EGL_WIDTH, 2, EGL_HEIGHT, 2, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(display, config, surfaceAttributes);
    if (surface == EGL_NO_SURFACE)
        return 4;
    std::vector<EGLint> attributes { EGL_CONTEXT_CLIENT_VERSION, 2 };
    if (robust) {
        attributes.insert(attributes.end(), { EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT,
            EGL_LOSE_CONTEXT_ON_RESET_EXT, EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT, EGL_TRUE });
    }
    attributes.push_back(EGL_NONE);
    auto create = [&](EGLContext share) { return eglCreateContext(display, config, share, attributes.data()); };
    auto bind = [&](EGLContext context) { return eglMakeCurrent(display, surface, surface, context) == EGL_TRUE; };
    auto unbind = [&] { return eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_TRUE; };
    EGLContext root = create(EGL_NO_CONTEXT), first = EGL_NO_CONTEXT, sibling = EGL_NO_CONTEXT, replacement = EGL_NO_CONTEXT;
    if (root == EGL_NO_CONTEXT || (first = create(root)) == EGL_NO_CONTEXT || (sibling = create(root)) == EGL_NO_CONTEXT || !bind(first))
        return 5;
    std::printf("GL_RENDERER: %s\nGL_VERSION: %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    const auto* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    const char* resetFunction = hasExtension(extensions, "GL_KHR_robustness") ? "glGetGraphicsResetStatusKHR"
        : hasExtension(extensions, "GL_EXT_robustness") ? "glGetGraphicsResetStatusEXT" : nullptr;
    using GetResetStatus = GLenum (*)();
    auto resetStatus = resetFunction ? reinterpret_cast<GetResetStatus>(eglGetProcAddress(resetFunction)) : nullptr;
    std::printf("Reset status entry point: %s\n", resetFunction ? resetFunction : "unavailable");
    if (robust)
        check("Reset status available and healthy", resetStatus && resetStatus() == GL_NO_ERROR);
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    const unsigned char pixel[] = { 204, 51, 102, 255 };
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    check("Texture created", texture && glIsTexture(texture) && glGetError() == GL_NO_ERROR);
    check("Destroy current context is deferred", eglDestroyContext(display, first) && eglGetCurrentContext() == first && glIsTexture(texture));
    check("Unbind destroyed context", unbind());
    const bool staleBind = bind(first);
    const EGLint staleError = eglGetError();
    check("Destroyed handle rejected", !staleBind && staleError == EGL_BAD_CONTEXT);
    std::printf("Destroyed handle EGL error: 0x%x\n", staleError);
    first = EGL_NO_CONTEXT;
    if (!check("Sibling binds after context destruction", bind(sibling)))
        return 6;
    check("Shared texture survives one context destruction", glIsTexture(texture));
    if (resetStatus)
        check("Destroying a context is not a graphics reset", resetStatus() == GL_NO_ERROR);
    replacement = create(root);
    if (!check("Replacement shares with existing group", replacement != EGL_NO_CONTEXT && bind(replacement) && glIsTexture(texture)))
        return 7;
    glDeleteTextures(1, &texture);
    unbind();
    eglDestroyContext(display, replacement);
    eglDestroyContext(display, sibling);
    eglDestroyContext(display, root);
    root = create(EGL_NO_CONTEXT);
    if (!check("Fresh share group binds", root != EGL_NO_CONTEXT && bind(root)))
        return 8;
    glViewport(0, 0, 2, 2);
    glClearColor(204.f / 255, 51.f / 255, 102.f / 255, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char actual[4] { };
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, actual);
    check("Fresh group renders exact pixels", !std::memcmp(actual, pixel, sizeof(pixel)) && glGetError() == GL_NO_ERROR);
    check("Final context unbinds", unbind());
    check("Final context destroyed", eglDestroyContext(display, root));
    check("Surface destroyed", eglDestroySurface(display, surface));
    check("Display terminated", eglTerminate(display));
    return passed ? 0 : 1;
}
