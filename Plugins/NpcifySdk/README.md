# Npcify Unreal SDK (`NpcifySdk`)

Unreal Engine 5 C++ & Blueprint plugin (`NpcifySdk.uplugin`) for embedding Npcify AI characters into Unreal Engine 5.3+ 3D games.

## Features

- **NPCP v1.0 Binary Performance Codec (`FNpcifyProtocol`, `FNpcifySchemaMapper`)**: Native C++17 implementation of the 24-byte NPCP envelope, IEEE 802.3 CRC-32 validation, bounded GZip decompression, and strict MessagePack wire validation.
- **Offline Baked Conversations (`FNpcifyBakedConversation`)**: Load `.npcp` baked conversation assets (`npcify.baked-conversation`) with embedded WAV/PCM audio, cycle/parent/option validation, and instant offline branching (`Start()`, `Select()`, `Reply()`, `Worldline()`).
- **Layered Facial Expression & Viseme Mixer (`FNpcifyFacialMixer`, `FNpcifyVisemePlayer`)**: Multi-layer blending (`Emotion`, `Viseme`, `Blink`, `Gaze`), exponential smoothing, automatic natural blinking, coarticulated viseme scheduling, and automatic alias resolution across VRM 1.0 (`aa`, `ih`, `ou`, `ee`, `oh`, `happy`), VRM 0.x (`A`, `I`, `U`, `E`, `O`, `Joy`), and ARKit/MetaHuman morph targets (`jawOpen`, `mouthSmileLeft`, `eyeBlinkLeft`, `Fcl_MTH_A`).
- **Unreal `UActorComponent` Integration (`UNpcifyCharacterComponent`)**:
  - Blueprint & C++ component (`Add Component > Npcify Character`).
  - Automatic binding to owner `USkeletalMeshComponent` morph targets and 3D spatial `UAudioComponent` (`USoundWaveProcedural`). See [Spatial Voice](#spatial-voice).
  - Voice input: push-to-talk with a key or your own input, or listening started by code. See [Voice Input](#voice-input).
  - Live motion: the generated body motion of each reply on a VRM skeleton, and `PrepareLiveMotion()`. See [Live Motion](#live-motion).
  - Fine-grained control: `LookAtActor`, `LookAtComponent`, `LookAtLocation`, `ClearLookAt`, `PlayAnimation`, `SetEmotion`, `PlayExpressionCue`, `Speak`, `Reply`, `StartBakedConversation`, `GetBoneTransform`, `GetMouthOpenness`.

## Installation

Download the latest plugin archive from `https://npcify.ai/sdk/unreal/latest` (or a specific version from `https://npcify.ai/sdk/unreal/app/0.5.0`) and extract it into your Unreal project's `Plugins/NpcifySdk` directory:

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

## Voice Input

The player can talk to the character when its owner turned voice input on
(the STT tab of the character in the dashboard; see
[voice input](../../docs/voice-input.md)). The microphone records while the
player holds a key or a button; at the release the recording goes to the
server, and its text becomes the player's message. The component reads the
character's info when the conversation starts; with voice input off,
listening fails with `OnError` and nothing is recorded or sent.

- **Trigger:** set `PushToTalkKey` (for example `V` or a gamepad button), or
  call `BeginPushToTalk()` and `EndPushToTalk()` from your own input, for
  example an Enhanced Input action or a UMG button. A press shorter than 0.3 s
  sends nothing.
- **By code:** `StartListening(MaxDurationSeconds, EndOnSilenceSeconds, bSend)`
  starts recording. Listening ends with `StopListening()`, after
  `MaxDurationSeconds` (30 s at most), or after `EndOnSilenceSeconds` of
  silence that follows speech (0: off). `CancelListening()` discards the
  recording. Example: listen when the character stops speaking.

  ```cpp
  // In the header: UFUNCTION() void HandleSpeechEnd(const FString& StagingId, const FString& Text);
  NpcifyCharacter->OnSpeechEnd.AddDynamic(this, &AMyNpcCharacter::HandleSpeechEnd);

  void AMyNpcCharacter::HandleSpeechEnd(const FString& StagingId, const FString& Text)
  {
      NpcifyCharacter->StartListening(10.0f, 0.8f, true);
  }
  ```

- **Events:** `OnListeningStateChanged` gives `Idle`, `Starting`, `Listening`
  and `Recognizing`. `OnTranscript(Text, bSent)` gives the recognised text;
  `bSent` is true when it became the player's message. An empty text means
  that no speech was heard; nothing is sent. With `bSend` false, show the text
  first and send it with `SendTranscript(Text)`.
- **Input modes:** `InputModes` (Details panel, or `inputModes` in
  `ConfigureFromJson`) is a list of `Options`, `Text` and `Voice`; one mode
  forces it. Empty: every mode that the character permits. A baked
  conversation permits only `Options`, a character without custom replies
  permits only `Options`, and `Voice` needs the character's voice input;
  another configuration fails with `OnError` when the conversation starts. With `InputModes` set, `Speak` needs `Text`, `Reply`
  needs `Options` or `Text`, and listening needs `Voice`.
- **Language:** `SpeechLanguage` is an optional hint (`ja`, `zh`, `en`). Without
  it the server detects the language.
- **Microphone:** the SDK records the default input device with the engine's
  `AudioCapture` plugin, which the plugin descriptor enables. The SDK does not
  ask for the microphone permission: on Android add `RECORD_AUDIO` and ask for
  it at run time, and on iOS set the microphone usage description in the
  project settings.
- **Upload:** at the release the SDK sends one WAV file (16 kHz mono) to
  `POST /conversations/:id/speech`, then the text as a turn. The server charges
  the recognition to the character owner at the provider's price.

## Live Motion

When the owner of a character turns **Live motion** on, the server generates
the body motion of each reply and streams it as pose frames
([real-time motion](../../docs/realtime-motion.md)). The component plays them
on the skeletal mesh of the owner; for any other character it keeps the
prepared animation clips (`OnAnimationRequested`).

- **Mesh:** the first skeletal mesh of the owner, or `LiveMotionMesh`. The SDK
  needs the VRM humanoid bones: it reads the table that VRM4U stores with the
  imported mesh, or you set `LiveMotionBones` (VRM bone such as `hips` or
  `leftUpperArm` to skeleton bone). The reference pose must be the VRM T-pose.
- **Animation:** play idle and other animations as before (an Animation
  Blueprint, `PlayAnimation`, montages). After the animation of each frame the
  component sets the 22 bones of the live pose with FK (in over 0.3 s, out
  over 0.5 s); hair, fingers and attachments follow their parents.
- **Warm-up:** call `PrepareLiveMotion()` when the player shows that they will
  reply, for example when the reply box gets focus or the player opens the
  conversation with a talk key. Push-to-talk calls it at the press. It runs once
  per conversation and costs the character owner only when the character has
  live motion on.
- **Options:** `bLiveMotion` turns it off; `LiveMotionRootMode` `World` keeps
  the generated hips movement (default `Anchored`: the character stays where
  you put it).
- **State:** `IsLiveMotionReady()`, `GetLiveMotionWeight()`,
  `GetLiveMotionStats(StagingId, ...)` (frames received and played, the staging
  time of the first played frame), `OnLiveMotionFrames` and
  `GetHumanoidBoneName(VrmBone)`.
- **Transport:** the frames come on the conversation stream (Socket.IO over the
  engine's `WebSockets` module).

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

The tests read fixtures from `packages/ts-shared/compat`; pass the repository
root.

```bash
c++ -std=c++17 -O2 -Iclients/ue-client/Source/NpcifySdk/Public \
  clients/ue-client/Source/NpcifySdk/Private/Core/*.cpp \
  clients/ue-client/Tests/*.cpp \
  -lz -o /tmp/npcify-ue-tests && /tmp/npcify-ue-tests .
```
