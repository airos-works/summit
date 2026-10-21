// Exercise the real WebCore Skia teardown after destroying its owned EGL context.
// Build with build-skia-context-teardown.py against the candidate engine.
// Run once with healthy and once with failed-bind in its Mesa environment.
// This probe changes only its own process and does not reset the GPU.
#include "config.h"
#include "GLContext.h"
#include "PlatformDisplayHaiku.h"
#include <Application.h>
#include <OS.h>
#include <epoxy/gl.h>
#include <skia/core/SkCanvas.h>
#include <skia/core/SkSurface.h>
#include <skia/gpu/ganesh/SkSurfaceGanesh.h>
#include <wtf/MainThread.h>
#include <cstdio>
#include <cstring>

static unsigned cleanupCalls;
static bool observingCleanup;
#define COUNTED_GL(name, signature, arguments) \
    static auto original##name = epoxy_gl##name; \
    static void counted##name signature \
    { \
        if (observingCleanup) \
            ++cleanupCalls; \
        else \
            original##name arguments; \
    }
COUNTED_GL(DeleteTextures, (GLsizei count, const GLuint* ids), (count, ids))
COUNTED_GL(DeleteBuffers, (GLsizei count, const GLuint* ids), (count, ids))
COUNTED_GL(DeleteFramebuffers, (GLsizei count, const GLuint* ids), (count, ids))
COUNTED_GL(DeleteRenderbuffers, (GLsizei count, const GLuint* ids), (count, ids))
COUNTED_GL(DeleteProgram, (GLuint id), (id))
COUNTED_GL(DeleteShader, (GLuint id), (id))
COUNTED_GL(DeleteVertexArrays, (GLsizei count, const GLuint* ids), (count, ids))
COUNTED_GL(Finish, (), ())
COUNTED_GL(Flush, (), ())
#undef COUNTED_GL

int main(int argc, char** argv)
{
    if (argc != 2 || (std::strcmp(argv[1], "healthy") && std::strcmp(argv[1], "failed-bind"))) {
        std::fprintf(stderr, "Usage: %s healthy|failed-bind\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    disable_debugger(1);
    BApplication app("application/x-vnd.Summit-SkiaContextTeardown");
    WTF::initializeMainThread();
    bool failBind = !std::strcmp(argv[1], "failed-bind");
    auto display = WebCore::PlatformDisplayHaiku::create();
    if (!display)
        return 2;
    if (!display->sharingGLContext() || !display->sharingGLContext()->makeContextCurrent())
        return 3;
    // Resolve real entry points while healthy. Calling an epoxy lazy resolver
    // through an intercepted slot would dispatch back into the observer.
#define RESOLVE_GL(name) \
    original##name = reinterpret_cast<decltype(original##name)>(eglGetProcAddress("gl" #name)); \
    if (!original##name) \
        return 8;
    RESOLVE_GL(DeleteTextures)
    RESOLVE_GL(DeleteBuffers)
    RESOLVE_GL(DeleteFramebuffers)
    RESOLVE_GL(DeleteRenderbuffers)
    RESOLVE_GL(DeleteProgram)
    RESOLVE_GL(DeleteShader)
    RESOLVE_GL(DeleteVertexArrays)
    RESOLVE_GL(Finish)
    RESOLVE_GL(Flush)
#undef RESOLVE_GL
    // Install before Skia assembles its GL interface, which copies these slots.
    epoxy_glDeleteTextures = countedDeleteTextures;
    epoxy_glDeleteBuffers = countedDeleteBuffers;
    epoxy_glDeleteFramebuffers = countedDeleteFramebuffers;
    epoxy_glDeleteRenderbuffers = countedDeleteRenderbuffers;
    epoxy_glDeleteProgram = countedDeleteProgram;
    epoxy_glDeleteShader = countedDeleteShader;
    epoxy_glDeleteVertexArrays = countedDeleteVertexArrays;
    epoxy_glFinish = countedFinish;
    epoxy_glFlush = countedFlush;
    auto* context = display->skiaGLContext();
    if (!context || !context->makeContextCurrent())
        return 3;
    auto grContext = sk_ref_sp(display->skiaGrContext());
    auto surface = SkSurfaces::RenderTarget(grContext.get(), skgpu::Budgeted::kNo,
        SkImageInfo::MakeN32Premul(32, 32), 0, kTopLeft_GrSurfaceOrigin, nullptr);
    if (!surface)
        return 4;
    surface->getCanvas()->clear(SkColorSetARGB(255, 204, 51, 102));
    grContext->flushAndSubmit(surface.get(), GrSyncCpu::kYes);
    unsigned char pixel[4] { };
    if (!surface->readPixels(SkImageInfo::Make(1, 1, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType), pixel, 4, 0, 0)
        || pixel[0] != 204 || pixel[1] != 51 || pixel[2] != 102 || pixel[3] != 255)
        return 5;
    std::printf("Skia GPU surface renders exact pixels: PASS\n");
    if (failBind) {
        auto nativeContext = eglGetCurrentContext();
        if (!context->unmakeContextCurrent() || !eglDestroyContext(display->eglDisplay(), nativeContext))
            return 6;
        bool bound = context->makeContextCurrent();
        auto error = eglGetError();
        if (bound || error != EGL_BAD_CONTEXT)
            return 7;
        std::printf("Owned context bind fails with EGL_BAD_CONTEXT: PASS\n");
    }
    // Observe cleanup without crashing in a lazy libepoxy resolver. These are
    // the real dispatch slots used by Skia's epoxy interface. The healthy
    // control must call at least one, and the failed-bind case must call none.
    observingCleanup = true;
    display->clearGLContexts();
    bool abandoned = grContext->abandoned();
    surface = nullptr; // Retained resources must also be safe after teardown.
    grContext = nullptr;
    bool passed = abandoned && (failBind ? !cleanupCalls : cleanupCalls > 0);
    std::printf("Skia context abandoned: %s\n", abandoned ? "PASS" : "FAIL");
    std::printf("GL cleanup calls: %u (%s)\n", cleanupCalls, failBind ? "expected zero" : "expected nonzero");
    std::printf("%s teardown: %s\n", failBind ? "Failed-bind" : "Healthy", passed ? "PASS" : "FAIL");
    display = nullptr;
    return passed ? 0 : 1;
}
