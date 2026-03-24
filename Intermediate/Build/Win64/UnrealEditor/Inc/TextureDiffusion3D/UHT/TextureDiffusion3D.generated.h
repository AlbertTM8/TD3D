// Copyright Epic Games, Inc. All Rights Reserved.
/*===========================================================================
	Generated code exported from UnrealHeaderTool.
	DO NOT modify this manually! Edit the corresponding .h files instead!
===========================================================================*/

// IWYU pragma: private, include "TextureDiffusion3D.h"

#ifdef TEXTUREDIFFUSION3D_TextureDiffusion3D_generated_h
#error "TextureDiffusion3D.generated.h already included, missing '#pragma once' in TextureDiffusion3D.h"
#endif
#define TEXTUREDIFFUSION3D_TextureDiffusion3D_generated_h

#include "Templates/IsUEnumClass.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ReflectedTypeAccessors.h"

PRAGMA_DISABLE_DEPRECATION_WARNINGS

#undef CURRENT_FILE_ID
#define CURRENT_FILE_ID FID_Unreal_Projects_Pls_Plugins_TextureDiffusion3D_Source_TextureDiffusion3D_Public_TextureDiffusion3D_h

// ********** Begin Enum EProjectionVariant ********************************************************
#define FOREACH_ENUM_EPROJECTIONVARIANT(op) \
	op(EProjectionVariant::BaseColor) \
	op(EProjectionVariant::Normal) \
	op(EProjectionVariant::Roughness) \
	op(EProjectionVariant::Metallic) \
	op(EProjectionVariant::AO) \
	op(EProjectionVariant::Shaded) 

enum class EProjectionVariant : uint8;
template<> struct TIsUEnumClass<EProjectionVariant> { enum { Value = true }; };
template<> TEXTUREDIFFUSION3D_API UEnum* StaticEnum<EProjectionVariant>();
// ********** End Enum EProjectionVariant **********************************************************

PRAGMA_ENABLE_DEPRECATION_WARNINGS
