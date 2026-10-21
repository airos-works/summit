// Exercise capability discovery across native context lifetimes (#100).
// Build with build-skia-context-teardown.py --source this-file.cpp.
#include "config.h"
#include <wtf/ThreadSafeRefCounted.h>
#include "GLContext.h"
#include "PlatformDisplayHaiku.h"
#include <Application.h>
#include <OS.h>
#include <epoxy/gl.h>
#include <wtf/MainThread.h>
#include <cstdio>
#include <cstring>

using namespace WebCore;
static bool passed = true;
static void check(const char* name, bool value)
{
    std::printf("%s: %s\n", name, value ? "PASS" : "FAIL");
    passed &= value;
}

static void checkContext(const char* name, GLContext& context)
{
    std::printf("Context: %s\n", name);
    if (!context.makeContextCurrent()) {
        check("Context binds", false);
        return;
    }
    const auto* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    const auto& cached = context.glExtensions();
    unsigned supported = 0;
    bool matches = true;
#define CHECK_EXTENSION(name) \
    { \
        bool actual = GLContext::isExtensionSupported(extensions, "GL_" #name); \
        supported += actual; \
        matches &= actual == cached.name; \
        std::printf("  %s: driver=%d cache=%d\n", #name, actual, cached.name); \
    }
    CHECK_EXTENSION(OES_texture_npot)
    CHECK_EXTENSION(EXT_unpack_subimage)
    CHECK_EXTENSION(APPLE_sync)
    CHECK_EXTENSION(OES_packed_depth_stencil)
    CHECK_EXTENSION(EXT_YUV_target)
#if USE(VULKAN)
    CHECK_EXTENSION(EXT_memory_object)
#endif
#undef CHECK_EXTENSION
    check("At least one tested extension is supported", supported);
    check("Cached capabilities match this native context", matches);
    check("Capability queries leave no GL error", glGetError() == GL_NO_ERROR);
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    disable_debugger(1);
    BApplication app("application/x-vnd.Summit-GLContextCapabilities");
    WTF::initializeMainThread();
    auto display = PlatformDisplayHaiku::create();
    if (!display)
        return 2;
    auto first = GLContext::createOffscreen(*display);
    auto sibling = GLContext::createOffscreen(*display);
    if (!first || !sibling)
        return 3;
    checkContext("first", *first);
    checkContext("sibling", *sibling);
    auto* siblingHandle = sibling->platformContext();
    first = nullptr;
    check("Destroying first preserves sibling binding", eglGetCurrentContext() == siblingHandle);
    checkContext("sibling after first is destroyed", *sibling);
    sibling = nullptr;
    display->clearGLContexts();
    auto replacement = GLContext::createOffscreen(*display);
    if (!replacement)
        return 4;
    checkContext("replacement in fresh share group", *replacement);
    replacement = nullptr;
    display->clearGLContexts();
    display->glDisplay().terminate();
    return passed ? 0 : 1;
}
