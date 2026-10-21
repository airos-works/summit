// Exercise root leases and Skia recovery on the threads that own each context.
// Build with build-skia-context-teardown.py --source this-file.cpp.
// This retires only the probe's share group; it does not reset the GPU.
#include "config.h"
#include "GLContext.h"
#include "PlatformDisplayHaiku.h"
#include <Application.h>
#include <OS.h>
#include <skia/core/SkCanvas.h>
#include <skia/core/SkSurface.h>
#include <skia/gpu/ganesh/SkSurfaceGanesh.h>
#include <wtf/MainThread.h>
#include <atomic>
#include <cstdio>
#include <thread>

using namespace WebCore;
static bool passed = true;
static void check(const char* name, bool value)
{
    std::printf("%s: %s\n", name, value ? "PASS" : "FAIL");
    passed &= value;
}

static bool rendersPixels(GrDirectContext& context)
{
    auto surface = SkSurfaces::RenderTarget(&context, skgpu::Budgeted::kNo,
        SkImageInfo::MakeN32Premul(32, 32), 0, kTopLeft_GrSurfaceOrigin, nullptr);
    if (!surface)
        return false;
    surface->getCanvas()->clear(SkColorSetARGB(255, 204, 51, 102));
    context.flushAndSubmit(surface.get(), GrSyncCpu::kYes);
    unsigned char pixel[4] { };
    return surface->readPixels(SkImageInfo::Make(1, 1, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType), pixel, 4, 0, 0)
        && pixel[0] == 204 && pixel[1] == 51 && pixel[2] == 102 && pixel[3] == 255;
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    disable_debugger(1);
    BApplication app("application/x-vnd.Summit-SharedContextRecovery");
    WTF::initializeMainThread();
    auto display = PlatformDisplayHaiku::create();
    if (!display)
        return 2;
    auto oldRoot = display->sharingGLContext();
    auto* mainContext = display->skiaGLContext();
    if (!oldRoot || !mainContext || !mainContext->makeContextCurrent())
        return 3;
    Ref oldMainState = mainContext->resourceState();
    auto oldMainSkia = sk_ref_sp(display->skiaGrContext());
    check("Main Skia context renders before loss", rendersPixels(*oldMainSkia));
    std::atomic<bool> workerReady { false }, resumeWorker { false };
    bool workerInitial = false, workerLost = false, workerAbandoned = false, workerReplaced = false, workerPixels = false;
    RefPtr<GLContextState> newWorkerState;
    std::thread worker([&] {
        auto* context = display->skiaGLContext();
        RefPtr<GLContextState> oldState;
        sk_sp<GrDirectContext> oldSkia;
        if (context && context->makeContextCurrent()) {
            oldState = &context->resourceState();
            oldSkia = sk_ref_sp(display->skiaGrContext());
            workerInitial = oldState->sharesObjectsWith(oldRoot->resourceState()) && rendersPixels(*oldSkia);
        }
        workerReady = true;
        while (!resumeWorker)
            std::this_thread::yield();
        if (oldState) {
            workerLost = oldState->isLost();
            context = display->skiaGLContext();
            workerAbandoned = oldSkia->abandoned();
            if (context && context->makeContextCurrent()) {
                newWorkerState = &context->resourceState();
                workerReplaced = !newWorkerState->isLost() && !newWorkerState->sharesObjectsWith(*oldState);
                workerPixels = rendersPixels(*display->skiaGrContext());
            }
        }
        oldSkia = nullptr;
        display->setSkiaGLContextForCurrentThread(nullptr);
    });
    while (!workerReady)
        std::this_thread::yield();
    oldRoot->resourceState().markLost();
    auto replacementRoot = display->sharingGLContext();
    check("Retained old root survives replacement", oldRoot->resourceState().isLost());
    check("Replacement root uses a healthy new share group", replacementRoot
        && !replacementRoot->resourceState().isLost()
        && !replacementRoot->resourceState().sharesObjectsWith(oldRoot->resourceState()));
    resumeWorker = true;
    mainContext = display->skiaGLContext();
    check("Old main Skia context is abandoned on its thread", oldMainSkia->abandoned());
    check("Main Skia context joins the replacement group", mainContext && replacementRoot
        && mainContext->resourceState().sharesObjectsWith(replacementRoot->resourceState())
        && !mainContext->resourceState().sharesObjectsWith(oldMainState));
    check("Main Skia context renders after loss", mainContext && mainContext->makeContextCurrent()
        && rendersPixels(*display->skiaGrContext()));
    worker.join();
    check("Worker Skia context renders before loss", workerInitial);
    check("Worker observes shared loss", workerLost);
    check("Old worker Skia context is abandoned on its thread", workerAbandoned);
    check("Worker replaces its context and renders after loss", workerReplaced && workerPixels);
    check("Main and worker use the same replacement group", mainContext && newWorkerState
        && mainContext->resourceState().sharesObjectsWith(*newWorkerState));
    oldMainSkia = nullptr;
    oldRoot = nullptr;
    replacementRoot = nullptr;
    display->clearGLContexts();
    display->glDisplay().terminate();
    return passed ? 0 : 1;
}
