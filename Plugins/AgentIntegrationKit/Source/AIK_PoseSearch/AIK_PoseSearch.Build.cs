// Copyright 2025-2026 Betide Studio. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class AIK_PoseSearch : ModuleRules
{
	public AIK_PoseSearch(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Lua 5.4.7 + sol2 headers (shared with core module)
		string PluginDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
		string LuaIncludePath = Path.Combine(PluginDir, "Source", "ThirdParty", "Lua", "include");
		string Sol2IncludePath = Path.Combine(PluginDir, "Source", "ThirdParty", "sol2", "include");
		PublicIncludePaths.Add(LuaIncludePath);
		PublicIncludePaths.Add(Sol2IncludePath);

		PublicDefinitions.Add("SOL_ALL_SAFETIES_ON=1");
		PublicDefinitions.Add("SOL_USING_CXX_LUA=0");
		PublicDefinitions.Add("SOL_PRINT_ERRORS=0");

		// Import Lua C API symbols from core module.
		// On Windows: triggers dllimport. On Mac/Linux: plain extern (symbols resolve at link time).
		PrivateDefinitions.Add("LUA_BUILD_AS_DLL=1");

		// Core engine
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// Core AIK module (for FLuaBindingRegistry, FLuaSessionData, Lua lib symbols)
			"AgentIntegrationKit",

			// Editor (FScopedTransaction, etc.)
			"UnrealEd",

			// PoseSearch
			"PoseSearch",
			"PoseSearchEditor",

			// Animation types used by PoseSearch binding
			"AnimGraphRuntime"
		});
	}
}
