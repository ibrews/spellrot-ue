#include "Modules/ModuleManager.h"
#include "Lua/LuaGraphResolverExtension.h"
#include "PCGGraph.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#include "PCGEditor.h"
#endif
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"

class FAIK_PCGModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		// PCG editor graph API (FPCGEditor) is only available in 5.7+
		LuaGraphResolver::RegisterExternalResolver([](UObject* Asset, TArray<FResolvedGraphInfo>& OutGraphs) -> bool
		{
			UPCGGraph* PCGGraph = Cast<UPCGGraph>(Asset);
			if (!PCGGraph) return false;

			// PCG editor graph is transient — only exists while the PCG editor is open.
			UPCGEditorGraph* EditorGraph = FPCGEditor::GetPCGEditorGraph(PCGGraph);
			if (!EditorGraph)
			{
				// Open the PCG editor to create the transient graph
				UEdGraph* Graph = LuaGraphResolver::EnsureEditorGraphViaEditorOpen(Asset, [](UObject* A) -> UEdGraph*
				{
					UPCGEditorGraph* EG = FPCGEditor::GetPCGEditorGraph(Cast<UPCGGraph>(A));
					return EG ? reinterpret_cast<UEdGraph*>(EG) : nullptr;
				});
				if (Graph)
				{
					OutGraphs.Add(FResolvedGraphInfo(TEXT("PCGGraph"), Graph));
				}
			}
			else
			{
				OutGraphs.Add(FResolvedGraphInfo(TEXT("PCGGraph"), reinterpret_cast<UEdGraph*>(EditorGraph)));
			}
			return true;
		});
#endif
	}

	virtual void ShutdownModule() override {}
};
IMPLEMENT_MODULE(FAIK_PCGModule, AIK_PCG)
