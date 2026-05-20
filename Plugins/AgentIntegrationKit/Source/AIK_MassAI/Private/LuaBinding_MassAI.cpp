// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"

#include "ZoneGraphSubsystem.h"
#include "ZoneShapeComponent.h"
#include "ZoneShapeActor.h"
#include "ZoneGraphSettings.h"
#include "ZoneGraphTypes.h"
#include "ZoneGraphData.h"
#include "ZoneGraphBuilder.h"
#include "MassEntityConfigAsset.h"
#include "MassEntityTraitBase.h"
#include "MassSpawner.h"
#include "MassSpawnerSubsystem.h"
#include "MassSimulationSubsystem.h"
#include "MassCrowdSubsystem.h"
#include "MassCrowdTypes.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorActorSubsystem.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS (in anonymous namespace to avoid Unity Build ambiguity)
// ============================================================================

namespace
{

UWorld* MassAI_GetEditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

FVector MassAI_TableToVector(const sol::table& T)
{
	float X = T["x"].valid() ? T["x"].get<float>() : (T[1].valid() ? T[1].get<float>() : 0.f);
	float Y = T["y"].valid() ? T["y"].get<float>() : (T[2].valid() ? T[2].get<float>() : 0.f);
	float Z = T["z"].valid() ? T["z"].get<float>() : (T[3].valid() ? T[3].get<float>() : 0.f);
	return FVector(X, Y, Z);
}

sol::table MassAI_VectorToTable(sol::state_view& Lua, const FVector& V)
{
	sol::table T = Lua.create_table();
	T["x"] = V.X;
	T["y"] = V.Y;
	T["z"] = V.Z;
	return T;
}

AZoneShape* FindZoneShapeByName(UWorld* World, const FString& NameOrLabel)
{
	if (!World) return nullptr;
	for (TActorIterator<AZoneShape> It(World); It; ++It)
	{
		AZoneShape* ZS = *It;
		if (ZS->GetActorLabel() == NameOrLabel || ZS->GetName() == NameOrLabel || ZS->GetActorNameOrLabel() == NameOrLabel)
		{
			return ZS;
		}
	}
	return nullptr;
}

AMassSpawner* FindMassSpawnerByName(UWorld* World, const FString& NameOrLabel)
{
	if (!World) return nullptr;
	for (TActorIterator<AMassSpawner> It(World); It; ++It)
	{
		AMassSpawner* Spawner = *It;
		if (Spawner->GetActorLabel() == NameOrLabel || Spawner->GetName() == NameOrLabel || Spawner->GetActorNameOrLabel() == NameOrLabel)
		{
			return Spawner;
		}
	}
	return nullptr;
}

const char* ShapeTypeToString(FZoneShapeType Type)
{
	switch (Type)
	{
	case FZoneShapeType::Spline: return "spline";
	case FZoneShapeType::Polygon: return "polygon";
	default: return "unknown";
	}
}

const char* LaneDirectionToString(EZoneLaneDirection Dir)
{
	switch (Dir)
	{
	case EZoneLaneDirection::Forward: return "forward";
	case EZoneLaneDirection::Backward: return "backward";
	case EZoneLaneDirection::None: return "none";
	default: return "unknown";
	}
}

EZoneLaneDirection StringToLaneDirection(const std::string& Str)
{
	FString S = UTF8_TO_TCHAR(Str.c_str());
	if (S.Equals(TEXT("forward"), ESearchCase::IgnoreCase)) return EZoneLaneDirection::Forward;
	if (S.Equals(TEXT("backward"), ESearchCase::IgnoreCase)) return EZoneLaneDirection::Backward;
	if (S.Equals(TEXT("none"), ESearchCase::IgnoreCase)) return EZoneLaneDirection::None;
	return EZoneLaneDirection::Forward;
}

const char* CrowdLaneStateToString(ECrowdLaneState State)
{
	switch (State)
	{
	case ECrowdLaneState::Opened: return "opened";
	case ECrowdLaneState::Closed: return "closed";
	default: return "unknown";
	}
}

} // anonymous namespace

// ============================================================================
// DOCS
// ============================================================================

static TArray<FLuaFunctionDoc> MassAIDocs = {
	// ZoneGraph shapes
	{ TEXT("zonegraph_list_shapes()"), TEXT("List all ZoneShape actors in the editor world"), TEXT("table[]") },
	{ TEXT("zonegraph_get_shape(name_or_label)"), TEXT("Get detailed info for a specific zone shape (points, lane profile, tags)"), TEXT("table or nil") },
	{ TEXT("zonegraph_spawn_shape(params)"), TEXT("Spawn a zone shape actor. params: {location, shape_type?, label?, points[], lane_profile?, tags[], reverse_profile?}"), TEXT("string or nil") },
	{ TEXT("zonegraph_remove_shape(name_or_label)"), TEXT("Remove a zone shape actor from the level"), TEXT("bool") },
	{ TEXT("zonegraph_set_shape_points(name_or_label, points)"), TEXT("Replace all points on a zone shape. points: [{x,y,z, type?, tangent_length?, inner_turn_radius?}]"), TEXT("bool") },
	{ TEXT("zonegraph_set_shape_tags(name_or_label, tags)"), TEXT("Set zone tags on a shape by tag names"), TEXT("bool") },
	// ZoneGraph settings & tags
	{ TEXT("zonegraph_list_tags()"), TEXT("List all defined zone graph tags"), TEXT("table[]") },
	{ TEXT("zonegraph_list_lane_profiles()"), TEXT("List all lane profiles defined in ZoneGraph settings"), TEXT("table[]") },
	// ZoneGraph queries
	{ TEXT("zonegraph_find_nearest_lane(location, radius, tag_filter?)"), TEXT("Find nearest lane to a world location within radius. tag_filter: {any?, all?, not?} (tag name arrays)"), TEXT("table or nil") },
	{ TEXT("zonegraph_find_overlapping_lanes(location, radius, tag_filter?)"), TEXT("Find lanes overlapping a sphere. Returns lane handles"), TEXT("table[] or nil") },
	{ TEXT("zonegraph_get_lane_info(data_index, lane_index)"), TEXT("Get detailed info for a specific lane by data/lane index"), TEXT("table or nil") },
	// ZoneGraph build
	{ TEXT("zonegraph_rebuild()"), TEXT("Force rebuild the zone graph from all registered shapes"), TEXT("bool") },
	{ TEXT("zonegraph_get_stats()"), TEXT("Get zone graph statistics (num zones, lanes, shapes, bounds)"), TEXT("table") },
	// Mass Entity Config (asset enrichment-style but global)
	{ TEXT("mass_get_entity_config(asset_path)"), TEXT("Read a MassEntityConfigAsset: parent, traits list with class names and properties"), TEXT("table or nil") },
	{ TEXT("mass_add_trait(asset_path, trait_class)"), TEXT("Add a trait to a MassEntityConfigAsset by class name (e.g. 'MassCrowdMemberTrait')"), TEXT("bool") },
	{ TEXT("mass_remove_trait(asset_path, trait_class)"), TEXT("Remove a trait from a MassEntityConfigAsset by class name"), TEXT("bool") },
	// Mass Spawners
	{ TEXT("mass_list_spawners()"), TEXT("List all AMassSpawner actors in the editor world"), TEXT("table[]") },
	{ TEXT("mass_get_spawner(name_or_label)"), TEXT("Get spawner details: count, entity types, scale"), TEXT("table or nil") },
	{ TEXT("mass_spawner_spawn(name_or_label)"), TEXT("Trigger spawning on a mass spawner (calls DoSpawning)"), TEXT("bool") },
	{ TEXT("mass_spawner_despawn(name_or_label)"), TEXT("Trigger despawning on a mass spawner (calls DoDespawning)"), TEXT("bool") },
	{ TEXT("mass_spawner_set_count(name_or_label, count)"), TEXT("Set the spawn count on a mass spawner"), TEXT("bool") },
	{ TEXT("mass_spawner_set_scale(name_or_label, scale)"), TEXT("Set the spawning count scale on a mass spawner"), TEXT("bool") },
	// Mass Simulation
	{ TEXT("mass_simulation_pause()"), TEXT("Pause the mass simulation"), TEXT("bool") },
	{ TEXT("mass_simulation_resume()"), TEXT("Resume the mass simulation"), TEXT("bool") },
	{ TEXT("mass_simulation_status()"), TEXT("Get mass simulation status (started, paused)"), TEXT("table") },
	// Mass Crowd lane state
	{ TEXT("mass_crowd_get_lane_state(data_index, lane_index)"), TEXT("Get crowd lane state (opened/closed)"), TEXT("string or nil") },
	{ TEXT("mass_crowd_set_lane_state(data_index, lane_index, state)"), TEXT("Set crowd lane state: 'opened' or 'closed'"), TEXT("bool") },
	{ TEXT("mass_crowd_rebuild_lane_data()"), TEXT("Rebuild all crowd lane data"), TEXT("bool") },
};

// ============================================================================
// TAG RESOLUTION HELPER
// ============================================================================

namespace
{

FZoneGraphTagMask MassAI_ResolveTagMask(const sol::table& TagNames, UZoneGraphSubsystem* ZGSub)
{
	FZoneGraphTagMask Mask;
	if (!ZGSub) return Mask;
	for (auto& Kv : TagNames)
	{
		sol::optional<std::string> NameOpt = Kv.second.as<sol::optional<std::string>>();
		if (NameOpt.has_value())
		{
			FName TagFName = FName(UTF8_TO_TCHAR(NameOpt.value().c_str()));
			FZoneGraphTag Tag = ZGSub->GetTagByName(TagFName);
			if (Tag.IsValid())
			{
				Mask.Add(Tag);
			}
		}
	}
	return Mask;
}

FZoneGraphTagFilter MassAI_BuildTagFilter(const sol::table& FilterTable, UZoneGraphSubsystem* ZGSub)
{
	FZoneGraphTagFilter Filter;
	if (!ZGSub) return Filter;

	sol::optional<sol::table> AnyOpt = FilterTable.get<sol::optional<sol::table>>("any");
	if (AnyOpt.has_value()) Filter.AnyTags = MassAI_ResolveTagMask(AnyOpt.value(), ZGSub);

	sol::optional<sol::table> AllOpt = FilterTable.get<sol::optional<sol::table>>("all");
	if (AllOpt.has_value()) Filter.AllTags = MassAI_ResolveTagMask(AllOpt.value(), ZGSub);

	sol::optional<sol::table> NotOpt = FilterTable.get<sol::optional<sol::table>>("not");
	if (NotOpt.has_value()) Filter.NotTags = MassAI_ResolveTagMask(NotOpt.value(), ZGSub);

	return Filter;
}

} // anonymous namespace

// ============================================================================
// BINDING
// ============================================================================

static void BindMassAI(sol::state& Lua, FLuaSessionData& Session)
{
	// ================================================================
	// zonegraph_list_shapes()
	// ================================================================
	Lua.set_function("zonegraph_list_shapes", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = MassAI_GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] zonegraph_list_shapes -> No editor world"));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;
		for (TActorIterator<AZoneShape> It(World); It; ++It)
		{
			AZoneShape* ZS = *It;
			if (!ZS) continue;

			const UZoneShapeComponent* Shape = ZS->GetShape();
			sol::table Entry = LuaView.create_table();
			Entry["name"] = TCHAR_TO_UTF8(*ZS->GetActorNameOrLabel());
			Entry["location"] = MassAI_VectorToTable(LuaView, ZS->GetActorLocation());
			Entry["shape_type"] = Shape ? ShapeTypeToString(Shape->GetShapeType()) : "unknown";
			Entry["num_points"] = Shape ? Shape->GetNumPoints() : 0;
			Entry["tags_mask"] = Shape ? Shape->GetTags().GetValue() : 0u;
			Result[Idx++] = Entry;
		}

		Session.Log(FString(TEXT("[OK] zonegraph_list_shapes -> ")) + FString::FromInt(Idx - 1) + TEXT(" shapes found"));
		return Result;
	});

	// ================================================================
	// zonegraph_get_shape(name_or_label)
	// ================================================================
	Lua.set_function("zonegraph_get_shape", [&Session](const std::string& NameStr, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = MassAI_GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] zonegraph_get_shape -> No editor world"));
			return sol::lua_nil;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AZoneShape* ZS = FindZoneShapeByName(World, Name);
		if (!ZS)
		{
			Session.Log(FString(TEXT("[FAIL] zonegraph_get_shape -> Could not find zone shape '")) + Name + TEXT("'"));
			return sol::lua_nil;
		}

		const UZoneShapeComponent* Shape = ZS->GetShape();
		if (!Shape)
		{
			Session.Log(FString(TEXT("[FAIL] zonegraph_get_shape -> Shape component is null for '")) + Name + TEXT("'"));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		Result["name"] = TCHAR_TO_UTF8(*ZS->GetActorNameOrLabel());
		Result["location"] = MassAI_VectorToTable(LuaView, ZS->GetActorLocation());
		Result["shape_type"] = ShapeTypeToString(Shape->GetShapeType());
		Result["is_closed"] = Shape->IsShapeClosed();
		Result["reverse_lane_profile"] = Shape->IsLaneProfileReversed();
		Result["tags_mask"] = Shape->GetTags().GetValue();

		// Points
		TConstArrayView<FZoneShapePoint> Points = Shape->GetPoints();
		sol::table PointsTable = LuaView.create_table();
		for (int32 i = 0; i < Points.Num(); i++)
		{
			sol::table Pt = LuaView.create_table();
			Pt["x"] = Points[i].Position.X;
			Pt["y"] = Points[i].Position.Y;
			Pt["z"] = Points[i].Position.Z;
			Pt["tangent_length"] = Points[i].TangentLength;
			Pt["inner_turn_radius"] = Points[i].InnerTurnRadius;
			switch (Points[i].Type)
			{
			case FZoneShapePointType::Sharp: Pt["type"] = "sharp"; break;
			case FZoneShapePointType::Bezier: Pt["type"] = "bezier"; break;
			case FZoneShapePointType::AutoBezier: Pt["type"] = "auto_bezier"; break;
			case FZoneShapePointType::LaneProfile: Pt["type"] = "lane_profile"; break;
			}
			PointsTable[i + 1] = Pt;
		}
		Result["points"] = PointsTable;

		// Lane profile ref (GetCommonLaneProfile is non-const; const_cast is safe for read-only access)
		const FZoneLaneProfileRef& ProfileRef = const_cast<UZoneShapeComponent*>(Shape)->GetCommonLaneProfile();
		sol::table LPTable = LuaView.create_table();
		LPTable["name"] = TCHAR_TO_UTF8(*ProfileRef.Name.ToString());
		LPTable["id"] = TCHAR_TO_UTF8(*ProfileRef.ID.ToString());
		Result["lane_profile"] = LPTable;

		// Resolve tag names
		UZoneGraphSubsystem* ZGSub = World->GetSubsystem<UZoneGraphSubsystem>();
		if (ZGSub)
		{
			sol::table TagNames = LuaView.create_table();
			int32 TagIdx = 1;
			TConstArrayView<FZoneGraphTagInfo> TagInfos = ZGSub->GetTagInfos();
			FZoneGraphTagMask ShapeTags = Shape->GetTags();
			for (const FZoneGraphTagInfo& Info : TagInfos)
			{
				if (Info.IsValid() && ShapeTags.Contains(Info.Tag))
				{
					TagNames[TagIdx++] = TCHAR_TO_UTF8(*Info.Name.ToString());
				}
			}
			Result["tag_names"] = TagNames;
		}

		Session.Log(FString(TEXT("[OK] zonegraph_get_shape -> '")) + Name + TEXT("'"));
		return Result;
	});

	// ================================================================
	// zonegraph_spawn_shape(params)
	// ================================================================
	Lua.set_function("zonegraph_spawn_shape", [&Session](const sol::table& Params, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = MassAI_GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] zonegraph_spawn_shape -> No editor world"));
			return sol::lua_nil;
		}

		// Location
		sol::optional<sol::table> LocOpt = Params.get<sol::optional<sol::table>>("location");
		FVector Location = FVector::ZeroVector;
		if (LocOpt.has_value())
		{
			Location = MassAI_TableToVector(LocOpt.value());
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Spawn Zone Shape")));

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AZoneShape* NewActor = World->SpawnActor<AZoneShape>(AZoneShape::StaticClass(), FTransform(FRotator::ZeroRotator, Location), SpawnParams);
		if (!NewActor)
		{
			Session.Log(TEXT("[FAIL] zonegraph_spawn_shape -> SpawnActor returned null"));
			return sol::lua_nil;
		}

		// Label
		sol::optional<std::string> LabelOpt = Params.get<sol::optional<std::string>>("label");
		if (LabelOpt.has_value())
		{
			NewActor->SetActorLabel(UTF8_TO_TCHAR(LabelOpt.value().c_str()));
		}

		// We need mutable access to the shape component — GetShape() returns const
		UZoneShapeComponent* Shape = const_cast<UZoneShapeComponent*>(NewActor->GetShape());
		if (!Shape)
		{
			Session.Log(TEXT("[FAIL] zonegraph_spawn_shape -> Shape component is null"));
			return sol::lua_nil;
		}

		// Shape type
		sol::optional<std::string> ShapeTypeOpt = Params.get<sol::optional<std::string>>("shape_type");
		if (ShapeTypeOpt.has_value())
		{
			FString TypeStr = UTF8_TO_TCHAR(ShapeTypeOpt.value().c_str());
			if (TypeStr.Equals(TEXT("polygon"), ESearchCase::IgnoreCase))
			{
				Shape->SetShapeType(FZoneShapeType::Polygon);
			}
			else
			{
				Shape->SetShapeType(FZoneShapeType::Spline);
			}
		}

		// Reverse profile
		sol::optional<bool> ReverseOpt = Params.get<sol::optional<bool>>("reverse_profile");
		if (ReverseOpt.has_value())
		{
			Shape->SetReverseLaneProfile(ReverseOpt.value());
		}

		// Lane profile (by name)
		sol::optional<std::string> ProfileNameOpt = Params.get<sol::optional<std::string>>("lane_profile");
		if (ProfileNameOpt.has_value())
		{
			FString ProfileName = UTF8_TO_TCHAR(ProfileNameOpt.value().c_str());
			const UZoneGraphSettings* Settings = GetDefault<UZoneGraphSettings>();
			if (Settings)
			{
				for (const FZoneLaneProfile& Profile : Settings->GetLaneProfiles())
				{
					if (Profile.Name.ToString().Equals(ProfileName, ESearchCase::IgnoreCase))
					{
						Shape->SetCommonLaneProfile(FZoneLaneProfileRef(Profile));
						break;
					}
				}
			}
		}

		// Tags
		sol::optional<sol::table> TagsOpt = Params.get<sol::optional<sol::table>>("tags");
		if (TagsOpt.has_value())
		{
			UZoneGraphSubsystem* ZGSub = World->GetSubsystem<UZoneGraphSubsystem>();
			if (ZGSub)
			{
				FZoneGraphTagMask TagMask = MassAI_ResolveTagMask(TagsOpt.value(), ZGSub);
				Shape->SetTags(TagMask);
			}
		}

		// Points
		sol::optional<sol::table> PointsOpt = Params.get<sol::optional<sol::table>>("points");
		if (PointsOpt.has_value())
		{
			TArray<FZoneShapePoint>& MutablePoints = Shape->GetMutablePoints();
			MutablePoints.Empty();

			sol::table Points = PointsOpt.value();
			for (auto& Kv : Points)
			{
				sol::optional<sol::table> PtOpt = Kv.second.as<sol::optional<sol::table>>();
				if (!PtOpt.has_value()) continue;

				sol::table Pt = PtOpt.value();
				FVector Pos = MassAI_TableToVector(Pt);
				// Convert world to local
				FVector LocalPos = NewActor->GetActorTransform().InverseTransformPosition(Pos);
				FZoneShapePoint ShapePoint(LocalPos);

				sol::optional<std::string> TypeOpt = Pt.get<sol::optional<std::string>>("type");
				if (TypeOpt.has_value())
				{
					FString TypeStr = UTF8_TO_TCHAR(TypeOpt.value().c_str());
					if (TypeStr.Equals(TEXT("bezier"), ESearchCase::IgnoreCase))
						ShapePoint.Type = FZoneShapePointType::Bezier;
					else if (TypeStr.Equals(TEXT("auto_bezier"), ESearchCase::IgnoreCase))
						ShapePoint.Type = FZoneShapePointType::AutoBezier;
					else if (TypeStr.Equals(TEXT("lane_profile"), ESearchCase::IgnoreCase))
						ShapePoint.Type = FZoneShapePointType::LaneProfile;
					else
						ShapePoint.Type = FZoneShapePointType::Sharp;
				}

				sol::optional<float> TangentOpt = Pt.get<sol::optional<float>>("tangent_length");
				if (TangentOpt.has_value())
					ShapePoint.TangentLength = TangentOpt.value();

				sol::optional<float> RadiusOpt = Pt.get<sol::optional<float>>("inner_turn_radius");
				if (RadiusOpt.has_value())
					ShapePoint.InnerTurnRadius = RadiusOpt.value();

				MutablePoints.Add(ShapePoint);
			}
		}

		Shape->UpdateShape();
		Shape->MarkPackageDirty();

		FString ActorName = NewActor->GetActorNameOrLabel();
		Session.Log(FString(TEXT("[OK] zonegraph_spawn_shape -> Spawned '")) + ActorName + TEXT("'"));
		return sol::make_object(LuaView, std::string(TCHAR_TO_UTF8(*ActorName)));
	});

	// ================================================================
	// zonegraph_remove_shape(name_or_label)
	// ================================================================
	Lua.set_function("zonegraph_remove_shape", [&Session](const std::string& NameStr) -> bool
	{
		UWorld* World = MassAI_GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] zonegraph_remove_shape -> No editor world"));
			return false;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AZoneShape* ZS = FindZoneShapeByName(World, Name);
		if (!ZS)
		{
			Session.Log(FString(TEXT("[FAIL] zonegraph_remove_shape -> Could not find '")) + Name + TEXT("'"));
			return false;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Remove Zone Shape")));
		World->DestroyActor(ZS);
		Session.Log(FString(TEXT("[OK] zonegraph_remove_shape -> Removed '")) + Name + TEXT("'"));
		return true;
	});

	// ================================================================
	// zonegraph_set_shape_points(name_or_label, points)
	// ================================================================
	Lua.set_function("zonegraph_set_shape_points", [&Session](const std::string& NameStr, const sol::table& PointsTable) -> bool
	{
		UWorld* World = MassAI_GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] zonegraph_set_shape_points -> No editor world"));
			return false;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AZoneShape* ZS = FindZoneShapeByName(World, Name);
		if (!ZS)
		{
			Session.Log(FString(TEXT("[FAIL] zonegraph_set_shape_points -> Could not find '")) + Name + TEXT("'"));
			return false;
		}

		UZoneShapeComponent* Shape = const_cast<UZoneShapeComponent*>(ZS->GetShape());
		if (!Shape)
		{
			Session.Log(TEXT("[FAIL] zonegraph_set_shape_points -> Shape component is null"));
			return false;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Set Zone Shape Points")));
		Shape->Modify();

		TArray<FZoneShapePoint>& MutablePoints = Shape->GetMutablePoints();
		MutablePoints.Empty();

		for (auto& Kv : PointsTable)
		{
			sol::optional<sol::table> PtOpt = Kv.second.as<sol::optional<sol::table>>();
			if (!PtOpt.has_value()) continue;

			sol::table Pt = PtOpt.value();
			FVector Pos = MassAI_TableToVector(Pt);
			FVector LocalPos = ZS->GetActorTransform().InverseTransformPosition(Pos);
			FZoneShapePoint ShapePoint(LocalPos);

			sol::optional<std::string> TypeOpt = Pt.get<sol::optional<std::string>>("type");
			if (TypeOpt.has_value())
			{
				FString TypeStr = UTF8_TO_TCHAR(TypeOpt.value().c_str());
				if (TypeStr.Equals(TEXT("bezier"), ESearchCase::IgnoreCase))
					ShapePoint.Type = FZoneShapePointType::Bezier;
				else if (TypeStr.Equals(TEXT("auto_bezier"), ESearchCase::IgnoreCase))
					ShapePoint.Type = FZoneShapePointType::AutoBezier;
				else if (TypeStr.Equals(TEXT("lane_profile"), ESearchCase::IgnoreCase))
					ShapePoint.Type = FZoneShapePointType::LaneProfile;
				else
					ShapePoint.Type = FZoneShapePointType::Sharp;
			}

			sol::optional<float> TangentOpt = Pt.get<sol::optional<float>>("tangent_length");
			if (TangentOpt.has_value())
				ShapePoint.TangentLength = TangentOpt.value();

			sol::optional<float> RadiusOpt = Pt.get<sol::optional<float>>("inner_turn_radius");
			if (RadiusOpt.has_value())
				ShapePoint.InnerTurnRadius = RadiusOpt.value();

			MutablePoints.Add(ShapePoint);
		}

		Shape->UpdateShape();
		Shape->MarkPackageDirty();

		Session.Log(FString(TEXT("[OK] zonegraph_set_shape_points -> Set ")) + FString::FromInt(MutablePoints.Num()) + TEXT(" points on '") + Name + TEXT("'"));
		return true;
	});

	// ================================================================
	// zonegraph_set_shape_tags(name_or_label, tags)
	// ================================================================
	Lua.set_function("zonegraph_set_shape_tags", [&Session](const std::string& NameStr, const sol::table& TagNames) -> bool
	{
		UWorld* World = MassAI_GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] zonegraph_set_shape_tags -> No editor world"));
			return false;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AZoneShape* ZS = FindZoneShapeByName(World, Name);
		if (!ZS)
		{
			Session.Log(FString(TEXT("[FAIL] zonegraph_set_shape_tags -> Could not find '")) + Name + TEXT("'"));
			return false;
		}

		UZoneShapeComponent* Shape = const_cast<UZoneShapeComponent*>(ZS->GetShape());
		if (!Shape)
		{
			Session.Log(TEXT("[FAIL] zonegraph_set_shape_tags -> Shape component is null"));
			return false;
		}

		UZoneGraphSubsystem* ZGSub = World->GetSubsystem<UZoneGraphSubsystem>();
		if (!ZGSub)
		{
			Session.Log(TEXT("[FAIL] zonegraph_set_shape_tags -> ZoneGraph subsystem not available"));
			return false;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Set Zone Shape Tags")));
		Shape->Modify();

		FZoneGraphTagMask TagMask = MassAI_ResolveTagMask(TagNames, ZGSub);
		Shape->SetTags(TagMask);
		Shape->UpdateShape();
		Shape->MarkPackageDirty();

		Session.Log(FString(TEXT("[OK] zonegraph_set_shape_tags -> Tags set on '")) + Name + TEXT("'"));
		return true;
	});

	// ================================================================
	// zonegraph_list_tags()
	// ================================================================
	Lua.set_function("zonegraph_list_tags", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		const UZoneGraphSettings* Settings = GetDefault<UZoneGraphSettings>();
		if (!Settings)
		{
			Session.Log(TEXT("[FAIL] zonegraph_list_tags -> No ZoneGraph settings"));
			return sol::lua_nil;
		}

		TArray<FZoneGraphTagInfo> ValidInfos;
		Settings->GetValidTagInfos(ValidInfos);

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;
		for (const FZoneGraphTagInfo& Info : ValidInfos)
		{
			sol::table Entry = LuaView.create_table();
			Entry["name"] = TCHAR_TO_UTF8(*Info.Name.ToString());
			Entry["bit"] = Info.Tag.Get();
			sol::table Color = LuaView.create_table();
			Color["r"] = Info.Color.R;
			Color["g"] = Info.Color.G;
			Color["b"] = Info.Color.B;
			Entry["color"] = Color;
			Result[Idx++] = Entry;
		}

		Session.Log(FString(TEXT("[OK] zonegraph_list_tags -> ")) + FString::FromInt(Idx - 1) + TEXT(" tags"));
		return Result;
	});

	// ================================================================
	// zonegraph_list_lane_profiles()
	// ================================================================
	Lua.set_function("zonegraph_list_lane_profiles", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		const UZoneGraphSettings* Settings = GetDefault<UZoneGraphSettings>();
		if (!Settings)
		{
			Session.Log(TEXT("[FAIL] zonegraph_list_lane_profiles -> No ZoneGraph settings"));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;
		for (const FZoneLaneProfile& Profile : Settings->GetLaneProfiles())
		{
			sol::table Entry = LuaView.create_table();
			Entry["name"] = TCHAR_TO_UTF8(*Profile.Name.ToString());
			Entry["id"] = TCHAR_TO_UTF8(*Profile.ID.ToString());
			Entry["total_width"] = Profile.GetLanesTotalWidth();
			Entry["num_lanes"] = Profile.Lanes.Num();
			Entry["is_symmetrical"] = Profile.IsSymmetrical();

			// Lane descriptions
			sol::table LanesTable = LuaView.create_table();
			int32 LaneIdx = 1;
			for (const FZoneLaneDesc& Lane : Profile.Lanes)
			{
				sol::table LaneEntry = LuaView.create_table();
				LaneEntry["width"] = Lane.Width;
				LaneEntry["direction"] = LaneDirectionToString(Lane.Direction);
				LaneEntry["tags_mask"] = Lane.Tags.GetValue();
				LanesTable[LaneIdx++] = LaneEntry;
			}
			Entry["lanes"] = LanesTable;

			Result[Idx++] = Entry;
		}

		Session.Log(FString(TEXT("[OK] zonegraph_list_lane_profiles -> ")) + FString::FromInt(Idx - 1) + TEXT(" profiles"));
		return Result;
	});

	// ================================================================
	// zonegraph_find_nearest_lane(location, radius, tag_filter?)
	// ================================================================
	Lua.set_function("zonegraph_find_nearest_lane", [&Session](const sol::table& LocationTable, float Radius, sol::optional<sol::table> FilterOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = MassAI_GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] zonegraph_find_nearest_lane -> No editor world"));
			return sol::lua_nil;
		}

		UZoneGraphSubsystem* ZGSub = World->GetSubsystem<UZoneGraphSubsystem>();
		if (!ZGSub)
		{
			Session.Log(TEXT("[FAIL] zonegraph_find_nearest_lane -> ZoneGraph subsystem not available"));
			return sol::lua_nil;
		}

		FVector Center = MassAI_TableToVector(LocationTable);
		FBox QueryBounds = FBox(Center - FVector(Radius), Center + FVector(Radius));

		FZoneGraphTagFilter TagFilter;
		if (FilterOpt.has_value())
		{
			TagFilter = MassAI_BuildTagFilter(FilterOpt.value(), ZGSub);
		}

		FZoneGraphLaneLocation LaneLocation;
		float DistanceSqr = 0.f;
		bool bFound = ZGSub->FindNearestLane(QueryBounds, TagFilter, LaneLocation, DistanceSqr);
		if (!bFound)
		{
			Session.Log(TEXT("[OK] zonegraph_find_nearest_lane -> No lane found"));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		Result["position"] = MassAI_VectorToTable(LuaView, LaneLocation.Position);
		Result["direction"] = MassAI_VectorToTable(LuaView, LaneLocation.Direction);
		Result["distance"] = FMath::Sqrt(DistanceSqr);
		Result["distance_along_lane"] = LaneLocation.DistanceAlongLane;
		Result["lane_index"] = LaneLocation.LaneHandle.Index;
		Result["data_index"] = static_cast<int>(LaneLocation.LaneHandle.DataHandle.Index);

		// Get lane length and width
		float LaneLength = 0.f;
		if (ZGSub->GetLaneLength(LaneLocation.LaneHandle, LaneLength))
		{
			Result["lane_length"] = LaneLength;
		}

		float LaneWidth = 0.f;
		if (ZGSub->GetLaneWidth(LaneLocation.LaneHandle, LaneWidth))
		{
			Result["lane_width"] = LaneWidth;
		}

		Session.Log(TEXT("[OK] zonegraph_find_nearest_lane -> Found lane"));
		return Result;
	});

	// ================================================================
	// zonegraph_find_overlapping_lanes(location, radius, tag_filter?)
	// ================================================================
	Lua.set_function("zonegraph_find_overlapping_lanes", [&Session](const sol::table& LocationTable, float Radius, sol::optional<sol::table> FilterOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = MassAI_GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] zonegraph_find_overlapping_lanes -> No editor world"));
			return sol::lua_nil;
		}

		UZoneGraphSubsystem* ZGSub = World->GetSubsystem<UZoneGraphSubsystem>();
		if (!ZGSub)
		{
			Session.Log(TEXT("[FAIL] zonegraph_find_overlapping_lanes -> ZoneGraph subsystem not available"));
			return sol::lua_nil;
		}

		FVector Center = MassAI_TableToVector(LocationTable);
		FBox QueryBounds = FBox(Center - FVector(Radius), Center + FVector(Radius));

		FZoneGraphTagFilter TagFilter;
		if (FilterOpt.has_value())
		{
			TagFilter = MassAI_BuildTagFilter(FilterOpt.value(), ZGSub);
		}

		TArray<FZoneGraphLaneHandle> Lanes;
		bool bFound = ZGSub->FindOverlappingLanes(QueryBounds, TagFilter, Lanes);
		if (!bFound || Lanes.Num() == 0)
		{
			Session.Log(TEXT("[OK] zonegraph_find_overlapping_lanes -> No lanes found"));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;
		for (const FZoneGraphLaneHandle& Handle : Lanes)
		{
			sol::table Entry = LuaView.create_table();
			Entry["lane_index"] = Handle.Index;
			Entry["data_index"] = static_cast<int>(Handle.DataHandle.Index);

			float LaneLength = 0.f;
			if (ZGSub->GetLaneLength(Handle, LaneLength))
				Entry["lane_length"] = LaneLength;

			float LaneWidth = 0.f;
			if (ZGSub->GetLaneWidth(Handle, LaneWidth))
				Entry["lane_width"] = LaneWidth;

			Result[Idx++] = Entry;
		}

		Session.Log(FString(TEXT("[OK] zonegraph_find_overlapping_lanes -> ")) + FString::FromInt(Lanes.Num()) + TEXT(" lanes found"));
		return Result;
	});

	// ================================================================
	// zonegraph_get_lane_info(data_index, lane_index)
	// ================================================================
	Lua.set_function("zonegraph_get_lane_info", [&Session](int DataIdx, int LaneIdx, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = MassAI_GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] zonegraph_get_lane_info -> No editor world"));
			return sol::lua_nil;
		}

		UZoneGraphSubsystem* ZGSub = World->GetSubsystem<UZoneGraphSubsystem>();
		if (!ZGSub)
		{
			Session.Log(TEXT("[FAIL] zonegraph_get_lane_info -> ZoneGraph subsystem not available"));
			return sol::lua_nil;
		}

		// Build a lane handle from the indices
		TConstArrayView<FRegisteredZoneGraphData> RegisteredData = ZGSub->GetRegisteredZoneGraphData();
		if (DataIdx < 0 || DataIdx >= RegisteredData.Num() || !RegisteredData[DataIdx].bInUse)
		{
			Session.Log(TEXT("[FAIL] zonegraph_get_lane_info -> Invalid data index"));
			return sol::lua_nil;
		}

		const AZoneGraphData* ZGData = RegisteredData[DataIdx].ZoneGraphData;
		if (!ZGData)
		{
			Session.Log(TEXT("[FAIL] zonegraph_get_lane_info -> ZoneGraph data is null"));
			return sol::lua_nil;
		}

		const FZoneGraphStorage& Storage = ZGData->GetStorage();
		if (LaneIdx < 0 || LaneIdx >= Storage.Lanes.Num())
		{
			Session.Log(TEXT("[FAIL] zonegraph_get_lane_info -> Invalid lane index"));
			return sol::lua_nil;
		}

		const FZoneLaneData& LaneData = Storage.Lanes[LaneIdx];
		FZoneGraphDataHandle DataHandle(static_cast<uint16>(DataIdx), static_cast<uint16>(RegisteredData[DataIdx].Generation));
		FZoneGraphLaneHandle LaneHandle(LaneIdx, DataHandle);

		sol::table Result = LuaView.create_table();
		Result["lane_index"] = LaneIdx;
		Result["data_index"] = DataIdx;
		Result["width"] = LaneData.Width;
		Result["zone_index"] = LaneData.ZoneIndex;
		Result["num_points"] = LaneData.GetNumPoints();
		Result["tags_mask"] = LaneData.Tags.GetValue();

		// Lane length
		float LaneLength = 0.f;
		if (ZGSub->GetLaneLength(LaneHandle, LaneLength))
			Result["length"] = LaneLength;

		// Resolve tag names
		sol::table TagNames = LuaView.create_table();
		int32 TagIdx = 1;
		TConstArrayView<FZoneGraphTagInfo> TagInfos = ZGSub->GetTagInfos();
		for (const FZoneGraphTagInfo& Info : TagInfos)
		{
			if (Info.IsValid() && LaneData.Tags.Contains(Info.Tag))
			{
				TagNames[TagIdx++] = TCHAR_TO_UTF8(*Info.Name.ToString());
			}
		}
		Result["tag_names"] = TagNames;

		// Lane start/end points
		if (LaneData.GetNumPoints() > 0)
		{
			FVector StartPt = Storage.LanePoints[LaneData.PointsBegin];
			FVector EndPt = Storage.LanePoints[LaneData.PointsEnd - 1];
			Result["start"] = MassAI_VectorToTable(LuaView, StartPt);
			Result["end"] = MassAI_VectorToTable(LuaView, EndPt);
		}

		// Links
		sol::table LinksTable = LuaView.create_table();
		int32 LinkIdx = 1;
		for (int32 i = LaneData.LinksBegin; i < LaneData.LinksEnd; i++)
		{
			const FZoneLaneLinkData& Link = Storage.LaneLinks[i];
			sol::table LinkEntry = LuaView.create_table();
			LinkEntry["dest_lane_index"] = Link.DestLaneIndex;
			switch (Link.Type)
			{
			case EZoneLaneLinkType::Outgoing: LinkEntry["type"] = "outgoing"; break;
			case EZoneLaneLinkType::Incoming: LinkEntry["type"] = "incoming"; break;
			case EZoneLaneLinkType::Adjacent: LinkEntry["type"] = "adjacent"; break;
			default: LinkEntry["type"] = "none"; break;
			}
			LinksTable[LinkIdx++] = LinkEntry;
		}
		Result["links"] = LinksTable;

		Session.Log(TEXT("[OK] zonegraph_get_lane_info -> Retrieved lane info"));
		return Result;
	});

	// ================================================================
	// zonegraph_rebuild()
	// ================================================================
	Lua.set_function("zonegraph_rebuild", [&Session]() -> bool
	{
		UWorld* World = MassAI_GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] zonegraph_rebuild -> No editor world"));
			return false;
		}

		UZoneGraphSubsystem* ZGSub = World->GetSubsystem<UZoneGraphSubsystem>();
		if (!ZGSub)
		{
			Session.Log(TEXT("[FAIL] zonegraph_rebuild -> ZoneGraph subsystem not available"));
			return false;
		}

#if WITH_EDITOR
		FZoneGraphBuilder& Builder = ZGSub->GetBuilder();

		// Collect all zone graph data actors
		TArray<AZoneGraphData*> AllData;
		for (TActorIterator<AZoneGraphData> It(World); It; ++It)
		{
			AllData.Add(*It);
		}

		Builder.BuildAll(AllData, true);

		Session.Log(TEXT("[OK] zonegraph_rebuild -> Zone graph rebuilt"));
		return true;
#else
		Session.Log(TEXT("[FAIL] zonegraph_rebuild -> Only available in editor builds"));
		return false;
#endif
	});

	// ================================================================
	// zonegraph_get_stats()
	// ================================================================
	Lua.set_function("zonegraph_get_stats", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = MassAI_GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] zonegraph_get_stats -> No editor world"));
			return sol::lua_nil;
		}

		UZoneGraphSubsystem* ZGSub = World->GetSubsystem<UZoneGraphSubsystem>();
		if (!ZGSub)
		{
			Session.Log(TEXT("[FAIL] zonegraph_get_stats -> ZoneGraph subsystem not available"));
			return sol::lua_nil;
		}

		TConstArrayView<FRegisteredZoneGraphData> RegisteredData = ZGSub->GetRegisteredZoneGraphData();
		int32 TotalZones = 0;
		int32 TotalLanes = 0;
		int32 ActiveData = 0;

		for (const FRegisteredZoneGraphData& Data : RegisteredData)
		{
			if (!Data.bInUse || !Data.ZoneGraphData) continue;
			ActiveData++;
			const FZoneGraphStorage& Storage = Data.ZoneGraphData->GetStorage();
			TotalZones += Storage.Zones.Num();
			TotalLanes += Storage.Lanes.Num();
		}

		// Count shapes
		int32 ShapeCount = 0;
		for (TActorIterator<AZoneShape> It(World); It; ++It)
		{
			ShapeCount++;
		}

		FBox Bounds = ZGSub->GetCombinedBounds();

		sol::table Result = LuaView.create_table();
		Result["num_data"] = ActiveData;
		Result["num_zones"] = TotalZones;
		Result["num_lanes"] = TotalLanes;
		Result["num_shapes"] = ShapeCount;

		if (Bounds.IsValid)
		{
			Result["bounds_min"] = MassAI_VectorToTable(LuaView, FVector(Bounds.Min));
			Result["bounds_max"] = MassAI_VectorToTable(LuaView, FVector(Bounds.Max));
		}

		Session.Log(TEXT("[OK] zonegraph_get_stats"));
		return Result;
	});

	// ================================================================
	// mass_get_entity_config(asset_path)
	// ================================================================
	Lua.set_function("mass_get_entity_config", [&Session](const std::string& PathStr, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString Path = UTF8_TO_TCHAR(PathStr.c_str());
		UMassEntityConfigAsset* ConfigAsset = LoadObject<UMassEntityConfigAsset>(nullptr, *Path);
		if (!ConfigAsset)
		{
			Session.Log(FString(TEXT("[FAIL] mass_get_entity_config -> Could not load '")) + Path + TEXT("'"));
			return sol::lua_nil;
		}

		const FMassEntityConfig& Config = ConfigAsset->GetConfig();

		sol::table Result = LuaView.create_table();
		Result["path"] = PathStr;

		// Parent
		const UMassEntityConfigAsset* Parent = Config.GetParent();
		if (Parent)
		{
			Result["parent"] = TCHAR_TO_UTF8(*Parent->GetPathName());
		}

		// Traits
		TConstArrayView<UMassEntityTraitBase*> Traits = Config.GetTraits();
		sol::table TraitsTable = LuaView.create_table();
		int32 Idx = 1;
		for (const UMassEntityTraitBase* Trait : Traits)
		{
			if (!Trait) continue;
			sol::table TraitEntry = LuaView.create_table();
			TraitEntry["class"] = TCHAR_TO_UTF8(*Trait->GetClass()->GetName());
			TraitEntry["class_path"] = TCHAR_TO_UTF8(*Trait->GetClass()->GetPathName());

			// Read UPROPERTY values via reflection
			sol::table Props = LuaView.create_table();
			for (TFieldIterator<FProperty> PropIt(Trait->GetClass()); PropIt; ++PropIt)
			{
				FProperty* Prop = *PropIt;
				if (!Prop || Prop->HasAnyPropertyFlags(CPF_Transient)) continue;
				// Only include properties declared on the trait class itself (not UObject base)
				if (Prop->GetOwnerClass() == UObject::StaticClass()) continue;

				FString PropName = Prop->GetName();
				FString ValueStr;
				Prop->ExportTextItem_Direct(ValueStr, Prop->ContainerPtrToValuePtr<void>(Trait), nullptr, nullptr, PPF_None);
				if (!ValueStr.IsEmpty())
				{
					Props[TCHAR_TO_UTF8(*PropName)] = TCHAR_TO_UTF8(*ValueStr);
				}
			}
			TraitEntry["properties"] = Props;

			TraitsTable[Idx++] = TraitEntry;
		}
		Result["traits"] = TraitsTable;
		Result["num_traits"] = Idx - 1;

		Session.Log(FString(TEXT("[OK] mass_get_entity_config -> '")) + Path + TEXT("' with ") + FString::FromInt(Idx - 1) + TEXT(" traits"));
		return Result;
	});

	// ================================================================
	// mass_add_trait(asset_path, trait_class)
	// ================================================================
	Lua.set_function("mass_add_trait", [&Session](const std::string& PathStr, const std::string& TraitClassStr) -> bool
	{
#if WITH_EDITOR
		FString Path = UTF8_TO_TCHAR(PathStr.c_str());
		UMassEntityConfigAsset* ConfigAsset = LoadObject<UMassEntityConfigAsset>(nullptr, *Path);
		if (!ConfigAsset)
		{
			Session.Log(FString(TEXT("[FAIL] mass_add_trait -> Could not load '")) + Path + TEXT("'"));
			return false;
		}

		FString ClassName = UTF8_TO_TCHAR(TraitClassStr.c_str());

		// Find the trait class by name
		UClass* TraitClass = nullptr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->IsChildOf(UMassEntityTraitBase::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract))
			{
				if (It->GetName() == ClassName || It->GetName().Equals(ClassName, ESearchCase::IgnoreCase))
				{
					TraitClass = *It;
					break;
				}
			}
		}

		if (!TraitClass)
		{
			Session.Log(FString(TEXT("[FAIL] mass_add_trait -> Could not find trait class '")) + ClassName + TEXT("'"));
			return false;
		}

		// Check if already has this trait
		const FMassEntityConfig& Config = ConfigAsset->GetConfig();
		if (Config.FindTrait(TraitClass))
		{
			Session.Log(FString(TEXT("[WARN] mass_add_trait -> '")) + ClassName + TEXT("' already exists on config"));
			return true;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Add Mass Trait")));
		ConfigAsset->Modify();

	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		UMassEntityTraitBase* NewTrait = ConfigAsset->AddTrait(TraitClass);
		if (!NewTrait)
		{
			Session.Log(FString(TEXT("[FAIL] mass_add_trait -> Failed to add trait '")) + ClassName + TEXT("'"));
			return false;
		}
#else
		Session.Log(TEXT("[FAIL] mass_add_trait -> AddTrait requires UE 5.5+"));
		return false;
#endif

		ConfigAsset->MarkPackageDirty();
		Session.Log(FString(TEXT("[OK] mass_add_trait -> Added '")) + ClassName + TEXT("' to '") + Path + TEXT("'"));
		return true;
#else
		Session.Log(TEXT("[FAIL] mass_add_trait -> Only available in editor builds"));
		return false;
#endif
	});

	// ================================================================
	// mass_remove_trait(asset_path, trait_class)
	// ================================================================
	Lua.set_function("mass_remove_trait", [&Session](const std::string& PathStr, const std::string& TraitClassStr) -> bool
	{
		FString Path = UTF8_TO_TCHAR(PathStr.c_str());
		UMassEntityConfigAsset* ConfigAsset = LoadObject<UMassEntityConfigAsset>(nullptr, *Path);
		if (!ConfigAsset)
		{
			Session.Log(FString(TEXT("[FAIL] mass_remove_trait -> Could not load '")) + Path + TEXT("'"));
			return false;
		}

		FString ClassName = UTF8_TO_TCHAR(TraitClassStr.c_str());
		FMassEntityConfig& Config = ConfigAsset->GetMutableConfig();
		TConstArrayView<UMassEntityTraitBase*> Traits = Config.GetTraits();

		int32 FoundIdx = INDEX_NONE;
		for (int32 i = 0; i < Traits.Num(); i++)
		{
			if (Traits[i] && (Traits[i]->GetClass()->GetName() == ClassName ||
				Traits[i]->GetClass()->GetName().Equals(ClassName, ESearchCase::IgnoreCase)))
			{
				FoundIdx = i;
				break;
			}
		}

		if (FoundIdx == INDEX_NONE)
		{
			Session.Log(FString(TEXT("[FAIL] mass_remove_trait -> Trait '")) + ClassName + TEXT("' not found on config"));
			return false;
		}

		// FMassEntityConfig stores traits in a TArray<TObjectPtr<UMassEntityTraitBase>>
		// We need to use reflection to access the protected Traits array
		FProperty* TraitsProp = FMassEntityConfig::StaticStruct()->FindPropertyByName(TEXT("Traits"));
		if (!TraitsProp)
		{
			Session.Log(TEXT("[FAIL] mass_remove_trait -> Could not find Traits property"));
			return false;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Remove Mass Trait")));
		ConfigAsset->Modify();

		FArrayProperty* ArrayProp = CastField<FArrayProperty>(TraitsProp);
		if (ArrayProp)
		{
			void* TraitsPtr = TraitsProp->ContainerPtrToValuePtr<void>(&Config);
			FScriptArrayHelper ArrayHelper(ArrayProp, TraitsPtr);
			ArrayHelper.RemoveValues(FoundIdx, 1);
		}

		ConfigAsset->MarkPackageDirty();
		Session.Log(FString(TEXT("[OK] mass_remove_trait -> Removed '")) + ClassName + TEXT("' from '") + Path + TEXT("'"));
		return true;
	});

	// ================================================================
	// mass_list_spawners()
	// ================================================================
	Lua.set_function("mass_list_spawners", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = MassAI_GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] mass_list_spawners -> No editor world"));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;
		for (TActorIterator<AMassSpawner> It(World); It; ++It)
		{
			AMassSpawner* Spawner = *It;
			if (!Spawner) continue;

			sol::table Entry = LuaView.create_table();
			Entry["name"] = TCHAR_TO_UTF8(*Spawner->GetActorNameOrLabel());
			Entry["location"] = MassAI_VectorToTable(LuaView, Spawner->GetActorLocation());
			Entry["count"] = Spawner->GetCount();
			Entry["scale"] = Spawner->GetSpawningCountScale();
			Result[Idx++] = Entry;
		}

		Session.Log(FString(TEXT("[OK] mass_list_spawners -> ")) + FString::FromInt(Idx - 1) + TEXT(" spawners found"));
		return Result;
	});

	// ================================================================
	// mass_get_spawner(name_or_label)
	// ================================================================
	Lua.set_function("mass_get_spawner", [&Session](const std::string& NameStr, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = MassAI_GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] mass_get_spawner -> No editor world"));
			return sol::lua_nil;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AMassSpawner* Spawner = FindMassSpawnerByName(World, Name);
		if (!Spawner)
		{
			Session.Log(FString(TEXT("[FAIL] mass_get_spawner -> Could not find '")) + Name + TEXT("'"));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		Result["name"] = TCHAR_TO_UTF8(*Spawner->GetActorNameOrLabel());
		Result["location"] = MassAI_VectorToTable(LuaView, Spawner->GetActorLocation());
		Result["count"] = Spawner->GetCount();
		Result["scale"] = Spawner->GetSpawningCountScale();

		// Entity types via reflection (EntityTypes is BlueprintReadWrite)
		FProperty* EntityTypesProp = AMassSpawner::StaticClass()->FindPropertyByName(TEXT("EntityTypes"));
		if (EntityTypesProp)
		{
			FArrayProperty* ArrayProp = CastField<FArrayProperty>(EntityTypesProp);
			if (ArrayProp)
			{
				const void* ArrayPtr = EntityTypesProp->ContainerPtrToValuePtr<void>(Spawner);
				FScriptArrayHelper ArrayHelper(ArrayProp, ArrayPtr);

				sol::table TypesTable = LuaView.create_table();
				for (int32 i = 0; i < ArrayHelper.Num(); i++)
				{
					const FMassSpawnedEntityType* EntityType = reinterpret_cast<const FMassSpawnedEntityType*>(ArrayHelper.GetRawPtr(i));
					if (!EntityType) continue;

					sol::table TypeEntry = LuaView.create_table();
					TypeEntry["proportion"] = EntityType->Proportion;

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
					const UMassEntityConfigAsset* Config = EntityType->GetEntityConfig();
					if (Config)
					{
						TypeEntry["config_path"] = TCHAR_TO_UTF8(*Config->GetPathName());
					}
					else
#endif
					{
						FString SoftPath = EntityType->EntityConfig.ToString();
						if (!SoftPath.IsEmpty())
						{
							TypeEntry["config_soft_path"] = TCHAR_TO_UTF8(*SoftPath);
						}
					}

					TypesTable[i + 1] = TypeEntry;
				}
				Result["entity_types"] = TypesTable;
			}
		}

		Session.Log(FString(TEXT("[OK] mass_get_spawner -> '")) + Name + TEXT("'"));
		return Result;
	});

	// ================================================================
	// mass_spawner_spawn(name_or_label)
	// ================================================================
	Lua.set_function("mass_spawner_spawn", [&Session](const std::string& NameStr) -> bool
	{
		UWorld* World = MassAI_GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] mass_spawner_spawn -> No editor world"));
			return false;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AMassSpawner* Spawner = FindMassSpawnerByName(World, Name);
		if (!Spawner)
		{
			Session.Log(FString(TEXT("[FAIL] mass_spawner_spawn -> Could not find '")) + Name + TEXT("'"));
			return false;
		}

		Spawner->DoSpawning();
		Session.Log(FString(TEXT("[OK] mass_spawner_spawn -> Spawning triggered on '")) + Name + TEXT("'"));
		return true;
	});

	// ================================================================
	// mass_spawner_despawn(name_or_label)
	// ================================================================
	Lua.set_function("mass_spawner_despawn", [&Session](const std::string& NameStr) -> bool
	{
		UWorld* World = MassAI_GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] mass_spawner_despawn -> No editor world"));
			return false;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AMassSpawner* Spawner = FindMassSpawnerByName(World, Name);
		if (!Spawner)
		{
			Session.Log(FString(TEXT("[FAIL] mass_spawner_despawn -> Could not find '")) + Name + TEXT("'"));
			return false;
		}

		Spawner->DoDespawning();
		Session.Log(FString(TEXT("[OK] mass_spawner_despawn -> Despawning triggered on '")) + Name + TEXT("'"));
		return true;
	});

	// ================================================================
	// mass_spawner_set_count(name_or_label, count)
	// ================================================================
	Lua.set_function("mass_spawner_set_count", [&Session](const std::string& NameStr, int Count) -> bool
	{
		UWorld* World = MassAI_GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] mass_spawner_set_count -> No editor world"));
			return false;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AMassSpawner* Spawner = FindMassSpawnerByName(World, Name);
		if (!Spawner)
		{
			Session.Log(FString(TEXT("[FAIL] mass_spawner_set_count -> Could not find '")) + Name + TEXT("'"));
			return false;
		}

		// Count is an EditAnywhere property — use reflection
		FProperty* CountProp = AMassSpawner::StaticClass()->FindPropertyByName(TEXT("Count"));
		if (!CountProp)
		{
			Session.Log(TEXT("[FAIL] mass_spawner_set_count -> Could not find Count property"));
			return false;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Set Mass Spawner Count")));
		Spawner->Modify();

		FIntProperty* IntProp = CastField<FIntProperty>(CountProp);
		if (IntProp)
		{
			IntProp->SetPropertyValue_InContainer(Spawner, Count);
		}

		Spawner->MarkPackageDirty();
		Session.Log(FString(TEXT("[OK] mass_spawner_set_count -> Set count to ")) + FString::FromInt(Count) + TEXT(" on '") + Name + TEXT("'"));
		return true;
	});

	// ================================================================
	// mass_spawner_set_scale(name_or_label, scale)
	// ================================================================
	Lua.set_function("mass_spawner_set_scale", [&Session](const std::string& NameStr, float Scale) -> bool
	{
		UWorld* World = MassAI_GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] mass_spawner_set_scale -> No editor world"));
			return false;
		}

		FString Name = UTF8_TO_TCHAR(NameStr.c_str());
		AMassSpawner* Spawner = FindMassSpawnerByName(World, Name);
		if (!Spawner)
		{
			Session.Log(FString(TEXT("[FAIL] mass_spawner_set_scale -> Could not find '")) + Name + TEXT("'"));
			return false;
		}

		Spawner->ScaleSpawningCount(Scale);
		Session.Log(FString(TEXT("[OK] mass_spawner_set_scale -> Set scale to ")) + FString::SanitizeFloat(Scale) + TEXT(" on '") + Name + TEXT("'"));
		return true;
	});

	// ================================================================
	// mass_simulation_pause()
	// ================================================================
	Lua.set_function("mass_simulation_pause", [&Session]() -> bool
	{
		UWorld* World = MassAI_GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] mass_simulation_pause -> No editor world"));
			return false;
		}

		UMassSimulationSubsystem* SimSub = World->GetSubsystem<UMassSimulationSubsystem>();
		if (!SimSub)
		{
			Session.Log(TEXT("[FAIL] mass_simulation_pause -> Mass simulation subsystem not available"));
			return false;
		}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
		SimSub->PauseSimulation();
		Session.Log(TEXT("[OK] mass_simulation_pause -> Simulation paused"));
		return true;
#else
		Session.Log(TEXT("[FAIL] mass_simulation_pause -> requires UE 5.6+"));
		return false;
#endif
	});

	// ================================================================
	// mass_simulation_resume()
	// ================================================================
	Lua.set_function("mass_simulation_resume", [&Session]() -> bool
	{
		UWorld* World = MassAI_GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] mass_simulation_resume -> No editor world"));
			return false;
		}

		UMassSimulationSubsystem* SimSub = World->GetSubsystem<UMassSimulationSubsystem>();
		if (!SimSub)
		{
			Session.Log(TEXT("[FAIL] mass_simulation_resume -> Mass simulation subsystem not available"));
			return false;
		}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
		SimSub->ResumeSimulation();
		Session.Log(TEXT("[OK] mass_simulation_resume -> Simulation resumed"));
		return true;
#else
		Session.Log(TEXT("[FAIL] mass_simulation_resume -> requires UE 5.6+"));
		return false;
#endif
	});

	// ================================================================
	// mass_simulation_status()
	// ================================================================
	Lua.set_function("mass_simulation_status", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = MassAI_GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] mass_simulation_status -> No editor world"));
			return sol::lua_nil;
		}

		UMassSimulationSubsystem* SimSub = World->GetSubsystem<UMassSimulationSubsystem>();

		sol::table Result = LuaView.create_table();
		if (SimSub)
		{
			Result["available"] = true;
			Result["started"] = SimSub->IsSimulationStarted();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			Result["paused"] = SimSub->IsSimulationPaused();
#else
			Result["paused"] = false;
#endif
		}
		else
		{
			Result["available"] = false;
			Result["started"] = false;
			Result["paused"] = false;
		}

		Session.Log(TEXT("[OK] mass_simulation_status"));
		return Result;
	});

	// ================================================================
	// mass_crowd_get_lane_state(data_index, lane_index)
	// ================================================================
	Lua.set_function("mass_crowd_get_lane_state", [&Session](int DataIdx, int LaneIdx, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* World = MassAI_GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] mass_crowd_get_lane_state -> No editor world"));
			return sol::lua_nil;
		}

		UMassCrowdSubsystem* CrowdSub = World->GetSubsystem<UMassCrowdSubsystem>();
		if (!CrowdSub)
		{
			Session.Log(TEXT("[FAIL] mass_crowd_get_lane_state -> MassCrowd subsystem not available"));
			return sol::lua_nil;
		}

		UZoneGraphSubsystem* ZGSub = World->GetSubsystem<UZoneGraphSubsystem>();
		if (!ZGSub)
		{
			Session.Log(TEXT("[FAIL] mass_crowd_get_lane_state -> ZoneGraph subsystem not available"));
			return sol::lua_nil;
		}

		// Build lane handle
		TConstArrayView<FRegisteredZoneGraphData> RegisteredData = ZGSub->GetRegisteredZoneGraphData();
		if (DataIdx < 0 || DataIdx >= RegisteredData.Num() || !RegisteredData[DataIdx].bInUse)
		{
			Session.Log(TEXT("[FAIL] mass_crowd_get_lane_state -> Invalid data index"));
			return sol::lua_nil;
		}

		FZoneGraphDataHandle DataHandle(static_cast<uint16>(DataIdx), static_cast<uint16>(RegisteredData[DataIdx].Generation));
		FZoneGraphLaneHandle LaneHandle(LaneIdx, DataHandle);

		if (!ZGSub->IsLaneValid(LaneHandle))
		{
			Session.Log(TEXT("[FAIL] mass_crowd_get_lane_state -> Invalid lane handle"));
			return sol::lua_nil;
		}

		ECrowdLaneState State = CrowdSub->GetLaneState(LaneHandle);
		std::string StateStr = CrowdLaneStateToString(State);

		Session.Log(FString(TEXT("[OK] mass_crowd_get_lane_state -> ")) + UTF8_TO_TCHAR(StateStr.c_str()));
		return sol::make_object(LuaView, StateStr);
	});

	// ================================================================
	// mass_crowd_set_lane_state(data_index, lane_index, state)
	// ================================================================
	Lua.set_function("mass_crowd_set_lane_state", [&Session](int DataIdx, int LaneIdx, const std::string& StateStr) -> bool
	{
		UWorld* World = MassAI_GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] mass_crowd_set_lane_state -> No editor world"));
			return false;
		}

		UMassCrowdSubsystem* CrowdSub = World->GetSubsystem<UMassCrowdSubsystem>();
		if (!CrowdSub)
		{
			Session.Log(TEXT("[FAIL] mass_crowd_set_lane_state -> MassCrowd subsystem not available"));
			return false;
		}

		UZoneGraphSubsystem* ZGSub = World->GetSubsystem<UZoneGraphSubsystem>();
		if (!ZGSub)
		{
			Session.Log(TEXT("[FAIL] mass_crowd_set_lane_state -> ZoneGraph subsystem not available"));
			return false;
		}

		TConstArrayView<FRegisteredZoneGraphData> RegisteredData = ZGSub->GetRegisteredZoneGraphData();
		if (DataIdx < 0 || DataIdx >= RegisteredData.Num() || !RegisteredData[DataIdx].bInUse)
		{
			Session.Log(TEXT("[FAIL] mass_crowd_set_lane_state -> Invalid data index"));
			return false;
		}

		FZoneGraphDataHandle DataHandle(static_cast<uint16>(DataIdx), static_cast<uint16>(RegisteredData[DataIdx].Generation));
		FZoneGraphLaneHandle LaneHandle(LaneIdx, DataHandle);

		if (!ZGSub->IsLaneValid(LaneHandle))
		{
			Session.Log(TEXT("[FAIL] mass_crowd_set_lane_state -> Invalid lane handle"));
			return false;
		}

		FString State = UTF8_TO_TCHAR(StateStr.c_str());
		ECrowdLaneState NewState;
		if (State.Equals(TEXT("closed"), ESearchCase::IgnoreCase))
		{
			NewState = ECrowdLaneState::Closed;
		}
		else if (State.Equals(TEXT("opened"), ESearchCase::IgnoreCase))
		{
			NewState = ECrowdLaneState::Opened;
		}
		else
		{
			Session.Log(FString(TEXT("[FAIL] mass_crowd_set_lane_state -> Invalid state '")) + State + TEXT("'. Use 'opened' or 'closed'."));
			return false;
		}

		bool bSuccess = CrowdSub->SetLaneState(LaneHandle, NewState);
		if (bSuccess)
		{
			Session.Log(FString(TEXT("[OK] mass_crowd_set_lane_state -> Set lane to '")) + State + TEXT("'"));
		}
		else
		{
			Session.Log(TEXT("[FAIL] mass_crowd_set_lane_state -> SetLaneState returned false"));
		}
		return bSuccess;
	});

	// ================================================================
	// mass_crowd_rebuild_lane_data()
	// ================================================================
	Lua.set_function("mass_crowd_rebuild_lane_data", [&Session]() -> bool
	{
#if WITH_EDITOR
		UWorld* World = MassAI_GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] mass_crowd_rebuild_lane_data -> No editor world"));
			return false;
		}

		UMassCrowdSubsystem* CrowdSub = World->GetSubsystem<UMassCrowdSubsystem>();
		if (!CrowdSub)
		{
			Session.Log(TEXT("[FAIL] mass_crowd_rebuild_lane_data -> MassCrowd subsystem not available"));
			return false;
		}

		CrowdSub->RebuildLaneData();
		Session.Log(TEXT("[OK] mass_crowd_rebuild_lane_data -> Lane data rebuilt"));
		return true;
#else
		Session.Log(TEXT("[FAIL] mass_crowd_rebuild_lane_data -> Only available in editor builds"));
		return false;
#endif
	});
}

REGISTER_LUA_BINDING(MassAI, MassAIDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindMassAI(Lua, Session);
});

