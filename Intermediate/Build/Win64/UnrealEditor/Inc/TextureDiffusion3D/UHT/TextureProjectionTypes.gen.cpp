// Copyright Epic Games, Inc. All Rights Reserved.
/*===========================================================================
	Generated code exported from UnrealHeaderTool.
	DO NOT modify this manually! Edit the corresponding .h files instead!
===========================================================================*/

#include "UObject/GeneratedCppIncludes.h"
#include "Helpers/TextureProjectionTypes.h"

PRAGMA_DISABLE_DEPRECATION_WARNINGS

void EmptyLinkFunctionForGeneratedCodeTextureProjectionTypes() {}

// ********** Begin Cross Module References ********************************************************
COREUOBJECT_API UScriptStruct* Z_Construct_UScriptStruct_FRotator();
COREUOBJECT_API UScriptStruct* Z_Construct_UScriptStruct_FVector();
ENGINE_API UClass* Z_Construct_UClass_UTexture2D_NoRegister();
TEXTUREDIFFUSION3D_API UScriptStruct* Z_Construct_UScriptStruct_FProjectionSettings();
UPackage* Z_Construct_UPackage__Script_TextureDiffusion3D();
// ********** End Cross Module References **********************************************************

// ********** Begin ScriptStruct FProjectionSettings ***********************************************
static FStructRegistrationInfo Z_Registration_Info_UScriptStruct_FProjectionSettings;
class UScriptStruct* FProjectionSettings::StaticStruct()
{
	if (!Z_Registration_Info_UScriptStruct_FProjectionSettings.OuterSingleton)
	{
		Z_Registration_Info_UScriptStruct_FProjectionSettings.OuterSingleton = GetStaticStruct(Z_Construct_UScriptStruct_FProjectionSettings, (UObject*)Z_Construct_UPackage__Script_TextureDiffusion3D(), TEXT("ProjectionSettings"));
	}
	return Z_Registration_Info_UScriptStruct_FProjectionSettings.OuterSingleton;
}
struct Z_Construct_UScriptStruct_FProjectionSettings_Statics
{
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Struct_MetaDataParams[] = {
		{ "BlueprintType", "true" },
		{ "ModuleRelativePath", "Public/Helpers/TextureProjectionTypes.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_CameraPosition_MetaData[] = {
		{ "Category", "Projection Settings" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "// --- Camera Settings ---\n" },
#endif
		{ "ModuleRelativePath", "Public/Helpers/TextureProjectionTypes.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "--- Camera Settings ---" },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_CameraRotation_MetaData[] = {
		{ "Category", "Projection Settings" },
		{ "ModuleRelativePath", "Public/Helpers/TextureProjectionTypes.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_FOVAngle_MetaData[] = {
		{ "Category", "Projection Settings" },
		{ "ModuleRelativePath", "Public/Helpers/TextureProjectionTypes.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_FadeStartAngle_MetaData[] = {
		{ "Category", "Projection Settings" },
		{ "ClampMax", "90.0" },
		{ "ClampMin", "0.0" },
		{ "ModuleRelativePath", "Public/Helpers/TextureProjectionTypes.h" },
		{ "UIMax", "90.0" },
		{ "UIMin", "0.0" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_EdgeFalloff_MetaData[] = {
		{ "Category", "Projection Settings" },
		{ "ModuleRelativePath", "Public/Helpers/TextureProjectionTypes.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_Weight_MetaData[] = {
		{ "Category", "Projection Settings" },
		{ "ModuleRelativePath", "Public/Helpers/TextureProjectionTypes.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_bUseComfyUI_MetaData[] = {
		{ "Category", "Projection Settings" },
		{ "ModuleRelativePath", "Public/Helpers/TextureProjectionTypes.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_SourceTexture_MetaData[] = {
		{ "Category", "Projection Settings" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "// UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = \"Projection Settings\")\n// FString BasePrompt;\n" },
#endif
		{ "ModuleRelativePath", "Public/Helpers/TextureProjectionTypes.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = \"Projection Settings\")\nFString BasePrompt;" },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_TargetMaterialSlotIndex_MetaData[] = {
		{ "Category", "Projection Settings" },
		{ "ModuleRelativePath", "Public/Helpers/TextureProjectionTypes.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_TargetMaterialSlotName_MetaData[] = {
		{ "Category", "Projection Settings" },
		{ "ModuleRelativePath", "Public/Helpers/TextureProjectionTypes.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_WorkflowApiJson_MetaData[] = {
		{ "Category", "ComfyUI" },
		{ "ModuleRelativePath", "Public/Helpers/TextureProjectionTypes.h" },
	};
#endif // WITH_METADATA
	static const UECodeGen_Private::FStructPropertyParams NewProp_CameraPosition;
	static const UECodeGen_Private::FStructPropertyParams NewProp_CameraRotation;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_FOVAngle;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_FadeStartAngle;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_EdgeFalloff;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_Weight;
	static void NewProp_bUseComfyUI_SetBit(void* Obj);
	static const UECodeGen_Private::FBoolPropertyParams NewProp_bUseComfyUI;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_SourceTexture;
	static const UECodeGen_Private::FIntPropertyParams NewProp_TargetMaterialSlotIndex;
	static const UECodeGen_Private::FStrPropertyParams NewProp_TargetMaterialSlotName;
	static const UECodeGen_Private::FStrPropertyParams NewProp_WorkflowApiJson;
	static const UECodeGen_Private::FPropertyParamsBase* const PropPointers[];
	static void* NewStructOps()
	{
		return (UScriptStruct::ICppStructOps*)new UScriptStruct::TCppStructOps<FProjectionSettings>();
	}
	static const UECodeGen_Private::FStructParams StructParams;
};
const UECodeGen_Private::FStructPropertyParams Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_CameraPosition = { "CameraPosition", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Struct, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(FProjectionSettings, CameraPosition), Z_Construct_UScriptStruct_FVector, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_CameraPosition_MetaData), NewProp_CameraPosition_MetaData) };
const UECodeGen_Private::FStructPropertyParams Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_CameraRotation = { "CameraRotation", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Struct, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(FProjectionSettings, CameraRotation), Z_Construct_UScriptStruct_FRotator, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_CameraRotation_MetaData), NewProp_CameraRotation_MetaData) };
const UECodeGen_Private::FFloatPropertyParams Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_FOVAngle = { "FOVAngle", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Float, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(FProjectionSettings, FOVAngle), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_FOVAngle_MetaData), NewProp_FOVAngle_MetaData) };
const UECodeGen_Private::FFloatPropertyParams Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_FadeStartAngle = { "FadeStartAngle", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Float, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(FProjectionSettings, FadeStartAngle), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_FadeStartAngle_MetaData), NewProp_FadeStartAngle_MetaData) };
const UECodeGen_Private::FFloatPropertyParams Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_EdgeFalloff = { "EdgeFalloff", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Float, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(FProjectionSettings, EdgeFalloff), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_EdgeFalloff_MetaData), NewProp_EdgeFalloff_MetaData) };
const UECodeGen_Private::FFloatPropertyParams Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_Weight = { "Weight", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Float, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(FProjectionSettings, Weight), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_Weight_MetaData), NewProp_Weight_MetaData) };
void Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_bUseComfyUI_SetBit(void* Obj)
{
	((FProjectionSettings*)Obj)->bUseComfyUI = 1;
}
const UECodeGen_Private::FBoolPropertyParams Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_bUseComfyUI = { "bUseComfyUI", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Bool | UECodeGen_Private::EPropertyGenFlags::NativeBool, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, sizeof(bool), sizeof(FProjectionSettings), &Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_bUseComfyUI_SetBit, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_bUseComfyUI_MetaData), NewProp_bUseComfyUI_MetaData) };
const UECodeGen_Private::FObjectPropertyParams Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_SourceTexture = { "SourceTexture", nullptr, (EPropertyFlags)0x0114000000000005, UECodeGen_Private::EPropertyGenFlags::Object | UECodeGen_Private::EPropertyGenFlags::ObjectPtr, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(FProjectionSettings, SourceTexture), Z_Construct_UClass_UTexture2D_NoRegister, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_SourceTexture_MetaData), NewProp_SourceTexture_MetaData) };
const UECodeGen_Private::FIntPropertyParams Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_TargetMaterialSlotIndex = { "TargetMaterialSlotIndex", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Int, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(FProjectionSettings, TargetMaterialSlotIndex), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_TargetMaterialSlotIndex_MetaData), NewProp_TargetMaterialSlotIndex_MetaData) };
const UECodeGen_Private::FStrPropertyParams Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_TargetMaterialSlotName = { "TargetMaterialSlotName", nullptr, (EPropertyFlags)0x0010000000020015, UECodeGen_Private::EPropertyGenFlags::Str, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(FProjectionSettings, TargetMaterialSlotName), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_TargetMaterialSlotName_MetaData), NewProp_TargetMaterialSlotName_MetaData) };
const UECodeGen_Private::FStrPropertyParams Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_WorkflowApiJson = { "WorkflowApiJson", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Str, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(FProjectionSettings, WorkflowApiJson), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_WorkflowApiJson_MetaData), NewProp_WorkflowApiJson_MetaData) };
const UECodeGen_Private::FPropertyParamsBase* const Z_Construct_UScriptStruct_FProjectionSettings_Statics::PropPointers[] = {
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_CameraPosition,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_CameraRotation,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_FOVAngle,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_FadeStartAngle,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_EdgeFalloff,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_Weight,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_bUseComfyUI,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_SourceTexture,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_TargetMaterialSlotIndex,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_TargetMaterialSlotName,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewProp_WorkflowApiJson,
};
static_assert(UE_ARRAY_COUNT(Z_Construct_UScriptStruct_FProjectionSettings_Statics::PropPointers) < 2048);
const UECodeGen_Private::FStructParams Z_Construct_UScriptStruct_FProjectionSettings_Statics::StructParams = {
	(UObject* (*)())Z_Construct_UPackage__Script_TextureDiffusion3D,
	nullptr,
	&NewStructOps,
	"ProjectionSettings",
	Z_Construct_UScriptStruct_FProjectionSettings_Statics::PropPointers,
	UE_ARRAY_COUNT(Z_Construct_UScriptStruct_FProjectionSettings_Statics::PropPointers),
	sizeof(FProjectionSettings),
	alignof(FProjectionSettings),
	RF_Public|RF_Transient|RF_MarkAsNative,
	EStructFlags(0x00000001),
	METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UScriptStruct_FProjectionSettings_Statics::Struct_MetaDataParams), Z_Construct_UScriptStruct_FProjectionSettings_Statics::Struct_MetaDataParams)
};
UScriptStruct* Z_Construct_UScriptStruct_FProjectionSettings()
{
	if (!Z_Registration_Info_UScriptStruct_FProjectionSettings.InnerSingleton)
	{
		UECodeGen_Private::ConstructUScriptStruct(Z_Registration_Info_UScriptStruct_FProjectionSettings.InnerSingleton, Z_Construct_UScriptStruct_FProjectionSettings_Statics::StructParams);
	}
	return Z_Registration_Info_UScriptStruct_FProjectionSettings.InnerSingleton;
}
// ********** End ScriptStruct FProjectionSettings *************************************************

// ********** Begin Registration *******************************************************************
struct Z_CompiledInDeferFile_FID_Unreal_Projects_Pls_Plugins_TextureDiffusion3D_Source_TextureDiffusion3D_Public_Helpers_TextureProjectionTypes_h__Script_TextureDiffusion3D_Statics
{
	static constexpr FStructRegisterCompiledInInfo ScriptStructInfo[] = {
		{ FProjectionSettings::StaticStruct, Z_Construct_UScriptStruct_FProjectionSettings_Statics::NewStructOps, TEXT("ProjectionSettings"), &Z_Registration_Info_UScriptStruct_FProjectionSettings, CONSTRUCT_RELOAD_VERSION_INFO(FStructReloadVersionInfo, sizeof(FProjectionSettings), 2999382303U) },
	};
};
static FRegisterCompiledInInfo Z_CompiledInDeferFile_FID_Unreal_Projects_Pls_Plugins_TextureDiffusion3D_Source_TextureDiffusion3D_Public_Helpers_TextureProjectionTypes_h__Script_TextureDiffusion3D_714526731(TEXT("/Script/TextureDiffusion3D"),
	nullptr, 0,
	Z_CompiledInDeferFile_FID_Unreal_Projects_Pls_Plugins_TextureDiffusion3D_Source_TextureDiffusion3D_Public_Helpers_TextureProjectionTypes_h__Script_TextureDiffusion3D_Statics::ScriptStructInfo, UE_ARRAY_COUNT(Z_CompiledInDeferFile_FID_Unreal_Projects_Pls_Plugins_TextureDiffusion3D_Source_TextureDiffusion3D_Public_Helpers_TextureProjectionTypes_h__Script_TextureDiffusion3D_Statics::ScriptStructInfo),
	nullptr, 0);
// ********** End Registration *********************************************************************

PRAGMA_ENABLE_DEPRECATION_WARNINGS
