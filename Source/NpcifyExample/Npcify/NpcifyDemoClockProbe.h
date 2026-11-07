#pragma once

#include "CoreMinimal.h"

namespace Npcify
{
    class FNpcifyCharacterRuntime;
}

/**
 * Measures how far the staging clock of a streamed reply is behind its voice. The end of a pending
 * block is an estimate; when the clock waits there while the audio plays on, the visemes after the
 * estimate come late. The voice time is the time since the block started (the component starts its
 * audio with the block). Sample once per frame, after the character ticked.
 */
class FNpcifyDemoClockProbe
{
public:
	void Sample(Npcify::FNpcifyCharacterRuntime& Runtime, TFunctionRef<void(const FString&)> Log);
	void Finish(TFunctionRef<void(const FString&)> Log);

private:
	void Begin(const FString& BlockId, bool bPending, double Estimate, const FString& Type);
	void Report(TFunctionRef<void(const FString&)> Log) const;

	struct FSample
	{
		double Audio = 0.0;
		double Clock = 0.0;
	};

	TArray<FSample> Samples;
	FString BlockId;
	FString BlockType;
	double BlockStart = 0.0;
	bool bPendingAtStart = false;
	double Estimate = 0.0;
	double FinalAt = -1.0;
	double FinalEnd = 0.0;
	double FirstCue = -1.0;
};
