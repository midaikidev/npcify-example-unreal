#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "NpcifyDialogueWidget.generated.h"

class UBorder;
class UCanvasPanel;
class UEditableTextBox;
class UTextBlock;
class UVerticalBox;

DECLARE_DELEGATE_OneParam(FNpcifyChoiceClicked, int32);
DECLARE_DELEGATE_OneParam(FNpcifyReplySubmitted, const FString&);

/** A reply choice button that reports its index. */
UCLASS()
class UNpcifyChoiceButton : public UButton
{
	GENERATED_BODY()

public:
	int32 Index = 0;
	FNpcifyChoiceClicked OnChoice;

	void Bind();

private:
	UFUNCTION()
	void HandleClicked();
};

/**
 * RPG-style screen UI built in code: NPC name labels, the interaction prompt, and the
 * dialogue box with typewriter text, numbered reply choices and a free-text reply.
 */
UCLASS()
class NPCIFYEXAMPLE_API UNpcifyDialogueWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	FNpcifyChoiceClicked OnChoiceClicked;
	FNpcifyReplySubmitted OnReplySubmitted;
	FSimpleDelegate OnCloseClicked;

	void SetLabelCount(int32 Count);
	void SetLabel(int32 Index, bool bVisible, const FVector2D& Position, const FText& Name, const FText& Status);

	void ShowPrompt(const FVector2D& Position, const FText& Text);
	void HidePrompt();

	void ShowDialogue(const FText& Speaker, const FText& Mode, bool bAllowCustomReply);
	void HideDialogue();
	bool IsDialogueVisible() const;
	void SetBody(const FText& Text);
	void SetHint(const FText& Text);
	void SetChoices(const TArray<FString>& Labels);
	void SetReplyEnabled(bool bEnabled);
	bool IsReplyFocused() const;
	void FocusReply();

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	void BuildTree();
	UTextBlock* MakeText(int32 Size, const FLinearColor& Color, bool bBold = false);

	UFUNCTION()
	void HandleReplyCommitted(const FText& Text, ETextCommit::Type CommitMethod);

	UFUNCTION()
	void HandleCloseClicked();

	UPROPERTY(Transient)
	TObjectPtr<UCanvasPanel> Root;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UBorder>> Labels;

	UPROPERTY(Transient)
	TObjectPtr<UBorder> Prompt;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> PromptText;

	UPROPERTY(Transient)
	TObjectPtr<UBorder> DialoguePanel;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> NameText;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> ModeText;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> BodyText;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> HintText;

	UPROPERTY(Transient)
	TObjectPtr<UEditableTextBox> ReplyBox;

	UPROPERTY(Transient)
	TObjectPtr<UVerticalBox> ChoiceList;
};
