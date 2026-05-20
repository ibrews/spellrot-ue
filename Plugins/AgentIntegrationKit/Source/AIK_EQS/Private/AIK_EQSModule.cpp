#include "Modules/ModuleManager.h"
#include "Lua/LuaGraphResolverExtension.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"

class FAIK_EQSModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		LuaGraphResolver::RegisterExternalResolver([](UObject* Asset, TArray<FResolvedGraphInfo>& OutGraphs) -> bool
		{
			UEnvQuery* Query = Cast<UEnvQuery>(Asset);
			if (!Query) return false;

#if WITH_EDITORONLY_DATA
			if (!Query->EdGraph)
			{
				// Open the EQS editor to create the graph on demand.
				// UEnvironmentQueryGraph is MinimalAPI — we can't reference its StaticClass() directly.
				// The editor handles graph creation internally.
				LuaGraphResolver::EnsureEditorGraphViaEditorOpen(Asset, [](UObject* A) -> UEdGraph*
				{
					return Cast<UEnvQuery>(A)->EdGraph;
				});
			}
			if (Query->EdGraph)
			{
				OutGraphs.Add(FResolvedGraphInfo(TEXT("EnvironmentQuery"), Query->EdGraph));
			}
#endif
			return true;
		});
	}

	virtual void ShutdownModule() override {}
};
IMPLEMENT_MODULE(FAIK_EQSModule, AIK_EQS)
