#include "NpcifySightCapture.h"

#include "NpcifySightGpu.h"

#include "Async/Async.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Containers/Queue.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Texture.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "RendererInterface.h"
#include "Rendering/SlateRenderer.h"
#include "Slate/SceneViewport.h"
#include "RenderingThread.h"
#include "Slate/SlateViewportProvider.h"
#include "TextureResource.h"
#include "Widgets/SWindow.h"

namespace
{
    /** A picture of this age or younger serves a frame that comes right after a check. */
    constexpr double FreshSeconds = 0.1;
    /** A readback that gives nothing in this time (a hidden window, a texture without data) gives no picture. */
    constexpr double ReadTimeoutSeconds = 1.0;

    IImageWrapperModule* GImageWrappers = nullptr;

    TAutoConsoleVariable<FString> CVarSightSaveFolder(
        TEXT("npcify.Sight.SaveFolder"),
        FString(),
        TEXT("A folder for a check of what the character sees: the SDK saves each sight frame there as a JPEG. Empty: off."));

    std::atomic<int32> GSavedFrames{0};
}

FNpcifySightCapture::FNpcifySightCapture(UTexture* InTexture, USceneCaptureComponent2D* InSceneCapture, bool bInCamera)
    : Texture(InTexture)
    , SceneCapture(InSceneCapture)
    , bViewport(InTexture == nullptr && InSceneCapture == nullptr)
    , bCamera(bInCamera)
    , Gpu(MakeShared<FNpcifySightGpu, ESPMode::ThreadSafe>())
    , Costs(std::make_shared<FCosts>())
{
}

FNpcifySightCapture::~FNpcifySightCapture()
{
    Stop();
}

std::string FNpcifySightCapture::Kind() const
{
    return bCamera ? "camera" : "game";
}

bool FNpcifySightCapture::IsLive() const
{
    if (bStopped)
    {
        return false;
    }
    if (bViewport)
    {
        return GEngine != nullptr && GEngine->GameViewport != nullptr && GEngine->GameViewport->GetGameViewport() != nullptr;
    }
    if (SceneCapture.IsValid())
    {
        return SceneCapture->TextureTarget != nullptr;
    }
    return Texture.IsValid();
}

void FNpcifySightCapture::Open(std::function<void(const std::string&)> Done)
{
    const std::string Unavailable = bCamera ? Npcify::SightCodes::CameraUnavailable : Npcify::SightCodes::SourceUnavailable;
    if (!FApp::CanEverRender() || !IsLive() || (bViewport && !FSlateApplication::IsInitialized()))
    {
        Done(Unavailable);
        return;
    }
    // Modules load on the game thread; the render thread and the workers use the pointers.
    Gpu->Renderer = &FModuleManager::GetModuleChecked<IRendererModule>(TEXT("Renderer"));
    GImageWrappers = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    if (bViewport && !BackBufferHook.IsValid())
    {
        FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer();
        if (Renderer == nullptr)
        {
            Done(Unavailable);
            return;
        }
        TWeakPtr<FNpcifySightGpu, ESPMode::ThreadSafe> WeakGpu = Gpu;
        BackBufferHook = Renderer->OnBackBufferReadyToPresent().AddLambda(
            [WeakGpu](SWindow& Window, ISlateViewportProvider& Provider)
            {
                const TSharedPtr<FNpcifySightGpu, ESPMode::ThreadSafe> State = WeakGpu.Pin();
                if (!State.IsValid())
                {
                    return;
                }
                int32 Generation = -1;
                FIntRect Rect;
                FIntPoint Size;
                {
                    FScopeLock Lock(&State->Lock);
                    if (State->WantGeneration < 0 || &Window != State->WantWindow)
                    {
                        return;
                    }
                    Generation = State->WantGeneration;
                    Rect = State->WantRect;
                    Size = State->WantSize;
                    State->WantGeneration = -1;
                }
                FRHITexture* BackBuffer = Provider.GetBackBufferResource();
                if (BackBuffer == nullptr || State->Renderer == nullptr)
                {
                    State->Results.Enqueue(MakeTuple(Generation, std::shared_ptr<FNpcifySightPicture>()));
                    return;
                }
                NpcifySightScaleAndRead(FRHICommandListImmediate::Get(), *State, BackBuffer, Rect, Size, Generation, false);
            });
    }
    Done(std::string());
}

void FNpcifySightCapture::ReadLuminance(std::function<void(std::vector<uint8_t>)> Done)
{
    ++Grids;
    std::shared_ptr<FCosts> Counts = Costs;
    WithPicture([Done, Counts](std::shared_ptr<const FNpcifySightPicture> Picture)
    {
        if (!Picture)
        {
            Done(std::vector<uint8_t>());
            return;
        }
        AsyncTask(ENamedThreads::AnyThread, [Done, Picture, Counts]()
        {
            const uint64 Start = FPlatformTime::Cycles64();
            std::vector<uint8_t> Grid = Npcify::LumaGridOfBgra(Picture->Bgra.data(), Picture->Width, Picture->Height, Picture->Width);
            Counts->GridCycles += static_cast<int64>(FPlatformTime::Cycles64() - Start);
            Done(std::move(Grid));
        });
    });
}

void FNpcifySightCapture::Capture(std::function<void(std::shared_ptr<Npcify::FSightFrame>)> Done)
{
    std::shared_ptr<FCosts> Counts = Costs;
    WithPicture([Done, Counts](std::shared_ptr<const FNpcifySightPicture> Picture)
    {
        if (!Picture)
        {
            Done(nullptr);
            return;
        }
        const FString SaveFolder = CVarSightSaveFolder.GetValueOnGameThread();
        // JPEG encoding takes milliseconds: not on the game thread.
        AsyncTask(ENamedThreads::AnyThread, [Done, Picture, Counts, SaveFolder]()
        {
            const uint64 Start = FPlatformTime::Cycles64();
            std::shared_ptr<Npcify::FSightFrame> Frame = Encode(*Picture);
            Counts->JpegCycles += static_cast<int64>(FPlatformTime::Cycles64() - Start);
            if (Frame)
            {
                ++Counts->Frames;
                Counts->FrameBytes += static_cast<int64>(Frame->Jpeg.size());
                if (!SaveFolder.IsEmpty())
                {
                    const FString Path = FPaths::Combine(SaveFolder, FString::Printf(TEXT("sight-%03d.jpg"), ++GSavedFrames));
                    FFileHelper::SaveArrayToFile(TArrayView64<const uint8>(Frame->Jpeg.data(), static_cast<int64>(Frame->Jpeg.size())), *Path);
                }
            }
            Done(Frame);
        });
    });
}

void FNpcifySightCapture::Stop()
{
    if (bStopped)
    {
        return;
    }
    bStopped = true;
    RemoveBackBufferHook();
    ++Generation;
    Deliver(nullptr);
    if (Gpu.IsValid())
    {
        RenderCycles[0] = Gpu->ScaleCycles.load();
        RenderCycles[1] = Gpu->ScaleLongest.load();
        RenderCycles[2] = Gpu->MapCycles.load();
        RenderCycles[3] = Gpu->MapLongest.load();
    }
    // The render thread releases its copy after the commands that use it.
    TSharedPtr<FNpcifySightGpu, ESPMode::ThreadSafe> Released = MoveTemp(Gpu);
    if (Released.IsValid())
    {
        ENQUEUE_RENDER_COMMAND(NpcifySightRelease)([Released](FRHICommandListImmediate&) mutable
        {
            Released->Readback.Reset();
            Released->Target.SafeRelease();
        });
    }
}

void FNpcifySightCapture::Tick()
{
    if (!Gpu.IsValid())
    {
        return;
    }
    const uint64 Start = FPlatformTime::Cycles64();
    TPair<int32, std::shared_ptr<FNpcifySightPicture>> Result;
    while (Gpu->Results.Dequeue(Result))
    {
        if (bReading && Result.Key == Generation)
        {
            if (Result.Value)
            {
                Result.Value->ReadAt = FPlatformTime::Seconds();
            }
            Deliver(Result.Value);
        }
    }
    if (bReading)
    {
        if (FPlatformTime::Seconds() - ReadStartedAt > ReadTimeoutSeconds)
        {
            ++Generation;
            Deliver(nullptr);
        }
        else if (!Gpu->bPollQueued.exchange(true))
        {
            ENQUEUE_RENDER_COMMAND(NpcifySightPoll)([State = Gpu](FRHICommandListImmediate&)
            {
                NpcifySightPollReadback(*State);
                State->bPollQueued = false;
            });
        }
    }
    GameCycles += static_cast<int64>(FPlatformTime::Cycles64() - Start);
}

FString FNpcifySightCapture::Describe() const
{
    const double GameMs = FPlatformTime::ToMilliseconds64(GameCycles);
    const bool bLive = Gpu.IsValid();
    const double ScaleMs = FPlatformTime::ToMilliseconds64(bLive ? Gpu->ScaleCycles.load() : RenderCycles[0]);
    const double ScaleLongestMs = FPlatformTime::ToMilliseconds64(bLive ? Gpu->ScaleLongest.load() : RenderCycles[1]);
    const double MapMs = FPlatformTime::ToMilliseconds64(bLive ? Gpu->MapCycles.load() : RenderCycles[2]);
    const double MapLongestMs = FPlatformTime::ToMilliseconds64(bLive ? Gpu->MapLongest.load() : RenderCycles[3]);
    const double GridMs = FPlatformTime::ToMilliseconds64(Costs->GridCycles.load());
    const double JpegMs = FPlatformTime::ToMilliseconds64(Costs->JpegCycles.load());
    const int32 Frames = Costs->Frames.load();
    const int64 FrameBytes = Costs->FrameBytes.load();
    return FString::Printf(
        TEXT("%d readbacks (game thread %.1f us; render thread: scaled copy %.1f us, longest %.1f us; map %.1f us, longest %.1f us), %d grids (worker %.1f us each), %d frames (worker JPEG %.2f ms each, %lld bytes mean)"),
        Readbacks,
        Readbacks > 0 ? GameMs * 1000.0 / Readbacks : 0.0,
        Readbacks > 0 ? ScaleMs * 1000.0 / Readbacks : 0.0,
        ScaleLongestMs * 1000.0,
        Readbacks > 0 ? MapMs * 1000.0 / Readbacks : 0.0,
        MapLongestMs * 1000.0,
        Grids,
        Grids > 0 ? GridMs * 1000.0 / Grids : 0.0,
        Frames,
        Frames > 0 ? JpegMs / Frames : 0.0,
        static_cast<long long>(Frames > 0 ? FrameBytes / Frames : 0));
}

std::shared_ptr<Npcify::FSightFrame> FNpcifySightCapture::Encode(const FNpcifySightPicture& Picture)
{
    IImageWrapperModule* Module = GImageWrappers;
    if (Module == nullptr || Picture.Width <= 0 || Picture.Height <= 0)
    {
        return nullptr;
    }
    const TSharedPtr<IImageWrapper> Wrapper = Module->CreateImageWrapper(EImageFormat::JPEG);
    if (!Wrapper.IsValid() ||
        !Wrapper->SetRaw(Picture.Bgra.data(), static_cast<int64>(Picture.Bgra.size()), Picture.Width, Picture.Height, ERGBFormat::BGRA, 8))
    {
        return nullptr;
    }
    TArray64<uint8> Jpeg = Wrapper->GetCompressed(Npcify::FSightLimits::JpegQuality);
    if (Jpeg.Num() > Npcify::FSightLimits::MaxFrameBytes)
    {
        Jpeg = Wrapper->GetCompressed(Npcify::FSightLimits::SmallerJpegQuality);
    }
    if (Jpeg.Num() == 0 || Jpeg.Num() > Npcify::FSightLimits::MaxFrameBytes)
    {
        return nullptr;
    }
    std::shared_ptr<Npcify::FSightFrame> Frame = std::make_shared<Npcify::FSightFrame>();
    Frame->Jpeg.assign(Jpeg.GetData(), Jpeg.GetData() + Jpeg.Num());
    Frame->Width = Picture.Width;
    Frame->Height = Picture.Height;
    return Frame;
}

void FNpcifySightCapture::WithPicture(FUse Use)
{
    if (bStopped || !IsLive())
    {
        Use(nullptr);
        return;
    }
    if (Latest && FPlatformTime::Seconds() - Latest->ReadAt <= FreshSeconds)
    {
        Use(Latest);
        return;
    }
    Waiting.push_back(std::move(Use));
    if (!bReading)
    {
        StartReadback();
    }
}

void FNpcifySightCapture::StartReadback()
{
    const uint64 Start = FPlatformTime::Cycles64();
    bReading = true;
    ReadStartedAt = FPlatformTime::Seconds();
    ++Generation;
    ++Readbacks;
    const int32 Request = Generation;
    if (bViewport)
    {
        UGameViewportClient* Client = GEngine != nullptr ? GEngine->GameViewport.Get() : nullptr;
        FSceneViewport* Viewport = Client != nullptr ? Client->GetGameViewport() : nullptr;
        const TSharedPtr<SWindow> Window = Client != nullptr ? Client->GetWindow() : nullptr;
        if (Viewport == nullptr || !Window.IsValid())
        {
            Deliver(nullptr);
            return;
        }
        const FIntRect Rect = NpcifySightViewportRect(*Viewport, Window);
        FIntPoint Size;
        Npcify::FitCaptureSize(Rect.Width(), Rect.Height(), Size.X, Size.Y);
        FScopeLock Lock(&Gpu->Lock);
        Gpu->WantGeneration = Request;
        Gpu->WantRect = Rect;
        Gpu->WantSize = Size;
        Gpu->WantWindow = Window.Get();
        GameCycles += static_cast<int64>(FPlatformTime::Cycles64() - Start);
        return;
    }
    USceneCaptureComponent2D* Capture = SceneCapture.Get();
    UTexture* Source = Capture != nullptr ? static_cast<UTexture*>(Capture->TextureTarget) : Texture.Get();
    if (Capture != nullptr && Source != nullptr && !Capture->bCaptureEveryFrame && !Capture->bCaptureOnMovement)
    {
        // A capture that does not render every frame renders now, before the copy.
        Capture->CaptureScene();
    }
    FTextureResource* Resource = Source != nullptr ? Source->GetResource() : nullptr;
    if (Resource == nullptr)
    {
        Deliver(nullptr);
        return;
    }
    FIntPoint Size;
    Npcify::FitCaptureSize(
        FMath::RoundToInt(Source->GetSurfaceWidth()),
        FMath::RoundToInt(Source->GetSurfaceHeight()),
        Size.X,
        Size.Y);
    // A render target holds linear colour in every format (measured in UE 5.8: a canvas and a scene
    // capture, also SCS_FinalColorLDR, into RTF_RGBA8 and RTF_RGBA16f).
    const bool bLinearContent = Source->IsA<UTextureRenderTarget>();
    ENQUEUE_RENDER_COMMAND(NpcifySightScale)([State = Gpu, Resource, Size, Request, bLinearContent](FRHICommandListImmediate& RHICmdList)
    {
        FRHITexture* RHITexture = Resource->GetTextureRHI();
        if (RHITexture == nullptr || State->Renderer == nullptr)
        {
            State->Results.Enqueue(MakeTuple(Request, std::shared_ptr<FNpcifySightPicture>()));
            return;
        }
        NpcifySightScaleAndRead(
            RHICmdList, *State, RHITexture, FIntRect(0, 0, RHITexture->GetSizeX(), RHITexture->GetSizeY()), Size, Request, bLinearContent);
    });
    GameCycles += static_cast<int64>(FPlatformTime::Cycles64() - Start);
}

void FNpcifySightCapture::Deliver(std::shared_ptr<const FNpcifySightPicture> Picture)
{
    bReading = false;
    if (Picture)
    {
        Latest = Picture;
    }
    std::vector<FUse> Uses;
    Uses.swap(Waiting);
    for (FUse& Use : Uses)
    {
        Use(Picture);
    }
}

void FNpcifySightCapture::RemoveBackBufferHook()
{
    if (BackBufferHook.IsValid() && FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer() != nullptr)
    {
        FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().Remove(BackBufferHook);
    }
    BackBufferHook.Reset();
}
