#include "NpcifyDialogueDirector.h"

#include "Blueprint/WidgetLayoutLibrary.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "Kismet/GameplayStatics.h"
#include "NpcifyCharacterComponent.h"
#include "NpcifyDialogueWidget.h"
#include "NpcifyExampleCharacter.h"
#include "NpcifyNpc.h"

namespace
{
	const FString LeaveLabel = TEXT("Leave");
	const FString RetryLabel = TEXT("Try again");
	const FKey NumberKeys[] = {
		EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five,
		EKeys::Six, EKeys::Seven, EKeys::Eight, EKeys::Nine,
	};
}

ANpcifyDialogueDirector::ANpcifyDialogueDirector()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;
}

void ANpcifyDialogueDirector::BeginPlay()
{
	Super::BeginPlay();
	for (TActorIterator<ANpcifyNpc> It(GetWorld()); It; ++It)
	{
		Npcs.Add(*It);
	}

	Widget = CreateWidget<UNpcifyDialogueWidget>(GetPlayerController(), UNpcifyDialogueWidget::StaticClass());
	Widget->AddToViewport(10);
	Widget->SetLabelCount(Npcs.Num());
	Widget->OnChoiceClicked.BindUObject(this, &ANpcifyDialogueDirector::Choose);
	Widget->OnReplySubmitted.BindUObject(this, &ANpcifyDialogueDirector::SendReply);
	Widget->OnCloseClicked.BindUObject(this, &ANpcifyDialogueDirector::Close);

	FActorSpawnParameters Params;
	Params.Name = TEXT("NpcifyDialogueCamera");
	DialogueCamera = GetWorld()->SpawnActor<ACameraActor>(Params);
	DialogueCamera->GetCameraComponent()->SetFieldOfView(42.f);
	DialogueCamera->GetCameraComponent()->bConstrainAspectRatio = false;
}

void ANpcifyDialogueDirector::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!GetPlayerPawn() || !Widget)
	{
		return;
	}
	if (!bPlayerConfigured)
	{
		bPlayerConfigured = true;
		ConfigureShoulderCamera();
	}
	UpdateLabels();
	if (Active)
	{
		UpdateConversation(DeltaSeconds);
	}
	else
	{
		UpdateExploring();
	}
}

void ANpcifyDialogueDirector::ConfigureShoulderCamera()
{
	// The template camera orbits behind the character; this moves it over the right shoulder.
	if (ANpcifyExampleCharacter* Character = Cast<ANpcifyExampleCharacter>(GetPlayerPawn()))
	{
		Character->GetCameraBoom()->TargetArmLength = 270.f;
		Character->GetCameraBoom()->SocketOffset = FVector(0.f, 85.f, 45.f);
		Character->GetCameraBoom()->bEnableCameraLag = true;
		Character->GetCameraBoom()->CameraLagSpeed = 12.f;
		Character->GetFollowCamera()->SetFieldOfView(80.f);
	}
	// Start with a level view towards the NPCs in front of the player start.
	GetPlayerController()->SetControlRotation(FRotator(-10.f, GetPlayerPawn()->GetActorRotation().Yaw, 0.f));
}

void ANpcifyDialogueDirector::UpdateLabels()
{
	const FVector Viewer = GetPlayerPawn()->GetActorLocation();
	for (int32 Index = 0; Index < Npcs.Num(); ++Index)
	{
		ANpcifyNpc* Npc = Npcs[Index];
		FVector2D Screen;
		const bool bVisible = Npc && Npc != Active && FVector::Dist(Viewer, Npc->GetActorLocation()) < 1400.f &&
			Project(Npc->GetLabelLocation(), Screen);
		const FText Status = Npc && Npc->IsReady() ? Npc->GetModeLabel() : FText::FromString(Npc ? Npc->GetStatusMessage() : FString());
		Widget->SetLabel(Index, bVisible, Screen, Npc ? Npc->DisplayName : FText::GetEmpty(), Status);
	}
}

void ANpcifyDialogueDirector::UpdateExploring()
{
	Nearby = FindNearbyNpc();
	FVector2D Screen;
	if (!Nearby || !Project(Nearby->GetLabelLocation(), Screen))
	{
		Widget->HidePrompt();
		return;
	}

	const FText Text = Nearby->IsReady()
		? FText::Format(NSLOCTEXT("Npcify", "Prompt", "[E]  Talk to {0}"), Nearby->DisplayName)
		: FText::FromString(Nearby->GetStatusMessage());
	Widget->ShowPrompt(Screen, Text);

	APlayerController* Controller = GetPlayerController();
	if (Controller->WasInputKeyJustPressed(EKeys::E) ||
		Controller->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Left) ||
		Controller->WasInputKeyJustPressed(EKeys::LeftMouseButton))
	{
		TryOpenNearby();
	}
}

void ANpcifyDialogueDirector::UpdateConversation(float DeltaSeconds)
{
	UpdateCamera(DeltaSeconds, false);
	APawn* Player = GetPlayerPawn();
	FVector ToNpc = Active->GetActorLocation() - Player->GetActorLocation();
	ToNpc.Z = 0.f;
	Player->SetActorRotation(FMath::RInterpConstantTo(Player->GetActorRotation(), ToNpc.Rotation(), DeltaSeconds, 360.f));

	UNpcifyCharacterComponent* Npcify = Active->Npcify;
	if (Phase == EPhase::Waiting)
	{
		const FString Dots = FString::ChrN(1 + static_cast<int32>(GetWorld()->GetTimeSeconds() * 2.5f) % 3, TEXT('.'));
		Widget->SetBody(FText::Format(NSLOCTEXT("Npcify", "Thinking", "{0} is thinking{1}"), Active->DisplayName, FText::FromString(Dots)));
	}
	else if (Phase == EPhase::Speaking)
	{
		const FString Revealed = Npcify->GetRevealedText();
		Widget->SetBody(FText::FromString(Revealed.IsEmpty() ? Npcify->GetCurrentText() : Revealed));
	}

	APlayerController* Controller = GetPlayerController();
	if (Controller->WasInputKeyJustPressed(EKeys::Escape) || Controller->WasInputKeyJustPressed(EKeys::Q))
	{
		Close();
		return;
	}
	if (Widget->IsReplyFocused())
	{
		return;
	}
	if (Controller->WasInputKeyJustPressed(EKeys::SpaceBar) && Phase == EPhase::Speaking)
	{
		Npcify->StopSpeaking();
		EnterChoosing();
	}
	if (Controller->WasInputKeyJustPressed(EKeys::Tab))
	{
		Widget->FocusReply();
	}
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(NumberKeys); ++Index)
	{
		if (Controller->WasInputKeyJustPressed(NumberKeys[Index]))
		{
			Choose(Index);
		}
	}
}

void ANpcifyDialogueDirector::UpdateCamera(float DeltaSeconds, bool bSnap)
{
	// Over the player's right shoulder, framing the NPC's face.
	const APawn* Player = GetPlayerPawn();
	const FVector Focus = Active->GetFocusLocation();
	FVector ToNpc = Focus - Player->GetActorLocation();
	ToNpc.Z = 0.f;
	ToNpc = ToNpc.GetSafeNormal();
	const FVector Right = FVector::CrossProduct(FVector::UpVector, ToNpc);
	const FVector Eyes = Player->GetPawnViewLocation();
	const FVector Desired = Eyes - ToNpc * 150.f + Right * 70.f + FVector(0.f, 0.f, 18.f);
	const FVector Location = bSnap ? Desired : FMath::VInterpTo(DialogueCamera->GetActorLocation(), Desired, DeltaSeconds, 6.f);
	DialogueCamera->SetActorLocationAndRotation(Location, (Focus - Location).Rotation());
}

void ANpcifyDialogueDirector::TryOpenNearby()
{
	if (!Active && Nearby && Nearby->IsReady())
	{
		Open(Nearby);
	}
}

void ANpcifyDialogueDirector::Open(ANpcifyNpc* Npc)
{
	Active = Npc;
	bEnding = false;
	Widget->HidePrompt();
	Widget->ShowDialogue(Npc->DisplayName, Npc->GetModeLabel(), Npc->AllowsCustomReply());
	Npc->FaceTowards(GetPlayerPawn());
	Npc->Npcify->OnSpeechStart.AddDynamic(this, &ANpcifyDialogueDirector::HandleSpeechStart);
	Npc->Npcify->OnSpeechEnd.AddDynamic(this, &ANpcifyDialogueDirector::HandleSpeechEnd);
	Npc->Npcify->OnError.AddDynamic(this, &ANpcifyDialogueDirector::HandleError);
	Npc->Npcify->OnConversationEnded.AddDynamic(this, &ANpcifyDialogueDirector::HandleConversationEnded);
	UpdateCamera(0.f, true);
	GetPlayerController()->SetViewTargetWithBlend(DialogueCamera, 0.7f, VTBlend_Cubic);
	SetPlayerControl(false);
	EnterWaiting();
	Npc->BeginConversation();
}

void ANpcifyDialogueDirector::Close()
{
	if (!Active)
	{
		return;
	}
	Active->Npcify->OnSpeechStart.RemoveDynamic(this, &ANpcifyDialogueDirector::HandleSpeechStart);
	Active->Npcify->OnSpeechEnd.RemoveDynamic(this, &ANpcifyDialogueDirector::HandleSpeechEnd);
	Active->Npcify->OnError.RemoveDynamic(this, &ANpcifyDialogueDirector::HandleError);
	Active->Npcify->OnConversationEnded.RemoveDynamic(this, &ANpcifyDialogueDirector::HandleConversationEnded);
	Active->Npcify->ResetConversation();
	Active->EndConversation();
	Active = nullptr;
	Phase = EPhase::Exploring;
	Widget->HideDialogue();
	GetPlayerController()->SetViewTargetWithBlend(GetPlayerPawn(), 0.7f, VTBlend_Cubic);
	SetPlayerControl(true);
}

void ANpcifyDialogueDirector::Choose(int32 Index)
{
	if (Phase != EPhase::Choosing || !Choices.IsValidIndex(Index))
	{
		return;
	}
	const FString Label = Choices[Index];
	if (Label == LeaveLabel)
	{
		Close();
		return;
	}
	if (Label == RetryLabel)
	{
		if (bRetryOpens)
		{
			EnterWaiting();
			Active->BeginConversation();
		}
		else
		{
			SendReply(PendingRetry);
		}
		return;
	}
	SendReply(Label);
}

void ANpcifyDialogueDirector::SendReply(const FString& Message)
{
	if (!Active || (Phase != EPhase::Choosing && !Active->AllowsCustomReply()))
	{
		return;
	}
	PendingRetry = Message;
	bRetryOpens = false;
	EnterWaiting();
	Active->Reply(Message);
}

void ANpcifyDialogueDirector::EnterWaiting()
{
	Phase = EPhase::Waiting;
	bRetryOpens = true;
	Widget->SetChoices({});
	Widget->SetReplyEnabled(false);
	Widget->SetHint(NSLOCTEXT("Npcify", "HintWaiting", "Q  Leave"));
}

void ANpcifyDialogueDirector::EnterChoosing()
{
	Phase = EPhase::Choosing;
	Widget->SetBody(FText::FromString(LastSpokenText));
	Choices = bEnding ? TArray<FString>() : Active->Npcify->GetCurrentReplyOptions();
	Choices.RemoveAll([](const FString& Choice) { return Choice.TrimStartAndEnd().IsEmpty(); });
	if (Choices.Num() == 0)
	{
		Choices.Add(LeaveLabel);
	}
	Widget->SetChoices(Choices);
	Widget->SetReplyEnabled(!bEnding);
	Widget->SetHint(Active->AllowsCustomReply()
		? NSLOCTEXT("Npcify", "HintChooseType", "1-9  Choose    Tab  Type    Q  Leave")
		: NSLOCTEXT("Npcify", "HintChoose", "1-9  Choose    Q  Leave"));
}

void ANpcifyDialogueDirector::ShowError(const FString& Message)
{
	UE_LOG(LogTemp, Warning, TEXT("Npcify demo: %s"), *Message);
	Phase = EPhase::Choosing;
	Widget->SetBody(FText::Format(NSLOCTEXT("Npcify", "Error", "The conversation stopped: {0}"), FText::FromString(Message)));
	Choices = { RetryLabel, LeaveLabel };
	Widget->SetChoices(Choices);
	Widget->SetReplyEnabled(false);
}

void ANpcifyDialogueDirector::HandleSpeechStart(const FString& StagingId, const FString& Text)
{
	LastSpokenText = Text;
	Phase = EPhase::Speaking;
	Widget->SetHint(NSLOCTEXT("Npcify", "HintSpeaking", "Space  Skip    Q  Leave"));
}

void ANpcifyDialogueDirector::HandleSpeechEnd(const FString& StagingId, const FString& Text)
{
	if (Phase == EPhase::Speaking)
	{
		LastSpokenText = Text;
		EnterChoosing();
	}
}

void ANpcifyDialogueDirector::HandleError(const FString& Message)
{
	// Audio problems do not stop the conversation; text and timeline still play.
	if (Phase == EPhase::Waiting)
	{
		ShowError(Message);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Npcify demo: %s"), *Message);
	}
}

void ANpcifyDialogueDirector::HandleConversationEnded()
{
	bEnding = true;
	EnterChoosing();
}

void ANpcifyDialogueDirector::SetPlayerControl(bool bEnabled)
{
	APlayerController* Controller = GetPlayerController();
	Controller->SetIgnoreMoveInput(!bEnabled);
	Controller->SetIgnoreLookInput(!bEnabled);
	Controller->SetShowMouseCursor(!bEnabled);
	if (bEnabled)
	{
		Controller->SetInputMode(FInputModeGameOnly());
	}
	else
	{
		FInputModeGameAndUI Mode;
		Mode.SetHideCursorDuringCapture(false);
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Controller->SetInputMode(Mode);
	}
}

ANpcifyNpc* ANpcifyDialogueDirector::FindNearbyNpc() const
{
	ANpcifyNpc* Best = nullptr;
	float BestDistance = InteractDistance;
	const FVector Player = GetPlayerPawn()->GetActorLocation();
	for (ANpcifyNpc* Npc : Npcs)
	{
		const float Distance = FVector::Dist2D(Player, Npc->GetActorLocation());
		if (Npc && Distance < BestDistance)
		{
			Best = Npc;
			BestDistance = Distance;
		}
	}
	return Best;
}

bool ANpcifyDialogueDirector::Project(const FVector& World, FVector2D& OutScreen) const
{
	return UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(GetPlayerController(), World, OutScreen, false);
}

APlayerController* ANpcifyDialogueDirector::GetPlayerController() const
{
	return UGameplayStatics::GetPlayerController(this, 0);
}

APawn* ANpcifyDialogueDirector::GetPlayerPawn() const
{
	const APlayerController* Controller = GetPlayerController();
	return Controller ? Controller->GetPawn() : nullptr;
}
