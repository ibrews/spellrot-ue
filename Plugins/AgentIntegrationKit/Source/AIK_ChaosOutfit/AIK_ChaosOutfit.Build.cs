using UnrealBuildTool;
using System;
using System.IO;

public class AIK_ChaosOutfit : ModuleRules
{
    public AIK_ChaosOutfit(ReadOnlyTargetRules Target) : base(Target)
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
            "ClothingSystemRuntimeInterface",
            "ClothingSystemRuntimeCommon",
            "ClothingSystemEditor",
        });

        // ChaosOutfitAsset has EnabledByDefault=false — only add dependency if compiled
        string ChaosOutfitPluginDir = Path.Combine(EngineDirectory, "Plugins", "Experimental", "ChaosOutfitAsset");
        bool bHasChaosOutfit = HasModuleGeneratedHeaders(ChaosOutfitPluginDir, "ChaosOutfitAssetEngine");
        if (bHasChaosOutfit)
        {
            PrivateDependencyModuleNames.Add("ChaosOutfitAssetEngine");
            PrivateDependencyModuleNames.Add("ChaosClothAssetEngine");
        }
        PrivateDefinitions.Add("WITH_CHAOS_OUTFIT_ASSET=" + (bHasChaosOutfit ? "1" : "0"));
    }

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
