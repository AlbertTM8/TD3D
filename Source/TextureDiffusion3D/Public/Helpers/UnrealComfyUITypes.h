// UnrealComfyUITypes.h

#pragma once

#include "CoreMinimal.h"

// Represents a single editable input widget from a tagged ComfyUI node.
struct FUnrealInputInfo
{
    FString InputName;
    EJson JsonType;
};
// Represents a whole ComfyUI node that has been tagged with the "Unreal" prefix.
struct FUnrealNodeInfo
{
    FString NodeId;
    FString CleanTitle;
    bool bIsImageInput;
    FString ImageKeyword;
    TArray<FUnrealInputInfo> ExposedInputs;
};