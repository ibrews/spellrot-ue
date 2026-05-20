// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"

#include "InterchangeManager.h"
#include "InterchangeSourceData.h"
#include "InterchangeAssetImportData.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "EditorReimportHandler.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Exporters/Exporter.h"
#include "AssetExportTask.h"
#include "Animation/Skeleton.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "InterchangeGenericAssetsPipeline.h"
#include "InterchangeGenericAssetsPipelineSharedSettings.h"
#include "InterchangeGenericMeshPipeline.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

/**
 * Construct a UAssetImportTask from common parameters.
 * Caller is responsible for executing the task via IAssetTools.
 */
static UAssetImportTask* CreateImportTask(
	const FString& SourcePath,
	const FString& DestPath,
	const FString& DestName,
	bool bReplace,
	bool bAutomated,
	bool bSave)
{
	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = FPaths::ConvertRelativePathToFull(SourcePath);
	Task->DestinationPath = DestPath;
	if (!DestName.IsEmpty())
	{
		Task->DestinationName = DestName;
	}
	Task->bReplaceExisting = bReplace;
	Task->bReplaceExistingSettings = bReplace;
	Task->bAutomated = bAutomated;
	Task->bSave = bSave;
	Task->bAsync = false;
	return Task;
}

/**
 * Execute a single import task and return results as an array of asset paths.
 */
static bool ExecuteImportTask(UAssetImportTask* Task, TArray<FString>& OutPaths)
{
	IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();

	TArray<UAssetImportTask*> Tasks;
	Tasks.Add(Task);
	AssetTools.ImportAssetTasks(Tasks);

	const TArray<UObject*>& Objects = Task->GetObjects();
	for (UObject* Obj : Objects)
	{
		if (Obj)
		{
			OutPaths.Add(Obj->GetPathName());
		}
	}
	return Objects.Num() > 0;
}

/**
 * Build a Lua result table for import operations.
 */
static sol::table MakeImportResult(sol::state_view& Lua, bool bSuccess, const TArray<FString>& Paths)
{
	sol::table Result = Lua.create_table();
	Result["success"] = bSuccess;

	sol::table Assets = Lua.create_table();
	for (int32 i = 0; i < Paths.Num(); i++)
	{
		Assets[i + 1] = TCHAR_TO_UTF8(*Paths[i]);
	}
	Result["imported_assets"] = Assets;
	return Result;
}

// ============================================================================
// TEXTURE PROPERTY HELPERS
// ============================================================================

static TextureCompressionSettings ParseCompressionSetting(const FString& Str)
{
	if (Str.Equals(TEXT("Normalmap"), ESearchCase::IgnoreCase) || Str.Equals(TEXT("TC_Normalmap"), ESearchCase::IgnoreCase)) return TC_Normalmap;
	if (Str.Equals(TEXT("Masks"), ESearchCase::IgnoreCase)) return TC_Masks;
	if (Str.Equals(TEXT("Grayscale"), ESearchCase::IgnoreCase)) return TC_Grayscale;
	if (Str.Equals(TEXT("Displacementmap"), ESearchCase::IgnoreCase)) return TC_Displacementmap;
	if (Str.Equals(TEXT("VectorDisplacementmap"), ESearchCase::IgnoreCase)) return TC_VectorDisplacementmap;
	if (Str.Equals(TEXT("HDR"), ESearchCase::IgnoreCase)) return TC_HDR;
	if (Str.Equals(TEXT("HDR_Compressed"), ESearchCase::IgnoreCase) || Str.Equals(TEXT("HDRCompressed"), ESearchCase::IgnoreCase) || Str.Equals(TEXT("BC6H"), ESearchCase::IgnoreCase)) return TC_HDR_Compressed;
	if (Str.Equals(TEXT("Alpha"), ESearchCase::IgnoreCase) || Str.Equals(TEXT("BC4"), ESearchCase::IgnoreCase)) return TC_Alpha;
	if (Str.Equals(TEXT("BC7"), ESearchCase::IgnoreCase)) return TC_BC7;
	if (Str.Equals(TEXT("HalfFloat"), ESearchCase::IgnoreCase) || Str.Equals(TEXT("R16F"), ESearchCase::IgnoreCase)) return TC_HalfFloat;
	if (Str.Equals(TEXT("SingleFloat"), ESearchCase::IgnoreCase) || Str.Equals(TEXT("R32F"), ESearchCase::IgnoreCase)) return TC_SingleFloat;
	if (Str.Equals(TEXT("DistanceFieldFont"), ESearchCase::IgnoreCase)) return TC_DistanceFieldFont;
	if (Str.Equals(TEXT("Uncompressed"), ESearchCase::IgnoreCase) || Str.Equals(TEXT("RGBA8"), ESearchCase::IgnoreCase)) return TC_EditorIcon;
	if (Str.Equals(TEXT("HDR_F32"), ESearchCase::IgnoreCase) || Str.Equals(TEXT("RGBA32F"), ESearchCase::IgnoreCase)) return TC_HDR_F32;
	return TC_Default;
}

static TextureGroup ParseLODGroup(const FString& Str)
{
	if (Str.Equals(TEXT("World"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_World;
	if (Str.Equals(TEXT("WorldNormalMap"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_WorldNormalMap;
	if (Str.Equals(TEXT("WorldSpecular"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_WorldSpecular;
	if (Str.Equals(TEXT("Character"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_Character;
	if (Str.Equals(TEXT("CharacterNormalMap"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_CharacterNormalMap;
	if (Str.Equals(TEXT("CharacterSpecular"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_CharacterSpecular;
	if (Str.Equals(TEXT("Weapon"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_Weapon;
	if (Str.Equals(TEXT("WeaponNormalMap"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_WeaponNormalMap;
	if (Str.Equals(TEXT("WeaponSpecular"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_WeaponSpecular;
	if (Str.Equals(TEXT("Vehicle"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_Vehicle;
	if (Str.Equals(TEXT("VehicleNormalMap"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_VehicleNormalMap;
	if (Str.Equals(TEXT("VehicleSpecular"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_VehicleSpecular;
	if (Str.Equals(TEXT("Cinematic"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_Cinematic;
	if (Str.Equals(TEXT("Effects"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_Effects;
	if (Str.Equals(TEXT("EffectsNotFiltered"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_EffectsNotFiltered;
	if (Str.Equals(TEXT("Skybox"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_Skybox;
	if (Str.Equals(TEXT("UI"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_UI;
	if (Str.Equals(TEXT("Lightmap"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_Lightmap;
	if (Str.Equals(TEXT("RenderTarget"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_RenderTarget;
	if (Str.Equals(TEXT("Shadowmap"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_Shadowmap;
	if (Str.Equals(TEXT("ColorLookupTable"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_ColorLookupTable;
	if (Str.Equals(TEXT("Bokeh"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_Bokeh;
	if (Str.Equals(TEXT("Pixels2D"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_Pixels2D;
	if (Str.Equals(TEXT("HierarchicalLOD"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_HierarchicalLOD;
	return TEXTUREGROUP_World;
}

static TextureMipGenSettings ParseMipGenSettings(const FString& Str)
{
	if (Str.Equals(TEXT("FromTextureGroup"), ESearchCase::IgnoreCase)) return TMGS_FromTextureGroup;
	if (Str.Equals(TEXT("SimpleAverage"), ESearchCase::IgnoreCase)) return TMGS_SimpleAverage;
	if (Str.Equals(TEXT("NoMipmaps"), ESearchCase::IgnoreCase)) return TMGS_NoMipmaps;
	if (Str.Equals(TEXT("LeaveExistingMips"), ESearchCase::IgnoreCase)) return TMGS_LeaveExistingMips;
	if (Str.Equals(TEXT("Unfiltered"), ESearchCase::IgnoreCase)) return TMGS_Unfiltered;
	if (Str.Equals(TEXT("Angular"), ESearchCase::IgnoreCase)) return TMGS_Angular;
	// Sharpen 0-10
	for (int32 i = 0; i <= 10; i++)
	{
		if (Str.Equals(FString::Printf(TEXT("Sharpen%d"), i), ESearchCase::IgnoreCase))
			return static_cast<TextureMipGenSettings>(TMGS_Sharpen0 + i);
	}
	// Blur 1-5
	for (int32 i = 1; i <= 5; i++)
	{
		if (Str.Equals(FString::Printf(TEXT("Blur%d"), i), ESearchCase::IgnoreCase))
			return static_cast<TextureMipGenSettings>(TMGS_Blur1 + (i - 1));
	}
	return TMGS_FromTextureGroup;
}

/**
 * Apply texture properties from opts table to all imported texture objects.
 * Supports: srgb, compression, lod_group, max_texture_size, mip_gen_settings
 */
static void ApplyTextureOptions(const TArray<UObject*>& Objects, const sol::table& Opts)
{
	for (UObject* Obj : Objects)
	{
		UTexture* Tex = Cast<UTexture>(Obj);
		if (!Tex) continue;
		Tex->Modify();

		// sRGB
		sol::optional<bool> SRGBOpt = Opts.get<sol::optional<bool>>("srgb");
		if (SRGBOpt.has_value())
		{
			Tex->SRGB = SRGBOpt.value() ? 1 : 0;
		}

		// Compression settings
		sol::optional<std::string> CompOpt = Opts.get<sol::optional<std::string>>("compression");
		if (!CompOpt.has_value()) CompOpt = Opts.get<sol::optional<std::string>>("compression_settings");
		if (CompOpt.has_value())
		{
			FString CompStr = UTF8_TO_TCHAR(CompOpt.value().c_str());
			Tex->CompressionSettings = ParseCompressionSetting(CompStr);
		}

		// LOD Group
		sol::optional<std::string> LODOpt = Opts.get<sol::optional<std::string>>("lod_group");
		if (LODOpt.has_value())
		{
			FString LODStr = UTF8_TO_TCHAR(LODOpt.value().c_str());
			Tex->LODGroup = ParseLODGroup(LODStr);
		}

		// Max texture size
		sol::optional<int> MaxSizeOpt = Opts.get<sol::optional<int>>("max_texture_size");
		if (MaxSizeOpt.has_value())
		{
			Tex->MaxTextureSize = FMath::Max(0, MaxSizeOpt.value());
		}

		// Mip gen settings
		sol::optional<std::string> MipOpt = Opts.get<sol::optional<std::string>>("mip_gen_settings");
		if (MipOpt.has_value())
		{
			FString MipStr = UTF8_TO_TCHAR(MipOpt.value().c_str());
			Tex->MipGenSettings = ParseMipGenSettings(MipStr);
		}

		Tex->PostEditChange();
		Tex->UpdateResource();
		Tex->MarkPackageDirty();
	}
}

// ============================================================================
// FBX IMPORT HELPERS
// ============================================================================

/**
 * Configure a UAssetImportTask with FBX-specific options from the opts table.
 * Creates a UFbxImportUI and attaches it to the task.
 * Supports: import_animations, import_mesh, import_materials, import_textures,
 *           skeleton (path), auto_generate_collision, import_as_skeletal,
 *           combine_meshes, build_nanite
 */
static void ConfigureFbxOptions(UAssetImportTask* Task, const sol::table& Opts)
{
	// Only create FBX UI if any FBX-specific option is provided
	bool bHasFbxOpts = false;
	static const char* FbxKeys[] = {
		"import_animations", "import_mesh", "import_materials", "import_textures",
		"skeleton", "auto_generate_collision", "import_as_skeletal", "type",
		"combine_meshes", "build_nanite", "rotation", "translation", "scale",
		"convert_scene", "convert_scene_unit", "force_front_x_axis",
		"transform_vertex_to_absolute", "bake_pivot_in_vertex", nullptr
	};
	for (int32 i = 0; FbxKeys[i]; i++)
	{
		if (Opts.get<sol::optional<sol::object>>(FbxKeys[i]).has_value())
		{
			bHasFbxOpts = true;
			break;
		}
	}
	if (!bHasFbxOpts) return;

	UFbxImportUI* FbxUI = NewObject<UFbxImportUI>();
	// Note: ResetToDefault() is not exported (MinimalAPI class) — but NewObject already
	// initializes all properties to CDO defaults, so the call is unnecessary.

	// Import animations
	sol::optional<bool> ImportAnimOpt = Opts.get<sol::optional<bool>>("import_animations");
	if (ImportAnimOpt.has_value()) FbxUI->bImportAnimations = ImportAnimOpt.value() ? 1 : 0;

	// Import mesh
	sol::optional<bool> ImportMeshOpt = Opts.get<sol::optional<bool>>("import_mesh");
	if (ImportMeshOpt.has_value()) FbxUI->bImportMesh = ImportMeshOpt.value();

	// Import materials
	sol::optional<bool> ImportMatOpt = Opts.get<sol::optional<bool>>("import_materials");
	if (ImportMatOpt.has_value()) FbxUI->bImportMaterials = ImportMatOpt.value() ? 1 : 0;

	// Import textures
	sol::optional<bool> ImportTexOpt = Opts.get<sol::optional<bool>>("import_textures");
	if (ImportTexOpt.has_value()) FbxUI->bImportTextures = ImportTexOpt.value() ? 1 : 0;

	// type="skeletal"|"static" shorthand (alternative to import_as_skeletal bool)
	sol::optional<std::string> TypeOpt = Opts.get<sol::optional<std::string>>("type");
	if (TypeOpt.has_value())
	{
		FString TypeStr = UTF8_TO_TCHAR(TypeOpt.value().c_str());
		if (TypeStr.Equals(TEXT("skeletal"), ESearchCase::IgnoreCase))
		{
			FbxUI->bImportAsSkeletal = true;
			FbxUI->SetMeshTypeToImport();
		}
		else if (TypeStr.Equals(TEXT("static"), ESearchCase::IgnoreCase))
		{
			FbxUI->bImportAsSkeletal = false;
			FbxUI->SetMeshTypeToImport();
		}
		// "auto" = default behavior, don't override
	}

	// Import as skeletal mesh (explicit bool, overrides type= if both set)
	sol::optional<bool> AsSkeletalOpt = Opts.get<sol::optional<bool>>("import_as_skeletal");
	if (AsSkeletalOpt.has_value())
	{
		FbxUI->bImportAsSkeletal = AsSkeletalOpt.value();
		FbxUI->SetMeshTypeToImport();
	}

	// Skeleton path (for skeletal mesh or animation import)
	sol::optional<std::string> SkeletonOpt = Opts.get<sol::optional<std::string>>("skeleton");
	if (SkeletonOpt.has_value())
	{
		FString SkelPath = UTF8_TO_TCHAR(SkeletonOpt.value().c_str());
		if (!SkelPath.Contains(TEXT(".")))
		{
			FString AssetName = FPaths::GetBaseFilename(SkelPath);
			SkelPath = SkelPath + TEXT(".") + AssetName;
		}
		USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *SkelPath);
		if (Skeleton)
		{
			FbxUI->Skeleton = Skeleton;
			// If skeleton is provided and not explicitly set as skeletal, assume skeletal mesh import
			if (!AsSkeletalOpt.has_value())
			{
				FbxUI->bImportAsSkeletal = true;
				FbxUI->SetMeshTypeToImport();
			}
		}
	}

	// Auto-generate collision (static mesh)
	sol::optional<bool> AutoCollisionOpt = Opts.get<sol::optional<bool>>("auto_generate_collision");
	if (AutoCollisionOpt.has_value() && FbxUI->StaticMeshImportData)
	{
		FbxUI->StaticMeshImportData->bAutoGenerateCollision = AutoCollisionOpt.value() ? 1 : 0;
	}

	// Combine meshes (static mesh)
	sol::optional<bool> CombineMeshesOpt = Opts.get<sol::optional<bool>>("combine_meshes");
	if (CombineMeshesOpt.has_value() && FbxUI->StaticMeshImportData)
	{
		FbxUI->StaticMeshImportData->bCombineMeshes = CombineMeshesOpt.value() ? 1 : 0;
	}

	// Build nanite (static mesh)
	sol::optional<bool> NaniteOpt = Opts.get<sol::optional<bool>>("build_nanite");
	if (NaniteOpt.has_value() && FbxUI->StaticMeshImportData)
	{
		FbxUI->StaticMeshImportData->bBuildNanite = NaniteOpt.value() ? 1 : 0;
	}

	// ---- Transform properties (rotation, translation, scale) ----
	// These are set on BOTH static and skeletal mesh import data sub-objects
	// so the correct one is used regardless of import type.

	// rotation={pitch=-90, yaw=0, roll=0} — e.g. Y-up to Z-up fix for CLO/Marvelous Designer
	sol::optional<sol::table> RotationOpt = Opts.get<sol::optional<sol::table>>("rotation");
	if (RotationOpt.has_value())
	{
		sol::table RT = RotationOpt.value();
		FRotator ImportRotation(
			RT.get_or("pitch", 0.0),
			RT.get_or("yaw", 0.0),
			RT.get_or("roll", 0.0)
		);
		if (FbxUI->StaticMeshImportData)
			FbxUI->StaticMeshImportData->ImportRotation = ImportRotation;
		if (FbxUI->SkeletalMeshImportData)
			FbxUI->SkeletalMeshImportData->ImportRotation = ImportRotation;
		if (FbxUI->AnimSequenceImportData)
			FbxUI->AnimSequenceImportData->ImportRotation = ImportRotation;
	}

	// translation={x=0, y=0, z=100} — offset the imported mesh
	sol::optional<sol::table> TranslationOpt = Opts.get<sol::optional<sol::table>>("translation");
	if (TranslationOpt.has_value())
	{
		sol::table TT = TranslationOpt.value();
		FVector ImportTranslation(
			TT.get_or("x", 0.0),
			TT.get_or("y", 0.0),
			TT.get_or("z", 0.0)
		);
		if (FbxUI->StaticMeshImportData)
			FbxUI->StaticMeshImportData->ImportTranslation = ImportTranslation;
		if (FbxUI->SkeletalMeshImportData)
			FbxUI->SkeletalMeshImportData->ImportTranslation = ImportTranslation;
		if (FbxUI->AnimSequenceImportData)
			FbxUI->AnimSequenceImportData->ImportTranslation = ImportTranslation;
	}

	// scale — uniform scale factor. Accepts number (scale=0.01) or table (scale={x=1,y=1,z=1})
	// FBX importer only supports uniform scale; for table input, uses x component.
	{
		float ImportScale = 0.f;
		bool bHasScale = false;
		sol::object ScaleObj = Opts["scale"];
		if (ScaleObj.is<double>() || ScaleObj.is<int>())
		{
			ImportScale = ScaleObj.is<double>() ? static_cast<float>(ScaleObj.as<double>()) : static_cast<float>(ScaleObj.as<int>());
			bHasScale = true;
		}
		else if (ScaleObj.is<sol::table>())
		{
			sol::table ST = ScaleObj.as<sol::table>();
			ImportScale = static_cast<float>(ST.get_or("x", 1.0));
			bHasScale = true;
		}
		if (bHasScale)
		{
			if (FbxUI->StaticMeshImportData)
				FbxUI->StaticMeshImportData->ImportUniformScale = ImportScale;
			if (FbxUI->SkeletalMeshImportData)
				FbxUI->SkeletalMeshImportData->ImportUniformScale = ImportScale;
			if (FbxUI->AnimSequenceImportData)
				FbxUI->AnimSequenceImportData->ImportUniformScale = ImportScale;
		}
	}

	// convert_scene=true — convert from FBX coordinate system to UE coordinate system
	sol::optional<bool> ConvertSceneOpt = Opts.get<sol::optional<bool>>("convert_scene");
	if (ConvertSceneOpt.has_value())
	{
		bool bVal = ConvertSceneOpt.value();
		if (FbxUI->StaticMeshImportData)
			FbxUI->StaticMeshImportData->bConvertScene = bVal;
		if (FbxUI->SkeletalMeshImportData)
			FbxUI->SkeletalMeshImportData->bConvertScene = bVal;
	}

	// convert_scene_unit=true — convert from FBX unit to UE unit (centimeters)
	sol::optional<bool> ConvertUnitOpt = Opts.get<sol::optional<bool>>("convert_scene_unit");
	if (ConvertUnitOpt.has_value())
	{
		bool bVal = ConvertUnitOpt.value();
		if (FbxUI->StaticMeshImportData)
			FbxUI->StaticMeshImportData->bConvertSceneUnit = bVal;
		if (FbxUI->SkeletalMeshImportData)
			FbxUI->SkeletalMeshImportData->bConvertSceneUnit = bVal;
	}

	// force_front_x_axis=true — force front axis to X instead of -Y
	sol::optional<bool> ForceFrontXOpt = Opts.get<sol::optional<bool>>("force_front_x_axis");
	if (ForceFrontXOpt.has_value())
	{
		bool bVal = ForceFrontXOpt.value();
		if (FbxUI->StaticMeshImportData)
			FbxUI->StaticMeshImportData->bForceFrontXAxis = bVal;
		if (FbxUI->SkeletalMeshImportData)
			FbxUI->SkeletalMeshImportData->bForceFrontXAxis = bVal;
	}

	// transform_vertex_to_absolute=true — apply node absolute transform to mesh vertices
	sol::optional<bool> TransformVertOpt = Opts.get<sol::optional<bool>>("transform_vertex_to_absolute");
	if (TransformVertOpt.has_value() && FbxUI->StaticMeshImportData)
	{
		FbxUI->StaticMeshImportData->bTransformVertexToAbsolute = TransformVertOpt.value();
	}

	// bake_pivot_in_vertex=true — apply inverse node rotation pivot to mesh vertices
	sol::optional<bool> BakePivotOpt = Opts.get<sol::optional<bool>>("bake_pivot_in_vertex");
	if (BakePivotOpt.has_value() && FbxUI->StaticMeshImportData)
	{
		FbxUI->StaticMeshImportData->bBakePivotInVertex = BakePivotOpt.value();
	}

	// Disable auto-detect so our settings are respected
	FbxUI->bAutomatedImportShouldDetectType = false;

	Task->Options = FbxUI;
}

// ============================================================================
// INTERCHANGE IMPORT (for GLB, OBJ, USD — non-FBX formats)
// ============================================================================

/**
 * Check if a file should use the Interchange pipeline instead of FBX.
 * FBX files use the legacy FBX importer (UFbxImportUI) for best compat.
 * All other formats (GLB, OBJ, USD, etc.) use Interchange directly.
 */
static bool ShouldUseInterchange(const FString& FilePath)
{
	FString Ext = FPaths::GetExtension(FilePath).ToLower();
	// FBX uses legacy path for maximum compatibility with UFbxImportUI settings
	return Ext != TEXT("fbx");
}

/**
 * Import a file via Interchange with full pipeline control.
 * Handles GLB, OBJ, USD, GLTF, and any format with a registered translator.
 */
static bool ExecuteInterchangeImport(
	const FString& SourcePath,
	const FString& DestPath,
	const FString& DestName,
	const sol::table& Opts,
	TArray<FString>& OutPaths,
	FLuaSessionData& Session)
{
	UInterchangeManager& Manager = UInterchangeManager::GetInterchangeManager();

	FString FullPath = FPaths::ConvertRelativePathToFull(SourcePath);
	UInterchangeSourceData* SourceData = NewObject<UInterchangeSourceData>();
	SourceData->SetFilename(FullPath);

	// Create the pipeline
	UInterchangeGenericAssetsPipeline* Pipeline = NewObject<UInterchangeGenericAssetsPipeline>();

	// Rotation offset: rotation={pitch=-90, yaw=0, roll=0}
	sol::optional<sol::table> RotOpt = Opts.get<sol::optional<sol::table>>("rotation");
	if (RotOpt.has_value())
	{
		sol::table RT = RotOpt.value();
		Pipeline->ImportOffsetRotation = FRotator(
			RT.get_or("pitch", 0.0),
			RT.get_or("yaw", 0.0),
			RT.get_or("roll", 0.0));
	}

	// Translation offset: translation={x=0, y=0, z=0}
	sol::optional<sol::table> TransOpt = Opts.get<sol::optional<sol::table>>("translation");
	if (TransOpt.has_value())
	{
		sol::table TT = TransOpt.value();
		Pipeline->ImportOffsetTranslation = FVector(
			TT.get_or("x", 0.0),
			TT.get_or("y", 0.0),
			TT.get_or("z", 0.0));
	}

	// Uniform scale
	sol::object ScaleObj = Opts["scale"];
	if (ScaleObj.is<double>() || ScaleObj.is<int>())
	{
		Pipeline->ImportOffsetUniformScale = ScaleObj.is<double>()
			? static_cast<float>(ScaleObj.as<double>())
			: static_cast<float>(ScaleObj.as<int>());
	}
	else if (ScaleObj.is<sol::table>())
	{
		Pipeline->ImportOffsetUniformScale = static_cast<float>(ScaleObj.as<sol::table>().get_or("x", 1.0));
	}

	// Force mesh type: type="skeletal"|"static" or import_as_skeletal=true
	bool bForceSkeletal = false;
	bool bForceStatic = false;

	sol::optional<std::string> TypeOpt = Opts.get<sol::optional<std::string>>("type");
	if (TypeOpt.has_value())
	{
		FString TypeStr = UTF8_TO_TCHAR(TypeOpt.value().c_str());
		if (TypeStr.Equals(TEXT("skeletal"), ESearchCase::IgnoreCase))
			bForceSkeletal = true;
		else if (TypeStr.Equals(TEXT("static"), ESearchCase::IgnoreCase))
			bForceStatic = true;
	}

	sol::optional<bool> AsSkeletalOpt = Opts.get<sol::optional<bool>>("import_as_skeletal");
	if (AsSkeletalOpt.has_value() && AsSkeletalOpt.value())
		bForceSkeletal = true;

	if (Pipeline->CommonMeshesProperties)
	{
		if (bForceSkeletal)
			Pipeline->CommonMeshesProperties->ForceAllMeshAsType = EInterchangeForceMeshType::IFMT_SkeletalMesh;
		else if (bForceStatic)
			Pipeline->CommonMeshesProperties->ForceAllMeshAsType = EInterchangeForceMeshType::IFMT_StaticMesh;
	}

	// Skeleton binding
	sol::optional<std::string> SkelOpt = Opts.get<sol::optional<std::string>>("skeleton");
	if (SkelOpt.has_value() && Pipeline->CommonSkeletalMeshesAndAnimationsProperties)
	{
		FString SkelPath = UTF8_TO_TCHAR(SkelOpt.value().c_str());
		if (!SkelPath.Contains(TEXT(".")))
		{
			FString AssetName = FPaths::GetBaseFilename(SkelPath);
			SkelPath = SkelPath + TEXT(".") + AssetName;
		}
		USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *SkelPath);
		if (Skeleton)
		{
			Pipeline->CommonSkeletalMeshesAndAnimationsProperties->Skeleton = Skeleton;
			// If skeleton provided and type not explicitly set, default to skeletal
			if (!bForceSkeletal && !bForceStatic && Pipeline->CommonMeshesProperties)
			{
				Pipeline->CommonMeshesProperties->ForceAllMeshAsType = EInterchangeForceMeshType::IFMT_SkeletalMesh;
			}
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[WARN] Interchange -> skeleton not found: %s"), *SkelPath));
		}
	}

	// Nanite
	sol::optional<bool> NaniteOpt = Opts.get<sol::optional<bool>>("build_nanite");
	if (NaniteOpt.has_value() && Pipeline->MeshPipeline)
	{
		Pipeline->MeshPipeline->bBuildNanite = NaniteOpt.value();
	}

	// Build import parameters
	FImportAssetParameters ImportParams;
	ImportParams.bIsAutomated = true;
	ImportParams.bReplaceExisting = Opts.get_or("replace", true);
	if (!DestName.IsEmpty())
	{
		ImportParams.DestinationName = DestName;
	}
	ImportParams.OverridePipelines.Add(FSoftObjectPath(Pipeline));

	// Synchronous import
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	UE::Interchange::FAssetImportResultRef Result = Manager.ImportAssetWithResult(DestPath, SourceData, ImportParams);
	Result->WaitUntilDone();

	TArray<UObject*> ImportedObjects = Result->GetImportedObjects();
	for (UObject* Obj : ImportedObjects)
	{
		if (Obj)
		{
			OutPaths.Add(Obj->GetPathName());
		}
	}

	return ImportedObjects.Num() > 0;
#else
	bool bSuccess = Manager.ImportAsset(DestPath, SourceData, ImportParams);
	return bSuccess;
#endif
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> AssetImportDocs = {
	{ TEXT("import_asset(source_path, dest_path, opts?)"), TEXT("Import a file into the project. FBX uses legacy importer; GLB/OBJ/USD use Interchange. opts: rotation, translation, scale, skeleton, type, import_animations, etc."), TEXT("{success, imported_assets}") },
	{ TEXT("import_assets(files_array, dest_path, opts?)"), TEXT("Batch import multiple files. Automatically routes FBX/non-FBX to appropriate pipeline."), TEXT("{success, count, imported_assets}") },
	{ TEXT("reimport_asset(asset_path)"), TEXT("Reimport an asset from its original source file"), TEXT("{success, source_file}") },
	{ TEXT("reimport_assets(asset_paths_array)"), TEXT("Batch reimport multiple assets from their original source files"), TEXT("{success, count}") },
	{ TEXT("can_import(source_path)"), TEXT("Check if Interchange can import the given file format"), TEXT("bool") },
	{ TEXT("can_reimport(asset_path)"), TEXT("Check if an asset can be reimported. Returns source file paths."), TEXT("{can_reimport, source_files}") },
	{ TEXT("get_import_data(asset_path)"), TEXT("Get import metadata for an asset (source files, interchange status)"), TEXT("table") },
	{ TEXT("get_supported_formats()"), TEXT("List all file formats supported by Interchange import"), TEXT("string[]") },
	{ TEXT("import_texture(source_path, dest_path, opts?)"), TEXT("Import a texture with post-import property overrides (srgb, compression, lod_group, etc.)"), TEXT("{success, imported_assets}") },
	{ TEXT("import_mesh(source_path, dest_path, opts?)"), TEXT("Import a mesh (FBX/GLB/OBJ/USD). opts: type='skeletal'|'static', rotation={pitch,yaw,roll}, translation={x,y,z}, scale, skeleton, build_nanite. GLB/OBJ use Interchange pipeline; FBX uses legacy importer."), TEXT("{success, imported_assets}") },
	{ TEXT("import_skeletal_mesh(source_path, dest_path, opts?)"), TEXT("Import as skeletal mesh (FBX/GLB/OBJ/USD). Auto-forces skeletal type. opts: skeleton, rotation={pitch,yaw,roll}, translation={x,y,z}, scale"), TEXT("{success, imported_assets}") },
	{ TEXT("import_audio(source_path, dest_path, opts?)"), TEXT("Import an audio file (WAV, OGG, etc.)"), TEXT("{success, imported_assets}") },
	{ TEXT("import_scene(source_path, dest_path, opts?)"), TEXT("Import an entire scene (GLB/USD/FBX) via Interchange — imports assets with transforms. opts: rotation, translation, scale, replace"), TEXT("{success, imported_assets}") },
	{ TEXT("import_with_options(source_path, dest_path, factory_class, options_object_path)"), TEXT("Import using a specific UFactory class and options object"), TEXT("{success, imported_assets}") },
	{ TEXT("export_asset(asset_path, file_path, opts?)"), TEXT("Export an asset to disk — format determined by file extension"), TEXT("{success, errors}") },
	{ TEXT("update_reimport_path(asset_path, source_file_path)"), TEXT("Update the source file path for reimporting an asset"), TEXT("{success}") },
};

REGISTER_LUA_BINDING(AssetImport, AssetImportDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("InterchangeImport")))
	{
		Session.Log(TEXT("[WARN] Interchange plugin is not loaded. Enable it in Edit > Plugins to use this feature."));
		return;
	}
	// ================================================================
	// 1. import_asset(source_path, dest_path, opts?)
	// ================================================================
	Lua.set_function("import_asset", [&Session](
		const std::string& SourcePath,
		const std::string& DestPath,
		sol::optional<sol::table> OptsOpt,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		FString FSrc = UTF8_TO_TCHAR(SourcePath.c_str());
		FString FDest = UTF8_TO_TCHAR(DestPath.c_str());

		if (!FPaths::FileExists(FPaths::ConvertRelativePathToFull(FSrc)))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] import_asset -> source file not found: %s"), *FSrc));
			sol::table R = LuaView.create_table();
			R["success"] = false;
			R["imported_assets"] = LuaView.create_table();
			return R;
		}

		FString DestName;
		bool bReplace = true;
		bool bAutomated = true;
		bool bSave = true;

		if (OptsOpt.has_value())
		{
			sol::table Opts = OptsOpt.value();
			sol::optional<std::string> NameOpt = Opts.get<sol::optional<std::string>>("name");
			if (NameOpt.has_value()) DestName = UTF8_TO_TCHAR(NameOpt.value().c_str());
			if (Opts.get<sol::optional<bool>>("replace").has_value()) bReplace = Opts.get<bool>("replace");
			if (Opts.get<sol::optional<bool>>("automated").has_value()) bAutomated = Opts.get<bool>("automated");
			if (Opts.get<sol::optional<bool>>("save").has_value()) bSave = Opts.get<bool>("save");
		}

		TArray<FString> ImportedPaths;
		bool bSuccess = false;

		// Route non-FBX formats through Interchange when options with transform/mesh-type
		// keys are present, so rotation/translation/scale/skeleton/type are properly applied.
		if (ShouldUseInterchange(FSrc) && OptsOpt.has_value())
		{
			bSuccess = ExecuteInterchangeImport(FSrc, FDest, DestName, OptsOpt.value(), ImportedPaths, Session);
		}
		else if (ShouldUseInterchange(FSrc))
		{
			// Non-FBX with no opts — use default task (IAssetTools delegates to Interchange internally)
			UAssetImportTask* Task = CreateImportTask(FSrc, FDest, DestName, bReplace, bAutomated, bSave);
			bSuccess = ExecuteImportTask(Task, ImportedPaths);
		}
		else
		{
			// FBX — use legacy path with full UFbxImportUI support
			UAssetImportTask* Task = CreateImportTask(FSrc, FDest, DestName, bReplace, bAutomated, bSave);
			if (OptsOpt.has_value())
			{
				ConfigureFbxOptions(Task, OptsOpt.value());
			}
			bSuccess = ExecuteImportTask(Task, ImportedPaths);
		}

		Session.Log(FString::Printf(TEXT("[%s] import_asset -> %d asset(s) from %s"),
			bSuccess ? TEXT("OK") : TEXT("FAIL"), ImportedPaths.Num(), *FPaths::GetCleanFilename(FSrc)));

		return MakeImportResult(LuaView, bSuccess, ImportedPaths);
	});

	// ================================================================
	// 2. import_assets(files_array, dest_path, opts?)
	// ================================================================
	Lua.set_function("import_assets", [&Session](
		sol::table FilesArray,
		const std::string& DestPath,
		sol::optional<sol::table> OptsOpt,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		FString FDest = UTF8_TO_TCHAR(DestPath.c_str());

		FString DestName;
		bool bReplace = true;
		bool bAutomated = true;
		bool bSave = true;

		if (OptsOpt.has_value())
		{
			sol::table Opts = OptsOpt.value();
			sol::optional<std::string> NameOpt = Opts.get<sol::optional<std::string>>("name");
			if (NameOpt.has_value()) DestName = UTF8_TO_TCHAR(NameOpt.value().c_str());
			if (Opts.get<sol::optional<bool>>("replace").has_value()) bReplace = Opts.get<bool>("replace");
			if (Opts.get<sol::optional<bool>>("automated").has_value()) bAutomated = Opts.get<bool>("automated");
			if (Opts.get<sol::optional<bool>>("save").has_value()) bSave = Opts.get<bool>("save");
		}

		TArray<UAssetImportTask*> LegacyTasks;
		TArray<FString> InterchangeFiles;
		int32 FileCount = 0;

		for (const auto& Pair : FilesArray)
		{
			sol::optional<std::string> FileOpt = Pair.second.as<sol::optional<std::string>>();
			if (!FileOpt.has_value()) continue;

			FString FSrc = UTF8_TO_TCHAR(FileOpt.value().c_str());
			if (!FPaths::FileExists(FPaths::ConvertRelativePathToFull(FSrc)))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] import_assets -> source file not found (skipped): %s"), *FSrc));
				continue;
			}

			// Route non-FBX through Interchange when opts are present, FBX through legacy
			if (ShouldUseInterchange(FSrc) && OptsOpt.has_value())
			{
				InterchangeFiles.Add(FSrc);
			}
			else
			{
				UAssetImportTask* Task = CreateImportTask(FSrc, FDest, DestName, bReplace, bAutomated, bSave);
				if (!ShouldUseInterchange(FSrc) && OptsOpt.has_value())
				{
					ConfigureFbxOptions(Task, OptsOpt.value());
				}
				LegacyTasks.Add(Task);
			}
			FileCount++;
		}

		if (FileCount == 0)
		{
			Session.Log(TEXT("[FAIL] import_assets -> no valid source files provided"));
			sol::table R = LuaView.create_table();
			R["success"] = false;
			R["count"] = 0;
			R["imported_assets"] = LuaView.create_table();
			return R;
		}

		TArray<FString> AllImported;

		// Execute legacy tasks in batch
		if (LegacyTasks.Num() > 0)
		{
			IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
			AssetTools.ImportAssetTasks(LegacyTasks);

			for (UAssetImportTask* Task : LegacyTasks)
			{
				const TArray<UObject*>& Objects = Task->GetObjects();
				for (UObject* Obj : Objects)
				{
					if (Obj)
					{
						AllImported.Add(Obj->GetPathName());
					}
				}
			}
		}

		// Execute Interchange imports individually (each needs its own pipeline)
		for (const FString& FSrc : InterchangeFiles)
		{
			TArray<FString> Paths;
			if (ExecuteInterchangeImport(FSrc, FDest, DestName, OptsOpt.value(), Paths, Session))
			{
				AllImported.Append(Paths);
			}
		}

		bool bSuccess = AllImported.Num() > 0;
		Session.Log(FString::Printf(TEXT("[%s] import_assets -> %d asset(s) imported from %d file(s)"),
			bSuccess ? TEXT("OK") : TEXT("FAIL"), AllImported.Num(), FileCount));

		sol::table Result = MakeImportResult(LuaView, bSuccess, AllImported);
		Result["count"] = AllImported.Num();
		return Result;
	});

	// ================================================================
	// 3. reimport_asset(asset_path)
	// ================================================================
	Lua.set_function("reimport_asset", [&Session](
		const std::string& AssetPath,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString FPath = UTF8_TO_TCHAR(AssetPath.c_str());

		UObject* Asset = LoadObject<UObject>(nullptr, *FPath);
		if (!Asset)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] reimport_asset -> could not load asset: %s"), *FPath));
			sol::table R = LuaView.create_table();
			R["success"] = false;
			return R;
		}

		// Get source file before reimport
		FString SourceFile;
		UAssetImportData* ImportData = nullptr;

		// Check for Interchange import data first
		UInterchangeAssetImportData* InterchangeData = UInterchangeAssetImportData::GetFromObject(Asset);
		if (InterchangeData)
		{
			ImportData = InterchangeData;
		}
		else
		{
			// Fall back to general asset import data by searching sub-objects
			TArray<UObject*> SubObjects;
			GetObjectsWithOuter(Asset, SubObjects);
			for (UObject* SubObj : SubObjects)
			{
				if (UAssetImportData* FoundData = Cast<UAssetImportData>(SubObj))
				{
					ImportData = FoundData;
					break;
				}
			}
		}

		if (ImportData)
		{
			SourceFile = ImportData->GetFirstFilename();
		}

		bool bSuccess = FReimportManager::Instance()->Reimport(
			Asset,
			/*bAskForNewFileIfMissing=*/ false,
			/*bShowNotification=*/ true,
			/*PreferredReimportFile=*/ TEXT(""),
			/*SpecifiedReimportHandler=*/ nullptr,
			/*SourceFileIndex=*/ INDEX_NONE,
			/*bForceNewFile=*/ false,
			/*bAutomated=*/ true
		);

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		if (!SourceFile.IsEmpty())
		{
			Result["source_file"] = TCHAR_TO_UTF8(*SourceFile);
		}

		Session.Log(FString::Printf(TEXT("[%s] reimport_asset -> %s%s"),
			bSuccess ? TEXT("OK") : TEXT("FAIL"),
			*Asset->GetName(),
			SourceFile.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (source: %s)"), *FPaths::GetCleanFilename(SourceFile))));

		return Result;
	});

	// ================================================================
	// 4. can_import(source_path)
	// ================================================================
	Lua.set_function("can_import", [&Session](
		const std::string& SourcePath,
		sol::this_state S) -> bool
	{
		FString FSrc = FPaths::ConvertRelativePathToFull(UTF8_TO_TCHAR(SourcePath.c_str()));

		if (!UInterchangeManager::IsInterchangeImportEnabled())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] can_import -> Interchange is disabled")));
			return false;
		}

		UInterchangeSourceData* SourceData = NewObject<UInterchangeSourceData>();
		SourceData->SetFilename(FSrc);

		UInterchangeManager& Manager = UInterchangeManager::GetInterchangeManager();
		bool bCan = Manager.CanTranslateSourceData(SourceData);

		Session.Log(FString::Printf(TEXT("[OK] can_import(\"%s\") -> %s"),
			*FPaths::GetCleanFilename(FSrc), bCan ? TEXT("true") : TEXT("false")));

		return bCan;
	});

	// ================================================================
	// 5. get_import_data(asset_path)
	// ================================================================
	Lua.set_function("get_import_data", [&Session](
		const std::string& AssetPath,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString FPath = UTF8_TO_TCHAR(AssetPath.c_str());

		UObject* Asset = LoadObject<UObject>(nullptr, *FPath);
		if (!Asset)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] get_import_data -> could not load asset: %s"), *FPath));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		bool bHasInterchangeData = false;
		TArray<FString> SourceFiles;

		// Check for Interchange import data first
		UInterchangeAssetImportData* InterchangeData = UInterchangeAssetImportData::GetFromObject(Asset);
		if (InterchangeData)
		{
			bHasInterchangeData = true;
			SourceFiles = InterchangeData->ExtractFilenames();
		}
		else
		{
			// Fall back to standard UAssetImportData by searching sub-objects
			TArray<UObject*> SubObjects;
			GetObjectsWithOuter(Asset, SubObjects);
			for (UObject* SubObj : SubObjects)
			{
				if (UAssetImportData* ImportData = Cast<UAssetImportData>(SubObj))
				{
					SourceFiles = ImportData->ExtractFilenames();
					break;
				}
			}
		}

		Result["has_interchange_data"] = bHasInterchangeData;

		sol::table FilesTable = LuaView.create_table();
		for (int32 i = 0; i < SourceFiles.Num(); i++)
		{
			FilesTable[i + 1] = TCHAR_TO_UTF8(*SourceFiles[i]);
		}
		Result["source_files"] = FilesTable;

		Session.Log(FString::Printf(TEXT("[OK] get_import_data(\"%s\") -> %d source file(s), interchange=%s"),
			*Asset->GetName(), SourceFiles.Num(),
			bHasInterchangeData ? TEXT("true") : TEXT("false")));

		return Result;
	});

	// ================================================================
	// 6. get_supported_formats()
	// ================================================================
	Lua.set_function("get_supported_formats", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (!UInterchangeManager::IsInterchangeImportEnabled())
		{
			Session.Log(TEXT("[FAIL] get_supported_formats -> Interchange is disabled"));
			return sol::lua_nil;
		}

		UInterchangeManager& Manager = UInterchangeManager::GetInterchangeManager();

		// Gather formats for both asset and scene translators
		TArray<FString> AllFormats = Manager.GetSupportedFormats(EInterchangeTranslatorType::Assets);
		TArray<FString> SceneFormats = Manager.GetSupportedFormats(EInterchangeTranslatorType::Scenes);

		// Merge scene formats (avoid duplicates)
		for (const FString& Fmt : SceneFormats)
		{
			AllFormats.AddUnique(Fmt);
		}

		AllFormats.Sort();

		sol::table Result = LuaView.create_table();
		for (int32 i = 0; i < AllFormats.Num(); i++)
		{
			Result[i + 1] = TCHAR_TO_UTF8(*AllFormats[i]);
		}

		Session.Log(FString::Printf(TEXT("[OK] get_supported_formats -> %d format(s)"), AllFormats.Num()));
		return Result;
	});

	// ================================================================
	// 7. import_texture(source_path, dest_path, opts?)
	// ================================================================
	Lua.set_function("import_texture", [&Session](
		const std::string& SourcePath,
		const std::string& DestPath,
		sol::optional<sol::table> OptsOpt,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		FString FSrc = UTF8_TO_TCHAR(SourcePath.c_str());
		FString FDest = UTF8_TO_TCHAR(DestPath.c_str());

		if (!FPaths::FileExists(FPaths::ConvertRelativePathToFull(FSrc)))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] import_texture -> source file not found: %s"), *FSrc));
			sol::table R = LuaView.create_table();
			R["success"] = false;
			R["imported_assets"] = LuaView.create_table();
			return R;
		}

		FString DestName;
		if (OptsOpt.has_value())
		{
			sol::table Opts = OptsOpt.value();
			sol::optional<std::string> NameOpt = Opts.get<sol::optional<std::string>>("name");
			if (NameOpt.has_value()) DestName = UTF8_TO_TCHAR(NameOpt.value().c_str());
		}

		UAssetImportTask* Task = CreateImportTask(FSrc, FDest, DestName, true, true, true);

		TArray<FString> ImportedPaths;
		bool bSuccess = ExecuteImportTask(Task, ImportedPaths);

		// Apply texture properties (srgb, compression, lod_group, max_texture_size, mip_gen_settings)
		if (bSuccess && OptsOpt.has_value())
		{
			ApplyTextureOptions(Task->GetObjects(), OptsOpt.value());
		}

		Session.Log(FString::Printf(TEXT("[%s] import_texture -> %d asset(s) from %s"),
			bSuccess ? TEXT("OK") : TEXT("FAIL"), ImportedPaths.Num(), *FPaths::GetCleanFilename(FSrc)));

		return MakeImportResult(LuaView, bSuccess, ImportedPaths);
	});

	// ================================================================
	// 8. import_mesh(source_path, dest_path, opts?)
	// ================================================================
	Lua.set_function("import_mesh", [&Session](
		const std::string& SourcePath,
		const std::string& DestPath,
		sol::optional<sol::table> OptsOpt,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		FString FSrc = UTF8_TO_TCHAR(SourcePath.c_str());
		FString FDest = UTF8_TO_TCHAR(DestPath.c_str());

		if (!FPaths::FileExists(FPaths::ConvertRelativePathToFull(FSrc)))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] import_mesh -> source file not found: %s"), *FSrc));
			sol::table R = LuaView.create_table();
			R["success"] = false;
			R["imported_assets"] = LuaView.create_table();
			return R;
		}

		FString DestName;
		if (OptsOpt.has_value())
		{
			sol::table Opts = OptsOpt.value();
			sol::optional<std::string> NameOpt = Opts.get<sol::optional<std::string>>("name");
			if (NameOpt.has_value()) DestName = UTF8_TO_TCHAR(NameOpt.value().c_str());
		}

		TArray<FString> ImportedPaths;
		bool bSuccess = false;

		// Route non-FBX formats (GLB, OBJ, USD, etc.) through Interchange pipeline
		// which properly handles rotation, skeleton binding, and force-skeletal for all formats.
		// FBX uses the legacy UFbxImportUI path for maximum compatibility.
		if (ShouldUseInterchange(FSrc) && OptsOpt.has_value())
		{
			bSuccess = ExecuteInterchangeImport(FSrc, FDest, DestName, OptsOpt.value(), ImportedPaths, Session);
		}
		else if (ShouldUseInterchange(FSrc))
		{
			// Non-FBX with no opts — use default Interchange (no UFbxImportUI to confuse it)
			UAssetImportTask* Task = CreateImportTask(FSrc, FDest, DestName, true, true, true);
			bSuccess = ExecuteImportTask(Task, ImportedPaths);
		}
		else
		{
			// FBX — use legacy path with full UFbxImportUI support
			UAssetImportTask* Task = CreateImportTask(FSrc, FDest, DestName, true, true, true);
			if (OptsOpt.has_value())
			{
				ConfigureFbxOptions(Task, OptsOpt.value());
			}
			bSuccess = ExecuteImportTask(Task, ImportedPaths);
		}

		Session.Log(FString::Printf(TEXT("[%s] import_mesh -> %d asset(s) from %s"),
			bSuccess ? TEXT("OK") : TEXT("FAIL"), ImportedPaths.Num(), *FPaths::GetCleanFilename(FSrc)));

		return MakeImportResult(LuaView, bSuccess, ImportedPaths);
	});

	// ================================================================
	// 9. import_skeletal_mesh(source_path, dest_path, opts?)
	// ================================================================
	Lua.set_function("import_skeletal_mesh", [&Session](
		const std::string& SourcePath,
		const std::string& DestPath,
		sol::optional<sol::table> OptsOpt,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		FString FSrc = UTF8_TO_TCHAR(SourcePath.c_str());
		FString FDest = UTF8_TO_TCHAR(DestPath.c_str());

		if (!FPaths::FileExists(FPaths::ConvertRelativePathToFull(FSrc)))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] import_skeletal_mesh -> source file not found: %s"), *FSrc));
			sol::table R = LuaView.create_table();
			R["success"] = false;
			R["imported_assets"] = LuaView.create_table();
			return R;
		}

		FString DestName;
		if (OptsOpt.has_value())
		{
			sol::table Opts = OptsOpt.value();
			sol::optional<std::string> NameOpt = Opts.get<sol::optional<std::string>>("name");
			if (NameOpt.has_value()) DestName = UTF8_TO_TCHAR(NameOpt.value().c_str());
		}

		TArray<FString> ImportedPaths;
		bool bSuccess = false;

		// For non-FBX (GLB, OBJ, etc.): use Interchange with force-skeletal
		if (ShouldUseInterchange(FSrc))
		{
			// Create a temp opts table with type="skeletal" forced
			sol::state_view TempLua(S);
			sol::table ForcedOpts = TempLua.create_table();
			ForcedOpts["type"] = "skeletal";
			if (OptsOpt.has_value())
			{
				// Copy user opts, ensuring type is skeletal
				for (const auto& Pair : OptsOpt.value())
				{
					if (Pair.first.is<std::string>())
					{
						std::string Key = Pair.first.as<std::string>();
						if (Key != "type") // Don't override our force
							ForcedOpts[Key] = Pair.second;
					}
				}
			}
			bSuccess = ExecuteInterchangeImport(FSrc, FDest, DestName, ForcedOpts, ImportedPaths, Session);
		}
		else
		{
			// FBX — legacy path, force skeletal mesh type
			UAssetImportTask* Task = CreateImportTask(FSrc, FDest, DestName, true, true, true);
			if (OptsOpt.has_value())
			{
				ConfigureFbxOptions(Task, OptsOpt.value());
			}
			// Ensure skeletal is forced — ConfigureFbxOptions may or may not have created a UFbxImportUI
			UFbxImportUI* FbxUI = Cast<UFbxImportUI>(Task->Options);
			if (!FbxUI)
			{
				FbxUI = NewObject<UFbxImportUI>();
				FbxUI->bAutomatedImportShouldDetectType = false;
				Task->Options = FbxUI;
			}
			FbxUI->bImportAsSkeletal = true;
			FbxUI->SetMeshTypeToImport();
			bSuccess = ExecuteImportTask(Task, ImportedPaths);
		}

		Session.Log(FString::Printf(TEXT("[%s] import_skeletal_mesh -> %d asset(s) from %s"),
			bSuccess ? TEXT("OK") : TEXT("FAIL"), ImportedPaths.Num(), *FPaths::GetCleanFilename(FSrc)));

		return MakeImportResult(LuaView, bSuccess, ImportedPaths);
	});

	// ================================================================
	// 10. import_audio(source_path, dest_path, opts?)
	// ================================================================
	Lua.set_function("import_audio", [&Session](
		const std::string& SourcePath,
		const std::string& DestPath,
		sol::optional<sol::table> OptsOpt,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		FString FSrc = UTF8_TO_TCHAR(SourcePath.c_str());
		FString FDest = UTF8_TO_TCHAR(DestPath.c_str());

		if (!FPaths::FileExists(FPaths::ConvertRelativePathToFull(FSrc)))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] import_audio -> source file not found: %s"), *FSrc));
			sol::table R = LuaView.create_table();
			R["success"] = false;
			R["imported_assets"] = LuaView.create_table();
			return R;
		}

		FString DestName;
		if (OptsOpt.has_value())
		{
			sol::table Opts = OptsOpt.value();
			sol::optional<std::string> NameOpt = Opts.get<sol::optional<std::string>>("name");
			if (NameOpt.has_value()) DestName = UTF8_TO_TCHAR(NameOpt.value().c_str());
		}

		UAssetImportTask* Task = CreateImportTask(FSrc, FDest, DestName, true, true, true);

		TArray<FString> ImportedPaths;
		bool bSuccess = ExecuteImportTask(Task, ImportedPaths);

		Session.Log(FString::Printf(TEXT("[%s] import_audio -> %d asset(s) from %s"),
			bSuccess ? TEXT("OK") : TEXT("FAIL"), ImportedPaths.Num(), *FPaths::GetCleanFilename(FSrc)));

		return MakeImportResult(LuaView, bSuccess, ImportedPaths);
	});

	// ================================================================
	// 11. import_with_options(source_path, dest_path, factory_class, options_object_path)
	// ================================================================
	Lua.set_function("import_with_options", [&Session](
		const std::string& SourcePath,
		const std::string& DestPath,
		const std::string& FactoryClassName,
		const std::string& OptionsObjectPath,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		FString FSrc = UTF8_TO_TCHAR(SourcePath.c_str());
		FString FDest = UTF8_TO_TCHAR(DestPath.c_str());
		FString FFactoryClass = UTF8_TO_TCHAR(FactoryClassName.c_str());
		FString FOptionsPath = UTF8_TO_TCHAR(OptionsObjectPath.c_str());

		if (!FPaths::FileExists(FPaths::ConvertRelativePathToFull(FSrc)))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] import_with_options -> source file not found: %s"), *FSrc));
			sol::table R = LuaView.create_table();
			R["success"] = false;
			R["imported_assets"] = LuaView.create_table();
			return R;
		}

		UAssetImportTask* Task = CreateImportTask(FSrc, FDest, TEXT(""), true, true, true);

		// Resolve the factory class
		UClass* FactoryClass = FindObject<UClass>(nullptr, *FFactoryClass);
		if (!FactoryClass)
		{
			// Try with /Script/UnrealEd. prefix
			FactoryClass = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/UnrealEd.%s"), *FFactoryClass));
		}
		if (!FactoryClass)
		{
			// Try FindFirstObject as last resort
			FactoryClass = FindFirstObject<UClass>(*FFactoryClass, EFindFirstObjectOptions::NativeFirst);
		}

		if (FactoryClass)
		{
			if (FactoryClass->HasAnyClassFlags(CLASS_Abstract))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] import_with_options -> factory class is abstract: %s"), *FFactoryClass));
				sol::table R = LuaView.create_table();
				R["success"] = false;
				R["imported_assets"] = LuaView.create_table();
				return R;
			}
			UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
			if (Factory)
			{
				Task->Factory = Factory;
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[FAIL] import_with_options -> could not instantiate factory: %s"), *FFactoryClass));
			}
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] import_with_options -> factory class not found: %s"), *FFactoryClass));
			sol::table R = LuaView.create_table();
			R["success"] = false;
			R["imported_assets"] = LuaView.create_table();
			return R;
		}

		// Resolve the options UObject
		if (!FOptionsPath.IsEmpty())
		{
			UObject* OptionsObj = LoadObject<UObject>(nullptr, *FOptionsPath);
			if (OptionsObj)
			{
				Task->Options = OptionsObj;
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[FAIL] import_with_options -> could not load options object: %s"), *FOptionsPath));
			}
		}

		TArray<FString> ImportedPaths;
		bool bSuccess = ExecuteImportTask(Task, ImportedPaths);

		Session.Log(FString::Printf(TEXT("[%s] import_with_options -> %d asset(s) from %s (factory: %s)"),
			bSuccess ? TEXT("OK") : TEXT("FAIL"),
			ImportedPaths.Num(),
			*FPaths::GetCleanFilename(FSrc),
			*FFactoryClass));

		return MakeImportResult(LuaView, bSuccess, ImportedPaths);
	});

	// ================================================================
	// 12. export_asset(asset_path, file_path, opts?)
	// ================================================================
	Lua.set_function("export_asset", [&Session](
		const std::string& AssetPath,
		const std::string& FilePath,
		sol::optional<sol::table> OptsOpt,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		FString FAssetPath = UTF8_TO_TCHAR(AssetPath.c_str());
		FString FFilePath = UTF8_TO_TCHAR(FilePath.c_str());

		UObject* Asset = LoadObject<UObject>(nullptr, *FAssetPath);
		if (!Asset)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] export_asset -> could not load asset: %s"), *FAssetPath));
			sol::table R = LuaView.create_table();
			R["success"] = false;
			return R;
		}

		bool bSelectedOnly = false;
		bool bReplace = true;

		if (OptsOpt.has_value())
		{
			sol::table Opts = OptsOpt.value();
			if (Opts.get<sol::optional<bool>>("selected_only").has_value()) bSelectedOnly = Opts.get<bool>("selected_only");
			if (Opts.get<sol::optional<bool>>("replace").has_value()) bReplace = Opts.get<bool>("replace");
		}

		UAssetExportTask* ExportTask = NewObject<UAssetExportTask>();
		ExportTask->Object = Asset;
		ExportTask->Filename = FPaths::ConvertRelativePathToFull(FFilePath);
		ExportTask->bSelected = bSelectedOnly;
		ExportTask->bReplaceIdentical = bReplace;
		ExportTask->bPrompt = false;
		ExportTask->bAutomated = true;
		ExportTask->bUseFileArchive = false;
		ExportTask->bWriteEmptyFiles = false;

		bool bSuccess = UExporter::RunAssetExportTask(ExportTask);

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;

		if (ExportTask->Errors.Num() > 0)
		{
			sol::table ErrorsTable = LuaView.create_table();
			for (int32 i = 0; i < ExportTask->Errors.Num(); i++)
			{
				ErrorsTable[i + 1] = TCHAR_TO_UTF8(*ExportTask->Errors[i]);
			}
			Result["errors"] = ErrorsTable;
		}

		Session.Log(FString::Printf(TEXT("[%s] export_asset -> %s to %s"),
			bSuccess ? TEXT("OK") : TEXT("FAIL"),
			*Asset->GetName(),
			*FPaths::GetCleanFilename(FFilePath)));

		return Result;
	});

	// ================================================================
	// 13. reimport_assets(asset_paths)
	// ================================================================
	Lua.set_function("reimport_assets", [&Session](
		sol::table AssetPathsArray,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		TArray<UObject*> Objects;
		TArray<FString> Paths;
		for (const auto& Pair : AssetPathsArray)
		{
			sol::optional<std::string> PathOpt = Pair.second.as<sol::optional<std::string>>();
			if (!PathOpt.has_value()) continue;

			FString FPath = UTF8_TO_TCHAR(PathOpt.value().c_str());
			UObject* Asset = LoadObject<UObject>(nullptr, *FPath);
			if (Asset)
			{
				Objects.Add(Asset);
				Paths.Add(FPath);
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[WARN] reimport_assets -> could not load asset (skipped): %s"), *FPath));
			}
		}

		if (Objects.Num() == 0)
		{
			Session.Log(TEXT("[FAIL] reimport_assets -> no valid assets to reimport"));
			sol::table R = LuaView.create_table();
			R["success"] = false;
			R["count"] = 0;
			return R;
		}

		bool bSuccess = FReimportManager::Instance()->ReimportMultiple(
			Objects,
			/*bAskForNewFileIfMissing=*/ false,
			/*bShowNotification=*/ true,
			/*PreferredReimportFile=*/ TEXT(""),
			/*SpecifiedReimportHandler=*/ nullptr,
			/*SourceFileIndex=*/ INDEX_NONE,
			/*bForceNewFile=*/ false,
			/*bAutomated=*/ true
		);

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		Result["count"] = Objects.Num();

		Session.Log(FString::Printf(TEXT("[%s] reimport_assets -> %d asset(s)"),
			bSuccess ? TEXT("OK") : TEXT("FAIL"), Objects.Num()));

		return Result;
	});

	// ================================================================
	// 14. import_scene(source_path, dest_path, opts?)
	// ================================================================
	Lua.set_function("import_scene", [&Session](
		const std::string& SourcePath,
		const std::string& DestPath,
		sol::optional<sol::table> OptsOpt,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		FString FSrc = UTF8_TO_TCHAR(SourcePath.c_str());
		FString FDest = UTF8_TO_TCHAR(DestPath.c_str());

		FString FullPath = FPaths::ConvertRelativePathToFull(FSrc);
		if (!FPaths::FileExists(FullPath))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] import_scene -> source file not found: %s"), *FSrc));
			sol::table R = LuaView.create_table();
			R["success"] = false;
			R["imported_assets"] = LuaView.create_table();
			return R;
		}

		UInterchangeManager& Manager = UInterchangeManager::GetInterchangeManager();

		UInterchangeSourceData* SourceData = NewObject<UInterchangeSourceData>();
		SourceData->SetFilename(FullPath);

		// Create the pipeline
		UInterchangeGenericAssetsPipeline* Pipeline = NewObject<UInterchangeGenericAssetsPipeline>();

		if (OptsOpt.has_value())
		{
			sol::table Opts = OptsOpt.value();

			// Rotation offset
			sol::optional<sol::table> RotOpt = Opts.get<sol::optional<sol::table>>("rotation");
			if (RotOpt.has_value())
			{
				sol::table RT = RotOpt.value();
				Pipeline->ImportOffsetRotation = FRotator(
					RT.get_or("pitch", 0.0),
					RT.get_or("yaw", 0.0),
					RT.get_or("roll", 0.0));
			}

			// Translation offset
			sol::optional<sol::table> TransOpt = Opts.get<sol::optional<sol::table>>("translation");
			if (TransOpt.has_value())
			{
				sol::table TT = TransOpt.value();
				Pipeline->ImportOffsetTranslation = FVector(
					TT.get_or("x", 0.0),
					TT.get_or("y", 0.0),
					TT.get_or("z", 0.0));
			}

			// Uniform scale
			sol::object ScaleObj = Opts["scale"];
			if (ScaleObj.is<double>() || ScaleObj.is<int>())
			{
				Pipeline->ImportOffsetUniformScale = ScaleObj.is<double>()
					? static_cast<float>(ScaleObj.as<double>())
					: static_cast<float>(ScaleObj.as<int>());
			}
			else if (ScaleObj.is<sol::table>())
			{
				Pipeline->ImportOffsetUniformScale = static_cast<float>(ScaleObj.as<sol::table>().get_or("x", 1.0));
			}
		}

		FImportAssetParameters ImportParams;
		ImportParams.bIsAutomated = true;
		ImportParams.bReplaceExisting = OptsOpt.has_value() ? OptsOpt.value().get_or("replace", true) : true;
		ImportParams.OverridePipelines.Add(FSoftObjectPath(Pipeline));

		bool bSuccess = Manager.ImportScene(FDest, SourceData, ImportParams);

		// Gather imported objects — ImportScene doesn't return them directly,
		// so we report success/fail based on the return value.
		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		Result["imported_assets"] = LuaView.create_table();

		Session.Log(FString::Printf(TEXT("[%s] import_scene -> %s"),
			bSuccess ? TEXT("OK") : TEXT("FAIL"),
			*FPaths::GetCleanFilename(FSrc)));

		return Result;
	});

	// ================================================================
	// 15. update_reimport_path(asset_path, source_file_path)
	// ================================================================
	Lua.set_function("update_reimport_path", [&Session](
		const std::string& AssetPath,
		const std::string& SourceFilePath,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString FAssetPath = UTF8_TO_TCHAR(AssetPath.c_str());
		FString FSourcePath = FPaths::ConvertRelativePathToFull(UTF8_TO_TCHAR(SourceFilePath.c_str()));

		UObject* Asset = LoadObject<UObject>(nullptr, *FAssetPath);
		if (!Asset)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] update_reimport_path -> could not load asset: %s"), *FAssetPath));
			sol::table R = LuaView.create_table();
			R["success"] = false;
			return R;
		}

		TArray<FString> NewPaths;
		NewPaths.Add(FSourcePath);
		FReimportManager::Instance()->UpdateReimportPaths(Asset, NewPaths);

		sol::table Result = LuaView.create_table();
		Result["success"] = true;

		Session.Log(FString::Printf(TEXT("[OK] update_reimport_path -> %s -> %s"),
			*Asset->GetName(), *FPaths::GetCleanFilename(FSourcePath)));

		return Result;
	});

	// ================================================================
	// 16. can_reimport(asset_path)
	// ================================================================
	Lua.set_function("can_reimport", [&Session](
		const std::string& AssetPath,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString FPath = UTF8_TO_TCHAR(AssetPath.c_str());

		UObject* Asset = LoadObject<UObject>(nullptr, *FPath);
		if (!Asset)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] can_reimport -> could not load asset: %s"), *FPath));
			sol::table R = LuaView.create_table();
			R["can_reimport"] = false;
			return R;
		}

		TArray<FString> SourceFilenames;
		bool bCanReimport = FReimportManager::Instance()->CanReimport(Asset, &SourceFilenames);

		sol::table Result = LuaView.create_table();
		Result["can_reimport"] = bCanReimport;

		sol::table FilesTable = LuaView.create_table();
		for (int32 i = 0; i < SourceFilenames.Num(); i++)
		{
			FilesTable[i + 1] = TCHAR_TO_UTF8(*SourceFilenames[i]);
		}
		Result["source_files"] = FilesTable;

		Session.Log(FString::Printf(TEXT("[OK] can_reimport(\"%s\") -> %s (%d source file(s))"),
			*Asset->GetName(), bCanReimport ? TEXT("true") : TEXT("false"), SourceFilenames.Num()));

		return Result;
	});
});

