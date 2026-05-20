// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "FoliageType.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "FoliageType_Actor.h"
#include "LandscapeGrassType.h"
#include "Engine/StaticMesh.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static const char* ScalingToString(EFoliageScaling S)
{
	switch (S)
	{
	case EFoliageScaling::Uniform: return "uniform";
	case EFoliageScaling::Free:    return "free";
	case EFoliageScaling::LockXY:  return "lock_xy";
	case EFoliageScaling::LockXZ:  return "lock_xz";
	case EFoliageScaling::LockYZ:  return "lock_yz";
	default:                       return "uniform";
	}
}

static EFoliageScaling StringToScaling(const std::string& S)
{
	if (S == "free")    return EFoliageScaling::Free;
	if (S == "lock_xy") return EFoliageScaling::LockXY;
	if (S == "lock_xz") return EFoliageScaling::LockXZ;
	if (S == "lock_yz") return EFoliageScaling::LockYZ;
	return EFoliageScaling::Uniform;
}

static const char* GrassScalingToString(EGrassScaling S)
{
	switch (S)
	{
	case EGrassScaling::Uniform: return "uniform";
	case EGrassScaling::Free:    return "free";
	case EGrassScaling::LockXY:  return "lock_xy";
	default:                     return "uniform";
	}
}

static EGrassScaling StringToGrassScaling(const std::string& S)
{
	if (S == "free")    return EGrassScaling::Free;
	if (S == "lock_xy") return EGrassScaling::LockXY;
	return EGrassScaling::Uniform;
}

static const char* MobilityToString(EComponentMobility::Type M)
{
	switch (M)
	{
	case EComponentMobility::Static:    return "static";
	case EComponentMobility::Stationary: return "stationary";
	case EComponentMobility::Movable:   return "movable";
	default:                            return "static";
	}
}

static EComponentMobility::Type StringToMobility(const std::string& S)
{
	if (S == "stationary") return EComponentMobility::Stationary;
	if (S == "movable")    return EComponentMobility::Movable;
	return EComponentMobility::Static;
}

// Helper to read FFloatInterval into a Lua table
static sol::table IntervalToTable(sol::state_view& Lua, const FFloatInterval& I)
{
	sol::table T = Lua.create_table();
	T["min"] = I.Min;
	T["max"] = I.Max;
	return T;
}

// Helper to read FInt32Interval into a Lua table
static sol::table IntIntervalToTable(sol::state_view& Lua, const FInt32Interval& I)
{
	sol::table T = Lua.create_table();
	T["min"] = I.Min;
	T["max"] = I.Max;
	return T;
}

// Helper: read interval from Lua table {min=, max=}
static bool ReadInterval(sol::table T, FFloatInterval& Out)
{
	sol::optional<double> Min = T.get<sol::optional<double>>("min");
	sol::optional<double> Max = T.get<sol::optional<double>>("max");
	if (Min.has_value()) Out.Min = static_cast<float>(Min.value());
	if (Max.has_value()) Out.Max = static_cast<float>(Max.value());
	return Min.has_value() || Max.has_value();
}

static bool ReadIntInterval(sol::table T, FInt32Interval& Out)
{
	sol::optional<int> Min = T.get<sol::optional<int>>("min");
	sol::optional<int> Max = T.get<sol::optional<int>>("max");
	if (Min.has_value()) Out.Min = Min.value();
	if (Max.has_value()) Out.Max = Max.value();
	return Min.has_value() || Max.has_value();
}

// ============================================================================
// UFoliageType ENRICHMENT
// ============================================================================

static TArray<FLuaFunctionDoc> FoliageTypeDocs = {};

static void BindFoliageType(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_foliage_type", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UFoliageType* Foliage = LoadObject<UFoliageType>(nullptr, *FPath);
		if (!Foliage) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"FoliageType enrichment methods:\n"
			"\n"
			"info() — structured summary of foliage settings:\n"
			"  type (instanced_static_mesh|actor), mesh/actor_class,\n"
			"  override_materials, nanite_override_materials,\n"
			"  attach_to_base_component, static_mesh_only (actor type),\n"
			"  density, density_adjustment_factor, radius,\n"
			"  scaling (uniform|free|lock_xy|lock_xz|lock_yz),\n"
			"  scale_x/y/z {min,max}, z_offset {min,max},\n"
			"  align_to_normal, align_max_angle, random_yaw, random_pitch_angle,\n"
			"  average_normal, average_normal_single_component, average_normal_sample_count,\n"
			"  ground_slope_angle {min,max}, height {min,max},\n"
			"  landscape_layers, exclusion_landscape_layers,\n"
			"  minimum_layer_weight, minimum_exclusion_layer_weight,\n"
			"  collision_with_world, collision_scale {x,y,z},\n"
			"  mobility (static|stationary|movable),\n"
			"  cull_distance {min,max}, cast_shadow, cast_dynamic_shadow, cast_static_shadow,\n"
			"  cast_contact_shadow, cast_shadow_as_two_sided, receives_decals,\n"
			"  affect_dynamic_indirect_lighting, affect_distance_field_lighting,\n"
			"  use_as_occluder, enable_density_scaling, enable_cull_distance_scaling,\n"
			"  visible_in_ray_tracing, evaluate_world_position_offset,\n"
			"  world_position_offset_disable_distance,\n"
			"  override_lightmap_res, overridden_lightmap_res,\n"
			"  lighting_channels {channel_0,channel_1,channel_2},\n"
			"  render_custom_depth, custom_depth_stencil_value,\n"
			"  translucency_sort_priority\n"
			"\n"
			"configure(params) — set foliage properties:\n"
			"  mesh (string path, ISM type), actor_class (string path, actor type),\n"
			"  attach_to_base_component (bool, actor type), static_mesh_only (bool, actor type),\n"
			"  density (float), density_adjustment_factor (float),\n"
			"  radius (float), scaling (string), scale_x/y/z ({min,max}),\n"
			"  z_offset ({min,max}), align_to_normal (bool), align_max_angle (float),\n"
			"  random_yaw (bool), random_pitch_angle (float),\n"
			"  average_normal (bool), average_normal_single_component (bool),\n"
			"  average_normal_sample_count (int),\n"
			"  ground_slope_angle ({min,max}), height ({min,max}),\n"
			"  landscape_layers (string array), exclusion_landscape_layers (string array),\n"
			"  minimum_layer_weight (float 0-1), minimum_exclusion_layer_weight (float 0-1),\n"
			"  collision_with_world (bool), collision_scale ({x,y,z}),\n"
			"  mobility (string), cull_distance ({min,max}),\n"
			"  cast_shadow (bool), cast_dynamic_shadow (bool), cast_static_shadow (bool),\n"
			"  cast_contact_shadow (bool), cast_shadow_as_two_sided (bool),\n"
			"  receives_decals (bool), affect_dynamic_indirect_lighting (bool),\n"
			"  affect_distance_field_lighting (bool), use_as_occluder (bool),\n"
			"  enable_density_scaling (bool), enable_cull_distance_scaling (bool),\n"
			"  visible_in_ray_tracing (bool), evaluate_world_position_offset (bool),\n"
			"  world_position_offset_disable_distance (int),\n"
			"  override_lightmap_res (bool), overridden_lightmap_res (int),\n"
			"  lighting_channels ({channel_0,channel_1,channel_2} bools),\n"
			"  render_custom_depth (bool), custom_depth_stencil_value (int),\n"
			"  translucency_sort_priority (int)\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [Foliage, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Foliage))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table R = Lua.create_table();

			// Type + source
			if (UFoliageType_InstancedStaticMesh* ISM = Cast<UFoliageType_InstancedStaticMesh>(Foliage))
			{
				R["type"] = "instanced_static_mesh";
				if (ISM->Mesh)
					R["mesh"] = std::string(TCHAR_TO_UTF8(*ISM->Mesh->GetPathName()));

				// Override materials
				if (ISM->OverrideMaterials.Num() > 0)
				{
					sol::table Mats = Lua.create_table();
					for (int32 i = 0; i < ISM->OverrideMaterials.Num(); i++)
					{
						if (ISM->OverrideMaterials[i])
							Mats[i + 1] = std::string(TCHAR_TO_UTF8(*ISM->OverrideMaterials[i]->GetPathName()));
					}
					R["override_materials"] = Mats;
				}

				if (ISM->NaniteOverrideMaterials.Num() > 0)
				{
					sol::table Mats = Lua.create_table();
					for (int32 i = 0; i < ISM->NaniteOverrideMaterials.Num(); i++)
					{
						if (ISM->NaniteOverrideMaterials[i])
							Mats[i + 1] = std::string(TCHAR_TO_UTF8(*ISM->NaniteOverrideMaterials[i]->GetPathName()));
					}
					R["nanite_override_materials"] = Mats;
				}
			}
			else if (UFoliageType_Actor* ActorFoliage = Cast<UFoliageType_Actor>(Foliage))
			{
				R["type"] = "actor";
				if (ActorFoliage->ActorClass)
					R["actor_class"] = std::string(TCHAR_TO_UTF8(*ActorFoliage->ActorClass->GetPathName()));
				R["attach_to_base_component"] = ActorFoliage->bShouldAttachToBaseComponent;
				R["static_mesh_only"] = ActorFoliage->bStaticMeshOnly;
			}
			else
			{
				R["type"] = "unknown";
			}

			// Painting
			R["density"] = Foliage->Density;
			R["density_adjustment_factor"] = Foliage->DensityAdjustmentFactor;
			R["radius"] = Foliage->Radius;
			R["scaling"] = ScalingToString(Foliage->Scaling);
			R["scale_x"] = IntervalToTable(Lua, Foliage->ScaleX);
			R["scale_y"] = IntervalToTable(Lua, Foliage->ScaleY);
			R["scale_z"] = IntervalToTable(Lua, Foliage->ScaleZ);

			// Placement
			R["z_offset"] = IntervalToTable(Lua, Foliage->ZOffset);
			R["align_to_normal"] = static_cast<bool>(Foliage->AlignToNormal);
			R["average_normal"] = static_cast<bool>(Foliage->AverageNormal);
			R["align_max_angle"] = Foliage->AlignMaxAngle;
			R["random_yaw"] = static_cast<bool>(Foliage->RandomYaw);
			R["random_pitch_angle"] = Foliage->RandomPitchAngle;
			R["ground_slope_angle"] = IntervalToTable(Lua, Foliage->GroundSlopeAngle);
			R["height"] = IntervalToTable(Lua, Foliage->Height);
			R["collision_with_world"] = static_cast<bool>(Foliage->CollisionWithWorld);
			{
				sol::table CS = Lua.create_table();
				CS["x"] = Foliage->CollisionScale.X;
				CS["y"] = Foliage->CollisionScale.Y;
				CS["z"] = Foliage->CollisionScale.Z;
				R["collision_scale"] = CS;
			}

			// Landscape layers
			if (Foliage->LandscapeLayers.Num() > 0)
			{
				sol::table Layers = Lua.create_table();
				for (int32 i = 0; i < Foliage->LandscapeLayers.Num(); i++)
					Layers[i + 1] = std::string(TCHAR_TO_UTF8(*Foliage->LandscapeLayers[i].ToString()));
				R["landscape_layers"] = Layers;
			}
			if (Foliage->ExclusionLandscapeLayers.Num() > 0)
			{
				sol::table Layers = Lua.create_table();
				for (int32 i = 0; i < Foliage->ExclusionLandscapeLayers.Num(); i++)
					Layers[i + 1] = std::string(TCHAR_TO_UTF8(*Foliage->ExclusionLandscapeLayers[i].ToString()));
				R["exclusion_landscape_layers"] = Layers;
			}

			// Instance Settings
			R["mobility"] = MobilityToString(Foliage->Mobility.GetValue());
			R["cull_distance"] = IntIntervalToTable(Lua, Foliage->CullDistance);
			R["cast_shadow"] = static_cast<bool>(Foliage->CastShadow);
			R["cast_dynamic_shadow"] = static_cast<bool>(Foliage->bCastDynamicShadow);
			R["cast_static_shadow"] = static_cast<bool>(Foliage->bCastStaticShadow);
			R["cast_contact_shadow"] = static_cast<bool>(Foliage->bCastContactShadow);
			R["cast_shadow_as_two_sided"] = static_cast<bool>(Foliage->bCastShadowAsTwoSided);
			R["receives_decals"] = static_cast<bool>(Foliage->bReceivesDecals);
			R["affect_dynamic_indirect_lighting"] = static_cast<bool>(Foliage->bAffectDynamicIndirectLighting);
			R["affect_distance_field_lighting"] = static_cast<bool>(Foliage->bAffectDistanceFieldLighting);
			R["use_as_occluder"] = static_cast<bool>(Foliage->bUseAsOccluder);
			R["render_custom_depth"] = static_cast<bool>(Foliage->bRenderCustomDepth);
			R["custom_depth_stencil_value"] = Foliage->CustomDepthStencilValue;
			R["translucency_sort_priority"] = Foliage->TranslucencySortPriority;

			// Scalability
			R["enable_density_scaling"] = static_cast<bool>(Foliage->bEnableDensityScaling);
			R["enable_discard_on_load"] = static_cast<bool>(Foliage->bEnableDiscardOnLoad);
			R["enable_cull_distance_scaling"] = static_cast<bool>(Foliage->bEnableCullDistanceScaling);

			// Ray tracing & WPO
			R["visible_in_ray_tracing"] = static_cast<bool>(Foliage->bVisibleInRayTracing);
			R["evaluate_world_position_offset"] = static_cast<bool>(Foliage->bEvaluateWorldPositionOffset);
			R["world_position_offset_disable_distance"] = Foliage->WorldPositionOffsetDisableDistance;

			// Placement extras
			R["average_normal_single_component"] = static_cast<bool>(Foliage->AverageNormalSingleComponent);
			R["average_normal_sample_count"] = Foliage->AverageNormalSampleCount;
			R["minimum_layer_weight"] = Foliage->MinimumLayerWeight;
			R["minimum_exclusion_layer_weight"] = Foliage->MinimumExclusionLayerWeight;

			// Lightmap
			R["override_lightmap_res"] = static_cast<bool>(Foliage->bOverrideLightMapRes);
			R["overridden_lightmap_res"] = Foliage->OverriddenLightMapRes;

			// Lighting channels
			{
				sol::table LC = Lua.create_table();
				LC["channel_0"] = static_cast<bool>(Foliage->LightingChannels.bChannel0);
				LC["channel_1"] = static_cast<bool>(Foliage->LightingChannels.bChannel1);
				LC["channel_2"] = static_cast<bool>(Foliage->LightingChannels.bChannel2);
				R["lighting_channels"] = LC;
			}

			Session.Log(FString::Printf(TEXT("[OK] info() -> FoliageType, density=%.1f, radius=%.1f"),
				Foliage->Density, Foliage->Radius));
			return R;
		});

		// ================================================================
		// configure(params)
		// ================================================================
		AssetObj.set_function("configure", [Foliage, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Foliage))
			{
				Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
				return sol::lua_nil;
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("FoliageType: Configure")));
			Foliage->Modify();
			bool bModified = false;
			FString Changes;

			// ---- Mesh (only for InstancedStaticMesh subtype) ----
			sol::optional<std::string> MeshPath = Params.get<sol::optional<std::string>>("mesh");
			if (MeshPath.has_value())
			{
				if (UFoliageType_InstancedStaticMesh* ISM = Cast<UFoliageType_InstancedStaticMesh>(Foliage))
				{
					FString MPath = UTF8_TO_TCHAR(MeshPath.value().c_str());
					UStaticMesh* NewMesh = LoadObject<UStaticMesh>(nullptr, *MPath);
					if (NewMesh)
					{
						ISM->SetStaticMesh(NewMesh); // calls UpdateBounds()
						Changes += TEXT(" mesh=") + MPath;
						bModified = true;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure: mesh '%s' not found"), *MPath));
					}
				}
			}

			// ---- Actor foliage type settings ----
			if (UFoliageType_Actor* ActorFoliage = Cast<UFoliageType_Actor>(Foliage))
			{
				sol::optional<std::string> ActorClassPath = Params.get<sol::optional<std::string>>("actor_class");
				if (ActorClassPath.has_value())
				{
					FString APath = UTF8_TO_TCHAR(ActorClassPath.value().c_str());
					UClass* NewClass = LoadObject<UClass>(nullptr, *APath);
					if (NewClass && NewClass->IsChildOf(AActor::StaticClass()))
					{
						ActorFoliage->ActorClass = NewClass;
						ActorFoliage->UpdateBounds();
						Changes += TEXT(" actor_class=") + APath;
						bModified = true;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure: actor_class '%s' not found or not an Actor"), *APath));
					}
				}

				sol::optional<bool> AttachBase = Params.get<sol::optional<bool>>("attach_to_base_component");
				if (AttachBase.has_value())
				{
					ActorFoliage->bShouldAttachToBaseComponent = AttachBase.value();
					Changes += FString::Printf(TEXT(" attach_to_base_component=%s"), AttachBase.value() ? TEXT("true") : TEXT("false"));
					bModified = true;
				}

				sol::optional<bool> StaticMeshOnly = Params.get<sol::optional<bool>>("static_mesh_only");
				if (StaticMeshOnly.has_value())
				{
					ActorFoliage->bStaticMeshOnly = StaticMeshOnly.value();
					Changes += FString::Printf(TEXT(" static_mesh_only=%s"), StaticMeshOnly.value() ? TEXT("true") : TEXT("false"));
					bModified = true;
				}
			}

			// ---- Painting ----
			sol::optional<double> DensityVal = Params.get<sol::optional<double>>("density");
			if (DensityVal.has_value())
			{
				Foliage->Density = FMath::Max(0.0f, static_cast<float>(DensityVal.value()));
				Changes += FString::Printf(TEXT(" density=%.1f"), (double)Foliage->Density);
				bModified = true;
			}

			sol::optional<double> DensityAdj = Params.get<sol::optional<double>>("density_adjustment_factor");
			if (DensityAdj.has_value())
			{
				Foliage->DensityAdjustmentFactor = FMath::Max(0.0f, static_cast<float>(DensityAdj.value()));
				Changes += FString::Printf(TEXT(" density_adj=%.2f"), (double)Foliage->DensityAdjustmentFactor);
				bModified = true;
			}

			sol::optional<double> RadiusVal = Params.get<sol::optional<double>>("radius");
			if (RadiusVal.has_value())
			{
				Foliage->Radius = FMath::Max(0.0f, static_cast<float>(RadiusVal.value()));
				Changes += FString::Printf(TEXT(" radius=%.1f"), (double)Foliage->Radius);
				bModified = true;
			}

			sol::optional<std::string> ScalingStr = Params.get<sol::optional<std::string>>("scaling");
			if (ScalingStr.has_value())
			{
				Foliage->Scaling = StringToScaling(ScalingStr.value());
				Changes += FString::Printf(TEXT(" scaling=%s"), UTF8_TO_TCHAR(ScalingStr.value().c_str()));
				bModified = true;
			}

			// Scale intervals
			sol::optional<sol::table> SX = Params.get<sol::optional<sol::table>>("scale_x");
			if (SX.has_value() && ReadInterval(SX.value(), Foliage->ScaleX))
			{
				Changes += FString::Printf(TEXT(" scale_x=[%.2f,%.2f]"), Foliage->ScaleX.Min, Foliage->ScaleX.Max);
				bModified = true;
			}
			sol::optional<sol::table> SY = Params.get<sol::optional<sol::table>>("scale_y");
			if (SY.has_value() && ReadInterval(SY.value(), Foliage->ScaleY))
			{
				Changes += FString::Printf(TEXT(" scale_y=[%.2f,%.2f]"), Foliage->ScaleY.Min, Foliage->ScaleY.Max);
				bModified = true;
			}
			sol::optional<sol::table> SZ = Params.get<sol::optional<sol::table>>("scale_z");
			if (SZ.has_value() && ReadInterval(SZ.value(), Foliage->ScaleZ))
			{
				Changes += FString::Printf(TEXT(" scale_z=[%.2f,%.2f]"), Foliage->ScaleZ.Min, Foliage->ScaleZ.Max);
				bModified = true;
			}

			// Z offset
			sol::optional<sol::table> ZOff = Params.get<sol::optional<sol::table>>("z_offset");
			if (ZOff.has_value() && ReadInterval(ZOff.value(), Foliage->ZOffset))
			{
				Changes += FString::Printf(TEXT(" z_offset=[%.1f,%.1f]"), Foliage->ZOffset.Min, Foliage->ZOffset.Max);
				bModified = true;
			}

			// ---- Placement bools/floats ----
			sol::optional<bool> AlignNormal = Params.get<sol::optional<bool>>("align_to_normal");
			if (AlignNormal.has_value())
			{
				Foliage->AlignToNormal = AlignNormal.value();
				Changes += FString::Printf(TEXT(" align_to_normal=%s"), AlignNormal.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			sol::optional<double> AlignMax = Params.get<sol::optional<double>>("align_max_angle");
			if (AlignMax.has_value())
			{
				Foliage->AlignMaxAngle = FMath::Clamp(static_cast<float>(AlignMax.value()), 0.0f, 359.0f);
				Changes += FString::Printf(TEXT(" align_max_angle=%.1f"), (double)Foliage->AlignMaxAngle);
				bModified = true;
			}

			sol::optional<bool> RandYaw = Params.get<sol::optional<bool>>("random_yaw");
			if (RandYaw.has_value())
			{
				Foliage->RandomYaw = RandYaw.value();
				Changes += FString::Printf(TEXT(" random_yaw=%s"), RandYaw.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			sol::optional<double> RandPitch = Params.get<sol::optional<double>>("random_pitch_angle");
			if (RandPitch.has_value())
			{
				Foliage->RandomPitchAngle = FMath::Clamp(static_cast<float>(RandPitch.value()), 0.0f, 359.0f);
				Changes += FString::Printf(TEXT(" random_pitch_angle=%.1f"), (double)Foliage->RandomPitchAngle);
				bModified = true;
			}

			sol::optional<bool> AvgNormal = Params.get<sol::optional<bool>>("average_normal");
			if (AvgNormal.has_value())
			{
				Foliage->AverageNormal = AvgNormal.value();
				Changes += FString::Printf(TEXT(" average_normal=%s"), AvgNormal.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			// Ground slope angle
			sol::optional<sol::table> GSA = Params.get<sol::optional<sol::table>>("ground_slope_angle");
			if (GSA.has_value() && ReadInterval(GSA.value(), Foliage->GroundSlopeAngle))
			{
				Changes += FString::Printf(TEXT(" ground_slope=[%.1f,%.1f]"), Foliage->GroundSlopeAngle.Min, Foliage->GroundSlopeAngle.Max);
				bModified = true;
			}

			// Height
			sol::optional<sol::table> HeightVal = Params.get<sol::optional<sol::table>>("height");
			if (HeightVal.has_value() && ReadInterval(HeightVal.value(), Foliage->Height))
			{
				Changes += FString::Printf(TEXT(" height=[%.1f,%.1f]"), Foliage->Height.Min, Foliage->Height.Max);
				bModified = true;
			}

			// Average normal extras
			sol::optional<bool> AvgNormSingle = Params.get<sol::optional<bool>>("average_normal_single_component");
			if (AvgNormSingle.has_value())
			{
				Foliage->AverageNormalSingleComponent = AvgNormSingle.value();
				Changes += FString::Printf(TEXT(" average_normal_single_component=%s"), AvgNormSingle.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			sol::optional<int> AvgNormCount = Params.get<sol::optional<int>>("average_normal_sample_count");
			if (AvgNormCount.has_value())
			{
				Foliage->AverageNormalSampleCount = FMath::Max(0, AvgNormCount.value());
				Changes += FString::Printf(TEXT(" average_normal_sample_count=%d"), Foliage->AverageNormalSampleCount);
				bModified = true;
			}

			// Landscape layers
			sol::optional<sol::table> LLayers = Params.get<sol::optional<sol::table>>("landscape_layers");
			if (LLayers.has_value())
			{
				Foliage->LandscapeLayers.Empty();
				sol::table Arr = LLayers.value();
				for (int32 i = 1; i <= static_cast<int32>(Arr.size()); i++)
				{
					sol::optional<std::string> Name = Arr.get<sol::optional<std::string>>(i);
					if (Name.has_value())
						Foliage->LandscapeLayers.Add(FName(UTF8_TO_TCHAR(Name.value().c_str())));
				}
				Changes += FString::Printf(TEXT(" landscape_layers=%d"), Foliage->LandscapeLayers.Num());
				bModified = true;
			}

			sol::optional<sol::table> ELLayers = Params.get<sol::optional<sol::table>>("exclusion_landscape_layers");
			if (ELLayers.has_value())
			{
				Foliage->ExclusionLandscapeLayers.Empty();
				sol::table Arr = ELLayers.value();
				for (int32 i = 1; i <= static_cast<int32>(Arr.size()); i++)
				{
					sol::optional<std::string> Name = Arr.get<sol::optional<std::string>>(i);
					if (Name.has_value())
						Foliage->ExclusionLandscapeLayers.Add(FName(UTF8_TO_TCHAR(Name.value().c_str())));
				}
				Changes += FString::Printf(TEXT(" exclusion_landscape_layers=%d"), Foliage->ExclusionLandscapeLayers.Num());
				bModified = true;
			}

			sol::optional<double> MinLayerWt = Params.get<sol::optional<double>>("minimum_layer_weight");
			if (MinLayerWt.has_value())
			{
				Foliage->MinimumLayerWeight = FMath::Clamp(static_cast<float>(MinLayerWt.value()), 0.0f, 1.0f);
				Changes += FString::Printf(TEXT(" minimum_layer_weight=%.2f"), Foliage->MinimumLayerWeight);
				bModified = true;
			}

			sol::optional<double> MinExclWt = Params.get<sol::optional<double>>("minimum_exclusion_layer_weight");
			if (MinExclWt.has_value())
			{
				Foliage->MinimumExclusionLayerWeight = FMath::Clamp(static_cast<float>(MinExclWt.value()), 0.0f, 1.0f);
				Changes += FString::Printf(TEXT(" minimum_exclusion_layer_weight=%.2f"), Foliage->MinimumExclusionLayerWeight);
				bModified = true;
			}

			// Collision with world
			sol::optional<bool> CollWorld = Params.get<sol::optional<bool>>("collision_with_world");
			if (CollWorld.has_value())
			{
				Foliage->CollisionWithWorld = CollWorld.value();
				Changes += FString::Printf(TEXT(" collision_with_world=%s"), CollWorld.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			// Collision scale
			sol::optional<sol::table> CollScale = Params.get<sol::optional<sol::table>>("collision_scale");
			if (CollScale.has_value())
			{
				sol::table CS = CollScale.value();
				sol::optional<double> CX = CS.get<sol::optional<double>>("x");
				sol::optional<double> CY = CS.get<sol::optional<double>>("y");
				sol::optional<double> CZ = CS.get<sol::optional<double>>("z");
				if (CX.has_value()) Foliage->CollisionScale.X = CX.value();
				if (CY.has_value()) Foliage->CollisionScale.Y = CY.value();
				if (CZ.has_value()) Foliage->CollisionScale.Z = CZ.value();
				Changes += FString::Printf(TEXT(" collision_scale=(%.2f,%.2f,%.2f)"),
					Foliage->CollisionScale.X, Foliage->CollisionScale.Y, Foliage->CollisionScale.Z);
				bModified = true;
			}

			// ---- Instance Settings ----
			sol::optional<std::string> MobilityStr = Params.get<sol::optional<std::string>>("mobility");
			if (MobilityStr.has_value())
			{
				Foliage->Mobility = StringToMobility(MobilityStr.value());
				Changes += FString::Printf(TEXT(" mobility=%s"), UTF8_TO_TCHAR(MobilityStr.value().c_str()));
				bModified = true;
			}

			sol::optional<sol::table> CullDist = Params.get<sol::optional<sol::table>>("cull_distance");
			if (CullDist.has_value() && ReadIntInterval(CullDist.value(), Foliage->CullDistance))
			{
				Changes += FString::Printf(TEXT(" cull_distance=[%d,%d]"), Foliage->CullDistance.Min, Foliage->CullDistance.Max);
				bModified = true;
			}

			// Shadow & rendering bools (bit-fields can't be referenced, use FBoolProperty reflection)
			auto SetBoolProp = [&](const char* Key, const TCHAR* PropName, const TCHAR* LogName)
			{
				sol::optional<bool> Val = Params.get<sol::optional<bool>>(Key);
				if (Val.has_value())
				{
					FBoolProperty* Prop = CastField<FBoolProperty>(Foliage->GetClass()->FindPropertyByName(PropName));
					if (Prop)
					{
						Prop->SetPropertyValue_InContainer(Foliage, Val.value());
						Changes += FString::Printf(TEXT(" %s=%s"), LogName, Val.value() ? TEXT("true") : TEXT("false"));
						bModified = true;
					}
				}
			};

			// CastShadow is a uint32 bit-field, not FBoolProperty — set directly
			if (auto V = Params.get<sol::optional<bool>>("cast_shadow"))
			{
				Foliage->CastShadow = V.value() ? 1 : 0;
				Changes += FString::Printf(TEXT(" cast_shadow=%s"), V.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}
			SetBoolProp("cast_dynamic_shadow", TEXT("bCastDynamicShadow"), TEXT("cast_dynamic_shadow"));
			SetBoolProp("cast_static_shadow", TEXT("bCastStaticShadow"), TEXT("cast_static_shadow"));
			SetBoolProp("cast_contact_shadow", TEXT("bCastContactShadow"), TEXT("cast_contact_shadow"));
			SetBoolProp("cast_shadow_as_two_sided", TEXT("bCastShadowAsTwoSided"), TEXT("cast_shadow_as_two_sided"));
			SetBoolProp("receives_decals", TEXT("bReceivesDecals"), TEXT("receives_decals"));
			SetBoolProp("affect_dynamic_indirect_lighting", TEXT("bAffectDynamicIndirectLighting"), TEXT("affect_dynamic_indirect_lighting"));
			SetBoolProp("affect_distance_field_lighting", TEXT("bAffectDistanceFieldLighting"), TEXT("affect_distance_field_lighting"));
			SetBoolProp("use_as_occluder", TEXT("bUseAsOccluder"), TEXT("use_as_occluder"));
			SetBoolProp("render_custom_depth", TEXT("bRenderCustomDepth"), TEXT("render_custom_depth"));
			SetBoolProp("enable_density_scaling", TEXT("bEnableDensityScaling"), TEXT("enable_density_scaling"));
			SetBoolProp("enable_discard_on_load", TEXT("bEnableDiscardOnLoad"), TEXT("enable_discard_on_load"));
			SetBoolProp("enable_cull_distance_scaling", TEXT("bEnableCullDistanceScaling"), TEXT("enable_cull_distance_scaling"));

			sol::optional<int> StencilVal = Params.get<sol::optional<int>>("custom_depth_stencil_value");
			if (StencilVal.has_value())
			{
				Foliage->CustomDepthStencilValue = FMath::Clamp(StencilVal.value(), 0, 255);
				Changes += FString::Printf(TEXT(" custom_depth_stencil_value=%d"), Foliage->CustomDepthStencilValue);
				bModified = true;
			}

			sol::optional<int> TransSort = Params.get<sol::optional<int>>("translucency_sort_priority");
			if (TransSort.has_value())
			{
				Foliage->TranslucencySortPriority = TransSort.value();
				Changes += FString::Printf(TEXT(" translucency_sort_priority=%d"), Foliage->TranslucencySortPriority);
				bModified = true;
			}

			// Ray tracing & WPO
			SetBoolProp("visible_in_ray_tracing", TEXT("bVisibleInRayTracing"), TEXT("visible_in_ray_tracing"));
			SetBoolProp("evaluate_world_position_offset", TEXT("bEvaluateWorldPositionOffset"), TEXT("evaluate_world_position_offset"));

			sol::optional<int> WPODist = Params.get<sol::optional<int>>("world_position_offset_disable_distance");
			if (WPODist.has_value())
			{
				Foliage->WorldPositionOffsetDisableDistance = FMath::Max(0, WPODist.value());
				Changes += FString::Printf(TEXT(" wpo_disable_distance=%d"), Foliage->WorldPositionOffsetDisableDistance);
				bModified = true;
			}

			// Lightmap
			SetBoolProp("override_lightmap_res", TEXT("bOverrideLightMapRes"), TEXT("override_lightmap_res"));

			sol::optional<int> LightmapRes = Params.get<sol::optional<int>>("overridden_lightmap_res");
			if (LightmapRes.has_value())
			{
				Foliage->OverriddenLightMapRes = FMath::Max(4, (LightmapRes.value() + 3) & ~3); // must be factor of 4
				Changes += FString::Printf(TEXT(" overridden_lightmap_res=%d"), Foliage->OverriddenLightMapRes);
				bModified = true;
			}

			// Lighting channels
			sol::optional<sol::table> LCTable = Params.get<sol::optional<sol::table>>("lighting_channels");
			if (LCTable.has_value())
			{
				sol::table LC = LCTable.value();
				sol::optional<bool> Ch0 = LC.get<sol::optional<bool>>("channel_0");
				sol::optional<bool> Ch1 = LC.get<sol::optional<bool>>("channel_1");
				sol::optional<bool> Ch2 = LC.get<sol::optional<bool>>("channel_2");
				if (Ch0.has_value()) Foliage->LightingChannels.bChannel0 = Ch0.value();
				if (Ch1.has_value()) Foliage->LightingChannels.bChannel1 = Ch1.value();
				if (Ch2.has_value()) Foliage->LightingChannels.bChannel2 = Ch2.value();
				Changes += TEXT(" lighting_channels");
				bModified = true;
			}

			if (bModified)
			{
				FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
				Foliage->PostEditChangeProperty(Event);
				Foliage->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(%s)"), *Changes.TrimStart()));
				return sol::make_object(Lua, true);
			}

			Session.Log(TEXT("[OK] configure() -> nothing changed. Use help() to see valid keys."));
			return sol::make_object(Lua, true);
		});
	});
}

// ============================================================================
// ULandscapeGrassType ENRICHMENT
// ============================================================================

static TArray<FLuaFunctionDoc> LandscapeGrassTypeDocs = {};

static void BindLandscapeGrassType(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_landscape_grass_type", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		ULandscapeGrassType* GrassType = LoadObject<ULandscapeGrassType>(nullptr, *FPath);
		if (!GrassType) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"LandscapeGrassType enrichment methods:\n"
			"\n"
			"info() — structured summary:\n"
			"  variety_count, enable_density_scaling,\n"
			"  varieties[] = { mesh, density, use_grid, placement_jitter,\n"
			"    start_cull_distance, end_cull_distance, min_lod,\n"
			"    scaling (uniform|free|lock_xy), scale_x/y/z {min,max},\n"
			"    random_rotation, align_to_surface, use_landscape_lightmap,\n"
			"    receives_decals, cast_dynamic_shadow, cast_contact_shadow,\n"
			"    affect_distance_field_lighting, keep_instance_buffer_cpu_copy,\n"
			"    wpo_disable_distance, allowed_density_range {min,max},\n"
			"    override_materials }\n"
			"\n"
			"list() — short variety summary: index, mesh name, density\n"
			"\n"
			"configure(params) — set grass type settings:\n"
			"  enable_density_scaling (bool)\n"
			"  variety_index (int, 0-based) + any variety field:\n"
			"    mesh (string path), density (float), use_grid (bool),\n"
			"    placement_jitter (float), start_cull_distance (int),\n"
			"    end_cull_distance (int), min_lod (int),\n"
			"    scaling (string), scale_x/y/z ({min,max}),\n"
			"    random_rotation (bool), align_to_surface (bool),\n"
			"    use_landscape_lightmap (bool), receives_decals (bool),\n"
			"    cast_dynamic_shadow (bool), cast_contact_shadow (bool),\n"
			"    affect_distance_field_lighting (bool),\n"
			"    keep_instance_buffer_cpu_copy (bool), wpo_disable_distance (int),\n"
			"    allowed_density_range ({min,max} floats 0-1)\n"
			"\n"
			"add(params) — add a new grass variety:\n"
			"  mesh (string path, required), density (float),\n"
			"  plus any other variety field from configure\n"
			"\n"
			"remove(params) — remove a grass variety:\n"
			"  index (int, 0-based, required)\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [GrassType, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(GrassType))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table R = Lua.create_table();
			R["variety_count"] = GrassType->GrassVarieties.Num();
			R["enable_density_scaling"] = static_cast<bool>(GrassType->bEnableDensityScaling);

			sol::table Varieties = Lua.create_table();
			for (int32 i = 0; i < GrassType->GrassVarieties.Num(); i++)
			{
				const FGrassVariety& V = GrassType->GrassVarieties[i];
				sol::table VT = Lua.create_table();

				if (V.GrassMesh)
					VT["mesh"] = std::string(TCHAR_TO_UTF8(*V.GrassMesh->GetPathName()));
				VT["density"] = V.GrassDensity.Default;
				VT["use_grid"] = V.bUseGrid;
				VT["placement_jitter"] = V.PlacementJitter;
				VT["start_cull_distance"] = V.StartCullDistance.Default;
				VT["end_cull_distance"] = V.EndCullDistance.Default;
				VT["min_lod"] = V.MinLOD;

				VT["scaling"] = GrassScalingToString(V.Scaling);
				VT["scale_x"] = IntervalToTable(Lua, V.ScaleX);
				VT["scale_y"] = IntervalToTable(Lua, V.ScaleY);
				VT["scale_z"] = IntervalToTable(Lua, V.ScaleZ);

				VT["random_rotation"] = V.RandomRotation;
				VT["align_to_surface"] = V.AlignToSurface;
				VT["use_landscape_lightmap"] = V.bUseLandscapeLightmap;
				VT["receives_decals"] = V.bReceivesDecals;
				VT["cast_dynamic_shadow"] = V.bCastDynamicShadow;
				VT["cast_contact_shadow"] = V.bCastContactShadow;
				VT["affect_distance_field_lighting"] = V.bAffectDistanceFieldLighting;
				VT["keep_instance_buffer_cpu_copy"] = V.bKeepInstanceBufferCPUCopy;
				VT["wpo_disable_distance"] = static_cast<int32>(V.InstanceWorldPositionOffsetDisableDistance);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				VT["allowed_density_range"] = IntervalToTable(Lua, V.AllowedDensityRange);
#endif

				if (V.OverrideMaterials.Num() > 0)
				{
					sol::table Mats = Lua.create_table();
					for (int32 j = 0; j < V.OverrideMaterials.Num(); j++)
					{
						if (V.OverrideMaterials[j])
							Mats[j + 1] = std::string(TCHAR_TO_UTF8(*V.OverrideMaterials[j]->GetPathName()));
					}
					VT["override_materials"] = Mats;
				}

				Varieties[i + 1] = VT;
			}
			R["varieties"] = Varieties;

			Session.Log(FString::Printf(TEXT("[OK] info() -> LandscapeGrassType, %d varieties"),
				GrassType->GrassVarieties.Num()));
			return R;
		});

		// ================================================================
		// list()
		// ================================================================
		AssetObj.set_function("list", [GrassType, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(GrassType))
			{
				Session.Log(TEXT("[FAIL] list -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table R = Lua.create_table();
			for (int32 i = 0; i < GrassType->GrassVarieties.Num(); i++)
			{
				const FGrassVariety& V = GrassType->GrassVarieties[i];
				sol::table VT = Lua.create_table();
				VT["index"] = i;
				VT["mesh"] = V.GrassMesh ? std::string(TCHAR_TO_UTF8(*V.GrassMesh->GetName())) : std::string("(none)");
				VT["density"] = V.GrassDensity.Default;
				R[i + 1] = VT;
			}

			Session.Log(FString::Printf(TEXT("[OK] list() -> %d varieties"), GrassType->GrassVarieties.Num()));
			return R;
		});

		// ================================================================
		// configure(params) — modify existing variety or top-level settings
		// ================================================================
		AssetObj.set_function("configure", [GrassType, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(GrassType))
			{
				Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
				return sol::lua_nil;
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("LandscapeGrassType: Configure")));
			GrassType->Modify();
			bool bModified = false;
			FString Changes;

			// Top-level settings
			sol::optional<bool> DensityScaling = Params.get<sol::optional<bool>>("enable_density_scaling");
			if (DensityScaling.has_value())
			{
				GrassType->bEnableDensityScaling = DensityScaling.value();
				Changes += FString::Printf(TEXT(" enable_density_scaling=%s"), DensityScaling.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			// Per-variety configuration
			sol::optional<int> VarIdx = Params.get<sol::optional<int>>("variety_index");
			if (VarIdx.has_value())
			{
				int32 Idx = VarIdx.value();
				if (Idx < 0 || Idx >= GrassType->GrassVarieties.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure: variety_index %d out of range [0,%d)"),
						Idx, GrassType->GrassVarieties.Num()));
					return sol::lua_nil;
				}

				FGrassVariety& V = GrassType->GrassVarieties[Idx];

				// Mesh
				sol::optional<std::string> MeshPath = Params.get<sol::optional<std::string>>("mesh");
				if (MeshPath.has_value())
				{
					FString MPath = UTF8_TO_TCHAR(MeshPath.value().c_str());
					UStaticMesh* NewMesh = LoadObject<UStaticMesh>(nullptr, *MPath);
					if (NewMesh)
					{
						V.GrassMesh = NewMesh;
						Changes += TEXT(" mesh=") + MPath;
						bModified = true;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure: mesh '%s' not found"), *MPath));
					}
				}

				// Density
				sol::optional<double> DensityVal = Params.get<sol::optional<double>>("density");
				if (DensityVal.has_value())
				{
					V.GrassDensity.Default = FMath::Max(0.0f, static_cast<float>(DensityVal.value()));
					Changes += FString::Printf(TEXT(" density=%.1f"), (double)V.GrassDensity.Default);
					bModified = true;
				}

				// Use grid
				sol::optional<bool> UseGrid = Params.get<sol::optional<bool>>("use_grid");
				if (UseGrid.has_value())
				{
					V.bUseGrid = UseGrid.value();
					Changes += FString::Printf(TEXT(" use_grid=%s"), UseGrid.value() ? TEXT("true") : TEXT("false"));
					bModified = true;
				}

				// Placement jitter
				sol::optional<double> Jitter = Params.get<sol::optional<double>>("placement_jitter");
				if (Jitter.has_value())
				{
					V.PlacementJitter = FMath::Clamp(static_cast<float>(Jitter.value()), 0.0f, 1.0f);
					Changes += FString::Printf(TEXT(" placement_jitter=%.2f"), (double)V.PlacementJitter);
					bModified = true;
				}

				// Cull distances
				sol::optional<int> StartCull = Params.get<sol::optional<int>>("start_cull_distance");
				if (StartCull.has_value())
				{
					V.StartCullDistance.Default = FMath::Max(0, StartCull.value());
					Changes += FString::Printf(TEXT(" start_cull_distance=%d"), V.StartCullDistance.Default);
					bModified = true;
				}

				sol::optional<int> EndCull = Params.get<sol::optional<int>>("end_cull_distance");
				if (EndCull.has_value())
				{
					V.EndCullDistance.Default = FMath::Max(0, EndCull.value());
					Changes += FString::Printf(TEXT(" end_cull_distance=%d"), V.EndCullDistance.Default);
					bModified = true;
				}

				// MinLOD
				sol::optional<int> MinLOD = Params.get<sol::optional<int>>("min_lod");
				if (MinLOD.has_value())
				{
					V.MinLOD = FMath::Clamp(MinLOD.value(), -1, 8);
					Changes += FString::Printf(TEXT(" min_lod=%d"), V.MinLOD);
					bModified = true;
				}

				// Scaling
				sol::optional<std::string> ScalingStr = Params.get<sol::optional<std::string>>("scaling");
				if (ScalingStr.has_value())
				{
					V.Scaling = StringToGrassScaling(ScalingStr.value());
					Changes += FString::Printf(TEXT(" scaling=%s"), UTF8_TO_TCHAR(ScalingStr.value().c_str()));
					bModified = true;
				}

				// Scale intervals
				sol::optional<sol::table> SX = Params.get<sol::optional<sol::table>>("scale_x");
				if (SX.has_value() && ReadInterval(SX.value(), V.ScaleX))
				{
					Changes += FString::Printf(TEXT(" scale_x=[%.2f,%.2f]"), V.ScaleX.Min, V.ScaleX.Max);
					bModified = true;
				}
				sol::optional<sol::table> SY = Params.get<sol::optional<sol::table>>("scale_y");
				if (SY.has_value() && ReadInterval(SY.value(), V.ScaleY))
				{
					Changes += FString::Printf(TEXT(" scale_y=[%.2f,%.2f]"), V.ScaleY.Min, V.ScaleY.Max);
					bModified = true;
				}
				sol::optional<sol::table> SZVal = Params.get<sol::optional<sol::table>>("scale_z");
				if (SZVal.has_value() && ReadInterval(SZVal.value(), V.ScaleZ))
				{
					Changes += FString::Printf(TEXT(" scale_z=[%.2f,%.2f]"), V.ScaleZ.Min, V.ScaleZ.Max);
					bModified = true;
				}

				// Bool fields
				auto SetBool = [&](const char* Key, bool& Field, const TCHAR* LogName)
				{
					sol::optional<bool> Val = Params.get<sol::optional<bool>>(Key);
					if (Val.has_value())
					{
						Field = Val.value();
						Changes += FString::Printf(TEXT(" %s=%s"), LogName, Val.value() ? TEXT("true") : TEXT("false"));
						bModified = true;
					}
				};

				SetBool("random_rotation", V.RandomRotation, TEXT("random_rotation"));
				SetBool("align_to_surface", V.AlignToSurface, TEXT("align_to_surface"));
				SetBool("use_landscape_lightmap", V.bUseLandscapeLightmap, TEXT("use_landscape_lightmap"));
				SetBool("receives_decals", V.bReceivesDecals, TEXT("receives_decals"));
				SetBool("cast_dynamic_shadow", V.bCastDynamicShadow, TEXT("cast_dynamic_shadow"));
				SetBool("cast_contact_shadow", V.bCastContactShadow, TEXT("cast_contact_shadow"));
				SetBool("affect_distance_field_lighting", V.bAffectDistanceFieldLighting, TEXT("affect_distance_field_lighting"));
				SetBool("keep_instance_buffer_cpu_copy", V.bKeepInstanceBufferCPUCopy, TEXT("keep_instance_buffer_cpu_copy"));

				sol::optional<int> WPODist = Params.get<sol::optional<int>>("wpo_disable_distance");
				if (WPODist.has_value())
				{
					V.InstanceWorldPositionOffsetDisableDistance = static_cast<uint32>(FMath::Max(0, WPODist.value()));
					Changes += FString::Printf(TEXT(" wpo_disable_distance=%u"), V.InstanceWorldPositionOffsetDisableDistance);
					bModified = true;
				}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				sol::optional<sol::table> AllowedDR = Params.get<sol::optional<sol::table>>("allowed_density_range");
				if (AllowedDR.has_value() && ReadInterval(AllowedDR.value(), V.AllowedDensityRange))
				{
					Changes += FString::Printf(TEXT(" allowed_density_range=[%.2f,%.2f]"), V.AllowedDensityRange.Min, V.AllowedDensityRange.Max);
					bModified = true;
				}
#endif

				if (bModified)
				{
					Changes = FString::Printf(TEXT("[%d]"), Idx) + Changes;
				}
			}

			if (bModified)
			{
				FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
				GrassType->PostEditChangeProperty(Event);
				GrassType->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(%s)"), *Changes.TrimStart()));
				return sol::make_object(Lua, true);
			}

			Session.Log(TEXT("[OK] configure() -> nothing changed. Use help() to see valid keys."));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// add(params) — add a new grass variety
		// ================================================================
		AssetObj.set_function("add", [GrassType, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(GrassType))
			{
				Session.Log(TEXT("[FAIL] add -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::optional<std::string> MeshPath = Params.get<sol::optional<std::string>>("mesh");
			if (!MeshPath.has_value())
			{
				Session.Log(TEXT("[FAIL] add: 'mesh' is required (asset path to a StaticMesh)"));
				return sol::lua_nil;
			}

			FString MPath = UTF8_TO_TCHAR(MeshPath.value().c_str());
			UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MPath);
			if (!Mesh)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add: mesh '%s' not found"), *MPath));
				return sol::lua_nil;
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("LandscapeGrassType: Add Variety")));
			GrassType->Modify();

			FGrassVariety NewVariety;
			NewVariety.GrassMesh = Mesh;

			// Optional fields
			sol::optional<double> DensityVal = Params.get<sol::optional<double>>("density");
			if (DensityVal.has_value())
				NewVariety.GrassDensity.Default = FMath::Max(0.0f, static_cast<float>(DensityVal.value()));

			sol::optional<bool> UseGrid = Params.get<sol::optional<bool>>("use_grid");
			if (UseGrid.has_value())
				NewVariety.bUseGrid = UseGrid.value();

			sol::optional<double> Jitter = Params.get<sol::optional<double>>("placement_jitter");
			if (Jitter.has_value())
				NewVariety.PlacementJitter = FMath::Clamp(static_cast<float>(Jitter.value()), 0.0f, 1.0f);

			sol::optional<int> StartCull = Params.get<sol::optional<int>>("start_cull_distance");
			if (StartCull.has_value())
				NewVariety.StartCullDistance.Default = FMath::Max(0, StartCull.value());

			sol::optional<int> EndCull = Params.get<sol::optional<int>>("end_cull_distance");
			if (EndCull.has_value())
				NewVariety.EndCullDistance.Default = FMath::Max(0, EndCull.value());

			sol::optional<std::string> ScalingStr = Params.get<sol::optional<std::string>>("scaling");
			if (ScalingStr.has_value())
				NewVariety.Scaling = StringToGrassScaling(ScalingStr.value());

			sol::optional<sol::table> SX = Params.get<sol::optional<sol::table>>("scale_x");
			if (SX.has_value()) ReadInterval(SX.value(), NewVariety.ScaleX);
			sol::optional<sol::table> SY = Params.get<sol::optional<sol::table>>("scale_y");
			if (SY.has_value()) ReadInterval(SY.value(), NewVariety.ScaleY);
			sol::optional<sol::table> SZ = Params.get<sol::optional<sol::table>>("scale_z");
			if (SZ.has_value()) ReadInterval(SZ.value(), NewVariety.ScaleZ);

			sol::optional<bool> RandRot = Params.get<sol::optional<bool>>("random_rotation");
			if (RandRot.has_value()) NewVariety.RandomRotation = RandRot.value();
			sol::optional<bool> AlignSurf = Params.get<sol::optional<bool>>("align_to_surface");
			if (AlignSurf.has_value()) NewVariety.AlignToSurface = AlignSurf.value();
			sol::optional<bool> UseLM = Params.get<sol::optional<bool>>("use_landscape_lightmap");
			if (UseLM.has_value()) NewVariety.bUseLandscapeLightmap = UseLM.value();
			sol::optional<bool> RecDecals = Params.get<sol::optional<bool>>("receives_decals");
			if (RecDecals.has_value()) NewVariety.bReceivesDecals = RecDecals.value();
			sol::optional<bool> CastDynShadow = Params.get<sol::optional<bool>>("cast_dynamic_shadow");
			if (CastDynShadow.has_value()) NewVariety.bCastDynamicShadow = CastDynShadow.value();
			sol::optional<bool> CastContact = Params.get<sol::optional<bool>>("cast_contact_shadow");
			if (CastContact.has_value()) NewVariety.bCastContactShadow = CastContact.value();
			sol::optional<bool> AffectDFL = Params.get<sol::optional<bool>>("affect_distance_field_lighting");
			if (AffectDFL.has_value()) NewVariety.bAffectDistanceFieldLighting = AffectDFL.value();
			sol::optional<bool> KeepCPU = Params.get<sol::optional<bool>>("keep_instance_buffer_cpu_copy");
			if (KeepCPU.has_value()) NewVariety.bKeepInstanceBufferCPUCopy = KeepCPU.value();
			sol::optional<int> WPODist = Params.get<sol::optional<int>>("wpo_disable_distance");
			if (WPODist.has_value()) NewVariety.InstanceWorldPositionOffsetDisableDistance = static_cast<uint32>(FMath::Max(0, WPODist.value()));
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			sol::optional<sol::table> AllowedDR = Params.get<sol::optional<sol::table>>("allowed_density_range");
			if (AllowedDR.has_value()) ReadInterval(AllowedDR.value(), NewVariety.AllowedDensityRange);
#endif

			int32 NewIdx = GrassType->GrassVarieties.Add(MoveTemp(NewVariety));

			FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
			GrassType->PostEditChangeProperty(Event);
			GrassType->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] add(mesh='%s') -> index %d"), *MPath, NewIdx));

			sol::table Result = Lua.create_table();
			Result["index"] = NewIdx;
			return sol::make_object(Lua, Result);
		});

		// ================================================================
		// remove(params) — remove a grass variety by index
		// ================================================================
		AssetObj.set_function("remove", [GrassType, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(GrassType))
			{
				Session.Log(TEXT("[FAIL] remove -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::optional<int> Idx = Params.get<sol::optional<int>>("index");
			if (!Idx.has_value())
			{
				Session.Log(TEXT("[FAIL] remove: 'index' is required (0-based variety index)"));
				return sol::lua_nil;
			}

			int32 Index = Idx.value();
			if (Index < 0 || Index >= GrassType->GrassVarieties.Num())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove: index %d out of range [0,%d)"),
					Index, GrassType->GrassVarieties.Num()));
				return sol::lua_nil;
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("LandscapeGrassType: Remove Variety")));
			GrassType->Modify();

			FString RemovedName = GrassType->GrassVarieties[Index].GrassMesh
				? GrassType->GrassVarieties[Index].GrassMesh->GetName()
				: TEXT("(none)");

			GrassType->GrassVarieties.RemoveAt(Index);

			FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
			GrassType->PostEditChangeProperty(Event);
			GrassType->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] remove(index=%d) -> removed '%s', %d varieties remain"),
				Index, *RemovedName, GrassType->GrassVarieties.Num()));
			return sol::make_object(Lua, true);
		});
	});
}

// ============================================================================
// REGISTRATION
// ============================================================================

REGISTER_LUA_BINDING(FoliageType, FoliageTypeDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindFoliageType(Lua, Session);
});

REGISTER_LUA_BINDING(LandscapeGrassType, LandscapeGrassTypeDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindLandscapeGrassType(Lua, Session);
});
