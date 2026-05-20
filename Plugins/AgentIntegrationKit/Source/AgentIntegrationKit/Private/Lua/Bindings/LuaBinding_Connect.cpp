#include "Lua/LuaBindingRegistry.h"
#include "Modules/ModuleManager.h"
#if WITH_CONTROLRIG
#include "Lua/LuaControlRigHelper.h"
#endif
#include "Lua/LuaPinHelper.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

static UEdGraphPin* FindPinOnNode(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction = EGPD_MAX)
{
	if (!Node) return nullptr;

	// Resolve common exec pin aliases before searching
	// "execute" / "exec" → first input exec pin, "then" / "output" → first output exec pin
	FString PinLower = PinName.ToLower();
	if (PinLower == TEXT("execute") || PinLower == TEXT("exec"))
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && Pin->Direction == EGPD_Input)
				return Pin;
		}
	}
	else if (PinLower == TEXT("then") || PinLower == TEXT("output exec"))
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && Pin->Direction == EGPD_Output)
				return Pin;
		}
	}

	// Exact match by PinName
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->bHidden) continue;
		if (Pin->PinName.ToString() == PinName)
		{
			if (Direction == EGPD_MAX || Pin->Direction == Direction)
				return Pin;
		}
	}

	// Try display name
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->bHidden) continue;
		FString Display = Pin->GetDisplayName().ToString();
		if (Display == PinName)
		{
			if (Direction == EGPD_MAX || Pin->Direction == Direction)
				return Pin;
		}
	}

	// Try generated usable name (handles "in_0", "out_0" fallbacks for blank pin names)
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->bHidden) continue;
		FString UsableName = NeoLuaPin::GetUsablePinName(Pin);
		if (UsableName == PinName)
		{
			if (Direction == EGPD_MAX || Pin->Direction == Direction)
				return Pin;
		}
	}

	// Case-insensitive fallback (all name sources)
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->bHidden) continue;
		if (Pin->PinName.ToString().ToLower() == PinLower
			|| Pin->GetDisplayName().ToString().ToLower() == PinLower
			|| NeoLuaPin::GetUsablePinName(Pin).ToLower() == PinLower)
		{
			if (Direction == EGPD_MAX || Pin->Direction == Direction)
				return Pin;
		}
	}

	return nullptr;
}

static FString ListAvailablePins(UEdGraphNode* Node, EEdGraphPinDirection Direction = EGPD_MAX)
{
	FString List;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->bHidden) continue;
		if (Direction != EGPD_MAX && Pin->Direction != Direction) continue;

		if (List.Len() > 0) List += TEXT(", ");
		FString Name = NeoLuaPin::GetUsablePinName(Pin);
		FString Dir = Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out");
		List += FString::Printf(TEXT("%s (%s)"), *Name, *Dir);
	}
	return List;
}

// Helper functions extracted outside REGISTER_LUA_BINDING to avoid #if inside macro arguments (MSVC C5101)
#if WITH_CONTROLRIG
// Returns true if handled (success or failure), false if not a RigVM node (fall through to Blueprint path)
static bool Connect_HandleControlRig(FLuaSessionData& Session, UEdGraphNode* FromNode, const FString& FFromHandle, const FString& FFromPin,
	UEdGraphNode* ToNode, const FString& FToHandle, const FString& FToPin, sol::this_state S, sol::object& OutResult)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("ControlRig"))) return false;
	if (!LuaControlRig::IsRigVMNode(FromNode)) return false;

	UEdGraph* Graph = FromNode->GetGraph();
	FString Error;
	bool bSuccess = LuaControlRig::Connect(Graph, FromNode, FFromPin, ToNode, FToPin, Error);
	if (bSuccess)
	{
		Session.Log(FString::Printf(TEXT("[OK] connect(%s:%s -> %s:%s) -> via RigVM controller"),
			*FFromHandle, *FFromPin, *FToHandle, *FToPin));
		OutResult = sol::make_object(S, true);
		return true;
	}
	Session.Log(FString::Printf(TEXT("[FAIL] connect(%s:%s -> %s:%s) -> %s"),
		*FFromHandle, *FFromPin, *FToHandle, *FToPin, *Error));
	OutResult = sol::object(sol::lua_nil);
	return true;
}

// Returns true if handled, false if not a RigVM node
static bool Disconnect_HandleControlRig(FLuaSessionData& Session, UEdGraphNode* Node, const FString& FHandle, const FString& FPinName, sol::this_state S, sol::object& OutResult)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("ControlRig"))) return false;
	if (!LuaControlRig::IsRigVMNode(Node)) return false;

	UEdGraph* Graph = Node->GetGraph();
	FString Error;
	bool bOk = LuaControlRig::Disconnect(Graph, Node, FPinName, Error);
	if (bOk)
	{
		Session.Log(FString::Printf(TEXT("[OK] disconnect(%s:%s) -> all connections broken via RigVM controller"), *FHandle, *FPinName));
		OutResult = sol::make_object(S, true);
	}
	else
	{
		Session.Log(FString::Printf(TEXT("[FAIL] disconnect(%s:%s) -> %s"), *FHandle, *FPinName, *Error));
		OutResult = sol::object(sol::lua_nil);
	}
	return true;
}

// Returns true if handled, false if not a RigVM node
static bool DisconnectAll_HandleControlRig(FLuaSessionData& Session, UEdGraphNode* Node, const FString& FHandle, sol::this_state S, sol::object& OutResult)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("ControlRig"))) return false;
	if (!LuaControlRig::IsRigVMNode(Node)) return false;

	UEdGraph* Graph = Node->GetGraph();
	URigVMController* Controller = LuaControlRig::GetController(Graph);
	if (!Controller)
	{
		Session.Log(FString::Printf(TEXT("[FAIL] disconnect_all(%s) -> no RigVM controller"), *FHandle));
		OutResult = sol::object(sol::lua_nil);
		return true;
	}

	URigVMNode* ModelNode = LuaControlRig::GetModelNode(Node);
	if (!ModelNode)
	{
		Session.Log(FString::Printf(TEXT("[FAIL] disconnect_all(%s) -> could not resolve RigVM model node"), *FHandle));
		OutResult = sol::object(sol::lua_nil);
		return true;
	}

	Controller->OpenUndoBracket(TEXT("Disconnect All Links"));
	for (URigVMPin* Pin : ModelNode->GetPins())
	{
		if (!Pin) continue;
		FString PinPath = Pin->GetPinPath();
		Controller->BreakAllLinks(PinPath, true, false);
		Controller->BreakAllLinks(PinPath, false, false);
	}
	Controller->CloseUndoBracket();

	Session.Log(FString::Printf(TEXT("[OK] disconnect_all(%s) -> all connections broken via RigVM controller"), *FHandle));
	OutResult = sol::make_object(S, true);
	return true;
}
#else
static bool Connect_HandleControlRig(FLuaSessionData&, UEdGraphNode*, const FString&, const FString&,
	UEdGraphNode*, const FString&, const FString&, sol::this_state, sol::object&) { return false; }
static bool Disconnect_HandleControlRig(FLuaSessionData&, UEdGraphNode*, const FString&, const FString&, sol::this_state, sol::object&) { return false; }
static bool DisconnectAll_HandleControlRig(FLuaSessionData&, UEdGraphNode*, const FString&, sol::this_state, sol::object&) { return false; }
#endif

static TArray<FLuaFunctionDoc> ConnectDocs = {
	{
		TEXT("connect(from_handle, from_pin, to_handle, to_pin)"),
		TEXT("Connect two node pins. Uses node handles from add_node. Pin names are case-insensitive. The engine will auto-create conversion nodes if needed."),
		TEXT("true on success, nil on failure")
	},
	{
		TEXT("disconnect(handle, pin_name)"),
		TEXT("Break all connections on a pin. For specific disconnect use disconnect_from(from_handle, from_pin, to_handle, to_pin)."),
		TEXT("true on success, nil on failure")
	},
	{
		TEXT("disconnect_all(handle)"),
		TEXT("Break all connections on all pins of a node at once."),
		TEXT("true on success, nil on failure")
	}
};

REGISTER_LUA_BINDING(Connect, ConnectDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("connect", [&Session](sol::object FromHandleObj, sol::object FromPinObj,
		sol::object ToHandleObj, sol::object ToPinObj, sol::this_state S) -> sol::object
	{
		if (!FromHandleObj.is<std::string>() || !FromPinObj.is<std::string>() ||
			!ToHandleObj.is<std::string>() || !ToPinObj.is<std::string>())
		{
			Session.Log(TEXT("[FAIL] connect(from_node, from_pin, to_node, to_pin) -> all 4 arguments must be strings (got nil — check that add_node/find_nodes returned valid handles)"));
			return sol::lua_nil;
		}
		FString FFromHandle = UTF8_TO_TCHAR(FromHandleObj.as<std::string>().c_str());
		FString FToHandle = UTF8_TO_TCHAR(ToHandleObj.as<std::string>().c_str());
		FString FFromPin = UTF8_TO_TCHAR(FromPinObj.as<std::string>().c_str());
		FString FToPin = UTF8_TO_TCHAR(ToPinObj.as<std::string>().c_str());

		UEdGraphNode* FromNode = Session.FindNode(FFromHandle);
		UEdGraphNode* ToNode = Session.FindNode(FToHandle);

		if (!FromNode)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] connect -> source node \"%s\" not found. Call read_graph() or add_node() first."), *FFromHandle));
			return sol::lua_nil;
		}
		if (!ToNode)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] connect -> target node \"%s\" not found. Call read_graph() or add_node() first."), *FToHandle));
			return sol::lua_nil;
		}

		// Validate both nodes belong to the same graph — cross-graph connections corrupt
		// blueprint state (pins get different outers, causing ensure failures and crash loops
		// during compile/save).
		UEdGraph* FromGraph = FromNode->GetGraph();
		UEdGraph* ToGraph = ToNode->GetGraph();
		if (!FromGraph || !ToGraph)
		{
			Session.Log(TEXT("[FAIL] connect -> one or both nodes have no owning graph (possibly orphaned in /Engine/Transient)"));
			return sol::lua_nil;
		}
		if (FromGraph != ToGraph)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] connect(%s -> %s) -> nodes belong to different graphs (\"%s\" vs \"%s\"). Cannot connect across graphs."),
				*FFromHandle, *FToHandle,
				*FromGraph->GetName(), *ToGraph->GetName()));
			return sol::lua_nil;
		}

		Session.MarkGraphDirty(FromGraph);

		// ControlRig/RigVM: use URigVMController::AddLink
		{
			sol::object CRResult;
			if (Connect_HandleControlRig(Session, FromNode, FFromHandle, FFromPin, ToNode, FToHandle, FToPin, S, CRResult))
				return CRResult;
		}

		UEdGraphPin* OutPin = FindPinOnNode(FromNode, FFromPin, EGPD_Output);
		if (!OutPin)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] connect -> pin \"%s\" not found on source node. Available output pins: %s"),
				*FFromPin, *ListAvailablePins(FromNode, EGPD_Output)));
			return sol::lua_nil;
		}

		UEdGraphPin* InPin = FindPinOnNode(ToNode, FToPin, EGPD_Input);
		if (!InPin)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] connect -> pin \"%s\" not found on target node. Available input pins: %s"),
				*FToPin, *ListAvailablePins(ToNode, EGPD_Input)));
			return sol::lua_nil;
		}

		// FromGraph already validated above
		const UEdGraphSchema* Schema = FromGraph->GetSchema();
		if (!Schema)
		{
			Session.Log(TEXT("[FAIL] connect -> could not get graph schema"));
			return sol::lua_nil;
		}

		// Check compatibility first for a better error message
		FPinConnectionResponse Response = Schema->CanCreateConnection(OutPin, InPin);
		if (Response.Response == CONNECT_RESPONSE_DISALLOW)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] connect(%s:%s -> %s:%s) -> %s"),
				*FFromHandle, *FFromPin, *FToHandle, *FToPin, *Response.Message.ToString()));
			return sol::lua_nil;
		}

		// TryCreateConnection handles auto-conversion nodes and calls PinConnectionListChanged
		bool bSuccess = Schema->TryCreateConnection(OutPin, InPin);
		if (bSuccess)
		{
			// Reconstruct nodes with wildcard pins so types propagate correctly
			// (e.g. ForEachLoop's Array Element pin infers type from connected array)
			auto ReconstructIfHasWildcard = [](UEdGraphNode* Node)
			{
				if (!Node) return;
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
					{
						Node->ReconstructNode();
						return;
					}
				}
			};
			ReconstructIfHasWildcard(FromNode);
			ReconstructIfHasWildcard(ToNode);

			FString Method = (Response.Response == CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE)
				? TEXT("with conversion") : TEXT("direct");
			Session.Log(FString::Printf(TEXT("[OK] connect(%s:%s -> %s:%s) -> %s"),
				*FFromHandle, *FFromPin, *FToHandle, *FToPin, *Method));
			return sol::make_object(S, true);
		}

		Session.Log(FString::Printf(TEXT("[FAIL] connect(%s:%s -> %s:%s) -> connection failed"),
			*FFromHandle, *FFromPin, *FToHandle, *FToPin));
		return sol::lua_nil;
	});

	Lua.set_function("disconnect", [&Session](sol::object HandleObj, sol::object PinNameObj, sol::this_state S) -> sol::object
	{
		if (!HandleObj.is<std::string>() || !PinNameObj.is<std::string>())
		{
			Session.Log(TEXT("[FAIL] disconnect(node_handle, pin_name) -> both arguments must be strings (got nil)"));
			return sol::lua_nil;
		}
		FString FHandle = UTF8_TO_TCHAR(HandleObj.as<std::string>().c_str());
		FString FPinName = UTF8_TO_TCHAR(PinNameObj.as<std::string>().c_str());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] disconnect -> node \"%s\" not found. Call read_graph() or add_node() first."), *FHandle));
			return sol::lua_nil;
		}

		Session.MarkGraphDirty(Node->GetGraph());

		// ControlRig/RigVM: use URigVMController::BreakAllLinks
		{
			sol::object CRResult;
			if (Disconnect_HandleControlRig(Session, Node, FHandle, FPinName, S, CRResult))
				return CRResult;
		}

		UEdGraphPin* Pin = FindPinOnNode(Node, FPinName);
		if (!Pin)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] disconnect -> pin \"%s\" not found. Available: %s"),
				*FPinName, *ListAvailablePins(Node)));
			return sol::lua_nil;
		}

		Pin->BreakAllPinLinks(true);
		// BreakAllPinLinks notifies linked-to nodes but NOT the owning node
		Node->PinConnectionListChanged(Pin);
		Session.Log(FString::Printf(TEXT("[OK] disconnect(%s:%s) -> all connections broken"), *FHandle, *FPinName));
		return sol::make_object(S, true);
	});

	Lua.set_function("disconnect_all", [&Session](sol::object HandleObj, sol::this_state S) -> sol::object
	{
		if (!HandleObj.is<std::string>())
		{
			Session.Log(TEXT("[FAIL] disconnect_all(node_handle) -> argument must be a string (got nil)"));
			return sol::lua_nil;
		}
		FString FHandle = UTF8_TO_TCHAR(HandleObj.as<std::string>().c_str());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] disconnect_all -> node \"%s\" not found. Call read_graph() or add_node() first."), *FHandle));
			return sol::lua_nil;
		}

		UEdGraph* Graph = Node->GetGraph();
		if (!Graph)
		{
			Session.Log(TEXT("[FAIL] disconnect_all -> node has no owning graph"));
			return sol::lua_nil;
		}

		Session.MarkGraphDirty(Graph);

		// ControlRig/RigVM: iterate model pins
		{
			sol::object CRResult;
			if (DisconnectAll_HandleControlRig(Session, Node, FHandle, S, CRResult))
				return CRResult;
		}

		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (!Schema)
		{
			Session.Log(TEXT("[FAIL] disconnect_all -> could not get graph schema"));
			return sol::lua_nil;
		}

		// BreakNodeLinks breaks all pins and sends NodeConnectionListChanged to all affected nodes
		Schema->BreakNodeLinks(*Node);
		Session.Log(FString::Printf(TEXT("[OK] disconnect_all(%s) -> all connections broken"), *FHandle));
		return sol::make_object(S, true);
	});
});
