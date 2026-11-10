#pragma once

#include "CoreMinimal.h"
#include "NpcifySightTypes.generated.h"

class USceneCaptureComponent2D;
class UTexture;

/** The state of sight (docs/sight.md). */
UENUM(BlueprintType)
enum class ENpcifySightState : uint8
{
    /** The character info did not turn sight on, or the conversation is baked. */
    Unavailable,
    Off,
    Starting,
    On,
};

/** The source and the schedule of sight. No source: the game viewport (with the HUD). */
USTRUCT(BlueprintType)
struct FNpcifySightOptions
{
    GENERATED_BODY()

    /** A render target or another texture, for example the media texture of a webcam. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Sight")
    TObjectPtr<UTexture> Texture = nullptr;

    /** A scene capture: its render target. A capture that does not render every frame renders when sight reads it. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Sight")
    TObjectPtr<USceneCaptureComponent2D> SceneCapture = nullptr;

    /** The texture shows the player's camera (source "camera"), not the game (source "game"). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Sight")
    bool bCameraSource = false;

    /** The longest time between two frames, in milliseconds. The minimum is 1000. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Sight", meta=(ClampMin="1000"))
    int32 IntervalMs = 5000;

    /** A change of the picture sends a frame at once (one each second at most). Off: only the interval. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Sight")
    bool bMotion = true;

    /** The mean change of the 32 x 18 luma grid between two checks that counts as motion, 0 to 255. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Sight", meta=(ClampMin="0", ClampMax="255"))
    float MotionThreshold = 8.0f;
};

/** One person or avatar in a frame. */
USTRUCT(BlueprintType)
struct FNpcifySightPerson
{
    GENERATED_BODY()

    /** left, center or right. */
    UPROPERTY(BlueprintReadOnly, Category="Npcify|Sight")
    FString Position;

    /** What the person does, in five words or fewer. */
    UPROPERTY(BlueprintReadOnly, Category="Npcify|Sight")
    FString Action;

    UPROPERTY(BlueprintReadOnly, Category="Npcify|Sight")
    bool bFacingCamera = false;
};

/** An observation of the conversation: what the vision model saw in one frame. */
USTRUCT(BlueprintType)
struct FNpcifySightObservation
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Npcify|Sight")
    FString ConversationId;

    UPROPERTY(BlueprintReadOnly, Category="Npcify|Sight")
    FString ObservationId;

    /** The capture time that the SDK sent, as an ISO 8601 string. */
    UPROPERTY(BlueprintReadOnly, Category="Npcify|Sight")
    FString CapturedAt;

    UPROPERTY(BlueprintReadOnly, Category="Npcify|Sight")
    int32 People = 0;

    UPROPERTY(BlueprintReadOnly, Category="Npcify|Sight")
    TArray<FNpcifySightPerson> Persons;

    /** Each readable piece of text, verbatim. */
    UPROPERTY(BlueprintReadOnly, Category="Npcify|Sight")
    TArray<FString> Text;

    /** One sentence about the frame, for the character. */
    UPROPERTY(BlueprintReadOnly, Category="Npcify|Sight")
    FString Summary;

    UPROPERTY(BlueprintReadOnly, Category="Npcify|Sight")
    FString Model;

    /** False while the charge is pending. */
    UPROPERTY(BlueprintReadOnly, Category="Npcify|Sight")
    bool bCostKnown = false;

    /** The points charged to the owner of the character. */
    UPROPERTY(BlueprintReadOnly, Category="Npcify|Sight")
    float CostPoints = 0.0f;
};
