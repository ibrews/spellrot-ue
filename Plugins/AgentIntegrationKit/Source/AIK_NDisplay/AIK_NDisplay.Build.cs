using UnrealBuildTool;
using System;
using System.IO;

public class AIK_NDisplay : ModuleRules
{
    public AIK_NDisplay(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        string PluginDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
        PublicIncludePaths.Add(Path.Combine(PluginDir, "Source", "ThirdParty", "Lua", "include"));
        PublicIncludePaths.Add(Path.Combine(PluginDir, "Source", "ThirdParty", "sol2", "include"));
        PublicDefinitions.Add("SOL_ALL_SAFETIES_ON=1");
        PublicDefinitions.Add("SOL_USING_CXX_LUA=0");
        PublicDefinitions.Add("SOL_PRINT_ERRORS=0");
        PrivateDefinitions.Add("LUA_BUILD_AS_DLL=1");
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
        PrivateDependencyModuleNames.AddRange(new string[] {
            "AgentIntegrationKit",
            "UnrealEd",
        });

        // nDisplay has EnabledByDefault=false — only add dependency if its UHT headers exist
        // (meaning the plugin was compiled in a prior build or is currently enabled)
        string NDisplayPluginDir = Path.Combine(EngineDirectory, "Plugins", "Runtime", "nDisplay");
        if (HasModuleGeneratedHeaders(NDisplayPluginDir, "DisplayClusterConfiguration"))
        {
            PrivateDependencyModuleNames.Add("DisplayClusterConfiguration");
            PrivateDependencyModuleNames.Add("DisplayClusterConfigurator");
        }
        else
        {
            PrivateDefinitions.Add("AIK_NDISPLAY_DISABLED=1");
        }
    }

    /// <summary>
    /// Check if a module's UHT-generated headers exist inside a specific plugin directory.
    /// Generated headers live at: PluginDir/Intermediate/Build/{Platform}/{Config}/Inc/{ModuleName}/
    /// </summary>
    private static bool HasModuleGeneratedHeaders(string BackingPluginDir, string ModuleName)
    {
        string IntermediateDir = Path.Combine(BackingPluginDir, "Intermediate", "Build");
        if (!Directory.Exists(IntermediateDir))
        {
            return false;
        }

        string[] PlatformDirs = { "Mac", "Win64", "Linux" };
        string[] ConfigDirs = { "UnrealEditor", "UnrealGame" };
        foreach (string Platform in PlatformDirs)
        {
            foreach (string Config in ConfigDirs)
            {
                string IncDir = Path.Combine(IntermediateDir, Platform, Config, "Inc", ModuleName);
                if (Directory.Exists(IncDir))
                {
                    return true;
                }
            }
        }
        return false;
    }
}
