// Copyright Epic Games, Inc. All Rights Reserved.
/*===========================================================================
	Generated code exported from UnrealHeaderTool.
	DO NOT modify this manually! Edit the corresponding .h files instead!
===========================================================================*/

#include "UObject/GeneratedCppIncludes.h"
#include "TextureDiffusion3D.h"

PRAGMA_DISABLE_DEPRECATION_WARNINGS

void EmptyLinkFunctionForGeneratedCodeTextureDiffusion3D() {}

// ********** Begin Cross Module References ********************************************************
TEXTUREDIFFUSION3D_API UEnum* Z_Construct_UEnum_TextureDiffusion3D_EProjectionVariant();
UPackage* Z_Construct_UPackage__Script_TextureDiffusion3D();
// ********** End Cross Module References **********************************************************

// ********** Begin Enum EProjectionVariant ********************************************************
static FEnumRegistrationInfo Z_Registration_Info_UEnum_EProjectionVariant;
static UEnum* EProjectionVariant_StaticEnum()
{
	if (!Z_Registration_Info_UEnum_EProjectionVariant.OuterSingleton)
	{
		Z_Registration_Info_UEnum_EProjectionVariant.OuterSingleton = GetStaticEnum(Z_Construct_UEnum_TextureDiffusion3D_EProjectionVariant, (UObject*)Z_Construct_UPackage__Script_TextureDiffusion3D(), TEXT("EProjectionVariant"));
	}
	return Z_Registration_Info_UEnum_EProjectionVariant.OuterSingleton;
}
template<> TEXTUREDIFFUSION3D_API UEnum* StaticEnum<EProjectionVariant>()
{
	return EProjectionVariant_StaticEnum();
}
struct Z_Construct_UEnum_TextureDiffusion3D_EProjectionVariant_Statics
{
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Enum_MetaDataParams[] = {
		{ "AO.Name", "EProjectionVariant::AO" },
		{ "BaseColor.Name", "EProjectionVariant::BaseColor" },
		{ "BlueprintType", "true" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/**\n * New Enum for managing different texture projection types.\n */" },
#endif
		{ "Metallic.Name", "EProjectionVariant::Metallic" },
		{ "ModuleRelativePath", "Public/TextureDiffusion3D.h" },
		{ "Normal.Name", "EProjectionVariant::Normal" },
		{ "Roughness.Name", "EProjectionVariant::Roughness" },
		{ "Shaded.Name", "EProjectionVariant::Shaded" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "New Enum for managing different texture projection types." },
#endif
	};
#endif // WITH_METADATA
	static constexpr UECodeGen_Private::FEnumeratorParam Enumerators[] = {
		{ "EProjectionVariant::BaseColor", (int64)EProjectionVariant::BaseColor },
		{ "EProjectionVariant::Normal", (int64)EProjectionVariant::Normal },
		{ "EProjectionVariant::Roughness", (int64)EProjectionVariant::Roughness },
		{ "EProjectionVariant::Metallic", (int64)EProjectionVariant::Metallic },
		{ "EProjectionVariant::AO", (int64)EProjectionVariant::AO },
		{ "EProjectionVariant::Shaded", (int64)EProjectionVariant::Shaded },
	};
	static const UECodeGen_Private::FEnumParams EnumParams;
};
const UECodeGen_Private::FEnumParams Z_Construct_UEnum_TextureDiffusion3D_EProjectionVariant_Statics::EnumParams = {
	(UObject*(*)())Z_Construct_UPackage__Script_TextureDiffusion3D,
	nullptr,
	"EProjectionVariant",
	"EProjectionVariant",
	Z_Construct_UEnum_TextureDiffusion3D_EProjectionVariant_Statics::Enumerators,
	RF_Public|RF_Transient|RF_MarkAsNative,
	UE_ARRAY_COUNT(Z_Construct_UEnum_TextureDiffusion3D_EProjectionVariant_Statics::Enumerators),
	EEnumFlags::None,
	(uint8)UEnum::ECppForm::EnumClass,
	METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UEnum_TextureDiffusion3D_EProjectionVariant_Statics::Enum_MetaDataParams), Z_Construct_UEnum_TextureDiffusion3D_EProjectionVariant_Statics::Enum_MetaDataParams)
};
UEnum* Z_Construct_UEnum_TextureDiffusion3D_EProjectionVariant()
{
	if (!Z_Registration_Info_UEnum_EProjectionVariant.InnerSingleton)
	{
		UECodeGen_Private::ConstructUEnum(Z_Registration_Info_UEnum_EProjectionVariant.InnerSingleton, Z_Construct_UEnum_TextureDiffusion3D_EProjectionVariant_Statics::EnumParams);
	}
	return Z_Registration_Info_UEnum_EProjectionVariant.InnerSingleton;
}
// ********** End Enum EProjectionVariant **********************************************************

// ********** Begin Registration *******************************************************************
struct Z_CompiledInDeferFile_FID_Unreal_Projects_Pls_Plugins_TextureDiffusion3D_Source_TextureDiffusion3D_Public_TextureDiffusion3D_h__Script_TextureDiffusion3D_Statics
{
	static constexpr FEnumRegisterCompiledInInfo EnumInfo[] = {
		{ EProjectionVariant_StaticEnum, TEXT("EProjectionVariant"), &Z_Registration_Info_UEnum_EProjectionVariant, CONSTRUCT_RELOAD_VERSION_INFO(FEnumReloadVersionInfo, 3192039925U) },
	};
};
static FRegisterCompiledInInfo Z_CompiledInDeferFile_FID_Unreal_Projects_Pls_Plugins_TextureDiffusion3D_Source_TextureDiffusion3D_Public_TextureDiffusion3D_h__Script_TextureDiffusion3D_960488286(TEXT("/Script/TextureDiffusion3D"),
	nullptr, 0,
	nullptr, 0,
	Z_CompiledInDeferFile_FID_Unreal_Projects_Pls_Plugins_TextureDiffusion3D_Source_TextureDiffusion3D_Public_TextureDiffusion3D_h__Script_TextureDiffusion3D_Statics::EnumInfo, UE_ARRAY_COUNT(Z_CompiledInDeferFile_FID_Unreal_Projects_Pls_Plugins_TextureDiffusion3D_Source_TextureDiffusion3D_Public_TextureDiffusion3D_h__Script_TextureDiffusion3D_Statics::EnumInfo));
// ********** End Registration *********************************************************************

PRAGMA_ENABLE_DEPRECATION_WARNINGS
