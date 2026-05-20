// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"

#ifndef AIK_METAHUMAN_DISABLED

#include "MetaHumanCharacter.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#include "MetaHumanCharacterSkin.h"
#include "MetaHumanCharacterEyes.h"
#include "MetaHumanCharacterMakeup.h"
#include "MetaHumanCharacterTeeth.h"
#include "MetaHumanCharacterViewport.h"
#include "MetaHumanCharacterAssemblySettings.h"
#endif
#include "MetaHumanBodyType.h"
#include "MetaHumanTypes.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#include "MetaHumanCharacterEditorSubsystem.h"
#include "MetaHumanCharacterBodyIdentity.h"
#include "MetaHumanCharacterIdentity.h"
#include "MetaHumanCharacterEditorActorInterface.h"
#include "Subsystem/MetaHumanCharacterBuild.h"
#if __has_include("MetaHumanIdentity.h")
#include "MetaHumanIdentity.h"
#define AIK_HAS_METAHUMAN_IDENTITY 1
#else
#define AIK_HAS_METAHUMAN_IDENTITY 0
#endif
#endif
#if __has_include("MetaHumanWardrobeItem.h")
#include "MetaHumanWardrobeItem.h"
#include "MetaHumanCharacterPalette.h"
#include "MetaHumanCharacterInstance.h"
#include "MetaHumanCollection.h"
#define AIK_HAS_METAHUMAN_WARDROBE 1
#else
#define AIK_HAS_METAHUMAN_WARDROBE 0
#endif
#include "ScopedTransaction.h"
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static const char* BodyTypeToString(EMetaHumanBodyType Type)
{
	switch (Type)
	{
	case EMetaHumanBodyType::f_med_nrw: return "f_med_nrw";
	case EMetaHumanBodyType::f_med_ovw: return "f_med_ovw";
	case EMetaHumanBodyType::f_med_unw: return "f_med_unw";
	case EMetaHumanBodyType::f_srt_nrw: return "f_srt_nrw";
	case EMetaHumanBodyType::f_srt_ovw: return "f_srt_ovw";
	case EMetaHumanBodyType::f_srt_unw: return "f_srt_unw";
	case EMetaHumanBodyType::f_tal_nrw: return "f_tal_nrw";
	case EMetaHumanBodyType::f_tal_ovw: return "f_tal_ovw";
	case EMetaHumanBodyType::f_tal_unw: return "f_tal_unw";
	case EMetaHumanBodyType::m_med_nrw: return "m_med_nrw";
	case EMetaHumanBodyType::m_med_ovw: return "m_med_ovw";
	case EMetaHumanBodyType::m_med_unw: return "m_med_unw";
	case EMetaHumanBodyType::m_srt_nrw: return "m_srt_nrw";
	case EMetaHumanBodyType::m_srt_ovw: return "m_srt_ovw";
	case EMetaHumanBodyType::m_srt_unw: return "m_srt_unw";
	case EMetaHumanBodyType::m_tal_nrw: return "m_tal_nrw";
	case EMetaHumanBodyType::m_tal_ovw: return "m_tal_ovw";
	case EMetaHumanBodyType::m_tal_unw: return "m_tal_unw";
	case EMetaHumanBodyType::BlendableBody: return "BlendableBody";
	default: return "Unknown";
	}
}

static EMetaHumanBodyType StringToBodyType(const std::string& Str)
{
	FString S = UTF8_TO_TCHAR(Str.c_str());
	if (S.Equals(TEXT("f_med_nrw"), ESearchCase::IgnoreCase)) return EMetaHumanBodyType::f_med_nrw;
	if (S.Equals(TEXT("f_med_ovw"), ESearchCase::IgnoreCase)) return EMetaHumanBodyType::f_med_ovw;
	if (S.Equals(TEXT("f_med_unw"), ESearchCase::IgnoreCase)) return EMetaHumanBodyType::f_med_unw;
	if (S.Equals(TEXT("f_srt_nrw"), ESearchCase::IgnoreCase)) return EMetaHumanBodyType::f_srt_nrw;
	if (S.Equals(TEXT("f_srt_ovw"), ESearchCase::IgnoreCase)) return EMetaHumanBodyType::f_srt_ovw;
	if (S.Equals(TEXT("f_srt_unw"), ESearchCase::IgnoreCase)) return EMetaHumanBodyType::f_srt_unw;
	if (S.Equals(TEXT("f_tal_nrw"), ESearchCase::IgnoreCase)) return EMetaHumanBodyType::f_tal_nrw;
	if (S.Equals(TEXT("f_tal_ovw"), ESearchCase::IgnoreCase)) return EMetaHumanBodyType::f_tal_ovw;
	if (S.Equals(TEXT("f_tal_unw"), ESearchCase::IgnoreCase)) return EMetaHumanBodyType::f_tal_unw;
	if (S.Equals(TEXT("m_med_nrw"), ESearchCase::IgnoreCase)) return EMetaHumanBodyType::m_med_nrw;
	if (S.Equals(TEXT("m_med_ovw"), ESearchCase::IgnoreCase)) return EMetaHumanBodyType::m_med_ovw;
	if (S.Equals(TEXT("m_med_unw"), ESearchCase::IgnoreCase)) return EMetaHumanBodyType::m_med_unw;
	if (S.Equals(TEXT("m_srt_nrw"), ESearchCase::IgnoreCase)) return EMetaHumanBodyType::m_srt_nrw;
	if (S.Equals(TEXT("m_srt_ovw"), ESearchCase::IgnoreCase)) return EMetaHumanBodyType::m_srt_ovw;
	if (S.Equals(TEXT("m_srt_unw"), ESearchCase::IgnoreCase)) return EMetaHumanBodyType::m_srt_unw;
	if (S.Equals(TEXT("m_tal_nrw"), ESearchCase::IgnoreCase)) return EMetaHumanBodyType::m_tal_nrw;
	if (S.Equals(TEXT("m_tal_ovw"), ESearchCase::IgnoreCase)) return EMetaHumanBodyType::m_tal_ovw;
	if (S.Equals(TEXT("m_tal_unw"), ESearchCase::IgnoreCase)) return EMetaHumanBodyType::m_tal_unw;
	if (S.Equals(TEXT("BlendableBody"), ESearchCase::IgnoreCase)) return EMetaHumanBodyType::BlendableBody;
	return EMetaHumanBodyType::f_med_nrw; // fallback
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
static UMetaHumanCharacterEditorSubsystem* GetMHEditorSubsystem()
{
	if (!GEditor) return nullptr;
	return GEditor->GetEditorSubsystem<UMetaHumanCharacterEditorSubsystem>();
}

/**
 * Ensure a MetaHuman character is registered for editing with the subsystem.
 * This is REQUIRED before any body/face operations — without it, the subsystem
 * crashes on CharacterDataMap access (TSharedRef default-construction → null deref).
 *
 * Follows the engine's own pattern from MetaHumanCharacterEditorTests.cpp:
 *   1. TryAddObjectToEdit(Character)  — creates FMetaHumanCharacterEditorData
 *   2. ... body operations ...
 *   3. RemoveObjectToEdit(Character)  — cleanup
 */
static bool EnsureMHCharacterRegistered(UMetaHumanCharacterEditorSubsystem* Subsystem,
	UMetaHumanCharacter* MHC, FLuaSessionData& Session)
{
	if (!Subsystem || !MHC) return false;

	// Already registered — safe to proceed
	if (Subsystem->IsObjectAddedForEditing(MHC))
	{
		return true;
	}

	// Register for editing — creates CharacterDataMap entry with BodyState, FaceState, meshes, etc.
	bool bAdded = Subsystem->TryAddObjectToEdit(MHC);
	if (!bAdded)
	{
		Session.Log(FString::Printf(TEXT("[FAIL] Could not register MetaHuman '%s' for editing. "
			"Ensure the character asset is valid and has proper DNA/body data."),
			*MHC->GetName()));
		return false;
	}

	Session.Log(FString::Printf(TEXT("[INFO] Registered MetaHuman '%s' for body editing"), *MHC->GetName()));
	return true;
}
#endif

static UMetaHumanCharacter* LoadMHCharacter(const std::string& AssetPath, FLuaSessionData& Session)
{
	FString Path = UTF8_TO_TCHAR(AssetPath.c_str());
	UMetaHumanCharacter* MHC = Cast<UMetaHumanCharacter>(
		StaticLoadObject(UMetaHumanCharacter::StaticClass(), nullptr, *Path));
	if (!MHC)
	{
		Session.Log(FString::Printf(TEXT("[FAIL] Could not load MetaHumanCharacter: %s"), *Path));
	}
	return MHC;
}

static const char* EyelashTypeToString(EMetaHumanCharacterEyelashesType Type)
{
	switch (Type)
	{
	case EMetaHumanCharacterEyelashesType::None: return "None";
	case EMetaHumanCharacterEyelashesType::Sparse: return "Sparse";
	case EMetaHumanCharacterEyelashesType::ShortFine: return "ShortFine";
	case EMetaHumanCharacterEyelashesType::Thin: return "Thin";
	case EMetaHumanCharacterEyelashesType::SlightCurl: return "SlightCurl";
	case EMetaHumanCharacterEyelashesType::LongCurl: return "LongCurl";
	case EMetaHumanCharacterEyelashesType::ThickCurl: return "ThickCurl";
	default: return "Unknown";
	}
}

static EMetaHumanCharacterEyelashesType StringToEyelashType(const std::string& Str)
{
	FString S = UTF8_TO_TCHAR(Str.c_str());
	if (S.Equals(TEXT("None"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyelashesType::None;
	if (S.Equals(TEXT("Sparse"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyelashesType::Sparse;
	if (S.Equals(TEXT("ShortFine"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyelashesType::ShortFine;
	if (S.Equals(TEXT("Thin"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyelashesType::Thin;
	if (S.Equals(TEXT("SlightCurl"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyelashesType::SlightCurl;
	if (S.Equals(TEXT("LongCurl"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyelashesType::LongCurl;
	if (S.Equals(TEXT("ThickCurl"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyelashesType::ThickCurl;
	return EMetaHumanCharacterEyelashesType::None;
}

static const char* IrisPatternToString(EMetaHumanCharacterEyesIrisPattern Pattern)
{
	switch (Pattern)
	{
	case EMetaHumanCharacterEyesIrisPattern::Iris001: return "Iris001";
	case EMetaHumanCharacterEyesIrisPattern::Iris002: return "Iris002";
	case EMetaHumanCharacterEyesIrisPattern::Iris003: return "Iris003";
	case EMetaHumanCharacterEyesIrisPattern::Iris004: return "Iris004";
	case EMetaHumanCharacterEyesIrisPattern::Iris005: return "Iris005";
	case EMetaHumanCharacterEyesIrisPattern::Iris006: return "Iris006";
	case EMetaHumanCharacterEyesIrisPattern::Iris007: return "Iris007";
	case EMetaHumanCharacterEyesIrisPattern::Iris008: return "Iris008";
	case EMetaHumanCharacterEyesIrisPattern::Iris009: return "Iris009";
	default: return "Unknown";
	}
}

static EMetaHumanCharacterEyesIrisPattern StringToIrisPattern(const std::string& Str)
{
	FString S = UTF8_TO_TCHAR(Str.c_str());
	if (S.Equals(TEXT("Iris001"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyesIrisPattern::Iris001;
	if (S.Equals(TEXT("Iris002"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyesIrisPattern::Iris002;
	if (S.Equals(TEXT("Iris003"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyesIrisPattern::Iris003;
	if (S.Equals(TEXT("Iris004"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyesIrisPattern::Iris004;
	if (S.Equals(TEXT("Iris005"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyesIrisPattern::Iris005;
	if (S.Equals(TEXT("Iris006"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyesIrisPattern::Iris006;
	if (S.Equals(TEXT("Iris007"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyesIrisPattern::Iris007;
	if (S.Equals(TEXT("Iris008"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyesIrisPattern::Iris008;
	if (S.Equals(TEXT("Iris009"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyesIrisPattern::Iris009;
	return EMetaHumanCharacterEyesIrisPattern::Iris001;
}

static const char* EyeMakeupTypeToString(EMetaHumanCharacterEyeMakeupType Type)
{
	switch (Type)
	{
	case EMetaHumanCharacterEyeMakeupType::None: return "None";
	case EMetaHumanCharacterEyeMakeupType::ThinLiner: return "ThinLiner";
	case EMetaHumanCharacterEyeMakeupType::SoftSmokey: return "SoftSmokey";
	case EMetaHumanCharacterEyeMakeupType::FullThinLiner: return "FullThinLiner";
	case EMetaHumanCharacterEyeMakeupType::CatEye: return "CatEye";
	case EMetaHumanCharacterEyeMakeupType::PandaSmudge: return "PandaSmudge";
	case EMetaHumanCharacterEyeMakeupType::DramaticSmudge: return "DramaticSmudge";
	case EMetaHumanCharacterEyeMakeupType::DoubleMod: return "DoubleMod";
	case EMetaHumanCharacterEyeMakeupType::ClassicBar: return "ClassicBar";
	default: return "Unknown";
	}
}

static EMetaHumanCharacterEyeMakeupType StringToEyeMakeupType(const std::string& Str)
{
	FString S = UTF8_TO_TCHAR(Str.c_str());
	if (S.Equals(TEXT("None"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyeMakeupType::None;
	if (S.Equals(TEXT("ThinLiner"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyeMakeupType::ThinLiner;
	if (S.Equals(TEXT("SoftSmokey"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyeMakeupType::SoftSmokey;
	if (S.Equals(TEXT("FullThinLiner"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyeMakeupType::FullThinLiner;
	if (S.Equals(TEXT("CatEye"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyeMakeupType::CatEye;
	if (S.Equals(TEXT("PandaSmudge"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyeMakeupType::PandaSmudge;
	if (S.Equals(TEXT("DramaticSmudge"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyeMakeupType::DramaticSmudge;
	if (S.Equals(TEXT("DoubleMod"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyeMakeupType::DoubleMod;
	if (S.Equals(TEXT("ClassicBar"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterEyeMakeupType::ClassicBar;
	return EMetaHumanCharacterEyeMakeupType::None;
}

static const char* BlushTypeToString(EMetaHumanCharacterBlushMakeupType Type)
{
	switch (Type)
	{
	case EMetaHumanCharacterBlushMakeupType::None: return "None";
	case EMetaHumanCharacterBlushMakeupType::Angled: return "Angled";
	case EMetaHumanCharacterBlushMakeupType::Apple: return "Apple";
	case EMetaHumanCharacterBlushMakeupType::LowSweep: return "LowSweep";
	case EMetaHumanCharacterBlushMakeupType::HighCurve: return "HighCurve";
	default: return "Unknown";
	}
}

static EMetaHumanCharacterBlushMakeupType StringToBlushType(const std::string& Str)
{
	FString S = UTF8_TO_TCHAR(Str.c_str());
	if (S.Equals(TEXT("None"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterBlushMakeupType::None;
	if (S.Equals(TEXT("Angled"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterBlushMakeupType::Angled;
	if (S.Equals(TEXT("Apple"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterBlushMakeupType::Apple;
	if (S.Equals(TEXT("LowSweep"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterBlushMakeupType::LowSweep;
	if (S.Equals(TEXT("HighCurve"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterBlushMakeupType::HighCurve;
	return EMetaHumanCharacterBlushMakeupType::None;
}

static const char* LipsMakeupTypeToString(EMetaHumanCharacterLipsMakeupType Type)
{
	switch (Type)
	{
	case EMetaHumanCharacterLipsMakeupType::None: return "None";
	case EMetaHumanCharacterLipsMakeupType::Natural: return "Natural";
	case EMetaHumanCharacterLipsMakeupType::Hollywood: return "Hollywood";
	case EMetaHumanCharacterLipsMakeupType::Cupid: return "Cupid";
	default: return "Unknown";
	}
}

static EMetaHumanCharacterLipsMakeupType StringToLipsMakeupType(const std::string& Str)
{
	FString S = UTF8_TO_TCHAR(Str.c_str());
	if (S.Equals(TEXT("None"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterLipsMakeupType::None;
	if (S.Equals(TEXT("Natural"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterLipsMakeupType::Natural;
	if (S.Equals(TEXT("Hollywood"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterLipsMakeupType::Hollywood;
	if (S.Equals(TEXT("Cupid"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterLipsMakeupType::Cupid;
	return EMetaHumanCharacterLipsMakeupType::None;
}

static const char* FrecklesMaskToString(EMetaHumanCharacterFrecklesMask Mask)
{
	switch (Mask)
	{
	case EMetaHumanCharacterFrecklesMask::None: return "None";
	case EMetaHumanCharacterFrecklesMask::Type1: return "Type1";
	case EMetaHumanCharacterFrecklesMask::Type2: return "Type2";
	case EMetaHumanCharacterFrecklesMask::Type3: return "Type3";
	default: return "Unknown";
	}
}

static EMetaHumanCharacterFrecklesMask StringToFrecklesMask(const std::string& Str)
{
	FString S = UTF8_TO_TCHAR(Str.c_str());
	if (S.Equals(TEXT("None"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterFrecklesMask::None;
	if (S.Equals(TEXT("Type1"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterFrecklesMask::Type1;
	if (S.Equals(TEXT("Type2"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterFrecklesMask::Type2;
	if (S.Equals(TEXT("Type3"), ESearchCase::IgnoreCase)) return EMetaHumanCharacterFrecklesMask::Type3;
	return EMetaHumanCharacterFrecklesMask::None;
}

static sol::table LinearColorToTable(sol::state_view& Lua, const FLinearColor& C)
{
	sol::table T = Lua.create_table();
	T["r"] = C.R;
	T["g"] = C.G;
	T["b"] = C.B;
	T["a"] = C.A;
	return T;
}

static FLinearColor TableToLinearColor(const sol::table& T)
{
	float R = T.get_or("r", 0.0f);
	float G = T.get_or("g", 0.0f);
	float B = T.get_or("b", 0.0f);
	float A = T.get_or("a", 1.0f);
	return FLinearColor(R, G, B, A);
}

static sol::table EyeIrisToTable(sol::state_view& Lua, const FMetaHumanCharacterEyeIrisProperties& Iris)
{
	sol::table T = Lua.create_table();
	T["pattern"] = IrisPatternToString(Iris.IrisPattern);
	T["rotation"] = Iris.IrisRotation;
	T["primary_color_u"] = Iris.PrimaryColorU;
	T["primary_color_v"] = Iris.PrimaryColorV;
	T["secondary_color_u"] = Iris.SecondaryColorU;
	T["secondary_color_v"] = Iris.SecondaryColorV;
	T["color_blend"] = Iris.ColorBlend;
	T["color_blend_softness"] = Iris.ColorBlendSoftness;
	T["blend_method"] = (Iris.BlendMethod == EMetaHumanCharacterEyesBlendMethod::Radial) ? "Radial" : "Structural";
	T["shadow_details"] = Iris.ShadowDetails;
	T["limbal_ring_size"] = Iris.LimbalRingSize;
	T["limbal_ring_softness"] = Iris.LimbalRingSoftness;
	T["limbal_ring_color"] = LinearColorToTable(Lua, Iris.LimbalRingColor);
	T["global_saturation"] = Iris.GlobalSaturation;
	T["global_tint"] = LinearColorToTable(Lua, Iris.GlobalTint);
	return T;
}

static sol::table EyePropsToTable(sol::state_view& Lua, const FMetaHumanCharacterEyeProperties& Eye)
{
	sol::table T = Lua.create_table();
	T["iris"] = EyeIrisToTable(Lua, Eye.Iris);

	sol::table PupilT = Lua.create_table();
	PupilT["dilation"] = Eye.Pupil.Dilation;
	PupilT["feather"] = Eye.Pupil.Feather;
	T["pupil"] = PupilT;

	sol::table CorneaT = Lua.create_table();
	CorneaT["size"] = Eye.Cornea.Size;
	CorneaT["limbus_softness"] = Eye.Cornea.LimbusSoftness;
	CorneaT["limbus_color"] = LinearColorToTable(Lua, Eye.Cornea.LimbusColor);
	T["cornea"] = CorneaT;

	sol::table ScleraT = Lua.create_table();
	ScleraT["rotation"] = Eye.Sclera.Rotation;
	ScleraT["use_custom_tint"] = Eye.Sclera.bUseCustomTint;
	ScleraT["tint"] = LinearColorToTable(Lua, Eye.Sclera.Tint);
	ScleraT["transmission_spread"] = Eye.Sclera.TransmissionSpread;
	ScleraT["transmission_color"] = LinearColorToTable(Lua, Eye.Sclera.TransmissionColor);
	ScleraT["vascularity_intensity"] = Eye.Sclera.VascularityIntensity;
	ScleraT["vascularity_coverage"] = Eye.Sclera.VascularityCoverage;
	T["sclera"] = ScleraT;

	return T;
}

static sol::table AccentRegionToTable(sol::state_view& Lua, const FMetaHumanCharacterAccentRegionProperties& Region)
{
	sol::table T = Lua.create_table();
	T["redness"] = Region.Redness;
	T["saturation"] = Region.Saturation;
	T["lightness"] = Region.Lightness;
	return T;
}

// ============================================================================
// DOCS
// ============================================================================

static TArray<FLuaFunctionDoc> MetaHumanDocs = {
	{ TEXT("metahuman_info(asset)"), TEXT("Get full info from a UMetaHumanCharacter asset: skin, eyes, makeup, teeth, face, body, viewport, assembly, DNA status"), TEXT("table or nil") },
	{ TEXT("metahuman_set_skin(asset, params)"), TEXT("Set skin properties: u, v, roughness, body_texture_index, face_texture_index, show_top_underwear"), TEXT("bool") },
	{ TEXT("metahuman_set_freckles(asset, params)"), TEXT("Set freckles: density, strength, saturation, tone_shift, mask (None/Type1/Type2/Type3)"), TEXT("bool") },
	{ TEXT("metahuman_set_accents(asset, params)"), TEXT("Set accent regions: {region={redness, saturation, lightness}} — regions: scalp, forehead, nose, under_eye, cheeks, lips, chin, ears"), TEXT("bool") },
	{ TEXT("metahuman_set_eyes(asset, params)"), TEXT("Set eye properties: {eye_left={iris={...}, pupil={...}, cornea={...}, sclera={...}}, eye_right={...}}"), TEXT("bool") },
	{ TEXT("metahuman_set_eyelashes(asset, params)"), TEXT("Set eyelash properties: type (None/Sparse/ShortFine/Thin/SlightCurl/LongCurl/ThickCurl), dye_color, melanin, redness, roughness, salt_and_pepper, lightness, enable_grooms"), TEXT("bool") },
	{ TEXT("metahuman_set_teeth(asset, params)"), TEXT("Set teeth: tooth_length, tooth_spacing, upper_shift, lower_shift, overbite, overjet, worn_down, polycanine, receding_gums, narrowness, variation, jaw_open, teeth_color, gum_color, plaque_color, plaque_amount"), TEXT("bool") },
	{ TEXT("metahuman_set_makeup(asset, params)"), TEXT("Set makeup: {foundation={...}, eyes={type, primary_color, secondary_color, ...}, blush={type, color, intensity, roughness}, lips={type, color, roughness, opacity, metalness}}"), TEXT("bool") },
	{ TEXT("metahuman_set_face(asset, params)"), TEXT("Set face evaluation: global_delta (0-1), high_frequency_delta (0-1), head_scale (0.8-1.3)"), TEXT("bool") },
	{ TEXT("metahuman_set_viewport(asset, params)"), TEXT("Set viewport settings: environment, background_color, light_rotation, lod, camera_frame, rendering_quality, tonemapper_enabled, show_face_bones, show_body_bones"), TEXT("bool") },
	{ TEXT("metahuman_set_assembly(asset, params)"), TEXT("Set assembly settings: pipeline_type (Cinematic/Optimized/UEFN/DCC), quality (Low/Medium/High/Cinematic), root_directory, common_directory, name_override, bake_makeup"), TEXT("bool") },
	// Body type & constraints (5.7+)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	{ TEXT("metahuman_set_body_type(asset, body_type)"), TEXT("Set body type: f_med_nrw, f_tal_unw, m_srt_ovw, BlendableBody, etc. Requires character open for editing."), TEXT("bool") },
	{ TEXT("metahuman_get_body_constraints(asset)"), TEXT("Get all body constraints with names, active state, target/min/max measurements"), TEXT("table[] or nil") },
	{ TEXT("metahuman_set_body_constraints(asset, constraints)"), TEXT("Set body constraints. constraints: array of {name, active, target} tables"), TEXT("bool") },
	{ TEXT("metahuman_conform_body(asset, body_mesh_path, opts?)"), TEXT("Conform body to a target mesh (e.g. imported DNA SkeletalMesh). opts: match_by_uvs (default true), repose (default true), estimate_joints (default true). This is the validated DNA conform pipeline."), TEXT("bool") },
	{ TEXT("metahuman_commit_body(asset)"), TEXT("Commit body state changes to the character asset for saving"), TEXT("bool") },
	{ TEXT("metahuman_build(asset, params?)"), TEXT("Build/assemble the MetaHuman character. params: pipeline_type, quality, root_directory, common_directory, name_override, bake_makeup"), TEXT("bool") },
	// Advanced body/face
	{ TEXT("metahuman_is_editing(asset)"), TEXT("Check if character is registered for editing with the subsystem"), TEXT("bool") },
	{ TEXT("metahuman_auto_rig(asset, opts?)"), TEXT("Request auto-rigging for the character. opts: rig_type ('JointsOnly'|'JointsAndBlendShapes'), blocking (default true), report_progress (default true)"), TEXT("bool") },
	{ TEXT("metahuman_import_face_dna(asset, dna_path, opts?)"), TEXT("Import face from a DNA file. opts: whole_rig (default true), alignment ('None'|'Translation'|'RotationTranslation'|'ScalingTranslation'|'ScalingRotationTranslation')"), TEXT("string") },
	{ TEXT("metahuman_request_textures(asset, opts?)"), TEXT("Request high-resolution texture synthesis. opts: blocking (default true), report_progress (default true)"), TEXT("bool") },
	{ TEXT("metahuman_get_face_landmarks(asset)"), TEXT("Get face landmark positions as array of {x,y,z} vectors"), TEXT("table[] or nil") },
	{ TEXT("metahuman_set_face_landmarks(asset, landmarks)"), TEXT("Translate face landmarks. landmarks: array of {index, delta={x,y,z}} tables"), TEXT("bool") },
	{ TEXT("metahuman_set_body_joints(asset, translations, rotations, opts?)"), TEXT("Set body joint positions and rotations. opts: import_helper_joints (default true)"), TEXT("bool") },
	{ TEXT("metahuman_set_body_mesh(asset, vertices, opts?)"), TEXT("Set body mesh vertex positions. opts: reposition_helper_joints (default true)"), TEXT("bool") },
	// Comparison/testing
	{ TEXT("metahuman_compare_face(asset1, asset2, tolerance?)"), TEXT("Compare face states of two characters within tolerance"), TEXT("bool") },
	{ TEXT("metahuman_compare_body(asset1, asset2, tolerance?)"), TEXT("Compare body states of two characters within tolerance"), TEXT("bool") },
	{ TEXT("metahuman_compare_textures(asset1, asset2, pixel_tolerance?)"), TEXT("Compare face textures of two characters within pixel tolerance"), TEXT("bool") },
	// Face/body state management
	{ TEXT("metahuman_commit_face(asset)"), TEXT("Commit face state changes to the character asset for saving"), TEXT("bool") },
	{ TEXT("metahuman_reset_face(asset)"), TEXT("Reset face to default archetype state"), TEXT("bool") },
	{ TEXT("metahuman_remove_face_rig(asset)"), TEXT("Remove the face rig from the character"), TEXT("bool") },
	{ TEXT("metahuman_remove_body_rig(asset)"), TEXT("Remove the body rig from the character"), TEXT("bool") },
	{ TEXT("metahuman_initialize_from_preset(asset, preset_asset)"), TEXT("Initialize character from a preset character asset"), TEXT("bool") },
	// Gizmos and sculpting
	{ TEXT("metahuman_get_face_gizmos(asset)"), TEXT("Get face gizmo positions as array of {x,y,z} vectors"), TEXT("table[] or nil") },
	{ TEXT("metahuman_set_face_gizmo(asset, opts)"), TEXT("Manipulate face gizmo. opts: index, type ('position'|'rotation'|'scale'), value ({x,y,z} or float for scale), symmetric (default false), enforce_bounds (default true)"), TEXT("table[] or nil") },
	{ TEXT("metahuman_get_body_gizmos(asset)"), TEXT("Get body region gizmo positions as array of {x,y,z} vectors"), TEXT("table[] or nil") },
	// Rigging and conforming
	{ TEXT("metahuman_get_rigging_state(asset)"), TEXT("Get character rigging state: 'Unrigged', 'RigPending', or 'Rigged'"), TEXT("string") },
	{ TEXT("metahuman_import_from_template(asset, template_mesh_path, opts?)"), TEXT("Import face from a SkelMesh/StaticMesh template. opts: match_by_uvs, use_eyes, use_teeth, alignment, left_eye_mesh, right_eye_mesh, teeth_mesh"), TEXT("string") },
	// Skin tone
	{ TEXT("metahuman_get_skin_tone(u, v)"), TEXT("Get skin tone color at UV coordinates. Returns {r,g,b,a}"), TEXT("table or nil") },
	{ TEXT("metahuman_estimate_skin_tone(color, hf_index)"), TEXT("Estimate skin tone UV from sRGB color. color: {r,g,b}, hf_index: int. Returns {u,v}"), TEXT("table or nil") },
	// Clothing visibility
	{ TEXT("metahuman_set_clothing_visibility(asset, state)"), TEXT("Set clothing visibility: 'Shown', 'Hidden', or 'UseOverrideMaterial'"), TEXT("bool") },
	{ TEXT("metahuman_get_clothing_visibility(asset)"), TEXT("Get clothing visibility state"), TEXT("string") },
	// Preview and utilities
	{ TEXT("metahuman_set_body_global_delta(asset, delta)"), TEXT("Set body vertex/joint global delta scale"), TEXT("bool") },
	{ TEXT("metahuman_get_body_global_delta(asset)"), TEXT("Get body vertex/joint global delta scale"), TEXT("number or nil") },
	{ TEXT("metahuman_update_preview_material(asset, type)"), TEXT("Set preview material type: 'Topology', 'Skin', or 'Clay'"), TEXT("bool") },
	{ TEXT("metahuman_remove_textures_and_rigs(asset)"), TEXT("Strip textures and rigs from character (convert to preset)"), TEXT("bool") },
	{ TEXT("metahuman_spawn_actor(asset)"), TEXT("Spawn a MetaHuman editor preview actor in the main level"), TEXT("string or nil") },
	// Blending and conforming
	{ TEXT("metahuman_blend_face_region(asset, opts)"), TEXT("Blend face region from preset characters. opts: region_index, preset_assets (paths), weights (array), blend_options ('Proportions'|'Features'|'Both'), symmetric (default false)"), TEXT("table[] or nil") },
	{ TEXT("metahuman_blend_body_region(asset, opts)"), TEXT("Blend body region from preset characters. opts: region_index, preset_assets (paths), weights (array), blend_options ('Skeleton'|'Shape'|'Both')"), TEXT("table[] or nil") },
	{ TEXT("metahuman_fit_face_to_vertices(asset, opts)"), TEXT("Fit face state to target head vertices. opts: head_vertices (array of {x,y,z}), left_eye_vertices, right_eye_vertices, teeth_vertices, alignment, disable_hf_delta"), TEXT("bool") },
#if AIK_HAS_METAHUMAN_IDENTITY
	{ TEXT("metahuman_import_from_identity(asset, identity_path, opts?)"), TEXT("Import face from a UMetaHumanIdentity asset. opts: use_eyes (default true), use_teeth (default true), use_metric_scale (default false)"), TEXT("string") },
#endif
	{ TEXT("metahuman_create_combined_mesh(asset, output_path)"), TEXT("Create a combined face+body skeletal mesh asset"), TEXT("string or nil") },
#endif
	// Wardrobe (requires MetaHumanCharacterPalette module)
#if AIK_HAS_METAHUMAN_WARDROBE
	{ TEXT("metahuman_list_wardrobe(asset)"), TEXT("List all wardrobe items on the character by slot"), TEXT("table or nil") },
	{ TEXT("metahuman_add_wardrobe_item(asset, slot_name, wardrobe_item_path)"), TEXT("Add a wardrobe item to a character slot"), TEXT("bool") },
	{ TEXT("metahuman_remove_wardrobe_item(asset, slot_name, item_index)"), TEXT("Remove a wardrobe item from a slot (1-based index)"), TEXT("bool") },
#endif
};

// ============================================================================
// BINDING
// ============================================================================

static void BindMetaHuman(sol::state& Lua, FLuaSessionData& Session)
{
	// ================================================================
	// metahuman_info(asset)
	// ================================================================
	Lua.set_function("metahuman_info", [&Session](sol::object AssetObj, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UMetaHumanCharacter* MHC = nullptr;

		if (AssetObj.is<std::string>())
		{
			FString Path = UTF8_TO_TCHAR(AssetObj.as<std::string>().c_str());
			MHC = Cast<UMetaHumanCharacter>(StaticLoadObject(UMetaHumanCharacter::StaticClass(), nullptr, *Path));
		}
		else if (AssetObj.is<sol::table>())
		{
			sol::table T = AssetObj.as<sol::table>();
			sol::optional<std::string> PathOpt = T["path"];
			if (PathOpt.has_value())
			{
				FString Path = UTF8_TO_TCHAR(PathOpt.value().c_str());
				MHC = Cast<UMetaHumanCharacter>(StaticLoadObject(UMetaHumanCharacter::StaticClass(), nullptr, *Path));
			}
		}
		else
		{
			// Try casting from userdata
			UObject* Obj = nullptr;
			if (AssetObj.is<UObject*>())
			{
				Obj = AssetObj.as<UObject*>();
			}
			MHC = Cast<UMetaHumanCharacter>(Obj);
		}

		if (!MHC)
		{
			Session.Log(TEXT("[FAIL] metahuman_info -> Could not resolve to UMetaHumanCharacter"));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		Result["name"] = TCHAR_TO_UTF8(*MHC->GetName());
		Result["path"] = TCHAR_TO_UTF8(*MHC->GetPathName());
		Result["is_valid"] = MHC->IsCharacterValid();
		Result["has_face_dna"] = MHC->HasFaceDNA();
		Result["has_face_dna_blendshapes"] = MHC->HasFaceDNABlendshapes();
		Result["has_body_dna"] = MHC->HasBodyDNA();
		Result["has_high_res_textures"] = MHC->HasHighResolutionTextures();
		Result["has_synthesized_textures"] = MHC->HasSynthesizedTextures();
		Result["fixed_body_type"] = MHC->bFixedBodyType;

		// Face evaluation
		sol::table FaceT = LuaView.create_table();
		FaceT["global_delta"] = MHC->FaceEvaluationSettings.GlobalDelta;
		FaceT["high_frequency_delta"] = MHC->FaceEvaluationSettings.HighFrequencyDelta;
		FaceT["head_scale"] = MHC->FaceEvaluationSettings.HeadScale;
		Result["face"] = FaceT;

		// Head model (eyelashes + teeth)
		sol::table HeadT = LuaView.create_table();
		{
			const FMetaHumanCharacterEyelashesProperties& EL = MHC->HeadModelSettings.Eyelashes;
			sol::table EyelashT = LuaView.create_table();
			EyelashT["type"] = EyelashTypeToString(EL.Type);
			EyelashT["dye_color"] = LinearColorToTable(LuaView, EL.DyeColor);
			EyelashT["melanin"] = EL.Melanin;
			EyelashT["redness"] = EL.Redness;
			EyelashT["roughness"] = EL.Roughness;
			EyelashT["salt_and_pepper"] = EL.SaltAndPepper;
			EyelashT["lightness"] = EL.Lightness;
			EyelashT["enable_grooms"] = EL.bEnableGrooms;
			HeadT["eyelashes"] = EyelashT;
		}
		{
			const FMetaHumanCharacterTeethProperties& Teeth = MHC->HeadModelSettings.Teeth;
			sol::table TeethT = LuaView.create_table();
			TeethT["tooth_length"] = Teeth.ToothLength;
			TeethT["tooth_spacing"] = Teeth.ToothSpacing;
			TeethT["upper_shift"] = Teeth.UpperShift;
			TeethT["lower_shift"] = Teeth.LowerShift;
			TeethT["overbite"] = Teeth.Overbite;
			TeethT["overjet"] = Teeth.Overjet;
			TeethT["worn_down"] = Teeth.WornDown;
			TeethT["polycanine"] = Teeth.Polycanine;
			TeethT["receding_gums"] = Teeth.RecedingGums;
			TeethT["narrowness"] = Teeth.Narrowness;
			TeethT["variation"] = Teeth.Variation;
			TeethT["jaw_open"] = Teeth.JawOpen;
			TeethT["teeth_color"] = LinearColorToTable(LuaView, Teeth.TeethColor);
			TeethT["gum_color"] = LinearColorToTable(LuaView, Teeth.GumColor);
			TeethT["plaque_color"] = LinearColorToTable(LuaView, Teeth.PlaqueColor);
			TeethT["plaque_amount"] = Teeth.PlaqueAmount;
			HeadT["teeth"] = TeethT;
		}
		Result["head"] = HeadT;

		// Skin settings
		{
			const FMetaHumanCharacterSkinSettings& SkinS = MHC->SkinSettings;
			sol::table SkinT = LuaView.create_table();

			sol::table SkinPropsT = LuaView.create_table();
			SkinPropsT["u"] = SkinS.Skin.U;
			SkinPropsT["v"] = SkinS.Skin.V;
			SkinPropsT["roughness"] = SkinS.Skin.Roughness;
			SkinPropsT["body_texture_index"] = SkinS.Skin.BodyTextureIndex;
			SkinPropsT["face_texture_index"] = SkinS.Skin.FaceTextureIndex;
			SkinPropsT["show_top_underwear"] = SkinS.Skin.bShowTopUnderwear;
			SkinT["skin"] = SkinPropsT;

			sol::table FrecklesT = LuaView.create_table();
			FrecklesT["density"] = SkinS.Freckles.Density;
			FrecklesT["strength"] = SkinS.Freckles.Strength;
			FrecklesT["saturation"] = SkinS.Freckles.Saturation;
			FrecklesT["tone_shift"] = SkinS.Freckles.ToneShift;
			FrecklesT["mask"] = FrecklesMaskToString(SkinS.Freckles.Mask);
			SkinT["freckles"] = FrecklesT;

			sol::table AccentsT = LuaView.create_table();
			AccentsT["scalp"] = AccentRegionToTable(LuaView, SkinS.Accents.Scalp);
			AccentsT["forehead"] = AccentRegionToTable(LuaView, SkinS.Accents.Forehead);
			AccentsT["nose"] = AccentRegionToTable(LuaView, SkinS.Accents.Nose);
			AccentsT["under_eye"] = AccentRegionToTable(LuaView, SkinS.Accents.UnderEye);
			AccentsT["cheeks"] = AccentRegionToTable(LuaView, SkinS.Accents.Cheeks);
			AccentsT["lips"] = AccentRegionToTable(LuaView, SkinS.Accents.Lips);
			AccentsT["chin"] = AccentRegionToTable(LuaView, SkinS.Accents.Chin);
			AccentsT["ears"] = AccentRegionToTable(LuaView, SkinS.Accents.Ears);
			SkinT["accents"] = AccentsT;

			Result["skin_settings"] = SkinT;
		}

		// Eyes
		{
			sol::table EyesT = LuaView.create_table();
			EyesT["eye_left"] = EyePropsToTable(LuaView, MHC->EyesSettings.EyeLeft);
			EyesT["eye_right"] = EyePropsToTable(LuaView, MHC->EyesSettings.EyeRight);
			Result["eyes"] = EyesT;
		}

		// Makeup
		{
			const FMetaHumanCharacterMakeupSettings& MU = MHC->MakeupSettings;
			sol::table MakeupT = LuaView.create_table();

			sol::table FoundT = LuaView.create_table();
			FoundT["apply"] = MU.Foundation.bApplyFoundation;
			FoundT["color"] = LinearColorToTable(LuaView, MU.Foundation.Color);
			FoundT["intensity"] = MU.Foundation.Intensity;
			FoundT["roughness"] = MU.Foundation.Roughness;
			FoundT["concealer"] = MU.Foundation.Concealer;
			MakeupT["foundation"] = FoundT;

			sol::table EyeMUT = LuaView.create_table();
			EyeMUT["type"] = EyeMakeupTypeToString(MU.Eyes.Type);
			EyeMUT["primary_color"] = LinearColorToTable(LuaView, MU.Eyes.PrimaryColor);
			EyeMUT["secondary_color"] = LinearColorToTable(LuaView, MU.Eyes.SecondaryColor);
			EyeMUT["roughness"] = MU.Eyes.Roughness;
			EyeMUT["opacity"] = MU.Eyes.Opacity;
			EyeMUT["metalness"] = MU.Eyes.Metalness;
			MakeupT["eyes"] = EyeMUT;

			sol::table BlushT = LuaView.create_table();
			BlushT["type"] = BlushTypeToString(MU.Blush.Type);
			BlushT["color"] = LinearColorToTable(LuaView, MU.Blush.Color);
			BlushT["intensity"] = MU.Blush.Intensity;
			BlushT["roughness"] = MU.Blush.Roughness;
			MakeupT["blush"] = BlushT;

			sol::table LipsT = LuaView.create_table();
			LipsT["type"] = LipsMakeupTypeToString(MU.Lips.Type);
			LipsT["color"] = LinearColorToTable(LuaView, MU.Lips.Color);
			LipsT["roughness"] = MU.Lips.Roughness;
			LipsT["opacity"] = MU.Lips.Opacity;
			LipsT["metalness"] = MU.Lips.Metalness;
			MakeupT["lips"] = LipsT;

			Result["makeup"] = MakeupT;
		}

		// Viewport
		{
			const FMetaHumanCharacterViewportSettings& VP = MHC->ViewportSettings;
			sol::table VPT = LuaView.create_table();
			VPT["environment"] = static_cast<int>(VP.CharacterEnvironment);
			VPT["light_rotation"] = VP.LightRotation;
			VPT["background_color"] = LinearColorToTable(LuaView, VP.BackgroundColor);
			VPT["tonemapper_enabled"] = VP.bTonemapperEnabled;
			VPT["lod"] = static_cast<int>(VP.LevelOfDetail);
			VPT["camera_frame"] = static_cast<int>(VP.CameraFrame);
			VPT["rendering_quality"] = static_cast<int>(VP.RenderingQuality);
			VPT["always_use_hair_cards"] = VP.bAlwaysUseHairCards;
			VPT["show_viewport_overlays"] = VP.bShowViewportOverlays;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			VPT["show_face_bones"] = VP.bShowFaceBones;
			VPT["show_body_bones"] = VP.bShowBodyBones;
			VPT["show_face_normals"] = VP.bShowFaceNormals;
			VPT["show_body_normals"] = VP.bShowBodyNormals;
			VPT["show_face_tangents"] = VP.bShowFaceTangents;
			VPT["show_body_tangents"] = VP.bShowBodyTangents;
			VPT["show_face_binormals"] = VP.bShowFaceBinormals;
			VPT["show_body_binormals"] = VP.bShowBodyBinormals;
			VPT["use_custom_environment"] = VP.bUseCustomEnvironment;
			VPT["custom_environment"] = TCHAR_TO_UTF8(*VP.CustomEnvironment.ToString());
#endif
			Result["viewport"] = VPT;
		}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		// Assembly
		{
			const FMetaHumanCharacterAssemblySettings& AS = MHC->AssemblySettings;
			sol::table AST = LuaView.create_table();
			switch (AS.PipelineType)
			{
			case EMetaHumanDefaultPipelineType::Cinematic: AST["pipeline_type"] = "Cinematic"; break;
			case EMetaHumanDefaultPipelineType::Optimized: AST["pipeline_type"] = "Optimized"; break;
			case EMetaHumanDefaultPipelineType::UEFN: AST["pipeline_type"] = "UEFN"; break;
			case EMetaHumanDefaultPipelineType::DCC: AST["pipeline_type"] = "DCC"; break;
			}
			switch (AS.PipelineQuality)
			{
			case EMetaHumanQualityLevel::Low: AST["quality"] = "Low"; break;
			case EMetaHumanQualityLevel::Medium: AST["quality"] = "Medium"; break;
			case EMetaHumanQualityLevel::High: AST["quality"] = "High"; break;
			case EMetaHumanQualityLevel::Cinematic: AST["quality"] = "Cinematic"; break;
			default: AST["quality"] = "High"; break;
			}
			AST["root_directory"] = TCHAR_TO_UTF8(*AS.RootDirectory.Path);
			AST["common_directory"] = TCHAR_TO_UTF8(*AS.CommonDirectory.Path);
			AST["name_override"] = TCHAR_TO_UTF8(*AS.NameOverride);
			AST["bake_makeup"] = AS.bBakeMakeup;
			AST["export_zip"] = AS.bExportZipFile;
			Result["assembly"] = AST;
		}
#endif

		Session.Log(TEXT("[OK] metahuman_info -> Retrieved MetaHuman character info"));
		return Result;
	});

	// ================================================================
	// metahuman_set_skin(asset, params)
	// ================================================================
	Lua.set_function("metahuman_set_skin", [&Session](const std::string& AssetPath, sol::table Params, sol::this_state S) -> bool
	{
		sol::state_view LuaView(S);
		FString Path = UTF8_TO_TCHAR(AssetPath.c_str());
		UMetaHumanCharacter* MHC = Cast<UMetaHumanCharacter>(StaticLoadObject(UMetaHumanCharacter::StaticClass(), nullptr, *Path));
		if (!MHC)
		{
			Session.Log(TEXT("[FAIL] metahuman_set_skin -> Could not load MetaHumanCharacter"));
			return false;
		}

		FScopedTransaction Tx(FText::FromString(TEXT("Set MetaHuman Skin")));
		MHC->Modify();

		FMetaHumanCharacterSkinProperties& Skin = MHC->SkinSettings.Skin;
		if (Params["u"].valid()) Skin.U = Params.get_or("u", 0.5f);
		if (Params["v"].valid()) Skin.V = Params.get_or("v", 0.5f);
		if (Params["roughness"].valid()) Skin.Roughness = Params.get_or("roughness", 1.06f);
		if (Params["body_texture_index"].valid()) Skin.BodyTextureIndex = Params.get_or("body_texture_index", 0);
		if (Params["face_texture_index"].valid()) Skin.FaceTextureIndex = Params.get_or("face_texture_index", 0);
		if (Params["show_top_underwear"].valid()) Skin.bShowTopUnderwear = Params.get_or("show_top_underwear", true);

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (Subsystem && Subsystem->IsObjectAddedForEditing(MHC))
		{
			Subsystem->CommitSkinSettings(MHC, MHC->SkinSettings);
		}
#endif
		MHC->PostEditChange();
		MHC->MarkPackageDirty();
		Session.Log(TEXT("[OK] metahuman_set_skin -> Updated skin properties"));
		return true;
	});

	// ================================================================
	// metahuman_set_freckles(asset, params)
	// ================================================================
	Lua.set_function("metahuman_set_freckles", [&Session](const std::string& AssetPath, sol::table Params, sol::this_state S) -> bool
	{
		FString Path = UTF8_TO_TCHAR(AssetPath.c_str());
		UMetaHumanCharacter* MHC = Cast<UMetaHumanCharacter>(StaticLoadObject(UMetaHumanCharacter::StaticClass(), nullptr, *Path));
		if (!MHC)
		{
			Session.Log(TEXT("[FAIL] metahuman_set_freckles -> Could not load MetaHumanCharacter"));
			return false;
		}

		FScopedTransaction Tx(FText::FromString(TEXT("Set MetaHuman Freckles")));
		MHC->Modify();

		FMetaHumanCharacterFrecklesProperties& Freckles = MHC->SkinSettings.Freckles;
		if (Params["density"].valid()) Freckles.Density = Params.get_or("density", 0.5f);
		if (Params["strength"].valid()) Freckles.Strength = Params.get_or("strength", 0.2f);
		if (Params["saturation"].valid()) Freckles.Saturation = Params.get_or("saturation", 0.6f);
		if (Params["tone_shift"].valid()) Freckles.ToneShift = Params.get_or("tone_shift", 0.65f);
		if (Params["mask"].valid())
		{
			sol::optional<std::string> MaskOpt = Params["mask"];
			if (MaskOpt.has_value()) Freckles.Mask = StringToFrecklesMask(MaskOpt.value());
		}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (Subsystem && Subsystem->IsObjectAddedForEditing(MHC))
		{
			Subsystem->CommitSkinSettings(MHC, MHC->SkinSettings);
		}
#endif
		MHC->PostEditChange();
		MHC->MarkPackageDirty();
		Session.Log(TEXT("[OK] metahuman_set_freckles -> Updated freckles"));
		return true;
	});

	// ================================================================
	// metahuman_set_accents(asset, params)
	// ================================================================
	Lua.set_function("metahuman_set_accents", [&Session](const std::string& AssetPath, sol::table Params, sol::this_state S) -> bool
	{
		FString Path = UTF8_TO_TCHAR(AssetPath.c_str());
		UMetaHumanCharacter* MHC = Cast<UMetaHumanCharacter>(StaticLoadObject(UMetaHumanCharacter::StaticClass(), nullptr, *Path));
		if (!MHC)
		{
			Session.Log(TEXT("[FAIL] metahuman_set_accents -> Could not load MetaHumanCharacter"));
			return false;
		}

		FScopedTransaction Tx(FText::FromString(TEXT("Set MetaHuman Accents")));
		MHC->Modify();

		auto ApplyRegion = [](FMetaHumanCharacterAccentRegionProperties& Region, const sol::table& T)
		{
			if (T["redness"].valid()) Region.Redness = T.get_or("redness", 0.5f);
			if (T["saturation"].valid()) Region.Saturation = T.get_or("saturation", 0.5f);
			if (T["lightness"].valid()) Region.Lightness = T.get_or("lightness", 0.5f);
		};

		FMetaHumanCharacterAccentRegions& Accents = MHC->SkinSettings.Accents;
		if (Params["scalp"].valid()) ApplyRegion(Accents.Scalp, Params["scalp"]);
		if (Params["forehead"].valid()) ApplyRegion(Accents.Forehead, Params["forehead"]);
		if (Params["nose"].valid()) ApplyRegion(Accents.Nose, Params["nose"]);
		if (Params["under_eye"].valid()) ApplyRegion(Accents.UnderEye, Params["under_eye"]);
		if (Params["cheeks"].valid()) ApplyRegion(Accents.Cheeks, Params["cheeks"]);
		if (Params["lips"].valid()) ApplyRegion(Accents.Lips, Params["lips"]);
		if (Params["chin"].valid()) ApplyRegion(Accents.Chin, Params["chin"]);
		if (Params["ears"].valid()) ApplyRegion(Accents.Ears, Params["ears"]);

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (Subsystem && Subsystem->IsObjectAddedForEditing(MHC))
		{
			Subsystem->CommitSkinSettings(MHC, MHC->SkinSettings);
		}
#endif
		MHC->PostEditChange();
		MHC->MarkPackageDirty();
		Session.Log(TEXT("[OK] metahuman_set_accents -> Updated accent regions"));
		return true;
	});

	// ================================================================
	// metahuman_set_eyes(asset, params)
	// ================================================================
	Lua.set_function("metahuman_set_eyes", [&Session](const std::string& AssetPath, sol::table Params, sol::this_state S) -> bool
	{
		FString Path = UTF8_TO_TCHAR(AssetPath.c_str());
		UMetaHumanCharacter* MHC = Cast<UMetaHumanCharacter>(StaticLoadObject(UMetaHumanCharacter::StaticClass(), nullptr, *Path));
		if (!MHC)
		{
			Session.Log(TEXT("[FAIL] metahuman_set_eyes -> Could not load MetaHumanCharacter"));
			return false;
		}

		FScopedTransaction Tx(FText::FromString(TEXT("Set MetaHuman Eyes")));
		MHC->Modify();

		auto ApplyIris = [](FMetaHumanCharacterEyeIrisProperties& Iris, const sol::table& T)
		{
			if (T["pattern"].valid())
			{
				sol::optional<std::string> PatternOpt = T["pattern"];
				if (PatternOpt.has_value()) Iris.IrisPattern = StringToIrisPattern(PatternOpt.value());
			}
			if (T["rotation"].valid()) Iris.IrisRotation = T.get_or("rotation", 0.0f);
			if (T["primary_color_u"].valid()) Iris.PrimaryColorU = T.get_or("primary_color_u", 0.5f);
			if (T["primary_color_v"].valid()) Iris.PrimaryColorV = T.get_or("primary_color_v", 0.5f);
			if (T["secondary_color_u"].valid()) Iris.SecondaryColorU = T.get_or("secondary_color_u", 0.5f);
			if (T["secondary_color_v"].valid()) Iris.SecondaryColorV = T.get_or("secondary_color_v", 0.5f);
			if (T["color_blend"].valid()) Iris.ColorBlend = T.get_or("color_blend", 0.5f);
			if (T["color_blend_softness"].valid()) Iris.ColorBlendSoftness = T.get_or("color_blend_softness", 0.5f);
			if (T["blend_method"].valid())
			{
				sol::optional<std::string> MethodOpt = T["blend_method"];
				if (MethodOpt.has_value())
				{
					FString Method = UTF8_TO_TCHAR(MethodOpt.value().c_str());
					Iris.BlendMethod = Method.Equals(TEXT("Radial"), ESearchCase::IgnoreCase)
						? EMetaHumanCharacterEyesBlendMethod::Radial
						: EMetaHumanCharacterEyesBlendMethod::Structural;
				}
			}
			if (T["shadow_details"].valid()) Iris.ShadowDetails = T.get_or("shadow_details", 0.5f);
			if (T["limbal_ring_size"].valid()) Iris.LimbalRingSize = T.get_or("limbal_ring_size", 0.725f);
			if (T["limbal_ring_softness"].valid()) Iris.LimbalRingSoftness = T.get_or("limbal_ring_softness", 0.085f);
			if (T["limbal_ring_color"].valid()) Iris.LimbalRingColor = TableToLinearColor(T["limbal_ring_color"]);
			if (T["global_saturation"].valid()) Iris.GlobalSaturation = T.get_or("global_saturation", 2.0f);
			if (T["global_tint"].valid()) Iris.GlobalTint = TableToLinearColor(T["global_tint"]);
		};

		auto ApplyPupil = [](FMetaHumanCharacterEyePupilProperties& Pupil, const sol::table& T)
		{
			if (T["dilation"].valid()) Pupil.Dilation = T.get_or("dilation", 0.95f);
			if (T["feather"].valid()) Pupil.Feather = T.get_or("feather", 0.45f);
		};

		auto ApplyCornea = [](FMetaHumanCharacterEyeCorneaProperties& Cornea, const sol::table& T)
		{
			if (T["size"].valid()) Cornea.Size = T.get_or("size", 0.165f);
			if (T["limbus_softness"].valid()) Cornea.LimbusSoftness = T.get_or("limbus_softness", 0.09f);
			if (T["limbus_color"].valid()) Cornea.LimbusColor = TableToLinearColor(T["limbus_color"]);
		};

		auto ApplySclera = [](FMetaHumanCharacterEyeScleraProperties& Sclera, const sol::table& T)
		{
			if (T["rotation"].valid()) Sclera.Rotation = T.get_or("rotation", 0.0f);
			if (T["use_custom_tint"].valid()) Sclera.bUseCustomTint = T.get_or("use_custom_tint", false);
			if (T["tint"].valid()) Sclera.Tint = TableToLinearColor(T["tint"]);
			if (T["transmission_spread"].valid()) Sclera.TransmissionSpread = T.get_or("transmission_spread", 0.115f);
			if (T["transmission_color"].valid()) Sclera.TransmissionColor = TableToLinearColor(T["transmission_color"]);
			if (T["vascularity_intensity"].valid()) Sclera.VascularityIntensity = T.get_or("vascularity_intensity", 1.0f);
			if (T["vascularity_coverage"].valid()) Sclera.VascularityCoverage = T.get_or("vascularity_coverage", 0.2f);
		};

		auto ApplyEye = [&](FMetaHumanCharacterEyeProperties& Eye, const sol::table& T)
		{
			if (T["iris"].valid()) ApplyIris(Eye.Iris, T["iris"]);
			if (T["pupil"].valid()) ApplyPupil(Eye.Pupil, T["pupil"]);
			if (T["cornea"].valid()) ApplyCornea(Eye.Cornea, T["cornea"]);
			if (T["sclera"].valid()) ApplySclera(Eye.Sclera, T["sclera"]);
		};

		if (Params["eye_left"].valid()) ApplyEye(MHC->EyesSettings.EyeLeft, Params["eye_left"]);
		if (Params["eye_right"].valid()) ApplyEye(MHC->EyesSettings.EyeRight, Params["eye_right"]);

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (Subsystem && Subsystem->IsObjectAddedForEditing(MHC))
		{
			Subsystem->CommitEyesSettings(MHC, MHC->EyesSettings);
		}
#endif
		MHC->PostEditChange();
		MHC->MarkPackageDirty();
		Session.Log(TEXT("[OK] metahuman_set_eyes -> Updated eye settings"));
		return true;
	});

	// ================================================================
	// metahuman_set_eyelashes(asset, params)
	// ================================================================
	Lua.set_function("metahuman_set_eyelashes", [&Session](const std::string& AssetPath, sol::table Params, sol::this_state S) -> bool
	{
		FString Path = UTF8_TO_TCHAR(AssetPath.c_str());
		UMetaHumanCharacter* MHC = Cast<UMetaHumanCharacter>(StaticLoadObject(UMetaHumanCharacter::StaticClass(), nullptr, *Path));
		if (!MHC)
		{
			Session.Log(TEXT("[FAIL] metahuman_set_eyelashes -> Could not load MetaHumanCharacter"));
			return false;
		}

		FScopedTransaction Tx(FText::FromString(TEXT("Set MetaHuman Eyelashes")));
		MHC->Modify();

		FMetaHumanCharacterEyelashesProperties& EL = MHC->HeadModelSettings.Eyelashes;
		if (Params["type"].valid())
		{
			sol::optional<std::string> TypeOpt = Params["type"];
			if (TypeOpt.has_value()) EL.Type = StringToEyelashType(TypeOpt.value());
		}
		if (Params["dye_color"].valid()) EL.DyeColor = TableToLinearColor(Params["dye_color"]);
		if (Params["melanin"].valid()) EL.Melanin = Params.get_or("melanin", 0.3f);
		if (Params["redness"].valid()) EL.Redness = Params.get_or("redness", 0.28f);
		if (Params["roughness"].valid()) EL.Roughness = Params.get_or("roughness", 0.25f);
		if (Params["salt_and_pepper"].valid()) EL.SaltAndPepper = Params.get_or("salt_and_pepper", 0.2f);
		if (Params["lightness"].valid()) EL.Lightness = Params.get_or("lightness", 0.5f);
		if (Params["enable_grooms"].valid()) EL.bEnableGrooms = Params.get_or("enable_grooms", true);

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (Subsystem && Subsystem->IsObjectAddedForEditing(MHC))
		{
			Subsystem->CommitHeadModelSettings(MHC, MHC->HeadModelSettings);
		}
#endif
		MHC->PostEditChange();
		MHC->MarkPackageDirty();
		Session.Log(TEXT("[OK] metahuman_set_eyelashes -> Updated eyelash settings"));
		return true;
	});

	// ================================================================
	// metahuman_set_teeth(asset, params)
	// ================================================================
	Lua.set_function("metahuman_set_teeth", [&Session](const std::string& AssetPath, sol::table Params, sol::this_state S) -> bool
	{
		FString Path = UTF8_TO_TCHAR(AssetPath.c_str());
		UMetaHumanCharacter* MHC = Cast<UMetaHumanCharacter>(StaticLoadObject(UMetaHumanCharacter::StaticClass(), nullptr, *Path));
		if (!MHC)
		{
			Session.Log(TEXT("[FAIL] metahuman_set_teeth -> Could not load MetaHumanCharacter"));
			return false;
		}

		FScopedTransaction Tx(FText::FromString(TEXT("Set MetaHuman Teeth")));
		MHC->Modify();

		FMetaHumanCharacterTeethProperties& T = MHC->HeadModelSettings.Teeth;
		if (Params["tooth_length"].valid()) T.ToothLength = Params.get_or("tooth_length", 0.0f);
		if (Params["tooth_spacing"].valid()) T.ToothSpacing = Params.get_or("tooth_spacing", 0.0f);
		if (Params["upper_shift"].valid()) T.UpperShift = Params.get_or("upper_shift", 0.0f);
		if (Params["lower_shift"].valid()) T.LowerShift = Params.get_or("lower_shift", 0.0f);
		if (Params["overbite"].valid()) T.Overbite = Params.get_or("overbite", 0.0f);
		if (Params["overjet"].valid()) T.Overjet = Params.get_or("overjet", 0.0f);
		if (Params["worn_down"].valid()) T.WornDown = Params.get_or("worn_down", 0.0f);
		if (Params["polycanine"].valid()) T.Polycanine = Params.get_or("polycanine", 0.0f);
		if (Params["receding_gums"].valid()) T.RecedingGums = Params.get_or("receding_gums", 0.0f);
		if (Params["narrowness"].valid()) T.Narrowness = Params.get_or("narrowness", 0.0f);
		if (Params["variation"].valid()) T.Variation = Params.get_or("variation", 0.0f);
		if (Params["jaw_open"].valid()) T.JawOpen = Params.get_or("jaw_open", 0.0f);
		if (Params["teeth_color"].valid()) T.TeethColor = TableToLinearColor(Params["teeth_color"]);
		if (Params["gum_color"].valid()) T.GumColor = TableToLinearColor(Params["gum_color"]);
		if (Params["plaque_color"].valid()) T.PlaqueColor = TableToLinearColor(Params["plaque_color"]);
		if (Params["plaque_amount"].valid()) T.PlaqueAmount = Params.get_or("plaque_amount", 0.0f);

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (Subsystem && Subsystem->IsObjectAddedForEditing(MHC))
		{
			Subsystem->CommitHeadModelSettings(MHC, MHC->HeadModelSettings);
		}
#endif
		MHC->PostEditChange();
		MHC->MarkPackageDirty();
		Session.Log(TEXT("[OK] metahuman_set_teeth -> Updated teeth settings"));
		return true;
	});

	// ================================================================
	// metahuman_set_makeup(asset, params)
	// ================================================================
	Lua.set_function("metahuman_set_makeup", [&Session](const std::string& AssetPath, sol::table Params, sol::this_state S) -> bool
	{
		FString Path = UTF8_TO_TCHAR(AssetPath.c_str());
		UMetaHumanCharacter* MHC = Cast<UMetaHumanCharacter>(StaticLoadObject(UMetaHumanCharacter::StaticClass(), nullptr, *Path));
		if (!MHC)
		{
			Session.Log(TEXT("[FAIL] metahuman_set_makeup -> Could not load MetaHumanCharacter"));
			return false;
		}

		FScopedTransaction Tx(FText::FromString(TEXT("Set MetaHuman Makeup")));
		MHC->Modify();

		FMetaHumanCharacterMakeupSettings& MU = MHC->MakeupSettings;

		if (Params["foundation"].valid())
		{
			sol::table FT = Params["foundation"];
			if (FT["apply"].valid()) MU.Foundation.bApplyFoundation = FT.get_or("apply", false);
			if (FT["color"].valid()) MU.Foundation.Color = TableToLinearColor(FT["color"]);
			if (FT["intensity"].valid()) MU.Foundation.Intensity = FT.get_or("intensity", 0.5f);
			if (FT["roughness"].valid()) MU.Foundation.Roughness = FT.get_or("roughness", 0.65f);
			if (FT["concealer"].valid()) MU.Foundation.Concealer = FT.get_or("concealer", 0.57f);
		}

		if (Params["eyes"].valid())
		{
			sol::table ET = Params["eyes"];
			if (ET["type"].valid())
			{
				sol::optional<std::string> TypeOpt = ET["type"];
				if (TypeOpt.has_value()) MU.Eyes.Type = StringToEyeMakeupType(TypeOpt.value());
			}
			if (ET["primary_color"].valid()) MU.Eyes.PrimaryColor = TableToLinearColor(ET["primary_color"]);
			if (ET["secondary_color"].valid()) MU.Eyes.SecondaryColor = TableToLinearColor(ET["secondary_color"]);
			if (ET["roughness"].valid()) MU.Eyes.Roughness = ET.get_or("roughness", 0.75f);
			if (ET["opacity"].valid()) MU.Eyes.Opacity = ET.get_or("opacity", 0.85f);
			if (ET["metalness"].valid()) MU.Eyes.Metalness = ET.get_or("metalness", 0.0f);
		}

		if (Params["blush"].valid())
		{
			sol::table BT = Params["blush"];
			if (BT["type"].valid())
			{
				sol::optional<std::string> TypeOpt = BT["type"];
				if (TypeOpt.has_value()) MU.Blush.Type = StringToBlushType(TypeOpt.value());
			}
			if (BT["color"].valid()) MU.Blush.Color = TableToLinearColor(BT["color"]);
			if (BT["intensity"].valid()) MU.Blush.Intensity = BT.get_or("intensity", 0.4f);
			if (BT["roughness"].valid()) MU.Blush.Roughness = BT.get_or("roughness", 0.6f);
		}

		if (Params["lips"].valid())
		{
			sol::table LT = Params["lips"];
			if (LT["type"].valid())
			{
				sol::optional<std::string> TypeOpt = LT["type"];
				if (TypeOpt.has_value()) MU.Lips.Type = StringToLipsMakeupType(TypeOpt.value());
			}
			if (LT["color"].valid()) MU.Lips.Color = TableToLinearColor(LT["color"]);
			if (LT["roughness"].valid()) MU.Lips.Roughness = LT.get_or("roughness", 0.25f);
			if (LT["opacity"].valid()) MU.Lips.Opacity = LT.get_or("opacity", 0.7f);
			if (LT["metalness"].valid()) MU.Lips.Metalness = LT.get_or("metalness", 1.0f);
		}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (Subsystem && Subsystem->IsObjectAddedForEditing(MHC))
		{
			Subsystem->CommitMakeupSettings(MHC, MHC->MakeupSettings);
		}
#endif
		MHC->PostEditChange();
		MHC->MarkPackageDirty();
		Session.Log(TEXT("[OK] metahuman_set_makeup -> Updated makeup settings"));
		return true;
	});

	// ================================================================
	// metahuman_set_face(asset, params)
	// ================================================================
	Lua.set_function("metahuman_set_face", [&Session](const std::string& AssetPath, sol::table Params, sol::this_state S) -> bool
	{
		FString Path = UTF8_TO_TCHAR(AssetPath.c_str());
		UMetaHumanCharacter* MHC = Cast<UMetaHumanCharacter>(StaticLoadObject(UMetaHumanCharacter::StaticClass(), nullptr, *Path));
		if (!MHC)
		{
			Session.Log(TEXT("[FAIL] metahuman_set_face -> Could not load MetaHumanCharacter"));
			return false;
		}

		FScopedTransaction Tx(FText::FromString(TEXT("Set MetaHuman Face")));
		MHC->Modify();

		if (Params["global_delta"].valid()) MHC->FaceEvaluationSettings.GlobalDelta = Params.get_or("global_delta", 1.0f);
		if (Params["high_frequency_delta"].valid()) MHC->FaceEvaluationSettings.HighFrequencyDelta = Params.get_or("high_frequency_delta", 1.0f);
		if (Params["head_scale"].valid()) MHC->FaceEvaluationSettings.HeadScale = Params.get_or("head_scale", 1.0f);

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (Subsystem && Subsystem->IsObjectAddedForEditing(MHC))
		{
			Subsystem->CommitFaceEvaluationSettings(MHC, MHC->FaceEvaluationSettings);
		}
#endif
		MHC->PostEditChange();
		MHC->MarkPackageDirty();
		Session.Log(TEXT("[OK] metahuman_set_face -> Updated face evaluation settings"));
		return true;
	});

	// ================================================================
	// metahuman_set_viewport(asset, params)
	// ================================================================
	Lua.set_function("metahuman_set_viewport", [&Session](const std::string& AssetPath, sol::table Params, sol::this_state S) -> bool
	{
		FString Path = UTF8_TO_TCHAR(AssetPath.c_str());
		UMetaHumanCharacter* MHC = Cast<UMetaHumanCharacter>(StaticLoadObject(UMetaHumanCharacter::StaticClass(), nullptr, *Path));
		if (!MHC)
		{
			Session.Log(TEXT("[FAIL] metahuman_set_viewport -> Could not load MetaHumanCharacter"));
			return false;
		}

		FScopedTransaction Tx(FText::FromString(TEXT("Set MetaHuman Viewport")));
		MHC->Modify();

		FMetaHumanCharacterViewportSettings& VP = MHC->ViewportSettings;
		if (Params["environment"].valid())
		{
			int Env = Params.get_or("environment", 0);
			if (Env >= 0 && Env < static_cast<int>(EMetaHumanCharacterEnvironment::Count))
			{
				VP.CharacterEnvironment = static_cast<EMetaHumanCharacterEnvironment>(Env);
			}
		}
		if (Params["background_color"].valid()) VP.BackgroundColor = TableToLinearColor(Params["background_color"]);
		if (Params["light_rotation"].valid()) VP.LightRotation = Params.get_or("light_rotation", 0.0f);
		if (Params["tonemapper_enabled"].valid()) VP.bTonemapperEnabled = Params.get_or("tonemapper_enabled", true);
		if (Params["lod"].valid())
		{
			int LOD = Params.get_or("lod", 0);
			if (LOD >= 0 && LOD < static_cast<int>(EMetaHumanCharacterLOD::Count))
			{
				VP.LevelOfDetail = static_cast<EMetaHumanCharacterLOD>(LOD);
			}
		}
		if (Params["camera_frame"].valid())
		{
			int CF = Params.get_or("camera_frame", 0);
			if (CF >= 0 && CF < static_cast<int>(EMetaHumanCharacterCameraFrame::Count))
			{
				VP.CameraFrame = static_cast<EMetaHumanCharacterCameraFrame>(CF);
			}
		}
		if (Params["rendering_quality"].valid())
		{
			int RQ = Params.get_or("rendering_quality", 0);
			if (RQ >= 0 && RQ < static_cast<int>(EMetaHumanCharacterRenderingQuality::Count))
			{
				VP.RenderingQuality = static_cast<EMetaHumanCharacterRenderingQuality>(RQ);
			}
		}
		if (Params["always_use_hair_cards"].valid()) VP.bAlwaysUseHairCards = Params.get_or("always_use_hair_cards", false);
		if (Params["show_viewport_overlays"].valid()) VP.bShowViewportOverlays = Params.get_or("show_viewport_overlays", true);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		if (Params["show_face_bones"].valid()) VP.bShowFaceBones = Params.get_or("show_face_bones", false);
		if (Params["show_body_bones"].valid()) VP.bShowBodyBones = Params.get_or("show_body_bones", false);
		if (Params["show_face_normals"].valid()) VP.bShowFaceNormals = Params.get_or("show_face_normals", false);
		if (Params["show_body_normals"].valid()) VP.bShowBodyNormals = Params.get_or("show_body_normals", false);
		if (Params["show_face_tangents"].valid()) VP.bShowFaceTangents = Params.get_or("show_face_tangents", false);
		if (Params["show_body_tangents"].valid()) VP.bShowBodyTangents = Params.get_or("show_body_tangents", false);
		if (Params["show_face_binormals"].valid()) VP.bShowFaceBinormals = Params.get_or("show_face_binormals", false);
		if (Params["show_body_binormals"].valid()) VP.bShowBodyBinormals = Params.get_or("show_body_binormals", false);
		if (Params["use_custom_environment"].valid()) VP.bUseCustomEnvironment = Params.get_or("use_custom_environment", false);
		if (Params["custom_environment"].valid())
		{
			sol::optional<std::string> EnvOpt = Params["custom_environment"];
			if (EnvOpt.has_value()) VP.CustomEnvironment = FSoftObjectPath(UTF8_TO_TCHAR(EnvOpt.value().c_str()));
		}
#endif

		MHC->PostEditChange();
		MHC->MarkPackageDirty();
		Session.Log(TEXT("[OK] metahuman_set_viewport -> Updated viewport settings"));
		return true;
	});

	// ================================================================
	// metahuman_set_assembly(asset, params)
	// ================================================================
	Lua.set_function("metahuman_set_assembly", [&Session](const std::string& AssetPath, sol::table Params, sol::this_state S) -> bool
	{
		FString Path = UTF8_TO_TCHAR(AssetPath.c_str());
		UMetaHumanCharacter* MHC = Cast<UMetaHumanCharacter>(StaticLoadObject(UMetaHumanCharacter::StaticClass(), nullptr, *Path));
		if (!MHC)
		{
			Session.Log(TEXT("[FAIL] metahuman_set_assembly -> Could not load MetaHumanCharacter"));
			return false;
		}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		FScopedTransaction Tx(FText::FromString(TEXT("Set MetaHuman Assembly")));
		MHC->Modify();

		FMetaHumanCharacterAssemblySettings& AS = MHC->AssemblySettings;

		if (Params["pipeline_type"].valid())
		{
			sol::optional<std::string> TypeOpt = Params["pipeline_type"];
			if (TypeOpt.has_value())
			{
				FString TypeStr = UTF8_TO_TCHAR(TypeOpt.value().c_str());
				if (TypeStr.Equals(TEXT("Cinematic"), ESearchCase::IgnoreCase)) AS.PipelineType = EMetaHumanDefaultPipelineType::Cinematic;
				else if (TypeStr.Equals(TEXT("Optimized"), ESearchCase::IgnoreCase)) AS.PipelineType = EMetaHumanDefaultPipelineType::Optimized;
				else if (TypeStr.Equals(TEXT("UEFN"), ESearchCase::IgnoreCase)) AS.PipelineType = EMetaHumanDefaultPipelineType::UEFN;
				else if (TypeStr.Equals(TEXT("DCC"), ESearchCase::IgnoreCase)) AS.PipelineType = EMetaHumanDefaultPipelineType::DCC;
			}
		}

		if (Params["quality"].valid())
		{
			sol::optional<std::string> QualOpt = Params["quality"];
			if (QualOpt.has_value())
			{
				FString QualStr = UTF8_TO_TCHAR(QualOpt.value().c_str());
				if (QualStr.Equals(TEXT("Low"), ESearchCase::IgnoreCase)) AS.PipelineQuality = EMetaHumanQualityLevel::Low;
				else if (QualStr.Equals(TEXT("Medium"), ESearchCase::IgnoreCase)) AS.PipelineQuality = EMetaHumanQualityLevel::Medium;
				else if (QualStr.Equals(TEXT("High"), ESearchCase::IgnoreCase)) AS.PipelineQuality = EMetaHumanQualityLevel::High;
				else if (QualStr.Equals(TEXT("Cinematic"), ESearchCase::IgnoreCase)) AS.PipelineQuality = EMetaHumanQualityLevel::Cinematic;
			}
		}

		if (Params["root_directory"].valid())
		{
			sol::optional<std::string> DirOpt = Params["root_directory"];
			if (DirOpt.has_value()) AS.RootDirectory.Path = UTF8_TO_TCHAR(DirOpt.value().c_str());
		}
		if (Params["common_directory"].valid())
		{
			sol::optional<std::string> DirOpt = Params["common_directory"];
			if (DirOpt.has_value()) AS.CommonDirectory.Path = UTF8_TO_TCHAR(DirOpt.value().c_str());
		}
		if (Params["name_override"].valid())
		{
			sol::optional<std::string> NameOpt = Params["name_override"];
			if (NameOpt.has_value()) AS.NameOverride = UTF8_TO_TCHAR(NameOpt.value().c_str());
		}
		if (Params["bake_makeup"].valid()) AS.bBakeMakeup = Params.get_or("bake_makeup", true);
		if (Params["export_zip"].valid()) AS.bExportZipFile = Params.get_or("export_zip", false);

		MHC->PostEditChange();
		MHC->MarkPackageDirty();
		Session.Log(TEXT("[OK] metahuman_set_assembly -> Updated assembly settings"));
		return true;
#else
		Session.Log(TEXT("[FAIL] metahuman_set_assembly -> Assembly settings are editor-only"));
		return false;
#endif
	});

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	// ================================================================
	// metahuman_set_body_type(asset, body_type)
	// ================================================================
	Lua.set_function("metahuman_set_body_type", [&Session](const std::string& AssetPath,
		const std::string& BodyTypeStr, sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return false;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_set_body_type -> MetaHumanCharacterEditorSubsystem not available"));
			return false;
		}

		// CRITICAL: Must register character for editing before body operations.
		// Without this, SetMetaHumanBodyType crashes on CharacterDataMap access.
		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
		{
			return false;
		}

		EMetaHumanBodyType BodyType = StringToBodyType(BodyTypeStr);

		FScopedTransaction Tx(FText::FromString(TEXT("Set MetaHuman Body Type")));
		MHC->Modify();

		Subsystem->SetMetaHumanBodyType(MHC, BodyType, UMetaHumanCharacterEditorSubsystem::EBodyMeshUpdateMode::Full);

		MHC->MarkPackageDirty();
		Session.Log(FString::Printf(TEXT("[OK] metahuman_set_body_type -> set to '%s'"),
			UTF8_TO_TCHAR(BodyTypeStr.c_str())));
		return true;
	});
	// ================================================================
	// metahuman_conform_body(asset, body_mesh_path, opts?)
	// Validated DNA conform pipeline: import mesh → get vertices → get joints → conform → commit
	// ================================================================
	Lua.set_function("metahuman_conform_body", [&Session](const std::string& AssetPath,
		const std::string& BodyMeshPath, sol::optional<sol::table> OptsOpt,
		sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return false;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_conform_body -> subsystem not available"));
			return false;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
		{
			return false;
		}

		// Load the body template mesh (e.g. imported DNA SkeletalMesh or any target body mesh)
		FString FMeshPath = UTF8_TO_TCHAR(BodyMeshPath.c_str());
		UObject* BodyMeshObj = StaticLoadObject(UObject::StaticClass(), nullptr, *FMeshPath);
		if (!BodyMeshObj)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] metahuman_conform_body -> body mesh not found: %s"), *FMeshPath));
			return false;
		}

		// Parse options
		bool bMatchByUVs = true;
		bool bRepose = true;
		bool bEstimateJoints = true;
		if (OptsOpt.has_value())
		{
			sol::table Opts = OptsOpt.value();
			if (Opts["match_by_uvs"].valid()) bMatchByUVs = Opts.get_or("match_by_uvs", true);
			if (Opts["repose"].valid()) bRepose = Opts.get_or("repose", true);
			if (Opts["estimate_joints"].valid()) bEstimateJoints = Opts.get_or("estimate_joints", true);
		}

		// Step 1: Get mesh vertices for body conforming
		TArray<FVector3f> Vertices;
		auto MeshResult = Subsystem->GetMeshForBodyConforming(MHC, BodyMeshObj, nullptr, bMatchByUVs, Vertices);
		if (MeshResult != EImportErrorCode::Success)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] metahuman_conform_body -> GetMeshForBodyConforming failed (error %d), got %d vertices"),
				static_cast<int>(MeshResult), Vertices.Num()));
			return false;
		}
		Session.Log(FString::Printf(TEXT("[INFO] metahuman_conform_body -> got %d vertices"), Vertices.Num()));

		// Step 2: Get joint rotations from the body mesh
		USkeletalMesh* BodySkelMesh = Cast<USkeletalMesh>(BodyMeshObj);
		if (!BodySkelMesh)
		{
			Session.Log(TEXT("[FAIL] metahuman_conform_body -> body mesh is not a SkeletalMesh (needed for joint extraction)"));
			return false;
		}

		TArray<FVector3f> JointTranslations;
		TArray<FVector3f> JointRotations;
		auto JointsResult = Subsystem->GetJointsForBodyConforming(BodySkelMesh, JointTranslations, JointRotations);
		if (JointsResult != EImportErrorCode::Success)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] metahuman_conform_body -> GetJointsForBodyConforming failed (error %d)"),
				static_cast<int>(JointsResult)));
			return false;
		}
		Session.Log(FString::Printf(TEXT("[INFO] metahuman_conform_body -> got %d joint rotations"), JointRotations.Num()));

		// Step 3: Conform body
		FScopedTransaction Tx(FText::FromString(TEXT("Conform MetaHuman Body")));
		MHC->Modify();

		bool bConformed = Subsystem->ConformBody(MHC, Vertices, JointRotations, bRepose, bEstimateJoints);
		if (!bConformed)
		{
			Session.Log(TEXT("[FAIL] metahuman_conform_body -> ConformBody returned false"));
			return false;
		}

		// Step 4: Commit body state
		Subsystem->CommitBodyState(MHC);
		MHC->MarkPackageDirty();

		Session.Log(FString::Printf(TEXT("[OK] metahuman_conform_body -> conformed to '%s' (%d verts, %d joints)"),
			*BodyMeshObj->GetName(), Vertices.Num(), JointRotations.Num()));
		return true;
	});

	// ================================================================
	// metahuman_get_body_constraints(asset)
	// ================================================================
	Lua.set_function("metahuman_get_body_constraints", [&Session](const std::string& AssetPath,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return sol::lua_nil;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_get_body_constraints -> subsystem not available"));
			return sol::lua_nil;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
		{
			return sol::lua_nil;
		}

		TArray<FMetaHumanCharacterBodyConstraint> Constraints = Subsystem->GetBodyConstraints(MHC, true);

		sol::table Result = LuaView.create_table();
		for (int32 i = 0; i < Constraints.Num(); ++i)
		{
			const FMetaHumanCharacterBodyConstraint& C = Constraints[i];
			sol::table Entry = LuaView.create_table();
			Entry["name"] = TCHAR_TO_UTF8(*C.Name.ToString());
			Entry["active"] = C.bIsActive;
			Entry["target"] = C.TargetMeasurement;
			Entry["min"] = C.MinMeasurement;
			Entry["max"] = C.MaxMeasurement;
			Entry["index"] = i + 1;
			Result[i + 1] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] metahuman_get_body_constraints -> %d constraints"), Constraints.Num()));
		return Result;
	});

	// ================================================================
	// metahuman_set_body_constraints(asset, constraints)
	// ================================================================
	Lua.set_function("metahuman_set_body_constraints", [&Session](const std::string& AssetPath,
		sol::table ConstraintsTable, sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return false;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_set_body_constraints -> subsystem not available"));
			return false;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
		{
			return false;
		}

		// Get current constraints as base
		TArray<FMetaHumanCharacterBodyConstraint> Constraints = Subsystem->GetBodyConstraints(MHC, true);

		// Apply modifications from Lua table
		for (const auto& Pair : ConstraintsTable)
		{
			if (!Pair.second.is<sol::table>()) continue;
			sol::table CT = Pair.second.as<sol::table>();

			sol::optional<std::string> NameOpt = CT.get<sol::optional<std::string>>("name");
			if (!NameOpt.has_value()) continue;

			FName TargetName = FName(UTF8_TO_TCHAR(NameOpt.value().c_str()));

			// Find matching constraint
			for (FMetaHumanCharacterBodyConstraint& C : Constraints)
			{
				if (C.Name == TargetName)
				{
					if (CT["active"].valid()) C.bIsActive = CT.get_or("active", false);
					if (CT["target"].valid()) C.TargetMeasurement = static_cast<float>(CT.get_or("target", 0.0));
					break;
				}
			}
		}

		FScopedTransaction Tx(FText::FromString(TEXT("Set MetaHuman Body Constraints")));
		MHC->Modify();

		Subsystem->SetBodyConstraints(MHC, Constraints);

		MHC->MarkPackageDirty();
		Session.Log(TEXT("[OK] metahuman_set_body_constraints -> applied constraints"));
		return true;
	});

	// ================================================================
	// metahuman_commit_body(asset)
	// ================================================================
	Lua.set_function("metahuman_commit_body", [&Session](const std::string& AssetPath,
		sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return false;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_commit_body -> subsystem not available"));
			return false;
		}

		// CommitBodyState(single-arg) has its own IsObjectAddedForEditing check,
		// but we still ensure registration for a better error message
		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
		{
			return false;
		}

		FScopedTransaction Tx(FText::FromString(TEXT("Commit MetaHuman Body")));
		MHC->Modify();

		Subsystem->CommitBodyState(MHC);

		MHC->MarkPackageDirty();
		Session.Log(TEXT("[OK] metahuman_commit_body -> body state committed"));
		return true;
	});

	// ================================================================
	// metahuman_build(asset, params?)
	// ================================================================
	Lua.set_function("metahuman_build", [&Session](const std::string& AssetPath,
		sol::optional<sol::table> ParamsOpt, sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return false;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_build -> subsystem not available"));
			return false;
		}

		// Build may require editing state for some operations
		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
		{
			return false;
		}

		if (!Subsystem->CanBuildMetaHuman(MHC, true))
		{
			Session.Log(TEXT("[FAIL] metahuman_build -> character is not ready for build (check CanBuildMetaHuman errors)"));
			return false;
		}

		// Build parameters from the character's own assembly settings by default
		FMetaHumanCharacterEditorBuildParameters BuildParams;

		if (ParamsOpt.has_value())
		{
			sol::table P = ParamsOpt.value();

			if (P["pipeline_type"].valid())
			{
				sol::optional<std::string> TypeOpt = P["pipeline_type"];
				if (TypeOpt.has_value())
				{
					FString TypeStr = UTF8_TO_TCHAR(TypeOpt.value().c_str());
					if (TypeStr.Equals(TEXT("Cinematic"), ESearchCase::IgnoreCase)) BuildParams.PipelineType = EMetaHumanDefaultPipelineType::Cinematic;
					else if (TypeStr.Equals(TEXT("Optimized"), ESearchCase::IgnoreCase)) BuildParams.PipelineType = EMetaHumanDefaultPipelineType::Optimized;
					else if (TypeStr.Equals(TEXT("UEFN"), ESearchCase::IgnoreCase)) BuildParams.PipelineType = EMetaHumanDefaultPipelineType::UEFN;
					else if (TypeStr.Equals(TEXT("DCC"), ESearchCase::IgnoreCase)) BuildParams.PipelineType = EMetaHumanDefaultPipelineType::DCC;
				}
			}

			if (P["quality"].valid())
			{
				sol::optional<std::string> QualOpt = P["quality"];
				if (QualOpt.has_value())
				{
					FString QualStr = UTF8_TO_TCHAR(QualOpt.value().c_str());
					if (QualStr.Equals(TEXT("Low"), ESearchCase::IgnoreCase)) BuildParams.PipelineQuality = EMetaHumanQualityLevel::Low;
					else if (QualStr.Equals(TEXT("Medium"), ESearchCase::IgnoreCase)) BuildParams.PipelineQuality = EMetaHumanQualityLevel::Medium;
					else if (QualStr.Equals(TEXT("High"), ESearchCase::IgnoreCase)) BuildParams.PipelineQuality = EMetaHumanQualityLevel::High;
					else if (QualStr.Equals(TEXT("Cinematic"), ESearchCase::IgnoreCase)) BuildParams.PipelineQuality = EMetaHumanQualityLevel::Cinematic;
				}
			}

			if (P["root_directory"].valid())
			{
				sol::optional<std::string> DirOpt = P["root_directory"];
				if (DirOpt.has_value()) BuildParams.AbsoluteBuildPath = UTF8_TO_TCHAR(DirOpt.value().c_str());
			}
			if (P["name_override"].valid())
			{
				sol::optional<std::string> NameOpt = P["name_override"];
				if (NameOpt.has_value()) BuildParams.NameOverride = UTF8_TO_TCHAR(NameOpt.value().c_str());
			}
			if (P["bake_makeup"].valid()) BuildParams.bBakeMakeup = P.get_or("bake_makeup", true);
		}

		Subsystem->BuildMetaHuman(MHC, BuildParams);

		Session.Log(TEXT("[OK] metahuman_build -> build initiated"));
		return true;
	});

	// ================================================================
	// metahuman_is_editing(asset)
	// ================================================================
	Lua.set_function("metahuman_is_editing", [&Session](const std::string& AssetPath,
		sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return false;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem) return false;

		return Subsystem->IsObjectAddedForEditing(MHC);
	});

	// ================================================================
	// metahuman_auto_rig(asset, opts?)
	// ================================================================
	Lua.set_function("metahuman_auto_rig", [&Session](const std::string& AssetPath,
		sol::optional<sol::table> OptsOpt, sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return false;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_auto_rig -> subsystem not available"));
			return false;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return false;

		FMetaHumanCharacterAutoRiggingRequestParams Params;
		Params.bBlocking = true; // Default to blocking for agent use
		Params.bReportProgress = true;

		if (OptsOpt.has_value())
		{
			sol::table Opts = OptsOpt.value();
			if (Opts["blocking"].valid()) Params.bBlocking = Opts.get_or("blocking", true);
			if (Opts["report_progress"].valid()) Params.bReportProgress = Opts.get_or("report_progress", true);
			if (Opts["rig_type"].valid())
			{
				sol::optional<std::string> RigTypeOpt = Opts["rig_type"];
				if (RigTypeOpt.has_value())
				{
					FString RigStr = UTF8_TO_TCHAR(RigTypeOpt.value().c_str());
					if (RigStr.Equals(TEXT("JointsAndBlendShapes"), ESearchCase::IgnoreCase))
						Params.RigType = EMetaHumanRigType::JointsAndBlendShapes;
					else
						Params.RigType = EMetaHumanRigType::JointsOnly;
				}
			}
		}

		Subsystem->RequestAutoRigging(MHC, Params);

		Session.Log(FString::Printf(TEXT("[OK] metahuman_auto_rig -> requested (blocking=%s, type=%s)"),
			Params.bBlocking ? TEXT("true") : TEXT("false"),
			Params.RigType == EMetaHumanRigType::JointsAndBlendShapes ? TEXT("JointsAndBlendShapes") : TEXT("JointsOnly")));
		return true;
	});

	// ================================================================
	// metahuman_import_face_dna(asset, dna_path, opts?)
	// ================================================================
	Lua.set_function("metahuman_import_face_dna", [&Session](const std::string& AssetPath,
		const std::string& DNAPath, sol::optional<sol::table> OptsOpt,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return sol::lua_nil;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_import_face_dna -> subsystem not available"));
			return sol::lua_nil;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return sol::lua_nil;

		FString FDNAPath = UTF8_TO_TCHAR(DNAPath.c_str());

		FImportFromDNAParams ImportParams;
		ImportParams.bImportWholeRig = true;
		ImportParams.AlignmentOptions = EAlignmentOptions::ScalingRotationTranslation;

		if (OptsOpt.has_value())
		{
			sol::table Opts = OptsOpt.value();
			if (Opts["whole_rig"].valid()) ImportParams.bImportWholeRig = Opts.get_or("whole_rig", true);
			if (Opts["alignment"].valid())
			{
				sol::optional<std::string> AlignOpt = Opts["alignment"];
				if (AlignOpt.has_value())
				{
					FString AlignStr = UTF8_TO_TCHAR(AlignOpt.value().c_str());
					if (AlignStr.Equals(TEXT("None"), ESearchCase::IgnoreCase))
						ImportParams.AlignmentOptions = EAlignmentOptions::None;
					else if (AlignStr.Equals(TEXT("Translation"), ESearchCase::IgnoreCase))
						ImportParams.AlignmentOptions = EAlignmentOptions::Translation;
					else if (AlignStr.Equals(TEXT("RotationTranslation"), ESearchCase::IgnoreCase))
						ImportParams.AlignmentOptions = EAlignmentOptions::RotationTranslation;
					else if (AlignStr.Equals(TEXT("ScalingTranslation"), ESearchCase::IgnoreCase))
						ImportParams.AlignmentOptions = EAlignmentOptions::ScalingTranslation;
					// else: default ScalingRotationTranslation
				}
			}
		}

		FScopedTransaction Tx(FText::FromString(TEXT("Import MetaHuman Face DNA")));
		MHC->Modify();

		EImportErrorCode Result = Subsystem->ImportFromFaceDna(MHC, FDNAPath, ImportParams);

		const char* ResultStr = "Unknown";
		switch (Result)
		{
		case EImportErrorCode::Success: ResultStr = "Success"; break;
		case EImportErrorCode::FittingError: ResultStr = "FittingError"; break;
		case EImportErrorCode::InvalidInputData: ResultStr = "InvalidInputData"; break;
		case EImportErrorCode::InvalidInputBones: ResultStr = "InvalidInputBones"; break;
		case EImportErrorCode::InvalidHeadMesh: ResultStr = "InvalidHeadMesh"; break;
		case EImportErrorCode::GeneralError: ResultStr = "GeneralError"; break;
		default: ResultStr = "Error"; break;
		}

		if (Result == EImportErrorCode::Success)
		{
			MHC->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] metahuman_import_face_dna -> imported from %s"), *FPaths::GetCleanFilename(FDNAPath)));
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] metahuman_import_face_dna -> %s"), UTF8_TO_TCHAR(ResultStr)));
		}

		return sol::make_object(LuaView, std::string(ResultStr));
	});

	// ================================================================
	// metahuman_request_textures(asset, opts?)
	// ================================================================
	Lua.set_function("metahuman_request_textures", [&Session](const std::string& AssetPath,
		sol::optional<sol::table> OptsOpt, sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return false;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_request_textures -> subsystem not available"));
			return false;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return false;

		FMetaHumanCharacterTextureRequestParams Params;
		Params.bBlocking = true;
		Params.bReportProgress = true;

		if (OptsOpt.has_value())
		{
			sol::table Opts = OptsOpt.value();
			if (Opts["blocking"].valid()) Params.bBlocking = Opts.get_or("blocking", true);
			if (Opts["report_progress"].valid()) Params.bReportProgress = Opts.get_or("report_progress", true);
		}

		Subsystem->RequestTextureSources(MHC, Params);

		Session.Log(FString::Printf(TEXT("[OK] metahuman_request_textures -> requested (blocking=%s)"),
			Params.bBlocking ? TEXT("true") : TEXT("false")));
		return true;
	});

	// ================================================================
	// metahuman_get_face_landmarks(asset)
	// ================================================================
	Lua.set_function("metahuman_get_face_landmarks", [&Session](const std::string& AssetPath,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return sol::lua_nil;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_get_face_landmarks -> subsystem not available"));
			return sol::lua_nil;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return sol::lua_nil;

		TArray<FVector> Landmarks;
		Subsystem->GetFaceLandmarks(MHC, Landmarks);

		sol::table Result = LuaView.create_table();
		for (int32 i = 0; i < Landmarks.Num(); ++i)
		{
			sol::table Entry = LuaView.create_table();
			Entry["x"] = Landmarks[i].X;
			Entry["y"] = Landmarks[i].Y;
			Entry["z"] = Landmarks[i].Z;
			Entry["index"] = i;
			Result[i + 1] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] metahuman_get_face_landmarks -> %d landmarks"), Landmarks.Num()));
		return Result;
	});

	// ================================================================
	// metahuman_set_face_landmarks(asset, landmarks)
	// ================================================================
	Lua.set_function("metahuman_set_face_landmarks", [&Session](const std::string& AssetPath,
		sol::table LandmarksTable, sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return false;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_set_face_landmarks -> subsystem not available"));
			return false;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return false;

		TArray<int32> Indices;
		TArray<FVector> Deltas;

		for (const auto& Pair : LandmarksTable)
		{
			if (!Pair.second.is<sol::table>()) continue;
			sol::table Entry = Pair.second.as<sol::table>();

			int32 Index = Entry.get_or("index", -1);
			if (Index < 0) continue;

			sol::optional<sol::table> DeltaOpt = Entry.get<sol::optional<sol::table>>("delta");
			if (!DeltaOpt.has_value()) continue;

			sol::table DT = DeltaOpt.value();
			FVector Delta(DT.get_or("x", 0.0), DT.get_or("y", 0.0), DT.get_or("z", 0.0));

			Indices.Add(Index);
			Deltas.Add(Delta);
		}

		if (Indices.Num() == 0)
		{
			Session.Log(TEXT("[FAIL] metahuman_set_face_landmarks -> no valid landmarks provided"));
			return false;
		}

		FScopedTransaction Tx(FText::FromString(TEXT("Translate MetaHuman Face Landmarks")));
		MHC->Modify();

		Subsystem->TranslateFaceLandmarks(MHC, Indices, Deltas);

		// Persist face state changes to the character asset
		Subsystem->CommitFaceState(MHC);

		MHC->MarkPackageDirty();
		Session.Log(FString::Printf(TEXT("[OK] metahuman_set_face_landmarks -> translated %d landmarks"), Indices.Num()));
		return true;
	});

	// ================================================================
	// metahuman_set_body_joints(asset, translations, rotations, opts?)
	// ================================================================
	Lua.set_function("metahuman_set_body_joints", [&Session](const std::string& AssetPath,
		sol::table TransTable, sol::table RotTable, sol::optional<sol::table> OptsOpt,
		sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return false;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_set_body_joints -> subsystem not available"));
			return false;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return false;

		bool bImportHelperJoints = true;
		if (OptsOpt.has_value())
		{
			bImportHelperJoints = OptsOpt.value().get_or("import_helper_joints", true);
		}

		TArray<FVector3f> Translations;
		for (const auto& Pair : TransTable)
		{
			if (!Pair.second.is<sol::table>()) continue;
			sol::table V = Pair.second.as<sol::table>();
			Translations.Add(FVector3f(
				static_cast<float>(V.get_or("x", 0.0)),
				static_cast<float>(V.get_or("y", 0.0)),
				static_cast<float>(V.get_or("z", 0.0))));
		}

		TArray<FVector3f> Rotations;
		for (const auto& Pair : RotTable)
		{
			if (!Pair.second.is<sol::table>()) continue;
			sol::table V = Pair.second.as<sol::table>();
			Rotations.Add(FVector3f(
				static_cast<float>(V.get_or("x", 0.0)),
				static_cast<float>(V.get_or("y", 0.0)),
				static_cast<float>(V.get_or("z", 0.0))));
		}

		FScopedTransaction Tx(FText::FromString(TEXT("Set MetaHuman Body Joints")));
		MHC->Modify();

		bool bSuccess = Subsystem->SetBodyJoints(MHC, Translations, Rotations, bImportHelperJoints);

		if (bSuccess)
		{
			MHC->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] metahuman_set_body_joints -> set %d translations, %d rotations"),
				Translations.Num(), Rotations.Num()));
		}
		else
		{
			Session.Log(TEXT("[FAIL] metahuman_set_body_joints -> SetBodyJoints returned false"));
		}
		return bSuccess;
	});

	// ================================================================
	// metahuman_set_body_mesh(asset, vertices, opts?)
	// ================================================================
	Lua.set_function("metahuman_set_body_mesh", [&Session](const std::string& AssetPath,
		sol::table VerticesTable, sol::optional<sol::table> OptsOpt,
		sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return false;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_set_body_mesh -> subsystem not available"));
			return false;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return false;

		bool bRepositionHelperJoints = true;
		if (OptsOpt.has_value())
		{
			bRepositionHelperJoints = OptsOpt.value().get_or("reposition_helper_joints", true);
		}

		TArray<FVector3f> Vertices;
		for (const auto& Pair : VerticesTable)
		{
			if (!Pair.second.is<sol::table>()) continue;
			sol::table V = Pair.second.as<sol::table>();
			Vertices.Add(FVector3f(
				static_cast<float>(V.get_or("x", 0.0)),
				static_cast<float>(V.get_or("y", 0.0)),
				static_cast<float>(V.get_or("z", 0.0))));
		}

		FScopedTransaction Tx(FText::FromString(TEXT("Set MetaHuman Body Mesh")));
		MHC->Modify();

		bool bSuccess = Subsystem->SetBodyMesh(MHC, Vertices, bRepositionHelperJoints);

		if (bSuccess)
		{
			MHC->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] metahuman_set_body_mesh -> set %d vertices"), Vertices.Num()));
		}
		else
		{
			Session.Log(TEXT("[FAIL] metahuman_set_body_mesh -> SetBodyMesh returned false"));
		}
		return bSuccess;
	});

	// ================================================================
	// metahuman_compare_face(asset1, asset2, tolerance?)
	// ================================================================
	Lua.set_function("metahuman_compare_face", [&Session](const std::string& Asset1Path,
		const std::string& Asset2Path, sol::optional<double> ToleranceOpt,
		sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC1 = LoadMHCharacter(Asset1Path, Session);
		UMetaHumanCharacter* MHC2 = LoadMHCharacter(Asset2Path, Session);
		if (!MHC1 || !MHC2) return false;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem) return false;

		if (!EnsureMHCharacterRegistered(Subsystem, MHC1, Session)) return false;
		if (!EnsureMHCharacterRegistered(Subsystem, MHC2, Session)) return false;

		float Tolerance = static_cast<float>(ToleranceOpt.value_or(0.01));
		bool bMatch = Subsystem->CompareFaceState(MHC1, MHC2, Tolerance);

		Session.Log(FString::Printf(TEXT("[OK] metahuman_compare_face -> %s (tolerance=%.4f)"),
			bMatch ? TEXT("MATCH") : TEXT("DIFFER"), Tolerance));
		return bMatch;
	});

	// ================================================================
	// metahuman_compare_body(asset1, asset2, tolerance?)
	// ================================================================
	Lua.set_function("metahuman_compare_body", [&Session](const std::string& Asset1Path,
		const std::string& Asset2Path, sol::optional<double> ToleranceOpt,
		sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC1 = LoadMHCharacter(Asset1Path, Session);
		UMetaHumanCharacter* MHC2 = LoadMHCharacter(Asset2Path, Session);
		if (!MHC1 || !MHC2) return false;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem) return false;

		if (!EnsureMHCharacterRegistered(Subsystem, MHC1, Session)) return false;
		if (!EnsureMHCharacterRegistered(Subsystem, MHC2, Session)) return false;

		float Tolerance = static_cast<float>(ToleranceOpt.value_or(0.01));
		bool bMatch = Subsystem->CompareBodyState(MHC1, MHC2, Tolerance);

		Session.Log(FString::Printf(TEXT("[OK] metahuman_compare_body -> %s (tolerance=%.4f)"),
			bMatch ? TEXT("MATCH") : TEXT("DIFFER"), Tolerance));
		return bMatch;
	});

	// ================================================================
	// metahuman_compare_textures(asset1, asset2, pixel_tolerance?)
	// ================================================================
	Lua.set_function("metahuman_compare_textures", [&Session](const std::string& Asset1Path,
		const std::string& Asset2Path, sol::optional<int> PixelToleranceOpt,
		sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC1 = LoadMHCharacter(Asset1Path, Session);
		UMetaHumanCharacter* MHC2 = LoadMHCharacter(Asset2Path, Session);
		if (!MHC1 || !MHC2) return false;

		int32 PixelTolerance = PixelToleranceOpt.value_or(1);
		bool bMatch = UMetaHumanCharacterEditorSubsystem::CompareFaceTextures(MHC1, MHC2, PixelTolerance);

		Session.Log(FString::Printf(TEXT("[OK] metahuman_compare_textures -> %s (pixel_tolerance=%d)"),
			bMatch ? TEXT("MATCH") : TEXT("DIFFER"), PixelTolerance));
		return bMatch;
	});

	// ================================================================
	// metahuman_commit_face(asset)
	// ================================================================
	Lua.set_function("metahuman_commit_face", [&Session](const std::string& AssetPath,
		sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return false;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_commit_face -> subsystem not available"));
			return false;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return false;

		FScopedTransaction Tx(FText::FromString(TEXT("Commit MetaHuman Face")));
		MHC->Modify();

		Subsystem->CommitFaceState(MHC);

		MHC->MarkPackageDirty();
		Session.Log(TEXT("[OK] metahuman_commit_face -> face state committed"));
		return true;
	});

	// ================================================================
	// metahuman_reset_face(asset)
	// ================================================================
	Lua.set_function("metahuman_reset_face", [&Session](const std::string& AssetPath,
		sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return false;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_reset_face -> subsystem not available"));
			return false;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return false;

		FScopedTransaction Tx(FText::FromString(TEXT("Reset MetaHuman Face")));
		MHC->Modify();

		Subsystem->ResetCharacterFace(MHC);

		MHC->MarkPackageDirty();
		Session.Log(TEXT("[OK] metahuman_reset_face -> face reset to default"));
		return true;
	});

	// ================================================================
	// metahuman_remove_face_rig(asset)
	// ================================================================
	Lua.set_function("metahuman_remove_face_rig", [&Session](const std::string& AssetPath,
		sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return false;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_remove_face_rig -> subsystem not available"));
			return false;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return false;

		FScopedTransaction Tx(FText::FromString(TEXT("Remove MetaHuman Face Rig")));
		MHC->Modify();

		Subsystem->RemoveFaceRig(MHC);

		MHC->MarkPackageDirty();
		Session.Log(TEXT("[OK] metahuman_remove_face_rig -> face rig removed"));
		return true;
	});

	// ================================================================
	// metahuman_remove_body_rig(asset)
	// ================================================================
	Lua.set_function("metahuman_remove_body_rig", [&Session](const std::string& AssetPath,
		sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return false;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_remove_body_rig -> subsystem not available"));
			return false;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return false;

		FScopedTransaction Tx(FText::FromString(TEXT("Remove MetaHuman Body Rig")));
		MHC->Modify();

		Subsystem->RemoveBodyRig(MHC);

		MHC->MarkPackageDirty();
		Session.Log(TEXT("[OK] metahuman_remove_body_rig -> body rig removed"));
		return true;
	});

	// ================================================================
	// metahuman_initialize_from_preset(asset, preset_asset)
	// ================================================================
	Lua.set_function("metahuman_initialize_from_preset", [&Session](const std::string& AssetPath,
		const std::string& PresetPath, sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return false;

		UMetaHumanCharacter* Preset = LoadMHCharacter(PresetPath, Session);
		if (!Preset)
		{
			Session.Log(TEXT("[FAIL] metahuman_initialize_from_preset -> could not load preset character"));
			return false;
		}

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_initialize_from_preset -> subsystem not available"));
			return false;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return false;

		FScopedTransaction Tx(FText::FromString(TEXT("Initialize MetaHuman From Preset")));
		MHC->Modify();

		Subsystem->InitializeFromPreset(MHC, Preset);

		MHC->MarkPackageDirty();
		Session.Log(FString::Printf(TEXT("[OK] metahuman_initialize_from_preset -> initialized from '%s'"),
			*Preset->GetName()));
		return true;
	});

	// ================================================================
	// metahuman_get_face_gizmos(asset)
	// ================================================================
	Lua.set_function("metahuman_get_face_gizmos", [&Session](const std::string& AssetPath,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return sol::lua_nil;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_get_face_gizmos -> subsystem not available"));
			return sol::lua_nil;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return sol::lua_nil;

		TArray<FVector3f> Gizmos = Subsystem->GetFaceGizmos(MHC);

		sol::table Result = LuaView.create_table();
		for (int32 i = 0; i < Gizmos.Num(); ++i)
		{
			sol::table Entry = LuaView.create_table();
			Entry["x"] = Gizmos[i].X;
			Entry["y"] = Gizmos[i].Y;
			Entry["z"] = Gizmos[i].Z;
			Entry["index"] = i;
			Result[i + 1] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] metahuman_get_face_gizmos -> %d gizmos"), Gizmos.Num()));
		return Result;
	});

	// ================================================================
	// metahuman_set_face_gizmo(asset, opts)
	// ================================================================
	Lua.set_function("metahuman_set_face_gizmo", [&Session](const std::string& AssetPath,
		sol::table Opts, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return sol::lua_nil;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_set_face_gizmo -> subsystem not available"));
			return sol::lua_nil;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return sol::lua_nil;

		int32 GizmoIndex = Opts.get_or("index", 0);
		bool bSymmetric = Opts.get_or("symmetric", false);
		bool bEnforceBounds = Opts.get_or("enforce_bounds", true);

		sol::optional<std::string> TypeOpt = Opts.get<sol::optional<std::string>>("type");
		if (!TypeOpt.has_value())
		{
			Session.Log(TEXT("[FAIL] metahuman_set_face_gizmo -> 'type' is required (position, rotation, scale)"));
			return sol::lua_nil;
		}

		FString TypeStr = UTF8_TO_TCHAR(TypeOpt.value().c_str());
		TSharedRef<const FMetaHumanCharacterIdentity::FState> FaceState = Subsystem->GetFaceState(MHC);

		FScopedTransaction Tx(FText::FromString(TEXT("Set MetaHuman Face Gizmo")));
		MHC->Modify();

		TArray<FVector3f> UpdatedGizmos;

		if (TypeStr.Equals(TEXT("position"), ESearchCase::IgnoreCase))
		{
			sol::optional<sol::table> ValOpt = Opts.get<sol::optional<sol::table>>("value");
			if (!ValOpt.has_value())
			{
				Session.Log(TEXT("[FAIL] metahuman_set_face_gizmo -> 'value' table {x,y,z} required for position"));
				return sol::lua_nil;
			}
			sol::table V = ValOpt.value();
			FVector3f Pos(
				static_cast<float>(V.get_or("x", 0.0)),
				static_cast<float>(V.get_or("y", 0.0)),
				static_cast<float>(V.get_or("z", 0.0)));
			UpdatedGizmos = Subsystem->SetFaceGizmoPosition(MHC, FaceState, GizmoIndex, Pos, bSymmetric, bEnforceBounds);
		}
		else if (TypeStr.Equals(TEXT("rotation"), ESearchCase::IgnoreCase))
		{
			sol::optional<sol::table> ValOpt = Opts.get<sol::optional<sol::table>>("value");
			if (!ValOpt.has_value())
			{
				Session.Log(TEXT("[FAIL] metahuman_set_face_gizmo -> 'value' table {x,y,z} required for rotation"));
				return sol::lua_nil;
			}
			sol::table V = ValOpt.value();
			FVector3f Rot(
				static_cast<float>(V.get_or("x", 0.0)),
				static_cast<float>(V.get_or("y", 0.0)),
				static_cast<float>(V.get_or("z", 0.0)));
			UpdatedGizmos = Subsystem->SetFaceGizmoRotation(MHC, FaceState, GizmoIndex, Rot, bSymmetric, bEnforceBounds);
		}
		else if (TypeStr.Equals(TEXT("scale"), ESearchCase::IgnoreCase))
		{
			float Scale = static_cast<float>(Opts.get_or("value", 1.0));
			UpdatedGizmos = Subsystem->SetFaceGizmoScale(MHC, FaceState, GizmoIndex, Scale, bSymmetric, bEnforceBounds);
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] metahuman_set_face_gizmo -> unknown type '%s'"), *TypeStr));
			return sol::lua_nil;
		}

		MHC->MarkPackageDirty();

		// Return updated gizmo positions
		sol::table Result = LuaView.create_table();
		for (int32 i = 0; i < UpdatedGizmos.Num(); ++i)
		{
			sol::table Entry = LuaView.create_table();
			Entry["x"] = UpdatedGizmos[i].X;
			Entry["y"] = UpdatedGizmos[i].Y;
			Entry["z"] = UpdatedGizmos[i].Z;
			Entry["index"] = i;
			Result[i + 1] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] metahuman_set_face_gizmo -> %s gizmo %d, %d gizmos updated"),
			*TypeStr, GizmoIndex, UpdatedGizmos.Num()));
		return Result;
	});

	// ================================================================
	// metahuman_get_body_gizmos(asset)
	// ================================================================
	Lua.set_function("metahuman_get_body_gizmos", [&Session](const std::string& AssetPath,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return sol::lua_nil;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_get_body_gizmos -> subsystem not available"));
			return sol::lua_nil;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return sol::lua_nil;

		TArray<FVector3f> Gizmos = Subsystem->GetBodyGizmos(MHC);

		sol::table Result = LuaView.create_table();
		for (int32 i = 0; i < Gizmos.Num(); ++i)
		{
			sol::table Entry = LuaView.create_table();
			Entry["x"] = Gizmos[i].X;
			Entry["y"] = Gizmos[i].Y;
			Entry["z"] = Gizmos[i].Z;
			Entry["index"] = i;
			Result[i + 1] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] metahuman_get_body_gizmos -> %d gizmos"), Gizmos.Num()));
		return Result;
	});

	// ================================================================
	// metahuman_get_rigging_state(asset)
	// ================================================================
	Lua.set_function("metahuman_get_rigging_state", [&Session](const std::string& AssetPath,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return sol::lua_nil;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_get_rigging_state -> subsystem not available"));
			return sol::lua_nil;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return sol::lua_nil;

		EMetaHumanCharacterRigState State = Subsystem->GetRiggingState(MHC);
		const char* StateStr = "Unknown";
		switch (State)
		{
		case EMetaHumanCharacterRigState::Unrigged: StateStr = "Unrigged"; break;
		case EMetaHumanCharacterRigState::RigPending: StateStr = "RigPending"; break;
		case EMetaHumanCharacterRigState::Rigged: StateStr = "Rigged"; break;
		}

		Session.Log(FString::Printf(TEXT("[OK] metahuman_get_rigging_state -> %s"), UTF8_TO_TCHAR(StateStr)));
		return sol::make_object(LuaView, std::string(StateStr));
	});

	// ================================================================
	// metahuman_import_from_template(asset, template_mesh_path, opts?)
	// ================================================================
	Lua.set_function("metahuman_import_from_template", [&Session](const std::string& AssetPath,
		const std::string& TemplateMeshPath, sol::optional<sol::table> OptsOpt,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return sol::lua_nil;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_import_from_template -> subsystem not available"));
			return sol::lua_nil;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return sol::lua_nil;

		FString FMeshPath = UTF8_TO_TCHAR(TemplateMeshPath.c_str());
		UObject* TemplateMesh = StaticLoadObject(UObject::StaticClass(), nullptr, *FMeshPath);
		if (!TemplateMesh)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] metahuman_import_from_template -> template mesh not found: %s"), *FMeshPath));
			return sol::lua_nil;
		}

		// Optional eye/teeth meshes
		UObject* LeftEyeMesh = nullptr;
		UObject* RightEyeMesh = nullptr;
		UObject* TeethMesh = nullptr;

		FImportFromTemplateParams ImportParams;
		ImportParams.AlignmentOptions = EAlignmentOptions::ScalingRotationTranslation;

		if (OptsOpt.has_value())
		{
			sol::table Opts = OptsOpt.value();
			if (Opts["match_by_uvs"].valid()) ImportParams.bMatchVerticesByUVs = Opts.get_or("match_by_uvs", false);
			if (Opts["use_eyes"].valid()) ImportParams.bUseEyeMeshes = Opts.get_or("use_eyes", true);
			if (Opts["use_teeth"].valid()) ImportParams.bUseTeethMesh = Opts.get_or("use_teeth", true);
			if (Opts["alignment"].valid())
			{
				sol::optional<std::string> AlignOpt = Opts["alignment"];
				if (AlignOpt.has_value())
				{
					FString AlignStr = UTF8_TO_TCHAR(AlignOpt.value().c_str());
					if (AlignStr.Equals(TEXT("None"), ESearchCase::IgnoreCase))
						ImportParams.AlignmentOptions = EAlignmentOptions::None;
					else if (AlignStr.Equals(TEXT("Translation"), ESearchCase::IgnoreCase))
						ImportParams.AlignmentOptions = EAlignmentOptions::Translation;
					else if (AlignStr.Equals(TEXT("RotationTranslation"), ESearchCase::IgnoreCase))
						ImportParams.AlignmentOptions = EAlignmentOptions::RotationTranslation;
					else if (AlignStr.Equals(TEXT("ScalingTranslation"), ESearchCase::IgnoreCase))
						ImportParams.AlignmentOptions = EAlignmentOptions::ScalingTranslation;
				}
			}

			sol::optional<std::string> LeftEyeOpt = Opts.get<sol::optional<std::string>>("left_eye_mesh");
			if (LeftEyeOpt.has_value())
			{
				FString P = UTF8_TO_TCHAR(LeftEyeOpt.value().c_str());
				LeftEyeMesh = StaticLoadObject(UObject::StaticClass(), nullptr, *P);
			}
			sol::optional<std::string> RightEyeOpt = Opts.get<sol::optional<std::string>>("right_eye_mesh");
			if (RightEyeOpt.has_value())
			{
				FString P = UTF8_TO_TCHAR(RightEyeOpt.value().c_str());
				RightEyeMesh = StaticLoadObject(UObject::StaticClass(), nullptr, *P);
			}
			sol::optional<std::string> TeethOpt = Opts.get<sol::optional<std::string>>("teeth_mesh");
			if (TeethOpt.has_value())
			{
				FString P = UTF8_TO_TCHAR(TeethOpt.value().c_str());
				TeethMesh = StaticLoadObject(UObject::StaticClass(), nullptr, *P);
			}
		}

		FScopedTransaction Tx(FText::FromString(TEXT("Import MetaHuman From Template")));
		MHC->Modify();

		EImportErrorCode Result = Subsystem->ImportFromTemplate(MHC, TemplateMesh, LeftEyeMesh, RightEyeMesh, TeethMesh, ImportParams);

		const char* ResultStr = "Unknown";
		switch (Result)
		{
		case EImportErrorCode::Success: ResultStr = "Success"; break;
		case EImportErrorCode::FittingError: ResultStr = "FittingError"; break;
		case EImportErrorCode::InvalidInputData: ResultStr = "InvalidInputData"; break;
		case EImportErrorCode::InvalidInputBones: ResultStr = "InvalidInputBones"; break;
		case EImportErrorCode::InvalidHeadMesh: ResultStr = "InvalidHeadMesh"; break;
		case EImportErrorCode::GeneralError: ResultStr = "GeneralError"; break;
		default: ResultStr = "Error"; break;
		}

		if (Result == EImportErrorCode::Success)
		{
			MHC->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] metahuman_import_from_template -> imported from %s"), *FPaths::GetCleanFilename(FMeshPath)));
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] metahuman_import_from_template -> %s"), UTF8_TO_TCHAR(ResultStr)));
		}

		return sol::make_object(LuaView, std::string(ResultStr));
	});

	// ================================================================
	// metahuman_get_skin_tone(u, v)
	// ================================================================
	Lua.set_function("metahuman_get_skin_tone", [&Session](double U, double V,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_get_skin_tone -> subsystem not available"));
			return sol::lua_nil;
		}

		FLinearColor Color = Subsystem->GetSkinTone(FVector2f(static_cast<float>(U), static_cast<float>(V)));

		sol::table Result = LuaView.create_table();
		Result["r"] = Color.R;
		Result["g"] = Color.G;
		Result["b"] = Color.B;
		Result["a"] = Color.A;

		Session.Log(FString::Printf(TEXT("[OK] metahuman_get_skin_tone -> (%.3f, %.3f, %.3f)"),
			Color.R, Color.G, Color.B));
		return Result;
	});

	// ================================================================
	// metahuman_estimate_skin_tone(color, hf_index)
	// ================================================================
	Lua.set_function("metahuman_estimate_skin_tone", [&Session](sol::table ColorTable, int HFIndex,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_estimate_skin_tone -> subsystem not available"));
			return sol::lua_nil;
		}

		FLinearColor Color(
			ColorTable.get_or("r", 0.5f),
			ColorTable.get_or("g", 0.5f),
			ColorTable.get_or("b", 0.5f),
			1.0f);

		FVector2f UV = Subsystem->EstimateSkinTone(Color, HFIndex);

		sol::table Result = LuaView.create_table();
		Result["u"] = UV.X;
		Result["v"] = UV.Y;

		Session.Log(FString::Printf(TEXT("[OK] metahuman_estimate_skin_tone -> u=%.4f, v=%.4f"), UV.X, UV.Y));
		return Result;
	});

	// ================================================================
	// metahuman_set_clothing_visibility(asset, state)
	// ================================================================
	Lua.set_function("metahuman_set_clothing_visibility", [&Session](const std::string& AssetPath,
		const std::string& StateStr, sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return false;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_set_clothing_visibility -> subsystem not available"));
			return false;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return false;

		FString S2 = UTF8_TO_TCHAR(StateStr.c_str());
		EMetaHumanClothingVisibilityState State = EMetaHumanClothingVisibilityState::Shown;
		if (S2.Equals(TEXT("Hidden"), ESearchCase::IgnoreCase))
			State = EMetaHumanClothingVisibilityState::Hidden;
		else if (S2.Equals(TEXT("UseOverrideMaterial"), ESearchCase::IgnoreCase))
			State = EMetaHumanClothingVisibilityState::UseOverrideMaterial;

		Subsystem->SetClothingVisibilityState(MHC, State, true);

		Session.Log(FString::Printf(TEXT("[OK] metahuman_set_clothing_visibility -> %s"), *S2));
		return true;
	});

	// ================================================================
	// metahuman_get_clothing_visibility(asset)
	// ================================================================
	Lua.set_function("metahuman_get_clothing_visibility", [&Session](const std::string& AssetPath,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return sol::lua_nil;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_get_clothing_visibility -> subsystem not available"));
			return sol::lua_nil;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return sol::lua_nil;

		EMetaHumanClothingVisibilityState State = Subsystem->GetClothingVisibilityState(MHC);
		const char* StateStr = "Shown";
		switch (State)
		{
		case EMetaHumanClothingVisibilityState::Shown: StateStr = "Shown"; break;
		case EMetaHumanClothingVisibilityState::Hidden: StateStr = "Hidden"; break;
		case EMetaHumanClothingVisibilityState::UseOverrideMaterial: StateStr = "UseOverrideMaterial"; break;
		}

		Session.Log(FString::Printf(TEXT("[OK] metahuman_get_clothing_visibility -> %s"), UTF8_TO_TCHAR(StateStr)));
		return sol::make_object(LuaView, std::string(StateStr));
	});

	// ================================================================
	// metahuman_set_body_global_delta(asset, delta)
	// ================================================================
	Lua.set_function("metahuman_set_body_global_delta", [&Session](const std::string& AssetPath,
		double Delta, sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return false;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_set_body_global_delta -> subsystem not available"));
			return false;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return false;

		Subsystem->SetBodyGlobalDeltaScale(MHC, static_cast<float>(Delta));

		Session.Log(FString::Printf(TEXT("[OK] metahuman_set_body_global_delta -> %.4f"), Delta));
		return true;
	});

	// ================================================================
	// metahuman_get_body_global_delta(asset)
	// ================================================================
	Lua.set_function("metahuman_get_body_global_delta", [&Session](const std::string& AssetPath,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return sol::lua_nil;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_get_body_global_delta -> subsystem not available"));
			return sol::lua_nil;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return sol::lua_nil;

		float Delta = Subsystem->GetBodyGlobalDeltaScale(MHC);

		Session.Log(FString::Printf(TEXT("[OK] metahuman_get_body_global_delta -> %.4f"), Delta));
		return sol::make_object(LuaView, static_cast<double>(Delta));
	});

	// ================================================================
	// metahuman_update_preview_material(asset, type)
	// ================================================================
	Lua.set_function("metahuman_update_preview_material", [&Session](const std::string& AssetPath,
		const std::string& TypeStr, sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return false;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_update_preview_material -> subsystem not available"));
			return false;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return false;

		FString TS = UTF8_TO_TCHAR(TypeStr.c_str());
		EMetaHumanCharacterSkinPreviewMaterial MatType = EMetaHumanCharacterSkinPreviewMaterial::Default;
		if (TS.Equals(TEXT("Skin"), ESearchCase::IgnoreCase))
			MatType = EMetaHumanCharacterSkinPreviewMaterial::Editable;
		else if (TS.Equals(TEXT("Clay"), ESearchCase::IgnoreCase))
			MatType = EMetaHumanCharacterSkinPreviewMaterial::Clay;

		Subsystem->UpdateCharacterPreviewMaterial(MHC, MatType);

		Session.Log(FString::Printf(TEXT("[OK] metahuman_update_preview_material -> %s"), *TS));
		return true;
	});

	// ================================================================
	// metahuman_remove_textures_and_rigs(asset)
	// ================================================================
	Lua.set_function("metahuman_remove_textures_and_rigs", [&Session](const std::string& AssetPath,
		sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return false;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_remove_textures_and_rigs -> subsystem not available"));
			return false;
		}

		// Character must NOT be open for editing for this operation
		if (Subsystem->IsObjectAddedForEditing(MHC))
		{
			Session.Log(TEXT("[FAIL] metahuman_remove_textures_and_rigs -> character must not be open for editing"));
			return false;
		}

		FScopedTransaction Tx(FText::FromString(TEXT("Remove MetaHuman Textures And Rigs")));
		MHC->Modify();

		bool bSuccess = Subsystem->RemoveTexturesAndRigs(MHC);

		if (bSuccess)
		{
			MHC->MarkPackageDirty();
			Session.Log(TEXT("[OK] metahuman_remove_textures_and_rigs -> stripped to preset"));
		}
		else
		{
			Session.Log(TEXT("[FAIL] metahuman_remove_textures_and_rigs -> failed"));
		}
		return bSuccess;
	});

	// ================================================================
	// metahuman_spawn_actor(asset)
	// ================================================================
	Lua.set_function("metahuman_spawn_actor", [&Session](const std::string& AssetPath,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return sol::lua_nil;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_spawn_actor -> subsystem not available"));
			return sol::lua_nil;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return sol::lua_nil;

		AActor* SpawnedActor = Subsystem->SpawnMetaHumanActor(MHC);
		if (!SpawnedActor)
		{
			Session.Log(TEXT("[FAIL] metahuman_spawn_actor -> spawn returned null"));
			return sol::lua_nil;
		}

		FString ActorName = SpawnedActor->GetName();
		Session.Log(FString::Printf(TEXT("[OK] metahuman_spawn_actor -> spawned '%s'"), *ActorName));
		return sol::make_object(LuaView, std::string(TCHAR_TO_UTF8(*ActorName)));
	});

	// ================================================================
	// metahuman_blend_face_region(asset, opts)
	// ================================================================
	Lua.set_function("metahuman_blend_face_region", [&Session](const std::string& AssetPath,
		sol::table Opts, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return sol::lua_nil;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_blend_face_region -> subsystem not available"));
			return sol::lua_nil;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return sol::lua_nil;

		int32 RegionIndex = Opts.get_or("region_index", 0);
		bool bSymmetric = Opts.get_or("symmetric", false);

		// Parse blend options
		EBlendOptions BlendOpts = EBlendOptions::Both;
		sol::optional<std::string> BlendOptStr = Opts.get<sol::optional<std::string>>("blend_options");
		if (BlendOptStr.has_value())
		{
			FString BO = UTF8_TO_TCHAR(BlendOptStr.value().c_str());
			if (BO.Equals(TEXT("Proportions"), ESearchCase::IgnoreCase)) BlendOpts = EBlendOptions::Proportions;
			else if (BO.Equals(TEXT("Features"), ESearchCase::IgnoreCase)) BlendOpts = EBlendOptions::Features;
		}

		// Get start state from current character
		TSharedPtr<const FMetaHumanCharacterIdentity::FState> StartState = Subsystem->CopyFaceState(MHC);

		// Load preset characters and extract their face states
		TArray<TSharedPtr<const FMetaHumanCharacterIdentity::FState>> PresetStates;
		TArray<float> Weights;

		sol::optional<sol::table> PresetsOpt = Opts.get<sol::optional<sol::table>>("preset_assets");
		sol::optional<sol::table> WeightsOpt = Opts.get<sol::optional<sol::table>>("weights");
		if (!PresetsOpt.has_value() || !WeightsOpt.has_value())
		{
			Session.Log(TEXT("[FAIL] metahuman_blend_face_region -> 'preset_assets' and 'weights' are required"));
			return sol::lua_nil;
		}

		sol::table PresetPaths = PresetsOpt.value();
		sol::table WeightsTable = WeightsOpt.value();

		for (const auto& Pair : PresetPaths)
		{
			if (!Pair.second.is<std::string>()) continue;
			std::string PresetPathStr = Pair.second.as<std::string>();
			UMetaHumanCharacter* PresetMHC = LoadMHCharacter(PresetPathStr, Session);
			if (!PresetMHC) return sol::lua_nil;

			if (!EnsureMHCharacterRegistered(Subsystem, PresetMHC, Session))
				return sol::lua_nil;

			PresetStates.Add(Subsystem->CopyFaceState(PresetMHC));
		}

		for (const auto& Pair : WeightsTable)
		{
			if (Pair.second.is<double>())
				Weights.Add(static_cast<float>(Pair.second.as<double>()));
		}

		if (PresetStates.Num() == 0 || Weights.Num() == 0)
		{
			Session.Log(TEXT("[FAIL] metahuman_blend_face_region -> no valid presets or weights"));
			return sol::lua_nil;
		}

		FScopedTransaction Tx(FText::FromString(TEXT("Blend MetaHuman Face Region")));
		MHC->Modify();

		TArray<FVector3f> UpdatedGizmos = Subsystem->BlendFaceRegion(MHC, RegionIndex, StartState, PresetStates, Weights, BlendOpts, bSymmetric);

		MHC->MarkPackageDirty();

		sol::table Result = LuaView.create_table();
		for (int32 i = 0; i < UpdatedGizmos.Num(); ++i)
		{
			sol::table Entry = LuaView.create_table();
			Entry["x"] = UpdatedGizmos[i].X;
			Entry["y"] = UpdatedGizmos[i].Y;
			Entry["z"] = UpdatedGizmos[i].Z;
			Result[i + 1] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] metahuman_blend_face_region -> region %d, %d presets, %d gizmos"),
			RegionIndex, PresetStates.Num(), UpdatedGizmos.Num()));
		return Result;
	});

	// ================================================================
	// metahuman_blend_body_region(asset, opts)
	// ================================================================
	Lua.set_function("metahuman_blend_body_region", [&Session](const std::string& AssetPath,
		sol::table Opts, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return sol::lua_nil;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_blend_body_region -> subsystem not available"));
			return sol::lua_nil;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return sol::lua_nil;

		int32 RegionIndex = Opts.get_or("region_index", 0);

		// Parse blend options
		EBodyBlendOptions BlendOpts = EBodyBlendOptions::Both;
		sol::optional<std::string> BlendOptStr = Opts.get<sol::optional<std::string>>("blend_options");
		if (BlendOptStr.has_value())
		{
			FString BO = UTF8_TO_TCHAR(BlendOptStr.value().c_str());
			if (BO.Equals(TEXT("Skeleton"), ESearchCase::IgnoreCase)) BlendOpts = EBodyBlendOptions::Skeleton;
			else if (BO.Equals(TEXT("Shape"), ESearchCase::IgnoreCase)) BlendOpts = EBodyBlendOptions::Shape;
		}

		// Get start state from current character
		TSharedPtr<const FMetaHumanCharacterBodyIdentity::FState> StartState = Subsystem->CopyBodyState(MHC);

		// Load preset characters and extract their body states
		TArray<TSharedPtr<const FMetaHumanCharacterBodyIdentity::FState>> PresetStates;
		TArray<float> Weights;

		sol::optional<sol::table> PresetsOpt = Opts.get<sol::optional<sol::table>>("preset_assets");
		sol::optional<sol::table> WeightsOpt = Opts.get<sol::optional<sol::table>>("weights");
		if (!PresetsOpt.has_value() || !WeightsOpt.has_value())
		{
			Session.Log(TEXT("[FAIL] metahuman_blend_body_region -> 'preset_assets' and 'weights' are required"));
			return sol::lua_nil;
		}

		sol::table PresetPaths = PresetsOpt.value();
		sol::table WeightsTable = WeightsOpt.value();

		for (const auto& Pair : PresetPaths)
		{
			if (!Pair.second.is<std::string>()) continue;
			std::string PresetPathStr = Pair.second.as<std::string>();
			UMetaHumanCharacter* PresetMHC = LoadMHCharacter(PresetPathStr, Session);
			if (!PresetMHC) return sol::lua_nil;

			if (!EnsureMHCharacterRegistered(Subsystem, PresetMHC, Session))
				return sol::lua_nil;

			PresetStates.Add(Subsystem->CopyBodyState(PresetMHC));
		}

		for (const auto& Pair : WeightsTable)
		{
			if (Pair.second.is<double>())
				Weights.Add(static_cast<float>(Pair.second.as<double>()));
		}

		if (PresetStates.Num() == 0 || Weights.Num() == 0)
		{
			Session.Log(TEXT("[FAIL] metahuman_blend_body_region -> no valid presets or weights"));
			return sol::lua_nil;
		}

		FScopedTransaction Tx(FText::FromString(TEXT("Blend MetaHuman Body Region")));
		MHC->Modify();

		TArray<FVector3f> UpdatedGizmos = Subsystem->BlendBodyRegion(MHC, RegionIndex, BlendOpts, StartState, PresetStates, Weights);

		MHC->MarkPackageDirty();

		sol::table Result = LuaView.create_table();
		for (int32 i = 0; i < UpdatedGizmos.Num(); ++i)
		{
			sol::table Entry = LuaView.create_table();
			Entry["x"] = UpdatedGizmos[i].X;
			Entry["y"] = UpdatedGizmos[i].Y;
			Entry["z"] = UpdatedGizmos[i].Z;
			Result[i + 1] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] metahuman_blend_body_region -> region %d, %d presets, %d gizmos"),
			RegionIndex, PresetStates.Num(), UpdatedGizmos.Num()));
		return Result;
	});

	// ================================================================
	// metahuman_fit_face_to_vertices(asset, opts)
	// ================================================================
	Lua.set_function("metahuman_fit_face_to_vertices", [&Session](const std::string& AssetPath,
		sol::table Opts, sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return false;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_fit_face_to_vertices -> subsystem not available"));
			return false;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return false;

		FMetaHumanCharacterFitToVerticesParams Params;

		// Parse alignment options
		sol::optional<std::string> AlignOpt = Opts.get<sol::optional<std::string>>("alignment");
		if (AlignOpt.has_value())
		{
			FString AlignStr = UTF8_TO_TCHAR(AlignOpt.value().c_str());
			if (AlignStr.Equals(TEXT("None"), ESearchCase::IgnoreCase))
				Params.Options.AlignmentOptions = EAlignmentOptions::None;
			else if (AlignStr.Equals(TEXT("Translation"), ESearchCase::IgnoreCase))
				Params.Options.AlignmentOptions = EAlignmentOptions::Translation;
			else if (AlignStr.Equals(TEXT("RotationTranslation"), ESearchCase::IgnoreCase))
				Params.Options.AlignmentOptions = EAlignmentOptions::RotationTranslation;
			else if (AlignStr.Equals(TEXT("ScalingTranslation"), ESearchCase::IgnoreCase))
				Params.Options.AlignmentOptions = EAlignmentOptions::ScalingTranslation;
		}
		if (Opts["disable_hf_delta"].valid())
			Params.Options.bDisableHighFrequencyDelta = Opts.get_or("disable_hf_delta", true);

		// Parse vertex arrays
		auto ParseVertices = [](const sol::table& T, TArray<FVector>& OutVerts)
		{
			for (const auto& Pair : T)
			{
				if (!Pair.second.is<sol::table>()) continue;
				sol::table V = Pair.second.as<sol::table>();
				OutVerts.Add(FVector(V.get_or("x", 0.0), V.get_or("y", 0.0), V.get_or("z", 0.0)));
			}
		};

		sol::optional<sol::table> HeadOpt = Opts.get<sol::optional<sol::table>>("head_vertices");
		if (HeadOpt.has_value()) ParseVertices(HeadOpt.value(), Params.HeadVertices);

		if (Params.HeadVertices.Num() == 0)
		{
			Session.Log(TEXT("[FAIL] metahuman_fit_face_to_vertices -> 'head_vertices' is required"));
			return false;
		}

		sol::optional<sol::table> LeftEyeOpt = Opts.get<sol::optional<sol::table>>("left_eye_vertices");
		if (LeftEyeOpt.has_value()) ParseVertices(LeftEyeOpt.value(), Params.LeftEyeVertices);

		sol::optional<sol::table> RightEyeOpt = Opts.get<sol::optional<sol::table>>("right_eye_vertices");
		if (RightEyeOpt.has_value()) ParseVertices(RightEyeOpt.value(), Params.RightEyeVertices);

		sol::optional<sol::table> TeethOpt = Opts.get<sol::optional<sol::table>>("teeth_vertices");
		if (TeethOpt.has_value()) ParseVertices(TeethOpt.value(), Params.TeethVertices);

		FScopedTransaction Tx(FText::FromString(TEXT("Fit MetaHuman Face To Vertices")));
		MHC->Modify();

		bool bSuccess = Subsystem->FitStateToTargetVertices(MHC, Params);

		if (bSuccess)
		{
			MHC->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] metahuman_fit_face_to_vertices -> fitted with %d head verts"),
				Params.HeadVertices.Num()));
		}
		else
		{
			Session.Log(TEXT("[FAIL] metahuman_fit_face_to_vertices -> fitting failed"));
		}
		return bSuccess;
	});

#if AIK_HAS_METAHUMAN_IDENTITY
	// ================================================================
	// metahuman_import_from_identity(asset, identity_path, opts?)
	// ================================================================
	Lua.set_function("metahuman_import_from_identity", [&Session](const std::string& AssetPath,
		const std::string& IdentityPath, sol::optional<sol::table> OptsOpt,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return sol::lua_nil;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_import_from_identity -> subsystem not available"));
			return sol::lua_nil;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return sol::lua_nil;

		FString FIdPath = UTF8_TO_TCHAR(IdentityPath.c_str());
		UObject* IdentityObj = StaticLoadObject(UObject::StaticClass(), nullptr, *FIdPath);
		UMetaHumanIdentity* Identity = Cast<UMetaHumanIdentity>(IdentityObj);
		if (!Identity)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] metahuman_import_from_identity -> could not load UMetaHumanIdentity: %s"), *FIdPath));
			return sol::lua_nil;
		}

		FImportFromIdentityParams ImportParams;
		if (OptsOpt.has_value())
		{
			sol::table Opts = OptsOpt.value();
			if (Opts["use_eyes"].valid()) ImportParams.bUseEyeMeshes = Opts.get_or("use_eyes", true);
			if (Opts["use_teeth"].valid()) ImportParams.bUseTeethMesh = Opts.get_or("use_teeth", true);
			if (Opts["use_metric_scale"].valid()) ImportParams.bUseMetricScale = Opts.get_or("use_metric_scale", false);
		}

		FScopedTransaction Tx(FText::FromString(TEXT("Import MetaHuman From Identity")));
		MHC->Modify();

		EImportErrorCode Result = Subsystem->ImportFromIdentity(MHC, Identity, ImportParams);

		const char* ResultStr = "Unknown";
		switch (Result)
		{
		case EImportErrorCode::Success: ResultStr = "Success"; break;
		case EImportErrorCode::FittingError: ResultStr = "FittingError"; break;
		case EImportErrorCode::InvalidInputData: ResultStr = "InvalidInputData"; break;
		case EImportErrorCode::InvalidInputBones: ResultStr = "InvalidInputBones"; break;
		case EImportErrorCode::InvalidHeadMesh: ResultStr = "InvalidHeadMesh"; break;
		case EImportErrorCode::IdentityNotConformed: ResultStr = "IdentityNotConformed"; break;
		case EImportErrorCode::GeneralError: ResultStr = "GeneralError"; break;
		default: ResultStr = "Error"; break;
		}

		if (Result == EImportErrorCode::Success)
		{
			MHC->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] metahuman_import_from_identity -> imported from %s"),
				*FPaths::GetCleanFilename(FIdPath)));
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] metahuman_import_from_identity -> %s"), UTF8_TO_TCHAR(ResultStr)));
		}

		return sol::make_object(LuaView, std::string(ResultStr));
	});
#endif // AIK_HAS_METAHUMAN_IDENTITY

	// ================================================================
	// metahuman_create_combined_mesh(asset, output_path)
	// ================================================================
	Lua.set_function("metahuman_create_combined_mesh", [&Session](const std::string& AssetPath,
		const std::string& OutputPath, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return sol::lua_nil;

		UMetaHumanCharacterEditorSubsystem* Subsystem = GetMHEditorSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] metahuman_create_combined_mesh -> subsystem not available"));
			return sol::lua_nil;
		}

		if (!EnsureMHCharacterRegistered(Subsystem, MHC, Session))
			return sol::lua_nil;

		FString FOutputPath = UTF8_TO_TCHAR(OutputPath.c_str());

		USkeletalMesh* CombinedMesh = Subsystem->CreateCombinedFaceAndBodyMesh(MHC, FOutputPath);
		if (!CombinedMesh)
		{
			Session.Log(TEXT("[FAIL] metahuman_create_combined_mesh -> creation failed (requires face+body DNA)"));
			return sol::lua_nil;
		}

		FString MeshPath = CombinedMesh->GetPathName();
		Session.Log(FString::Printf(TEXT("[OK] metahuman_create_combined_mesh -> created at %s"), *MeshPath));
		return sol::make_object(LuaView, std::string(TCHAR_TO_UTF8(*MeshPath)));
	});

#endif // ENGINE_MINOR_VERSION >= 7

#if AIK_HAS_METAHUMAN_WARDROBE
	// ================================================================
	// metahuman_list_wardrobe(asset)
	// ================================================================
	Lua.set_function("metahuman_list_wardrobe", [&Session](const std::string& AssetPath,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return sol::lua_nil;

		sol::table Result = LuaView.create_table();
		int32 TotalItems = 0;

#if WITH_EDITORONLY_DATA
		// Iterate wardrobe individual assets (per-slot map)
		for (const auto& SlotPair : MHC->WardrobeIndividualAssets)
		{
			FString SlotName = SlotPair.Key.ToString();
			const FMetaHumanCharacterWardrobeIndividualAssets& SlotItems = SlotPair.Value;

			sol::table SlotTable = LuaView.create_table();
			int32 Idx = 1;
			for (const TSoftObjectPtr<UMetaHumanWardrobeItem>& ItemPtr : SlotItems.Items)
			{
				sol::table ItemEntry = LuaView.create_table();
				ItemEntry["path"] = TCHAR_TO_UTF8(*ItemPtr.ToString());
				ItemEntry["is_loaded"] = ItemPtr.IsValid();

				if (UMetaHumanWardrobeItem* Item = ItemPtr.Get())
				{
					ItemEntry["name"] = TCHAR_TO_UTF8(*Item->GetName());
				}

				SlotTable[Idx++] = ItemEntry;
				TotalItems++;
			}
			Result[TCHAR_TO_UTF8(*SlotName)] = SlotTable;
		}

		// Also list monitored wardrobe paths
		sol::table PathsTable = LuaView.create_table();
		int32 PathIdx = 1;
		for (const FMetaHumanCharacterAssetsSection& Section : MHC->WardrobePaths)
		{
			sol::table PathEntry = LuaView.create_table();
			PathEntry["directory"] = TCHAR_TO_UTF8(*Section.ContentDirectoryToMonitor.Path);
			PathEntry["slot"] = TCHAR_TO_UTF8(*Section.SlotName.ToString());
			PathsTable[PathIdx++] = PathEntry;
		}
		Result["_monitored_paths"] = PathsTable;
#endif

		Session.Log(FString::Printf(TEXT("[OK] metahuman_list_wardrobe -> %d items"), TotalItems));
		return Result;
	});

	// ================================================================
	// metahuman_add_wardrobe_item(asset, slot_name, wardrobe_item_path)
	// ================================================================
	Lua.set_function("metahuman_add_wardrobe_item", [&Session](const std::string& AssetPath,
		const std::string& SlotNameStr, const std::string& WardrobeItemPath,
		sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return false;

		FString FWIPath = UTF8_TO_TCHAR(WardrobeItemPath.c_str());
		UMetaHumanWardrobeItem* WardrobeItem = Cast<UMetaHumanWardrobeItem>(
			StaticLoadObject(UMetaHumanWardrobeItem::StaticClass(), nullptr, *FWIPath));
		if (!WardrobeItem)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] metahuman_add_wardrobe_item -> wardrobe item not found: %s"), *FWIPath));
			return false;
		}

		FName SlotName = FName(UTF8_TO_TCHAR(SlotNameStr.c_str()));

#if WITH_EDITORONLY_DATA
		FScopedTransaction Tx(FText::FromString(TEXT("Add MetaHuman Wardrobe Item")));
		MHC->Modify();

		FMetaHumanCharacterWardrobeIndividualAssets& SlotItems = MHC->WardrobeIndividualAssets.FindOrAdd(SlotName);
		SlotItems.Items.Add(TSoftObjectPtr<UMetaHumanWardrobeItem>(WardrobeItem));

		MHC->MarkPackageDirty();
		Session.Log(FString::Printf(TEXT("[OK] metahuman_add_wardrobe_item -> added '%s' to slot '%s'"),
			*WardrobeItem->GetName(), UTF8_TO_TCHAR(SlotNameStr.c_str())));
		return true;
#else
		Session.Log(TEXT("[FAIL] metahuman_add_wardrobe_item -> editor-only feature"));
		return false;
#endif
	});

	// ================================================================
	// metahuman_remove_wardrobe_item(asset, slot_name, item_index)
	// ================================================================
	Lua.set_function("metahuman_remove_wardrobe_item", [&Session](const std::string& AssetPath,
		const std::string& SlotNameStr, int ItemIndex,
		sol::this_state S) -> bool
	{
		UMetaHumanCharacter* MHC = LoadMHCharacter(AssetPath, Session);
		if (!MHC) return false;

		FName SlotName = FName(UTF8_TO_TCHAR(SlotNameStr.c_str()));

#if WITH_EDITORONLY_DATA
		FMetaHumanCharacterWardrobeIndividualAssets* SlotItems = MHC->WardrobeIndividualAssets.Find(SlotName);
		if (!SlotItems)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] metahuman_remove_wardrobe_item -> slot '%s' not found"),
				UTF8_TO_TCHAR(SlotNameStr.c_str())));
			return false;
		}

		int32 ZeroIndex = ItemIndex - 1; // Lua 1-based to C++ 0-based
		if (ZeroIndex < 0 || ZeroIndex >= SlotItems->Items.Num())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] metahuman_remove_wardrobe_item -> index %d out of range (1-%d)"),
				ItemIndex, SlotItems->Items.Num()));
			return false;
		}

		FScopedTransaction Tx(FText::FromString(TEXT("Remove MetaHuman Wardrobe Item")));
		MHC->Modify();

		SlotItems->Items.RemoveAt(ZeroIndex);
		MHC->MarkPackageDirty();

		Session.Log(FString::Printf(TEXT("[OK] metahuman_remove_wardrobe_item -> removed index %d from slot '%s'"),
			ItemIndex, UTF8_TO_TCHAR(SlotNameStr.c_str())));
		return true;
#else
		Session.Log(TEXT("[FAIL] metahuman_remove_wardrobe_item -> editor-only feature"));
		return false;
#endif
	});
#endif // AIK_HAS_METAHUMAN_WARDROBE
}

REGISTER_LUA_BINDING(MetaHuman, MetaHumanDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindMetaHuman(Lua, Session);
});

#endif // !AIK_METAHUMAN_DISABLED

