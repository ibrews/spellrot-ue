#pragma once

#include "CoreMinimal.h"
#include "Engine/Blueprint.h"

class USCS_Node;
class UK2Node_CustomEvent;
class UK2Node_Timeline;

struct FBlueprintInfo
{
	FString Name;
	FString AssetPath;
	FString ParentClass;
	TArray<FString> Graphs;
	UBlueprint* Blueprint = nullptr;
};

struct FCompileResult
{
	bool bSuccess = false;
	TArray<FString> Errors;
	TArray<FString> Warnings;
};

struct FParamDesc
{
	FString Name;
	FString Type;
};

namespace NeoBlueprint
{
	FBlueprintInfo LoadBlueprint(const FString& AssetPath);
	UEdGraph* FindGraph(UBlueprint* BP, const FString& GraphName);
	FCompileResult CompileBlueprint(UBlueprint* BP);
	bool SaveBlueprint(UBlueprint* BP);
	UBlueprint* CreateBlueprint(const FString& AssetPath, const FString& ParentClassName);
	bool AddVariable(UBlueprint* BP, const FString& VarName, const FString& VarType, const FString& DefaultValue);
	bool DeleteNode(UEdGraphNode* Node);
	USCS_Node* AddComponent(UBlueprint* BP, const FString& ComponentName, const FString& ComponentClassName, const FString& ParentName);
	UEdGraph* AddFunctionGraph(UBlueprint* BP, const FString& FunctionName);
	bool MoveNode(UEdGraphNode* Node, double X, double Y);

	// Event Dispatchers (delegate variable + signature graph)
	UEdGraph* AddEventDispatcher(UBlueprint* BP, const FString& Name, const TArray<FParamDesc>& Params);

	// Function params/returns (adds pins to entry/result nodes)
	bool AddFunctionParam(UBlueprint* BP, const FString& FuncName, const FString& ParamName, const FString& ParamType);
	bool AddFunctionReturn(UBlueprint* BP, const FString& FuncName, const FString& ReturnName, const FString& ReturnType);

	// Function flags
	bool SetFunctionFlags(UBlueprint* BP, const FString& FuncName, uint32 AddFlags, uint32 RemoveFlags = 0);

	// Variable/function metadata
	bool SetVariableProperty(UBlueprint* BP, const FString& VarName, const FString& Key, const FString& Value);

	// Generic property access via reflection (ImportText/ExportText — works for ANY property type)
	bool SetObjectProperty(UObject* Object, const FString& PropertyName, const FString& Value, FString& OutError);
	bool GetObjectProperty(UObject* Object, const FString& PropertyName, FString& OutValue);
	void ListObjectProperties(UObject* Object, const FString& Filter, TArray<TPair<FString, FString>>& OutProps);
	FString FuzzyMatchProperty(UObject* Object, const FString& PropertyName);

	// Component template access
	UActorComponent* GetComponentTemplate(UBlueprint* BP, const FString& ComponentName);

	// Remove (auto-cleans references)
	bool RemoveVariable(UBlueprint* BP, const FString& VarName);
	bool RemoveFunction(UBlueprint* BP, const FString& FuncName);
	bool RemoveComponent(UBlueprint* BP, const FString& ComponentName);

	// Rename (auto-updates references)
	bool RenameVariable(UBlueprint* BP, const FString& OldName, const FString& NewName);
	bool RenameFunction(UBlueprint* BP, const FString& OldName, const FString& NewName);

	// Local variables
	bool AddLocalVariable(UBlueprint* BP, const FString& FuncName, const FString& VarName, const FString& VarType, const FString& DefaultValue);
	bool RemoveLocalVariable(UBlueprint* BP, const FString& FuncName, const FString& VarName);
	bool RenameLocalVariable(UBlueprint* BP, const FString& FuncName, const FString& OldName, const FString& NewName);

	// Change variable type
	bool ChangeVariableType(UBlueprint* BP, const FString& VarName, const FString& NewType);
	bool ChangeLocalVariableType(UBlueprint* BP, const FString& FuncName, const FString& VarName, const FString& NewType);

	// Reorder variables
	bool MoveVariableBefore(UBlueprint* BP, const FString& VarName, const FString& TargetVarName);
	bool MoveVariableAfter(UBlueprint* BP, const FString& VarName, const FString& TargetVarName);

	// Query
	bool IsVariableUsed(UBlueprint* BP, const FString& VarName);

	// Interfaces
	UBlueprint* CreateInterfaceBlueprint(const FString& AssetPath);
	bool AddInterface(UBlueprint* BP, const FString& InterfaceName);
	bool RemoveInterface(UBlueprint* BP, const FString& InterfaceName, bool bPreserveFunctions = false);

	// Macros
	UEdGraph* AddMacroGraph(UBlueprint* BP, const FString& MacroName);

	// Custom Events (red event nodes in EventGraph) — upserts: creates or updates
	UK2Node_CustomEvent* FindCustomEvent(UBlueprint* BP, const FString& EventName);
	UK2Node_CustomEvent* AddCustomEvent(UBlueprint* BP, const FString& EventName, const TArray<FParamDesc>& Params);

	// Animation Blueprint — state machines, states, transitions
	UEdGraph* FindAnimGraph(UBlueprint* BP);

	struct FAnimStateInfo
	{
		FString Name;
		FString Handle;       // NodeGuid
		FString GraphName;    // selector path e.g. "Locomotion/Idle"
		UEdGraph* BoundGraph = nullptr;
		UEdGraphNode* Node = nullptr;
	};

	struct FAnimTransitionInfo
	{
		FString FromName;
		FString ToName;
		FString Handle;
		FString GraphName;      // e.g. "Locomotion/Idle->Run"
		FString ResultHandle;   // TransitionResult node guid
		UEdGraph* BoundGraph = nullptr;
		UEdGraphNode* Node = nullptr;
	};

	// Collect all AnimBP subgraphs with selector-style names into OutGraphs
	void CollectAnimBPGraphs(UBlueprint* BP, TArray<TPair<FString, UEdGraph*>>& OutGraphs);

	// Generic asset creation via factory auto-discovery
	struct FCreateAssetResult
	{
		UObject* Asset = nullptr;
		FString Error;
		TArray<FString> FactoryProperties; // Available UPROPERTY names+types for error reporting
	};

	FCreateAssetResult CreateAssetGeneric(const FString& AssetPath, const FString& AssetTypeName,
		const TMap<FString, FString>& Options);

	// List available factory properties for a given asset type
	void ListFactoryProperties(const FString& AssetTypeName, TArray<TPair<FString, FString>>& OutProps);

	// List all supported asset type aliases (alias → class path)
	void ListAssetTypeAliases(TArray<TPair<FString, FString>>& OutTypes);

	// Timelines — upserts: creates or updates
	struct FTimelineTrackDesc
	{
		FString Name;
		FString Type;  // "float", "vector", "color", "event"
		TArray<TPair<float, FString>> Keys;  // time → value pairs
	};
	UK2Node_Timeline* FindTimelineNode(UBlueprint* BP, const FString& TimelineName);
	UK2Node_Timeline* AddTimeline(UBlueprint* BP, const FString& TimelineName, float Length,
		bool bAutoPlay, bool bLoop, const TArray<FTimelineTrackDesc>& Tracks);
	bool AddTimelineTrack(UBlueprint* BP, const FString& TimelineName, const FTimelineTrackDesc& Track);
	bool RemoveTimeline(UBlueprint* BP, const FString& TimelineName);
}
