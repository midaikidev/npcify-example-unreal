#include "NpcifyDemoAutotest.h"

#include "Animation/MorphTarget.h"
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
			Step(Tag + TEXT(" open"), 0.f, nullptr, [Dialogue]() { Dialogue->TryOpenNearby(); });
			Step(Tag + TEXT(" speaking"), 2.5f, nullptr, [this, Tag, Shot, Dialogue]()
			{
				const UAudioComponent* Audio = Dialogue->GetActiveNpc() ? Dialogue->GetActiveNpc()->Npcify->GetAudioComponent() : nullptr;
				UE_LOG(LogTemp, Display, TEXT("Npcify autotest: %s audio playing=%d mouth=%.2f"), *Tag,
					Audio && Audio->IsPlaying(), Dialogue->GetActiveNpc() ? Dialogue->GetActiveNpc()->Npcify->GetMouthOpenness() : 0.f);
				Capture(FString::Printf(TEXT("%02d-%s-speaking"), Shot + 1, *Tag));
			});
			Step(Tag + TEXT(" choices"), 0.5f, [Dialogue]() { return Dialogue->IsChoosing(); }, [this, Tag, Shot, Dialogue]()
			{
				Capture(FString::Printf(TEXT("%02d-%s-choices"), Shot + 2, *Tag));
				UE_LOG(LogTemp, Display, TEXT("Npcify autotest: %s choices: %s (max mouth openness %.2f)"), *Tag,
					*FString::Join(Dialogue->GetChoices(), TEXT(" | ")), MaxMouthOpenness);
				MaxMouthOpenness = 0.f;
			});
			Step(Tag + TEXT(" choose"), 0.5f, nullptr, [Dialogue]() { Dialogue->Choose(0); });
			Step(Tag + TEXT(" reply"), 1.f, [Dialogue]() { return Dialogue->IsChoosing(); }, [this, Tag, Shot, Dialogue]()
			{
				Capture(FString::Printf(TEXT("%02d-%s-reply"), Shot + 3, *Tag));
				UE_LOG(LogTemp, Display, TEXT("Npcify autotest: %s reply choices: %s"), *Tag, *FString::Join(Dialogue->GetChoices(), TEXT(" | ")));
				if (Dialogue->GetChoices().Contains(TEXT("Try again")))
				{
					++Failures;
				}
			});
			Step(Tag + TEXT(" close"), 1.f, nullptr, [Dialogue]() { Dialogue->Close(); });
			Shot += 4;
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
