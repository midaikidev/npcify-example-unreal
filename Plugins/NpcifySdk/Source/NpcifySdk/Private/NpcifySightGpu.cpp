#include "NpcifySightGpu.h"

#include "NpcifySightCapture.h"

#include "CommonRenderResources.h"
#include "GlobalShader.h"
#include "HAL/PlatformTime.h"
#include "Layout/ArrangedChildren.h"
#include "Layout/WidgetPath.h"
#include "PipelineStateCache.h"
#include "RHIStaticStates.h"
#include "RenderTargetPool.h"
#include "ScreenRendering.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

// The render-thread part of sight in Unreal: a scaled copy of the picture and its readback.

#if PLATFORM_APPLE
// Private/Apple/NpcifySightApple.mm: false when the texture is not in shared memory.
bool NpcifyReadSharedMetalTexture(void* NativeTexture, uint8* Out, int32 Width, int32 Height);
#endif

namespace
{
    void AddCycles(std::atomic<int64>& Sum, std::atomic<int64>& Longest, uint64 Start)
    {
        const int64 Cycles = static_cast<int64>(FPlatformTime::Cycles64() - Start);
        Sum += Cycles;
        int64 Previous = Longest.load();
        while (Cycles > Previous && !Longest.compare_exchange_weak(Previous, Cycles))
        {
        }
    }
}

namespace
{
    /** The sampler gives linear colour for an sRGB or a float texture: a JPEG needs the gamma back. */
    bool IsLinearSource(FRHITexture* Source)
    {
        const FRHITextureDesc& Desc = Source->GetDesc();
        return EnumHasAnyFlags(Desc.Flags, ETextureCreateFlags::SRGB) || IsFloatFormat(Desc.Format);
    }

    template <typename TPixelShader>
    void DrawScaled(FRHICommandListImmediate& RHICmdList, FNpcifySightGpu& Gpu, FRHITexture* Source, const FIntRect& SourceRect, const FIntPoint& Size)
    {
        FGraphicsPipelineStateInitializer PipelineState;
        RHICmdList.ApplyCachedRenderTargets(PipelineState);
        PipelineState.BlendState = TStaticBlendState<>::GetRHI();
        PipelineState.RasterizerState = TStaticRasterizerState<>::GetRHI();
        PipelineState.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
        FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
        TShaderMapRef<FScreenVS> VertexShader(ShaderMap);
        TShaderMapRef<TPixelShader> PixelShader(ShaderMap);
        PipelineState.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
        PipelineState.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
        PipelineState.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
        PipelineState.PrimitiveType = PT_TriangleList;
        SetGraphicsPipelineState(RHICmdList, PipelineState, 0);
        SetShaderParametersLegacyPS(RHICmdList, PixelShader, TStaticSamplerState<SF_Bilinear>::GetRHI(), Source);
        const float Width = static_cast<float>(Source->GetSizeX());
        const float Height = static_cast<float>(Source->GetSizeY());
        Gpu.Renderer->DrawRectangle(
            RHICmdList,
            0.0f, 0.0f, static_cast<float>(Size.X), static_cast<float>(Size.Y),
            SourceRect.Min.X / Width, SourceRect.Min.Y / Height,
            SourceRect.Width() / Width, SourceRect.Height() / Height,
            Size,
            FIntPoint(1, 1),
            VertexShader,
            EDRF_Default);
    }
}

void NpcifySightScaleAndRead(
    FRHICommandListImmediate& RHICmdList,
    FNpcifySightGpu& Gpu,
    FRHITexture* Source,
    FIntRect SourceRect,
    const FIntPoint& Size,
    int32 Generation,
    bool bLinearContent)
{
    const uint64 Start = FPlatformTime::Cycles64();
    const FIntRect Whole(FIntPoint::ZeroValue, FIntPoint(Source->GetSizeX(), Source->GetSizeY()));
    SourceRect.Clip(Whole);
    if (SourceRect.Width() <= 0 || SourceRect.Height() <= 0)
    {
        SourceRect = Whole;
    }
    if (!Gpu.Target.IsValid() || Gpu.Target->GetDesc().Extent != Size)
    {
        Gpu.Target.SafeRelease();
        const FPooledRenderTargetDesc Desc = FPooledRenderTargetDesc::Create2DDesc(
            Size, PF_B8G8R8A8, FClearValueBinding::None, TexCreate_None, TexCreate_RenderTargetable, false);
        GRenderTargetPool.FindFreeElement(RHICmdList, Desc, Gpu.Target, TEXT("NpcifySightScaled"));
    }
    FRHITexture* Destination = Gpu.Target->GetRHI();
    RHICmdList.Transition(FRHITransitionInfo(Destination, ERHIAccess::Unknown, ERHIAccess::RTV));
    FRHIRenderPassInfo PassInfo(Destination, ERenderTargetActions::DontLoad_Store);
    RHICmdList.BeginRenderPass(PassInfo, TEXT("NpcifySightScale"));
    RHICmdList.SetViewport(0.0f, 0.0f, 0.0f, static_cast<float>(Size.X), static_cast<float>(Size.Y), 1.0f);
    if (bLinearContent || IsLinearSource(Source))
    {
        DrawScaled<FScreenPSsRGBSource>(RHICmdList, Gpu, Source, SourceRect, Size);
    }
    else
    {
        DrawScaled<FScreenPS>(RHICmdList, Gpu, Source, SourceRect, Size);
    }
    RHICmdList.EndRenderPass();
    RHICmdList.Transition(FRHITransitionInfo(Destination, ERHIAccess::RTV, ERHIAccess::CopySrc));
    if (!Gpu.Readback.IsValid() || Gpu.ReadbackSize != Size)
    {
        // The staging texture keeps the size of its first copy.
        Gpu.Readback = MakeUnique<FRHIGPUTextureReadback>(TEXT("NpcifySightReadback"));
        Gpu.ReadbackSize = Size;
    }
    Gpu.Readback->EnqueueCopy(RHICmdList, Destination, FIntVector::ZeroValue, 0, FIntVector(Size.X, Size.Y, 1));
    Gpu.CopyGeneration = Generation;
    Gpu.bCopyPending = true;
    AddCycles(Gpu.ScaleCycles, Gpu.ScaleLongest, Start);
}

void NpcifySightPollReadback(FNpcifySightGpu& Gpu)
{
    if (!Gpu.bCopyPending || !Gpu.Readback.IsValid() || !Gpu.Readback->IsReady())
    {
        return;
    }
    const uint64 Start = FPlatformTime::Cycles64();
    const FIntPoint Size = Gpu.ReadbackSize;
#if PLATFORM_APPLE
    // The fence of the copy passed: a texture in shared memory needs no wait for the GPU.
    if (FRHITexture* Staging = Gpu.Readback->DestinationStagingTextures[Gpu.Readback->GetLastCopyGPUMask().GetFirstIndex()])
    {
        std::shared_ptr<FNpcifySightPicture> Shared = std::make_shared<FNpcifySightPicture>();
        Shared->Width = Size.X;
        Shared->Height = Size.Y;
        Shared->Bgra.resize(static_cast<size_t>(Size.X) * static_cast<size_t>(Size.Y) * 4);
        if (NpcifyReadSharedMetalTexture(Staging->GetNativeResource(), Shared->Bgra.data(), Size.X, Size.Y))
        {
            Gpu.bCopyPending = false;
            Gpu.Results.Enqueue(MakeTuple(Gpu.CopyGeneration, Shared));
            AddCycles(Gpu.MapCycles, Gpu.MapLongest, Start);
            return;
        }
    }
#endif
    int32 RowPitch = 0;
    int32 BufferHeight = 0;
    const uint8* Data = static_cast<const uint8*>(Gpu.Readback->Lock(RowPitch, &BufferHeight));
    std::shared_ptr<FNpcifySightPicture> Picture;
    if (Data && RowPitch >= Size.X && BufferHeight >= Size.Y)
    {
        Picture = std::make_shared<FNpcifySightPicture>();
        Picture->Width = Size.X;
        Picture->Height = Size.Y;
        Picture->Bgra.resize(static_cast<size_t>(Size.X) * static_cast<size_t>(Size.Y) * 4);
        for (int32 Row = 0; Row < Size.Y; ++Row)
        {
            FMemory::Memcpy(
                &Picture->Bgra[static_cast<size_t>(Row) * static_cast<size_t>(Size.X) * 4],
                Data + static_cast<size_t>(Row) * static_cast<size_t>(RowPitch) * 4,
                static_cast<size_t>(Size.X) * 4);
        }
    }
    Gpu.Readback->Unlock();
    Gpu.bCopyPending = false;
    Gpu.Results.Enqueue(MakeTuple(Gpu.CopyGeneration, Picture));
    AddCycles(Gpu.MapCycles, Gpu.MapLongest, Start);
}

FIntRect NpcifySightViewportRect(FSceneViewport& Viewport, const TSharedPtr<SWindow>& Window)
{
    FIntRect Rect(FIntPoint::ZeroValue, Viewport.GetSizeXY());
    const TSharedPtr<SViewport> Widget = Viewport.GetViewportWidget().Pin();
    if (!Widget.IsValid() || !Window.IsValid())
    {
        return Rect;
    }
    const FGeometry WindowGeometry = Window->GetWindowGeometryInWindow();
    FArrangedChildren JustWindow(EVisibility::Visible);
    JustWindow.AddWidget(FArrangedWidget(Window.ToSharedRef(), WindowGeometry));
    FWidgetPath Path(Window.ToSharedRef(), JustWindow);
    if (Path.ExtendPathTo(FWidgetMatcher(Widget.ToSharedRef()), EVisibility::Visible))
    {
        const FArrangedWidget Arranged = Path.FindArrangedWidget(Widget.ToSharedRef()).Get(FArrangedWidget::GetNullWidget());
        const FVector2D Position(Arranged.Geometry.GetAbsolutePosition());
        const FVector2D Extent(Arranged.Geometry.GetAbsoluteSize());
        Rect = FIntRect(
            FMath::RoundToInt(Position.X),
            FMath::RoundToInt(Position.Y),
            FMath::RoundToInt(Position.X + Extent.X),
            FMath::RoundToInt(Position.Y + Extent.Y));
    }
    return Rect;
}
