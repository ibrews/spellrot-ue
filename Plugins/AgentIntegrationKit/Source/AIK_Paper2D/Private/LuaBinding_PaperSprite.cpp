// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"

#include "PaperSprite.h"
#include "PaperFlipbook.h"
#include "PaperSpriteAtlas.h"
#include "PaperTerrainMaterial.h"
#include "SpriteEditorOnlyTypes.h"
#include "PaperFlipbookHelpers.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Modules/ModuleManager.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// Paper Sprite Enrichment
// ============================================================================

static TArray<FLuaFunctionDoc> PaperSpriteDocs = {};

static void BindPaperSprite(sol::state& Lua, FLuaSessionData& Session)
{
	// ==================================================================
	// _enrich_paper_sprite  (called by open_asset for UPaperSprite)
	// ==================================================================
	Lua.set_function("_enrich_paper_sprite", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UPaperSprite* Sprite = LoadObject<UPaperSprite>(nullptr, *FPath);
		if (!Sprite) return;

		AssetObj["_help_text"] =
			"PaperSprite enrichment:\n"
			"\n"
			"info() -> structured summary (source_texture, source_uv, source_dimension,\n"
			"  pixels_per_unit, pivot_mode, pivot, collision_mode, sockets, materials,\n"
			"  trimming info, atlas_group, render_bounds, additional_textures,\n"
			"  collision/render geometry settings)\n"
			"\n"
			"list(type):\n"
			"  list(\"sockets\")           -> {name, location, rotation, scale}\n"
			"  list(\"collision_shapes\")  -> {type, position, size, rotation, vertices}\n"
			"  list(\"render_shapes\")     -> {type, position, size, rotation, vertices}\n"
			"\n"
			"add(type, params):\n"
			"  add(\"socket\", {name=\"Muzzle\", location={x=10,y=5,z=0}, scale={x=1,y=1,z=1}})\n"
			"  add(\"collision_shape\", {type=\"box\", position={x=0,y=0}, size={w=64,h=64}})  -> add box\n"
			"  add(\"collision_shape\", {type=\"circle\", position={x=32,y=32}, size={w=16,h=16}})\n"
			"  add(\"collision_shape\", {type=\"polygon\", vertices={{0,0},{64,0},{64,64},{0,64}}})\n"
			"\n"
			"remove(type, id):\n"
			"  remove(\"socket\", \"Muzzle\")        -> remove socket by name\n"
			"  remove(\"collision_shape\", 1)       -> remove shape by 1-based index\n"
			"\n"
			"configure(type, id, params):\n"
			"  configure(\"sprite\", nil, {pivot_mode=\"Center_Center\"})  -> change pivot\n"
			"  configure(\"sprite\", nil, {collision_mode=\"Use3DPhysics\"})\n"
			"  configure(\"sprite\", nil, {collision_thickness=10.0})\n"
			"  configure(\"sprite\", nil, {source_uv={x=0,y=0}, source_dimension={x=64,y=64}})\n"
			"  configure(\"sprite\", nil, {source_texture=\"/Game/Textures/T_Sprite\"})\n"
			"  configure(\"sprite\", nil, {default_material=\"/Game/Mat\", alternate_material=\"/Game/Mat2\"})\n"
			"  configure(\"sprite\", nil, {trimmed=true, trim_origin={x=0,y=0}, trim_dimension={x=256,y=256}})\n"
			"  configure(\"sprite\", nil, {rotated=false})\n"
			"  configure(\"collision_geometry\", nil, {geometry_type=\"FullyCustom\", alpha_threshold=0.1,\n"
			"    detail_amount=0.5, simplify_epsilon=2.0, pixels_per_subdivision_x=32})\n"
			"  configure(\"render_geometry\", nil, {geometry_type=\"TightBoundingBox\"})\n"
			"  configure(\"socket\", \"Muzzle\", {location={x=20,y=10,z=0}, scale={x=2,y=2,z=2}})\n"
			"\n"
			"rebuild()                -> rebuild collision + render data\n"
			"rebuild_collision()      -> rebuild collision data only\n"
			"rebuild_render()         -> rebuild render data only\n"
			"extract_region(x,y)      -> auto-detect source region from texture point\n"
			"find_texture_bounds(alpha_threshold?) -> find tightest bounds in source region\n"
			"find_contours(opts?)     -> find contour outlines from texture alpha\n"
			"get_render_bounds()      -> get render bounding box\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [Sprite, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Sprite))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();

#if WITH_EDITOR
			// Source texture
			UTexture2D* SrcTex = Sprite->GetSourceTexture();
			Result["source_texture"] = SrcTex ? TCHAR_TO_UTF8(*SrcTex->GetPathName()) : "None";

			// Source region
			FVector2D SrcUV = Sprite->GetSourceUV();
			sol::table UV = Lua.create_table();
			UV["x"] = SrcUV.X;
			UV["y"] = SrcUV.Y;
			Result["source_uv"] = UV;

			FVector2D SrcDim = Sprite->GetSourceSize();
			sol::table Dim = Lua.create_table();
			Dim["x"] = SrcDim.X;
			Dim["y"] = SrcDim.Y;
			Result["source_dimension"] = Dim;

			// Pivot
			FVector2D CustomPivot;
			ESpritePivotMode::Type PivotMode = Sprite->GetPivotMode(CustomPivot);

			const char* PivotModeNames[] = {
				"Top_Left", "Top_Center", "Top_Right",
				"Center_Left", "Center_Center", "Center_Right",
				"Bottom_Left", "Bottom_Center", "Bottom_Right",
				"Custom"
			};
			Result["pivot_mode"] = (PivotMode >= 0 && PivotMode <= 9) ? PivotModeNames[PivotMode] : "Unknown";

			FVector2D PivotPos = Sprite->GetPivotPosition();
			sol::table Pivot = Lua.create_table();
			Pivot["x"] = PivotPos.X;
			Pivot["y"] = PivotPos.Y;
			Result["pivot"] = Pivot;

			// Collision
			ESpriteCollisionMode::Type CollMode = Sprite->GetSpriteCollisionDomain();
			switch (CollMode)
			{
			case ESpriteCollisionMode::None:         Result["collision_mode"] = "None"; break;
			case ESpriteCollisionMode::Use2DPhysics: Result["collision_mode"] = "Use2DPhysics"; break;
			case ESpriteCollisionMode::Use3DPhysics: Result["collision_mode"] = "Use3DPhysics"; break;
			default:                                 Result["collision_mode"] = "Unknown"; break;
			}
			Result["collision_thickness"] = Sprite->GetCollisionThickness();

			// Trimming / rotation
			Result["is_trimmed"] = Sprite->IsTrimmedInSourceImage();
			Result["is_rotated"] = Sprite->IsRotatedInSourceImage();
			if (Sprite->IsTrimmedInSourceImage())
			{
				FVector2D TrimOrigin = Sprite->GetOriginInSourceImageBeforeTrimming();
				sol::table TO = Lua.create_table();
				TO["x"] = TrimOrigin.X;
				TO["y"] = TrimOrigin.Y;
				Result["trim_origin"] = TO;

				FVector2D TrimDim = Sprite->GetSourceImageDimensionBeforeTrimming();
				sol::table TD = Lua.create_table();
				TD["x"] = TrimDim.X;
				TD["y"] = TrimDim.Y;
				Result["trim_dimension"] = TD;
			}

			// Atlas group
			const UPaperSpriteAtlas* AtlasGrp = Sprite->GetAtlasGroup();
			Result["atlas_group"] = AtlasGrp ? TCHAR_TO_UTF8(*AtlasGrp->GetPathName()) : "None";
#endif

			// Pixels per unit
			Result["pixels_per_unit"] = Sprite->GetPixelsPerUnrealUnit();
			Result["unreal_units_per_pixel"] = Sprite->GetUnrealUnitsPerPixel();

			// Render bounds
			FBoxSphereBounds RenderBounds = Sprite->GetRenderBounds();
			sol::table BoundsT = Lua.create_table();
			sol::table BOrigin = Lua.create_table();
			BOrigin["x"] = RenderBounds.Origin.X;
			BOrigin["y"] = RenderBounds.Origin.Y;
			BOrigin["z"] = RenderBounds.Origin.Z;
			BoundsT["origin"] = BOrigin;
			sol::table BExtent = Lua.create_table();
			BExtent["x"] = RenderBounds.BoxExtent.X;
			BExtent["y"] = RenderBounds.BoxExtent.Y;
			BExtent["z"] = RenderBounds.BoxExtent.Z;
			BoundsT["extent"] = BExtent;
			BoundsT["sphere_radius"] = RenderBounds.SphereRadius;
			Result["render_bounds"] = BoundsT;

			// Materials
			Result["material_count"] = Sprite->GetNumMaterials();
			UMaterialInterface* DefMat = Sprite->GetDefaultMaterial();
			Result["default_material"] = DefMat ? TCHAR_TO_UTF8(*DefMat->GetPathName()) : "None";
			UMaterialInterface* AltMat = Sprite->GetAlternateMaterial();
			Result["alternate_material"] = AltMat ? TCHAR_TO_UTF8(*AltMat->GetPathName()) : "None";

			// Additional source textures
			FProperty* AddTexProp = Sprite->GetClass()->FindPropertyByName(TEXT("AdditionalSourceTextures"));
			if (AddTexProp)
			{
				FArrayProperty* AddTexArr = CastField<FArrayProperty>(AddTexProp);
				if (AddTexArr)
				{
					void* ArrContainer = AddTexArr->ContainerPtrToValuePtr<void>(Sprite);
					FScriptArrayHelper ArrHelper(AddTexArr, ArrContainer);
					sol::table AddTexList = Lua.create_table();
					for (int32 t = 0; t < ArrHelper.Num(); ++t)
					{
						UTexture** TexPtr = reinterpret_cast<UTexture**>(ArrHelper.GetRawPtr(t));
						if (TexPtr && *TexPtr)
							AddTexList[t + 1] = TCHAR_TO_UTF8(*(*TexPtr)->GetPathName());
						else
							AddTexList[t + 1] = "None";
					}
					Result["additional_textures"] = AddTexList;
					Result["additional_texture_count"] = ArrHelper.Num();
				}
			}

			// Sockets
			Result["has_sockets"] = Sprite->HasAnySockets();

			// Count sockets via reflection
			FProperty* SocketsProp = Sprite->GetClass()->FindPropertyByName(TEXT("Sockets"));
			if (SocketsProp)
			{
				FArrayProperty* ArrayProp = CastField<FArrayProperty>(SocketsProp);
				if (ArrayProp)
				{
					void* ArrayContainer = ArrayProp->ContainerPtrToValuePtr<void>(Sprite);
					FScriptArrayHelper Helper(ArrayProp, ArrayContainer);
					Result["socket_count"] = Helper.Num();
				}
			}

			// Geometry collection details via reflection
#if WITH_EDITORONLY_DATA
			auto ReadGeomCollection = [&Lua](FSpriteGeometryCollection* Geom) -> sol::table
			{
				sol::table GT = Lua.create_table();
				const char* GeomTypeNames[] = { "SourceBoundingBox", "TightBoundingBox", "ShrinkWrapped", "FullyCustom", "Diced" };
				int32 GTVal = static_cast<int32>(Geom->GeometryType.GetValue());
				GT["geometry_type"] = (GTVal >= 0 && GTVal <= 4) ? GeomTypeNames[GTVal] : "Unknown";
				GT["shape_count"] = Geom->Shapes.Num();
				GT["alpha_threshold"] = Geom->AlphaThreshold;
				GT["detail_amount"] = Geom->DetailAmount;
				GT["simplify_epsilon"] = Geom->SimplifyEpsilon;
				GT["pixels_per_subdivision_x"] = Geom->PixelsPerSubdivisionX;
				GT["pixels_per_subdivision_y"] = Geom->PixelsPerSubdivisionY;
				GT["avoid_vertex_merging"] = Geom->bAvoidVertexMerging;
				return GT;
			};

			FProperty* CollisionProp = Sprite->GetClass()->FindPropertyByName(TEXT("CollisionGeometry"));
			if (CollisionProp)
			{
				FSpriteGeometryCollection* Geom = CollisionProp->ContainerPtrToValuePtr<FSpriteGeometryCollection>(Sprite);
				if (Geom)
				{
					Result["collision_geometry"] = ReadGeomCollection(Geom);
					Result["collision_shape_count"] = Geom->Shapes.Num();

					const char* GeomTypeNames[] = { "SourceBoundingBox", "TightBoundingBox", "ShrinkWrapped", "FullyCustom", "Diced" };
					int32 GT = static_cast<int32>(Geom->GeometryType.GetValue());
					Result["collision_geometry_type"] = (GT >= 0 && GT <= 4) ? GeomTypeNames[GT] : "Unknown";
				}
			}

			FProperty* RenderProp = Sprite->GetClass()->FindPropertyByName(TEXT("RenderGeometry"));
			if (RenderProp)
			{
				FSpriteGeometryCollection* Geom = RenderProp->ContainerPtrToValuePtr<FSpriteGeometryCollection>(Sprite);
				if (Geom)
				{
					Result["render_geometry"] = ReadGeomCollection(Geom);

					const char* GeomTypeNames[] = { "SourceBoundingBox", "TightBoundingBox", "ShrinkWrapped", "FullyCustom", "Diced" };
					int32 GT = static_cast<int32>(Geom->GeometryType.GetValue());
					Result["render_geometry_type"] = (GT >= 0 && GT <= 4) ? GeomTypeNames[GT] : "Unknown";
				}
			}
#endif

			Session.Log(FString::Printf(TEXT("[OK] info() -> PaperSprite")));
			return Result;
		});

		// ================================================================
		// list(type)
		// ================================================================
		AssetObj.set_function("list", [Sprite, &Session](sol::table Self,
			sol::optional<std::string> TypeOpt, sol::optional<sol::table> /*OptsOpt*/,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Sprite))
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

			// ---- list("sockets") ----
			if (FType.Equals(TEXT("sockets"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("socket"), ESearchCase::IgnoreCase))
			{
				FProperty* SocketsProp = Sprite->GetClass()->FindPropertyByName(TEXT("Sockets"));
				if (!SocketsProp)
				{
					Session.Log(TEXT("[FAIL] list(\"sockets\") -> Sockets property not found"));
					return sol::lua_nil;
				}

				FArrayProperty* ArrayProp = CastField<FArrayProperty>(SocketsProp);
				if (!ArrayProp) return sol::lua_nil;

				void* ArrayContainer = ArrayProp->ContainerPtrToValuePtr<void>(Sprite);
				FScriptArrayHelper Helper(ArrayProp, ArrayContainer);

				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Helper.Num(); ++i)
				{
					FPaperSpriteSocket* Socket = reinterpret_cast<FPaperSpriteSocket*>(Helper.GetRawPtr(i));
					if (!Socket) continue;

					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*Socket->SocketName.ToString());

					FVector Translation = Socket->LocalTransform.GetTranslation();
					sol::table Loc = Lua.create_table();
					Loc["x"] = Translation.X;
					Loc["y"] = Translation.Y;
					Loc["z"] = Translation.Z;
					E["location"] = Loc;

					FRotator Rotation = Socket->LocalTransform.Rotator();
					sol::table Rot = Lua.create_table();
					Rot["pitch"] = Rotation.Pitch;
					Rot["yaw"] = Rotation.Yaw;
					Rot["roll"] = Rotation.Roll;
					E["rotation"] = Rot;

					FVector Scale = Socket->LocalTransform.GetScale3D();
					sol::table Scl = Lua.create_table();
					Scl["x"] = Scale.X;
					Scl["y"] = Scale.Y;
					Scl["z"] = Scale.Z;
					E["scale"] = Scl;

					Result[i + 1] = E;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"sockets\") -> %d"), Helper.Num()));
				return Result;
			}

			// ---- list("collision_shapes") ----
#if WITH_EDITORONLY_DATA
			if (FType.Equals(TEXT("collision_shapes"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("collision_shape"), ESearchCase::IgnoreCase))
			{
				FProperty* CollisionProp = Sprite->GetClass()->FindPropertyByName(TEXT("CollisionGeometry"));
				if (!CollisionProp)
				{
					Session.Log(TEXT("[FAIL] list(\"collision_shapes\") -> property not found"));
					return sol::lua_nil;
				}

				FSpriteGeometryCollection* Geom = CollisionProp->ContainerPtrToValuePtr<FSpriteGeometryCollection>(Sprite);
				if (!Geom) return sol::lua_nil;

				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Geom->Shapes.Num(); ++i)
				{
					const FSpriteGeometryShape& Shape = Geom->Shapes[i];
					sol::table E = Lua.create_table();
					E["index"] = i + 1;

					switch (Shape.ShapeType)
					{
					case ESpriteShapeType::Box:     E["type"] = "box"; break;
					case ESpriteShapeType::Circle:  E["type"] = "circle"; break;
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

					if (Shape.ShapeType == ESpriteShapeType::Polygon && Shape.Vertices.Num() > 0)
					{
						sol::table Verts = Lua.create_table();
						for (int32 v = 0; v < Shape.Vertices.Num(); ++v)
						{
							sol::table Vert = Lua.create_table();
							Vert["x"] = Shape.Vertices[v].X;
							Vert["y"] = Shape.Vertices[v].Y;
							Verts[v + 1] = Vert;
						}
						E["vertices"] = Verts;
					}

					Result[i + 1] = E;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"collision_shapes\") -> %d"), Geom->Shapes.Num()));
				return Result;
			}

			// ---- list("render_shapes") ----
			if (FType.Equals(TEXT("render_shapes"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("render_shape"), ESearchCase::IgnoreCase))
			{
				FProperty* RenderPropL = Sprite->GetClass()->FindPropertyByName(TEXT("RenderGeometry"));
				if (!RenderPropL)
				{
					Session.Log(TEXT("[FAIL] list(\"render_shapes\") -> property not found"));
					return sol::lua_nil;
				}

				FSpriteGeometryCollection* Geom = RenderPropL->ContainerPtrToValuePtr<FSpriteGeometryCollection>(Sprite);
				if (!Geom) return sol::lua_nil;

				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Geom->Shapes.Num(); ++i)
				{
					const FSpriteGeometryShape& Shape = Geom->Shapes[i];
					sol::table E = Lua.create_table();
					E["index"] = i + 1;

					switch (Shape.ShapeType)
					{
					case ESpriteShapeType::Box:     E["type"] = "box"; break;
					case ESpriteShapeType::Circle:  E["type"] = "circle"; break;
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

					if (Shape.ShapeType == ESpriteShapeType::Polygon && Shape.Vertices.Num() > 0)
					{
						sol::table Verts = Lua.create_table();
						for (int32 v = 0; v < Shape.Vertices.Num(); ++v)
						{
							sol::table Vert = Lua.create_table();
							Vert["x"] = Shape.Vertices[v].X;
							Vert["y"] = Shape.Vertices[v].Y;
							Verts[v + 1] = Vert;
						}
						E["vertices"] = Verts;
					}

					Result[i + 1] = E;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"render_shapes\") -> %d"), Geom->Shapes.Num()));
				return Result;
			}
#endif

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: sockets, collision_shapes, render_shapes"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// add(type, params)
		// ================================================================
		AssetObj.set_function("add", [Sprite, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Sprite))
			{
				Session.Log(TEXT("[FAIL] add -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- add("socket", {...}) ----
			if (FType.Equals(TEXT("socket"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"socket\") -> params required (name, location?)"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();

				FName SocketName = FName(UTF8_TO_TCHAR(P.get_or<std::string>("name", "NewSocket").c_str()));

				FTransform Transform = FTransform::Identity;
				sol::optional<sol::table> LocOpt = P.get<sol::optional<sol::table>>("location");
				if (LocOpt.has_value())
				{
					sol::table L = LocOpt.value();
					Transform.SetTranslation(FVector(
						L.get_or("x", 0.0),
						L.get_or("y", 0.0),
						L.get_or("z", 0.0)
					));
				}
				sol::optional<sol::table> RotOpt = P.get<sol::optional<sol::table>>("rotation");
				if (RotOpt.has_value())
				{
					sol::table R = RotOpt.value();
					Transform.SetRotation(FQuat(FRotator(
						R.get_or("pitch", 0.0),
						R.get_or("yaw", 0.0),
						R.get_or("roll", 0.0)
					)));
				}
				sol::optional<sol::table> ScaleOpt = P.get<sol::optional<sol::table>>("scale");
				if (ScaleOpt.has_value())
				{
					sol::table Sc = ScaleOpt.value();
					Transform.SetScale3D(FVector(
						Sc.get_or("x", 1.0),
						Sc.get_or("y", 1.0),
						Sc.get_or("z", 1.0)
					));
				}

				// Access Sockets array via reflection
				FProperty* SocketsProp = Sprite->GetClass()->FindPropertyByName(TEXT("Sockets"));
				if (!SocketsProp)
				{
					Session.Log(TEXT("[FAIL] add(\"socket\") -> Sockets property not found"));
					return sol::lua_nil;
				}
				FArrayProperty* ArrayProp = CastField<FArrayProperty>(SocketsProp);
				if (!ArrayProp) return sol::lua_nil;

				Sprite->Modify();
				Sprite->PreEditChange(SocketsProp);

				void* ArrayContainer = ArrayProp->ContainerPtrToValuePtr<void>(Sprite);
				FScriptArrayHelper Helper(ArrayProp, ArrayContainer);
				int32 NewIdx = Helper.AddValue();

				FPaperSpriteSocket* NewSocket = reinterpret_cast<FPaperSpriteSocket*>(Helper.GetRawPtr(NewIdx));
				NewSocket->SocketName = SocketName;
				NewSocket->LocalTransform = Transform;

				Sprite->MarkPackageDirty();
#if WITH_EDITOR
				Sprite->ValidateSocketNames();
#endif
				FPropertyChangedEvent Event(SocketsProp, EPropertyChangeType::ArrayAdd);
				Sprite->PostEditChangeProperty(Event);

				Session.Log(FString::Printf(TEXT("[OK] add(\"socket\", \"%s\")"), *SocketName.ToString()));
				return sol::make_object(Lua, true);
			}

			// ---- add("collision_shape", {...}) ----
#if WITH_EDITORONLY_DATA
			if (FType.Equals(TEXT("collision_shape"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"collision_shape\") -> params required (type, position?, size?)"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();

				FProperty* CollisionProp = Sprite->GetClass()->FindPropertyByName(TEXT("CollisionGeometry"));
				if (!CollisionProp)
				{
					Session.Log(TEXT("[FAIL] add(\"collision_shape\") -> CollisionGeometry not found"));
					return sol::lua_nil;
				}

				FSpriteGeometryCollection* Geom = CollisionProp->ContainerPtrToValuePtr<FSpriteGeometryCollection>(Sprite);
				if (!Geom) return sol::lua_nil;

				Sprite->Modify();
				Sprite->PreEditChange(CollisionProp);

				// Set to FullyCustom if not already
				Geom->GeometryType = ESpritePolygonMode::FullyCustom;

				FString ShapeType = UTF8_TO_TCHAR(P.get_or<std::string>("type", "box").c_str());
				FSpriteGeometryShape NewShape;

				sol::optional<sol::table> PosOpt = P.get<sol::optional<sol::table>>("position");
				if (PosOpt.has_value())
				{
					sol::table Pos = PosOpt.value();
					NewShape.BoxPosition = FVector2D(
						Pos.get_or("x", 0.0),
						Pos.get_or("y", 0.0)
					);
				}

				sol::optional<sol::table> SizeOpt = P.get<sol::optional<sol::table>>("size");
				if (SizeOpt.has_value())
				{
					sol::table Sz = SizeOpt.value();
					NewShape.BoxSize = FVector2D(
						Sz.get_or("w", 32.0),
						Sz.get_or("h", 32.0)
					);
				}

				NewShape.Rotation = static_cast<float>(P.get_or("rotation", 0.0));

				if (ShapeType.Equals(TEXT("circle"), ESearchCase::IgnoreCase))
				{
					NewShape.ShapeType = ESpriteShapeType::Circle;
				}
				else if (ShapeType.Equals(TEXT("polygon"), ESearchCase::IgnoreCase))
				{
					NewShape.ShapeType = ESpriteShapeType::Polygon;
					sol::optional<sol::table> VertsOpt = P.get<sol::optional<sol::table>>("vertices");
					if (VertsOpt.has_value())
					{
						sol::table Verts = VertsOpt.value();
						for (auto& Pair : Verts)
						{
							if (Pair.second.is<sol::table>())
							{
								sol::table V = Pair.second.as<sol::table>();
								double VX = V.get_or("x", V.get_or(1, 0.0));
								double VY = V.get_or("y", V.get_or(2, 0.0));
								NewShape.Vertices.Add(FVector2D(VX, VY));
							}
						}
					}
				}
				else
				{
					NewShape.ShapeType = ESpriteShapeType::Box;
				}

				Geom->Shapes.Add(NewShape);

				Sprite->MarkPackageDirty();
				FPropertyChangedEvent Event(CollisionProp, EPropertyChangeType::ValueSet);
				Sprite->PostEditChangeProperty(Event);

				Session.Log(FString::Printf(TEXT("[OK] add(\"collision_shape\", type=%s) -> %d shapes total"),
					*ShapeType, Geom->Shapes.Num()));
				return sol::make_object(Lua, true);
			}
#endif

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: socket, collision_shape"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// remove(type, id)
		// ================================================================
		AssetObj.set_function("remove", [Sprite, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Sprite))
			{
				Session.Log(TEXT("[FAIL] remove -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- remove("socket", "Name") ----
			if (FType.Equals(TEXT("socket"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>())
				{
					Session.Log(TEXT("[FAIL] remove(\"socket\") -> socket name required as string"));
					return sol::lua_nil;
				}

				FName SocketName = FName(UTF8_TO_TCHAR(Id.as<std::string>().c_str()));

#if WITH_EDITOR
				Sprite->Modify();
				Sprite->RemoveSocket(SocketName);
				Sprite->ValidateSocketNames();
				Sprite->MarkPackageDirty();
#endif

				Session.Log(FString::Printf(TEXT("[OK] remove(\"socket\", \"%s\")"), *SocketName.ToString()));
				return sol::make_object(Lua, true);
			}

			// ---- remove("collision_shape", index) ----
#if WITH_EDITORONLY_DATA
			if (FType.Equals(TEXT("collision_shape"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<int>())
				{
					Session.Log(TEXT("[FAIL] remove(\"collision_shape\") -> 1-based index required"));
					return sol::lua_nil;
				}

				int32 Index = Id.as<int>() - 1; // Convert to 0-based

				FProperty* CollisionProp = Sprite->GetClass()->FindPropertyByName(TEXT("CollisionGeometry"));
				if (!CollisionProp) return sol::lua_nil;

				FSpriteGeometryCollection* Geom = CollisionProp->ContainerPtrToValuePtr<FSpriteGeometryCollection>(Sprite);
				if (!Geom || Index < 0 || Index >= Geom->Shapes.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"collision_shape\", %d) -> index out of range"), Id.as<int>()));
					return sol::lua_nil;
				}

				Sprite->Modify();
				Sprite->PreEditChange(CollisionProp);

				Geom->Shapes.RemoveAt(Index);

				Sprite->MarkPackageDirty();
				FPropertyChangedEvent Event(CollisionProp, EPropertyChangeType::ArrayRemove);
				Sprite->PostEditChangeProperty(Event);

				Session.Log(FString::Printf(TEXT("[OK] remove(\"collision_shape\", %d) -> %d remaining"), Id.as<int>(), Geom->Shapes.Num()));
				return sol::make_object(Lua, true);
			}
#endif

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: socket, collision_shape"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// configure(type, id, params)
		// ================================================================
		AssetObj.set_function("configure", [Sprite, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::table Params,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Sprite))
			{
				Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- configure("sprite", nil, {...}) ----
			if (FType.Equals(TEXT("sprite"), ESearchCase::IgnoreCase))
			{
#if WITH_EDITOR
				// Pivot mode
				sol::optional<std::string> PivotModeOpt = Params.get<sol::optional<std::string>>("pivot_mode");
				if (PivotModeOpt.has_value())
				{
					FString PMStr = UTF8_TO_TCHAR(PivotModeOpt.value().c_str());
					ESpritePivotMode::Type PM = ESpritePivotMode::Center_Center;

					if (PMStr.Equals(TEXT("Top_Left"))) PM = ESpritePivotMode::Top_Left;
					else if (PMStr.Equals(TEXT("Top_Center"))) PM = ESpritePivotMode::Top_Center;
					else if (PMStr.Equals(TEXT("Top_Right"))) PM = ESpritePivotMode::Top_Right;
					else if (PMStr.Equals(TEXT("Center_Left"))) PM = ESpritePivotMode::Center_Left;
					else if (PMStr.Equals(TEXT("Center_Center"))) PM = ESpritePivotMode::Center_Center;
					else if (PMStr.Equals(TEXT("Center_Right"))) PM = ESpritePivotMode::Center_Right;
					else if (PMStr.Equals(TEXT("Bottom_Left"))) PM = ESpritePivotMode::Bottom_Left;
					else if (PMStr.Equals(TEXT("Bottom_Center"))) PM = ESpritePivotMode::Bottom_Center;
					else if (PMStr.Equals(TEXT("Bottom_Right"))) PM = ESpritePivotMode::Bottom_Right;
					else if (PMStr.Equals(TEXT("Custom"))) PM = ESpritePivotMode::Custom;

					FVector2D CustomPivot = FVector2D::ZeroVector;
					sol::optional<sol::table> PivotOpt = Params.get<sol::optional<sol::table>>("pivot");
					if (PivotOpt.has_value())
					{
						sol::table PV = PivotOpt.value();
						CustomPivot = FVector2D(PV.get_or("x", 0.0), PV.get_or("y", 0.0));
					}

					Sprite->Modify();
					Sprite->SetPivotMode(PM, CustomPivot, true);
					Sprite->MarkPackageDirty();

					Session.Log(FString::Printf(TEXT("[OK] configure(\"sprite\") -> pivot_mode = %s"), *PMStr));
				}

				// Pixels per unit (via reflection since it's a simple property)
				sol::optional<double> PPUOpt = Params.get<sol::optional<double>>("pixels_per_unit");
				if (PPUOpt.has_value())
				{
					FProperty* PPUProp = Sprite->GetClass()->FindPropertyByName(TEXT("PixelsPerUnrealUnit"));
					if (PPUProp)
					{
						Sprite->Modify();
						Sprite->PreEditChange(PPUProp);

						float* PPUPtr = PPUProp->ContainerPtrToValuePtr<float>(Sprite);
						*PPUPtr = static_cast<float>(PPUOpt.value());

						Sprite->MarkPackageDirty();
						FPropertyChangedEvent Event(PPUProp, EPropertyChangeType::ValueSet);
						Sprite->PostEditChangeProperty(Event);

						Session.Log(FString::Printf(TEXT("[OK] configure(\"sprite\") -> pixels_per_unit = %f"), PPUOpt.value()));
					}
				}

				// Collision mode
				sol::optional<std::string> CollModeOpt = Params.get<sol::optional<std::string>>("collision_mode");
				if (CollModeOpt.has_value())
				{
					FString CMStr = UTF8_TO_TCHAR(CollModeOpt.value().c_str());
					ESpriteCollisionMode::Type CM = ESpriteCollisionMode::None;
					bool bValidCollMode = true;

					if (CMStr.Equals(TEXT("None"), ESearchCase::IgnoreCase))
						CM = ESpriteCollisionMode::None;
					else if (CMStr.Equals(TEXT("Use3DPhysics"), ESearchCase::IgnoreCase) || CMStr.Equals(TEXT("3D"), ESearchCase::IgnoreCase))
						CM = ESpriteCollisionMode::Use3DPhysics;
					else if (CMStr.Equals(TEXT("Use2DPhysics"), ESearchCase::IgnoreCase) || CMStr.Equals(TEXT("2D"), ESearchCase::IgnoreCase))
						CM = ESpriteCollisionMode::Use2DPhysics;
					else
					{
						bValidCollMode = false;
						Session.Log(FString::Printf(TEXT("[WARN] configure(\"sprite\") -> unknown collision_mode '%s'. Valid: None, Use3DPhysics, Use2DPhysics. Skipping."), *CMStr));
					}

					FProperty* CollProp = bValidCollMode ? Sprite->GetClass()->FindPropertyByName(TEXT("SpriteCollisionDomain")) : nullptr;
					if (CollProp)
					{
						Sprite->Modify();
						Sprite->PreEditChange(CollProp);

						uint8* ValPtr = CollProp->ContainerPtrToValuePtr<uint8>(Sprite);
						*ValPtr = static_cast<uint8>(CM);

						Sprite->MarkPackageDirty();
						FPropertyChangedEvent Event(CollProp, EPropertyChangeType::ValueSet);
						Sprite->PostEditChangeProperty(Event);

						Session.Log(FString::Printf(TEXT("[OK] configure(\"sprite\") -> collision_mode = %s"), *CMStr));
					}
				}

				// Collision thickness
				sol::optional<double> CTOpt = Params.get<sol::optional<double>>("collision_thickness");
				if (CTOpt.has_value())
				{
					FProperty* CTProp = Sprite->GetClass()->FindPropertyByName(TEXT("CollisionThickness"));
					if (CTProp)
					{
						Sprite->Modify();
						Sprite->PreEditChange(CTProp);

						float* CTPtr = CTProp->ContainerPtrToValuePtr<float>(Sprite);
						*CTPtr = static_cast<float>(CTOpt.value());

						Sprite->MarkPackageDirty();
						FPropertyChangedEvent Event(CTProp, EPropertyChangeType::ValueSet);
						Sprite->PostEditChangeProperty(Event);

						Session.Log(FString::Printf(TEXT("[OK] configure(\"sprite\") -> collision_thickness = %f"), CTOpt.value()));
					}
				}

				// Source UV
				sol::optional<sol::table> SrcUVOpt = Params.get<sol::optional<sol::table>>("source_uv");
				if (SrcUVOpt.has_value())
				{
					FProperty* UVProp = Sprite->GetClass()->FindPropertyByName(TEXT("SourceUV"));
					if (UVProp)
					{
						Sprite->Modify();
						Sprite->PreEditChange(UVProp);

						FVector2D* UVPtr = UVProp->ContainerPtrToValuePtr<FVector2D>(Sprite);
						sol::table UV = SrcUVOpt.value();
						UVPtr->X = UV.get_or("x", UVPtr->X);
						UVPtr->Y = UV.get_or("y", UVPtr->Y);

						Sprite->MarkPackageDirty();
						FPropertyChangedEvent Event(UVProp, EPropertyChangeType::ValueSet);
						Sprite->PostEditChangeProperty(Event);

						Session.Log(FString::Printf(TEXT("[OK] configure(\"sprite\") -> source_uv = (%.0f, %.0f)"), UVPtr->X, UVPtr->Y));
					}
				}

				// Source dimension
				sol::optional<sol::table> SrcDimOpt = Params.get<sol::optional<sol::table>>("source_dimension");
				if (SrcDimOpt.has_value())
				{
					FProperty* DimProp = Sprite->GetClass()->FindPropertyByName(TEXT("SourceDimension"));
					if (DimProp)
					{
						Sprite->Modify();
						Sprite->PreEditChange(DimProp);

						FVector2D* DimPtr = DimProp->ContainerPtrToValuePtr<FVector2D>(Sprite);
						sol::table Dim = SrcDimOpt.value();
						DimPtr->X = Dim.get_or("width", Dim.get_or("x", DimPtr->X));
						DimPtr->Y = Dim.get_or("height", Dim.get_or("y", DimPtr->Y));

						Sprite->MarkPackageDirty();
						FPropertyChangedEvent Event(DimProp, EPropertyChangeType::ValueSet);
						Sprite->PostEditChangeProperty(Event);

						Session.Log(FString::Printf(TEXT("[OK] configure(\"sprite\") -> source_dimension = (%.0f, %.0f)"), DimPtr->X, DimPtr->Y));
					}
				}

				// Source texture
				sol::optional<std::string> SrcTexOpt = Params.get<sol::optional<std::string>>("source_texture");
				if (SrcTexOpt.has_value())
				{
					FProperty* TexProp = Sprite->GetClass()->FindPropertyByName(TEXT("SourceTexture"));
					if (TexProp)
					{
						FString TexPath = UTF8_TO_TCHAR(SrcTexOpt.value().c_str());
						if (!TexPath.StartsWith(TEXT("/")))
						{
							TexPath = TEXT("/Game/") + TexPath;
						}

						UTexture2D* NewTex = LoadObject<UTexture2D>(nullptr, *TexPath);
						if (NewTex)
						{
							Sprite->Modify();
							Sprite->PreEditChange(TexProp);

							TSoftObjectPtr<UTexture2D>* SoftPtr = TexProp->ContainerPtrToValuePtr<TSoftObjectPtr<UTexture2D>>(Sprite);
							*SoftPtr = NewTex;

							Sprite->MarkPackageDirty();
							FPropertyChangedEvent Event(TexProp, EPropertyChangeType::ValueSet);
							Sprite->PostEditChangeProperty(Event);

							Session.Log(FString::Printf(TEXT("[OK] configure(\"sprite\") -> source_texture = %s"), *TexPath));
						}
						else
						{
							Session.Log(FString::Printf(TEXT("[FAIL] configure(\"sprite\") -> source_texture not found: %s"), *TexPath));
						}
					}
				}

				// Default material
				sol::optional<std::string> DefMatOpt = Params.get<sol::optional<std::string>>("default_material");
				if (DefMatOpt.has_value())
				{
					FProperty* MatProp = Sprite->GetClass()->FindPropertyByName(TEXT("DefaultMaterial"));
					if (MatProp)
					{
						FString MatPath = UTF8_TO_TCHAR(DefMatOpt.value().c_str());
						if (!MatPath.StartsWith(TEXT("/")))
						{
							MatPath = TEXT("/Game/") + MatPath;
						}

						UMaterialInterface* NewMat = LoadObject<UMaterialInterface>(nullptr, *MatPath);
						if (NewMat)
						{
							Sprite->Modify();
							Sprite->PreEditChange(MatProp);

							TObjectPtr<UMaterialInterface>* MatPtr = MatProp->ContainerPtrToValuePtr<TObjectPtr<UMaterialInterface>>(Sprite);
							*MatPtr = NewMat;

							Sprite->MarkPackageDirty();
							FPropertyChangedEvent Event(MatProp, EPropertyChangeType::ValueSet);
							Sprite->PostEditChangeProperty(Event);

							Session.Log(FString::Printf(TEXT("[OK] configure(\"sprite\") -> default_material = %s"), *MatPath));
						}
						else
						{
							Session.Log(FString::Printf(TEXT("[FAIL] configure(\"sprite\") -> default_material not found: %s"), *MatPath));
						}
					}
				}

				// Alternate material
				sol::optional<std::string> AltMatOpt = Params.get<sol::optional<std::string>>("alternate_material");
				if (AltMatOpt.has_value())
				{
					FProperty* MatProp = Sprite->GetClass()->FindPropertyByName(TEXT("AlternateMaterial"));
					if (MatProp)
					{
						FString MatPath = UTF8_TO_TCHAR(AltMatOpt.value().c_str());
						if (!MatPath.StartsWith(TEXT("/")))
						{
							MatPath = TEXT("/Game/") + MatPath;
						}

						UMaterialInterface* NewMat = LoadObject<UMaterialInterface>(nullptr, *MatPath);
						if (NewMat)
						{
							Sprite->Modify();
							Sprite->PreEditChange(MatProp);

							TObjectPtr<UMaterialInterface>* MatPtr = MatProp->ContainerPtrToValuePtr<TObjectPtr<UMaterialInterface>>(Sprite);
							*MatPtr = NewMat;

							Sprite->MarkPackageDirty();
							FPropertyChangedEvent Event(MatProp, EPropertyChangeType::ValueSet);
							Sprite->PostEditChangeProperty(Event);

							Session.Log(FString::Printf(TEXT("[OK] configure(\"sprite\") -> alternate_material = %s"), *MatPath));
						}
						else
						{
							Session.Log(FString::Printf(TEXT("[FAIL] configure(\"sprite\") -> alternate_material not found: %s"), *MatPath));
						}
					}
				}

				// Trimming
				sol::optional<bool> TrimmedOpt = Params.get<sol::optional<bool>>("trimmed");
				if (TrimmedOpt.has_value())
				{
					FVector2D TrimOrigin = FVector2D::ZeroVector;
					FVector2D TrimDim = FVector2D::ZeroVector;

					sol::optional<sol::table> TrimOriginOpt = Params.get<sol::optional<sol::table>>("trim_origin");
					if (TrimOriginOpt.has_value())
					{
						sol::table TO = TrimOriginOpt.value();
						TrimOrigin = FVector2D(TO.get_or("x", 0.0), TO.get_or("y", 0.0));
					}

					sol::optional<sol::table> TrimDimOpt = Params.get<sol::optional<sol::table>>("trim_dimension");
					if (TrimDimOpt.has_value())
					{
						sol::table TD = TrimDimOpt.value();
						TrimDim = FVector2D(TD.get_or("x", 0.0), TD.get_or("y", 0.0));
					}

					Sprite->Modify();
					Sprite->SetTrim(TrimmedOpt.value(), TrimOrigin, TrimDim, true);
					Sprite->MarkPackageDirty();

					Session.Log(FString::Printf(TEXT("[OK] configure(\"sprite\") -> trimmed = %s"), TrimmedOpt.value() ? TEXT("true") : TEXT("false")));
				}

				// Rotation in atlas
				sol::optional<bool> RotatedOpt = Params.get<sol::optional<bool>>("rotated");
				if (RotatedOpt.has_value())
				{
					Sprite->Modify();
					Sprite->SetRotated(RotatedOpt.value(), true);
					Sprite->MarkPackageDirty();

					Session.Log(FString::Printf(TEXT("[OK] configure(\"sprite\") -> rotated = %s"), RotatedOpt.value() ? TEXT("true") : TEXT("false")));
				}
#endif

				return sol::make_object(Lua, true);
			}

			// ---- configure("collision_geometry"|"render_geometry", nil, {...}) ----
#if WITH_EDITORONLY_DATA
			if (FType.Equals(TEXT("collision_geometry"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("render_geometry"), ESearchCase::IgnoreCase))
			{
				bool bIsCollision = FType.Equals(TEXT("collision_geometry"), ESearchCase::IgnoreCase);
				FName PropName = bIsCollision ? TEXT("CollisionGeometry") : TEXT("RenderGeometry");

				FProperty* GeomProp = Sprite->GetClass()->FindPropertyByName(PropName);
				if (!GeomProp)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> property not found"), *FType));
					return sol::lua_nil;
				}

				FSpriteGeometryCollection* Geom = GeomProp->ContainerPtrToValuePtr<FSpriteGeometryCollection>(Sprite);
				if (!Geom)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> geometry not accessible"), *FType));
					return sol::lua_nil;
				}

				Sprite->Modify();
				Sprite->PreEditChange(GeomProp);

				// Geometry type
				sol::optional<std::string> GeomTypeOpt = Params.get<sol::optional<std::string>>("geometry_type");
				if (GeomTypeOpt.has_value())
				{
					FString GTStr = UTF8_TO_TCHAR(GeomTypeOpt.value().c_str());
					if (GTStr.Equals(TEXT("SourceBoundingBox"), ESearchCase::IgnoreCase))
						Geom->GeometryType = ESpritePolygonMode::SourceBoundingBox;
					else if (GTStr.Equals(TEXT("TightBoundingBox"), ESearchCase::IgnoreCase))
						Geom->GeometryType = ESpritePolygonMode::TightBoundingBox;
					else if (GTStr.Equals(TEXT("ShrinkWrapped"), ESearchCase::IgnoreCase))
						Geom->GeometryType = ESpritePolygonMode::ShrinkWrapped;
					else if (GTStr.Equals(TEXT("FullyCustom"), ESearchCase::IgnoreCase))
						Geom->GeometryType = ESpritePolygonMode::FullyCustom;
					else if (GTStr.Equals(TEXT("Diced"), ESearchCase::IgnoreCase))
						Geom->GeometryType = ESpritePolygonMode::Diced;
					else
						Session.Log(FString::Printf(TEXT("[WARN] configure(\"%s\") -> unknown geometry_type '%s'"), *FType, *GTStr));
				}

				if (auto V = Params.get<sol::optional<double>>("alpha_threshold")) Geom->AlphaThreshold = static_cast<float>(V.value());
				if (auto V = Params.get<sol::optional<double>>("detail_amount")) Geom->DetailAmount = static_cast<float>(V.value());
				if (auto V = Params.get<sol::optional<double>>("simplify_epsilon")) Geom->SimplifyEpsilon = static_cast<float>(V.value());
				if (auto V = Params.get<sol::optional<int>>("pixels_per_subdivision_x")) Geom->PixelsPerSubdivisionX = V.value();
				if (auto V = Params.get<sol::optional<int>>("pixels_per_subdivision_y")) Geom->PixelsPerSubdivisionY = V.value();
				if (auto V = Params.get<sol::optional<bool>>("avoid_vertex_merging")) Geom->bAvoidVertexMerging = V.value();

				Sprite->MarkPackageDirty();
				FPropertyChangedEvent Event(GeomProp, EPropertyChangeType::ValueSet);
				Sprite->PostEditChangeProperty(Event);

				Session.Log(FString::Printf(TEXT("[OK] configure(\"%s\")"), *FType));
				return sol::make_object(Lua, true);
			}
#endif

			// ---- configure("socket", "Name", {...}) ----
			if (FType.Equals(TEXT("socket"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>())
				{
					Session.Log(TEXT("[FAIL] configure(\"socket\") -> socket name required"));
					return sol::lua_nil;
				}

				FName SocketName = FName(UTF8_TO_TCHAR(Id.as<std::string>().c_str()));
				FPaperSpriteSocket* Socket = Sprite->FindSocket(SocketName);
				if (!Socket)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"socket\", \"%s\") -> not found"), *SocketName.ToString()));
					return sol::lua_nil;
				}

				Sprite->Modify();

				sol::optional<sol::table> LocOpt = Params.get<sol::optional<sol::table>>("location");
				if (LocOpt.has_value())
				{
					sol::table L = LocOpt.value();
					Socket->LocalTransform.SetTranslation(FVector(
						L.get_or("x", Socket->LocalTransform.GetTranslation().X),
						L.get_or("y", Socket->LocalTransform.GetTranslation().Y),
						L.get_or("z", Socket->LocalTransform.GetTranslation().Z)
					));
				}

				sol::optional<sol::table> RotOpt = Params.get<sol::optional<sol::table>>("rotation");
				if (RotOpt.has_value())
				{
					sol::table R = RotOpt.value();
					Socket->LocalTransform.SetRotation(FQuat(FRotator(
						R.get_or("pitch", 0.0),
						R.get_or("yaw", 0.0),
						R.get_or("roll", 0.0)
					)));
				}

				sol::optional<sol::table> ScaleOpt = Params.get<sol::optional<sol::table>>("scale");
				if (ScaleOpt.has_value())
				{
					sol::table Sc = ScaleOpt.value();
					Socket->LocalTransform.SetScale3D(FVector(
						Sc.get_or("x", Socket->LocalTransform.GetScale3D().X),
						Sc.get_or("y", Socket->LocalTransform.GetScale3D().Y),
						Sc.get_or("z", Socket->LocalTransform.GetScale3D().Z)
					));
				}

				sol::optional<std::string> NewNameOpt = Params.get<sol::optional<std::string>>("name");
				if (NewNameOpt.has_value())
				{
					Socket->SocketName = FName(UTF8_TO_TCHAR(NewNameOpt.value().c_str()));
				}

				Sprite->MarkPackageDirty();
#if WITH_EDITOR
				Sprite->ValidateSocketNames();
#endif
				FProperty* SocketsPropCfg = Sprite->GetClass()->FindPropertyByName(TEXT("Sockets"));
				if (SocketsPropCfg)
				{
					FPropertyChangedEvent Event(SocketsPropCfg, EPropertyChangeType::ValueSet);
					Sprite->PostEditChangeProperty(Event);
				}

				Session.Log(FString::Printf(TEXT("[OK] configure(\"socket\", \"%s\")"), *SocketName.ToString()));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: sprite, collision_geometry, render_geometry, socket"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// rebuild()
		// ================================================================
#if WITH_EDITOR
		AssetObj.set_function("rebuild", [Sprite, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Sprite))
			{
				Session.Log(TEXT("[FAIL] rebuild -> asset no longer valid"));
				return sol::lua_nil;
			}

			Sprite->Modify();
			Sprite->RebuildData();
			Sprite->MarkPackageDirty();

			Session.Log(TEXT("[OK] rebuild() -> collision + render data rebuilt"));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// rebuild_collision()
		// ================================================================
		AssetObj.set_function("rebuild_collision", [Sprite, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Sprite))
			{
				Session.Log(TEXT("[FAIL] rebuild_collision -> asset no longer valid"));
				return sol::lua_nil;
			}

			Sprite->Modify();
			Sprite->RebuildCollisionData();
			Sprite->MarkPackageDirty();

			Session.Log(TEXT("[OK] rebuild_collision() -> collision data rebuilt"));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// rebuild_render()
		// ================================================================
		AssetObj.set_function("rebuild_render", [Sprite, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Sprite))
			{
				Session.Log(TEXT("[FAIL] rebuild_render -> asset no longer valid"));
				return sol::lua_nil;
			}

			Sprite->Modify();
			Sprite->RebuildRenderData();
			Sprite->MarkPackageDirty();

			Session.Log(TEXT("[OK] rebuild_render() -> render data rebuilt"));
			return sol::make_object(Lua, true);
		});
#endif

		// ================================================================
		// extract_region(x, y)
		// ================================================================
#if WITH_EDITOR
		AssetObj.set_function("extract_region", [Sprite, &Session](sol::table /*self*/,
			double X, double Y, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Sprite))
			{
				Session.Log(TEXT("[FAIL] extract_region -> asset no longer valid"));
				return sol::lua_nil;
			}

			Sprite->Modify();
			Sprite->ExtractSourceRegionFromTexturePoint(FVector2D(X, Y));
			Sprite->MarkPackageDirty();

			FVector2D SrcUV = Sprite->GetSourceUV();
			FVector2D SrcDim = Sprite->GetSourceSize();
			Session.Log(FString::Printf(TEXT("[OK] extract_region(%.0f, %.0f) -> UV=(%.0f, %.0f) Dim=(%.0f, %.0f)"),
				X, Y, SrcUV.X, SrcUV.Y, SrcDim.X, SrcDim.Y));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// find_texture_bounds(alpha_threshold?)
		// ================================================================
		AssetObj.set_function("find_texture_bounds", [Sprite, &Session](sol::table /*self*/,
			sol::optional<double> AlphaOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Sprite))
			{
				Session.Log(TEXT("[FAIL] find_texture_bounds -> asset no longer valid"));
				return sol::lua_nil;
			}

			float Alpha = static_cast<float>(AlphaOpt.value_or(0.0));
			FVector2D OutPos, OutSize;
			Sprite->FindTextureBoundingBox(Alpha, OutPos, OutSize);

			sol::table Result = Lua.create_table();
			sol::table Pos = Lua.create_table();
			Pos["x"] = OutPos.X;
			Pos["y"] = OutPos.Y;
			Result["position"] = Pos;

			sol::table Size = Lua.create_table();
			Size["width"] = OutSize.X;
			Size["height"] = OutSize.Y;
			Result["size"] = Size;

			Session.Log(FString::Printf(TEXT("[OK] find_texture_bounds(%.2f) -> pos=(%.0f, %.0f) size=(%.0f, %.0f)"),
				Alpha, OutPos.X, OutPos.Y, OutSize.X, OutSize.Y));
			return Result;
		});

		// ================================================================
		// find_contours(opts?)
		// ================================================================
		AssetObj.set_function("find_contours", [Sprite, &Session](sol::table /*self*/,
			sol::optional<sol::table> OptsOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Sprite))
			{
				Session.Log(TEXT("[FAIL] find_contours -> asset no longer valid"));
				return sol::lua_nil;
			}

			UTexture2D* SrcTex = Sprite->GetSourceTexture();
			if (!SrcTex)
			{
				Session.Log(TEXT("[FAIL] find_contours -> no source texture"));
				return sol::lua_nil;
			}

			float AlphaThreshold = 0.0f;
			float Detail = 0.5f;
			if (OptsOpt.has_value())
			{
				sol::table Opts = OptsOpt.value();
				AlphaThreshold = static_cast<float>(Opts.get_or("alpha_threshold", 0.0));
				Detail = static_cast<float>(Opts.get_or("detail", 0.5));
			}

			FVector2D SrcUV = Sprite->GetSourceUV();
			FVector2D SrcDim = Sprite->GetSourceSize();
			FIntPoint ScanPos(FMath::RoundToInt32(SrcUV.X), FMath::RoundToInt32(SrcUV.Y));
			FIntPoint ScanSize(FMath::RoundToInt32(SrcDim.X), FMath::RoundToInt32(SrcDim.Y));

			TArray<TArray<FIntPoint>> Contours;
			UPaperSprite::FindContours(ScanPos, ScanSize, AlphaThreshold, Detail, SrcTex, Contours);

			sol::table Result = Lua.create_table();
			for (int32 c = 0; c < Contours.Num(); ++c)
			{
				sol::table ContourT = Lua.create_table();
				for (int32 p = 0; p < Contours[c].Num(); ++p)
				{
					sol::table Pt = Lua.create_table();
					Pt["x"] = Contours[c][p].X;
					Pt["y"] = Contours[c][p].Y;
					ContourT[p + 1] = Pt;
				}
				Result[c + 1] = ContourT;
			}

			Session.Log(FString::Printf(TEXT("[OK] find_contours() -> %d contours found"), Contours.Num()));
			return Result;
		});

		// ================================================================
		// get_render_bounds()
		// ================================================================
		AssetObj.set_function("get_render_bounds", [Sprite, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Sprite))
			{
				Session.Log(TEXT("[FAIL] get_render_bounds -> asset no longer valid"));
				return sol::lua_nil;
			}

			FBoxSphereBounds Bounds = Sprite->GetRenderBounds();
			sol::table Result = Lua.create_table();

			sol::table Origin = Lua.create_table();
			Origin["x"] = Bounds.Origin.X;
			Origin["y"] = Bounds.Origin.Y;
			Origin["z"] = Bounds.Origin.Z;
			Result["origin"] = Origin;

			sol::table Extent = Lua.create_table();
			Extent["x"] = Bounds.BoxExtent.X;
			Extent["y"] = Bounds.BoxExtent.Y;
			Extent["z"] = Bounds.BoxExtent.Z;
			Result["extent"] = Extent;

			Result["sphere_radius"] = Bounds.SphereRadius;

			Session.Log(TEXT("[OK] get_render_bounds()"));
			return Result;
		});
#endif
	});

	// ==================================================================
	// _enrich_paper_flipbook  (called by open_asset for UPaperFlipbook)
	// ==================================================================
	Lua.set_function("_enrich_paper_flipbook", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UPaperFlipbook* Flipbook = LoadObject<UPaperFlipbook>(nullptr, *FPath);
		if (!Flipbook) return;

		AssetObj["_help_text"] =
			"PaperFlipbook enrichment:\n"
			"\n"
			"info() -> fps, num_key_frames, total_frames, duration, collision_source,\n"
			"  default_material, key_frames (array with sprite + frame_run)\n"
			"\n"
			"list(\"frames\") -> all key frames with sprite path and frame_run\n"
			"\n"
			"add(\"frame\", {sprite=\"/Game/...\", frame_run=3, index=N?})  -> append or insert frame\n"
			"remove(\"frame\", index)  -> remove key frame by 1-based index\n"
			"\n"
			"configure(type, id, params):\n"
			"  configure(\"flipbook\", nil, {fps=24, collision_source=\"FirstFrameCollision\"})\n"
			"  configure(\"frame\", 3, {sprite=\"/Game/...\", frame_run=5})\n"
			"\n"
			"get_sprite_at_time(time, clamp?)  -> sprite path at time in seconds\n"
			"get_sprite_at_frame(frame)        -> sprite path at frame index\n"
			"contains_sprite(path)             -> true if flipbook contains sprite\n"
			"get_render_bounds()               -> bounding box\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [Flipbook, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Flipbook))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();
			Result["fps"] = Flipbook->GetFramesPerSecond();
			Result["num_key_frames"] = Flipbook->GetNumKeyFrames();
			Result["total_frames"] = Flipbook->GetNumFrames();
			Result["duration"] = Flipbook->GetTotalDuration();

			auto CollSrc = Flipbook->GetCollisionSource();
			switch (CollSrc)
			{
			case EFlipbookCollisionMode::NoCollision:         Result["collision_source"] = "NoCollision"; break;
			case EFlipbookCollisionMode::FirstFrameCollision: Result["collision_source"] = "FirstFrameCollision"; break;
			case EFlipbookCollisionMode::EachFrameCollision:  Result["collision_source"] = "EachFrameCollision"; break;
			default: Result["collision_source"] = "Unknown"; break;
			}

			UMaterialInterface* DefMat = Flipbook->GetDefaultMaterial();
			Result["default_material"] = DefMat ? TCHAR_TO_UTF8(*DefMat->GetPathName()) : "None";
			Result["has_sockets"] = Flipbook->HasAnySockets();

			// Render bounds
			FBoxSphereBounds FBounds = Flipbook->GetRenderBounds();
			sol::table FBoundsT = Lua.create_table();
			sol::table FBOrigin = Lua.create_table();
			FBOrigin["x"] = FBounds.Origin.X;
			FBOrigin["y"] = FBounds.Origin.Y;
			FBOrigin["z"] = FBounds.Origin.Z;
			FBoundsT["origin"] = FBOrigin;
			sol::table FBExtent = Lua.create_table();
			FBExtent["x"] = FBounds.BoxExtent.X;
			FBExtent["y"] = FBounds.BoxExtent.Y;
			FBExtent["z"] = FBounds.BoxExtent.Z;
			FBoundsT["extent"] = FBExtent;
			FBoundsT["sphere_radius"] = FBounds.SphereRadius;
			Result["render_bounds"] = FBoundsT;

			// Key frames
			sol::table Frames = Lua.create_table();
			for (int32 i = 0; i < Flipbook->GetNumKeyFrames(); ++i)
			{
				const FPaperFlipbookKeyFrame& KF = Flipbook->GetKeyFrameChecked(i);
				sol::table F = Lua.create_table();
				F["index"] = i + 1;
				F["sprite"] = KF.Sprite ? TCHAR_TO_UTF8(*KF.Sprite->GetPathName()) : "None";
				F["frame_run"] = KF.FrameRun;
				Frames[i + 1] = F;
			}
			Result["key_frames"] = Frames;

			Session.Log(FString::Printf(TEXT("[OK] info() -> %d key frames, %.1f fps, %.2f sec"),
				Flipbook->GetNumKeyFrames(), Flipbook->GetFramesPerSecond(), Flipbook->GetTotalDuration()));
			return Result;
		});

		// ================================================================
		// list("frames")
		// ================================================================
		AssetObj.set_function("list", [Flipbook, &Session](sol::table Self,
			sol::optional<std::string> TypeOpt, sol::optional<sol::table> /*OptsOpt*/,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Flipbook))
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

			if (FType.Equals(TEXT("frames"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("frame"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Flipbook->GetNumKeyFrames(); ++i)
				{
					const FPaperFlipbookKeyFrame& KF = Flipbook->GetKeyFrameChecked(i);
					sol::table F = Lua.create_table();
					F["index"] = i + 1;
					F["sprite"] = KF.Sprite ? TCHAR_TO_UTF8(*KF.Sprite->GetPathName()) : "None";
					F["frame_run"] = KF.FrameRun;
					Result[i + 1] = F;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"frames\") -> %d"), Flipbook->GetNumKeyFrames()));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: frames"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// add("frame", params)
		// ================================================================
		AssetObj.set_function("add", [Flipbook, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Flipbook))
			{
				Session.Log(TEXT("[FAIL] add -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FType = UTF8_TO_TCHAR(Type.c_str());
			if (!FType.Equals(TEXT("frame"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: frame"), *FType));
				return sol::lua_nil;
			}

			if (!Params.has_value())
			{
				Session.Log(TEXT("[FAIL] add(\"frame\") -> params required (sprite, frame_run?)"));
				return sol::lua_nil;
			}
			sol::table P = Params.value();

			FString SpritePath = UTF8_TO_TCHAR(P.get_or<std::string>("sprite", "").c_str());
			int32 FrameRun = P.get_or("frame_run", 1);
			int32 InsertIndex = P.get_or("index", -1); // 1-based, -1 = append

			UPaperSprite* SpriteAsset = nullptr;
			if (!SpritePath.IsEmpty())
			{
				if (!SpritePath.StartsWith(TEXT("/")))
				{
					SpritePath = TEXT("/Game/") + SpritePath;
				}
				SpriteAsset = LoadObject<UPaperSprite>(nullptr, *SpritePath);
				if (!SpriteAsset)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"frame\") -> sprite not found: %s"), *SpritePath));
					return sol::lua_nil;
				}
			}

			FPaperFlipbookKeyFrame NewFrame;
			NewFrame.Sprite = SpriteAsset;
			NewFrame.FrameRun = FMath::Max(1, FrameRun);

			// Use FScopedFlipbookMutator for safe mutation
			{
				FScopedFlipbookMutator Mutator(Flipbook);
				Flipbook->Modify();

				if (InsertIndex > 0 && InsertIndex <= Mutator.KeyFrames.Num())
				{
					Mutator.KeyFrames.Insert(NewFrame, InsertIndex - 1);
				}
				else
				{
					Mutator.KeyFrames.Add(NewFrame);
				}
			}

			Flipbook->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] add(\"frame\", sprite=%s, frame_run=%d) -> %d key frames"),
				*SpritePath, FrameRun, Flipbook->GetNumKeyFrames()));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// remove("frame", index)
		// ================================================================
		AssetObj.set_function("remove", [Flipbook, &Session](sol::table /*self*/,
			const std::string& Type, int Index,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Flipbook))
			{
				Session.Log(TEXT("[FAIL] remove -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FType = UTF8_TO_TCHAR(Type.c_str());
			if (!FType.Equals(TEXT("frame"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: frame"), *FType));
				return sol::lua_nil;
			}

			int32 ZeroIndex = Index - 1;

			if (!Flipbook->IsValidKeyFrameIndex(ZeroIndex))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"frame\", %d) -> index out of range (count=%d)"),
					Index, Flipbook->GetNumKeyFrames()));
				return sol::lua_nil;
			}

			{
				FScopedFlipbookMutator Mutator(Flipbook);
				Flipbook->Modify();
				Mutator.KeyFrames.RemoveAt(ZeroIndex);
			}

			Flipbook->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] remove(\"frame\", %d) -> %d remaining"),
				Index, Flipbook->GetNumKeyFrames()));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// configure(type, id, params)
		// ================================================================
		AssetObj.set_function("configure", [Flipbook, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::table Params,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Flipbook))
			{
				Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- configure("flipbook", nil, {...}) ----
			if (FType.Equals(TEXT("flipbook"), ESearchCase::IgnoreCase))
			{
				// FPS (requires FScopedFlipbookMutator for protected member access)
				sol::optional<double> FpsOpt = Params.get<sol::optional<double>>("fps");
				if (FpsOpt.has_value())
				{
					FScopedFlipbookMutator Mutator(Flipbook);
					Flipbook->Modify();
					Mutator.FramesPerSecond = static_cast<float>(FpsOpt.value());
				}

				// Collision source via reflection
				sol::optional<std::string> CollOpt = Params.get<sol::optional<std::string>>("collision_source");
				if (CollOpt.has_value())
				{
					FProperty* CollProp = Flipbook->GetClass()->FindPropertyByName(TEXT("CollisionSource"));
					if (CollProp)
					{
						FString CollStr = UTF8_TO_TCHAR(CollOpt.value().c_str());
						Flipbook->Modify();
						Flipbook->PreEditChange(CollProp);

						FString ImportStr;
						if (CollStr.Contains(TEXT("No"))) ImportStr = TEXT("NoCollision");
						else if (CollStr.Contains(TEXT("First"))) ImportStr = TEXT("FirstFrameCollision");
						else if (CollStr.Contains(TEXT("Each"))) ImportStr = TEXT("EachFrameCollision");
						else ImportStr = CollStr;

						CollProp->ImportText_InContainer(*ImportStr, Flipbook, Flipbook, PPF_None);

						FPropertyChangedEvent Event(CollProp, EPropertyChangeType::ValueSet);
						Flipbook->PostEditChangeProperty(Event);
					}
				}

				// Default material via reflection
				sol::optional<std::string> MatOpt = Params.get<sol::optional<std::string>>("default_material");
				if (MatOpt.has_value())
				{
					FProperty* MatProp = Flipbook->GetClass()->FindPropertyByName(TEXT("DefaultMaterial"));
					if (MatProp)
					{
						FString MatStr = UTF8_TO_TCHAR(MatOpt.value().c_str());
						Flipbook->Modify();
						Flipbook->PreEditChange(MatProp);
						MatProp->ImportText_InContainer(*MatStr, Flipbook, Flipbook, PPF_None);
						FPropertyChangedEvent Event(MatProp, EPropertyChangeType::ValueSet);
						Flipbook->PostEditChangeProperty(Event);
					}
				}

				Flipbook->MarkPackageDirty();

				Session.Log(TEXT("[OK] configure(\"flipbook\")"));
				return sol::make_object(Lua, true);
			}

			// ---- configure("frame", index, {...}) ----
			if (FType.Equals(TEXT("frame"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<int>())
				{
					Session.Log(TEXT("[FAIL] configure(\"frame\") -> 1-based index required"));
					return sol::lua_nil;
				}

				int32 ZeroIndex = Id.as<int>() - 1;
				if (!Flipbook->IsValidKeyFrameIndex(ZeroIndex))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"frame\", %d) -> index out of range"), Id.as<int>()));
					return sol::lua_nil;
				}

				{
					FScopedFlipbookMutator Mutator(Flipbook);
					Flipbook->Modify();

					FPaperFlipbookKeyFrame& KF = Mutator.KeyFrames[ZeroIndex];

					sol::optional<std::string> SpriteOpt = Params.get<sol::optional<std::string>>("sprite");
					if (SpriteOpt.has_value())
					{
						FString SpritePath = UTF8_TO_TCHAR(SpriteOpt.value().c_str());
						if (!SpritePath.StartsWith(TEXT("/")))
						{
							SpritePath = TEXT("/Game/") + SpritePath;
						}
						UPaperSprite* SpriteAsset = LoadObject<UPaperSprite>(nullptr, *SpritePath);
						if (SpriteAsset)
						{
							KF.Sprite = SpriteAsset;
						}
					}

					sol::optional<int> RunOpt = Params.get<sol::optional<int>>("frame_run");
					if (RunOpt.has_value())
					{
						KF.FrameRun = FMath::Max(1, RunOpt.value());
					}
				}

				Flipbook->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"frame\", %d)"), Id.as<int>()));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: flipbook, frame"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// get_sprite_at_time(time, clamp?)
		// ================================================================
		AssetObj.set_function("get_sprite_at_time", [Flipbook, &Session](sol::table /*self*/,
			double Time, sol::optional<bool> ClampOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Flipbook))
			{
				Session.Log(TEXT("[FAIL] get_sprite_at_time -> asset no longer valid"));
				return sol::lua_nil;
			}

			bool bClamp = ClampOpt.value_or(false);
			UPaperSprite* SpriteAt = Flipbook->GetSpriteAtTime(static_cast<float>(Time), bClamp);
			if (!SpriteAt)
			{
				Session.Log(FString::Printf(TEXT("[OK] get_sprite_at_time(%.3f) -> None"), Time));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();
			Result["sprite"] = TCHAR_TO_UTF8(*SpriteAt->GetPathName());
			Result["key_frame_index"] = Flipbook->GetKeyFrameIndexAtTime(static_cast<float>(Time), bClamp) + 1;

			Session.Log(FString::Printf(TEXT("[OK] get_sprite_at_time(%.3f) -> %s"), Time, *SpriteAt->GetName()));
			return Result;
		});

		// ================================================================
		// get_sprite_at_frame(frame)
		// ================================================================
		AssetObj.set_function("get_sprite_at_frame", [Flipbook, &Session](sol::table /*self*/,
			int Frame, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Flipbook))
			{
				Session.Log(TEXT("[FAIL] get_sprite_at_frame -> asset no longer valid"));
				return sol::lua_nil;
			}

			UPaperSprite* SpriteAt = Flipbook->GetSpriteAtFrame(Frame);
			if (!SpriteAt)
			{
				Session.Log(FString::Printf(TEXT("[OK] get_sprite_at_frame(%d) -> None"), Frame));
				return sol::lua_nil;
			}

			std::string Path = TCHAR_TO_UTF8(*SpriteAt->GetPathName());
			Session.Log(FString::Printf(TEXT("[OK] get_sprite_at_frame(%d) -> %s"), Frame, *SpriteAt->GetName()));
			return sol::make_object(Lua, Path);
		});

		// ================================================================
		// contains_sprite(path)
		// ================================================================
		AssetObj.set_function("contains_sprite", [Flipbook, &Session](sol::table /*self*/,
			const std::string& SpritePath, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Flipbook))
			{
				Session.Log(TEXT("[FAIL] contains_sprite -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FSpritePath = UTF8_TO_TCHAR(SpritePath.c_str());
			if (!FSpritePath.StartsWith(TEXT("/")))
				FSpritePath = TEXT("/Game/") + FSpritePath;

			UPaperSprite* SpriteAsset = LoadObject<UPaperSprite>(nullptr, *FSpritePath);
			if (!SpriteAsset)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] contains_sprite -> sprite not found: %s"), *FSpritePath));
				return sol::make_object(Lua, false);
			}

			bool bContains = Flipbook->ContainsSprite(SpriteAsset);
			Session.Log(FString::Printf(TEXT("[OK] contains_sprite(\"%s\") -> %s"),
				*FSpritePath, bContains ? TEXT("true") : TEXT("false")));
			return sol::make_object(Lua, bContains);
		});

		// ================================================================
		// get_render_bounds()
		// ================================================================
		AssetObj.set_function("get_render_bounds", [Flipbook, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Flipbook))
			{
				Session.Log(TEXT("[FAIL] get_render_bounds -> asset no longer valid"));
				return sol::lua_nil;
			}

			FBoxSphereBounds Bounds = Flipbook->GetRenderBounds();
			sol::table Result = Lua.create_table();

			sol::table Origin = Lua.create_table();
			Origin["x"] = Bounds.Origin.X;
			Origin["y"] = Bounds.Origin.Y;
			Origin["z"] = Bounds.Origin.Z;
			Result["origin"] = Origin;

			sol::table Extent = Lua.create_table();
			Extent["x"] = Bounds.BoxExtent.X;
			Extent["y"] = Bounds.BoxExtent.Y;
			Extent["z"] = Bounds.BoxExtent.Z;
			Result["extent"] = Extent;

			Result["sphere_radius"] = Bounds.SphereRadius;

			Session.Log(TEXT("[OK] get_render_bounds()"));
			return Result;
		});
	});

	// ==================================================================
	// _enrich_paper_sprite_atlas  (UPaperSpriteAtlas - experimental)
	// ==================================================================
	Lua.set_function("_enrich_paper_sprite_atlas", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UPaperSpriteAtlas* Atlas = LoadObject<UPaperSpriteAtlas>(nullptr, *FPath);
		if (!Atlas) return;

		AssetObj["_help_text"] =
			"PaperSpriteAtlas enrichment (experimental):\n"
			"\n"
			"info() -> description, max_width, max_height, padding, slots, textures\n"
			"list(\"slots\") -> sprite placements in atlas\n"
			"list(\"textures\") -> generated atlas textures\n"
			"\n"
			"configure(\"atlas\", nil, {max_width=2048, max_height=2048, padding=2,\n"
			"  padding_type=\"DilateBorder\"|\"PadWithZero\", mip_count=1, rebuild=true})\n";

		// info()
		AssetObj.set_function("info", [Atlas, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Atlas)) { Session.Log(TEXT("[FAIL] info -> invalid")); return sol::lua_nil; }

			sol::table Result = Lua.create_table();
#if WITH_EDITORONLY_DATA
			Result["description"] = TCHAR_TO_UTF8(*Atlas->AtlasDescription);
			Result["max_width"] = Atlas->MaxWidth;
			Result["max_height"] = Atlas->MaxHeight;
			Result["mip_count"] = Atlas->MipCount;
			Result["padding"] = Atlas->Padding;
			Result["padding_type"] = (Atlas->PaddingType == EPaperSpriteAtlasPadding::DilateBorder) ? "DilateBorder" : "PadWithZero";
			Result["slot_count"] = Atlas->AtlasSlots.Num();
			Result["texture_count"] = Atlas->GeneratedTextures.Num();
			Result["built_width"] = Atlas->BuiltWidth;
			Result["built_height"] = Atlas->BuiltHeight;
			Result["incremental_builds"] = Atlas->NumIncrementalBuilds;
			Result["guid"] = TCHAR_TO_UTF8(*Atlas->AtlasGUID.ToString());
#endif
			Session.Log(TEXT("[OK] info() -> PaperSpriteAtlas"));
			return Result;
		});

		// list(type)
		AssetObj.set_function("list", [Atlas, &Session](sol::table Self,
			sol::optional<std::string> TypeOpt, sol::optional<sol::table>,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Atlas)) { Session.Log(TEXT("[FAIL] list -> invalid")); return sol::lua_nil; }
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = Self["info"];
				if (InfoFn.valid()) return InfoFn(Self);
				return sol::lua_nil;
			}

#if WITH_EDITORONLY_DATA
			if (FType.Equals(TEXT("slots"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("slot"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Atlas->AtlasSlots.Num(); ++i)
				{
					const FPaperSpriteAtlasSlot& Slot = Atlas->AtlasSlots[i];
					sol::table E = Lua.create_table();
					E["index"] = i + 1;
					E["sprite"] = Slot.SpriteRef.IsNull() ? "None" : TCHAR_TO_UTF8(*Slot.SpriteRef.ToString());
					E["atlas_index"] = Slot.AtlasIndex;
					E["x"] = Slot.X;
					E["y"] = Slot.Y;
					E["width"] = Slot.Width;
					E["height"] = Slot.Height;
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"slots\") -> %d"), Atlas->AtlasSlots.Num()));
				return Result;
			}

			if (FType.Equals(TEXT("textures"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Atlas->GeneratedTextures.Num(); ++i)
				{
					sol::table E = Lua.create_table();
					E["index"] = i + 1;
					E["path"] = Atlas->GeneratedTextures[i] ? TCHAR_TO_UTF8(*Atlas->GeneratedTextures[i]->GetPathName()) : "None";
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"textures\") -> %d"), Atlas->GeneratedTextures.Num()));
				return Result;
			}
#endif

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: slots, textures"), *FType));
			return sol::lua_nil;
		});

		// configure("atlas", nil, {...})
		AssetObj.set_function("configure", [Atlas, &Session](sol::table /*self*/,
			const std::string& /*Type*/, sol::object /*Id*/, sol::table Params,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Atlas)) { Session.Log(TEXT("[FAIL] configure -> invalid")); return sol::lua_nil; }

#if WITH_EDITORONLY_DATA
			Atlas->Modify();

			if (auto V = Params.get<sol::optional<int>>("max_width")) Atlas->MaxWidth = V.value();
			if (auto V = Params.get<sol::optional<int>>("max_height")) Atlas->MaxHeight = V.value();
			if (auto V = Params.get<sol::optional<int>>("mip_count")) Atlas->MipCount = V.value();
			if (auto V = Params.get<sol::optional<int>>("padding")) Atlas->Padding = V.value();
			if (auto V = Params.get<sol::optional<std::string>>("description")) Atlas->AtlasDescription = UTF8_TO_TCHAR(V.value().c_str());

			if (auto V = Params.get<sol::optional<std::string>>("padding_type"))
			{
				FString PT = UTF8_TO_TCHAR(V.value().c_str());
				Atlas->PaddingType = PT.Contains(TEXT("Zero")) ? EPaperSpriteAtlasPadding::PadWithZero : EPaperSpriteAtlasPadding::DilateBorder;
			}

			if (Params.get_or("rebuild", false))
			{
				Atlas->bRebuildAtlas = true;
			}

			Atlas->MarkPackageDirty();
#endif

			Session.Log(TEXT("[OK] configure(\"atlas\")"));
			return sol::make_object(Lua, true);
		});
	});

	// ==================================================================
	// _enrich_paper_terrain_material  (UPaperTerrainMaterial)
	// ==================================================================
	Lua.set_function("_enrich_paper_terrain_material", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UPaperTerrainMaterial* TerrainMat = LoadObject<UPaperTerrainMaterial>(nullptr, *FPath);
		if (!TerrainMat) return;

		AssetObj["_help_text"] =
			"PaperTerrainMaterial enrichment:\n"
			"\n"
			"info() -> rule_count, interior_fill, rules summary\n"
			"list(\"rules\") -> all terrain rules with caps, body sprites, angles\n"
			"\n"
			"add(\"rule\", {start_cap=\"/Game/...\", end_cap=\"/Game/...\",\n"
			"  body={\"/Game/S1\",\"/Game/S2\"}, min_angle=0, max_angle=360,\n"
			"  collision=true, collision_offset=0, draw_order=0})\n"
			"remove(\"rule\", index) -> 1-based\n"
			"\n"
			"configure(\"material\", nil, {interior_fill=\"/Game/...\"})  -> set fill sprite\n"
			"configure(\"rule\", 1, {min_angle=0, max_angle=180, collision=true})\n";

		// info()
		AssetObj.set_function("info", [TerrainMat, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TerrainMat)) { Session.Log(TEXT("[FAIL] info -> invalid")); return sol::lua_nil; }

			sol::table Result = Lua.create_table();
			Result["rule_count"] = TerrainMat->Rules.Num();
			Result["interior_fill"] = TerrainMat->InteriorFill ? TCHAR_TO_UTF8(*TerrainMat->InteriorFill->GetPathName()) : "None";

			sol::table Rules = Lua.create_table();
			for (int32 i = 0; i < TerrainMat->Rules.Num(); ++i)
			{
				const FPaperTerrainMaterialRule& Rule = TerrainMat->Rules[i];
				sol::table R = Lua.create_table();
				R["index"] = i + 1;
				R["start_cap"] = Rule.StartCap ? TCHAR_TO_UTF8(*Rule.StartCap->GetPathName()) : "None";
				R["end_cap"] = Rule.EndCap ? TCHAR_TO_UTF8(*Rule.EndCap->GetPathName()) : "None";
				R["body_count"] = Rule.Body.Num();
				R["min_angle"] = Rule.MinimumAngle;
				R["max_angle"] = Rule.MaximumAngle;
				R["collision"] = Rule.bEnableCollision;
				R["collision_offset"] = Rule.CollisionOffset;
				R["draw_order"] = Rule.DrawOrder;
				Rules[i + 1] = R;
			}
			Result["rules"] = Rules;

			Session.Log(FString::Printf(TEXT("[OK] info() -> %d rules"), TerrainMat->Rules.Num()));
			return Result;
		});

		// list("rules")
		AssetObj.set_function("list", [TerrainMat, &Session](sol::table Self,
			sol::optional<std::string> TypeOpt, sol::optional<sol::table>,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TerrainMat)) { Session.Log(TEXT("[FAIL] list -> invalid")); return sol::lua_nil; }
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = Self["info"];
				if (InfoFn.valid()) return InfoFn(Self);
				return sol::lua_nil;
			}

			if (FType.Equals(TEXT("rules"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("rule"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < TerrainMat->Rules.Num(); ++i)
				{
					const FPaperTerrainMaterialRule& Rule = TerrainMat->Rules[i];
					sol::table R = Lua.create_table();
					R["index"] = i + 1;
					R["start_cap"] = Rule.StartCap ? TCHAR_TO_UTF8(*Rule.StartCap->GetPathName()) : "None";
					R["end_cap"] = Rule.EndCap ? TCHAR_TO_UTF8(*Rule.EndCap->GetPathName()) : "None";
					R["min_angle"] = Rule.MinimumAngle;
					R["max_angle"] = Rule.MaximumAngle;
					R["collision"] = Rule.bEnableCollision;
					R["collision_offset"] = Rule.CollisionOffset;
					R["draw_order"] = Rule.DrawOrder;

					sol::table BodySprites = Lua.create_table();
					for (int32 b = 0; b < Rule.Body.Num(); ++b)
					{
						BodySprites[b + 1] = Rule.Body[b] ? TCHAR_TO_UTF8(*Rule.Body[b]->GetPathName()) : "None";
					}
					R["body"] = BodySprites;
					Result[i + 1] = R;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"rules\") -> %d"), TerrainMat->Rules.Num()));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: rules"), *FType));
			return sol::lua_nil;
		});

		// add("rule", {...})
		AssetObj.set_function("add", [TerrainMat, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TerrainMat)) { Session.Log(TEXT("[FAIL] add -> invalid")); return sol::lua_nil; }
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (!FType.Equals(TEXT("rule"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: rule"), *FType));
				return sol::lua_nil;
			}
			if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"rule\") -> params required")); return sol::lua_nil; }

			sol::table P = Params.value();
			FPaperTerrainMaterialRule NewRule;

			if (auto V = P.get<sol::optional<std::string>>("start_cap"))
			{
				NewRule.StartCap = LoadObject<UPaperSprite>(nullptr, UTF8_TO_TCHAR(V.value().c_str()));
			}
			if (auto V = P.get<sol::optional<std::string>>("end_cap"))
			{
				NewRule.EndCap = LoadObject<UPaperSprite>(nullptr, UTF8_TO_TCHAR(V.value().c_str()));
			}

			if (auto BodyOpt = P.get<sol::optional<sol::table>>("body"))
			{
				for (auto& Pair : BodyOpt.value())
				{
					if (Pair.second.is<std::string>())
					{
						UPaperSprite* BodySprite = LoadObject<UPaperSprite>(nullptr, UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str()));
						if (BodySprite) NewRule.Body.Add(BodySprite);
					}
				}
			}

			NewRule.MinimumAngle = static_cast<float>(P.get_or("min_angle", 0.0));
			NewRule.MaximumAngle = static_cast<float>(P.get_or("max_angle", 360.0));
			NewRule.bEnableCollision = P.get_or("collision", true);
			NewRule.CollisionOffset = static_cast<float>(P.get_or("collision_offset", 0.0));
			NewRule.DrawOrder = P.get_or("draw_order", 0);

			TerrainMat->Modify();
			FProperty* RulesProp = TerrainMat->GetClass()->FindPropertyByName(TEXT("Rules"));
			if (RulesProp) TerrainMat->PreEditChange(RulesProp);
			TerrainMat->Rules.Add(NewRule);
			TerrainMat->MarkPackageDirty();
			if (RulesProp)
			{
				FPropertyChangedEvent Event(RulesProp, EPropertyChangeType::ArrayAdd);
				TerrainMat->PostEditChangeProperty(Event);
			}

			Session.Log(FString::Printf(TEXT("[OK] add(\"rule\") -> %d rules total"), TerrainMat->Rules.Num()));
			return sol::make_object(Lua, true);
		});

		// remove("rule", index)
		AssetObj.set_function("remove", [TerrainMat, &Session](sol::table /*self*/,
			const std::string& Type, int Index,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TerrainMat)) { Session.Log(TEXT("[FAIL] remove -> invalid")); return sol::lua_nil; }

			int32 ZeroIndex = Index - 1;
			if (ZeroIndex < 0 || ZeroIndex >= TerrainMat->Rules.Num())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"rule\", %d) -> out of range"), Index));
				return sol::lua_nil;
			}

			TerrainMat->Modify();
			FProperty* RulesPropR = TerrainMat->GetClass()->FindPropertyByName(TEXT("Rules"));
			if (RulesPropR) TerrainMat->PreEditChange(RulesPropR);
			TerrainMat->Rules.RemoveAt(ZeroIndex);
			TerrainMat->MarkPackageDirty();
			if (RulesPropR)
			{
				FPropertyChangedEvent Event(RulesPropR, EPropertyChangeType::ArrayRemove);
				TerrainMat->PostEditChangeProperty(Event);
			}

			Session.Log(FString::Printf(TEXT("[OK] remove(\"rule\", %d) -> %d remaining"), Index, TerrainMat->Rules.Num()));
			return sol::make_object(Lua, true);
		});

		// configure("material"|"rule", ...)
		AssetObj.set_function("configure", [TerrainMat, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::table Params,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(TerrainMat)) { Session.Log(TEXT("[FAIL] configure -> invalid")); return sol::lua_nil; }
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("material"), ESearchCase::IgnoreCase))
			{
				TerrainMat->Modify();
				FProperty* FillProp = TerrainMat->GetClass()->FindPropertyByName(TEXT("InteriorFill"));
				if (FillProp) TerrainMat->PreEditChange(FillProp);
				if (auto V = Params.get<sol::optional<std::string>>("interior_fill"))
				{
					TerrainMat->InteriorFill = LoadObject<UPaperSprite>(nullptr, UTF8_TO_TCHAR(V.value().c_str()));
				}
				TerrainMat->MarkPackageDirty();
				if (FillProp)
				{
					FPropertyChangedEvent Event(FillProp, EPropertyChangeType::ValueSet);
					TerrainMat->PostEditChangeProperty(Event);
				}
				Session.Log(TEXT("[OK] configure(\"material\")"));
				return sol::make_object(Lua, true);
			}

			if (FType.Equals(TEXT("rule"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<int>()) { Session.Log(TEXT("[FAIL] configure(\"rule\") -> 1-based index required")); return sol::lua_nil; }
				int32 ZeroIndex = Id.as<int>() - 1;
				if (ZeroIndex < 0 || ZeroIndex >= TerrainMat->Rules.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"rule\", %d) -> out of range"), Id.as<int>()));
					return sol::lua_nil;
				}

				TerrainMat->Modify();
				FPaperTerrainMaterialRule& Rule = TerrainMat->Rules[ZeroIndex];

				if (auto V = Params.get<sol::optional<double>>("min_angle")) Rule.MinimumAngle = static_cast<float>(V.value());
				if (auto V = Params.get<sol::optional<double>>("max_angle")) Rule.MaximumAngle = static_cast<float>(V.value());
				if (auto V = Params.get<sol::optional<bool>>("collision")) Rule.bEnableCollision = V.value();
				if (auto V = Params.get<sol::optional<double>>("collision_offset")) Rule.CollisionOffset = static_cast<float>(V.value());
				if (auto V = Params.get<sol::optional<int>>("draw_order")) Rule.DrawOrder = V.value();
				if (auto V = Params.get<sol::optional<std::string>>("start_cap"))
					Rule.StartCap = LoadObject<UPaperSprite>(nullptr, UTF8_TO_TCHAR(V.value().c_str()));
				if (auto V = Params.get<sol::optional<std::string>>("end_cap"))
					Rule.EndCap = LoadObject<UPaperSprite>(nullptr, UTF8_TO_TCHAR(V.value().c_str()));

				TerrainMat->MarkPackageDirty();
				FProperty* RulesPropC = TerrainMat->GetClass()->FindPropertyByName(TEXT("Rules"));
				if (RulesPropC)
				{
					FPropertyChangedEvent Event(RulesPropC, EPropertyChangeType::ValueSet);
					TerrainMat->PostEditChangeProperty(Event);
				}
				Session.Log(FString::Printf(TEXT("[OK] configure(\"rule\", %d)"), Id.as<int>()));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown. Valid: material, rule"), *FType));
			return sol::lua_nil;
		});
	});

	// ==================================================================
	// Standalone utilities: slice_sprite_sheet, auto_flipbooks
	// ==================================================================

	// slice_sprite_sheet(texture_path) -> auto-detect sprite rectangles from a texture
	Lua.set_function("slice_sprite_sheet", [&Session](const std::string& TexturePath, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FTexPath = UTF8_TO_TCHAR(TexturePath.c_str());
		if (!FTexPath.StartsWith(TEXT("/"))) FTexPath = TEXT("/Game/") + FTexPath;

		UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *FTexPath);
		if (!Texture)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] slice_sprite_sheet -> texture not found: %s"), *FTexPath));
			return sol::lua_nil;
		}

#if WITH_EDITOR
		TArray<FIntRect> Rects;
		UPaperSprite::ExtractRectsFromTexture(Texture, Rects);

		sol::table Result = Lua.create_table();
		for (int32 i = 0; i < Rects.Num(); ++i)
		{
			sol::table R = Lua.create_table();
			R["index"] = i + 1;
			R["x"] = Rects[i].Min.X;
			R["y"] = Rects[i].Min.Y;
			R["width"] = Rects[i].Width();
			R["height"] = Rects[i].Height();
			Result[i + 1] = R;
		}

		Session.Log(FString::Printf(TEXT("[OK] slice_sprite_sheet(\"%s\") -> %d regions detected"), *FTexPath, Rects.Num()));
		return Result;
#else
		Session.Log(TEXT("[FAIL] slice_sprite_sheet -> editor only"));
		return sol::lua_nil;
#endif
	});

	// auto_flipbooks(sprite_paths) -> group sprites by name pattern into flipbook groups
	Lua.set_function("auto_flipbooks", [&Session](sol::table SpritePaths, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);

		TArray<UPaperSprite*> Sprites;
		TArray<FString> Names;

		for (auto& Pair : SpritePaths)
		{
			if (!Pair.second.is<std::string>()) continue;
			FString Path = UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str());
			if (!Path.StartsWith(TEXT("/"))) Path = TEXT("/Game/") + Path;

			UPaperSprite* Sprite = LoadObject<UPaperSprite>(nullptr, *Path);
			if (Sprite)
			{
				Sprites.Add(Sprite);
				Names.Add(Sprite->GetName());
			}
		}

		if (Sprites.Num() == 0)
		{
			Session.Log(TEXT("[FAIL] auto_flipbooks -> no valid sprites found"));
			return sol::lua_nil;
		}

		TMap<FString, TArray<UPaperSprite*>> FlipbookMap;
		FPaperFlipbookHelpers::ExtractFlipbooksFromSprites(FlipbookMap, Sprites, Names);

		sol::table Result = Lua.create_table();
		int32 Idx = 1;
		for (auto& KV : FlipbookMap)
		{
			sol::table Group = Lua.create_table();
			Group["name"] = TCHAR_TO_UTF8(*KV.Key);

			sol::table SpriteList = Lua.create_table();
			for (int32 i = 0; i < KV.Value.Num(); ++i)
			{
				SpriteList[i + 1] = KV.Value[i] ? TCHAR_TO_UTF8(*KV.Value[i]->GetPathName()) : "None";
			}
			Group["sprites"] = SpriteList;
			Group["count"] = KV.Value.Num();
			Result[Idx++] = Group;
		}

		Session.Log(FString::Printf(TEXT("[OK] auto_flipbooks -> %d groups from %d sprites"), FlipbookMap.Num(), Sprites.Num()));
		return Result;
	});
}

REGISTER_LUA_BINDING(PaperSprite, PaperSpriteDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("Paper2D")))
	{
		Session.Log(TEXT("[WARN] Paper2D plugin is not loaded. Enable it in Edit > Plugins to use this feature."));
		return;
	}
	BindPaperSprite(Lua, Session);
});

