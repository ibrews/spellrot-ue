// Copyright 2025-2026 Betide Studio. All Rights Reserved.
// Shared utility: resolve any asset path to its UEdGraph(s).

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Blueprint/BlueprintUtils.h"

// Graph-owning asset types
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "BehaviorTree/BehaviorTree.h"
#if WITH_NIAGARA
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraCommon.h"
#include "NiagaraGraph.h"
#endif

// MetaSound asset types
#if WITH_METASOUND
#include "MetasoundSource.h"
#include "Metasound.h"
#endif

// SoundCue
#include "Sound/SoundCue.h"

// BehaviorTree editor (for on-demand graph creation)
#include "BehaviorTreeGraph.h"
#include "EdGraphSchema_BehaviorTree.h"

// EQS (for on-demand graph creation)
#if WITH_EQS
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQueryGraph.h"
#include "EdGraphSchema_EnvironmentQuery.h"
#endif

// Editor subsystem (for opening editors to create transient graphs)
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/ToolkitManager.h"

// PCG support (UE 5.7+)
#if WITH_PCG && ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#include "PCGGraph.h"
#include "PCGEditor.h"
#endif

// FResolvedGraphInfo, external resolver API, WaitForGraph, EnsureEditorGraphViaEditorOpen
// are defined in the lightweight extension header (no asset-specific includes).
#include "Lua/LuaGraphResolverExtension.h"

namespace LuaGraphResolver
{

// Helper: open the Material Editor for a UMaterial or UMaterialFunction, then return
// the preview copy's MaterialGraph. The Material Editor always works on a transient
// duplicate — the preview copy is the only place where expressions, graph nodes, and
// connections are consistent. We identify it by finding the UMaterial in the editor's
// ObjectsCurrentlyBeingEdited that lives in the transient package and has a MaterialGraph.
inline UMaterialGraph* GetEditorMaterialGraph(UObject* OriginalAsset)
{
	if (!GEditor) return nullptr;

	UAssetEditorSubsystem* Sub = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!Sub) return nullptr;

	// Open the editor if not already open
	if (!Sub->FindEditorForAsset(OriginalAsset, false))
	{
		Sub->OpenEditorForAsset(OriginalAsset);
		WaitForGraph([&]()
		{
			return Sub->FindEditorForAsset(OriginalAsset, false) != nullptr;
		});
	}

	// Use FToolkitManager to get the IToolkit, which has GetObjectsCurrentlyBeingEdited().
	// UAssetEditorSubsystem::FindEditorForAsset returns IAssetEditorInstance which lacks this method.
	TSharedPtr<IToolkit> Toolkit = FToolkitManager::Get().FindEditorForAsset(OriginalAsset);
	if (!Toolkit.IsValid()) return nullptr;

	const TArray<UObject*>* EditedObjects = Toolkit->GetObjectsCurrentlyBeingEdited();
	if (!EditedObjects) return nullptr;

	// Find the preview UMaterial — it lives in the transient package and has a MaterialGraph.
	// For materials: the preview is a UPreviewMaterial duplicate of the original.
	// For material functions: the editor creates a transient UMaterial as a backing proxy.
	for (UObject* Obj : *EditedObjects)
	{
		UMaterial* Mat = Cast<UMaterial>(Obj);
		if (Mat && Mat->MaterialGraph && Mat->GetOutermost() == GetTransientPackage())
		{
			return Mat->MaterialGraph;
		}
	}

	return nullptr;
}

#if WITH_NIAGARA
// Helper: extract UEdGraph from a UNiagaraScript via its source graph.
// Uses GetLatestSource() directly — GetLatestScriptData() returns null when
// VersionData is empty (common for emitter scripts embedded in systems).
inline UEdGraph* GetNiagaraScriptGraph(UNiagaraScript* Script)
{
	if (!Script) return nullptr;
	UNiagaraScriptSourceBase* SourceBase = Script->GetLatestSource();
	if (!SourceBase) return nullptr;
	UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(SourceBase);
	if (!Source || !Source->NodeGraph) return nullptr;
	// UNiagaraGraph inherits from UEdGraph — explicit cast from TObjectPtr
	return Cast<UEdGraph>(Source->NodeGraph.Get());
}
#endif // WITH_NIAGARA

// Get all UEdGraphs from any loaded asset.
inline TArray<FResolvedGraphInfo> GetGraphs(UObject* Asset)
{
	TArray<FResolvedGraphInfo> Result;
	if (!Asset) return Result;

	// Check external resolvers first (extension modules like AIK_Niagara)
	for (const FExternalResolverFunc& Resolver : GetExternalResolvers())
	{
		if (Resolver(Asset, Result))
		{
			return Result;
		}
	}

	// ---- Blueprint ----
	if (UBlueprint* BP = Cast<UBlueprint>(Asset))
	{
		for (UEdGraph* G : BP->UbergraphPages)
		{
			if (G) Result.Add(FResolvedGraphInfo(G->GetName(), G));
		}
		for (UEdGraph* G : BP->FunctionGraphs)
		{
			if (G) Result.Add(FResolvedGraphInfo(G->GetName(), G));
		}
		for (UEdGraph* G : BP->MacroGraphs)
		{
			if (G) Result.Add(FResolvedGraphInfo(G->GetName(), G));
		}
		// AnimBP: also collect state machine / state / transition subgraphs
		TArray<TPair<FString, UEdGraph*>> AnimGraphs;
		NeoBlueprint::CollectAnimBPGraphs(BP, AnimGraphs);
		for (auto& Pair : AnimGraphs)
		{
			Result.Add(FResolvedGraphInfo(Pair.Key, Pair.Value));
		}
		return Result;
	}

	// ---- Material / Material Function ----
	// The Material Editor works on a PREVIEW COPY of the material, not the original.
	// All expression creation, graph nodes, and connections live on this copy.
	// On save/apply, the editor duplicates the copy back over the original.
	// We MUST use the preview copy's MaterialGraph — it's the single source of truth.
#if WITH_EDITORONLY_DATA
	if (Cast<UMaterial>(Asset) || Cast<UMaterialFunction>(Asset))
	{
		UMaterialGraph* EditorGraph = GetEditorMaterialGraph(Asset);
		if (EditorGraph)
		{
			Result.Add(FResolvedGraphInfo(TEXT("MaterialGraph"), Cast<UEdGraph>(EditorGraph)));
		}
		return Result;
	}
#endif

	// ---- BehaviorTree ----
#if WITH_EDITORONLY_DATA
	if (UBehaviorTree* BT = Cast<UBehaviorTree>(Asset))
	{
		if (!BT->BTGraph)
		{
			// Create on demand — same pattern as FBehaviorTreeEditor::CreateAssetEditorStuff()
			BT->BTGraph = FBlueprintEditorUtils::CreateNewGraph(
				BT, TEXT("BehaviorTree"),
				UBehaviorTreeGraph::StaticClass(), UEdGraphSchema_BehaviorTree::StaticClass());

			const UEdGraphSchema* Schema = BT->BTGraph->GetSchema();
			if (Schema)
			{
				Schema->CreateDefaultNodesForGraph(*BT->BTGraph);
			}
			if (UBehaviorTreeGraph* BTGraph = Cast<UBehaviorTreeGraph>(BT->BTGraph))
			{
				BTGraph->OnCreated();
			}
		}
		Result.Add(FResolvedGraphInfo(TEXT("BehaviorTree"), BT->BTGraph));
		return Result;
	}
#endif

	// ---- SoundCue ----
#if WITH_EDITORONLY_DATA
	if (USoundCue* SoundCue = Cast<USoundCue>(Asset))
	{
		if (!SoundCue->SoundCueGraph)
		{
			// SoundCue normally creates its graph in PostInitProperties.
			// If somehow null, open the editor to trigger creation.
			EnsureEditorGraphViaEditorOpen(Asset, [](UObject* A) -> UEdGraph*
			{
				return Cast<USoundCue>(A)->SoundCueGraph;
			});
		}
		if (SoundCue->SoundCueGraph)
		{
			Result.Add(FResolvedGraphInfo(TEXT("SoundCueGraph"), SoundCue->SoundCueGraph));
		}
		return Result;
	}
#endif

	// ---- EQS (Environment Query) ----
#if WITH_EQS && WITH_EDITORONLY_DATA
	if (UEnvQuery* Query = Cast<UEnvQuery>(Asset))
	{
		if (!Query->EdGraph)
		{
			// Create on demand — same pattern as FEnvironmentQueryEditor
			Query->EdGraph = FBlueprintEditorUtils::CreateNewGraph(
				Query, TEXT("EnvironmentQuery"),
				UEnvironmentQueryGraph::StaticClass(), UEdGraphSchema_EnvironmentQuery::StaticClass());

			const UEdGraphSchema* Schema = Query->EdGraph->GetSchema();
			if (Schema)
			{
				Schema->CreateDefaultNodesForGraph(*Query->EdGraph);
			}
			if (UEnvironmentQueryGraph* EQSGraph = Cast<UEnvironmentQueryGraph>(Query->EdGraph))
			{
				EQSGraph->OnCreated();
			}
		}
		Result.Add(FResolvedGraphInfo(TEXT("EnvironmentQuery"), Query->EdGraph));
		return Result;
	}
#endif

	// ---- Niagara Script (direct, standalone modules/functions) ----
	// NOTE: The Niagara Script editor works on a transient copy (like Materials),
	// but the edited copy is not exposed via GetObjectsCurrentlyBeingEdited().
	// We read from the original's source graph, which reflects the last-saved state.
	// For live editing, agents use the stack-based enrichment API on the parent system.
#if WITH_NIAGARA
	if (UNiagaraScript* Script = Cast<UNiagaraScript>(Asset))
	{
		if (UEdGraph* Graph = GetNiagaraScriptGraph(Script))
		{
			Result.Add(FResolvedGraphInfo(TEXT("NiagaraGraph"), Graph));
		}
		return Result;
	}

	// ---- Niagara System (enumerate system + emitter scripts) ----
	if (UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset))
	{
		System->EnsureFullyLoaded();

		// System spawn/update share a single graph source. GetLatestSource() works
		// after EnsureFullyLoaded() triggers PostLoad which sets up VersionData.
		// We add the graph once as "SystemGraph" since both scripts share the same UNiagaraGraph.
		UNiagaraScript* SpawnScript = System->GetSystemSpawnScript();
		if (SpawnScript)
		{
			UNiagaraScriptSourceBase* SourceBase = SpawnScript->GetLatestSource();
			UNiagaraScriptSource* Source = SourceBase ? Cast<UNiagaraScriptSource>(SourceBase) : nullptr;
			if (Source && Source->NodeGraph)
			{
				Result.Add(FResolvedGraphInfo(TEXT("SystemGraph"), Cast<UEdGraph>(Source->NodeGraph.Get())));
			}
		}

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
				Result.Add(FResolvedGraphInfo(Label, Cast<UEdGraph>(EmitterGraphSource->NodeGraph.Get())));
			}
		}
		return Result;
	}
#endif // WITH_NIAGARA

	// ---- MetaSound (Source or Patch) ----
	// MetaSound graphs are transient — only exist while the editor is open.
	// Open the editor on demand to ensure the graph is available.
#if WITH_METASOUND && WITH_EDITORONLY_DATA
	if (UMetaSoundSource* MSSource = Cast<UMetaSoundSource>(Asset))
	{
		UEdGraph* Graph = EnsureEditorGraphViaEditorOpen(Asset, [](UObject* A) -> UEdGraph*
		{
			return Cast<UMetaSoundSource>(A)->GetGraph();
		});
		if (Graph)
		{
			Result.Add(FResolvedGraphInfo(TEXT("MetaSoundGraph"), Graph));
		}
		return Result;
	}
	if (UMetaSoundPatch* MSPatch = Cast<UMetaSoundPatch>(Asset))
	{
		UEdGraph* Graph = EnsureEditorGraphViaEditorOpen(Asset, [](UObject* A) -> UEdGraph*
		{
			return Cast<UMetaSoundPatch>(A)->GetGraph();
		});
		if (Graph)
		{
			Result.Add(FResolvedGraphInfo(TEXT("MetaSoundGraph"), Graph));
		}
		return Result;
	}
#endif

	// ---- PCG (Procedural Content Generation) ----
	// PCG editor graph is transient — only exists while the PCG editor is open.
	// Open the editor on demand to ensure the graph is available.
#if WITH_PCG && ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	if (UPCGGraph* PCGGraph = Cast<UPCGGraph>(Asset))
	{
		// First try the fast path — editor may already be open
		UPCGEditorGraph* EditorGraph = FPCGEditor::GetPCGEditorGraph(PCGGraph);
		if (!EditorGraph)
		{
			// Open the PCG editor to create the transient graph
			UEdGraph* Graph = EnsureEditorGraphViaEditorOpen(Asset, [](UObject* A) -> UEdGraph*
			{
				UPCGEditorGraph* EG = FPCGEditor::GetPCGEditorGraph(Cast<UPCGGraph>(A));
				return EG ? reinterpret_cast<UEdGraph*>(EG) : nullptr;
			});
			if (Graph)
			{
				Result.Add(FResolvedGraphInfo(TEXT("PCGGraph"), Graph));
			}
		}
		else
		{
			Result.Add(FResolvedGraphInfo(TEXT("PCGGraph"), reinterpret_cast<UEdGraph*>(EditorGraph)));
		}
		return Result;
	}
#endif

	return Result;
}

// Find a specific graph by name. If GraphName is empty and there's exactly one graph, return it.
inline UEdGraph* FindGraph(UObject* Asset, const FString& GraphName)
{
	TArray<FResolvedGraphInfo> Graphs = GetGraphs(Asset);

	// If name empty and single graph, return it
	if (GraphName.IsEmpty() && Graphs.Num() == 1)
	{
		return Graphs[0].Graph;
	}

	// Exact match on friendly name
	for (const FResolvedGraphInfo& G : Graphs)
	{
		if (G.Name == GraphName) return G.Graph;
	}

	// Exact match on UObject name (e.g. "MaterialGraph_0" from older code)
	for (const FResolvedGraphInfo& G : Graphs)
	{
		if (G.Graph && G.Graph->GetName() == GraphName) return G.Graph;
	}

	// Case-insensitive on friendly name
	for (const FResolvedGraphInfo& G : Graphs)
	{
		if (G.Name.Equals(GraphName, ESearchCase::IgnoreCase)) return G.Graph;
	}

	// Case-insensitive on UObject name
	for (const FResolvedGraphInfo& G : Graphs)
	{
		if (G.Graph && G.Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase)) return G.Graph;
	}

	// Substring match (e.g. "Event" matches "EventGraph")
	for (const FResolvedGraphInfo& G : Graphs)
	{
		if (G.Name.Contains(GraphName, ESearchCase::IgnoreCase)) return G.Graph;
	}

	return nullptr;
}

// Build a string listing available graph names (for error messages)
inline FString ListGraphNames(UObject* Asset)
{
	TArray<FResolvedGraphInfo> Graphs = GetGraphs(Asset);
	FString List;
	for (const FResolvedGraphInfo& G : Graphs)
	{
		if (List.Len() > 0) List += TEXT(", ");
		List += FString::Printf(TEXT("\"%s\""), *G.Name);
	}
	return List.IsEmpty() ? TEXT("(none)") : List;
}

} // namespace LuaGraphResolver
