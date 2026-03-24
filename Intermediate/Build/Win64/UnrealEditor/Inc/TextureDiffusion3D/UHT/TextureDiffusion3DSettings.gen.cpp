// Copyright Epic Games, Inc. All Rights Reserved.
/*===========================================================================
	Generated code exported from UnrealHeaderTool.
	DO NOT modify this manually! Edit the corresponding .h files instead!
===========================================================================*/

#include "UObject/GeneratedCppIncludes.h"
#include "TextureDiffusion3DSettings.h"

PRAGMA_DISABLE_DEPRECATION_WARNINGS

void EmptyLinkFunctionForGeneratedCodeTextureDiffusion3DSettings() {}

// ********** Begin Cross Module References ********************************************************
COREUOBJECT_API UScriptStruct* Z_Construct_UScriptStruct_FDirectoryPath();
COREUOBJECT_API UScriptStruct* Z_Construct_UScriptStruct_FFilePath();
DEVELOPERSETTINGS_API UClass* Z_Construct_UClass_UDeveloperSettings();
TEXTUREDIFFUSION3D_API UClass* Z_Construct_UClass_UTextureDiffusion3DSettings();
TEXTUREDIFFUSION3D_API UClass* Z_Construct_UClass_UTextureDiffusion3DSettings_NoRegister();
UPackage* Z_Construct_UPackage__Script_TextureDiffusion3D();
// ********** End Cross Module References **********************************************************

// ********** Begin Class UTextureDiffusion3DSettings **********************************************
void UTextureDiffusion3DSettings::StaticRegisterNativesUTextureDiffusion3DSettings()
{
}
FClassRegistrationInfo Z_Registration_Info_UClass_UTextureDiffusion3DSettings;
UClass* UTextureDiffusion3DSettings::GetPrivateStaticClass()
{
	using TClass = UTextureDiffusion3DSettings;
	if (!Z_Registration_Info_UClass_UTextureDiffusion3DSettings.InnerSingleton)
	{
		GetPrivateStaticClassBody(
			StaticPackage(),
			TEXT("TextureDiffusion3DSettings"),
			Z_Registration_Info_UClass_UTextureDiffusion3DSettings.InnerSingleton,
			StaticRegisterNativesUTextureDiffusion3DSettings,
			sizeof(TClass),
			alignof(TClass),
			TClass::StaticClassFlags,
			TClass::StaticClassCastFlags(),
			TClass::StaticConfigName(),
			(UClass::ClassConstructorType)InternalConstructor<TClass>,
			(UClass::ClassVTableHelperCtorCallerType)InternalVTableHelperCtorCaller<TClass>,
			UOBJECT_CPPCLASS_STATICFUNCTIONS_FORCLASS(TClass),
			&TClass::Super::StaticClass,
			&TClass::WithinClass::StaticClass
		);
	}
	return Z_Registration_Info_UClass_UTextureDiffusion3DSettings.InnerSingleton;
}
UClass* Z_Construct_UClass_UTextureDiffusion3DSettings_NoRegister()
{
	return UTextureDiffusion3DSettings::GetPrivateStaticClass();
}
struct Z_Construct_UClass_UTextureDiffusion3DSettings_Statics
{
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Class_MetaDataParams[] = {
#if !UE_BUILD_SHIPPING
		{ "Comment", "/**\n * Settings for the Texture Diffusion 3D Plugin.\n * These settings are configured in Edit -> Editor Preferences -> Plugins -> Texture Diffusion 3D.\n */" },
#endif
		{ "DisplayName", "Texture Diffusion 3D" },
		{ "IncludePath", "TextureDiffusion3DSettings.h" },
		{ "ModuleRelativePath", "Public/TextureDiffusion3DSettings.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Settings for the Texture Diffusion 3D Plugin.\nThese settings are configured in Edit -> Editor Preferences -> Plugins -> Texture Diffusion 3D." },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_ComfyUIBasePath_MetaData[] = {
		{ "Category", "ComfyUI" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** The absolute path to the root directory of your ComfyUI installation. */" },
#endif
		{ "DisplayName", "ComfyUI Installation Path" },
		{ "ModuleRelativePath", "Public/TextureDiffusion3DSettings.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "The absolute path to the root directory of your ComfyUI installation." },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_ComfyUIServerAddress_MetaData[] = {
		{ "Category", "ComfyUI" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** The address of the ComfyUI server, including the port and trailing slash. */" },
#endif
		{ "DisplayName", "Server Address" },
		{ "ModuleRelativePath", "Public/TextureDiffusion3DSettings.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "The address of the ComfyUI server, including the port and trailing slash." },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_InitialProjectionWorkflow_MetaData[] = {
		{ "Category", "Workflows" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** The absolute path to the workflow file for the first camera. */" },
#endif
		{ "DisplayName", "Initial Projection Workflow" },
		{ "FilePathFilter", "json" },
		{ "ModuleRelativePath", "Public/TextureDiffusion3DSettings.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "The absolute path to the workflow file for the first camera." },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_InpaintProjectionWorkflow_MetaData[] = {
		{ "Category", "Workflows" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** The absolute path to the workflow file for subsequent cameras that inpaint on existing context. */" },
#endif
		{ "DisplayName", "Inpaint/Multi-View Workflow" },
		{ "FilePathFilter", "json" },
		{ "ModuleRelativePath", "Public/TextureDiffusion3DSettings.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "The absolute path to the workflow file for subsequent cameras that inpaint on existing context." },
#endif
	};
#endif // WITH_METADATA
	static const UECodeGen_Private::FStructPropertyParams NewProp_ComfyUIBasePath;
	static const UECodeGen_Private::FStrPropertyParams NewProp_ComfyUIServerAddress;
	static const UECodeGen_Private::FStructPropertyParams NewProp_InitialProjectionWorkflow;
	static const UECodeGen_Private::FStructPropertyParams NewProp_InpaintProjectionWorkflow;
	static const UECodeGen_Private::FPropertyParamsBase* const PropPointers[];
	static UObject* (*const DependentSingletons[])();
	static constexpr FCppClassTypeInfoStatic StaticCppClassTypeInfo = {
		TCppClassTypeTraits<UTextureDiffusion3DSettings>::IsAbstract,
	};
	static const UECodeGen_Private::FClassParams ClassParams;
};
const UECodeGen_Private::FStructPropertyParams Z_Construct_UClass_UTextureDiffusion3DSettings_Statics::NewProp_ComfyUIBasePath = { "ComfyUIBasePath", nullptr, (EPropertyFlags)0x0010000000004001, UECodeGen_Private::EPropertyGenFlags::Struct, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UTextureDiffusion3DSettings, ComfyUIBasePath), Z_Construct_UScriptStruct_FDirectoryPath, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_ComfyUIBasePath_MetaData), NewProp_ComfyUIBasePath_MetaData) };
const UECodeGen_Private::FStrPropertyParams Z_Construct_UClass_UTextureDiffusion3DSettings_Statics::NewProp_ComfyUIServerAddress = { "ComfyUIServerAddress", nullptr, (EPropertyFlags)0x0010000000004001, UECodeGen_Private::EPropertyGenFlags::Str, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UTextureDiffusion3DSettings, ComfyUIServerAddress), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_ComfyUIServerAddress_MetaData), NewProp_ComfyUIServerAddress_MetaData) };
const UECodeGen_Private::FStructPropertyParams Z_Construct_UClass_UTextureDiffusion3DSettings_Statics::NewProp_InitialProjectionWorkflow = { "InitialProjectionWorkflow", nullptr, (EPropertyFlags)0x0010000000004001, UECodeGen_Private::EPropertyGenFlags::Struct, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UTextureDiffusion3DSettings, InitialProjectionWorkflow), Z_Construct_UScriptStruct_FFilePath, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_InitialProjectionWorkflow_MetaData), NewProp_InitialProjectionWorkflow_MetaData) };
const UECodeGen_Private::FStructPropertyParams Z_Construct_UClass_UTextureDiffusion3DSettings_Statics::NewProp_InpaintProjectionWorkflow = { "InpaintProjectionWorkflow", nullptr, (EPropertyFlags)0x0010000000004001, UECodeGen_Private::EPropertyGenFlags::Struct, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UTextureDiffusion3DSettings, InpaintProjectionWorkflow), Z_Construct_UScriptStruct_FFilePath, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_InpaintProjectionWorkflow_MetaData), NewProp_InpaintProjectionWorkflow_MetaData) };
const UECodeGen_Private::FPropertyParamsBase* const Z_Construct_UClass_UTextureDiffusion3DSettings_Statics::PropPointers[] = {
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UTextureDiffusion3DSettings_Statics::NewProp_ComfyUIBasePath,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UTextureDiffusion3DSettings_Statics::NewProp_ComfyUIServerAddress,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UTextureDiffusion3DSettings_Statics::NewProp_InitialProjectionWorkflow,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UTextureDiffusion3DSettings_Statics::NewProp_InpaintProjectionWorkflow,
};
static_assert(UE_ARRAY_COUNT(Z_Construct_UClass_UTextureDiffusion3DSettings_Statics::PropPointers) < 2048);
UObject* (*const Z_Construct_UClass_UTextureDiffusion3DSettings_Statics::DependentSingletons[])() = {
	(UObject* (*)())Z_Construct_UClass_UDeveloperSettings,
	(UObject* (*)())Z_Construct_UPackage__Script_TextureDiffusion3D,
};
static_assert(UE_ARRAY_COUNT(Z_Construct_UClass_UTextureDiffusion3DSettings_Statics::DependentSingletons) < 16);
const UECodeGen_Private::FClassParams Z_Construct_UClass_UTextureDiffusion3DSettings_Statics::ClassParams = {
	&UTextureDiffusion3DSettings::StaticClass,
	"EditorSettings",
	&StaticCppClassTypeInfo,
	DependentSingletons,
	nullptr,
	Z_Construct_UClass_UTextureDiffusion3DSettings_Statics::PropPointers,
	nullptr,
	UE_ARRAY_COUNT(DependentSingletons),
	0,
	UE_ARRAY_COUNT(Z_Construct_UClass_UTextureDiffusion3DSettings_Statics::PropPointers),
	0,
	0x081000A4u,
	METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UClass_UTextureDiffusion3DSettings_Statics::Class_MetaDataParams), Z_Construct_UClass_UTextureDiffusion3DSettings_Statics::Class_MetaDataParams)
};
UClass* Z_Construct_UClass_UTextureDiffusion3DSettings()
{
	if (!Z_Registration_Info_UClass_UTextureDiffusion3DSettings.OuterSingleton)
	{
		UECodeGen_Private::ConstructUClass(Z_Registration_Info_UClass_UTextureDiffusion3DSettings.OuterSingleton, Z_Construct_UClass_UTextureDiffusion3DSettings_Statics::ClassParams);
	}
	return Z_Registration_Info_UClass_UTextureDiffusion3DSettings.OuterSingleton;
}
DEFINE_VTABLE_PTR_HELPER_CTOR(UTextureDiffusion3DSettings);
UTextureDiffusion3DSettings::~UTextureDiffusion3DSettings() {}
// ********** End Class UTextureDiffusion3DSettings ************************************************

// ********** Begin Registration *******************************************************************
struct Z_CompiledInDeferFile_FID_Unreal_Projects_Pls_Plugins_TextureDiffusion3D_Source_TextureDiffusion3D_Public_TextureDiffusion3DSettings_h__Script_TextureDiffusion3D_Statics
{
	static constexpr FClassRegisterCompiledInInfo ClassInfo[] = {
		{ Z_Construct_UClass_UTextureDiffusion3DSettings, UTextureDiffusion3DSettings::StaticClass, TEXT("UTextureDiffusion3DSettings"), &Z_Registration_Info_UClass_UTextureDiffusion3DSettings, CONSTRUCT_RELOAD_VERSION_INFO(FClassReloadVersionInfo, sizeof(UTextureDiffusion3DSettings), 829820336U) },
	};
};
static FRegisterCompiledInInfo Z_CompiledInDeferFile_FID_Unreal_Projects_Pls_Plugins_TextureDiffusion3D_Source_TextureDiffusion3D_Public_TextureDiffusion3DSettings_h__Script_TextureDiffusion3D_4198832260(TEXT("/Script/TextureDiffusion3D"),
	Z_CompiledInDeferFile_FID_Unreal_Projects_Pls_Plugins_TextureDiffusion3D_Source_TextureDiffusion3D_Public_TextureDiffusion3DSettings_h__Script_TextureDiffusion3D_Statics::ClassInfo, UE_ARRAY_COUNT(Z_CompiledInDeferFile_FID_Unreal_Projects_Pls_Plugins_TextureDiffusion3D_Source_TextureDiffusion3D_Public_TextureDiffusion3DSettings_h__Script_TextureDiffusion3D_Statics::ClassInfo),
	nullptr, 0,
	nullptr, 0);
// ********** End Registration *********************************************************************

PRAGMA_ENABLE_DEPRECATION_WARNINGS
