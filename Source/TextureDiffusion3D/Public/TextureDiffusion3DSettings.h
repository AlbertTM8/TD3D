// TextureDiffusion3DSettings.h
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "TextureDiffusion3DSettings.generated.h"

/**
 * Settings for the Texture Diffusion 3D Plugin.
 * These settings are configured in Edit -> Editor Preferences -> Plugins -> Texture Diffusion 3D.
 */
UCLASS(Config = EditorSettings, GlobalUserConfig, meta = (DisplayName = "Texture Diffusion 3D"))
class TEXTUREDIFFUSION3D_API UTextureDiffusion3DSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UTextureDiffusion3DSettings();

    virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

    /** The absolute path to the root directory of your ComfyUI installation. */
    UPROPERTY(Config, EditAnywhere, Category = "ComfyUI", meta = (DisplayName = "ComfyUI Installation Path"))
    FDirectoryPath ComfyUIBasePath;

    /** The address of the ComfyUI server, including the port and trailing slash. */
    UPROPERTY(Config, EditAnywhere, Category = "ComfyUI", meta = (DisplayName = "Server Address"))
    FString ComfyUIServerAddress;

    /** The absolute path to the workflow file for the first camera. */
    UPROPERTY(Config, EditAnywhere, Category = "Workflows", meta = (DisplayName = "Initial Projection Workflow", FilePathFilter = "json"))
    FFilePath InitialProjectionWorkflow;

    /** The absolute path to the workflow file for subsequent cameras that inpaint on existing context. */
    UPROPERTY(Config, EditAnywhere, Category = "Workflows", meta = (DisplayName = "Inpaint/Multi-View Workflow", FilePathFilter = "json"))
    FFilePath InpaintProjectionWorkflow;

};