#include "NpcifyNpc.h"

#include "Animation/AnimSequence.h"
#include "Components/AudioComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NpcifyCharacterComponent.h"
#include "NpcifyDemoEnv.h"
#include "TimerManager.h"

ANpcifyNpc::ANpcifyNpc()
{
	PrimaryActorTick.bCanEverTick = true;

	Capsule = CreateDefaultSubobject<UCapsuleComponent>(TEXT("Capsule"));
	Capsule->InitCapsuleSize(34.f, 90.f);
	Capsule->SetCollisionProfileName(UCollisionProfile::Pawn_ProfileName);
	RootComponent = Capsule;

	Mesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Capsule);
	// The capsule is centered; the model's origin is at its feet. VRM4U models face +Y.
	Mesh->SetRelativeLocationAndRotation(FVector(0.f, 0.f, -90.f), FRotator(0.f, -90.f, 0.f));
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	// Stencil 1: the toon post-process (PP_NpcifyToon) keeps the MToon shading of the
	// character and adds only the ink lines.
	Mesh->SetRenderCustomDepth(true);
	Mesh->SetCustomDepthStencilValue(1);

	Npcify = CreateDefaultSubobject<UNpcifyCharacterComponent>(TEXT("Npcify"));
	Npcify->HeadBoneName = TEXT("head");
}

FText ANpcifyNpc::GetModeLabel() const
{
	return Mode == ENpcifyConnectionMode::OfflineBaked
		? NSLOCTEXT("Npcify", "OfflineMode", "Offline · baked")
		: NSLOCTEXT("Npcify", "OnlineMode", "Online · API");
}

void ANpcifyNpc::BeginPlay()
{
	Super::BeginPlay();
	RestRotation = GetActorRotation();

	if (USkeletalMesh* Loaded = Model.LoadSynchronous())
	{
		Mesh->SetSkeletalMesh(Loaded);
	}
	else
	{
		StatusMessage = TEXT("Model is missing. Run Scripts/import_characters.sh.");
		return;
	}

	const FString IdKey = Mode == ENpcifyConnectionMode::OfflineBaked
		? FNpcifyDemoEnv::OfflineCharacterIdKey
		: FNpcifyDemoEnv::OnlineCharacterIdKey;
	const FString CharacterId = FNpcifyDemoEnv::Get(IdKey, DefaultCharacterId);
	Npcify->Configure(FNpcifyDemoEnv::ApiBaseUrl(), CharacterId, FNpcifyDemoEnv::ApiKey());
	Npcify->RefreshAvatarBindings();
	if (UAudioComponent* Audio = Npcify->GetAudioComponent())
	{
		Audio->AttachToComponent(Mesh, FAttachmentTransformRules::KeepRelativeTransform);
		Audio->SetRelativeLocation(FVector(0.f, 0.f, 150.f));
	}
	Npcify->OnAnimationRequested.AddDynamic(this, &ANpcifyNpc::HandleAnimationRequested);
	PlayNpcAnimation(IdleAnimationId, true);

	if (Mode == ENpcifyConnectionMode::OfflineBaked)
	{
		TArray<uint8> Bytes;
		const FString File = FPaths::Combine(FPaths::ProjectContentDir(), OfflineConversationFile);
		if (!FFileHelper::LoadFileToArray(Bytes, *File) || !Npcify->LoadBakedConversationBytes(Bytes))
		{
			StatusMessage = TEXT("Baked conversation is missing. Run Scripts/fetch_characters.py.");
			return;
		}
	}
	else if (!FNpcifyDemoEnv::HasApiKey())
	{
		StatusMessage = FString::Printf(TEXT("Set %s in .env to talk to this character."), FNpcifyDemoEnv::ApiKeyKey);
		return;
	}

	bReady = true;
}

void ANpcifyNpc::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	FRotator Desired = RestRotation;
	if (const AActor* Target = FaceTarget.Get())
	{
		FVector Flat = Target->GetActorLocation() - GetActorLocation();
		Flat.Z = 0.f;
		if (!Flat.IsNearlyZero())
		{
			Desired = Flat.Rotation();
		}
	}
	SetActorRotation(FMath::RInterpConstantTo(GetActorRotation(), Desired, DeltaSeconds, 240.f));
}

FVector ANpcifyNpc::GetLabelLocation() const
{
	const int32 Head = Mesh->GetBoneIndex(Npcify->HeadBoneName);
	return Head != INDEX_NONE
		? Mesh->GetBoneLocation(Npcify->HeadBoneName) + FVector(0.f, 0.f, 34.f)
		: GetActorLocation() + FVector(0.f, 0.f, 100.f);
}

FVector ANpcifyNpc::GetFocusLocation() const
{
	const int32 Head = Mesh->GetBoneIndex(Npcify->HeadBoneName);
	return Head != INDEX_NONE ? Mesh->GetBoneLocation(Npcify->HeadBoneName) : GetActorLocation() + FVector(0.f, 0.f, 60.f);
}

void ANpcifyNpc::BeginConversation()
{
	Npcify->ResetConversation();
	Npcify->CreateConversation();
}

void ANpcifyNpc::Reply(const FString& Message)
{
	Npcify->Speak(Message);
}

void ANpcifyNpc::EndConversation()
{
	Npcify->StopSpeaking();
	FaceTowards(nullptr);
}

void ANpcifyNpc::FaceTowards(AActor* Target)
{
	FaceTarget = Target;
}

void ANpcifyNpc::PlayNpcAnimation(const FString& AnimationId, bool bLoop)
{
	UAnimSequence* Animation = FindAnimation(AnimationId);
	if (Animation == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("Npcify demo: %s has no animation '%s'."), *GetName(), *AnimationId);
		return;
	}

	const bool bIdle = AnimationId == IdleAnimationId;
	Mesh->PlayAnimation(Animation, bLoop || bIdle);
	GetWorldTimerManager().ClearTimer(ReturnToIdleTimer);
	if (!bLoop && !bIdle)
	{
		GetWorldTimerManager().SetTimer(ReturnToIdleTimer, [this]()
		{
			PlayNpcAnimation(IdleAnimationId, true);
		}, FMath::Max(0.1f, Animation->GetPlayLength()), false);
	}
}

void ANpcifyNpc::HandleAnimationRequested(const FString& AnimationId, bool bLoop)
{
	PlayNpcAnimation(AnimationId, bLoop);
}

UAnimSequence* ANpcifyNpc::FindAnimation(const FString& AnimationId) const
{
	const FString Name = TEXT("AS_") + AnimationId;
	const FString Path = FString::Printf(TEXT("%s/%s.%s"), *AnimationFolder.Path, *Name, *Name);
	return LoadObject<UAnimSequence>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
}
