// Copyright 2025 Betide Studio. All Rights Reserved.

#if WITH_PYTHON

#include "Tools/ExecutePythonTool.h"
#include "AgentIntegrationKitModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// Python Script Plugin
#include "IPythonScriptPlugin.h"
#include "PythonScriptTypes.h"
#include "Editor.h"

namespace
{
bool TryGetOptionalBoolField(const TSharedPtr<FJsonObject>& Args, const TCHAR* FieldName, bool& OutValue, bool& bOutWasSet, FString& OutError)
{
	bOutWasSet = false;

	if (!Args.IsValid() || !Args->HasField(FieldName))
	{
		return true;
	}

	bOutWasSet = true;

	if (Args->TryGetBoolField(FieldName, OutValue))
	{
		return true;
	}

	FString StringValue;
	if (Args->TryGetStringField(FieldName, StringValue))
	{
		StringValue.TrimStartAndEndInline();
		if (StringValue.Equals(TEXT("true"), ESearchCase::IgnoreCase) ||
			StringValue.Equals(TEXT("1")) ||
			StringValue.Equals(TEXT("yes"), ESearchCase::IgnoreCase))
		{
			OutValue = true;
			return true;
		}

		if (StringValue.Equals(TEXT("false"), ESearchCase::IgnoreCase) ||
			StringValue.Equals(TEXT("0")) ||
			StringValue.Equals(TEXT("no"), ESearchCase::IgnoreCase))
		{
			OutValue = false;
			return true;
		}
	}

	OutError = FString::Printf(TEXT("Field '%s' must be a boolean (true/false)"), FieldName);
	return false;
}

bool ContainsAnyToken(const FString& Source, const TArray<FString>& Tokens)
{
	for (const FString& Token : Tokens)
	{
		if (Source.Contains(Token, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}

	return false;
}

// Strip Python string literals and comments so heuristic pattern matching only sees actual code.
// Handles single/double quotes, triple quotes, escape chars, and # comments.
FString StripPythonCommentsAndStrings(const FString& Code)
{
	FString Result;
	Result.Reserve(Code.Len());

	bool bInTripleQuote = false;
	TCHAR TripleQuoteChar = 0;
	bool bInString = false;
	TCHAR StringDelim = 0;

	for (int32 i = 0; i < Code.Len(); ++i)
	{
		const TCHAR C = Code[i];

		if (bInTripleQuote)
		{
			if (C == TripleQuoteChar && i + 2 < Code.Len() && Code[i + 1] == TripleQuoteChar && Code[i + 2] == TripleQuoteChar)
			{
				i += 2;
				bInTripleQuote = false;
			}
			continue;
		}

		if (bInString)
		{
			if (C == TEXT('\\') && i + 1 < Code.Len())
			{
				++i;
				continue;
			}
			if (C == StringDelim)
			{
				bInString = false;
			}
			continue;
		}

		// Check for triple quotes
		if ((C == TEXT('\'') || C == TEXT('"')) && i + 2 < Code.Len() && Code[i + 1] == C && Code[i + 2] == C)
		{
			bInTripleQuote = true;
			TripleQuoteChar = C;
			i += 2;
			continue;
		}

		// Check for single/double quote strings
		if (C == TEXT('\'') || C == TEXT('"'))
		{
			bInString = true;
			StringDelim = C;
			continue;
		}

		// Check for comment — skip to end of line
		if (C == TEXT('#'))
		{
			while (i < Code.Len() && Code[i] != TEXT('\n'))
			{
				++i;
			}
			if (i < Code.Len())
			{
				Result += TEXT('\n');
			}
			continue;
		}

		Result += C;
	}

	return Result;
}

void WritePythonExecutionBreadcrumb(const FString& Stage, const FString& Code, const bool bUnattended, const bool bAllowUI, const FString& Extra = FString())
{
	const FString BreadcrumbLogPath = FPaths::ProjectLogDir() / TEXT("AIK_ExecutePython_LastCommand.txt");
	const uint32 CodeHash = FCrc::StrCrc32(*Code);
	FString Preview = Code.Left(160);
	Preview.ReplaceInline(TEXT("\r"), TEXT(" "));
	Preview.ReplaceInline(TEXT("\n"), TEXT("\\n"));

	FString Content;
	Content += FString::Printf(TEXT("Timestamp=%s\n"), *FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")));
	Content += FString::Printf(TEXT("Stage=%s\n"), *Stage);
	Content += FString::Printf(TEXT("CodeLength=%d\n"), Code.Len());
	Content += FString::Printf(TEXT("CodeCRC32=0x%08X\n"), CodeHash);
	Content += FString::Printf(TEXT("Unattended=%s\n"), bUnattended ? TEXT("true") : TEXT("false"));
	Content += FString::Printf(TEXT("AllowUI=%s\n"), bAllowUI ? TEXT("true") : TEXT("false"));
	Content += FString::Printf(TEXT("IsInPIE=%s\n"), (GEditor && GEditor->IsPlayingSessionInEditor()) ? TEXT("true") : TEXT("false"));
	Content += FString::Printf(TEXT("IsInGameThread=%s\n"), IsInGameThread() ? TEXT("true") : TEXT("false"));
	Content += FString::Printf(TEXT("Preview=%s\n"), *Preview);
	if (!Extra.IsEmpty())
	{
		Content += FString::Printf(TEXT("Extra=%s\n"), *Extra.Left(2048));
	}

	Content += TEXT("\n# CODE\n");
	Content += Code;
	Content += TEXT("\n");

	if (!FFileHelper::SaveStringToFile(Content, *BreadcrumbLogPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("[AIK] Failed to write Python execution breadcrumb to %s"), *BreadcrumbLogPath);
	}
}
}

FString FExecutePythonTool::GetDescription() const
{
	return TEXT(
			"Execute Python in Unreal Editor. Use 'import unreal' to access 1000+ APIs. Print output is captured.\n\n"
			"Runs in unattended mode by default. If you need to open editor UI (asset editors/windows), pass allow_ui=true.\n\n"

			"USE OTHER TOOLS FOR: graph nodes and node search (edit_graph), BP structure (edit_blueprint)\n"
			"USE PYTHON FOR: assets, actors, import/export, batch ops, BP variables, project management\n\n"

		"[CRITICAL SAFETY RULES - violating these WILL CRASH the editor]\n"
		"1. ALWAYS check load_asset/load_object results for None before use:\n"
		"   asset = unreal.load_asset('/Game/Path')\n"
		"   if asset is None:\n"
		"       print('ERROR: Asset not found'); return  # or skip operation\n"
		"2. ALWAYS verify asset exists before loading user-provided paths:\n"
		"   if not unreal.EditorAssetLibrary.does_asset_exist(path):\n"
		"       print(f'Asset not found: {path}'); return\n"
		"3. NEVER chain calls without checking each step:\n"
		"   BAD:  unreal.load_asset(p).get_editor_property('x')  # crashes if None\n"
		"   GOOD: obj = unreal.load_asset(p)\n"
		"         if obj: val = obj.get_editor_property('x')\n"
		"4. ALWAYS wrap operations in try/except when outcome is uncertain:\n"
		"   try:\n"
		"       obj.set_editor_property('prop', value)\n"
		"   except Exception as e:\n"
		"       print(f'Failed: {e}')\n"
		"5. ALWAYS check array/list length before indexing:\n"
		"   actors = subsystem.get_all_level_actors()\n"
		"   if len(actors) > 0: first = actors[0]\n"
		"6. NEVER assume object types - verify before type-specific ops:\n"
		"   bp = unreal.load_asset(path)\n"
		"   if not isinstance(bp, unreal.Blueprint):\n"
		"       print(f'Not a Blueprint: {type(bp)}'); return\n"
			"7. ALWAYS use get_editor_subsystem() for subsystems, not constructors:\n"
			"   GOOD: unreal.get_editor_subsystem(unreal.EditorActorSubsystem)\n"
			"   BAD:  unreal.EditorActorSubsystem()\n"
			"8. NEVER use get_default_object() on subsystem classes for editor actions:\n"
			"   GOOD: unreal.get_editor_subsystem(unreal.AssetEditorSubsystem).open_editor_for_assets([asset])\n"
			"   BAD:  unreal.AssetEditorSubsystem.get_default_object().open_editor_for_assets([asset])\n\n"

		"[BLUEPRINT VARIABLES - PREFERRED over edit_blueprint]\n"
		"bp = unreal.load_asset('/Game/MyBP')\n"
		"if bp is None: print('Blueprint not found')\n"
		"else: unreal.BlueprintEditorLibrary.add_member_variable(bp, 'VarName', type)\n"
		"Type helpers:\n"
		"  get_basic_type_by_name('real')  # bool/byte/int/real/name/string/text\n"
		"  get_object_reference_type(unreal.Actor)  # object refs\n"
		"  get_struct_type(unreal.Vector)  # structs like Vector, Transform, TimerHandle\n"
		"  get_array_type(inner_type)  # arrays of any type\n"
		"Cleanup: gather_unused_variables(bp), remove_unused_variables(bp)\n"
		"Compile: compile_blueprint(bp)\n\n"

		"[BLUEPRINT INTERFACES]\n"
		"factory = unreal.BlueprintInterfaceFactory()\n"
		"asset_tools = unreal.AssetToolsHelpers.get_asset_tools()\n"
		"bpi = asset_tools.create_asset('BPI_Name', '/Game/Blueprints', unreal.Blueprint, factory)\n\n"

		"[ASSET MANAGEMENT - unreal.EditorAssetLibrary]\n"
		"load_asset('/Game/Path') -> UObject\n"
		"does_asset_exist('/Game/Path') -> bool\n"
		"save_asset('/Game/Path', only_if_dirty=True)\n"
		"save_directory('/Game/Folder', only_if_dirty=True, recursive=True)\n"
		"delete_asset('/Game/Path')\n"
		"duplicate_asset('/Game/Src', '/Game/Dst') -> UObject\n"
		"rename_asset('/Game/Old', '/Game/New') -> bool\n"
		"list_assets('/Game/Folder', recursive=True, include_folder=False) -> [str]\n"
		"make_directory('/Game/NewFolder')\n"
		"get_metadata_tag(asset, 'TagName') / set_metadata_tag(asset, 'Tag', 'Value')\n\n"

		"[ADVANCED ASSETS - unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)]\n"
		"find_package_referencers_for_asset('/Game/Path', load_assets=False) -> [str] # dependencies\n"
		"consolidate_assets(target_asset, [assets_to_consolidate]) # merge duplicates\n"
		"list_assets_by_tag_value('TagName', 'Value') -> [FAssetData]\n\n"

		"[ACTORS - unreal.get_editor_subsystem(unreal.EditorActorSubsystem)]\n"
		"spawn_actor_from_class(unreal.StaticMeshActor, location, rotation, transient=False)\n"
		"spawn_actor_from_object(asset, location, rotation, transient=False)\n"
		"get_all_level_actors() -> [AActor]\n"
		"get_selected_level_actors() -> [AActor]\n"
		"set_selected_level_actors([actors])\n"
		"destroy_actor(actor) / destroy_actors([actors])\n"
		"duplicate_actor(actor, world=None, offset=Vector(0,0,0))\n\n"

		"[LEVELS - unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)]\n"
		"new_level('/Game/Maps/Name') / load_level('/Game/Maps/Name')\n"
		"save_current_level() / save_all_dirty_levels()\n"
		"editor_play_simulate() / editor_request_end_play()\n\n"

		"[STATIC MESH - unreal.EditorStaticMeshLibrary]\n"
		"get_lod_count(mesh) / set_lods(mesh, reduction_options)\n"
		"import_lod(mesh, lod_index, '/path/file.fbx')\n"
		"add_simple_collisions(mesh, unreal.ScriptingCollisionShapeType.BOX)\n"
		"set_convex_decomposition_collisions(mesh, hull_count, max_hull_verts, hull_precision)\n"
		"remove_collisions(mesh)\n"
		"get_number_verts(mesh, lod_index) / has_vertex_colors(mesh)\n\n"

		"[SKELETAL MESH - unreal.EditorSkeletalMeshLibrary]\n"
		"get_lod_count(skel) / regenerate_lod(skel, new_lod_count)\n"
		"import_lod(skel, lod_index, '/path/file.fbx')\n"
		"create_physics_asset(skel) / rename_socket(skel, 'OldName', 'NewName')\n\n"

		"[MATERIAL INSTANCES - unreal.MaterialEditingLibrary]\n"
		"set_material_instance_scalar_parameter_value(mi, 'Param', 1.0)\n"
		"set_material_instance_vector_parameter_value(mi, 'Color', LinearColor(r,g,b,a))\n"
		"set_material_instance_texture_parameter_value(mi, 'Tex', texture_asset)\n"
		"get_scalar/vector/texture_parameter_names(material) -> [str]\n"
		"update_material_instance(mi)  # recompile after changes\n\n"

		"[DATATABLES - unreal.DataTableFunctionLibrary]\n"
		"get_data_table_row_names(dt) -> [str]\n"
		"get_data_table_column_names(dt) -> [str]\n"
		"does_data_table_row_exist(dt, 'RowName') -> bool\n"
		"fill_data_table_from_csv_file(dt, '/path/file.csv')\n"
		"fill_data_table_from_json_file(dt, '/path/file.json')\n"
		"export_data_table_to_csv_string(dt) -> str\n\n"

		"[ASSET IMPORT]\n"
		"task = unreal.AssetImportTask()\n"
		"task.set_editor_property('filename', '/path/mesh.fbx')\n"
		"task.set_editor_property('destination_path', '/Game/Imported')\n"
		"task.set_editor_property('automated', True)  # no dialogs\n"
		"task.set_editor_property('replace_existing', True)\n"
		"unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])\n\n"

		"[COMMON]\n"
		"Types: Vector(x,y,z), Rotator(pitch,yaw,roll), Transform(loc,rot,scale), LinearColor(r,g,b,a)\n"
		"Props: obj.get_editor_property('name'), obj.set_editor_property('name', value)\n"
		"Load: load_asset, load_object, find_object, new_object, get_default_object\n\n"

		"[PCG - Procedural Content Generation]\n"
		"# Node settings via settings_interface\n"
		"settings.set_editor_property('points_per_squared_meter', 0.01)  # Surface Sampler density\n"
		"settings.set_editor_property('rotation_max', unreal.Rotator(0, 360, 0))  # Transform Points\n"
		"settings.set_editor_property('scale_min', unreal.Vector(0.8, 0.8, 0.8))\n"
		"# Mesh Spawner - weighted mesh entries\n"
		"entry = unreal.PCGMeshSelectorWeightedEntry()\n"
		"descriptor = entry.get_editor_property('descriptor')\n"
		"descriptor.set_editor_property('static_mesh', unreal.load_asset('/Game/Mesh'))\n"
		"mesh_selector.set_editor_property('mesh_entries', [entry])\n"
		"# Spawn PCG Volume\n"
		"sub = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)\n"
		"pcg_volume = sub.spawn_actor_from_class(unreal.PCGVolume, loc, rot)\n"
		"if pcg_volume:\n"
		"    pcg_comp = pcg_volume.get_component_by_class(unreal.PCGComponent)\n"
		"    if pcg_comp:\n"
		"        graph = unreal.load_asset('/Game/PCG/Graph')\n"
		"        if graph: pcg_comp.set_graph(graph)\n"
		"        pcg_comp.generate(True)  # Force regenerate\n\n"

		"[CANNOT DO - use edit_blueprint tool instead]\n"
		"Widget Blueprint widget_tree editing, component adding, event dispatchers, anim state machines"
	);
}

TSharedPtr<FJsonObject> FExecutePythonTool::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> CodeProp = MakeShared<FJsonObject>();
	CodeProp->SetStringField(TEXT("type"), TEXT("string"));
	CodeProp->SetStringField(TEXT("description"),
		TEXT("Python code to execute. Use 'import unreal' to access UE APIs. "
			 "Available APIs include: unreal.EditorAssetLibrary (asset operations), "
			 "unreal.EditorLevelLibrary (actor spawning), unreal.MaterialEditingLibrary (material nodes), "
			 "unreal.BlueprintEditorLibrary (blueprint editing), unreal.get_editor_subsystem() (subsystems), "
			 "and 1000+ more. Print statements will be captured as output."));
	Properties->SetObjectField(TEXT("code"), CodeProp);

	TSharedPtr<FJsonObject> AllowUIProp = MakeShared<FJsonObject>();
	AllowUIProp->SetStringField(TEXT("type"), TEXT("boolean"));
	AllowUIProp->SetStringField(TEXT("description"),
		TEXT("Optional. Set true if the script needs to open editor UI (asset editor tabs/windows). "
			 "Default: false (tool runs unattended)."));
	Properties->SetObjectField(TEXT("allow_ui"), AllowUIProp);

	TSharedPtr<FJsonObject> UnattendedProp = MakeShared<FJsonObject>();
	UnattendedProp->SetStringField(TEXT("type"), TEXT("boolean"));
	UnattendedProp->SetStringField(TEXT("description"),
		TEXT("Optional override for Python unattended mode. Default: true. "
			 "Set false for UI-interactive scripts."));
	Properties->SetObjectField(TEXT("unattended"), UnattendedProp);

	TSharedPtr<FJsonObject> ModeProp = MakeShared<FJsonObject>();
	ModeProp->SetStringField(TEXT("type"), TEXT("string"));
	ModeProp->SetStringField(TEXT("description"),
		TEXT("Execution mode. 'file' (default): multi-statement scripts. "
			 "'statement': single statement, prints result. "
			 "'evaluate': single expression, returns its value."));
	TArray<TSharedPtr<FJsonValue>> ModeEnum;
	ModeEnum.Add(MakeShared<FJsonValueString>(TEXT("file")));
	ModeEnum.Add(MakeShared<FJsonValueString>(TEXT("statement")));
	ModeEnum.Add(MakeShared<FJsonValueString>(TEXT("evaluate")));
	ModeProp->SetArrayField(TEXT("enum"), ModeEnum);
	Properties->SetObjectField(TEXT("mode"), ModeProp);

	TSharedPtr<FJsonObject> ScopeProp = MakeShared<FJsonObject>();
	ScopeProp->SetStringField(TEXT("type"), TEXT("string"));
	ScopeProp->SetStringField(TEXT("description"),
		TEXT("Execution scope. 'private' (default): isolated environment per call. "
			 "'public': shared globals with the Python console, so imports and variables persist across calls."));
	TArray<TSharedPtr<FJsonValue>> ScopeEnum;
	ScopeEnum.Add(MakeShared<FJsonValueString>(TEXT("private")));
	ScopeEnum.Add(MakeShared<FJsonValueString>(TEXT("public")));
	ScopeProp->SetArrayField(TEXT("enum"), ScopeEnum);
	Properties->SetObjectField(TEXT("scope"), ScopeProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("code")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult FExecutePythonTool::Execute(const TSharedPtr<FJsonObject>& Args)
{
	if (!Args.IsValid())
	{
		return FToolResult::Fail(TEXT("Invalid arguments"));
	}

	// ExecutePython touches editor systems; keep this game-thread safe even if called outside registry.
	if (!IsInGameThread())
	{
		FToolResult GameThreadResult;
		FEvent* DoneEvent = FPlatformProcess::GetSynchEventFromPool();
		AsyncTask(ENamedThreads::GameThread, [this, &Args, &GameThreadResult, DoneEvent]()
		{
			GameThreadResult = Execute(Args);
			DoneEvent->Trigger();
		});
		DoneEvent->Wait();
		FPlatformProcess::ReturnSynchEventToPool(DoneEvent);
		return GameThreadResult;
	}

	FString Code;

	if (!Args->TryGetStringField(TEXT("code"), Code) || Code.IsEmpty())
	{
		return FToolResult::Fail(TEXT("Missing required parameter: code"));
	}

	// Prevent memory exhaustion from excessively large code strings
	static constexpr int32 MaxCodeLength = 512 * 1024; // 512KB
	if (Code.Len() > MaxCodeLength)
	{
		return FToolResult::Fail(FString::Printf(TEXT("Code exceeds maximum size (%d chars, limit: %d)"), Code.Len(), MaxCodeLength));
	}

	bool bAllowUI = false;
	bool bAllowUIWasSet = false;
	FString BoolParseError;
	if (!TryGetOptionalBoolField(Args, TEXT("allow_ui"), bAllowUI, bAllowUIWasSet, BoolParseError))
	{
		return FToolResult::Fail(BoolParseError);
	}

	bool bUnattended = true;
	bool bUnattendedWasSet = false;
	if (!TryGetOptionalBoolField(Args, TEXT("unattended"), bUnattended, bUnattendedWasSet, BoolParseError))
	{
		return FToolResult::Fail(BoolParseError);
	}

	if (bAllowUI && !bUnattendedWasSet)
	{
		bUnattended = false;
	}

	if (bAllowUI && bUnattended)
	{
		return FToolResult::Fail(TEXT("Conflicting options: allow_ui=true requires unattended=false."));
	}

	// Parse execution mode: file (default), statement, evaluate
	EPythonCommandExecutionMode ExecMode = EPythonCommandExecutionMode::ExecuteFile;
	FString ModeStr;
	if (Args->TryGetStringField(TEXT("mode"), ModeStr))
	{
		ModeStr.TrimStartAndEndInline();
		if (ModeStr.Equals(TEXT("file"), ESearchCase::IgnoreCase))
		{
			ExecMode = EPythonCommandExecutionMode::ExecuteFile;
		}
		else if (ModeStr.Equals(TEXT("statement"), ESearchCase::IgnoreCase))
		{
			ExecMode = EPythonCommandExecutionMode::ExecuteStatement;
		}
		else if (ModeStr.Equals(TEXT("evaluate"), ESearchCase::IgnoreCase))
		{
			ExecMode = EPythonCommandExecutionMode::EvaluateStatement;
		}
		else
		{
			return FToolResult::Fail(FString::Printf(TEXT("Invalid mode: '%s'. Must be 'file', 'statement', or 'evaluate'."), *ModeStr));
		}
	}

	// Parse execution scope: private (default), public
	EPythonFileExecutionScope ExecScope = EPythonFileExecutionScope::Private;
	FString ScopeStr;
	if (Args->TryGetStringField(TEXT("scope"), ScopeStr))
	{
		ScopeStr.TrimStartAndEndInline();
		if (ScopeStr.Equals(TEXT("private"), ESearchCase::IgnoreCase))
		{
			ExecScope = EPythonFileExecutionScope::Private;
		}
		else if (ScopeStr.Equals(TEXT("public"), ESearchCase::IgnoreCase))
		{
			ExecScope = EPythonFileExecutionScope::Public;
		}
		else
		{
			return FToolResult::Fail(FString::Printf(TEXT("Invalid scope: '%s'. Must be 'private' or 'public'."), *ScopeStr));
		}
	}

	// Strip comments and string literals so heuristic pattern matching only fires on actual code,
	// avoiding false positives when patterns appear inside Python strings or comments.
	const FString StrippedCode = StripPythonCommentsAndStrings(Code);

	static const TArray<FString> SubsystemCtorPatterns = {
		TEXT("unreal.EditorActorSubsystem("),
		TEXT("unreal.EditorAssetSubsystem("),
		TEXT("unreal.LevelEditorSubsystem("),
		TEXT("unreal.AssetEditorSubsystem("),
		TEXT("unreal.EditorUtilitySubsystem("),
		TEXT("unreal.UnrealEditorSubsystem(")
	};

	if (ContainsAnyToken(StrippedCode, SubsystemCtorPatterns))
	{
		return FToolResult::Fail(
			TEXT("Unsafe subsystem construction detected. Use unreal.get_editor_subsystem(unreal.YourSubsystemClass) instead of calling subsystem constructors."));
	}

	static const TArray<FString> SubsystemDefaultObjectPatterns = {
		TEXT("AssetEditorSubsystem.get_default_object("),
		TEXT("EditorAssetSubsystem.get_default_object("),
		TEXT("EditorActorSubsystem.get_default_object("),
		TEXT("LevelEditorSubsystem.get_default_object("),
		TEXT("EditorUtilitySubsystem.get_default_object("),
		TEXT("UnrealEditorSubsystem.get_default_object(")
	};

	if (ContainsAnyToken(StrippedCode, SubsystemDefaultObjectPatterns))
	{
		return FToolResult::Fail(
			TEXT("Unsafe subsystem access detected. Do not use get_default_object() on subsystem classes. Use unreal.get_editor_subsystem(unreal.YourSubsystemClass)."));
	}

	if (bUnattended)
	{
		static const TArray<FString> UIOnlyPatterns = {
			TEXT("open_editor_for_asset"),
			TEXT("open_editor_for_assets"),
			TEXT("spawn_and_register_tab"),
			TEXT("invoke_tab")
		};

		if (ContainsAnyToken(StrippedCode, UIOnlyPatterns))
		{
			return FToolResult::Fail(
				TEXT("Script appears to open editor UI, but execute_python runs unattended by default. Retry with allow_ui=true (or unattended=false)."));
		}
	}

	// Check Python availability
	FString Error;
	if (!IsPythonAvailable(Error))
	{
		return FToolResult::Fail(Error);
	}

	// Get Python plugin - re-validate after availability check
	IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
	if (!Python)
	{
		return FToolResult::Fail(TEXT("Python plugin became unavailable"));
	}

	// Block known-dangerous operations during PIE. Python can call arbitrary engine APIs
	// that trigger blueprint compilation or asset deletion — both crash during PIE due to
	// stale FRepLayout pointers in UNetDriver. This is a lightweight heuristic; the central
	// safety net in NeoStackToolRegistry provides crash-level protection.
	if (GEditor && GEditor->IsPlayingSessionInEditor())
	{
		if (StrippedCode.Contains(TEXT("compile_blueprint")) || StrippedCode.Contains(TEXT("delete_asset")) ||
		    StrippedCode.Contains(TEXT("mark_blueprint_as_structurally_modified")))
		{
			return FToolResult::Fail(
				TEXT("Cannot execute blueprint compilation or asset deletion during PIE. Stop PIE first."));
		}
	}

	// Set up the command
	FPythonCommandEx Command;
	Command.Command = Code;
	Command.ExecutionMode = ExecMode;
	Command.FileExecutionScope = ExecScope;
	Command.Flags = bUnattended ? EPythonCommandFlags::Unattended : EPythonCommandFlags::None;

	// Execute the Python code
	const uint32 CodeHash = FCrc::StrCrc32(*Code);
	UE_LOG(LogAgentIntegrationKit, Log,
		TEXT("[AIK] execute_python start: len=%d crc=0x%08X mode=%s scope=%s unattended=%s allow_ui=%s pie=%s"),
		Code.Len(),
		CodeHash,
		LexToString(ExecMode),
		ExecScope == EPythonFileExecutionScope::Public ? TEXT("public") : TEXT("private"),
		bUnattended ? TEXT("true") : TEXT("false"),
		bAllowUI ? TEXT("true") : TEXT("false"),
		(GEditor && GEditor->IsPlayingSessionInEditor()) ? TEXT("true") : TEXT("false"));
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] Executing Python code:\n%s"), *Code);
	WritePythonExecutionBreadcrumb(TEXT("Begin"), Code, bUnattended, bAllowUI);

	bool bSuccess = Python->ExecPythonCommandEx(Command);
	WritePythonExecutionBreadcrumb(
		bSuccess ? TEXT("Success") : TEXT("Failure"),
		Code,
		bUnattended,
		bAllowUI,
		Command.CommandResult.Left(2048));

	// Format and return output
	FString Output = FormatPythonOutput(Command.LogOutput, Command.CommandResult, bSuccess);

	if (bSuccess)
	{
		// For EvaluateStatement mode, CommandResult contains the expression value (not an error)
		if (ExecMode == EPythonCommandExecutionMode::EvaluateStatement && !Command.CommandResult.IsEmpty())
		{
			if (Output.IsEmpty() || Output == TEXT("Python code executed successfully (no output)"))
			{
				Output = Command.CommandResult;
			}
			else
			{
				Output = Command.CommandResult + TEXT("\n") + Output;
			}
		}

		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] Python execution succeeded"));
		return FToolResult::Ok(Output);
	}
	else
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("[AIK] Python execution failed: %s"), *Command.CommandResult);
		return FToolResult::Fail(Output);
	}
}

bool FExecutePythonTool::IsPythonAvailable(FString& OutError) const
{
	IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();

	if (!Python)
	{
		OutError = TEXT("Python plugin is not loaded. Ensure PythonScriptPlugin is enabled in your project.");
		return false;
	}

	if (!Python->IsPythonAvailable())
	{
		OutError = TEXT("Python is not available. Check that Python is properly installed and configured.");
		return false;
	}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	if (!Python->IsPythonInitialized())
	{
		// Try to force enable Python at runtime (UE 5.7+)
		if (!Python->ForceEnablePythonAtRuntime())
		{
			OutError = TEXT("Python failed to initialize. Check Python installation and plugin settings.");
			return false;
		}
	}
#endif

	return true;
}

FString FExecutePythonTool::FormatPythonOutput(const TArray<FPythonLogOutputEntry>& LogOutput, const FString& CommandResult, bool bSuccess) const
{
	FString Output;

	// Collect output from log entries
	TArray<FString> InfoLines;
	TArray<FString> WarningLines;
	TArray<FString> ErrorLines;

	for (const FPythonLogOutputEntry& Entry : LogOutput)
	{
		switch (Entry.Type)
		{
		case EPythonLogOutputType::Info:
			InfoLines.Add(Entry.Output);
			break;
		case EPythonLogOutputType::Warning:
			WarningLines.Add(Entry.Output);
			break;
		case EPythonLogOutputType::Error:
			ErrorLines.Add(Entry.Output);
			break;
		}
	}

	// Build output string
	if (InfoLines.Num() > 0)
	{
		for (const FString& Line : InfoLines)
		{
			if (!Output.IsEmpty())
			{
				Output += TEXT("\n");
			}
			Output += Line;
		}
	}

	// Add warnings if any
	if (WarningLines.Num() > 0)
	{
		if (!Output.IsEmpty())
		{
			Output += TEXT("\n\n");
		}
		Output += TEXT("# WARNINGS\n");
		for (const FString& Line : WarningLines)
		{
			Output += FString::Printf(TEXT("[Warning] %s\n"), *Line);
		}
	}

	// Add errors if any (for failed execution)
	if (!bSuccess || ErrorLines.Num() > 0)
	{
		if (!Output.IsEmpty())
		{
			Output += TEXT("\n\n");
		}
		Output += TEXT("# ERRORS\n");

		for (const FString& Line : ErrorLines)
		{
			Output += FString::Printf(TEXT("[Error] %s\n"), *Line);
		}

		// Add command result (usually contains the traceback)
		if (!CommandResult.IsEmpty())
		{
			Output += TEXT("\n");
			Output += CommandResult;
		}
	}

	// If no output at all, provide a success message
	if (Output.IsEmpty() && bSuccess)
	{
		Output = TEXT("Python code executed successfully (no output)");
	}

	return Output;
}

#else // WITH_PYTHON

#include "Tools/ExecutePythonTool.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Minimal definition so the header's forward-declared FPythonLogOutputEntry compiles
struct FPythonLogOutputEntry {};

FString FExecutePythonTool::GetDescription() const
{
	return TEXT("Python plugin is not enabled. Enable it in Edit > Plugins.");
}

TSharedPtr<FJsonObject> FExecutePythonTool::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> CodeProp = MakeShared<FJsonObject>();
	CodeProp->SetStringField(TEXT("type"), TEXT("string"));
	CodeProp->SetStringField(TEXT("description"), TEXT("Python code to execute (Python plugin not available)"));
	Properties->SetObjectField(TEXT("code"), CodeProp);
	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("code")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult FExecutePythonTool::Execute(const TSharedPtr<FJsonObject>& Args)
{
	return FToolResult::Fail(TEXT("Python plugin is not enabled. Enable it in Edit > Plugins to use execute_python."));
}

bool FExecutePythonTool::IsPythonAvailable(FString& OutError) const
{
	OutError = TEXT("Python plugin is not enabled.");
	return false;
}

FString FExecutePythonTool::FormatPythonOutput(const TArray<FPythonLogOutputEntry>& LogOutput, const FString& CommandResult, bool bSuccess) const
{
	return TEXT("Python plugin is not enabled.");
}

#endif // WITH_PYTHON
