// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/MaterialLayersFunctions.h"
#include "Materials/MaterialFunctionInterface.h"
#include "StaticParameterSet.h"
#include "Engine/Texture.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
#include "Engine/TextureCollection.h"
#endif
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Engine/Font.h"
#include "Engine/SubsurfaceProfile.h"
#include "Engine/SpecularProfile.h"
#include "VT/RuntimeVirtualTexture.h"
#include "SparseVolumeTexture/SparseVolumeTexture.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialOverrideNanite.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#include "Materials/MaterialParameters.h"
#else
#include "MaterialShared.h"
#endif

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// Helpers
// ============================================================================

/** PostEditChangeProperty + MarkPackageDirty for MI parameter changes */
static void FinalizeMaterialInstance(UMaterialInstance* MI, FProperty* ChangedProp = nullptr)
{
	if (ChangedProp)
	{
		FPropertyChangedEvent Event(ChangedProp, EPropertyChangeType::ValueSet);
		MI->PostEditChangeProperty(Event);
	}
	MI->MarkPackageDirty();
}

/** Find property by name on UMaterialInstance for PostEditChangeProperty */
static FProperty* FindMIProperty(const TCHAR* PropName)
{
	return UMaterialInstance::StaticClass()->FindPropertyByName(PropName);
}

/** Get blend mode as string */
static const char* BlendModeToString(EBlendMode Mode)
{
	switch (Mode)
	{
	case BLEND_Opaque:                         return "Opaque";
	case BLEND_Masked:                         return "Masked";
	case BLEND_Translucent:                    return "Translucent";
	case BLEND_Additive:                       return "Additive";
	case BLEND_Modulate:                       return "Modulate";
	case BLEND_AlphaComposite:                 return "AlphaComposite";
	case BLEND_AlphaHoldout:                   return "AlphaHoldout";
	case BLEND_TranslucentColoredTransmittance: return "TranslucentColoredTransmittance";
	default:                                   return "Unknown";
	}
}

/** Parse blend mode from string */
static bool ParseBlendMode(const std::string& Str, EBlendMode& OutMode)
{
	FString S = UTF8_TO_TCHAR(Str.c_str());
	if (S.Equals(TEXT("Opaque"), ESearchCase::IgnoreCase))                         { OutMode = BLEND_Opaque; return true; }
	if (S.Equals(TEXT("Masked"), ESearchCase::IgnoreCase))                         { OutMode = BLEND_Masked; return true; }
	if (S.Equals(TEXT("Translucent"), ESearchCase::IgnoreCase))                    { OutMode = BLEND_Translucent; return true; }
	if (S.Equals(TEXT("Additive"), ESearchCase::IgnoreCase))                       { OutMode = BLEND_Additive; return true; }
	if (S.Equals(TEXT("Modulate"), ESearchCase::IgnoreCase))                       { OutMode = BLEND_Modulate; return true; }
	if (S.Equals(TEXT("AlphaComposite"), ESearchCase::IgnoreCase))                 { OutMode = BLEND_AlphaComposite; return true; }
	if (S.Equals(TEXT("AlphaHoldout"), ESearchCase::IgnoreCase))                   { OutMode = BLEND_AlphaHoldout; return true; }
	if (S.Equals(TEXT("TranslucentColoredTransmittance"), ESearchCase::IgnoreCase)){ OutMode = BLEND_TranslucentColoredTransmittance; return true; }
	return false;
}

static int32 GetStaticMaskCount(const FStaticParameterSet& StaticParams)
{
#if WITH_EDITORONLY_DATA
	return StaticParams.EditorOnly.StaticComponentMaskParameters.Num();
#else
	(void)StaticParams;
	return 0;
#endif
}

static void AddStaticMaskEntries(sol::table& Result, const FStaticParameterSet& StaticParams, sol::state_view& Lua)
{
#if WITH_EDITORONLY_DATA
	sol::table Arr = Lua.create_table();
	for (int32 i = 0; i < StaticParams.EditorOnly.StaticComponentMaskParameters.Num(); i++)
	{
		const FStaticComponentMaskParameter& P = StaticParams.EditorOnly.StaticComponentMaskParameters[i];
		sol::table E = Lua.create_table();
		E["name"] = TCHAR_TO_UTF8(*P.ParameterInfo.Name.ToString());
		E["r"] = P.R;
		E["g"] = P.G;
		E["b"] = P.B;
		E["a"] = P.A;
		E["overridden"] = P.bOverride;
		Arr[i + 1] = E;
	}
	Result["static_masks"] = Arr;
#else
	(void)Result;
	(void)StaticParams;
	(void)Lua;
#endif
}

static void AddLayerEditorData(sol::table& Entry, const FMaterialLayersFunctions& LayerFunctions, int32 Index)
{
#if WITH_EDITORONLY_DATA
	if (Index < LayerFunctions.EditorOnly.LayerStates.Num())
		Entry["enabled"] = LayerFunctions.EditorOnly.LayerStates[Index];
	if (Index < LayerFunctions.EditorOnly.LayerNames.Num())
		Entry["name"] = TCHAR_TO_UTF8(*LayerFunctions.EditorOnly.LayerNames[Index].ToString());
	if (Index < LayerFunctions.EditorOnly.LayerLinkStates.Num())
		Entry["linked_to_parent"] = (LayerFunctions.EditorOnly.LayerLinkStates[Index] == EMaterialLayerLinkState::LinkedToParent);
#else
	(void)Entry;
	(void)LayerFunctions;
	(void)Index;
#endif
}

static bool SupportsStaticMasks()
{
#if WITH_EDITORONLY_DATA
	return true;
#else
	return false;
#endif
}

static bool SupportsLayerEditing()
{
#if WITH_EDITOR
	return true;
#else
	return false;
#endif
}

static bool ApplyStaticMaskParameter(FStaticParameterSet& StaticParams, FName ParamName, bool bR, bool bG, bool bB, bool bA)
{
#if WITH_EDITORONLY_DATA
	for (FStaticComponentMaskParameter& Mask : StaticParams.EditorOnly.StaticComponentMaskParameters)
	{
		if (Mask.ParameterInfo.Name == ParamName)
		{
			Mask.bOverride = true;
			Mask.R = bR;
			Mask.G = bG;
			Mask.B = bB;
			Mask.A = bA;
			return true;
		}
	}

	FStaticComponentMaskParameter NewMask;
	NewMask.ParameterInfo.Name = ParamName;
	NewMask.bOverride = true;
	NewMask.R = bR;
	NewMask.G = bG;
	NewMask.B = bB;
	NewMask.A = bA;
	StaticParams.EditorOnly.StaticComponentMaskParameters.Add(NewMask);
	return true;
#else
	(void)StaticParams;
	(void)ParamName;
	(void)bR;
	(void)bG;
	(void)bB;
	(void)bA;
	return false;
#endif
}

static bool ClearStaticMaskOverride(FStaticParameterSet& StaticParams, FName ParamName)
{
#if WITH_EDITORONLY_DATA
	for (int32 i = 0; i < StaticParams.EditorOnly.StaticComponentMaskParameters.Num(); i++)
	{
		if (StaticParams.EditorOnly.StaticComponentMaskParameters[i].ParameterInfo.Name == ParamName)
		{
			StaticParams.EditorOnly.StaticComponentMaskParameters[i].bOverride = false;
			return true;
		}
	}
	return false;
#else
	(void)StaticParams;
	(void)ParamName;
	return false;
#endif
}

/** Convert EMaterialParameterAssociation to string */
static const char* AssociationToString(EMaterialParameterAssociation Assoc)
{
	switch (Assoc)
	{
	case EMaterialParameterAssociation::LayerParameter:  return "layer";
	case EMaterialParameterAssociation::BlendParameter:  return "blend";
	case EMaterialParameterAssociation::GlobalParameter: return "global";
	default: return "global";
	}
}

/** Parse association from string */
static EMaterialParameterAssociation ParseAssociation(const std::string& Str)
{
	FString S = UTF8_TO_TCHAR(Str.c_str());
	if (S.Equals(TEXT("layer"), ESearchCase::IgnoreCase)) return EMaterialParameterAssociation::LayerParameter;
	if (S.Equals(TEXT("blend"), ESearchCase::IgnoreCase)) return EMaterialParameterAssociation::BlendParameter;
	return EMaterialParameterAssociation::GlobalParameter;
}

/** Add association/index fields to a parameter listing entry */
static void AddAssociationInfo(sol::table& Entry, const FMaterialParameterInfo& Info)
{
	Entry["association"] = AssociationToString(Info.Association);
	if (Info.Index != INDEX_NONE)
		Entry["layer_index"] = Info.Index;
}

/** Build FMaterialParameterInfo from Lua table, reading name + optional association/layer_index */
static FMaterialParameterInfo BuildParameterInfo(FName ParamName, const sol::table& P)
{
	std::string AssocStr = P.get_or("association", std::string());
	EMaterialParameterAssociation Assoc = AssocStr.empty()
		? EMaterialParameterAssociation::GlobalParameter
		: ParseAssociation(AssocStr);
	int32 LayerIndex = static_cast<int32>(P.get_or("layer_index", -1));
	return FMaterialParameterInfo(ParamName, Assoc, LayerIndex);
}

static void SetLayerNameIfSupported(FMaterialLayersFunctions& LayerFunctions, int32 Index, const sol::optional<std::string>& NameOpt)
{
#if WITH_EDITORONLY_DATA
	if (NameOpt.has_value() && Index < LayerFunctions.EditorOnly.LayerNames.Num())
	{
		LayerFunctions.EditorOnly.LayerNames[Index] = FText::FromString(UTF8_TO_TCHAR(NameOpt.value().c_str()));
	}
#else
	(void)LayerFunctions;
	(void)Index;
	(void)NameOpt;
#endif
}

// ============================================================================
// Binding
// ============================================================================

static TArray<FLuaFunctionDoc> MaterialInstanceDocs = {};

static void BindMaterialInstance(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_material_instance", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UMaterialInstance* MI = LoadObject<UMaterialInstance>(nullptr, *FPath);
		if (!MI) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"Material Instance — element types for list/configure/remove:\n"
			"\n"
			"info() — summary: parent chain, parameter counts, base property overrides, profiles, nanite override\n"
			"\n"
			"list(type):\n"
			"  list(\"parameters\")   — all parameter overrides grouped by type (incl. association/layer_index)\n"
			"  list(\"parent_chain\") — walk parent chain to base material\n"
			"\n"
			"configure(type, params):\n"
			"  configure(\"scalar\", {name=\"Metallic\", value=0.8})\n"
			"  configure(\"vector\", {name=\"BaseColor\", r=1, g=0.5, b=0.2, a=1})\n"
			"  configure(\"texture\", {name=\"DiffuseMap\", path=\"/Game/Textures/T_Diff\"})\n"
			"  configure(\"texture_collection\", {name=\"MyTC\", path=\"/Game/TC/MyCollection\"})\n"
			"  configure(\"parameter_collection\", {name=\"MyPC\", path=\"/Game/MPC/MyCollection\"})\n"
			"  configure(\"double_vector\", {name=\"LWCParam\", x=0, y=0, z=0, w=1})\n"
			"  configure(\"font\", {name=\"HUDFont\", font=\"/Game/Fonts/MyFont\", page=0})\n"
			"  configure(\"runtime_virtual_texture\", {name=\"RVTParam\", texture=\"/Game/VT/MyRVT\"})\n"
			"  configure(\"sparse_volume_texture\", {name=\"SVTParam\", texture=\"/Game/VT/MySVT\"})\n"
			"  configure(\"static_switch\", {name=\"UseNormal\", value=true})\n"
			"  configure(\"static_mask\", {name=\"ChannelMask\", r=true, g=false, b=false, a=false})\n"
			"  configure(\"base_property\", {blend_mode=\"Masked\", two_sided=true, opacity_mask_clip=0.333,\n"
			"    has_pixel_animation=false, displacement_scaling={magnitude=4, center=0.5},\n"
			"    enable_displacement_fade=true, displacement_fade_range={start_size_pixels=4, end_size_pixels=1},\n"
			"    max_world_position_offset=0, compatible_with_lumen_card_sharing=false})\n"
			"  configure(\"subsurface_profile\", {path=\"/Game/Profiles/SSP\"}) — or {path=\"none\"} to clear\n"
			"  configure(\"specular_profile\", {path=\"/Game/Profiles/SP\"}) — or {path=\"none\"} to clear\n"
			"  configure(\"nanite_override\", {path=\"/Game/Mat/NaniteMat\", enabled=true}) — or {path=\"none\"}\n"
			"  configure(\"user_scene_texture\", {key=\"Name\", value=\"OverrideName\"}) — post-process only\n"
			"  configure(\"phys_material_map\", {index=N, path=\"/Game/PhysMat\"}) — mask color index 0-7\n"
			"\n"
			"  Blend modes: Opaque, Masked, Translucent, Additive, Modulate, AlphaComposite, AlphaHoldout, TranslucentColoredTransmittance\n"
			"\n"
			"  Layer association (optional on all param types): association=\"layer\"|\"blend\"|\"global\", layer_index=N\n"
			"    e.g. configure(\"scalar\", {name=\"Metallic\", value=0.8, association=\"layer\", layer_index=1})\n"
			"\n"
			"remove(\"parameter\", {type=\"scalar\", name=\"Metallic\"})\n"
			"  Valid types: scalar, vector, double_vector, texture, texture_collection, parameter_collection, font, rvt, svt, static_switch, static_mask\n"
			"remove(\"user_scene_texture\", {key=\"Name\"})\n"
			"\n"
			"list(\"layers\"):\n"
			"  Returns array of {index, layer_function, blend_function, name, enabled, linked_to_parent}.\n"
			"\n"
			"add(\"layer\", {function=\"/Path/To/LayerFunc\", blend=\"/Path/To/BlendFunc\", name=\"LayerName\"}):\n"
			"  Appends a new blended layer. Also accepts function_path/layer_function and blend_path/blend_function.\n"
			"\n"
			"remove(\"layer\", {index=N}):\n"
			"  Removes layer at index (0-based). Cannot remove background layer (index 0).\n"
			"\n"
			"configure(\"layer\", {index=N, function=\"/Path\", enabled=true/false, name=\"NewName\"}):\n"
			"  Modify layer function, visibility, or name.\n"
			"\n"
			"configure(\"blend\", {index=N, function=\"/Path\"}):\n"
			"  Set blend function for a layer.\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [MI, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(MI))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();
			Result["type"] = TCHAR_TO_UTF8(*MI->GetClass()->GetName());
			Result["path"] = TCHAR_TO_UTF8(*MI->GetPathName());

			// Parent
			if (MI->Parent)
			{
				sol::table ParentT = Lua.create_table();
				ParentT["name"] = TCHAR_TO_UTF8(*MI->Parent->GetName());
				ParentT["path"] = TCHAR_TO_UTF8(*MI->Parent->GetPathName());
				Result["parent"] = ParentT;
			}

			// Base material (root of parent chain)
			UMaterial* BaseMat = MI->GetMaterial();
			if (BaseMat)
			{
				sol::table BaseT = Lua.create_table();
				BaseT["name"] = TCHAR_TO_UTF8(*BaseMat->GetName());
				BaseT["path"] = TCHAR_TO_UTF8(*BaseMat->GetPathName());
				Result["base_material"] = BaseT;
			}

			// Parameter counts
			Result["scalar_params"] = static_cast<int>(MI->ScalarParameterValues.Num());
			Result["vector_params"] = static_cast<int>(MI->VectorParameterValues.Num());
			Result["texture_params"] = static_cast<int>(MI->TextureParameterValues.Num());
			Result["font_params"] = static_cast<int>(MI->FontParameterValues.Num());
			Result["double_vector_params"] = static_cast<int>(MI->DoubleVectorParameterValues.Num());
			Result["rvt_params"] = static_cast<int>(MI->RuntimeVirtualTextureParameterValues.Num());
			Result["svt_params"] = static_cast<int>(MI->SparseVolumeTextureParameterValues.Num());
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			Result["texture_collection_params"] = static_cast<int>(MI->TextureCollectionParameterValues.Num());
#endif // ENGINE_MINOR_VERSION >= 5

			// Static parameters
			FStaticParameterSet StaticParams = MI->GetStaticParameters();
			Result["static_switch_params"] = static_cast<int>(StaticParams.StaticSwitchParameters.Num());
			Result["static_mask_params"] = GetStaticMaskCount(StaticParams);

			// Base property overrides
			const FMaterialInstanceBasePropertyOverrides& BaseProps = MI->BasePropertyOverrides;
			sol::table OverridesT = Lua.create_table();
			if (BaseProps.bOverride_BlendMode)
				OverridesT["blend_mode"] = BlendModeToString(BaseProps.BlendMode);
			if (BaseProps.bOverride_TwoSided)
				OverridesT["two_sided"] = static_cast<bool>(BaseProps.TwoSided);
			if (BaseProps.bOverride_OpacityMaskClipValue)
				OverridesT["opacity_mask_clip"] = BaseProps.OpacityMaskClipValue;
			if (BaseProps.bOverride_ShadingModel)
				OverridesT["shading_model"] = static_cast<int>(BaseProps.ShadingModel);
			if (BaseProps.bOverride_DitheredLODTransition)
				OverridesT["dithered_lod_transition"] = static_cast<bool>(BaseProps.DitheredLODTransition);
			if (BaseProps.bOverride_CastDynamicShadowAsMasked)
				OverridesT["cast_shadow_as_masked"] = static_cast<bool>(BaseProps.bCastDynamicShadowAsMasked);
			if (BaseProps.bOverride_bIsThinSurface)
				OverridesT["thin_surface"] = static_cast<bool>(BaseProps.bIsThinSurface);
			if (BaseProps.bOverride_OutputTranslucentVelocity)
				OverridesT["output_translucent_velocity"] = static_cast<bool>(BaseProps.bOutputTranslucentVelocity);
			if (BaseProps.bOverride_bEnableTessellation)
				OverridesT["enable_tessellation"] = static_cast<bool>(BaseProps.bEnableTessellation);
			if (BaseProps.bOverride_bHasPixelAnimation)
				OverridesT["has_pixel_animation"] = static_cast<bool>(BaseProps.bHasPixelAnimation);
			if (BaseProps.bOverride_DisplacementScaling)
			{
				sol::table DS = Lua.create_table();
				DS["magnitude"] = BaseProps.DisplacementScaling.Magnitude;
				DS["center"] = BaseProps.DisplacementScaling.Center;
				OverridesT["displacement_scaling"] = DS;
			}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			if (BaseProps.bOverride_bEnableDisplacementFade)
				OverridesT["enable_displacement_fade"] = static_cast<bool>(BaseProps.bEnableDisplacementFade);
			if (BaseProps.bOverride_DisplacementFadeRange)
			{
				sol::table DFR = Lua.create_table();
				DFR["start_size_pixels"] = BaseProps.DisplacementFadeRange.StartSizePixels;
				DFR["end_size_pixels"] = BaseProps.DisplacementFadeRange.EndSizePixels;
				OverridesT["displacement_fade_range"] = DFR;
			}
#endif
			if (BaseProps.bOverride_MaxWorldPositionOffsetDisplacement)
				OverridesT["max_world_position_offset"] = BaseProps.MaxWorldPositionOffsetDisplacement;
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			if (BaseProps.bOverride_CompatibleWithLumenCardSharing)
				OverridesT["compatible_with_lumen_card_sharing"] = static_cast<bool>(BaseProps.bCompatibleWithLumenCardSharing);
#endif
			Result["base_property_overrides"] = OverridesT;

			// Physical material
			if (MI->PhysMaterial)
				Result["phys_material"] = TCHAR_TO_UTF8(*MI->PhysMaterial->GetPathName());

			// Physical material map
			{
				sol::table PhysMapT = Lua.create_table();
				bool bHasAny = false;
				for (int32 i = 0; i < EPhysicalMaterialMaskColor::MAX; i++)
				{
					if (MI->PhysicalMaterialMap[i])
					{
						PhysMapT[i] = TCHAR_TO_UTF8(*MI->PhysicalMaterialMap[i]->GetPathName());
						bHasAny = true;
					}
				}
				if (bHasAny)
					Result["phys_material_map"] = PhysMapT;
			}

			// Nanite override material
#if WITH_EDITORONLY_DATA
			{
				UMaterialInterface* NaniteOverride = MI->NaniteOverrideMaterial.GetOverrideMaterial();
				if (NaniteOverride)
				{
					sol::table NaniteT = Lua.create_table();
					NaniteT["path"] = TCHAR_TO_UTF8(*NaniteOverride->GetPathName());
					NaniteT["enabled"] = MI->NaniteOverrideMaterial.bEnableOverride;
					Result["nanite_override"] = NaniteT;
				}
			}
#endif

			// Subsurface profile override
			Result["override_subsurface_profile"] = static_cast<bool>(MI->bOverrideSubsurfaceProfile);
			if (MI->SubsurfaceProfile)
				Result["subsurface_profile"] = TCHAR_TO_UTF8(*MI->SubsurfaceProfile->GetPathName());

			// Specular profile override
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			Result["override_specular_profile"] = static_cast<bool>(MI->bOverrideSpecularProfile);
			if (MI->SpecularProfileOverride)
				Result["specular_profile"] = TCHAR_TO_UTF8(*MI->SpecularProfileOverride->GetPathName());
#endif

			// User scene texture overrides (post-process materials) — 5.5+
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			if (MI->UserSceneTextureOverrides.Num() > 0)
			{
				sol::table USTArr = Lua.create_table();
				for (int32 i = 0; i < MI->UserSceneTextureOverrides.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["key"] = TCHAR_TO_UTF8(*MI->UserSceneTextureOverrides[i].Key.ToString());
					E["value"] = TCHAR_TO_UTF8(*MI->UserSceneTextureOverrides[i].Value.ToString());
					USTArr[i + 1] = E;
				}
				Result["user_scene_texture_overrides"] = USTArr;
			}
#endif

			// Parameter collection params count
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			Result["parameter_collection_params"] = static_cast<int>(MI->ParameterCollectionParameterValues.Num());
#else
			Result["parameter_collection_params"] = 0;
#endif

			// Material layers
			FMaterialLayersFunctions LayerFunctions;
			if (MI->GetMaterialLayers(LayerFunctions))
			{
				Result["layer_count"] = static_cast<int>(LayerFunctions.Layers.Num());
				Result["has_material_layers"] = true;
			}
			else
			{
				Result["has_material_layers"] = false;
			}

			Session.Log(FString::Printf(TEXT("[OK] info() -> %s, %d scalar, %d vector, %d texture, %d static_switch params"),
				*MI->GetClass()->GetName(),
				MI->ScalarParameterValues.Num(),
				MI->VectorParameterValues.Num(),
				MI->TextureParameterValues.Num(),
				StaticParams.StaticSwitchParameters.Num()));
			return Result;
		});

		// ================================================================
		// list(type)
		// ================================================================
		AssetObj.set_function("list", [MI, &Session](sol::table Self,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(MI))
			{
				Session.Log(TEXT("[FAIL] list -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = Self["info"];
				if (InfoFn.valid()) return InfoFn(Self);
				return sol::lua_nil;
			}

			// ---- list("parameters") ----
			if (FType.Equals(TEXT("parameters"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("params"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();

				// Scalars
				{
					sol::table Arr = Lua.create_table();
					for (int32 i = 0; i < MI->ScalarParameterValues.Num(); i++)
					{
						const FScalarParameterValue& P = MI->ScalarParameterValues[i];
						sol::table E = Lua.create_table();
						E["name"] = TCHAR_TO_UTF8(*P.ParameterInfo.Name.ToString());
						E["value"] = P.ParameterValue;
						AddAssociationInfo(E, P.ParameterInfo);
						Arr[i + 1] = E;
					}
					Result["scalars"] = Arr;
				}

				// Vectors
				{
					sol::table Arr = Lua.create_table();
					for (int32 i = 0; i < MI->VectorParameterValues.Num(); i++)
					{
						const FVectorParameterValue& P = MI->VectorParameterValues[i];
						sol::table E = Lua.create_table();
						E["name"] = TCHAR_TO_UTF8(*P.ParameterInfo.Name.ToString());
						E["r"] = P.ParameterValue.R;
						E["g"] = P.ParameterValue.G;
						E["b"] = P.ParameterValue.B;
						E["a"] = P.ParameterValue.A;
						AddAssociationInfo(E, P.ParameterInfo);
						Arr[i + 1] = E;
					}
					Result["vectors"] = Arr;
				}

				// Textures
				{
					sol::table Arr = Lua.create_table();
					for (int32 i = 0; i < MI->TextureParameterValues.Num(); i++)
					{
						const FTextureParameterValue& P = MI->TextureParameterValues[i];
						sol::table E = Lua.create_table();
						E["name"] = TCHAR_TO_UTF8(*P.ParameterInfo.Name.ToString());
						E["asset_path"] = P.ParameterValue ? TCHAR_TO_UTF8(*P.ParameterValue->GetPathName()) : "None";
						AddAssociationInfo(E, P.ParameterInfo);
						Arr[i + 1] = E;
					}
					Result["textures"] = Arr;
				}

				// Fonts
				{
					sol::table Arr = Lua.create_table();
					for (int32 i = 0; i < MI->FontParameterValues.Num(); i++)
					{
						const FFontParameterValue& P = MI->FontParameterValues[i];
						sol::table E = Lua.create_table();
						E["name"] = TCHAR_TO_UTF8(*P.ParameterInfo.Name.ToString());
						E["font_path"] = P.FontValue ? TCHAR_TO_UTF8(*P.FontValue->GetPathName()) : "None";
						E["page"] = P.FontPage;
						AddAssociationInfo(E, P.ParameterInfo);
						Arr[i + 1] = E;
					}
					Result["fonts"] = Arr;
				}

				// Double vectors
				{
					sol::table Arr = Lua.create_table();
					for (int32 i = 0; i < MI->DoubleVectorParameterValues.Num(); i++)
					{
						const FDoubleVectorParameterValue& P = MI->DoubleVectorParameterValues[i];
						sol::table E = Lua.create_table();
						E["name"] = TCHAR_TO_UTF8(*P.ParameterInfo.Name.ToString());
						E["x"] = P.ParameterValue.X;
						E["y"] = P.ParameterValue.Y;
						E["z"] = P.ParameterValue.Z;
						E["w"] = P.ParameterValue.W;
						AddAssociationInfo(E, P.ParameterInfo);
						Arr[i + 1] = E;
					}
					Result["double_vectors"] = Arr;
				}

				// Runtime Virtual Textures
				{
					sol::table Arr = Lua.create_table();
					for (int32 i = 0; i < MI->RuntimeVirtualTextureParameterValues.Num(); i++)
					{
						const FRuntimeVirtualTextureParameterValue& P = MI->RuntimeVirtualTextureParameterValues[i];
						sol::table E = Lua.create_table();
						E["name"] = TCHAR_TO_UTF8(*P.ParameterInfo.Name.ToString());
						E["asset_path"] = P.ParameterValue ? TCHAR_TO_UTF8(*P.ParameterValue->GetPathName()) : "None";
						AddAssociationInfo(E, P.ParameterInfo);
						Arr[i + 1] = E;
					}
					Result["runtime_virtual_textures"] = Arr;
				}

				// Sparse Volume Textures
				{
					sol::table Arr = Lua.create_table();
					for (int32 i = 0; i < MI->SparseVolumeTextureParameterValues.Num(); i++)
					{
						const FSparseVolumeTextureParameterValue& P = MI->SparseVolumeTextureParameterValues[i];
						sol::table E = Lua.create_table();
						E["name"] = TCHAR_TO_UTF8(*P.ParameterInfo.Name.ToString());
						E["asset_path"] = P.ParameterValue ? TCHAR_TO_UTF8(*P.ParameterValue->GetPathName()) : "None";
						AddAssociationInfo(E, P.ParameterInfo);
						Arr[i + 1] = E;
					}
					Result["sparse_volume_textures"] = Arr;
				}

				// Texture Collections (UE 5.5+)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				{
					sol::table Arr = Lua.create_table();
					for (int32 i = 0; i < MI->TextureCollectionParameterValues.Num(); i++)
					{
						const FTextureCollectionParameterValue& P = MI->TextureCollectionParameterValues[i];
						sol::table E = Lua.create_table();
						E["name"] = TCHAR_TO_UTF8(*P.ParameterInfo.Name.ToString());
						E["asset_path"] = P.ParameterValue ? TCHAR_TO_UTF8(*P.ParameterValue->GetPathName()) : "None";
						AddAssociationInfo(E, P.ParameterInfo);
						Arr[i + 1] = E;
					}
					Result["texture_collections"] = Arr;
				}
#endif // ENGINE_MINOR_VERSION >= 5

				// Parameter Collections (5.7+ only)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				{
					sol::table Arr = Lua.create_table();
					for (int32 i = 0; i < MI->ParameterCollectionParameterValues.Num(); i++)
					{
						const FParameterCollectionParameterValue& P = MI->ParameterCollectionParameterValues[i];
						sol::table E = Lua.create_table();
						E["name"] = TCHAR_TO_UTF8(*P.ParameterInfo.Name.ToString());
						E["asset_path"] = P.ParameterValue ? TCHAR_TO_UTF8(*P.ParameterValue->GetPathName()) : "None";
						AddAssociationInfo(E, P.ParameterInfo);
						Arr[i + 1] = E;
					}
					Result["parameter_collections"] = Arr;
				}
#endif

				// Static switches (from GetStaticParameters — returns by value)
				FStaticParameterSet StaticParams = MI->GetStaticParameters();
				{
					sol::table Arr = Lua.create_table();
					for (int32 i = 0; i < StaticParams.StaticSwitchParameters.Num(); i++)
					{
						const FStaticSwitchParameter& P = StaticParams.StaticSwitchParameters[i];
						sol::table E = Lua.create_table();
						E["name"] = TCHAR_TO_UTF8(*P.ParameterInfo.Name.ToString());
						E["value"] = P.Value;
						E["overridden"] = P.bOverride;
						AddAssociationInfo(E, P.ParameterInfo);
						Arr[i + 1] = E;
					}
					Result["static_switches"] = Arr;
				}

				AddStaticMaskEntries(Result, StaticParams, Lua);

				int32 TotalCount = MI->ScalarParameterValues.Num() + MI->VectorParameterValues.Num()
					+ MI->TextureParameterValues.Num() + MI->FontParameterValues.Num()
					+ StaticParams.StaticSwitchParameters.Num();
				Session.Log(FString::Printf(TEXT("[OK] list(\"parameters\") -> %d total parameter overrides"), TotalCount));
				return Result;
			}

			// ---- list("parent_chain") ----
			if (FType.Equals(TEXT("parent_chain"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				int32 Index = 1;

				UMaterialInterface* Current = MI;
				int32 MaxDepth = 64; // Safety limit
				while (Current && MaxDepth-- > 0)
				{
					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*Current->GetName());
					E["path"] = TCHAR_TO_UTF8(*Current->GetPathName());
					E["class"] = TCHAR_TO_UTF8(*Current->GetClass()->GetName());
					Result[Index++] = E;

					UMaterialInstance* AsMI = Cast<UMaterialInstance>(Current);
					if (AsMI && AsMI->Parent)
					{
						Current = AsMI->Parent;
					}
					else
					{
						break;
					}
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"parent_chain\") -> %d entries"), Index - 1));
				return Result;
			}

			// ---- list("layers") ----
			if (FType.Equals(TEXT("layers"), ESearchCase::IgnoreCase))
			{
				FMaterialLayersFunctions LayerFunctions;
				if (!MI->GetMaterialLayers(LayerFunctions))
				{
					Session.Log(TEXT("[OK] list(\"layers\") -> no material layers"));
					return Lua.create_table(); // empty
				}

				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < LayerFunctions.Layers.Num(); i++)
				{
					sol::table Entry = Lua.create_table();
					Entry["index"] = i;

					// Layer function
					UMaterialFunctionInterface* LayerFunc = LayerFunctions.Layers[i];
					Entry["layer_function"] = LayerFunc ? TCHAR_TO_UTF8(*LayerFunc->GetPathName()) : "";

					// Blend function (Blends array is offset by 1: Blends[0] is for Layers[1])
					if (i > 0 && (i - 1) < LayerFunctions.Blends.Num())
					{
						UMaterialFunctionInterface* BlendFunc = LayerFunctions.Blends[i - 1];
						Entry["blend_function"] = BlendFunc ? TCHAR_TO_UTF8(*BlendFunc->GetPathName()) : "";
					}
					else
					{
						Entry["blend_function"] = ""; // background layer has no blend
					}

					AddLayerEditorData(Entry, LayerFunctions, i);

					Result[i + 1] = Entry;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"layers\") -> %d"), LayerFunctions.Layers.Num()));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: parameters, parent_chain, layers"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// configure(type, params)
		// ================================================================
		AssetObj.set_function("configure", [MI, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(MI))
			{
				Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- configure("scalar", {name=, value=}) ----
			if (FType.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"scalar\") -> table required: {name=.., value=..}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				std::string Name = P.get_or("name", std::string());
				if (Name.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"scalar\") -> 'name' required"));
					return sol::lua_nil;
				}

				sol::optional<double> Value = P.get<sol::optional<double>>("value");
				if (!Value.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"scalar\") -> 'value' required"));
					return sol::lua_nil;
				}

				FName ParamName = FName(UTF8_TO_TCHAR(Name.c_str()));
				float FloatVal = static_cast<float>(Value.value());
				FMaterialParameterInfo ParamInfo = BuildParameterInfo(ParamName, P);

				const FScopedTransaction Transaction(FText::FromString(TEXT("MI: Set Scalar Parameter")));
				MI->Modify();

				// Find existing or add new (match by name + association + index)
				bool bFound = false;
				for (FScalarParameterValue& Existing : MI->ScalarParameterValues)
				{
					if (Existing.ParameterInfo.Name == ParamName
						&& Existing.ParameterInfo.Association == ParamInfo.Association
						&& Existing.ParameterInfo.Index == ParamInfo.Index)
					{
						Existing.ParameterValue = FloatVal;
						bFound = true;
						break;
					}
				}
				if (!bFound)
				{
					FScalarParameterValue NewParam(ParamInfo, FloatVal);
					MI->ScalarParameterValues.Add(NewParam);
				}

				FinalizeMaterialInstance(MI, FindMIProperty(TEXT("ScalarParameterValues")));

				Session.Log(FString::Printf(TEXT("[OK] configure(\"scalar\", name=\"%s\", value=%.4f)"),
					*ParamName.ToString(), FloatVal));
				return sol::make_object(Lua, true);
			}

			// ---- configure("vector", {name=, r=, g=, b=, a=1}) ----
			if (FType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"vector\") -> table required: {name=.., r=, g=, b=, a=}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				std::string Name = P.get_or("name", std::string());
				if (Name.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"vector\") -> 'name' required"));
					return sol::lua_nil;
				}

				float R = static_cast<float>(P.get_or("r", 0.0));
				float G = static_cast<float>(P.get_or("g", 0.0));
				float B = static_cast<float>(P.get_or("b", 0.0));
				float A = static_cast<float>(P.get_or("a", 1.0));

				FName ParamName = FName(UTF8_TO_TCHAR(Name.c_str()));
				FLinearColor Color(R, G, B, A);
				FMaterialParameterInfo ParamInfo = BuildParameterInfo(ParamName, P);

				const FScopedTransaction Transaction(FText::FromString(TEXT("MI: Set Vector Parameter")));
				MI->Modify();

				bool bFound = false;
				for (FVectorParameterValue& Existing : MI->VectorParameterValues)
				{
					if (Existing.ParameterInfo.Name == ParamName
						&& Existing.ParameterInfo.Association == ParamInfo.Association
						&& Existing.ParameterInfo.Index == ParamInfo.Index)
					{
						Existing.ParameterValue = Color;
						bFound = true;
						break;
					}
				}
				if (!bFound)
				{
					FVectorParameterValue NewParam(ParamInfo, Color);
					MI->VectorParameterValues.Add(NewParam);
				}

				FinalizeMaterialInstance(MI, FindMIProperty(TEXT("VectorParameterValues")));

				Session.Log(FString::Printf(TEXT("[OK] configure(\"vector\", name=\"%s\", (%.3f, %.3f, %.3f, %.3f))"),
					*ParamName.ToString(), R, G, B, A));
				return sol::make_object(Lua, true);
			}

			// ---- configure("texture", {name=, path=}) ----
			if (FType.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"texture\") -> table required: {name=.., path=..}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				std::string Name = P.get_or("name", std::string());
				if (Name.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"texture\") -> 'name' required"));
					return sol::lua_nil;
				}

				std::string TexPath = P.get_or("path", std::string());
				if (TexPath.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"texture\") -> 'path' required"));
					return sol::lua_nil;
				}

				FString FTexPath = UTF8_TO_TCHAR(TexPath.c_str());
				UTexture* Texture = NeoStackToolUtils::LoadAssetWithFallback<UTexture>(FTexPath);
				if (!Texture && !FTexPath.StartsWith(TEXT("/")))
				{
					Texture = NeoStackToolUtils::LoadAssetWithFallback<UTexture>(TEXT("/Game/") + FTexPath);
				}
				if (!Texture)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"texture\") -> texture not found: %s"), *FTexPath));
					return sol::lua_nil;
				}

				FName ParamName = FName(UTF8_TO_TCHAR(Name.c_str()));
				FMaterialParameterInfo ParamInfo = BuildParameterInfo(ParamName, P);

				const FScopedTransaction Transaction(FText::FromString(TEXT("MI: Set Texture Parameter")));
				MI->Modify();

				bool bFound = false;
				for (FTextureParameterValue& Existing : MI->TextureParameterValues)
				{
					if (Existing.ParameterInfo.Name == ParamName
						&& Existing.ParameterInfo.Association == ParamInfo.Association
						&& Existing.ParameterInfo.Index == ParamInfo.Index)
					{
						Existing.ParameterValue = Texture;
						bFound = true;
						break;
					}
				}
				if (!bFound)
				{
					FTextureParameterValue NewParam(ParamInfo, Texture);
					MI->TextureParameterValues.Add(NewParam);
				}

				FinalizeMaterialInstance(MI, FindMIProperty(TEXT("TextureParameterValues")));

				Session.Log(FString::Printf(TEXT("[OK] configure(\"texture\", name=\"%s\", path=\"%s\")"),
					*ParamName.ToString(), *Texture->GetPathName()));
				return sol::make_object(Lua, true);
			}

			// ---- configure("double_vector", {name=, x=, y=, z=, w=}) ----
			if (FType.Equals(TEXT("double_vector"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"double_vector\") -> table required: {name=.., x=, y=, z=, w=}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				std::string Name = P.get_or("name", std::string());
				if (Name.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"double_vector\") -> 'name' required"));
					return sol::lua_nil;
				}

				double X = P.get_or("x", 0.0);
				double Y = P.get_or("y", 0.0);
				double Z = P.get_or("z", 0.0);
				double W = P.get_or("w", 1.0);

				FName ParamName = FName(UTF8_TO_TCHAR(Name.c_str()));
				FVector4d Vec(X, Y, Z, W);
				FMaterialParameterInfo ParamInfo = BuildParameterInfo(ParamName, P);

				const FScopedTransaction Transaction(FText::FromString(TEXT("MI: Set Double Vector Parameter")));
				MI->Modify();

				bool bFound = false;
				for (FDoubleVectorParameterValue& Existing : MI->DoubleVectorParameterValues)
				{
					if (Existing.ParameterInfo.Name == ParamName
						&& Existing.ParameterInfo.Association == ParamInfo.Association
						&& Existing.ParameterInfo.Index == ParamInfo.Index)
					{
						Existing.ParameterValue = Vec;
						bFound = true;
						break;
					}
				}
				if (!bFound)
				{
					FDoubleVectorParameterValue NewParam(ParamInfo, Vec);
					MI->DoubleVectorParameterValues.Add(NewParam);
				}

				FinalizeMaterialInstance(MI, FindMIProperty(TEXT("DoubleVectorParameterValues")));

				Session.Log(FString::Printf(TEXT("[OK] configure(\"double_vector\", name=\"%s\", (%.4f, %.4f, %.4f, %.4f))"),
					*ParamName.ToString(), X, Y, Z, W));
				return sol::make_object(Lua, true);
			}

			// ---- configure("font", {name=, font=, page=0}) ----
			if (FType.Equals(TEXT("font"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"font\") -> table required: {name=.., font=.., page=0}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				std::string Name = P.get_or("name", std::string());
				if (Name.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"font\") -> 'name' required"));
					return sol::lua_nil;
				}

				std::string FontPath = P.get_or("font", std::string());
				if (FontPath.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"font\") -> 'font' (path) required"));
					return sol::lua_nil;
				}

				int32 FontPage = static_cast<int32>(P.get_or("page", 0));

				FString FFontPath = UTF8_TO_TCHAR(FontPath.c_str());
				UFont* Font = NeoStackToolUtils::LoadAssetWithFallback<UFont>(FFontPath);
				if (!Font && !FFontPath.StartsWith(TEXT("/")))
				{
					Font = NeoStackToolUtils::LoadAssetWithFallback<UFont>(TEXT("/Game/") + FFontPath);
				}
				if (!Font)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"font\") -> font not found: %s"), *FFontPath));
					return sol::lua_nil;
				}

				FName ParamName = FName(UTF8_TO_TCHAR(Name.c_str()));
				FMaterialParameterInfo ParamInfo = BuildParameterInfo(ParamName, P);

				const FScopedTransaction Transaction(FText::FromString(TEXT("MI: Set Font Parameter")));
				MI->Modify();

				bool bFound = false;
				for (FFontParameterValue& Existing : MI->FontParameterValues)
				{
					if (Existing.ParameterInfo.Name == ParamName
						&& Existing.ParameterInfo.Association == ParamInfo.Association
						&& Existing.ParameterInfo.Index == ParamInfo.Index)
					{
						Existing.FontValue = Font;
						Existing.FontPage = FontPage;
						bFound = true;
						break;
					}
				}
				if (!bFound)
				{
					FFontParameterValue NewParam(ParamInfo, Font, FontPage);
					MI->FontParameterValues.Add(NewParam);
				}

				FinalizeMaterialInstance(MI, FindMIProperty(TEXT("FontParameterValues")));

				Session.Log(FString::Printf(TEXT("[OK] configure(\"font\", name=\"%s\", font=\"%s\", page=%d)"),
					*ParamName.ToString(), *Font->GetPathName(), FontPage));
				return sol::make_object(Lua, true);
			}

			// ---- configure("runtime_virtual_texture", {name=, texture=}) ----
			if (FType.Equals(TEXT("runtime_virtual_texture"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("rvt"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"runtime_virtual_texture\") -> table required: {name=.., texture=..}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				std::string Name = P.get_or("name", std::string());
				if (Name.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"runtime_virtual_texture\") -> 'name' required"));
					return sol::lua_nil;
				}

				std::string TexPath = P.get_or("texture", std::string());
				if (TexPath.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"runtime_virtual_texture\") -> 'texture' (path) required"));
					return sol::lua_nil;
				}

				FString FTexPath = UTF8_TO_TCHAR(TexPath.c_str());
				URuntimeVirtualTexture* RVT = NeoStackToolUtils::LoadAssetWithFallback<URuntimeVirtualTexture>(FTexPath);
				if (!RVT && !FTexPath.StartsWith(TEXT("/")))
				{
					RVT = NeoStackToolUtils::LoadAssetWithFallback<URuntimeVirtualTexture>(TEXT("/Game/") + FTexPath);
				}
				if (!RVT)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"runtime_virtual_texture\") -> asset not found: %s"), *FTexPath));
					return sol::lua_nil;
				}

				FName ParamName = FName(UTF8_TO_TCHAR(Name.c_str()));
				FMaterialParameterInfo ParamInfo = BuildParameterInfo(ParamName, P);

				const FScopedTransaction Transaction(FText::FromString(TEXT("MI: Set Runtime Virtual Texture Parameter")));
				MI->Modify();

				bool bFound = false;
				for (FRuntimeVirtualTextureParameterValue& Existing : MI->RuntimeVirtualTextureParameterValues)
				{
					if (Existing.ParameterInfo.Name == ParamName
						&& Existing.ParameterInfo.Association == ParamInfo.Association
						&& Existing.ParameterInfo.Index == ParamInfo.Index)
					{
						Existing.ParameterValue = RVT;
						bFound = true;
						break;
					}
				}
				if (!bFound)
				{
					FRuntimeVirtualTextureParameterValue NewParam(ParamInfo, RVT);
					MI->RuntimeVirtualTextureParameterValues.Add(NewParam);
				}

				FinalizeMaterialInstance(MI, FindMIProperty(TEXT("RuntimeVirtualTextureParameterValues")));

				Session.Log(FString::Printf(TEXT("[OK] configure(\"runtime_virtual_texture\", name=\"%s\", texture=\"%s\")"),
					*ParamName.ToString(), *RVT->GetPathName()));
				return sol::make_object(Lua, true);
			}

			// ---- configure("sparse_volume_texture", {name=, texture=}) ----
			if (FType.Equals(TEXT("sparse_volume_texture"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("svt"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"sparse_volume_texture\") -> table required: {name=.., texture=..}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				std::string Name = P.get_or("name", std::string());
				if (Name.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"sparse_volume_texture\") -> 'name' required"));
					return sol::lua_nil;
				}

				std::string TexPath = P.get_or("texture", std::string());
				if (TexPath.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"sparse_volume_texture\") -> 'texture' (path) required"));
					return sol::lua_nil;
				}

				FString FTexPath = UTF8_TO_TCHAR(TexPath.c_str());
				USparseVolumeTexture* SVT = NeoStackToolUtils::LoadAssetWithFallback<USparseVolumeTexture>(FTexPath);
				if (!SVT && !FTexPath.StartsWith(TEXT("/")))
				{
					SVT = NeoStackToolUtils::LoadAssetWithFallback<USparseVolumeTexture>(TEXT("/Game/") + FTexPath);
				}
				if (!SVT)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"sparse_volume_texture\") -> asset not found: %s"), *FTexPath));
					return sol::lua_nil;
				}

				FName ParamName = FName(UTF8_TO_TCHAR(Name.c_str()));
				FMaterialParameterInfo ParamInfo = BuildParameterInfo(ParamName, P);

				const FScopedTransaction Transaction(FText::FromString(TEXT("MI: Set Sparse Volume Texture Parameter")));
				MI->Modify();

				bool bFound = false;
				for (FSparseVolumeTextureParameterValue& Existing : MI->SparseVolumeTextureParameterValues)
				{
					if (Existing.ParameterInfo.Name == ParamName
						&& Existing.ParameterInfo.Association == ParamInfo.Association
						&& Existing.ParameterInfo.Index == ParamInfo.Index)
					{
						Existing.ParameterValue = SVT;
						bFound = true;
						break;
					}
				}
				if (!bFound)
				{
					FSparseVolumeTextureParameterValue NewParam(ParamInfo, SVT);
					MI->SparseVolumeTextureParameterValues.Add(NewParam);
				}

				FinalizeMaterialInstance(MI, FindMIProperty(TEXT("SparseVolumeTextureParameterValues")));

				Session.Log(FString::Printf(TEXT("[OK] configure(\"sparse_volume_texture\", name=\"%s\", texture=\"%s\")"),
					*ParamName.ToString(), *SVT->GetPathName()));
				return sol::make_object(Lua, true);
			}

			// ---- configure("texture_collection", {name=, path=}) ----
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			if (FType.Equals(TEXT("texture_collection"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"texture_collection\") -> table required: {name=.., path=..}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				std::string Name = P.get_or("name", std::string());
				if (Name.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"texture_collection\") -> 'name' required"));
					return sol::lua_nil;
				}

				std::string TCPath = P.get_or("path", std::string());
				if (TCPath.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"texture_collection\") -> 'path' required"));
					return sol::lua_nil;
				}

				FString FTCPath = UTF8_TO_TCHAR(TCPath.c_str());
				UTextureCollection* TC = NeoStackToolUtils::LoadAssetWithFallback<UTextureCollection>(FTCPath);
				if (!TC && !FTCPath.StartsWith(TEXT("/")))
				{
					TC = NeoStackToolUtils::LoadAssetWithFallback<UTextureCollection>(TEXT("/Game/") + FTCPath);
				}
				if (!TC)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"texture_collection\") -> asset not found: %s"), *FTCPath));
					return sol::lua_nil;
				}

				FName ParamName = FName(UTF8_TO_TCHAR(Name.c_str()));
				FMaterialParameterInfo ParamInfo = BuildParameterInfo(ParamName, P);

				const FScopedTransaction Transaction(FText::FromString(TEXT("MI: Set Texture Collection Parameter")));
				MI->Modify();

				bool bFound = false;
				for (FTextureCollectionParameterValue& Existing : MI->TextureCollectionParameterValues)
				{
					if (Existing.ParameterInfo.Name == ParamName
						&& Existing.ParameterInfo.Association == ParamInfo.Association
						&& Existing.ParameterInfo.Index == ParamInfo.Index)
					{
						Existing.ParameterValue = TC;
						bFound = true;
						break;
					}
				}
				if (!bFound)
				{
					FTextureCollectionParameterValue NewParam(ParamInfo, TC);
					MI->TextureCollectionParameterValues.Add(NewParam);
				}

				FinalizeMaterialInstance(MI, FindMIProperty(TEXT("TextureCollectionParameterValues")));

				Session.Log(FString::Printf(TEXT("[OK] configure(\"texture_collection\", name=\"%s\", path=\"%s\")"),
					*ParamName.ToString(), *TC->GetPathName()));
				return sol::make_object(Lua, true);
			}
#endif // ENGINE_MINOR_VERSION >= 5

			// ---- configure("parameter_collection", {name=, path=}) ---- (5.7+ only: FParameterCollectionParameterValue)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			if (FType.Equals(TEXT("parameter_collection"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"parameter_collection\") -> table required: {name=.., path=..}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				std::string Name = P.get_or("name", std::string());
				if (Name.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"parameter_collection\") -> 'name' required"));
					return sol::lua_nil;
				}

				std::string PCPath = P.get_or("path", std::string());
				if (PCPath.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"parameter_collection\") -> 'path' required"));
					return sol::lua_nil;
				}

				FString FPCPath = UTF8_TO_TCHAR(PCPath.c_str());
				UMaterialParameterCollection* PC = NeoStackToolUtils::LoadAssetWithFallback<UMaterialParameterCollection>(FPCPath);
				if (!PC && !FPCPath.StartsWith(TEXT("/")))
				{
					PC = NeoStackToolUtils::LoadAssetWithFallback<UMaterialParameterCollection>(TEXT("/Game/") + FPCPath);
				}
				if (!PC)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"parameter_collection\") -> asset not found: %s"), *FPCPath));
					return sol::lua_nil;
				}

				FName ParamName = FName(UTF8_TO_TCHAR(Name.c_str()));
				FMaterialParameterInfo ParamInfo = BuildParameterInfo(ParamName, P);

				const FScopedTransaction Transaction(FText::FromString(TEXT("MI: Set Parameter Collection")));
				MI->Modify();

				bool bFound = false;
				for (FParameterCollectionParameterValue& Existing : MI->ParameterCollectionParameterValues)
				{
					if (Existing.ParameterInfo.Name == ParamName
						&& Existing.ParameterInfo.Association == ParamInfo.Association
						&& Existing.ParameterInfo.Index == ParamInfo.Index)
					{
						Existing.ParameterValue = PC;
						bFound = true;
						break;
					}
				}
				if (!bFound)
				{
					FParameterCollectionParameterValue NewParam(ParamInfo, PC);
					MI->ParameterCollectionParameterValues.Add(NewParam);
				}

				FinalizeMaterialInstance(MI, FindMIProperty(TEXT("ParameterCollectionParameterValues")));

				Session.Log(FString::Printf(TEXT("[OK] configure(\"parameter_collection\", name=\"%s\", path=\"%s\")"),
					*ParamName.ToString(), *PC->GetPathName()));
				return sol::make_object(Lua, true);
			}
#endif // ENGINE_MINOR_VERSION >= 7

			// ---- configure("nanite_override", {path="/Path/To/Material"}) or {path="none"} ----
			if (FType.Equals(TEXT("nanite_override"), ESearchCase::IgnoreCase))
			{
#if WITH_EDITORONLY_DATA
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"nanite_override\") -> table required: {path=\"/Path/..\"} or {path=\"none\", enabled=false}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				const FScopedTransaction Transaction(FText::FromString(TEXT("MI: Set Nanite Override Material")));
				MI->Modify();

				sol::optional<std::string> PathOpt = P.get<sol::optional<std::string>>("path");
				if (PathOpt.has_value())
				{
					FString FMatPath = UTF8_TO_TCHAR(PathOpt.value().c_str());
					if (FMatPath.Equals(TEXT("none"), ESearchCase::IgnoreCase))
					{
						// SetOverrideMaterial is not ENGINE_API — set members directly
						MI->NaniteOverrideMaterial.OverrideMaterialEditor = nullptr;
						MI->NaniteOverrideMaterial.bEnableOverride = false;
						Session.Log(TEXT("[OK] configure(\"nanite_override\") -> cleared"));
					}
					else
					{
						UMaterialInterface* OverrideMat = NeoStackToolUtils::LoadAssetWithFallback<UMaterialInterface>(FMatPath);
						if (!OverrideMat && !FMatPath.StartsWith(TEXT("/")))
						{
							OverrideMat = NeoStackToolUtils::LoadAssetWithFallback<UMaterialInterface>(TEXT("/Game/") + FMatPath);
						}
						if (!OverrideMat)
						{
							Session.Log(FString::Printf(TEXT("[FAIL] configure(\"nanite_override\") -> material not found: %s"), *FMatPath));
							return sol::lua_nil;
						}

						bool bEnable = P.get_or("enabled", true);
						MI->NaniteOverrideMaterial.OverrideMaterialEditor = OverrideMat;
						MI->NaniteOverrideMaterial.bEnableOverride = bEnable;
						Session.Log(FString::Printf(TEXT("[OK] configure(\"nanite_override\") -> %s (enabled=%s)"),
							*OverrideMat->GetPathName(), bEnable ? TEXT("true") : TEXT("false")));
					}
				}

				sol::optional<bool> EnabledOpt = P.get<sol::optional<bool>>("enabled");
				if (EnabledOpt.has_value() && !PathOpt.has_value())
				{
					// Just toggle enable without changing the material
					MI->NaniteOverrideMaterial.bEnableOverride = EnabledOpt.value();
					Session.Log(FString::Printf(TEXT("[OK] configure(\"nanite_override\") -> enabled=%s"),
						EnabledOpt.value() ? TEXT("true") : TEXT("false")));
				}

				MI->PostEditChange();
				MI->MarkPackageDirty();
				return sol::make_object(Lua, true);
#else
				Session.Log(TEXT("[FAIL] configure(\"nanite_override\") -> not available in non-editor builds"));
				return sol::lua_nil;
#endif
			}

			// ---- configure("user_scene_texture", {key="Name", value="OverrideName"}) ---- (5.5+)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			if (FType.Equals(TEXT("user_scene_texture"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"user_scene_texture\") -> table required: {key=\"Name\", value=\"OverrideName\"}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				std::string Key = P.get_or("key", std::string());
				std::string Value = P.get_or("value", std::string());

				FName FKey = Key.empty() ? NAME_None : FName(UTF8_TO_TCHAR(Key.c_str()));
				FName FValue = Value.empty() ? NAME_None : FName(UTF8_TO_TCHAR(Value.c_str()));

				const FScopedTransaction Transaction(FText::FromString(TEXT("MI: Set User Scene Texture Override")));
				MI->Modify();

				// Find existing override with same key
				bool bFound = false;
				for (FUserSceneTextureOverride& Existing : MI->UserSceneTextureOverrides)
				{
					if (Existing.Key == FKey)
					{
						Existing.Value = FValue;
						bFound = true;
						break;
					}
				}
				if (!bFound)
				{
					FUserSceneTextureOverride NewOverride;
					NewOverride.Key = FKey;
					NewOverride.Value = FValue;
					MI->UserSceneTextureOverrides.Add(NewOverride);
				}

				MI->PostEditChange();
				MI->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"user_scene_texture\", key=\"%s\", value=\"%s\")"),
					*FKey.ToString(), *FValue.ToString()));
				return sol::make_object(Lua, true);
			}
#endif

			// ---- configure("phys_material_map", {index=N, path="/Path/To/PhysMat"}) ----
			if (FType.Equals(TEXT("phys_material_map"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"phys_material_map\") -> table required: {index=N, path=\"/Path/..\"}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				sol::optional<int> IndexOpt = P.get<sol::optional<int>>("index");
				if (!IndexOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"phys_material_map\") -> 'index' required (0-7)"));
					return sol::lua_nil;
				}
				int32 Index = static_cast<int32>(IndexOpt.value());
				if (Index < 0 || Index >= EPhysicalMaterialMaskColor::MAX)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"phys_material_map\") -> index %d out of range [0, %d)"),
						Index, EPhysicalMaterialMaskColor::MAX));
					return sol::lua_nil;
				}

				std::string PhysPath = P.get_or("path", std::string());
				if (PhysPath.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"phys_material_map\") -> 'path' required"));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("MI: Set PhysMaterial Map Entry")));
				MI->Modify();

				FString FPhysPath = UTF8_TO_TCHAR(PhysPath.c_str());
				if (FPhysPath.Equals(TEXT("none"), ESearchCase::IgnoreCase))
				{
					MI->PhysicalMaterialMap[Index] = nullptr;
					Session.Log(FString::Printf(TEXT("[OK] configure(\"phys_material_map\", index=%d) -> cleared"), Index));
				}
				else
				{
					UPhysicalMaterial* PhysMat = LoadObject<UPhysicalMaterial>(nullptr, *FPhysPath);
					if (!PhysMat)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"phys_material_map\") -> not found: %s"), *FPhysPath));
						return sol::lua_nil;
					}
					MI->PhysicalMaterialMap[Index] = PhysMat;
					Session.Log(FString::Printf(TEXT("[OK] configure(\"phys_material_map\", index=%d) -> %s"), Index, *PhysMat->GetName()));
				}

				MI->PostEditChange();
				MI->MarkPackageDirty();
				return sol::make_object(Lua, true);
			}

			// ---- configure("subsurface_profile", {path="/Path/To/Profile"}) or "none" ----
			if (FType.Equals(TEXT("subsurface_profile"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"subsurface_profile\") -> table required: {path=\"/Path/..\"} or {path=\"none\"}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				std::string ProfilePath = P.get_or("path", std::string());
				if (ProfilePath.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"subsurface_profile\") -> 'path' required"));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("MI: Set Subsurface Profile")));
				MI->Modify();

				FString FProfilePath = UTF8_TO_TCHAR(ProfilePath.c_str());
				if (FProfilePath.Equals(TEXT("none"), ESearchCase::IgnoreCase))
				{
					MI->bOverrideSubsurfaceProfile = true;
					MI->SubsurfaceProfile = nullptr;
					Session.Log(TEXT("[OK] configure(\"subsurface_profile\") -> cleared (override enabled, profile=none)"));
				}
				else
				{
					USubsurfaceProfile* Profile = LoadObject<USubsurfaceProfile>(nullptr, *FProfilePath);
					if (!Profile)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"subsurface_profile\") -> not found: %s"), *FProfilePath));
						return sol::lua_nil;
					}
					MI->bOverrideSubsurfaceProfile = true;
					MI->SubsurfaceProfile = Profile;
					Session.Log(FString::Printf(TEXT("[OK] configure(\"subsurface_profile\") -> %s"), *Profile->GetName()));
				}

				// Optional: explicitly control override flag
				sol::optional<bool> OverrideOpt = P.get<sol::optional<bool>>("override");
				if (OverrideOpt.has_value())
				{
					MI->bOverrideSubsurfaceProfile = OverrideOpt.value();
				}

				MI->PostEditChange();
				MI->MarkPackageDirty();
				return sol::make_object(Lua, true);
			}

	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			// ---- configure("specular_profile", {path="/Path/To/Profile"}) or "none" ----
			if (FType.Equals(TEXT("specular_profile"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"specular_profile\") -> table required: {path=\"/Path/..\"} or {path=\"none\"}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				std::string ProfilePath = P.get_or("path", std::string());
				if (ProfilePath.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"specular_profile\") -> 'path' required"));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("MI: Set Specular Profile")));
				MI->Modify();

				FString FProfilePath = UTF8_TO_TCHAR(ProfilePath.c_str());
				if (FProfilePath.Equals(TEXT("none"), ESearchCase::IgnoreCase))
				{
					MI->bOverrideSpecularProfile = true;
					MI->SpecularProfileOverride = nullptr;
					Session.Log(TEXT("[OK] configure(\"specular_profile\") -> cleared (override enabled, profile=none)"));
				}
				else
				{
					USpecularProfile* Profile = LoadObject<USpecularProfile>(nullptr, *FProfilePath);
					if (!Profile)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"specular_profile\") -> not found: %s"), *FProfilePath));
						return sol::lua_nil;
					}
					MI->bOverrideSpecularProfile = true;
					MI->SpecularProfileOverride = Profile;
					Session.Log(FString::Printf(TEXT("[OK] configure(\"specular_profile\") -> %s"), *Profile->GetName()));
				}

				// Optional: explicitly control override flag
				sol::optional<bool> OverrideOpt = P.get<sol::optional<bool>>("override");
				if (OverrideOpt.has_value())
				{
					MI->bOverrideSpecularProfile = OverrideOpt.value();
				}

				MI->PostEditChange();
				MI->MarkPackageDirty();
				return sol::make_object(Lua, true);
			}
#endif

			// ---- configure("static_switch", {name=, value=}) ----
			if (FType.Equals(TEXT("static_switch"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"static_switch\") -> table required: {name=.., value=..}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				std::string Name = P.get_or("name", std::string());
				if (Name.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"static_switch\") -> 'name' required"));
					return sol::lua_nil;
				}

				sol::optional<bool> Value = P.get<sol::optional<bool>>("value");
				if (!Value.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"static_switch\") -> 'value' (bool) required"));
					return sol::lua_nil;
				}

				FName ParamName = FName(UTF8_TO_TCHAR(Name.c_str()));
				bool bValue = Value.value();

				// GetStaticParameters returns by value — copy, modify, set back
				FStaticParameterSet StaticParams = MI->GetStaticParameters();

				bool bFound = false;
				for (FStaticSwitchParameter& Switch : StaticParams.StaticSwitchParameters)
				{
					if (Switch.ParameterInfo.Name == ParamName)
					{
						Switch.bOverride = true;
						Switch.Value = bValue;
						bFound = true;
						break;
					}
				}
				if (!bFound)
				{
					// Add new static switch parameter
					FStaticSwitchParameter NewSwitch;
					NewSwitch.ParameterInfo.Name = ParamName;
					NewSwitch.bOverride = true;
					NewSwitch.Value = bValue;
					StaticParams.StaticSwitchParameters.Add(NewSwitch);
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("MI: Set Static Switch")));
				MI->Modify();

				// UpdateStaticPermutation recompiles the shader permutation
				MI->UpdateStaticPermutation(StaticParams);
				MI->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"static_switch\", name=\"%s\", value=%s)"),
					*ParamName.ToString(), bValue ? TEXT("true") : TEXT("false")));
				return sol::make_object(Lua, true);
			}

			// ---- configure("static_mask", {name=, r=, g=, b=, a=}) ----
			if (FType.Equals(TEXT("static_mask"), ESearchCase::IgnoreCase))
			{
				if (!SupportsStaticMasks())
				{
					Session.Log(TEXT("[FAIL] configure(\"static_mask\") -> not available in this build"));
					return sol::lua_nil;
				}
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"static_mask\") -> table required: {name=.., r=, g=, b=, a=}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				std::string Name = P.get_or("name", std::string());
				if (Name.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"static_mask\") -> 'name' required"));
					return sol::lua_nil;
				}

				bool bR = P.get_or("r", false);
				bool bG = P.get_or("g", false);
				bool bB = P.get_or("b", false);
				bool bA = P.get_or("a", false);

				FName ParamName = FName(UTF8_TO_TCHAR(Name.c_str()));

				FStaticParameterSet StaticParams = MI->GetStaticParameters();
				ApplyStaticMaskParameter(StaticParams, ParamName, bR, bG, bB, bA);

				const FScopedTransaction Transaction(FText::FromString(TEXT("MI: Set Static Mask")));
				MI->Modify();
				MI->UpdateStaticPermutation(StaticParams);
				MI->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"static_mask\", name=\"%s\", R=%d G=%d B=%d A=%d)"),
					*ParamName.ToString(), bR, bG, bB, bA));
				return sol::make_object(Lua, true);
			}

			// ---- configure("base_property", {blend_mode=, two_sided=, opacity_mask_clip=, ...}) ----
			if (FType.Equals(TEXT("base_property"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"base_property\") -> table required"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				const FScopedTransaction Transaction(FText::FromString(TEXT("MI: Set Base Property Overrides")));
				MI->Modify();

				FMaterialInstanceBasePropertyOverrides& BaseProps = MI->BasePropertyOverrides;

				// Blend mode
				sol::optional<std::string> BlendModeStr = P.get<sol::optional<std::string>>("blend_mode");
				if (BlendModeStr.has_value())
				{
					EBlendMode Mode;
					if (ParseBlendMode(BlendModeStr.value(), Mode))
					{
						BaseProps.bOverride_BlendMode = true;
						BaseProps.BlendMode = Mode;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure(\"base_property\") -> unknown blend_mode '%s'"),
							UTF8_TO_TCHAR(BlendModeStr.value().c_str())));
					}
				}

				// Two sided
				sol::optional<bool> TwoSided = P.get<sol::optional<bool>>("two_sided");
				if (TwoSided.has_value())
				{
					BaseProps.bOverride_TwoSided = true;
					BaseProps.TwoSided = TwoSided.value();
				}

				// Opacity mask clip value
				sol::optional<double> OpacityClip = P.get<sol::optional<double>>("opacity_mask_clip");
				if (OpacityClip.has_value())
				{
					BaseProps.bOverride_OpacityMaskClipValue = true;
					BaseProps.OpacityMaskClipValue = static_cast<float>(OpacityClip.value());
				}

				// Shading model (integer)
				sol::optional<int> ShadingModel = P.get<sol::optional<int>>("shading_model");
				if (ShadingModel.has_value())
				{
					BaseProps.bOverride_ShadingModel = true;
					BaseProps.ShadingModel = static_cast<TEnumAsByte<EMaterialShadingModel>>(ShadingModel.value());
				}

				// Dithered LOD transition
				sol::optional<bool> DitheredLOD = P.get<sol::optional<bool>>("dithered_lod_transition");
				if (DitheredLOD.has_value())
				{
					BaseProps.bOverride_DitheredLODTransition = true;
					BaseProps.DitheredLODTransition = DitheredLOD.value();
				}

				// Cast shadow as masked
				sol::optional<bool> CastShadowMasked = P.get<sol::optional<bool>>("cast_shadow_as_masked");
				if (CastShadowMasked.has_value())
				{
					BaseProps.bOverride_CastDynamicShadowAsMasked = true;
					BaseProps.bCastDynamicShadowAsMasked = CastShadowMasked.value();
				}

				// Thin surface
				sol::optional<bool> ThinSurface = P.get<sol::optional<bool>>("thin_surface");
				if (ThinSurface.has_value())
				{
					BaseProps.bOverride_bIsThinSurface = true;
					BaseProps.bIsThinSurface = ThinSurface.value();
				}

				// Enable tessellation
				sol::optional<bool> Tessellation = P.get<sol::optional<bool>>("enable_tessellation");
				if (Tessellation.has_value())
				{
					BaseProps.bOverride_bEnableTessellation = true;
					BaseProps.bEnableTessellation = Tessellation.value();
				}

				// Has pixel animation
				sol::optional<bool> PixelAnimation = P.get<sol::optional<bool>>("has_pixel_animation");
				if (PixelAnimation.has_value())
				{
					BaseProps.bOverride_bHasPixelAnimation = true;
					BaseProps.bHasPixelAnimation = PixelAnimation.value();
				}

				// Displacement scaling (magnitude + center)
				sol::optional<sol::table> DispScaling = P.get<sol::optional<sol::table>>("displacement_scaling");
				if (DispScaling.has_value())
				{
					BaseProps.bOverride_DisplacementScaling = true;
					BaseProps.DisplacementScaling.Magnitude = static_cast<float>(DispScaling.value().get_or("magnitude", 4.0));
					BaseProps.DisplacementScaling.Center = static_cast<float>(DispScaling.value().get_or("center", 0.5));
				}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				// Enable displacement fade
				sol::optional<bool> DispFade = P.get<sol::optional<bool>>("enable_displacement_fade");
				if (DispFade.has_value())
				{
					BaseProps.bOverride_bEnableDisplacementFade = true;
					BaseProps.bEnableDisplacementFade = DispFade.value();
				}

				// Displacement fade range
				sol::optional<sol::table> DispFadeRange = P.get<sol::optional<sol::table>>("displacement_fade_range");
				if (DispFadeRange.has_value())
				{
					BaseProps.bOverride_DisplacementFadeRange = true;
					BaseProps.DisplacementFadeRange.StartSizePixels = static_cast<float>(DispFadeRange.value().get_or("start_size_pixels", 4.0));
					BaseProps.DisplacementFadeRange.EndSizePixels = static_cast<float>(DispFadeRange.value().get_or("end_size_pixels", 1.0));
				}
#endif

				// Max world position offset displacement
				sol::optional<double> MaxWPO = P.get<sol::optional<double>>("max_world_position_offset");
				if (MaxWPO.has_value())
				{
					BaseProps.bOverride_MaxWorldPositionOffsetDisplacement = true;
					BaseProps.MaxWorldPositionOffsetDisplacement = static_cast<float>(MaxWPO.value());
				}

	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
				sol::optional<bool> LumenCardSharing = P.get<sol::optional<bool>>("compatible_with_lumen_card_sharing");
				if (LumenCardSharing.has_value())
				{
					BaseProps.bOverride_CompatibleWithLumenCardSharing = true;
					BaseProps.bCompatibleWithLumenCardSharing = LumenCardSharing.value();
				}
#endif

				// Use UpdateStaticPermutation with base property overrides to trigger shader recompilation
				FStaticParameterSet StaticParams = MI->GetStaticParameters();
				MI->UpdateStaticPermutation(StaticParams, BaseProps, true);
				MI->MarkPackageDirty();

				Session.Log(TEXT("[OK] configure(\"base_property\") -> overrides applied"));
				return sol::make_object(Lua, true);
			}

			// ---- configure("physical_material", {phys_material="/Path/To/PhysMat"}) ----
			if (FType.Equals(TEXT("physical_material"), ESearchCase::IgnoreCase) ||
				FType.Equals(TEXT("phys_material"), ESearchCase::IgnoreCase))
			{
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"physical_material\") -> table required: {phys_material=\"/Path/...\"}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				const FScopedTransaction Tx(FText::FromString(TEXT("MaterialInstance: Set PhysMaterial")));
				MI->Modify();

				std::string PhysMatPath = P.get_or<std::string>("phys_material", "");
				if (!PhysMatPath.empty())
				{
					FString FPhysPath = UTF8_TO_TCHAR(PhysMatPath.c_str());
					if (FPhysPath.Equals(TEXT("none"), ESearchCase::IgnoreCase))
					{
						MI->PhysMaterial = nullptr;
						Session.Log(TEXT("[OK] configure(\"physical_material\") -> cleared"));
					}
					else
					{
						UPhysicalMaterial* PhysMat = LoadObject<UPhysicalMaterial>(nullptr, *FPhysPath);
						if (PhysMat)
						{
							MI->PhysMaterial = PhysMat;
							Session.Log(FString::Printf(TEXT("[OK] configure(\"physical_material\") -> %s"), *PhysMat->GetName()));
						}
						else
						{
							Session.Log(FString::Printf(TEXT("[FAIL] configure(\"physical_material\") -> not found: %s"), *FPhysPath));
							return sol::lua_nil;
						}
					}
				}

				MI->PostEditChange();
				MI->MarkPackageDirty();
				return sol::make_object(Lua, true);
			}

			// ---- configure("layer", {index=, function=, enabled=, name=}) ----
			if (FType.Equals(TEXT("layer"), ESearchCase::IgnoreCase))
			{
				if (!SupportsLayerEditing())
				{
					Session.Log(TEXT("[FAIL] configure(\"layer\") -> not available in this build"));
					return sol::lua_nil;
				}
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"layer\") -> table required: {index=N, function=, enabled=, name=}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				sol::optional<int> IndexOpt = P.get<sol::optional<int>>("index");
				if (!IndexOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"layer\") -> 'index' required"));
					return sol::lua_nil;
				}
				int32 Index = static_cast<int32>(IndexOpt.value());

				FMaterialLayersFunctions LayerFunctions;
				if (!MI->GetMaterialLayers(LayerFunctions))
				{
					Session.Log(TEXT("[FAIL] configure(\"layer\") -> material has no layers"));
					return sol::lua_nil;
				}

				if (Index < 0 || Index >= LayerFunctions.Layers.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"layer\") -> index %d out of range [0, %d)"), Index, LayerFunctions.Layers.Num()));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("MI: Configure Material Layer")));
				MI->Modify();

				// Set layer function
				sol::optional<std::string> FuncPath = P.get<sol::optional<std::string>>("function");
				if (FuncPath.has_value())
				{
					FString FFuncPath = UTF8_TO_TCHAR(FuncPath.value().c_str());
					UMaterialFunctionInterface* Func = NeoStackToolUtils::LoadAssetWithFallback<UMaterialFunctionInterface>(FFuncPath);
					if (!Func && !FFuncPath.StartsWith(TEXT("/")))
					{
						Func = NeoStackToolUtils::LoadAssetWithFallback<UMaterialFunctionInterface>(TEXT("/Game/") + FFuncPath);
					}
					if (!Func)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"layer\") -> function not found: %s"), *FFuncPath));
						return sol::lua_nil;
					}
					LayerFunctions.Layers[Index] = Func;
				}

				// Set enabled/visibility (bounds-check LayerStates — can be shorter than Layers in corrupt data)
				sol::optional<bool> Enabled = P.get<sol::optional<bool>>("enabled");
				if (Enabled.has_value() && Index < LayerFunctions.EditorOnly.LayerStates.Num())
				{
					LayerFunctions.EditorOnly.LayerStates[Index] = Enabled.value();
				}

				// Set name
				sol::optional<std::string> NameOpt = P.get<sol::optional<std::string>>("name");
				SetLayerNameIfSupported(LayerFunctions, Index, NameOpt);

				MI->SetMaterialLayers(LayerFunctions);
				MI->UpdateStaticPermutation();
				MI->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"layer\", index=%d)"), Index));
				return sol::make_object(Lua, true);
			}

			// ---- configure("blend", {index=, function=}) ----
			if (FType.Equals(TEXT("blend"), ESearchCase::IgnoreCase))
			{
				if (!SupportsLayerEditing())
				{
					Session.Log(TEXT("[FAIL] configure(\"blend\") -> not available in this build"));
					return sol::lua_nil;
				}
				if (!Params.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"blend\") -> table required: {index=N, function=\"/Path\"}"));
					return sol::lua_nil;
				}
				sol::table P = Params.as<sol::table>();

				sol::optional<int> IndexOpt = P.get<sol::optional<int>>("index");
				if (!IndexOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"blend\") -> 'index' required"));
					return sol::lua_nil;
				}
				int32 Index = static_cast<int32>(IndexOpt.value());

				sol::optional<std::string> FuncPath = P.get<sol::optional<std::string>>("function");
				if (!FuncPath.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"blend\") -> 'function' (path) required"));
					return sol::lua_nil;
				}

				FMaterialLayersFunctions LayerFunctions;
				if (!MI->GetMaterialLayers(LayerFunctions))
				{
					Session.Log(TEXT("[FAIL] configure(\"blend\") -> material has no layers"));
					return sol::lua_nil;
				}

				// Index is the layer index — background (0) has no blend, so reject it
				if (Index <= 0)
				{
					Session.Log(TEXT("[FAIL] configure(\"blend\") -> index must be > 0 (background layer has no blend)"));
					return sol::lua_nil;
				}
				int32 BlendIdx = Index - 1; // Blends[0] = blend for Layers[1]
				if (BlendIdx >= LayerFunctions.Blends.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"blend\") -> layer index %d out of range (1-%d)"), Index, LayerFunctions.Layers.Num() - 1));
					return sol::lua_nil;
				}

				FString FFuncPath = UTF8_TO_TCHAR(FuncPath.value().c_str());
				UMaterialFunctionInterface* Func = NeoStackToolUtils::LoadAssetWithFallback<UMaterialFunctionInterface>(FFuncPath);
				if (!Func && !FFuncPath.StartsWith(TEXT("/")))
				{
					Func = NeoStackToolUtils::LoadAssetWithFallback<UMaterialFunctionInterface>(TEXT("/Game/") + FFuncPath);
				}
				if (!Func)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"blend\") -> function not found: %s"), *FFuncPath));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("MI: Configure Blend Function")));
				MI->Modify();

				LayerFunctions.Blends[BlendIdx] = Func;

				MI->SetMaterialLayers(LayerFunctions);
				MI->UpdateStaticPermutation();
				MI->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"blend\", index=%d, function=\"%s\")"), Index, *Func->GetPathName()));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: scalar, vector, double_vector, texture, texture_collection, parameter_collection, font, runtime_virtual_texture (rvt), sparse_volume_texture (svt), static_switch, static_mask, base_property, physical_material, phys_material_map, subsurface_profile, specular_profile, nanite_override, user_scene_texture, layer, blend"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// add(type, params)
		// ================================================================
		AssetObj.set_function("add", [MI, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(MI))
			{
				Session.Log(TEXT("[FAIL] add -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FType = UTF8_TO_TCHAR(Type.c_str());
			if (!SupportsLayerEditing())
			{
				Session.Log(TEXT("[FAIL] add -> not available in this build"));
				return sol::lua_nil;
			}

			// ---- add("layer", {function=, blend=, name=}) ----
			if (FType.Equals(TEXT("layer"), ESearchCase::IgnoreCase))
			{
				sol::table P = Params.is<sol::table>() ? Params.as<sol::table>() : Lua.create_table();

				FMaterialLayersFunctions LayerFunctions;
				if (!MI->GetMaterialLayers(LayerFunctions))
				{
					// Initialize with default background layer if no layers exist
					LayerFunctions.AddDefaultBackgroundLayer();
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("MI: Add Material Layer")));
				MI->Modify();

				int32 NewIndex = LayerFunctions.AppendBlendedLayer();

				// Set layer function if provided (accept "function", "function_path", or "layer_function")
				sol::optional<std::string> FuncPath = P.get<sol::optional<std::string>>("function");
				if (!FuncPath.has_value()) FuncPath = P.get<sol::optional<std::string>>("function_path");
				if (!FuncPath.has_value()) FuncPath = P.get<sol::optional<std::string>>("layer_function");
				if (FuncPath.has_value())
				{
					FString FFuncPath = UTF8_TO_TCHAR(FuncPath.value().c_str());
					UMaterialFunctionInterface* Func = NeoStackToolUtils::LoadAssetWithFallback<UMaterialFunctionInterface>(FFuncPath);
					if (!Func && !FFuncPath.StartsWith(TEXT("/")))
					{
						Func = NeoStackToolUtils::LoadAssetWithFallback<UMaterialFunctionInterface>(TEXT("/Game/") + FFuncPath);
					}
					if (Func)
					{
						LayerFunctions.Layers[NewIndex] = Func;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] add(\"layer\") -> layer function not found: %s"), *FFuncPath));
					}
				}

				// Set blend function if provided (accept "blend", "blend_path", or "blend_function")
				sol::optional<std::string> BlendPath = P.get<sol::optional<std::string>>("blend");
				if (!BlendPath.has_value()) BlendPath = P.get<sol::optional<std::string>>("blend_path");
				if (!BlendPath.has_value()) BlendPath = P.get<sol::optional<std::string>>("blend_function");
				int32 BlendIdx = NewIndex - 1; // Blends[0] = blend for Layers[1]
				if (BlendPath.has_value() && BlendIdx >= 0 && BlendIdx < LayerFunctions.Blends.Num())
				{
					FString FBlendPath = UTF8_TO_TCHAR(BlendPath.value().c_str());
					UMaterialFunctionInterface* BlendFunc = NeoStackToolUtils::LoadAssetWithFallback<UMaterialFunctionInterface>(FBlendPath);
					if (!BlendFunc && !FBlendPath.StartsWith(TEXT("/")))
					{
						BlendFunc = NeoStackToolUtils::LoadAssetWithFallback<UMaterialFunctionInterface>(TEXT("/Game/") + FBlendPath);
					}
					if (BlendFunc)
					{
						LayerFunctions.Blends[BlendIdx] = BlendFunc;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] add(\"layer\") -> blend function not found: %s"), *FBlendPath));
					}
				}

				// Set name if provided
				sol::optional<std::string> NameOpt = P.get<sol::optional<std::string>>("name");
				SetLayerNameIfSupported(LayerFunctions, NewIndex, NameOpt);

				MI->SetMaterialLayers(LayerFunctions);
				MI->UpdateStaticPermutation();
				MI->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add(\"layer\") -> index %d"), NewIndex));
				sol::table Result = Lua.create_table();
				Result["index"] = NewIndex;
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: layer"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// remove("parameter", {type=, name=})
		// ================================================================
		AssetObj.set_function("remove", [MI, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(MI))
			{
				Session.Log(TEXT("[FAIL] remove -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- remove("layer", {index=N}) ----
			if (FType.Equals(TEXT("layer"), ESearchCase::IgnoreCase))
			{
				if (!SupportsLayerEditing())
				{
					Session.Log(TEXT("[FAIL] remove(\"layer\") -> not available in this build"));
					return sol::lua_nil;
				}
				if (!Id.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] remove(\"layer\") -> table required: {index=N}"));
					return sol::lua_nil;
				}
				sol::table P = Id.as<sol::table>();

				sol::optional<int> IndexOpt = P.get<sol::optional<int>>("index");
				if (!IndexOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] remove(\"layer\") -> 'index' required"));
					return sol::lua_nil;
				}
				int32 Index = static_cast<int32>(IndexOpt.value());

				if (Index <= 0)
				{
					Session.Log(TEXT("[FAIL] remove(\"layer\") -> cannot remove background layer (index 0)"));
					return sol::lua_nil;
				}

				FMaterialLayersFunctions LayerFunctions;
				if (!MI->GetMaterialLayers(LayerFunctions))
				{
					Session.Log(TEXT("[FAIL] remove(\"layer\") -> material has no layers"));
					return sol::lua_nil;
				}

				if (Index >= LayerFunctions.Layers.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"layer\") -> index %d out of range [1, %d)"), Index, LayerFunctions.Layers.Num()));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("MI: Remove Material Layer")));
				MI->Modify();

				LayerFunctions.RemoveBlendedLayerAt(Index);
				MI->RemoveLayerParameterIndex(Index);
				MI->SetMaterialLayers(LayerFunctions);
				MI->UpdateStaticPermutation();
				MI->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"layer\", index=%d)"), Index));
				return sol::make_object(Lua, true);
			}

			// ---- remove("user_scene_texture", {key="Name"}) ---- (5.5+)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			if (FType.Equals(TEXT("user_scene_texture"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] remove(\"user_scene_texture\") -> table required: {key=\"Name\"}"));
					return sol::lua_nil;
				}
				sol::table P = Id.as<sol::table>();

				std::string Key = P.get_or("key", std::string());
				FName FKey = Key.empty() ? NAME_None : FName(UTF8_TO_TCHAR(Key.c_str()));

				const FScopedTransaction Transaction(FText::FromString(TEXT("MI: Remove User Scene Texture Override")));
				MI->Modify();

				for (int32 i = 0; i < MI->UserSceneTextureOverrides.Num(); i++)
				{
					if (MI->UserSceneTextureOverrides[i].Key == FKey)
					{
						MI->UserSceneTextureOverrides.RemoveAt(i);
						MI->PostEditChange();
						MI->MarkPackageDirty();
						Session.Log(FString::Printf(TEXT("[OK] remove(\"user_scene_texture\", key=\"%s\")"), *FKey.ToString()));
						return sol::make_object(Lua, true);
					}
				}

				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"user_scene_texture\") -> key \"%s\" not found"), *FKey.ToString()));
				return sol::lua_nil;
			}
#endif

			if (!FType.Equals(TEXT("parameter"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: parameter, layer, user_scene_texture"), *FType));
				return sol::lua_nil;
			}

			if (!Id.is<sol::table>())
			{
				Session.Log(TEXT("[FAIL] remove(\"parameter\") -> table required: {type=.., name=..}"));
				return sol::lua_nil;
			}
			sol::table P = Id.as<sol::table>();

			std::string ParamType = P.get_or("type", std::string());
			std::string ParamName = P.get_or("name", std::string());
			if (ParamType.empty() || ParamName.empty())
			{
				Session.Log(TEXT("[FAIL] remove(\"parameter\") -> 'type' and 'name' required"));
				return sol::lua_nil;
			}

			FString FParamType = UTF8_TO_TCHAR(ParamType.c_str());
			FName FParamName = FName(UTF8_TO_TCHAR(ParamName.c_str()));

			const FScopedTransaction Transaction(FText::FromString(TEXT("MI: Remove Parameter Override")));
			MI->Modify();

			// Helper lambda to remove from typed arrays
			auto RemoveFromArray = [&](auto& Array, const TCHAR* PropName) -> bool
			{
				for (int32 i = 0; i < Array.Num(); i++)
				{
					if (Array[i].ParameterInfo.Name == FParamName)
					{
						Array.RemoveAt(i);
						FinalizeMaterialInstance(MI, FindMIProperty(PropName));
						return true;
					}
				}
				return false;
			};

			if (FParamType.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
			{
				if (RemoveFromArray(MI->ScalarParameterValues, TEXT("ScalarParameterValues")))
				{
					Session.Log(FString::Printf(TEXT("[OK] remove(\"parameter\", scalar \"%s\")"), *FParamName.ToString()));
					return sol::make_object(Lua, true);
				}
			}
			else if (FParamType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
			{
				if (RemoveFromArray(MI->VectorParameterValues, TEXT("VectorParameterValues")))
				{
					Session.Log(FString::Printf(TEXT("[OK] remove(\"parameter\", vector \"%s\")"), *FParamName.ToString()));
					return sol::make_object(Lua, true);
				}
			}
			else if (FParamType.Equals(TEXT("double_vector"), ESearchCase::IgnoreCase))
			{
				if (RemoveFromArray(MI->DoubleVectorParameterValues, TEXT("DoubleVectorParameterValues")))
				{
					Session.Log(FString::Printf(TEXT("[OK] remove(\"parameter\", double_vector \"%s\")"), *FParamName.ToString()));
					return sol::make_object(Lua, true);
				}
			}
			else if (FParamType.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
			{
				if (RemoveFromArray(MI->TextureParameterValues, TEXT("TextureParameterValues")))
				{
					Session.Log(FString::Printf(TEXT("[OK] remove(\"parameter\", texture \"%s\")"), *FParamName.ToString()));
					return sol::make_object(Lua, true);
				}
			}
			else if (FParamType.Equals(TEXT("font"), ESearchCase::IgnoreCase))
			{
				if (RemoveFromArray(MI->FontParameterValues, TEXT("FontParameterValues")))
				{
					Session.Log(FString::Printf(TEXT("[OK] remove(\"parameter\", font \"%s\")"), *FParamName.ToString()));
					return sol::make_object(Lua, true);
				}
			}
			else if (FParamType.Equals(TEXT("rvt"), ESearchCase::IgnoreCase) || FParamType.Equals(TEXT("runtime_virtual_texture"), ESearchCase::IgnoreCase))
			{
				if (RemoveFromArray(MI->RuntimeVirtualTextureParameterValues, TEXT("RuntimeVirtualTextureParameterValues")))
				{
					Session.Log(FString::Printf(TEXT("[OK] remove(\"parameter\", rvt \"%s\")"), *FParamName.ToString()));
					return sol::make_object(Lua, true);
				}
			}
			else if (FParamType.Equals(TEXT("svt"), ESearchCase::IgnoreCase) || FParamType.Equals(TEXT("sparse_volume_texture"), ESearchCase::IgnoreCase))
			{
				if (RemoveFromArray(MI->SparseVolumeTextureParameterValues, TEXT("SparseVolumeTextureParameterValues")))
				{
					Session.Log(FString::Printf(TEXT("[OK] remove(\"parameter\", svt \"%s\")"), *FParamName.ToString()));
					return sol::make_object(Lua, true);
				}
			}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			else if (FParamType.Equals(TEXT("texture_collection"), ESearchCase::IgnoreCase))
			{
				if (RemoveFromArray(MI->TextureCollectionParameterValues, TEXT("TextureCollectionParameterValues")))
				{
					Session.Log(FString::Printf(TEXT("[OK] remove(\"parameter\", texture_collection \"%s\")"), *FParamName.ToString()));
					return sol::make_object(Lua, true);
				}
			}
#endif // ENGINE_MINOR_VERSION >= 5
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			else if (FParamType.Equals(TEXT("parameter_collection"), ESearchCase::IgnoreCase))
			{
				if (RemoveFromArray(MI->ParameterCollectionParameterValues, TEXT("ParameterCollectionParameterValues")))
				{
					Session.Log(FString::Printf(TEXT("[OK] remove(\"parameter\", parameter_collection \"%s\")"), *FParamName.ToString()));
					return sol::make_object(Lua, true);
				}
			}
#endif
			else if (FParamType.Equals(TEXT("static_switch"), ESearchCase::IgnoreCase))
			{
				FStaticParameterSet StaticParams = MI->GetStaticParameters();
				for (int32 i = 0; i < StaticParams.StaticSwitchParameters.Num(); i++)
				{
					if (StaticParams.StaticSwitchParameters[i].ParameterInfo.Name == FParamName)
					{
						// Set bOverride to false (don't remove — let it revert to parent value)
						StaticParams.StaticSwitchParameters[i].bOverride = false;
						MI->UpdateStaticPermutation(StaticParams);
						MI->MarkPackageDirty();
						Session.Log(FString::Printf(TEXT("[OK] remove(\"parameter\", static_switch \"%s\") -> override cleared"), *FParamName.ToString()));
						return sol::make_object(Lua, true);
					}
				}
			}
			else if (FParamType.Equals(TEXT("static_mask"), ESearchCase::IgnoreCase))
			{
				if (!SupportsStaticMasks())
				{
					Session.Log(TEXT("[FAIL] remove(\"parameter\", static_mask) -> not available in this build"));
					return sol::lua_nil;
				}
				FStaticParameterSet StaticParams = MI->GetStaticParameters();
				if (ClearStaticMaskOverride(StaticParams, FParamName))
				{
					MI->UpdateStaticPermutation(StaticParams);
					MI->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] remove(\"parameter\", static_mask \"%s\") -> override cleared"), *FParamName.ToString()));
					return sol::make_object(Lua, true);
				}
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"parameter\") -> unknown param type '%s'. Valid: scalar, vector, double_vector, texture, texture_collection, parameter_collection, font, rvt, svt, static_switch, static_mask"), *FParamType));
				return sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"parameter\", %s \"%s\") -> parameter not found"), *FParamType, *FParamName.ToString()));
			return sol::lua_nil;
		});
	});
}

REGISTER_LUA_BINDING(MaterialInstance, MaterialInstanceDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindMaterialInstance(Lua, Session);
});
