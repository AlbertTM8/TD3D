// Copyright Epic Games, Inc. All Rights Reserved.

#include "TextureDiffusion3DCommands.h"

#define LOCTEXT_NAMESPACE "FTextureDiffusion3DModule"

void FTextureDiffusion3DCommands::RegisterCommands()
{
    UI_COMMAND(PluginAction, "TextureDiffusion3D", "Execute TextureDiffusion3D action", EUserInterfaceActionType::Button, FInputChord());
    // Add this line for the new command
    UI_COMMAND(ProjectTextureAction, "Project Texture", "Project a texture onto a mesh from the current camera", EUserInterfaceActionType::Button, FInputChord());
}

#undef LOCTEXT_NAMESPACE
