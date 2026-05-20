#include "Lua/LuaBindingRegistry.h"
#include "Modules/ModuleManager.h"
#if WITH_CONTROLRIG
#include "Lua/LuaControlRigHelper.h"
#endif
#include "Blueprint/BlueprintUtils.h"
#include "K2Node_Event.h"
#include "MaterialGraph/MaterialGraph.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// Helper function extracted outside REGISTER_LUA_BINDING to avoid #if inside macro arguments (MSVC C5101)
#if WITH_CONTROLRIG
// Returns true if handled (success or failure), false if not a RigVM node
static bool DeleteNode_HandleControlRig(FLuaSessionData& Session, UEdGraphNode* Node, const FString& FHandle,
	const FString& NodeName, sol::this_state S, sol::object& OutResult)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("ControlRig"))) return false;
	if (!LuaControlRig::IsRigVMNode(Node)) return false;

	UEdGraph* Graph = Node->GetGraph();
	FString Error;
	if (!LuaControlRig::DeleteNode(Graph, Node, Error))
	{
		Session.Log(FString::Printf(TEXT("[FAIL] delete_node(\"%s\") -> %s"), *FHandle, *Error));
		OutResult = sol::object(sol::lua_nil);
		return true;
	}
	Session.Nodes.Remove(FHandle);
	Session.Log(FString::Printf(TEXT("[OK] delete_node(\"%s\") -> removed \"%s\" via RigVM controller"), *FHandle, *NodeName));
	OutResult = sol::make_object(S, true);
	return true;
}
#else
static bool DeleteNode_HandleControlRig(FLuaSessionData& Session, UEdGraphNode* Node, const FString& FHandle,
	const FString& NodeName, sol::this_state S, sol::object& OutResult)
{
	// Runtime RigVM detection without ControlRig headers
	if (!Node || !Node->GetClass()->GetName().Contains(TEXT("RigVM"))) return false;

	// Without ControlRig module linkage, we cannot use URigVMController for proper deletion.
	// Return an error rather than crash with K2 deletion on a RigVM node.
	Session.Log(FString::Printf(TEXT("[FAIL] delete_node(\"%s\") -> \"%s\" is a ControlRig/RigVM node. "
		"Ensure the AIK_ControlRig extension module is loaded."), *FHandle, *NodeName));
	OutResult = sol::object(sol::lua_nil);
	return true;
}
#endif

static TArray<FLuaFunctionDoc> DeleteNodeDocs = {
	{
		TEXT("delete_node(handle)"),
		TEXT("Remove a node from its graph. Refuses undeletable nodes (entry/result/tunnel). Also available as graph:delete_node(handle)."),
		TEXT("true on success, nil on failure")
	}
};

REGISTER_LUA_BINDING(DeleteNode, DeleteNodeDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("delete_node", [&Session](const std::string& Handle, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] delete_node -> node \"%s\" not found"), *FHandle));
			return sol::lua_nil;
		}

		FString NodeName = Node->GetNodeTitle(ENodeTitleType::MenuTitle).ToString();
		UEdGraph* Graph = Node->GetGraph(); // Cache before any deletion (Node is destroyed after)

		// Guard against orphaned nodes (e.g. in /Engine/Transient with no blueprint owner).
		// These arise from cross-graph corruption or stale state. Calling BreakNodeLinks or
		// FindBlueprintForNodeChecked on them triggers a fatal assert crash loop.
		if (!Graph)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] delete_node(\"%s\") -> \"%s\" has no owning graph (orphaned node)"), *FHandle, *NodeName));
			Session.Nodes.Remove(FHandle); // Clean up stale handle so subsequent calls don't hit the same node
			return sol::lua_nil;
		}
		// Reject truly orphaned nodes in the transient package, but allow Material Editor
		// preview graphs — those legitimately live in the transient package (the Material Editor
		// works on a transient duplicate of the original material).
		if (Graph->GetOutermost() == GetTransientPackage())
		{
			bool bIsMaterialEditorGraph = Graph->IsA<UMaterialGraph>();
			if (!bIsMaterialEditorGraph)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] delete_node(\"%s\") -> \"%s\" is in /Engine/Transient (orphaned). Removing stale handle."), *FHandle, *NodeName));
				Session.Nodes.Remove(FHandle);
				return sol::lua_nil;
			}
		}

		// Guard against undeletable nodes (function entry/result, tunnel endpoints, etc.)
		if (!Node->CanUserDeleteNode())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] delete_node(\"%s\") -> \"%s\" cannot be deleted (entry/result/tunnel node)"), *FHandle, *NodeName));
			return sol::lua_nil;
		}

		// Guard against override event nodes (BeginPlay, Tick, etc.) — these are technically
		// deletable by the engine but should be protected from agent deletion to prevent
		// accidentally breaking blueprints
		if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
		{
			if (EventNode->bOverrideFunction)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] delete_node(\"%s\") -> \"%s\" is an override event node and cannot be deleted. Use override_function() to manage overrides."), *FHandle, *NodeName));
				return sol::lua_nil;
			}
		}

		// ControlRig/RigVM: use URigVMController::RemoveNode
		{
			sol::object CRResult;
			if (DeleteNode_HandleControlRig(Session, Node, FHandle, NodeName, S, CRResult))
			{
				// Mark dirty only after successful deletion
				if (CRResult.is<bool>() && CRResult.as<bool>())
					Session.MarkGraphDirty(Graph);
				return CRResult;
			}
		}

		if (!NeoBlueprint::DeleteNode(Node))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] delete_node(\"%s\") -> removal of \"%s\" failed"), *FHandle, *NodeName));
			return sol::lua_nil;
		}

		// Mark dirty only after successful deletion (Node is destroyed, use cached Graph)
		Session.MarkGraphDirty(Graph);
		Session.Nodes.Remove(FHandle);
		Session.Log(FString::Printf(TEXT("[OK] delete_node(\"%s\") -> removed \"%s\""), *FHandle, *NodeName));
		return sol::make_object(S, true);
	});
});
