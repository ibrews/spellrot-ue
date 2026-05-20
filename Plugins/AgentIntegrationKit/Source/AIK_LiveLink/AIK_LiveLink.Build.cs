using UnrealBuildTool;
using System.IO;

public class AIK_LiveLink : ModuleRules
{
    public AIK_LiveLink(ReadOnlyTargetRules Target) : base(Target)
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
            "LiveLinkInterface",
            "LiveLink",
        });
    }
}
