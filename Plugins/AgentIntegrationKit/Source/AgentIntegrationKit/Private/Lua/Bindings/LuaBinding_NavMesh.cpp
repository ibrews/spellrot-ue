// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "ScopedTransaction.h"

#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavAreas/NavArea.h"
#include "NavAreas/NavArea_Null.h"
#include "NavAreas/NavArea_Default.h"
#include "NavAreas/NavArea_Obstacle.h"
#include "NavAreas/NavArea_LowHeight.h"
#include "NavModifierVolume.h"
#include "Navigation/NavLinkProxy.h"
#include "NavLinkCustomComponent.h"
#include "NavigationData.h"
#include "AI/NavigationSystemConfig.h"
#include "NavigationPath.h"
#include "NavMesh/NavMeshBoundsVolume.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

namespace
{

static UNavigationSystemV1* GetNavSys(UWorld* World = nullptr)
{
	if (!World) World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	return World ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr;
}

static ARecastNavMesh* GetRecastNavMesh(UNavigationSystemV1* NavSys)
{
	if (!NavSys) return nullptr;
	ANavigationData* NavData = NavSys->GetDefaultNavDataInstance();
	return Cast<ARecastNavMesh>(NavData);
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

} // anonymous namespace

// ============================================================================
// DOCUMENTATION
// ============================================================================

static TArray<FLuaFunctionDoc> NavMeshDocs = {
	{ TEXT("navmesh_build()"), TEXT("Full navigation mesh build (editor build). Returns bool on success."), TEXT("bool") },
	{ TEXT("navmesh_rebuild_all()"), TEXT("Full rebuild of all navigation data actors. Recreates generators and rebuilds from scratch. Returns bool on success."), TEXT("bool") },
	{ TEXT("navmesh_cancel_build()"), TEXT("Cancel all currently running navigation builds. Returns bool."), TEXT("bool") },
	{ TEXT("navmesh_get_config()"), TEXT("Returns current Recast nav mesh configuration as a table with all key generation properties."), TEXT("table") },
	{ TEXT("navmesh_set_config(params)"), TEXT("Set nav mesh configuration properties from a table. Keys: cell_size, cell_height, agent_radius, agent_height, agent_max_slope, agent_max_step_height, tile_size, min_region_area, merge_region_size, max_simplification_error, simplification_elevation_ratio, fixed_tile_pool_size, tile_pool_size, max_tile_generation_jobs, tile_number_hard_limit. Returns bool."), TEXT("bool") },
	{ TEXT("navmesh_list_areas()"), TEXT("List all registered nav area classes. Returns array of {name, class_path, default_cost, fixed_entering_cost, draw_color, is_low_height}."), TEXT("table[]") },
	{ TEXT("navmesh_get_area_cost(area_class_name)"), TEXT("Get costs for a specific nav area by class name. Returns {default_cost, fixed_entering_cost}."), TEXT("table") },
	{ TEXT("navmesh_set_area_cost(area_class_name, params)"), TEXT("Set costs for a nav area. Params: {default_cost=1.0, fixed_entering_cost=0.0}. Wrapped in undo transaction."), TEXT("true/nil") },
	{ TEXT("navmesh_find_path(start, end, agent_radius?)"), TEXT("Find a navigation path between two points. start/end are {x,y,z} tables. If agent_radius provided, uses agent-specific nav data. Returns {found, path_points, path_length, is_partial}."), TEXT("table") },
	{ TEXT("navmesh_project_point(point, extent?)"), TEXT("Project a point onto the nav mesh. Returns {x,y,z} or nil if not on nav mesh. extent defaults to {50,50,250}."), TEXT("table|nil") },
	{ TEXT("navmesh_random_point(origin?, radius?)"), TEXT("Get a random navigable point. If origin provided, finds reachable point within radius (default 10000). Without origin, finds any navigable point. Returns {x,y,z}."), TEXT("table") },
	{ TEXT("navmesh_test_path(start, end)"), TEXT("Quick boolean test if a path exists between two points."), TEXT("bool") },
	{ TEXT("navmesh_is_point_navigable(point, extent?)"), TEXT("Check if a point is on the nav mesh. extent defaults to {50,50,250}."), TEXT("bool") },
	{ TEXT("navmesh_raycast(start, end)"), TEXT("Navigation raycast (line-of-sight test on nav mesh). Returns {hit, hit_location}. hit=true if the ray is obstructed."), TEXT("table") },
	{ TEXT("navmesh_get_path_cost(start, end)"), TEXT("Get the path cost and length between two points. Returns {cost, length} or nil on failure. Potentially expensive."), TEXT("table|nil") },
	{ TEXT("navmesh_info()"), TEXT("Get overall nav mesh statistics: nav_data_count, bounds_count, total_tiles, is_building, has_nav_data, agents list."), TEXT("table") },
	{ TEXT("navmesh_list_bounds()"), TEXT("List all nav mesh bounds volumes in the world. Returns array of {name, location, extent}."), TEXT("table[]") },
	{ TEXT("navmesh_list_modifiers()"), TEXT("List all nav modifier volumes. Returns array of {name, location, area_class, is_visible}."), TEXT("table[]") },
	{ TEXT("navmesh_list_links()"), TEXT("List all nav link proxies. Returns array of {name, location, left_point, right_point, is_smart_link, is_visible, is_smart_link_enabled}."), TEXT("table[]") },
};

// ============================================================================
// BINDING
// ============================================================================

REGISTER_LUA_BINDING(NavMesh, NavMeshDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	// ================================================================
	// navmesh_build()
	// ================================================================
	Lua.set_function("navmesh_build", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UNavigationSystemV1* NavSys = GetNavSys();
		if (!NavSys)
		{
			Session.Log(TEXT("[FAIL] navmesh_build() -> no navigation system found"));
			return sol::make_object(LuaView, false);
		}

		NavSys->Build();

		Session.Log(TEXT("[OK] navmesh_build() -> navigation build initiated"));
		return sol::make_object(LuaView, true);
	});

	// ================================================================
	// navmesh_rebuild_all()
	// ================================================================
	Lua.set_function("navmesh_rebuild_all", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		UNavigationSystemV1* NavSys = GetNavSys(World);
		if (!NavSys)
		{
			Session.Log(TEXT("[FAIL] navmesh_rebuild_all() -> no navigation system found"));
			return sol::make_object(LuaView, false);
		}

		// RebuildAll() recreates the nav data generator and does a full rebuild from scratch
		for (TActorIterator<ANavigationData> It(World); It; ++It)
		{
			ANavigationData* NavData = *It;
			if (NavData)
			{
				NavData->RebuildAll();
			}
		}

		Session.Log(TEXT("[OK] navmesh_rebuild_all() -> full rebuild initiated on all nav data actors"));
		return sol::make_object(LuaView, true);
	});

	// ================================================================
	// navmesh_cancel_build()
	// ================================================================
	Lua.set_function("navmesh_cancel_build", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UNavigationSystemV1* NavSys = GetNavSys();
		if (!NavSys)
		{
			Session.Log(TEXT("[FAIL] navmesh_cancel_build() -> no navigation system found"));
			return sol::make_object(LuaView, false);
		}

		NavSys->CancelBuild();

		Session.Log(TEXT("[OK] navmesh_cancel_build() -> all running navigation builds cancelled"));
		return sol::make_object(LuaView, true);
	});

	// ================================================================
	// navmesh_get_config()
	// ================================================================
	Lua.set_function("navmesh_get_config", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UNavigationSystemV1* NavSys = GetNavSys();
		if (!NavSys)
		{
			Session.Log(TEXT("[FAIL] navmesh_get_config() -> no navigation system found"));
			return sol::lua_nil;
		}

		ARecastNavMesh* Recast = GetRecastNavMesh(NavSys);
		if (!Recast)
		{
			Session.Log(TEXT("[FAIL] navmesh_get_config() -> no Recast nav mesh found"));
			return sol::lua_nil;
		}

		sol::table Config = LuaView.create_table();
		Config["cell_size"] = Recast->GetCellSize(ENavigationDataResolution::Default);
		Config["cell_height"] = Recast->GetCellHeight(ENavigationDataResolution::Default);
		Config["agent_radius"] = Recast->AgentRadius;
		Config["agent_height"] = Recast->AgentHeight;
		Config["agent_max_slope"] = Recast->AgentMaxSlope;
		Config["agent_max_step_height"] = Recast->GetAgentMaxStepHeight(ENavigationDataResolution::Default);
		Config["tile_size"] = Recast->TileSizeUU;
		Config["min_region_area"] = Recast->MinRegionArea;
		Config["merge_region_size"] = Recast->MergeRegionSize;
		Config["max_simplification_error"] = Recast->MaxSimplificationError;
		Config["simplification_elevation_ratio"] = Recast->SimplificationElevationRatio;
		Config["fixed_tile_pool_size"] = static_cast<bool>(Recast->bFixedTilePoolSize);
		Config["tile_pool_size"] = Recast->TilePoolSize;
		Config["max_tile_generation_jobs"] = Recast->GetMaxSimultaneousTileGenerationJobsCount();
		Config["tile_number_hard_limit"] = Recast->GetTileNumberHardLimit();
		Config["sort_areas_by_cost"] = static_cast<bool>(Recast->bSortNavigationAreasByCost);
		Config["perform_voxel_filtering"] = static_cast<bool>(Recast->bPerformVoxelFiltering);
		Config["mark_low_height_areas"] = static_cast<bool>(Recast->bMarkLowHeightAreas);
		Config["filter_low_span_sequences"] = static_cast<bool>(Recast->bFilterLowSpanSequences);
		Config["filter_low_span_from_tile_cache"] = static_cast<bool>(Recast->bFilterLowSpanFromTileCache);
		Config["do_fully_async_nav_data_gathering"] = static_cast<bool>(Recast->bDoFullyAsyncNavDataGathering);

		Session.Log(TEXT("[OK] navmesh_get_config() -> returned configuration"));
		return Config;
	});

	// ================================================================
	// navmesh_set_config(params)
	// ================================================================
	Lua.set_function("navmesh_set_config", [&Session](sol::table Params, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UNavigationSystemV1* NavSys = GetNavSys();
		if (!NavSys)
		{
			Session.Log(TEXT("[FAIL] navmesh_set_config() -> no navigation system found"));
			return sol::make_object(LuaView, false);
		}

		ARecastNavMesh* Recast = GetRecastNavMesh(NavSys);
		if (!Recast)
		{
			Session.Log(TEXT("[FAIL] navmesh_set_config() -> no Recast nav mesh found"));
			return sol::make_object(LuaView, false);
		}

		const FScopedTransaction Transaction(FText::FromString(TEXT("NavMesh: Set Config")));
		Recast->Modify();

		int32 ChangedCount = 0;
		FProperty* LastChangedProp = nullptr;

		sol::optional<double> CellSize = Params.get<sol::optional<double>>("cell_size");
		if (CellSize.has_value()) { Recast->SetCellSize(ENavigationDataResolution::Default, FMath::Max(0.0f, static_cast<float>(CellSize.value()))); ++ChangedCount; }

		sol::optional<double> CellHeight = Params.get<sol::optional<double>>("cell_height");
		if (CellHeight.has_value()) { Recast->SetCellHeight(ENavigationDataResolution::Default, FMath::Max(0.0f, static_cast<float>(CellHeight.value()))); ++ChangedCount; }

		sol::optional<double> AgentRadius = Params.get<sol::optional<double>>("agent_radius");
		if (AgentRadius.has_value())
		{
			Recast->AgentRadius = FMath::Clamp(static_cast<float>(AgentRadius.value()), 0.0f, 100000.0f);
			LastChangedProp = Recast->GetClass()->FindPropertyByName(TEXT("AgentRadius"));
			++ChangedCount;
		}

		sol::optional<double> AgentHeight = Params.get<sol::optional<double>>("agent_height");
		if (AgentHeight.has_value())
		{
			Recast->AgentHeight = FMath::Clamp(static_cast<float>(AgentHeight.value()), 0.0f, 100000.0f);
			LastChangedProp = Recast->GetClass()->FindPropertyByName(TEXT("AgentHeight"));
			++ChangedCount;
		}

		sol::optional<double> AgentMaxSlope = Params.get<sol::optional<double>>("agent_max_slope");
		if (AgentMaxSlope.has_value())
		{
			Recast->AgentMaxSlope = FMath::Clamp(static_cast<float>(AgentMaxSlope.value()), 0.0f, 89.0f);
			LastChangedProp = Recast->GetClass()->FindPropertyByName(TEXT("AgentMaxSlope"));
			++ChangedCount;
		}

		sol::optional<double> AgentMaxStepHeight = Params.get<sol::optional<double>>("agent_max_step_height");
		if (AgentMaxStepHeight.has_value()) { Recast->SetAgentMaxStepHeight(ENavigationDataResolution::Default, FMath::Max(0.0f, static_cast<float>(AgentMaxStepHeight.value()))); ++ChangedCount; }

		sol::optional<double> TileSize = Params.get<sol::optional<double>>("tile_size");
		if (TileSize.has_value())
		{
			Recast->TileSizeUU = FMath::Max(300.0f, static_cast<float>(TileSize.value()));
			LastChangedProp = Recast->GetClass()->FindPropertyByName(TEXT("TileSizeUU"));
			++ChangedCount;
		}

		sol::optional<double> MinRegionArea = Params.get<sol::optional<double>>("min_region_area");
		if (MinRegionArea.has_value())
		{
			Recast->MinRegionArea = FMath::Max(0.0f, static_cast<float>(MinRegionArea.value()));
			LastChangedProp = Recast->GetClass()->FindPropertyByName(TEXT("MinRegionArea"));
			++ChangedCount;
		}

		sol::optional<double> MergeRegionSize = Params.get<sol::optional<double>>("merge_region_size");
		if (MergeRegionSize.has_value())
		{
			Recast->MergeRegionSize = FMath::Max(0.0f, static_cast<float>(MergeRegionSize.value()));
			LastChangedProp = Recast->GetClass()->FindPropertyByName(TEXT("MergeRegionSize"));
			++ChangedCount;
		}

		sol::optional<double> MaxSimplError = Params.get<sol::optional<double>>("max_simplification_error");
		if (MaxSimplError.has_value())
		{
			Recast->MaxSimplificationError = FMath::Max(0.0f, static_cast<float>(MaxSimplError.value()));
			LastChangedProp = Recast->GetClass()->FindPropertyByName(TEXT("MaxSimplificationError"));
			++ChangedCount;
		}

		sol::optional<double> SimplElevRatio = Params.get<sol::optional<double>>("simplification_elevation_ratio");
		if (SimplElevRatio.has_value())
		{
			Recast->SimplificationElevationRatio = FMath::Max(0.0f, static_cast<float>(SimplElevRatio.value()));
			LastChangedProp = Recast->GetClass()->FindPropertyByName(TEXT("SimplificationElevationRatio"));
			++ChangedCount;
		}

		sol::optional<bool> FixedPool = Params.get<sol::optional<bool>>("fixed_tile_pool_size");
		if (FixedPool.has_value())
		{
			Recast->bFixedTilePoolSize = FixedPool.value();
			LastChangedProp = Recast->GetClass()->FindPropertyByName(TEXT("bFixedTilePoolSize"));
			++ChangedCount;
		}

		sol::optional<int> TilePoolSize = Params.get<sol::optional<int>>("tile_pool_size");
		if (TilePoolSize.has_value())
		{
			Recast->TilePoolSize = TilePoolSize.value();
			LastChangedProp = Recast->GetClass()->FindPropertyByName(TEXT("TilePoolSize"));
			++ChangedCount;
		}

		sol::optional<int> MaxTileGenJobs = Params.get<sol::optional<int>>("max_tile_generation_jobs");
		if (MaxTileGenJobs.has_value())
		{
			Recast->SetMaxSimultaneousTileGenerationJobsCount(FMath::Max(0, MaxTileGenJobs.value()));
			++ChangedCount;
		}

		sol::optional<int> TileHardLimit = Params.get<sol::optional<int>>("tile_number_hard_limit");
		if (TileHardLimit.has_value())
		{
			Recast->TileNumberHardLimit = FMath::Max(1, TileHardLimit.value());
			LastChangedProp = Recast->GetClass()->FindPropertyByName(TEXT("TileNumberHardLimit"));
			++ChangedCount;
		}

		if (ChangedCount > 0)
		{
			FPropertyChangedEvent PCE(LastChangedProp, EPropertyChangeType::ValueSet);
			Recast->PostEditChangeProperty(PCE);
			Recast->MarkPackageDirty();
		}

		Session.Log(FString::Printf(TEXT("[OK] navmesh_set_config() -> %d properties changed"), ChangedCount));
		return sol::make_object(LuaView, true);
	});

	// ================================================================
	// navmesh_list_areas()
	// ================================================================
	Lua.set_function("navmesh_list_areas", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;

		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Cls = *It;
			if (!Cls->IsChildOf(UNavArea::StaticClass())) continue;
			if (Cls->HasAnyClassFlags(CLASS_Abstract)) continue;

			UNavArea* AreaCDO = Cls->GetDefaultObject<UNavArea>();
			if (!AreaCDO) continue;

			sol::table Entry = LuaView.create_table();
			Entry["name"] = TCHAR_TO_UTF8(*Cls->GetName());
			Entry["class_path"] = TCHAR_TO_UTF8(*Cls->GetPathName());
			Entry["default_cost"] = AreaCDO->DefaultCost;
			Entry["fixed_entering_cost"] = AreaCDO->GetFixedAreaEnteringCost();

			// Draw color
			FColor DrawColor = AreaCDO->DrawColor;
			sol::table ColorTable = LuaView.create_table();
			ColorTable["r"] = DrawColor.R;
			ColorTable["g"] = DrawColor.G;
			ColorTable["b"] = DrawColor.B;
			ColorTable["a"] = DrawColor.A;
			Entry["draw_color"] = ColorTable;

			Entry["is_low_height"] = Cls->IsChildOf(UNavArea_LowHeight::StaticClass());

			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] navmesh_list_areas() -> %d areas"), Idx - 1));
		return Result;
	});

	// ================================================================
	// navmesh_get_area_cost(area_class_name)
	// ================================================================
	Lua.set_function("navmesh_get_area_cost", [&Session](const std::string& AreaClassName, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString FAreaClassName = UTF8_TO_TCHAR(AreaClassName.c_str());

		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Cls = *It;
			if (!Cls->IsChildOf(UNavArea::StaticClass())) continue;
			if (Cls->HasAnyClassFlags(CLASS_Abstract)) continue;
			if (!Cls->GetName().Equals(FAreaClassName, ESearchCase::IgnoreCase)) continue;

			UNavArea* AreaCDO = Cls->GetDefaultObject<UNavArea>();
			if (!AreaCDO) continue;

			sol::table Result = LuaView.create_table();
			Result["default_cost"] = AreaCDO->DefaultCost;
			Result["fixed_entering_cost"] = AreaCDO->GetFixedAreaEnteringCost();

			Session.Log(FString::Printf(TEXT("[OK] navmesh_get_area_cost(\"%s\") -> default_cost=%.2f, fixed_entering_cost=%.2f"),
				*FAreaClassName, AreaCDO->DefaultCost, AreaCDO->GetFixedAreaEnteringCost()));
			return Result;
		}

		Session.Log(FString::Printf(TEXT("[FAIL] navmesh_get_area_cost(\"%s\") -> area class not found"), *FAreaClassName));
		return sol::lua_nil;
	});

	// ================================================================
	// navmesh_set_area_cost(area_class_name, params)
	// ================================================================
	Lua.set_function("navmesh_set_area_cost", [&Session](const std::string& AreaClassName, sol::table Params, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FAreaClassName = UTF8_TO_TCHAR(AreaClassName.c_str());

		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (!It->IsChildOf(UNavArea::StaticClass())) continue;
			if (It->GetName().Equals(FAreaClassName, ESearchCase::IgnoreCase) ||
				It->GetName().Equals(TEXT("NavArea_") + FAreaClassName, ESearchCase::IgnoreCase))
			{
				UNavArea* AreaCDO = It->GetDefaultObject<UNavArea>();
				if (!AreaCDO) continue;

				const FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("NavMesh: Set Area Cost (%s)"), *It->GetName())));
				AreaCDO->Modify();

				bool bChanged = false;

				sol::optional<double> DefaultCost = Params.get<sol::optional<double>>("default_cost");
				if (DefaultCost.has_value())
				{
					FFloatProperty* CostProp = CastField<FFloatProperty>(AreaCDO->GetClass()->FindPropertyByName(TEXT("DefaultCost")));
					if (CostProp)
					{
						CostProp->SetPropertyValue_InContainer(AreaCDO, FMath::Max(0.0f, static_cast<float>(DefaultCost.value())));
						bChanged = true;
					}
				}

				sol::optional<double> EnteringCost = Params.get<sol::optional<double>>("fixed_entering_cost");
				if (EnteringCost.has_value())
				{
					FFloatProperty* EnterProp = CastField<FFloatProperty>(AreaCDO->GetClass()->FindPropertyByName(TEXT("FixedAreaEnteringCost")));
					if (EnterProp)
					{
						EnterProp->SetPropertyValue_InContainer(AreaCDO, FMath::Max(0.0f, static_cast<float>(EnteringCost.value())));
						bChanged = true;
					}
				}

				if (bChanged)
				{
					FPropertyChangedEvent PCE(AreaCDO->GetClass()->FindPropertyByName(TEXT("DefaultCost")), EPropertyChangeType::ValueSet);
					AreaCDO->PostEditChangeProperty(PCE);
				}

				float ReportDefaultCost = AreaCDO->DefaultCost;
				float ReportEnteringCost = AreaCDO->GetFixedAreaEnteringCost();

				Session.Log(FString::Printf(TEXT("[OK] navmesh_set_area_cost(\"%s\") -> default_cost=%.2f, fixed_entering_cost=%.2f"),
					*It->GetName(), ReportDefaultCost, ReportEnteringCost));
				return sol::make_object(Lua, true);
			}
		}
		Session.Log(FString::Printf(TEXT("[FAIL] navmesh_set_area_cost(\"%s\") -> area class not found"), *FAreaClassName));
		return sol::lua_nil;
	});

	// ================================================================
	// navmesh_find_path(start, end, agent_radius?)
	// ================================================================
	Lua.set_function("navmesh_find_path", [&Session](sol::table Start, sol::table End,
		sol::optional<double> AgentRadiusOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UNavigationSystemV1* NavSys = GetNavSys();
		if (!NavSys)
		{
			Session.Log(TEXT("[FAIL] navmesh_find_path() -> no navigation system found"));
			return sol::lua_nil;
		}

		FVector StartVec = TableToVector(Start);
		FVector EndVec = TableToVector(End);

		ANavigationData* NavData = NavSys->GetDefaultNavDataInstance();
		if (!NavData)
		{
			Session.Log(TEXT("[FAIL] navmesh_find_path() -> no nav data instance"));
			return sol::lua_nil;
		}

		FPathFindingQuery Query(nullptr, *NavData, StartVec, EndVec);
		FPathFindingResult PathResult;

		if (AgentRadiusOpt.has_value())
		{
			// Use the overload that takes agent properties to find agent-specific nav data
			FNavAgentProperties AgentProps;
			AgentProps.AgentRadius = static_cast<float>(AgentRadiusOpt.value());
			PathResult = NavSys->FindPathSync(AgentProps, Query);
		}
		else
		{
			PathResult = NavSys->FindPathSync(Query);
		}

		sol::table Result = LuaView.create_table();
		Result["found"] = PathResult.IsSuccessful();
		Result["is_partial"] = PathResult.IsPartial();

		sol::table PathPoints = LuaView.create_table();
		double TotalLength = 0.0;

		if (PathResult.IsSuccessful() && PathResult.Path.IsValid())
		{
			const TArray<FNavPathPoint>& Points = PathResult.Path->GetPathPoints();
			for (int32 i = 0; i < Points.Num(); i++)
			{
				PathPoints[i + 1] = VectorToTable(LuaView, Points[i].Location);
				if (i > 0)
				{
					TotalLength += FVector::Dist(Points[i - 1].Location, Points[i].Location);
				}
			}
		}

		Result["path_points"] = PathPoints;
		Result["path_length"] = TotalLength;

		if (PathResult.IsSuccessful())
		{
			Session.Log(FString::Printf(TEXT("[OK] navmesh_find_path() -> found=%s, points=%d, length=%.1f, partial=%s"),
				PathResult.IsSuccessful() ? TEXT("true") : TEXT("false"),
				PathResult.Path.IsValid() ? PathResult.Path->GetPathPoints().Num() : 0,
				TotalLength,
				PathResult.IsPartial() ? TEXT("true") : TEXT("false")));
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] navmesh_find_path() -> no path found from (%.0f,%.0f,%.0f) to (%.0f,%.0f,%.0f)"),
				StartVec.X, StartVec.Y, StartVec.Z, EndVec.X, EndVec.Y, EndVec.Z));
		}

		return Result;
	});

	// ================================================================
	// navmesh_project_point(point, extent?)
	// ================================================================
	Lua.set_function("navmesh_project_point", [&Session](sol::table Point,
		sol::optional<sol::table> ExtentOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UNavigationSystemV1* NavSys = GetNavSys();
		if (!NavSys)
		{
			Session.Log(TEXT("[FAIL] navmesh_project_point() -> no navigation system found"));
			return sol::lua_nil;
		}

		FVector PointVec = TableToVector(Point);
		FVector Extent(50.f, 50.f, 250.f);
		if (ExtentOpt.has_value())
		{
			Extent = TableToVector(ExtentOpt.value());
		}

		FNavLocation NavLocation;
		bool bSuccess = NavSys->ProjectPointToNavigation(PointVec, NavLocation, Extent);

		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] navmesh_project_point() -> (%.1f, %.1f, %.1f)"),
				NavLocation.Location.X, NavLocation.Location.Y, NavLocation.Location.Z));
			return VectorToTable(LuaView, NavLocation.Location);
		}

		Session.Log(FString::Printf(TEXT("[FAIL] navmesh_project_point() -> point (%.0f,%.0f,%.0f) not on nav mesh"),
			PointVec.X, PointVec.Y, PointVec.Z));
		return sol::lua_nil;
	});

	// ================================================================
	// navmesh_random_point(origin?, radius?)
	// ================================================================
	Lua.set_function("navmesh_random_point", [&Session](sol::optional<sol::table> OriginOpt,
		sol::optional<double> RadiusOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UNavigationSystemV1* NavSys = GetNavSys();
		if (!NavSys)
		{
			Session.Log(TEXT("[FAIL] navmesh_random_point() -> no navigation system found"));
			return sol::lua_nil;
		}

		FNavLocation ResultLocation;
		bool bSuccess = false;

		if (OriginOpt.has_value())
		{
			FVector Origin = TableToVector(OriginOpt.value());
			float Radius = RadiusOpt.has_value() ? static_cast<float>(RadiusOpt.value()) : 10000.f;
			bSuccess = NavSys->GetRandomReachablePointInRadius(Origin, Radius, ResultLocation);
		}
		else
		{
			// No origin — get any random navigable point
			bSuccess = NavSys->GetRandomPoint(ResultLocation);
		}

		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] navmesh_random_point() -> (%.1f, %.1f, %.1f)"),
				ResultLocation.Location.X, ResultLocation.Location.Y, ResultLocation.Location.Z));
			return VectorToTable(LuaView, ResultLocation.Location);
		}

		Session.Log(TEXT("[FAIL] navmesh_random_point() -> could not find a navigable point"));
		return sol::lua_nil;
	});

	// ================================================================
	// navmesh_test_path(start, end)
	// ================================================================
	Lua.set_function("navmesh_test_path", [&Session](sol::table Start, sol::table End, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UNavigationSystemV1* NavSys = GetNavSys();
		if (!NavSys)
		{
			Session.Log(TEXT("[FAIL] navmesh_test_path() -> no navigation system found"));
			return sol::make_object(LuaView, false);
		}

		FVector StartVec = TableToVector(Start);
		FVector EndVec = TableToVector(End);

		ANavigationData* NavData = NavSys->GetDefaultNavDataInstance();
		if (!NavData)
		{
			Session.Log(TEXT("[FAIL] navmesh_test_path() -> no nav data instance"));
			return sol::make_object(LuaView, false);
		}

		FPathFindingQuery Query(nullptr, *NavData, StartVec, EndVec);
		bool bPathExists = NavSys->TestPathSync(Query);

		Session.Log(FString::Printf(TEXT("[OK] navmesh_test_path() -> %s"),
			bPathExists ? TEXT("path exists") : TEXT("no path")));
		return sol::make_object(LuaView, bPathExists);
	});

	// ================================================================
	// navmesh_is_point_navigable(point, extent?)
	// ================================================================
	Lua.set_function("navmesh_is_point_navigable", [&Session](sol::table Point,
		sol::optional<sol::table> ExtentOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UNavigationSystemV1* NavSys = GetNavSys();
		if (!NavSys)
		{
			Session.Log(TEXT("[FAIL] navmesh_is_point_navigable() -> no navigation system found"));
			return sol::make_object(LuaView, false);
		}

		FVector PointVec = TableToVector(Point);
		FVector Extent(50.f, 50.f, 250.f);
		if (ExtentOpt.has_value())
		{
			Extent = TableToVector(ExtentOpt.value());
		}

		FNavLocation NavLocation;
		bool bNavigable = NavSys->ProjectPointToNavigation(PointVec, NavLocation, Extent);

		Session.Log(FString::Printf(TEXT("[OK] navmesh_is_point_navigable(%.0f,%.0f,%.0f) -> %s"),
			PointVec.X, PointVec.Y, PointVec.Z,
			bNavigable ? TEXT("true") : TEXT("false")));
		return sol::make_object(LuaView, bNavigable);
	});

	// ================================================================
	// navmesh_raycast(start, end)
	// ================================================================
	Lua.set_function("navmesh_raycast", [&Session](sol::table Start, sol::table End, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] navmesh_raycast() -> no editor world"));
			return sol::lua_nil;
		}

		FVector StartVec = TableToVector(Start);
		FVector EndVec = TableToVector(End);

		FVector HitLocation;
		bool bHit = UNavigationSystemV1::NavigationRaycast(World, StartVec, EndVec, HitLocation);

		sol::table Result = LuaView.create_table();
		Result["hit"] = bHit;
		Result["hit_location"] = VectorToTable(LuaView, HitLocation);

		Session.Log(FString::Printf(TEXT("[OK] navmesh_raycast() -> hit=%s, location=(%.1f, %.1f, %.1f)"),
			bHit ? TEXT("true") : TEXT("false"),
			HitLocation.X, HitLocation.Y, HitLocation.Z));
		return Result;
	});

	// ================================================================
	// navmesh_get_path_cost(start, end)
	// ================================================================
	Lua.set_function("navmesh_get_path_cost", [&Session](sol::table Start, sol::table End, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UNavigationSystemV1* NavSys = GetNavSys();
		if (!NavSys)
		{
			Session.Log(TEXT("[FAIL] navmesh_get_path_cost() -> no navigation system found"));
			return sol::lua_nil;
		}

		FVector StartVec = TableToVector(Start);
		FVector EndVec = TableToVector(End);

		FVector::FReal PathCost = 0.0;
		FVector::FReal PathLength = 0.0;
		ENavigationQueryResult::Type QueryResult = NavSys->GetPathLengthAndCost(StartVec, EndVec, PathLength, PathCost);

		if (QueryResult != ENavigationQueryResult::Success)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] navmesh_get_path_cost() -> query failed (result=%d)"), static_cast<int>(QueryResult)));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		Result["cost"] = static_cast<double>(PathCost);
		Result["length"] = static_cast<double>(PathLength);

		Session.Log(FString::Printf(TEXT("[OK] navmesh_get_path_cost() -> cost=%.2f, length=%.2f"),
			static_cast<double>(PathCost), static_cast<double>(PathLength)));
		return Result;
	});

	// ================================================================
	// navmesh_info()
	// ================================================================
	Lua.set_function("navmesh_info", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		UNavigationSystemV1* NavSys = GetNavSys(World);
		if (!NavSys)
		{
			Session.Log(TEXT("[FAIL] navmesh_info() -> no navigation system found"));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();

		// Count nav data actors
		int32 NavDataCount = 0;
		for (TActorIterator<ANavigationData> It(World); It; ++It)
		{
			++NavDataCount;
		}
		Result["nav_data_count"] = NavDataCount;

		// Count nav mesh bounds volumes
		int32 BoundsCount = 0;
		for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It)
		{
			++BoundsCount;
		}
		Result["bounds_count"] = BoundsCount;

		// Building status
		Result["is_building"] = NavSys->IsNavigationBuildInProgress();
		Result["has_nav_data"] = (NavSys->GetDefaultNavDataInstance() != nullptr);

		// Tile count from Recast
		ARecastNavMesh* Recast = GetRecastNavMesh(NavSys);
		if (Recast)
		{
			Result["total_tiles"] = Recast->GetNavMeshTilesCount();
		}
		else
		{
			Result["total_tiles"] = 0;
		}

		// Supported agents
		const TArray<FNavDataConfig>& SupportedAgents = NavSys->GetSupportedAgents();
		sol::table AgentsTable = LuaView.create_table();
		for (int32 i = 0; i < SupportedAgents.Num(); i++)
		{
			const FNavDataConfig& AgentConfig = SupportedAgents[i];
			sol::table AgentEntry = LuaView.create_table();
			AgentEntry["name"] = TCHAR_TO_UTF8(*AgentConfig.Name.ToString());
			AgentEntry["radius"] = AgentConfig.AgentRadius;
			AgentEntry["height"] = AgentConfig.AgentHeight;
			AgentsTable[i + 1] = AgentEntry;
		}
		Result["agents"] = AgentsTable;

		Session.Log(FString::Printf(TEXT("[OK] navmesh_info() -> %d nav data, %d bounds, building=%s"),
			NavDataCount, BoundsCount,
			NavSys->IsNavigationBuildInProgress() ? TEXT("true") : TEXT("false")));
		return Result;
	});

	// ================================================================
	// navmesh_list_bounds()
	// ================================================================
	Lua.set_function("navmesh_list_bounds", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] navmesh_list_bounds() -> no editor world"));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;

		for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It)
		{
			ANavMeshBoundsVolume* Volume = *It;
			if (!Volume) continue;

			sol::table Entry = LuaView.create_table();
			Entry["name"] = TCHAR_TO_UTF8(*Volume->GetActorLabel());

			FVector Origin, BoxExtent;
			Volume->GetActorBounds(false, Origin, BoxExtent);

			Entry["location"] = VectorToTable(LuaView, Origin);
			Entry["extent"] = VectorToTable(LuaView, BoxExtent);

			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] navmesh_list_bounds() -> %d volumes"), Idx - 1));
		return Result;
	});

	// ================================================================
	// navmesh_list_modifiers()
	// ================================================================
	Lua.set_function("navmesh_list_modifiers", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] navmesh_list_modifiers() -> no editor world"));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;

		for (TActorIterator<ANavModifierVolume> It(World); It; ++It)
		{
			ANavModifierVolume* ModVol = *It;
			if (!ModVol) continue;

			sol::table Entry = LuaView.create_table();
			Entry["name"] = TCHAR_TO_UTF8(*ModVol->GetActorLabel());
			Entry["location"] = VectorToTable(LuaView, ModVol->GetActorLocation());

			TSubclassOf<UNavArea> AreaClass = ModVol->GetAreaClass();
			if (AreaClass)
			{
				Entry["area_class"] = TCHAR_TO_UTF8(*AreaClass->GetName());
			}
			else
			{
				Entry["area_class"] = "None";
			}

			Entry["is_visible"] = !ModVol->IsHidden();

			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] navmesh_list_modifiers() -> %d modifier volumes"), Idx - 1));
		return Result;
	});

	// ================================================================
	// navmesh_list_links()
	// ================================================================
	Lua.set_function("navmesh_list_links", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] navmesh_list_links() -> no editor world"));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;

		for (TActorIterator<ANavLinkProxy> It(World); It; ++It)
		{
			ANavLinkProxy* LinkProxy = *It;
			if (!LinkProxy) continue;

			sol::table Entry = LuaView.create_table();
			Entry["name"] = TCHAR_TO_UTF8(*LinkProxy->GetActorLabel());
			Entry["location"] = VectorToTable(LuaView, LinkProxy->GetActorLocation());

			// Get simple link points if available
			const TArray<FNavigationLink>& SimpleLinks = LinkProxy->PointLinks;
			if (SimpleLinks.Num() > 0)
			{
				const FNavigationLink& FirstLink = SimpleLinks[0];
				Entry["left_point"] = VectorToTable(LuaView, FirstLink.Left);
				Entry["right_point"] = VectorToTable(LuaView, FirstLink.Right);
			}
			else
			{
				Entry["left_point"] = VectorToTable(LuaView, FVector::ZeroVector);
				Entry["right_point"] = VectorToTable(LuaView, FVector::ZeroVector);
			}

			// Check for smart link component
			UNavLinkCustomComponent* SmartLink = LinkProxy->GetSmartLinkComp();
			Entry["is_smart_link"] = (SmartLink != nullptr);
			Entry["is_visible"] = !LinkProxy->IsHidden();
			Entry["is_smart_link_enabled"] = LinkProxy->IsSmartLinkEnabled();

			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] navmesh_list_links() -> %d link proxies"), Idx - 1));
		return Result;
	});
});
