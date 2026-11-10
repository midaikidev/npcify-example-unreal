#pragma once

#include "CoreMinimal.h"
#include "Core/NpcifySight.h"
#include "UObject/WeakObjectPtr.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class UTexture;
class USceneCaptureComponent2D;
struct FNpcifySightGpu;

/** One picture of a sight source: BGRA pixels (Unreal's FColor) that fit inside 640 x 360. */
struct FNpcifySightPicture
{
    std::vector<uint8_t> Bgra;
    int32 Width = 0;
    int32 Height = 0;
    /** FPlatformTime::Seconds when the picture reached the game thread. */
    double ReadAt = 0.0;
};

/**
 * The pictures of sight in Unreal: the game viewport (its back buffer, with the HUD), a render
 * target, the target of a scene capture, or another texture. The render thread scales the
 * picture to fit inside 640 x 360 and FRHIGPUTextureReadback copies it to the CPU, without a
 * flush of the game or render thread. A worker thread makes the luma grid and the JPEG
 * (IImageWrapper, quality 70, then 50 once above 200 KiB). Call Tick on the game thread.
 */
class FNpcifySightCapture final : public Npcify::ISightSource, public std::enable_shared_from_this<FNpcifySightCapture>
{
public:
    /** The game viewport: nothing set. A texture source: Texture (a render target or a media texture) or SceneCapture. */
    FNpcifySightCapture(UTexture* Texture, USceneCaptureComponent2D* SceneCapture, bool bCamera);
    ~FNpcifySightCapture() override;

    std::string Kind() const override;
    bool IsLive() const override;
    void Open(std::function<void(const std::string&)> Done) override;
    void ReadLuminance(std::function<void(std::vector<uint8_t>)> Done) override;
    void Capture(std::function<void(std::shared_ptr<Npcify::FSightFrame>)> Done) override;
    void Stop() override;

    /** Moves the readback on: call it on the game thread every frame. */
    void Tick();
    /** The counts and the mean costs of this source, for the log. */
    FString Describe() const;

    /** The JPEG of BGRA pixels: quality 70, then 50 once when it is above 200 KiB. Null when it fails. */
    static std::shared_ptr<Npcify::FSightFrame> Encode(const FNpcifySightPicture& Picture);

private:
    using FUse = std::function<void(std::shared_ptr<const FNpcifySightPicture>)>;

    /** Calls Use with the current picture (a fresh one is shared), or with null when there is none. */
    void WithPicture(FUse Use);
    void StartReadback();
    void Deliver(std::shared_ptr<const FNpcifySightPicture> Picture);
    void RemoveBackBufferHook();

    TWeakObjectPtr<UTexture> Texture;
    TWeakObjectPtr<USceneCaptureComponent2D> SceneCapture;
    const bool bViewport;
    const bool bCamera;
    bool bStopped = false;
    /** The render-thread state: the scaled copy and its readback. */
    TSharedPtr<FNpcifySightGpu, ESPMode::ThreadSafe> Gpu;
    FDelegateHandle BackBufferHook;
    std::vector<FUse> Waiting;
    bool bReading = false;
    double ReadStartedAt = 0.0;
    int32 Generation = 0;
    std::shared_ptr<const FNpcifySightPicture> Latest;

    /** The costs for Describe; the workers add to them. */
    struct FCosts
    {
        std::atomic<int64> GridCycles{0};
        std::atomic<int64> JpegCycles{0};
        std::atomic<int32> Frames{0};
        std::atomic<int64> FrameBytes{0};
    };
    std::shared_ptr<FCosts> Costs;
    /** Game-thread time of the readback requests, the polls and the deliveries. */
    int64 GameCycles = 0;
    /** Render-thread times (scaled copies, their longest, maps, their longest), kept when the render state goes. */
    int64 RenderCycles[4] = {0, 0, 0, 0};
    int32 Readbacks = 0;
    int32 Grids = 0;
};
