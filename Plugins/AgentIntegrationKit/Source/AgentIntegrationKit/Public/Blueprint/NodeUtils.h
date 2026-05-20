#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UBlueprintNodeSpawner;
struct FEdGraphSchemaAction;

struct FNodeSearchResult
{
	FString Name;
	FString Category;
	FString Tooltip;
	FString Keywords;
	FString Signature;
	TArray<FString> InputPins;
	TArray<FString> OutputPins;
	int32 Score = 0;
	TWeakObjectPtr<UBlueprintNodeSpawner> Spawner;

	// Schema action (non-Blueprint graphs: BT, Material, SoundCue, EQS, etc.)
	TSharedPtr<FEdGraphSchemaAction> SchemaAction;
	TWeakObjectPtr<UEdGraph> SchemaGraph;
};

namespace NeoNodes
{
	TArray<FNodeSearchResult> FindNodes(const FString& Query, UBlueprint* ContextBP, int32 MaxResults = 10);

	// Context-aware node search: uses FBlueprintActionMenuUtils::MakeContextMenu with the
	// specific graph, matching how the editor's right-click context menu works.
	// This is essential for sub-graphs (AnimBP transition/state graphs) where the global
	// FBlueprintActionDatabase search may return spawners that fail on the specific graph.
	TArray<FNodeSearchResult> FindNodesForGraph(const FString& Query, UBlueprint* ContextBP, UEdGraph* TargetGraph, int32 MaxResults = 10);

	UEdGraphNode* SpawnNode(UEdGraph* Graph, UBlueprintNodeSpawner* Spawner, double X, double Y);
	UBlueprintNodeSpawner* FindSpawnerBySignature(const FString& Signature);

	// Ensures the asset editor is open for schemas that require it (BehaviorTree, Material, SoundCue, EQS).
	// Pumps Slate instead of sleeping so the game thread can process initialization.
	AGENTINTEGRATIONKIT_API void EnsureEditorOpenForSchema(UEdGraph* Graph);
}
