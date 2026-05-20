using UnrealBuildTool;
using System;
using System.IO;

public class AIK_MetaHuman : ModuleRules
{
    public AIK_MetaHuman(ReadOnlyTargetRules Target) : base(Target)
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

        // MetaHumanCharacter has EnabledByDefault=false — only add dependency if compiled
        string MetaHumanPluginDir = Path.Combine(EngineDirectory, "Plugins", "MetaHuman", "MetaHumanCharacter");
        if (HasModuleGeneratedHeaders(MetaHumanPluginDir, "MetaHumanCharacter"))
        {
            PrivateDependencyModuleNames.AddRange(new string[] {
                "MetaHumanCharacter",
                "MetaHumanSDKRuntime",
            });

            // 5.7+ modules for body conform, constraints, build, wardrobe palette
            // These live under MetaHumanCharacter plugin dir
            string[] CharacterOptionalModules = {
                "MetaHumanCharacterEditor",
                "MetaHumanCharacterPalette",
                "MetaHumanCharacterPaletteEditor",
            };
            foreach (string Mod in CharacterOptionalModules)
            {
                if (HasModuleGeneratedHeaders(MetaHumanPluginDir, Mod))
                {
                    PrivateDependencyModuleNames.Add(Mod);
                }
            }

            // MetaHumanCoreTechLib is a separate plugin
            string CoreTechDir = Path.Combine(EngineDirectory, "Plugins", "MetaHuman", "MetaHumanCoreTechLib");
            if (HasModuleGeneratedHeaders(CoreTechDir, "MetaHumanCoreTechLib"))
            {
                PrivateDependencyModuleNames.Add("MetaHumanCoreTechLib");
            }

            // MetaHumanIdentity (under MetaHumanAnimator plugin) for ImportFromIdentity
            string AnimatorDir = Path.Combine(EngineDirectory, "Plugins", "MetaHuman", "MetaHumanAnimator");
            if (HasModuleGeneratedHeaders(AnimatorDir, "MetaHumanIdentity"))
            {
                PrivateDependencyModuleNames.Add("MetaHumanIdentity");
            }
        }
        else
        {
            PrivateDefinitions.Add("AIK_METAHUMAN_DISABLED=1");
        }
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
