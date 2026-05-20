// ControlRig-specific graph operations via URigVMController:
// cr_add_array_pin, cr_insert_array_pin, cr_remove_array_pin, cr_clear_array_pin,
// cr_duplicate_array_pin, cr_set_array_pin_size, cr_bind_pin_variable, cr_promote_pin,
// cr_set_pin_expansion,
// cr_add_exposed_pin, cr_remove_exposed_pin, cr_rename_exposed_pin,
// cr_change_exposed_pin_type, cr_reorder_exposed_pin,
// cr_set_node_category, cr_set_node_keywords, cr_set_node_description,
// cr_set_pin_category, cr_unbind_pin_variable,
// cr_add_local_variable, cr_remove_local_variable, cr_rename_local_variable,
// cr_set_local_variable_type, cr_set_local_variable_default,
// cr_collapse_nodes, cr_expand_node, cr_promote_to_function, cr_promote_to_collapse,
// cr_add_trait, cr_remove_trait

#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaControlRigHelper.h"
#include "Lua/LuaPinHelper.h"
#include "EdGraph/EdGraph.h"
#include "RigVMModel/Nodes/RigVMCollapseNode.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "Modules/ModuleManager.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// Helpers

static URigVMController* GetCtrlForNode(FLuaSessionData& Session, UEdGraphNode* Node, const FString& FuncName)
{
	if (!Node) return nullptr;
	if (!LuaControlRig::IsRigVMNode(Node))
	{
		Session.Log(FString::Printf(TEXT("[FAIL] %s -> node is not a ControlRig/RigVM node"), *FuncName));
		return nullptr;
	}
	URigVMController* Ctrl = LuaControlRig::GetController(Node->GetGraph());
	if (!Ctrl) Session.Log(FString::Printf(TEXT("[FAIL] %s -> could not get URigVMController"), *FuncName));
	return Ctrl;
}

static ERigVMPinDirection ParseRigVMDirection(const FString& Dir)
{
	if (Dir.Equals(TEXT("input"), ESearchCase::IgnoreCase)) return ERigVMPinDirection::Input;
	if (Dir.Equals(TEXT("output"), ESearchCase::IgnoreCase)) return ERigVMPinDirection::Output;
	if (Dir.Equals(TEXT("io"), ESearchCase::IgnoreCase)) return ERigVMPinDirection::IO;
	if (Dir.Equals(TEXT("visible"), ESearchCase::IgnoreCase)) return ERigVMPinDirection::Visible;
	if (Dir.Equals(TEXT("hidden"), ESearchCase::IgnoreCase)) return ERigVMPinDirection::Hidden;
	return ERigVMPinDirection::Input; // default
}

// Docs

static TArray<FLuaFunctionDoc> CRGraphOpsDocs = {
	{ TEXT("cr_add_array_pin(handle, pin_name, default_value?)"), TEXT("Add array element pin"), TEXT("pin_path or nil") },
	{ TEXT("cr_insert_array_pin(handle, pin_name, index, default_value?)"), TEXT("Insert array element at index"), TEXT("pin_path or nil") },
	{ TEXT("cr_remove_array_pin(handle, pin_name)"), TEXT("Remove array element pin"), TEXT("true or nil") },
	{ TEXT("cr_clear_array_pin(handle, pin_name)"), TEXT("Clear all array elements"), TEXT("true or nil") },
	{ TEXT("cr_duplicate_array_pin(handle, pin_name)"), TEXT("Duplicate array element"), TEXT("pin_path or nil") },
	{ TEXT("cr_set_array_pin_size(handle, pin_name, size, default_value?)"), TEXT("Set array pin element count"), TEXT("true or nil") },
	{ TEXT("cr_bind_pin_variable(handle, pin_name, variable)"), TEXT("Bind pin to variable"), TEXT("true or nil") },
	{ TEXT("cr_promote_pin(handle, pin_name, create_node?, x?, y?)"), TEXT("Promote pin to variable"), TEXT("true or nil") },
	{ TEXT("cr_set_pin_expansion(handle, pin_name, expanded)"), TEXT("Expand/collapse struct/array pin"), TEXT("true or nil") },
	{ TEXT("cr_add_exposed_pin(name, direction, cpp_type, ...)"), TEXT("Add exposed pin to CR function graph"), TEXT("pin_name or nil") },
	{ TEXT("cr_remove_exposed_pin(name, graph?)"), TEXT("Remove exposed pin"), TEXT("true or nil") },
	{ TEXT("cr_rename_exposed_pin(name, new_name, graph?)"), TEXT("Rename exposed pin"), TEXT("true or nil") },
	{ TEXT("cr_change_exposed_pin_type(name, cpp_type, ...)"), TEXT("Change exposed pin type"), TEXT("true or nil") },
	{ TEXT("cr_reorder_exposed_pin(name, index, graph?)"), TEXT("Reorder exposed pin"), TEXT("true or nil") },
	{ TEXT("cr_set_node_category(handle, category)"), TEXT("Set CR function node category"), TEXT("true or nil") },
	{ TEXT("cr_set_node_keywords(handle, keywords)"), TEXT("Set CR function node keywords"), TEXT("true or nil") },
	{ TEXT("cr_set_node_description(handle, description)"), TEXT("Set CR function node description"), TEXT("true or nil") },
	{ TEXT("cr_set_pin_category(handle, pin_name, category?)"), TEXT("Set/clear pin category"), TEXT("true or nil") },
	{ TEXT("cr_unbind_pin_variable(handle, pin_name)"), TEXT("Unbind pin from variable"), TEXT("true or nil") },
	{ TEXT("cr_add_local_variable(name, type, default_value?, type_object_path?, graph?)"), TEXT("Add local variable to CR graph"), TEXT("true or nil") },
	{ TEXT("cr_remove_local_variable(name, graph?)"), TEXT("Remove local variable"), TEXT("true or nil") },
	{ TEXT("cr_rename_local_variable(name, new_name, graph?)"), TEXT("Rename local variable"), TEXT("true or nil") },
	{ TEXT("cr_set_local_variable_type(name, cpp_type, type_object_path?, graph?)"), TEXT("Change local variable type"), TEXT("true or nil") },
	{ TEXT("cr_set_local_variable_default(name, default_value, graph?)"), TEXT("Set local variable default value"), TEXT("true or nil") },
	{ TEXT("cr_collapse_nodes(handles, name?)"), TEXT("Collapse selected CR nodes into a collapse node"), TEXT("{node_name, handle} or nil") },
	{ TEXT("cr_expand_node(handle)"), TEXT("Expand a collapsed/library CR node back into its contained nodes"), TEXT("{handles} or nil") },
	{ TEXT("cr_promote_to_function(handle, function_path?)"), TEXT("Promote a collapse node to a function reference node"), TEXT("new_node_name or nil") },
	{ TEXT("cr_promote_to_collapse(handle, remove_definition?)"), TEXT("Demote a function reference node back to a collapse node"), TEXT("new_node_name or nil") },
	{ TEXT("cr_add_trait(handle, trait_type_path, trait_name?, default_value?, pin_index?)"), TEXT("Add a trait to a CR node"), TEXT("trait_name or nil") },
	{ TEXT("cr_remove_trait(handle, trait_name)"), TEXT("Remove a trait from a CR node"), TEXT("true or nil") },
};

static void BindControlRigGraphOps(sol::state& Lua, FLuaSessionData& Session)
{
	// ---- _cr_add_node(graph_ptr_as_int, node_name, x, y) ----
	// Internal bridge: main module's add_node delegates here at runtime for RigVM graphs.
	// Takes a graph pointer (as lightuserdata) since Session doesn't cross module boundaries cleanly.
	Lua.set_function("_cr_add_node", [&Session](sol::lightuserdata_value GraphUD, const std::string& NodeName,
		double X, double Y, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UEdGraph* Graph = reinterpret_cast<UEdGraph*>(GraphUD.value);
		if (!Graph || !LuaControlRig::IsRigVMGraph(Graph))
		{
			Session.Log(TEXT("[FAIL] _cr_add_node -> invalid or non-RigVM graph"));
			return sol::lua_nil;
		}

		FString DisplayName = UTF8_TO_TCHAR(NodeName.c_str());
		FString Error;
		UEdGraphNode* Node = LuaControlRig::AddNodeByName(Graph, DisplayName, X, Y, Error);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] _cr_add_node(\"%s\") -> %s"), *DisplayName, *Error));
			return sol::lua_nil;
		}

		FString NodeGuid = Node->NodeGuid.ToString();
		Session.Nodes.Add(NodeGuid, Node);

		FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::MenuTitle).ToString();
		Session.Log(FString::Printf(TEXT("[OK] add_node(\"%s\") -> placed \"%s\" at (%.0f, %.0f) handle=%s"),
			*DisplayName, *NodeTitle, X, Y, *NodeGuid));

		sol::table T = LuaView.create_table();
		T["handle"] = TCHAR_TO_UTF8(*NodeGuid);
		T["name"] = TCHAR_TO_UTF8(*NodeTitle);

		sol::table InPins = LuaView.create_table();
		sol::table OutPins = LuaView.create_table();
		int32 InIdx = 1, OutIdx = 1;
		for (UEdGraphPin* Pin : Node->Pins)
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

	// ---- cr_add_array_pin(handle, pin_name, default_value?) ----
	Lua.set_function("cr_add_array_pin", [&Session](const std::string& Handle, const std::string& PinName,
		sol::optional<std::string> DefaultValue, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] cr_add_array_pin -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		URigVMController* Ctrl = GetCtrlForNode(Session, Node, TEXT("cr_add_array_pin"));
		if (!Ctrl) return sol::lua_nil;

		FString PinPath = LuaControlRig::BuildPinPath(Node, UTF8_TO_TCHAR(PinName.c_str()));
		FString DefVal = DefaultValue ? UTF8_TO_TCHAR(DefaultValue->c_str()) : FString();

		FString NewPath = Ctrl->AddArrayPin(PinPath, DefVal, false, false);
		if (NewPath.IsEmpty())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_add_array_pin -> AddArrayPin failed for \"%s\""), *PinPath));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_add_array_pin -> %s"), *NewPath));
		return sol::make_object(S, std::string(TCHAR_TO_UTF8(*NewPath)));
	});

	// ---- cr_insert_array_pin(handle, pin_name, index, default_value?) ----
	Lua.set_function("cr_insert_array_pin", [&Session](const std::string& Handle, const std::string& PinName,
		int32 Index, sol::optional<std::string> DefaultValue, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] cr_insert_array_pin -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		URigVMController* Ctrl = GetCtrlForNode(Session, Node, TEXT("cr_insert_array_pin"));
		if (!Ctrl) return sol::lua_nil;

		FString PinPath = LuaControlRig::BuildPinPath(Node, UTF8_TO_TCHAR(PinName.c_str()));
		FString DefVal = DefaultValue ? UTF8_TO_TCHAR(DefaultValue->c_str()) : FString();

		FString NewPath = Ctrl->InsertArrayPin(PinPath, Index, DefVal, false, false);
		if (NewPath.IsEmpty())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_insert_array_pin -> InsertArrayPin failed for \"%s\" at %d"), *PinPath, Index));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_insert_array_pin -> %s"), *NewPath));
		return sol::make_object(S, std::string(TCHAR_TO_UTF8(*NewPath)));
	});

	// ---- cr_remove_array_pin(handle, pin_name) ----
	Lua.set_function("cr_remove_array_pin", [&Session](const std::string& Handle, const std::string& PinName, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] cr_remove_array_pin -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		URigVMController* Ctrl = GetCtrlForNode(Session, Node, TEXT("cr_remove_array_pin"));
		if (!Ctrl) return sol::lua_nil;

		FString PinPath = LuaControlRig::BuildPinPath(Node, UTF8_TO_TCHAR(PinName.c_str()));
		if (!Ctrl->RemoveArrayPin(PinPath, false, false))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_remove_array_pin -> RemoveArrayPin failed for \"%s\""), *PinPath));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_remove_array_pin(%s)"), *PinPath));
		return sol::make_object(S, true);
	});

	// ---- cr_clear_array_pin(handle, pin_name) ----
	Lua.set_function("cr_clear_array_pin", [&Session](const std::string& Handle, const std::string& PinName, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] cr_clear_array_pin -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		URigVMController* Ctrl = GetCtrlForNode(Session, Node, TEXT("cr_clear_array_pin"));
		if (!Ctrl) return sol::lua_nil;

		FString PinPath = LuaControlRig::BuildPinPath(Node, UTF8_TO_TCHAR(PinName.c_str()));
		if (!Ctrl->ClearArrayPin(PinPath, false, false))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_clear_array_pin -> ClearArrayPin failed for \"%s\""), *PinPath));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_clear_array_pin(%s)"), *PinPath));
		return sol::make_object(S, true);
	});

	// ---- cr_duplicate_array_pin(handle, pin_name) ----
	Lua.set_function("cr_duplicate_array_pin", [&Session](const std::string& Handle, const std::string& PinName, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] cr_duplicate_array_pin -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		URigVMController* Ctrl = GetCtrlForNode(Session, Node, TEXT("cr_duplicate_array_pin"));
		if (!Ctrl) return sol::lua_nil;

		FString PinPath = LuaControlRig::BuildPinPath(Node, UTF8_TO_TCHAR(PinName.c_str()));
		FString NewPath = Ctrl->DuplicateArrayPin(PinPath, false, false);
		if (NewPath.IsEmpty())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_duplicate_array_pin -> DuplicateArrayPin failed for \"%s\""), *PinPath));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_duplicate_array_pin -> %s"), *NewPath));
		return sol::make_object(S, std::string(TCHAR_TO_UTF8(*NewPath)));
	});

	// ---- cr_set_array_pin_size(handle, pin_name, size, default_value?) ----
	Lua.set_function("cr_set_array_pin_size", [&Session](const std::string& Handle, const std::string& PinName,
		int32 Size, sol::optional<std::string> DefaultValue, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] cr_set_array_pin_size -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		URigVMController* Ctrl = GetCtrlForNode(Session, Node, TEXT("cr_set_array_pin_size"));
		if (!Ctrl) return sol::lua_nil;

		FString PinPath = LuaControlRig::BuildPinPath(Node, UTF8_TO_TCHAR(PinName.c_str()));
		FString DefVal = DefaultValue ? UTF8_TO_TCHAR(DefaultValue->c_str()) : FString();

		if (!Ctrl->SetArrayPinSize(PinPath, Size, DefVal, false, false))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_set_array_pin_size -> SetArrayPinSize failed for \"%s\" size=%d"), *PinPath, Size));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_set_array_pin_size(%s, %d)"), *PinPath, Size));
		return sol::make_object(S, true);
	});

	// ---- cr_bind_pin_variable(handle, pin_name, variable) ----
	Lua.set_function("cr_bind_pin_variable", [&Session](const std::string& Handle, const std::string& PinName,
		const std::string& Variable, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] cr_bind_pin_variable -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		URigVMController* Ctrl = GetCtrlForNode(Session, Node, TEXT("cr_bind_pin_variable"));
		if (!Ctrl) return sol::lua_nil;

		FString PinPath = LuaControlRig::BuildPinPath(Node, UTF8_TO_TCHAR(PinName.c_str()));
		FString VarPath = UTF8_TO_TCHAR(Variable.c_str());

		if (!Ctrl->BindPinToVariable(PinPath, VarPath, false, false))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_bind_pin_variable -> BindPinToVariable failed: %s -> %s. Variable must be a Blueprint member variable (add_variable), not a hierarchy variable (cr:add('variable'))."), *PinPath, *VarPath));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_bind_pin_variable(%s -> %s)"), *PinPath, *VarPath));
		return sol::make_object(S, true);
	});

	// ---- cr_promote_pin(handle, pin_name, create_variable_node?, x?, y?) ----
	Lua.set_function("cr_promote_pin", [&Session](const std::string& Handle, const std::string& PinName,
		sol::optional<bool> CreateNode, sol::optional<double> X, sol::optional<double> Y, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] cr_promote_pin -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		URigVMController* Ctrl = GetCtrlForNode(Session, Node, TEXT("cr_promote_pin"));
		if (!Ctrl) return sol::lua_nil;

		FString PinPath = LuaControlRig::BuildPinPath(Node, UTF8_TO_TCHAR(PinName.c_str()));
		bool bCreateNode = CreateNode.value_or(false);
		FVector2D Pos(X.value_or(0.0), Y.value_or(0.0));

		if (!Ctrl->PromotePinToVariable(PinPath, bCreateNode, Pos, false, false))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_promote_pin -> PromotePinToVariable failed for \"%s\""), *PinPath));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_promote_pin(%s)"), *PinPath));
		return sol::make_object(S, true);
	});

	// ---- cr_set_pin_expansion(handle, pin_name, expanded) ----
	Lua.set_function("cr_set_pin_expansion", [&Session](const std::string& Handle, const std::string& PinName,
		bool Expanded, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] cr_set_pin_expansion -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		URigVMController* Ctrl = GetCtrlForNode(Session, Node, TEXT("cr_set_pin_expansion"));
		if (!Ctrl) return sol::lua_nil;

		FString PinPath = LuaControlRig::BuildPinPath(Node, UTF8_TO_TCHAR(PinName.c_str()));
		if (!Ctrl->SetPinExpansion(PinPath, Expanded, false, false))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_set_pin_expansion -> failed for \"%s\""), *PinPath));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_set_pin_expansion(%s, %s)"), *PinPath, Expanded ? TEXT("expanded") : TEXT("collapsed")));
		return sol::make_object(S, true);
	});

	// ---- Exposed pin ops use a graph handle to find the controller ----
	// Helper to get controller from a node handle, graph name, OR any loaded CR graph
	auto GetCtrlFromContext = [&Session](sol::optional<std::string> GraphHandle, const FString& FuncName) -> URigVMController*
	{
		if (GraphHandle)
		{
			FString FHandle = UTF8_TO_TCHAR(GraphHandle->c_str());

			// 1. Try as node handle
			UEdGraphNode* Node = Session.FindNode(FHandle);
			if (Node && LuaControlRig::IsRigVMNode(Node))
			{
				return LuaControlRig::GetController(Node->GetGraph());
			}

			// 2. Try as graph name — match against loaded graphs
			for (UEdGraph* G : Session.LoadedGraphs)
			{
				if (LuaControlRig::IsRigVMGraph(G) && G->GetName().Contains(FHandle))
				{
					return LuaControlRig::GetController(G);
				}
			}
		}

		// 3. Fallback: find any RigVM graph in loaded graphs
		for (UEdGraph* G : Session.LoadedGraphs)
		{
			if (LuaControlRig::IsRigVMGraph(G))
			{
				return LuaControlRig::GetController(G);
			}
		}

		Session.Log(FString::Printf(TEXT("[FAIL] %s -> no ControlRig graph found. Read a CR graph or pass a node handle."), *FuncName));
		return nullptr;
	};

	// ---- cr_add_exposed_pin(name, direction, cpp_type, cpp_type_object_path?, default_value?, graph_handle?) ----
	Lua.set_function("cr_add_exposed_pin", [&Session, GetCtrlFromContext](const std::string& Name, const std::string& Direction,
		const std::string& CppType, sol::optional<std::string> CppTypeObjectPath,
		sol::optional<std::string> DefaultValue, sol::optional<std::string> GraphHandle, sol::this_state S) -> sol::object
	{
		// If graph_handle is nil but default_value looks like a graph name (not a numeric/bool value),
		// treat it as the graph handle — common mistake due to many optional params
		sol::optional<std::string> EffectiveGraphHandle = GraphHandle;
		FString EffectiveDefVal;
		if (!GraphHandle.has_value() && DefaultValue.has_value())
		{
			FString DV = UTF8_TO_TCHAR(DefaultValue->c_str());
			bool bLooksLikeGraphName = !DV.IsEmpty() && !DV.IsNumeric()
				&& !DV.Equals(TEXT("true"), ESearchCase::IgnoreCase)
				&& !DV.Equals(TEXT("false"), ESearchCase::IgnoreCase);
			if (bLooksLikeGraphName)
			{
				// Check if it matches any loaded graph
				for (UEdGraph* G : Session.LoadedGraphs)
				{
					if (LuaControlRig::IsRigVMGraph(G) && G->GetName().Contains(DV))
					{
						EffectiveGraphHandle = DefaultValue;
						break;
					}
				}
			}
			if (!EffectiveGraphHandle.has_value())
				EffectiveDefVal = DV;
		}
		else if (DefaultValue.has_value())
		{
			EffectiveDefVal = UTF8_TO_TCHAR(DefaultValue->c_str());
		}

		URigVMController* Ctrl = GetCtrlFromContext(EffectiveGraphHandle, TEXT("cr_add_exposed_pin"));
		if (!Ctrl) return sol::lua_nil;

		// Exposed pins only work on nested/function graphs, not the main Rig graph.
		// Auto-search loaded graphs for a function graph if we landed on top-level.
		URigVMGraph* Graph = Ctrl->GetGraph();
		if (Graph && Graph->IsTopLevelGraph())
		{
			URigVMController* FuncCtrl = nullptr;
			for (UEdGraph* G : Session.LoadedGraphs)
			{
				if (!LuaControlRig::IsRigVMGraph(G)) continue;
				URigVMController* C = LuaControlRig::GetController(G);
				if (C && C->GetGraph() && !C->GetGraph()->IsTopLevelGraph())
				{
					FuncCtrl = C;
					break;
				}
			}
			if (FuncCtrl)
			{
				Ctrl = FuncCtrl;
			}
			else
			{
				Session.Log(TEXT("[FAIL] cr_add_exposed_pin -> no function graph loaded. Read a function graph first: read_graph(path, \"FunctionName\")"));
				return sol::lua_nil;
			}
		}

		ERigVMPinDirection Dir = ParseRigVMDirection(UTF8_TO_TCHAR(Direction.c_str()));
		FString FCppType = UTF8_TO_TCHAR(CppType.c_str());
		FName ObjPath = CppTypeObjectPath ? FName(UTF8_TO_TCHAR(CppTypeObjectPath->c_str())) : NAME_None;

		FName NewName = Ctrl->AddExposedPin(FName(UTF8_TO_TCHAR(Name.c_str())), Dir, FCppType, ObjPath, EffectiveDefVal, false, false);
		if (NewName == NAME_None)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_add_exposed_pin -> failed for \"%s\""), UTF8_TO_TCHAR(Name.c_str())));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_add_exposed_pin(\"%s\")"), *NewName.ToString()));
		return sol::make_object(S, std::string(TCHAR_TO_UTF8(*NewName.ToString())));
	});

	// ---- cr_remove_exposed_pin(name, graph_handle?) ----
	Lua.set_function("cr_remove_exposed_pin", [&Session, GetCtrlFromContext](const std::string& Name,
		sol::optional<std::string> GraphHandle, sol::this_state S) -> sol::object
	{
		URigVMController* Ctrl = GetCtrlFromContext(GraphHandle, TEXT("cr_remove_exposed_pin"));
		if (!Ctrl) return sol::lua_nil;

		if (!Ctrl->RemoveExposedPin(FName(UTF8_TO_TCHAR(Name.c_str())), false, false))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_remove_exposed_pin -> failed for \"%s\""), UTF8_TO_TCHAR(Name.c_str())));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_remove_exposed_pin(\"%s\")"), UTF8_TO_TCHAR(Name.c_str())));
		return sol::make_object(S, true);
	});

	// ---- cr_rename_exposed_pin(name, new_name, graph_handle?) ----
	Lua.set_function("cr_rename_exposed_pin", [&Session, GetCtrlFromContext](const std::string& Name, const std::string& NewName,
		sol::optional<std::string> GraphHandle, sol::this_state S) -> sol::object
	{
		URigVMController* Ctrl = GetCtrlFromContext(GraphHandle, TEXT("cr_rename_exposed_pin"));
		if (!Ctrl) return sol::lua_nil;

		if (!Ctrl->RenameExposedPin(FName(UTF8_TO_TCHAR(Name.c_str())), FName(UTF8_TO_TCHAR(NewName.c_str())), false, false))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_rename_exposed_pin -> failed \"%s\" -> \"%s\""), UTF8_TO_TCHAR(Name.c_str()), UTF8_TO_TCHAR(NewName.c_str())));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_rename_exposed_pin(\"%s\" -> \"%s\")"), UTF8_TO_TCHAR(Name.c_str()), UTF8_TO_TCHAR(NewName.c_str())));
		return sol::make_object(S, true);
	});

	// ---- cr_change_exposed_pin_type(name, cpp_type, cpp_type_object_path?, graph_handle?) ----
	Lua.set_function("cr_change_exposed_pin_type", [&Session, GetCtrlFromContext](const std::string& Name, const std::string& CppType,
		sol::optional<std::string> CppTypeObjectPath, sol::optional<std::string> GraphHandle, sol::this_state S) -> sol::object
	{
		URigVMController* Ctrl = GetCtrlFromContext(GraphHandle, TEXT("cr_change_exposed_pin_type"));
		if (!Ctrl) return sol::lua_nil;

		FString FCppType = UTF8_TO_TCHAR(CppType.c_str());
		FName ObjPath = CppTypeObjectPath ? FName(UTF8_TO_TCHAR(CppTypeObjectPath->c_str())) : NAME_None;

		bool bSetupUndoRedo = false;
		if (!Ctrl->ChangeExposedPinType(FName(UTF8_TO_TCHAR(Name.c_str())), FCppType, ObjPath, bSetupUndoRedo, true, false))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_change_exposed_pin_type -> failed for \"%s\""), UTF8_TO_TCHAR(Name.c_str())));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_change_exposed_pin_type(\"%s\" -> %s)"), UTF8_TO_TCHAR(Name.c_str()), *FCppType));
		return sol::make_object(S, true);
	});

	// ---- cr_reorder_exposed_pin(name, index, graph_handle?) ----
	Lua.set_function("cr_reorder_exposed_pin", [&Session, GetCtrlFromContext](const std::string& Name, int32 Index,
		sol::optional<std::string> GraphHandle, sol::this_state S) -> sol::object
	{
		URigVMController* Ctrl = GetCtrlFromContext(GraphHandle, TEXT("cr_reorder_exposed_pin"));
		if (!Ctrl) return sol::lua_nil;

		if (!Ctrl->SetExposedPinIndex(FName(UTF8_TO_TCHAR(Name.c_str())), Index, false, false))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_reorder_exposed_pin -> failed for \"%s\" at index %d"), UTF8_TO_TCHAR(Name.c_str()), Index));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_reorder_exposed_pin(\"%s\" -> index %d)"), UTF8_TO_TCHAR(Name.c_str()), Index));
		return sol::make_object(S, true);
	});

	// ---- cr_set_node_category(handle, category) ----
	Lua.set_function("cr_set_node_category", [&Session](const std::string& Handle, const std::string& Category, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] cr_set_node_category -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		URigVMController* Ctrl = GetCtrlForNode(Session, Node, TEXT("cr_set_node_category"));
		if (!Ctrl) return sol::lua_nil;

		FName NodeName = LuaControlRig::GetModelNodeName(Node);
		if (!Ctrl->SetNodeCategoryByName(NodeName, UTF8_TO_TCHAR(Category.c_str()), false, false))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_set_node_category -> failed for \"%s\". Only works on collapse nodes, not function references. Use on collapse node before promoting to function."), *FHandle));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_set_node_category(%s, \"%s\")"), *FHandle, UTF8_TO_TCHAR(Category.c_str())));
		return sol::make_object(S, true);
	});

	// ---- cr_set_node_keywords(handle, keywords) ----
	Lua.set_function("cr_set_node_keywords", [&Session](const std::string& Handle, const std::string& Keywords, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] cr_set_node_keywords -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		URigVMController* Ctrl = GetCtrlForNode(Session, Node, TEXT("cr_set_node_keywords"));
		if (!Ctrl) return sol::lua_nil;

		FName NodeName = LuaControlRig::GetModelNodeName(Node);
		if (!Ctrl->SetNodeKeywordsByName(NodeName, UTF8_TO_TCHAR(Keywords.c_str()), false, false))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_set_node_keywords -> failed for \"%s\". Only works on collapse nodes, not function references."), *FHandle));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_set_node_keywords(%s)"), *FHandle));
		return sol::make_object(S, true);
	});

	// ---- cr_set_node_description(handle, description) ----
	Lua.set_function("cr_set_node_description", [&Session](const std::string& Handle, const std::string& Description, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] cr_set_node_description -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		URigVMController* Ctrl = GetCtrlForNode(Session, Node, TEXT("cr_set_node_description"));
		if (!Ctrl) return sol::lua_nil;

		FName NodeName = LuaControlRig::GetModelNodeName(Node);
		if (!Ctrl->SetNodeDescriptionByName(NodeName, UTF8_TO_TCHAR(Description.c_str()), false, false))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_set_node_description -> failed for \"%s\""), *FHandle));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_set_node_description(%s)"), *FHandle));
		return sol::make_object(S, true);
	});

	// ---- cr_set_pin_category(handle, pin_name, category?) ----
	Lua.set_function("cr_set_pin_category", [&Session](const std::string& Handle, const std::string& PinName,
		sol::optional<std::string> Category, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] cr_set_pin_category -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		URigVMController* Ctrl = GetCtrlForNode(Session, Node, TEXT("cr_set_pin_category"));
		if (!Ctrl) return sol::lua_nil;

	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		FString PinPath = LuaControlRig::BuildPinPath(Node, UTF8_TO_TCHAR(PinName.c_str()));
		bool bSuccess;
		if (!Category || Category->empty())
		{
			bSuccess = Ctrl->ClearPinCategory(PinPath, false, false);
		}
		else
		{
			bSuccess = Ctrl->SetPinCategory(PinPath, UTF8_TO_TCHAR(Category->c_str()), false, false);
		}

		if (!bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_set_pin_category -> failed for \"%s\""), *PinPath));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_set_pin_category(%s)"), *PinPath));
		return sol::make_object(S, true);
#else
		Session.Log(TEXT("[FAIL] cr_set_pin_category requires UE 5.5+"));
		return sol::lua_nil;
#endif
	});

	// ---- cr_unbind_pin_variable(handle, pin_name) ----
	Lua.set_function("cr_unbind_pin_variable", [&Session](const std::string& Handle, const std::string& PinName, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] cr_unbind_pin_variable -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		URigVMController* Ctrl = GetCtrlForNode(Session, Node, TEXT("cr_unbind_pin_variable"));
		if (!Ctrl) return sol::lua_nil;

		FString PinPath = LuaControlRig::BuildPinPath(Node, UTF8_TO_TCHAR(PinName.c_str()));
		if (!Ctrl->UnbindPinFromVariable(PinPath, false, false))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_unbind_pin_variable -> UnbindPinFromVariable failed for \"%s\""), *PinPath));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_unbind_pin_variable(%s)"), *PinPath));
		return sol::make_object(S, true);
	});

	// ---- cr_add_local_variable(name, type, default_value?, type_object_path?, graph_handle?) ----
	Lua.set_function("cr_add_local_variable", [&Session, GetCtrlFromContext](const std::string& Name, const std::string& CppType,
		sol::optional<std::string> DefaultValue, sol::optional<std::string> TypeObjectPath,
		sol::optional<std::string> GraphHandle, sol::this_state S) -> sol::object
	{
		URigVMController* Ctrl = GetCtrlFromContext(GraphHandle, TEXT("cr_add_local_variable"));
		if (!Ctrl) return sol::lua_nil;

		// Local variables only work on function sub-graphs (where Outer is URigVMLibraryNode in URigVMFunctionLibrary)
		URigVMGraph* Graph = Ctrl->GetGraph();
		if (Graph)
		{
			URigVMLibraryNode* LibraryNode = Cast<URigVMLibraryNode>(Graph->GetOuter());
			if (!LibraryNode || !LibraryNode->GetOuter()->IsA<URigVMFunctionLibrary>())
			{
				Session.Log(TEXT("[FAIL] cr_add_local_variable -> local variables can only be added to function graphs, not the main Rig graph. "
					"Create a function first (cr_collapse_nodes or cr_promote_to_function), then read that function graph."));
				return sol::lua_nil;
			}
		}

		FName VarName = FName(UTF8_TO_TCHAR(Name.c_str()));
		FString FCppType = UTF8_TO_TCHAR(CppType.c_str());
		FString DefVal = DefaultValue ? UTF8_TO_TCHAR(DefaultValue->c_str()) : FString();

		FRigVMGraphVariableDescription Desc;
		if (TypeObjectPath && !TypeObjectPath->empty())
		{
			FString FObjPath = UTF8_TO_TCHAR(TypeObjectPath->c_str());
			Desc = Ctrl->AddLocalVariableFromObjectPath(VarName, FCppType, FObjPath, DefVal, false);
		}
		else
		{
			Desc = Ctrl->AddLocalVariable(VarName, FCppType, nullptr, DefVal, false, false);
		}

		if (Desc.Name.IsNone())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_add_local_variable -> AddLocalVariable failed for \"%s\""), *VarName.ToString()));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_add_local_variable(\"%s\", %s)"), *Desc.Name.ToString(), *FCppType));
		return sol::make_object(S, true);
	});

	// ---- cr_remove_local_variable(name, graph_handle?) ----
	Lua.set_function("cr_remove_local_variable", [&Session, GetCtrlFromContext](const std::string& Name,
		sol::optional<std::string> GraphHandle, sol::this_state S) -> sol::object
	{
		URigVMController* Ctrl = GetCtrlFromContext(GraphHandle, TEXT("cr_remove_local_variable"));
		if (!Ctrl) return sol::lua_nil;

		FName VarName = FName(UTF8_TO_TCHAR(Name.c_str()));
		if (!Ctrl->RemoveLocalVariable(VarName, false, false))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_remove_local_variable -> RemoveLocalVariable failed for \"%s\""), *VarName.ToString()));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_remove_local_variable(\"%s\")"), *VarName.ToString()));
		return sol::make_object(S, true);
	});

	// ---- cr_rename_local_variable(name, new_name, graph_handle?) ----
	Lua.set_function("cr_rename_local_variable", [&Session, GetCtrlFromContext](const std::string& Name, const std::string& NewName,
		sol::optional<std::string> GraphHandle, sol::this_state S) -> sol::object
	{
		URigVMController* Ctrl = GetCtrlFromContext(GraphHandle, TEXT("cr_rename_local_variable"));
		if (!Ctrl) return sol::lua_nil;

		FName VarName = FName(UTF8_TO_TCHAR(Name.c_str()));
		FName NewVarName = FName(UTF8_TO_TCHAR(NewName.c_str()));
		if (!Ctrl->RenameLocalVariable(VarName, NewVarName, false, false))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_rename_local_variable -> failed \"%s\" -> \"%s\""), *VarName.ToString(), *NewVarName.ToString()));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_rename_local_variable(\"%s\" -> \"%s\")"), *VarName.ToString(), *NewVarName.ToString()));
		return sol::make_object(S, true);
	});

	// ---- cr_set_local_variable_type(name, cpp_type, type_object_path?, graph_handle?) ----
	Lua.set_function("cr_set_local_variable_type", [&Session, GetCtrlFromContext](const std::string& Name, const std::string& CppType,
		sol::optional<std::string> TypeObjectPath, sol::optional<std::string> GraphHandle, sol::this_state S) -> sol::object
	{
		URigVMController* Ctrl = GetCtrlFromContext(GraphHandle, TEXT("cr_set_local_variable_type"));
		if (!Ctrl) return sol::lua_nil;

		FName VarName = FName(UTF8_TO_TCHAR(Name.c_str()));
		FString FCppType = UTF8_TO_TCHAR(CppType.c_str());

		bool bSuccess;
		if (TypeObjectPath && !TypeObjectPath->empty())
		{
			FString FObjPath = UTF8_TO_TCHAR(TypeObjectPath->c_str());
			bSuccess = Ctrl->SetLocalVariableTypeFromObjectPath(VarName, FCppType, FObjPath, false, false);
		}
		else
		{
			bSuccess = Ctrl->SetLocalVariableType(VarName, FCppType, nullptr, false, false);
		}

		if (!bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_set_local_variable_type -> failed for \"%s\""), *VarName.ToString()));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_set_local_variable_type(\"%s\", %s)"), *VarName.ToString(), *FCppType));
		return sol::make_object(S, true);
	});

	// ---- cr_set_local_variable_default(name, default_value, graph_handle?) ----
	Lua.set_function("cr_set_local_variable_default", [&Session, GetCtrlFromContext](const std::string& Name, const std::string& DefaultValue,
		sol::optional<std::string> GraphHandle, sol::this_state S) -> sol::object
	{
		URigVMController* Ctrl = GetCtrlFromContext(GraphHandle, TEXT("cr_set_local_variable_default"));
		if (!Ctrl) return sol::lua_nil;

		FName VarName = FName(UTF8_TO_TCHAR(Name.c_str()));
		FString DefVal = UTF8_TO_TCHAR(DefaultValue.c_str());

		if (!Ctrl->SetLocalVariableDefaultValue(VarName, DefVal, false, false))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_set_local_variable_default -> failed for \"%s\""), *VarName.ToString()));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_set_local_variable_default(\"%s\")"), *VarName.ToString()));
		return sol::make_object(S, true);
	});

	// ---- cr_collapse_nodes(handles, name?) ----
	Lua.set_function("cr_collapse_nodes", [&Session](sol::table Handles, sol::optional<std::string> NameOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		// Collect node names from handles — all must be in the same graph
		TArray<FName> NodeNames;
		TArray<FString> OriginalHandles; // Track for cleanup after collapse
		UEdGraph* SourceGraph = nullptr;
		URigVMController* Ctrl = nullptr;

		for (auto& Pair : Handles)
		{
			sol::optional<std::string> HandleOpt = Pair.second.as<sol::optional<std::string>>();
			if (!HandleOpt) continue;
			FString FHandle = UTF8_TO_TCHAR(HandleOpt->c_str());
			UEdGraphNode* Node = Session.FindNode(FHandle);
			if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] cr_collapse_nodes -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }
			if (!LuaControlRig::IsRigVMNode(Node))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] cr_collapse_nodes -> node \"%s\" is not a ControlRig node"), *FHandle));
				return sol::lua_nil;
			}
			if (!SourceGraph)
			{
				SourceGraph = Node->GetGraph();
				Ctrl = LuaControlRig::GetController(SourceGraph);
				if (!Ctrl) { Session.Log(TEXT("[FAIL] cr_collapse_nodes -> could not get URigVMController")); return sol::lua_nil; }
			}
			else if (Node->GetGraph() != SourceGraph)
			{
				Session.Log(TEXT("[FAIL] cr_collapse_nodes -> all nodes must be in the same graph"));
				return sol::lua_nil;
			}

			FName ModelName = LuaControlRig::GetModelNodeName(Node);
			if (ModelName.IsNone())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] cr_collapse_nodes -> could not resolve model name for \"%s\""), *FHandle));
				return sol::lua_nil;
			}
			NodeNames.Add(ModelName);
			OriginalHandles.Add(FHandle);
		}

		if (NodeNames.Num() == 0)
		{
			Session.Log(TEXT("[FAIL] cr_collapse_nodes -> no valid nodes provided"));
			return sol::lua_nil;
		}

		FString CollapseName = NameOpt ? UTF8_TO_TCHAR(NameOpt->c_str()) : FString();

		URigVMCollapseNode* CollapseNode = Ctrl->CollapseNodes(NodeNames, CollapseName, false, false, false);
		if (!CollapseNode)
		{
			Session.Log(TEXT("[FAIL] cr_collapse_nodes -> CollapseNodes failed"));
			return sol::lua_nil;
		}

		// Remove stale handles for the collapsed nodes
		for (const FString& OldHandle : OriginalHandles)
		{
			Session.Nodes.Remove(OldHandle);
		}

		// Find the EdGraph wrapper for the new collapse node
		URigVMEdGraph* RigGraph = LuaControlRig::GetEdGraph(SourceGraph);
		UEdGraphNode* EdNode = RigGraph ? RigGraph->FindNodeForModelNodeName(CollapseNode->GetFName()) : nullptr;

		FString NodeNameStr = CollapseNode->GetName();
		FString HandleStr;
		if (EdNode)
		{
			HandleStr = EdNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
			Session.Nodes.Add(HandleStr, EdNode);
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_collapse_nodes -> \"%s\", handle=%s"), *NodeNameStr, *HandleStr));

		sol::table Result = LuaView.create_table();
		Result["node_name"] = TCHAR_TO_UTF8(*NodeNameStr);
		if (!HandleStr.IsEmpty()) Result["handle"] = TCHAR_TO_UTF8(*HandleStr);
		return Result;
	});

	// ---- cr_expand_node(handle) ----
	Lua.set_function("cr_expand_node", [&Session](const std::string& Handle, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] cr_expand_node -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		URigVMController* Ctrl = GetCtrlForNode(Session, Node, TEXT("cr_expand_node"));
		if (!Ctrl) return sol::lua_nil;

		FName ModelName = LuaControlRig::GetModelNodeName(Node);
		if (ModelName.IsNone())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_expand_node -> could not resolve model name for \"%s\""), *FHandle));
			return sol::lua_nil;
		}

		// Cache the graph before expand — Node may become stale after ExpandLibraryNode
		UEdGraph* SourceGraph = Node->GetGraph();

		TArray<URigVMNode*> ExpandedNodes = Ctrl->ExpandLibraryNode(ModelName, false, false);
		if (ExpandedNodes.Num() == 0)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_expand_node -> ExpandLibraryNode failed for \"%s\""), *FHandle));
			return sol::lua_nil;
		}

		// Remove stale handle for the expanded node
		Session.Nodes.Remove(FHandle);

		// Find EdGraph wrappers and register them
		URigVMEdGraph* RigGraph = LuaControlRig::GetEdGraph(SourceGraph);
		sol::table HandlesTable = LuaView.create_table();
		int32 Idx = 1;
		for (URigVMNode* ModelNode : ExpandedNodes)
		{
			if (!ModelNode) continue;
			UEdGraphNode* EdNode = RigGraph ? RigGraph->FindNodeForModelNodeName(ModelNode->GetFName()) : nullptr;
			if (EdNode)
			{
				FString Guid = EdNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
				Session.Nodes.Add(Guid, EdNode);
				HandlesTable[Idx++] = TCHAR_TO_UTF8(*Guid);
			}
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_expand_node -> expanded %d nodes"), ExpandedNodes.Num()));

		sol::table Result = LuaView.create_table();
		Result["handles"] = HandlesTable;
		return Result;
	});

	// ---- cr_promote_to_function(handle, function_path?) ----
	Lua.set_function("cr_promote_to_function", [&Session](const std::string& Handle,
		sol::optional<std::string> FunctionPath, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] cr_promote_to_function -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		URigVMController* Ctrl = GetCtrlForNode(Session, Node, TEXT("cr_promote_to_function"));
		if (!Ctrl) return sol::lua_nil;

		FName ModelName = LuaControlRig::GetModelNodeName(Node);
		if (ModelName.IsNone())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_promote_to_function -> could not resolve model name for \"%s\""), *FHandle));
			return sol::lua_nil;
		}

		// Cache graph before the node gets replaced
		UEdGraph* SourceGraph = Node->GetGraph();
		FString ExistingPath = FunctionPath ? UTF8_TO_TCHAR(FunctionPath->c_str()) : FString();

		FName NewNodeName = Ctrl->PromoteCollapseNodeToFunctionReferenceNode(ModelName, false, false, ExistingPath);
		if (NewNodeName.IsNone())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_promote_to_function -> PromoteCollapseNodeToFunctionReferenceNode failed for \"%s\""), *FHandle));
			return sol::lua_nil;
		}

		// Remove stale handle, register new node
		Session.Nodes.Remove(FHandle);
		URigVMEdGraph* RigGraph = LuaControlRig::GetEdGraph(SourceGraph);
		UEdGraphNode* NewEdNode = RigGraph ? RigGraph->FindNodeForModelNodeName(NewNodeName) : nullptr;
		if (NewEdNode)
		{
			FString NewHandle = NewEdNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
			Session.Nodes.Add(NewHandle, NewEdNode);
			Session.Log(FString::Printf(TEXT("[OK] cr_promote_to_function -> \"%s\", handle=%s"), *NewNodeName.ToString(), *NewHandle));

			sol::state_view LuaView(S);
			sol::table Result = LuaView.create_table();
			Result["node_name"] = TCHAR_TO_UTF8(*NewNodeName.ToString());
			Result["handle"] = TCHAR_TO_UTF8(*NewHandle);
			return Result;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_promote_to_function -> \"%s\""), *NewNodeName.ToString()));
		return sol::make_object(S, std::string(TCHAR_TO_UTF8(*NewNodeName.ToString())));
	});

	// ---- cr_promote_to_collapse(handle, remove_definition?) ----
	Lua.set_function("cr_promote_to_collapse", [&Session](const std::string& Handle,
		sol::optional<bool> RemoveDefinition, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] cr_promote_to_collapse -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		URigVMController* Ctrl = GetCtrlForNode(Session, Node, TEXT("cr_promote_to_collapse"));
		if (!Ctrl) return sol::lua_nil;

		FName ModelName = LuaControlRig::GetModelNodeName(Node);
		if (ModelName.IsNone())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_promote_to_collapse -> could not resolve model name for \"%s\""), *FHandle));
			return sol::lua_nil;
		}

		// Cache graph before the node gets replaced
		UEdGraph* SourceGraph = Node->GetGraph();
		bool bRemoveDef = RemoveDefinition.value_or(false);

		FName NewNodeName = Ctrl->PromoteFunctionReferenceNodeToCollapseNode(ModelName, false, false, bRemoveDef);
		if (NewNodeName.IsNone())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_promote_to_collapse -> PromoteFunctionReferenceNodeToCollapseNode failed for \"%s\""), *FHandle));
			return sol::lua_nil;
		}

		// Remove stale handle, register new node
		Session.Nodes.Remove(FHandle);
		URigVMEdGraph* RigGraph = LuaControlRig::GetEdGraph(SourceGraph);
		UEdGraphNode* NewEdNode = RigGraph ? RigGraph->FindNodeForModelNodeName(NewNodeName) : nullptr;
		if (NewEdNode)
		{
			FString NewHandle = NewEdNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
			Session.Nodes.Add(NewHandle, NewEdNode);
			Session.Log(FString::Printf(TEXT("[OK] cr_promote_to_collapse -> \"%s\", handle=%s"), *NewNodeName.ToString(), *NewHandle));

			sol::state_view LuaView(S);
			sol::table Result = LuaView.create_table();
			Result["node_name"] = TCHAR_TO_UTF8(*NewNodeName.ToString());
			Result["handle"] = TCHAR_TO_UTF8(*NewHandle);
			return Result;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_promote_to_collapse -> \"%s\""), *NewNodeName.ToString()));
		return sol::make_object(S, std::string(TCHAR_TO_UTF8(*NewNodeName.ToString())));
	});

	// ---- cr_add_trait(handle, trait_type_path, trait_name?, default_value?, pin_index?) ----
	Lua.set_function("cr_add_trait", [&Session](const std::string& Handle, const std::string& TraitTypePath,
		sol::optional<std::string> TraitName, sol::optional<std::string> DefaultValue,
		sol::optional<int> PinIndex, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] cr_add_trait -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		URigVMController* Ctrl = GetCtrlForNode(Session, Node, TEXT("cr_add_trait"));
		if (!Ctrl) return sol::lua_nil;

		FName ModelName = LuaControlRig::GetModelNodeName(Node);
		if (ModelName.IsNone())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_add_trait -> could not resolve model name for \"%s\""), *FHandle));
			return sol::lua_nil;
		}

		FName TypePath = FName(UTF8_TO_TCHAR(TraitTypePath.c_str()));
		FName TName = TraitName ? FName(UTF8_TO_TCHAR(TraitName->c_str())) : NAME_None;
		FString DefVal = DefaultValue ? UTF8_TO_TCHAR(DefaultValue->c_str()) : FString();
		int32 PIdx = PinIndex.value_or(-1);

	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		FName ResultName = Ctrl->AddTrait(ModelName, TypePath, TName, DefVal, PIdx, false, false);
		if (ResultName.IsNone())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_add_trait -> AddTrait failed for node \"%s\", trait \"%s\""), *FHandle, *TypePath.ToString()));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_add_trait(%s, \"%s\") -> \"%s\""), *FHandle, *TypePath.ToString(), *ResultName.ToString()));
		return sol::make_object(S, std::string(TCHAR_TO_UTF8(*ResultName.ToString())));
#else
		Session.Log(TEXT("[FAIL] cr_add_trait requires UE 5.5+"));
		return sol::lua_nil;
#endif
	});

	// ---- cr_remove_trait(handle, trait_name) ----
	Lua.set_function("cr_remove_trait", [&Session](const std::string& Handle, const std::string& TraitName, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] cr_remove_trait -> node \"%s\" not found"), *FHandle)); return sol::lua_nil; }

		URigVMController* Ctrl = GetCtrlForNode(Session, Node, TEXT("cr_remove_trait"));
		if (!Ctrl) return sol::lua_nil;

		FName ModelName = LuaControlRig::GetModelNodeName(Node);
		if (ModelName.IsNone())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_remove_trait -> could not resolve model name for \"%s\""), *FHandle));
			return sol::lua_nil;
		}

		FName TName = FName(UTF8_TO_TCHAR(TraitName.c_str()));
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		if (!Ctrl->RemoveTrait(ModelName, TName, false, false))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] cr_remove_trait -> RemoveTrait failed for node \"%s\", trait \"%s\""), *FHandle, *TName.ToString()));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] cr_remove_trait(%s, \"%s\")"), *FHandle, *TName.ToString()));
		return sol::make_object(S, true);
#else
		Session.Log(TEXT("[FAIL] cr_remove_trait requires UE 5.5+"));
		return sol::lua_nil;
#endif
	});
}

REGISTER_LUA_BINDING(ControlRigGraphOps, CRGraphOpsDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("ControlRig")))
	{
		Session.Log(TEXT("[WARN] ControlRig plugin is not loaded. Enable it in Edit > Plugins to use this feature."));
		return;
	}
	BindControlRigGraphOps(Lua, Session);
});
