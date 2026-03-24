// In TextureProjectionTypes.h

#pragma once

#include "CoreMinimal.h"
#include "Engine/Texture2D.h"
// #include "JsonObject.h" 
#include "Dom/JsonObject.h"
#include "TextureProjectionTypes.generated.h"

USTRUCT(BlueprintType)
struct FProjectionSettings
{
	GENERATED_BODY()

	// --- Camera Settings ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projection Settings")
	FVector CameraPosition;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projection Settings")
	FRotator CameraRotation;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projection Settings")
	float FOVAngle;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projection Settings", meta = (ClampMin = "0.0", ClampMax = "90.0", UIMin = "0.0", UIMax = "90.0"))
	float FadeStartAngle;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projection Settings")
	float EdgeFalloff;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projection Settings")
	float Weight;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projection Settings")
	bool bUseComfyUI;

	// UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projection Settings")
	// FString BasePrompt;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projection Settings")
	TObjectPtr<UTexture2D> SourceTexture;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projection Settings")
    int32 TargetMaterialSlotIndex;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Projection Settings")
    FString TargetMaterialSlotName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComfyUI")
    FString WorkflowApiJson;
	
	// --- Internal Tab Management (Not a UPROPERTY) ---
	int32 TabId;
	FString TabName;

	TMap<FString, TSharedPtr<FJsonValue>> ComfyControlValues;
	TMap<FString, bool> ComfySeedStates;
	
	
	// --- Constructor with Defaults ---
	FProjectionSettings()
		: CameraPosition(FVector::ZeroVector)
		, CameraRotation(FRotator::ZeroRotator)
		, FOVAngle(45.0f)
		, FadeStartAngle(45.0f)
        , EdgeFalloff(2.0f)
		, Weight(1.0f)
		, bUseComfyUI(true)
		// , BasePrompt(TEXT("award-winning photo of object"))
		, SourceTexture(nullptr)
		, TargetMaterialSlotIndex(0)
        , TargetMaterialSlotName(TEXT("Slot 0"))
		, WorkflowApiJson(TEXT("Juggernaut_ControlNet.json"))
		, TabId(0)
		, TabName(TEXT("Camera 1"))

	{
		ComfyControlValues.Empty();
		ComfySeedStates.Empty();
	}
};