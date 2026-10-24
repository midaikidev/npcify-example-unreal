#include "NpcifyDialogueWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/ButtonSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Styling/CoreStyle.h"

namespace
{
	const FLinearColor PanelColor(0.006f, 0.007f, 0.012f, 0.86f);
	const FLinearColor AccentColor(0.96f, 0.6f, 0.13f, 1.f);
	const FLinearColor ChoiceColor(0.01f, 0.013f, 0.025f, 0.9f);
	const FLinearColor ChoiceHoverColor(0.04f, 0.055f, 0.1f, 0.95f);
	const FLinearColor MutedColor(0.6f, 0.63f, 0.7f, 1.f);

	FSlateBrush RoundedBrush(const FLinearColor& Color, float Radius = 6.f)
	{
		FSlateBrush Brush;
		Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
		Brush.TintColor = FSlateColor(Color);
		Brush.OutlineSettings.CornerRadii = FVector4(Radius, Radius, Radius, Radius);
		Brush.OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
		return Brush;
	}

	FButtonStyle ButtonStyle(const FLinearColor& Normal, const FLinearColor& Hovered)
	{
		FButtonStyle Style;
		Style.SetNormal(RoundedBrush(Normal));
		Style.SetHovered(RoundedBrush(Hovered));
		Style.SetPressed(RoundedBrush(AccentColor * FLinearColor(0.5f, 0.5f, 0.5f, 1.f)));
		Style.SetNormalPadding(FMargin(18.f, 10.f));
		Style.SetPressedPadding(FMargin(18.f, 10.f));
		return Style;
	}
}

void UNpcifyChoiceButton::Bind()
{
	OnClicked.AddDynamic(this, &UNpcifyChoiceButton::HandleClicked);
}

void UNpcifyChoiceButton::HandleClicked()
{
	OnChoice.ExecuteIfBound(Index);
}

TSharedRef<SWidget> UNpcifyDialogueWidget::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
	return Super::RebuildWidget();
}

UTextBlock* UNpcifyDialogueWidget::MakeText(int32 Size, const FLinearColor& Color, bool bBold)
{
	UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>();
	Text->SetFont(FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", Size));
	Text->SetColorAndOpacity(FSlateColor(Color));
	return Text;
}

void UNpcifyDialogueWidget::BuildTree()
{
	Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
	WidgetTree->RootWidget = Root;

	// Interaction prompt above the nearby NPC.
	Prompt = WidgetTree->ConstructWidget<UBorder>();
	Prompt->SetBrush(RoundedBrush(FLinearColor(0.f, 0.f, 0.f, 0.72f), 8.f));
	Prompt->SetPadding(FMargin(20.f, 10.f));
	PromptText = MakeText(20, FLinearColor::White);
	Prompt->SetContent(PromptText);
	UCanvasPanelSlot* PromptSlot = Root->AddChildToCanvas(Prompt);
	PromptSlot->SetAutoSize(true);
	PromptSlot->SetAlignment(FVector2D(0.5f, 1.f));

	// Dialogue box at the bottom of the screen.
	DialoguePanel = WidgetTree->ConstructWidget<UBorder>();
	DialoguePanel->SetBrush(RoundedBrush(PanelColor, 10.f));
	DialoguePanel->SetPadding(FMargin(36.f, 18.f, 28.f, 18.f));
	UCanvasPanelSlot* PanelSlot = Root->AddChildToCanvas(DialoguePanel);
	PanelSlot->SetAnchors(FAnchors(0.12f, 1.f, 0.88f, 1.f));
	PanelSlot->SetAlignment(FVector2D(0.f, 1.f));
	PanelSlot->SetOffsets(FMargin(0.f, -24.f, 0.f, 250.f));

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();
	DialoguePanel->SetContent(Column);

	UHorizontalBox* Header = WidgetTree->ConstructWidget<UHorizontalBox>();
	Column->AddChildToVerticalBox(Header);
	UBorder* NamePlate = WidgetTree->ConstructWidget<UBorder>();
	NamePlate->SetBrush(RoundedBrush(AccentColor, 6.f));
	NamePlate->SetPadding(FMargin(16.f, 4.f));
	NameText = MakeText(24, FLinearColor(0.02f, 0.02f, 0.03f, 1.f), true);
	NamePlate->SetContent(NameText);
	Header->AddChildToHorizontalBox(NamePlate);
	UHorizontalBoxSlot* ModeSlot = Header->AddChildToHorizontalBox(ModeText = MakeText(16, MutedColor));
	ModeSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	ModeSlot->SetHorizontalAlignment(HAlign_Right);
	ModeSlot->SetVerticalAlignment(VAlign_Center);
	ModeSlot->SetPadding(FMargin(0.f, 0.f, 16.f, 0.f));
	UButton* Close = WidgetTree->ConstructWidget<UButton>();
	Close->SetStyle(ButtonStyle(FLinearColor(0.f, 0.f, 0.f, 0.f), ChoiceHoverColor));
	Close->SetContent(MakeText(20, FLinearColor::White));
	Cast<UTextBlock>(Close->GetContent())->SetText(FText::FromString(TEXT("×")));
	Close->OnClicked.AddDynamic(this, &UNpcifyDialogueWidget::HandleCloseClicked);
	Header->AddChildToHorizontalBox(Close)->SetVerticalAlignment(VAlign_Center);

	BodyText = MakeText(26, FLinearColor::White);
	BodyText->SetAutoWrapText(true);
	UVerticalBoxSlot* BodySlot = Column->AddChildToVerticalBox(BodyText);
	BodySlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	BodySlot->SetPadding(FMargin(4.f, 18.f, 4.f, 10.f));

	UHorizontalBox* Footer = WidgetTree->ConstructWidget<UHorizontalBox>();
	Column->AddChildToVerticalBox(Footer);
	ReplyBox = WidgetTree->ConstructWidget<UEditableTextBox>();
	ReplyBox->SetHintText(NSLOCTEXT("Npcify", "ReplyHint", "Type your own reply and press Enter"));
	ReplyBox->WidgetStyle.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 18));
	ReplyBox->WidgetStyle.SetForegroundColor(FSlateColor(FLinearColor::White));
	ReplyBox->WidgetStyle.SetBackgroundImageNormal(RoundedBrush(FLinearColor(1.f, 1.f, 1.f, 0.06f)));
	ReplyBox->WidgetStyle.SetBackgroundImageHovered(RoundedBrush(FLinearColor(1.f, 1.f, 1.f, 0.1f)));
	ReplyBox->WidgetStyle.SetBackgroundImageFocused(RoundedBrush(FLinearColor(1.f, 1.f, 1.f, 0.12f)));
	ReplyBox->WidgetStyle.SetBackgroundImageReadOnly(RoundedBrush(FLinearColor(1.f, 1.f, 1.f, 0.04f)));
	ReplyBox->WidgetStyle.SetPadding(FMargin(12.f, 8.f));
	ReplyBox->OnTextCommitted.AddDynamic(this, &UNpcifyDialogueWidget::HandleReplyCommitted);
	UHorizontalBoxSlot* ReplySlot = Footer->AddChildToHorizontalBox(ReplyBox);
	ReplySlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	ReplySlot->SetPadding(FMargin(0.f, 0.f, 24.f, 0.f));
	HintText = MakeText(15, MutedColor);
	Footer->AddChildToHorizontalBox(HintText)->SetVerticalAlignment(VAlign_Center);

	// Reply choices above the right side of the dialogue box.
	ChoiceList = WidgetTree->ConstructWidget<UVerticalBox>();
	UCanvasPanelSlot* ChoiceSlot = Root->AddChildToCanvas(ChoiceList);
	ChoiceSlot->SetAnchors(FAnchors(0.88f, 1.f));
	ChoiceSlot->SetAlignment(FVector2D(1.f, 1.f));
	ChoiceSlot->SetAutoSize(true);
	ChoiceSlot->SetPosition(FVector2D(0.f, -290.f));

	HidePrompt();
	HideDialogue();
}

void UNpcifyDialogueWidget::SetLabelCount(int32 Count)
{
	while (Labels.Num() < Count)
	{
		UBorder* Label = WidgetTree->ConstructWidget<UBorder>();
		Label->SetBrush(RoundedBrush(FLinearColor(0.f, 0.f, 0.f, 0.f)));
		UVerticalBox* Lines = WidgetTree->ConstructWidget<UVerticalBox>();
		UTextBlock* Name = MakeText(20, FLinearColor::White, true);
		Name->SetShadowOffset(FVector2D(1.f, 1.f));
		Name->SetShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.9f));
		UTextBlock* Status = MakeText(14, AccentColor);
		Status->SetShadowOffset(FVector2D(1.f, 1.f));
		Status->SetShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.9f));
		Lines->AddChildToVerticalBox(Name)->SetHorizontalAlignment(HAlign_Center);
		Lines->AddChildToVerticalBox(Status)->SetHorizontalAlignment(HAlign_Center);
		Label->SetContent(Lines);
		UCanvasPanelSlot* Slot = Root->AddChildToCanvas(Label);
		Slot->SetAutoSize(true);
		Slot->SetAlignment(FVector2D(0.5f, 1.f));
		Slot->SetZOrder(-1);
		Labels.Add(Label);
	}
}

void UNpcifyDialogueWidget::SetLabel(int32 Index, bool bVisible, const FVector2D& Position, const FText& Name, const FText& Status)
{
	if (!Labels.IsValidIndex(Index))
	{
		return;
	}
	UBorder* Label = Labels[Index];
	Label->SetVisibility(bVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	if (!bVisible)
	{
		return;
	}
	Cast<UCanvasPanelSlot>(Label->Slot)->SetPosition(Position);
	UVerticalBox* Lines = Cast<UVerticalBox>(Label->GetContent());
	Cast<UTextBlock>(Lines->GetChildAt(0))->SetText(Name);
	Cast<UTextBlock>(Lines->GetChildAt(1))->SetText(Status);
}

void UNpcifyDialogueWidget::ShowPrompt(const FVector2D& Position, const FText& Text)
{
	Prompt->SetVisibility(ESlateVisibility::HitTestInvisible);
	Cast<UCanvasPanelSlot>(Prompt->Slot)->SetPosition(Position - FVector2D(0.f, 56.f));
	PromptText->SetText(Text);
}

void UNpcifyDialogueWidget::HidePrompt()
{
	Prompt->SetVisibility(ESlateVisibility::Collapsed);
}

void UNpcifyDialogueWidget::ShowDialogue(const FText& Speaker, const FText& Mode, bool bAllowCustomReply)
{
	DialoguePanel->SetVisibility(ESlateVisibility::Visible);
	ChoiceList->SetVisibility(ESlateVisibility::Visible);
	NameText->SetText(Speaker);
	ModeText->SetText(Mode);
	ReplyBox->SetText(FText::GetEmpty());
	ReplyBox->SetVisibility(bAllowCustomReply ? ESlateVisibility::Visible : ESlateVisibility::Hidden);
	SetBody(FText::GetEmpty());
	SetChoices({});
}

void UNpcifyDialogueWidget::HideDialogue()
{
	DialoguePanel->SetVisibility(ESlateVisibility::Collapsed);
	ChoiceList->SetVisibility(ESlateVisibility::Collapsed);
}

bool UNpcifyDialogueWidget::IsDialogueVisible() const
{
	return DialoguePanel->GetVisibility() == ESlateVisibility::Visible;
}

void UNpcifyDialogueWidget::SetBody(const FText& Text)
{
	BodyText->SetText(Text);
}

void UNpcifyDialogueWidget::SetHint(const FText& Text)
{
	HintText->SetText(Text);
}

void UNpcifyDialogueWidget::SetChoices(const TArray<FString>& Choices)
{
	ChoiceList->ClearChildren();
	for (int32 Index = 0; Index < Choices.Num(); ++Index)
	{
		UNpcifyChoiceButton* Button = WidgetTree->ConstructWidget<UNpcifyChoiceButton>();
		Button->SetStyle(ButtonStyle(ChoiceColor, ChoiceHoverColor));
		Button->Index = Index;
		Button->OnChoice.BindLambda([this](int32 Chosen) { OnChoiceClicked.ExecuteIfBound(Chosen); });
		Button->Bind();
		UTextBlock* Label = MakeText(20, FLinearColor::White);
		Label->SetText(FText::FromString(FString::Printf(TEXT("%d.  %s"), Index + 1, *Choices[Index])));
		Label->SetAutoWrapText(true);
		Label->SetJustification(ETextJustify::Left);
		Button->SetContent(Label);
		if (UButtonSlot* Content = Cast<UButtonSlot>(Label->Slot))
		{
			Content->SetHorizontalAlignment(HAlign_Fill);
		}
		USizeBox* Width = WidgetTree->ConstructWidget<USizeBox>();
		Width->SetWidthOverride(600.f);
		Width->SetContent(Button);
		ChoiceList->AddChildToVerticalBox(Width)->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
	}
}

void UNpcifyDialogueWidget::SetReplyEnabled(bool bEnabled)
{
	ReplyBox->SetIsEnabled(bEnabled);
}

bool UNpcifyDialogueWidget::IsReplyFocused() const
{
	return ReplyBox->HasKeyboardFocus();
}

void UNpcifyDialogueWidget::FocusReply()
{
	if (ReplyBox->GetVisibility() == ESlateVisibility::Visible)
	{
		ReplyBox->SetKeyboardFocus();
	}
}

void UNpcifyDialogueWidget::HandleReplyCommitted(const FText& Text, ETextCommit::Type CommitMethod)
{
	const FString Message = Text.ToString().TrimStartAndEnd();
	if (CommitMethod == ETextCommit::OnEnter && !Message.IsEmpty())
	{
		ReplyBox->SetText(FText::GetEmpty());
		OnReplySubmitted.ExecuteIfBound(Message);
	}
}

void UNpcifyDialogueWidget::HandleCloseClicked()
{
	OnCloseClicked.ExecuteIfBound();
}
