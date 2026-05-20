// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"

#include "WaterBodyActor.h"
#include "WaterBodyOceanActor.h"
#include "WaterBodyLakeActor.h"
#include "WaterBodyRiverActor.h"
#include "WaterBodyCustomActor.h"
#include "WaterBodyComponent.h"
#include "WaterBodyOceanComponent.h"
#include "WaterBodyLakeComponent.h"
#include "WaterBodyRiverComponent.h"
#include "WaterBodyCustomComponent.h"
#include "WaterSplineComponent.h"
#include "WaterSplineMetadata.h"
#include "GerstnerWaterWaves.h"
#include "WaterMeshComponent.h"
#include "WaterSubsystem.h"
#include "WaterBodyIslandActor.h"
#include "WaterBodyExclusionVolume.h"
#include "BuoyancyComponent.h"
#include "BuoyancyTypes.h"
#include "WaterZoneActor.h"
#include "WaterBodyHeightmapSettings.h"
#include "WaterBodyWeightmapSettings.h"
#include "WaterBrushEffects.h"
#include "WaterFalloffSettings.h"
#include "WaterRuntimeSettings.h"
#include "WaterEditorSettings.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "ScopedTransaction.h"
#include "Components/SplineComponent.h"
#include "Subsystems/EditorActorSubsystem.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static UWorld* GetEditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

static FVector TableToVector(const sol::table& T)
{
	float X = T["x"].valid() ? T["x"].get<float>() : (T[1].valid() ? T[1].get<float>() : 0.f);
	float Y = T["y"].valid() ? T["y"].get<float>() : (T[2].valid() ? T[2].get<float>() : 0.f);
	float Z = T["z"].valid() ? T["z"].get<float>() : (T[3].valid() ? T[3].get<float>() : 0.f);
	return FVector(X, Y, Z);
}

static sol::table VectorToTable(sol::state_view& Lua, const FVector& V)
{
	sol::table T = Lua.create_table();
	T["x"] = V.X;
	T["y"] = V.Y;
	T["z"] = V.Z;
	return T;
}

static const char* WaterBodyTypeToString(EWaterBodyType Type)
{
	switch (Type)
	{
	case EWaterBodyType::River: return "River";
	case EWaterBodyType::Lake: return "Lake";
	case EWaterBodyType::Ocean: return "Ocean";
	case EWaterBodyType::Transition: return "Custom";
	default: return "Unknown";
	}
}

static EWaterBodyType StringToWaterBodyType(const std::string& Str)
{
	FString S = UTF8_TO_TCHAR(Str.c_str());
	if (S.Equals(TEXT("river"), ESearchCase::IgnoreCase)) return EWaterBodyType::River;
	if (S.Equals(TEXT("lake"), ESearchCase::IgnoreCase)) return EWaterBodyType::Lake;
	if (S.Equals(TEXT("ocean"), ESearchCase::IgnoreCase)) return EWaterBodyType::Ocean;
	if (S.Equals(TEXT("custom"), ESearchCase::IgnoreCase)) return EWaterBodyType::Transition;
	return EWaterBodyType::Num; // invalid sentinel
}

static AWaterBody* FindWaterBodyByName(UWorld* World, const FString& NameOrLabel)
{
	if (!World) return nullptr;
	for (TActorIterator<AWaterBody> It(World); It; ++It)
	{
		AWaterBody* WB = *It;
		if (WB->GetActorLabel() == NameOrLabel || WB->GetName() == NameOrLabel || WB->GetActorNameOrLabel() == NameOrLabel)
		{
			return WB;
		}
	}
	return nullptr;
}

static AActor* FindActorWithBuoyancyByName(UWorld* World, const FString& NameOrLabel)
{
	if (!World) return nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) continue;
		UBuoyancyComponent* Buoy = Actor->FindComponentByClass<UBuoyancyComponent>();
		if (!Buoy) continue;
		if (Actor->GetActorLabel() == NameOrLabel || Actor->GetName() == NameOrLabel || Actor->GetActorNameOrLabel() == NameOrLabel)
		{
			return Actor;
		}
	}
	return nullptr;
}

static AWaterZone* FindWaterZoneByName(UWorld* World, const FString& NameOrLabel)
{
	if (!World) return nullptr;
	for (TActorIterator<AWaterZone> It(World); It; ++It)
	{
		AWaterZone* WZ = *It;
		if (WZ->GetActorLabel() == NameOrLabel || WZ->GetName() == NameOrLabel || WZ->GetActorNameOrLabel() == NameOrLabel)
		{
			return WZ;
		}
	}
	return nullptr;
}

// ============================================================================
// DOCS
// ============================================================================

static TArray<FLuaFunctionDoc> WaterDocs = {
	{ TEXT("water_list_bodies(type?)"), TEXT("List water body actors — optional type filter: ocean, lake, river, custom"), TEXT("table[]") },
	{ TEXT("water_list_zones()"), TEXT("List all water zones in the editor world"), TEXT("table[]") },
	{ TEXT("water_get_body(name_or_label)"), TEXT("Get detailed info for a specific water body"), TEXT("table or nil") },
	{ TEXT("water_spawn(type, location, params?)"), TEXT("Spawn a water body — types: ocean, lake, river, custom"), TEXT("string or nil") },
	{ TEXT("water_set_waves(actor_name, params)"), TEXT("Configure wave generation on ocean/lake bodies"), TEXT("bool") },
	{ TEXT("water_set_material(actor_name, slot, material_path)"), TEXT("Set water material — slots: water, underwater, static_mesh, info"), TEXT("bool") },
	{ TEXT("water_set_landscape(actor_name, params)"), TEXT("Configure landscape carving settings for a water body"), TEXT("bool") },
	{ TEXT("water_river_add_point(actor_name, location, width?, depth?, velocity?)"), TEXT("Add a spline point to a river water body"), TEXT("int or nil") },
	{ TEXT("water_river_set_point(actor_name, index, params)"), TEXT("Modify a river spline point by 1-based index"), TEXT("bool") },
	{ TEXT("water_river_get_points(actor_name)"), TEXT("Get all spline points with metadata for a river"), TEXT("table[] or nil") },
	{ TEXT("water_zone_set_extent(zone_name, width, height)"), TEXT("Set the extent of a water zone"), TEXT("bool") },
	{ TEXT("water_zone_set_resolution(zone_name, width, height)"), TEXT("Set render target resolution of a water zone"), TEXT("bool") },
	{ TEXT("water_zone_rebuild(zone_name?)"), TEXT("Rebuild water zone(s) — rebuilds all if no name given"), TEXT("bool") },
	{ TEXT("water_ocean_set_flood(height)"), TEXT("Set the ocean flood height via the water subsystem"), TEXT("bool") },
	{ TEXT("water_ocean_get_height()"), TEXT("Get the ocean base height via the water subsystem"), TEXT("number or nil") },
	{ TEXT("water_remove_body(name_or_label)"), TEXT("Remove a water body actor from the level"), TEXT("bool") },
	{ TEXT("water_get_buoyancy(actor_name)"), TEXT("Get buoyancy settings from an actor's UBuoyancyComponent"), TEXT("table or nil") },
	{ TEXT("water_set_buoyancy(actor_name, params)"), TEXT("Configure buoyancy on an actor's UBuoyancyComponent"), TEXT("bool") },
	{ TEXT("water_add_pontoon(actor_name, params)"), TEXT("Add a pontoon to an actor's buoyancy component"), TEXT("int or nil") },
	{ TEXT("water_remove_pontoon(actor_name, index)"), TEXT("Remove a pontoon by 1-based index from buoyancy component"), TEXT("bool") },
	{ TEXT("water_list_buoyancy()"), TEXT("List all actors with a UBuoyancyComponent"), TEXT("table[]") },
	{ TEXT("water_query_surface(location)"), TEXT("Query water surface info at a world location {x,y,z}"), TEXT("table or nil") },
	{ TEXT("water_create_island(params)"), TEXT("Spawn a water body island with spline points and heightmap/weightmap settings. params: {location, points[], label?, heightmap={blend_mode, falloff_width, edge_offset, z_offset}, layers={name={falloff_width, edge_offset, opacity, texture_tiling, texture_influence, midpoint}}}"), TEXT("string or nil") },
	{ TEXT("water_create_zone(params)"), TEXT("Spawn a water zone. params: {location, width, height, resolution_x?, resolution_y?, tile_size?, label?}"), TEXT("string or nil") },
	{ TEXT("water_set_landscape_painting(actor_name, params)"), TEXT("Set layer weightmap painting settings on a water body. params: {layers={name={falloff_width, edge_offset, opacity, texture_tiling, texture_influence, midpoint}}}"), TEXT("bool") },
	{ TEXT("water_set_brush_effects(actor_name, params)"), TEXT("Set brush effects on a water body's heightmap. params: {blend_mode?, falloff_width?, edge_offset?, z_offset?, blurring={enabled, radius}, curl_noise={amount1, amount2, tiling1, tiling2}, displacement={height, tiling, midpoint}, terracing={alpha, spacing, smoothness}, smooth_blending={inner, outer}}"), TEXT("bool") },
	{ TEXT("water_ocean_fill_zone(actor_name)"), TEXT("Fill the owning water zone with the ocean mesh (calls FillWaterZoneWithOcean on ocean component)"), TEXT("bool") },
	{ TEXT("water_configure_body(actor_name, params)"), TEXT("Configure water body properties: overlap_material_priority, target_wave_mask_depth, max_wave_height_offset, collision_height_offset, fixed_water_depth, constant_depth, generate_overlap_events"), TEXT("bool") },
	{ TEXT("water_get_zone(name_or_label)"), TEXT("Get detailed info for a specific water zone"), TEXT("table or nil") },
	{ TEXT("water_set_zone_override(actor_name, zone_name_or_nil)"), TEXT("Override which water zone a water body belongs to (nil to clear override)"), TEXT("bool") },
	{ TEXT("water_ocean_get_total_height()"), TEXT("Get the total ocean height (base + flood) via the water subsystem"), TEXT("number or nil") },
	{ TEXT("water_add_exclusion_volume(actor_name, volume_name)"), TEXT("Add an exclusion volume to a water body"), TEXT("bool") },
	{ TEXT("water_remove_exclusion_volume(actor_name, volume_name)"), TEXT("Remove an exclusion volume from a water body"), TEXT("bool") },
	{ TEXT("water_list_exclusion_volumes(actor_name)"), TEXT("List exclusion volumes associated with a water body"), TEXT("table[] or nil") },
};

// ============================================================================
// BINDING
// ============================================================================

static void BindWater(sol::state& Lua, FLuaSessionData& Session)
{
	// ================================================================
	// water_list_bodies(type?)
	// ================================================================
	Lua.set_function("water_list_bodies", [&Session](sol::optional<std::string> TypeFilterOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_list_bodies -> No editor world"));
			return sol::lua_nil;
		}

		EWaterBodyType FilterType = EWaterBodyType::Num;
		if (TypeFilterOpt.has_value())
		{
			FilterType = StringToWaterBodyType(TypeFilterOpt.value());
			if (FilterType == EWaterBodyType::Num)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] water_list_bodies -> Unknown type filter '%s'"), UTF8_TO_TCHAR(TypeFilterOpt.value().c_str())));
				return sol::lua_nil;
			}
		}

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;
		for (TActorIterator<AWaterBody> It(World); It; ++It)
		{
			AWaterBody* WB = *It;
			if (!WB) continue;

			EWaterBodyType BodyType = WB->GetWaterBodyType();
			if (FilterType != EWaterBodyType::Num && BodyType != FilterType) continue;

			UWaterBodyComponent* Comp = WB->GetWaterBodyComponent();
			UWaterSplineComponent* Spline = WB->GetWaterSpline();

			sol::table Entry = LuaView.create_table();
			Entry["name"] = TCHAR_TO_UTF8(*WB->GetActorNameOrLabel());
			Entry["type"] = WaterBodyTypeToString(BodyType);
			Entry["location"] = VectorToTable(LuaView, WB->GetActorLocation());
			Entry["spline_point_count"] = Spline ? Spline->GetNumberOfSplinePoints() : 0;
			Entry["has_waves"] = Comp ? Comp->HasWaves() : false;
			Entry["water_depth"] = Comp ? static_cast<float>(Comp->GetConstantDepth()) : 0.f;
			Entry["affects_landscape"] = Comp ? Comp->bAffectsLandscape : false;

			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] water_list_bodies -> %d bodies found"), Idx - 1));
		return Result;
	});

	// ================================================================
	// water_list_zones()
	// ================================================================
	Lua.set_function("water_list_zones", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_list_zones -> No editor world"));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;
		for (TActorIterator<AWaterZone> It(World); It; ++It)
		{
			AWaterZone* WZ = *It;
			if (!WZ) continue;

			FVector2D Extent = WZ->GetZoneExtent();
			const UWaterMeshComponent* WaterMesh = WZ->GetWaterMeshComponent();

			int32 BodyCount = 0;
			WZ->ForEachWaterBodyComponent([&BodyCount](UWaterBodyComponent*) -> bool
			{
				BodyCount++;
				return true;
			});

			sol::table Entry = LuaView.create_table();
			Entry["name"] = TCHAR_TO_UTF8(*WZ->GetActorNameOrLabel());
			Entry["location"] = VectorToTable(LuaView, WZ->GetActorLocation());

			sol::table ExtentTable = LuaView.create_table();
			ExtentTable["x"] = Extent.X;
			ExtentTable["y"] = Extent.Y;
			Entry["extent"] = ExtentTable;

			Entry["body_count"] = BodyCount;
			Entry["tile_size"] = WaterMesh ? WaterMesh->GetTileSize() : 0.f;
			Entry["tessellation_factor"] = WaterMesh ? WaterMesh->GetTessellationFactor() : 0;

			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] water_list_zones -> %d zones found"), Idx - 1));
		return Result;
	});

	// ================================================================
	// water_get_body(name_or_label)
	// ================================================================
	Lua.set_function("water_get_body", [&Session](const std::string& NameStr, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_get_body -> No editor world"));
			return sol::lua_nil;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AWaterBody* WB = FindWaterBodyByName(World, Name);
		if (!WB)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_get_body -> Could not find water body '%s'"), *Name));
			return sol::lua_nil;
		}

		UWaterBodyComponent* Comp = WB->GetWaterBodyComponent();
		UWaterSplineComponent* Spline = WB->GetWaterSpline();
		UWaterSplineMetadata* Meta = WB->GetWaterSplineMetadata();

		sol::table Result = LuaView.create_table();
		Result["name"] = TCHAR_TO_UTF8(*WB->GetActorNameOrLabel());
		Result["type"] = WaterBodyTypeToString(WB->GetWaterBodyType());
		Result["location"] = VectorToTable(LuaView, WB->GetActorLocation());

		// Spline points with metadata
		if (Spline)
		{
			int32 NumPoints = Spline->GetNumberOfSplinePoints();
			sol::table SplinePoints = LuaView.create_table();
			for (int32 i = 0; i < NumPoints; i++)
			{
				sol::table Pt = LuaView.create_table();
				FVector Loc = Spline->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::World);
				Pt["x"] = Loc.X;
				Pt["y"] = Loc.Y;
				Pt["z"] = Loc.Z;

				if (Meta)
				{
					float InputKey = static_cast<float>(i);
					Pt["width"] = (Meta->RiverWidth.Points.IsValidIndex(i)) ? Meta->RiverWidth.Points[i].OutVal : 0.f;
					Pt["depth"] = (Meta->Depth.Points.IsValidIndex(i)) ? Meta->Depth.Points[i].OutVal : 0.f;
					Pt["velocity"] = (Meta->WaterVelocityScalar.Points.IsValidIndex(i)) ? Meta->WaterVelocityScalar.Points[i].OutVal : 0.f;
				}

				SplinePoints[i + 1] = Pt;
			}
			Result["spline_points"] = SplinePoints;
		}

		// Wave info
		{
			sol::table WaveInfo = LuaView.create_table();
			WaveInfo["supported"] = Comp ? Comp->IsWaveSupported() : false;
			WaveInfo["has_waves"] = Comp ? Comp->HasWaves() : false;
			WaveInfo["max_wave_height"] = Comp ? Comp->GetMaxWaveHeight() : 0.f;

			UWaterWavesBase* WavesBase = WB->GetWaterWaves();
			if (WavesBase)
			{
				UGerstnerWaterWaves* Gerstner = Cast<UGerstnerWaterWaves>(WavesBase);
				if (Gerstner)
				{
					WaveInfo["type"] = "gerstner";
					WaveInfo["wave_count"] = Gerstner->GetGerstnerWaves().Num();

					if (Gerstner->GerstnerWaveGenerator)
					{
						UGerstnerWaterWaveGeneratorSimple* SimpleGen = Cast<UGerstnerWaterWaveGeneratorSimple>(Gerstner->GerstnerWaveGenerator);
						if (SimpleGen)
						{
							WaveInfo["generator"] = "simple";
							WaveInfo["num_waves"] = SimpleGen->NumWaves;
							WaveInfo["seed"] = SimpleGen->Seed;
							WaveInfo["min_wavelength"] = SimpleGen->MinWavelength;
							WaveInfo["max_wavelength"] = SimpleGen->MaxWavelength;
							WaveInfo["min_amplitude"] = SimpleGen->MinAmplitude;
							WaveInfo["max_amplitude"] = SimpleGen->MaxAmplitude;
							WaveInfo["wind_angle"] = SimpleGen->WindAngleDeg;
						}
						else
						{
							UGerstnerWaterWaveGeneratorSpectrum* SpectrumGen = Cast<UGerstnerWaterWaveGeneratorSpectrum>(Gerstner->GerstnerWaveGenerator);
							if (SpectrumGen)
							{
								WaveInfo["generator"] = "spectrum";
								WaveInfo["octave_count"] = SpectrumGen->Octaves.Num();
							}
						}
					}
				}
			}
			Result["wave_info"] = WaveInfo;
		}

		// Materials
		if (Comp)
		{
			sol::table Mats = LuaView.create_table();
			Mats["water"] = Comp->WaterMaterial ? TCHAR_TO_UTF8(*Comp->WaterMaterial->GetPathName()) : "";
			Mats["underwater"] = Comp->UnderwaterPostProcessMaterial ? TCHAR_TO_UTF8(*Comp->UnderwaterPostProcessMaterial->GetPathName()) : "";
			Mats["static_mesh"] = Comp->WaterStaticMeshMaterial ? TCHAR_TO_UTF8(*Comp->WaterStaticMeshMaterial->GetPathName()) : "";
			Mats["info"] = Comp->WaterInfoMaterial ? TCHAR_TO_UTF8(*Comp->WaterInfoMaterial->GetPathName()) : "";
			Result["materials"] = Mats;

			Result["affects_landscape"] = Comp->bAffectsLandscape;
			Result["overlap_priority"] = Comp->GetOverlapMaterialPriority();
			Result["water_depth"] = static_cast<float>(Comp->GetConstantDepth());
			Result["shape_dilation"] = Comp->ShapeDilation;
		}

		Session.Log(FString::Printf(TEXT("[OK] water_get_body -> '%s' (%s)"), *Name, UTF8_TO_TCHAR(WaterBodyTypeToString(WB->GetWaterBodyType()))));
		return Result;
	});

	// ================================================================
	// water_spawn(type, location, params?)
	// ================================================================
	Lua.set_function("water_spawn", [&Session](const std::string& TypeStr, const sol::table& LocationTable, sol::optional<sol::table> ParamsOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_spawn -> No editor world"));
			return sol::lua_nil;
		}

		EWaterBodyType BodyType = StringToWaterBodyType(TypeStr);
		if (BodyType == EWaterBodyType::Num)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_spawn -> Invalid type '%s'. Use 'ocean','lake','river','custom'."), UTF8_TO_TCHAR(TypeStr.c_str())));
			return sol::lua_nil;
		}

		FVector Location = TableToVector(LocationTable);
		FTransform SpawnTransform(FRotator::ZeroRotator, Location);

		UClass* SpawnClass = nullptr;
		switch (BodyType)
		{
		case EWaterBodyType::Ocean: SpawnClass = AWaterBodyOcean::StaticClass(); break;
		case EWaterBodyType::Lake: SpawnClass = AWaterBodyLake::StaticClass(); break;
		case EWaterBodyType::River: SpawnClass = AWaterBodyRiver::StaticClass(); break;
		case EWaterBodyType::Transition: SpawnClass = AWaterBodyCustom::StaticClass(); break;
		default: break;
		}

		if (!SpawnClass)
		{
			Session.Log(TEXT("[FAIL] water_spawn -> Could not determine spawn class"));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Spawn Water Body")));

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWaterBody* NewActor = World->SpawnActor<AWaterBody>(SpawnClass, SpawnTransform, SpawnParams);
		if (!NewActor)
		{
			Session.Log(TEXT("[FAIL] water_spawn -> SpawnActor returned null"));
			return sol::lua_nil;
		}

		sol::table P = ParamsOpt.has_value() ? ParamsOpt.value() : LuaView.create_table();

		// Set label if provided
		sol::optional<std::string> LabelOpt = P.get<sol::optional<std::string>>("label");
		if (LabelOpt.has_value())
		{
			NewActor->SetActorLabel(UTF8_TO_TCHAR(LabelOpt.value().c_str()));
		}

		// For rivers, add spline points if provided
		if (BodyType == EWaterBodyType::River)
		{
			sol::optional<sol::table> PointsOpt = P.get<sol::optional<sol::table>>("points");
			if (PointsOpt.has_value())
			{
				sol::table Points = PointsOpt.value();
				UWaterSplineComponent* Spline = NewActor->GetWaterSpline();
				if (Spline)
				{
					// Clear default points and add user-specified ones
					Spline->ClearSplinePoints(false);
					for (auto& Kv : Points)
					{
						sol::optional<sol::table> PtOpt = Kv.second.as<sol::optional<sol::table>>();
						if (PtOpt.has_value())
						{
							FVector PtLoc = TableToVector(PtOpt.value());
							// Convert to local space
							FVector LocalPt = NewActor->GetActorTransform().InverseTransformPosition(PtLoc);
							Spline->AddSplinePoint(LocalPt, ESplineCoordinateSpace::Local, false);
						}
					}
					Spline->UpdateSpline();
					Spline->K2_SynchronizeAndBroadcastDataChange();
				}
			}
		}

		// Apply default materials (matching what the engine's actor factory does in PostSpawnActor)
		UWaterBodyComponent* Comp = NewActor->GetWaterBodyComponent();
		if (Comp)
		{
			// Set default materials from WaterEditorSettings (same as UWaterBodyActorFactory::PostSpawnActor)
			const FWaterBodyDefaults* WaterBodyDefaults = nullptr;
			const FWaterBrushActorDefaults* BrushDefaults = nullptr;
			const UWaterEditorSettings* EditorSettings = GetDefault<UWaterEditorSettings>();

			switch (BodyType)
			{
			case EWaterBodyType::River:
				WaterBodyDefaults = &EditorSettings->WaterBodyRiverDefaults;
				BrushDefaults = &EditorSettings->WaterBodyRiverDefaults.BrushDefaults;
				break;
			case EWaterBodyType::Lake:
				WaterBodyDefaults = &EditorSettings->WaterBodyLakeDefaults;
				BrushDefaults = &EditorSettings->WaterBodyLakeDefaults.BrushDefaults;
				break;
			case EWaterBodyType::Ocean:
				WaterBodyDefaults = &EditorSettings->WaterBodyOceanDefaults;
				BrushDefaults = &EditorSettings->WaterBodyOceanDefaults.BrushDefaults;
				break;
			case EWaterBodyType::Transition:
				WaterBodyDefaults = &EditorSettings->WaterBodyCustomDefaults;
				break;
			default: break;
			}

			if (BrushDefaults)
			{
				Comp->CurveSettings = BrushDefaults->CurveSettings;
				Comp->WaterHeightmapSettings = BrushDefaults->HeightmapSettings;
				Comp->LayerWeightmapSettings = BrushDefaults->LayerWeightmapSettings;
			}

			if (WaterBodyDefaults)
			{
				Comp->SetWaterMaterial(WaterBodyDefaults->GetWaterMaterial());
				Comp->SetWaterStaticMeshMaterial(WaterBodyDefaults->GetWaterStaticMeshMaterial());
				Comp->SetHLODMaterial(WaterBodyDefaults->GetWaterHLODMaterial());
				Comp->SetUnderwaterPostProcessMaterial(WaterBodyDefaults->GetUnderwaterPostProcessMaterial());
			}

			// Water Info Material comes from runtime settings (critical for Water Info Texture masking)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			Comp->SetWaterInfoMaterial(GetDefault<UWaterRuntimeSettings>()->GetDefaultWaterInfoMaterial());
#endif

			// River-specific: transition materials
			if (BodyType == EWaterBodyType::River)
			{
				if (UWaterBodyRiverComponent* RiverComp = Cast<UWaterBodyRiverComponent>(Comp))
				{
					RiverComp->SetLakeTransitionMaterial(EditorSettings->WaterBodyRiverDefaults.GetRiverToLakeTransitionMaterial());
					RiverComp->SetOceanTransitionMaterial(EditorSettings->WaterBodyRiverDefaults.GetRiverToOceanTransitionMaterial());
				}
			}

			// If spawned into a zone with local-only tessellation, enable static meshes
			if (const AWaterZone* WaterZone = Comp->GetWaterZone())
			{
				if (WaterZone->IsLocalOnlyTessellationEnabled())
				{
					Comp->SetWaterBodyStaticMeshEnabled(true);
				}
			}

			// Register with water zones, build render data, and trigger full rebuild
			Comp->UpdateWaterZones();

			FOnWaterBodyChangedParams Params;
			Params.bShapeOrPositionChanged = true;
			Params.bWeightmapSettingsChanged = true;
			Params.bUserTriggered = true;
			Comp->UpdateAll(Params);

			// Create/update info mesh components (renders water body shape into Water Info Texture)
			Comp->UpdateWaterBodyRenderData();

	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		Comp->MarkOwningWaterZoneForRebuild(EWaterZoneRebuildFlags::All);
#endif
		}

		FString ActorName = NewActor->GetActorNameOrLabel();
		Session.Log(FString::Printf(TEXT("[OK] water_spawn -> Spawned %s water body '%s'"), UTF8_TO_TCHAR(WaterBodyTypeToString(BodyType)), *ActorName));
		return sol::make_object(LuaView, std::string(TCHAR_TO_UTF8(*ActorName)));
	});

	// ================================================================
	// water_set_waves(actor_name, params)
	// ================================================================
	Lua.set_function("water_set_waves", [&Session](const std::string& NameStr, const sol::table& Params, sol::this_state S) -> bool
	{
		sol::state_view LuaView(S);
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_set_waves -> No editor world"));
			return false;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AWaterBody* WB = FindWaterBodyByName(World, Name);
		if (!WB)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_set_waves -> Could not find water body '%s'"), *Name));
			return false;
		}

		UWaterBodyComponent* Comp = WB->GetWaterBodyComponent();
		if (!Comp || !Comp->IsWaveSupported())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_set_waves -> Water body '%s' does not support waves"), *Name));
			return false;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Set Water Waves")));
		WB->Modify();

		// Create or reuse existing GerstnerWaterWaves
		UGerstnerWaterWaves* Waves = Cast<UGerstnerWaterWaves>(WB->GetWaterWaves());
		if (!Waves)
		{
			Waves = NewObject<UGerstnerWaterWaves>(WB, NAME_None, RF_Transactional);
			WB->SetWaterWaves(Waves);
		}

		// Create or reuse simple generator
		UGerstnerWaterWaveGeneratorSimple* Gen = Cast<UGerstnerWaterWaveGeneratorSimple>(Waves->GerstnerWaveGenerator);
		if (!Gen)
		{
			Gen = NewObject<UGerstnerWaterWaveGeneratorSimple>(Waves, NAME_None, RF_Transactional);
			Waves->GerstnerWaveGenerator = Gen;
		}

		Gen->Modify();

		// Apply parameters
		Gen->NumWaves = static_cast<int32>(Params.get_or("num_waves", Gen->NumWaves));
		Gen->Seed = static_cast<int32>(Params.get_or("seed", Gen->Seed));
		Gen->MinWavelength = static_cast<float>(Params.get_or("min_wavelength", static_cast<double>(Gen->MinWavelength)));
		Gen->MaxWavelength = static_cast<float>(Params.get_or("max_wavelength", static_cast<double>(Gen->MaxWavelength)));
		Gen->MinAmplitude = static_cast<float>(Params.get_or("min_amplitude", static_cast<double>(Gen->MinAmplitude)));
		Gen->MaxAmplitude = static_cast<float>(Params.get_or("max_amplitude", static_cast<double>(Gen->MaxAmplitude)));
		Gen->WindAngleDeg = static_cast<float>(Params.get_or("wind_angle", static_cast<double>(Gen->WindAngleDeg)));
		Gen->SmallWaveSteepness = static_cast<float>(Params.get_or("small_wave_steepness", static_cast<double>(Gen->SmallWaveSteepness)));
		Gen->LargeWaveSteepness = static_cast<float>(Params.get_or("large_wave_steepness", static_cast<double>(Gen->LargeWaveSteepness)));
		Gen->SteepnessFalloff = static_cast<float>(Params.get_or("steepness_falloff", static_cast<double>(Gen->SteepnessFalloff)));
		Gen->DirectionAngularSpreadDeg = static_cast<float>(Params.get_or("direction_spread", static_cast<double>(Gen->DirectionAngularSpreadDeg)));
		Gen->WavelengthFalloff = static_cast<float>(Params.get_or("wavelength_falloff", static_cast<double>(Gen->WavelengthFalloff)));
		Gen->AmplitudeFalloff = static_cast<float>(Params.get_or("amplitude_falloff", static_cast<double>(Gen->AmplitudeFalloff)));
		Gen->Randomness = static_cast<float>(Params.get_or("randomness", static_cast<double>(Gen->Randomness)));

		Waves->RecomputeWaves(true);

		// Trigger update
		FOnWaterBodyChangedParams ChangedParams;
		ChangedParams.bUserTriggered = true;
		Comp->UpdateAll(ChangedParams);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		Comp->MarkOwningWaterZoneForRebuild(EWaterZoneRebuildFlags::All);
#endif

		Session.Log(FString::Printf(TEXT("[OK] water_set_waves -> Configured %d waves on '%s'"), Gen->NumWaves, *Name));
		return true;
	});

	// ================================================================
	// water_set_material(actor_name, slot, material_path)
	// ================================================================
	Lua.set_function("water_set_material", [&Session](const std::string& NameStr, const std::string& SlotStr, const std::string& MatPathStr, sol::this_state S) -> bool
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_set_material -> No editor world"));
			return false;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AWaterBody* WB = FindWaterBodyByName(World, Name);
		if (!WB)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_set_material -> Could not find water body '%s'"), *Name));
			return false;
		}

		UWaterBodyComponent* Comp = WB->GetWaterBodyComponent();
		if (!Comp)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_set_material -> Water body '%s' has no component"), *Name));
			return false;
		}

		FString MatPath = UTF8_TO_TCHAR(MatPathStr.c_str());
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MatPath);
		if (!Material)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_set_material -> Could not load material '%s'"), *MatPath));
			return false;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Set Water Material")));
		Comp->Modify();

		FString Slot = UTF8_TO_TCHAR(SlotStr.c_str());
		if (Slot.Equals(TEXT("water"), ESearchCase::IgnoreCase))
		{
			Comp->SetWaterMaterial(Material);
		}
		else if (Slot.Equals(TEXT("underwater"), ESearchCase::IgnoreCase))
		{
			Comp->SetUnderwaterPostProcessMaterial(Material);
		}
		else if (Slot.Equals(TEXT("static_mesh"), ESearchCase::IgnoreCase))
		{
			Comp->SetWaterStaticMeshMaterial(Material);
		}
		else if (Slot.Equals(TEXT("info"), ESearchCase::IgnoreCase))
		{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			Comp->SetWaterInfoMaterial(Material);
#else
			Session.Log(TEXT("[FAIL] water_set_material -> 'info' slot requires UE 5.6+"));
			return false;
#endif
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_set_material -> Unknown slot '%s'. Use 'water','underwater','static_mesh','info'."), *Slot));
			return false;
		}

		// Rebuild render data (info mesh components, static mesh components) with the new material
		Comp->UpdateWaterBodyRenderData();

		// Mark the zone for a full rebuild (water info texture + water mesh) so the new material takes effect
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		Comp->MarkOwningWaterZoneForRebuild(EWaterZoneRebuildFlags::All);
#endif

		Session.Log(FString::Printf(TEXT("[OK] water_set_material -> Set '%s' material on '%s'"), *Slot, *Name));
		return true;
	});

	// ================================================================
	// water_set_landscape(actor_name, params)
	// ================================================================
	Lua.set_function("water_set_landscape", [&Session](const std::string& NameStr, const sol::table& Params, sol::this_state S) -> bool
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_set_landscape -> No editor world"));
			return false;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AWaterBody* WB = FindWaterBodyByName(World, Name);
		if (!WB)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_set_landscape -> Could not find water body '%s'"), *Name));
			return false;
		}

		UWaterBodyComponent* Comp = WB->GetWaterBodyComponent();
		if (!Comp)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_set_landscape -> Water body '%s' has no component"), *Name));
			return false;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Set Water Landscape Settings")));
		Comp->Modify();

		sol::optional<bool> AffectsOpt = Params.get<sol::optional<bool>>("affects_landscape");
		if (AffectsOpt.has_value())
		{
			Comp->bAffectsLandscape = AffectsOpt.value();
		}

		sol::optional<double> ChannelDepthOpt = Params.get<sol::optional<double>>("channel_depth");
		if (ChannelDepthOpt.has_value())
		{
			Comp->CurveSettings.ChannelDepth = static_cast<float>(ChannelDepthOpt.value());
		}

		sol::optional<double> ShapeDilationOpt = Params.get<sol::optional<double>>("shape_dilation");
		if (ShapeDilationOpt.has_value())
		{
			Comp->ShapeDilation = static_cast<float>(ShapeDilationOpt.value());
		}

		FOnWaterBodyChangedParams ChangedParams;
		ChangedParams.bShapeOrPositionChanged = true;
		ChangedParams.bWeightmapSettingsChanged = true;
		ChangedParams.bUserTriggered = true;
		Comp->UpdateAll(ChangedParams);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		Comp->MarkOwningWaterZoneForRebuild(EWaterZoneRebuildFlags::All);
#endif

		Session.Log(FString::Printf(TEXT("[OK] water_set_landscape -> Updated landscape settings on '%s'"), *Name));
		return true;
	});

	// ================================================================
	// water_river_add_point(actor_name, location, width?, depth?, velocity?)
	// ================================================================
	Lua.set_function("water_river_add_point", [&Session](const std::string& NameStr, const sol::table& LocationTable, sol::optional<double> WidthOpt, sol::optional<double> DepthOpt, sol::optional<double> VelocityOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_river_add_point -> No editor world"));
			return sol::lua_nil;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AWaterBody* WB = FindWaterBodyByName(World, Name);
		if (!WB)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_river_add_point -> Could not find water body '%s'"), *Name));
			return sol::lua_nil;
		}

		if (WB->GetWaterBodyType() != EWaterBodyType::River)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_river_add_point -> '%s' is not a river"), *Name));
			return sol::lua_nil;
		}

		UWaterSplineComponent* Spline = WB->GetWaterSpline();
		UWaterSplineMetadata* Meta = WB->GetWaterSplineMetadata();
		if (!Spline)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_river_add_point -> '%s' has no spline"), *Name));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Add River Spline Point")));
		WB->Modify();
		Spline->Modify();

		FVector WorldLoc = TableToVector(LocationTable);
		FVector LocalLoc = WB->GetActorTransform().InverseTransformPosition(WorldLoc);
		Spline->AddSplinePoint(LocalLoc, ESplineCoordinateSpace::Local, true);

		int32 NewIndex = Spline->GetNumberOfSplinePoints() - 1;

		// Set metadata if provided
		if (Meta)
		{
			Meta->Modify();
			if (WidthOpt.has_value() && Meta->RiverWidth.Points.IsValidIndex(NewIndex))
			{
				Meta->RiverWidth.Points[NewIndex].OutVal = static_cast<float>(WidthOpt.value());
			}
			if (DepthOpt.has_value() && Meta->Depth.Points.IsValidIndex(NewIndex))
			{
				Meta->Depth.Points[NewIndex].OutVal = static_cast<float>(DepthOpt.value());
			}
			if (VelocityOpt.has_value() && Meta->WaterVelocityScalar.Points.IsValidIndex(NewIndex))
			{
				Meta->WaterVelocityScalar.Points[NewIndex].OutVal = static_cast<float>(VelocityOpt.value());
			}
		}

		Spline->UpdateSpline();
		Spline->K2_SynchronizeAndBroadcastDataChange();

		UWaterBodyComponent* Comp = WB->GetWaterBodyComponent();
		if (Comp)
		{
			FOnWaterBodyChangedParams Params;
			Params.bShapeOrPositionChanged = true;
			Params.bUserTriggered = true;
			Comp->UpdateAll(Params);
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		Comp->MarkOwningWaterZoneForRebuild(EWaterZoneRebuildFlags::All);
#endif
		}

		int32 LuaIndex = NewIndex + 1; // 1-based
		Session.Log(FString::Printf(TEXT("[OK] water_river_add_point -> Added point %d to '%s'"), LuaIndex, *Name));
		return sol::make_object(LuaView, LuaIndex);
	});

	// ================================================================
	// water_river_set_point(actor_name, index, params)
	// ================================================================
	Lua.set_function("water_river_set_point", [&Session](const std::string& NameStr, int LuaIndex, const sol::table& Params, sol::this_state S) -> bool
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_river_set_point -> No editor world"));
			return false;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AWaterBody* WB = FindWaterBodyByName(World, Name);
		if (!WB)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_river_set_point -> Could not find water body '%s'"), *Name));
			return false;
		}

		if (WB->GetWaterBodyType() != EWaterBodyType::River)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_river_set_point -> '%s' is not a river"), *Name));
			return false;
		}

		UWaterSplineComponent* Spline = WB->GetWaterSpline();
		UWaterSplineMetadata* Meta = WB->GetWaterSplineMetadata();
		if (!Spline)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_river_set_point -> '%s' has no spline"), *Name));
			return false;
		}

		int32 Index = LuaIndex - 1; // Convert from 1-based to 0-based
		if (Index < 0 || Index >= Spline->GetNumberOfSplinePoints())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_river_set_point -> Index %d out of range [1..%d]"), LuaIndex, Spline->GetNumberOfSplinePoints()));
			return false;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Set River Spline Point")));
		WB->Modify();
		Spline->Modify();

		// Set location if provided
		sol::optional<sol::table> LocOpt = Params.get<sol::optional<sol::table>>("location");
		if (LocOpt.has_value())
		{
			FVector WorldLoc = TableToVector(LocOpt.value());
			FVector LocalLoc = WB->GetActorTransform().InverseTransformPosition(WorldLoc);
			Spline->SetLocationAtSplinePoint(Index, LocalLoc, ESplineCoordinateSpace::Local, true);
		}

		// Set metadata
		if (Meta)
		{
			Meta->Modify();
			sol::optional<double> WidthOpt = Params.get<sol::optional<double>>("width");
			if (WidthOpt.has_value() && Meta->RiverWidth.Points.IsValidIndex(Index))
			{
				Meta->RiverWidth.Points[Index].OutVal = static_cast<float>(WidthOpt.value());
			}

			sol::optional<double> DepthOpt = Params.get<sol::optional<double>>("depth");
			if (DepthOpt.has_value() && Meta->Depth.Points.IsValidIndex(Index))
			{
				Meta->Depth.Points[Index].OutVal = static_cast<float>(DepthOpt.value());
			}

			sol::optional<double> VelocityOpt = Params.get<sol::optional<double>>("velocity");
			if (VelocityOpt.has_value() && Meta->WaterVelocityScalar.Points.IsValidIndex(Index))
			{
				Meta->WaterVelocityScalar.Points[Index].OutVal = static_cast<float>(VelocityOpt.value());
			}
		}

		Spline->UpdateSpline();
		Spline->K2_SynchronizeAndBroadcastDataChange();

		UWaterBodyComponent* Comp = WB->GetWaterBodyComponent();
		if (Comp)
		{
			FOnWaterBodyChangedParams ChangedParams;
			ChangedParams.bShapeOrPositionChanged = true;
			ChangedParams.bUserTriggered = true;
			Comp->UpdateAll(ChangedParams);
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		Comp->MarkOwningWaterZoneForRebuild(EWaterZoneRebuildFlags::All);
#endif
		}

		Session.Log(FString::Printf(TEXT("[OK] water_river_set_point -> Updated point %d on '%s'"), LuaIndex, *Name));
		return true;
	});

	// ================================================================
	// water_river_get_points(actor_name)
	// ================================================================
	Lua.set_function("water_river_get_points", [&Session](const std::string& NameStr, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_river_get_points -> No editor world"));
			return sol::lua_nil;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AWaterBody* WB = FindWaterBodyByName(World, Name);
		if (!WB)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_river_get_points -> Could not find water body '%s'"), *Name));
			return sol::lua_nil;
		}

		if (WB->GetWaterBodyType() != EWaterBodyType::River)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_river_get_points -> '%s' is not a river"), *Name));
			return sol::lua_nil;
		}

		UWaterSplineComponent* Spline = WB->GetWaterSpline();
		UWaterSplineMetadata* Meta = WB->GetWaterSplineMetadata();
		if (!Spline)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_river_get_points -> '%s' has no spline"), *Name));
			return sol::lua_nil;
		}

		int32 NumPoints = Spline->GetNumberOfSplinePoints();
		sol::table Result = LuaView.create_table();
		for (int32 i = 0; i < NumPoints; i++)
		{
			sol::table Pt = LuaView.create_table();
			Pt["index"] = i + 1; // 1-based

			FVector Loc = Spline->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::World);
			Pt["location"] = VectorToTable(LuaView, Loc);

			if (Meta)
			{
				Pt["width"] = (Meta->RiverWidth.Points.IsValidIndex(i)) ? Meta->RiverWidth.Points[i].OutVal : 0.f;
				Pt["depth"] = (Meta->Depth.Points.IsValidIndex(i)) ? Meta->Depth.Points[i].OutVal : 0.f;
				Pt["velocity"] = (Meta->WaterVelocityScalar.Points.IsValidIndex(i)) ? Meta->WaterVelocityScalar.Points[i].OutVal : 0.f;
			}

			Result[i + 1] = Pt;
		}

		Session.Log(FString::Printf(TEXT("[OK] water_river_get_points -> %d points on '%s'"), NumPoints, *Name));
		return Result;
	});

	// ================================================================
	// water_zone_set_extent(zone_name, width, height)
	// ================================================================
	Lua.set_function("water_zone_set_extent", [&Session](const std::string& NameStr, double Width, double Height, sol::this_state S) -> bool
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_zone_set_extent -> No editor world"));
			return false;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AWaterZone* WZ = FindWaterZoneByName(World, Name);
		if (!WZ)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_zone_set_extent -> Could not find water zone '%s'"), *Name));
			return false;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Set Water Zone Extent")));
		WZ->Modify();

		WZ->SetZoneExtent(FVector2D(Width, Height));
		WZ->MarkForRebuild(EWaterZoneRebuildFlags::All);

		Session.Log(FString::Printf(TEXT("[OK] water_zone_set_extent -> Set extent (%.0f, %.0f) on '%s'"), Width, Height, *Name));
		return true;
	});

	// ================================================================
	// water_zone_set_resolution(zone_name, width, height)
	// ================================================================
	Lua.set_function("water_zone_set_resolution", [&Session](const std::string& NameStr, int Width, int Height, sol::this_state S) -> bool
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_zone_set_resolution -> No editor world"));
			return false;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AWaterZone* WZ = FindWaterZoneByName(World, Name);
		if (!WZ)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_zone_set_resolution -> Could not find water zone '%s'"), *Name));
			return false;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Set Water Zone Resolution")));
		WZ->Modify();

		WZ->SetRenderTargetResolution(FIntPoint(Width, Height));
		WZ->MarkForRebuild(EWaterZoneRebuildFlags::All);

		Session.Log(FString::Printf(TEXT("[OK] water_zone_set_resolution -> Set resolution (%d, %d) on '%s'"), Width, Height, *Name));
		return true;
	});

	// ================================================================
	// water_zone_rebuild(zone_name?)
	// ================================================================
	Lua.set_function("water_zone_rebuild", [&Session](sol::optional<std::string> NameOpt, sol::this_state S) -> bool
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_zone_rebuild -> No editor world"));
			return false;
		}

		if (NameOpt.has_value())
		{
			FString Name = UTF8_TO_TCHAR(NameOpt.value().c_str());
			AWaterZone* WZ = FindWaterZoneByName(World, Name);
			if (!WZ)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] water_zone_rebuild -> Could not find water zone '%s'"), *Name));
				return false;
			}

			WZ->MarkForRebuild(EWaterZoneRebuildFlags::All);
			Session.Log(FString::Printf(TEXT("[OK] water_zone_rebuild -> Rebuilt zone '%s'"), *Name));
		}
		else
		{
			UWaterSubsystem* Subsystem = UWaterSubsystem::GetWaterSubsystem(World);
			if (!Subsystem)
			{
				Session.Log(TEXT("[FAIL] water_zone_rebuild -> No water subsystem"));
				return false;
			}

			Subsystem->MarkAllWaterZonesForRebuild(EWaterZoneRebuildFlags::All);
			Session.Log(TEXT("[OK] water_zone_rebuild -> Rebuilt all water zones"));
		}

		return true;
	});

	// ================================================================
	// water_ocean_set_flood(height)
	// ================================================================
	Lua.set_function("water_ocean_set_flood", [&Session](double Height, sol::this_state S) -> bool
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_ocean_set_flood -> No editor world"));
			return false;
		}

		UWaterSubsystem* Subsystem = UWaterSubsystem::GetWaterSubsystem(World);
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] water_ocean_set_flood -> No water subsystem"));
			return false;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Set Ocean Flood Height")));
		Subsystem->SetOceanFloodHeight(static_cast<float>(Height));

		Session.Log(FString::Printf(TEXT("[OK] water_ocean_set_flood -> Set flood height to %.2f"), Height));
		return true;
	});

	// ================================================================
	// water_ocean_get_height()
	// ================================================================
	Lua.set_function("water_ocean_get_height", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_ocean_get_height -> No editor world"));
			return sol::lua_nil;
		}

		UWaterSubsystem* Subsystem = UWaterSubsystem::GetWaterSubsystem(World);
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] water_ocean_get_height -> No water subsystem"));
			return sol::lua_nil;
		}

		float Height = Subsystem->GetOceanBaseHeight();
		Session.Log(FString::Printf(TEXT("[OK] water_ocean_get_height -> %.2f"), Height));
		return sol::make_object(LuaView, Height);
	});

	// ================================================================
	// water_remove_body(name_or_label)
	// ================================================================
	Lua.set_function("water_remove_body", [&Session](const std::string& NameStr, sol::this_state S) -> bool
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_remove_body -> No editor world"));
			return false;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AWaterBody* WB = FindWaterBodyByName(World, Name);
		if (!WB)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_remove_body -> Could not find water body '%s'"), *Name));
			return false;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Remove Water Body")));

		UEditorActorSubsystem* ActorSub = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
		if (ActorSub && ActorSub->DestroyActor(WB))
		{
			Session.Log(FString::Printf(TEXT("[OK] water_remove_body -> Removed '%s'"), *Name));
			return true;
		}

		Session.Log(FString::Printf(TEXT("[FAIL] water_remove_body -> Failed to destroy '%s'"), *Name));
		return false;
	});

	// ================================================================
	// water_list_buoyancy()
	// ================================================================
	Lua.set_function("water_list_buoyancy", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_list_buoyancy -> No editor world"));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor) continue;

			UBuoyancyComponent* Buoy = Actor->FindComponentByClass<UBuoyancyComponent>();
			if (!Buoy) continue;

			sol::table Entry = LuaView.create_table();
			Entry["name"] = TCHAR_TO_UTF8(*Actor->GetActorNameOrLabel());
			Entry["class"] = TCHAR_TO_UTF8(*Actor->GetClass()->GetName());
			Entry["pontoon_count"] = Buoy->BuoyancyData.Pontoons.Num();
			Entry["buoyancy_coefficient"] = Buoy->BuoyancyData.BuoyancyCoefficient;
			Entry["in_water"] = Buoy->IsOverlappingWaterBody();
			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] water_list_buoyancy -> %d actors with buoyancy"), Idx - 1));
		return Result;
	});

	// ================================================================
	// water_get_buoyancy(actor_name)
	// ================================================================
	Lua.set_function("water_get_buoyancy", [&Session](const std::string& NameStr, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_get_buoyancy -> No editor world"));
			return sol::lua_nil;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AActor* Actor = FindActorWithBuoyancyByName(World, Name);
		if (!Actor)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_get_buoyancy -> No actor with buoyancy component named '%s'"), *Name));
			return sol::lua_nil;
		}

		UBuoyancyComponent* Buoy = Actor->FindComponentByClass<UBuoyancyComponent>();
		const FBuoyancyData& Data = Buoy->BuoyancyData;

		sol::table Result = LuaView.create_table();
		Result["actor_name"] = TCHAR_TO_UTF8(*Actor->GetActorNameOrLabel());

		// Core buoyancy
		Result["buoyancy_coefficient"] = Data.BuoyancyCoefficient;
		Result["buoyancy_damp"] = Data.BuoyancyDamp;
		Result["buoyancy_damp2"] = Data.BuoyancyDamp2;
		Result["ramp_min_velocity"] = Data.BuoyancyRampMinVelocity;
		Result["ramp_max_velocity"] = Data.BuoyancyRampMaxVelocity;
		Result["ramp_max"] = Data.BuoyancyRampMax;
		Result["max_buoyant_force"] = Data.MaxBuoyantForce;
		Result["center_on_com"] = Data.bCenterPontoonsOnCOM;

		// Drag
		Result["apply_drag"] = Data.bApplyDragForcesInWater;
		Result["drag_coefficient"] = Data.DragCoefficient;
		Result["drag_coefficient2"] = Data.DragCoefficient2;
		Result["angular_drag_coefficient"] = Data.AngularDragCoefficient;
		Result["max_drag_speed"] = Data.MaxDragSpeed;

		// Pontoons
		sol::table PontoonsTable = LuaView.create_table();
		for (int32 i = 0; i < Data.Pontoons.Num(); i++)
		{
			const FSphericalPontoon& P = Data.Pontoons[i];
			sol::table Pt = LuaView.create_table();
			Pt["index"] = i + 1;
			Pt["socket"] = TCHAR_TO_UTF8(*P.CenterSocket.ToString());
			Pt["location"] = VectorToTable(LuaView, P.RelativeLocation);
			Pt["radius"] = P.Radius;
			Pt["fx_enabled"] = P.bFXEnabled;
			Pt["is_in_water"] = P.bIsInWater;
			PontoonsTable[i + 1] = Pt;
		}
		Result["pontoons"] = PontoonsTable;

		// River behavior
		sol::table River = LuaView.create_table();
		River["apply_forces"] = Data.bApplyRiverForces;
		River["pontoon_index"] = Data.RiverPontoonIndex + 1; // 1-based
		River["shore_push_factor"] = Data.WaterShorePushFactor;
		River["path_width"] = Data.RiverTraversalPathWidth;
		River["max_shore_force"] = Data.MaxShorePushForce;
		River["velocity_strength"] = Data.WaterVelocityStrength;
		River["max_water_force"] = Data.MaxWaterForce;
		River["always_lateral_push"] = Data.bAlwaysAllowLateralPush;
		River["allow_upstream_current"] = Data.bAllowCurrentWhenMovingFastUpstream;
		River["apply_downstream_rotation"] = Data.bApplyDownstreamAngularRotation;
		River["downstream_axis"] = VectorToTable(LuaView, Data.DownstreamAxisOfRotation);
		River["downstream_rotation_strength"] = Data.DownstreamRotationStrength;
		River["downstream_rotation_stiffness"] = Data.DownstreamRotationStiffness;
		River["downstream_angular_damping"] = Data.DownstreamRotationAngularDamping;
		River["downstream_max_acceleration"] = Data.DownstreamMaxAcceleration;
		Result["river"] = River;

		// Runtime state
		Result["is_overlapping_water"] = Buoy->IsOverlappingWaterBody();
		Result["is_in_water"] = Buoy->IsInWaterBody();

		Session.Log(FString::Printf(TEXT("[OK] water_get_buoyancy -> '%s' (%d pontoons)"), *Name, Data.Pontoons.Num()));
		return Result;
	});

	// ================================================================
	// water_set_buoyancy(actor_name, params)
	// ================================================================
	Lua.set_function("water_set_buoyancy", [&Session](const std::string& NameStr, const sol::table& Params, sol::this_state S) -> bool
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_set_buoyancy -> No editor world"));
			return false;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AActor* Actor = FindActorWithBuoyancyByName(World, Name);
		if (!Actor)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_set_buoyancy -> No actor with buoyancy component named '%s'"), *Name));
			return false;
		}

		UBuoyancyComponent* Buoy = Actor->FindComponentByClass<UBuoyancyComponent>();
		if (!Buoy)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_set_buoyancy -> actor '%s' has no BuoyancyComponent"), *Name));
			return false;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Set Buoyancy")));
		Buoy->Modify();

		FBuoyancyData& Data = Buoy->BuoyancyData;

		// Core buoyancy
		Data.BuoyancyCoefficient = static_cast<float>(Params.get_or("buoyancy_coefficient", static_cast<double>(Data.BuoyancyCoefficient)));
		Data.BuoyancyDamp = static_cast<float>(Params.get_or("buoyancy_damp", static_cast<double>(Data.BuoyancyDamp)));
		Data.BuoyancyDamp2 = static_cast<float>(Params.get_or("buoyancy_damp2", static_cast<double>(Data.BuoyancyDamp2)));
		Data.BuoyancyRampMinVelocity = static_cast<float>(Params.get_or("ramp_min_velocity", static_cast<double>(Data.BuoyancyRampMinVelocity)));
		Data.BuoyancyRampMaxVelocity = static_cast<float>(Params.get_or("ramp_max_velocity", static_cast<double>(Data.BuoyancyRampMaxVelocity)));
		Data.BuoyancyRampMax = static_cast<float>(Params.get_or("ramp_max", static_cast<double>(Data.BuoyancyRampMax)));
		Data.MaxBuoyantForce = static_cast<float>(Params.get_or("max_buoyant_force", static_cast<double>(Data.MaxBuoyantForce)));

		sol::optional<bool> CenterOnCOMOpt = Params.get<sol::optional<bool>>("center_on_com");
		if (CenterOnCOMOpt.has_value()) Data.bCenterPontoonsOnCOM = CenterOnCOMOpt.value();

		// Drag
		sol::optional<bool> ApplyDragOpt = Params.get<sol::optional<bool>>("apply_drag");
		if (ApplyDragOpt.has_value()) Data.bApplyDragForcesInWater = ApplyDragOpt.value();

		Data.DragCoefficient = static_cast<float>(Params.get_or("drag_coefficient", static_cast<double>(Data.DragCoefficient)));
		Data.DragCoefficient2 = static_cast<float>(Params.get_or("drag_coefficient2", static_cast<double>(Data.DragCoefficient2)));
		Data.AngularDragCoefficient = static_cast<float>(Params.get_or("angular_drag_coefficient", static_cast<double>(Data.AngularDragCoefficient)));
		Data.MaxDragSpeed = static_cast<float>(Params.get_or("max_drag_speed", static_cast<double>(Data.MaxDragSpeed)));

		// River behavior (nested table)
		sol::optional<sol::table> RiverOpt = Params.get<sol::optional<sol::table>>("river");
		if (RiverOpt.has_value())
		{
			sol::table R = RiverOpt.value();

			sol::optional<bool> ApplyRiverOpt = R.get<sol::optional<bool>>("apply_forces");
			if (ApplyRiverOpt.has_value()) Data.bApplyRiverForces = ApplyRiverOpt.value();

			sol::optional<int> PontoonIdxOpt = R.get<sol::optional<int>>("pontoon_index");
			if (PontoonIdxOpt.has_value()) Data.RiverPontoonIndex = PontoonIdxOpt.value() - 1; // 1-based to 0-based

			Data.WaterShorePushFactor = static_cast<float>(R.get_or("shore_push_factor", static_cast<double>(Data.WaterShorePushFactor)));
			Data.RiverTraversalPathWidth = static_cast<float>(R.get_or("path_width", static_cast<double>(Data.RiverTraversalPathWidth)));
			Data.MaxShorePushForce = static_cast<float>(R.get_or("max_shore_force", static_cast<double>(Data.MaxShorePushForce)));
			Data.WaterVelocityStrength = static_cast<float>(R.get_or("velocity_strength", static_cast<double>(Data.WaterVelocityStrength)));
			Data.MaxWaterForce = static_cast<float>(R.get_or("max_water_force", static_cast<double>(Data.MaxWaterForce)));

			sol::optional<bool> LateralOpt = R.get<sol::optional<bool>>("always_lateral_push");
			if (LateralOpt.has_value()) Data.bAlwaysAllowLateralPush = LateralOpt.value();

			sol::optional<bool> UpstreamOpt = R.get<sol::optional<bool>>("allow_upstream_current");
			if (UpstreamOpt.has_value()) Data.bAllowCurrentWhenMovingFastUpstream = UpstreamOpt.value();

			sol::optional<bool> DownstreamRotOpt = R.get<sol::optional<bool>>("apply_downstream_rotation");
			if (DownstreamRotOpt.has_value()) Data.bApplyDownstreamAngularRotation = DownstreamRotOpt.value();

			sol::optional<sol::table> AxisOpt = R.get<sol::optional<sol::table>>("downstream_axis");
			if (AxisOpt.has_value()) Data.DownstreamAxisOfRotation = TableToVector(AxisOpt.value());

			Data.DownstreamRotationStrength = static_cast<float>(R.get_or("downstream_rotation_strength", static_cast<double>(Data.DownstreamRotationStrength)));
			Data.DownstreamRotationStiffness = static_cast<float>(R.get_or("downstream_rotation_stiffness", static_cast<double>(Data.DownstreamRotationStiffness)));
			Data.DownstreamRotationAngularDamping = static_cast<float>(R.get_or("downstream_angular_damping", static_cast<double>(Data.DownstreamRotationAngularDamping)));
			Data.DownstreamMaxAcceleration = static_cast<float>(R.get_or("downstream_max_acceleration", static_cast<double>(Data.DownstreamMaxAcceleration)));
		}

		Buoy->UpdatePontoonCoefficients();

		Session.Log(FString::Printf(TEXT("[OK] water_set_buoyancy -> Configured buoyancy on '%s'"), *Name));
		return true;
	});

	// ================================================================
	// water_add_pontoon(actor_name, params)
	// ================================================================
	Lua.set_function("water_add_pontoon", [&Session](const std::string& NameStr, const sol::table& Params, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_add_pontoon -> No editor world"));
			return sol::lua_nil;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AActor* Actor = FindActorWithBuoyancyByName(World, Name);
		if (!Actor)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_add_pontoon -> No actor with buoyancy component named '%s'"), *Name));
			return sol::lua_nil;
		}

		UBuoyancyComponent* Buoy = Actor->FindComponentByClass<UBuoyancyComponent>();

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Add Pontoon")));
		Buoy->Modify();

		float Radius = static_cast<float>(Params.get_or("radius", 100.0));

		sol::optional<std::string> SocketOpt = Params.get<sol::optional<std::string>>("socket");
		if (SocketOpt.has_value())
		{
			Buoy->AddCustomPontoon(Radius, FName(UTF8_TO_TCHAR(SocketOpt.value().c_str())));
		}
		else
		{
			FVector RelLoc = FVector::ZeroVector;
			sol::optional<sol::table> LocOpt = Params.get<sol::optional<sol::table>>("location");
			if (LocOpt.has_value())
			{
				RelLoc = TableToVector(LocOpt.value());
			}
			Buoy->AddCustomPontoon(Radius, RelLoc);
		}

		int32 NewIdx = Buoy->BuoyancyData.Pontoons.Num(); // already added, 1-based = Num()

		// Apply fx_enabled if specified
		sol::optional<bool> FxOpt = Params.get<sol::optional<bool>>("fx_enabled");
		if (FxOpt.has_value() && Buoy->BuoyancyData.Pontoons.IsValidIndex(NewIdx - 1))
		{
			Buoy->BuoyancyData.Pontoons[NewIdx - 1].bFXEnabled = FxOpt.value();
		}

		Buoy->UpdatePontoonCoefficients();

		Session.Log(FString::Printf(TEXT("[OK] water_add_pontoon -> Added pontoon %d to '%s' (radius=%.1f)"), NewIdx, *Name, Radius));
		return sol::make_object(LuaView, NewIdx);
	});

	// ================================================================
	// water_remove_pontoon(actor_name, index)
	// ================================================================
	Lua.set_function("water_remove_pontoon", [&Session](const std::string& NameStr, int LuaIndex, sol::this_state S) -> bool
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_remove_pontoon -> No editor world"));
			return false;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AActor* Actor = FindActorWithBuoyancyByName(World, Name);
		if (!Actor)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_remove_pontoon -> No actor with buoyancy component named '%s'"), *Name));
			return false;
		}

		UBuoyancyComponent* Buoy = Actor->FindComponentByClass<UBuoyancyComponent>();

		int32 Index = LuaIndex - 1; // 1-based to 0-based
		if (Index < 0 || Index >= Buoy->BuoyancyData.Pontoons.Num())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_remove_pontoon -> Index %d out of range [1..%d]"), LuaIndex, Buoy->BuoyancyData.Pontoons.Num()));
			return false;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Remove Pontoon")));
		Buoy->Modify();

		Buoy->BuoyancyData.Pontoons.RemoveAt(Index);
		Buoy->UpdatePontoonCoefficients();

		Session.Log(FString::Printf(TEXT("[OK] water_remove_pontoon -> Removed pontoon %d from '%s' (%d remaining)"), LuaIndex, *Name, Buoy->BuoyancyData.Pontoons.Num()));
		return true;
	});

	// ================================================================
	// water_query_surface(location)
	// ================================================================
	Lua.set_function("water_query_surface", [&Session](const sol::table& LocationTable, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_query_surface -> No editor world"));
			return sol::lua_nil;
		}

		FVector QueryLoc = TableToVector(LocationTable);

		// Find the closest water body using spline-based distance (not actor origin)
		AWaterBody* ClosestBody = nullptr;
		float ClosestDist = TNumericLimits<float>::Max();

		for (TActorIterator<AWaterBody> It(World); It; ++It)
		{
			AWaterBody* WB = *It;
			if (!WB || !WB->GetWaterBodyComponent()) continue;

			UWaterBodyComponent* WBComp = WB->GetWaterBodyComponent();
			float SplineKey = WBComp->FindInputKeyClosestToWorldLocation(QueryLoc);
			UWaterSplineComponent* WBSpline = WB->GetWaterSpline();
			float Dist;
			if (WBSpline)
			{
				FVector ClosestPoint = WBSpline->GetLocationAtSplineInputKey(SplineKey, ESplineCoordinateSpace::World);
				Dist = static_cast<float>(FVector::Dist(ClosestPoint, QueryLoc));
			}
			else
			{
				Dist = static_cast<float>(FVector::Dist(WB->GetActorLocation(), QueryLoc));
			}
			if (Dist < ClosestDist)
			{
				ClosestDist = Dist;
				ClosestBody = WB;
			}
		}

		if (!ClosestBody)
		{
			Session.Log(TEXT("[FAIL] water_query_surface -> No water bodies in the world"));
			return sol::lua_nil;
		}

		UWaterBodyComponent* Comp = ClosestBody->GetWaterBodyComponent();
		FVector SurfaceLocation;
		FVector SurfaceNormal;
		FVector Velocity;
		float Depth = 0.f;

	
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		bool bSuccess = Comp->GetWaterSurfaceInfoAtLocation(QueryLoc, SurfaceLocation, SurfaceNormal, Velocity, Depth, true);
#else
		Comp->GetWaterSurfaceInfoAtLocation(QueryLoc, SurfaceLocation, SurfaceNormal, Velocity, Depth, true);
		bool bSuccess = true;
#endif
		if (!bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_query_surface -> Query failed on '%s'"), *ClosestBody->GetActorNameOrLabel()));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		Result["surface_location"] = VectorToTable(LuaView, SurfaceLocation);
		Result["surface_normal"] = VectorToTable(LuaView, SurfaceNormal);
		Result["velocity"] = VectorToTable(LuaView, Velocity);
		Result["depth"] = Depth;
		Result["water_body"] = TCHAR_TO_UTF8(*ClosestBody->GetActorNameOrLabel());

		Session.Log(FString::Printf(TEXT("[OK] water_query_surface -> Queried '%s' at (%.0f, %.0f, %.0f)"), *ClosestBody->GetActorNameOrLabel(), QueryLoc.X, QueryLoc.Y, QueryLoc.Z));
		return Result;
	});

	// ================================================================
	// water_create_island(params)
	// ================================================================
	Lua.set_function("water_create_island", [&Session](const sol::table& Params, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_create_island -> No editor world"));
			return sol::lua_nil;
		}

		// Location
		FVector Location = FVector::ZeroVector;
		sol::optional<sol::table> LocOpt = Params.get<sol::optional<sol::table>>("location");
		if (LocOpt.has_value())
		{
			Location = TableToVector(LocOpt.value());
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Create Water Body Island")));

		FTransform SpawnTransform(FRotator::ZeroRotator, Location);
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWaterBodyIsland* Island = World->SpawnActor<AWaterBodyIsland>(AWaterBodyIsland::StaticClass(), SpawnTransform, SpawnParams);
		if (!Island)
		{
			Session.Log(TEXT("[FAIL] water_create_island -> SpawnActor returned null"));
			return sol::lua_nil;
		}

		// Label
		sol::optional<std::string> LabelOpt = Params.get<sol::optional<std::string>>("label");
		if (LabelOpt.has_value())
		{
			Island->SetActorLabel(UTF8_TO_TCHAR(LabelOpt.value().c_str()));
		}

		// Apply editor defaults (mirrors UWaterBodyIslandActorFactory::PostSpawnActor)
		const UWaterEditorSettings* EditorSettings = GetDefault<UWaterEditorSettings>();
		const FWaterBrushActorDefaults& IslandDefaults = EditorSettings->WaterBodyIslandDefaults.BrushDefaults;
		Island->WaterCurveSettings = IslandDefaults.CurveSettings;
		Island->WaterHeightmapSettings = IslandDefaults.HeightmapSettings;
		Island->WaterWeightmapSettings = IslandDefaults.LayerWeightmapSettings;

		// Spline points
		sol::optional<sol::table> PointsOpt = Params.get<sol::optional<sol::table>>("points");
		UWaterSplineComponent* SplineComp = Island->GetWaterSpline();
		if (PointsOpt.has_value() && SplineComp)
		{
			SplineComp->ClearSplinePoints(false);
			for (auto& Kv : PointsOpt.value())
			{
				sol::optional<sol::table> PtOpt = Kv.second.as<sol::optional<sol::table>>();
				if (PtOpt.has_value())
				{
					FVector WorldPt = TableToVector(PtOpt.value());
					FVector LocalPt = Island->GetActorTransform().InverseTransformPosition(WorldPt);
					SplineComp->AddSplinePoint(LocalPt, ESplineCoordinateSpace::Local, false);
				}
			}
			SplineComp->UpdateSpline();
		}

		// Heightmap settings (user overrides applied on top of defaults)
		sol::optional<sol::table> HeightmapOpt = Params.get<sol::optional<sol::table>>("heightmap");
		if (HeightmapOpt.has_value())
		{
			sol::table HM = HeightmapOpt.value();
			FWaterBodyHeightmapSettings& Settings = Island->WaterHeightmapSettings;

			sol::optional<std::string> BlendModeOpt = HM.get<sol::optional<std::string>>("blend_mode");
			if (BlendModeOpt.has_value())
			{
				FString BM = UTF8_TO_TCHAR(BlendModeOpt.value().c_str());
				if (BM.Equals(TEXT("alpha_blend"), ESearchCase::IgnoreCase)) Settings.BlendMode = EWaterBrushBlendType::AlphaBlend;
				else if (BM.Equals(TEXT("min"), ESearchCase::IgnoreCase)) Settings.BlendMode = EWaterBrushBlendType::Min;
				else if (BM.Equals(TEXT("max"), ESearchCase::IgnoreCase)) Settings.BlendMode = EWaterBrushBlendType::Max;
				else if (BM.Equals(TEXT("additive"), ESearchCase::IgnoreCase)) Settings.BlendMode = EWaterBrushBlendType::Additive;
			}

			Settings.FalloffSettings.FalloffWidth = static_cast<float>(HM.get_or("falloff_width", static_cast<double>(Settings.FalloffSettings.FalloffWidth)));
			Settings.FalloffSettings.EdgeOffset = static_cast<float>(HM.get_or("edge_offset", static_cast<double>(Settings.FalloffSettings.EdgeOffset)));
			Settings.FalloffSettings.ZOffset = static_cast<float>(HM.get_or("z_offset", static_cast<double>(Settings.FalloffSettings.ZOffset)));
		}

		// Weightmap layer settings
		sol::optional<sol::table> LayersOpt = Params.get<sol::optional<sol::table>>("layers");
		if (LayersOpt.has_value())
		{
			Island->WaterWeightmapSettings.Empty();
			for (auto& Kv : LayersOpt.value())
			{
				sol::optional<std::string> LayerNameOpt = Kv.first.as<sol::optional<std::string>>();
				sol::optional<sol::table> LayerDataOpt = Kv.second.as<sol::optional<sol::table>>();
				if (!LayerNameOpt.has_value() || !LayerDataOpt.has_value()) continue;

				FName LayerName = FName(UTF8_TO_TCHAR(LayerNameOpt.value().c_str()));
				sol::table LD = LayerDataOpt.value();

				FWaterBodyWeightmapSettings WS;
				WS.FalloffWidth = static_cast<float>(LD.get_or("falloff_width", static_cast<double>(WS.FalloffWidth)));
				WS.EdgeOffset = static_cast<float>(LD.get_or("edge_offset", static_cast<double>(WS.EdgeOffset)));
				WS.FinalOpacity = static_cast<float>(LD.get_or("opacity", static_cast<double>(WS.FinalOpacity)));
				WS.TextureTiling = static_cast<float>(LD.get_or("texture_tiling", static_cast<double>(WS.TextureTiling)));
				WS.TextureInfluence = static_cast<float>(LD.get_or("texture_influence", static_cast<double>(WS.TextureInfluence)));
				WS.Midpoint = static_cast<float>(LD.get_or("midpoint", static_cast<double>(WS.Midpoint)));

				Island->WaterWeightmapSettings.Add(LayerName, WS);
			}
		}

#if WITH_EDITOR
		Island->UpdateOverlappingWaterBodyComponents();
#endif

		FString ActorName = Island->GetActorNameOrLabel();
		Session.Log(FString::Printf(TEXT("[OK] water_create_island -> Spawned island '%s' with %d spline points"), *ActorName, SplineComp ? SplineComp->GetNumberOfSplinePoints() : 0));
		return sol::make_object(LuaView, std::string(TCHAR_TO_UTF8(*ActorName)));
	});

	// ================================================================
	// water_create_zone(params)
	// ================================================================
	Lua.set_function("water_create_zone", [&Session](const sol::table& Params, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_create_zone -> No editor world"));
			return sol::lua_nil;
		}

		FVector Location = FVector::ZeroVector;
		sol::optional<sol::table> LocOpt = Params.get<sol::optional<sol::table>>("location");
		if (LocOpt.has_value())
		{
			Location = TableToVector(LocOpt.value());
		}

		double Width = Params.get_or("width", 51200.0);
		double Height = Params.get_or("height", 51200.0);

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Create Water Zone")));

		FTransform SpawnTransform(FRotator::ZeroRotator, Location);
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWaterZone* Zone = World->SpawnActor<AWaterZone>(AWaterZone::StaticClass(), SpawnTransform, SpawnParams);
		if (!Zone)
		{
			Session.Log(TEXT("[FAIL] water_create_zone -> SpawnActor returned null"));
			return sol::lua_nil;
		}

		// Label
		sol::optional<std::string> LabelOpt = Params.get<sol::optional<std::string>>("label");
		if (LabelOpt.has_value())
		{
			Zone->SetActorLabel(UTF8_TO_TCHAR(LabelOpt.value().c_str()));
		}

		Zone->SetZoneExtent(FVector2D(Width, Height));

		// Resolution
		sol::optional<int> ResXOpt = Params.get<sol::optional<int>>("resolution_x");
		sol::optional<int> ResYOpt = Params.get<sol::optional<int>>("resolution_y");
		if (ResXOpt.has_value() || ResYOpt.has_value())
		{
			FIntPoint CurrentRes = Zone->GetRenderTargetResolution();
			int32 ResX = ResXOpt.has_value() ? ResXOpt.value() : CurrentRes.X;
			int32 ResY = ResYOpt.has_value() ? ResYOpt.value() : CurrentRes.Y;
			Zone->SetRenderTargetResolution(FIntPoint(ResX, ResY));
		}

		// Tile size
		sol::optional<double> TileSizeOpt = Params.get<sol::optional<double>>("tile_size");
		if (TileSizeOpt.has_value())
		{
			UWaterMeshComponent* WaterMesh = Zone->GetWaterMeshComponent();
			if (WaterMesh)
			{
				WaterMesh->SetTileSize(static_cast<float>(TileSizeOpt.value()));
			}
		}

		Zone->MarkForRebuild(EWaterZoneRebuildFlags::All);

		FString ActorName = Zone->GetActorNameOrLabel();
		Session.Log(FString::Printf(TEXT("[OK] water_create_zone -> Spawned zone '%s' (%.0f x %.0f)"), *ActorName, Width, Height));
		return sol::make_object(LuaView, std::string(TCHAR_TO_UTF8(*ActorName)));
	});

	// ================================================================
	// water_set_landscape_painting(actor_name, params)
	// ================================================================
	Lua.set_function("water_set_landscape_painting", [&Session](const std::string& NameStr, const sol::table& Params, sol::this_state S) -> bool
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_set_landscape_painting -> No editor world"));
			return false;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AWaterBody* WB = FindWaterBodyByName(World, Name);
		if (!WB)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_set_landscape_painting -> Could not find water body '%s'"), *Name));
			return false;
		}

		UWaterBodyComponent* Comp = WB->GetWaterBodyComponent();
		if (!Comp)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_set_landscape_painting -> Water body '%s' has no component"), *Name));
			return false;
		}

		sol::optional<sol::table> LayersOpt = Params.get<sol::optional<sol::table>>("layers");
		if (!LayersOpt.has_value())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_set_landscape_painting -> No 'layers' table provided")));
			return false;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Set Water Landscape Painting")));
		Comp->Modify();

		Comp->LayerWeightmapSettings.Empty();
		int32 LayerCount = 0;

		for (auto& Kv : LayersOpt.value())
		{
			sol::optional<std::string> LayerNameOpt = Kv.first.as<sol::optional<std::string>>();
			sol::optional<sol::table> LayerDataOpt = Kv.second.as<sol::optional<sol::table>>();
			if (!LayerNameOpt.has_value() || !LayerDataOpt.has_value()) continue;

			FName LayerName = FName(UTF8_TO_TCHAR(LayerNameOpt.value().c_str()));
			sol::table LD = LayerDataOpt.value();

			FWaterBodyWeightmapSettings WS;
			WS.FalloffWidth = static_cast<float>(LD.get_or("falloff_width", static_cast<double>(WS.FalloffWidth)));
			WS.EdgeOffset = static_cast<float>(LD.get_or("edge_offset", static_cast<double>(WS.EdgeOffset)));
			WS.FinalOpacity = static_cast<float>(LD.get_or("opacity", static_cast<double>(WS.FinalOpacity)));
			WS.TextureTiling = static_cast<float>(LD.get_or("texture_tiling", static_cast<double>(WS.TextureTiling)));
			WS.TextureInfluence = static_cast<float>(LD.get_or("texture_influence", static_cast<double>(WS.TextureInfluence)));
			WS.Midpoint = static_cast<float>(LD.get_or("midpoint", static_cast<double>(WS.Midpoint)));

			Comp->LayerWeightmapSettings.Add(LayerName, WS);
			LayerCount++;
		}

		FOnWaterBodyChangedParams ChangedParams;
		ChangedParams.bWeightmapSettingsChanged = true;
		ChangedParams.bUserTriggered = true;
		Comp->UpdateAll(ChangedParams);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		Comp->MarkOwningWaterZoneForRebuild(EWaterZoneRebuildFlags::All);
#endif

		Session.Log(FString::Printf(TEXT("[OK] water_set_landscape_painting -> Set %d layer(s) on '%s'"), LayerCount, *Name));
		return true;
	});

	// ================================================================
	// water_set_brush_effects(actor_name, params)
	// ================================================================
	Lua.set_function("water_set_brush_effects", [&Session](const std::string& NameStr, const sol::table& Params, sol::this_state S) -> bool
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_set_brush_effects -> No editor world"));
			return false;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AWaterBody* WB = FindWaterBodyByName(World, Name);
		if (!WB)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_set_brush_effects -> Could not find water body '%s'"), *Name));
			return false;
		}

		UWaterBodyComponent* Comp = WB->GetWaterBodyComponent();
		if (!Comp)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_set_brush_effects -> Water body '%s' has no component"), *Name));
			return false;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Set Water Brush Effects")));
		Comp->Modify();

		FWaterBodyHeightmapSettings& HM = Comp->WaterHeightmapSettings;

		// Blend mode
		sol::optional<std::string> BlendModeOpt = Params.get<sol::optional<std::string>>("blend_mode");
		if (BlendModeOpt.has_value())
		{
			FString BM = UTF8_TO_TCHAR(BlendModeOpt.value().c_str());
			if (BM.Equals(TEXT("alpha_blend"), ESearchCase::IgnoreCase)) HM.BlendMode = EWaterBrushBlendType::AlphaBlend;
			else if (BM.Equals(TEXT("min"), ESearchCase::IgnoreCase)) HM.BlendMode = EWaterBrushBlendType::Min;
			else if (BM.Equals(TEXT("max"), ESearchCase::IgnoreCase)) HM.BlendMode = EWaterBrushBlendType::Max;
			else if (BM.Equals(TEXT("additive"), ESearchCase::IgnoreCase)) HM.BlendMode = EWaterBrushBlendType::Additive;
		}

		// Falloff settings
		HM.FalloffSettings.FalloffWidth = static_cast<float>(Params.get_or("falloff_width", static_cast<double>(HM.FalloffSettings.FalloffWidth)));
		HM.FalloffSettings.EdgeOffset = static_cast<float>(Params.get_or("edge_offset", static_cast<double>(HM.FalloffSettings.EdgeOffset)));
		HM.FalloffSettings.ZOffset = static_cast<float>(Params.get_or("z_offset", static_cast<double>(HM.FalloffSettings.ZOffset)));

		// Blurring
		sol::optional<sol::table> BlurOpt = Params.get<sol::optional<sol::table>>("blurring");
		if (BlurOpt.has_value())
		{
			sol::table B = BlurOpt.value();
			sol::optional<bool> EnabledOpt = B.get<sol::optional<bool>>("enabled");
			if (EnabledOpt.has_value()) HM.Effects.Blurring.bBlurShape = EnabledOpt.value();
			HM.Effects.Blurring.Radius = static_cast<int32>(B.get_or("radius", HM.Effects.Blurring.Radius));
		}

		// Curl noise
		sol::optional<sol::table> CurlOpt = Params.get<sol::optional<sol::table>>("curl_noise");
		if (CurlOpt.has_value())
		{
			sol::table C = CurlOpt.value();
			HM.Effects.CurlNoise.Curl1Amount = static_cast<float>(C.get_or("amount1", static_cast<double>(HM.Effects.CurlNoise.Curl1Amount)));
			HM.Effects.CurlNoise.Curl2Amount = static_cast<float>(C.get_or("amount2", static_cast<double>(HM.Effects.CurlNoise.Curl2Amount)));
			HM.Effects.CurlNoise.Curl1Tiling = static_cast<float>(C.get_or("tiling1", static_cast<double>(HM.Effects.CurlNoise.Curl1Tiling)));
			HM.Effects.CurlNoise.Curl2Tiling = static_cast<float>(C.get_or("tiling2", static_cast<double>(HM.Effects.CurlNoise.Curl2Tiling)));
		}

		// Displacement
		sol::optional<sol::table> DispOpt = Params.get<sol::optional<sol::table>>("displacement");
		if (DispOpt.has_value())
		{
			sol::table D = DispOpt.value();
			HM.Effects.Displacement.DisplacementHeight = static_cast<float>(D.get_or("height", static_cast<double>(HM.Effects.Displacement.DisplacementHeight)));
			HM.Effects.Displacement.DisplacementTiling = static_cast<float>(D.get_or("tiling", static_cast<double>(HM.Effects.Displacement.DisplacementTiling)));
			HM.Effects.Displacement.Midpoint = static_cast<float>(D.get_or("midpoint", static_cast<double>(HM.Effects.Displacement.Midpoint)));
		}

		// Terracing
		sol::optional<sol::table> TerrOpt = Params.get<sol::optional<sol::table>>("terracing");
		if (TerrOpt.has_value())
		{
			sol::table T = TerrOpt.value();
			HM.Effects.Terracing.TerraceAlpha = static_cast<float>(T.get_or("alpha", static_cast<double>(HM.Effects.Terracing.TerraceAlpha)));
			HM.Effects.Terracing.TerraceSpacing = static_cast<float>(T.get_or("spacing", static_cast<double>(HM.Effects.Terracing.TerraceSpacing)));
			HM.Effects.Terracing.TerraceSmoothness = static_cast<float>(T.get_or("smoothness", static_cast<double>(HM.Effects.Terracing.TerraceSmoothness)));
		}

		// Smooth blending
		sol::optional<sol::table> SmoothOpt = Params.get<sol::optional<sol::table>>("smooth_blending");
		if (SmoothOpt.has_value())
		{
			sol::table SB = SmoothOpt.value();
			HM.Effects.SmoothBlending.InnerSmoothDistance = static_cast<float>(SB.get_or("inner", static_cast<double>(HM.Effects.SmoothBlending.InnerSmoothDistance)));
			HM.Effects.SmoothBlending.OuterSmoothDistance = static_cast<float>(SB.get_or("outer", static_cast<double>(HM.Effects.SmoothBlending.OuterSmoothDistance)));
		}

		FOnWaterBodyChangedParams ChangedParams;
		ChangedParams.bShapeOrPositionChanged = true;
		ChangedParams.bUserTriggered = true;
		Comp->UpdateAll(ChangedParams);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		Comp->MarkOwningWaterZoneForRebuild(EWaterZoneRebuildFlags::All);
#endif

		Session.Log(FString::Printf(TEXT("[OK] water_set_brush_effects -> Configured brush effects on '%s'"), *Name));
		return true;
	});

	// ================================================================
	// water_ocean_fill_zone(actor_name)
	// ================================================================
	Lua.set_function("water_ocean_fill_zone", [&Session](const std::string& NameStr, sol::this_state S) -> bool
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_ocean_fill_zone -> No editor world"));
			return false;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AWaterBody* WB = FindWaterBodyByName(World, Name);
		if (!WB)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_ocean_fill_zone -> Could not find water body '%s'"), *Name));
			return false;
		}

		AWaterBodyOcean* Ocean = Cast<AWaterBodyOcean>(WB);
		if (!Ocean)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_ocean_fill_zone -> '%s' is not an ocean water body"), *Name));
			return false;
		}

		UWaterBodyOceanComponent* OceanComp = Cast<UWaterBodyOceanComponent>(Ocean->GetWaterBodyComponent());
		if (!OceanComp)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_ocean_fill_zone -> '%s' has no ocean component"), *Name));
			return false;
		}

#if WITH_EDITOR
		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Ocean Fill Zone")));
		Ocean->Modify();

		OceanComp->FillWaterZoneWithOcean();

		FOnWaterBodyChangedParams ChangedParams;
		ChangedParams.bShapeOrPositionChanged = true;
		ChangedParams.bUserTriggered = true;
		OceanComp->UpdateAll(ChangedParams);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		OceanComp->MarkOwningWaterZoneForRebuild(EWaterZoneRebuildFlags::All);
#endif

		Session.Log(FString::Printf(TEXT("[OK] water_ocean_fill_zone -> Filled zone with ocean '%s'"), *Name));
		return true;
#else
		Session.Log(TEXT("[FAIL] water_ocean_fill_zone -> Only available in editor builds"));
		return false;
#endif
	});

	// ================================================================
	// water_configure_body(actor_name, params)
	// ================================================================
	Lua.set_function("water_configure_body", [&Session](const std::string& NameStr, const sol::table& Params, sol::this_state S) -> bool
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_configure_body -> No editor world"));
			return false;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AWaterBody* WB = FindWaterBodyByName(World, Name);
		if (!WB)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_configure_body -> Could not find water body '%s'"), *Name));
			return false;
		}

		UWaterBodyComponent* Comp = WB->GetWaterBodyComponent();
		if (!Comp)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_configure_body -> Water body '%s' has no component"), *Name));
			return false;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Configure Water Body")));
		Comp->Modify();

		// Overlap material priority (-8192 to 8191)
		sol::optional<int> PriorityOpt = Params.get<sol::optional<int>>("overlap_material_priority");
		if (PriorityOpt.has_value())
		{
			// OverlapMaterialPriority is protected UPROPERTY, use reflection
			FProperty* Prop = Comp->GetClass()->FindPropertyByName(TEXT("OverlapMaterialPriority"));
			if (Prop)
			{
				int32 Val = FMath::Clamp(PriorityOpt.value(), -8192, 8191);
				int32* ValuePtr = Prop->ContainerPtrToValuePtr<int32>(Comp);
				*ValuePtr = Val;
			}
		}

		// Target wave mask depth
		sol::optional<double> WaveMaskOpt = Params.get<sol::optional<double>>("target_wave_mask_depth");
		if (WaveMaskOpt.has_value())
		{
			Comp->TargetWaveMaskDepth = static_cast<float>(WaveMaskOpt.value());
		}

		// Max wave height offset
		sol::optional<double> WaveHeightOffsetOpt = Params.get<sol::optional<double>>("max_wave_height_offset");
		if (WaveHeightOffsetOpt.has_value())
		{
			Comp->MaxWaveHeightOffset = static_cast<float>(WaveHeightOffsetOpt.value());
		}

		// Collision height offset
		sol::optional<double> CollisionOffsetOpt = Params.get<sol::optional<double>>("collision_height_offset");
		if (CollisionOffsetOpt.has_value())
		{
			Comp->CollisionHeightOffset = static_cast<float>(CollisionOffsetOpt.value());
		}

		// Shape dilation
		sol::optional<double> ShapeDilationOpt = Params.get<sol::optional<double>>("shape_dilation");
		if (ShapeDilationOpt.has_value())
		{
			Comp->ShapeDilation = static_cast<float>(ShapeDilationOpt.value());
		}

		// Affects landscape
		sol::optional<bool> AffectsLandscapeOpt = Params.get<sol::optional<bool>>("affects_landscape");
		if (AffectsLandscapeOpt.has_value())
		{
			Comp->bAffectsLandscape = AffectsLandscapeOpt.value();
		}

		FOnWaterBodyChangedParams ChangedParams;
		ChangedParams.bShapeOrPositionChanged = true;
		ChangedParams.bWeightmapSettingsChanged = true;
		ChangedParams.bUserTriggered = true;
		Comp->UpdateAll(ChangedParams);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		Comp->MarkOwningWaterZoneForRebuild(EWaterZoneRebuildFlags::All);
#endif

		Session.Log(FString::Printf(TEXT("[OK] water_configure_body -> Configured '%s'"), *Name));
		return true;
	});

	// ================================================================
	// water_get_zone(name_or_label)
	// ================================================================
	Lua.set_function("water_get_zone", [&Session](const std::string& NameStr, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_get_zone -> No editor world"));
			return sol::lua_nil;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AWaterZone* WZ = FindWaterZoneByName(World, Name);
		if (!WZ)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_get_zone -> Could not find water zone '%s'"), *Name));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		Result["name"] = TCHAR_TO_UTF8(*WZ->GetActorNameOrLabel());
		Result["location"] = VectorToTable(LuaView, WZ->GetActorLocation());

		FVector2D Extent = WZ->GetZoneExtent();
		sol::table ExtentTable = LuaView.create_table();
		ExtentTable["x"] = Extent.X;
		ExtentTable["y"] = Extent.Y;
		Result["extent"] = ExtentTable;

		FIntPoint Resolution = WZ->GetRenderTargetResolution();
		sol::table ResTable = LuaView.create_table();
		ResTable["x"] = Resolution.X;
		ResTable["y"] = Resolution.Y;
		Result["resolution"] = ResTable;

		Result["overlap_priority"] = WZ->GetOverlapPriority();
		Result["local_tessellation"] = WZ->IsLocalOnlyTessellationEnabled();
		Result["water_zone_index"] = WZ->GetWaterZoneIndex();
		Result["velocity_blur_radius"] = static_cast<int>(WZ->GetVelocityBlurRadius());

		const UWaterMeshComponent* WaterMesh = WZ->GetWaterMeshComponent();
		if (WaterMesh)
		{
			Result["tile_size"] = WaterMesh->GetTileSize();
			Result["tessellation_factor"] = WaterMesh->GetTessellationFactor();
		}

		// Count water bodies
		int32 BodyCount = 0;
		sol::table Bodies = LuaView.create_table();
		WZ->ForEachWaterBodyComponent([&](UWaterBodyComponent* WBComp) -> bool
		{
			BodyCount++;
			AWaterBody* WBActor = WBComp->GetWaterBodyActor();
			if (WBActor)
			{
				Bodies[BodyCount] = TCHAR_TO_UTF8(*WBActor->GetActorNameOrLabel());
			}
			return true;
		});
		Result["body_count"] = BodyCount;
		Result["water_bodies"] = Bodies;

		Session.Log(FString::Printf(TEXT("[OK] water_get_zone -> '%s' (%d bodies)"), *Name, BodyCount));
		return Result;
	});

	// ================================================================
	// water_set_zone_override(actor_name, zone_name_or_nil)
	// ================================================================
	Lua.set_function("water_set_zone_override", [&Session](const std::string& NameStr, sol::optional<std::string> ZoneNameOpt, sol::this_state S) -> bool
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_set_zone_override -> No editor world"));
			return false;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AWaterBody* WB = FindWaterBodyByName(World, Name);
		if (!WB)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_set_zone_override -> Could not find water body '%s'"), *Name));
			return false;
		}

		UWaterBodyComponent* Comp = WB->GetWaterBodyComponent();
		if (!Comp)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_set_zone_override -> Water body '%s' has no component"), *Name));
			return false;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Set Water Zone Override")));
		Comp->Modify();

		if (ZoneNameOpt.has_value())
		{
			FString ZoneName = UTF8_TO_TCHAR(ZoneNameOpt.value().c_str());
			AWaterZone* Zone = FindWaterZoneByName(World, ZoneName);
			if (!Zone)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] water_set_zone_override -> Could not find water zone '%s'"), *ZoneName));
				return false;
			}

			Comp->SetWaterZoneOverride(TSoftObjectPtr<AWaterZone>(Zone));
			Comp->UpdateWaterZones();

			Session.Log(FString::Printf(TEXT("[OK] water_set_zone_override -> Set '%s' to zone '%s'"), *Name, *ZoneName));
		}
		else
		{
			// Clear override
			Comp->SetWaterZoneOverride(TSoftObjectPtr<AWaterZone>());
			Comp->UpdateWaterZones();

			Session.Log(FString::Printf(TEXT("[OK] water_set_zone_override -> Cleared zone override on '%s'"), *Name));
		}

		return true;
	});

	// ================================================================
	// water_ocean_get_total_height()
	// ================================================================
	Lua.set_function("water_ocean_get_total_height", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_ocean_get_total_height -> No editor world"));
			return sol::lua_nil;
		}

		UWaterSubsystem* Subsystem = UWaterSubsystem::GetWaterSubsystem(World);
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] water_ocean_get_total_height -> No water subsystem"));
			return sol::lua_nil;
		}

		float TotalHeight = Subsystem->GetOceanTotalHeight();
		Session.Log(FString::Printf(TEXT("[OK] water_ocean_get_total_height -> %.2f (base=%.2f, flood=%.2f)"),
			TotalHeight, Subsystem->GetOceanBaseHeight(), Subsystem->GetOceanFloodHeight()));
		return sol::make_object(LuaView, TotalHeight);
	});

	// ================================================================
	// water_add_exclusion_volume(actor_name, volume_name)
	// ================================================================
	Lua.set_function("water_add_exclusion_volume", [&Session](const std::string& NameStr, const std::string& VolumeNameStr, sol::this_state S) -> bool
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_add_exclusion_volume -> No editor world"));
			return false;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AWaterBody* WB = FindWaterBodyByName(World, Name);
		if (!WB)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_add_exclusion_volume -> Could not find water body '%s'"), *Name));
			return false;
		}

		UWaterBodyComponent* Comp = WB->GetWaterBodyComponent();
		if (!Comp)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_add_exclusion_volume -> Water body '%s' has no component"), *Name));
			return false;
		}

		FString VolumeName = UTF8_TO_TCHAR(VolumeNameStr.c_str());
		AWaterBodyExclusionVolume* Volume = nullptr;
		for (TActorIterator<AWaterBodyExclusionVolume> It(World); It; ++It)
		{
			if ((*It)->GetActorLabel() == VolumeName || (*It)->GetName() == VolumeName || (*It)->GetActorNameOrLabel() == VolumeName)
			{
				Volume = *It;
				break;
			}
		}

		if (!Volume)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_add_exclusion_volume -> Could not find exclusion volume '%s'"), *VolumeName));
			return false;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Add Exclusion Volume")));
		Comp->Modify();
		Comp->AddExclusionVolume(Volume);

		FOnWaterBodyChangedParams ChangedParams;
		ChangedParams.bShapeOrPositionChanged = true;
		ChangedParams.bUserTriggered = true;
		Comp->UpdateAll(ChangedParams);

		Session.Log(FString::Printf(TEXT("[OK] water_add_exclusion_volume -> Added '%s' to '%s'"), *VolumeName, *Name));
		return true;
	});

	// ================================================================
	// water_remove_exclusion_volume(actor_name, volume_name)
	// ================================================================
	Lua.set_function("water_remove_exclusion_volume", [&Session](const std::string& NameStr, const std::string& VolumeNameStr, sol::this_state S) -> bool
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_remove_exclusion_volume -> No editor world"));
			return false;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AWaterBody* WB = FindWaterBodyByName(World, Name);
		if (!WB)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_remove_exclusion_volume -> Could not find water body '%s'"), *Name));
			return false;
		}

		UWaterBodyComponent* Comp = WB->GetWaterBodyComponent();
		if (!Comp)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_remove_exclusion_volume -> Water body '%s' has no component"), *Name));
			return false;
		}

		FString VolumeName = UTF8_TO_TCHAR(VolumeNameStr.c_str());
		AWaterBodyExclusionVolume* Volume = nullptr;
		for (TActorIterator<AWaterBodyExclusionVolume> It(World); It; ++It)
		{
			if ((*It)->GetActorLabel() == VolumeName || (*It)->GetName() == VolumeName || (*It)->GetActorNameOrLabel() == VolumeName)
			{
				Volume = *It;
				break;
			}
		}

		if (!Volume)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_remove_exclusion_volume -> Could not find exclusion volume '%s'"), *VolumeName));
			return false;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Remove Exclusion Volume")));
		Comp->Modify();
		Comp->RemoveExclusionVolume(Volume);

		FOnWaterBodyChangedParams ChangedParams;
		ChangedParams.bShapeOrPositionChanged = true;
		ChangedParams.bUserTriggered = true;
		Comp->UpdateAll(ChangedParams);

		Session.Log(FString::Printf(TEXT("[OK] water_remove_exclusion_volume -> Removed '%s' from '%s'"), *VolumeName, *Name));
		return true;
	});

	// ================================================================
	// water_list_exclusion_volumes(actor_name)
	// ================================================================
	Lua.set_function("water_list_exclusion_volumes", [&Session](const std::string& NameStr, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] water_list_exclusion_volumes -> No editor world"));
			return sol::lua_nil;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AWaterBody* WB = FindWaterBodyByName(World, Name);
		if (!WB)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_list_exclusion_volumes -> Could not find water body '%s'"), *Name));
			return sol::lua_nil;
		}

		UWaterBodyComponent* Comp = WB->GetWaterBodyComponent();
		if (!Comp)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] water_list_exclusion_volumes -> Water body '%s' has no component"), *Name));
			return sol::lua_nil;
		}

		TArray<AWaterBodyExclusionVolume*> Volumes = Comp->GetExclusionVolumes();
		sol::table Result = LuaView.create_table();
		int32 Idx = 1;
		for (AWaterBodyExclusionVolume* Vol : Volumes)
		{
			if (!Vol) continue;
			sol::table Entry = LuaView.create_table();
			Entry["name"] = TCHAR_TO_UTF8(*Vol->GetActorNameOrLabel());
			Entry["location"] = VectorToTable(LuaView, Vol->GetActorLocation());

			const char* ModeStr = "remove_from_exclusion";
			if (Vol->ExclusionMode == EWaterExclusionMode::AddWaterBodiesListToExclusion)
			{
				ModeStr = "add_to_exclusion";
			}
			Entry["mode"] = ModeStr;
			Entry["water_body_count"] = Vol->WaterBodies.Num();

			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] water_list_exclusion_volumes -> %d volumes on '%s'"), Idx - 1, *Name));
		return Result;
	});
}

REGISTER_LUA_BINDING(Water, WaterDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindWater(Lua, Session);
});

