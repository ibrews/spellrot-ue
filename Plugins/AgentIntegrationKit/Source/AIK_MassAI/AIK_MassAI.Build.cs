using UnrealBuildTool;
using System.IO;

public class AIK_MassAI : ModuleRules
{
    public AIK_MassAI(ReadOnlyTargetRules Target) : base(Target)
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
            "ZoneGraph",
            "MassEntity",
            "MassSpawner",
            "MassSimulation",
            "MassCommon",
            "MassCrowd",
            "AIModule",
        });

        // StructUtils merged into CoreUObject in 5.6+, only needed as separate dep for 5.4/5.5
        if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion <= 5)
        {
            PrivateDependencyModuleNames.Add("StructUtils");
        }
    }
}
