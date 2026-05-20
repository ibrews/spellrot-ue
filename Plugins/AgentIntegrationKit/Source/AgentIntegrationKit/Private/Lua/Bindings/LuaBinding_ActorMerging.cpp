// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include <sol/sol.hpp>
#include "Tools/NeoStackToolUtils.h"

#include "ScopedTransaction.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/Package.h"

#include "MeshMergeModule.h"
#include "IMeshMergeUtilities.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
#include "MeshMerge/MeshMergingSettings.h"
#include "MeshMerge/MeshProxySettings.h"
#include "MeshMerge/MeshInstancingSettings.h"
#else
#include "Engine/MeshMerging.h"
#endif

// ─── Documentation ───

static TArray<FLuaFunctionDoc> ActorMergingDocs = {
	{ TEXT("merge_actors(params)"), TEXT("Merge static mesh components into a single mesh — params: actors, output_path, merge_materials, nanite, etc."), TEXT("true or nil") },
	{ TEXT("create_proxy_mesh(params)"), TEXT("Create a proxy/simplified mesh from actors — params: actors, output_path, screen_size, etc."), TEXT("true or nil") },
	{ TEXT("merge_to_instances(params)"), TEXT("Replace duplicate static meshes with instanced static mesh components"), TEXT("true or nil") },
	{ TEXT("bake_materials(params)"), TEXT("Bake materials in-place for a static mesh asset — params: mesh_path"), TEXT("true or nil") },
};

// ─── Helpers ───

static TArray<AActor*> FindActorsByLabels(UWorld* World, const sol::table& Labels)
{
	TArray<AActor*> Result;
	TSet<FString> LabelSet;
	for (auto& Pair : Labels)
	{
		if (Pair.second.is<std::string>())
		{
			LabelSet.Add(UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str()));
		}
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (LabelSet.Contains(It->GetActorLabel()))
		{
			Result.Add(*It);
		}
	}
	return Result;
}

static TArray<UPrimitiveComponent*> CollectPrimitiveComponents(const TArray<AActor*>& Actors)
{
	TArray<UPrimitiveComponent*> Components;
	for (AActor* Actor : Actors)
	{
		TArray<UPrimitiveComponent*> ActorComps;
		Actor->GetComponents<UPrimitiveComponent>(ActorComps);
		Components.Append(ActorComps);
	}
	return Components;
}

// Parse an optional {w, h} or {1=w, 2=h} table into FIntPoint
static FIntPoint ParseIntPointTable(const sol::table& Tbl, FIntPoint Default)
{
	int32 W = Tbl.get_or(1, Default.X);
	int32 H = Tbl.get_or(2, Default.Y);
	return FIntPoint(W, H);
}

// Parse a material_settings sub-table into FMaterialProxySettings
static void ParseMaterialProxySettings(const sol::table& Tbl, FMaterialProxySettings& Out)
{
	// Texture sizing type
	std::string SizingType = Tbl.get_or<std::string>("texture_sizing_type", "");
	if (!SizingType.empty())
	{
		FString SizStr = UTF8_TO_TCHAR(SizingType.c_str());
		if (SizStr.Equals(TEXT("single"), ESearchCase::IgnoreCase))
			Out.TextureSizingType = TextureSizingType_UseSingleTextureSize;
		else if (SizStr.Equals(TEXT("biased"), ESearchCase::IgnoreCase))
			Out.TextureSizingType = TextureSizingType_UseAutomaticBiasedSizes;
		else if (SizStr.Equals(TEXT("manual"), ESearchCase::IgnoreCase))
			Out.TextureSizingType = TextureSizingType_UseManualOverrideTextureSize;
		else if (SizStr.Equals(TEXT("texel_density"), ESearchCase::IgnoreCase))
			Out.TextureSizingType = TextureSizingType_AutomaticFromTexelDensity;
		else if (SizStr.Equals(TEXT("screen_size"), ESearchCase::IgnoreCase))
			Out.TextureSizingType = TextureSizingType_AutomaticFromMeshScreenSize;
		else if (SizStr.Equals(TEXT("draw_distance"), ESearchCase::IgnoreCase))
			Out.TextureSizingType = TextureSizingType_AutomaticFromMeshDrawDistance;
	}

	// Global texture size
	sol::optional<sol::table> TexSize = Tbl.get<sol::optional<sol::table>>("texture_size");
	if (TexSize.has_value())
	{
		Out.TextureSize = ParseIntPointTable(TexSize.value(), Out.TextureSize);
	}

	Out.TargetTexelDensityPerMeter = static_cast<float>(Tbl.get_or("target_texel_density", static_cast<double>(Out.TargetTexelDensityPerMeter)));
	Out.MeshMaxScreenSizePercent = static_cast<float>(Tbl.get_or("mesh_max_screen_size", static_cast<double>(Out.MeshMaxScreenSizePercent)));
	Out.MeshMinDrawDistance = Tbl.get_or("mesh_min_draw_distance", Out.MeshMinDrawDistance);
	Out.GutterSpace = static_cast<float>(Tbl.get_or("gutter_space", static_cast<double>(Out.GutterSpace)));

	// Map toggles
	Out.bNormalMap = Tbl.get_or("normal_map", Out.bNormalMap != 0) ? 1 : 0;
	Out.bTangentMap = Tbl.get_or("tangent_map", Out.bTangentMap != 0) ? 1 : 0;
	Out.bMetallicMap = Tbl.get_or("metallic_map", Out.bMetallicMap != 0) ? 1 : 0;
	Out.bRoughnessMap = Tbl.get_or("roughness_map", Out.bRoughnessMap != 0) ? 1 : 0;
	Out.bAnisotropyMap = Tbl.get_or("anisotropy_map", Out.bAnisotropyMap != 0) ? 1 : 0;
	Out.bSpecularMap = Tbl.get_or("specular_map", Out.bSpecularMap != 0) ? 1 : 0;
	Out.bEmissiveMap = Tbl.get_or("emissive_map", Out.bEmissiveMap != 0) ? 1 : 0;
	Out.bOpacityMap = Tbl.get_or("opacity_map", Out.bOpacityMap != 0) ? 1 : 0;
	Out.bOpacityMaskMap = Tbl.get_or("opacity_mask_map", Out.bOpacityMaskMap != 0) ? 1 : 0;
	Out.bAmbientOcclusionMap = Tbl.get_or("ambient_occlusion_map", Out.bAmbientOcclusionMap != 0) ? 1 : 0;

	// Constants
	Out.MetallicConstant = static_cast<float>(Tbl.get_or("metallic_constant", static_cast<double>(Out.MetallicConstant)));
	Out.RoughnessConstant = static_cast<float>(Tbl.get_or("roughness_constant", static_cast<double>(Out.RoughnessConstant)));
	Out.AnisotropyConstant = static_cast<float>(Tbl.get_or("anisotropy_constant", static_cast<double>(Out.AnisotropyConstant)));
	Out.SpecularConstant = static_cast<float>(Tbl.get_or("specular_constant", static_cast<double>(Out.SpecularConstant)));
	Out.OpacityConstant = static_cast<float>(Tbl.get_or("opacity_constant", static_cast<double>(Out.OpacityConstant)));
	Out.OpacityMaskConstant = static_cast<float>(Tbl.get_or("opacity_mask_constant", static_cast<double>(Out.OpacityMaskConstant)));
	Out.AmbientOcclusionConstant = static_cast<float>(Tbl.get_or("ambient_occlusion_constant", static_cast<double>(Out.AmbientOcclusionConstant)));

	// Blend mode
	std::string BlendStr = Tbl.get_or<std::string>("blend_mode", "");
	if (!BlendStr.empty())
	{
		FString BM = UTF8_TO_TCHAR(BlendStr.c_str());
		if (BM.Equals(TEXT("translucent"), ESearchCase::IgnoreCase))
			Out.BlendMode = BLEND_Translucent;
		else if (BM.Equals(TEXT("additive"), ESearchCase::IgnoreCase))
			Out.BlendMode = BLEND_Additive;
		else if (BM.Equals(TEXT("modulate"), ESearchCase::IgnoreCase))
			Out.BlendMode = BLEND_Modulate;
		else if (BM.Equals(TEXT("masked"), ESearchCase::IgnoreCase))
			Out.BlendMode = BLEND_Masked;
		else
			Out.BlendMode = BLEND_Opaque;
	}

	Out.bAllowTwoSidedMaterial = Tbl.get_or("allow_two_sided", Out.bAllowTwoSidedMaterial != 0) ? 1 : 0;

	// Per-property texture size overrides (each is a {w, h} sub-table)
	auto TryParseTexSize = [&](const char* Key, FIntPoint& Target)
	{
		sol::optional<sol::table> Sz = Tbl.get<sol::optional<sol::table>>(Key);
		if (Sz.has_value())
		{
			Target = ParseIntPointTable(Sz.value(), Target);
		}
	};
	TryParseTexSize("diffuse_texture_size", Out.DiffuseTextureSize);
	TryParseTexSize("normal_texture_size", Out.NormalTextureSize);
	TryParseTexSize("tangent_texture_size", Out.TangentTextureSize);
	TryParseTexSize("metallic_texture_size", Out.MetallicTextureSize);
	TryParseTexSize("roughness_texture_size", Out.RoughnessTextureSize);
	TryParseTexSize("anisotropy_texture_size", Out.AnisotropyTextureSize);
	TryParseTexSize("specular_texture_size", Out.SpecularTextureSize);
	TryParseTexSize("emissive_texture_size", Out.EmissiveTextureSize);
	TryParseTexSize("opacity_texture_size", Out.OpacityTextureSize);
	TryParseTexSize("opacity_mask_texture_size", Out.OpacityMaskTextureSize);
	TryParseTexSize("ambient_occlusion_texture_size", Out.AmbientOcclusionTextureSize);
}

// Parse a nanite sub-table into FMeshNaniteSettings
static void ParseNaniteSettings(const sol::table& Tbl, FMeshNaniteSettings& Out)
{
	Out.bEnabled = Tbl.get_or("enabled", Out.bEnabled != 0) ? 1 : 0;
	Out.bExplicitTangents = Tbl.get_or("explicit_tangents", Out.bExplicitTangents != 0) ? 1 : 0;
	Out.bLerpUVs = Tbl.get_or("lerp_uvs", Out.bLerpUVs != 0) ? 1 : 0;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	Out.bSeparable = Tbl.get_or("separable", Out.bSeparable != 0) ? 1 : 0;
	Out.bVoxelNDF = Tbl.get_or("voxel_ndf", Out.bVoxelNDF != 0) ? 1 : 0;
	Out.bVoxelOpacity = Tbl.get_or("voxel_opacity", Out.bVoxelOpacity != 0) ? 1 : 0;
#endif

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	std::string ShapePres = Tbl.get_or<std::string>("shape_preservation", "");
	if (!ShapePres.empty())
	{
		FString ShapeStr = UTF8_TO_TCHAR(ShapePres.c_str());
		if (ShapeStr.Equals(TEXT("preserve_area"), ESearchCase::IgnoreCase))
			Out.ShapePreservation = ENaniteShapePreservation::PreserveArea;
		else if (ShapeStr.Equals(TEXT("voxelize"), ESearchCase::IgnoreCase))
			Out.ShapePreservation = ENaniteShapePreservation::Voxelize;
		else
			Out.ShapePreservation = ENaniteShapePreservation::None;
	}
#endif

	Out.PositionPrecision = Tbl.get_or("position_precision", Out.PositionPrecision);
	Out.NormalPrecision = Tbl.get_or("normal_precision", Out.NormalPrecision);
	Out.TangentPrecision = Tbl.get_or("tangent_precision", Out.TangentPrecision);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	Out.BoneWeightPrecision = Tbl.get_or("bone_weight_precision", Out.BoneWeightPrecision);
#endif
	Out.TargetMinimumResidencyInKB = static_cast<uint32>(Tbl.get_or("target_minimum_residency_kb", static_cast<int>(Out.TargetMinimumResidencyInKB)));

	Out.KeepPercentTriangles = static_cast<float>(Tbl.get_or("keep_percent_triangles", static_cast<double>(Out.KeepPercentTriangles)));
	Out.TrimRelativeError = static_cast<float>(Tbl.get_or("trim_relative_error", static_cast<double>(Out.TrimRelativeError)));

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	std::string GenFallback = Tbl.get_or<std::string>("generate_fallback", "");
	if (!GenFallback.empty())
	{
		FString FbStr = UTF8_TO_TCHAR(GenFallback.c_str());
		if (FbStr.Equals(TEXT("enabled"), ESearchCase::IgnoreCase))
			Out.GenerateFallback = ENaniteGenerateFallback::Enabled;
		else
			Out.GenerateFallback = ENaniteGenerateFallback::PlatformDefault;
	}

	std::string FbTarget = Tbl.get_or<std::string>("fallback_target", "");
	if (!FbTarget.empty())
	{
		FString FbtStr = UTF8_TO_TCHAR(FbTarget.c_str());
		if (FbtStr.Equals(TEXT("percent_triangles"), ESearchCase::IgnoreCase))
			Out.FallbackTarget = ENaniteFallbackTarget::PercentTriangles;
		else if (FbtStr.Equals(TEXT("relative_error"), ESearchCase::IgnoreCase))
			Out.FallbackTarget = ENaniteFallbackTarget::RelativeError;
		else
			Out.FallbackTarget = ENaniteFallbackTarget::Auto;
	}
#endif

	Out.FallbackPercentTriangles = static_cast<float>(Tbl.get_or("fallback_percent_triangles", static_cast<double>(Out.FallbackPercentTriangles)));
	Out.FallbackRelativeError = static_cast<float>(Tbl.get_or("fallback_relative_error", static_cast<double>(Out.FallbackRelativeError)));
	Out.MaxEdgeLengthFactor = static_cast<float>(Tbl.get_or("max_edge_length_factor", static_cast<double>(Out.MaxEdgeLengthFactor)));

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	Out.NumRays = Tbl.get_or("num_rays", Out.NumRays);
	Out.VoxelLevel = Tbl.get_or("voxel_level", Out.VoxelLevel);
	Out.RayBackUp = static_cast<float>(Tbl.get_or("ray_backup", static_cast<double>(Out.RayBackUp)));
#endif
	Out.DisplacementUVChannel = Tbl.get_or("displacement_uv_channel", Out.DisplacementUVChannel);
}

// ─── Binding ───

static void BindActorMerging(sol::state& Lua, FLuaSessionData& Session)
{
	// ---- merge_actors(params) ----
	Lua.set_function("merge_actors", [&Session](sol::table Params, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);

		sol::optional<sol::table> ActorLabels = Params.get<sol::optional<sol::table>>("actors");
		std::string OutputPath = Params.get_or<std::string>("output_path", "");

		if (!ActorLabels.has_value() || OutputPath.empty())
		{
			Session.Log(TEXT("[FAIL] merge_actors -> actors and output_path required"));
			return sol::lua_nil;
		}

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] merge_actors -> no editor world"));
			return sol::lua_nil;
		}

		TArray<AActor*> Actors = FindActorsByLabels(World, ActorLabels.value());
		if (Actors.Num() == 0)
		{
			Session.Log(TEXT("[FAIL] merge_actors -> no matching actors found"));
			return sol::lua_nil;
		}

		TArray<UPrimitiveComponent*> Components = CollectPrimitiveComponents(Actors);
		if (Components.Num() == 0)
		{
			Session.Log(TEXT("[FAIL] merge_actors -> no primitive components found on actors"));
			return sol::lua_nil;
		}

		// Load the MeshMergeUtilities module
		IMeshMergeModule* MergeModule = FModuleManager::GetModulePtr<IMeshMergeModule>("MeshMergeUtilities");
		if (!MergeModule)
		{
			MergeModule = FModuleManager::LoadModulePtr<IMeshMergeModule>("MeshMergeUtilities");
		}
		if (!MergeModule)
		{
			Session.Log(TEXT("[FAIL] merge_actors -> MeshMergeUtilities module not available"));
			return sol::lua_nil;
		}

		// Build settings
		FMeshMergingSettings Settings;

		// Mesh settings
		Settings.bMergeMaterials = Params.get_or("merge_materials", false);
		Settings.bMergePhysicsData = Params.get_or("merge_physics", true);
		Settings.bMergeMeshSockets = Params.get_or("merge_sockets", false);
		Settings.bPivotPointAtZero = Params.get_or("pivot_at_zero", false);
		Settings.bBakeVertexDataToMesh = Params.get_or("bake_vertex_data", false);
		Settings.bUseVertexDataForBakingMaterial = Params.get_or("use_vertex_data_for_baking", false);
		Settings.bUseTextureBinning = Params.get_or("use_texture_binning", false);
		Settings.bReuseMeshLightmapUVs = Params.get_or("reuse_lightmap_uvs", false);
		Settings.bMergeEquivalentMaterials = Params.get_or("merge_equivalent_materials", false);

		// Lightmap
		Settings.bGenerateLightMapUV = Params.get_or("generate_lightmap_uv", false);
		Settings.bComputedLightMapResolution = Params.get_or("computed_lightmap_resolution", false);
		Settings.TargetLightMapResolution = Params.get_or("target_lightmap_resolution", 256);

		// Culling / LOD
		Settings.bUseLandscapeCulling = Params.get_or("landscape_culling", false);
		Settings.bIncludeImposters = Params.get_or("include_imposters", false);
		Settings.bSupportRayTracing = Params.get_or("support_ray_tracing", true);
		Settings.bAllowDistanceField = Params.get_or("allow_distance_field", true);

		// LOD selection
		std::string LodSel = Params.get_or<std::string>("lod_selection", "all");
		FString LodStr = UTF8_TO_TCHAR(LodSel.c_str());
		if (LodStr.Equals(TEXT("specific"), ESearchCase::IgnoreCase))
			Settings.LODSelectionType = EMeshLODSelectionType::SpecificLOD;
		else if (LodStr.Equals(TEXT("calculate"), ESearchCase::IgnoreCase))
			Settings.LODSelectionType = EMeshLODSelectionType::CalculateLOD;
		else if (LodStr.Equals(TEXT("lowest"), ESearchCase::IgnoreCase))
			Settings.LODSelectionType = EMeshLODSelectionType::LowestDetailLOD;
		else
			Settings.LODSelectionType = EMeshLODSelectionType::AllLODs;

		Settings.SpecificLOD = Params.get_or("specific_lod", 0);

		// Gutter size
		Settings.GutterSize = Params.get_or("gutter_size", 4);

		// UV channel output control (array of 8 bools)
		sol::optional<sol::table> UVOutputs = Params.get<sol::optional<sol::table>>("output_uvs");
		if (UVOutputs.has_value())
		{
			for (int32 i = 0; i < 8; ++i)
			{
				bool bOutput = UVOutputs.value().get_or(i + 1, true);
				Settings.OutputUVs[i] = bOutput ? EUVOutput::OutputChannel : EUVOutput::DoNotOutputChannel;
			}
		}

		// Nanite settings (sub-table)
		sol::optional<sol::table> NaniteTbl = Params.get<sol::optional<sol::table>>("nanite");
		if (NaniteTbl.has_value())
		{
			ParseNaniteSettings(NaniteTbl.value(), Settings.NaniteSettings);
		}

		// Material settings (sub-table)
		sol::optional<sol::table> MatTbl = Params.get<sol::optional<sol::table>>("material_settings");
		if (MatTbl.has_value())
		{
			ParseMaterialProxySettings(MatTbl.value(), Settings.MaterialSettings);
		}

		FString PackageName = UTF8_TO_TCHAR(OutputPath.c_str());

		const FScopedTransaction Tx(NSLOCTEXT("AIK", "MergeActors", "Merge Actors"));

		TArray<UObject*> OutAssets;
		FVector OutLocation;

		IMeshMergeUtilities& Utils = MergeModule->GetUtilities();
		Utils.MergeComponentsToStaticMesh(
			Components,
			World,
			Settings,
			nullptr,	// InBaseMaterial
			nullptr,	// InOuter (auto-create package)
			PackageName,
			OutAssets,
			OutLocation,
			1.0f,		// ScreenSize
			true		// bSilent
		);

		if (OutAssets.Num() == 0)
		{
			Session.Log(TEXT("[FAIL] merge_actors -> merge produced no output assets"));
			return sol::lua_nil;
		}

		// Save the created assets
		for (UObject* Asset : OutAssets)
		{
			if (Asset && Asset->GetOutermost())
			{
				Asset->GetOutermost()->MarkPackageDirty();
			}
		}

		Session.Log(FString::Printf(TEXT("[OK] merge_actors -> merged %d components from %d actors, %d assets created at %s"),
			Components.Num(), Actors.Num(), OutAssets.Num(), *PackageName));
		return sol::make_object(Lua, true);
	});

	// ---- create_proxy_mesh(params) ----
	Lua.set_function("create_proxy_mesh", [&Session](sol::table Params, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);

		sol::optional<sol::table> ActorLabels = Params.get<sol::optional<sol::table>>("actors");
		std::string OutputPath = Params.get_or<std::string>("output_path", "");

		if (!ActorLabels.has_value() || OutputPath.empty())
		{
			Session.Log(TEXT("[FAIL] create_proxy_mesh -> actors and output_path required"));
			return sol::lua_nil;
		}

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] create_proxy_mesh -> no editor world"));
			return sol::lua_nil;
		}

		TArray<AActor*> Actors = FindActorsByLabels(World, ActorLabels.value());
		if (Actors.Num() == 0)
		{
			Session.Log(TEXT("[FAIL] create_proxy_mesh -> no matching actors found"));
			return sol::lua_nil;
		}

		IMeshMergeModule* MergeModule = FModuleManager::GetModulePtr<IMeshMergeModule>("MeshMergeUtilities");
		if (!MergeModule)
		{
			MergeModule = FModuleManager::LoadModulePtr<IMeshMergeModule>("MeshMergeUtilities");
		}
		if (!MergeModule)
		{
			Session.Log(TEXT("[FAIL] create_proxy_mesh -> MeshMergeUtilities module not available"));
			return sol::lua_nil;
		}

		FString PackageName = UTF8_TO_TCHAR(OutputPath.c_str());

		FMeshProxySettings ProxySettings;
		ProxySettings.ScreenSize = Params.get_or("screen_size", 300);
		float ApiScreenSize = static_cast<float>(Params.get_or("api_screen_size", static_cast<double>(ProxySettings.ScreenSize)));
		ProxySettings.MergeDistance = static_cast<float>(Params.get_or("merge_distance", 0.0));

		// Voxel size
		ProxySettings.bOverrideVoxelSize = Params.get_or("override_voxel_size", false);
		ProxySettings.VoxelSize = static_cast<float>(Params.get_or("voxel_size", static_cast<double>(ProxySettings.VoxelSize)));

		// LOD / normals
		ProxySettings.bCalculateCorrectLODModel = Params.get_or("calculate_correct_lod", false);
		ProxySettings.bRecalculateNormals = Params.get_or("recalculate_normals", true);

		// Hard angle threshold
		ProxySettings.bUseHardAngleThreshold = Params.get_or("use_hard_angle_threshold", false);
		ProxySettings.HardAngleThreshold = static_cast<float>(Params.get_or("hard_angle_threshold", 80.0));

		// Normal calculation method
		std::string NormalCalc = Params.get_or<std::string>("normal_calculation", "angle");
		FString NormalStr = UTF8_TO_TCHAR(NormalCalc.c_str());
		if (NormalStr.Equals(TEXT("area"), ESearchCase::IgnoreCase))
			ProxySettings.NormalCalculationMethod = EProxyNormalComputationMethod::AreaWeighted;
		else if (NormalStr.Equals(TEXT("equal"), ESearchCase::IgnoreCase))
			ProxySettings.NormalCalculationMethod = EProxyNormalComputationMethod::EqualWeighted;
		else
			ProxySettings.NormalCalculationMethod = EProxyNormalComputationMethod::AngleWeighted;

		// Transfer distance
		ProxySettings.bOverrideTransferDistance = Params.get_or("override_transfer_distance", false);
		ProxySettings.MaxRayCastDist = static_cast<float>(Params.get_or("max_ray_cast_dist", 0.0));

		// Lightmap
		ProxySettings.LightMapResolution = Params.get_or("lightmap_resolution", 256);
		ProxySettings.bComputeLightMapResolution = Params.get_or("compute_lightmap_resolution", false);

		// Landscape culling
		ProxySettings.bUseLandscapeCulling = Params.get_or("landscape_culling", false);
		std::string CullingPrec = Params.get_or<std::string>("landscape_culling_precision", "medium");
		FString CullingStr = UTF8_TO_TCHAR(CullingPrec.c_str());
		if (CullingStr.Equals(TEXT("high"), ESearchCase::IgnoreCase))
			ProxySettings.LandscapeCullingPrecision = ELandscapeCullingPrecision::High;
		else if (CullingStr.Equals(TEXT("low"), ESearchCase::IgnoreCase))
			ProxySettings.LandscapeCullingPrecision = ELandscapeCullingPrecision::Low;
		else
			ProxySettings.LandscapeCullingPrecision = ELandscapeCullingPrecision::Medium;

		// Rendering features
		ProxySettings.bSupportRayTracing = Params.get_or("support_ray_tracing", true);
		ProxySettings.bAllowDistanceField = Params.get_or("allow_distance_field", true);
		ProxySettings.bReuseMeshLightmapUVs = Params.get_or("reuse_lightmap_uvs", false);
		ProxySettings.bGroupIdenticalMeshesForBaking = Params.get_or("group_identical_meshes", false);

		// Collision / vertex / lightmap
		ProxySettings.bCreateCollision = Params.get_or("create_collision", true);
		ProxySettings.bAllowVertexColors = Params.get_or("allow_vertex_colors", true);
		ProxySettings.bGenerateLightmapUVs = Params.get_or("generate_lightmap_uvs", true);

		// Unresolved geometry color
		sol::optional<sol::table> ColorTbl = Params.get<sol::optional<sol::table>>("unresolved_geometry_color");
		if (ColorTbl.has_value())
		{
			ProxySettings.UnresolvedGeometryColor = FColor(
				static_cast<uint8>(ColorTbl.value().get_or("r", 0)),
				static_cast<uint8>(ColorTbl.value().get_or("g", 0)),
				static_cast<uint8>(ColorTbl.value().get_or("b", 0)),
				static_cast<uint8>(ColorTbl.value().get_or("a", 255))
			);
		}

		// Nanite settings (sub-table)
		sol::optional<sol::table> NaniteTbl = Params.get<sol::optional<sol::table>>("nanite");
		if (NaniteTbl.has_value())
		{
			ParseNaniteSettings(NaniteTbl.value(), ProxySettings.NaniteSettings);
		}

		// Material settings (sub-table)
		sol::optional<sol::table> MatTbl = Params.get<sol::optional<sol::table>>("material_settings");
		if (MatTbl.has_value())
		{
			ParseMaterialProxySettings(MatTbl.value(), ProxySettings.MaterialSettings);
		}

		const FScopedTransaction Tx(NSLOCTEXT("AIK", "CreateProxyMesh", "Create Proxy Mesh"));

		// CreateProxyMesh is async — use a synchronous flag
		IMeshMergeUtilities& Utils = MergeModule->GetUtilities();

		FCreateProxyDelegate ProxyDelegate;
		bool bProxyCreated = false;
		TArray<UObject*> OutAssets;

		ProxyDelegate.BindLambda([&bProxyCreated, &OutAssets](const FGuid, TArray<UObject*>& InOutAssets)
		{
			OutAssets = InOutAssets;
			bProxyCreated = true;
		});

		Utils.CreateProxyMesh(
			Actors,
			ProxySettings,
			nullptr,		// InOuter
			PackageName,
			FGuid::NewGuid(),
			ProxyDelegate,
			false,			// bAllowAsync = false for synchronous
			ApiScreenSize
		);

		if (!bProxyCreated || OutAssets.Num() == 0)
		{
			Session.Log(TEXT("[FAIL] create_proxy_mesh -> proxy generation produced no output"));
			return sol::lua_nil;
		}

		for (UObject* Asset : OutAssets)
		{
			if (Asset && Asset->GetOutermost())
			{
				Asset->GetOutermost()->MarkPackageDirty();
			}
		}

		Session.Log(FString::Printf(TEXT("[OK] create_proxy_mesh -> created proxy from %d actors, %d assets at %s"),
			Actors.Num(), OutAssets.Num(), *PackageName));
		return sol::make_object(Lua, true);
	});

	// ---- merge_to_instances(params) ----
	Lua.set_function("merge_to_instances", [&Session](sol::table Params, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);

		sol::optional<sol::table> ActorLabels = Params.get<sol::optional<sol::table>>("actors");
		if (!ActorLabels.has_value())
		{
			Session.Log(TEXT("[FAIL] merge_to_instances -> actors required"));
			return sol::lua_nil;
		}

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] merge_to_instances -> no editor world"));
			return sol::lua_nil;
		}

		TArray<AActor*> Actors = FindActorsByLabels(World, ActorLabels.value());
		if (Actors.Num() == 0)
		{
			Session.Log(TEXT("[FAIL] merge_to_instances -> no matching actors found"));
			return sol::lua_nil;
		}

		TArray<UPrimitiveComponent*> Components = CollectPrimitiveComponents(Actors);
		if (Components.Num() == 0)
		{
			Session.Log(TEXT("[FAIL] merge_to_instances -> no primitive components found on actors"));
			return sol::lua_nil;
		}

		IMeshMergeModule* MergeModule = FModuleManager::GetModulePtr<IMeshMergeModule>("MeshMergeUtilities");
		if (!MergeModule)
		{
			MergeModule = FModuleManager::LoadModulePtr<IMeshMergeModule>("MeshMergeUtilities");
		}
		if (!MergeModule)
		{
			Session.Log(TEXT("[FAIL] merge_to_instances -> MeshMergeUtilities module not available"));
			return sol::lua_nil;
		}

		FMeshInstancingSettings InstSettings;
		InstSettings.InstanceReplacementThreshold = Params.get_or("instance_threshold", 2);
		InstSettings.bSkipMeshesWithVertexColors = Params.get_or("skip_vertex_colors", false);
		InstSettings.bUseHLODVolumes = Params.get_or("use_hlod_volumes", false);

		bool bReplaceSourceActors = Params.get_or("replace_source_actors", false);

		ULevel* Level = World->GetCurrentLevel();
		if (!Level)
		{
			Session.Log(TEXT("[FAIL] merge_to_instances -> no current level"));
			return sol::lua_nil;
		}

		const FScopedTransaction Tx(NSLOCTEXT("AIK", "MergeToInstances", "Merge To Instances"));

		FText ResultsText;
		IMeshMergeUtilities& Utils = MergeModule->GetUtilities();
		Utils.MergeComponentsToInstances(
			Components,
			World,
			Level,
			InstSettings,
			true,					// bActuallyMerge
			bReplaceSourceActors,
			&ResultsText
		);

		FString ResultStr = ResultsText.ToString();
		if (ResultStr.IsEmpty())
		{
			ResultStr = TEXT("completed");
		}

		Session.Log(FString::Printf(TEXT("[OK] merge_to_instances -> %d actors processed: %s"),
			Actors.Num(), *ResultStr));
		return sol::make_object(Lua, true);
	});

	// ---- bake_materials(params) ----
	Lua.set_function("bake_materials", [&Session](sol::table Params, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);

		std::string MeshPath = Params.get_or<std::string>("mesh_path", "");
		if (MeshPath.empty())
		{
			Session.Log(TEXT("[FAIL] bake_materials -> mesh_path required"));
			return sol::lua_nil;
		}

		FString AssetPath = UTF8_TO_TCHAR(MeshPath.c_str());
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
		if (!Mesh)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] bake_materials -> could not load static mesh at %s"), *AssetPath));
			return sol::lua_nil;
		}

		IMeshMergeModule* MergeModule = FModuleManager::GetModulePtr<IMeshMergeModule>("MeshMergeUtilities");
		if (!MergeModule)
		{
			MergeModule = FModuleManager::LoadModulePtr<IMeshMergeModule>("MeshMergeUtilities");
		}
		if (!MergeModule)
		{
			Session.Log(TEXT("[FAIL] bake_materials -> MeshMergeUtilities module not available"));
			return sol::lua_nil;
		}

		const FScopedTransaction Tx(NSLOCTEXT("AIK", "BakeMaterials", "Bake Materials"));

		const IMeshMergeUtilities& Utils = MergeModule->GetUtilities();
		Utils.BakeMaterialsForMesh(Mesh);

		if (Mesh->GetOutermost())
		{
			Mesh->GetOutermost()->MarkPackageDirty();
		}

		Session.Log(FString::Printf(TEXT("[OK] bake_materials -> baked materials for %s"), *AssetPath));
		return sol::make_object(Lua, true);
	});
}

REGISTER_LUA_BINDING(ActorMerging, ActorMergingDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindActorMerging(Lua, Session);
});
