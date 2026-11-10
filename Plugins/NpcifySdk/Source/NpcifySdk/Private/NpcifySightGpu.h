#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "RHIGPUReadback.h"
#include "RendererInterface.h"

#include <atomic>
#include <memory>

class FSceneViewport;
class SWindow;
struct FNpcifySightPicture;

/** The render-thread state of a sight source: the scaled copy and its readback. */
struct FNpcifySightGpu
{
    // Render thread only.
    TRefCountPtr<IPooledRenderTarget> Target;
    TUniquePtr<FRHIGPUTextureReadback> Readback;
    FIntPoint ReadbackSize = FIntPoint::ZeroValue;
    int32 CopyGeneration = -1;
    bool bCopyPending = false;
    IRendererModule* Renderer = nullptr;

    // A copy of the back buffer that the game thread wants (the viewport source).
    FCriticalSection Lock;
    int32 WantGeneration = -1;
    FIntRect WantRect;
    FIntPoint WantSize = FIntPoint::ZeroValue;
    const void* WantWindow = nullptr;

    // Pictures for the game thread: the generation of the request and the picture (null: none).
    TQueue<TPair<int32, std::shared_ptr<FNpcifySightPicture>>, EQueueMode::Mpsc> Results;
    std::atomic<bool> bPollQueued{false};
    // Render-thread time of the scaled copies and of the maps of the readbacks, with the longest one.
    std::atomic<int64> ScaleCycles{0};
    std::atomic<int64> MapCycles{0};
    std::atomic<int64> ScaleLongest{0};
    std::atomic<int64> MapLongest{0};
};

/**
 * Draws a rectangle of Source into the scaled copy and starts its readback (render thread).
 * bLinearContent: the texture holds linear colour in any format, as a render target does (a canvas
 * or a scene capture writes linear values); the copy gets the gamma back. Else only an sRGB or a
 * float texture does, and other textures (the back buffer) are copied as they are.
 */
void NpcifySightScaleAndRead(
    FRHICommandListImmediate& RHICmdList,
    FNpcifySightGpu& Gpu,
    FRHITexture* Source,
    FIntRect SourceRect,
    const FIntPoint& Size,
    int32 Generation,
    bool bLinearContent);

/** Copies a finished readback to a picture for the game thread (render thread). */
void NpcifySightPollReadback(FNpcifySightGpu& Gpu);

/** The rectangle of the game viewport in its window, as FFrameGrabber finds it (game thread). */
FIntRect NpcifySightViewportRect(FSceneViewport& Viewport, const TSharedPtr<SWindow>& Window);
