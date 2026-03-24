// Copyright Epic Games, Inc. All Rights Reserved.

#include "TextureDiffusion3DStyle.h"
#include "TextureDiffusion3D.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/SlateStyleRegistry.h"
#include "Slate/SlateGameResources.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyleMacros.h"

#define RootToContentDir Style->RootToContentDir

TSharedPtr<FSlateStyleSet> FTextureDiffusion3DStyle::StyleInstance = nullptr;

void FTextureDiffusion3DStyle::Initialize()
{
	if (!StyleInstance.IsValid())
	{
		StyleInstance = Create();
		FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
	}
}

void FTextureDiffusion3DStyle::Shutdown()
{
	FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
	ensure(StyleInstance.IsUnique());
	StyleInstance.Reset();
}

FName FTextureDiffusion3DStyle::GetStyleSetName()
{
	static FName StyleSetName(TEXT("TextureDiffusion3DStyle"));
	return StyleSetName;
}


const FVector2D Icon16x16(16.0f, 16.0f);
const FVector2D Icon20x20(20.0f, 20.0f);

TSharedRef< FSlateStyleSet > FTextureDiffusion3DStyle::Create()
{
	TSharedRef< FSlateStyleSet > Style = MakeShareable(new FSlateStyleSet("TextureDiffusion3DStyle"));
	Style->SetContentRoot(IPluginManager::Get().FindPlugin("TextureDiffusion3D")->GetBaseDir() / TEXT("Resources"));

	Style->Set("TextureDiffusion3D.PluginAction", new IMAGE_BRUSH_SVG(TEXT("PlaceholderButtonIcon"), Icon20x20));
	return Style;
}

void FTextureDiffusion3DStyle::ReloadTextures()
{
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().GetRenderer()->ReloadTextureResources();
	}
}

const ISlateStyle& FTextureDiffusion3DStyle::Get()
{
	return *StyleInstance;
}
