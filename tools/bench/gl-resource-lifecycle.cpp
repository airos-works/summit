// Native engine probe for #100. Only this process's contexts are affected.
// Build using build-skia-context-teardown.py --source this-file.cpp.
#include "config.h"
#include "BitmapTexture.h"
#include "GLContext.h"
#include "GLFenceEGL.h"
#include "GLFenceGL.h"
#include "PlatformDisplayHaiku.h"
#include "TextureMapperGPUBuffer.h"
#include "TextureMapperShaderProgram.h"
#include <Application.h>
#include <OS.h>
#include <epoxy/gl.h>
#include <wtf/MainThread.h>
#include <cstdio>
#include <cstring>

using namespace WebCore;
static bool passed = true;
static unsigned cleanupCalls, framebufferDeletes, resetQueries, resetStatus;
static bool suppressCleanup;
static decltype(epoxy_eglGetProcAddress) realGetProcAddress;
static unsigned fakeResetStatus() { ++resetQueries; return resetStatus; }
static __eglMustCastToProperFunctionPointerType getProcAddress(const char* name)
{
    if (!std::strcmp(name, "glGetGraphicsResetStatusKHR") || !std::strcmp(name, "glGetGraphicsResetStatusEXT")
        || !std::strcmp(name, "glGetGraphicsResetStatus"))
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(fakeResetStatus);
    return realGetProcAddress(name);
}
static void check(const char* name, bool value)
{
    std::printf("%s: %s\n", name, value ? "PASS" : "FAIL");
    passed &= value;
}

#define OBSERVE_GL(name, signature, arguments) \
    static decltype(epoxy_gl##name) real##name; \
    static void observe##name signature \
    { \
        ++cleanupCalls; \
        if (!suppressCleanup && eglGetCurrentContext() != EGL_NO_CONTEXT) \
            real##name arguments; \
    }
OBSERVE_GL(DeleteTextures, (GLsizei n, const GLuint* ids), (n, ids))
OBSERVE_GL(DeleteBuffers, (GLsizei n, const GLuint* ids), (n, ids))
OBSERVE_GL(DeleteRenderbuffers, (GLsizei n, const GLuint* ids), (n, ids))
OBSERVE_GL(DeleteShader, (GLuint id), (id))
OBSERVE_GL(DeleteProgram, (GLuint id), (id))
OBSERVE_GL(DetachShader, (GLuint program, GLuint shader), (program, shader))
OBSERVE_GL(DeleteSync, (GLsync sync), (sync))
#undef OBSERVE_GL
static decltype(epoxy_glDeleteFramebuffers) realDeleteFramebuffers;
static void observeDeleteFramebuffers(GLsizei n, const GLuint* ids)
{
    framebufferDeletes += n;
    ++cleanupCalls;
    if (!suppressCleanup && eglGetCurrentContext() != EGL_NO_CONTEXT)
        realDeleteFramebuffers(n, ids);
}

int main(int argc, char** argv)
{
    const char* mode = argc == 2 ? argv[1] : "";
    bool healthy = !std::strcmp(mode, "healthy"), siblingMode = !std::strcmp(mode, "sibling");
    bool bindFailure = !std::strcmp(mode, "failed-bind"), resetMode = !std::strcmp(mode, "reset");
    bool foreign = !std::strcmp(mode, "foreign"), destroyed = !std::strcmp(mode, "destroyed");
    bool terminated = !std::strcmp(mode, "terminated");
    if (!healthy && !siblingMode && !bindFailure && !resetMode && !foreign && !destroyed && !terminated) {
        std::fprintf(stderr, "Usage: %s healthy|sibling|failed-bind|reset|foreign|destroyed|terminated\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    disable_debugger(1);
    BApplication app("application/x-vnd.Summit-GLResourceLifecycle");
    WTF::initializeMainThread();
    image_info image;
    int32 cookie = 0;
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &image) == B_OK) {
        if (std::strstr(image.name, "libWebKit"))
            std::printf("Engine: %s\n", image.name);
    }
    std::printf("BitmapTexture size: %zu\n", sizeof(BitmapTexture));
    auto display = PlatformDisplayHaiku::create();
    if (!display)
        return 3;
    // Resolve epoxy's EGL entry point before intercepting it; its lazy resolver
    // would otherwise dispatch straight back into our observer.
    eglGetProcAddress("glClear");
    realGetProcAddress = epoxy_eglGetProcAddress;
    if (resetMode)
        epoxy_eglGetProcAddress = getProcAddress;
    auto owner = GLContext::createOffscreen(*display);
    auto sibling = GLContext::createOffscreen(*display);
    if (!owner || !sibling || !owner->makeContextCurrent())
        return 4;
    RefPtr<BitmapTexture> texture = BitmapTexture::create({ 16, 16 });
    std::printf("Texture: id=%u size=%dx%d\n", texture->id(), texture->size().width(), texture->size().height());
    texture->bindAsSurface();
    glClearColor(204.f / 255, 51.f / 255, 102.f / 255, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char pixel[4] { };
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    check("Texture framebuffer renders exact pixels", pixel[0] == 204 && pixel[1] == 51 && pixel[2] == 102 && pixel[3] == 255);
    RefPtr<TextureMapperGPUBuffer> buffer = TextureMapperGPUBuffer::create(256, TextureMapperGPUBuffer::Type::Vertex, TextureMapperGPUBuffer::Usage::Dynamic);
    RefPtr<TextureMapperShaderProgram> shader = TextureMapperShaderProgram::create({ TextureMapperShaderProgram::SolidColor });
    GLint linked = 0;
    glGetProgramiv(shader->programID(), GL_LINK_STATUS, &linked);
    check("Real texture, GPU buffer and shader exist", glIsTexture(texture->id()) && glIsBuffer(buffer->bufferID()) && linked);
#if HAVE(GL_FENCE)
    auto glFence = GLFenceGL::create();
    auto eglFence = GLFenceEGL::create(display->glDisplay());
    check("GL and EGL fences created", glFence && eglFence);
#endif

#define INSTALL_GL(name) \
    real##name = reinterpret_cast<decltype(real##name)>(realGetProcAddress("gl" #name)); \
    if (!real##name) return 5; \
    epoxy_gl##name = observe##name;
    INSTALL_GL(DeleteTextures)
    INSTALL_GL(DeleteBuffers)
    INSTALL_GL(DeleteRenderbuffers)
    INSTALL_GL(DeleteFramebuffers)
    INSTALL_GL(DeleteShader)
    INSTALL_GL(DeleteProgram)
    INSTALL_GL(DetachShader)
    INSTALL_GL(DeleteSync)
#undef INSTALL_GL
    suppressCleanup = !(healthy || siblingMode);
    std::unique_ptr<GLContext> unrelated;
    if (bindFailure) {
        auto handle = owner->platformContext();
        check("Owned context unbound", owner->unmakeContextCurrent());
        check("Owned native context destroyed", eglDestroyContext(display->eglDisplay(), handle));
        bool rebound = owner->makeContextCurrent();
        check("Destroyed context bind rejected", !rebound && eglGetError() == EGL_BAD_CONTEXT);
        check("Failed group is retired for siblings", !sibling->makeContextCurrent());
    } else if (resetMode) {
        // Loss must use the function cached during the healthy bind. Looking
        // it up again now would obtain the real driver's healthy response.
        epoxy_eglGetProcAddress = realGetProcAddress;
        auto queriesBeforeReset = resetQueries;
        resetStatus = GL_GUILTY_CONTEXT_RESET_KHR;
        check("Cached-current path detects reset", !owner->makeContextCurrent());
        check("Cached reset entry point was queried", resetQueries > queriesBeforeReset);
        resetStatus = GL_NO_ERROR;
        check("Reset remains sticky after status clears", !owner->makeContextCurrent());
        check("Reset retires sibling contexts", !sibling->makeContextCurrent());
    } else if (foreign) {
        unrelated = GLContext::createSharing(*display);
        check("Unrelated share group binds", unrelated && unrelated->makeContextCurrent());
    } else if (siblingMode) {
        check("Sibling binds", sibling->makeContextCurrent());
        auto handle = sibling->platformContext();
        owner = nullptr;
        check("Destroying non-current owner preserves sibling binding", eglGetCurrentContext() == handle && GLContext::current() == sibling.get());
    } else if (destroyed) {
        check("Owner unbound", owner->unmakeContextCurrent());
        owner = nullptr;
    } else if (terminated) {
        display->glDisplay().terminate();
        check("Terminated display rejects binding", !owner->makeContextCurrent());
    }
    texture = nullptr;
    buffer = nullptr;
    shader = nullptr;
#if HAVE(GL_FENCE)
    glFence = nullptr;
    eglFence = nullptr;
#endif
    std::printf("Observed GL cleanup calls: %u; framebuffer deletes: %u\n", cleanupCalls, framebufferDeletes);
    check("Resource cleanup matches context lifetime", (healthy || siblingMode) ? cleanupCalls > 0 : !cleanupCalls);
    if (siblingMode)
        check("Context-local framebuffer is not deleted in sibling", !framebufferDeletes);
    unrelated = nullptr;
    owner = nullptr;
    sibling = nullptr;
    display->clearGLContexts();
    display->glDisplay().terminate();
    return passed ? 0 : 1;
}
