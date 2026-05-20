#include "Modules/ModuleManager.h"
#include "Lua/LuaGraphResolverExtension.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#include "NiagaraEmitterHandle.h"

class FAIK_NiagaraModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Register Niagara graph resolver so read_graph/add_node/find_nodes work for Niagara assets.
		// The main AgentIntegrationKit module compiles with WITH_NIAGARA=0, so Niagara resolution
		// must be provided by this extension module.
		LuaGraphResolver::RegisterExternalResolver([](UObject* Asset, TArray<FResolvedGraphInfo>& OutGraphs) -> bool
		{
			// ---- Niagara Script (standalone modules/functions) ----
			if (UNiagaraScript* Script = Cast<UNiagaraScript>(Asset))
			{
				UNiagaraScriptSourceBase* SourceBase = Script->GetLatestSource();
				UNiagaraScriptSource* Source = SourceBase ? Cast<UNiagaraScriptSource>(SourceBase) : nullptr;
				if (Source && Source->NodeGraph)
				{
					OutGraphs.Add(FResolvedGraphInfo(TEXT("NiagaraGraph"), Cast<UEdGraph>(Source->NodeGraph.Get())));
				}
				return true; // handled
			}

			// ---- Niagara System ----
			if (UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset))
			{
				System->EnsureFullyLoaded();

				// System spawn/update share a single graph source
				UNiagaraScript* SpawnScript = System->GetSystemSpawnScript();
				if (SpawnScript)
				{
					UNiagaraScriptSourceBase* SourceBase = SpawnScript->GetLatestSource();
					UNiagaraScriptSource* Source = SourceBase ? Cast<UNiagaraScriptSource>(SourceBase) : nullptr;
					if (Source && Source->NodeGraph)
					{
						OutGraphs.Add(FResolvedGraphInfo(TEXT("SystemGraph"), Cast<UEdGraph>(Source->NodeGraph.Get())));
					}
				}

				// Each emitter has its own shared graph source
				const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
				for (int32 i = 0; i < Handles.Num(); i++)
				{
					const FNiagaraEmitterHandle& Handle = Handles[i];
					FString EmitterName = Handle.GetName().ToString();

					FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
					if (!EmitterData) continue;

					UNiagaraScriptSource* EmitterGraphSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
					if (EmitterGraphSource && EmitterGraphSource->NodeGraph)
					{
						FString Label = FString::Printf(TEXT("%s/Graph"), *EmitterName);
						OutGraphs.Add(FResolvedGraphInfo(Label, Cast<UEdGraph>(EmitterGraphSource->NodeGraph.Get())));
					}
				}
				return true; // handled
			}

			// ---- Niagara Emitter (standalone) ----
			if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Asset))
			{
				FVersionedNiagaraEmitterData* EmitterData = Emitter->GetLatestEmitterData();
				if (EmitterData)
				{
					UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
					if (Source && Source->NodeGraph)
					{
						OutGraphs.Add(FResolvedGraphInfo(TEXT("EmitterGraph"), Cast<UEdGraph>(Source->NodeGraph.Get())));
					}
				}
				return true; // handled
			}

			return false; // not a Niagara asset, let other resolvers handle it
		});
	}

	virtual void ShutdownModule() override {}
};
IMPLEMENT_MODULE(FAIK_NiagaraModule, AIK_Niagara)
