// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Modules/ModuleManager.h"
#include "Tools/NeoStackToolUtils.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "Selection.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyAccessUtil.h"
#include "UObject/Package.h"

// Actor subsystem
#include "Subsystems/EditorActorSubsystem.h"
#include "LevelEditorSubsystem.h"

// Actor / Components
#include "GameFramework/Actor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SplineComponent.h"
#include "Components/LightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"

// Folders & Groups
#include "Folder.h"
#include "EditorActorFolders.h"
#include "Editor/GroupActor.h"

// Landscape
#include "Landscape.h"
#include "LandscapeProxy.h"
#include "LandscapeInfo.h"
#include "LandscapeComponent.h"
#include "LandscapeEdit.h"
#include "LandscapeDataAccess.h"
#include "LandscapeLayerInfoObject.h"
#include "PhysicalMaterials/PhysicalMaterial.h"

// Foliage
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "FoliageType.h"
#include "FoliageType_InstancedStaticMesh.h"

// Landscape Import (heightmap/weightmap file import)
#include "LandscapeImportHelper.h"

// Procedural Noise (FractalBrownianMotionNoise)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
#include "ProceduralNoise.h"
#endif

// Landscape Splines
#include "LandscapeSplinesComponent.h"
#include "LandscapeSplineControlPoint.h"
#include "LandscapeSplineSegment.h"

// Navigation
#include "NavigationSystem.h"

// Landscape Edit Layers
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
#include "LandscapeEditLayer.h"
#include "LandscapeEditTypes.h"
#endif

// HLOD / Editor Build
#include "EditorBuildUtils.h"

// Audio Component (for add_component)
#include "Components/AudioComponent.h"

// Level Streaming
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingDynamic.h"
#include "EditorLevelUtils.h"

// Level loading / saving
#include "FileHelpers.h"

// Layers
#include "Layers/LayersSubsystem.h"

// World Partition
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerInstanceWithAsset.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "WorldPartition/WorldPartitionRuntimeSpatialHash.h"

// Water (optional plugin)
#if WITH_WATER
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "WaterBodyRiverComponent.h"
#include "WaterSplineComponent.h"
#endif

// Geometry Scripting moved to AIK_GeometryScripting extension module

// Paper2D actors & components
#if WITH_PAPER2D
#include "PaperSprite.h"
#include "PaperFlipbook.h"
#include "PaperTileMap.h"
#include "PaperSpriteActor.h"
#include "PaperFlipbookActor.h"
#include "PaperTileMapActor.h"
#include "PaperCharacter.h"
#include "PaperSpriteComponent.h"
#include "PaperFlipbookComponent.h"
#include "PaperTileMapComponent.h"
#include "PaperGroupedSpriteComponent.h"
#include "PaperTerrainComponent.h"
#include "PaperTerrainMaterial.h"
#endif

// Collision
#include "Engine/HitResult.h"
#include "CollisionQueryParams.h"

// Asset creation
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"

// Math
#include "Math/UnrealMathUtility.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include "LevelSerializerUtils.h"

// ============================================================================
// Helpers
// ============================================================================

namespace
{

UWorld* GetEditorWorld()
{
	if (!GEditor) return nullptr;
	return GEditor->GetEditorWorldContext().World();
}

UEditorActorSubsystem* GetActorSubsystem()
{
	if (!GEditor) return nullptr;
	return GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
}

ULayersSubsystem* GetLayersSubsystem()
{
	if (GEditor) return GEditor->GetEditorSubsystem<ULayersSubsystem>();
	return nullptr;
}

AActor* FindActorByLabel(UWorld* World, const FString& Label)
{
	if (!World) return nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (A && A->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase))
			return A;
	}
	return nullptr;
}

AActor* FindActorByNameOrLabel(UWorld* World, const FString& Id)
{
	if (!World) return nullptr;

	// Try label first (user-visible name)
	if (AActor* A = FindActorByLabel(World, Id)) return A;

	// Try internal name
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (A && A->GetName().Equals(Id, ESearchCase::IgnoreCase))
			return A;
	}
	return nullptr;
}

FVector TableToVector(const sol::table& T)
{
	return FVector(
		T.get_or("x", 0.0),
		T.get_or("y", 0.0),
		T.get_or("z", 0.0)
	);
}

FRotator TableToRotator(const sol::table& T)
{
	return FRotator(
		T.get_or("pitch", 0.0),
		T.get_or("yaw", 0.0),
		T.get_or("roll", 0.0)
	);
}

sol::table VectorToTable(sol::state_view& Lua, const FVector& V)
{
	sol::table T = Lua.create_table();
	T["x"] = V.X;
	T["y"] = V.Y;
	T["z"] = V.Z;
	return T;
}

sol::table RotatorToTable(sol::state_view& Lua, const FRotator& R)
{
	sol::table T = Lua.create_table();
	T["pitch"] = R.Pitch;
	T["yaw"] = R.Yaw;
	T["roll"] = R.Roll;
	return T;
}

sol::table ActorToTable(sol::state_view& Lua, AActor* Actor)
{
	sol::table T = Lua.create_table();
	T["name"] = TCHAR_TO_UTF8(*Actor->GetName());
	T["label"] = TCHAR_TO_UTF8(*Actor->GetActorLabel());
	T["class"] = TCHAR_TO_UTF8(*Actor->GetClass()->GetName());
	T["location"] = VectorToTable(Lua, Actor->GetActorLocation());
	T["rotation"] = RotatorToTable(Lua, Actor->GetActorRotation());
	T["folder"] = TCHAR_TO_UTF8(*Actor->GetFolderPath().ToString());
	T["hidden"] = Actor->IsTemporarilyHiddenInEditor();

	// Tags
	if (Actor->Tags.Num() > 0)
	{
		sol::table Tags = Lua.create_table();
		for (int32 i = 0; i < Actor->Tags.Num(); i++)
			Tags[i + 1] = TCHAR_TO_UTF8(*Actor->Tags[i].ToString());
		T["tags"] = Tags;
	}

	// Layers
	if (Actor->Layers.Num() > 0)
	{
		sol::table LayerArr = Lua.create_table();
		for (int32 i = 0; i < Actor->Layers.Num(); i++)
			LayerArr[i + 1] = std::string(TCHAR_TO_UTF8(*Actor->Layers[i].ToString()));
		T["layers"] = LayerArr;
	}

	// Mobility
	if (USceneComponent* Root = Actor->GetRootComponent())
	{
		switch (Root->Mobility)
		{
			case EComponentMobility::Static:     T["mobility"] = "static"; break;
			case EComponentMobility::Stationary:  T["mobility"] = "stationary"; break;
			case EComponentMobility::Movable:     T["mobility"] = "movable"; break;
		}
	}

	return T;
}

// Surface trace: find ground position below a point
bool TraceGround(UWorld* World, const FVector& Start, FVector& OutLocation, FVector& OutNormal)
{
	FHitResult Hit;
	FVector End = Start - FVector(0, 0, 1000000.0);
	FCollisionQueryParams Params;
	Params.bTraceComplex = false;
	if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic, Params))
	{
		OutLocation = Hit.ImpactPoint;
		OutNormal = Hit.ImpactNormal;
		return true;
	}
	OutLocation = Start;
	OutNormal = FVector::UpVector;
	return false;
}

ALandscapeProxy* FindLandscapeByIndex(UWorld* W, int32 Index)
{
	int32 Idx = 0;
	for (TActorIterator<ALandscapeProxy> It(W); It; ++It)
	{
		if (Idx == Index) return *It;
		Idx++;
	}
	return nullptr;
}

} // anonymous namespace

// ============================================================================
// Documentation
// ============================================================================

static TArray<FLuaFunctionDoc> LevelDesignDocs = {
	{ TEXT("open_level()"), TEXT("Open level design handle — verbs: add, remove, list, configure, sculpt, paint, auto_paint, create_landscape_layer, delete_landscape_layer, scatter, navigate, water, partition, geometry, info, serialize, get_heightmap, get_weightmap, help"), TEXT("level table or nil") },
	{ TEXT("list_layers()"), TEXT("List all layers in the current editor world"), TEXT("string[]") },
	{ TEXT("create_layer(name)"), TEXT("Create a new layer"), TEXT("true or nil") },
	{ TEXT("delete_layer(name)"), TEXT("Delete a layer by name"), TEXT("true or nil") },
	{ TEXT("add_actor_to_layer(actor_name, layer_name)"), TEXT("Add an actor to a layer"), TEXT("true or nil") },
	{ TEXT("remove_actor_from_layer(actor_name, layer_name)"), TEXT("Remove an actor from a layer"), TEXT("true or nil") },
	{ TEXT("get_actors_in_layer(layer_name)"), TEXT("Get all actor labels in a given layer"), TEXT("string[]") },
	{ TEXT("duplicate_actor(name, opts?)"), TEXT("Duplicate an actor — opts: offset, new_label"), TEXT("table or nil") },
	{ TEXT("select_actor(name)"), TEXT("Select an actor by label or name"), TEXT("true or nil") },
	{ TEXT("deselect_all()"), TEXT("Clear the editor actor selection"), TEXT("true") },
	{ TEXT("get_selected_actors()"), TEXT("Get all currently selected actors in the editor"), TEXT("table[]") },
	{ TEXT("list_streaming_levels()"), TEXT("List all streaming sub-levels in the current world"), TEXT("table[]") },
	{ TEXT("set_streaming_level(name, opts)"), TEXT("Control a streaming level — opts: loaded, visible, location={x,y,z}, rotation={pitch,yaw,roll}"), TEXT("true or nil") },
	{ TEXT("add_streaming_level(package_path)"), TEXT("Add a streaming sub-level to the world"), TEXT("table or nil") },
	{ TEXT("remove_streaming_level(name)"), TEXT("Remove a streaming sub-level from the world"), TEXT("true or nil") },
	{ TEXT("create_level(path, opts?)"), TEXT("Create a new level/map — opts: world_partition, template (\"open_world\"|\"basic\"|full path), open (default true). Open World template includes sky, atmosphere, lighting, WP."), TEXT("table or nil") },
	{ TEXT("load_level(path)"), TEXT("Open/load a level in the editor by content path or filename"), TEXT("true or nil") },
	{ TEXT("save_level_as(path)"), TEXT("Save the current level to a new content path"), TEXT("true or nil") },
	{ TEXT("import_heightmap(params)"), TEXT("Import heightmap from file (.png 16-bit or .r16) — params: file, size?, scale?, material?, location?"), TEXT("table or nil") },
	{ TEXT("import_weightmap(params)"), TEXT("Import weightmap/splatmap from file (.png 8-bit grayscale) — params: file, landscape?, layer, region?"), TEXT("true or nil") },
	{ TEXT("add_component(actor, params)"), TEXT("Add a component to an actor — params: type (class name), name?, mesh?, material?"), TEXT("table or nil") },
	{ TEXT("remove_component(actor, name)"), TEXT("Remove a component from an actor by name"), TEXT("true or nil") },
	{ TEXT("configure_component(actor, name, params)"), TEXT("Configure a component — params: property bag, mesh?, material?, intensity?, color?"), TEXT("table or nil") },
	{ TEXT("get_actor_properties(actor, opts?)"), TEXT("Read UProperties on a level actor — opts: filter string, or {filter?, all?, changed_only?(default false)}. changed_only=true compares against CDO to only return modified properties."), TEXT("table[]") },
	{ TEXT("get_actor_property(actor, property)"), TEXT("Read a single property value from a level actor (tries actor then root component)"), TEXT("string or nil") },
	{ TEXT("get_component_properties(actor, comp, opts?)"), TEXT("Read changed UProperties on a component — opts: filter string, or {filter?, all?, changed_only?(default true)}"), TEXT("table[]") },
	{ TEXT("list_actor_components(actor)"), TEXT("List all components on a level actor — returns array of {name, class}"), TEXT("table[]") },
};

// ============================================================================
// Help text
// ============================================================================

static const char* LevelDesign_HelpText =
	"Level Design Tool — place actors, sculpt landscapes, scatter foliage\n"
	"\n"
	"add(type, params):\n"
	"  add(\"actor\", {class=\"/Game/BP_Wall\", location={x=0,y=0,z=0}, rotation?={yaw=90}, label?=\"Wall1\", folder?=\"Buildings\"})\n"
	"  add(\"actor\", {mesh=\"/Game/Meshes/SM_Floor\", location={x=0,y=0,z=0}})  -- spawns StaticMeshActor\n"
	"  add(\"landscape\", {size=2017, scale?={x=100,y=100,z=100}, material?=\"/Game/M_Ground\",\n"
	"       noise?={amplitude=5000, frequency=0.002, octaves=4,\n"
	"              mode?=\"standard|turbulent|ridge\", lacunarity?=2.0, gain?=0.5}, flat_height?=0})\n"
	"  add(\"foliage\", {mesh=\"/Game/SM_Tree\", transforms=[{location,rotation?,scale?},...]})\n"
	"  add(\"instances\", {mesh=\"/Game/SM_Rock\", transforms=[{location,rotation?,scale?},...]})\n"
	"  add(\"folder\", {path=\"Buildings/House_01\"})\n"
	"  add(\"spline\", {actor=\"MyActor\", component=\"SplineComp\", points=[{x,y,z},...], closed?=false})\n"
	"  add(\"light\", {type=\"point|spot|directional|sky\", location?={}, rotation?={}, intensity?=10, color?=\"255,255,255\"})\n"
	"\n"
	"remove(type, id):\n"
	"  remove(\"actor\", \"Wall1\")  -- by label or internal name\n"
	"  remove(\"folder\", \"Buildings/House_01\")\n"
	"  remove(\"foliage\", {mesh=\"/Game/SM_Tree\", region?={x1,y1,x2,y2}})  -- remove all instances\n"
	"\n"
	"list(type?, filter?):\n"
	"  list(\"actors\")  -- all actors\n"
	"  list(\"actors\", {class=\"StaticMeshActor\", folder?=\"Buildings\", name?=\"Wall\"})  -- filtered\n"
	"  list(\"folders\")\n"
	"  list(\"landscapes\")\n"
	"  list(\"foliage\")\n"
	"\n"
	"configure(type, id, params):\n"
	"  configure(\"actor\", \"Wall1\", {location?={}, rotation?={}, scale?={}, label?=\"NewName\",\n"
	"            folder?=\"NewFolder\", mesh?=\"/Game/NewMesh\", material?=\"/Game/NewMat\", property?={name=val}})\n"
	"  configure(\"landscape\", 0, {material?=\"/Game/M_Grass\"})  -- 0 = first landscape\n"
	"\n"
	"sculpt(params):\n"
	"  sculpt({landscape?=0, region={x1,y1,x2,y2}, mode=\"set|add|smooth|flatten|noise|hole|erode|hydraulic\",\n"
	"          height?=5000, amplitude?=3000, frequency?=0.005, octaves?=4, strength?=1.0,\n"
	"          noise_mode?=\"standard|turbulent|ridge\", lacunarity?=2.0, gain?=0.5})\n"
	"  sculpt({mode=\"hole\", region={...}})  -- punch a hole in landscape (fill=true to restore)\n"
	"  sculpt({mode=\"erode\", iterations?=5, threshold?=0.5, strength?=0.3})  -- thermal erosion\n"
	"  sculpt({mode=\"hydraulic\", iterations?=8, rain?=0.01, solubility?=0.01, evaporation?=0.5, capacity?=0.1})\n"
	"\n"
	"paint(params):\n"
	"  paint({landscape?=0, region={x1,y1,x2,y2}, layer=\"Grass\", weight?=255})\n"
	"  paint({landscape?=0, region={x1,y1,x2,y2}, layers={{name=\"Grass\",weight=200},{name=\"Rock\",weight=55}}})  -- multi-layer\n"
	"\n"
	"auto_paint(params) — paint by slope/height rules:\n"
	"  auto_paint({landscape?=0, noise_strength?=0.1, region?={x1,y1,x2,y2},\n"
	"    layers={{name=\"Grass\",min_slope=0,max_slope=30,min_height=-99999,max_height=5000},\n"
	"            {name=\"Rock\",min_slope=30,max_slope=90},\n"
	"            {name=\"Snow\",min_height=8000,max_height=99999}}})\n"
	"\n"
	"create_landscape_layer(params):\n"
	"  create_landscape_layer({landscape?=0, name=\"Rock\", blend_method?=\"alpha|weight|none\",\n"
	"    hardness?=0.5, physical_material?=\"/Game/PM_Rock\"})\n"
	"\n"
	"delete_landscape_layer(params):\n"
	"  delete_landscape_layer({landscape?=0, name=\"Rock\"})  -- removes a paint layer from landscape\n"
	"\n"
	"scatter(params):\n"
	"  scatter({mesh=\"/Game/SM_Tree\", region={x1,y1,x2,y2}, count=200,\n"
	"           distribution?=\"random|grid\", align_to_surface?=true, seed?=0, as_foliage?=true})\n"
	"\n"
	"navigate(verb, params?):\n"
	"  navigate(\"build\")  -- trigger navmesh build\n"
	"  navigate(\"rebuild\")  -- full navmesh rebuild\n"
	"  navigate(\"cancel\")  -- cancel active build\n"
	"  navigate(\"info\")  -- returns navmesh status\n"
	"\n"
	"water(verb, params):  [requires Water plugin]\n"
	"  NOTE: To CREATE water bodies, use: water_spawn(\"ocean|lake|river|custom\", {x,y,z}, opts?)\n"
	"    water_spawn(\"ocean\", {x=0,y=0,z=0})\n"
	"    water_spawn(\"river\", {x=0,y=0,z=0}, {points={{x=0,y=0,z=0},{x=5000,y=0,z=0}}})\n"
	"    water_spawn(\"lake\", {x=0,y=0,z=0})\n"
	"  For existing water bodies:\n"
	"  water(\"sync\", {actor=\"River1\"})  -- sync spline after edits\n"
	"  water(\"set_width\", {actor=\"River1\", key=0.5, width=500})\n"
	"  water(\"set_depth\", {actor=\"River1\", key=0.5, depth=100})\n"
	"  water(\"set_velocity\", {actor=\"River1\", key=0.5, velocity=1.0})\n"
	"  water(\"set_material\", {actor=\"River1\", material=\"/Game/M_Water\"})\n"
	"  For full water API: open_asset() on any water body actor, then call help()\n"
	"\n"
	"partition(verb, params?):\n"
	"  partition(\"enable\")  -- enable world partition\n"
	"  partition(\"info\")  -- returns WP status + data layers\n"
	"  partition(\"set_layer_state\", {layer=\"MyLayer\", state=\"activated|loaded|unloaded\", recursive?=false})\n"
	"  partition(\"set_streaming\", {enabled=true})\n"
	"  partition(\"configure_grid\", {cell_size=12800, loading_range=25600, debug_draw?=false})\n"
	"  partition(\"create_layer\", {name=\"LayerName\", type?=\"runtime|editor\"})\n"
	"  partition(\"delete_layer\", {name=\"LayerName\"})\n"
	"  partition(\"add_to_layer\", {actor=\"ActorLabel\", layer=\"LayerName\"})\n"
	"  partition(\"remove_from_layer\", {actor=\"ActorLabel\", layer=\"LayerName\"})\n"
	"  partition(\"get_layer_actors\", {name=\"LayerName\"})  -- returns list of actor labels\n"
	"\n"
	"geometry(params):  [requires GeometryScripting plugin]\n"
	"  geometry({type=\"box\", size={x=100,y=100,z=100}, save=\"/Game/Gen/MyBox\", spawn_location?={x=0,y=0,z=0}})\n"
	"  geometry({type=\"sphere\", radius=50, steps?=16, save=\"/Game/Gen/MySphere\"})\n"
	"  geometry({type=\"cylinder\", radius=50, height=100, steps?=12, save=\"/Game/Gen/MyCyl\"})\n"
	"  geometry({type=\"cone\", base_radius=50, top_radius=5, height=100, save=\"/Game/Gen/MyCone\"})\n"
	"  geometry({type=\"capsule\", radius=30, length=75, save=\"/Game/Gen/MyCapsule\"})\n"
	"  geometry({type=\"boolean\", target=\"/Game/MeshA\", tool=\"/Game/MeshB\", operation=\"subtract|union|intersect\", save=\"/Game/Gen/Result\"})\n"
	"\n"
	"add(\"landscape_spline\", {landscape?=0, points=[{x,y,z},...], width?=500, falloff?=200, layer?=\"Road\"}):\n"
	"  Creates landscape spline (road/river carved into terrain)\n"
	"\n"
	"list(\"landscape_splines\"):\n"
	"  Returns all landscape spline segments\n"
	"\n"
	"import_heightmap(params):\n"
	"  import_heightmap({file=\"/path/to/heightmap.png\", size?=2017, scale?={x=100,y=100,z=100},\n"
	"                    material?=\"/Game/M_Ground\", location?={x=0,y=0,z=0}})\n"
	"  Imports a heightmap file (.png 16-bit or .r16) to create a new landscape\n"
	"\n"
	"import_weightmap(params):\n"
	"  import_weightmap({file=\"/path/to/splatmap.png\", landscape?=0, layer=\"Grass\",\n"
	"                    region?={x1=0,y1=0,x2=504,y2=504}})\n"
	"  Imports a weightmap/splatmap file (8-bit grayscale) onto an existing landscape layer\n"
	"\n"
	"info():\n"
	"  Returns {actor_count, landscape_count, foliage_types, folders, bounds}\n"
	"\n"
	"serialize(mode?):\n"
	"  serialize() or serialize(\"script\") — returns Lua script string that recreates the entire level\n"
	"  serialize(\"table\") — returns array of detailed actor tables with mesh, materials, light, scale, components\n"
	"  Use for training data generation: captures actors, lights, foliage, folders\n"
	"\n"
	"save():\n"
	"  Saves the current level\n"
	"\n"
	"configure(\"actor\", name, {add_layer?=\"LayerName\", remove_layer?=\"LayerName\", ...}):\n"
	"  Add/remove actor layer membership via configure\n"
	"\n"
	"Layer management (global functions):\n"
	"  list_layers()  -- returns array of all layer names\n"
	"  create_layer(\"MyLayer\")  -- create a new layer\n"
	"  delete_layer(\"MyLayer\")  -- delete a layer\n"
	"  add_actor_to_layer(\"Wall1\", \"MyLayer\")  -- add actor to layer\n"
	"  remove_actor_from_layer(\"Wall1\", \"MyLayer\")  -- remove actor from layer\n"
	"  get_actors_in_layer(\"MyLayer\")  -- returns array of actor labels in layer\n"
	"\n"
	"Actor duplication & selection (global functions):\n"
	"  duplicate_actor(\"Wall1\", {offset={x=100,y=0,z=0}, new_label=\"Wall2\"})  -- duplicate with offset\n"
	"  select_actor(\"Wall1\")  -- add actor to selection\n"
	"  deselect_all()  -- clear selection\n"
	"  get_selected_actors()  -- returns array of {name, label, class, location, rotation, ...}\n"
	"\n"
	"Component management (global functions):\n"
	"  add_component(\"MyActor\", {type=\"PointLightComponent\", name?=\"MyLight\"})\n"
	"  add_component(\"MyActor\", {type=\"StaticMeshComponent\", name?=\"MeshComp\", mesh?=\"/Game/SM_Rock\"})\n"
	"  add_component(\"MyActor\", {type=\"AudioComponent\", name?=\"Audio\"})\n"
	"  remove_component(\"MyActor\", \"MyLight\")\n"
	"  configure_component(\"MyActor\", \"MyLight\", {intensity=5000, color=\"255,200,100\"})\n"
	"  configure_component(\"MyActor\", \"MeshComp\", {mesh=\"/Game/SM_Tree\", material=\"/Game/M_Bark\"})\n"
	"  configure_component(\"MyActor\", \"CompName\", {property={PropName=\"Value\"}})\n"
	"\n"
	"edit_layer(verb, params) — landscape edit layers (non-destructive):\n"
	"  edit_layer(\"list\", {landscape?=0})  -- returns all edit layers\n"
	"  edit_layer(\"create\", {landscape?=0, name=\"Mountains\"})\n"
	"  edit_layer(\"delete\", {landscape?=0, name=\"Mountains\"})\n"
	"  edit_layer(\"set_visibility\", {landscape?=0, name=\"Mountains\", visible=false})\n"
	"  edit_layer(\"set_locked\", {landscape?=0, name=\"Mountains\", locked=true})\n"
	"  edit_layer(\"set_alpha\", {landscape?=0, name=\"Mountains\", heightmap_alpha?=0.5, weightmap_alpha?=1.0})\n"
	"\n"
	"hlod(verb) — HLOD generation:\n"
	"  hlod(\"build\")  -- trigger HLOD build\n"
	"  hlod(\"info\")   -- get HLOD actor count\n";

// ============================================================================
// Binding
// ============================================================================

static void BindLevelDesign(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("open_level", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] open_level() -> no editor world"));
			return sol::lua_nil;
		}

		sol::table LevelObj = LuaView.create_table();
		LevelObj["_help_text"] = LevelDesign_HelpText;
		LevelObj["world_name"] = TCHAR_TO_UTF8(*World->GetName());

		// ================================================================
		// help()
		// ================================================================
		LevelObj.set_function("help", [](sol::table self, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::optional<std::string> H = self["_help_text"];
			if (H.has_value()) return sol::make_object(Lua, H.value());
			return sol::lua_nil;
		});

		// ================================================================
		// save()
		// ================================================================
		LevelObj.set_function("save", [&Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			ULevelEditorSubsystem* LevelSub = GEditor ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
			if (LevelSub && LevelSub->SaveCurrentLevel())
			{
				Session.Log(TEXT("[OK] save()"));
				return sol::make_object(Lua, true);
			}
			Session.Log(TEXT("[FAIL] save() -> could not save level"));
			return sol::lua_nil;
		});

		// ================================================================
		// info()
		// ================================================================
		LevelObj.set_function("info", [&Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UWorld* W = GetEditorWorld();
			if (!W) { Session.Log(TEXT("[FAIL] info() -> no world")); return sol::lua_nil; }

			int32 ActorCount = 0;
			int32 LandscapeCount = 0;
			int32 LightCount = 0;
			TSet<FName> Folders;
			FBox Bounds(ForceInit);

			for (TActorIterator<AActor> It(W); It; ++It)
			{
				AActor* A = *It;
				if (!A || A->IsA<AWorldSettings>()) continue;
				ActorCount++;
				if (A->IsA<ALandscapeProxy>()) LandscapeCount++;
				if (A->FindComponentByClass<ULightComponent>()) LightCount++;
				FName Folder = A->GetFolderPath();
				if (!Folder.IsNone()) Folders.Add(Folder);
				FVector Origin, Extent;
				A->GetActorBounds(false, Origin, Extent);
				if (!Extent.IsNearlyZero())
					Bounds += FBox(Origin - Extent, Origin + Extent);
			}

			sol::table R = Lua.create_table();
			R["actor_count"] = ActorCount;
			R["landscape_count"] = LandscapeCount;
			R["light_count"] = LightCount;
			R["folder_count"] = Folders.Num();

			sol::table FolderList = Lua.create_table();
			int32 Idx = 1;
			for (const FName& F : Folders) FolderList[Idx++] = TCHAR_TO_UTF8(*F.ToString());
			R["folders"] = FolderList;

			if (Bounds.IsValid)
			{
				R["bounds_min"] = VectorToTable(Lua, Bounds.Min);
				R["bounds_max"] = VectorToTable(Lua, Bounds.Max);
			}

			Session.Log(FString::Printf(TEXT("[OK] info() -> %d actors, %d landscapes, %d lights, %d folders"),
				ActorCount, LandscapeCount, LightCount, Folders.Num()));
			return R;
		});

		// ================================================================
		// serialize(mode?) — export level state for training data
		//   serialize()         → Lua script string that recreates the level
		//   serialize("script") → same as above
		//   serialize("table")  → array of detailed actor tables
		// ================================================================
		LevelObj.set_function("serialize", [&Session](sol::table /*self*/,
			sol::optional<std::string> Mode, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UWorld* W = GetEditorWorld();
			if (!W) { Session.Log(TEXT("[FAIL] serialize() -> no world")); return sol::lua_nil; }

			FString FMode = Mode.has_value() ? UTF8_TO_TCHAR(Mode.value().c_str()) : TEXT("script");

			if (FMode.Equals(TEXT("table"), ESearchCase::IgnoreCase))
			{
				// Return detailed table of all actors (uses basic ActorToTable + extras)
				sol::table Results = Lua.create_table();
				int32 Idx = 1;
				for (TActorIterator<AActor> It(W); It; ++It)
				{
					AActor* A = *It;
					if (!A || A->IsA<AWorldSettings>()) continue;

					sol::table T = ActorToTable(Lua, A);

					// Add scale
					FVector Scale = A->GetActorScale3D();
					if (!Scale.Equals(FVector::OneVector, 0.01))
						T["scale"] = VectorToTable(Lua, Scale);

					// Add mesh path
					if (UStaticMeshComponent* SMC = A->FindComponentByClass<UStaticMeshComponent>())
					{
						if (UStaticMesh* SM = SMC->GetStaticMesh())
							T["mesh"] = TCHAR_TO_UTF8(*SM->GetPathName());
						int32 NumMats = SMC->GetNumMaterials();
						if (NumMats > 0)
						{
							sol::table Mats = Lua.create_table();
							for (int32 mi = 0; mi < NumMats; mi++)
								if (UMaterialInterface* Mat = SMC->GetMaterial(mi))
									Mats[mi + 1] = TCHAR_TO_UTF8(*Mat->GetPathName());
							T["materials"] = Mats;
						}
					}

					// Add light properties
					if (ULightComponent* LC = A->FindComponentByClass<ULightComponent>())
					{
						sol::table Light = Lua.create_table();
						if (A->IsA<APointLight>())           Light["type"] = "point";
						else if (A->IsA<ASpotLight>())        Light["type"] = "spot";
						else if (A->IsA<ADirectionalLight>()) Light["type"] = "directional";
						else if (A->IsA<ASkyLight>())         Light["type"] = "sky";
						Light["intensity"] = LC->Intensity;
						FLinearColor Color = LC->GetLightColor();
						Light["color"] = TCHAR_TO_UTF8(*FString::Printf(TEXT("%d,%d,%d"),
							FMath::RoundToInt(Color.R * 255), FMath::RoundToInt(Color.G * 255), FMath::RoundToInt(Color.B * 255)));
						T["light"] = Light;
					}

					// Add blueprint class
					if (A->GetClass()->ClassGeneratedBy)
						T["blueprint"] = TCHAR_TO_UTF8(*A->GetClass()->ClassGeneratedBy->GetPathName());

					Results[Idx++] = T;
				}
				Session.Log(FString::Printf(TEXT("[OK] serialize(\"table\") -> %d actors"), Idx - 1));
				return Results;
			}

			// Default: generate Lua script
			FString Script = LevelSerializerUtils::GenerateScript(W);

			// Count actors for log
			int32 ActorCount = 0;
			for (TActorIterator<AActor> It(W); It; ++It)
			{
				if (*It && !(*It)->IsA<AWorldSettings>()) ActorCount++;
			}

			Session.Log(FString::Printf(TEXT("[OK] serialize() -> %d actors, %d chars of Lua script"),
				ActorCount, Script.Len()));
			return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*Script)));
		});

		// ================================================================
		// add(type, params)
		// ================================================================
		LevelObj.set_function("add", [&Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());
			UWorld* W = GetEditorWorld();
			if (!W) { Session.Log(TEXT("[FAIL] add -> no editor world")); return sol::lua_nil; }

			// ---- add("actor", {...}) ----
			if (FType.Equals(TEXT("actor"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"actor\") -> params required")); return sol::lua_nil; }
				sol::table P = Params.value();

				FVector Location = FVector::ZeroVector;
				FRotator Rotation = FRotator::ZeroRotator;
				if (auto L = P.get<sol::optional<sol::table>>("location")) Location = TableToVector(L.value());
				if (auto R = P.get<sol::optional<sol::table>>("rotation")) Rotation = TableToRotator(R.value());

				std::string ClassStr = P.get_or("class", std::string());
				std::string MeshStr = P.get_or("mesh", std::string());
				std::string LabelStr = P.get_or("label", std::string());
				std::string FolderStr = P.get_or("folder", std::string());

				UEditorActorSubsystem* ActorSub = GetActorSubsystem();
				if (!ActorSub) { Session.Log(TEXT("[FAIL] add(\"actor\") -> no actor subsystem")); return sol::lua_nil; }

				AActor* NewActor = nullptr;

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDAddActor", "Level Design: Add Actor"));

				if (!MeshStr.empty())
				{
					// Spawn from mesh asset
					UObject* MeshObj = LoadObject<UObject>(nullptr, UTF8_TO_TCHAR(MeshStr.c_str()));
					if (!MeshObj)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"actor\") -> mesh '%s' not found"),
							UTF8_TO_TCHAR(MeshStr.c_str())));
						return sol::lua_nil;
					}
					NewActor = ActorSub->SpawnActorFromObject(MeshObj, Location, Rotation);
				}
				else if (!ClassStr.empty())
				{
					// Spawn from class
					FString FClass = UTF8_TO_TCHAR(ClassStr.c_str());
					UClass* ActorClass = LoadObject<UClass>(nullptr, *FClass);
					if (!ActorClass)
					{
						// Try finding by short name
						ActorClass = FindObject<UClass>(nullptr, *FClass);
					}
					if (!ActorClass)
					{
						// TObjectIterator fallback
						FString ClassLower = FClass.ToLower();
						for (TObjectIterator<UClass> It; It; ++It)
						{
							if (It->IsChildOf(AActor::StaticClass()) && It->GetName().ToLower() == ClassLower)
							{
								ActorClass = *It;
								break;
							}
						}
					}
					if (!ActorClass)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"actor\") -> class '%s' not found"), *FClass));
						return sol::lua_nil;
					}
					NewActor = ActorSub->SpawnActorFromClass(ActorClass, Location, Rotation);
				}
				else
				{
					Session.Log(TEXT("[FAIL] add(\"actor\") -> 'class' or 'mesh' required"));
					return sol::lua_nil;
				}

				if (!NewActor)
				{
					Session.Log(TEXT("[FAIL] add(\"actor\") -> spawn failed"));
					return sol::lua_nil;
				}

				// Apply optional label
				if (!LabelStr.empty())
					NewActor->SetActorLabel(UTF8_TO_TCHAR(LabelStr.c_str()));

				// Apply optional folder
				if (!FolderStr.empty())
					NewActor->SetFolderPath(FName(UTF8_TO_TCHAR(FolderStr.c_str())));

				// Apply optional scale
				if (auto ScaleT = P.get<sol::optional<sol::table>>("scale"))
				{
					FVector Scale = TableToVector(ScaleT.value());
					NewActor->SetActorScale3D(Scale);
				}

				// Apply optional material override
				std::string MatStr = P.get_or("material", std::string());
				if (!MatStr.empty())
				{
					if (UStaticMeshComponent* SMC = NewActor->FindComponentByClass<UStaticMeshComponent>())
					{
						UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, UTF8_TO_TCHAR(MatStr.c_str()));
						if (Mat) SMC->SetMaterial(0, Mat);
					}
				}

				#if WITH_PAPER2D
				// Paper2D post-spawn: set sprite on PaperSpriteActor
				std::string SpriteStr = P.get_or("sprite", std::string());
				if (!SpriteStr.empty())
				{
					if (UPaperSpriteComponent* PSC = NewActor->FindComponentByClass<UPaperSpriteComponent>())
					{
						UPaperSprite* Spr = LoadObject<UPaperSprite>(nullptr, UTF8_TO_TCHAR(SpriteStr.c_str()));
						if (Spr) PSC->SetSprite(Spr);
					}
				}

				// Paper2D post-spawn: set flipbook on PaperFlipbookActor/PaperCharacter
				std::string FlipbookStr = P.get_or("flipbook", std::string());
				if (!FlipbookStr.empty())
				{
					if (UPaperFlipbookComponent* PFC = NewActor->FindComponentByClass<UPaperFlipbookComponent>())
					{
						UPaperFlipbook* FB = LoadObject<UPaperFlipbook>(nullptr, UTF8_TO_TCHAR(FlipbookStr.c_str()));
						if (FB) PFC->SetFlipbook(FB);
					}
				}

				// Paper2D post-spawn: set tilemap on PaperTileMapActor
				std::string TileMapStr = P.get_or("tilemap", std::string());
				if (!TileMapStr.empty())
				{
					if (UPaperTileMapComponent* PTMC = NewActor->FindComponentByClass<UPaperTileMapComponent>())
					{
						UPaperTileMap* TM = LoadObject<UPaperTileMap>(nullptr, UTF8_TO_TCHAR(TileMapStr.c_str()));
						if (TM) PTMC->SetTileMap(TM);
					}
				}

				// Paper2D post-spawn: set color tint
				if (auto ColorT = P.get<sol::optional<sol::table>>("color"))
				{
					FLinearColor Color(
						static_cast<float>(ColorT.value().get_or("r", 1.0)),
						static_cast<float>(ColorT.value().get_or("g", 1.0)),
						static_cast<float>(ColorT.value().get_or("b", 1.0)),
						static_cast<float>(ColorT.value().get_or("a", 1.0))
					);
					if (UPaperSpriteComponent* PSC = NewActor->FindComponentByClass<UPaperSpriteComponent>())
						PSC->SetSpriteColor(Color);
					else if (UPaperFlipbookComponent* PFC = NewActor->FindComponentByClass<UPaperFlipbookComponent>())
						PFC->SetSpriteColor(Color);
				}

				// Paper2D post-spawn: set terrain material on PaperTerrainComponent
				std::string TerrainMatStr = P.get_or("terrain_material", std::string());
				if (!TerrainMatStr.empty())
				{
					if (UPaperTerrainComponent* PTC = NewActor->FindComponentByClass<UPaperTerrainComponent>())
					{
						UPaperTerrainMaterial* TMat = LoadObject<UPaperTerrainMaterial>(nullptr, UTF8_TO_TCHAR(TerrainMatStr.c_str()));
						if (TMat) PTC->TerrainMaterial = TMat;
					}
				}
				#endif // WITH_PAPER2D

				Session.Log(FString::Printf(TEXT("[OK] add(\"actor\") -> %s at (%.0f, %.0f, %.0f)"),
					*NewActor->GetActorLabel(), Location.X, Location.Y, Location.Z));
				return ActorToTable(Lua, NewActor);
			}

			// ---- add("folder", {path=...}) ----
			if (FType.Equals(TEXT("folder"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"folder\") -> {path=..} required")); return sol::lua_nil; }
				std::string PathStr = Params.value().get_or("path", std::string());
				if (PathStr.empty()) { Session.Log(TEXT("[FAIL] add(\"folder\") -> path required")); return sol::lua_nil; }

				::FName FolderName(UTF8_TO_TCHAR(PathStr.c_str()));
				FFolder WorldRoot = FFolder::GetWorldRootFolder(W);
				FFolder NewFolder(WorldRoot.GetRootObject(), FolderName);
				bool bOK = FActorFolders::Get().CreateFolder(*W, NewFolder);
				if (bOK)
					Session.Log(FString::Printf(TEXT("[OK] add(\"folder\", \"%s\")"), *FolderName.ToString()));
				else
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"folder\", \"%s\") -> creation failed"), *FolderName.ToString()));
				return bOK ? sol::make_object(Lua, true) : sol::lua_nil;
			}

			// ---- add("light", {type=..., location?, rotation?, intensity?, color?}) ----
			if (FType.Equals(TEXT("light"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"light\") -> params required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string LightType = P.get_or("type", std::string("point"));

				FVector Location = FVector::ZeroVector;
				FRotator Rotation = FRotator::ZeroRotator;
				if (auto L = P.get<sol::optional<sol::table>>("location")) Location = TableToVector(L.value());
				if (auto R = P.get<sol::optional<sol::table>>("rotation")) Rotation = TableToRotator(R.value());

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDAddLight", "Level Design: Add Light"));

				UEditorActorSubsystem* ActorSub = GetActorSubsystem();
				AActor* LightActor = nullptr;
				FString FLightType = UTF8_TO_TCHAR(LightType.c_str());

				if (FLightType.Equals(TEXT("point"), ESearchCase::IgnoreCase))
					LightActor = ActorSub->SpawnActorFromClass(APointLight::StaticClass(), Location, Rotation);
				else if (FLightType.Equals(TEXT("spot"), ESearchCase::IgnoreCase))
					LightActor = ActorSub->SpawnActorFromClass(ASpotLight::StaticClass(), Location, Rotation);
				else if (FLightType.Equals(TEXT("directional"), ESearchCase::IgnoreCase))
					LightActor = ActorSub->SpawnActorFromClass(ADirectionalLight::StaticClass(), Location, Rotation);
				else if (FLightType.Equals(TEXT("sky"), ESearchCase::IgnoreCase))
					LightActor = ActorSub->SpawnActorFromClass(ASkyLight::StaticClass(), Location, Rotation);
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"light\") -> unknown type '%s' (point|spot|directional|sky)"), *FLightType));
					return sol::lua_nil;
				}

				if (!LightActor) { Session.Log(TEXT("[FAIL] add(\"light\") -> spawn failed")); return sol::lua_nil; }

				// Set intensity and color
				if (ULightComponent* LC = LightActor->FindComponentByClass<ULightComponent>())
				{
					if (auto I = P.get<sol::optional<double>>("intensity")) LC->SetIntensity(static_cast<float>(I.value()));
					std::string ColorStr = P.get_or("color", std::string());
					if (!ColorStr.empty())
					{
						FString FColorStr = UTF8_TO_TCHAR(ColorStr.c_str());
						// Support both "R,G,B" CSV format and UE "(R=255,G=255,B=255)" format
						FColor C;
						if (FColorStr.Contains(TEXT(",")))
						{
							TArray<FString> Parts;
							FColorStr.ParseIntoArray(Parts, TEXT(","), true);
							if (Parts.Num() >= 3)
								C = FColor(FCString::Atoi(*Parts[0]), FCString::Atoi(*Parts[1]), FCString::Atoi(*Parts[2]));
						}
						else
						{
							C.InitFromString(FColorStr);
						}
						LC->SetLightColor(FLinearColor(C));
					}
				}

				std::string FolderStr = P.get_or("folder", std::string());
				if (!FolderStr.empty()) LightActor->SetFolderPath(FName(UTF8_TO_TCHAR(FolderStr.c_str())));
				std::string LabelStr = P.get_or("label", std::string());
				if (!LabelStr.empty()) LightActor->SetActorLabel(UTF8_TO_TCHAR(LabelStr.c_str()));

				Session.Log(FString::Printf(TEXT("[OK] add(\"light\", \"%s\") -> %s"),
					*FLightType, *LightActor->GetActorLabel()));
				return ActorToTable(Lua, LightActor);
			}

			// ---- add("landscape", {size, scale?, material?, noise?, flat_height?}) ----
			if (FType.Equals(TEXT("landscape"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"landscape\") -> params required")); return sol::lua_nil; }
				sol::table P = Params.value();
				int32 Size = P.get_or("size", 505);

				// Validate: size must be (N * Q + 1) for valid landscape
				// Common: 127, 253, 505, 1009, 2017, 4033, 8129
				// Engine hard limit: 8191 vertices per side (256 components max)
				if (Size < 2) { Session.Log(TEXT("[FAIL] add(\"landscape\") -> size must be >= 2")); return sol::lua_nil; }
				if (Size > 8191) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"landscape\") -> size %d exceeds engine max 8191. For larger worlds, create multiple landscapes or use World Partition streaming proxies"), Size)); return sol::lua_nil; }

				FVector Scale(100.0, 100.0, 100.0);
				if (auto ST = P.get<sol::optional<sol::table>>("scale")) Scale = TableToVector(ST.value());

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDAddLandscape", "Level Design: Add Landscape"));

				int32 Width = Size;
				int32 Height = Size;
				double FlatHeight = P.get_or("flat_height", 0.0);
				bool bHasNoise = P.get<sol::optional<sol::table>>("noise").has_value();
				bool bHasCustomHeight = bHasNoise || FlatHeight != 0.0;

				// Only generate height data if noise or non-zero flat_height requested
				// Otherwise Import() creates a flat landscape at mid-height (much faster)
				TArray<uint16> HeightData;
				if (bHasCustomHeight)
				{
					HeightData.SetNum(Width * Height);
					if (bHasNoise)
					{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
						sol::table N = P.get<sol::table>("noise");
						double Amplitude = N.get_or("amplitude", 5000.0);
						double Frequency = N.get_or("frequency", 0.005);
						int32 Octaves = N.get_or("octaves", 4);
						double Seed = N.get_or("seed", 0.0);
						double Lacunarity = N.get_or("lacunarity", 2.0);
						double Gain = N.get_or("gain", 0.5);
						std::string NoiseMode = N.get_or("mode", std::string("standard"));

						// Convert amplitude from world units to local height units
						// GetTexHeight expects local units where full range is ±256 (LANDSCAPE_INV_ZSCALE=128)
						// User specifies world units (e.g., 5000 = 50m at Z scale 100)
						// Local = World / ZScale
						double LocalAmplitude = Amplitude / FMath::Max(1.0, static_cast<double>(Scale.Z));

						UE::Geometry::EFBMMode FBMMode = UE::Geometry::EFBMMode::Standard;
						if (NoiseMode == "turbulent") FBMMode = UE::Geometry::EFBMMode::Turbulent;
						else if (NoiseMode == "ridge") FBMMode = UE::Geometry::EFBMMode::Ridge;

						for (int32 Y = 0; Y < Height; ++Y)
						{
							for (int32 X = 0; X < Width; ++X)
							{
								double NoiseVal = UE::Geometry::FractalBrownianMotionNoise<double>(
									FBMMode,
									static_cast<uint32>(Octaves),
									FVector2d((X + Seed) * Frequency, (Y + Seed) * Frequency),
									Lacunarity,
									Gain,
									0.0, // Smoothness
									1.0  // Gamma
								);
								double Total = NoiseVal * LocalAmplitude + FlatHeight;
								HeightData[Y * Width + X] = LandscapeDataAccess::GetTexHeight(static_cast<float>(Total));
							}
						}
#else
						Session.Log(TEXT("[FAIL] create_landscape noise requires UE 5.6+"));
						uint16 BaseHeight = LandscapeDataAccess::GetTexHeight(static_cast<float>(FlatHeight));
						for (int32 I = 0; I < HeightData.Num(); ++I)
							HeightData[I] = BaseHeight;
#endif
					}
					else
					{
						uint16 BaseHeight = LandscapeDataAccess::GetTexHeight(static_cast<float>(FlatHeight));
						for (int32 I = 0; I < HeightData.Num(); ++I)
							HeightData[I] = BaseHeight;
					}
				}

				// Auto-choose optimal component sizes for the given resolution
				int32 QuadsPerSection = 63;
				int32 SectionsPerComponent = 1;
				FIntPoint ComponentCount(0, 0);
				FLandscapeImportHelper::ChooseBestComponentSizeForImport(Width, Height, QuadsPerSection, SectionsPerComponent, ComponentCount);
				int32 ComponentCountX = ComponentCount.X;
				int32 ComponentCountY = ComponentCount.Y;

				// Adjust size to fit exactly
				int32 ActualWidth = ComponentCountX * QuadsPerSection * SectionsPerComponent + 1;
				int32 ActualHeight = ComponentCountY * QuadsPerSection * SectionsPerComponent + 1;
				if (ActualWidth > 8191 || ActualHeight > 8191)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"landscape\") -> resampled size %dx%d exceeds 8191 vertex limit"), ActualWidth, ActualHeight));
					return sol::lua_nil;
				}
				if (ActualWidth != Width || ActualHeight != Height)
				{
					// Resample height data to fit
					TArray<uint16> Resampled;
					Resampled.SetNum(ActualWidth * ActualHeight);
					for (int32 Y = 0; Y < ActualHeight; ++Y)
					{
						for (int32 X = 0; X < ActualWidth; ++X)
						{
							int32 SrcX = FMath::Min(X * (Width - 1) / FMath::Max(1, ActualWidth - 1), Width - 1);
							int32 SrcY = FMath::Min(Y * (Height - 1) / FMath::Max(1, ActualHeight - 1), Height - 1);
							Resampled[Y * ActualWidth + X] = HeightData[SrcY * Width + SrcX];
						}
					}
					HeightData = MoveTemp(Resampled);
					Width = ActualWidth;
					Height = ActualHeight;
				}

				// Spawn landscape actor
				FVector Location = FVector::ZeroVector;
				if (auto LT = P.get<sol::optional<sol::table>>("location")) Location = TableToVector(LT.value());
				ALandscape* Landscape = W->SpawnActor<ALandscape>(Location, FRotator::ZeroRotator);
				if (!Landscape) { Session.Log(TEXT("[FAIL] add(\"landscape\") -> spawn failed")); return sol::lua_nil; }

				Landscape->SetActorScale3D(Scale);

				// Import — match editor pattern: empty heightmap for initial creation,
				// then sculpt in height data after. Engine fills default mid-height.
				FGuid LandscapeGuid = FGuid::NewGuid();
				TMap<FGuid, TArray<uint16>> HeightDataPerLayers;
				HeightDataPerLayers.Add(FGuid(), TArray<uint16>()); // empty = flat at mid-height

				TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayerDataPerLayers;
				MaterialLayerDataPerLayers.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());

				// Auto lighting LOD (engine pattern: prevents lightmass crash)
				Landscape->StaticLightingLOD = FMath::DivideAndRoundUp(
					FMath::CeilLogTwo((Width * Height) / (2048 * 2048) + 1), (uint32)2);

				Landscape->Import(
					LandscapeGuid,
					0, 0, Width - 1, Height - 1,
					SectionsPerComponent, QuadsPerSection,
					HeightDataPerLayers, nullptr,
					MaterialLayerDataPerLayers, ELandscapeImportAlphamapType::Additive,
					{}
				);

				// Apply procedural height data via sculpt interface if noise was requested
				if (bHasCustomHeight)
				{
					ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
					if (Info)
					{
						FGuid EditLayerGuid;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
						// Get the default edit layer GUID — Import() creates it via CreateDefaultLayer()
						const auto& EditLayers = Landscape->GetEditLayers();
						if (EditLayers.Num() > 0 && EditLayers[0])
						{
							EditLayerGuid = EditLayers[0]->GetGuid();
						}
#endif

						{
							// Use explicit edit layer GUID to ensure we write to the correct texture
							// (single-arg constructor uses GetEditingLayer() which may be invalid after Import)
							FLandscapeEditDataInterface EditInterface(Info, EditLayerGuid);
							EditInterface.SetHeightData(0, 0, Width - 1, Height - 1,
								HeightData.GetData(), 0, true, nullptr, nullptr, nullptr,
								false, nullptr, nullptr, true, true, true);
							EditInterface.Flush();
						}
						// Force merge edit layers so the heightmap is visible immediately
						Info->ForceLayersFullUpdate();
					}
				}

				// Note: Import() internally calls CreateLandscapeInfo() + RegisterAllComponents()

				// Apply material
				std::string MatStr = P.get_or("material", std::string());
				if (!MatStr.empty())
				{
					UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, UTF8_TO_TCHAR(MatStr.c_str()));
					if (Mat) Landscape->LandscapeMaterial = Mat;
				}

				std::string FolderStr = P.get_or("folder", std::string());
				if (!FolderStr.empty()) Landscape->SetFolderPath(FName(UTF8_TO_TCHAR(FolderStr.c_str())));
				std::string LabelStr = P.get_or("label", std::string());
				if (!LabelStr.empty()) Landscape->SetActorLabel(UTF8_TO_TCHAR(LabelStr.c_str()));

				Session.Log(FString::Printf(TEXT("[OK] add(\"landscape\") -> %dx%d, %d components"),
					Width, Height, ComponentCountX * ComponentCountY));
				return ActorToTable(Lua, Landscape);
			}

			// ---- add("instances", {mesh=..., transforms=[{location,rotation?,scale?}]}) ----
			if (FType.Equals(TEXT("instances"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"instances\") -> params required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string MeshPath = P.get_or("mesh", std::string());
				if (MeshPath.empty()) { Session.Log(TEXT("[FAIL] add(\"instances\") -> mesh required")); return sol::lua_nil; }

				UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, UTF8_TO_TCHAR(MeshPath.c_str()));
				if (!Mesh) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"instances\") -> mesh '%s' not found"), UTF8_TO_TCHAR(MeshPath.c_str()))); return sol::lua_nil; }

				sol::optional<sol::table> TransformsOpt = P.get<sol::optional<sol::table>>("transforms");
				if (!TransformsOpt.has_value()) { Session.Log(TEXT("[FAIL] add(\"instances\") -> transforms array required")); return sol::lua_nil; }

				TArray<FTransform> Transforms;
				for (auto& Pair : TransformsOpt.value())
				{
					if (!Pair.second.is<sol::table>()) continue;
					sol::table T = Pair.second.as<sol::table>();
					FVector Loc = FVector::ZeroVector;
					FRotator Rot = FRotator::ZeroRotator;
					FVector Scl = FVector::OneVector;
					if (auto LT = T.get<sol::optional<sol::table>>("location")) Loc = TableToVector(LT.value());
					else { Loc.X = T.get_or("x", 0.0); Loc.Y = T.get_or("y", 0.0); Loc.Z = T.get_or("z", 0.0); }
					if (auto RT = T.get<sol::optional<sol::table>>("rotation")) Rot = TableToRotator(RT.value());
					if (auto ST = T.get<sol::optional<sol::table>>("scale")) Scl = TableToVector(ST.value());
					Transforms.Add(FTransform(Rot, Loc, Scl));
				}

				if (Transforms.Num() == 0) { Session.Log(TEXT("[FAIL] add(\"instances\") -> no valid transforms")); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDAddInstances", "Level Design: Add Instances"));

				// Spawn an actor with HISM component
				AActor* ISMActor = W->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator);
				if (!ISMActor) { Session.Log(TEXT("[FAIL] add(\"instances\") -> actor spawn failed")); return sol::lua_nil; }
				ISMActor->SetFlags(RF_Transactional);

				UHierarchicalInstancedStaticMeshComponent* HISM = NewObject<UHierarchicalInstancedStaticMeshComponent>(
					ISMActor, NAME_None, RF_Transactional);
				HISM->SetStaticMesh(Mesh);
				HISM->RegisterComponent();
				ISMActor->SetRootComponent(HISM);
				ISMActor->AddInstanceComponent(HISM);

				HISM->AddInstances(Transforms, false, true);

				std::string LabelStr = P.get_or("label", std::string());
				if (!LabelStr.empty()) ISMActor->SetActorLabel(UTF8_TO_TCHAR(LabelStr.c_str()));
				std::string FolderStr = P.get_or("folder", std::string());
				if (!FolderStr.empty()) ISMActor->SetFolderPath(FName(UTF8_TO_TCHAR(FolderStr.c_str())));

				Session.Log(FString::Printf(TEXT("[OK] add(\"instances\") -> %d instances of %s"),
					Transforms.Num(), UTF8_TO_TCHAR(MeshPath.c_str())));

				sol::table R = ActorToTable(Lua, ISMActor);
				R["instance_count"] = Transforms.Num();
				return R;
			}

			// ---- add("foliage", {mesh=..., transforms=[{location,rotation?,scale?}]}) ----
			if (FType.Equals(TEXT("foliage"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"foliage\") -> params required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string MeshPath = P.get_or("mesh", std::string());
				if (MeshPath.empty()) { Session.Log(TEXT("[FAIL] add(\"foliage\") -> mesh required")); return sol::lua_nil; }

				UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, UTF8_TO_TCHAR(MeshPath.c_str()));
				if (!Mesh) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"foliage\") -> mesh '%s' not found"), UTF8_TO_TCHAR(MeshPath.c_str()))); return sol::lua_nil; }

				sol::optional<sol::table> TransformsOpt = P.get<sol::optional<sol::table>>("transforms");
				if (!TransformsOpt.has_value()) { Session.Log(TEXT("[FAIL] add(\"foliage\") -> transforms array required")); return sol::lua_nil; }

				TArray<FTransform> Transforms;
				for (auto& Pair : TransformsOpt.value())
				{
					if (!Pair.second.is<sol::table>()) continue;
					sol::table T = Pair.second.as<sol::table>();
					FVector Loc = FVector::ZeroVector;
					FRotator Rot = FRotator::ZeroRotator;
					FVector Scl = FVector::OneVector;
					if (auto LT = T.get<sol::optional<sol::table>>("location")) Loc = TableToVector(LT.value());
					else { Loc.X = T.get_or("x", 0.0); Loc.Y = T.get_or("y", 0.0); Loc.Z = T.get_or("z", 0.0); }
					if (auto RT = T.get<sol::optional<sol::table>>("rotation")) Rot = TableToRotator(RT.value());
					if (auto ST = T.get<sol::optional<sol::table>>("scale")) Scl = TableToVector(ST.value());
					Transforms.Add(FTransform(Rot, Loc, Scl));
				}

				if (Transforms.Num() == 0) { Session.Log(TEXT("[FAIL] add(\"foliage\") -> no valid transforms")); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDAddFoliage", "Level Design: Add Foliage"));

				// Get or create foliage actor first (needed as outer for foliage type)
				AInstancedFoliageActor* IFA = AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel(W, true);
				if (!IFA) { Session.Log(TEXT("[FAIL] add(\"foliage\") -> no foliage actor")); return sol::lua_nil; }

				// Reuse existing foliage type for this mesh if one already exists
				UFoliageType_InstancedStaticMesh* FoliageType = nullptr;
				FFoliageInfo* ExistingInfo = nullptr;
				IFA->ForEachFoliageInfo([&](UFoliageType* FT, FFoliageInfo& FI) -> bool
				{
					if (UFoliageType_InstancedStaticMesh* FTISM = Cast<UFoliageType_InstancedStaticMesh>(FT))
					{
						if (FTISM->Mesh == Mesh)
						{
							FoliageType = FTISM;
							ExistingInfo = &FI;
							return false; // stop
						}
					}
					return true;
				});

				IFA->Modify();

				if (!FoliageType)
				{
					// Create new foliage type with IFA as outer to prevent GC
					FoliageType = NewObject<UFoliageType_InstancedStaticMesh>(IFA);
					FoliageType->Mesh = Mesh;
				}

				// Add via exported FFoliageInfo API
				FFoliageInfo& Info = ExistingInfo ? *ExistingInfo : *IFA->AddFoliageInfo(FoliageType);
				for (const FTransform& Xform : Transforms)
				{
					FFoliageInstance Inst;
					Inst.Location = Xform.GetLocation();
					Inst.Rotation = Xform.Rotator();
					Inst.DrawScale3D = FVector3f(Xform.GetScale3D());
					Info.AddInstance(FoliageType, Inst);
				}

				Session.Log(FString::Printf(TEXT("[OK] add(\"foliage\") -> %d instances of %s"),
					Transforms.Num(), UTF8_TO_TCHAR(MeshPath.c_str())));

				sol::table R = Lua.create_table();
				R["mesh"] = MeshPath;
				R["count"] = Transforms.Num();
				return R;
			}

			// ---- add("spline", {actor, component?, points, closed?}) ----
			if (FType.Equals(TEXT("spline"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"spline\") -> params required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string ActorName = P.get_or("actor", std::string());
				if (ActorName.empty()) { Session.Log(TEXT("[FAIL] add(\"spline\") -> actor required")); return sol::lua_nil; }

				AActor* Actor = FindActorByNameOrLabel(W, UTF8_TO_TCHAR(ActorName.c_str()));
				if (!Actor) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"spline\") -> actor '%s' not found"), UTF8_TO_TCHAR(ActorName.c_str()))); return sol::lua_nil; }

				// Find spline component
				USplineComponent* Spline = nullptr;
				std::string CompName = P.get_or("component", std::string());
				if (!CompName.empty())
				{
					FString FCompName = UTF8_TO_TCHAR(CompName.c_str());
					TInlineComponentArray<USplineComponent*> Splines;
					Actor->GetComponents(Splines);
					for (USplineComponent* SC : Splines)
					{
						if (SC->GetName().Equals(FCompName, ESearchCase::IgnoreCase))
						{
							Spline = SC;
							break;
						}
					}
				}
				else
				{
					Spline = Actor->FindComponentByClass<USplineComponent>();
				}

				if (!Spline) { Session.Log(TEXT("[FAIL] add(\"spline\") -> no SplineComponent found")); return sol::lua_nil; }

				sol::optional<sol::table> PointsOpt = P.get<sol::optional<sol::table>>("points");
				if (!PointsOpt.has_value()) { Session.Log(TEXT("[FAIL] add(\"spline\") -> points array required")); return sol::lua_nil; }

				TArray<FVector> Points;
				for (auto& Pair : PointsOpt.value())
				{
					if (!Pair.second.is<sol::table>()) continue;
					sol::table PT = Pair.second.as<sol::table>();
					Points.Add(TableToVector(PT));
				}

				if (Points.Num() == 0) { Session.Log(TEXT("[FAIL] add(\"spline\") -> no valid points")); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDSetSpline", "Level Design: Set Spline Points"));
				Spline->Modify();
				Spline->SetSplinePoints(Points, ESplineCoordinateSpace::World, false);

				bool bClosed = P.get_or("closed", false);
				Spline->SetClosedLoop(bClosed, false);
				Spline->UpdateSpline();

				Session.Log(FString::Printf(TEXT("[OK] add(\"spline\") -> %d points on %s:%s"),
					Points.Num(), *Actor->GetActorLabel(), *Spline->GetName()));

				sol::table R = Lua.create_table();
				R["point_count"] = Points.Num();
				R["closed"] = bClosed;
				return R;
			}

			// ---- add("landscape_spline", {landscape?, points=[{x,y,z},...], width?, falloff?, layer?}) ----
			if (FType.Equals(TEXT("landscape_spline"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"landscape_spline\") -> {points=...} required")); return sol::lua_nil; }
				sol::table P = Params.value();

				int32 LandscapeIndex = P.get_or("landscape", 0);
				ALandscapeProxy* Target = FindLandscapeByIndex(W, LandscapeIndex);
				if (!Target) { Session.Log(TEXT("[FAIL] add(\"landscape_spline\") -> landscape not found")); return sol::lua_nil; }

				sol::optional<sol::table> PointsOpt = P.get<sol::optional<sol::table>>("points");
				if (!PointsOpt.has_value()) { Session.Log(TEXT("[FAIL] add(\"landscape_spline\") -> points array required")); return sol::lua_nil; }
				sol::table PointsTable = PointsOpt.value();

				TArray<FVector> Points;
				for (auto& KV : PointsTable)
				{
					if (KV.second.is<sol::table>())
						Points.Add(TableToVector(KV.second.as<sol::table>()));
				}
				if (Points.Num() < 2) { Session.Log(TEXT("[FAIL] add(\"landscape_spline\") -> at least 2 points required")); return sol::lua_nil; }

				float SplineWidth = static_cast<float>(P.get_or("width", 500.0));
				float SideFalloff = static_cast<float>(P.get_or("falloff", 200.0));
				std::string LayerStr = P.get_or("layer", std::string());

				ULandscapeSplinesComponent* Splines = Target->GetSplinesComponent();
				if (!Splines)
				{
					// Create a new splines component if none exists
					Splines = NewObject<ULandscapeSplinesComponent>(Target, NAME_None, RF_Transactional);
					Splines->AttachToComponent(Target->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
					Splines->RegisterComponent();
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDAddLandSpline", "Level Design: Add Landscape Spline"));
				Splines->Modify();

				// Create control points
				TArray<ULandscapeSplineControlPoint*> CPs;
				FTransform LandscapeToWorld = Target->LandscapeActorToWorld();
				FTransform WorldToLandscape = LandscapeToWorld.Inverse();

				for (const FVector& Pt : Points)
				{
					ULandscapeSplineControlPoint* CP = NewObject<ULandscapeSplineControlPoint>(
						Splines, NAME_None, RF_Transactional | RF_TextExportTransient);
					CP->Location = WorldToLandscape.TransformPosition(Pt);
					CP->Width = SplineWidth;
					CP->SideFalloff = SideFalloff;
#if WITH_EDITORONLY_DATA
					if (!LayerStr.empty()) CP->LayerName = FName(UTF8_TO_TCHAR(LayerStr.c_str()));
#endif
					Splines->GetControlPoints().Add(CP);
					CPs.Add(CP);
				}

				// Create segments connecting consecutive control points
				for (int32 I = 0; I < CPs.Num() - 1; ++I)
				{
					ULandscapeSplineSegment* Seg = NewObject<ULandscapeSplineSegment>(
						Splines, NAME_None, RF_Transactional | RF_TextExportTransient);
					Seg->Connections[0].ControlPoint = CPs[I];
					Seg->Connections[0].TangentLen = SplineWidth;
					Seg->Connections[1].ControlPoint = CPs[I + 1];
					Seg->Connections[1].TangentLen = SplineWidth;

					// Register connections on control points
					CPs[I]->ConnectedSegments.Add(FLandscapeSplineConnection(Seg, 0));
					CPs[I + 1]->ConnectedSegments.Add(FLandscapeSplineConnection(Seg, 1));

					Splines->GetSegments().Add(Seg);
				}

				Splines->RebuildAllSplines();
				Splines->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add(\"landscape_spline\") -> %d control points, %d segments"),
					CPs.Num(), CPs.Num() - 1));

				sol::table R = Lua.create_table();
				R["points"] = CPs.Num();
				R["segments"] = CPs.Num() - 1;
				return R;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type (actor|landscape|foliage|instances|folder|light|spline|landscape_spline)"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// remove(type, id)
		// ================================================================
		LevelObj.set_function("remove", [&Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());
			UWorld* W = GetEditorWorld();
			if (!W) { Session.Log(TEXT("[FAIL] remove -> no editor world")); return sol::lua_nil; }

			// ---- remove("actor", "Name") ----
			if (FType.Equals(TEXT("actor"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"actor\") -> name/label string required")); return sol::lua_nil; }
				FString ActorId = UTF8_TO_TCHAR(Id.as<std::string>().c_str());

				AActor* Actor = FindActorByNameOrLabel(W, ActorId);
				if (!Actor) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"actor\", \"%s\") -> not found"), *ActorId)); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDRemoveActor", "Level Design: Remove Actor"));

				UEditorActorSubsystem* ActorSub = GetActorSubsystem();
				if (ActorSub && ActorSub->DestroyActor(Actor))
				{
					Session.Log(FString::Printf(TEXT("[OK] remove(\"actor\", \"%s\")"), *ActorId));
					return sol::make_object(Lua, true);
				}
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"actor\", \"%s\") -> destroy failed"), *ActorId));
				return sol::lua_nil;
			}

			// ---- remove("folder", "path") ----
			if (FType.Equals(TEXT("folder"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"folder\") -> path string required")); return sol::lua_nil; }
				FString FolderPath = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				FFolder WorldRoot = FFolder::GetWorldRootFolder(W);
				FFolder FolderToDelete(WorldRoot.GetRootObject(), ::FName(*FolderPath));
				if (!FActorFolders::Get().ContainsFolder(*W, FolderToDelete))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"folder\", \"%s\") -> folder not found"), *FolderPath));
					return sol::lua_nil;
				}
				FActorFolders::Get().DeleteFolder(*W, FolderToDelete);
				Session.Log(FString::Printf(TEXT("[OK] remove(\"folder\", \"%s\")"), *FolderPath));
				return sol::make_object(Lua, true);
			}

			// ---- remove("foliage", {mesh=..., region?={x1,y1,x2,y2}}) ----
			if (FType.Equals(TEXT("foliage"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<sol::table>()) { Session.Log(TEXT("[FAIL] remove(\"foliage\") -> {mesh=..} required")); return sol::lua_nil; }
				sol::table P = Id.as<sol::table>();
				std::string MeshPath = P.get_or("mesh", std::string());
				if (MeshPath.empty()) { Session.Log(TEXT("[FAIL] remove(\"foliage\") -> mesh required")); return sol::lua_nil; }

				UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, UTF8_TO_TCHAR(MeshPath.c_str()));
				if (!Mesh) { Session.Log(TEXT("[FAIL] remove(\"foliage\") -> mesh not found")); return sol::lua_nil; }

				// Find the foliage info matching this mesh and remove all instances
				AInstancedFoliageActor* IFA = AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel(W, false);
				if (!IFA) { Session.Log(TEXT("[FAIL] remove(\"foliage\") -> no foliage actor in level")); return sol::lua_nil; }

				IFA->Modify();
				bool bRemoved = false;
				IFA->ForEachFoliageInfo([&](UFoliageType* FT, FFoliageInfo& Info) -> bool
				{
					if (UFoliageType_InstancedStaticMesh* FTISM = Cast<UFoliageType_InstancedStaticMesh>(FT))
					{
						if (FTISM->Mesh == Mesh)
						{
							// Build index array of all instances
							TArray<int32> AllIndices;
							AllIndices.Reserve(Info.Instances.Num());
							for (int32 i = 0; i < Info.Instances.Num(); ++i)
								AllIndices.Add(i);
							if (AllIndices.Num() > 0)
								Info.RemoveInstances(AllIndices, true);
							bRemoved = true;
						}
					}
					return true; // continue
				});

				if (bRemoved)
					Session.Log(FString::Printf(TEXT("[OK] remove(\"foliage\") -> removed all instances of %s"), UTF8_TO_TCHAR(MeshPath.c_str())));
				else
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"foliage\") -> no foliage found for %s"), UTF8_TO_TCHAR(MeshPath.c_str())));
				return bRemoved ? sol::make_object(Lua, true) : sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type (actor|folder|foliage)"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// list(type?, filter?)
		// ================================================================
		LevelObj.set_function("list", [&Session](sol::table /*self*/,
			sol::optional<std::string> TypeOpt, sol::optional<sol::table> Filter, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UWorld* W = GetEditorWorld();
			if (!W) { Session.Log(TEXT("[FAIL] list -> no editor world")); return sol::lua_nil; }

			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("actors");

			// ---- list("actors", {class?, folder?, name?}) ----
			if (FType.Equals(TEXT("actors"), ESearchCase::IgnoreCase))
			{
				FString ClassFilter, FolderFilter, NameFilter;
				if (Filter.has_value())
				{
					sol::table F = Filter.value();
					if (auto V = F.get<sol::optional<std::string>>("class")) ClassFilter = UTF8_TO_TCHAR(V.value().c_str());
					if (auto V = F.get<sol::optional<std::string>>("folder")) FolderFilter = UTF8_TO_TCHAR(V.value().c_str());
					if (auto V = F.get<sol::optional<std::string>>("name")) NameFilter = UTF8_TO_TCHAR(V.value().c_str());
				}

				sol::table Results = Lua.create_table();
				int32 Idx = 1;

				for (TActorIterator<AActor> It(W); It; ++It)
				{
					AActor* A = *It;
					if (!A || A->IsA<AWorldSettings>()) continue;

					// Apply class filter
					if (!ClassFilter.IsEmpty() && !A->GetClass()->GetName().Contains(ClassFilter)) continue;

					// Apply folder filter
					if (!FolderFilter.IsEmpty())
					{
						FString AF = A->GetFolderPath().ToString();
						if (!AF.Contains(FolderFilter)) continue;
					}

					// Apply name filter
					if (!NameFilter.IsEmpty())
					{
						if (!A->GetActorLabel().Contains(NameFilter) && !A->GetName().Contains(NameFilter)) continue;
					}

					Results[Idx++] = ActorToTable(Lua, A);
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"actors\") -> %d"), Idx - 1));
				return Results;
			}

			// ---- list("folders") ----
			if (FType.Equals(TEXT("folders"), ESearchCase::IgnoreCase))
			{
				sol::table Results = Lua.create_table();
				int32 Idx = 1;
				FActorFolders::Get().ForEachFolder(*W, [&](const FFolder& Folder) -> bool
				{
					Results[Idx++] = TCHAR_TO_UTF8(*Folder.GetPath().ToString());
					return true; // continue
				});
				Session.Log(FString::Printf(TEXT("[OK] list(\"folders\") -> %d"), Idx - 1));
				return Results;
			}

			// ---- list("landscapes") ----
			if (FType.Equals(TEXT("landscapes"), ESearchCase::IgnoreCase))
			{
				sol::table Results = Lua.create_table();
				int32 Idx = 1;
				for (TActorIterator<ALandscapeProxy> It(W); It; ++It)
				{
					ALandscapeProxy* LP = *It;
					sol::table Entry = ActorToTable(Lua, LP);

					ULandscapeInfo* Info = LP->GetLandscapeInfo();
					if (Info)
					{
						int32 MinX, MinY, MaxX, MaxY;
						if (Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY))
						{
							Entry["extent_min"] = VectorToTable(Lua, FVector(MinX, MinY, 0));
							Entry["extent_max"] = VectorToTable(Lua, FVector(MaxX, MaxY, 0));
							Entry["resolution_x"] = MaxX - MinX + 1;
							Entry["resolution_y"] = MaxY - MinY + 1;
						}
					}
					Results[Idx++] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"landscapes\") -> %d"), Idx - 1));
				return Results;
			}

			// ---- list("foliage") ----
			if (FType.Equals(TEXT("foliage"), ESearchCase::IgnoreCase))
			{
				sol::table Results = Lua.create_table();
				int32 Idx = 1;
				for (TActorIterator<AInstancedFoliageActor> It(W); It; ++It)
				{
					AInstancedFoliageActor* IFA = *It;
					IFA->ForEachFoliageInfo([&](UFoliageType* FoliageType, FFoliageInfo& Info)
					{
						sol::table Entry = Lua.create_table();
						Entry["type"] = TCHAR_TO_UTF8(*FoliageType->GetName());
						if (UFoliageType_InstancedStaticMesh* FISM = Cast<UFoliageType_InstancedStaticMesh>(FoliageType))
						{
							if (FISM->Mesh)
								Entry["mesh"] = TCHAR_TO_UTF8(*FISM->Mesh->GetPathName());
						}
						if (UHierarchicalInstancedStaticMeshComponent* Comp = Info.GetComponent())
							Entry["instance_count"] = Comp->GetInstanceCount();
						Results[Idx++] = Entry;
						return true; // continue
					});
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"foliage\") -> %d types"), Idx - 1));
				return Results;
			}

			// ---- list("landscape_splines") ----
			if (FType.Equals(TEXT("landscape_splines"), ESearchCase::IgnoreCase))
			{
				sol::table Results = Lua.create_table();
				int32 Idx = 1;
				for (TActorIterator<ALandscapeProxy> It(W); It; ++It)
				{
					ULandscapeSplinesComponent* Splines = (*It)->GetSplinesComponent();
					if (!Splines) continue;
					const auto& CPs = Splines->GetControlPoints();
					const auto& Segs = Splines->GetSegments();
					for (int32 SegIdx = 0; SegIdx < Segs.Num(); ++SegIdx)
					{
						sol::table Entry = Lua.create_table();
						ULandscapeSplineSegment* Seg = Segs[SegIdx];
						if (Seg && Seg->Connections[0].ControlPoint && Seg->Connections[1].ControlPoint)
						{
							FTransform LtoW = (*It)->LandscapeActorToWorld();
							Entry["start"] = VectorToTable(Lua, LtoW.TransformPosition(Seg->Connections[0].ControlPoint->Location));
							Entry["end"] = VectorToTable(Lua, LtoW.TransformPosition(Seg->Connections[1].ControlPoint->Location));
							Entry["width"] = Seg->Connections[0].ControlPoint->Width;
							Entry["landscape"] = TCHAR_TO_UTF8(*(*It)->GetActorLabel());
						}
						Results[Idx++] = Entry;
					}
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"landscape_splines\") -> %d segments"), Idx - 1));
				return Results;
			}

			// ---- list("landscape_layers", {landscape?=0}) ----
			if (FType.Equals(TEXT("landscape_layers"), ESearchCase::IgnoreCase))
			{
				int32 LandscapeIdx = 0;
				if (Filter.has_value()) LandscapeIdx = Filter.value().get_or("landscape", 0);

				ALandscapeProxy* LP = FindLandscapeByIndex(W, LandscapeIdx);
				if (!LP) { Session.Log(TEXT("[FAIL] list(\"landscape_layers\") -> no landscape found")); return sol::lua_nil; }

				ULandscapeInfo* Info = LP->GetLandscapeInfo();
				if (!Info) { Session.Log(TEXT("[FAIL] list(\"landscape_layers\") -> no landscape info")); return sol::lua_nil; }

				sol::table Results = Lua.create_table();
				int32 Idx = 1;
				for (const FLandscapeInfoLayerSettings& Layer : Info->Layers)
				{
					if (!Layer.LayerInfoObj) continue;
					sol::table Entry = Lua.create_table();
					Entry["name"] = TCHAR_TO_UTF8(*Layer.LayerName.ToString());
					Entry["layer_info"] = TCHAR_TO_UTF8(*Layer.LayerInfoObj->GetPathName());
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
					if (Layer.LayerInfoObj->GetPhysicalMaterial())
						Entry["physical_material"] = TCHAR_TO_UTF8(*Layer.LayerInfoObj->GetPhysicalMaterial()->GetPathName());
#else
					if (Layer.LayerInfoObj->PhysMaterial)
						Entry["physical_material"] = TCHAR_TO_UTF8(*Layer.LayerInfoObj->PhysMaterial->GetPathName());
#endif
					Results[Idx++] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"landscape_layers\") -> %d layers"), Idx - 1));
				return Results;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type (actors|folders|landscapes|foliage|landscape_splines|landscape_layers)"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// get_heightmap({landscape?=0, region?={x1,y1,x2,y2}, sample_step?=1}) -> table of heights
		// Reads landscape height data. Returns array of {x, y, height} in world units.
		// sample_step controls sampling resolution (1=every vertex, 4=every 4th vertex)
		// ================================================================
		LevelObj.set_function("get_heightmap", [&Session](sol::table /*self*/,
			sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UWorld* W = GetEditorWorld();
			if (!W) { Session.Log(TEXT("[FAIL] get_heightmap -> no editor world")); return sol::lua_nil; }

			int32 LandscapeIdx = 0;
			int32 SampleStep = 1;
			bool bHasRegion = false;
			int32 RX1 = 0, RY1 = 0, RX2 = 0, RY2 = 0;

			if (Params.has_value())
			{
				sol::table P = Params.value();
				LandscapeIdx = P.get_or("landscape", 0);
				SampleStep = FMath::Max(1, P.get_or("sample_step", 1));
				if (auto R = P.get<sol::optional<sol::table>>("region"))
				{
					bHasRegion = true;
					RX1 = static_cast<int32>(R.value().get_or("x1", 0.0));
					RY1 = static_cast<int32>(R.value().get_or("y1", 0.0));
					RX2 = static_cast<int32>(R.value().get_or("x2", 0.0));
					RY2 = static_cast<int32>(R.value().get_or("y2", 0.0));
				}
			}

			ALandscapeProxy* LP = FindLandscapeByIndex(W, LandscapeIdx);
			if (!LP) { Session.Log(TEXT("[FAIL] get_heightmap -> no landscape found")); return sol::lua_nil; }

			ULandscapeInfo* Info = LP->GetLandscapeInfo();
			if (!Info) { Session.Log(TEXT("[FAIL] get_heightmap -> no landscape info")); return sol::lua_nil; }

			// Get landscape extent if no region specified
			int32 MinX, MinY, MaxX, MaxY;
			if (!Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY))
			{
				Session.Log(TEXT("[FAIL] get_heightmap -> could not get landscape extent"));
				return sol::lua_nil;
			}

			if (bHasRegion)
			{
				MinX = FMath::Max(MinX, RX1);
				MinY = FMath::Max(MinY, RY1);
				MaxX = FMath::Min(MaxX, RX2);
				MaxY = FMath::Min(MaxY, RY2);
			}

			int32 Width = MaxX - MinX + 1;
			int32 Height = MaxY - MinY + 1;

			// Safety cap: max 2048x2048 at step=1
			if ((Width / SampleStep) * (Height / SampleStep) > 2048 * 2048)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] get_heightmap -> region too large (%dx%d at step=%d). Use a smaller region or larger sample_step"),
					Width, Height, SampleStep));
				return sol::lua_nil;
			}

			// Read height data
			TArray<uint16> HeightData;
			HeightData.SetNum(Width * Height);
			{
				FLandscapeEditDataInterface ReadInterface(Info);
				ReadInterface.GetHeightDataFast(MinX, MinY, MaxX, MaxY, HeightData.GetData(), 0);
			}

			// Convert to world heights and return as table
			FTransform LtoW = LP->LandscapeActorToWorld();
			FVector Scale = LtoW.GetScale3D();

			sol::table Result = Lua.create_table();
			Result["min_x"] = MinX;
			Result["min_y"] = MinY;
			Result["max_x"] = MaxX;
			Result["max_y"] = MaxY;
			Result["sample_step"] = SampleStep;

			sol::table Heights = Lua.create_table();
			int32 Idx = 1;
			for (int32 Y = 0; Y < Height; Y += SampleStep)
			{
				for (int32 X = 0; X < Width; X += SampleStep)
				{
					uint16 RawHeight = HeightData[Y * Width + X];
					// Convert uint16 to world height: center at 32768, scale by Z
					double WorldHeight = (static_cast<double>(RawHeight) - 32768.0) * Scale.Z / 128.0;
					Heights[Idx++] = WorldHeight;
				}
			}
			Result["heights"] = Heights;
			Result["width"] = (Width + SampleStep - 1) / SampleStep;
			Result["height"] = (Height + SampleStep - 1) / SampleStep;

			Session.Log(FString::Printf(TEXT("[OK] get_heightmap -> %dx%d samples (%d total, step=%d)"),
				(Width + SampleStep - 1) / SampleStep, (Height + SampleStep - 1) / SampleStep, Idx - 1, SampleStep));
			return Result;
		});

		// ================================================================
		// get_weightmap({landscape?=0, layer="LayerName", region?={x1,y1,x2,y2}, sample_step?=1}) -> table of weights
		// Reads landscape paint layer weights (0-255) for a specific layer.
		// ================================================================
		LevelObj.set_function("get_weightmap", [&Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UWorld* W = GetEditorWorld();
			if (!W) { Session.Log(TEXT("[FAIL] get_weightmap -> no editor world")); return sol::lua_nil; }

			int32 LandscapeIdx = Params.get_or("landscape", 0);
			std::string LayerStr = Params.get_or("layer", std::string());
			int32 SampleStep = FMath::Max(1, Params.get_or("sample_step", 1));

			if (LayerStr.empty())
			{
				Session.Log(TEXT("[FAIL] get_weightmap -> 'layer' name required"));
				return sol::lua_nil;
			}

			ALandscapeProxy* LP = FindLandscapeByIndex(W, LandscapeIdx);
			if (!LP) { Session.Log(TEXT("[FAIL] get_weightmap -> no landscape found")); return sol::lua_nil; }

			ULandscapeInfo* Info = LP->GetLandscapeInfo();
			if (!Info) { Session.Log(TEXT("[FAIL] get_weightmap -> no landscape info")); return sol::lua_nil; }

			// Find the layer info object
			FName LayerName = FName(UTF8_TO_TCHAR(LayerStr.c_str()));
			ULandscapeLayerInfoObject* LayerInfo = nullptr;
			for (const FLandscapeInfoLayerSettings& Layer : Info->Layers)
			{
				if (Layer.LayerName == LayerName && Layer.LayerInfoObj)
				{
					LayerInfo = Layer.LayerInfoObj;
					break;
				}
			}
			if (!LayerInfo)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] get_weightmap -> layer '%s' not found"), *LayerName.ToString()));
				return sol::lua_nil;
			}

			// Get extent
			int32 MinX, MinY, MaxX, MaxY;
			if (!Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY))
			{
				Session.Log(TEXT("[FAIL] get_weightmap -> could not get landscape extent"));
				return sol::lua_nil;
			}

			if (auto R = Params.get<sol::optional<sol::table>>("region"))
			{
				MinX = FMath::Max(MinX, static_cast<int32>(R.value().get_or("x1", 0.0)));
				MinY = FMath::Max(MinY, static_cast<int32>(R.value().get_or("y1", 0.0)));
				MaxX = FMath::Min(MaxX, static_cast<int32>(R.value().get_or("x2", 0.0)));
				MaxY = FMath::Min(MaxY, static_cast<int32>(R.value().get_or("y2", 0.0)));
			}

			int32 Width = MaxX - MinX + 1;
			int32 Height = MaxY - MinY + 1;

			if ((Width / SampleStep) * (Height / SampleStep) > 2048 * 2048)
			{
				Session.Log(TEXT("[FAIL] get_weightmap -> region too large. Use smaller region or larger sample_step"));
				return sol::lua_nil;
			}

			// Read alpha/weight data
			TArray<uint8> AlphaData;
			AlphaData.SetNum(Width * Height);
			{
				FLandscapeEditDataInterface ReadInterface(Info);
				ReadInterface.GetWeightDataFast(LayerInfo, MinX, MinY, MaxX, MaxY, AlphaData.GetData(), 0);
			}

			sol::table Result = Lua.create_table();
			Result["layer"] = TCHAR_TO_UTF8(*LayerName.ToString());
			Result["min_x"] = MinX;
			Result["min_y"] = MinY;
			Result["max_x"] = MaxX;
			Result["max_y"] = MaxY;
			Result["sample_step"] = SampleStep;

			sol::table Weights = Lua.create_table();
			int32 Idx = 1;
			for (int32 Y = 0; Y < Height; Y += SampleStep)
			{
				for (int32 X = 0; X < Width; X += SampleStep)
				{
					Weights[Idx++] = static_cast<int>(AlphaData[Y * Width + X]);
				}
			}
			Result["weights"] = Weights;
			Result["width"] = (Width + SampleStep - 1) / SampleStep;
			Result["height"] = (Height + SampleStep - 1) / SampleStep;

			Session.Log(FString::Printf(TEXT("[OK] get_weightmap(\"%s\") -> %dx%d samples"),
				*LayerName.ToString(), (Width + SampleStep - 1) / SampleStep, (Height + SampleStep - 1) / SampleStep));
			return Result;
		});

		// ================================================================
		// configure(type, id, params)
		// ================================================================
		LevelObj.set_function("configure", [&Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());
			UWorld* W = GetEditorWorld();
			if (!W) { Session.Log(TEXT("[FAIL] configure -> no editor world")); return sol::lua_nil; }

			// ---- configure("actor", "Name", {location?, rotation?, scale?, label?, folder?, mesh?, material?, property?}) ----
			if (FType.Equals(TEXT("actor"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] configure(\"actor\") -> name/label required")); return sol::lua_nil; }
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] configure(\"actor\") -> params required")); return sol::lua_nil; }

				FString ActorId = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				AActor* Actor = FindActorByNameOrLabel(W, ActorId);
				if (!Actor) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"actor\", \"%s\") -> not found"), *ActorId)); return sol::lua_nil; }

				sol::table P = Params.value();
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDConfigure", "Level Design: Configure Actor"));
				Actor->Modify();

				TArray<FString> Changes;

				if (auto LT = P.get<sol::optional<sol::table>>("location"))
				{
					Actor->SetActorLocation(TableToVector(LT.value()));
					Changes.Add(TEXT("location"));
				}
				if (auto RT = P.get<sol::optional<sol::table>>("rotation"))
				{
					Actor->SetActorRotation(TableToRotator(RT.value()));
					Changes.Add(TEXT("rotation"));
				}
				if (auto ST = P.get<sol::optional<sol::table>>("scale"))
				{
					Actor->SetActorScale3D(TableToVector(ST.value()));
					Changes.Add(TEXT("scale"));
				}
				if (auto V = P.get<sol::optional<std::string>>("label"))
				{
					Actor->SetActorLabel(UTF8_TO_TCHAR(V.value().c_str()));
					Changes.Add(TEXT("label"));
				}
				if (auto V = P.get<sol::optional<std::string>>("folder"))
				{
					Actor->SetFolderPath(::FName(UTF8_TO_TCHAR(V.value().c_str())));
					Changes.Add(TEXT("folder"));
				}
				if (auto V = P.get<sol::optional<std::string>>("mesh"))
				{
					UStaticMesh* NewMesh = LoadObject<UStaticMesh>(nullptr, UTF8_TO_TCHAR(V.value().c_str()));
					if (NewMesh)
					{
						if (UStaticMeshComponent* SMC = Actor->FindComponentByClass<UStaticMeshComponent>())
						{
							SMC->SetStaticMesh(NewMesh);
							Changes.Add(TEXT("mesh"));
						}
					}
				}
				if (auto V = P.get<sol::optional<std::string>>("material"))
				{
					UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, UTF8_TO_TCHAR(V.value().c_str()));
					if (Mat)
					{
						if (UStaticMeshComponent* SMC = Actor->FindComponentByClass<UStaticMeshComponent>())
						{
							SMC->SetMaterial(0, Mat);
							Changes.Add(TEXT("material"));
						}
					}
				}
				// Property bag — set arbitrary UPROPERTYs
				if (auto PropT = P.get<sol::optional<sol::table>>("property"))
				{
					// Resolve target object: actor, specific component, or root component fallback
					UObject* ComponentTarget = nullptr;
					if (auto CompOpt = P.get<sol::optional<std::string>>("component"))
					{
						FString CompKey = UTF8_TO_TCHAR(CompOpt.value().c_str());
						// Try matching by component name or class name
						TArray<UActorComponent*> AllComps;
						Actor->GetComponents(AllComps);
						for (UActorComponent* Comp : AllComps)
						{
							if (Comp->GetName() == CompKey || Comp->GetReadableName() == CompKey
								|| Comp->GetClass()->GetName() == CompKey)
							{
								ComponentTarget = Comp;
								break;
							}
						}
						if (!ComponentTarget)
						{
							Session.Log(FString::Printf(TEXT("[FAIL] configure -> component '%s' not found on actor"), *CompKey));
						}
					}

					for (auto& KV : PropT.value())
					{
						if (!KV.first.is<std::string>() || !KV.second.is<std::string>()) continue;
						FString PropName = UTF8_TO_TCHAR(KV.first.as<std::string>().c_str());
						FString PropValue = UTF8_TO_TCHAR(KV.second.as<std::string>().c_str());

						// Check for dot-notation (nested struct property)
						if (PropName.Contains(TEXT(".")))
						{
							TArray<FString> Segments;
							PropName.ParseIntoArray(Segments, TEXT("."), true);
							if (Segments.Num() < 2) continue;

							// Determine base target object
							UObject* BaseTarget = ComponentTarget ? ComponentTarget : static_cast<UObject*>(Actor);
							FProperty* TopProp = BaseTarget->GetClass()->FindPropertyByName(FName(*Segments[0]));
							if (!TopProp && !ComponentTarget)
							{
								if (USceneComponent* Root = Actor->GetRootComponent())
								{
									FProperty* RootProp = Root->GetClass()->FindPropertyByName(FName(*Segments[0]));
									if (RootProp) { BaseTarget = Root; TopProp = RootProp; }
								}
							}
							if (!TopProp)
							{
								Session.Log(FString::Printf(TEXT("[FAIL] configure -> property '%s' not found (failed at '%s')"), *PropName, *Segments[0]));
								continue;
							}

							// Walk the chain
							FProperty* CurrentProp = TopProp;
							void* ContainerPtr = BaseTarget;
							bool bFailed = false;
							for (int32 i = 1; i < Segments.Num(); ++i)
							{
								FStructProperty* StructProp = CastField<FStructProperty>(CurrentProp);
								if (!StructProp || !StructProp->Struct)
								{
									Session.Log(FString::Printf(TEXT("[FAIL] configure -> property '%s' not found ('%s' is not a struct)"), *PropName, *Segments[i-1]));
									bFailed = true;
									break;
								}
								void* StructDataPtr = StructProp->ContainerPtrToValuePtr<void>(ContainerPtr);
								FProperty* NextProp = StructProp->Struct->FindPropertyByName(FName(*Segments[i]));
								if (!NextProp)
								{
									Session.Log(FString::Printf(TEXT("[FAIL] configure -> property '%s' not found (failed at '%s')"), *PropName, *Segments[i]));
									bFailed = true;
									break;
								}
								ContainerPtr = StructDataPtr;
								CurrentProp = NextProp;
							}
							if (bFailed) continue;

							// Set value: CurrentProp is the leaf, ContainerPtr is its container
							BaseTarget->Modify();
							BaseTarget->PreEditChange(TopProp);
							FStringOutputDevice Errors;
							if (!CurrentProp->ImportText_InContainer(*PropValue, ContainerPtr, BaseTarget, 0, &Errors))
							{
								Session.Log(FString::Printf(TEXT("[FAIL] configure -> property '%s' failed to import value '%s'"), *PropName, *PropValue));
								continue;
							}
							FPropertyChangedEvent PropEvent(TopProp, EPropertyChangeType::ValueSet);
							BaseTarget->PostEditChangeProperty(PropEvent);
							Changes.Add(PropName);
							continue;
						}

						// Flat property (no dot) — original logic with component target support
						UObject* Target = ComponentTarget ? ComponentTarget : static_cast<UObject*>(Actor);
						FName PropFName(*PropName);
						FProperty* Prop = Target->GetClass()->FindPropertyByName(PropFName);
						if (!Prop && !ComponentTarget)
						{
							// Try root component fallback
							if (USceneComponent* Root = Actor->GetRootComponent())
							{
								FProperty* RootProp = Root->GetClass()->FindPropertyByName(PropFName);
								if (RootProp) { Target = Root; Prop = RootProp; }
							}
						}
						if (Prop)
						{
							Target->Modify();
							Target->PreEditChange(Prop);
							FStringOutputDevice Errors;
							Prop->ImportText_InContainer(*PropValue, Target, Target, 0, &Errors);
							FPropertyChangedEvent PropEvent(Prop, EPropertyChangeType::ValueSet);
							Target->PostEditChangeProperty(PropEvent);
							Changes.Add(PropName);
						}
					}
				}

				#if WITH_PAPER2D
				// Paper2D: set sprite
				if (auto V = P.get<sol::optional<std::string>>("sprite"))
				{
					if (UPaperSpriteComponent* PSC = Actor->FindComponentByClass<UPaperSpriteComponent>())
					{
						UPaperSprite* Spr = LoadObject<UPaperSprite>(nullptr, UTF8_TO_TCHAR(V.value().c_str()));
						if (Spr) { PSC->SetSprite(Spr); Changes.Add(TEXT("sprite")); }
					}
				}
				// Paper2D: set flipbook
				if (auto V = P.get<sol::optional<std::string>>("flipbook"))
				{
					if (UPaperFlipbookComponent* PFC = Actor->FindComponentByClass<UPaperFlipbookComponent>())
					{
						UPaperFlipbook* FB = LoadObject<UPaperFlipbook>(nullptr, UTF8_TO_TCHAR(V.value().c_str()));
						if (FB) { PFC->SetFlipbook(FB); Changes.Add(TEXT("flipbook")); }
					}
				}
				// Paper2D: set tilemap
				if (auto V = P.get<sol::optional<std::string>>("tilemap"))
				{
					if (UPaperTileMapComponent* PTMC = Actor->FindComponentByClass<UPaperTileMapComponent>())
					{
						UPaperTileMap* TM = LoadObject<UPaperTileMap>(nullptr, UTF8_TO_TCHAR(V.value().c_str()));
						if (TM) { PTMC->SetTileMap(TM); Changes.Add(TEXT("tilemap")); }
					}
				}
				// Paper2D: set sprite color
				if (auto CT = P.get<sol::optional<sol::table>>("sprite_color"))
				{
					FLinearColor Color(
						static_cast<float>(CT.value().get_or("r", 1.0)),
						static_cast<float>(CT.value().get_or("g", 1.0)),
						static_cast<float>(CT.value().get_or("b", 1.0)),
						static_cast<float>(CT.value().get_or("a", 1.0))
					);
					if (UPaperSpriteComponent* PSC = Actor->FindComponentByClass<UPaperSpriteComponent>())
					{ PSC->SetSpriteColor(Color); Changes.Add(TEXT("sprite_color")); }
					else if (UPaperFlipbookComponent* PFC = Actor->FindComponentByClass<UPaperFlipbookComponent>())
					{ PFC->SetSpriteColor(Color); Changes.Add(TEXT("sprite_color")); }
				}
				// Paper2D: set terrain material
				if (auto V = P.get<sol::optional<std::string>>("terrain_material"))
				{
					if (UPaperTerrainComponent* PTC = Actor->FindComponentByClass<UPaperTerrainComponent>())
					{
						UPaperTerrainMaterial* TMat = LoadObject<UPaperTerrainMaterial>(nullptr, UTF8_TO_TCHAR(V.value().c_str()));
						if (TMat) { PTC->TerrainMaterial = TMat; Changes.Add(TEXT("terrain_material")); }
					}
				}
				// Paper2D: flipbook playback control
				if (auto V = P.get<sol::optional<bool>>("playing"))
				{
					if (UPaperFlipbookComponent* PFC = Actor->FindComponentByClass<UPaperFlipbookComponent>())
					{
						if (V.value()) PFC->Play(); else PFC->Stop();
						Changes.Add(TEXT("playing"));
					}
				}
				if (auto V = P.get<sol::optional<double>>("playback_rate"))
				{
					if (UPaperFlipbookComponent* PFC = Actor->FindComponentByClass<UPaperFlipbookComponent>())
					{
						PFC->SetPlayRate(static_cast<float>(V.value()));
						Changes.Add(TEXT("playback_rate"));
					}
				}
				#endif // WITH_PAPER2D

				// Attach support
				if (auto V = P.get<sol::optional<std::string>>("attach_to"))
				{
					AActor* Parent = FindActorByNameOrLabel(W, UTF8_TO_TCHAR(V.value().c_str()));
					if (Parent)
					{
						Actor->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
						Changes.Add(TEXT("attach"));
					}
				}

				// Tags (add/remove/set)
				if (auto TagsTable = P.get<sol::optional<sol::table>>("tags"))
				{
					Actor->Tags.Empty();
					for (auto& Pair : TagsTable.value())
					{
						if (Pair.second.is<std::string>())
							Actor->Tags.AddUnique(FName(UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str())));
					}
					Changes.Add(TEXT("tags"));
				}
				if (auto V = P.get<sol::optional<std::string>>("add_tag"))
				{
					Actor->Tags.AddUnique(FName(UTF8_TO_TCHAR(V.value().c_str())));
					Changes.Add(TEXT("add_tag"));
				}
				if (auto V = P.get<sol::optional<std::string>>("remove_tag"))
				{
					Actor->Tags.Remove(FName(UTF8_TO_TCHAR(V.value().c_str())));
					Changes.Add(TEXT("remove_tag"));
				}

				// Visibility
				if (auto V = P.get<sol::optional<bool>>("hidden"))
				{
					Actor->SetIsTemporarilyHiddenInEditor(V.value());
					Changes.Add(TEXT("hidden"));
				}

				// Mobility
				if (auto V = P.get<sol::optional<std::string>>("mobility"))
				{
					FString Mob = UTF8_TO_TCHAR(V.value().c_str());
					USceneComponent* Root = Actor->GetRootComponent();
					if (Root)
					{
						if (Mob.Equals(TEXT("static"), ESearchCase::IgnoreCase))
							Root->SetMobility(EComponentMobility::Static);
						else if (Mob.Equals(TEXT("stationary"), ESearchCase::IgnoreCase))
							Root->SetMobility(EComponentMobility::Stationary);
						else if (Mob.Equals(TEXT("movable"), ESearchCase::IgnoreCase))
							Root->SetMobility(EComponentMobility::Movable);
						Changes.Add(TEXT("mobility"));
					}
				}

				// Layer management
				if (auto V = P.get<sol::optional<std::string>>("add_layer"))
				{
					ULayersSubsystem* LayersSub = GetLayersSubsystem();
					if (LayersSub)
					{
						LayersSub->AddActorToLayer(Actor, FName(UTF8_TO_TCHAR(V.value().c_str())));
						Changes.Add(TEXT("add_layer"));
					}
				}
				if (auto V = P.get<sol::optional<std::string>>("remove_layer"))
				{
					ULayersSubsystem* LayersSub = GetLayersSubsystem();
					if (LayersSub)
					{
						LayersSub->RemoveActorFromLayer(Actor, FName(UTF8_TO_TCHAR(V.value().c_str())));
						Changes.Add(TEXT("remove_layer"));
					}
				}

				Actor->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"actor\", \"%s\") -> %s"),
					*ActorId, *FString::Join(Changes, TEXT(", "))));
				return ActorToTable(Lua, Actor);
			}

			// ---- configure("landscape", index, {material?}) ----
			if (FType.Equals(TEXT("landscape"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] configure(\"landscape\") -> params required")); return sol::lua_nil; }
				int32 Index = Id.is<int>() ? Id.as<int>() : 0;

				int32 Current = 0;
				ALandscapeProxy* Target = nullptr;
				for (TActorIterator<ALandscapeProxy> It(W); It; ++It)
				{
					if (Current == Index) { Target = *It; break; }
					Current++;
				}
				if (!Target) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"landscape\", %d) -> not found"), Index)); return sol::lua_nil; }

				sol::table P = Params.value();
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDConfigLandscape", "Level Design: Configure Landscape"));
				Target->Modify();

				if (auto V = P.get<sol::optional<std::string>>("material"))
				{
					UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, UTF8_TO_TCHAR(V.value().c_str()));
					if (Mat) Target->LandscapeMaterial = Mat;
				}

				Target->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"landscape\", %d)"), Index));
				return ActorToTable(Lua, Target);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type (actor|landscape)"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// sculpt(params)
		// ================================================================
		LevelObj.set_function("sculpt", [&Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UWorld* W = GetEditorWorld();
			if (!W) { Session.Log(TEXT("[FAIL] sculpt -> no editor world")); return sol::lua_nil; }

			int32 LandscapeIndex = Params.get_or("landscape", 0);
			ALandscapeProxy* Target = nullptr;
			{
				int32 Idx = 0;
				for (TActorIterator<ALandscapeProxy> It(W); It; ++It)
				{
					if (Idx == LandscapeIndex) { Target = *It; break; }
					Idx++;
				}
			}
			if (!Target) { Session.Log(TEXT("[FAIL] sculpt -> landscape not found")); return sol::lua_nil; }

			ULandscapeInfo* Info = Target->GetLandscapeInfo();
			if (!Info) Info = Target->CreateLandscapeInfo();
			if (!Info) { Session.Log(TEXT("[FAIL] sculpt -> no landscape info")); return sol::lua_nil; }

			// Region
			sol::optional<sol::table> RegionOpt = Params.get<sol::optional<sol::table>>("region");
			int32 MinX, MinY, MaxX, MaxY;
			if (RegionOpt.has_value())
			{
				sol::table R = RegionOpt.value();
				MinX = R.get_or("x1", 0);
				MinY = R.get_or("y1", 0);
				MaxX = R.get_or("x2", 0);
				MaxY = R.get_or("y2", 0);
			}
			else
			{
				if (!Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY))
				{
					Session.Log(TEXT("[FAIL] sculpt -> cannot get landscape extent"));
					return sol::lua_nil;
				}
			}

			int32 Width = MaxX - MinX + 1;
			int32 Height = MaxY - MinY + 1;
			if (Width <= 0 || Height <= 0) { Session.Log(TEXT("[FAIL] sculpt -> invalid region")); return sol::lua_nil; }

			// Safety: cap sculpt region to prevent OOM (2048x2048 = 4M vertices is safe)
			constexpr int32 MaxSculptDim = 2048;
			if (Width > MaxSculptDim || Height > MaxSculptDim)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] sculpt -> region %dx%d too large (max %dx%d per call). Use region={x1,y1,x2,y2} to sculpt in smaller chunks."), Width, Height, MaxSculptDim, MaxSculptDim));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDSculpt", "Level Design: Sculpt"));

			std::string ModeStr = Params.get_or("mode", std::string("set"));
			FString FMode = UTF8_TO_TCHAR(ModeStr.c_str());

			// Get default edit layer GUID for correct texture targeting in 5.6+ edit layer system
			FGuid EditLayerGuid;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			ALandscape* LandscapeActor = Info->LandscapeActor.Get();
			if (LandscapeActor)
			{
				const auto& EditLayers = LandscapeActor->GetEditLayers();
				if (EditLayers.Num() > 0 && EditLayers[0])
				{
					EditLayerGuid = EditLayers[0]->GetGuid();
				}
			}
#endif

			// Read existing height data (use explicit edit layer GUID)
			TArray<uint16> ExistingData;
			ExistingData.SetNum(Width * Height);
			{
				FLandscapeEditDataInterface ReadInterface(Info, EditLayerGuid);
				ReadInterface.GetHeightDataFast(MinX, MinY, MaxX, MaxY, ExistingData.GetData(), 0);
			}

			TArray<uint16> NewData;
			NewData.SetNum(Width * Height);

			// Convert user height values from world units to local height units
			double ZScale = FMath::Max(1.0, static_cast<double>(Target->GetActorScale3D().Z));

			if (FMode.Equals(TEXT("set"), ESearchCase::IgnoreCase))
			{
				float SetHeight = static_cast<float>(Params.get_or("height", 0.0) / ZScale);
				uint16 TexH = LandscapeDataAccess::GetTexHeight(SetHeight);
				for (int32 I = 0; I < NewData.Num(); ++I) NewData[I] = TexH;
			}
			else if (FMode.Equals(TEXT("add"), ESearchCase::IgnoreCase))
			{
				float AddHeight = static_cast<float>(Params.get_or("height", 1000.0) / ZScale);
				float Strength = static_cast<float>(Params.get_or("strength", 1.0));
				for (int32 I = 0; I < NewData.Num(); ++I)
				{
					float Existing = LandscapeDataAccess::GetLocalHeight(ExistingData[I]);
					NewData[I] = LandscapeDataAccess::GetTexHeight(Existing + AddHeight * Strength);
				}
			}
			else if (FMode.Equals(TEXT("noise"), ESearchCase::IgnoreCase))
			{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
				double Amplitude = Params.get_or("amplitude", 3000.0);
				double Frequency = Params.get_or("frequency", 0.005);
				int32 Octaves = Params.get_or("octaves", 4);
				double Seed = Params.get_or("seed", 0.0);
				double Strength = Params.get_or("strength", 1.0);
				bool bAdditive = Params.get_or("additive", true);
				double Lacunarity = Params.get_or("lacunarity", 2.0);
				double Gain = Params.get_or("gain", 0.5);
				std::string NoiseMode = Params.get_or("noise_mode", std::string("standard"));

				// Convert amplitude from world units to local height units (ZScale already at outer scope)
				double LocalAmplitude = Amplitude / ZScale;

				UE::Geometry::EFBMMode FBMMode = UE::Geometry::EFBMMode::Standard;
				if (NoiseMode == "turbulent") FBMMode = UE::Geometry::EFBMMode::Turbulent;
				else if (NoiseMode == "ridge") FBMMode = UE::Geometry::EFBMMode::Ridge;

				for (int32 Y = 0; Y < Height; ++Y)
				{
					for (int32 X = 0; X < Width; ++X)
					{
						double NoiseVal = UE::Geometry::FractalBrownianMotionNoise<double>(
							FBMMode,
							static_cast<uint32>(Octaves),
							FVector2d((X + MinX + Seed) * Frequency, (Y + MinY + Seed) * Frequency),
							Lacunarity,
							Gain,
							0.0, // Smoothness
							1.0  // Gamma
						);
						double Total = NoiseVal * LocalAmplitude * Strength;

						float Existing = LandscapeDataAccess::GetLocalHeight(ExistingData[Y * Width + X]);
						float Final = bAdditive ? (Existing + static_cast<float>(Total)) : static_cast<float>(Total);
						NewData[Y * Width + X] = LandscapeDataAccess::GetTexHeight(Final);
					}
				}
#else
				Session.Log(TEXT("[FAIL] modify_landscape noise mode requires UE 5.6+"));
				NewData = ExistingData;
#endif
			}
			else if (FMode.Equals(TEXT("smooth"), ESearchCase::IgnoreCase))
			{
				int32 Iterations = Params.get_or("iterations", 3);
				float Strength = static_cast<float>(Params.get_or("strength", 0.5));

				// Copy existing
				NewData = ExistingData;

				for (int32 Iter = 0; Iter < Iterations; ++Iter)
				{
					TArray<uint16> Temp = NewData;
					for (int32 Y = 1; Y < Height - 1; ++Y)
					{
						for (int32 X = 1; X < Width - 1; ++X)
						{
							float Center = LandscapeDataAccess::GetLocalHeight(NewData[Y * Width + X]);
							float Avg = (
								LandscapeDataAccess::GetLocalHeight(NewData[(Y-1) * Width + X]) +
								LandscapeDataAccess::GetLocalHeight(NewData[(Y+1) * Width + X]) +
								LandscapeDataAccess::GetLocalHeight(NewData[Y * Width + (X-1)]) +
								LandscapeDataAccess::GetLocalHeight(NewData[Y * Width + (X+1)])
							) * 0.25f;
							float Blended = FMath::Lerp(Center, Avg, Strength);
							Temp[Y * Width + X] = LandscapeDataAccess::GetTexHeight(Blended);
						}
					}
					NewData = MoveTemp(Temp);
				}
			}
			else if (FMode.Equals(TEXT("flatten"), ESearchCase::IgnoreCase))
			{
				// Flatten to average or specified height
				float FlatHeight;
				if (auto V = Params.get<sol::optional<double>>("height"))
				{
					FlatHeight = static_cast<float>(V.value());
				}
				else
				{
					// Calculate average
					double Sum = 0.0;
					for (int32 I = 0; I < ExistingData.Num(); ++I)
						Sum += LandscapeDataAccess::GetLocalHeight(ExistingData[I]);
					FlatHeight = static_cast<float>(Sum / ExistingData.Num());
				}
				uint16 TexH = LandscapeDataAccess::GetTexHeight(FlatHeight);
				for (int32 I = 0; I < NewData.Num(); ++I) NewData[I] = TexH;
			}
			else if (FMode.Equals(TEXT("hole"), ESearchCase::IgnoreCase))
			{
				// Landscape visibility — 0 = hole, 255 = visible
				bool bFill = Params.get_or("fill", false); // fill=true restores visibility
				uint8 VisValue = bFill ? 255 : 0;

				TArray<uint8> VisData;
				VisData.SetNum(Width * Height);
				for (int32 I = 0; I < VisData.Num(); ++I) VisData[I] = VisValue;

				FLandscapeEditDataInterface EditInterface(Info);
				EditInterface.SetAlphaData(ALandscapeProxy::VisibilityLayer, MinX, MinY, MaxX, MaxY,
					VisData.GetData(), 0);
				EditInterface.Flush();

				Session.Log(FString::Printf(TEXT("[OK] sculpt(\"hole\") -> %dx%d region, fill=%s"),
					Width, Height, bFill ? TEXT("true") : TEXT("false")));
				return sol::make_object(Lua, true);
			}
			else if (FMode.Equals(TEXT("erode"), ESearchCase::IgnoreCase))
			{
				// Thermal erosion — slope-based neighbor redistribution
				int32 Iterations = Params.get_or("iterations", 5);
				float Threshold = static_cast<float>(Params.get_or("threshold", 0.5));
				float Strength = static_cast<float>(Params.get_or("strength", 0.3));

				NewData = ExistingData;

				for (int32 Iter = 0; Iter < Iterations; ++Iter)
				{
					bool bChanged = false;
					TArray<uint16> Temp = NewData;

					for (int32 Y = 1; Y < Height - 1; ++Y)
					{
						for (int32 X = 1; X < Width - 1; ++X)
						{
							int32 CI = Y * Width + X;
							float CenterH = LandscapeDataAccess::GetLocalHeight(NewData[CI]);

							// 4-neighbor offsets
							const int32 Offsets[4] = { -1, 1, -Width, Width };
							float SlopeTotal = 0.0f;
							float Slopes[4] = { 0, 0, 0, 0 };

							for (int32 N = 0; N < 4; ++N)
							{
								float NH = LandscapeDataAccess::GetLocalHeight(NewData[CI + Offsets[N]]);
								float Diff = CenterH - NH;
								if (Diff > Threshold)
								{
									Slopes[N] = Diff;
									SlopeTotal += Diff;
								}
							}

							if (SlopeTotal > 0.0f)
							{
								float TotalTransfer = 0.0f;
								for (int32 N = 0; N < 4; ++N)
								{
									if (Slopes[N] > 0.0f)
									{
										float Transfer = Strength * Slopes[N] * (Slopes[N] / SlopeTotal);
										float NH = LandscapeDataAccess::GetLocalHeight(NewData[CI + Offsets[N]]);
										Temp[CI + Offsets[N]] = LandscapeDataAccess::GetTexHeight(NH + Transfer);
										TotalTransfer += Transfer;
									}
								}
								Temp[CI] = LandscapeDataAccess::GetTexHeight(CenterH - TotalTransfer);
								bChanged = true;
							}
						}
					}
					NewData = MoveTemp(Temp);
					if (!bChanged) break;
				}
			}
			else if (FMode.Equals(TEXT("hydraulic"), ESearchCase::IgnoreCase))
			{
				// Hydraulic erosion — water flow + sediment transport
				int32 Iterations = Params.get_or("iterations", 8);
				float RainAmount = static_cast<float>(Params.get_or("rain", 0.01));
				float Solubility = static_cast<float>(Params.get_or("solubility", 0.01));
				float Evaporation = static_cast<float>(Params.get_or("evaporation", 0.5));
				float Capacity = static_cast<float>(Params.get_or("capacity", 0.1));

				int32 Total = Width * Height;
				TArray<float> HeightF; HeightF.SetNum(Total);
				TArray<float> WaterH; WaterH.SetNumZeroed(Total);
				TArray<float> Sediment; Sediment.SetNumZeroed(Total);

				// Convert to float for precision
				NewData = ExistingData;
				for (int32 I = 0; I < Total; ++I)
					HeightF[I] = LandscapeDataAccess::GetLocalHeight(ExistingData[I]);

				for (int32 Iter = 0; Iter < Iterations; ++Iter)
				{
					// Rain
					for (int32 I = 0; I < Total; ++I) WaterH[I] += RainAmount;

					// Dissolve terrain
					for (int32 I = 0; I < Total; ++I)
					{
						float Dissolved = FMath::Min(Solubility * WaterH[I], HeightF[I]);
						HeightF[I] -= Dissolved;
						Sediment[I] += Dissolved;
					}

					// Water flow to lower neighbors (4-neighbor)
					TArray<float> NewWater = WaterH;
					TArray<float> NewSediment = Sediment;
					for (int32 Y = 1; Y < Height - 1; ++Y)
					{
						for (int32 X = 1; X < Width - 1; ++X)
						{
							int32 CI = Y * Width + X;
							float Alt = HeightF[CI] + WaterH[CI];
							const int32 Offsets[4] = { -1, 1, -Width, Width };
							float TotalDiff = 0.0f;
							float Diffs[4] = { 0, 0, 0, 0 };

							for (int32 N = 0; N < 4; ++N)
							{
								float NAlt = HeightF[CI + Offsets[N]] + WaterH[CI + Offsets[N]];
								float D = Alt - NAlt;
								if (D > 0) { Diffs[N] = D; TotalDiff += D; }
							}

							if (TotalDiff > 0.0f && WaterH[CI] > 0.0f)
							{
								float FlowTotal = FMath::Min(WaterH[CI], TotalDiff * 0.5f);
								for (int32 N = 0; N < 4; ++N)
								{
									if (Diffs[N] > 0.0f)
									{
										float Frac = Diffs[N] / TotalDiff;
										float WFlow = FlowTotal * Frac;
										float SFlow = (WaterH[CI] > 0.001f) ? Sediment[CI] * (WFlow / WaterH[CI]) : 0.0f;
										NewWater[CI + Offsets[N]] += WFlow;
										NewSediment[CI + Offsets[N]] += SFlow;
										NewWater[CI] -= WFlow;
										NewSediment[CI] -= SFlow;
									}
								}
							}
						}
					}
					WaterH = MoveTemp(NewWater);
					Sediment = MoveTemp(NewSediment);

					// Evaporation + sediment deposition
					for (int32 I = 0; I < Total; ++I)
					{
						WaterH[I] *= (1.0f - Evaporation);
						float Cap = Capacity * WaterH[I];
						if (Sediment[I] > Cap)
						{
							float Deposit = Sediment[I] - Cap;
							Sediment[I] -= Deposit;
							HeightF[I] += Deposit;
						}
					}
				}

				// Convert back to uint16
				for (int32 I = 0; I < Total; ++I)
					NewData[I] = LandscapeDataAccess::GetTexHeight(HeightF[I]);
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[FAIL] sculpt -> unknown mode '%s' (set|add|noise|smooth|flatten|hole|erode|hydraulic)"), *FMode));
				return sol::lua_nil;
			}

			// Write height data (use explicit edit layer GUID)
			{
				FLandscapeEditDataInterface EditInterface(Info, EditLayerGuid);
				EditInterface.SetHeightData(MinX, MinY, MaxX, MaxY,
					NewData.GetData(), 0,
					true, nullptr, nullptr, nullptr,
					false, nullptr, nullptr,
					true, true, true);
				EditInterface.Flush();
			}

			// Force merge edit layers so changes are visible immediately
			Info->ForceLayersFullUpdate();

			Session.Log(FString::Printf(TEXT("[OK] sculpt(\"%s\") -> %dx%d region (%d,%d)-(%d,%d)"),
				*FMode, Width, Height, MinX, MinY, MaxX, MaxY));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// paint(params)
		// ================================================================
		LevelObj.set_function("paint", [&Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UWorld* W = GetEditorWorld();
			if (!W) { Session.Log(TEXT("[FAIL] paint -> no editor world")); return sol::lua_nil; }

			int32 LandscapeIndex = Params.get_or("landscape", 0);
			ALandscapeProxy* Target = nullptr;
			{
				int32 Idx = 0;
				for (TActorIterator<ALandscapeProxy> It(W); It; ++It)
				{
					if (Idx == LandscapeIndex) { Target = *It; break; }
					Idx++;
				}
			}
			if (!Target) { Session.Log(TEXT("[FAIL] paint -> landscape not found")); return sol::lua_nil; }

			ULandscapeInfo* Info = Target->GetLandscapeInfo();
			if (!Info) { Session.Log(TEXT("[FAIL] paint -> no landscape info")); return sol::lua_nil; }

			// Check for multi-layer mode (layers={...}) vs single-layer mode (layer="Name")
			sol::optional<sol::table> LayersOpt = Params.get<sol::optional<sol::table>>("layers");
			std::string LayerStr = Params.get_or("layer", std::string());

			if (!LayersOpt.has_value() && LayerStr.empty())
			{
				Session.Log(TEXT("[FAIL] paint -> 'layer' (string) or 'layers' (table) required"));
				return sol::lua_nil;
			}

			// Get region
			sol::optional<sol::table> RegionOpt = Params.get<sol::optional<sol::table>>("region");
			int32 MinX, MinY, MaxX, MaxY;
			if (RegionOpt.has_value())
			{
				sol::table R = RegionOpt.value();
				MinX = R.get_or("x1", 0);
				MinY = R.get_or("y1", 0);
				MaxX = R.get_or("x2", 0);
				MaxY = R.get_or("y2", 0);
			}
			else
			{
				if (!Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY))
				{
					Session.Log(TEXT("[FAIL] paint -> cannot get landscape extent"));
					return sol::lua_nil;
				}
			}

			int32 Width = MaxX - MinX + 1;
			int32 Height = MaxY - MinY + 1;
			if (Width <= 0 || Height <= 0) { Session.Log(TEXT("[FAIL] paint -> invalid region")); return sol::lua_nil; }

			constexpr int32 MaxPaintDim = 2048;
			if (Width > MaxPaintDim || Height > MaxPaintDim)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] paint -> region %dx%d too large (max %dx%d per call). Use region={x1,y1,x2,y2} to paint in smaller chunks."), Width, Height, MaxPaintDim, MaxPaintDim));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDPaint", "Level Design: Paint Layer"));

			if (LayersOpt.has_value())
			{
				// ---- Multi-layer paint mode ----
				sol::table LayersTable = LayersOpt.value();

				struct FLayerPaintEntry
				{
					ULandscapeLayerInfoObject* LayerInfo;
					uint8 Weight;
					FName Name;
				};
				TArray<FLayerPaintEntry> Entries;

				for (auto& Pair : LayersTable)
				{
					sol::table Entry = Pair.second.as<sol::table>();
					std::string Name = Entry.get_or("name", std::string());
					if (Name.empty()) continue;
					FName LName(UTF8_TO_TCHAR(Name.c_str()));
					ULandscapeLayerInfoObject* LI = Info->GetLayerInfoByName(LName);
					if (!LI)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] paint -> layer '%s' not found"), *LName.ToString()));
						return sol::lua_nil;
					}
					uint8 W8 = static_cast<uint8>(Entry.get_or("weight", 255));
					Entries.Add({ LI, W8, LName });
				}

				if (Entries.Num() == 0)
				{
					Session.Log(TEXT("[FAIL] paint -> no valid layers in 'layers' table"));
					return sol::lua_nil;
				}

				int32 NumLayers = Info->Layers.Num();
				int32 Stride = Width * NumLayers;
				TArray<uint8> InterleavedData;
				InterleavedData.SetNumZeroed(Width * Height * NumLayers);

				TSet<ULandscapeLayerInfoObject*> DirtyLayers;

				for (const FLayerPaintEntry& E : Entries)
				{
					int32 LayerIndex = Info->GetLayerInfoIndex(E.LayerInfo);
					if (LayerIndex == INDEX_NONE) continue;
					DirtyLayers.Add(E.LayerInfo);

					for (int32 Y = 0; Y < Height; ++Y)
					{
						for (int32 X = 0; X < Width; ++X)
						{
							InterleavedData[Y * Stride + X * NumLayers + LayerIndex] = E.Weight;
						}
					}
				}

				FLandscapeEditDataInterface EditInterface(Info);
				EditInterface.SetAlphaData(DirtyLayers, MinX, MinY, MaxX, MaxY,
					InterleavedData.GetData(), 0);
				EditInterface.Flush();

				Session.Log(FString::Printf(TEXT("[OK] paint(multi-layer) -> %d layers on %dx%d region"),
					Entries.Num(), Width, Height));
			}
			else
			{
				// ---- Single-layer paint mode (backward compat) ----
				FName LayerName(UTF8_TO_TCHAR(LayerStr.c_str()));

				ULandscapeLayerInfoObject* LayerInfo = Info->GetLayerInfoByName(LayerName);
				if (!LayerInfo)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] paint -> layer '%s' not found on landscape"), *LayerName.ToString()));
					return sol::lua_nil;
				}

				uint8 Weight = static_cast<uint8>(Params.get_or("weight", 255));

				TArray<uint8> WeightData;
				WeightData.SetNum(Width * Height);
				for (int32 I = 0; I < WeightData.Num(); ++I) WeightData[I] = Weight;

				FLandscapeEditDataInterface EditInterface(Info);
				EditInterface.SetAlphaData(LayerInfo, MinX, MinY, MaxX, MaxY,
					WeightData.GetData(), 0);
				EditInterface.Flush();

				Session.Log(FString::Printf(TEXT("[OK] paint(\"%s\") -> %dx%d at weight %d"),
					*LayerName.ToString(), Width, Height, Weight));
			}

			return sol::make_object(Lua, true);
		});

		// ================================================================
		// auto_paint(params) — paint by slope/height rules
		// ================================================================
		LevelObj.set_function("auto_paint", [&Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UWorld* W = GetEditorWorld();
			if (!W) { Session.Log(TEXT("[FAIL] auto_paint -> no editor world")); return sol::lua_nil; }

			int32 LandscapeIndex = Params.get_or("landscape", 0);
			ALandscapeProxy* Target = FindLandscapeByIndex(W, LandscapeIndex);
			if (!Target) { Session.Log(TEXT("[FAIL] auto_paint -> landscape not found")); return sol::lua_nil; }

			ULandscapeInfo* Info = Target->GetLandscapeInfo();
			if (!Info) { Session.Log(TEXT("[FAIL] auto_paint -> no landscape info")); return sol::lua_nil; }

			sol::optional<sol::table> LayersOpt = Params.get<sol::optional<sol::table>>("layers");
			if (!LayersOpt.has_value()) { Session.Log(TEXT("[FAIL] auto_paint -> layers table required")); return sol::lua_nil; }
			sol::table LayersTable = LayersOpt.value();

			double NoiseStrength = Params.get_or("noise_strength", 0.1);

			// Parse layer rules
			struct FAutoPaintRule
			{
				ULandscapeLayerInfoObject* LayerInfo;
				FName Name;
				float MinSlope;
				float MaxSlope;
				float MinHeight;
				float MaxHeight;
			};
			TArray<FAutoPaintRule> Rules;

			for (auto& Pair : LayersTable)
			{
				sol::table Entry = Pair.second.as<sol::table>();
				std::string Name = Entry.get_or("name", std::string());
				if (Name.empty()) continue;
				FName LName(UTF8_TO_TCHAR(Name.c_str()));
				ULandscapeLayerInfoObject* LI = Info->GetLayerInfoByName(LName);
				if (!LI)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] auto_paint -> layer '%s' not found"), *LName.ToString()));
					return sol::lua_nil;
				}
				FAutoPaintRule Rule;
				Rule.LayerInfo = LI;
				Rule.Name = LName;
				Rule.MinSlope = static_cast<float>(Entry.get_or("min_slope", 0.0));
				Rule.MaxSlope = static_cast<float>(Entry.get_or("max_slope", 90.0));
				Rule.MinHeight = static_cast<float>(Entry.get_or("min_height", -99999.0));
				Rule.MaxHeight = static_cast<float>(Entry.get_or("max_height", 99999.0));
				Rules.Add(Rule);
			}

			if (Rules.Num() == 0) { Session.Log(TEXT("[FAIL] auto_paint -> no valid layer rules")); return sol::lua_nil; }

			// Get region
			int32 MinX, MinY, MaxX, MaxY;
			sol::optional<sol::table> RegionOpt = Params.get<sol::optional<sol::table>>("region");
			if (RegionOpt.has_value())
			{
				sol::table R = RegionOpt.value();
				MinX = R.get_or("x1", 0);
				MinY = R.get_or("y1", 0);
				MaxX = R.get_or("x2", 0);
				MaxY = R.get_or("y2", 0);
			}
			else
			{
				if (!Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY))
				{
					Session.Log(TEXT("[FAIL] auto_paint -> cannot get landscape extent"));
					return sol::lua_nil;
				}
			}

			int32 Width = MaxX - MinX + 1;
			int32 Height = MaxY - MinY + 1;
			if (Width <= 0 || Height <= 0) { Session.Log(TEXT("[FAIL] auto_paint -> invalid region")); return sol::lua_nil; }

			constexpr int32 MaxPaintDim = 2048;
			if (Width > MaxPaintDim || Height > MaxPaintDim)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] auto_paint -> region %dx%d too large (max %dx%d)"), Width, Height, MaxPaintDim, MaxPaintDim));
				return sol::lua_nil;
			}

			// Read height data
			TArray<uint16> HeightData;
			HeightData.SetNum(Width * Height);
			{
				FLandscapeEditDataInterface ReadInterface(Info);
				ReadInterface.GetHeightDataFast(MinX, MinY, MaxX, MaxY, HeightData.GetData(), 0);
			}

			// Build interleaved weight data
			int32 NumLayers = Info->Layers.Num();
			int32 Stride = Width * NumLayers;
			TArray<uint8> InterleavedData;
			InterleavedData.SetNumZeroed(Width * Height * NumLayers);

			// Map rules to layer indices
			struct FRuleWithIndex
			{
				FAutoPaintRule Rule;
				int32 LayerIndex;
			};
			TArray<FRuleWithIndex> IndexedRules;
			TSet<ULandscapeLayerInfoObject*> DirtyLayers;
			for (const FAutoPaintRule& R : Rules)
			{
				int32 Idx = Info->GetLayerInfoIndex(R.LayerInfo);
				if (Idx == INDEX_NONE) continue;
				IndexedRules.Add({ R, Idx });
				DirtyLayers.Add(R.LayerInfo);
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDAutoPaint", "Level Design: Auto Paint"));

			// For each vertex, compute slope + height, then assign weights
			for (int32 Y = 0; Y < Height; ++Y)
			{
				for (int32 X = 0; X < Width; ++X)
				{
					int32 Idx = Y * Width + X;
					uint16 CenterH = HeightData[Idx];

					// Height in world units (landscape Z scale = 1/128)
					float LocalHeight = (static_cast<float>(CenterH) - 32768.0f) / 128.0f;

					// Compute slope from neighbors (clamp at edges)
					uint16 hL = HeightData[Y * Width + FMath::Max(0, X - 1)];
					uint16 hR = HeightData[Y * Width + FMath::Min(Width - 1, X + 1)];
					uint16 hD = HeightData[FMath::Max(0, Y - 1) * Width + X];
					uint16 hU = HeightData[FMath::Min(Height - 1, Y + 1) * Width + X];
					float dX = (static_cast<float>(hR) - static_cast<float>(hL)) / 2.0f;
					float dY = (static_cast<float>(hU) - static_cast<float>(hD)) / 2.0f;
					// 128.0f = LANDSCAPE_INV_ZSCALE — converts height gradient to proper slope
					float SlopeDeg = FMath::RadiansToDegrees(FMath::Atan2(FMath::Sqrt(dX * dX + dY * dY), 128.0f));

					// Compute raw weights per rule
					TArray<float> RawWeights;
					RawWeights.SetNumZeroed(IndexedRules.Num());

					for (int32 RI = 0; RI < IndexedRules.Num(); ++RI)
					{
						const FAutoPaintRule& Rule = IndexedRules[RI].Rule;

						// Check slope range with smooth falloff (5 degree transition)
						float SlopeW = 1.0f;
						constexpr float SlopeFalloff = 5.0f;
						if (SlopeDeg < Rule.MinSlope)
							SlopeW = FMath::Clamp(1.0f - (Rule.MinSlope - SlopeDeg) / SlopeFalloff, 0.0f, 1.0f);
						else if (SlopeDeg > Rule.MaxSlope)
							SlopeW = FMath::Clamp(1.0f - (SlopeDeg - Rule.MaxSlope) / SlopeFalloff, 0.0f, 1.0f);

						// Check height range with smooth falloff (500 unit transition)
						float HeightW = 1.0f;
						constexpr float HeightFalloff = 500.0f;
						if (LocalHeight < Rule.MinHeight)
							HeightW = FMath::Clamp(1.0f - (Rule.MinHeight - LocalHeight) / HeightFalloff, 0.0f, 1.0f);
						else if (LocalHeight > Rule.MaxHeight)
							HeightW = FMath::Clamp(1.0f - (LocalHeight - Rule.MaxHeight) / HeightFalloff, 0.0f, 1.0f);

						float Weight = SlopeW * HeightW;

						// Add noise variation at transitions
						if (NoiseStrength > 0.0 && Weight > 0.01f && Weight < 0.99f)
						{
							float Noise = FMath::PerlinNoise2D(FVector2D(
								(X + MinX) * 0.01f + RI * 17.3f,
								(Y + MinY) * 0.01f + RI * 31.7f));
							Weight += static_cast<float>(NoiseStrength) * Noise;
							Weight = FMath::Clamp(Weight, 0.0f, 1.0f);
						}

						RawWeights[RI] = Weight;
					}

					// Normalize weights so they sum to 255
					float TotalWeight = 0.0f;
					for (float RW : RawWeights) TotalWeight += RW;

					if (TotalWeight > 0.0f)
					{
						for (int32 RI = 0; RI < IndexedRules.Num(); ++RI)
						{
							uint8 ByteWeight = static_cast<uint8>(FMath::Clamp(
								FMath::RoundToInt32(RawWeights[RI] / TotalWeight * 255.0f), 0, 255));
							InterleavedData[Y * Stride + X * NumLayers + IndexedRules[RI].LayerIndex] = ByteWeight;
						}
					}
				}
			}

			FLandscapeEditDataInterface EditInterface(Info);
			EditInterface.SetAlphaData(DirtyLayers, MinX, MinY, MaxX, MaxY,
				InterleavedData.GetData(), 0);
			EditInterface.Flush();

			Session.Log(FString::Printf(TEXT("[OK] auto_paint -> %d rules on %dx%d region"),
				Rules.Num(), Width, Height));

			sol::table Result = Lua.create_table();
			Result["rules"] = Rules.Num();
			Result["width"] = Width;
			Result["height"] = Height;
			return Result;
		});

		// ================================================================
		// create_landscape_layer(params)
		// ================================================================
		LevelObj.set_function("create_landscape_layer", [&Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UWorld* W = GetEditorWorld();
			if (!W) { Session.Log(TEXT("[FAIL] create_landscape_layer -> no editor world")); return sol::lua_nil; }

			int32 LandscapeIndex = Params.get_or("landscape", 0);
			ALandscapeProxy* Target = FindLandscapeByIndex(W, LandscapeIndex);
			if (!Target) { Session.Log(TEXT("[FAIL] create_landscape_layer -> landscape not found")); return sol::lua_nil; }

			ULandscapeInfo* Info = Target->GetLandscapeInfo();
			if (!Info) { Session.Log(TEXT("[FAIL] create_landscape_layer -> no landscape info")); return sol::lua_nil; }

			std::string NameStr = Params.get_or("name", std::string());
			if (NameStr.empty()) { Session.Log(TEXT("[FAIL] create_landscape_layer -> name required")); return sol::lua_nil; }
			FString LayerName = UTF8_TO_TCHAR(NameStr.c_str());

			// Check if layer already exists
			FName FLayerName(*LayerName);
			if (Info->GetLayerInfoByName(FLayerName))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] create_landscape_layer -> layer '%s' already exists"), *LayerName));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDCreateLayer", "Level Design: Create Landscape Layer"));

			// Create the layer info object in a persistent package so it survives editor restart
			std::string PathStr = Params.get_or("path", std::string());
			FString PackagePath = PathStr.empty()
				? FString::Printf(TEXT("/Game/LandscapeLayers/LayerInfo_%s"), *LayerName)
				: FString::Printf(TEXT("%s/LayerInfo_%s"), UTF8_TO_TCHAR(PathStr.c_str()), *LayerName);
			UPackage* Package = CreatePackage(*PackagePath);
			ULandscapeLayerInfoObject* LayerInfo = NewObject<ULandscapeLayerInfoObject>(
				Package, ULandscapeLayerInfoObject::StaticClass(),
				*FString::Printf(TEXT("LayerInfo_%s"), *LayerName), RF_Public | RF_Standalone);

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			// Configure using setter methods (properties are becoming private in 5.7)
			LayerInfo->SetLayerName(FLayerName, false);

			// Blend method
			std::string BlendStr = Params.get_or("blend_method", std::string("alpha"));
			if (BlendStr == "weight")
				LayerInfo->SetBlendMethod(ELandscapeTargetLayerBlendMethod::FinalWeightBlending, false);
			else if (BlendStr == "none")
				LayerInfo->SetBlendMethod(ELandscapeTargetLayerBlendMethod::None, false);
			else
				LayerInfo->SetBlendMethod(ELandscapeTargetLayerBlendMethod::PremultipliedAlphaBlending, false);

			// Optional hardness (use setter for 5.7+ forward compat)
			double Hardness = Params.get_or("hardness", -1.0);
			if (Hardness >= 0.0)
			{
				LayerInfo->SetHardness(static_cast<float>(Hardness), false, EPropertyChangeType::ValueSet);
			}

			// Optional physical material (use setter for 5.7+ forward compat)
			std::string PhysMatStr = Params.get_or("physical_material", std::string());
			if (!PhysMatStr.empty())
			{
				UPhysicalMaterial* PhysMat = LoadObject<UPhysicalMaterial>(nullptr, UTF8_TO_TCHAR(PhysMatStr.c_str()));
				if (PhysMat)
				{
					LayerInfo->SetPhysicalMaterial(PhysMat, false);
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[WARN] create_landscape_layer -> physical_material '%s' not found, skipping"),
						UTF8_TO_TCHAR(PhysMatStr.c_str())));
				}
			}
#else
			LayerInfo->LayerName = FLayerName;
			std::string BlendStr = Params.get_or("blend_method", std::string("alpha"));
			Session.Log(TEXT("[WARN] create_landscape_layer -> Landscape layer configuration requires UE 5.7+; blend_method/hardness/physical_material ignored"));
#endif

			// Mark package dirty so the layer info asset gets saved
			LayerInfo->MarkPackageDirty();

			// Register with landscape
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			Info->CreateTargetLayerSettingsFor(LayerInfo);
#endif
			Info->UpdateLayerInfoMap();

			Session.Log(FString::Printf(TEXT("[OK] create_landscape_layer(\"%s\", blend=%s)"),
				*LayerName, UTF8_TO_TCHAR(BlendStr.c_str())));

			sol::table Result = Lua.create_table();
			Result["name"] = NameStr;
			Result["blend_method"] = BlendStr;
			return Result;
		});

		// ================================================================
		// delete_landscape_layer(params)
		// ================================================================
		LevelObj.set_function("delete_landscape_layer", [&Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UWorld* W = GetEditorWorld();
			if (!W) { Session.Log(TEXT("[FAIL] delete_landscape_layer -> no editor world")); return sol::lua_nil; }

			int32 LandscapeIndex = Params.get_or("landscape", 0);
			ALandscapeProxy* Target = FindLandscapeByIndex(W, LandscapeIndex);
			if (!Target) { Session.Log(TEXT("[FAIL] delete_landscape_layer -> landscape not found")); return sol::lua_nil; }

			ULandscapeInfo* Info = Target->GetLandscapeInfo();
			if (!Info) { Session.Log(TEXT("[FAIL] delete_landscape_layer -> no landscape info")); return sol::lua_nil; }

			std::string NameStr = Params.get_or("name", std::string());
			if (NameStr.empty()) { Session.Log(TEXT("[FAIL] delete_landscape_layer -> name required")); return sol::lua_nil; }
			FName LayerName(UTF8_TO_TCHAR(NameStr.c_str()));

			ULandscapeLayerInfoObject* LayerInfo = Info->GetLayerInfoByName(LayerName);
			if (!LayerInfo)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] delete_landscape_layer -> layer '%s' not found"), *LayerName.ToString()));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDDeleteLandLayer", "Level Design: Delete Landscape Layer"));
			Info->DeleteLayer(LayerInfo, LayerName);

			Session.Log(FString::Printf(TEXT("[OK] delete_landscape_layer(\"%s\")"), *LayerName.ToString()));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// scatter(params)
		// ================================================================
		LevelObj.set_function("scatter", [&Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UWorld* W = GetEditorWorld();
			if (!W) { Session.Log(TEXT("[FAIL] scatter -> no editor world")); return sol::lua_nil; }

			std::string MeshPath = Params.get_or("mesh", std::string());
			if (MeshPath.empty()) { Session.Log(TEXT("[FAIL] scatter -> mesh required")); return sol::lua_nil; }
			UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, UTF8_TO_TCHAR(MeshPath.c_str()));
			if (!Mesh) { Session.Log(TEXT("[FAIL] scatter -> mesh not found")); return sol::lua_nil; }

			sol::optional<sol::table> RegionOpt = Params.get<sol::optional<sol::table>>("region");
			if (!RegionOpt.has_value()) { Session.Log(TEXT("[FAIL] scatter -> region {x1,y1,x2,y2} required")); return sol::lua_nil; }
			sol::table R = RegionOpt.value();
			double X1 = R.get_or("x1", 0.0);
			double Y1 = R.get_or("y1", 0.0);
			double X2 = R.get_or("x2", 1000.0);
			double Y2 = R.get_or("y2", 1000.0);

			int32 Count = Params.get_or("count", 100);
			double MinScale = Params.get_or("min_scale", 1.0);
			double MaxScale = Params.get_or("max_scale", 1.0);
			bool bAlignToSurface = Params.get_or("align_to_surface", true);
			int32 Seed = Params.get_or("seed", 0);
			bool bAsFoliage = Params.get_or("as_foliage", false);

			std::string DistStr = Params.get_or("distribution", std::string("random"));

			FRandomStream RNG(Seed);

			TArray<FTransform> Transforms;
			Transforms.Reserve(Count);

			if (FString(UTF8_TO_TCHAR(DistStr.c_str())).Equals(TEXT("grid"), ESearchCase::IgnoreCase))
			{
				int32 GridSize = FMath::Max(1, FMath::CeilToInt32(FMath::Sqrt(static_cast<float>(Count))));
				double StepX = (X2 - X1) / GridSize;
				double StepY = (Y2 - Y1) / GridSize;
				for (int32 GY = 0; GY < GridSize && Transforms.Num() < Count; ++GY)
				{
					for (int32 GX = 0; GX < GridSize && Transforms.Num() < Count; ++GX)
					{
						double PX = X1 + (GX + 0.5) * StepX;
						double PY = Y1 + (GY + 0.5) * StepY;
						// Add jitter
						PX += RNG.FRandRange(-StepX * 0.3, StepX * 0.3);
						PY += RNG.FRandRange(-StepY * 0.3, StepY * 0.3);

						FVector Loc(PX, PY, 50000.0);
						FRotator Rot(0, RNG.FRandRange(0.0f, 360.0f), 0);
						float Scale = RNG.FRandRange(static_cast<float>(MinScale), static_cast<float>(MaxScale));

						if (bAlignToSurface)
						{
							FVector HitLoc, HitNormal;
							if (TraceGround(W, Loc, HitLoc, HitNormal))
								Loc = HitLoc;
							else
								Loc.Z = 0;
						}

						Transforms.Add(FTransform(Rot, Loc, FVector(Scale)));
					}
				}
			}
			else // random (default) or poisson
			{
				for (int32 I = 0; I < Count; ++I)
				{
					double PX = RNG.FRandRange(static_cast<float>(X1), static_cast<float>(X2));
					double PY = RNG.FRandRange(static_cast<float>(Y1), static_cast<float>(Y2));
					FVector Loc(PX, PY, 50000.0);
					FRotator Rot(0, RNG.FRandRange(0.0f, 360.0f), 0);
					float Scale = RNG.FRandRange(static_cast<float>(MinScale), static_cast<float>(MaxScale));

					if (bAlignToSurface)
					{
						FVector HitLoc, HitNormal;
						if (TraceGround(W, Loc, HitLoc, HitNormal))
							Loc = HitLoc;
						else
							Loc.Z = 0;
					}

					Transforms.Add(FTransform(Rot, Loc, FVector(Scale)));
				}
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDScatter", "Level Design: Scatter"));

			if (bAsFoliage)
			{
				AInstancedFoliageActor* IFA = AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel(W, true);
				if (IFA)
				{
					// Reuse existing foliage type for this mesh if one already exists
					UFoliageType_InstancedStaticMesh* FoliageType = nullptr;
					FFoliageInfo* ExistingFI = nullptr;
					IFA->ForEachFoliageInfo([&](UFoliageType* FT, FFoliageInfo& FI) -> bool
					{
						if (UFoliageType_InstancedStaticMesh* FTISM = Cast<UFoliageType_InstancedStaticMesh>(FT))
						{
							if (FTISM->Mesh == Mesh) { FoliageType = FTISM; ExistingFI = &FI; return false; }
						}
						return true;
					});
					IFA->Modify();
					if (!FoliageType)
					{
						FoliageType = NewObject<UFoliageType_InstancedStaticMesh>(IFA);
						FoliageType->Mesh = Mesh;
					}
					FFoliageInfo& Info = ExistingFI ? *ExistingFI : *IFA->AddFoliageInfo(FoliageType);
					for (const FTransform& Xform : Transforms)
					{
						FFoliageInstance Inst;
						Inst.Location = Xform.GetLocation();
						Inst.Rotation = Xform.Rotator();
						Inst.DrawScale3D = FVector3f(Xform.GetScale3D());
						Info.AddInstance(FoliageType, Inst);
					}
				}

				Session.Log(FString::Printf(TEXT("[OK] scatter -> %d foliage instances of %s"),
					Transforms.Num(), UTF8_TO_TCHAR(MeshPath.c_str())));
			}
			else
			{
				// Create HISM actor
				AActor* ISMActor = W->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator);
				if (ISMActor)
				{
					ISMActor->SetFlags(RF_Transactional);
					UHierarchicalInstancedStaticMeshComponent* HISM = NewObject<UHierarchicalInstancedStaticMeshComponent>(
						ISMActor, NAME_None, RF_Transactional);
					HISM->SetStaticMesh(Mesh);
					HISM->RegisterComponent();
					ISMActor->SetRootComponent(HISM);
					ISMActor->AddInstanceComponent(HISM);

					// Optimized bulk add: disable auto-rebuild, pre-allocate, then rebuild once
					HISM->bAutoRebuildTreeOnInstanceChanges = false;
					HISM->PreAllocateInstancesMemory(Transforms.Num());
					HISM->AddInstances(Transforms, false/*no indices*/, true/*world space*/, false/*no nav update*/);
					HISM->bAutoRebuildTreeOnInstanceChanges = true;
					HISM->BuildTreeIfOutdated(true/*async*/, true/*force*/);

					std::string LabelStr = Params.get_or("label", std::string());
					if (!LabelStr.empty()) ISMActor->SetActorLabel(UTF8_TO_TCHAR(LabelStr.c_str()));
					std::string FolderStr = Params.get_or("folder", std::string());
					if (!FolderStr.empty()) ISMActor->SetFolderPath(FName(UTF8_TO_TCHAR(FolderStr.c_str())));
				}

				Session.Log(FString::Printf(TEXT("[OK] scatter -> %d HISM instances of %s"),
					Transforms.Num(), UTF8_TO_TCHAR(MeshPath.c_str())));
			}

			sol::table Result = Lua.create_table();
			Result["count"] = Transforms.Num();
			Result["mesh"] = MeshPath;
			return Result;
		});

		// ================================================================
		// import_heightmap(params)
		// ================================================================
		LevelObj.set_function("import_heightmap", [&Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UWorld* W = GetEditorWorld();
			if (!W) { Session.Log(TEXT("[FAIL] import_heightmap -> no editor world")); return sol::lua_nil; }

			std::string FileStr = Params.get_or("file", std::string());
			if (FileStr.empty()) { Session.Log(TEXT("[FAIL] import_heightmap -> file path required")); return sol::lua_nil; }
			FString FilePath = UTF8_TO_TCHAR(FileStr.c_str());

			if (!FPaths::FileExists(FilePath))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] import_heightmap -> file not found: %s"), *FilePath));
				return sol::lua_nil;
			}

			// 1. Get heightmap descriptor
			FLandscapeImportDescriptor Descriptor;
			FText Message;
			ELandscapeImportResult DescResult = FLandscapeImportHelper::GetHeightmapImportDescriptor(FilePath, /*bSingleFile=*/true, /*bFlipYAxis=*/false, Descriptor, Message);
			if (DescResult == ELandscapeImportResult::Error)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] import_heightmap -> descriptor error: %s"), *Message.ToString()));
				return sol::lua_nil;
			}

			if (Descriptor.ImportResolutions.Num() == 0)
			{
				Session.Log(TEXT("[FAIL] import_heightmap -> no valid resolutions in file"));
				return sol::lua_nil;
			}

			int32 FileWidth = Descriptor.ImportResolutions[0].Width;
			int32 FileHeight = Descriptor.ImportResolutions[0].Height;

			// 2. Load height data
			TArray<uint16> HeightData;
			ELandscapeImportResult DataResult = FLandscapeImportHelper::GetHeightmapImportData(Descriptor, /*DescriptorIndex=*/0, HeightData, Message);
			if (DataResult == ELandscapeImportResult::Error)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] import_heightmap -> data load error: %s"), *Message.ToString()));
				return sol::lua_nil;
			}

			// 3. Optionally resample if user requested different size
			int32 SizeX = FileWidth;
			int32 SizeY = FileHeight;
			if (auto UserSize = Params.get<sol::optional<int>>("size"))
			{
				int32 ReqSize = UserSize.value();
				if (ReqSize > 1 && (ReqSize != FileWidth || ReqSize != FileHeight))
				{
					FLandscapeImportResolution CurrentRes(FileWidth, FileHeight);
					FLandscapeImportResolution RequiredRes(ReqSize, ReqSize);
					TArray<uint16> Resampled;
					FLandscapeImportHelper::TransformHeightmapImportData(HeightData, Resampled, CurrentRes, RequiredRes, ELandscapeImportTransformType::Resample);
					HeightData = MoveTemp(Resampled);
					SizeX = ReqSize;
					SizeY = ReqSize;
				}
			}

			// 4. Auto-calculate optimal component sizes
			int32 QuadsPerSection = 63;
			int32 SectionsPerComponent = 1;
			FIntPoint ComponentCount(0, 0);
			FLandscapeImportHelper::ChooseBestComponentSizeForImport(SizeX, SizeY, QuadsPerSection, SectionsPerComponent, ComponentCount);

			// Adjust size to fit component grid exactly
			int32 ActualWidth = ComponentCount.X * QuadsPerSection * SectionsPerComponent + 1;
			int32 ActualHeight = ComponentCount.Y * QuadsPerSection * SectionsPerComponent + 1;
			if (ActualWidth != SizeX || ActualHeight != SizeY)
			{
				FLandscapeImportResolution CurrentRes(SizeX, SizeY);
				FLandscapeImportResolution RequiredRes(ActualWidth, ActualHeight);
				TArray<uint16> Resampled;
				FLandscapeImportHelper::TransformHeightmapImportData(HeightData, Resampled, CurrentRes, RequiredRes, ELandscapeImportTransformType::Resample);
				HeightData = MoveTemp(Resampled);
				SizeX = ActualWidth;
				SizeY = ActualHeight;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDImportHeightmap", "Level Design: Import Heightmap"));

			// 5. Spawn landscape
			FVector SpawnLocation = FVector::ZeroVector;
			if (auto LocT = Params.get<sol::optional<sol::table>>("location")) SpawnLocation = TableToVector(LocT.value());

			FVector Scale(100.0, 100.0, 100.0);
			if (auto ST = Params.get<sol::optional<sol::table>>("scale")) Scale = TableToVector(ST.value());

			ALandscape* Landscape = W->SpawnActor<ALandscape>(SpawnLocation, FRotator::ZeroRotator);
			if (!Landscape)
			{
				Session.Log(TEXT("[FAIL] import_heightmap -> landscape spawn failed"));
				return sol::lua_nil;
			}

			Landscape->SetActorScale3D(Scale);

			// Apply material
			std::string MatStr = Params.get_or("material", std::string());
			if (!MatStr.empty())
			{
				UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, UTF8_TO_TCHAR(MatStr.c_str()));
				if (Mat) Landscape->LandscapeMaterial = Mat;
			}

			// Auto lighting LOD
			Landscape->StaticLightingLOD = FMath::DivideAndRoundUp(
				FMath::CeilLogTwo((SizeX * SizeY) / (2048 * 2048) + 1), (uint32)2);

			// Import with height data
			FGuid LandscapeGuid = FGuid::NewGuid();
			TMap<FGuid, TArray<uint16>> HeightDataPerLayers;
			HeightDataPerLayers.Add(FGuid(), MoveTemp(HeightData));

			TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayerDataPerLayers;
			MaterialLayerDataPerLayers.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());

			Landscape->Import(
				LandscapeGuid,
				0, 0, SizeX - 1, SizeY - 1,
				SectionsPerComponent, QuadsPerSection,
				HeightDataPerLayers, *FilePath,
				MaterialLayerDataPerLayers, ELandscapeImportAlphamapType::Additive,
				{}
			);

			// 6. Post-import
			ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
			if (LandscapeInfo)
			{
				LandscapeInfo->UpdateLayerInfoMap();
			}

			std::string FolderStr = Params.get_or("folder", std::string());
			if (!FolderStr.empty()) Landscape->SetFolderPath(FName(UTF8_TO_TCHAR(FolderStr.c_str())));
			std::string LabelStr = Params.get_or("label", std::string());
			if (!LabelStr.empty()) Landscape->SetActorLabel(UTF8_TO_TCHAR(LabelStr.c_str()));

			Session.Log(FString::Printf(TEXT("[OK] import_heightmap -> %dx%d from %s, %d components"),
				SizeX, SizeY, *FilePath, ComponentCount.X * ComponentCount.Y));
			return ActorToTable(Lua, Landscape);
		});

		// ================================================================
		// import_weightmap(params)
		// ================================================================
		LevelObj.set_function("import_weightmap", [&Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UWorld* W = GetEditorWorld();
			if (!W) { Session.Log(TEXT("[FAIL] import_weightmap -> no editor world")); return sol::lua_nil; }

			std::string FileStr = Params.get_or("file", std::string());
			if (FileStr.empty()) { Session.Log(TEXT("[FAIL] import_weightmap -> file path required")); return sol::lua_nil; }
			FString FilePath = UTF8_TO_TCHAR(FileStr.c_str());

			if (!FPaths::FileExists(FilePath))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] import_weightmap -> file not found: %s"), *FilePath));
				return sol::lua_nil;
			}

			std::string LayerStr = Params.get_or("layer", std::string());
			if (LayerStr.empty()) { Session.Log(TEXT("[FAIL] import_weightmap -> layer name required")); return sol::lua_nil; }
			FName LayerName(UTF8_TO_TCHAR(LayerStr.c_str()));

			// Find landscape
			int32 LandscapeIndex = Params.get_or("landscape", 0);
			ALandscapeProxy* Target = FindLandscapeByIndex(W, LandscapeIndex);
			if (!Target) { Session.Log(TEXT("[FAIL] import_weightmap -> landscape not found")); return sol::lua_nil; }

			ULandscapeInfo* Info = Target->GetLandscapeInfo();
			if (!Info) Info = Target->CreateLandscapeInfo();
			if (!Info) { Session.Log(TEXT("[FAIL] import_weightmap -> no landscape info")); return sol::lua_nil; }

			ULandscapeLayerInfoObject* LayerInfo = Info->GetLayerInfoByName(LayerName);
			if (!LayerInfo)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] import_weightmap -> layer '%s' not found on landscape"), *LayerName.ToString()));
				return sol::lua_nil;
			}

			// Get weightmap descriptor
			FLandscapeImportDescriptor Descriptor;
			FText Message;
			ELandscapeImportResult DescResult = FLandscapeImportHelper::GetWeightmapImportDescriptor(FilePath, /*bSingleFile=*/true, /*bFlipYAxis=*/false, LayerName, Descriptor, Message);
			if (DescResult == ELandscapeImportResult::Error)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] import_weightmap -> descriptor error: %s"), *Message.ToString()));
				return sol::lua_nil;
			}

			if (Descriptor.ImportResolutions.Num() == 0)
			{
				Session.Log(TEXT("[FAIL] import_weightmap -> no valid resolutions in file"));
				return sol::lua_nil;
			}

			// Load weight data
			TArray<uint8> WeightData;
			ELandscapeImportResult DataResult = FLandscapeImportHelper::GetWeightmapImportData(Descriptor, /*DescriptorIndex=*/0, LayerName, WeightData, Message);
			if (DataResult == ELandscapeImportResult::Error)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] import_weightmap -> data load error: %s"), *Message.ToString()));
				return sol::lua_nil;
			}

			int32 FileWidth = Descriptor.ImportResolutions[0].Width;
			int32 FileHeight = Descriptor.ImportResolutions[0].Height;

			// Determine target region
			int32 MinX, MinY, MaxX, MaxY;
			sol::optional<sol::table> RegionOpt = Params.get<sol::optional<sol::table>>("region");
			if (RegionOpt.has_value())
			{
				sol::table R = RegionOpt.value();
				MinX = R.get_or("x1", 0);
				MinY = R.get_or("y1", 0);
				MaxX = R.get_or("x2", 0);
				MaxY = R.get_or("y2", 0);
			}
			else
			{
				if (!Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY))
				{
					Session.Log(TEXT("[FAIL] import_weightmap -> cannot get landscape extent"));
					return sol::lua_nil;
				}
			}

			int32 RegionWidth = MaxX - MinX + 1;
			int32 RegionHeight = MaxY - MinY + 1;
			if (RegionWidth <= 0 || RegionHeight <= 0)
			{
				Session.Log(TEXT("[FAIL] import_weightmap -> invalid region"));
				return sol::lua_nil;
			}

			// Resample if weight data size doesn't match region
			if (FileWidth != RegionWidth || FileHeight != RegionHeight)
			{
				FLandscapeImportResolution CurrentRes(FileWidth, FileHeight);
				FLandscapeImportResolution RequiredRes(RegionWidth, RegionHeight);
				TArray<uint8> Resampled;
				FLandscapeImportHelper::TransformWeightmapImportData(WeightData, Resampled, CurrentRes, RequiredRes, ELandscapeImportTransformType::Resample);
				WeightData = MoveTemp(Resampled);
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDImportWeightmap", "Level Design: Import Weightmap"));

			FLandscapeEditDataInterface EditInterface(Info);
			EditInterface.SetAlphaData(LayerInfo, MinX, MinY, MaxX, MaxY, WeightData.GetData(), 0);
			EditInterface.Flush();

			Session.Log(FString::Printf(TEXT("[OK] import_weightmap -> layer '%s', %dx%d region from %s"),
				*LayerName.ToString(), RegionWidth, RegionHeight, *FilePath));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// navigate(verb, params?)
		// ================================================================
		LevelObj.set_function("navigate", [&Session](sol::table /*self*/,
			const std::string& Verb, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UWorld* W = GetEditorWorld();
			if (!W) { Session.Log(TEXT("[FAIL] navigate -> no editor world")); return sol::lua_nil; }

			FString FVerb = UTF8_TO_TCHAR(Verb.c_str());

			if (FVerb.Equals(TEXT("build"), ESearchCase::IgnoreCase))
			{
				UNavigationSystemV1* NavSys = UNavigationSystemV1::GetCurrent(W);
				if (!NavSys) { Session.Log(TEXT("[FAIL] navigate(\"build\") -> no navigation system")); return sol::lua_nil; }
				NavSys->Build();
				Session.Log(TEXT("[OK] navigate(\"build\") -> navigation rebuild triggered"));
				return sol::make_object(Lua, true);
			}

			if (FVerb.Equals(TEXT("rebuild"), ESearchCase::IgnoreCase))
			{
				UNavigationSystemV1* NavSys = UNavigationSystemV1::GetCurrent(W);
				if (!NavSys) { Session.Log(TEXT("[FAIL] navigate(\"rebuild\") -> no navigation system")); return sol::lua_nil; }
				NavSys->Build();
				Session.Log(TEXT("[OK] navigate(\"rebuild\") -> full navigation rebuild triggered"));
				return sol::make_object(Lua, true);
			}

			if (FVerb.Equals(TEXT("cancel"), ESearchCase::IgnoreCase))
			{
				UNavigationSystemV1* NavSys = UNavigationSystemV1::GetCurrent(W);
				if (!NavSys) { Session.Log(TEXT("[FAIL] navigate(\"cancel\") -> no navigation system")); return sol::lua_nil; }
				NavSys->CancelBuild();
				Session.Log(TEXT("[OK] navigate(\"cancel\") -> build cancelled"));
				return sol::make_object(Lua, true);
			}

			if (FVerb.Equals(TEXT("info"), ESearchCase::IgnoreCase))
			{
				UNavigationSystemV1* NavSys = UNavigationSystemV1::GetCurrent(W);
				sol::table R = Lua.create_table();
				R["has_navigation"] = (NavSys != nullptr);
				if (NavSys)
				{
					R["is_building"] = NavSys->IsNavigationBuildInProgress();
					R["is_locked"] = NavSys->IsNavigationBuildingLocked();
				}
				Session.Log(TEXT("[OK] navigate(\"info\")"));
				return R;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] navigate(\"%s\") -> unknown verb (build|rebuild|cancel|info)"), *FVerb));
			return sol::lua_nil;
		});

#if WITH_WATER
		// ================================================================
		// water(verb, params)
		// ================================================================
		LevelObj.set_function("water", [&Session](sol::table /*self*/,
			const std::string& Verb, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UWorld* W = GetEditorWorld();
			if (!W) { Session.Log(TEXT("[FAIL] water -> no editor world")); return sol::lua_nil; }

			FString FVerb = UTF8_TO_TCHAR(Verb.c_str());

			// Find the water body by name/label
			std::string ActorStr = Params.get_or("actor", std::string());
			AWaterBody* WaterBody = nullptr;
			if (!ActorStr.empty())
			{
				AActor* Found = FindActorByNameOrLabel(W, UTF8_TO_TCHAR(ActorStr.c_str()));
				WaterBody = Cast<AWaterBody>(Found);
			}

			if (FVerb.Equals(TEXT("sync"), ESearchCase::IgnoreCase))
			{
				if (!WaterBody) { Session.Log(TEXT("[FAIL] water(\"sync\") -> water body not found")); return sol::lua_nil; }

				UWaterBodyComponent* WBC = WaterBody->GetWaterBodyComponent();
				if (!WBC) { Session.Log(TEXT("[FAIL] water(\"sync\") -> no water body component")); return sol::lua_nil; }

				UWaterSplineComponent* Spline = WBC->GetWaterSpline();
				if (Spline)
				{
					Spline->K2_SynchronizeAndBroadcastDataChange();
					Session.Log(TEXT("[OK] water(\"sync\") -> spline synchronized"));
					return sol::make_object(Lua, true);
				}
				Session.Log(TEXT("[FAIL] water(\"sync\") -> no water spline"));
				return sol::lua_nil;
			}

			if (FVerb.Equals(TEXT("set_width"), ESearchCase::IgnoreCase))
			{
				if (!WaterBody) { Session.Log(TEXT("[FAIL] water(\"set_width\") -> water body not found")); return sol::lua_nil; }
				UWaterBodyRiverComponent* River = Cast<UWaterBodyRiverComponent>(WaterBody->GetWaterBodyComponent());
				if (!River) { Session.Log(TEXT("[FAIL] water(\"set_width\") -> not a river")); return sol::lua_nil; }

				float Key = static_cast<float>(Params.get_or("key", 0.0));
				float Width = static_cast<float>(Params.get_or("width", 500.0));

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDWaterWidth", "Level Design: Set River Width"));
				River->SetRiverWidthAtSplineInputKey(Key, Width);

				UWaterSplineComponent* Spline = River->GetWaterSpline();
				if (Spline) Spline->K2_SynchronizeAndBroadcastDataChange();

				Session.Log(FString::Printf(TEXT("[OK] water(\"set_width\") -> key=%.2f, width=%.0f"), Key, Width));
				return sol::make_object(Lua, true);
			}

			if (FVerb.Equals(TEXT("set_depth"), ESearchCase::IgnoreCase))
			{
				if (!WaterBody) { Session.Log(TEXT("[FAIL] water(\"set_depth\") -> water body not found")); return sol::lua_nil; }
				UWaterBodyRiverComponent* River = Cast<UWaterBodyRiverComponent>(WaterBody->GetWaterBodyComponent());
				if (!River) { Session.Log(TEXT("[FAIL] water(\"set_depth\") -> not a river")); return sol::lua_nil; }

				float Key = static_cast<float>(Params.get_or("key", 0.0));
				float Depth = static_cast<float>(Params.get_or("depth", 100.0));

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDWaterDepth", "Level Design: Set River Depth"));
				River->SetRiverDepthAtSplineInputKey(Key, Depth);

				UWaterSplineComponent* Spline = River->GetWaterSpline();
				if (Spline) Spline->K2_SynchronizeAndBroadcastDataChange();

				Session.Log(FString::Printf(TEXT("[OK] water(\"set_depth\") -> key=%.2f, depth=%.0f"), Key, Depth));
				return sol::make_object(Lua, true);
			}

			if (FVerb.Equals(TEXT("set_velocity"), ESearchCase::IgnoreCase))
			{
				if (!WaterBody) { Session.Log(TEXT("[FAIL] water(\"set_velocity\") -> water body not found")); return sol::lua_nil; }
				UWaterBodyComponent* WBC = WaterBody->GetWaterBodyComponent();
				if (!WBC) { Session.Log(TEXT("[FAIL] water(\"set_velocity\") -> no component")); return sol::lua_nil; }

				float Key = static_cast<float>(Params.get_or("key", 0.0));
				float Velocity = static_cast<float>(Params.get_or("velocity", 1.0));

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDWaterVel", "Level Design: Set Water Velocity"));
				WBC->SetWaterVelocityAtSplineInputKey(Key, Velocity);

				UWaterSplineComponent* Spline = WBC->GetWaterSpline();
				if (Spline) Spline->K2_SynchronizeAndBroadcastDataChange();

				Session.Log(FString::Printf(TEXT("[OK] water(\"set_velocity\") -> key=%.2f, velocity=%.2f"), Key, Velocity));
				return sol::make_object(Lua, true);
			}

			if (FVerb.Equals(TEXT("set_material"), ESearchCase::IgnoreCase))
			{
				if (!WaterBody) { Session.Log(TEXT("[FAIL] water(\"set_material\") -> water body not found")); return sol::lua_nil; }
				UWaterBodyComponent* WBC = WaterBody->GetWaterBodyComponent();
				if (!WBC) { Session.Log(TEXT("[FAIL] water(\"set_material\") -> no component")); return sol::lua_nil; }

				std::string MatPath = Params.get_or("material", std::string());
				UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, UTF8_TO_TCHAR(MatPath.c_str()));
				if (!Mat) { Session.Log(TEXT("[FAIL] water(\"set_material\") -> material not found")); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDWaterMat", "Level Design: Set Water Material"));
				WBC->SetWaterMaterial(Mat);

				Session.Log(FString::Printf(TEXT("[OK] water(\"set_material\") -> %s"), UTF8_TO_TCHAR(MatPath.c_str())));
				return sol::make_object(Lua, true);
			}

			// ---- water("get_spline", {actor="..."}) -> read spline points ----
			if (FVerb.Equals(TEXT("get_spline"), ESearchCase::IgnoreCase))
			{
				if (!WaterBody) { Session.Log(TEXT("[FAIL] water(\"get_spline\") -> water body not found")); return sol::lua_nil; }
				UWaterBodyComponent* WBC = WaterBody->GetWaterBodyComponent();
				if (!WBC) { Session.Log(TEXT("[FAIL] water(\"get_spline\") -> no component")); return sol::lua_nil; }

				UWaterSplineComponent* Spline = WBC->GetWaterSpline();
				if (!Spline) { Session.Log(TEXT("[FAIL] water(\"get_spline\") -> no spline")); return sol::lua_nil; }

				int32 NumPoints = Spline->GetNumberOfSplinePoints();
				sol::table Result = Lua.create_table();
				Result["type"] = TCHAR_TO_UTF8(*WaterBody->GetClass()->GetName());
				Result["num_points"] = NumPoints;

				sol::table Points = Lua.create_table();
				for (int32 i = 0; i < NumPoints; i++)
				{
					sol::table Pt = Lua.create_table();
					FVector Loc = Spline->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::World);
					Pt["location"] = VectorToTable(Lua, Loc);

					// Get width/depth at this point if it's a river
					float InputKey = Spline->GetInputKeyAtDistanceAlongSpline(Spline->GetDistanceAlongSplineAtSplinePoint(i));
					UWaterBodyRiverComponent* River = Cast<UWaterBodyRiverComponent>(WBC);
					if (River)
					{
						Pt["width"] = River->GetRiverWidthAtSplineInputKey(InputKey);
						Pt["depth"] = River->GetRiverDepthAtSplineInputKey(InputKey);
					}
					Pt["velocity"] = WBC->GetWaterVelocityAtSplineInputKey(InputKey);

					Points[i + 1] = Pt;
				}
				Result["points"] = Points;

				Session.Log(FString::Printf(TEXT("[OK] water(\"get_spline\") -> %d points"), NumPoints));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] water(\"%s\") -> unknown verb (sync|set_width|set_depth|set_velocity|set_material|get_spline)"), *FVerb));
			return sol::lua_nil;
		});
#endif // WITH_WATER

		// ================================================================
		// partition(verb, params?)
		// ================================================================
		LevelObj.set_function("partition", [&Session](sol::table /*self*/,
			const std::string& Verb, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UWorld* W = GetEditorWorld();
			if (!W) { Session.Log(TEXT("[FAIL] partition -> no editor world")); return sol::lua_nil; }

			FString FVerb = UTF8_TO_TCHAR(Verb.c_str());

			if (FVerb.Equals(TEXT("enable"), ESearchCase::IgnoreCase))
			{
#if WITH_EDITOR
				AWorldSettings* Settings = W->GetWorldSettings();
				if (!Settings) { Session.Log(TEXT("[FAIL] partition(\"enable\") -> no world settings")); return sol::lua_nil; }
				UWorldPartition* WP = UWorldPartition::CreateOrRepairWorldPartition(Settings);
				if (WP)
				{
					Session.Log(TEXT("[OK] partition(\"enable\") -> world partition enabled"));
					return sol::make_object(Lua, true);
				}
				Session.Log(TEXT("[FAIL] partition(\"enable\") -> creation failed"));
#endif
				return sol::lua_nil;
			}

			if (FVerb.Equals(TEXT("info"), ESearchCase::IgnoreCase))
			{
				sol::table R = Lua.create_table();
				UWorldPartition* WP = W->GetWorldPartition();
				R["enabled"] = (WP != nullptr);
				if (WP)
				{
					R["initialized"] = WP->IsInitialized();
					R["streaming_enabled"] = WP->IsStreamingEnabled();

					UDataLayerManager* DLM = WP->GetDataLayerManager();
					if (DLM)
					{
						TArray<UDataLayerInstance*> Layers = DLM->GetDataLayerInstances();
						sol::table LayerList = Lua.create_table();
						int32 Idx = 1;
						for (UDataLayerInstance* DL : Layers)
						{
							if (!DL) continue;
							sol::table Entry = Lua.create_table();
							Entry["name"] = TCHAR_TO_UTF8(*DL->GetDataLayerShortName());
							Entry["type"] = TCHAR_TO_UTF8(*UEnum::GetValueAsString(DL->GetType()));
							EDataLayerRuntimeState State = DLM->GetDataLayerInstanceRuntimeState(DL);
							Entry["state"] = (State == EDataLayerRuntimeState::Activated) ? "activated" :
								(State == EDataLayerRuntimeState::Loaded) ? "loaded" : "unloaded";
							LayerList[Idx++] = Entry;
						}
						R["data_layers"] = LayerList;
					}
				}
				Session.Log(TEXT("[OK] partition(\"info\")"));
				return R;
			}

			if (FVerb.Equals(TEXT("set_layer_state"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] partition(\"set_layer_state\") -> params required")); return sol::lua_nil; }
				sol::table P = Params.value();

				UWorldPartition* WP = W->GetWorldPartition();
				if (!WP) { Session.Log(TEXT("[FAIL] partition(\"set_layer_state\") -> no world partition")); return sol::lua_nil; }

				UDataLayerManager* DLM = WP->GetDataLayerManager();
				if (!DLM) { Session.Log(TEXT("[FAIL] partition(\"set_layer_state\") -> no data layer manager")); return sol::lua_nil; }

				std::string LayerName = P.get_or("layer", std::string());
				std::string StateStr = P.get_or("state", std::string("activated"));
				bool bRecursive = P.get_or("recursive", false);

				if (LayerName.empty()) { Session.Log(TEXT("[FAIL] partition(\"set_layer_state\") -> layer name required")); return sol::lua_nil; }

				EDataLayerRuntimeState NewState = EDataLayerRuntimeState::Activated;
				if (FString(UTF8_TO_TCHAR(StateStr.c_str())).Equals(TEXT("unloaded"), ESearchCase::IgnoreCase))
					NewState = EDataLayerRuntimeState::Unloaded;
				else if (FString(UTF8_TO_TCHAR(StateStr.c_str())).Equals(TEXT("loaded"), ESearchCase::IgnoreCase))
					NewState = EDataLayerRuntimeState::Loaded;

				const UDataLayerInstance* Target = DLM->GetDataLayerInstanceFromName(FName(UTF8_TO_TCHAR(LayerName.c_str())));
				if (!Target) { Session.Log(FString::Printf(TEXT("[FAIL] partition(\"set_layer_state\") -> layer '%s' not found"), UTF8_TO_TCHAR(LayerName.c_str()))); return sol::lua_nil; }

				bool bOK = DLM->SetDataLayerInstanceRuntimeState(Target, NewState, bRecursive);
				if (bOK)
					Session.Log(FString::Printf(TEXT("[OK] partition(\"set_layer_state\") -> %s = %s"), UTF8_TO_TCHAR(LayerName.c_str()), UTF8_TO_TCHAR(StateStr.c_str())));
				else
					Session.Log(FString::Printf(TEXT("[FAIL] partition(\"set_layer_state\") -> failed to set %s"), UTF8_TO_TCHAR(LayerName.c_str())));
				return bOK ? sol::make_object(Lua, true) : sol::lua_nil;
			}

			if (FVerb.Equals(TEXT("set_streaming"), ESearchCase::IgnoreCase))
			{
				UWorldPartition* WP = W->GetWorldPartition();
				if (!WP) { Session.Log(TEXT("[FAIL] partition(\"set_streaming\") -> no world partition")); return sol::lua_nil; }

				bool bEnable = Params.has_value() ? Params.value().get_or("enabled", true) : true;
				WP->SetEnableStreaming(bEnable);
				Session.Log(FString::Printf(TEXT("[OK] partition(\"set_streaming\") -> %s"), bEnable ? TEXT("enabled") : TEXT("disabled")));
				return sol::make_object(Lua, true);
			}

			// ---- configure_grid ----
			if (FVerb.Equals(TEXT("configure_grid"), ESearchCase::IgnoreCase))
			{
#if WITH_EDITORONLY_DATA
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] partition(\"configure_grid\") -> params required")); return sol::lua_nil; }
				sol::table P = Params.value();

				UWorldPartition* WP = W->GetWorldPartition();
				if (!WP) { Session.Log(TEXT("[FAIL] partition(\"configure_grid\") -> no world partition")); return sol::lua_nil; }

				UWorldPartitionRuntimeSpatialHash* SpatialHash = Cast<UWorldPartitionRuntimeSpatialHash>(WP->RuntimeHash);
				if (!SpatialHash) { Session.Log(TEXT("[FAIL] partition(\"configure_grid\") -> runtime hash is not spatial hash")); return sol::lua_nil; }

				// Access the Grids array via reflection (it's a private UPROPERTY)
				FProperty* GridsProp = SpatialHash->GetClass()->FindPropertyByName(TEXT("Grids"));
				if (!GridsProp) { Session.Log(TEXT("[FAIL] partition(\"configure_grid\") -> cannot find Grids property")); return sol::lua_nil; }

				FArrayProperty* GridsArrayProp = CastField<FArrayProperty>(GridsProp);
				if (!GridsArrayProp) { Session.Log(TEXT("[FAIL] partition(\"configure_grid\") -> Grids is not an array")); return sol::lua_nil; }

				FScriptArrayHelper ArrayHelper(GridsArrayProp, GridsProp->ContainerPtrToValuePtr<void>(SpatialHash));

				if (ArrayHelper.Num() == 0)
				{
					// Add a default grid entry
					ArrayHelper.AddValue();
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDPartGrid", "Level Design: Configure Grid"));
				SpatialHash->Modify();

				// Modify the first grid entry (the default/primary grid)
				FSpatialHashRuntimeGrid* Grid = reinterpret_cast<FSpatialHashRuntimeGrid*>(ArrayHelper.GetRawPtr(0));

				bool bChanged = false;
				int32 CellSize = P.get_or("cell_size", 0);
				if (CellSize > 0) { Grid->CellSize = CellSize; bChanged = true; }

				double LoadingRange = P.get_or("loading_range", 0.0);
				if (LoadingRange > 0.0) { Grid->LoadingRange = static_cast<float>(LoadingRange); bChanged = true; }

				bool bBlockSlow = P.get_or("block_on_slow_streaming", Grid->bBlockOnSlowStreaming);
				if (bBlockSlow != Grid->bBlockOnSlowStreaming) { Grid->bBlockOnSlowStreaming = bBlockSlow; bChanged = true; }

				if (auto V = P.get<sol::optional<std::string>>("grid_name"))
				{
					Grid->GridName = FName(UTF8_TO_TCHAR(V.value().c_str()));
					bChanged = true;
				}

				bool bDebugDraw = P.get_or("debug_draw", false);
				if (bDebugDraw) { SpatialHash->SetPreviewGrids(true); bChanged = true; }

				if (bChanged)
				{
					// Fire PostEditChangeProperty so internal state recalculates
					FPropertyChangedEvent GridsChangedEvent(GridsProp, EPropertyChangeType::ValueSet);
					SpatialHash->PostEditChangeProperty(GridsChangedEvent);
					SpatialHash->MarkPackageDirty();
				}

				Session.Log(FString::Printf(TEXT("[OK] partition(\"configure_grid\") -> cell_size=%d, loading_range=%.0f"),
					Grid->CellSize, Grid->LoadingRange));
				return sol::make_object(Lua, true);
#else
				Session.Log(TEXT("[FAIL] partition(\"configure_grid\") -> editor-only feature"));
				return sol::lua_nil;
#endif
			}

			// ---- create_layer ----
			if (FVerb.Equals(TEXT("create_layer"), ESearchCase::IgnoreCase))
			{
#if WITH_EDITOR
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] partition(\"create_layer\") -> params required")); return sol::lua_nil; }
				sol::table P = Params.value();

				std::string NameStr = P.get_or("name", std::string());
				if (NameStr.empty()) { Session.Log(TEXT("[FAIL] partition(\"create_layer\") -> name required")); return sol::lua_nil; }

				std::string TypeStr = P.get_or("type", std::string("runtime"));
				EDataLayerType DLType = EDataLayerType::Runtime;
				if (FString(UTF8_TO_TCHAR(TypeStr.c_str())).Equals(TEXT("editor"), ESearchCase::IgnoreCase))
					DLType = EDataLayerType::Editor;

				// 1. Create the UDataLayerAsset in /Game/DataLayers/
				FString AssetName = UTF8_TO_TCHAR(NameStr.c_str());
				FString PackagePath = FString::Printf(TEXT("/Game/DataLayers/%s"), *AssetName);

				UPackage* Pkg = CreatePackage(*PackagePath);
				if (!Pkg) { Session.Log(TEXT("[FAIL] partition(\"create_layer\") -> failed to create package")); return sol::lua_nil; }

				UDataLayerAsset* DLAsset = NewObject<UDataLayerAsset>(Pkg, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
				if (!DLAsset) { Session.Log(TEXT("[FAIL] partition(\"create_layer\") -> failed to create DataLayerAsset")); return sol::lua_nil; }

				DLAsset->SetType(DLType);
				DLAsset->OnCreated();
				FAssetRegistryModule::AssetCreated(DLAsset);
				DLAsset->MarkPackageDirty();

				// 2. Create the instance in the world's AWorldDataLayers
				UWorldPartition* WP = W->GetWorldPartition();
				if (!WP) { Session.Log(TEXT("[FAIL] partition(\"create_layer\") -> no world partition")); return sol::lua_nil; }

				AWorldDataLayers* WDL = W->PersistentLevel->GetWorldDataLayers();
				if (!WDL)
				{
					WDL = AWorldDataLayers::Create(W);
					if (!WDL) { Session.Log(TEXT("[FAIL] partition(\"create_layer\") -> failed to create WorldDataLayers")); return sol::lua_nil; }
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDPartCreateLayer", "Level Design: Create Data Layer"));
				WDL->Modify();

				UDataLayerInstanceWithAsset* NewInstance = WDL->CreateDataLayer<UDataLayerInstanceWithAsset>(DLAsset);
				if (!NewInstance) { Session.Log(FString::Printf(TEXT("[FAIL] partition(\"create_layer\") -> failed to create instance for '%s'"), *AssetName)); return sol::lua_nil; }

				Session.Log(FString::Printf(TEXT("[OK] partition(\"create_layer\") -> '%s' (%s)"), *AssetName, *UEnum::GetValueAsString(DLType)));
				return sol::make_object(Lua, true);
#else
				Session.Log(TEXT("[FAIL] partition(\"create_layer\") -> editor-only feature"));
				return sol::lua_nil;
#endif
			}

			// ---- delete_layer ----
			if (FVerb.Equals(TEXT("delete_layer"), ESearchCase::IgnoreCase))
			{
#if WITH_EDITOR
				// Accept layer name via table: partition("delete_layer", {name="LayerName"})
				FString LayerName;
				if (Params.has_value())
				{
					auto NameOpt = Params.value().get<sol::optional<std::string>>("name");
					if (NameOpt.has_value())
						LayerName = UTF8_TO_TCHAR(NameOpt.value().c_str());
				}
				if (LayerName.IsEmpty()) { Session.Log(TEXT("[FAIL] partition(\"delete_layer\") -> {name=\"LayerName\"} required")); return sol::lua_nil; }

				UWorldPartition* WP = W->GetWorldPartition();
				if (!WP) { Session.Log(TEXT("[FAIL] partition(\"delete_layer\") -> no world partition")); return sol::lua_nil; }

				UDataLayerManager* DLM = WP->GetDataLayerManager();
				if (!DLM) { Session.Log(TEXT("[FAIL] partition(\"delete_layer\") -> no data layer manager")); return sol::lua_nil; }

				// Find the instance by short name
				UDataLayerInstance* Target = nullptr;
				DLM->ForEachDataLayerInstance([&](UDataLayerInstance* DL) -> bool
				{
					if (DL && DL->GetDataLayerShortName().Equals(LayerName, ESearchCase::IgnoreCase))
					{
						Target = DL;
						return false; // stop
					}
					return true;
				});

				if (!Target)
				{
					// Also try by instance name
					const UDataLayerInstance* ConstTarget = DLM->GetDataLayerInstanceFromName(FName(*LayerName));
					Target = const_cast<UDataLayerInstance*>(ConstTarget);
				}

				if (!Target) { Session.Log(FString::Printf(TEXT("[FAIL] partition(\"delete_layer\") -> layer '%s' not found"), *LayerName)); return sol::lua_nil; }

				AWorldDataLayers* WDL = Target->GetOuterWorldDataLayers();
				if (!WDL) { Session.Log(TEXT("[FAIL] partition(\"delete_layer\") -> no WorldDataLayers")); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDPartDelLayer", "Level Design: Delete Data Layer"));
				WDL->Modify();
				bool bOK = WDL->RemoveDataLayer(Target);

				if (bOK)
					Session.Log(FString::Printf(TEXT("[OK] partition(\"delete_layer\") -> removed '%s'"), *LayerName));
				else
					Session.Log(FString::Printf(TEXT("[FAIL] partition(\"delete_layer\") -> failed to remove '%s'"), *LayerName));
				return bOK ? sol::make_object(Lua, true) : sol::lua_nil;
#else
				Session.Log(TEXT("[FAIL] partition(\"delete_layer\") -> editor-only feature"));
				return sol::lua_nil;
#endif
			}

			// ---- add_to_layer ----
			if (FVerb.Equals(TEXT("add_to_layer"), ESearchCase::IgnoreCase))
			{
#if WITH_EDITOR
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] partition(\"add_to_layer\") -> params required")); return sol::lua_nil; }
				sol::table P = Params.value();

				std::string ActorStr = P.get_or("actor", std::string());
				std::string LayerStr = P.get_or("layer", std::string());
				if (ActorStr.empty()) { Session.Log(TEXT("[FAIL] partition(\"add_to_layer\") -> actor required")); return sol::lua_nil; }
				if (LayerStr.empty()) { Session.Log(TEXT("[FAIL] partition(\"add_to_layer\") -> layer required")); return sol::lua_nil; }

				AActor* Actor = FindActorByNameOrLabel(W, UTF8_TO_TCHAR(ActorStr.c_str()));
				if (!Actor) { Session.Log(FString::Printf(TEXT("[FAIL] partition(\"add_to_layer\") -> actor '%s' not found"), UTF8_TO_TCHAR(ActorStr.c_str()))); return sol::lua_nil; }

				UWorldPartition* WP = W->GetWorldPartition();
				if (!WP) { Session.Log(TEXT("[FAIL] partition(\"add_to_layer\") -> no world partition")); return sol::lua_nil; }

				UDataLayerManager* DLM = WP->GetDataLayerManager();
				if (!DLM) { Session.Log(TEXT("[FAIL] partition(\"add_to_layer\") -> no data layer manager")); return sol::lua_nil; }

				// Find layer by short name or instance name
				FString FLayerName = UTF8_TO_TCHAR(LayerStr.c_str());
				UDataLayerInstance* Target = nullptr;
				DLM->ForEachDataLayerInstance([&](UDataLayerInstance* DL) -> bool
				{
					if (DL && DL->GetDataLayerShortName().Equals(FLayerName, ESearchCase::IgnoreCase))
					{
						Target = DL;
						return false;
					}
					return true;
				});
				if (!Target)
				{
					const UDataLayerInstance* ConstTarget = DLM->GetDataLayerInstanceFromName(FName(*FLayerName));
					Target = const_cast<UDataLayerInstance*>(ConstTarget);
				}
				if (!Target) { Session.Log(FString::Printf(TEXT("[FAIL] partition(\"add_to_layer\") -> layer '%s' not found"), *FLayerName)); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDPartAddToLayer", "Level Design: Add Actor to Data Layer"));
				Actor->Modify();
				bool bOK = Actor->AddDataLayer(Target);
				if (bOK)
				{
					Actor->FixupDataLayers();
					Session.Log(FString::Printf(TEXT("[OK] partition(\"add_to_layer\") -> '%s' added to '%s'"), UTF8_TO_TCHAR(ActorStr.c_str()), *FLayerName));
				}
				else
					Session.Log(FString::Printf(TEXT("[FAIL] partition(\"add_to_layer\") -> failed to add '%s' to '%s'"), UTF8_TO_TCHAR(ActorStr.c_str()), *FLayerName));
				return bOK ? sol::make_object(Lua, true) : sol::lua_nil;
#else
				Session.Log(TEXT("[FAIL] partition(\"add_to_layer\") -> editor-only feature"));
				return sol::lua_nil;
#endif
			}

			// ---- remove_from_layer ----
			if (FVerb.Equals(TEXT("remove_from_layer"), ESearchCase::IgnoreCase))
			{
#if WITH_EDITOR
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] partition(\"remove_from_layer\") -> params required")); return sol::lua_nil; }
				sol::table P = Params.value();

				std::string ActorStr = P.get_or("actor", std::string());
				std::string LayerStr = P.get_or("layer", std::string());
				if (ActorStr.empty()) { Session.Log(TEXT("[FAIL] partition(\"remove_from_layer\") -> actor required")); return sol::lua_nil; }
				if (LayerStr.empty()) { Session.Log(TEXT("[FAIL] partition(\"remove_from_layer\") -> layer required")); return sol::lua_nil; }

				AActor* Actor = FindActorByNameOrLabel(W, UTF8_TO_TCHAR(ActorStr.c_str()));
				if (!Actor) { Session.Log(FString::Printf(TEXT("[FAIL] partition(\"remove_from_layer\") -> actor '%s' not found"), UTF8_TO_TCHAR(ActorStr.c_str()))); return sol::lua_nil; }

				UWorldPartition* WP = W->GetWorldPartition();
				if (!WP) { Session.Log(TEXT("[FAIL] partition(\"remove_from_layer\") -> no world partition")); return sol::lua_nil; }

				UDataLayerManager* DLM = WP->GetDataLayerManager();
				if (!DLM) { Session.Log(TEXT("[FAIL] partition(\"remove_from_layer\") -> no data layer manager")); return sol::lua_nil; }

				FString FLayerName = UTF8_TO_TCHAR(LayerStr.c_str());
				UDataLayerInstance* Target = nullptr;
				DLM->ForEachDataLayerInstance([&](UDataLayerInstance* DL) -> bool
				{
					if (DL && DL->GetDataLayerShortName().Equals(FLayerName, ESearchCase::IgnoreCase))
					{
						Target = DL;
						return false;
					}
					return true;
				});
				if (!Target)
				{
					const UDataLayerInstance* ConstTarget = DLM->GetDataLayerInstanceFromName(FName(*FLayerName));
					Target = const_cast<UDataLayerInstance*>(ConstTarget);
				}
				if (!Target) { Session.Log(FString::Printf(TEXT("[FAIL] partition(\"remove_from_layer\") -> layer '%s' not found"), *FLayerName)); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDPartRemoveFromLayer", "Level Design: Remove Actor from Data Layer"));
				Actor->Modify();
				bool bOK = Actor->RemoveDataLayer(Target);
				if (bOK)
				{
					Actor->FixupDataLayers();
					Session.Log(FString::Printf(TEXT("[OK] partition(\"remove_from_layer\") -> '%s' removed from '%s'"), UTF8_TO_TCHAR(ActorStr.c_str()), *FLayerName));
				}
				else
					Session.Log(FString::Printf(TEXT("[FAIL] partition(\"remove_from_layer\") -> failed to remove '%s' from '%s'"), UTF8_TO_TCHAR(ActorStr.c_str()), *FLayerName));
				return bOK ? sol::make_object(Lua, true) : sol::lua_nil;
#else
				Session.Log(TEXT("[FAIL] partition(\"remove_from_layer\") -> editor-only feature"));
				return sol::lua_nil;
#endif
			}

			// ---- get_layer_actors ----
			if (FVerb.Equals(TEXT("get_layer_actors"), ESearchCase::IgnoreCase))
			{
#if WITH_EDITOR
				// Accept layer name via table: partition("get_layer_actors", {name="LayerName"})
				FString LayerName;
				if (Params.has_value())
				{
					auto NameOpt = Params.value().get<sol::optional<std::string>>("name");
					if (NameOpt.has_value())
						LayerName = UTF8_TO_TCHAR(NameOpt.value().c_str());
				}
				if (LayerName.IsEmpty()) { Session.Log(TEXT("[FAIL] partition(\"get_layer_actors\") -> {name=\"LayerName\"} required")); return sol::lua_nil; }

				UWorldPartition* WP = W->GetWorldPartition();
				if (!WP) { Session.Log(TEXT("[FAIL] partition(\"get_layer_actors\") -> no world partition")); return sol::lua_nil; }

				UDataLayerManager* DLM = WP->GetDataLayerManager();
				if (!DLM) { Session.Log(TEXT("[FAIL] partition(\"get_layer_actors\") -> no data layer manager")); return sol::lua_nil; }

				// Find the layer
				const UDataLayerInstance* Target = nullptr;
				DLM->ForEachDataLayerInstance([&](UDataLayerInstance* DL) -> bool
				{
					if (DL && DL->GetDataLayerShortName().Equals(LayerName, ESearchCase::IgnoreCase))
					{
						Target = DL;
						return false;
					}
					return true;
				});
				if (!Target)
					Target = DLM->GetDataLayerInstanceFromName(FName(*LayerName));

				if (!Target) { Session.Log(FString::Printf(TEXT("[FAIL] partition(\"get_layer_actors\") -> layer '%s' not found"), *LayerName)); return sol::lua_nil; }

				// Find the DataLayerAsset for this instance to match against actor DataLayerAssets
				const UDataLayerAsset* TargetAsset = Target->GetAsset();

				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (TActorIterator<AActor> It(W); It; ++It)
				{
					AActor* A = *It;
					if (!A) continue;

					// Check if this actor is in the target data layer
					bool bInLayer = false;
					if (TargetAsset)
					{
						TArray<const UDataLayerAsset*> ActorAssets = A->GetDataLayerAssets();
						for (const UDataLayerAsset* Asset : ActorAssets)
						{
							if (Asset == TargetAsset)
							{
								bInLayer = true;
								break;
							}
						}
					}

					// Also check via instance names
					if (!bInLayer)
					{
						TArray<FName> InstanceNames = A->GetDataLayerInstanceNames();
						for (const FName& N : InstanceNames)
						{
							if (N == Target->GetDataLayerFName())
							{
								bInLayer = true;
								break;
							}
						}
					}

					if (bInLayer)
					{
						sol::table Entry = Lua.create_table();
						Entry["label"] = TCHAR_TO_UTF8(*A->GetActorLabel());
						Entry["name"] = TCHAR_TO_UTF8(*A->GetName());
						Entry["class"] = TCHAR_TO_UTF8(*A->GetClass()->GetName());
						Result[Idx++] = Entry;
					}
				}

				Session.Log(FString::Printf(TEXT("[OK] partition(\"get_layer_actors\") -> %d actors in '%s'"), Idx - 1, *LayerName));
				return Result;
#else
				Session.Log(TEXT("[FAIL] partition(\"get_layer_actors\") -> editor-only feature"));
				return sol::lua_nil;
#endif
			}

			Session.Log(FString::Printf(TEXT("[FAIL] partition(\"%s\") -> unknown verb (enable|info|set_layer_state|set_streaming|configure_grid|create_layer|delete_layer|add_to_layer|remove_from_layer|get_layer_actors)"), *FVerb));
			return sol::lua_nil;
		});

		// Geometry Scripting moved to AIK_GeometryScripting extension module
		// (was: LevelObj.set_function("geometry", ...) — now available as geometry_create())

		// ================================================================
		// edit_layer(verb, params) — landscape edit layers (5.6+)
		// ================================================================
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
		LevelObj.set_function("edit_layer", [&Session](sol::table /*self*/,
			const std::string& Verb, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FVerb = UTF8_TO_TCHAR(Verb.c_str());
			UWorld* W = GetEditorWorld();
			if (!W) { Session.Log(TEXT("[FAIL] edit_layer -> no editor world")); return sol::lua_nil; }

			int32 LandscapeIndex = 0;
			FString LayerName;
			if (Params.has_value())
			{
				LandscapeIndex = Params.value().get_or("landscape", 0);
				if (auto V = Params.value().get<sol::optional<std::string>>("name"))
					LayerName = UTF8_TO_TCHAR(V.value().c_str());
			}

			// Find the ALandscape actor (not just ALandscapeProxy — edit layers are on ALandscape)
			ALandscape* Landscape = nullptr;
			{
				int32 Idx = 0;
				for (TActorIterator<ALandscape> It(W); It; ++It)
				{
					if (Idx == LandscapeIndex) { Landscape = *It; break; }
					Idx++;
				}
			}
			if (!Landscape)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] edit_layer -> ALandscape at index %d not found"), LandscapeIndex));
				return sol::lua_nil;
			}

			// ---- list ----
			if (FVerb.Equals(TEXT("list"), ESearchCase::IgnoreCase))
			{
				TArray<ULandscapeEditLayerBase*> EditLayers = Landscape->GetEditLayers();
				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (ULandscapeEditLayerBase* Layer : EditLayers)
				{
					if (!Layer) continue;
					sol::table Entry = Lua.create_table();
					Entry["name"] = std::string(TCHAR_TO_UTF8(*Layer->GetName().ToString()));
					Entry["visible"] = Layer->IsVisible();
					Entry["locked"] = Layer->IsLocked();
					Entry["heightmap_alpha"] = Layer->GetAlphaForTargetType(ELandscapeToolTargetType::Heightmap);
					Entry["weightmap_alpha"] = Layer->GetAlphaForTargetType(ELandscapeToolTargetType::Weightmap);
					Entry["guid"] = std::string(TCHAR_TO_UTF8(*Layer->GetGuid().ToString()));
					Result[Idx++] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] edit_layer(\"list\") -> %d layers"), Idx - 1));
				return Result;
			}

			// ---- create ----
			if (FVerb.Equals(TEXT("create"), ESearchCase::IgnoreCase))
			{
				if (LayerName.IsEmpty()) { Session.Log(TEXT("[FAIL] edit_layer(\"create\") -> name required")); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDCreateEditLayer", "Level Design: Create Edit Layer"));
				Landscape->Modify();
				int32 NewIndex = Landscape->CreateLayer(FName(*LayerName), ULandscapeEditLayer::StaticClass(), /*bIgnoreLayerCountLimit=*/ true);
				if (NewIndex < 0)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] edit_layer(\"create\", \"%s\") -> CreateLayer failed"), *LayerName));
					return sol::lua_nil;
				}

				Session.Log(FString::Printf(TEXT("[OK] edit_layer(\"create\", \"%s\") -> index %d"), *LayerName, NewIndex));
				sol::table Result = Lua.create_table();
				Result["index"] = NewIndex;
				Result["name"] = std::string(TCHAR_TO_UTF8(*LayerName));
				return Result;
			}

			// ---- delete ----
			if (FVerb.Equals(TEXT("delete"), ESearchCase::IgnoreCase))
			{
				if (LayerName.IsEmpty()) { Session.Log(TEXT("[FAIL] edit_layer(\"delete\") -> name required")); return sol::lua_nil; }

				ULandscapeEditLayerBase* Layer = Landscape->GetEditLayer(FName(*LayerName));
				if (!Layer)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] edit_layer(\"delete\", \"%s\") -> layer not found"), *LayerName));
					return sol::lua_nil;
				}

				// Find index for this layer
				TArray<ULandscapeEditLayerBase*> EditLayers = Landscape->GetEditLayers();
				int32 FoundIndex = INDEX_NONE;
				for (int32 i = 0; i < EditLayers.Num(); ++i)
				{
					if (EditLayers[i] == Layer) { FoundIndex = i; break; }
				}
				if (FoundIndex == INDEX_NONE)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] edit_layer(\"delete\", \"%s\") -> could not find layer index"), *LayerName));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDDeleteEditLayer", "Level Design: Delete Edit Layer"));
				Landscape->Modify();
				bool bDeleted = Landscape->DeleteLayer(FoundIndex);
				if (!bDeleted)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] edit_layer(\"delete\", \"%s\") -> DeleteLayer failed"), *LayerName));
					return sol::lua_nil;
				}

				Session.Log(FString::Printf(TEXT("[OK] edit_layer(\"delete\", \"%s\")"), *LayerName));
				return sol::make_object(Lua, true);
			}

			// ---- set_visibility ----
			if (FVerb.Equals(TEXT("set_visibility"), ESearchCase::IgnoreCase))
			{
				if (LayerName.IsEmpty()) { Session.Log(TEXT("[FAIL] edit_layer(\"set_visibility\") -> name required")); return sol::lua_nil; }
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] edit_layer(\"set_visibility\") -> params required")); return sol::lua_nil; }

				ULandscapeEditLayerBase* Layer = Landscape->GetEditLayer(FName(*LayerName));
				if (!Layer)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] edit_layer(\"set_visibility\", \"%s\") -> layer not found"), *LayerName));
					return sol::lua_nil;
				}

				bool bVisible = Params.value().get_or("visible", true);
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDEditLayerVis", "Level Design: Set Edit Layer Visibility"));
				Layer->SetVisible(bVisible, /*bModify=*/ true);

				Session.Log(FString::Printf(TEXT("[OK] edit_layer(\"set_visibility\", \"%s\", %s)"), *LayerName, bVisible ? TEXT("true") : TEXT("false")));
				return sol::make_object(Lua, true);
			}

			// ---- set_locked ----
			if (FVerb.Equals(TEXT("set_locked"), ESearchCase::IgnoreCase))
			{
				if (LayerName.IsEmpty()) { Session.Log(TEXT("[FAIL] edit_layer(\"set_locked\") -> name required")); return sol::lua_nil; }
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] edit_layer(\"set_locked\") -> params required")); return sol::lua_nil; }

				ULandscapeEditLayerBase* Layer = Landscape->GetEditLayer(FName(*LayerName));
				if (!Layer)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] edit_layer(\"set_locked\", \"%s\") -> layer not found"), *LayerName));
					return sol::lua_nil;
				}

				bool bLocked = Params.value().get_or("locked", false);
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDEditLayerLock", "Level Design: Set Edit Layer Locked"));
				Layer->SetLocked(bLocked, /*bModify=*/ true);

				Session.Log(FString::Printf(TEXT("[OK] edit_layer(\"set_locked\", \"%s\", %s)"), *LayerName, bLocked ? TEXT("true") : TEXT("false")));
				return sol::make_object(Lua, true);
			}

			// ---- set_alpha ----
			if (FVerb.Equals(TEXT("set_alpha"), ESearchCase::IgnoreCase))
			{
				if (LayerName.IsEmpty()) { Session.Log(TEXT("[FAIL] edit_layer(\"set_alpha\") -> name required")); return sol::lua_nil; }
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] edit_layer(\"set_alpha\") -> params required")); return sol::lua_nil; }

				ULandscapeEditLayerBase* Layer = Landscape->GetEditLayer(FName(*LayerName));
				if (!Layer)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] edit_layer(\"set_alpha\", \"%s\") -> layer not found"), *LayerName));
					return sol::lua_nil;
				}

				sol::table P = Params.value();
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDEditLayerAlpha", "Level Design: Set Edit Layer Alpha"));
				TArray<FString> Changes;

				if (auto V = P.get<sol::optional<double>>("heightmap_alpha"))
				{
					Layer->SetAlphaForTargetType(ELandscapeToolTargetType::Heightmap, static_cast<float>(V.value()), /*bModify=*/ true, EPropertyChangeType::ValueSet);
					Changes.Add(FString::Printf(TEXT("heightmap_alpha=%.2f"), V.value()));
				}
				if (auto V = P.get<sol::optional<double>>("weightmap_alpha"))
				{
					Layer->SetAlphaForTargetType(ELandscapeToolTargetType::Weightmap, static_cast<float>(V.value()), /*bModify=*/ true, EPropertyChangeType::ValueSet);
					Changes.Add(FString::Printf(TEXT("weightmap_alpha=%.2f"), V.value()));
				}

				if (Changes.Num() == 0)
				{
					Session.Log(TEXT("[FAIL] edit_layer(\"set_alpha\") -> specify heightmap_alpha and/or weightmap_alpha"));
					return sol::lua_nil;
				}

				Session.Log(FString::Printf(TEXT("[OK] edit_layer(\"set_alpha\", \"%s\") -> %s"), *LayerName, *FString::Join(Changes, TEXT(", "))));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] edit_layer(\"%s\") -> unknown verb (list|create|delete|set_visibility|set_locked|set_alpha)"), *FVerb));
			return sol::lua_nil;
		});
#else
		LevelObj.set_function("edit_layer", [&Session](sol::table /*self*/,
			const std::string& /*Verb*/, sol::optional<sol::table> /*Params*/, sol::this_state S) -> sol::object
		{
			Session.Log(TEXT("[FAIL] edit_layer -> requires UE 5.6+"));
			return sol::lua_nil;
		});
#endif // ENGINE_MINOR_VERSION >= 6

		// ================================================================
		// hlod(verb) — HLOD generation
		// ================================================================
		LevelObj.set_function("hlod", [&Session](sol::table /*self*/,
			const std::string& Verb, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FVerb = UTF8_TO_TCHAR(Verb.c_str());
			UWorld* W = GetEditorWorld();
			if (!W) { Session.Log(TEXT("[FAIL] hlod -> no editor world")); return sol::lua_nil; }

			// ---- build ----
			if (FVerb.Equals(TEXT("build"), ESearchCase::IgnoreCase))
			{
				bool bSuccess = FEditorBuildUtils::EditorBuild(W, FBuildOptions::BuildHierarchicalLOD, /*bAllowLightingDialog=*/ false);
				if (bSuccess)
				{
					Session.Log(TEXT("[OK] hlod(\"build\") -> HLOD build completed"));
					return sol::make_object(Lua, true);
				}
				else
				{
					Session.Log(TEXT("[FAIL] hlod(\"build\") -> HLOD build failed or was canceled"));
					return sol::lua_nil;
				}
			}

			// ---- info ----
			if (FVerb.Equals(TEXT("info"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();

				// Count HLOD actors in the world
				int32 HLODActorCount = 0;
				for (TActorIterator<AActor> It(W); It; ++It)
				{
					AActor* Actor = *It;
					if (Actor && Actor->GetClass()->GetName().Contains(TEXT("HLOD")))
					{
						HLODActorCount++;
					}
				}
				Result["hlod_actor_count"] = HLODActorCount;
				Result["world"] = std::string(TCHAR_TO_UTF8(*W->GetName()));

				Session.Log(FString::Printf(TEXT("[OK] hlod(\"info\") -> %d HLOD actors"), HLODActorCount));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] hlod(\"%s\") -> unknown verb (build|info)"), *FVerb));
			return sol::lua_nil;
		});

		Session.Log(FString::Printf(TEXT("[OK] open_level() -> %s"), *World->GetName()));
		return LevelObj;
	});
}

// ============================================================================
// Layer Management (standalone functions)
// ============================================================================

static void BindLayerManagement(sol::state& Lua, FLuaSessionData& Session)
{
	// list_layers() -> array of layer names
	Lua.set_function("list_layers", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		ULayersSubsystem* Layers = GetLayersSubsystem();
		if (!Layers) { Session.Log(TEXT("[FAIL] list_layers -> subsystem not available")); return sol::lua_nil; }

		TArray<FName> LayerNames;
		Layers->AddAllLayerNamesTo(LayerNames);

		sol::table Result = LuaView.create_table();
		for (int32 i = 0; i < LayerNames.Num(); i++)
			Result[i + 1] = std::string(TCHAR_TO_UTF8(*LayerNames[i].ToString()));

		Session.Log(FString::Printf(TEXT("[OK] list_layers -> %d layers"), LayerNames.Num()));
		return Result;
	});

	// create_layer(name) -> true/nil
	Lua.set_function("create_layer", [&Session](const std::string& Name, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		ULayersSubsystem* Layers = GetLayersSubsystem();
		if (!Layers) { Session.Log(TEXT("[FAIL] create_layer -> subsystem not available")); return sol::lua_nil; }

		FName LayerName(UTF8_TO_TCHAR(Name.c_str()));
		ULayer* NewLayer = Layers->CreateLayer(LayerName);
		if (!NewLayer) { Session.Log(FString::Printf(TEXT("[FAIL] create_layer(\"%s\") -> failed"), *LayerName.ToString())); return sol::lua_nil; }

		Session.Log(FString::Printf(TEXT("[OK] create_layer(\"%s\")"), *LayerName.ToString()));
		return sol::make_object(LuaView, true);
	});

	// delete_layer(name) -> true/nil
	Lua.set_function("delete_layer", [&Session](const std::string& Name, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		ULayersSubsystem* Layers = GetLayersSubsystem();
		if (!Layers) { Session.Log(TEXT("[FAIL] delete_layer -> subsystem not available")); return sol::lua_nil; }

		FName LayerName(UTF8_TO_TCHAR(Name.c_str()));
		Layers->DeleteLayer(LayerName);

		Session.Log(FString::Printf(TEXT("[OK] delete_layer(\"%s\")"), *LayerName.ToString()));
		return sol::make_object(LuaView, true);
	});

	// add_actor_to_layer(actor_name, layer_name) -> true/nil
	Lua.set_function("add_actor_to_layer", [&Session](const std::string& ActorName, const std::string& LayerName, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* W = GetEditorWorld();
		if (!W) { Session.Log(TEXT("[FAIL] add_actor_to_layer -> no editor world")); return sol::lua_nil; }

		AActor* Actor = FindActorByNameOrLabel(W, UTF8_TO_TCHAR(ActorName.c_str()));
		if (!Actor) { Session.Log(FString::Printf(TEXT("[FAIL] add_actor_to_layer -> actor \"%s\" not found"), UTF8_TO_TCHAR(ActorName.c_str()))); return sol::lua_nil; }

		ULayersSubsystem* Layers = GetLayersSubsystem();
		if (!Layers) { Session.Log(TEXT("[FAIL] add_actor_to_layer -> subsystem not available")); return sol::lua_nil; }

		FName FLayerName(UTF8_TO_TCHAR(LayerName.c_str()));
		bool bSuccess = Layers->AddActorToLayer(Actor, FLayerName);
		if (!bSuccess) { Session.Log(FString::Printf(TEXT("[FAIL] add_actor_to_layer(\"%s\", \"%s\") -> failed"), UTF8_TO_TCHAR(ActorName.c_str()), *FLayerName.ToString())); return sol::lua_nil; }

		Session.Log(FString::Printf(TEXT("[OK] add_actor_to_layer(\"%s\", \"%s\")"), UTF8_TO_TCHAR(ActorName.c_str()), *FLayerName.ToString()));
		return sol::make_object(LuaView, true);
	});

	// remove_actor_from_layer(actor_name, layer_name) -> true/nil
	Lua.set_function("remove_actor_from_layer", [&Session](const std::string& ActorName, const std::string& LayerName, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* W = GetEditorWorld();
		if (!W) { Session.Log(TEXT("[FAIL] remove_actor_from_layer -> no editor world")); return sol::lua_nil; }

		AActor* Actor = FindActorByNameOrLabel(W, UTF8_TO_TCHAR(ActorName.c_str()));
		if (!Actor) { Session.Log(FString::Printf(TEXT("[FAIL] remove_actor_from_layer -> actor \"%s\" not found"), UTF8_TO_TCHAR(ActorName.c_str()))); return sol::lua_nil; }

		ULayersSubsystem* Layers = GetLayersSubsystem();
		if (!Layers) { Session.Log(TEXT("[FAIL] remove_actor_from_layer -> subsystem not available")); return sol::lua_nil; }

		FName FLayerName(UTF8_TO_TCHAR(LayerName.c_str()));
		bool bSuccess = Layers->RemoveActorFromLayer(Actor, FLayerName);
		if (!bSuccess) { Session.Log(FString::Printf(TEXT("[FAIL] remove_actor_from_layer(\"%s\", \"%s\") -> failed"), UTF8_TO_TCHAR(ActorName.c_str()), *FLayerName.ToString())); return sol::lua_nil; }

		Session.Log(FString::Printf(TEXT("[OK] remove_actor_from_layer(\"%s\", \"%s\")"), UTF8_TO_TCHAR(ActorName.c_str()), *FLayerName.ToString()));
		return sol::make_object(LuaView, true);
	});

	// get_actors_in_layer(layer_name) -> array of actor labels
	Lua.set_function("get_actors_in_layer", [&Session](const std::string& LayerName, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		ULayersSubsystem* Layers = GetLayersSubsystem();
		if (!Layers) { Session.Log(TEXT("[FAIL] get_actors_in_layer -> subsystem not available")); return sol::lua_nil; }

		FName FLayerName(UTF8_TO_TCHAR(LayerName.c_str()));
		TArray<AActor*> Actors = Layers->GetActorsFromLayer(FLayerName);

		sol::table Result = LuaView.create_table();
		for (int32 i = 0; i < Actors.Num(); i++)
		{
			if (Actors[i])
				Result[i + 1] = std::string(TCHAR_TO_UTF8(*Actors[i]->GetActorLabel()));
		}

		Session.Log(FString::Printf(TEXT("[OK] get_actors_in_layer(\"%s\") -> %d actors"), *FLayerName.ToString(), Actors.Num()));
		return Result;
	});
}

// ============================================================================
// Paper2D Grouped Sprite helpers (standalone functions)
// ============================================================================

#if WITH_PAPER2D
static void BindPaper2DGroupedSprite(sol::state& Lua, FLuaSessionData& Session)
{
	// grouped_sprite_add(actor_name, {sprite="/Game/...", location={x,y,z}, rotation={pitch,yaw,roll}, scale={x,y,z}, color={r,g,b,a}})
	Lua.set_function("grouped_sprite_add", [&Session](const std::string& ActorName, sol::table Params, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!W) { Session.Log(TEXT("[FAIL] grouped_sprite_add -> no editor world")); return sol::lua_nil; }

		FString FActorName = UTF8_TO_TCHAR(ActorName.c_str());
		AActor* Actor = nullptr;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			if (It->GetActorLabel().Equals(FActorName, ESearchCase::IgnoreCase) ||
				It->GetName().Equals(FActorName, ESearchCase::IgnoreCase))
			{
				Actor = *It;
				break;
			}
		}
		if (!Actor) { Session.Log(FString::Printf(TEXT("[FAIL] grouped_sprite_add -> actor '%s' not found"), *FActorName)); return sol::lua_nil; }

		UPaperGroupedSpriteComponent* GSC = Actor->FindComponentByClass<UPaperGroupedSpriteComponent>();
		if (!GSC) { Session.Log(TEXT("[FAIL] grouped_sprite_add -> no PaperGroupedSpriteComponent on actor")); return sol::lua_nil; }

		std::string SpriteStr = Params.get_or("sprite", std::string());
		UPaperSprite* Sprite = nullptr;
		if (!SpriteStr.empty())
		{
			Sprite = LoadObject<UPaperSprite>(nullptr, UTF8_TO_TCHAR(SpriteStr.c_str()));
		}
		if (!Sprite) { Session.Log(TEXT("[FAIL] grouped_sprite_add -> 'sprite' asset path required")); return sol::lua_nil; }

		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		FVector Scale = FVector::OneVector;
		FLinearColor Color = FLinearColor::White;

		if (auto LT = Params.get<sol::optional<sol::table>>("location"))
		{
			Location.X = static_cast<float>(LT.value().get_or("x", 0.0));
			Location.Y = static_cast<float>(LT.value().get_or("y", 0.0));
			Location.Z = static_cast<float>(LT.value().get_or("z", 0.0));
		}
		if (auto RT = Params.get<sol::optional<sol::table>>("rotation"))
		{
			Rotation.Pitch = static_cast<float>(RT.value().get_or("pitch", 0.0));
			Rotation.Yaw = static_cast<float>(RT.value().get_or("yaw", 0.0));
			Rotation.Roll = static_cast<float>(RT.value().get_or("roll", 0.0));
		}
		if (auto ST = Params.get<sol::optional<sol::table>>("scale"))
		{
			Scale.X = static_cast<float>(ST.value().get_or("x", 1.0));
			Scale.Y = static_cast<float>(ST.value().get_or("y", 1.0));
			Scale.Z = static_cast<float>(ST.value().get_or("z", 1.0));
		}
		if (auto CT = Params.get<sol::optional<sol::table>>("color"))
		{
			Color.R = static_cast<float>(CT.value().get_or("r", 1.0));
			Color.G = static_cast<float>(CT.value().get_or("g", 1.0));
			Color.B = static_cast<float>(CT.value().get_or("b", 1.0));
			Color.A = static_cast<float>(CT.value().get_or("a", 1.0));
		}

		FTransform Transform(Rotation, Location, Scale);
		int32 InstanceIdx = GSC->AddInstance(Transform, Sprite, false, Color);
		Actor->MarkPackageDirty();

		Session.Log(FString::Printf(TEXT("[OK] grouped_sprite_add('%s') -> instance %d (total=%d)"),
			*FActorName, InstanceIdx, GSC->GetInstanceCount()));
		return sol::make_object(LuaView, InstanceIdx);
	});

	// grouped_sprite_remove(actor_name, instance_index)
	Lua.set_function("grouped_sprite_remove", [&Session](const std::string& ActorName, int Index, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!W) { Session.Log(TEXT("[FAIL] grouped_sprite_remove -> no editor world")); return sol::lua_nil; }

		FString FActorName = UTF8_TO_TCHAR(ActorName.c_str());
		AActor* Actor = nullptr;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			if (It->GetActorLabel().Equals(FActorName, ESearchCase::IgnoreCase) ||
				It->GetName().Equals(FActorName, ESearchCase::IgnoreCase))
			{ Actor = *It; break; }
		}
		if (!Actor) { Session.Log(FString::Printf(TEXT("[FAIL] grouped_sprite_remove -> actor '%s' not found"), *FActorName)); return sol::lua_nil; }

		UPaperGroupedSpriteComponent* GSC = Actor->FindComponentByClass<UPaperGroupedSpriteComponent>();
		if (!GSC) { Session.Log(TEXT("[FAIL] grouped_sprite_remove -> no grouped sprite component")); return sol::lua_nil; }

		if (Index < 0 || Index >= GSC->GetInstanceCount())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] grouped_sprite_remove -> index %d out of range (count=%d)"), Index, GSC->GetInstanceCount()));
			return sol::lua_nil;
		}

		bool bOK = GSC->RemoveInstance(Index);
		Actor->MarkPackageDirty();

		Session.Log(FString::Printf(TEXT("[OK] grouped_sprite_remove('%s', %d) -> %s (remaining=%d)"),
			*FActorName, Index, bOK ? TEXT("removed") : TEXT("failed"), GSC->GetInstanceCount()));
		return sol::make_object(LuaView, bOK);
	});

	// grouped_sprite_update(actor_name, instance_index, {location?, rotation?, scale?, color?})
	Lua.set_function("grouped_sprite_update", [&Session](const std::string& ActorName, int Index, sol::table Params, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!W) { Session.Log(TEXT("[FAIL] grouped_sprite_update -> no editor world")); return sol::lua_nil; }

		FString FActorName = UTF8_TO_TCHAR(ActorName.c_str());
		AActor* Actor = nullptr;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			if (It->GetActorLabel().Equals(FActorName, ESearchCase::IgnoreCase) ||
				It->GetName().Equals(FActorName, ESearchCase::IgnoreCase))
			{ Actor = *It; break; }
		}
		if (!Actor) { Session.Log(FString::Printf(TEXT("[FAIL] grouped_sprite_update -> actor '%s' not found"), *FActorName)); return sol::lua_nil; }

		UPaperGroupedSpriteComponent* GSC = Actor->FindComponentByClass<UPaperGroupedSpriteComponent>();
		if (!GSC) { Session.Log(TEXT("[FAIL] grouped_sprite_update -> no grouped sprite component")); return sol::lua_nil; }

		if (Index < 0 || Index >= GSC->GetInstanceCount())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] grouped_sprite_update -> index %d out of range"), Index));
			return sol::lua_nil;
		}

		TArray<FString> Changes;

		// Update transform
		if (Params.get<sol::optional<sol::table>>("location").has_value() ||
			Params.get<sol::optional<sol::table>>("rotation").has_value() ||
			Params.get<sol::optional<sol::table>>("scale").has_value())
		{
			FVector Loc = FVector::ZeroVector;
			FRotator Rot = FRotator::ZeroRotator;
			FVector Scl = FVector::OneVector;

			if (auto LT = Params.get<sol::optional<sol::table>>("location"))
			{
				Loc.X = static_cast<float>(LT.value().get_or("x", 0.0));
				Loc.Y = static_cast<float>(LT.value().get_or("y", 0.0));
				Loc.Z = static_cast<float>(LT.value().get_or("z", 0.0));
			}
			if (auto RT = Params.get<sol::optional<sol::table>>("rotation"))
			{
				Rot.Pitch = static_cast<float>(RT.value().get_or("pitch", 0.0));
				Rot.Yaw = static_cast<float>(RT.value().get_or("yaw", 0.0));
				Rot.Roll = static_cast<float>(RT.value().get_or("roll", 0.0));
			}
			if (auto ST = Params.get<sol::optional<sol::table>>("scale"))
			{
				Scl.X = static_cast<float>(ST.value().get_or("x", 1.0));
				Scl.Y = static_cast<float>(ST.value().get_or("y", 1.0));
				Scl.Z = static_cast<float>(ST.value().get_or("z", 1.0));
			}

			GSC->UpdateInstanceTransform(Index, FTransform(Rot, Loc, Scl));
			Changes.Add(TEXT("transform"));
		}

		// Update color
		if (auto CT = Params.get<sol::optional<sol::table>>("color"))
		{
			FLinearColor Color(
				static_cast<float>(CT.value().get_or("r", 1.0)),
				static_cast<float>(CT.value().get_or("g", 1.0)),
				static_cast<float>(CT.value().get_or("b", 1.0)),
				static_cast<float>(CT.value().get_or("a", 1.0))
			);
			GSC->UpdateInstanceColor(Index, Color);
			Changes.Add(TEXT("color"));
		}

		Actor->MarkPackageDirty();

		Session.Log(FString::Printf(TEXT("[OK] grouped_sprite_update('%s', %d) -> %s"),
			*FActorName, Index, *FString::Join(Changes, TEXT(", "))));
		return sol::make_object(LuaView, true);
	});

	// grouped_sprite_clear(actor_name)
	Lua.set_function("grouped_sprite_clear", [&Session](const std::string& ActorName, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!W) { Session.Log(TEXT("[FAIL] grouped_sprite_clear -> no editor world")); return sol::lua_nil; }

		FString FActorName = UTF8_TO_TCHAR(ActorName.c_str());
		AActor* Actor = nullptr;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			if (It->GetActorLabel().Equals(FActorName, ESearchCase::IgnoreCase) ||
				It->GetName().Equals(FActorName, ESearchCase::IgnoreCase))
			{ Actor = *It; break; }
		}
		if (!Actor) { Session.Log(FString::Printf(TEXT("[FAIL] grouped_sprite_clear -> actor '%s' not found"), *FActorName)); return sol::lua_nil; }

		UPaperGroupedSpriteComponent* GSC = Actor->FindComponentByClass<UPaperGroupedSpriteComponent>();
		if (!GSC) { Session.Log(TEXT("[FAIL] grouped_sprite_clear -> no grouped sprite component")); return sol::lua_nil; }

		int32 PrevCount = GSC->GetInstanceCount();
		GSC->ClearInstances();
		Actor->MarkPackageDirty();

		Session.Log(FString::Printf(TEXT("[OK] grouped_sprite_clear('%s') -> %d instances cleared"), *FActorName, PrevCount));
		return sol::make_object(LuaView, true);
	});

	// grouped_sprite_count(actor_name) -> integer
	Lua.set_function("grouped_sprite_count", [&Session](const std::string& ActorName, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!W) { Session.Log(TEXT("[FAIL] grouped_sprite_count -> no editor world")); return sol::lua_nil; }

		FString FActorName = UTF8_TO_TCHAR(ActorName.c_str());
		AActor* Actor = nullptr;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			if (It->GetActorLabel().Equals(FActorName, ESearchCase::IgnoreCase) ||
				It->GetName().Equals(FActorName, ESearchCase::IgnoreCase))
			{ Actor = *It; break; }
		}
		if (!Actor) { Session.Log(FString::Printf(TEXT("[FAIL] grouped_sprite_count -> actor '%s' not found"), *FActorName)); return sol::lua_nil; }

		UPaperGroupedSpriteComponent* GSC = Actor->FindComponentByClass<UPaperGroupedSpriteComponent>();
		if (!GSC) { Session.Log(TEXT("[FAIL] grouped_sprite_count -> no grouped sprite component")); return sol::lua_nil; }

		return sol::make_object(LuaView, GSC->GetInstanceCount());
	});
}
#endif // WITH_PAPER2D

// ============================================================================
// Actor Property Reading (reflection-based, matches configure's write side)
// ============================================================================

static void BindActorPropertyReading(sol::state& Lua, FLuaSessionData& Session)
{
	// get_actor_properties(actor_name, opts?) -> array of {name, type, value, category}
	// opts can be: filter string, or table {filter?, all?, changed_only?}
	// changed_only=true (default) compares against CDO to only return modified properties
	Lua.set_function("get_actor_properties", [&Session](const std::string& Name,
		sol::optional<sol::object> Opts,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* W = GetEditorWorld();
		if (!W) { Session.Log(TEXT("[FAIL] get_actor_properties -> no editor world")); return sol::lua_nil; }

		FString ActorId = UTF8_TO_TCHAR(Name.c_str());
		AActor* Actor = FindActorByNameOrLabel(W, ActorId);
		if (!Actor) { Session.Log(FString::Printf(TEXT("[FAIL] get_actor_properties(\"%s\") -> not found"), *ActorId)); return sol::lua_nil; }

		// Parse options
		FString FFilter;
		bool bAll = false;
		bool bChangedOnly = false; // default: show all properties so agent can discover them

		if (Opts.has_value())
		{
			if (Opts.value().is<std::string>())
			{
				// Simple filter string
				FFilter = UTF8_TO_TCHAR(Opts.value().as<std::string>().c_str());
			}
			else if (Opts.value().is<sol::table>())
			{
				sol::table T = Opts.value().as<sol::table>();
				if (auto V = T.get<sol::optional<std::string>>("filter")) FFilter = UTF8_TO_TCHAR(V.value().c_str());
				bAll = T.get_or("all", false);
				bChangedOnly = T.get_or("changed_only", false);
			}
			else if (Opts.value().is<bool>())
			{
				// Legacy: second arg was "all" bool
				bAll = Opts.value().as<bool>();
			}
		}

		// Get CDO for comparison if changed_only
		UObject* CDO = bChangedOnly ? Actor->GetClass()->GetDefaultObject() : nullptr;

		sol::table Result = LuaView.create_table();
		int32 Index = 1;

		for (TFieldIterator<FProperty> PropIt(Actor->GetClass()); PropIt; ++PropIt)
		{
			FProperty* Property = *PropIt;
			if (Property->HasAnyPropertyFlags(CPF_Deprecated)) continue;
			if (!bAll && !Property->HasAnyPropertyFlags(CPF_Edit)) continue;
			if (bAll && Property->HasAnyPropertyFlags(CPF_Transient)) continue;

			FString PropName = Property->GetName();
			if (!FFilter.IsEmpty() && !PropName.Contains(FFilter, ESearchCase::IgnoreCase)) continue;

			// Compare against CDO — skip if identical
			if (CDO)
			{
				if (Property->Identical_InContainer(Actor, CDO))
					continue;
			}

			FString Type = NeoStackToolUtils::GetPropertyTypeName(Property);
			FString Value = NeoStackToolUtils::GetPropertyValueAsString(Actor, Property, Actor);
			FString Category = Property->GetMetaData(TEXT("Category"));
			if (Category.IsEmpty()) Category = TEXT("Default");

			if (Value.Len() > 200) Value = Value.Left(197) + TEXT("...");

			sol::table Entry = LuaView.create_table();
			Entry["name"] = TCHAR_TO_UTF8(*PropName);
			Entry["type"] = TCHAR_TO_UTF8(*Type);
			Entry["value"] = TCHAR_TO_UTF8(*Value);
			Entry["category"] = TCHAR_TO_UTF8(*Category);
			Result[Index++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] get_actor_properties(\"%s\", %s%s) -> %d properties"),
			*ActorId, FFilter.IsEmpty() ? TEXT("*") : *FFilter,
			bChangedOnly ? TEXT(", changed_only") : TEXT(""), Index - 1));
		return Result;
	});

	// get_actor_property(actor_name, property_name) -> value string
	// Reads a single property value from a level actor
	Lua.set_function("get_actor_property", [&Session](const std::string& Name,
		const std::string& PropertyName, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* W = GetEditorWorld();
		if (!W) { Session.Log(TEXT("[FAIL] get_actor_property -> no editor world")); return sol::lua_nil; }

		FString ActorId = UTF8_TO_TCHAR(Name.c_str());
		AActor* Actor = FindActorByNameOrLabel(W, ActorId);
		if (!Actor) { Session.Log(FString::Printf(TEXT("[FAIL] get_actor_property(\"%s\") -> not found"), *ActorId)); return sol::lua_nil; }

		FString FProp = UTF8_TO_TCHAR(PropertyName.c_str());

		// Try actor first, then root component fallback
		FProperty* Property = PropertyAccessUtil::FindPropertyByName(FName(*FProp), Actor->GetClass());
		UObject* Target = Actor;
		if (!Property)
		{
			// Case-insensitive fallback on actor
			for (TFieldIterator<FProperty> PropIt(Actor->GetClass()); PropIt; ++PropIt)
			{
				if (PropIt->GetName().Equals(FProp, ESearchCase::IgnoreCase))
				{
					Property = *PropIt;
					break;
				}
			}
		}
		if (!Property)
		{
			// Try root component
			if (USceneComponent* Root = Actor->GetRootComponent())
			{
				Property = PropertyAccessUtil::FindPropertyByName(FName(*FProp), Root->GetClass());
				if (Property) Target = Root;
				if (!Property)
				{
					for (TFieldIterator<FProperty> PropIt(Root->GetClass()); PropIt; ++PropIt)
					{
						if (PropIt->GetName().Equals(FProp, ESearchCase::IgnoreCase))
						{
							Property = *PropIt;
							Target = Root;
							break;
						}
					}
				}
			}
		}

		if (!Property)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] get_actor_property(\"%s\", \"%s\") -> property not found"), *ActorId, *FProp));
			return sol::lua_nil;
		}

		FString Value = NeoStackToolUtils::GetPropertyValueAsString(Target, Property, Target);
		Session.Log(FString::Printf(TEXT("[OK] get_actor_property(\"%s\", \"%s\") = \"%s\""),
			*ActorId, *FProp, *Value));
		return sol::make_object(LuaView, std::string(TCHAR_TO_UTF8(*Value)));
	});

	// get_component_properties(actor_name, component_name, filter?, all?) -> array of {name, type, value, category}
	// Reads ALL UProperties on a specific component of a level actor
	// get_component_properties(actor_name, comp_name, opts?) -> array of {name, type, value, category}
	// opts: filter string, or table {filter?, all?, changed_only?}
	// changed_only=true (default) compares against CDO to only return modified properties
	Lua.set_function("get_component_properties", [&Session](const std::string& ActorName,
		const std::string& CompName, sol::optional<sol::object> Opts,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* W = GetEditorWorld();
		if (!W) { Session.Log(TEXT("[FAIL] get_component_properties -> no editor world")); return sol::lua_nil; }

		FString ActorId = UTF8_TO_TCHAR(ActorName.c_str());
		AActor* Actor = FindActorByNameOrLabel(W, ActorId);
		if (!Actor) { Session.Log(FString::Printf(TEXT("[FAIL] get_component_properties(\"%s\") -> actor not found"), *ActorId)); return sol::lua_nil; }

		FString FCompName = UTF8_TO_TCHAR(CompName.c_str());
		UActorComponent* FoundComp = nullptr;
		TArray<UActorComponent*> AllComps;
		Actor->GetComponents(AllComps);
		for (UActorComponent* Comp : AllComps)
		{
			if (Comp->GetName().Equals(FCompName, ESearchCase::IgnoreCase)
				|| Comp->GetReadableName().Equals(FCompName, ESearchCase::IgnoreCase)
				|| Comp->GetClass()->GetName().Equals(FCompName, ESearchCase::IgnoreCase))
			{
				FoundComp = Comp;
				break;
			}
		}
		if (!FoundComp) { Session.Log(FString::Printf(TEXT("[FAIL] get_component_properties -> component \"%s\" not found on \"%s\""), *FCompName, *ActorId)); return sol::lua_nil; }

		// Parse options
		FString FFilter;
		bool bAll = false;
		bool bChangedOnly = false;

		if (Opts.has_value())
		{
			if (Opts.value().is<std::string>())
			{
				FFilter = UTF8_TO_TCHAR(Opts.value().as<std::string>().c_str());
			}
			else if (Opts.value().is<sol::table>())
			{
				sol::table T = Opts.value().as<sol::table>();
				if (auto V = T.get<sol::optional<std::string>>("filter")) FFilter = UTF8_TO_TCHAR(V.value().c_str());
				bAll = T.get_or("all", false);
				bChangedOnly = T.get_or("changed_only", true);
			}
			else if (Opts.value().is<bool>())
			{
				bAll = Opts.value().as<bool>();
			}
		}

		// Get CDO for comparison
		UObject* CDO = bChangedOnly ? FoundComp->GetClass()->GetDefaultObject() : nullptr;

		sol::table Result = LuaView.create_table();
		int32 Index = 1;

		for (TFieldIterator<FProperty> PropIt(FoundComp->GetClass()); PropIt; ++PropIt)
		{
			FProperty* Property = *PropIt;
			if (Property->HasAnyPropertyFlags(CPF_Deprecated)) continue;
			if (!bAll && !Property->HasAnyPropertyFlags(CPF_Edit)) continue;
			if (bAll && Property->HasAnyPropertyFlags(CPF_Transient)) continue;

			FString PropName = Property->GetName();
			if (!FFilter.IsEmpty() && !PropName.Contains(FFilter, ESearchCase::IgnoreCase)) continue;

			// Compare against CDO — skip if identical
			if (CDO)
			{
				if (Property->Identical_InContainer(FoundComp, CDO))
					continue;
			}

			FString Type = NeoStackToolUtils::GetPropertyTypeName(Property);
			FString Value = NeoStackToolUtils::GetPropertyValueAsString(FoundComp, Property, FoundComp);
			FString Category = Property->GetMetaData(TEXT("Category"));
			if (Category.IsEmpty()) Category = TEXT("Default");

			if (Value.Len() > 200) Value = Value.Left(197) + TEXT("...");

			sol::table Entry = LuaView.create_table();
			Entry["name"] = TCHAR_TO_UTF8(*PropName);
			Entry["type"] = TCHAR_TO_UTF8(*Type);
			Entry["value"] = TCHAR_TO_UTF8(*Value);
			Entry["category"] = TCHAR_TO_UTF8(*Category);
			Result[Index++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] get_component_properties(\"%s\", \"%s\"%s) -> %d properties"),
			*ActorId, *FCompName, bChangedOnly ? TEXT(", changed_only") : TEXT(""), Index - 1));
		return Result;
	});

	// list_actor_components(actor_name) -> array of {name, class}
	Lua.set_function("list_actor_components", [&Session](const std::string& Name, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* W = GetEditorWorld();
		if (!W) { Session.Log(TEXT("[FAIL] list_actor_components -> no editor world")); return sol::lua_nil; }

		FString ActorId = UTF8_TO_TCHAR(Name.c_str());
		AActor* Actor = FindActorByNameOrLabel(W, ActorId);
		if (!Actor) { Session.Log(FString::Printf(TEXT("[FAIL] list_actor_components(\"%s\") -> not found"), *ActorId)); return sol::lua_nil; }

		TArray<UActorComponent*> AllComps;
		Actor->GetComponents(AllComps);

		sol::table Result = LuaView.create_table();
		int32 Index = 1;
		for (UActorComponent* Comp : AllComps)
		{
			if (!Comp) continue;
			sol::table Entry = LuaView.create_table();
			Entry["name"] = TCHAR_TO_UTF8(*Comp->GetName());
			Entry["class"] = TCHAR_TO_UTF8(*Comp->GetClass()->GetName());
			Result[Index++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] list_actor_components(\"%s\") -> %d components"), *ActorId, Index - 1));
		return Result;
	});
}

// ============================================================================
// Actor Duplication & Selection
// ============================================================================

static void BindSelectionAndDuplication(sol::state& Lua, FLuaSessionData& Session)
{
	// duplicate_actor(name, opts?) -> actor info table or nil
	Lua.set_function("duplicate_actor", [&Session](const std::string& Name, sol::optional<sol::table> Opts, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* W = GetEditorWorld();
		if (!W) { Session.Log(TEXT("[FAIL] duplicate_actor -> no editor world")); return sol::lua_nil; }

		AActor* Actor = FindActorByNameOrLabel(W, UTF8_TO_TCHAR(Name.c_str()));
		if (!Actor) { Session.Log(FString::Printf(TEXT("[FAIL] duplicate_actor -> actor \"%s\" not found"), UTF8_TO_TCHAR(Name.c_str()))); return sol::lua_nil; }

		UEditorActorSubsystem* ActorSub = GetActorSubsystem();
		if (!ActorSub) { Session.Log(TEXT("[FAIL] duplicate_actor -> subsystem not available")); return sol::lua_nil; }

		// Parse offset
		FVector Offset = FVector::ZeroVector;
		if (Opts.has_value())
		{
			sol::optional<sol::table> OffsetTbl = (*Opts)["offset"];
			if (OffsetTbl.has_value())
				Offset = TableToVector(*OffsetTbl);
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Duplicate Actor")));
		AActor* NewActor = ActorSub->DuplicateActor(Actor, W, Offset);
		if (!NewActor)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] duplicate_actor(\"%s\") -> duplication failed"), UTF8_TO_TCHAR(Name.c_str())));
			return sol::lua_nil;
		}

		// Optionally set new label
		if (Opts.has_value())
		{
			sol::optional<std::string> NewLabel = (*Opts).get<sol::optional<std::string>>("new_label");
			if (NewLabel.has_value())
			{
				NewActor->SetActorLabel(UTF8_TO_TCHAR(NewLabel->c_str()));
			}
		}

		Session.Log(FString::Printf(TEXT("[OK] duplicate_actor(\"%s\") -> \"%s\""), UTF8_TO_TCHAR(Name.c_str()), *NewActor->GetActorLabel()));
		return ActorToTable(LuaView, NewActor);
	});

	// select_actor(name) -> true or nil
	Lua.set_function("select_actor", [&Session](const std::string& Name, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		if (!GEditor) { Session.Log(TEXT("[FAIL] select_actor -> no editor")); return sol::lua_nil; }
		UWorld* W = GetEditorWorld();
		if (!W) { Session.Log(TEXT("[FAIL] select_actor -> no editor world")); return sol::lua_nil; }

		AActor* Actor = FindActorByNameOrLabel(W, UTF8_TO_TCHAR(Name.c_str()));
		if (!Actor) { Session.Log(FString::Printf(TEXT("[FAIL] select_actor -> actor \"%s\" not found"), UTF8_TO_TCHAR(Name.c_str()))); return sol::lua_nil; }

		GEditor->SelectActor(Actor, /*bInSelected=*/true, /*bNotify=*/true);

		Session.Log(FString::Printf(TEXT("[OK] select_actor(\"%s\")"), UTF8_TO_TCHAR(Name.c_str())));
		return sol::make_object(LuaView, true);
	});

	// deselect_all() -> true
	Lua.set_function("deselect_all", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		if (!GEditor) { Session.Log(TEXT("[FAIL] deselect_all -> no editor")); return sol::lua_nil; }
		GEditor->SelectNone(/*bNoteSelectionChange=*/true, /*bDeselectBSPSurfs=*/true);

		Session.Log(TEXT("[OK] deselect_all()"));
		return sol::make_object(LuaView, true);
	});

	// get_selected_actors() -> array of actor info tables
	Lua.set_function("get_selected_actors", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		if (!GEditor) { Session.Log(TEXT("[FAIL] get_selected_actors -> no editor")); return sol::lua_nil; }
		USelection* Selection = GEditor->GetSelectedActors();
		if (!Selection) { Session.Log(TEXT("[FAIL] get_selected_actors -> no selection object")); return sol::lua_nil; }

		TArray<AActor*> SelectedActors;
		Selection->GetSelectedObjects<AActor>(SelectedActors);

		sol::table Result = LuaView.create_table();
		for (int32 i = 0; i < SelectedActors.Num(); i++)
		{
			if (SelectedActors[i])
				Result[i + 1] = ActorToTable(LuaView, SelectedActors[i]);
		}

		Session.Log(FString::Printf(TEXT("[OK] get_selected_actors -> %d actors"), SelectedActors.Num()));
		return Result;
	});
}

// ============================================================================
// Streaming Level Management
// ============================================================================

namespace
{

/** Find a streaming level by short name (e.g. "SubLevel_01") or full package name. */
ULevelStreaming* FindStreamingLevelByName(UWorld* World, const FString& Name)
{
	if (!World) return nullptr;

	for (ULevelStreaming* SL : World->GetStreamingLevels())
	{
		if (!SL) continue;

		// Match against full package name
		FString PkgName = SL->GetWorldAssetPackageFName().ToString();
		if (PkgName.Equals(Name, ESearchCase::IgnoreCase))
			return SL;

		// Match against short name (last part after /)
		FString ShortName;
		PkgName.Split(TEXT("/"), nullptr, &ShortName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (ShortName.Equals(Name, ESearchCase::IgnoreCase))
			return SL;
	}
	return nullptr;
}

/** Convert streaming level state enum to string. */
const TCHAR* StreamingStateToString(ELevelStreamingState State)
{
	switch (State)
	{
	case ELevelStreamingState::Removed:          return TEXT("Removed");
	case ELevelStreamingState::Unloaded:         return TEXT("Unloaded");
	case ELevelStreamingState::FailedToLoad:     return TEXT("FailedToLoad");
	case ELevelStreamingState::Loading:          return TEXT("Loading");
	case ELevelStreamingState::LoadedNotVisible:  return TEXT("LoadedNotVisible");
	case ELevelStreamingState::MakingVisible:    return TEXT("MakingVisible");
	case ELevelStreamingState::LoadedVisible:    return TEXT("LoadedVisible");
	case ELevelStreamingState::MakingInvisible:  return TEXT("MakingInvisible");
	default:                                     return TEXT("Unknown");
	}
}

/** Build a Lua table describing a streaming level. */
sol::table StreamingLevelToTable(sol::state_view& LuaView, ULevelStreaming* SL)
{
	sol::table T = LuaView.create_table();

	FString PkgName = SL->GetWorldAssetPackageFName().ToString();
	FString ShortName;
	PkgName.Split(TEXT("/"), nullptr, &ShortName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);

	T["name"] = std::string(TCHAR_TO_UTF8(*ShortName));
	T["package_name"] = std::string(TCHAR_TO_UTF8(*PkgName));
	T["is_loaded"] = SL->IsLevelLoaded();
	T["is_visible"] = SL->IsLevelVisible();
	T["streaming_state"] = std::string(TCHAR_TO_UTF8(StreamingStateToString(SL->GetLevelStreamingState())));

	// Level color
	FLinearColor C = SL->LevelColor;
	sol::table ColorT = LuaView.create_table();
	ColorT["r"] = C.R;
	ColorT["g"] = C.G;
	ColorT["b"] = C.B;
	ColorT["a"] = C.A;
	T["level_color"] = ColorT;

	// Transform
	FTransform Xf = SL->LevelTransform;
	sol::table XfT = LuaView.create_table();
	FVector Loc = Xf.GetLocation();
	FRotator Rot = Xf.Rotator();
	FVector Scale = Xf.GetScale3D();
	sol::table LocT = LuaView.create_table();
	LocT["x"] = Loc.X; LocT["y"] = Loc.Y; LocT["z"] = Loc.Z;
	sol::table RotT = LuaView.create_table();
	RotT["pitch"] = Rot.Pitch; RotT["yaw"] = Rot.Yaw; RotT["roll"] = Rot.Roll;
	sol::table ScT = LuaView.create_table();
	ScT["x"] = Scale.X; ScT["y"] = Scale.Y; ScT["z"] = Scale.Z;
	XfT["location"] = LocT;
	XfT["rotation"] = RotT;
	XfT["scale"] = ScT;
	T["transform"] = XfT;

	return T;
}

} // anonymous namespace

static void BindStreamingLevels(sol::state& Lua, FLuaSessionData& Session)
{
	// list_streaming_levels() -> array of streaming level info tables
	Lua.set_function("list_streaming_levels", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* W = GetEditorWorld();
		if (!W) { Session.Log(TEXT("[FAIL] list_streaming_levels -> no editor world")); return sol::lua_nil; }

		const TArray<ULevelStreaming*>& Levels = W->GetStreamingLevels();

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;
		for (ULevelStreaming* SL : Levels)
		{
			if (!SL) continue;
			Result[Idx++] = StreamingLevelToTable(LuaView, SL);
		}

		Session.Log(FString::Printf(TEXT("[OK] list_streaming_levels -> %d levels"), Idx - 1));
		return Result;
	});

	// set_streaming_level(name, opts) -> true or nil
	Lua.set_function("set_streaming_level", [&Session](const std::string& Name, sol::table Opts, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* W = GetEditorWorld();
		if (!W) { Session.Log(TEXT("[FAIL] set_streaming_level -> no editor world")); return sol::lua_nil; }

		FString LevelName = UTF8_TO_TCHAR(Name.c_str());
		ULevelStreaming* SL = FindStreamingLevelByName(W, LevelName);
		if (!SL)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] set_streaming_level -> streaming level \"%s\" not found"), *LevelName));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Set Streaming Level")));

		sol::optional<bool> Loaded = Opts.get<sol::optional<bool>>("loaded");
		sol::optional<bool> Visible = Opts.get<sol::optional<bool>>("visible");

		TArray<FString> Changes;

		if (Loaded.has_value())
		{
			SL->SetShouldBeLoaded(*Loaded);
			Changes.Add(FString::Printf(TEXT("loaded=%s"), *Loaded ? TEXT("true") : TEXT("false")));
		}

		if (Visible.has_value())
		{
			SL->SetShouldBeVisible(*Visible);
			Changes.Add(FString::Printf(TEXT("visible=%s"), *Visible ? TEXT("true") : TEXT("false")));
		}

		// Transform support: location, rotation, scale
		if (auto LT = Opts.get<sol::optional<sol::table>>("location"))
		{
			FTransform Xf = SL->LevelTransform;
			Xf.SetLocation(TableToVector(LT.value()));
			SL->LevelTransform = Xf;
			Changes.Add(TEXT("location"));
		}
		if (auto RT = Opts.get<sol::optional<sol::table>>("rotation"))
		{
			FTransform Xf = SL->LevelTransform;
			Xf.SetRotation(FQuat(TableToRotator(RT.value())));
			SL->LevelTransform = Xf;
			Changes.Add(TEXT("rotation"));
		}

#if WITH_EDITOR
		// Also sync editor visibility so the level shows/hides in the viewport immediately
		if (Visible.has_value())
		{
			SL->SetShouldBeVisibleInEditor(*Visible);
		}
		else if (Loaded.has_value() && !(*Loaded))
		{
			// If unloading, also hide in editor
			SL->SetShouldBeVisibleInEditor(false);
		}
#endif

		Session.Log(FString::Printf(TEXT("[OK] set_streaming_level(\"%s\") -> %s"),
			*LevelName, *FString::Join(Changes, TEXT(", "))));
		return sol::make_object(LuaView, true);
	});

	// add_streaming_level(package_path) -> streaming level info table or nil
	Lua.set_function("add_streaming_level", [&Session](const std::string& PackagePath, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* W = GetEditorWorld();
		if (!W) { Session.Log(TEXT("[FAIL] add_streaming_level -> no editor world")); return sol::lua_nil; }

		FString PkgPath = UTF8_TO_TCHAR(PackagePath.c_str());

		// Check if already exists
		ULevelStreaming* Existing = FindStreamingLevelByName(W, PkgPath);
		if (Existing)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] add_streaming_level -> \"%s\" already exists as a streaming level"), *PkgPath));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Add Streaming Level")));

		ULevelStreaming* NewSL = UEditorLevelUtils::AddLevelToWorld(W, *PkgPath, ULevelStreamingDynamic::StaticClass());
		if (!NewSL)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] add_streaming_level(\"%s\") -> failed to add level"), *PkgPath));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] add_streaming_level(\"%s\")"), *PkgPath));
		return StreamingLevelToTable(LuaView, NewSL);
	});

	// remove_streaming_level(name) -> true or nil
	Lua.set_function("remove_streaming_level", [&Session](const std::string& Name, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* W = GetEditorWorld();
		if (!W) { Session.Log(TEXT("[FAIL] remove_streaming_level -> no editor world")); return sol::lua_nil; }

		FString LevelName = UTF8_TO_TCHAR(Name.c_str());
		ULevelStreaming* SL = FindStreamingLevelByName(W, LevelName);
		if (!SL)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] remove_streaming_level -> streaming level \"%s\" not found"), *LevelName));
			return sol::lua_nil;
		}

		ULevel* LoadedLevel = SL->GetLoadedLevel();

		FScopedTransaction Transaction(FText::FromString(TEXT("Lua: Remove Streaming Level")));

		if (LoadedLevel)
		{
			// Use editor utility to properly remove the level
			bool bRemoved = UEditorLevelUtils::RemoveLevelFromWorld(LoadedLevel);
			if (!bRemoved)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_streaming_level(\"%s\") -> RemoveLevelFromWorld failed"), *LevelName));
				return sol::lua_nil;
			}
		}
		else
		{
			// Level not loaded — just remove the streaming object from the world
			W->RemoveStreamingLevel(SL);
			SL->MarkAsGarbage();
		}

		Session.Log(FString::Printf(TEXT("[OK] remove_streaming_level(\"%s\")"), *LevelName));
		return sol::make_object(LuaView, true);
	});
}

static void BindLevelManagement(sol::state& Lua, FLuaSessionData& Session)
{
	// create_level(path, opts?) -> table or nil
	Lua.set_function("create_level", [&Session](const std::string& Path, sol::optional<sol::table> MaybeOpts, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString AssetPath = UTF8_TO_TCHAR(Path.c_str());

		bool bWorldPartition = false;
		bool bOpen = true;
		FString TemplatePath;

		if (MaybeOpts.has_value())
		{
			sol::table Opts = *MaybeOpts;
			bWorldPartition = Opts.get_or("world_partition", false);
			bOpen = Opts.get_or("open", true);
			TemplatePath = UTF8_TO_TCHAR(Opts.get_or<std::string>("template", "").c_str());
		}

		// Resolve template aliases — agents can use short names
		if (!TemplatePath.IsEmpty())
		{
			if (TemplatePath.Equals(TEXT("open_world"), ESearchCase::IgnoreCase) || TemplatePath.Equals(TEXT("OpenWorld"), ESearchCase::IgnoreCase))
				TemplatePath = TEXT("/Engine/Maps/Templates/OpenWorld");
			else if (TemplatePath.Equals(TEXT("basic"), ESearchCase::IgnoreCase) || TemplatePath.Equals(TEXT("default"), ESearchCase::IgnoreCase))
				TemplatePath = TEXT("/Engine/Maps/Templates/Template_Default");
		}

		UWorld* NewWorld = nullptr;

		if (!TemplatePath.IsEmpty())
		{
			NewWorld = UEditorLoadingAndSavingUtils::NewMapFromTemplate(TemplatePath, /*bSaveExistingMap=*/false);
		}
		else
		{
			NewWorld = UEditorLoadingAndSavingUtils::NewBlankMap(/*bSaveExistingMap=*/false);
		}

		if (!NewWorld)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] create_level(\"%s\") -> failed to create new map"), *AssetPath));
			return sol::lua_nil;
		}

		// Enable World Partition if requested
		if (bWorldPartition)
		{
			AWorldSettings* WS = NewWorld->GetWorldSettings();
			if (WS)
			{
				UWorldPartition::CreateOrRepairWorldPartition(WS);
			}
		}

		// Save to the requested path
		bool bSaved = UEditorLoadingAndSavingUtils::SaveMap(NewWorld, AssetPath);
		if (!bSaved)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] create_level(\"%s\") -> map created but failed to save"), *AssetPath));
			return sol::lua_nil;
		}

		// If open=false, load back the previously open map (create always opens it)
		// Since NewBlankMap/NewMapFromTemplate already open the map, we only need to handle the case where
		// the user does NOT want to keep it open. For simplicity, if open=false we note it but don't
		// attempt to reload the old map — the agent can call load_level() to switch back.

		// Build result table
		sol::table Result = LuaView.create_table();
		Result["path"] = std::string(TCHAR_TO_UTF8(*AssetPath));
		Result["world_partition"] = bWorldPartition;

		Session.Log(FString::Printf(TEXT("[OK] create_level(\"%s\", world_partition=%s)"),
			*AssetPath, bWorldPartition ? TEXT("true") : TEXT("false")));
		return Result;
	});

	// load_level(path) -> true or nil
	Lua.set_function("load_level", [&Session](const std::string& Path, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString LevelPath = UTF8_TO_TCHAR(Path.c_str());

		if (LevelPath.IsEmpty())
		{
			Session.Log(TEXT("[FAIL] load_level -> path is empty"));
			return sol::lua_nil;
		}

		// UEditorLoadingAndSavingUtils::LoadMap takes a content path or filename and returns UWorld*
		UWorld* LoadedWorld = UEditorLoadingAndSavingUtils::LoadMap(LevelPath);
		if (!LoadedWorld)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] load_level(\"%s\") -> failed to load map"), *LevelPath));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] load_level(\"%s\")"), *LevelPath));
		return sol::make_object(LuaView, true);
	});

	// save_level_as(path) -> true or nil
	Lua.set_function("save_level_as", [&Session](const std::string& Path, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString AssetPath = UTF8_TO_TCHAR(Path.c_str());

		if (AssetPath.IsEmpty())
		{
			Session.Log(TEXT("[FAIL] save_level_as -> path is empty"));
			return sol::lua_nil;
		}

		UWorld* W = GetEditorWorld();
		if (!W)
		{
			Session.Log(TEXT("[FAIL] save_level_as -> no editor world"));
			return sol::lua_nil;
		}

		bool bSaved = UEditorLoadingAndSavingUtils::SaveMap(W, AssetPath);
		if (!bSaved)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] save_level_as(\"%s\") -> save failed"), *AssetPath));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] save_level_as(\"%s\")"), *AssetPath));
		return sol::make_object(LuaView, true);
	});
}

// ============================================================================
// Component Management (standalone functions)
// ============================================================================

static UActorComponent* FindComponentByName(AActor* Actor, const FString& CompName)
{
	TArray<UActorComponent*> AllComps;
	Actor->GetComponents(AllComps);
	for (UActorComponent* Comp : AllComps)
	{
		if (Comp->GetName().Equals(CompName, ESearchCase::IgnoreCase)
			|| Comp->GetReadableName().Equals(CompName, ESearchCase::IgnoreCase))
		{
			return Comp;
		}
	}
	return nullptr;
}

static UClass* FindComponentClass(const FString& ClassName)
{
	// Try exact UClass lookup first
	FString FullPath = FString::Printf(TEXT("/Script/Engine.%s"), *ClassName);
	UClass* FoundClass = FindObject<UClass>(nullptr, *FullPath);
	if (FoundClass && FoundClass->IsChildOf(UActorComponent::StaticClass()))
		return FoundClass;

	// Try common module paths
	static const TCHAR* ModulePaths[] = {
		TEXT("/Script/Engine."),
#if WITH_PAPER2D
		TEXT("/Script/Paper2D."),
#endif
		TEXT("/Script/Niagara."),
		TEXT("/Script/UMG."),
		TEXT("/Script/NavigationSystem."),
	};
	for (const TCHAR* Prefix : ModulePaths)
	{
		FString Path = FString(Prefix) + ClassName;
		FoundClass = FindObject<UClass>(nullptr, *Path);
		if (FoundClass && FoundClass->IsChildOf(UActorComponent::StaticClass()))
			return FoundClass;
	}

	// Fall back to iterating all classes
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->GetName().Equals(ClassName, ESearchCase::IgnoreCase)
			&& It->IsChildOf(UActorComponent::StaticClass())
			&& !It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
		{
			return *It;
		}
	}
	return nullptr;
}

static void BindComponentManagement(sol::state& Lua, FLuaSessionData& Session)
{
	// add_component(actor_name, params) -> table or nil
	Lua.set_function("add_component", [&Session](const std::string& ActorName, sol::table Params, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* W = GetEditorWorld();
		if (!W) { Session.Log(TEXT("[FAIL] add_component -> no editor world")); return sol::lua_nil; }

		FString ActorId = UTF8_TO_TCHAR(ActorName.c_str());
		AActor* Actor = FindActorByNameOrLabel(W, ActorId);
		if (!Actor) { Session.Log(FString::Printf(TEXT("[FAIL] add_component(\"%s\") -> actor not found"), *ActorId)); return sol::lua_nil; }

		// Required: type
		auto TypeOpt = Params.get<sol::optional<std::string>>("type");
		if (!TypeOpt.has_value()) { Session.Log(TEXT("[FAIL] add_component -> 'type' required (component class name)")); return sol::lua_nil; }
		FString TypeName = UTF8_TO_TCHAR(TypeOpt.value().c_str());

		UClass* CompClass = FindComponentClass(TypeName);
		if (!CompClass)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] add_component -> component class '%s' not found"), *TypeName));
			return sol::lua_nil;
		}

		// Optional: name
		FString CompName = TypeName;
		if (auto V = Params.get<sol::optional<std::string>>("name"))
			CompName = UTF8_TO_TCHAR(V.value().c_str());

		const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDAddComponent", "Level Design: Add Component"));
		Actor->Modify();

		UActorComponent* NewComp = NewObject<UActorComponent>(Actor, CompClass, FName(*CompName), RF_Transactional);
		if (!NewComp)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] add_component -> failed to create '%s'"), *TypeName));
			return sol::lua_nil;
		}

		// If it's a scene component, attach to root
		USceneComponent* SceneComp = Cast<USceneComponent>(NewComp);
		if (SceneComp)
		{
			USceneComponent* Root = Actor->GetRootComponent();
			if (Root)
			{
				SceneComp->AttachToComponent(Root, FAttachmentTransformRules::KeepRelativeTransform);
			}
			else
			{
				// No root — make this the root
				Actor->SetRootComponent(SceneComp);
			}
		}

		NewComp->RegisterComponent();
		Actor->AddInstanceComponent(NewComp);

		// Optional post-creation setup: mesh
		if (auto V = Params.get<sol::optional<std::string>>("mesh"))
		{
			UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(NewComp);
			if (SMC)
			{
				UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, UTF8_TO_TCHAR(V.value().c_str()));
				if (Mesh) SMC->SetStaticMesh(Mesh);
			}
		}

		// Optional: material
		if (auto V = Params.get<sol::optional<std::string>>("material"))
		{
			UMeshComponent* MC = Cast<UMeshComponent>(NewComp);
			if (MC)
			{
				UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, UTF8_TO_TCHAR(V.value().c_str()));
				if (Mat) MC->SetMaterial(0, Mat);
			}
		}

		Actor->RerunConstructionScripts();
		Actor->MarkPackageDirty();

		// Build result
		sol::table Result = LuaView.create_table();
		Result["name"] = std::string(TCHAR_TO_UTF8(*NewComp->GetName()));
		Result["class"] = std::string(TCHAR_TO_UTF8(*NewComp->GetClass()->GetName()));
		Result["is_scene_component"] = (SceneComp != nullptr);

		Session.Log(FString::Printf(TEXT("[OK] add_component(\"%s\", {type=\"%s\", name=\"%s\"})"),
			*ActorId, *TypeName, *NewComp->GetName()));
		return Result;
	});

	// remove_component(actor_name, component_name) -> true or nil
	Lua.set_function("remove_component", [&Session](const std::string& ActorName, const std::string& CompName, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* W = GetEditorWorld();
		if (!W) { Session.Log(TEXT("[FAIL] remove_component -> no editor world")); return sol::lua_nil; }

		FString ActorId = UTF8_TO_TCHAR(ActorName.c_str());
		AActor* Actor = FindActorByNameOrLabel(W, ActorId);
		if (!Actor) { Session.Log(FString::Printf(TEXT("[FAIL] remove_component(\"%s\") -> actor not found"), *ActorId)); return sol::lua_nil; }

		FString ComponentName = UTF8_TO_TCHAR(CompName.c_str());
		UActorComponent* Comp = FindComponentByName(Actor, ComponentName);
		if (!Comp)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] remove_component(\"%s\", \"%s\") -> component not found"), *ActorId, *ComponentName));
			return sol::lua_nil;
		}

		const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDRemoveComponent", "Level Design: Remove Component"));
		Actor->Modify();
		Actor->RemoveInstanceComponent(Comp);
		Comp->DestroyComponent();
		Actor->RerunConstructionScripts();
		Actor->MarkPackageDirty();

		Session.Log(FString::Printf(TEXT("[OK] remove_component(\"%s\", \"%s\")"), *ActorId, *ComponentName));
		return sol::make_object(LuaView, true);
	});

	// configure_component(actor_name, component_name, params) -> table or nil
	Lua.set_function("configure_component", [&Session](const std::string& ActorName, const std::string& CompName, sol::table Params, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UWorld* W = GetEditorWorld();
		if (!W) { Session.Log(TEXT("[FAIL] configure_component -> no editor world")); return sol::lua_nil; }

		FString ActorId = UTF8_TO_TCHAR(ActorName.c_str());
		AActor* Actor = FindActorByNameOrLabel(W, ActorId);
		if (!Actor) { Session.Log(FString::Printf(TEXT("[FAIL] configure_component(\"%s\") -> actor not found"), *ActorId)); return sol::lua_nil; }

		FString ComponentName = UTF8_TO_TCHAR(CompName.c_str());
		UActorComponent* Comp = FindComponentByName(Actor, ComponentName);
		if (!Comp)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] configure_component(\"%s\", \"%s\") -> component not found"), *ActorId, *ComponentName));
			return sol::lua_nil;
		}

		const FScopedTransaction Tx(NSLOCTEXT("AIK", "LDConfigComp", "Level Design: Configure Component"));
		Comp->Modify();
		TArray<FString> Changes;

		// Specific handlers for common component properties

		// mesh (StaticMeshComponent)
		if (auto V = Params.get<sol::optional<std::string>>("mesh"))
		{
			UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(Comp);
			if (SMC)
			{
				UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, UTF8_TO_TCHAR(V.value().c_str()));
				if (Mesh) { SMC->SetStaticMesh(Mesh); Changes.Add(TEXT("mesh")); }
			}
		}

		// material
		if (auto V = Params.get<sol::optional<std::string>>("material"))
		{
			UMeshComponent* MC = Cast<UMeshComponent>(Comp);
			if (MC)
			{
				UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, UTF8_TO_TCHAR(V.value().c_str()));
				if (Mat) { MC->SetMaterial(0, Mat); Changes.Add(TEXT("material")); }
			}
		}

		// material_index + material (set material at specific index)
		if (auto IdxOpt = Params.get<sol::optional<int>>("material_index"))
		{
			if (auto MatOpt = Params.get<sol::optional<std::string>>("material"))
			{
				UMeshComponent* MC = Cast<UMeshComponent>(Comp);
				if (MC)
				{
					UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, UTF8_TO_TCHAR(MatOpt.value().c_str()));
					if (Mat) { MC->SetMaterial(IdxOpt.value(), Mat); Changes.Add(TEXT("material_index")); }
				}
			}
		}

		// intensity (LightComponent)
		if (auto V = Params.get<sol::optional<double>>("intensity"))
		{
			ULightComponent* LC = Cast<ULightComponent>(Comp);
			if (LC) { LC->SetIntensity(static_cast<float>(V.value())); Changes.Add(TEXT("intensity")); }
		}

		// color (LightComponent, as "R,G,B" string)
		if (auto V = Params.get<sol::optional<std::string>>("color"))
		{
			ULightComponent* LC = Cast<ULightComponent>(Comp);
			if (LC)
			{
				FString ColorStr = UTF8_TO_TCHAR(V.value().c_str());
				TArray<FString> Parts;
				ColorStr.ParseIntoArray(Parts, TEXT(","), true);
				if (Parts.Num() >= 3)
				{
					FColor C(FCString::Atoi(*Parts[0]), FCString::Atoi(*Parts[1]), FCString::Atoi(*Parts[2]));
					LC->SetLightColor(C);
					Changes.Add(TEXT("color"));
				}
			}
		}

		// attenuation_radius (PointLight/SpotLight)
		if (auto V = Params.get<sol::optional<double>>("attenuation_radius"))
		{
			UPointLightComponent* PLC = Cast<UPointLightComponent>(Comp);
			if (PLC) { PLC->SetAttenuationRadius(static_cast<float>(V.value())); Changes.Add(TEXT("attenuation_radius")); }
		}

		// visibility
		if (auto V = Params.get<sol::optional<bool>>("visible"))
		{
			USceneComponent* SC = Cast<USceneComponent>(Comp);
			if (SC) { SC->SetVisibility(V.value(), /*bPropagateToChildren=*/ true); Changes.Add(TEXT("visible")); }
		}

		// relative location
		if (auto LT = Params.get<sol::optional<sol::table>>("location"))
		{
			USceneComponent* SC = Cast<USceneComponent>(Comp);
			if (SC) { SC->SetRelativeLocation(TableToVector(LT.value())); Changes.Add(TEXT("location")); }
		}

		// relative rotation
		if (auto RT = Params.get<sol::optional<sol::table>>("rotation"))
		{
			USceneComponent* SC = Cast<USceneComponent>(Comp);
			if (SC) { SC->SetRelativeRotation(TableToRotator(RT.value())); Changes.Add(TEXT("rotation")); }
		}

		// relative scale
		if (auto ST = Params.get<sol::optional<sol::table>>("scale"))
		{
			USceneComponent* SC = Cast<USceneComponent>(Comp);
			if (SC) { SC->SetRelativeScale3D(TableToVector(ST.value())); Changes.Add(TEXT("scale")); }
		}

		// Generic property bag — set arbitrary UPROPERTYs
		if (auto PropT = Params.get<sol::optional<sol::table>>("property"))
		{
			for (auto& KV : PropT.value())
			{
				if (!KV.first.is<std::string>() || !KV.second.is<std::string>()) continue;
				FString PropName = UTF8_TO_TCHAR(KV.first.as<std::string>().c_str());
				FString PropValue = UTF8_TO_TCHAR(KV.second.as<std::string>().c_str());

				FProperty* Prop = Comp->GetClass()->FindPropertyByName(FName(*PropName));
				if (Prop)
				{
					Comp->PreEditChange(Prop);
					FStringOutputDevice Errors;
					Prop->ImportText_InContainer(*PropValue, Comp, Comp, 0, &Errors);
					FPropertyChangedEvent PropEvent(Prop, EPropertyChangeType::ValueSet);
					Comp->PostEditChangeProperty(PropEvent);
					Changes.Add(PropName);
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[WARN] configure_component -> property '%s' not found on %s"), *PropName, *Comp->GetClass()->GetName()));
				}
			}
		}

		Comp->MarkRenderStateDirty();
		Actor->MarkPackageDirty();

		if (Changes.Num() == 0)
		{
			Session.Log(FString::Printf(TEXT("[WARN] configure_component(\"%s\", \"%s\") -> no properties changed"), *ActorId, *ComponentName));
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[OK] configure_component(\"%s\", \"%s\") -> %s"),
				*ActorId, *ComponentName, *FString::Join(Changes, TEXT(", "))));
		}

		sol::table Result = LuaView.create_table();
		Result["name"] = std::string(TCHAR_TO_UTF8(*Comp->GetName()));
		Result["class"] = std::string(TCHAR_TO_UTF8(*Comp->GetClass()->GetName()));
		return Result;
	});
}

// Helper to conditionally bind Paper2D, extracted outside macro to avoid #if inside macro arguments (MSVC C5101)
#if WITH_PAPER2D
static void LevelDesign_BindPaper2D(sol::state& Lua, FLuaSessionData& Session) { if (!FModuleManager::Get().IsModuleLoaded(TEXT("Paper2D"))) return; BindPaper2DGroupedSprite(Lua, Session); }
#else
static void LevelDesign_BindPaper2D(sol::state&, FLuaSessionData&) {}
#endif

REGISTER_LUA_BINDING(LevelDesign, LevelDesignDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindLevelDesign(Lua, Session);
	BindLayerManagement(Lua, Session);
	LevelDesign_BindPaper2D(Lua, Session);
	BindActorPropertyReading(Lua, Session);
	BindSelectionAndDuplication(Lua, Session);
	BindStreamingLevels(Lua, Session);
	BindLevelManagement(Lua, Session);
	BindComponentManagement(Lua, Session);
});
