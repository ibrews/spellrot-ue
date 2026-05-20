#include "Lua/LuaBindingRegistry.h"
#include "Modules/ModuleManager.h"
#include "Lua/LuaGraphResolver.h"
#if WITH_CONTROLRIG
#include "Lua/LuaControlRigHelper.h"
#endif
#include "Blueprint/NodeUtils.h"
#include "Blueprint/BlueprintUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintVariableNodeSpawner.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// Generic node discovery via schema context actions — works for Material, BT, SoundCue, EQS, etc.
// Uses the engine's standard GetGraphContextActions API (same system the editor context menu uses).
static void FindNodesViaSchema(UEdGraph* Graph, const FString& Query, int32 MaxResults,
	TArray<FNodeSearchResult>& OutResults)
{
	if (!Graph) return;

	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (!Schema) return;

	// Some schemas need the editor open for class discovery
	NeoNodes::EnsureEditorOpenForSchema(Graph);

	// Construct the context menu builder — same as the editor does for right-click menu
	FGraphContextMenuBuilder ContextMenuBuilder(Graph);
	Schema->GetGraphContextActions(ContextMenuBuilder);

	FString QueryLower = Query.ToLower();

	TArray<FNodeSearchResult> ScoredActions;

	const int32 NumActions = ContextMenuBuilder.GetNumActions();
	for (int32 i = 0; i < NumActions; i++)
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		TSharedPtr<FEdGraphSchemaAction> Action = ContextMenuBuilder.GetSchemaAction(i);
#else
		FGraphActionListBuilderBase::ActionGroup& Group = ContextMenuBuilder.GetAction(i);
		TSharedPtr<FEdGraphSchemaAction> Action = Group.Actions.Num() > 0 ? Group.Actions[0] : nullptr;
#endif
		if (!Action.IsValid()) continue;

		FString Name = Action->GetMenuDescription().ToString();
		FString Category = Action->GetCategory().ToString();
		FString Tooltip = Action->GetTooltipDescription().ToString();
		FString NameLower = Name.ToLower();

		// Build combined keywords string from engine search keywords
		FString Keywords;
		const TArray<FString>& KWArray = Action->GetSearchKeywordsArray();
		if (KWArray.Num() > 0)
		{
			Keywords = FString::Join(KWArray, TEXT(" "));
		}

		// Score: exact > starts with > contains > category > keywords > tooltip
		int32 Score = 0;
		if (NameLower == QueryLower) Score = 100;
		else if (NameLower.StartsWith(QueryLower)) Score = 80;
		else if (NameLower.Contains(QueryLower)) Score = 60;
		else if (Category.ToLower().Contains(QueryLower)) Score = 40;
		else if (!Keywords.IsEmpty() && Keywords.ToLower().Contains(QueryLower)) Score = 30;
		else if (Tooltip.ToLower().Contains(QueryLower)) Score = 20;
		else continue;

		FNodeSearchResult R;
		R.Name = Name;
		R.Category = Category;
		R.Tooltip = Tooltip;
		R.Keywords = Keywords;
		R.Score = Score;
		R.SchemaAction = Action;
		R.SchemaGraph = Graph;
		ScoredActions.Add(MoveTemp(R));
	}

	// Sort by score descending
	ScoredActions.Sort([](const FNodeSearchResult& A, const FNodeSearchResult& B) { return A.Score > B.Score; });

	int32 Count = FMath::Min(ScoredActions.Num(), MaxResults);
	for (int32 i = 0; i < Count; i++)
	{
		OutResults.Add(MoveTemp(ScoredActions[i]));
	}
}

static TArray<FLuaFunctionDoc> FindNodesDocs = {
	{
		TEXT("find_nodes(query, asset_path?, graph_name?, max_results?)"),
		TEXT("Search for available node types to add to a graph.\n"
			"  IMPORTANT: First argument is the search QUERY string (e.g. \"Delay\", \"Print String\"), NOT the asset path.\n"
			"  Example: find_nodes(\"Delay\", \"/Game/MyBP\", \"EventGraph\", 5)\n"
			"  For Blueprint graphs: uses the Blueprint action database (rich results with _spawner_id).\n"
			"  For Material/BehaviorTree/SoundCue/EQS graphs: uses schema context actions.\n"
			"  For ControlRig graphs: queries the RigVM unit struct registry.\n"
			"  asset_path and graph_name help narrow results to compatible nodes.\n"
			"  max_results defaults to 10, clamped to 1-100.\n"
			"  Searches name, category, keywords, and tooltip (in priority order).\n"
			"  Returns results sorted by relevance score."),
		TEXT("table of {name, category, tooltip, keywords?, score, _spawner_id?, _action_id?, owning_class?, pins_in?, pins_out?}")
	}
};

// Runtime RigVM graph detection — works without ControlRig module linkage.
// Leaf class name check only. UControlRigGraph won't match — that's OK,
// find_nodes falls through to schema context actions which work fine.
static bool IsRigVMGraphRuntime(UEdGraph* Graph)
{
	if (!Graph) return false;
	return Graph->GetClass()->GetName().Contains(TEXT("RigVM"));
}

// Helper functions extracted outside REGISTER_LUA_BINDING to avoid #if inside macro arguments (MSVC C5101)
#if WITH_CONTROLRIG
static bool FindNodes_IsControlRigGraph(UObject* Asset, const FString& FGraphName, UEdGraph*& OutGraph)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("ControlRig"))) return false;
	UEdGraph* TestGraph = LuaGraphResolver::FindGraph(Asset, FGraphName);
	if (TestGraph && LuaControlRig::IsRigVMGraph(TestGraph))
	{
		OutGraph = TestGraph;
		return true;
	}
	return false;
}

static void FindNodes_QueryControlRig(const FString& FQuery, int32 MaxResults, TArray<FNodeSearchResult>& Results)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("ControlRig"))) return;
	TArray<LuaControlRig::FRigVMNodeSearchResult> CRResults = LuaControlRig::FindNodes(FQuery, MaxResults);
	for (const LuaControlRig::FRigVMNodeSearchResult& CR : CRResults)
	{
		FNodeSearchResult R;
		R.Name = CR.Name;
		R.Category = CR.Category;
		R.Tooltip = CR.Keywords;
		R.Score = CR.Score;
		Results.Add(R);
	}
}
#else
static bool FindNodes_IsControlRigGraph(UObject* Asset, const FString& FGraphName, UEdGraph*& OutGraph)
{
	// Runtime detection: check graph class name without ControlRig headers
	UEdGraph* TestGraph = LuaGraphResolver::FindGraph(Asset, FGraphName);
	if (TestGraph && IsRigVMGraphRuntime(TestGraph))
	{
		OutGraph = TestGraph;
		return true;
	}
	return false;
}

static void FindNodes_QueryControlRig(const FString& FQuery, int32 MaxResults, TArray<FNodeSearchResult>& Results)
{
	// Without ControlRig module linkage, we can't query the RigVM unit struct registry.
	// Schema context actions will be used instead (handled by caller).
}
#endif

REGISTER_LUA_BINDING(FindNodes, FindNodesDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("find_nodes", [&Session](const std::string& Query,
		sol::optional<std::string> AssetPath, sol::optional<std::string> GraphNameOpt,
		sol::optional<int> Max, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString FQuery = UTF8_TO_TCHAR(Query.c_str());
		int32 MaxResults = FMath::Clamp(Max.value_or(10), 1, 100);

		UBlueprint* ContextBP = nullptr;
		UEdGraph* ContextGraph = nullptr;
		bool bIsControlRig = false;

		if (AssetPath.has_value())
		{
			FString FPath = UTF8_TO_TCHAR(AssetPath.value().c_str());
			if (!FPath.StartsWith(TEXT("/")))
			{
				FPath = TEXT("/Game/") + FPath;
			}

			UObject* Asset = LoadObject<UObject>(nullptr, *FPath);
			if (Asset)
			{
				ContextBP = Cast<UBlueprint>(Asset);
				if (ContextBP)
				{
					// Check if this is a RigVM blueprint (ControlRig, etc.)
					FString FGraphName = GraphNameOpt.has_value()
						? UTF8_TO_TCHAR(GraphNameOpt.value().c_str()) : TEXT("");
					UEdGraph* CRGraph = nullptr;
					if (FindNodes_IsControlRigGraph(Asset, FGraphName, CRGraph))
					{
						bIsControlRig = true;
						ContextGraph = CRGraph;
					}
					else if (!FGraphName.IsEmpty())
					{
						// Resolve the specific sub-graph for context-aware discovery
						// (e.g. AnimBP transition/state graphs need schema-based search)
						ContextGraph = NeoBlueprint::FindGraph(ContextBP, FGraphName);
					}
				}
				else if (Asset->GetClass()->GetName() == TEXT("NiagaraSystem") || Asset->GetClass()->GetName() == TEXT("NiagaraScript"))
				{
					// Niagara uses stack-based editing, not schema actions
					Session.Log(FString::Printf(TEXT("[FAIL] find_nodes on Niagara is not supported. "
						"Niagara uses stack-based editing. Use open_asset(\"%s\") then list(\"modules\"), "
						"add(\"module\", {...}), configure(\"module\", ...) instead."),
						*FPath));
					return LuaView.create_table();
				}
				else
				{
					// Non-Blueprint: resolve to graph for schema-based discovery
					FString FGraphName = GraphNameOpt.has_value()
						? UTF8_TO_TCHAR(GraphNameOpt.value().c_str()) : TEXT("");
					ContextGraph = LuaGraphResolver::FindGraph(Asset, FGraphName);
				}
			}
		}

		TArray<FNodeSearchResult> Results;

		if (bIsControlRig)
		{
			// ControlRig: query FRigVMRegistry via URigVMController (when linked),
			// or fall back to schema context actions (generic discovery)
			FindNodes_QueryControlRig(FQuery, MaxResults, Results);
			if (Results.Num() == 0 && ContextGraph)
			{
				// Fallback: use schema context actions (works for any graph type)
				FindNodesViaSchema(ContextGraph, FQuery, MaxResults, Results);
			}
		}
		else if (ContextBP || !AssetPath.has_value())
		{
			if (ContextGraph && ContextBP)
			{
				// Blueprint with a specific sub-graph targeted (e.g. AnimBP transition/state graph)
				// Use the engine's context-aware action menu (same as editor right-click)
				Results = NeoNodes::FindNodesForGraph(FQuery, ContextBP, ContextGraph, MaxResults);
			}
			else
			{
				// Blueprint path — global action DB search
				Results = NeoNodes::FindNodes(FQuery, ContextBP, MaxResults);
			}
		}
		else if (ContextGraph)
		{
			// Non-Blueprint: schema-based discovery
			FindNodesViaSchema(ContextGraph, FQuery, MaxResults, Results);
		}
		else
		{
			// Fallback
			Results = NeoNodes::FindNodes(FQuery, nullptr, MaxResults);
		}

		if (Results.Num() == 0)
		{
			Session.Log(FString::Printf(TEXT("[OK] find_nodes(\"%s\") -> 0 results"), *FQuery));
			return LuaView.create_table();
		}

		Session.Log(FString::Printf(TEXT("[OK] find_nodes(\"%s\") -> %d results"), *FQuery, Results.Num()));

		sol::table ResultTable = LuaView.create_table();
		for (int32 i = 0; i < Results.Num(); i++)
		{
			const FNodeSearchResult& R = Results[i];

			sol::table Entry = LuaView.create_table();
			Entry["name"] = TCHAR_TO_UTF8(*R.Name);
			Entry["category"] = TCHAR_TO_UTF8(*R.Category);
			Entry["tooltip"] = TCHAR_TO_UTF8(*R.Tooltip);
			Entry["score"] = R.Score;

			if (!R.Keywords.IsEmpty())
			{
				Entry["keywords"] = TCHAR_TO_UTF8(*R.Keywords);
			}

			// Blueprint spawner
			if (R.Spawner.IsValid())
			{
				FString SpawnerId = Session.CacheSpawner(R.Spawner.Get());
				Entry["_spawner_id"] = TCHAR_TO_UTF8(*SpawnerId);

				// Extract owning class for disambiguation (e.g., Pawn::GetControlRotation vs Controller::GetControlRotation)
				UClass const* OwnerClass = nullptr;
				if (const UBlueprintFunctionNodeSpawner* FuncSpawner = Cast<UBlueprintFunctionNodeSpawner>(R.Spawner.Get()))
				{
					if (UFunction const* Func = FuncSpawner->GetFunction())
						OwnerClass = Func->GetOwnerClass();
				}
				else if (const UBlueprintVariableNodeSpawner* VarSpawner = Cast<UBlueprintVariableNodeSpawner>(R.Spawner.Get()))
				{
					if (const FProperty* VarProp = VarSpawner->GetVarProperty())
						OwnerClass = VarProp->GetOwnerClass();
				}
				if (OwnerClass)
				{
					Entry["owning_class"] = TCHAR_TO_UTF8(*OwnerClass->GetName());
					// Log owning_class so agent can disambiguate without extra iteration
					Session.Log(FString::Printf(TEXT("  %d: %s | %s | class=%s | %s"),
						i + 1, *R.Name, *R.Category, *OwnerClass->GetName(), *SpawnerId));
				}
				else
				{
					Session.Log(FString::Printf(TEXT("  %d: %s | %s | %s"),
						i + 1, *R.Name, *R.Category, *SpawnerId));
				}
			}
			// Schema action (non-Blueprint graphs: BT, Material, SoundCue, EQS, etc.)
			else if (R.SchemaAction.IsValid() && R.SchemaGraph.IsValid())
			{
				FString ActionId = Session.CacheSchemaAction(R.SchemaAction, R.SchemaGraph.Get());
				Entry["_action_id"] = TCHAR_TO_UTF8(*ActionId);
				Session.Log(FString::Printf(TEXT("  %d: %s | %s | %s"),
					i + 1, *R.Name, *R.Category, *ActionId));
			}

			// Pin info (only when available — Blueprint spawners populate these)
			if (R.InputPins.Num() > 0)
			{
				sol::table InPins = LuaView.create_table();
				for (int32 p = 0; p < R.InputPins.Num(); p++)
				{
					InPins[p + 1] = TCHAR_TO_UTF8(*R.InputPins[p]);
				}
				Entry["pins_in"] = InPins;
			}

			if (R.OutputPins.Num() > 0)
			{
				sol::table OutPins = LuaView.create_table();
				for (int32 p = 0; p < R.OutputPins.Num(); p++)
				{
					OutPins[p + 1] = TCHAR_TO_UTF8(*R.OutputPins[p]);
				}
				Entry["pins_out"] = OutPins;
			}

			ResultTable[i + 1] = Entry;
		}

		return ResultTable;
	});
});
