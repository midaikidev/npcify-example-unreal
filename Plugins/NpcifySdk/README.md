# Npcify Unreal SDK (`NpcifySdk`)

Unreal Engine 5 C++ & Blueprint plugin (`NpcifySdk.uplugin`) for embedding Npcify AI characters into Unreal Engine 5.3+ 3D games.

## Features

- **NPCP v1.0 Binary Performance Codec (`FNpcifyProtocol`, `FNpcifySchemaMapper`)**: Native C++17 implementation of the 24-byte NPCP envelope, IEEE 802.3 CRC-32 validation, bounded GZip decompression, and strict MessagePack wire validation.
- **Offline Baked Conversations (`FNpcifyBakedConversation`)**: Load `.npcp` baked conversation assets (`npcify.baked-conversation`) with embedded WAV/PCM audio, cycle/parent/option validation, and instant offline branching (`Start()`, `Select()`, `Reply()`, `Worldline()`).
- **Layered Facial Expression & Viseme Mixer (`FNpcifyFacialMixer`, `FNpcifyVisemePlayer`)**: Multi-layer blending (`Emotion`, `Viseme`, `Blink`, `Gaze`), exponential smoothing, automatic natural blinking, coarticulated viseme scheduling, and automatic alias resolution across VRM 1.0 (`aa`, `ih`, `ou`, `ee`, `oh`, `happy`), VRM 0.x (`A`, `I`, `U`, `E`, `O`, `Joy`), and ARKit/MetaHuman morph targets (`jawOpen`, `mouthSmileLeft`, `eyeBlinkLeft`, `Fcl_MTH_A`).
- **Unreal `UActorComponent` Integration (`UNpcifyCharacterComponent`)**:
  - Blueprint & C++ component (`Add Component > Npcify Character`).
  - Automatic binding to owner `USkeletalMeshComponent` morph targets and 3D spatial `UAudioComponent` (`USoundWaveProcedural`).
  - Fine-grained control: `LookAtActor`, `LookAtComponent`, `LookAtLocation`, `ClearLookAt`, `PlayAnimation`, `SetEmotion`, `PlayExpressionCue`, `Speak`, `Reply`, `StartBakedConversation`, `GetBoneTransform`, `GetMouthOpenness`.

## Installation

Download the latest plugin archive from `https://npcify.ai/sdk/unreal/latest` (or a specific version from `https://npcify.ai/sdk/unreal/app/0.1.0`) and extract it into your Unreal project's `Plugins/NpcifySdk` directory:

```bash
mkdir -p Plugins/NpcifySdk
curl -fL https://npcify.ai/sdk/unreal/latest | tar -xz -C Plugins/NpcifySdk --strip-components=1
```

Then add `"NpcifySdk"` to `PublicDependencyModuleNames` in your game module's `.Build.cs`.

## Quick Start (C++)

```cpp
#include "NpcifyCharacterComponent.h"

void AMyNpcCharacter::BeginPlay()
{
    Super::BeginPlay();

    NpcifyCharacter->Configure(
        TEXT("https://api.npcify.ai"),
        TEXT("<character-id>"),
        TEXT("<paste-your-scoped-api-key>")
    );

    NpcifyCharacter->RefreshAvatarBindings();
    NpcifyCharacter->LookAtActor(GetWorld()->GetFirstPlayerController()->GetPawn());
    NpcifyCharacter->SetEmotion(TEXT("happy"), 0.75f);
    NpcifyCharacter->PlayAnimation(TEXT("Greeting"), false);
    NpcifyCharacter->Speak(TEXT("Welcome to the citadel!"));
}
```

## Running Standalone C++ Tests

```bash
c++ -std=c++17 -O2 -Iclients/ue-client/Source/NpcifySdk/Public \
  clients/ue-client/Source/NpcifySdk/Private/Core/*.cpp \
  clients/ue-client/Tests/NpcifySdkTests.cpp \
  -lz -o /tmp/npcify-ue-tests && /tmp/npcify-ue-tests .
```
