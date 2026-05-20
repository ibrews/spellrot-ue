// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"

#include "PaperTileMap.h"
#include "PaperTileSet.h"
#include "PaperTileLayer.h"
#include "SpriteEditorOnlyTypes.h"
#include "Modules/ModuleManager.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// Paper TileSet + TileMap Enrichment
// ============================================================================

static TArray<FLuaFunctionDoc> PaperTileMapDocs = {};

// Helper: read a FPaperTileInfo into a Lua table
static sol::table TileInfoToTable(sol::state_view& Lua, const FPaperTileInfo& Tile)
{
	sol::table T = Lua.create_table();
	if (Tile.IsValid())
	{
		T["tile_index"] = Tile.GetTileIndex();
		T["tile_set"] = Tile.TileSet ? TCHAR_TO_UTF8(*Tile.TileSet->GetPathName()) : "None";
		T["flip_h"] = Tile.HasFlag(EPaperTileFlags::FlipHorizontal);
		T["flip_v"] = Tile.HasFlag(EPaperTileFlags::FlipVertical);
		T["flip_d"] = Tile.HasFlag(EPaperTileFlags::FlipDiagonal);

		// Include user data if available
		if (Tile.TileSet)
		{
			FName UserData = Tile.TileSet->GetTileUserData(Tile.GetTileIndex());
			if (!UserData.IsNone())
			{
				T["user_data"] = TCHAR_TO_UTF8(*UserData.ToString());
			}
		}
	}
	else
	{
		T["empty"] = true;
	}
	return T;
}

// Helper: build FPaperTileInfo from Lua params table
static FPaperTileInfo TableToTileInfo(sol::table& P)
{
	FPaperTileInfo Tile;

	sol::optional<std::string> TileSetOpt = P.get<sol::optional<std::string>>("tile_set");
	if (TileSetOpt.has_value())
	{
		FString TSPath = UTF8_TO_TCHAR(TileSetOpt.value().c_str());
		if (!TSPath.StartsWith(TEXT("/")))
		{
			TSPath = TEXT("/Game/") + TSPath;
		}
		Tile.TileSet = LoadObject<UPaperTileSet>(nullptr, *TSPath);
	}

	int32 TileIndex = P.get_or("tile_index", 0);
	Tile.PackedTileIndex = TileIndex;

	// Set flags via bitwise OR directly — SetFlagValue() requires IsValid() (non-null TileSet),
	// which would silently drop flags when TileSet hasn't been loaded yet
	if (P.get_or("flip_h", false))
		Tile.PackedTileIndex |= static_cast<int32>(EPaperTileFlags::FlipHorizontal);
	if (P.get_or("flip_v", false))
		Tile.PackedTileIndex |= static_cast<int32>(EPaperTileFlags::FlipVertical);
	if (P.get_or("flip_d", false))
		Tile.PackedTileIndex |= static_cast<int32>(EPaperTileFlags::FlipDiagonal);

	return Tile;
}

static bool PaperTileMap_GetLayerCollides(UPaperTileLayer* Layer)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	return Layer->GetLayerCollides();
#else
	FProperty* Prop = UPaperTileLayer::StaticClass()->FindPropertyByName(TEXT("bLayerCollides"));
	if (Prop)
	{
		bool bVal = false;
		Prop->GetValue_InContainer(Layer, &bVal);
		return bVal;
	}
	return false;
#endif
}

static void BindPaperTileMap(sol::state& Lua, FLuaSessionData& Session)
{
	// ==================================================================
	// _enrich_paper_tile_set
	// ==================================================================
	Lua.set_function("_enrich_paper_tile_set", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UPaperTileSet* TileSet = LoadObject<UPaperTileSet>(nullptr, *FPath);
		if (!TileSet) return;

		AssetObj["_help_text"] =
			"PaperTileSet enrichment:\n"
			"\n"
			"info() -> tile_sheet, tile_size, tile_count, tiles_x, tiles_y, margin, spacing, terrains, additional_textures\n"
			"\n"
			"list(type, opts?):\n"
			"  list(\"tiles\")                          -> per-tile metadata\n"
			"  list(\"terrains\")                       -> terrain definitions\n"
			"  list(\"tile_collision\", {tile_index=N}) -> collision shapes for one tile\n"
			"\n"
			"add(type, params):\n"
			"  add(\"terrain\", {name=\"Water\", center_tile=12})\n"
			"  add(\"tile_collision\", {tile_index=5, type=\"box\", position={x=0,y=0}, size={w=32,h=32}})\n"
			"  add(\"tile_collision\", {tile_index=5, type=\"circle\", position={x=16,y=16}, size={w=8,h=8}})\n"
			"  add(\"tile_collision\", {tile_index=5, type=\"polygon\", vertices={{0,0},{32,0},{32,32},{0,32}}})\n"
			"\n"
			"remove(\"terrain\", index_or_name)                         -> remove terrain\n"
			"remove(\"tile_collision\", {tile_index=5, shape_index=0})  -> remove one shape\n"
			"remove(\"tile_collision\", {tile_index=5, clear=true})     -> remove all shapes\n"
			"\n"
			"configure(type, id, params):\n"
			"  configure(\"tileset\", nil, {tile_size={x=32,y=32}, tile_sheet=\"/Game/Tex\"})\n"
			"  configure(\"tileset\", nil, {margin={left=1,top=1,right=1,bottom=1}, spacing={x=1,y=1}})\n"
			"  configure(\"tile\", 5, {user_data=\"walkable\"})\n"
			"\n"
			"get_tile_uv(tile_index)                                   -> {x, y} texture-space UV\n"
			"tile_xy_from_uv(u, v, round_up?)                         -> {x, y} tile coordinates\n"
			"uses_tile_set(path)                                       -> true if map references tile set\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [TileSet, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TileSet))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();

			UTexture2D* Sheet = TileSet->GetTileSheetTexture();
			Result["tile_sheet"] = Sheet ? TCHAR_TO_UTF8(*Sheet->GetPathName()) : "None";

			FIntPoint TileSz = TileSet->GetTileSize();
			sol::table TS = Lua.create_table();
			TS["x"] = TileSz.X;
			TS["y"] = TileSz.Y;
			Result["tile_size"] = TS;

			Result["tile_count"] = TileSet->GetTileCount();
			Result["tiles_x"] = TileSet->GetTileCountX();
			Result["tiles_y"] = TileSet->GetTileCountY();

			FIntMargin Margin = TileSet->GetMargin();
			sol::table M = Lua.create_table();
			M["left"] = Margin.Left;
			M["top"] = Margin.Top;
			M["right"] = Margin.Right;
			M["bottom"] = Margin.Bottom;
			Result["margin"] = M;

			FIntPoint Spacing = TileSet->GetPerTileSpacing();
			sol::table Sp = Lua.create_table();
			Sp["x"] = Spacing.X;
			Sp["y"] = Spacing.Y;
			Result["spacing"] = Sp;

			FIntPoint Offset = TileSet->GetDrawingOffset();
			sol::table Off = Lua.create_table();
			Off["x"] = Offset.X;
			Off["y"] = Offset.Y;
			Result["drawing_offset"] = Off;

			// Terrains
			sol::table Terrains = Lua.create_table();
			for (int32 i = 0; i < TileSet->GetNumTerrains(); ++i)
			{
				FPaperTileSetTerrain Terrain = TileSet->GetTerrain(i);
				sol::table T = Lua.create_table();
				T["index"] = i;
				T["name"] = TCHAR_TO_UTF8(*Terrain.TerrainName);
				T["center_tile"] = Terrain.CenterTileIndex;
				Terrains[i + 1] = T;
			}
			Result["terrains"] = Terrains;
			Result["terrain_count"] = TileSet->GetNumTerrains();

			// Additional textures
			TArray<UTexture*> AdditionalTextures = TileSet->GetAdditionalTextures();
			if (AdditionalTextures.Num() > 0)
			{
				sol::table AddTex = Lua.create_table();
				for (int32 i = 0; i < AdditionalTextures.Num(); ++i)
				{
					AddTex[i + 1] = AdditionalTextures[i] ? TCHAR_TO_UTF8(*AdditionalTextures[i]->GetPathName()) : "None";
				}
				Result["additional_textures"] = AddTex;
			}

#if WITH_EDITORONLY_DATA
			FLinearColor BgColor = TileSet->GetBackgroundColor();
			sol::table Bg = Lua.create_table();
			Bg["r"] = BgColor.R;
			Bg["g"] = BgColor.G;
			Bg["b"] = BgColor.B;
			Result["background_color"] = Bg;
#endif

			Session.Log(FString::Printf(TEXT("[OK] info() -> TileSet: %dx%d tiles (%dx%d px each)"),
				TileSet->GetTileCountX(), TileSet->GetTileCountY(), TileSz.X, TileSz.Y));
			return Result;
		});

		// ================================================================
		// list(type)
		// ================================================================
		AssetObj.set_function("list", [TileSet, &Session](sol::table Self,
			sol::optional<std::string> TypeOpt, sol::optional<sol::table> OptsOpt,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TileSet))
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

			// ---- list("tiles") ----
			if (FType.Equals(TEXT("tiles"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("tile"), ESearchCase::IgnoreCase))
			{
				int32 Count = TileSet->GetTileCount();
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Count; ++i)
				{
					const FPaperTileMetadata* Meta = TileSet->GetTileMetadata(i);
					if (!Meta) continue;

					sol::table E = Lua.create_table();
					E["index"] = i;
					E["user_data"] = Meta->UserDataName.IsNone() ? "" : TCHAR_TO_UTF8(*Meta->UserDataName.ToString());
					E["has_collision"] = Meta->HasCollision();

					sol::table Terrain = Lua.create_table();
					for (int32 t = 0; t < 4; ++t)
					{
						Terrain[t + 1] = static_cast<int>(Meta->TerrainMembership[t]);
					}
					E["terrain_membership"] = Terrain;

					Result[i + 1] = E;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"tiles\") -> %d"), Count));
				return Result;
			}

			// ---- list("terrains") ----
			if (FType.Equals(TEXT("terrains"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("terrain"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < TileSet->GetNumTerrains(); ++i)
				{
					FPaperTileSetTerrain Terrain = TileSet->GetTerrain(i);
					sol::table E = Lua.create_table();
					E["index"] = i;
					E["name"] = TCHAR_TO_UTF8(*Terrain.TerrainName);
					E["center_tile"] = Terrain.CenterTileIndex;
					Result[i + 1] = E;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"terrains\") -> %d"), TileSet->GetNumTerrains()));
				return Result;
			}

			// ---- list("tile_collision") ----
			if (FType.Equals(TEXT("tile_collision"), ESearchCase::IgnoreCase))
			{
				if (!OptsOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] list(\"tile_collision\") -> {tile_index=N} required"));
					return sol::lua_nil;
				}
				sol::table Opts = OptsOpt.value();
				int32 TileIndex = Opts.get_or("tile_index", -1);
				const FPaperTileMetadata* Meta = TileSet->GetTileMetadata(TileIndex);
				if (!Meta)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] list(\"tile_collision\", tile_index=%d) -> out of range"), TileIndex));
					return sol::lua_nil;
				}

				sol::table Result = Lua.create_table();
				const TArray<FSpriteGeometryShape>& Shapes = Meta->CollisionData.Shapes;
				for (int32 i = 0; i < Shapes.Num(); ++i)
				{
					const FSpriteGeometryShape& Shape = Shapes[i];
					sol::table E = Lua.create_table();
					E["index"] = i;

					switch (Shape.ShapeType)
					{
					case ESpriteShapeType::Box: E["type"] = "box"; break;
					case ESpriteShapeType::Circle: E["type"] = "circle"; break;
					case ESpriteShapeType::Polygon: E["type"] = "polygon"; break;
					}

					sol::table Pos = Lua.create_table();
					Pos["x"] = Shape.BoxPosition.X;
					Pos["y"] = Shape.BoxPosition.Y;
					E["position"] = Pos;

					sol::table Size = Lua.create_table();
					Size["w"] = Shape.BoxSize.X;
					Size["h"] = Shape.BoxSize.Y;
					E["size"] = Size;

					E["rotation"] = Shape.Rotation;
					E["negative_winding"] = Shape.bNegativeWinding;

					if (Shape.ShapeType == ESpriteShapeType::Polygon && Shape.Vertices.Num() > 0)
					{
						sol::table Verts = Lua.create_table();
						for (int32 v = 0; v < Shape.Vertices.Num(); ++v)
						{
							sol::table V = Lua.create_table();
							V[1] = Shape.Vertices[v].X;
							V[2] = Shape.Vertices[v].Y;
							Verts[v + 1] = V;
						}
						E["vertices"] = Verts;
					}

					Result[i + 1] = E;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"tile_collision\", tile_index=%d) -> %d shapes"), TileIndex, Shapes.Num()));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: tiles, terrains, tile_collision"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// add("terrain", params)
		// ================================================================
		AssetObj.set_function("add", [TileSet, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TileSet))
			{
				Session.Log(TEXT("[FAIL] add -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- add("tile_collision", {tile_index=N, type="box", position={x,y}, size={w,h}, ...}) ----
			if (FType.Equals(TEXT("tile_collision"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"tile_collision\") -> params required ({tile_index, type, ...})"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();

				int32 TileIndex = P.get_or("tile_index", -1);
				FPaperTileMetadata* Meta = TileSet->GetMutableTileMetadata(TileIndex);
				if (!Meta)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"tile_collision\", tile_index=%d) -> out of range"), TileIndex));
					return sol::lua_nil;
				}

				FSpriteGeometryShape NewShape;

				FString ShapeType = UTF8_TO_TCHAR(P.get_or<std::string>("type", "box").c_str());
				if (ShapeType.Equals(TEXT("circle"), ESearchCase::IgnoreCase))
					NewShape.ShapeType = ESpriteShapeType::Circle;
				else if (ShapeType.Equals(TEXT("polygon"), ESearchCase::IgnoreCase))
					NewShape.ShapeType = ESpriteShapeType::Polygon;
				else
					NewShape.ShapeType = ESpriteShapeType::Box;

				if (auto PosT = P.get<sol::optional<sol::table>>("position"))
				{
					NewShape.BoxPosition.X = static_cast<float>(PosT.value().get_or("x", 0.0));
					NewShape.BoxPosition.Y = static_cast<float>(PosT.value().get_or("y", 0.0));
				}

				if (auto SzT = P.get<sol::optional<sol::table>>("size"))
				{
					NewShape.BoxSize.X = static_cast<float>(SzT.value().get_or("w", 16.0));
					NewShape.BoxSize.Y = static_cast<float>(SzT.value().get_or("h", 16.0));
				}

				NewShape.Rotation = static_cast<float>(P.get_or("rotation", 0.0));

				// Polygon vertices
				if (NewShape.ShapeType == ESpriteShapeType::Polygon)
				{
					if (auto VertsT = P.get<sol::optional<sol::table>>("vertices"))
					{
						for (auto& KV : VertsT.value())
						{
							if (KV.second.is<sol::table>())
							{
								sol::table V = KV.second.as<sol::table>();
								NewShape.Vertices.Add(FVector2D(
									static_cast<float>(V.get_or(1, 0.0)),
									static_cast<float>(V.get_or(2, 0.0))
								));
							}
						}
					}
				}

				TileSet->Modify();
				Meta->CollisionData.Shapes.Add(NewShape);
				TileSet->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add(\"tile_collision\", tile_index=%d) -> %d shapes"),
					TileIndex, Meta->CollisionData.Shapes.Num()));
				return sol::make_object(Lua, Meta->CollisionData.Shapes.Num());
			}

			if (!FType.Equals(TEXT("terrain"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: terrain, tile_collision"), *FType));
				return sol::lua_nil;
			}

			if (!Params.has_value())
			{
				Session.Log(TEXT("[FAIL] add(\"terrain\") -> params required (name, center_tile?)"));
				return sol::lua_nil;
			}
			sol::table P = Params.value();

			FPaperTileSetTerrain NewTerrain;
			NewTerrain.TerrainName = UTF8_TO_TCHAR(P.get_or<std::string>("name", "NewTerrain").c_str());
			NewTerrain.CenterTileIndex = P.get_or("center_tile", 0);

			TileSet->Modify();
			bool bAdded = TileSet->AddTerrainDescription(NewTerrain);
			TileSet->MarkPackageDirty();

			if (bAdded)
			{
				Session.Log(FString::Printf(TEXT("[OK] add(\"terrain\", \"%s\") -> %d terrains"),
					*NewTerrain.TerrainName, TileSet->GetNumTerrains()));
				return sol::make_object(Lua, true);
			}
			else
			{
				Session.Log(TEXT("[FAIL] add(\"terrain\") -> max terrains reached"));
				return sol::lua_nil;
			}
		});

		// ================================================================
		// remove("tile_collision", {tile_index=N, shape_index=I}) or remove("tile_collision", {tile_index=N, clear=true})
		// ================================================================
		AssetObj.set_function("remove", [TileSet, &Session](sol::table /*self*/,
			const std::string& Type, sol::object IdOrParams,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TileSet))
			{
				Session.Log(TEXT("[FAIL] remove -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- remove("terrain", index_or_name) ----
			if (FType.Equals(TEXT("terrain"), ESearchCase::IgnoreCase))
			{
				// Terrains is private — access via reflection
				FProperty* TerrainsProp = TileSet->GetClass()->FindPropertyByName(TEXT("Terrains"));
				if (!TerrainsProp)
				{
					Session.Log(TEXT("[FAIL] remove(\"terrain\") -> Terrains property not found"));
					return sol::lua_nil;
				}
				FArrayProperty* ArrProp = CastField<FArrayProperty>(TerrainsProp);
				if (!ArrProp)
				{
					Session.Log(TEXT("[FAIL] remove(\"terrain\") -> Terrains is not an array property"));
					return sol::lua_nil;
				}

				TArray<FPaperTileSetTerrain>* TerrainsPtr = ArrProp->ContainerPtrToValuePtr<TArray<FPaperTileSetTerrain>>(TileSet);
				if (!TerrainsPtr || TerrainsPtr->Num() == 0)
				{
					Session.Log(TEXT("[FAIL] remove(\"terrain\") -> no terrains to remove"));
					return sol::lua_nil;
				}

				int32 RemoveIdx = INDEX_NONE;

				if (IdOrParams.is<int>())
				{
					RemoveIdx = IdOrParams.as<int>();
				}
				else if (IdOrParams.is<std::string>())
				{
					FString Name = UTF8_TO_TCHAR(IdOrParams.as<std::string>().c_str());
					for (int32 i = 0; i < TerrainsPtr->Num(); ++i)
					{
						if ((*TerrainsPtr)[i].TerrainName.Equals(Name, ESearchCase::IgnoreCase))
						{
							RemoveIdx = i;
							break;
						}
					}
				}

				if (RemoveIdx < 0 || RemoveIdx >= TerrainsPtr->Num())
				{
					Session.Log(TEXT("[FAIL] remove(\"terrain\") -> terrain not found or index out of range"));
					return sol::lua_nil;
				}

				TileSet->Modify();
				FString RemovedName = (*TerrainsPtr)[RemoveIdx].TerrainName;
				TerrainsPtr->RemoveAt(RemoveIdx);

				// Fix up terrain membership indices in all tile metadata:
				// shift indices above RemoveIdx down by 1, reset entries equal to RemoveIdx to 0xFF
				int32 TileCount = TileSet->GetTileCount();
				for (int32 ti = 0; ti < TileCount; ++ti)
				{
					FPaperTileMetadata* TileMeta = TileSet->GetMutableTileMetadata(ti);
					if (!TileMeta) continue;
					for (int32 c = 0; c < 4; ++c)
					{
						if (TileMeta->TerrainMembership[c] == static_cast<uint8>(RemoveIdx))
						{
							TileMeta->TerrainMembership[c] = 0xFF;
						}
						else if (TileMeta->TerrainMembership[c] != 0xFF && TileMeta->TerrainMembership[c] > static_cast<uint8>(RemoveIdx))
						{
							TileMeta->TerrainMembership[c]--;
						}
					}
				}

				TileSet->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"terrain\", \"%s\") -> %d remaining"),
					*RemovedName, TerrainsPtr->Num()));
				return sol::make_object(Lua, true);
			}

			// ---- remove("tile_collision", {...}) ----
			if (!FType.Equals(TEXT("tile_collision"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: terrain, tile_collision"), *FType));
				return sol::lua_nil;
			}

			if (!IdOrParams.is<sol::table>())
			{
				Session.Log(TEXT("[FAIL] remove(\"tile_collision\") -> params table required ({tile_index, shape_index or clear})"));
				return sol::lua_nil;
			}

			sol::table P = IdOrParams.as<sol::table>();
			int32 TileIndex = P.get_or("tile_index", -1);
			FPaperTileMetadata* Meta = TileSet->GetMutableTileMetadata(TileIndex);
			if (!Meta)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"tile_collision\", tile_index=%d) -> out of range"), TileIndex));
				return sol::lua_nil;
			}

			TileSet->Modify();

			bool bClear = P.get_or("clear", false);
			if (bClear)
			{
				int32 Removed = Meta->CollisionData.Shapes.Num();
				Meta->CollisionData.Shapes.Empty();
				TileSet->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"tile_collision\", tile_index=%d, clear) -> %d shapes removed"), TileIndex, Removed));
				return sol::make_object(Lua, true);
			}

			int32 ShapeIndex = P.get_or("shape_index", -1);
			if (ShapeIndex < 0 || ShapeIndex >= Meta->CollisionData.Shapes.Num())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"tile_collision\") -> shape_index %d out of range (count=%d)"),
					ShapeIndex, Meta->CollisionData.Shapes.Num()));
				return sol::lua_nil;
			}

			Meta->CollisionData.Shapes.RemoveAt(ShapeIndex);
			TileSet->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] remove(\"tile_collision\", tile_index=%d, shape=%d) -> %d remaining"),
				TileIndex, ShapeIndex, Meta->CollisionData.Shapes.Num()));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// configure(type, id, params)
		// ================================================================
		AssetObj.set_function("configure", [TileSet, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::table Params,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TileSet))
			{
				Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- configure("tileset", nil, {...}) ----
			if (FType.Equals(TEXT("tileset"), ESearchCase::IgnoreCase))
			{
				TileSet->Modify();

				sol::optional<sol::table> TileSizeOpt = Params.get<sol::optional<sol::table>>("tile_size");
				if (TileSizeOpt.has_value())
				{
					sol::table TS = TileSizeOpt.value();
					TileSet->SetTileSize(FIntPoint(
						TS.get_or("x", TileSet->GetTileSize().X),
						TS.get_or("y", TileSet->GetTileSize().Y)
					));
				}

				sol::optional<std::string> SheetOpt = Params.get<sol::optional<std::string>>("tile_sheet");
				if (SheetOpt.has_value())
				{
					FString SheetPath = UTF8_TO_TCHAR(SheetOpt.value().c_str());
					if (!SheetPath.StartsWith(TEXT("/")))
					{
						SheetPath = TEXT("/Game/") + SheetPath;
					}
					UTexture2D* Tex = LoadObject<UTexture2D>(nullptr, *SheetPath);
					if (Tex)
					{
						TileSet->SetTileSheetTexture(Tex);
					}
				}

				sol::optional<sol::table> MarginOpt = Params.get<sol::optional<sol::table>>("margin");
				if (MarginOpt.has_value())
				{
					sol::table M = MarginOpt.value();
					FIntMargin NewMargin(
						M.get_or("left", 0),
						M.get_or("top", 0),
						M.get_or("right", 0),
						M.get_or("bottom", 0)
					);
					TileSet->SetMargin(NewMargin);
				}

				sol::optional<sol::table> SpacingOpt = Params.get<sol::optional<sol::table>>("spacing");
				if (SpacingOpt.has_value())
				{
					sol::table Sp = SpacingOpt.value();
					TileSet->SetPerTileSpacing(FIntPoint(
						Sp.get_or("x", 0),
						Sp.get_or("y", 0)
					));
				}

				sol::optional<sol::table> OffsetOpt = Params.get<sol::optional<sol::table>>("drawing_offset");
				if (OffsetOpt.has_value())
				{
					sol::table Off = OffsetOpt.value();
					TileSet->SetDrawingOffset(FIntPoint(
						Off.get_or("x", 0),
						Off.get_or("y", 0)
					));
				}

				TileSet->MarkPackageDirty();

				// Trigger PostEditChangeProperty for recalculation
				FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
				TileSet->PostEditChangeProperty(Event);

				Session.Log(TEXT("[OK] configure(\"tileset\")"));
				return sol::make_object(Lua, true);
			}

			// ---- configure("tile", index, {...}) ----
			if (FType.Equals(TEXT("tile"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<int>())
				{
					Session.Log(TEXT("[FAIL] configure(\"tile\") -> tile index required"));
					return sol::lua_nil;
				}

				int32 TileIndex = Id.as<int>();
				FPaperTileMetadata* Meta = TileSet->GetMutableTileMetadata(TileIndex);
				if (!Meta)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"tile\", %d) -> out of range (count=%d)"),
						TileIndex, TileSet->GetTileCount()));
					return sol::lua_nil;
				}

				TileSet->Modify();

				sol::optional<std::string> UserDataOpt = Params.get<sol::optional<std::string>>("user_data");
				if (UserDataOpt.has_value())
				{
					Meta->UserDataName = FName(UTF8_TO_TCHAR(UserDataOpt.value().c_str()));
				}

				sol::optional<sol::table> TerrainOpt = Params.get<sol::optional<sol::table>>("terrain_membership");
				if (TerrainOpt.has_value())
				{
					sol::table TM = TerrainOpt.value();
					for (int32 t = 0; t < 4; ++t)
					{
						Meta->TerrainMembership[t] = static_cast<uint8>(TM.get_or(t + 1, 0xFF));
					}
				}

				TileSet->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"tile\", %d)"), TileIndex));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: tileset, tile"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// get_tile_uv(tile_index) -> {x, y} texture-space UV
		// ================================================================
		AssetObj.set_function("get_tile_uv", [TileSet, &Session](sol::table /*self*/,
			int TileIndex, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TileSet))
			{
				Session.Log(TEXT("[FAIL] get_tile_uv -> asset no longer valid"));
				return sol::lua_nil;
			}

			FVector2D UV;
			if (!TileSet->GetTileUV(TileIndex, UV))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] get_tile_uv(%d) -> tile index out of range"), TileIndex));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();
			Result["x"] = UV.X;
			Result["y"] = UV.Y;
			return Result;
		});

		// ================================================================
		// tile_xy_from_uv(u, v, round_up?) -> {x, y} tile coordinates
		// ================================================================
		AssetObj.set_function("tile_xy_from_uv", [TileSet, &Session](sol::table /*self*/,
			double U, double V, sol::optional<bool> RoundUpOpt,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TileSet))
			{
				Session.Log(TEXT("[FAIL] tile_xy_from_uv -> asset no longer valid"));
				return sol::lua_nil;
			}

			bool bRoundUp = RoundUpOpt.value_or(false);
			FIntPoint TileXY = TileSet->GetTileXYFromTextureUV(FVector2D(U, V), bRoundUp);

			sol::table Result = Lua.create_table();
			Result["x"] = TileXY.X;
			Result["y"] = TileXY.Y;
			return Result;
		});
	});

	// ==================================================================
	// _enrich_paper_tile_map
	// ==================================================================
	Lua.set_function("_enrich_paper_tile_map", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UPaperTileMap* TileMap = LoadObject<UPaperTileMap>(nullptr, *FPath);
		if (!TileMap) return;

		AssetObj["_help_text"] =
			"PaperTileMap enrichment:\n"
			"\n"
			"info() -> map dimensions, tile size, projection, layers, collision, separation_per_tile_x/y, hex_side_length\n"
			"\n"
			"list(type, opts?):\n"
			"  list(\"layers\")                       -> all layers with stats\n"
			"  list(\"tiles\", {layer=0})              -> all non-empty tiles in layer\n"
			"  list(\"tiles\", {layer=0, x=0, y=0, width=10, height=10}) -> region\n"
			"\n"
			"add(\"layer\", {name=\"Overlay\", index=N?})       -> add layer\n"
			"remove(\"layer\", index_or_name)                  -> remove layer\n"
			"reorder_layer(from_index, to_index)               -> move layer\n"
			"\n"
			"set_tile(x, y, layer, {tile_set=\"...\", tile_index=N, flip_h?, flip_v?, flip_d?})\n"
			"set_tile(x, y, layer, nil)                       -> clear tile\n"
			"get_tile(x, y, layer)                             -> tile info table\n"
			"fill_tiles({layer=0, tile_set=\"...\", tile_index=N, x?, y?, width?, height?})\n"
			"fill_tiles({layer=0, clear=true})                 -> clear layer\n"
			"\n"
			"resize(width, height, preserve?)                  -> resize map\n"
			"rebuild_collision()                               -> rebuild physics\n"
			"\n"
			"configure(type, id, params):\n"
			"  configure(\"map\", nil, {projection=\"Orthogonal\", tile_width=32, tile_height=32})\n"
			"  configure(\"map\", nil, {separation_per_tile_x=0.5, separation_per_tile_y=0.5})\n"
			"  configure(\"map\", nil, {hex_side_length=16, material=\"/Game/Mat\"})\n"
			"  configure(\"map\", nil, {material=\"None\"})     -> clear material\n"
			"  configure(\"layer\", 0, {name=\"NewName\", visible_game=true, collides=true, color={r,g,b,a}})\n"
			"  configure(\"layer\", 0, {collision_thickness=50})              -> set override\n"
			"  configure(\"layer\", 0, {override_collision_thickness=false})  -> disable override\n"
			"  configure(\"layer\", 0, {collision_offset=10})                 -> set override\n"
			"  configure(\"layer\", 0, {override_collision_offset=false})     -> disable override\n"
			"\n"
			"get_tile_position(x, y, layer?)              -> {x, y, z} local-space top-left corner\n"
			"get_tile_center(x, y, layer?)                -> {x, y, z} local-space center\n"
			"uses_tile_set(path)                          -> true if any layer references tile set\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [TileMap, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TileMap))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();

			sol::table MapSize = Lua.create_table();
			MapSize["width"] = TileMap->MapWidth;
			MapSize["height"] = TileMap->MapHeight;
			Result["map_size"] = MapSize;

			sol::table TileSz = Lua.create_table();
			TileSz["width"] = TileMap->TileWidth;
			TileSz["height"] = TileMap->TileHeight;
			Result["tile_size"] = TileSz;

			Result["pixels_per_unit"] = TileMap->PixelsPerUnrealUnit;
			Result["separation_per_layer"] = TileMap->SeparationPerLayer;
			Result["separation_per_tile_x"] = TileMap->SeparationPerTileX;
			Result["separation_per_tile_y"] = TileMap->SeparationPerTileY;
			Result["hex_side_length"] = TileMap->HexSideLength;

			const char* ProjectionNames[] = { "Orthogonal", "IsometricDiamond", "IsometricStaggered", "HexagonalStaggered" };
			int32 ProjIdx = static_cast<int32>(TileMap->ProjectionMode.GetValue());
			Result["projection"] = (ProjIdx >= 0 && ProjIdx <= 3) ? ProjectionNames[ProjIdx] : "Unknown";

			Result["collision_mode"] = (TileMap->GetSpriteCollisionDomain() == ESpriteCollisionMode::None) ? "None" : "Use3DPhysics";
			Result["collision_thickness"] = TileMap->GetCollisionThickness();

			Result["num_layers"] = TileMap->TileLayers.Num();

			// Layer summary
			sol::table Layers = Lua.create_table();
			for (int32 i = 0; i < TileMap->TileLayers.Num(); ++i)
			{
				UPaperTileLayer* Layer = TileMap->TileLayers[i];
				if (!Layer) continue;

				sol::table L = Lua.create_table();
				L["index"] = i;
				L["name"] = TCHAR_TO_UTF8(*Layer->LayerName.ToString());
				L["width"] = Layer->GetLayerWidth();
				L["height"] = Layer->GetLayerHeight();
				L["visible_game"] = Layer->ShouldRenderInGame();
#if WITH_EDITORONLY_DATA
				L["visible_editor"] = Layer->ShouldRenderInEditor();
#endif
				L["collides"] = PaperTileMap_GetLayerCollides(Layer);
				L["occupied_cells"] = Layer->GetNumOccupiedCells();

				FLinearColor Color = Layer->GetLayerColor();
				sol::table C = Lua.create_table();
				C["r"] = Color.R;
				C["g"] = Color.G;
				C["b"] = Color.B;
				C["a"] = Color.A;
				L["color"] = C;

				// Collision override status (private bitfield — use reflection)
				FBoolProperty* OverThickProp = CastField<FBoolProperty>(Layer->GetClass()->FindPropertyByName(TEXT("bOverrideCollisionThickness")));
				if (OverThickProp)
				{
					L["override_collision_thickness"] = OverThickProp->GetPropertyValue_InContainer(Layer);
				}
				FBoolProperty* OverOffProp = CastField<FBoolProperty>(Layer->GetClass()->FindPropertyByName(TEXT("bOverrideCollisionOffset")));
				if (OverOffProp)
				{
					L["override_collision_offset"] = OverOffProp->GetPropertyValue_InContainer(Layer);
				}

				Layers[i + 1] = L;
			}
			Result["layers"] = Layers;

			UMaterialInterface* Mat = TileMap->Material;
			Result["material"] = Mat ? TCHAR_TO_UTF8(*Mat->GetPathName()) : "None";

			Session.Log(FString::Printf(TEXT("[OK] info() -> TileMap: %dx%d, %d layers"),
				TileMap->MapWidth, TileMap->MapHeight, TileMap->TileLayers.Num()));
			return Result;
		});

		// ================================================================
		// list(type, opts?)
		// ================================================================
		AssetObj.set_function("list", [TileMap, &Session](sol::table Self,
			sol::optional<std::string> TypeOpt, sol::optional<sol::table> OptsOpt,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TileMap))
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

			// ---- list("layers") ----
			if (FType.Equals(TEXT("layers"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("layer"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < TileMap->TileLayers.Num(); ++i)
				{
					UPaperTileLayer* Layer = TileMap->TileLayers[i];
					if (!Layer) continue;

					sol::table L = Lua.create_table();
					L["index"] = i;
					L["name"] = TCHAR_TO_UTF8(*Layer->LayerName.ToString());
					L["width"] = Layer->GetLayerWidth();
					L["height"] = Layer->GetLayerHeight();
					L["visible_game"] = Layer->ShouldRenderInGame();
				L["collides"] = PaperTileMap_GetLayerCollides(Layer);
					L["occupied_cells"] = Layer->GetNumOccupiedCells();
					Result[i + 1] = L;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"layers\") -> %d"), TileMap->TileLayers.Num()));
				return Result;
			}

			// ---- list("tiles", {layer=N, ...}) ----
			if (FType.Equals(TEXT("tiles"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("tile"), ESearchCase::IgnoreCase))
			{
				if (!OptsOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] list(\"tiles\") -> options required ({layer=N})"));
					return sol::lua_nil;
				}
				sol::table Opts = OptsOpt.value();
				int32 LayerIdx = Opts.get_or("layer", 0);

				if (LayerIdx < 0 || LayerIdx >= TileMap->TileLayers.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] list(\"tiles\") -> layer %d out of range"), LayerIdx));
					return sol::lua_nil;
				}

				UPaperTileLayer* Layer = TileMap->TileLayers[LayerIdx];
				if (!Layer) return sol::lua_nil;

				int32 StartX = FMath::Max(0, Opts.get_or("x", 0));
				int32 StartY = FMath::Max(0, Opts.get_or("y", 0));
				int32 Width = Opts.get_or("width", Layer->GetLayerWidth() - StartX);
				int32 Height = Opts.get_or("height", Layer->GetLayerHeight() - StartY);

				// Clamp
				int32 EndX = FMath::Min(StartX + Width, Layer->GetLayerWidth());
				int32 EndY = FMath::Min(StartY + Height, Layer->GetLayerHeight());

				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (int32 Y = StartY; Y < EndY; ++Y)
				{
					for (int32 X = StartX; X < EndX; ++X)
					{
						FPaperTileInfo Tile = Layer->GetCell(X, Y);
						if (!Tile.IsValid()) continue;

						sol::table E = TileInfoToTable(Lua, Tile);
						E["x"] = X;
						E["y"] = Y;
						Result[Idx++] = E;
					}
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"tiles\", layer=%d) -> %d non-empty tiles"), LayerIdx, Idx - 1));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: layers, tiles"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// add("layer", params)
		// ================================================================
		AssetObj.set_function("add", [TileMap, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TileMap))
			{
				Session.Log(TEXT("[FAIL] add -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FType = UTF8_TO_TCHAR(Type.c_str());
			if (!FType.Equals(TEXT("layer"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: layer"), *FType));
				return sol::lua_nil;
			}

			int32 InsertIndex = INDEX_NONE;
			FString LayerName;

			if (Params.has_value())
			{
				sol::table P = Params.value();
				InsertIndex = P.get_or("index", INDEX_NONE);
				LayerName = UTF8_TO_TCHAR(P.get_or<std::string>("name", "").c_str());
			}

			TileMap->Modify();
			UPaperTileLayer* NewLayer = TileMap->AddNewLayer(InsertIndex);

			if (NewLayer && !LayerName.IsEmpty())
			{
				NewLayer->LayerName = FText::FromString(LayerName);
			}

			TileMap->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] add(\"layer\", \"%s\") -> %d layers"),
				*LayerName, TileMap->TileLayers.Num()));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// remove("layer", index_or_name)
		// ================================================================
		AssetObj.set_function("remove", [TileMap, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TileMap))
			{
				Session.Log(TEXT("[FAIL] remove -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FType = UTF8_TO_TCHAR(Type.c_str());
			if (!FType.Equals(TEXT("layer"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: layer"), *FType));
				return sol::lua_nil;
			}

			int32 LayerIdx = INDEX_NONE;

			if (Id.is<int>())
			{
				LayerIdx = Id.as<int>();
			}
			else if (Id.is<std::string>())
			{
				FString Name = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				for (int32 i = 0; i < TileMap->TileLayers.Num(); ++i)
				{
					if (TileMap->TileLayers[i] && TileMap->TileLayers[i]->LayerName.ToString().Equals(Name, ESearchCase::IgnoreCase))
					{
						LayerIdx = i;
						break;
					}
				}
			}

			if (LayerIdx < 0 || LayerIdx >= TileMap->TileLayers.Num())
			{
				Session.Log(TEXT("[FAIL] remove(\"layer\") -> layer not found"));
				return sol::lua_nil;
			}

			TileMap->Modify();
			if (UPaperTileLayer* RemovedLayer = TileMap->TileLayers[LayerIdx])
			{
				RemovedLayer->Modify();
			}
			TileMap->TileLayers.RemoveAt(LayerIdx);
			TileMap->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] remove(\"layer\", %d) -> %d remaining"), LayerIdx, TileMap->TileLayers.Num()));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// reorder_layer(from_index, to_index)
		// ================================================================
		AssetObj.set_function("reorder_layer", [TileMap, &Session](sol::table /*self*/,
			int FromIndex, int ToIndex,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TileMap))
			{
				Session.Log(TEXT("[FAIL] reorder_layer -> asset no longer valid"));
				return sol::lua_nil;
			}

			int32 Count = TileMap->TileLayers.Num();
			if (FromIndex < 0 || FromIndex >= Count)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] reorder_layer -> from_index %d out of range (count=%d)"), FromIndex, Count));
				return sol::lua_nil;
			}
			if (ToIndex < 0 || ToIndex >= Count)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] reorder_layer -> to_index %d out of range (count=%d)"), ToIndex, Count));
				return sol::lua_nil;
			}
			if (FromIndex == ToIndex)
			{
				Session.Log(TEXT("[OK] reorder_layer -> no change needed"));
				return sol::make_object(Lua, true);
			}

			TileMap->Modify();

			UPaperTileLayer* Layer = TileMap->TileLayers[FromIndex];
			TileMap->TileLayers.RemoveAt(FromIndex);
			TileMap->TileLayers.Insert(Layer, ToIndex);

			TileMap->MarkPackageDirty();

			FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
			TileMap->PostEditChangeProperty(Event);

			Session.Log(FString::Printf(TEXT("[OK] reorder_layer(%d -> %d)"), FromIndex, ToIndex));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// set_tile(x, y, layer, params_or_nil)
		// ================================================================
		AssetObj.set_function("set_tile", [TileMap, &Session](sol::table /*self*/,
			int X, int Y, int LayerIdx, sol::object ParamsOrNil,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TileMap))
			{
				Session.Log(TEXT("[FAIL] set_tile -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (LayerIdx < 0 || LayerIdx >= TileMap->TileLayers.Num())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_tile -> layer %d out of range"), LayerIdx));
				return sol::lua_nil;
			}

			UPaperTileLayer* Layer = TileMap->TileLayers[LayerIdx];
			if (!Layer || !Layer->InBounds(X, Y))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_tile(%d, %d, %d) -> out of bounds"), X, Y, LayerIdx));
				return sol::lua_nil;
			}

			TileMap->Modify();

			if (ParamsOrNil.get_type() == sol::type::lua_nil)
			{
				// Clear tile
				FPaperTileInfo Empty;
				Layer->SetCell(X, Y, Empty);
				Session.Log(FString::Printf(TEXT("[OK] set_tile(%d, %d, %d) -> cleared"), X, Y, LayerIdx));
			}
			else if (ParamsOrNil.is<sol::table>())
			{
				sol::table P = ParamsOrNil.as<sol::table>();
				FPaperTileInfo Tile = TableToTileInfo(P);
				Layer->SetCell(X, Y, Tile);
				Session.Log(FString::Printf(TEXT("[OK] set_tile(%d, %d, %d) -> tile_index=%d"), X, Y, LayerIdx, Tile.GetTileIndex()));
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_tile(%d, %d, %d) -> expected table or nil"), X, Y, LayerIdx));
				return sol::lua_nil;
			}

			TileMap->MarkPackageDirty();
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// get_tile(x, y, layer)
		// ================================================================
		AssetObj.set_function("get_tile", [TileMap, &Session](sol::table /*self*/,
			int X, int Y, int LayerIdx,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TileMap))
			{
				Session.Log(TEXT("[FAIL] get_tile -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (LayerIdx < 0 || LayerIdx >= TileMap->TileLayers.Num())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] get_tile -> layer %d out of range"), LayerIdx));
				return sol::lua_nil;
			}

			UPaperTileLayer* Layer = TileMap->TileLayers[LayerIdx];
			if (!Layer || !Layer->InBounds(X, Y))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] get_tile(%d, %d, %d) -> out of bounds"), X, Y, LayerIdx));
				return sol::lua_nil;
			}

			FPaperTileInfo Tile = Layer->GetCell(X, Y);
			sol::table Result = TileInfoToTable(Lua, Tile);
			Result["x"] = X;
			Result["y"] = Y;
			Result["layer"] = LayerIdx;

			return Result;
		});

		// ================================================================
		// fill_tiles({layer, tile_set?, tile_index?, x?, y?, width?, height?, clear?})
		// ================================================================
		AssetObj.set_function("fill_tiles", [TileMap, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TileMap))
			{
				Session.Log(TEXT("[FAIL] fill_tiles -> asset no longer valid"));
				return sol::lua_nil;
			}

			int32 LayerIdx = Params.get_or("layer", 0);
			if (LayerIdx < 0 || LayerIdx >= TileMap->TileLayers.Num())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] fill_tiles -> layer %d out of range"), LayerIdx));
				return sol::lua_nil;
			}

			UPaperTileLayer* Layer = TileMap->TileLayers[LayerIdx];
			if (!Layer) return sol::lua_nil;

			int32 StartX = FMath::Max(0, Params.get_or("x", 0));
			int32 StartY = FMath::Max(0, Params.get_or("y", 0));
			int32 Width = Params.get_or("width", Layer->GetLayerWidth() - StartX);
			int32 Height = Params.get_or("height", Layer->GetLayerHeight() - StartY);

			int32 EndX = FMath::Min(StartX + Width, Layer->GetLayerWidth());
			int32 EndY = FMath::Min(StartY + Height, Layer->GetLayerHeight());

			bool bClear = Params.get_or("clear", false);

			TileMap->Modify();

			int32 Count = 0;
			if (bClear)
			{
				FPaperTileInfo Empty;
				for (int32 Y = StartY; Y < EndY; ++Y)
				{
					for (int32 X = StartX; X < EndX; ++X)
					{
						Layer->SetCell(X, Y, Empty);
						Count++;
					}
				}
				Session.Log(FString::Printf(TEXT("[OK] fill_tiles(layer=%d, clear) -> %d tiles cleared"), LayerIdx, Count));
			}
			else
			{
				FPaperTileInfo Tile = TableToTileInfo(Params);
				if (!Tile.TileSet)
				{
					Session.Log(TEXT("[FAIL] fill_tiles -> tile_set required when not clearing"));
					return sol::lua_nil;
				}

				for (int32 Y = StartY; Y < EndY; ++Y)
				{
					for (int32 X = StartX; X < EndX; ++X)
					{
						Layer->SetCell(X, Y, Tile);
						Count++;
					}
				}
				Session.Log(FString::Printf(TEXT("[OK] fill_tiles(layer=%d, tile=%d) -> %d tiles filled"),
					LayerIdx, Tile.GetTileIndex(), Count));
			}

			TileMap->MarkPackageDirty();
			return sol::make_object(Lua, Count);
		});

		// ================================================================
		// resize(width, height, preserve?)
		// ================================================================
		AssetObj.set_function("resize", [TileMap, &Session](sol::table /*self*/,
			int Width, int Height, sol::optional<bool> PreserveOpt,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TileMap))
			{
				Session.Log(TEXT("[FAIL] resize -> asset no longer valid"));
				return sol::lua_nil;
			}

			bool bForceResize = !PreserveOpt.value_or(true);

			TileMap->Modify();
			TileMap->ResizeMap(Width, Height, bForceResize);
			TileMap->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] resize(%d, %d) -> map is now %dx%d"),
				Width, Height, TileMap->MapWidth, TileMap->MapHeight));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// rebuild_collision()
		// ================================================================
		AssetObj.set_function("rebuild_collision", [TileMap, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TileMap))
			{
				Session.Log(TEXT("[FAIL] rebuild_collision -> asset no longer valid"));
				return sol::lua_nil;
			}

			TileMap->Modify();
			TileMap->RebuildCollision();
			TileMap->MarkPackageDirty();

			Session.Log(TEXT("[OK] rebuild_collision()"));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// configure(type, id, params)
		// ================================================================
		AssetObj.set_function("configure", [TileMap, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::table Params,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TileMap))
			{
				Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- configure("map", nil, {...}) ----
			if (FType.Equals(TEXT("map"), ESearchCase::IgnoreCase))
			{
				TileMap->Modify();

				sol::optional<std::string> ProjOpt = Params.get<sol::optional<std::string>>("projection");
				if (ProjOpt.has_value())
				{
					FString PStr = UTF8_TO_TCHAR(ProjOpt.value().c_str());
					if (PStr.Contains(TEXT("Diamond")))
						TileMap->ProjectionMode = ETileMapProjectionMode::IsometricDiamond;
					else if (PStr.Contains(TEXT("Staggered")) && PStr.Contains(TEXT("Hex")))
						TileMap->ProjectionMode = ETileMapProjectionMode::HexagonalStaggered;
					else if (PStr.Contains(TEXT("Staggered")))
						TileMap->ProjectionMode = ETileMapProjectionMode::IsometricStaggered;
					else
						TileMap->ProjectionMode = ETileMapProjectionMode::Orthogonal;
				}

				sol::optional<int> TWOpt = Params.get<sol::optional<int>>("tile_width");
				if (TWOpt.has_value())
					TileMap->TileWidth = FMath::Max(1, TWOpt.value());

				sol::optional<int> THOpt = Params.get<sol::optional<int>>("tile_height");
				if (THOpt.has_value())
					TileMap->TileHeight = FMath::Max(1, THOpt.value());

				sol::optional<double> PPUOpt = Params.get<sol::optional<double>>("pixels_per_unit");
				if (PPUOpt.has_value())
					TileMap->PixelsPerUnrealUnit = static_cast<float>(PPUOpt.value());

				sol::optional<double> SepLayerOpt = Params.get<sol::optional<double>>("separation_per_layer");
				if (SepLayerOpt.has_value())
					TileMap->SeparationPerLayer = static_cast<float>(SepLayerOpt.value());

				sol::optional<double> SepTileXOpt = Params.get<sol::optional<double>>("separation_per_tile_x");
				if (SepTileXOpt.has_value())
					TileMap->SeparationPerTileX = static_cast<float>(SepTileXOpt.value());

				sol::optional<double> SepTileYOpt = Params.get<sol::optional<double>>("separation_per_tile_y");
				if (SepTileYOpt.has_value())
					TileMap->SeparationPerTileY = static_cast<float>(SepTileYOpt.value());

				sol::optional<int> HexSideOpt = Params.get<sol::optional<int>>("hex_side_length");
				if (HexSideOpt.has_value())
					TileMap->HexSideLength = FMath::Max(0, HexSideOpt.value());

				sol::optional<std::string> MatOpt = Params.get<sol::optional<std::string>>("material");
				if (MatOpt.has_value())
				{
					FString MatPath = UTF8_TO_TCHAR(MatOpt.value().c_str());
					if (MatPath.Equals(TEXT("None"), ESearchCase::IgnoreCase) || MatPath.IsEmpty())
					{
						TileMap->Material = nullptr;
					}
					else
					{
						if (!MatPath.StartsWith(TEXT("/")))
						{
							MatPath = TEXT("/Game/") + MatPath;
						}
						UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MatPath);
						if (Mat)
						{
							TileMap->Material = Mat;
						}
						else
						{
							Session.Log(FString::Printf(TEXT("[WARN] configure(\"map\") -> material not found: %s"), *MatPath));
						}
					}
				}

				sol::optional<std::string> CollOpt = Params.get<sol::optional<std::string>>("collision_mode");
				if (CollOpt.has_value())
				{
					FString CStr = UTF8_TO_TCHAR(CollOpt.value().c_str());
					if (CStr.Contains(TEXT("None")))
						TileMap->SetCollisionDomain(ESpriteCollisionMode::None);
					else
						TileMap->SetCollisionDomain(ESpriteCollisionMode::Use3DPhysics);
				}

				sol::optional<double> CollThickOpt = Params.get<sol::optional<double>>("collision_thickness");
				if (CollThickOpt.has_value())
					TileMap->SetCollisionThickness(static_cast<float>(CollThickOpt.value()));

				TileMap->MarkPackageDirty();

				FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
				TileMap->PostEditChangeProperty(Event);

				Session.Log(TEXT("[OK] configure(\"map\")"));
				return sol::make_object(Lua, true);
			}

			// ---- configure("layer", index, {...}) ----
			if (FType.Equals(TEXT("layer"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<int>())
				{
					Session.Log(TEXT("[FAIL] configure(\"layer\") -> layer index required"));
					return sol::lua_nil;
				}

				int32 LayerIdx = Id.as<int>();
				if (LayerIdx < 0 || LayerIdx >= TileMap->TileLayers.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"layer\", %d) -> out of range"), LayerIdx));
					return sol::lua_nil;
				}

				UPaperTileLayer* Layer = TileMap->TileLayers[LayerIdx];
				if (!Layer) return sol::lua_nil;

				TileMap->Modify();
				Layer->Modify();

				sol::optional<std::string> NameOpt = Params.get<sol::optional<std::string>>("name");
				if (NameOpt.has_value())
				{
					Layer->LayerName = FText::FromString(UTF8_TO_TCHAR(NameOpt.value().c_str()));
				}

				sol::optional<bool> VisGameOpt = Params.get<sol::optional<bool>>("visible_game");
				if (VisGameOpt.has_value())
				{
					// bHiddenInGame is a uint32 bitfield — must use FBoolProperty API, not raw pointer
					FBoolProperty* HidProp = CastField<FBoolProperty>(Layer->GetClass()->FindPropertyByName(TEXT("bHiddenInGame")));
					if (HidProp)
					{
						HidProp->SetPropertyValue_InContainer(Layer, !VisGameOpt.value());
					}
				}

#if WITH_EDITORONLY_DATA
				sol::optional<bool> VisEditorOpt = Params.get<sol::optional<bool>>("visible_editor");
				if (VisEditorOpt.has_value())
				{
					Layer->SetShouldRenderInEditor(VisEditorOpt.value());
				}
#endif

				sol::optional<bool> CollidesOpt = Params.get<sol::optional<bool>>("collides");
				if (CollidesOpt.has_value())
				{
					Layer->SetLayerCollides(CollidesOpt.value());
				}

				sol::optional<sol::table> ColorOpt = Params.get<sol::optional<sol::table>>("color");
				if (ColorOpt.has_value())
				{
					sol::table C = ColorOpt.value();
					Layer->SetLayerColor(FLinearColor(
						static_cast<float>(C.get_or("r", 1.0)),
						static_cast<float>(C.get_or("g", 1.0)),
						static_cast<float>(C.get_or("b", 1.0)),
						static_cast<float>(C.get_or("a", 1.0))
					));
				}

				sol::optional<double> CollThickOpt = Params.get<sol::optional<double>>("collision_thickness");
				if (CollThickOpt.has_value())
				{
					Layer->SetLayerCollisionThickness(true, static_cast<float>(CollThickOpt.value()));
				}

				sol::optional<bool> CollThickOverrideOpt = Params.get<sol::optional<bool>>("override_collision_thickness");
				if (CollThickOverrideOpt.has_value() && !CollThickOverrideOpt.value())
				{
					// Disable the override (reset to map default)
					Layer->SetLayerCollisionThickness(false, 0.0f);
				}

				sol::optional<double> CollOffsetOpt = Params.get<sol::optional<double>>("collision_offset");
				if (CollOffsetOpt.has_value())
				{
					Layer->SetLayerCollisionOffset(true, static_cast<float>(CollOffsetOpt.value()));
				}

				sol::optional<bool> CollOffsetOverrideOpt = Params.get<sol::optional<bool>>("override_collision_offset");
				if (CollOffsetOverrideOpt.has_value() && !CollOffsetOverrideOpt.value())
				{
					// Disable the override (reset to map default)
					Layer->SetLayerCollisionOffset(false, 0.0f);
				}

				TileMap->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"layer\", %d)"), LayerIdx));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: map, layer"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// get_tile_position(x, y, layer?) -> {x, y, z} local-space top-left corner
		// ================================================================
		AssetObj.set_function("get_tile_position", [TileMap, &Session](sol::table /*self*/,
			double TileX, double TileY, sol::optional<int> LayerOpt,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TileMap))
			{
				Session.Log(TEXT("[FAIL] get_tile_position -> asset no longer valid"));
				return sol::lua_nil;
			}

			int32 LayerIdx = LayerOpt.value_or(0);
			FVector Pos = TileMap->GetTilePositionInLocalSpace(static_cast<float>(TileX), static_cast<float>(TileY), LayerIdx);

			sol::table Result = Lua.create_table();
			Result["x"] = Pos.X;
			Result["y"] = Pos.Y;
			Result["z"] = Pos.Z;
			return Result;
		});

		// ================================================================
		// get_tile_center(x, y, layer?) -> {x, y, z} local-space center
		// ================================================================
		AssetObj.set_function("get_tile_center", [TileMap, &Session](sol::table /*self*/,
			double TileX, double TileY, sol::optional<int> LayerOpt,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TileMap))
			{
				Session.Log(TEXT("[FAIL] get_tile_center -> asset no longer valid"));
				return sol::lua_nil;
			}

			int32 LayerIdx = LayerOpt.value_or(0);
			FVector Center = TileMap->GetTileCenterInLocalSpace(static_cast<float>(TileX), static_cast<float>(TileY), LayerIdx);

			sol::table Result = Lua.create_table();
			Result["x"] = Center.X;
			Result["y"] = Center.Y;
			Result["z"] = Center.Z;
			return Result;
		});

		// ================================================================
		// uses_tile_set(path) -> bool
		// ================================================================
		AssetObj.set_function("uses_tile_set", [TileMap, &Session](sol::table /*self*/,
			const std::string& Path, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TileMap))
			{
				Session.Log(TEXT("[FAIL] uses_tile_set -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString TSPath = UTF8_TO_TCHAR(Path.c_str());
			if (!TSPath.StartsWith(TEXT("/")))
			{
				TSPath = TEXT("/Game/") + TSPath;
			}

			UPaperTileSet* TS = LoadObject<UPaperTileSet>(nullptr, *TSPath);
			if (!TS)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] uses_tile_set -> tile set not found: %s"), *TSPath));
				return sol::lua_nil;
			}

			bool bUses = TileMap->UsesTileSet(TS);
			Session.Log(FString::Printf(TEXT("[OK] uses_tile_set(\"%s\") -> %s"), *TSPath, bUses ? TEXT("true") : TEXT("false")));
			return sol::make_object(Lua, bUses);
		});
	});
}

REGISTER_LUA_BINDING(PaperTileMap, PaperTileMapDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("Paper2D")))
	{
		Session.Log(TEXT("[WARN] Paper2D plugin is not loaded. Enable it in Edit > Plugins to use this feature."));
		return;
	}
	BindPaperTileMap(Lua, Session);
});

