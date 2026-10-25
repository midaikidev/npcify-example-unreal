#include "NpcifyDemoSubsystem.h"

#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "Misc/CommandLine.h"
#include "NpcifyDemoAutotest.h"
#include "NpcifyDialogueDirector.h"
#include "NpcifyNpc.h"

namespace
{
	struct FDemoNpc
	{
		const TCHAR* Name;
		ENpcifyConnectionMode Mode;
		const TCHAR* CharacterId;
		const TCHAR* Folder;
		const TCHAR* ConversationFile;
		/** Offset to the right of the player start, in cm. */
		float Side;
	};

	const FDemoNpc DemoNpcs[] = {
		{ TEXT("ひなた"), ENpcifyConnectionMode::OfflineBaked, TEXT("221608de-65d0-49f0-97b4-800ce08c198d"), TEXT("hinata"),
			TEXT("NpcifyData/hinata/conversation.npcp"), -110.f },
		{ TEXT("Vespera"), ENpcifyConnectionMode::OnlineApi, TEXT("37d81b69-04a8-4720-9cda-4172d4317803"), TEXT("vespera"),
			TEXT(""), 110.f },
	};

	FVector SnapToGround(UWorld& World, const FVector& Location)
	{
		FHitResult Hit;
		const FVector Start = Location + FVector(0.f, 0.f, 500.f);
		const FVector End = Location - FVector(0.f, 0.f, 2000.f);
		if (World.LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility))
		{
			return Hit.ImpactPoint;
		}
		return Location;
	}
}

bool UNpcifyDemoSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	return Super::ShouldCreateSubsystem(Outer);
}

void UNpcifyDemoSubsystem::OnWorldBeginPlay(UWorld& World)
{
	Super::OnWorldBeginPlay(World);
	if (!World.IsGameWorld() || TActorIterator<ANpcifyDialogueDirector>(&World))
	{
		return;
	}

	if (!TActorIterator<ANpcifyNpc>(&World))
	{
		TActorIterator<APlayerStart> Start(&World);
		const FTransform Origin = Start ? Start->GetActorTransform() : FTransform::Identity;
		const FVector Forward = Origin.GetRotation().GetForwardVector().GetSafeNormal2D();
		const FVector Right = FVector::CrossProduct(FVector::UpVector, Forward);
		for (const FDemoNpc& Entry : DemoNpcs)
		{
			// On the raised platform in front of the template's player start.
			const FVector Ground = SnapToGround(World, Origin.GetLocation() + Forward * 640.f + Right * Entry.Side);
			const FRotator Facing = (-Forward).Rotation();
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Params.bDeferConstruction = true;
			ANpcifyNpc* Npc = World.SpawnActor<ANpcifyNpc>(Ground + FVector(0.f, 0.f, 90.f), Facing, Params);
			Npc->DisplayName = FText::FromString(Entry.Name);
			Npc->Mode = Entry.Mode;
			Npc->DefaultCharacterId = Entry.CharacterId;
			Npc->Model = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(
				FString::Printf(TEXT("/Game/Npcify/Characters/%s/SK_character.SK_character"), Entry.Folder)));
			Npc->AnimationFolder.Path = FString::Printf(TEXT("/Game/Npcify/Characters/%s/Animations"), Entry.Folder);
			Npc->OfflineConversationFile = Entry.ConversationFile;
			Npc->FinishSpawning(FTransform(Facing, Ground + FVector(0.f, 0.f, 90.f)));
		}
	}

	World.SpawnActor<ANpcifyDialogueDirector>();

	// Anime look for the whole level: cel shading and ink lines (Scripts/make_toon_material.py).
	if (UMaterialInterface* Toon = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Npcify/Rendering/PP_NpcifyToon.PP_NpcifyToon")))
	{
		APostProcessVolume* Volume = World.SpawnActor<APostProcessVolume>();
		Volume->bUnbound = true;
		Volume->Priority = 10.f;
		Volume->Settings.AddBlendable(Toon, 1.f);
		Volume->Settings.bOverride_ColorSaturation = true;
		Volume->Settings.ColorSaturation = FVector4(1.12f, 1.12f, 1.12f, 1.f);
		// Noise of Lumen and ambient occlusion makes speckles at the cel band edges.
		// A flat sky light gives one clean shade tone.
		Volume->Settings.bOverride_DynamicGlobalIlluminationMethod = true;
		Volume->Settings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::None;
		Volume->Settings.bOverride_ReflectionMethod = true;
		Volume->Settings.ReflectionMethod = EReflectionMethod::None;
		Volume->Settings.bOverride_AmbientOcclusionIntensity = true;
		Volume->Settings.AmbientOcclusionIntensity = 0.f;
	}

	FString AutotestFolder;
	if (FParse::Value(FCommandLine::Get(), TEXT("NpcifyAutotest="), AutotestFolder))
	{
		ANpcifyDemoAutotest* Autotest = World.SpawnActor<ANpcifyDemoAutotest>();
		Autotest->OutputFolder = AutotestFolder;
	}
}
