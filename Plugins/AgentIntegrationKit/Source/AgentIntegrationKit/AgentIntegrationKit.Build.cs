// Copyright 2025-2026 Betide Studio. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.IO;
using UnrealBuildTool;

public class AgentIntegrationKit : ModuleRules
{
	public AgentIntegrationKit(ReadOnlyTargetRules Target) : base(Target)
	{
		// Optional integrations still linked by core (no extension module for these yet)
		bool bWithCommonUI = IsOptionalPluginAvailable(Target, "CommonUI");
		bool bWithMetaSound = IsOptionalPluginAvailable(Target, "Metasound");

		// All other optional plugin dependencies have been moved to AIK_* extension modules.
		// They are loaded at runtime when the backing plugin is available. The WITH_* defines
		// are kept as =0 so that #if guards in shared headers compile correctly in core.

		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Lua 5.4.7: public headers for consumers, src path for internal #includes in LuaAll.c
		string LuaIncludePath = Path.Combine(PluginDirectory, "Source", "ThirdParty", "Lua", "include");
		string LuaSrcPath = Path.Combine(PluginDirectory, "Source", "ThirdParty", "Lua", "src");
		PublicIncludePaths.Add(LuaIncludePath);
		PrivateIncludePaths.Add(LuaSrcPath);

		// sol2 v3.3.0: header-only C++ binding library
		string Sol2IncludePath = Path.Combine(PluginDirectory, "Source", "ThirdParty", "sol2", "include");
		PublicIncludePaths.Add(Sol2IncludePath);

		PublicDefinitions.Add("SOL_ALL_SAFETIES_ON=1");
		PublicDefinitions.Add("SOL_USING_CXX_LUA=0");
		PublicDefinitions.Add("SOL_PRINT_ERRORS=0");

		// Suppress C5321 (UTF-8 escape in sol2) — not available as a UBT property,
		// so we disable it via pragma in the sol2 wrapper header instead.

		// Lua symbol export: LUA_BUILD_AS_DLL is defined in LuaLib.c (not here) so it only
		// applies to the C translation unit that compiles Lua, not to C++ files in this module.
		// Extension modules define LUA_BUILD_AS_DLL in their Build.cs to get the import path.

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Slate",
			"SlateCore",
			"InputCore",
			"Json",
			"JsonUtilities",
			"HTTP",
			"HTTPServer",
			"RenderCore",
			"RHI",
			"Renderer"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"EditorStyle",
			"ToolMenus",
			"WorkspaceMenuStructure",
			"Projects",
			"DeveloperSettings",
			"ApplicationCore",
			"EditorFramework",
			// Blueprint tools dependencies
			"AssetTools",
			"AssetRegistry",
			"Kismet",
			"KismetCompiler",
			"BlueprintGraph",
			"UMG",
			"UMGEditor",
			// Core AI/Animation (always present — part of engine)
			"AIModule",
			"AIGraph",
			"AnimGraph",
			"AnimGraphRuntime",
			"MaterialEditor",
			"GraphEditor",
			"PhysicsCore",
			"PhysicsUtilities",
			// Context attachment dependencies
			"ContentBrowser",
			"DesktopPlatform",
			// Settings detail customization
			"PropertyEditor",
			// Actor detail panel extension (OnExtendActorDetails delegate)
			"DetailCustomizations",
			"ImageWrapper",
			// WebBrowser for embedded web UI
			"WebBrowser",
			// WebSockets for relay client (remote editor access)
			"WebSockets",
			// Level Editor tab management (viewport activation)
			"LevelEditor",
			// Logging and diagnostics
			"MessageLog",
			// Behavior Tree editor graph support (BT runtime headers are in AIModule)
			"BehaviorTreeEditor",
			// SoundCue graph editor support
			"AudioEditor",
			// Level Sequence / Cinematics support
			"LevelSequence",
			"MovieScene",
			"MovieSceneTracks",
			"AssetDefinition",
			// Asset management (UEditorAssetLibrary)
			"EditorScriptingUtilities",
			// Editor utility support (EditorUtilityBlueprint/Widget)
			"Blutility",
			// Clothing system (UClothingAssetBase for skeletal mesh clothing data)
			"ClothingSystemRuntimeInterface",
			// Level Design: Landscape + Foliage + Navigation
			"Landscape",
			"LandscapeEditor",
			"Foliage",
			"NavigationSystem",
			// Procedural noise (FractalBrownianMotionNoise for landscape generation)
			"GeometryCore",
			// Asset Collections support
			"CollectionManager",
			// Actor Merging / Proxy Mesh support
			"MeshMergeUtilities",
			// Settings discovery & plugin management
			"Settings",
			// Localization workflow support
			"Localization",
			"LocalizationCommandletExecution",
			// Level Instance / Packed Level Actor editor support
			"LevelInstanceEditor",
			// Automation Testing support
			"AutomationController",
			"AutomationTest",
			"FunctionalTesting",
			// Replay System file management support
			"NetworkReplayStreaming",
			"LocalFileNetworkReplayStreaming",
			// GameplayTags (always present — core engine module)
			"GameplayTags",
			"GameplayTagsEditor",
			// Project indexing: file-change watcher for auto-index
			"DirectoryWatcher",
			// Python scripting — required for execute_python tool
			"PythonScriptPlugin",
			// Asset drop target widget (SAssetDropTarget) for Content Browser drag-drop
			"EditorWidgets",
		});

		// Niagara: moved to AIK_Niagara extension module
		PublicDefinitions.Add("WITH_NIAGARA=0");

		// ControlRig: moved to AIK_ControlRig extension module
		PublicDefinitions.Add("WITH_CONTROLRIG=0");

		// IKRig: moved to AIK_IKRig extension module
		PublicDefinitions.Add("WITH_IKRIG=0");

		// Optional: MetaSound
		if (bWithMetaSound)
		{
			PrivateDependencyModuleNames.AddRange(new string[] { "MetasoundEngine", "MetasoundFrontend", "MetasoundEditor" });
		}
		PublicDefinitions.Add("WITH_METASOUND=" + (bWithMetaSound ? "1" : "0"));

		// StateTree: moved to AIK_StateTree extension module
		PublicDefinitions.Add("WITH_STATE_TREE=0");

		// Paper2D: moved to AIK_Paper2D extension module
		PublicDefinitions.Add("WITH_PAPER2D=0");

		// EnhancedInput: moved to AIK_EnhancedInput extension module
		PublicDefinitions.Add("WITH_ENHANCED_INPUT=0");

		// GameplayAbilities: moved to AIK_GameplayAbilities extension module
		PublicDefinitions.Add("WITH_GAMEPLAY_ABILITIES=0");

		// SmartObjects: moved to AIK_SmartObjects extension module
		PublicDefinitions.Add("WITH_SMART_OBJECTS=0");

		// Chooser: moved to AIK_Chooser extension module
		PublicDefinitions.Add("WITH_CHOOSER=0");

		// Python: required dependency — the plugin cannot function without Python
		PublicDefinitions.Add("WITH_PYTHON=1");

		// ChaosFracture: moved to AIK_ChaosFracture extension module
		PublicDefinitions.Add("WITH_CHAOS_FRACTURE=0");

		// Interchange: moved to AIK_Interchange extension module
		PublicDefinitions.Add("WITH_INTERCHANGE=0");

		// PCG: moved to AIK_PCG extension module
		PublicDefinitions.Add("WITH_PCG=0");

		// EQS: moved to AIK_EQS extension module
		PublicDefinitions.Add("WITH_EQS=0");

		// PoseSearch: moved to AIK_PoseSearch extension module (loaded at runtime if PoseSearch is available)
		// WITH_POSE_SEARCH define kept for backward compat but always 0 in core module
		PublicDefinitions.Add("WITH_POSE_SEARCH=0");

		// Optional: CommonUI (set bWithCommonUI = false if unavailable)
		if (bWithCommonUI)
		{
			PrivateDependencyModuleNames.Add("CommonUI");
		}
		PublicDefinitions.Add("WITH_COMMON_UI=" + (bWithCommonUI ? "1" : "0"));

		// Water: moved to AIK_Water extension module
		PublicDefinitions.Add("WITH_WATER=0");

		// Geometry Scripting: moved to AIK_GeometryScripting extension module
		PublicDefinitions.Add("WITH_GEOMETRY_SCRIPTING=0");

		// nDisplay: moved to AIK_NDisplay extension module
		PublicDefinitions.Add("WITH_NDISPLAY=0");

		// Dataflow: moved to AIK_Dataflow extension module
		PublicDefinitions.Add("WITH_DATAFLOW=0");

		// MovieRenderPipeline: moved to AIK_MovieRenderPipeline extension module
		PublicDefinitions.Add("WITH_MOVIE_RENDER_PIPELINE=0");

		// LiveLink: moved to AIK_LiveLink extension module
		PublicDefinitions.Add("WITH_LIVE_LINK=0");

		// MetaHuman: moved to AIK_MetaHuman extension module
		PublicDefinitions.Add("WITH_METAHUMAN=0");

		// MassAI: moved to AIK_MassAI extension module
		PublicDefinitions.Add("WITH_MASS_AI=0");

		// Clothing system editor (clothing asset factory for creating/binding cloth from mesh sections)
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ClothingSystemRuntimeCommon",
			"ClothingSystemEditor"
		});

		// HLOD generation support (World Partition + Legacy)
		// WorldPartitionHLODUtilities is an editor plugin that may be absent on UE 5.5 or custom builds.
		// The HLOD binding only uses Engine-level headers (HLODBuilder.h, HLODActor.h, HLODLayer.h etc.)
		// so no C++ #if guard is needed — the plugin dependency just ensures builder classes are registered.
		bool bWithHLODUtilities = IsOptionalPluginAvailable(Target, "WorldPartitionHLODUtilities");
		if (bWithHLODUtilities)
		{
			PrivateDependencyModuleNames.Add("WorldPartitionHLODUtilities");
		}
		PublicDefinitions.Add("WITH_HLOD_UTILITIES=" + (bWithHLODUtilities ? "1" : "0"));

		// Source control integration (branch info, changelists UI)
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"SourceControl",
			"SourceControlWindows"
		});

		// Live Coding support (Windows only)
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
		}

		// AppKit framework for clipboard image reading (macOS)
		if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			PublicFrameworks.Add("AppKit");
		}

		// libutil for forkpty() — PTY support for integrated terminal (macOS/Linux)
		if (Target.Platform == UnrealTargetPlatform.Mac || Target.Platform == UnrealTargetPlatform.Linux)
		{
			PublicSystemLibraries.Add("util");
		}

	}

	private bool IsOptionalPluginAvailable(ReadOnlyTargetRules Target, string PluginName)
	{
		if (IsPluginExplicitlyDisabled(Target, PluginName))
		{
			return false;
		}

		return IsPluginDescriptorAvailable(Target, PluginName);
	}

	private static bool IsPluginExplicitlyDisabled(ReadOnlyTargetRules Target, string PluginName)
	{
		foreach (string DisabledPlugin in Target.DisablePlugins)
		{
			if (DisabledPlugin.Equals(PluginName, StringComparison.OrdinalIgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	private bool IsPluginDescriptorAvailable(ReadOnlyTargetRules Target, string PluginName)
	{
		foreach (string CandidatePath in GetLikelyPluginDescriptorPaths(Target, PluginName))
		{
			if (File.Exists(CandidatePath))
			{
				return true;
			}
		}

		// Fallback for custom engine/plugin layouts.
		string EnginePluginsDir = Path.Combine(EngineDirectory, "Plugins");
		if (ContainsPluginDescriptorRecursive(EnginePluginsDir, PluginName))
		{
			return true;
		}

		if (Target.ProjectFile != null)
		{
			string ProjectDir = Path.GetDirectoryName(Target.ProjectFile.FullName);
			if (!string.IsNullOrEmpty(ProjectDir))
			{
				string ProjectPluginsDir = Path.Combine(ProjectDir, "Plugins");
				if (ContainsPluginDescriptorRecursive(ProjectPluginsDir, PluginName))
				{
					return true;
				}
			}
		}

		return false;
	}

	private IEnumerable<string> GetLikelyPluginDescriptorPaths(ReadOnlyTargetRules Target, string PluginName)
	{
		// Known engine plugin locations for optional dependencies we gate in this module.
		// Fast path — avoids recursive directory scan for common plugins.
		var KnownPaths = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
		{
			{ "PoseSearch",        Path.Combine("Animation", "PoseSearch", "PoseSearch.uplugin") },
			{ "CommonUI",          Path.Combine("Runtime", "CommonUI", "CommonUI.uplugin") },
			{ "nDisplay",          Path.Combine("Runtime", "nDisplay", "nDisplay.uplugin") },
			{ "Dataflow",          Path.Combine("Experimental", "Dataflow", "Dataflow.uplugin") },
			{ "ChaosOutfitAsset",  Path.Combine("Experimental", "ChaosOutfitAsset", "ChaosOutfitAsset.uplugin") },
			{ "MetaHumanCharacter", Path.Combine("MetaHuman", "MetaHumanCharacter", "MetaHumanCharacter.uplugin") },
			{ "Niagara",           Path.Combine("FX", "Niagara", "Niagara.uplugin") },
			{ "ControlRig",        Path.Combine("Animation", "ControlRig", "ControlRig.uplugin") },
			{ "IKRig",             Path.Combine("Animation", "IKRig", "IKRig.uplugin") },
			{ "Metasound",         Path.Combine("Runtime", "Metasound", "Metasound.uplugin") },
			{ "StateTree",         Path.Combine("Runtime", "StateTree", "StateTree.uplugin") },
			{ "Paper2D",           Path.Combine("2D", "Paper2D", "Paper2D.uplugin") },
			{ "EnhancedInput",     Path.Combine("EnhancedInput", "EnhancedInput.uplugin") },
			{ "GameplayAbilities", Path.Combine("Runtime", "GameplayAbilities", "GameplayAbilities.uplugin") },
			{ "SmartObjects",      Path.Combine("Runtime", "SmartObjects", "SmartObjects.uplugin") },
			{ "Chooser",           Path.Combine("Chooser", "Chooser.uplugin") },
			{ "PythonScriptPlugin", Path.Combine("Scripting", "PythonScriptPlugin", "PythonScriptPlugin.uplugin") },
			{ "Fracture",          Path.Combine("Experimental", "ChaosEditor", "Fracture.uplugin") },
			{ "Interchange",       Path.Combine("Interchange", "Runtime", "Interchange.uplugin") },
			{ "PCG",               Path.Combine("PCG", "PCG.uplugin") },
			{ "MovieRenderPipeline", Path.Combine("MovieScene", "MovieRenderPipeline", "MovieRenderPipeline.uplugin") },
			{ "LiveLink",          Path.Combine("Animation", "LiveLink", "LiveLink.uplugin") },
			{ "Water",             Path.Combine("Water", "Water.uplugin") },
			{ "MassGameplay",      Path.Combine("Runtime", "MassGameplay", "MassGameplay.uplugin") },
			{ "MassAI",            Path.Combine("AI", "MassAI", "MassAI.uplugin") },
			{ "MassCrowd",         Path.Combine("AI", "MassCrowd", "MassCrowd.uplugin") },
			{ "ZoneGraph",         Path.Combine("Runtime", "ZoneGraph", "ZoneGraph.uplugin") },
			// GeometryScripting moved to AIK_GeometryScripting extension module
			{ "WorldPartitionHLODUtilities", Path.Combine("Editor", "WorldPartitionHLODUtilities", "WorldPartitionHLODUtilities.uplugin") },
		};

		if (KnownPaths.TryGetValue(PluginName, out string RelativePath))
		{
			yield return Path.Combine(EngineDirectory, "Plugins", RelativePath);
		}

		if (Target.ProjectFile != null)
		{
			string ProjectDir = Path.GetDirectoryName(Target.ProjectFile.FullName);
			if (!string.IsNullOrEmpty(ProjectDir))
			{
				yield return Path.Combine(ProjectDir, "Plugins", PluginName, $"{PluginName}.uplugin");
			}
		}
	}

	private static bool ContainsPluginDescriptorRecursive(string RootDir, string PluginName)
	{
		if (!Directory.Exists(RootDir))
		{
			return false;
		}

		try
		{
			string TargetFile = $"{PluginName}.uplugin";
			foreach (string _ in Directory.EnumerateFiles(RootDir, TargetFile, SearchOption.AllDirectories))
			{
				return true;
			}
		}
		catch (Exception)
		{
			return false;
		}

		return false;
	}

	private bool IsPluginModuleCompiled(ReadOnlyTargetRules Target, string ModuleName)
	{
		// Check if the module's generated headers exist (meaning the plugin was actually compiled/enabled).
		// Without this, we'd try to include headers that reference .generated.h files that don't exist.
		string[] PlatformDirs = { "Mac", "Win64", "Linux" };
		string[] ConfigDirs = { "UnrealEditor", "UnrealGame" };
		string EngineIntermediatePath = Path.Combine(EngineDirectory, "..", "Intermediate", "Build");
		foreach (string Platform in PlatformDirs)
		{
			foreach (string Config in ConfigDirs)
			{
				string GeneratedDir = Path.Combine(EngineIntermediatePath, Platform, Config, "Inc", ModuleName);
				if (Directory.Exists(GeneratedDir))
				{
					return true;
				}
			}
		}

		// Also check under the plugin's own Intermediate directory
		string PluginIntermediateBase = Path.Combine(EngineDirectory, "Plugins");
		try
		{
			string GeneratedHeader = $"{ModuleName}.init.gen.cpp";
			foreach (string _ in Directory.EnumerateFiles(PluginIntermediateBase, GeneratedHeader, SearchOption.AllDirectories))
			{
				return true;
			}
		}
		catch (Exception) { }

		return false;
	}
}
