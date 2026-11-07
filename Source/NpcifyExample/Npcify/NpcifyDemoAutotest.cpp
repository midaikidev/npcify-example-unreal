#include "NpcifyDemoAutotest.h"

#include "Animation/MorphTarget.h"
#include "AudioMixerBlueprintLibrary.h"
#include "Engine/SkeletalMesh.h"
#include "Camera/CameraActor.h"
#include "Components/AudioComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "NpcifyDialogueDirector.h"
#include "Components/SkeletalMeshComponent.h"
#include "NpcifyCharacterComponent.h"
#include "NpcifyNpc.h"
#include "UnrealClient.h"

ANpcifyDemoAutotest::ANpcifyDemoAutotest()
{
	PrimaryActorTick.bCanEverTick = true;
}

void ANpcifyDemoAutotest::LogVoiceSpatialization(const FString& Tag, const UAudioComponent* Audio)
{
	const FSoundAttenuationSettings* Settings = Audio ? Audio->GetAttenuationSettingsToApply() : nullptr;
	APlayerController* Controller = UGameplayStatics::GetPlayerController(this, 0);
	FVector Listener = FVector::ZeroVector;
	if (Controller)
	{
		FVector Front;
		FVector Right;
		Controller->GetAudioListenerPosition(Listener, Front, Right);
		FVector Override;
		if (Controller->GetAudioListenerAttenuationOverridePosition(Override))
		{
			Listener = Override;
		}
	}
	UE_LOG(LogTemp, Display, TEXT("Npcify autotest: %s voice spatialize=%d attenuate=%d inner=%.0f falloff=%.0f distance=%.0f"), *Tag,
		Settings && Settings->bSpatialize, Settings && Settings->bAttenuate,
		Settings ? Settings->AttenuationShapeExtents.X : 0.f, Settings ? Settings->FalloffDistance : 0.f,
		Audio ? FVector::Dist(Audio->GetComponentLocation(), Listener) : -1.f);
}

void ANpcifyDemoAutotest::Step(const FString& Label, float Delay, TFunction<bool()> Done, TFunction<void()> Action)
{
	Steps.Add({ Label, Delay, MoveTemp(Done), MoveTemp(Action) });
}

ANpcifyDialogueDirector* ANpcifyDemoAutotest::Director() const
{
	TActorIterator<ANpcifyDialogueDirector> It(GetWorld());
	return It ? *It : nullptr;
}

void ANpcifyDemoAutotest::Capture(const FString& Name)
{
	const FString Path = FPaths::Combine(OutputFolder, Name + TEXT(".png"));
	FScreenshotRequest::RequestScreenshot(Path, true, false);
	UE_LOG(LogTemp, Display, TEXT("Npcify autotest: screenshot %s"), *Path);
}

void ANpcifyDemoAutotest::LogLiveMotion(const FString& Tag, ANpcifyNpc* Npc)
{
	UNpcifyCharacterComponent* Npcify = Npc->Npcify;
	int32 Received = 0;
	int32 Played = 0;
	float FirstFrame = -1.f;
	if (!Npcify->GetLiveMotionStats(Npcify->GetActiveStagingId(), Received, Played, FirstFrame) || Played == 0)
	{
		UE_LOG(LogTemp, Display, TEXT("Npcify autotest: %s live motion: no frames played (ready %d)"), *Tag, Npcify->IsLiveMotionReady());
		if (Npcify->IsLiveMotionReady())
		{
			++Failures;
		}
		return;
	}
	UE_LOG(LogTemp, Display,
		TEXT("Npcify autotest: %s live motion: %d frames played, %d frames received, first frame at %.2f s of the staging clock, max weight %.2f, hands at most %.1f cm (right) and %.1f cm (left) above their shoulders"),
		*Tag, Played, Received, FirstFrame, MaxLiveWeight, MaxHandAboveShoulder, MaxLeftHandAboveShoulder);
}

void ANpcifyDemoAutotest::HandleLiveMotionFrames(const FString& StagingId, int32 StartFrame, int32 FrameCount)
{
	const bool bSpeaking = FramesNpc && FramesNpc->Npcify->IsSpeaking();
	UE_LOG(LogTemp, Display, TEXT("Npcify autotest: live frames %d+%d at %.2f s after the reply was sent%s"),
		StartFrame, FrameCount, FPlatformTime::Seconds() - ReplySentAt,
		bSpeaking ? TEXT(", during playback") : TEXT(", before playback"));
}

void ANpcifyDemoAutotest::MovePlayerNear(ANpcifyNpc* Npc)
{
	APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	const FVector Front = Npc->GetActorLocation() + Npc->GetActorForwardVector() * 200.f;
	Player->SetActorLocation(FVector(Front.X, Front.Y, Player->GetActorLocation().Z), false, nullptr, ETeleportType::TeleportPhysics);
	const FRotator Facing = (-Npc->GetActorForwardVector()).Rotation();
	Player->SetActorRotation(Facing);
	if (APlayerController* Controller = Cast<APlayerController>(Player->GetController()))
	{
		Controller->SetControlRotation(FRotator(-10.f, Facing.Yaw, 0.f));
	}
}

void ANpcifyDemoAutotest::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	ANpcifyDialogueDirector* Dialogue = Director();
	if (!Dialogue)
	{
		return;
	}
	if (const ANpcifyNpc* Talking = Dialogue->GetActiveNpc())
	{
		MaxMouthOpenness = FMath::Max(MaxMouthOpenness, Talking->Npcify->GetMouthOpenness());
		MaxLiveWeight = FMath::Max(MaxLiveWeight, Talking->Npcify->GetLiveMotionWeight());
		const FName Hand = Talking->Npcify->GetHumanoidBoneName(TEXT("rightHand"));
		const FName Shoulder = Talking->Npcify->GetHumanoidBoneName(TEXT("rightUpperArm"));
		if (!Hand.IsNone() && !Shoulder.IsNone())
		{
			MaxHandAboveShoulder = FMath::Max(MaxHandAboveShoulder,
				static_cast<float>(Talking->Mesh->GetBoneLocation(Hand).Z - Talking->Mesh->GetBoneLocation(Shoulder).Z));
		}
		const FName LeftHand = Talking->Npcify->GetHumanoidBoneName(TEXT("leftHand"));
		const FName LeftShoulder = Talking->Npcify->GetHumanoidBoneName(TEXT("leftUpperArm"));
		if (!LeftHand.IsNone() && !LeftShoulder.IsNone())
		{
			MaxLeftHandAboveShoulder = FMath::Max(MaxLeftHandAboveShoulder,
				static_cast<float>(Talking->Mesh->GetBoneLocation(LeftHand).Z - Talking->Mesh->GetBoneLocation(LeftShoulder).Z));
		}
		if (ClockProbe.IsValid() && Talking == FramesNpc)
		{
			ClockProbe->Sample(Talking->Npcify->GetRuntime(), [this](const FString& Note)
			{
				UE_LOG(LogTemp, Display, TEXT("Npcify autotest: %s %s"), *ClockProbeTag, *Note);
			});
		}
	}

	if (!bPlanned)
	{
		bPlanned = true;
		Step(TEXT("scene"), 3.f, nullptr, [this]() { Capture(TEXT("01-scene")); });
		int32 Shot = 2;
		for (ANpcifyNpc* Npc : Dialogue->GetNpcs())
		{
			const FString Tag = Npc->Mode == ENpcifyConnectionMode::OfflineBaked ? TEXT("offline") : TEXT("online");
			UE_LOG(LogTemp, Display, TEXT("Npcify autotest: %s ready=%d %s"), *Npc->GetName(), Npc->IsReady(), *Npc->GetStatusMessage());
			TArray<FString> Morphs;
			const USkeletalMesh* Model = Npc->Mesh->GetSkeletalMeshAsset();
			for (const UMorphTarget* Morph : Model ? Model->GetMorphTargets() : TArray<TObjectPtr<UMorphTarget>>())
			{
				if (Morphs.Num() < 16)
				{
					Morphs.Add(Morph->GetName());
				}
			}
			UE_LOG(LogTemp, Display, TEXT("Npcify autotest: %s morph targets: %s"), *Npc->GetName(), *FString::Join(Morphs, TEXT(", ")));
			if (!Npc->IsReady())
			{
				++Failures;
				continue;
			}
			Step(Tag + TEXT(" inspect"), 0.f, nullptr, [this, Npc]()
			{
				ACameraActor* Camera = GetWorld()->SpawnActor<ACameraActor>();
				const FVector Eye = Npc->GetFocusLocation() + Npc->GetActorForwardVector() * 90.f;
				Camera->SetActorLocationAndRotation(Eye, (Npc->GetFocusLocation() - Eye).Rotation());
				UGameplayStatics::GetPlayerController(this, 0)->SetViewTarget(Camera);
			});
			Step(Tag + TEXT(" inspect shot"), 1.f, nullptr, [this, Tag]() { Capture(TEXT("00-inspect-") + Tag); });
			Step(Tag + TEXT(" inspect end"), 0.2f, nullptr, [this]()
			{
				APlayerController* Controller = UGameplayStatics::GetPlayerController(this, 0);
				Controller->SetViewTarget(Controller->GetPawn());
			});
			Step(Tag + TEXT(" walk"), 0.f, nullptr, [this, Npc]() { MovePlayerNear(Npc); });
			Step(Tag + TEXT(" prompt"), 1.5f, nullptr, [this, Tag, Shot]() { Capture(FString::Printf(TEXT("%02d-%s-prompt"), Shot, *Tag)); });
			Step(Tag + TEXT(" open"), 0.f, nullptr, [this, Dialogue]()
			{
				Dialogue->TryOpenNearby();
				// The output mix of the first line, to check the voice level and panning.
				UAudioMixerBlueprintLibrary::StartRecordingOutput(this, 10.f);
			});
			Step(Tag + TEXT(" speaking"), 2.5f, nullptr, [this, Tag, Shot, Dialogue]()
			{
				const UAudioComponent* Audio = Dialogue->GetActiveNpc() ? Dialogue->GetActiveNpc()->Npcify->GetAudioComponent() : nullptr;
				UE_LOG(LogTemp, Display, TEXT("Npcify autotest: %s audio playing=%d mouth=%.2f"), *Tag,
					Audio && Audio->IsPlaying(), Dialogue->GetActiveNpc() ? Dialogue->GetActiveNpc()->Npcify->GetMouthOpenness() : 0.f);
				LogVoiceSpatialization(Tag, Audio);
				UAudioMixerBlueprintLibrary::StopRecordingOutput(this, EAudioRecordingExportType::WavFile, Tag + TEXT("-voice"), OutputFolder);
				Capture(FString::Printf(TEXT("%02d-%s-speaking"), Shot + 1, *Tag));
			});
			Step(Tag + TEXT(" choices"), 0.5f, [Dialogue]() { return Dialogue->IsChoosing(); }, [this, Tag, Shot, Dialogue]()
			{
				Capture(FString::Printf(TEXT("%02d-%s-choices"), Shot + 2, *Tag));
				UE_LOG(LogTemp, Display, TEXT("Npcify autotest: %s choices: %s (max mouth openness %.2f)"), *Tag,
					*FString::Join(Dialogue->GetChoices(), TEXT(" | ")), MaxMouthOpenness);
				MaxMouthOpenness = 0.f;
			});
			Step(Tag + TEXT(" choose"), 0.5f, nullptr, [this, Dialogue, Npc, Tag]()
			{
				MaxLiveWeight = 0.f;
				MaxHandAboveShoulder = -1000.f;
				MaxLeftHandAboveShoulder = -1000.f;
				ReplySentAt = FPlatformTime::Seconds();
				FramesNpc = Npc;
				if (Npc->Mode == ENpcifyConnectionMode::OnlineApi)
				{
					ClockProbe = MakeUnique<FNpcifyDemoClockProbe>();
					ClockProbeTag = Tag;
				}
				Npc->Npcify->OnLiveMotionFrames.AddUniqueDynamic(this, &ANpcifyDemoAutotest::HandleLiveMotionFrames);
				if (!CustomReply.IsEmpty() && Npc->AllowsCustomReply())
				{
					UE_LOG(LogTemp, Display, TEXT("Npcify autotest: %s reply: %s"), *Tag, *CustomReply);
					Dialogue->SendCustomReply(CustomReply);
				}
				else
				{
					Dialogue->Choose(0);
				}
			});
			if (Npc->Mode == ENpcifyConnectionMode::OnlineApi)
			{
				// A picture during the reply, when live motion moves the character.
				Step(Tag + TEXT(" reply speaking"), 0.5f, [Npc]() { return Npc->Npcify->IsSpeaking(); }, [this, Tag]()
				{
					UE_LOG(LogTemp, Display, TEXT("Npcify autotest: %s reply plays %.2f s after it was sent"), *Tag, FPlatformTime::Seconds() - ReplySentAt);
				});
				Step(Tag + TEXT(" reply motion"), 2.5f, nullptr, [this, Tag, Shot]() { Capture(FString::Printf(TEXT("%02d-%s-reply-motion"), Shot + 3, *Tag)); });
			}
			// Panning check (offline NPC: its reply starts at once): during the reply, the listener
			// turns so that the NPC is on its right. The right channel must be louder.
			if (Npc->Mode == ENpcifyConnectionMode::OfflineBaked)
			{
			Step(Tag + TEXT(" pan"), 1.f, nullptr, [this, Npc]()
			{
				APlayerController* Controller = UGameplayStatics::GetPlayerController(this, 0);
				const FVector NpcLocation = Npc->GetActorLocation();
				const FVector ListenerLocation = NpcLocation + Npc->GetActorForwardVector() * 200.f;
				const FVector ToNpc = (NpcLocation - ListenerLocation).GetSafeNormal2D();
				const FVector Forward = FVector::CrossProduct(ToNpc, FVector::UpVector);
				Controller->SetAudioListenerOverride(nullptr, ListenerLocation, Forward.Rotation());
				UAudioMixerBlueprintLibrary::StartRecordingOutput(this, 10.f);
			});
			Step(Tag + TEXT(" pan end"), 1.5f, nullptr, [this, Tag]()
			{
				UAudioMixerBlueprintLibrary::StopRecordingOutput(this, EAudioRecordingExportType::WavFile, Tag + TEXT("-voice-npc-on-right"), OutputFolder);
				UGameplayStatics::GetPlayerController(this, 0)->ClearAudioListenerOverride();
			});
			}
			Step(Tag + TEXT(" reply"), 1.f, [Dialogue]() { return Dialogue->IsChoosing(); }, [this, Tag, Shot, Dialogue]()
			{
				if (ClockProbe.IsValid())
				{
					ClockProbe->Finish([this](const FString& Note)
					{
						UE_LOG(LogTemp, Display, TEXT("Npcify autotest: %s %s"), *ClockProbeTag, *Note);
					});
					ClockProbe.Reset();
				}
				Capture(FString::Printf(TEXT("%02d-%s-reply"), Shot + 4, *Tag));
				UE_LOG(LogTemp, Display, TEXT("Npcify autotest: %s reply choices: %s"), *Tag, *FString::Join(Dialogue->GetChoices(), TEXT(" | ")));
				if (ANpcifyNpc* Talking = Dialogue->GetActiveNpc())
				{
					Talking->Npcify->OnLiveMotionFrames.RemoveDynamic(this, &ANpcifyDemoAutotest::HandleLiveMotionFrames);
					LogLiveMotion(Tag, Talking);
				}
				if (Dialogue->GetChoices().Contains(TEXT("Try again")))
				{
					++Failures;
				}
			});
			Step(Tag + TEXT(" close"), 1.f, nullptr, [Dialogue]() { Dialogue->Close(); });
			Shot += 5;
		}
		Step(TEXT("finish"), 1.5f, nullptr, [this]()
		{
			UE_LOG(LogTemp, Display, TEXT("Npcify autotest: %s"), Failures == 0 ? TEXT("AUTOTEST PASSED") : TEXT("AUTOTEST FAILED"));
			FPlatformMisc::RequestExitWithStatus(false, Failures == 0 ? 0 : 1);
		});
	}

	if (Current < 0)
	{
		Current = 0;
		StepTime = 0.f;
	}
	if (!Steps.IsValidIndex(Current))
	{
		return;
	}

	FStep& Active = Steps[Current];
	StepTime += DeltaSeconds;
	const bool bReady = StepTime >= Active.Delay && (!Active.Done || Active.Done());
	if (!bReady && StepTime > Active.Delay + 90.f)
	{
		UE_LOG(LogTemp, Warning, TEXT("Npcify autotest: timeout at %s"), *Active.Label);
		++Failures;
	}
	else if (!bReady)
	{
		return;
	}
	if (Active.Action)
	{
		Active.Action();
	}
	++Current;
	StepTime = 0.f;
}
