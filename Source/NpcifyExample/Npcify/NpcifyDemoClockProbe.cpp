#include "NpcifyDemoClockProbe.h"

#include "Core/NpcifyCharacterRuntime.h"
#include "HAL/PlatformTime.h"

void FNpcifyDemoClockProbe::Sample(Npcify::FNpcifyCharacterRuntime& Runtime, TFunctionRef<void(const FString&)> Log)
{
	// Only the inline members of the SDK core link from another module: the block that plays is
	// the last block that the clock reached.
	Npcify::FNpcifyStagingPlayer& Player = Runtime.GetStagingPlayer();
	const Npcify::FStagingResultPayload* Staging = Player.IsPlaying() ? Player.GetCurrentStaging() : nullptr;
	const Npcify::FStagingDialogueBlock* Block = nullptr;
	if (Staging)
	{
		for (const Npcify::FStagingDialogueBlock& Item : Staging->Timeline.Dialogue)
		{
			if (Item.StartTime <= Player.GetTimeSeconds() + 1e-9)
			{
				Block = &Item;
			}
		}
	}
	const FString Id = Block ? FString(UTF8_TO_TCHAR(Block->Id.c_str())) : FString();
	if (Id != BlockId)
	{
		Report(Log);
		Begin(Id, Block && Block->bPending, Block ? Block->EndTime - Block->StartTime : 0.0,
			Block ? FString(UTF8_TO_TCHAR(Block->Type.c_str())) : FString());
	}
	if (!Block || BlockType != TEXT("speech"))
	{
		return;
	}
	const double Audio = FPlatformTime::Seconds() - BlockStart;
	if (Block->bPending)
	{
		Estimate = FMath::Max(Estimate, Block->EndTime - Block->StartTime);
	}
	else if (FinalAt < 0.0)
	{
		FinalAt = Audio;
		FinalEnd = Block->EndTime - Block->StartTime;
		for (const Npcify::FStagingVisemeCue& Cue : Block->Visemes)
		{
			if (Cue.Time > Estimate && (FirstCue < 0.0 || Cue.Time < FirstCue))
			{
				FirstCue = Cue.Time;
			}
		}
	}
	Samples.Add(FSample{Audio, Player.GetTimeSeconds() - Block->StartTime});
}

void FNpcifyDemoClockProbe::Finish(TFunctionRef<void(const FString&)> Log)
{
	Report(Log);
	Begin(FString(), false, 0.0, FString());
}

void FNpcifyDemoClockProbe::Begin(const FString& InBlockId, bool bPending, double InEstimate, const FString& Type)
{
	BlockId = InBlockId;
	BlockType = Type;
	Samples.Reset();
	BlockStart = FPlatformTime::Seconds();
	bPendingAtStart = bPending;
	Estimate = InEstimate;
	FinalAt = bPending ? -1.0 : 0.0;
	FinalEnd = InEstimate;
	FirstCue = -1.0;
}

void FNpcifyDemoClockProbe::Report(TFunctionRef<void(const FString&)> Log) const
{
	if (BlockId.IsEmpty() || BlockType != TEXT("speech") || Samples.Num() == 0)
	{
		return;
	}
	// The final end of a block is the length of its audio (the server and minimp3 decode the same samples).
	const double End = FinalEnd;
	double Before = 0.0;
	double After = 0.0;
	double CueLag = -1.0;
	bool bCueLag = false;
	for (const FSample& Item : Samples)
	{
		const double Lag = Item.Audio - Item.Clock;
		if (Item.Audio <= Estimate)
		{
			Before = FMath::Max(Before, Lag);
		}
		else if (Item.Audio <= End)
		{
			After = FMath::Max(After, Lag);
		}
		if (FirstCue >= 0.0 && !bCueLag && Item.Clock >= FirstCue)
		{
			CueLag = Lag;
			bCueLag = true;
		}
	}
	Log(FString::Printf(
		TEXT("clock probe: block %s pending at start %d, estimated end %.3f s, final version at %s (end %.3f s); ")
		TEXT("clock lag before the estimate %.3f s, between the estimate and the end of the audio %.3f s; first viseme after the estimate %s"),
		*BlockId, bPendingAtStart ? 1 : 0, Estimate,
		FinalAt < 0.0 ? TEXT("none") : *FString::Printf(TEXT("%.3f s of the audio"), FinalAt), FinalEnd, Before, After,
		FirstCue < 0.0 ? TEXT("none") : *FString::Printf(TEXT("at %.3f s, lag %.3f s"), FirstCue, CueLag)));
}
