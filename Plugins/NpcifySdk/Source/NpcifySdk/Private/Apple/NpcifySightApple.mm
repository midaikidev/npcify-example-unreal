#include "CoreMinimal.h"

// The system headers define a struct FVector, as Mac/MacSystemIncludes.h of the engine notes.
#include "Apple/PreAppleSystemHeaders.h"
#define FVector FVectorWorkaround
#import <Metal/Metal.h>
#undef FVector
#include "Apple/PostAppleSystemHeaders.h"

// Sight on Apple GPUs: Unreal's Metal RHI waits until the GPU is idle when it maps a readback (a
// render-thread stall of about one GPU frame). A CPU-readback texture of an Apple GPU is in shared
// memory, so after the fence of its copy the SDK reads it at once.

bool NpcifyReadSharedMetalTexture(void* NativeTexture, uint8* Out, int32 Width, int32 Height)
{
    id<MTLTexture> Texture = (__bridge id<MTLTexture>)NativeTexture;
    if (Texture == nil || Out == nullptr || Width <= 0 || Height <= 0 ||
        Texture.storageMode != MTLStorageModeShared ||
        Texture.pixelFormat != MTLPixelFormatBGRA8Unorm ||
        static_cast<int32>(Texture.width) < Width || static_cast<int32>(Texture.height) < Height)
    {
        return false;
    }
    [Texture getBytes:Out
          bytesPerRow:static_cast<NSUInteger>(Width) * 4
           fromRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(Width), static_cast<NSUInteger>(Height))
          mipmapLevel:0];
    return true;
}
