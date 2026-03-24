// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class TextureDiffusion3D : ModuleRules
{
	public TextureDiffusion3D(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				// ... add other public dependencies that you statically link with here ...
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"AssetRegistry",
				"AppFramework",
				"AssetTools",
				"Projects",
				"InputCore",
				"EditorFramework",
				"UnrealEd",
				"ToolMenus",
				"Core",
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
				"RHI",
				"RenderCore",
				"PropertyEditor",
				"EditorScriptingUtilities",
				"MaterialEditor",
				"ImageWriteQueue",
				"RenderCore",
				"HTTP",
				"Json",
				"JsonUtilities",
				"Projects",
				"MaterialEditor",
				"PropertyEditor",
				"ContentBrowser",
				"DesktopWidgets",
				"EditorStyle",
				"DeveloperSettings" ,
				"UnrealEd",
				"MikkTSpace",
				// ... add private dependencies that you statically link with here ...	
			}
			);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
