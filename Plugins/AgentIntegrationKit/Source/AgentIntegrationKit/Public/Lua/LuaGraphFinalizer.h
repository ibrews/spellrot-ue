// Copyright 2025-2026 Betide Studio. All Rights Reserved.
// Post-mutation finalization for graph types that need it.
// Replicates the finalization logic from EditGraphTool for Lua graph bindings.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"

// Material
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialEditorUtilities.h"

// BehaviorTree
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeGraph.h"

// EQS
#if WITH_EQS
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQueryGraph.h"
#endif

// MetaSound
#if WITH_METASOUND
#include "MetasoundSource.h"
#include "Metasound.h"
#endif

// SoundCue
#include "Sound/SoundCue.h"

// Editor
#include "Editor.h"

namespace LuaGraphFinalizer
{

// Call after any graph mutation (add_node, connect, disconnect, delete_node, set_pin).
// Syncs the graph's visual/runtime state for graph types that need special handling.
inline void FinalizeGraph(UEdGraph* Graph, UObject* Asset)
{
	if (!Graph || !Asset) return;

	// ---- Blueprint ----
	if (UBlueprint* BP = Cast<UBlueprint>(Asset))
	{
		if (GEditor && GEditor->IsPlayingSessionInEditor())
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		}
		else
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		}
		return;
	}

	// ---- Material ----
#if WITH_EDITORONLY_DATA
	if (UMaterialGraph* MatGraph = Cast<UMaterialGraph>(Graph))
	{
		UObject* MaterialOrFunction = nullptr;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
		MaterialOrFunction = MatGraph->GetMaterialOrFunction();
#else
		MaterialOrFunction = MatGraph->MaterialFunction
			? static_cast<UObject*>(MatGraph->MaterialFunction)
			: static_cast<UObject*>(MatGraph->Material);
#endif

		if (UMaterial* Mat = Cast<UMaterial>(MaterialOrFunction))
		{
			Mat->Modify();

			// Clear stale compilation errors from all expressions before recompile
			for (UMaterialExpression* Expr : Mat->GetExpressionCollection().Expressions)
			{
				if (Expr) Expr->LastErrorText.Empty();
			}

			// CRITICAL: Sync graph pin connections → expression FExpressionInput data.
			// UMaterial::MaterialGraph has no UPROPERTY — the graph is NOT serialized.
			// On load, RebuildGraph() rebuilds pin connections FROM expression data.
			// So pin connections only persist if synced to expressions here.
			// We call this DIRECTLY because FMaterialEditorUtilities::UpdateMaterialAfterGraphChange
			// is a no-op when the material editor isn't open (it delegates to IMaterialEditor).
			MatGraph->LinkMaterialExpressionsFromGraph();

			// Also notify the editor if open — handles preview updates, pin types, etc.
			FMaterialEditorUtilities::UpdateMaterialAfterGraphChange(MatGraph);

			// Rebuild Slate node widget tree — this is the critical call that makes
			// programmatically added nodes visually appear in the graph editor panel.
			// The engine's own CreateNewMaterialExpression calls this after every add.
			MatGraph->NotifyGraphChanged();

			Mat->MarkPackageDirty();
			Mat->ForceRecompileForRendering();
		}
		else if (UMaterialFunction* MatFunc = Cast<UMaterialFunction>(MaterialOrFunction))
		{
			MatFunc->Modify();

			// Same direct sync for material functions
			MatGraph->LinkMaterialExpressionsFromGraph();

			FMaterialEditorUtilities::UpdateMaterialAfterGraphChange(MatGraph);
			MatGraph->NotifyGraphChanged();
			MatFunc->MarkPackageDirty();
		}
		return;
	}
#endif

	// ---- BehaviorTree ----
	if (UBehaviorTreeGraph* BTGraph = Cast<UBehaviorTreeGraph>(Graph))
	{
		// UpdatePinConnectionTypes() is an OnLoaded()-only operation that converts
		// legacy "Transition" pin categories. Calling it after mutations can crash
		// (it has check(Node) asserts and touches every pin). The BT editor only
		// calls UpdateAsset() after graph changes — match that flow.
		BTGraph->UpdateAsset();
		BTGraph->UpdateBlackboardChange();
		Asset->MarkPackageDirty();
		return;
	}

	// ---- EQS ----
#if WITH_EQS
	if (UEnvironmentQueryGraph* EQSGraph = Cast<UEnvironmentQueryGraph>(Graph))
	{
		EQSGraph->UpdateAsset();
		Asset->MarkPackageDirty();
		return;
	}
#endif

	// ---- MetaSound ----
#if WITH_METASOUND
	if (Cast<UMetaSoundSource>(Asset) || Cast<UMetaSoundPatch>(Asset))
	{
		Graph->NotifyGraphChanged();
		Asset->MarkPackageDirty();
		return;
	}
#endif

	// ---- SoundCue ----
	if (Cast<USoundCue>(Asset))
	{
		Graph->NotifyGraphChanged();
		Asset->MarkPackageDirty();
		return;
	}

	// ---- PCG / Generic ----
	// For PCG and any other graph types, NotifyGraphChanged + MarkPackageDirty is sufficient
	Graph->NotifyGraphChanged();
	Asset->MarkPackageDirty();
}

} // namespace LuaGraphFinalizer
