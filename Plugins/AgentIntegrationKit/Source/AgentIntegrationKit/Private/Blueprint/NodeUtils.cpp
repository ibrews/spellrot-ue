#include "Blueprint/NodeUtils.h"
#include "BlueprintActionDatabase.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintVariableNodeSpawner.h"
#include "BlueprintNodeSignature.h"
#include "BlueprintActionMenuBuilder.h"
#include "BlueprintActionMenuUtils.h"
#include "BlueprintActionFilter.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Framework/Application/SlateApplication.h"
#include "RenderingThread.h"
#include "BehaviorTree/BehaviorTree.h"
#if WITH_EQS
#include "EnvironmentQuery/EnvQuery.h"
#endif
#include "Materials/Material.h"
#include "Sound/SoundCue.h"

void NeoNodes::EnsureEditorOpenForSchema(UEdGraph* Graph)
{
	if (!Graph || !GEditor) return;
	UObject* Outer = Graph->GetOuter();
	if (!Outer) return;

	// Check if graph's owning asset needs its editor open for schema/notification init.
	// ControlRig: class name check since main module doesn't link ControlRig headers.
	FString OuterClassName = Outer->GetClass()->GetName();
	bool bNeedsEditor = Cast<UBehaviorTree>(Outer) != nullptr
#if WITH_EQS
		|| Cast<UEnvQuery>(Outer) != nullptr
#endif
		|| Cast<UMaterial>(Outer) != nullptr
		|| Cast<USoundCue>(Outer) != nullptr
		|| OuterClassName.Contains(TEXT("ControlRigBlueprint"))
		|| OuterClassName.Contains(TEXT("RigVMBlueprint"));
	if (!bNeedsEditor) return;

	UAssetEditorSubsystem* Sub = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!Sub) return;

	// Already open?
	if (Sub->FindEditorForAsset(Outer, false)) return;

	Sub->OpenEditorForAsset(Outer);
	// Pump Slate instead of sleeping — Sleep() blocks the game thread,
	// preventing editor initialization from completing.
	const int32 MaxSteps = 40;
	for (int32 i = 0; i < MaxSteps; ++i)
	{
		if (Sub->FindEditorForAsset(Outer, false)) break;
		FSlateApplication::Get().Tick();
		FlushRenderingCommands();
	}
}

// Detects spawners whose owning class was recompiled (live coding / hot reload).
// Calling PrimeDefaultUiSpec or GetTemplateNode on these crashes.
static bool IsStaleSpawner(UBlueprintNodeSpawner const* Spawner)
{
	if (!Spawner) return true;

	UClass* ClassOwner = nullptr;
	if (const UBlueprintFunctionNodeSpawner* FuncSpawner = Cast<UBlueprintFunctionNodeSpawner>(Spawner))
	{
		const UFunction* Func = FuncSpawner->GetFunction();
		if (!Func) return true;
		ClassOwner = Func->GetOwnerClass();
	}
	else if (const UBlueprintVariableNodeSpawner* VarSpawner = Cast<UBlueprintVariableNodeSpawner>(Spawner))
	{
		const FProperty* Prop = VarSpawner->GetVarProperty();
		if (!Prop) return true;
		ClassOwner = Prop->GetOwnerClass();
	}
	if (ClassOwner)
	{
		return ClassOwner->HasAnyClassFlags(CLASS_NewerVersionExists)
			|| ClassOwner->GetOutermost() == GetTransientPackage();
	}
	return false;
}

static int32 ScoreMatch(const FString& NameLower, const FString& QueryLower)
{
	if (NameLower == QueryLower)
	{
		return 100;
	}
	if (NameLower.StartsWith(QueryLower))
	{
		return 90;
	}

	// Word boundary match — query matches a word start
	// e.g. "print" matches "Set Print String"
	int32 WordIdx = NameLower.Find(FString(TEXT(" ")) + QueryLower);
	if (WordIdx != INDEX_NONE)
	{
		return 85;
	}

	if (NameLower.Contains(QueryLower))
	{
		return 70;
	}

	// Simple fuzzy: check if all query chars appear in order
	int32 NamePos = 0;
	int32 Matched = 0;
	for (int32 i = 0; i < QueryLower.Len(); i++)
	{
		int32 Found = INDEX_NONE;
		for (int32 j = NamePos; j < NameLower.Len(); j++)
		{
			if (NameLower[j] == QueryLower[i])
			{
				Found = j;
				break;
			}
		}
		if (Found != INDEX_NONE)
		{
			NamePos = Found + 1;
			Matched++;
		}
	}

	if (Matched == QueryLower.Len())
	{
		float Ratio = (float)QueryLower.Len() / (float)NameLower.Len();
		return FMath::Clamp((int32)(50.0f * Ratio + 10.0f), 10, 55);
	}

	return 0;
}

static void ExtractPinNames(UEdGraphNode* Node, TArray<FString>& InPins, TArray<FString>& OutPins)
{
	if (!Node) return;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->bHidden) continue;

		FString PinName = Pin->GetDisplayName().IsEmpty()
			? Pin->PinName.ToString()
			: Pin->GetDisplayName().ToString();

		if (Pin->Direction == EGPD_Input)
		{
			InPins.Add(PinName);
		}
		else
		{
			OutPins.Add(PinName);
		}
	}
}

TArray<FNodeSearchResult> NeoNodes::FindNodes(const FString& Query, UBlueprint* ContextBP, int32 MaxResults)
{
	TArray<FNodeSearchResult> Results;
	FString QueryLower = Query.ToLower();

	FBlueprintActionDatabase& ActionDB = FBlueprintActionDatabase::Get();

	for (auto& ActionPair : ActionDB.GetAllActions())
	{
		for (UBlueprintNodeSpawner* Spawner : ActionPair.Value)
		{
			if (!Spawner) continue;
			if (IsStaleSpawner(Spawner)) continue;

			// Use PrimeDefaultUiSpec for the display name — safe without context.
			// GetUiSpec() crashes for variable spawners (accesses Context.Blueprints[0] unchecked).
			// PrimeDefaultUiSpec is safe and spawners pre-populate MenuName at creation time
			// (e.g., "Get Health", "Set Health", "Add StaticMeshComponent", "Subtract").
			const FBlueprintActionUiSpec& UiSpec = Spawner->PrimeDefaultUiSpec(nullptr);

			FString NodeName = UiSpec.MenuName.ToString();
			if (NodeName.IsEmpty()) continue;

			int32 Score = ScoreMatch(NodeName.ToLower(), QueryLower);
			if (Score <= 0) continue;

			FNodeSearchResult Result;
			Result.Name = NodeName;
			Result.Score = Score;
			Result.Spawner = Spawner;
			Result.Signature = Spawner->GetSpawnerSignature().ToString();
			Result.Category = UiSpec.Category.ToString();
			Result.Tooltip = UiSpec.Tooltip.ToString().Left(200);

			// Deprioritize "Execution" category spawners — they often fail to spawn
			// because they are schema-level action wrappers, not proper BlueprintNodeSpawners.
			// The same nodes exist under "Utilities|Flow Control" etc. and work reliably.
			if (Result.Category == TEXT("Execution"))
			{
				Result.Score = FMath::Max(1, Result.Score - 50);
			}

			UEdGraphNode* NodeCDO = Spawner->GetTemplateNode();
			if (NodeCDO)
			{
				ExtractPinNames(NodeCDO, Result.InputPins, Result.OutputPins);
			}

			Results.Add(MoveTemp(Result));
		}
	}

	// Sort by score descending
	Results.Sort([](const FNodeSearchResult& A, const FNodeSearchResult& B)
	{
		return A.Score > B.Score;
	});

	if (Results.Num() > MaxResults)
	{
		Results.SetNum(MaxResults);
	}

	return Results;
}

TArray<FNodeSearchResult> NeoNodes::FindNodesForGraph(const FString& Query, UBlueprint* ContextBP, UEdGraph* TargetGraph, int32 MaxResults)
{
	TArray<FNodeSearchResult> Results;
	if (!ContextBP || !TargetGraph) return FindNodes(Query, ContextBP, MaxResults);

	FString QueryLower = Query.ToLower();

	// Build a context-aware action menu using the engine's MakeContextMenu —
	// the same API the editor's right-click context menu uses. This properly
	// filters by graph type (transition graphs, state graphs, etc.).
	FBlueprintActionContext Context;
	Context.Blueprints.Add(ContextBP);
	Context.Graphs.Add(TargetGraph);

	FBlueprintActionMenuBuilder MenuBuilder;
	FBlueprintActionMenuUtils::MakeContextMenu(Context, /*bIsContextSensitive=*/false,
		EContextTargetFlags::TARGET_Blueprint | EContextTargetFlags::TARGET_BlueprintLibraries, MenuBuilder);

	// Search the filtered action list
	const int32 NumActions = MenuBuilder.GetNumActions();
	for (int32 i = 0; i < NumActions; i++)
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		TSharedPtr<FEdGraphSchemaAction> Action = MenuBuilder.GetSchemaAction(i);
#else
		FGraphActionListBuilderBase::ActionGroup& Group = MenuBuilder.GetAction(i);
		TSharedPtr<FEdGraphSchemaAction> Action = Group.Actions.Num() > 0 ? Group.Actions[0] : nullptr;
#endif
		if (!Action.IsValid()) continue;

		FString Name = Action->GetMenuDescription().ToString();
		FString NameLower = Name.ToLower();

		int32 Score = 0;
		if (NameLower == QueryLower) Score = 100;
		else if (NameLower.StartsWith(QueryLower)) Score = 80;
		else if (NameLower.Contains(QueryLower)) Score = 60;
		else continue;

		FNodeSearchResult R;
		R.Name = Name;
		R.Category = Action->GetCategory().ToString();
		R.Tooltip = Action->GetTooltipDescription().ToString();
		R.Score = Score;
		R.SchemaAction = Action;
		R.SchemaGraph = TargetGraph;
		Results.Add(MoveTemp(R));
	}

	// Sort by score descending
	Results.Sort([](const FNodeSearchResult& A, const FNodeSearchResult& B) { return A.Score > B.Score; });
	if (Results.Num() > MaxResults)
	{
		Results.SetNum(MaxResults);
	}

	return Results;
}

UBlueprintNodeSpawner* NeoNodes::FindSpawnerBySignature(const FString& Signature)
{
	FBlueprintActionDatabase& ActionDB = FBlueprintActionDatabase::Get();

	for (auto& ActionPair : ActionDB.GetAllActions())
	{
		for (UBlueprintNodeSpawner* Spawner : ActionPair.Value)
		{
			if (!Spawner) continue;
			if (IsStaleSpawner(Spawner)) continue;
			if (Spawner->GetSpawnerSignature().ToString() == Signature)
			{
				return Spawner;
			}
		}
	}

	return nullptr;
}

UEdGraphNode* NeoNodes::SpawnNode(UEdGraph* Graph, UBlueprintNodeSpawner* Spawner, double X, double Y)
{
	if (!Graph || !Spawner) return nullptr;

	IBlueprintNodeBinder::FBindingSet Bindings;
	// Invoke handles SetFlags(RF_Transactional), AllocateDefaultPins(), PostPlacedNewNode(),
	// and AddNode() internally via SpawnEdGraphNode — do NOT call them again.
	// Double PostPlacedNewNode() crashes on UK2Node_CallFunction (check() assertion on enabled state).
	UEdGraphNode* Node = Spawner->Invoke(Graph, Bindings, FVector2D(X, Y));

	if (Node)
	{
		UBlueprint* BP = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
		if (BP)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		}
	}

	return Node;
}
