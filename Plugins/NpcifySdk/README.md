# Npcify Unreal SDK (`NpcifySdk`)

Unreal Engine 5 C++ & Blueprint plugin (`NpcifySdk.uplugin`) for embedding Npcify AI characters into Unreal Engine 5.3+ 3D games.

## Features

- **NPCP v1.0 Binary Performance Codec (`FNpcifyProtocol`, `FNpcifySchemaMapper`)**: Native C++17 implementation of the 24-byte NPCP envelope, IEEE 802.3 CRC-32 validation, bounded GZip decompression, and strict MessagePack wire validation.
- **Offline Baked Conversations (`FNpcifyBakedConversation`)**: Load `.npcp` baked conversation assets (`npcify.baked-conversation`) with embedded WAV/PCM audio, cycle/parent/option validation, and instant offline branching (`Start()`, `Select()`, `Reply()`, `Worldline()`).
- **Layered Facial Expression & Viseme Mixer (`FNpcifyFacialMixer`, `FNpcifyVisemePlayer`)**: Multi-layer blending (`Emotion`, `Viseme`, `Blink`, `Gaze`), exponential smoothing, automatic natural blinking, coarticulated viseme scheduling, and automatic alias resolution across VRM 1.0 (`aa`, `ih`, `ou`, `ee`, `oh`, `happy`), VRM 0.x (`A`, `I`, `U`, `E`, `O`, `Joy`), and ARKit/MetaHuman morph targets (`jawOpen`, `mouthSmileLeft`, `eyeBlinkLeft`, `Fcl_MTH_A`).
- **Unreal `UActorComponent` Integration (`UNpcifyCharacterComponent`)**:
  - Blueprint & C++ component (`Add Component > Npcify Character`).
  - Automatic binding to owner `USkeletalMeshComponent` morph targets and 3D spatial `UAudioComponent` (`USoundWaveProcedural`). See [Spatial Voice](#spatial-voice).
  - Fine-grained control: `LookAtActor`, `LookAtComponent`, `LookAtLocation`, `ClearLookAt`, `PlayAnimation`, `SetEmotion`, `PlayExpressionCue`, `Speak`, `Reply`, `StartBakedConversation`, `GetBoneTransform`, `GetMouthOpenness`.

## Installation

Download the latest plugin archive from `https://npcify.ai/sdk/unreal/latest` (or a specific version from `https://npcify.ai/sdk/unreal/app/0.2.1`) and extract it into your Unreal project's `Plugins/NpcifySdk` directory:

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

## Speech Audio and Conversations

- Speech audio arrives as MPEG bytes (baked conversations) or as a signed URL
  (live stagings). The component downloads URL audio before the staging starts
  and decodes MPEG with the bundled [minimp3](https://github.com/lieff/minimp3)
  (CC0) into a `USoundWaveProcedural`. WAV and raw PCM bytes also play.
- Live requests send the same JSON bodies as the Web and Unity SDKs and accept
  NPCP responses. Retries keep one RFC 4122 `Idempotency-Key`.
- `ResetConversation()` stops speech and ignores late responses of the previous
  conversation. `OnConversationEnded` fires when the player picks an ending
  option of a baked conversation.
- `OnAnimationRequested(AnimationId, bLoop)` reports the animation cues of a
  staging; play them with your own animation assets.

## Spatial Voice

The voice is 3D by default (`bSpatialAudio`): it comes from the character, with
left/right panning and distance falloff. Unreal plays a sound without attenuation
settings in 2D, so the SDK gives the voice its own attenuation: linear falloff,
full volume within `VoiceInnerRadius` (250 cm), and silence `VoiceFalloffDistance`
(2750 cm) after that.

- Set `VoiceAttenuation` to use a `USoundAttenuation` asset of your game
  (for example with a binaural spatialization plugin, occlusion or reverb sends).
- An `UAudioComponent` that the owner actor already has keeps its own
  attenuation settings.
- Turn `bSpatialAudio` off for a 2D voice, for example in a menu or a phone call.
- After you change these properties at run time, call `ApplyVoiceSpatialization()`.

Unreal hears from the camera by default. In a third-person game, call
`APlayerController::SetAudioListenerAttenuationOverride` with the player character
so that the distance to the player sets the volume, while the camera direction
sets the panning.

## Example Project

[midaikidev/npcify-example-unreal](https://github.com/midaikidev/npcify-example-unreal)
is a third-person UE 5.8 game with a baked offline NPC and an online NPC. It
imports the VRM models with VRM4U and has an RPG dialogue UI in C++.

## Running Standalone C++ Tests

```bash
c++ -std=c++17 -O2 -Iclients/ue-client/Source/NpcifySdk/Public \
  clients/ue-client/Source/NpcifySdk/Private/Core/*.cpp \
  clients/ue-client/Tests/NpcifySdkTests.cpp \
  -lz -o /tmp/npcify-ue-tests && /tmp/npcify-ue-tests .
```
