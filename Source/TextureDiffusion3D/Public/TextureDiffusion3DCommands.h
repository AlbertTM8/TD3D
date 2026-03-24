// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Framework/Commands/Commands.h"
#include "TextureDiffusion3DStyle.h"

class FTextureDiffusion3DCommands : public TCommands<FTextureDiffusion3DCommands>
{
public:

	FTextureDiffusion3DCommands()
		: TCommands<FTextureDiffusion3DCommands>(TEXT("TextureDiffusion3D"), NSLOCTEXT("Contexts", "TextureDiffusion3D", "TextureDiffusion3D Plugin"), NAME_None, FTextureDiffusion3DStyle::GetStyleSetName())
	{
	}

	// TCommands<> interface
	virtual void RegisterCommands() override;

public:
	TSharedPtr< FUICommandInfo > PluginAction;
	TSharedPtr<FUICommandInfo> ProjectTextureAction;
};
