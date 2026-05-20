// Copyright 2025-2026 Betide Studio. All Rights Reserved.
// Shared utility: ControlRig/RigVM graph operations via URigVMController.
// ControlRig cannot use standard schema actions — all edits go through the controller.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraph.h"
#include "Blueprint/NodeUtils.h"

#if WITH_CONTROLRIG || defined(AIK_CONTROLRIG_MODULE)

// RigVM model (source of truth)
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/RigVMClient.h"

// RigVM editor graph (UEdGraph wrapper)
#include "EdGraph/RigVMEdGraph.h"
#include "EdGraph/RigVMEdGraphNode.h"

namespace LuaControlRig
{

// ---- Detection ----

inline bool IsRigVMGraph(UEdGraph* Graph)
{
	return Graph && Graph->IsA<URigVMEdGraph>();
}

inline bool IsRigVMNode(UEdGraphNode* Node)
{
	return Node && Node->IsA<URigVMEdGraphNode>();
}

// ---- Accessors ----

inline URigVMEdGraph* GetEdGraph(UEdGraph* Graph)
{
	return Cast<URigVMEdGraph>(Graph);
}

inline URigVMController* GetController(UEdGraph* Graph)
{
	URigVMEdGraph* RigGraph = Cast<URigVMEdGraph>(Graph);
	return RigGraph ? RigGraph->GetController() : nullptr;
}

inline URigVMGraph* GetModel(UEdGraph* Graph)
{
	URigVMEdGraph* RigGraph = Cast<URigVMEdGraph>(Graph);
	return RigGraph ? RigGraph->GetModel() : nullptr;
}

inline URigVMNode* GetModelNode(UEdGraphNode* EdNode)
{
	URigVMEdGraphNode* RigNode = Cast<URigVMEdGraphNode>(EdNode);
	return RigNode ? RigNode->GetModelNode() : nullptr;
}

inline FName GetModelNodeName(UEdGraphNode* EdNode)
{
	URigVMEdGraphNode* RigNode = Cast<URigVMEdGraphNode>(EdNode);
	return RigNode ? RigNode->GetModelNodeName() : NAME_None;
}

// Build the dot-notation pin path that URigVMController expects: "NodeName.PinName"
inline FString BuildPinPath(UEdGraphNode* EdNode, const FString& PinName)
{
	FName NodeName = GetModelNodeName(EdNode);
	if (NodeName.IsNone()) return FString();
	return FString::Printf(TEXT("%s.%s"), *NodeName.ToString(), *PinName);
}

// ---- Graph Enumeration ----

// Get all URigVMGraph models from a RigVM-based blueprint, with their EdGraph wrappers.
// Returns pairs of (GraphName, UEdGraph*) for use in LuaGraphResolver.
inline void GetAllGraphs(UBlueprint* BP, TArray<TPair<FString, UEdGraph*>>& OutGraphs)
{
	if (!BP) return;

	// Collect all EdGraphs that are URigVMEdGraph
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		URigVMEdGraph* RigGraph = Cast<URigVMEdGraph>(Graph);
		if (RigGraph)
		{
			URigVMGraph* Model = RigGraph->GetModel();
			FString Name = Model ? Model->GetNodePath() : Graph->GetName();
			// Use just the graph name for simple display
			if (Name.Contains(TEXT(".")))
			{
				Name = Name.RightChop(Name.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd) + 1);
			}
			if (Name.IsEmpty()) Name = Graph->GetName();
			OutGraphs.Add(TPair<FString, UEdGraph*>(Name, Graph));
		}
	}

	// Also check function graphs
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		URigVMEdGraph* RigGraph = Cast<URigVMEdGraph>(Graph);
		if (RigGraph)
		{
			URigVMGraph* Model = RigGraph->GetModel();
			FString Name = Model ? Model->GetNodePath() : Graph->GetName();
			if (Name.Contains(TEXT(".")))
			{
				Name = Name.RightChop(Name.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd) + 1);
			}
			if (Name.IsEmpty()) Name = Graph->GetName();
			OutGraphs.Add(TPair<FString, UEdGraph*>(Name, Graph));
		}
	}
}

// ---- Node Discovery (find_nodes) ----

struct FRigVMNodeSearchResult
{
	FString Name;
	FString Category;
	FString Keywords;
	FString StructPath;  // e.g. "/Script/ControlRig.FRigUnit_SetTransform"
	int32 Score = 0;
};

inline TArray<FRigVMNodeSearchResult> FindNodes(const FString& Query, int32 MaxResults)
{
	TArray<FRigVMNodeSearchResult> Results;
	FString QueryLower = Query.ToLower();

	// Query all registered unit structs
	TArray<UScriptStruct*> UnitStructs = URigVMController::GetRegisteredUnitStructs();

	struct FScoredResult
	{
		FRigVMNodeSearchResult Info;
		int32 Score;
	};
	TArray<FScoredResult> Scored;

	for (UScriptStruct* Struct : UnitStructs)
	{
		if (!Struct) continue;

		FString DisplayName = Struct->GetDisplayNameText().ToString();
		FString StructName = Struct->GetName();
		FString StructPath = Struct->GetPathName();

		// Get metadata
		FString Category = Struct->GetMetaData(TEXT("Category"));
		FString Keywords = Struct->GetMetaData(TEXT("Keywords"));

		FString DisplayLower = DisplayName.ToLower();
		FString StructLower = StructName.ToLower();
		FString KeywordsLower = Keywords.ToLower();

		// Score: exact > starts with > contains > keywords > category
		int32 Score = 0;
		if (DisplayLower == QueryLower || StructLower == QueryLower) Score = 100;
		else if (DisplayLower.StartsWith(QueryLower) || StructLower.StartsWith(QueryLower)) Score = 80;
		else if (DisplayLower.Contains(QueryLower) || StructLower.Contains(QueryLower)) Score = 60;
		else if (KeywordsLower.Contains(QueryLower)) Score = 40;
		else if (Category.ToLower().Contains(QueryLower)) Score = 20;
		else continue;

		FRigVMNodeSearchResult Info;
		Info.Name = DisplayName.IsEmpty() ? StructName : DisplayName;
		Info.Category = Category;
		Info.Keywords = Keywords;
		Info.StructPath = StructPath;
		Info.Score = Score;

		Scored.Add({Info, Score});
	}

	// Sort by score descending
	Scored.Sort([](const FScoredResult& A, const FScoredResult& B) { return A.Score > B.Score; });

	int32 Count = FMath::Min(Scored.Num(), MaxResults);
	for (int32 i = 0; i < Count; i++)
	{
		Results.Add(Scored[i].Info);
	}

	return Results;
}

// ---- Node Creation (add_node) ----

// Add a node by struct path. Returns the new UEdGraphNode* (the EdGraph wrapper) or nullptr.
inline UEdGraphNode* AddNode(UEdGraph* Graph, const FString& StructPath, double X, double Y, FString& OutError)
{
	URigVMController* Controller = GetController(Graph);
	if (!Controller)
	{
		OutError = TEXT("could not get URigVMController for this graph");
		return nullptr;
	}

	URigVMEdGraph* RigGraph = GetEdGraph(Graph);
	if (!RigGraph)
	{
		OutError = TEXT("graph is not a RigVM graph");
		return nullptr;
	}

	// Ensure the asset editor is open — this initializes the EdGraph's notification
	// delegate so that model changes create the EdGraph wrapper nodes.
	NeoNodes::EnsureEditorOpenForSchema(Graph);

	Controller->OpenUndoBracket(FString::Printf(TEXT("Add Node %s"), *StructPath));

	URigVMUnitNode* ModelNode = Controller->AddUnitNodeFromStructPath(
		StructPath,
		TEXT("Execute"),
		FVector2D(X, Y),
		FString(),  // auto-name
		true,       // bSetupUndoRedo
		true        // bPrintPythonCommand
	);

	if (!ModelNode)
	{
		Controller->CancelUndoBracket();
		OutError = FString::Printf(TEXT("AddUnitNodeFromStructPath failed for \"%s\""), *StructPath);
		return nullptr;
	}

	Controller->CloseUndoBracket();

	// Find the EdGraph wrapper node
	UEdGraphNode* EdNode = RigGraph->FindNodeForModelNodeName(ModelNode->GetFName());
	if (!EdNode)
	{
		OutError = TEXT("node created in model but EdGraph wrapper not found");
	}

	return EdNode;
}

// Add a node by display name (fuzzy match). Resolves to struct path, then creates.
inline UEdGraphNode* AddNodeByName(UEdGraph* Graph, const FString& NodeName, double X, double Y, FString& OutError)
{
	// Search for matching node types
	TArray<FRigVMNodeSearchResult> Results = FindNodes(NodeName, 10);

	if (Results.Num() == 0)
	{
		OutError = FString::Printf(TEXT("no RigVM node type matching \"%s\" found"), *NodeName);
		return nullptr;
	}

	// Check for ambiguity at top score
	int32 TopScore = Results[0].Score;
	int32 AmbiguousCount = 0;
	for (const FRigVMNodeSearchResult& R : Results)
	{
		if (R.Score == TopScore) AmbiguousCount++;
		else break;
	}

	if (AmbiguousCount > 1 && TopScore < 100)
	{
		FString Options;
		for (int32 i = 0; i < AmbiguousCount && i < 5; i++)
		{
			if (i > 0) Options += TEXT("\n");
			Options += FString::Printf(TEXT("  [%d] \"%s\" (%s)"), i + 1, *Results[i].Name, *Results[i].Category);
		}
		OutError = FString::Printf(TEXT("ambiguous, %d nodes match \"%s\" equally. Use find_nodes() for exact name:\n%s"),
			AmbiguousCount, *NodeName, *Options);
		return nullptr;
	}

	if (Results[0].Name != NodeName)
	{
		// Log the fuzzy match (caller will handle this)
	}

	return AddNode(Graph, Results[0].StructPath, X, Y, OutError);
}

// ---- Connections (connect) ----

inline bool Connect(UEdGraph* Graph, UEdGraphNode* FromNode, const FString& FromPin,
	UEdGraphNode* ToNode, const FString& ToPin, FString& OutError)
{
	URigVMController* Controller = GetController(Graph);
	if (!Controller)
	{
		OutError = TEXT("could not get URigVMController");
		return false;
	}

	FString OutputPath = BuildPinPath(FromNode, FromPin);
	FString InputPath = BuildPinPath(ToNode, ToPin);

	if (OutputPath.IsEmpty() || InputPath.IsEmpty())
	{
		OutError = TEXT("could not resolve model node names for pin paths");
		return false;
	}

	Controller->OpenUndoBracket(TEXT("Connect Pins"));

	bool bSuccess = Controller->AddLink(OutputPath, InputPath, true, true);

	if (bSuccess)
	{
		Controller->CloseUndoBracket();
	}
	else
	{
		Controller->CancelUndoBracket();
		OutError = FString::Printf(TEXT("AddLink failed: %s -> %s"), *OutputPath, *InputPath);
	}

	return bSuccess;
}

// ---- Disconnect ----

inline bool Disconnect(UEdGraph* Graph, UEdGraphNode* Node, const FString& PinName, FString& OutError)
{
	URigVMController* Controller = GetController(Graph);
	if (!Controller)
	{
		OutError = TEXT("could not get URigVMController");
		return false;
	}

	FString PinPath = BuildPinPath(Node, PinName);
	if (PinPath.IsEmpty())
	{
		OutError = TEXT("could not resolve model node name");
		return false;
	}

	Controller->OpenUndoBracket(TEXT("Disconnect Pin"));
	bool bSuccess = Controller->BreakAllLinks(PinPath, true, true);
	if (bSuccess)
	{
		Controller->CloseUndoBracket();
	}
	else
	{
		// BreakAllLinks on a pin with no links still returns false — not necessarily an error
		Controller->CancelUndoBracket();
	}

	return true; // breaking links on an unconnected pin is not a failure
}

// ---- Set Pin Default Value ----

inline bool SetPinValue(UEdGraph* Graph, UEdGraphNode* Node, const FString& PinName,
	const FString& Value, FString& OutError)
{
	URigVMController* Controller = GetController(Graph);
	if (!Controller)
	{
		OutError = TEXT("could not get URigVMController");
		return false;
	}

	FString PinPath = BuildPinPath(Node, PinName);
	if (PinPath.IsEmpty())
	{
		OutError = TEXT("could not resolve model node name");
		return false;
	}

	bool bSuccess = Controller->SetPinDefaultValue(PinPath, Value, true, true, false, true);
	if (!bSuccess)
	{
		OutError = FString::Printf(TEXT("SetPinDefaultValue failed for \"%s\" = \"%s\""), *PinPath, *Value);
	}

	return bSuccess;
}

// ---- Get Pin Default Value ----

inline FString GetPinValue(UEdGraph* Graph, UEdGraphNode* Node, const FString& PinName)
{
	URigVMController* Controller = GetController(Graph);
	if (!Controller) return FString();

	FString PinPath = BuildPinPath(Node, PinName);
	if (PinPath.IsEmpty()) return FString();

	return Controller->GetPinDefaultValue(PinPath);
}

// ---- Delete Node ----

inline bool DeleteNode(UEdGraph* Graph, UEdGraphNode* EdNode, FString& OutError)
{
	URigVMController* Controller = GetController(Graph);
	if (!Controller)
	{
		OutError = TEXT("could not get URigVMController");
		return false;
	}

	URigVMNode* ModelNode = GetModelNode(EdNode);
	if (!ModelNode)
	{
		OutError = TEXT("could not find model node for this EdGraph node");
		return false;
	}

	Controller->OpenUndoBracket(TEXT("Delete Node"));

	bool bSuccess = Controller->RemoveNode(ModelNode, true, true);

	if (bSuccess)
	{
		Controller->CloseUndoBracket();
	}
	else
	{
		Controller->CancelUndoBracket();
		OutError = TEXT("RemoveNode failed");
	}

	return bSuccess;
}

} // namespace LuaControlRig

#else // !WITH_CONTROLRIG && !AIK_CONTROLRIG_MODULE

// Stub namespace when ControlRig plugin is not enabled
namespace LuaControlRig
{

inline bool IsRigVMGraph(UEdGraph* Graph) { return false; }
inline bool IsRigVMNode(UEdGraphNode* Node) { return false; }

} // namespace LuaControlRig

#endif // WITH_CONTROLRIG || AIK_CONTROLRIG_MODULE
