// Graph operations: split_pin, recombine_pin, add_exec_pin, remove_exec_pin,
// layout_nodes, convert_pin_type, set_node_comment, distribute_nodes, reset_pin,
// set_breakpoint, clear_breakpoint, toggle_breakpoint, add_reroute_node, delete_nodes,
// promote_pin_to_variable, promote_pin_to_local_variable, move_node, duplicate_nodes
// Also: specific disconnect (break one link, not all)

#include "Lua/LuaBindingRegistry.h"
#include "Modules/ModuleManager.h"
#if WITH_CONTROLRIG
#include "Lua/LuaControlRigHelper.h"
#endif
#include "Lua/LuaPinHelper.h"
#include "Blueprint/BlueprintUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_K2_Actions.h"
#include "EdGraphUtilities.h"
#include "K2Node_AddPinInterface.h"
#include "K2Node_Switch.h"
#include "K2Node_Knot.h"
#include "K2Node_PromotableOperator.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "BlueprintTypePromotion.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetDebugUtilities.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/Breakpoint.h"
#include "Editor.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ---- Pin finder (same as LuaBinding_Connect) ----

static UEdGraphPin* FindPinOnNodeAny(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction = EGPD_MAX)
{
	if (!Node) return nullptr;

	// Exact match by PinName
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) continue;
		if (Pin->PinName.ToString() == PinName && (Direction == EGPD_MAX || Pin->Direction == Direction))
			return Pin;
	}

	// Display name
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) continue;
		if (Pin->GetDisplayName().ToString() == PinName && (Direction == EGPD_MAX || Pin->Direction == Direction))
			return Pin;
	}

	// Generated usable name (handles "in_0", "out_0" fallbacks for blank pin names)
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) continue;
		if (NeoLuaPin::GetUsablePinName(Pin) == PinName && (Direction == EGPD_MAX || Pin->Direction == Direction))
			return Pin;
	}

	// Case-insensitive (all name sources)
	FString Lower = PinName.ToLower();
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) continue;
		if ((Pin->PinName.ToString().ToLower() == Lower
			|| Pin->GetDisplayName().ToString().ToLower() == Lower
			|| NeoLuaPin::GetUsablePinName(Pin).ToLower() == Lower)
			&& (Direction == EGPD_MAX || Pin->Direction == Direction))
			return Pin;
	}

	return nullptr;
}

static FString ListPins(UEdGraphNode* Node, EEdGraphPinDirection Direction = EGPD_MAX)
{
	if (!Node) return FString();
	FString List;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->bHidden) continue;
		if (Direction != EGPD_MAX && Pin->Direction != Direction) continue;
		if (List.Len() > 0) List += TEXT(", ");
		FString Name = NeoLuaPin::GetUsablePinName(Pin);
		List += Name;
	}
	return List;
}

// ---- Helper functions extracted outside REGISTER_LUA_BINDING to avoid #if inside macro arguments (MSVC C5101) ----

#if WITH_CONTROLRIG

static bool GraphOps_IsRigVMNode(UEdGraphNode* Node) { return FModuleManager::Get().IsModuleLoaded(TEXT("ControlRig")) && LuaControlRig::IsRigVMNode(Node); }
static bool GraphOps_IsRigVMGraph(UEdGraph* Graph) { return FModuleManager::Get().IsModuleLoaded(TEXT("ControlRig")) && LuaControlRig::IsRigVMGraph(Graph); }

static bool DisconnectFrom_HandleControlRig(FLuaSessionData& Session, UEdGraphNode* FromNode, const FString& FFromHandle, const FString& FFromPin,
	UEdGraphNode* ToNode, const FString& FToHandle, const FString& FToPin, sol::this_state S, sol::object& OutResult)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("ControlRig"))) return false;
	if (!LuaControlRig::IsRigVMNode(FromNode)) return false;

	URigVMController* Controller = LuaControlRig::GetController(FromNode->GetGraph());
	if (!Controller) { Session.Log(TEXT("[FAIL] disconnect_from -> no RigVM controller")); OutResult = sol::object(sol::lua_nil); return true; }

	FString OutputPath = LuaControlRig::BuildPinPath(FromNode, FFromPin);
	FString InputPath = LuaControlRig::BuildPinPath(ToNode, FToPin);
	if (OutputPath.IsEmpty() || InputPath.IsEmpty()) { Session.Log(TEXT("[FAIL] disconnect_from -> could not resolve pin paths")); OutResult = sol::object(sol::lua_nil); return true; }

	Controller->OpenUndoBracket(TEXT("Break Link"));
	bool bSuccess = Controller->BreakLink(OutputPath, InputPath, true, true);
	if (bSuccess)
	{
		Controller->CloseUndoBracket();
		Session.Log(FString::Printf(TEXT("[OK] disconnect_from(%s:%s -> %s:%s)"), *FFromHandle, *FFromPin, *FToHandle, *FToPin));
		OutResult = sol::make_object(S, true);
		return true;
	}
	Controller->CancelUndoBracket();
	Session.Log(FString::Printf(TEXT("[FAIL] disconnect_from -> BreakLink failed: %s -> %s"), *OutputPath, *InputPath));
	OutResult = sol::object(sol::lua_nil);
	return true;
}

static bool SplitPin_HandleControlRig(FLuaSessionData& Session, UEdGraphNode* Node, const FString& FHandle, const FString& FPinName, sol::this_state S, sol::object& OutResult)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("ControlRig"))) return false;
	if (!LuaControlRig::IsRigVMNode(Node)) return false;

	URigVMController* Controller = LuaControlRig::GetController(Node->GetGraph());
	if (!Controller) { Session.Log(TEXT("[FAIL] split_pin -> no RigVM controller")); OutResult = sol::object(sol::lua_nil); return true; }
	FString PinPath = LuaControlRig::BuildPinPath(Node, FPinName);
	if (Controller->SetPinExpansion(PinPath, true, false, false))
	{
		Session.Log(FString::Printf(TEXT("[OK] split_pin(%s, \"%s\") -> expanded via RigVM"), *FHandle, *FPinName));
		OutResult = sol::make_object(S, true);
		return true;
	}
	Session.Log(FString::Printf(TEXT("[FAIL] split_pin -> SetPinExpansion failed for \"%s\""), *PinPath));
	OutResult = sol::object(sol::lua_nil);
	return true;
}

static bool RecombinePin_HandleControlRig(FLuaSessionData& Session, UEdGraphNode* Node, const FString& FHandle, const FString& FPinName, sol::this_state S, sol::object& OutResult)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("ControlRig"))) return false;
	if (!LuaControlRig::IsRigVMNode(Node)) return false;

	URigVMController* Controller = LuaControlRig::GetController(Node->GetGraph());
	if (!Controller) { Session.Log(TEXT("[FAIL] recombine_pin -> no RigVM controller")); OutResult = sol::object(sol::lua_nil); return true; }
	FString PinPath = LuaControlRig::BuildPinPath(Node, FPinName);
	if (Controller->SetPinExpansion(PinPath, false, false, false))
	{
		Session.Log(FString::Printf(TEXT("[OK] recombine_pin(%s, \"%s\") -> collapsed via RigVM"), *FHandle, *FPinName));
		OutResult = sol::make_object(S, true);
		return true;
	}
	Session.Log(FString::Printf(TEXT("[FAIL] recombine_pin -> SetPinExpansion(false) failed for \"%s\""), *PinPath));
	OutResult = sol::object(sol::lua_nil);
	return true;
}

static bool SetNodeComment_HandleControlRig(FLuaSessionData& Session, UEdGraphNode* Node, const FString& FHandle, const FString& FText, bool bVisible, sol::this_state S, sol::object& OutResult)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("ControlRig"))) return false;
	if (!LuaControlRig::IsRigVMNode(Node)) return false;

	URigVMController* Controller = LuaControlRig::GetController(Node->GetGraph());
	if (!Controller) { Session.Log(TEXT("[FAIL] set_node_comment -> no RigVM controller")); OutResult = sol::object(sol::lua_nil); return true; }

	FName NodeName = LuaControlRig::GetModelNodeName(Node);
	if (Controller->SetCommentTextByName(NodeName, FText, 18, bVisible, false, false, false))
	{
		Session.Log(FString::Printf(TEXT("[OK] set_node_comment(%s) via RigVM"), *FHandle));
		OutResult = sol::make_object(S, true);
		return true;
	}
	Session.Log(TEXT("[FAIL] set_node_comment -> SetCommentTextByName failed (node may not be a comment)"));
	OutResult = sol::object(sol::lua_nil);
	return true;
}

static bool DeleteNodes_HandleControlRig(FLuaSessionData& Session, UEdGraphNode* Node, const FString& FHandle, bool& bSuccess)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("ControlRig"))) return false;
	if (!LuaControlRig::IsRigVMNode(Node)) return false;

	UEdGraph* Graph = Node->GetGraph();
	FString Error;
	bSuccess = LuaControlRig::DeleteNode(Graph, Node, Error);
	if (!bSuccess)
	{
		Session.Log(FString::Printf(TEXT("[WARN] delete_nodes -> failed to delete \"%s\": %s"), *FHandle, *Error));
	}
	return true;
}

#else

static bool GraphOps_IsRigVMNode(UEdGraphNode* Node) { return Node && Node->GetClass()->GetName().Contains(TEXT("RigVM")); }
static bool GraphOps_IsRigVMGraph(UEdGraph* Graph) { return Graph && Graph->GetClass()->GetName().Contains(TEXT("RigVM"));
}
static bool DisconnectFrom_HandleControlRig(FLuaSessionData&, UEdGraphNode*, const FString&, const FString&,
	UEdGraphNode*, const FString&, const FString&, sol::this_state, sol::object&) { return false; }
static bool SplitPin_HandleControlRig(FLuaSessionData&, UEdGraphNode*, const FString&, const FString&, sol::this_state, sol::object&) { return false; }
static bool RecombinePin_HandleControlRig(FLuaSessionData&, UEdGraphNode*, const FString&, const FString&, sol::this_state, sol::object&) { return false; }
static bool SetNodeComment_HandleControlRig(FLuaSessionData&, UEdGraphNode*, const FString&, const FString&, bool, sol::this_state, sol::object&) { return false; }
static bool DeleteNodes_HandleControlRig(FLuaSessionData&, UEdGraphNode*, const FString&, bool&) { return false; }

#endif // WITH_CONTROLRIG

// ---- Version-dependent PerformAction helper (FVector2f in 5.6+, FVector2D in 5.5) ----
static UEdGraphNode* PerformActionAtPosition(FEdGraphSchemaAction_K2NewNode& NodeInfo, UEdGraph* Graph, UEdGraphPin* Pin, float X, float Y)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	return NodeInfo.PerformAction(Graph, Pin, FVector2f(X, Y), false);
#else
	return NodeInfo.PerformAction(Graph, Pin, FVector2D(X, Y), false);
#endif
}

// ---- Docs ----

static TArray<FLuaFunctionDoc> GraphOpsDocs = {
	{ TEXT("disconnect_from(from_handle, from_pin, to_handle, to_pin)"), TEXT("Break a specific connection between two pins"), TEXT("true or nil") },
	{ TEXT("split_pin(handle, pin_name)"), TEXT("Split struct pin into components (Vector→X,Y,Z)"), TEXT("sub-pin names or nil") },
	{ TEXT("recombine_pin(handle, pin_name)"), TEXT("Recombine a split pin"), TEXT("true or nil") },
	{ TEXT("add_exec_pin(handle)"), TEXT("Add dynamic pin (Sequence, MakeArray, etc.)"), TEXT("pin name or nil") },
	{ TEXT("remove_exec_pin(handle)"), TEXT("Remove last dynamic pin"), TEXT("true or nil") },
	{ TEXT("layout_nodes(handles, opts?)"), TEXT("Auto-layout nodes on grid — opts: start_x/y, spacing_x/y, columns"), TEXT("true or nil") },
	{ TEXT("convert_pin_type(handle, pin_name, type?)"), TEXT("Convert pin type on promotable operators"), TEXT("string or nil") },
	{ TEXT("set_node_comment(handle, text, visible?)"), TEXT("Set comment bubble on node"), TEXT("true or nil") },
	{ TEXT("set_node_enabled(handle, state)"), TEXT("Set node enabled state: 'enabled', 'disabled', or 'development_only'"), TEXT("true or nil") },
	{ TEXT("distribute_nodes(handles, axis)"), TEXT("Distribute nodes evenly on 'x' or 'y'"), TEXT("true or nil") },
	{ TEXT("reset_pin(handle, pin_name)"), TEXT("Reset pin to default value"), TEXT("true or nil") },
	{ TEXT("set_breakpoint(handle, enabled?)"), TEXT("Set breakpoint on node"), TEXT("true or nil") },
	{ TEXT("clear_breakpoint(handle)"), TEXT("Clear breakpoint"), TEXT("true or nil") },
	{ TEXT("toggle_breakpoint(handle)"), TEXT("Toggle breakpoint"), TEXT("true or nil") },
	{ TEXT("add_reroute_node(graph_handle?, x?, y?)"), TEXT("Create a reroute/knot node at position"), TEXT("{handle, name, pins_in, pins_out} or nil") },
	{ TEXT("delete_nodes(handles)"), TEXT("Batch delete nodes from array of handles"), TEXT("{deleted=N, failed=N} or nil") },
	// collapse_to_function / collapse_to_macro removed: FBlueprintEditor collapse APIs are all protected with no public alternative
	{ TEXT("promote_pin_to_variable(handle, pin_name)"), TEXT("Promote pin to a member variable, creates get/set node and connects it"), TEXT("{var_name, node_handle} or nil") },
	{ TEXT("promote_pin_to_local_variable(handle, pin_name)"), TEXT("Promote pin to a function-local variable (pin must be in a function graph)"), TEXT("{var_name, node_handle} or nil") },
	{ TEXT("move_node(handle, x, y)"), TEXT("Move node to absolute position"), TEXT("true or nil") },
	{ TEXT("duplicate_nodes(handles, offset_x?, offset_y?)"), TEXT("Duplicate nodes and their connections within the same graph"), TEXT("{old_handle=new_handle, ...} or nil") },
};

REGISTER_LUA_BINDING(GraphOps, GraphOpsDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	// ---- disconnect_from(from_handle, from_pin, to_handle, to_pin) ----
	Lua.set_function("disconnect_from", [&Session](const std::string& FromHandle, const std::string& FromPinName,
		const std::string& ToHandle, const std::string& ToPinName, sol::this_state S) -> sol::object
	{
		FString FFromHandle = UTF8_TO_TCHAR(FromHandle.c_str());
		FString FToHandle = UTF8_TO_TCHAR(ToHandle.c_str());
		FString FFromPin = UTF8_TO_TCHAR(FromPinName.c_str());
		FString FToPin = UTF8_TO_TCHAR(ToPinName.c_str());

		UEdGraphNode* FromNode = Session.FindNode(FFromHandle);
		UEdGraphNode* ToNode = Session.FindNode(FToHandle);

		if (!FromNode) { Session.Log(FString::Printf(TEXT("[FAIL] disconnect_from -> source node \"%s\" not found"), *FFromHandle)); return sol::lua_nil; }
		if (!ToNode) { Session.Log(FString::Printf(TEXT("[FAIL] disconnect_from -> target node \"%s\" not found"), *FToHandle)); return sol::lua_nil; }
		Session.MarkGraphDirty(FromNode->GetGraph());

		// ControlRig: use URigVMController::BreakLink
		{
			sol::object CRResult;
			if (DisconnectFrom_HandleControlRig(Session, FromNode, FFromHandle, FFromPin, ToNode, FToHandle, FToPin, S, CRResult))
				return CRResult;
		}

		// Blueprint: find pins and break specific link
		UEdGraphPin* SrcPin = FindPinOnNodeAny(FromNode, FFromPin, EGPD_Output);
		if (!SrcPin) SrcPin = FindPinOnNodeAny(FromNode, FFromPin); // fallback to any direction
		UEdGraphPin* DstPin = FindPinOnNodeAny(ToNode, FToPin, EGPD_Input);
		if (!DstPin) DstPin = FindPinOnNodeAny(ToNode, FToPin);

		if (!SrcPin) { Session.Log(FString::Printf(TEXT("[FAIL] disconnect_from -> pin \"%s\" not found on source. Available: %s"), *FFromPin, *ListPins(FromNode))); return sol::lua_nil; }
		if (!DstPin) { Session.Log(FString::Printf(TEXT("[FAIL] disconnect_from -> pin \"%s\" not found on target. Available: %s"), *FToPin, *ListPins(ToNode))); return sol::lua_nil; }

		if (!SrcPin->LinkedTo.Contains(DstPin))
		{
			Session.Log(TEXT("[FAIL] disconnect_from -> pins are not connected"));
			return sol::lua_nil;
		}

		SrcPin->BreakLinkTo(DstPin);
		FromNode->PinConnectionListChanged(SrcPin);
		ToNode->PinConnectionListChanged(DstPin);
		Session.Log(FString::Printf(TEXT("[OK] disconnect_from(%s:%s -> %s:%s)"), *FFromHandle, *FFromPin, *FToHandle, *FToPin));
		return sol::make_object(S, true);
	});

	// ---- split_pin(handle, pin_name) ----
	Lua.set_function("split_pin", [&Session](const std::string& Handle, const std::string& PinName, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		FString FPinName = UTF8_TO_TCHAR(PinName.c_str());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] split_pin -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }
		Session.MarkGraphDirty(Node->GetGraph());

		// ControlRig: use set_pin_expansion
		{
			sol::object CRResult;
			if (SplitPin_HandleControlRig(Session, Node, FHandle, FPinName, S, CRResult))
				return CRResult;
		}

		UEdGraphPin* Pin = FindPinOnNodeAny(Node, FPinName);
		if (!Pin) { Session.Log(FString::Printf(TEXT("[FAIL] split_pin -> pin \"%s\" not found. Available: %s"), *FPinName, *ListPins(Node))); return sol::lua_nil; }

		if (Pin->SubPins.Num() > 0) { Session.Log(FString::Printf(TEXT("[FAIL] split_pin -> pin \"%s\" is already split"), *FPinName)); return sol::lua_nil; }

		UEdGraph* PinGraph = Node->GetGraph();
		if (!PinGraph) { Session.Log(TEXT("[FAIL] split_pin -> node has no owning graph")); return sol::lua_nil; }
		const UEdGraphSchema* Schema = PinGraph->GetSchema();
		if (!Schema) { Session.Log(TEXT("[FAIL] split_pin -> no graph schema")); return sol::lua_nil; }

		const UEdGraphSchema_K2* K2Schema = Cast<const UEdGraphSchema_K2>(Schema);
		if (K2Schema && !K2Schema->PinHasSplittableStructType(Pin))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] split_pin -> pin \"%s\" cannot be split (not a splittable struct type)"), *FPinName));
			return sol::lua_nil;
		}

		Schema->SplitPin(Pin, true);

		if (Pin->SubPins.Num() == 0)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] split_pin -> schema did not split \"%s\""), *FPinName));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();
		int32 Idx = 1;
		for (UEdGraphPin* SubPin : Pin->SubPins)
		{
			// Return the display name (e.g. "New Location X") not the internal PinName ("New Location_X")
			FString DisplayName = SubPin->GetDisplayName().ToString();
			if (DisplayName.IsEmpty()) DisplayName = SubPin->PinName.ToString();
			Result[Idx++] = std::string(TCHAR_TO_UTF8(*DisplayName));
		}
		Session.Log(FString::Printf(TEXT("[OK] split_pin(%s, \"%s\") -> %d sub-pins"), *FHandle, *FPinName, Pin->SubPins.Num()));
		return Result;
	});

	// ---- recombine_pin(handle, pin_name) ----
	Lua.set_function("recombine_pin", [&Session](const std::string& Handle, const std::string& PinName, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		FString FPinName = UTF8_TO_TCHAR(PinName.c_str());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] recombine_pin -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }
		Session.MarkGraphDirty(Node->GetGraph());

		// ControlRig: collapse pin
		{
			sol::object CRResult;
			if (RecombinePin_HandleControlRig(Session, Node, FHandle, FPinName, S, CRResult))
				return CRResult;
		}

		// Search ALL pins including hidden (split parent pins are hidden) by PinName, display name, case-insensitive
		UEdGraphPin* Pin = nullptr;
		FString PinNameLower = FPinName.ToLower();
		for (UEdGraphPin* P : Node->Pins)
		{
			if (!P) continue;
			if (P->PinName.ToString().Equals(FPinName, ESearchCase::IgnoreCase)
				|| P->GetDisplayName().ToString().Equals(FPinName, ESearchCase::IgnoreCase)
				|| NeoLuaPin::GetUsablePinName(P).Equals(FPinName, ESearchCase::IgnoreCase))
			{
				Pin = P;
				break;
			}
		}
		if (!Pin) { Session.Log(FString::Printf(TEXT("[FAIL] recombine_pin -> pin \"%s\" not found"), *FPinName)); return sol::lua_nil; }

		// Navigate to parent if this is a sub-pin
		UEdGraphPin* ParentPin = Pin->ParentPin ? Pin->ParentPin : Pin;

		if (ParentPin->SubPins.Num() == 0) { Session.Log(FString::Printf(TEXT("[FAIL] recombine_pin -> pin \"%s\" is not split"), *FPinName)); return sol::lua_nil; }

		UEdGraph* RecombGraph = Node->GetGraph();
		if (!RecombGraph) { Session.Log(TEXT("[FAIL] recombine_pin -> node has no owning graph")); return sol::lua_nil; }
		const UEdGraphSchema* Schema = RecombGraph->GetSchema();
		if (!Schema) { Session.Log(TEXT("[FAIL] recombine_pin -> no graph schema")); return sol::lua_nil; }

		Schema->RecombinePin(ParentPin);

		if (ParentPin->SubPins.Num() > 0)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] recombine_pin -> schema did not recombine \"%s\""), *FPinName));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] recombine_pin(%s, \"%s\")"), *FHandle, *ParentPin->PinName.ToString()));
		return sol::make_object(S, true);
	});

	// ---- add_exec_pin(handle) ----
	Lua.set_function("add_exec_pin", [&Session](const std::string& Handle, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] add_exec_pin -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }
		Session.MarkGraphDirty(Node->GetGraph());

		// ControlRig: not applicable (use add_array_pin instead)
		if (GraphOps_IsRigVMNode(Node))
		{
			Session.Log(TEXT("[FAIL] add_exec_pin -> not supported for ControlRig nodes (use cr_add_array_pin)"));
			return sol::lua_nil;
		}

		// Switch nodes (Switch on String, etc.) use AddPinToSwitchNode() instead of IK2Node_AddPinInterface
		UK2Node_Switch* SwitchNode = Cast<UK2Node_Switch>(Node);
		if (SwitchNode)
		{
			int32 Before = Node->Pins.Num();
			SwitchNode->AddPinToSwitchNode();

			if (Node->Pins.Num() > Before)
			{
				UEdGraphPin* NewPin = Node->Pins.Last();
				FString NewPinName = NewPin->PinName.ToString();
				Session.Log(FString::Printf(TEXT("[OK] add_exec_pin(%s) -> added switch case \"%s\""), *FHandle, *NewPinName));
				return sol::make_object(S, std::string(TCHAR_TO_UTF8(*NewPinName)));
			}
			Session.Log(FString::Printf(TEXT("[FAIL] add_exec_pin -> switch node did not add a pin"), *FHandle));
			return sol::lua_nil;
		}

		IK2Node_AddPinInterface* AddPinNode = Cast<IK2Node_AddPinInterface>(Node);
		if (!AddPinNode)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] add_exec_pin -> node \"%s\" does not support dynamic pins"), *FHandle));
			return sol::lua_nil;
		}

		if (!AddPinNode->CanAddPin())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] add_exec_pin -> node \"%s\" is at maximum pins"), *FHandle));
			return sol::lua_nil;
		}

		int32 Before = Node->Pins.Num();
		AddPinNode->AddInputPin();

		if (Node->Pins.Num() > Before)
		{
			UEdGraphPin* NewPin = Node->Pins.Last();
			FString NewPinName = NewPin->PinName.ToString();
			Session.Log(FString::Printf(TEXT("[OK] add_exec_pin(%s) -> added \"%s\""), *FHandle, *NewPinName));
			return sol::make_object(S, std::string(TCHAR_TO_UTF8(*NewPinName)));
		}

		Session.Log(FString::Printf(TEXT("[FAIL] add_exec_pin -> failed to add pin to \"%s\""), *FHandle));
		return sol::lua_nil;
	});

	// ---- remove_exec_pin(handle) ----
	Lua.set_function("remove_exec_pin", [&Session](const std::string& Handle, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] remove_exec_pin -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }
		Session.MarkGraphDirty(Node->GetGraph());

		if (GraphOps_IsRigVMNode(Node))
		{
			Session.Log(TEXT("[FAIL] remove_exec_pin -> not supported for ControlRig nodes (use cr_remove_array_pin)"));
			return sol::lua_nil;
		}

		IK2Node_AddPinInterface* AddPinNode = Cast<IK2Node_AddPinInterface>(Node);
		if (!AddPinNode)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] remove_exec_pin -> node \"%s\" does not support dynamic pins"), *FHandle));
			return sol::lua_nil;
		}

		// Find last removable pin
		UEdGraphPin* PinToRemove = nullptr;
		for (int32 i = Node->Pins.Num() - 1; i >= 0; i--)
		{
			if (AddPinNode->CanRemovePin(Node->Pins[i]))
			{
				PinToRemove = Node->Pins[i];
				break;
			}
		}

		if (!PinToRemove)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] remove_exec_pin -> no removable pins on \"%s\""), *FHandle));
			return sol::lua_nil;
		}

		FString RemovedName = PinToRemove->PinName.ToString();
		AddPinNode->RemoveInputPin(PinToRemove);

		Session.Log(FString::Printf(TEXT("[OK] remove_exec_pin(%s) -> removed \"%s\""), *FHandle, *RemovedName));
		return sol::make_object(S, true);
	});

	// ---- layout_nodes(handles, options?) ----
	Lua.set_function("layout_nodes", [&Session](sol::table Handles, sol::optional<sol::table> Options, sol::this_state S) -> sol::object
	{
		TArray<UEdGraphNode*> Nodes;
		for (auto& kv : Handles)
		{
			auto HandleOpt = kv.second.as<sol::optional<std::string>>();
			if (!HandleOpt) continue;
			FString FHandle = UTF8_TO_TCHAR(HandleOpt->c_str());
			UEdGraphNode* Node = Session.FindNode(FHandle);
			if (Node) Nodes.Add(Node);
		}

		if (Nodes.Num() == 0) { Session.Log(TEXT("[FAIL] layout_nodes -> no valid nodes")); return sol::lua_nil; }

		int32 StartX = 0, StartY = 0, SpacingX = 300, SpacingY = 200, Columns = Nodes.Num();

		if (Options)
		{
			auto sx = Options->get<sol::optional<int32>>("start_x");
			auto sy = Options->get<sol::optional<int32>>("start_y");
			auto spx = Options->get<sol::optional<int32>>("spacing_x");
			auto spy = Options->get<sol::optional<int32>>("spacing_y");
			auto cols = Options->get<sol::optional<int32>>("columns");
			if (sx) StartX = *sx;
			if (sy) StartY = *sy;
			if (spx) SpacingX = *spx;
			if (spy) SpacingY = *spy;
			if (cols) Columns = FMath::Max(1, *cols);
		}

		for (int32 i = 0; i < Nodes.Num(); i++)
		{
			int32 Row = i / Columns;
			int32 Col = i % Columns;
			Nodes[i]->Modify();
			Nodes[i]->NodePosX = StartX + Col * SpacingX;
			Nodes[i]->NodePosY = StartY + Row * SpacingY;
		}

		Session.Log(FString::Printf(TEXT("[OK] layout_nodes(%d nodes, cols=%d, spacing=%dx%d)"), Nodes.Num(), Columns, SpacingX, SpacingY));
		return sol::make_object(S, true);
	});

	// ---- convert_pin_type(handle, pin_name, type?) ----
	Lua.set_function("convert_pin_type", [&Session](const std::string& Handle, const std::string& PinName,
		sol::optional<std::string> TargetTypeOpt, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		FString FPinName = UTF8_TO_TCHAR(PinName.c_str());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] convert_pin_type -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }
		Session.MarkGraphDirty(Node->GetGraph());

		UK2Node_PromotableOperator* PromotableNode = Cast<UK2Node_PromotableOperator>(Node);
		if (!PromotableNode)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] convert_pin_type -> node \"%s\" is not a promotable operator"), *FHandle));
			return sol::lua_nil;
		}

		UEdGraphPin* TargetPin = FindPinOnNodeAny(Node, FPinName, EGPD_Input);
		if (!TargetPin) TargetPin = FindPinOnNodeAny(Node, FPinName, EGPD_Output);
		if (!TargetPin) { Session.Log(FString::Printf(TEXT("[FAIL] convert_pin_type -> pin \"%s\" not found"), *FPinName)); return sol::lua_nil; }

		if (TargetPin->ParentPin) TargetPin = TargetPin->ParentPin;

		if (!PromotableNode->CanConvertPinType(TargetPin))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] convert_pin_type -> pin \"%s\" cannot be converted"), *FPinName));
			return sol::lua_nil;
		}

		const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();

		// Gather available types
		TArray<UFunction*> AvailableFunctions;
		FTypePromotion::GetAllFuncsForOp(PromotableNode->GetOperationName(), AvailableFunctions);

		TArray<FEdGraphPinType> PossibleTypes;
		for (const UFunction* Func : AvailableFunctions)
		{
			for (TFieldIterator<FProperty> PropIt(Func); PropIt && (PropIt->PropertyFlags & CPF_Parm); ++PropIt)
			{
				FEdGraphPinType ParamType;
				if (K2Schema->ConvertPropertyToPinType(*PropIt, ParamType))
				{
					PossibleTypes.AddUnique(ParamType);
				}
			}
		}

		if (!TargetTypeOpt)
		{
			// List available types
			TArray<FString> TypeNames;
			for (const FEdGraphPinType& PinType : PossibleTypes)
			{
				TypeNames.Add(K2Schema->TypeToText(PinType).ToString());
			}
			TypeNames.Add(TEXT("Wildcard"));
			FString CurrentType = K2Schema->TypeToText(TargetPin->PinType).ToString();
			FString Result = FString::Printf(TEXT("Current: %s. Available: %s"), *CurrentType, *FString::Join(TypeNames, TEXT(", ")));
			Session.Log(FString::Printf(TEXT("[OK] convert_pin_type(%s, \"%s\") -> %s"), *FHandle, *FPinName, *Result));
			return sol::make_object(S, std::string(TCHAR_TO_UTF8(*Result)));
		}

		FString TargetType = UTF8_TO_TCHAR(TargetTypeOpt->c_str());

		// Wildcard reset
		if (TargetType.Equals(TEXT("Wildcard"), ESearchCase::IgnoreCase) || TargetType.Equals(TEXT("Reset"), ESearchCase::IgnoreCase))
		{
			FEdGraphPinType WildcardType;
			WildcardType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
			PromotableNode->ConvertPinType(TargetPin, WildcardType);
			Session.Log(FString::Printf(TEXT("[OK] convert_pin_type(%s, \"%s\") -> Wildcard"), *FHandle, *FPinName));
			return sol::make_object(S, std::string("Wildcard"));
		}

		// Match type
		FString LowerTarget = TargetType.ToLower();
		const FEdGraphPinType* MatchedType = nullptr;
		for (const FEdGraphPinType& PinType : PossibleTypes)
		{
			FString TypeText = K2Schema->TypeToText(PinType).ToString();
			if (TypeText.Equals(TargetType, ESearchCase::IgnoreCase))
			{
				MatchedType = &PinType;
				break;
			}
		}
		if (!MatchedType)
		{
			for (const FEdGraphPinType& PinType : PossibleTypes)
			{
				FString TypeText = K2Schema->TypeToText(PinType).ToString().ToLower();
				if (TypeText.Contains(LowerTarget) || LowerTarget.Contains(TypeText))
				{
					MatchedType = &PinType;
					break;
				}
			}
		}

		if (!MatchedType)
		{
			TArray<FString> TypeNames;
			for (const FEdGraphPinType& PinType : PossibleTypes)
			{
				TypeNames.Add(K2Schema->TypeToText(PinType).ToString());
			}
			Session.Log(FString::Printf(TEXT("[FAIL] convert_pin_type -> type \"%s\" not found. Available: %s"), *TargetType, *FString::Join(TypeNames, TEXT(", "))));
			return sol::lua_nil;
		}

		FString TypeName = K2Schema->TypeToText(*MatchedType).ToString();
		PromotableNode->ConvertPinType(TargetPin, *MatchedType);
		Session.Log(FString::Printf(TEXT("[OK] convert_pin_type(%s, \"%s\") -> %s"), *FHandle, *FPinName, *TypeName));
		return sol::make_object(S, std::string(TCHAR_TO_UTF8(*TypeName)));
	});

	// ---- set_node_comment(handle, text, visible?) ----
	Lua.set_function("set_node_comment", [&Session](const std::string& Handle, const std::string& Text,
		sol::optional<bool> Visible, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		FString FText = UTF8_TO_TCHAR(Text.c_str());
		bool bVisible = Visible.value_or(true);

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] set_node_comment -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }
		Session.MarkGraphDirty(Node->GetGraph());

		// ControlRig: use SetCommentTextByName
		{
			sol::object CRResult;
			if (SetNodeComment_HandleControlRig(Session, Node, FHandle, FText, bVisible, S, CRResult))
				return CRResult;
		}

		Node->Modify();
		Node->NodeComment = FText;
		Node->bCommentBubbleVisible = bVisible && !FText.IsEmpty();
		Node->bCommentBubblePinned = bVisible && !FText.IsEmpty();

		Session.Log(FString::Printf(TEXT("[OK] set_node_comment(%s, \"%s\", visible=%s)"), *FHandle, *FText.Left(40), bVisible ? TEXT("true") : TEXT("false")));
		return sol::make_object(S, true);
	});

	// ---- set_node_enabled(handle, state) ----
	Lua.set_function("set_node_enabled", [&Session](const std::string& Handle, const std::string& State, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		FString FState = UTF8_TO_TCHAR(State.c_str());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] set_node_enabled -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }
		Session.MarkGraphDirty(Node->GetGraph());

		ENodeEnabledState NewState;
		if (FState.Equals(TEXT("enabled"), ESearchCase::IgnoreCase))
			NewState = ENodeEnabledState::Enabled;
		else if (FState.Equals(TEXT("disabled"), ESearchCase::IgnoreCase))
			NewState = ENodeEnabledState::Disabled;
		else if (FState.Equals(TEXT("development_only"), ESearchCase::IgnoreCase) || FState.Equals(TEXT("dev"), ESearchCase::IgnoreCase))
			NewState = ENodeEnabledState::DevelopmentOnly;
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] set_node_enabled -> unknown state '%s'. Valid: enabled, disabled, development_only"), *FState));
			return sol::lua_nil;
		}

		Node->Modify();
		Node->SetEnabledState(NewState, /*bUserAction=*/ true);
		Node->GetGraph()->NotifyGraphChanged();

		Session.Log(FString::Printf(TEXT("[OK] set_node_enabled(%s, \"%s\")"), *FHandle, *FState));
		return sol::make_object(S, true);
	});

	// ---- distribute_nodes(handles, axis) ----
	Lua.set_function("distribute_nodes", [&Session](sol::table Handles, const std::string& Axis, sol::this_state S) -> sol::object
	{
		FString FAxis = UTF8_TO_TCHAR(Axis.c_str());
		bool bAxisX = FAxis.Equals(TEXT("x"), ESearchCase::IgnoreCase);
		if (!bAxisX && !FAxis.Equals(TEXT("y"), ESearchCase::IgnoreCase))
		{
			Session.Log(TEXT("[FAIL] distribute_nodes -> axis must be 'x' or 'y'"));
			return sol::lua_nil;
		}

		TArray<UEdGraphNode*> Nodes;
		for (auto& kv : Handles)
		{
			auto HandleOpt = kv.second.as<sol::optional<std::string>>();
			if (!HandleOpt) continue;
			FString FHandle = UTF8_TO_TCHAR(HandleOpt->c_str());
			UEdGraphNode* Node = Session.FindNode(FHandle);
			if (Node) Nodes.Add(Node);
		}

		if (Nodes.Num() < 3) { Session.Log(TEXT("[FAIL] distribute_nodes -> need at least 3 nodes")); return sol::lua_nil; }

		// Sort by position on the axis
		Nodes.Sort([bAxisX](const UEdGraphNode& A, const UEdGraphNode& B) {
			return bAxisX ? (A.NodePosX < B.NodePosX) : (A.NodePosY < B.NodePosY);
		});

		int32 MinPos = bAxisX ? Nodes[0]->NodePosX : Nodes[0]->NodePosY;
		int32 MaxPos = bAxisX ? Nodes.Last()->NodePosX : Nodes.Last()->NodePosY;
		float Spacing = (float)(MaxPos - MinPos) / (float)(Nodes.Num() - 1);

		for (int32 i = 1; i < Nodes.Num() - 1; i++)
		{
			Nodes[i]->Modify();
			int32 NewPos = MinPos + FMath::RoundToInt(Spacing * i);
			if (bAxisX) Nodes[i]->NodePosX = NewPos;
			else Nodes[i]->NodePosY = NewPos;
		}

		Session.Log(FString::Printf(TEXT("[OK] distribute_nodes(%d nodes, axis=%s)"), Nodes.Num(), *FAxis));
		return sol::make_object(S, true);
	});

	// ---- reset_pin(handle, pin_name) ----
	Lua.set_function("reset_pin", [&Session](const std::string& Handle, const std::string& PinName, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		FString FPinName = UTF8_TO_TCHAR(PinName.c_str());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] reset_pin -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }
		Session.MarkGraphDirty(Node->GetGraph());

		if (GraphOps_IsRigVMNode(Node))
		{
			Session.Log(TEXT("[FAIL] reset_pin -> not supported for ControlRig nodes"));
			return sol::lua_nil;
		}

		UEdGraphPin* Pin = FindPinOnNodeAny(Node, FPinName, EGPD_Input);
		if (!Pin) { Session.Log(FString::Printf(TEXT("[FAIL] reset_pin -> input pin \"%s\" not found"), *FPinName)); return sol::lua_nil; }

		UEdGraph* ResetGraph = Node->GetGraph();
		if (!ResetGraph) { Session.Log(TEXT("[FAIL] reset_pin -> node has no owning graph")); return sol::lua_nil; }
		const UEdGraphSchema* Schema = ResetGraph->GetSchema();
		if (!Schema) { Session.Log(TEXT("[FAIL] reset_pin -> no graph schema")); return sol::lua_nil; }

		Schema->ResetPinToAutogeneratedDefaultValue(Pin, true);

		Session.Log(FString::Printf(TEXT("[OK] reset_pin(%s, \"%s\") -> \"%s\""), *FHandle, *FPinName, *Pin->GetDefaultAsString()));
		return sol::make_object(S, true);
	});

	// ---- set_breakpoint(handle, enabled?) ----
	Lua.set_function("set_breakpoint", [&Session](const std::string& Handle, sol::optional<bool> Enabled, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		bool bEnabled = Enabled.value_or(true);

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] set_breakpoint -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		UBlueprint* BP = FBlueprintEditorUtils::FindBlueprintForNode(Node);
		if (!BP) { Session.Log(TEXT("[FAIL] set_breakpoint -> could not find owning Blueprint")); return sol::lua_nil; }

		FBlueprintBreakpoint* Existing = FKismetDebugUtilities::FindBreakpointForNode(Node, BP, false);
		if (Existing)
		{
			FKismetDebugUtilities::SetBreakpointEnabled(*Existing, bEnabled);
		}
		else
		{
			FKismetDebugUtilities::CreateBreakpoint(BP, Node, bEnabled);
		}

		Session.Log(FString::Printf(TEXT("[OK] set_breakpoint(%s, enabled=%s)"), *FHandle, bEnabled ? TEXT("true") : TEXT("false")));
		return sol::make_object(S, true);
	});

	// ---- clear_breakpoint(handle) ----
	Lua.set_function("clear_breakpoint", [&Session](const std::string& Handle, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] clear_breakpoint -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		UBlueprint* BP = FBlueprintEditorUtils::FindBlueprintForNode(Node);
		if (!BP) { Session.Log(TEXT("[FAIL] clear_breakpoint -> could not find owning Blueprint")); return sol::lua_nil; }

		FKismetDebugUtilities::RemoveBreakpointFromNode(Node, BP);

		Session.Log(FString::Printf(TEXT("[OK] clear_breakpoint(%s)"), *FHandle));
		return sol::make_object(S, true);
	});

	// ---- toggle_breakpoint(handle) ----
	Lua.set_function("toggle_breakpoint", [&Session](const std::string& Handle, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] toggle_breakpoint -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		UBlueprint* BP = FBlueprintEditorUtils::FindBlueprintForNode(Node);
		if (!BP) { Session.Log(TEXT("[FAIL] toggle_breakpoint -> could not find owning Blueprint")); return sol::lua_nil; }

		FBlueprintBreakpoint* Existing = FKismetDebugUtilities::FindBreakpointForNode(Node, BP, false);
		if (Existing)
		{
			FKismetDebugUtilities::SetBreakpointEnabled(*Existing, !Existing->IsEnabled());
			Session.Log(FString::Printf(TEXT("[OK] toggle_breakpoint(%s) -> %s"), *FHandle, Existing->IsEnabled() ? TEXT("enabled") : TEXT("disabled")));
		}
		else
		{
			FKismetDebugUtilities::CreateBreakpoint(BP, Node, true);
			Session.Log(FString::Printf(TEXT("[OK] toggle_breakpoint(%s) -> enabled (created)"), *FHandle));
		}

		return sol::make_object(S, true);
	});

	// ---- add_reroute_node(graph_handle?, x?, y?) ----
	Lua.set_function("add_reroute_node", [&Session](sol::optional<std::string> GraphHandle,
		sol::optional<double> X, sol::optional<double> Y, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		// Find a graph from the handle or fallback to first loaded graph
		UEdGraph* Graph = nullptr;
		if (GraphHandle)
		{
			FString FHandle = UTF8_TO_TCHAR(GraphHandle->c_str());
			UEdGraphNode* RefNode = Session.FindNode(FHandle);
			if (RefNode)
			{
				Graph = RefNode->GetGraph();
			}
		}

		if (!Graph && Session.LoadedGraphs.Num() > 0)
		{
			Graph = *Session.LoadedGraphs.begin();
		}

		if (!Graph)
		{
			Session.Log(TEXT("[FAIL] add_reroute_node -> no graph found. Pass a node handle or read a graph first."));
			return sol::lua_nil;
		}

		// ControlRig graphs don't use K2Node_Knot
		if (GraphOps_IsRigVMGraph(Graph))
		{
			Session.Log(TEXT("[FAIL] add_reroute_node -> not supported for ControlRig graphs. Use add_node with a reroute unit instead."));
			return sol::lua_nil;
		}

		// Only K2 (Blueprint) graphs support UK2Node_Knot
		if (!Graph->GetSchema() || !Cast<const UEdGraphSchema_K2>(Graph->GetSchema()))
		{
			Session.Log(TEXT("[FAIL] add_reroute_node -> graph is not a Blueprint graph (K2 schema required)"));
			return sol::lua_nil;
		}

		double PosX = X.value_or(0.0);
		double PosY = Y.value_or(0.0);

		const FScopedTransaction Transaction(NSLOCTEXT("AgentIntegrationKit", "AddRerouteNode", "Add Reroute Node"));
		Graph->Modify();

		UK2Node_Knot* KnotNode = NewObject<UK2Node_Knot>(Graph);
		if (!KnotNode)
		{
			Session.Log(TEXT("[FAIL] add_reroute_node -> failed to create UK2Node_Knot"));
			return sol::lua_nil;
		}

		KnotNode->SetFlags(RF_Transactional);
		Graph->AddNode(KnotNode, /*bFromUI=*/ false, /*bSelectNewNode=*/ false);
		KnotNode->CreateNewGuid();
		KnotNode->PostPlacedNewNode();
		KnotNode->AllocateDefaultPins();
		KnotNode->NodePosX = FMath::RoundToInt(PosX);
		KnotNode->NodePosY = FMath::RoundToInt(PosY);

		FString NodeGuid = KnotNode->NodeGuid.ToString();
		Session.Nodes.Add(NodeGuid, KnotNode);
		Session.MarkGraphDirty(Graph);

		Session.Log(FString::Printf(TEXT("[OK] add_reroute_node -> placed at (%.0f, %.0f) handle=%s"), PosX, PosY, *NodeGuid));

		sol::table T = LuaView.create_table();
		T["handle"] = TCHAR_TO_UTF8(*NodeGuid);
		T["name"] = "Reroute";

		sol::table InPins = LuaView.create_table();
		sol::table OutPins = LuaView.create_table();
		int32 InIdx = 1, OutIdx = 1;
		for (UEdGraphPin* Pin : KnotNode->Pins)
		{
			if (!Pin || Pin->bHidden) continue;
			sol::table PinInfo = NeoLuaPin::BuildPinTable(LuaView, Pin);
			if (Pin->Direction == EGPD_Input)
				InPins[InIdx++] = PinInfo;
			else
				OutPins[OutIdx++] = PinInfo;
		}
		T["pins_in"] = InPins;
		T["pins_out"] = OutPins;

		return T;
	});

	// ---- delete_nodes(handles) ----
	Lua.set_function("delete_nodes", [&Session](sol::table Handles, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		// Collect valid nodes first
		TArray<TPair<FString, UEdGraphNode*>> NodesToDelete;
		for (auto& kv : Handles)
		{
			auto HandleOpt = kv.second.as<sol::optional<std::string>>();
			if (!HandleOpt) continue;
			FString FHandle = UTF8_TO_TCHAR(HandleOpt->c_str());
			UEdGraphNode* Node = Session.FindNode(FHandle);
			if (Node)
			{
				NodesToDelete.Add(TPair<FString, UEdGraphNode*>(FHandle, Node));
			}
		}

		if (NodesToDelete.Num() == 0)
		{
			Session.Log(TEXT("[FAIL] delete_nodes -> no valid nodes found in handles array"));
			return sol::lua_nil;
		}

		int32 Deleted = 0;
		int32 Failed = 0;

		const FScopedTransaction Transaction(NSLOCTEXT("AgentIntegrationKit", "DeleteNodes", "Delete Nodes"));

		for (const auto& Pair : NodesToDelete)
		{
			const FString& FHandle = Pair.Key;
			UEdGraphNode* Node = Pair.Value;

			FString NodeName = Node->GetNodeTitle(ENodeTitleType::MenuTitle).ToString();
			UEdGraph* Graph = Node->GetGraph(); // Cache before deletion (Node is destroyed after)

			// Guard against undeletable nodes (function entry/result, tunnel endpoints, etc.)
			if (!Node->CanUserDeleteNode())
			{
				Session.Log(FString::Printf(TEXT("[WARN] delete_nodes -> \"%s\" cannot be deleted (entry/result/tunnel node)"), *FHandle));
				Failed++;
				continue;
			}

			bool bSuccess = false;

			if (!DeleteNodes_HandleControlRig(Session, Node, FHandle, bSuccess))
			{
				bSuccess = NeoBlueprint::DeleteNode(Node);
				if (!bSuccess)
				{
					Session.Log(FString::Printf(TEXT("[WARN] delete_nodes -> failed to delete \"%s\""), *FHandle));
				}
			}

			if (bSuccess)
			{
				Session.MarkGraphDirty(Graph); // Mark dirty only after successful deletion
				Session.Nodes.Remove(FHandle);
				Deleted++;
			}
			else
			{
				Failed++;
			}
		}

		Session.Log(FString::Printf(TEXT("[OK] delete_nodes -> deleted %d, failed %d"), Deleted, Failed));

		sol::table Result = LuaView.create_table();
		Result["deleted"] = Deleted;
		Result["failed"] = Failed;
		return Result;
	});

	// NOTE: collapse_to_function and collapse_to_macro removed — FBlueprintEditor::CollapseSelectionToFunction/Macro
	// and CanCollapseSelectionToFunction/Macro are all protected members with no public alternative.
	// The agent can achieve similar results by manually creating a function/macro graph and moving nodes.

	// ---- promote_pin_to_variable(handle, pin_name) ----
	Lua.set_function("promote_pin_to_variable", [&Session](const std::string& Handle, const std::string& PinName, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		FString FPinName = UTF8_TO_TCHAR(PinName.c_str());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] promote_pin_to_variable -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		if (GraphOps_IsRigVMNode(Node))
		{
			Session.Log(TEXT("[FAIL] promote_pin_to_variable -> not supported for ControlRig nodes (use cr_promote_pin)"));
			return sol::lua_nil;
		}

		UEdGraph* GraphObj = Node->GetGraph();
		if (!GraphObj) { Session.Log(TEXT("[FAIL] promote_pin_to_variable -> node has no owning graph")); return sol::lua_nil; }

		UBlueprint* BP = FBlueprintEditorUtils::FindBlueprintForGraph(GraphObj);
		if (!BP) { Session.Log(TEXT("[FAIL] promote_pin_to_variable -> could not find owning Blueprint")); return sol::lua_nil; }

		UEdGraphPin* TargetPin = FindPinOnNodeAny(Node, FPinName);
		if (!TargetPin) { Session.Log(FString::Printf(TEXT("[FAIL] promote_pin_to_variable -> pin \"%s\" not found. Available: %s"), *FPinName, *ListPins(Node))); return sol::lua_nil; }

		const FScopedTransaction Transaction(NSLOCTEXT("AgentIntegrationKit", "PromotePinToVariable", "Promote Pin to Variable"));
		BP->Modify();
		GraphObj->Modify();

		// Build variable type from pin (strip const/ref/weak qualifiers)
		FEdGraphPinType NewPinType = TargetPin->PinType;
		NewPinType.bIsConst = false;
		NewPinType.bIsReference = false;
		NewPinType.bIsWeakPointer = false;

		// Generate variable name from pin display name
		FName VarName;
		if (const UEdGraphSchema* Schema = TargetPin->GetSchema())
		{
			FString IdealVarName = FText::TrimPrecedingAndTrailing(Schema->GetPinDisplayName(TargetPin)).ToString();
			if (IdealVarName == TEXT("Return Value")) IdealVarName.Empty();
			if (const UK2Node* K2Node = Cast<UK2Node>(Node))
			{
				if (K2Node->ShouldDrawCompact()) IdealVarName.Empty();
			}
			if (!IdealVarName.IsEmpty())
			{
				VarName = FBlueprintEditorUtils::FindUniqueKismetName(BP, IdealVarName);
			}
		}
		if (VarName == NAME_None)
		{
			VarName = FBlueprintEditorUtils::FindUniqueKismetName(BP, TEXT("NewVar"));
		}

		bool bWasSuccessful = FBlueprintEditorUtils::AddMemberVariable(BP, VarName, NewPinType, TargetPin->GetDefaultAsString());
		if (!bWasSuccessful)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] promote_pin_to_variable -> AddMemberVariable failed for \"%s\""), *VarName.ToString()));
			return sol::lua_nil;
		}

		// Pin may have been reconstructed after adding variable — re-find it
		TargetPin = Node->FindPin(TargetPin->PinName);
		if (!TargetPin) { Session.Log(TEXT("[FAIL] promote_pin_to_variable -> pin lost after variable creation")); return sol::lua_nil; }

		// Create get or set node depending on pin direction
		FEdGraphSchemaAction_K2NewNode NodeInfo;
		UK2Node_Variable* TemplateNode = nullptr;
		if (TargetPin->Direction == EGPD_Input)
		{
			TemplateNode = NewObject<UK2Node_VariableGet>();
		}
		else
		{
			TemplateNode = NewObject<UK2Node_VariableSet>();
		}
		TemplateNode->VariableReference.SetSelfMember(VarName);
		NodeInfo.NodeTemplate = TemplateNode;

		// Ensure the skeleton class has the new property available before creating the node.
		// Without this, array/container-type variables may not be resolvable yet, causing
		// AllocateDefaultPins → CreatePinForVariable → GetPropertyForVariable to fail
		// and produce a ghost node with zero pins.
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

		// Position near the source node
		float NewX = (TargetPin->Direction == EGPD_Input) ? Node->NodePosX - 200.0f : Node->NodePosX + 400.0f;
		float NewY = static_cast<float>(Node->NodePosY);
		UEdGraphNode* NewNode = PerformActionAtPosition(NodeInfo, GraphObj, TargetPin, NewX, NewY);
		if (!NewNode)
		{
			Session.Log(TEXT("[FAIL] promote_pin_to_variable -> failed to create variable get/set node"));
			return sol::lua_nil;
		}

		// Validate the node has pins — for array/container types, the initial
		// AllocateDefaultPins may fail if the property wasn't ready. Reconstruct once.
		bool bHasOutputPins = false;
		for (UEdGraphPin* Pin : NewNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output) { bHasOutputPins = true; break; }
		}
		if (!bHasOutputPins && TargetPin->Direction == EGPD_Input)
		{
			// Get node should have at least one output pin — try reconstructing
			NewNode->ReconstructNode();
			for (UEdGraphPin* Pin : NewNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Output) { bHasOutputPins = true; break; }
			}
			if (!bHasOutputPins)
			{
				// Still broken — remove the ghost node to prevent editor crashes
				NeoBlueprint::DeleteNode(NewNode);
				Session.Log(TEXT("[FAIL] promote_pin_to_variable -> created node has no pins (array/container type not resolved). Remove and re-add the variable manually."));
				return sol::lua_nil;
			}
		}

		// Register the new node
		FString NewNodeGuid = NewNode->NodeGuid.ToString();
		Session.Nodes.Add(NewNodeGuid, NewNode);
		Session.MarkGraphDirty(GraphObj);

		Session.Log(FString::Printf(TEXT("[OK] promote_pin_to_variable(%s, \"%s\") -> var \"%s\", node handle=%s"),
			*FHandle, *FPinName, *VarName.ToString(), *NewNodeGuid));

		sol::table Result = LuaView.create_table();
		Result["var_name"] = TCHAR_TO_UTF8(*VarName.ToString());
		Result["node_handle"] = TCHAR_TO_UTF8(*NewNodeGuid);
		return Result;
	});

	// ---- promote_pin_to_local_variable(handle, pin_name) ----
	Lua.set_function("promote_pin_to_local_variable", [&Session](const std::string& Handle, const std::string& PinName, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		FString FPinName = UTF8_TO_TCHAR(PinName.c_str());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] promote_pin_to_local_variable -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		if (GraphOps_IsRigVMNode(Node))
		{
			Session.Log(TEXT("[FAIL] promote_pin_to_local_variable -> not supported for ControlRig nodes"));
			return sol::lua_nil;
		}

		UEdGraph* GraphObj = Node->GetGraph();
		if (!GraphObj) { Session.Log(TEXT("[FAIL] promote_pin_to_local_variable -> node has no owning graph")); return sol::lua_nil; }

		if (!FBlueprintEditorUtils::DoesSupportLocalVariables(GraphObj))
		{
			Session.Log(TEXT("[FAIL] promote_pin_to_local_variable -> graph does not support local variables (must be a function graph)"));
			return sol::lua_nil;
		}

		UBlueprint* BP = FBlueprintEditorUtils::FindBlueprintForGraph(GraphObj);
		if (!BP) { Session.Log(TEXT("[FAIL] promote_pin_to_local_variable -> could not find owning Blueprint")); return sol::lua_nil; }

		UEdGraphPin* TargetPin = FindPinOnNodeAny(Node, FPinName);
		if (!TargetPin) { Session.Log(FString::Printf(TEXT("[FAIL] promote_pin_to_local_variable -> pin \"%s\" not found. Available: %s"), *FPinName, *ListPins(Node))); return sol::lua_nil; }

		const FScopedTransaction Transaction(NSLOCTEXT("AgentIntegrationKit", "PromotePinToLocalVar", "Promote Pin to Local Variable"));
		BP->Modify();
		GraphObj->Modify();

		// Build variable type from pin
		FEdGraphPinType NewPinType = TargetPin->PinType;
		NewPinType.bIsConst = false;
		NewPinType.bIsReference = false;
		NewPinType.bIsWeakPointer = false;

		FName VarName = FBlueprintEditorUtils::FindUniqueKismetName(BP, TEXT("NewLocalVar"));
		UEdGraph* FunctionGraph = FBlueprintEditorUtils::GetTopLevelGraph(GraphObj);

		bool bWasSuccessful = FBlueprintEditorUtils::AddLocalVariable(BP, FunctionGraph, VarName, NewPinType, TargetPin->GetDefaultAsString());
		if (!bWasSuccessful)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] promote_pin_to_local_variable -> AddLocalVariable failed for \"%s\""), *VarName.ToString()));
			return sol::lua_nil;
		}

		// Pin may have been reconstructed — re-find it
		TargetPin = Node->FindPin(TargetPin->PinName);
		if (!TargetPin) { Session.Log(TEXT("[FAIL] promote_pin_to_local_variable -> pin lost after variable creation")); return sol::lua_nil; }

		// Create get or set node
		FEdGraphSchemaAction_K2NewNode NodeInfo;
		UK2Node_Variable* TemplateNode = nullptr;
		if (TargetPin->Direction == EGPD_Input)
		{
			TemplateNode = NewObject<UK2Node_VariableGet>();
		}
		else
		{
			TemplateNode = NewObject<UK2Node_VariableSet>();
		}

		FGuid LocalVarGuid = FBlueprintEditorUtils::FindLocalVariableGuidByName(BP, FunctionGraph, VarName);
		TemplateNode->VariableReference.SetLocalMember(VarName, FunctionGraph->GetName(), LocalVarGuid);
		NodeInfo.NodeTemplate = TemplateNode;

		// Position near the source node
		float NewX = (TargetPin->Direction == EGPD_Input) ? Node->NodePosX - 200.0f : Node->NodePosX + 400.0f;
		float NewY = static_cast<float>(Node->NodePosY);
		UEdGraphNode* NewNode = PerformActionAtPosition(NodeInfo, GraphObj, TargetPin, NewX, NewY);
		if (!NewNode)
		{
			Session.Log(TEXT("[FAIL] promote_pin_to_local_variable -> failed to create variable get/set node"));
			return sol::lua_nil;
		}

		// Register the new node
		FString NewNodeGuid = NewNode->NodeGuid.ToString();
		Session.Nodes.Add(NewNodeGuid, NewNode);
		Session.MarkGraphDirty(GraphObj);

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		Session.Log(FString::Printf(TEXT("[OK] promote_pin_to_local_variable(%s, \"%s\") -> local var \"%s\", node handle=%s"),
			*FHandle, *FPinName, *VarName.ToString(), *NewNodeGuid));

		sol::table Result = LuaView.create_table();
		Result["var_name"] = TCHAR_TO_UTF8(*VarName.ToString());
		Result["node_handle"] = TCHAR_TO_UTF8(*NewNodeGuid);
		return Result;
	});

	// ---- move_node(handle, x, y) ----
	Lua.set_function("move_node", [&Session](const std::string& Handle, double X, double Y, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] move_node -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }
		Session.MarkGraphDirty(Node->GetGraph());

		Node->Modify();
		Node->NodePosX = FMath::RoundToInt(X);
		Node->NodePosY = FMath::RoundToInt(Y);

		Session.Log(FString::Printf(TEXT("[OK] move_node(%s, %d, %d)"), *FHandle, Node->NodePosX, Node->NodePosY));
		return sol::make_object(S, true);
	});

	// ---- duplicate_nodes(handles, offset_x?, offset_y?) ----
	Lua.set_function("duplicate_nodes", [&Session](sol::table Handles, sol::optional<double> OffsetX, sol::optional<double> OffsetY, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		// Collect valid nodes, all must be in the same graph
		TSet<UObject*> NodesToExport;
		UEdGraph* SharedGraph = nullptr;
		TArray<FString> OriginalHandles;

		for (auto& kv : Handles)
		{
			auto HandleOpt = kv.second.as<sol::optional<std::string>>();
			if (!HandleOpt) continue;
			FString FHandle = UTF8_TO_TCHAR(HandleOpt->c_str());
			UEdGraphNode* Node = Session.FindNode(FHandle);
			if (!Node) continue;

			UEdGraph* NodeGraph = Node->GetGraph();
			if (!NodeGraph) continue;

			if (!SharedGraph)
			{
				SharedGraph = NodeGraph;
			}
			else if (SharedGraph != NodeGraph)
			{
				Session.Log(TEXT("[FAIL] duplicate_nodes -> all nodes must be in the same graph"));
				return sol::lua_nil;
			}

			NodesToExport.Add(Node);
			OriginalHandles.Add(FHandle);
		}

		if (NodesToExport.Num() == 0) { Session.Log(TEXT("[FAIL] duplicate_nodes -> no valid nodes found")); return sol::lua_nil; }

		// Record original node GUIDs before export — we'll collect all existing GUIDs
		// in the graph so we can identify which nodes are new after import
		TSet<FGuid> PreExistingGuids;
		for (UEdGraphNode* N : SharedGraph->Nodes)
		{
			if (N) PreExistingGuids.Add(N->NodeGuid);
		}

		// Export nodes to text
		FString ExportedText;
		FEdGraphUtilities::ExportNodesToText(NodesToExport, ExportedText);

		if (ExportedText.IsEmpty()) { Session.Log(TEXT("[FAIL] duplicate_nodes -> export produced empty text")); return sol::lua_nil; }

		// Import into the same graph
		const FScopedTransaction Transaction(NSLOCTEXT("AgentIntegrationKit", "DuplicateNodes", "Duplicate Nodes"));
		SharedGraph->Modify();

		TSet<UEdGraphNode*> ImportedNodeSet;
		FEdGraphUtilities::ImportNodesFromText(SharedGraph, ExportedText, ImportedNodeSet);

		if (ImportedNodeSet.Num() == 0) { Session.Log(TEXT("[FAIL] duplicate_nodes -> import produced no nodes")); return sol::lua_nil; }

		// Post-process (fixes up pin references, etc.)
		FEdGraphUtilities::PostProcessPastedNodes(ImportedNodeSet);

		// Apply offset to duplicated nodes
		int32 DX = FMath::RoundToInt(OffsetX.value_or(50.0));
		int32 DY = FMath::RoundToInt(OffsetY.value_or(50.0));

		sol::table Result = LuaView.create_table();

		// Build originals list from the user's handle order (deterministic)
		struct FOriginalInfo { FString Handle; FString ClassName; int32 PosX; int32 PosY; bool bMatched = false; };
		TArray<FOriginalInfo> Originals;
		for (const FString& Handle : OriginalHandles)
		{
			UEdGraphNode* N = Session.FindNode(Handle);
			if (N)
			{
				FOriginalInfo Info;
				Info.Handle = Handle;
				Info.ClassName = N->GetClass()->GetName();
				Info.PosX = N->NodePosX;
				Info.PosY = N->NodePosY;
				Originals.Add(Info);
			}
		}

		// Match imported nodes to originals by class name + closest position
		for (UEdGraphNode* NewNode : ImportedNodeSet)
		{
			if (!NewNode) continue;

			NewNode->NodePosX += DX;
			NewNode->NodePosY += DY;

			FString NewGuid = NewNode->NodeGuid.ToString();
			Session.Nodes.Add(NewGuid, NewNode);

			// Find matching original by class + position
			FString NewClassName = NewNode->GetClass()->GetName();
			int32 BestIdx = -1;
			int32 BestDist = TNumericLimits<int32>::Max();
			for (int32 i = 0; i < Originals.Num(); i++)
			{
				if (Originals[i].bMatched) continue;
				if (Originals[i].ClassName != NewClassName) continue;
				// Imported nodes are at original positions (before our offset)
				int32 Dist = FMath::Abs((NewNode->NodePosX - DX) - Originals[i].PosX) +
				             FMath::Abs((NewNode->NodePosY - DY) - Originals[i].PosY);
				if (Dist < BestDist)
				{
					BestDist = Dist;
					BestIdx = i;
				}
			}

			if (BestIdx >= 0)
			{
				Originals[BestIdx].bMatched = true;
				Result[TCHAR_TO_UTF8(*Originals[BestIdx].Handle)] = TCHAR_TO_UTF8(*NewGuid);
			}
		}

		Session.MarkGraphDirty(SharedGraph);
		Session.Log(FString::Printf(TEXT("[OK] duplicate_nodes -> duplicated %d nodes"), ImportedNodeSet.Num()));
		return Result;
	});
});
