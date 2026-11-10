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
  - Streamed replies: a reply starts to play when the audio of its first block is complete, before the reply is complete. See [Streamed Replies](#streamed-replies).
  - Sight: the character sees the game viewport, a render target or a scene capture, and its replies use what it sees. See [Sight](#sight).
  - Fine-grained control: `LookAtActor`, `LookAtComponent`, `LookAtLocation`, `ClearLookAt`, `PlayAnimation`, `SetEmotion`, `PlayExpressionCue`, `Speak`, `Reply`, `StartBakedConversation`, `GetBoneTransform`, `GetMouthOpenness`.

## Installation

Download the latest plugin archive from `https://npcify.ai/sdk/unreal/latest` (or a specific version from `https://npcify.ai/sdk/unreal/app/0.7.0`) and extract it into your Unreal project's `Plugins/NpcifySdk` directory:

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

- Speech audio arrives as MPEG bytes (baked conversations and streamed replies)
  or as a signed URL (the HTTP result of a turn). The component downloads URL
  audio before the block or the staging starts and decodes MPEG with the
  bundled [minimp3](https://github.com/lieff/minimp3) (CC0) into a
  `USoundWaveProcedural`. WAV and raw PCM bytes also play.
- Live requests send the same JSON bodies as the Web and Unity SDKs and accept
  NPCP responses. Retries keep one RFC 4122 `Idempotency-Key`.
- `ResetConversation()` stops speech and ignores late responses of the previous
  conversation. `OnConversationEnded` fires when the player picks an ending
  option of a baked conversation.
- `OnAnimationRequested(AnimationId, bLoop)` reports the animation cues of a
  staging; play them with your own animation assets.

## Streamed Replies

Since 0.6.0 a reply plays while the server makes it, as in the Web SDK
([streaming reply](../../docs/streaming_reply.md#engine-sdks)). The first
dialogue block plays when its audio is complete; the next blocks follow without
a gap and without a repeated block.

- **Transport:** the component joins the conversation stream (Socket.IO over the
  engine's `WebSockets` module) when the conversation starts. A turn waits for
  the join for 3 s at most.
- **Audio:** the speech of a block arrives as MP3 chunks. When the chunks of a
  block are complete, the block can start (minimp3 decodes it). The last block
  can play while the server still aligns its lip-sync: later revisions add to
  its visemes and text timing. A block whose chunks did not arrive plays the
  stored audio of the HTTP result.
- **Clock:** while the server aligns the last block, its estimated end can come
  before the end of its audio. The block then plays to the end of the audio that
  minimp3 decoded: the staging clock, and with it the live motion frames, runs
  on with the voice. The part without lip-sync has a neutral mouth until a later
  revision adds its visemes and text timing at the current time. At the end of
  its audio, a block that is not final waits for its final version, which ends
  it at its end time. The clock never goes back, and a block never starts again:
  when the clock already passed the new end, the next block starts at the
  current time and plays from its own start. At the end of the ready blocks the
  reply waits with a neutral mouth until the next block can play. The staging
  clock and the live motion frames wait too.
- **Events:** `OnSpeechStart` fires when the first block plays, with the text
  known at that time. `OnSpeechEnd` fires once, after the last block and when
  the reply options are known, so `GetCurrentReplyOptions()` gives them.
- **Fallback:** without the stream (no join in time, a lost connection, an older
  server) the reply plays after its HTTP result, as before 0.6.0.
  `StopSpeaking()` stops a streamed reply for good.
- **Failures:** the failed request is the failure of the turn, and `OnError`
  reports it. An `error` event of the stream only stops the reply earlier; the
  server does not send one for every failure. When the component retries the
  request and the server runs the turn again, the new run plays from its start.
  An older reply never plays over a newer one.
- **Options:** `bStreamReplies` turns streaming off. The log category
  `LogNpcifyStream` reports when the first revision, the first audio and the
  playback of each streamed reply came, and the audio length of a pending block.
- **Own integrations:** a host of `FNpcifyCharacterRuntime` that plays the audio
  itself stops the voice of the current block on `OnSpeechStopped`. It gives the
  length of the decoded audio of each block that starts (`OnAudioBlock`) with
  `SetBlockAudioLength`; without it, the clock of a pending block waits at its
  estimated end. For each
  turn it calls `BeginTurnRequest` (its order goes to `BeginStreamTurn`),
  `NoteTurnRetry` before it sends the request again, `EndTurnRequest` when the
  request fails, and plays a result that `HandleTurnResult` gives back with
  `PlayTurnStaging` and the order.

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
  engine's `WebSockets` module), with the streamed reply. A turn declares the
  frames only when the stream has joined the conversation, and waits for the
  join for 3 s at most.
  After that time, or at once while the stream fails, the turn plays the
  prepared clips and the stream connects again in the background, with a wait
  of at most 10 s between attempts while the component lives. When the server
  refuses the conversation (for example after you rotate the API key), the
  stream stops its attempts for that conversation, and the turns keep the
  prepared clips until the next conversation (`ResetConversation` or another
  conversation ID). The refusal of a conversation that you left while the
  server checked it does not stop the next conversation.

## Sight

When the owner of a character turns **sight** on in its Sight tab, the
character can see: the component sends frames of the game view on the
conversation stream, the server describes each frame with the owner's vision
provider, and the character reads the observations of the last 60 s in its
next reply ([sight](../../docs/sight.md)). The owner pays each frame.

```cpp
Npc->OnSight.AddDynamic(this, &AMyGame::HandleSight); // const FNpcifySightObservation&
Npc->OnSightState.AddDynamic(this, &AMyGame::HandleSightState);
if (Npc->GetSightState() == ENpcifySightState::Off)
{
    Npc->StartSight(FNpcifySightOptions()); // the game viewport
}
```

- **State:** `GetSightState()` is `Unavailable` until the character info turns
  sight on (the component reads it when a conversation starts), and in a baked
  conversation; then `Off`, `Starting` or `On`. `OnSightState(State,
  ErrorCode)` reports each change; `ErrorCode` is the code that stopped sight
  or its start.
- **Source:** no source in `FNpcifySightOptions`: the game viewport, with the
  HUD (its back buffer, read in `OnBackBufferReadyToPresent`). `Texture`: a
  render target or another texture. `SceneCapture`: the render target of a scene
  capture, for example a capture at the character's eyes; a capture that does
  not render every frame renders the scene at each check, four times a second
  (use `SCS_FinalColorLDR`). These frames have the source `game`. With
  `bCameraSource` the texture is the player's camera (source `camera`), for
  example the `UMediaTexture` of a webcam media player of your game; the SDK
  does not open a webcam.
- **Colour:** a render target holds linear colour in every format (a canvas,
  and a scene capture also with `SCS_FinalColorLDR`, write linear values into
  `RTF_RGBA8` and `RTF_RGBA16f`), so the SDK gives its frames the gamma back.
  A scene capture into `RTF_RGBA8_SRGB` gives dark frames: use `RTF_RGBA8` or
  `RTF_RGBA16f`. A scene capture has no auto exposure of the main view, so its
  frames can be darker than the viewport. Another texture (a media texture) is
  copied as it is, unless it is an sRGB or a float texture. Set the console
  variable `npcify.Sight.SaveFolder` to a folder to keep each frame as a JPEG
  and see what the character sees.
- **Frames:** the render thread scales the picture to fit inside 640 x 360,
  and `FRHIGPUTextureReadback` copies it to the CPU without a flush of the game
  or render thread. On an Apple GPU the SDK reads the readback from shared
  memory after its fence (the Metal RHI of the engine waits for an idle GPU
  when it maps a readback); an Intel Mac takes that wait at each check.
  Worker threads make the 32 x 18 luma grid and the JPEG
  (`IImageWrapper`, quality 70, then 50 once above 200 KiB). Every 250 ms the
  SDK compares the grid with the last one: a mean change above
  `MotionThreshold` (8 on 0 to 255) sends a frame when one second passed since
  the last frame; else a frame goes every `IntervalMs` (5000, at least 1000).
  One frame waits for its acknowledgement at a time. No frames while the
  application is in the background or the game is paused.
- **Codes:** `SIGHT_DISABLED`, `INSUFFICIENT_POINTS`,
  `CONVERSATION_ACCESS_DENIED` and `SIGHT_SOURCE_ENDED` stop sight and go to
  `OnError` (`"CODE: message"`) and `OnSightState`. After `SIGHT_DISABLED` the
  state is `Unavailable` until the next conversation reads the character info
  again. Other codes, a timeout (20 s) and a lost connection skip the frame;
  after a lost connection the next frame joins the conversation again first.
  `StartSight` returns false and reports `OnError` for `SIGHT_DISABLED`,
  `CONVERSATION_REQUIRED` and `SIGHT_ALREADY_STARTED`; a start that fails later
  (`SIGHT_SOURCE_UNAVAILABLE`, `CAMERA_UNAVAILABLE`, a failed join) reports
  `OnSightState(Off, Code)` and `OnError`.
- **Events:** `OnSight` gets the observations of the current conversation
  (`Summary`, `People`, `Persons`, `Text`, `Model`, `CostPoints`). `StopSight`,
  `ResetConversation` and `EndPlay` stop sight. `LogNpcifySight` notes the size
  and the acknowledgement of each frame, and the costs of the source when sight
  stops.
- **Transport:** the frames go on the conversation stream as the binary
  Socket.IO event `conversations:sight`. The plugin needs the engine modules
  `ImageWrapper`, `RenderCore`, `RHI`, `Slate` and `SlateCore`.

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
