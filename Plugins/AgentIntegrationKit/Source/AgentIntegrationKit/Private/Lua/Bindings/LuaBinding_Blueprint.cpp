#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaPinHelper.h"
#include "Blueprint/BlueprintUtils.h"
#include "Blueprint/NodeUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Timeline.h"
#include "Engine/TimelineTemplate.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphNode_Comment.h"
#include "Engine/BlueprintGeneratedClass.h"

// AnimBP support
#include "Kismet2/Kismet2NameValidators.h"
#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimationStateMachineGraph.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimStateEntryNode.h"
#include "AnimationGraph.h"
#include "AnimationTransitionGraph.h"
#include "AnimGraphNode_TransitionResult.h"

// Widget Blueprint support
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/NamedSlotInterface.h"
#include "WidgetBlueprintEditorUtils.h"
#include "Components/CanvasPanel.h"
#include "Components/VerticalBox.h"
#include "Components/HorizontalBox.h"

// Event binding support
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Event.h"
#include "EdGraphSchema_K2_Actions.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"

// Linked anim layer support
#include "AnimGraphNode_LinkedAnimLayer.h"
#include "Animation/AnimLayerInterface.h"
#include "Engine/MemberReference.h"

#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

// Widget rename/replace support
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "MovieScene.h"
#include "MovieScenePossessable.h"
#include "Templates/WidgetTemplateClass.h"

// Safe GetNodeTitle — some AnimGraph nodes can crash on GetNodeTitle
static FString SafeGetNodeTitle(UEdGraphNode* Node)
{
	// AnimGraph module nodes can crash in GetNodeTitle if inner anim node isn't fully initialized
	UClass* NodeClass = Node->GetClass();
	UPackage* Package = NodeClass->GetOuterUPackage();
	if (Package && Package->GetName().Contains(TEXT("AnimGraph")))
	{
		// Use the editable title for anim state nodes (returns the user-facing name)
		if (Cast<UAnimStateNode>(Node) || Cast<UAnimStateTransitionNode>(Node) || Cast<UAnimStateEntryNode>(Node))
		{
			return Node->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
		}
		// For other anim nodes, try MenuTitle but fallback to class name on failure
		FText Title = Node->GetNodeTitle(ENodeTitleType::MenuTitle);
		if (!Title.IsEmpty())
		{
			return Title.ToString();
		}
		return NodeClass->GetName();
	}
	return Node->GetNodeTitle(ENodeTitleType::MenuTitle).ToString();
}

static sol::table BuildNodeTable(sol::state_view& LuaView, UEdGraphNode* Node)
{
	FString NodeGuid = Node->NodeGuid.ToString();

	sol::table NodeEntry = LuaView.create_table();
	NodeEntry["handle"] = TCHAR_TO_UTF8(*NodeGuid);
	NodeEntry["name"] = TCHAR_TO_UTF8(*SafeGetNodeTitle(Node));
	NodeEntry["type"] = TCHAR_TO_UTF8(*Node->GetClass()->GetName());
	NodeEntry["x"] = Node->NodePosX;
	NodeEntry["y"] = Node->NodePosY;

	sol::table InPins = LuaView.create_table();
	sol::table OutPins = LuaView.create_table();
	sol::table Connections = LuaView.create_table();
	int32 InIdx = 1, OutIdx = 1, ConnIdx = 1;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->bHidden) continue;

		sol::table PinInfo = NeoLuaPin::BuildPinTable(LuaView, Pin);
		FString PinName = Pin->GetDisplayName().IsEmpty()
			? Pin->PinName.ToString()
			: Pin->GetDisplayName().ToString();

		if (Pin->Direction == EGPD_Input)
		{
			InPins[InIdx++] = PinInfo;
		}
		else
		{
			OutPins[OutIdx++] = PinInfo;
		}

		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (!LinkedPin || !LinkedPin->GetOwningNode()) continue;

			sol::table Conn = LuaView.create_table();
			Conn["from_node"] = TCHAR_TO_UTF8(*NodeGuid);
			Conn["from_pin"] = TCHAR_TO_UTF8(*PinName);
			Conn["to_node"] = TCHAR_TO_UTF8(*LinkedPin->GetOwningNode()->NodeGuid.ToString());

			FString LinkedPinName = LinkedPin->GetDisplayName().IsEmpty()
				? LinkedPin->PinName.ToString()
				: LinkedPin->GetDisplayName().ToString();
			Conn["to_pin"] = TCHAR_TO_UTF8(*LinkedPinName);

			Connections[ConnIdx++] = Conn;
		}
	}

	NodeEntry["pins_in"] = InPins;
	NodeEntry["pins_out"] = OutPins;
	NodeEntry["connections"] = Connections;
	return NodeEntry;
}

static sol::table BuildGraphObject(sol::state_view& LuaView, FLuaSessionData& Session,
	UEdGraph* Graph, const std::string& BPPath, const std::string& GraphName)
{
	Session.RegisterGraphNodes(Graph);

	sol::table GraphObj = LuaView.create_table();
	GraphObj["name"] = GraphName;

	sol::table NodesTable = LuaView.create_table();
	int32 NodeIdx = 1;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		NodesTable[NodeIdx++] = BuildNodeTable(LuaView, Node);
	}
	GraphObj["nodes"] = NodesTable;

	// graph:add_node(node, x?, y?) — delegates to global add_node(bp_path, graph_name, ...)
	GraphObj.set_function("add_node", [BPPath, GraphName](sol::table /*self*/, sol::object NodeArg,
		sol::optional<double> X, sol::optional<double> Y, sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["add_node"];
		auto result = fn(BPPath, GraphName, NodeArg, X, Y);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:connect(from_handle, from_pin, to_handle, to_pin)
	GraphObj.set_function("connect", [](sol::table /*self*/,
		const std::string& FromHandle, const std::string& FromPin,
		const std::string& ToHandle, const std::string& ToPin,
		sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["connect"];
		auto result = fn(FromHandle, FromPin, ToHandle, ToPin);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:disconnect(handle, pin_name)
	GraphObj.set_function("disconnect", [](sol::table /*self*/,
		const std::string& Handle, const std::string& PinName,
		sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["disconnect"];
		auto result = fn(Handle, PinName);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:set_pin(handle, pin_name, value)
	GraphObj.set_function("set_pin", [](sol::table /*self*/,
		const std::string& Handle, const std::string& PinName, const std::string& Value,
		sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["set_pin"];
		auto result = fn(Handle, PinName, Value);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:delete_node(handle)
	GraphObj.set_function("delete_node", [](sol::table /*self*/,
		const std::string& Handle, sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["delete_node"];
		auto result = fn(Handle);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:get_pin(handle, pin_name)
	GraphObj.set_function("get_pin", [](sol::table /*self*/,
		const std::string& Handle, const std::string& PinName,
		sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["get_pin"];
		auto result = fn(Handle, PinName);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:move_node(handle, x, y, relative?)
	// If relative is true, x and y are deltas (dx, dy) rather than absolute positions
	GraphObj.set_function("move_node", [&Session](sol::table /*self*/,
		const std::string& Handle, double X, double Y, sol::optional<bool> Relative, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] move_node -> node \"%s\" not found"), *FHandle));
			return sol::lua_nil;
		}
		if (Relative.value_or(false))
		{
			Node->Modify();
			Node->NodePosX += (int32)X;
			Node->NodePosY += (int32)Y;
			Session.Log(FString::Printf(TEXT("[OK] move_node(\"%s\") -> relative (%+.0f, %+.0f) = (%d, %d)"),
				*FHandle, X, Y, Node->NodePosX, Node->NodePosY));
		}
		else
		{
			NeoBlueprint::MoveNode(Node, X, Y);
			Session.Log(FString::Printf(TEXT("[OK] move_node(\"%s\") -> (%.0f, %.0f)"), *FHandle, X, Y));
		}
		return sol::make_object(S, true);
	});

	// graph:disconnect_from(from_handle, from_pin, to_handle, to_pin) — delegates to global
	GraphObj.set_function("disconnect_from", [](sol::table /*self*/,
		const std::string& FromHandle, const std::string& FromPin,
		const std::string& ToHandle, const std::string& ToPin,
		sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["disconnect_from"];
		auto result = fn(FromHandle, FromPin, ToHandle, ToPin);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:split_pin(handle, pin_name) — delegates to global
	GraphObj.set_function("split_pin", [](sol::table /*self*/,
		const std::string& Handle, const std::string& PinName,
		sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["split_pin"];
		auto result = fn(Handle, PinName);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:recombine_pin(handle, pin_name) — delegates to global
	GraphObj.set_function("recombine_pin", [](sol::table /*self*/,
		const std::string& Handle, const std::string& PinName,
		sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["recombine_pin"];
		auto result = fn(Handle, PinName);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:add_exec_pin(handle) — delegates to global
	GraphObj.set_function("add_exec_pin", [](sol::table /*self*/,
		const std::string& Handle, sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["add_exec_pin"];
		auto result = fn(Handle);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:remove_exec_pin(handle) — delegates to global
	GraphObj.set_function("remove_exec_pin", [](sol::table /*self*/,
		const std::string& Handle, sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["remove_exec_pin"];
		auto result = fn(Handle);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:reset_pin(handle, pin_name) — delegates to global
	GraphObj.set_function("reset_pin", [](sol::table /*self*/,
		const std::string& Handle, const std::string& PinName,
		sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["reset_pin"];
		auto result = fn(Handle, PinName);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:set_node_comment(handle, text, visible?) — delegates to global
	GraphObj.set_function("set_node_comment", [](sol::table /*self*/,
		const std::string& Handle, const std::string& Text,
		sol::optional<bool> Visible, sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["set_node_comment"];
		if (Visible)
			{ auto result = fn(Handle, Text, *Visible); if (result.valid()) return result; }
		else
			{ auto result = fn(Handle, Text); if (result.valid()) return result; }
		return sol::lua_nil;
	});

	return GraphObj;
}

static sol::table BuildComponentTable(sol::state_view& LuaView, USCS_Node* Node)
{
	sol::table Comp = LuaView.create_table();
	Comp["name"] = TCHAR_TO_UTF8(*Node->GetVariableName().ToString());
	Comp["class"] = Node->ComponentClass ? TCHAR_TO_UTF8(*Node->ComponentClass->GetName()) : "None";

	if (Node->ParentComponentOrVariableName != NAME_None)
	{
		Comp["parent"] = TCHAR_TO_UTF8(*Node->ParentComponentOrVariableName.ToString());
	}

	sol::table Children = LuaView.create_table();
	int32 ChildIdx = 1;
	for (USCS_Node* Child : Node->GetChildNodes())
	{
		if (Child)
		{
			Children[ChildIdx++] = BuildComponentTable(LuaView, Child);
		}
	}
	if (ChildIdx > 1)
	{
		Comp["children"] = Children;
	}

	return Comp;
}

static sol::table BuildVariablesTable(sol::state_view& LuaView, UBlueprint* BP)
{
	sol::table Vars = LuaView.create_table();
	int32 VarIdx = 1;

	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		sol::table VarInfo = LuaView.create_table();
		VarInfo["name"] = TCHAR_TO_UTF8(*Var.VarName.ToString());
		VarInfo["type"] = TCHAR_TO_UTF8(*Var.VarType.PinCategory.ToString());
		if (Var.VarType.PinSubCategoryObject.IsValid())
		{
			VarInfo["sub_type"] = TCHAR_TO_UTF8(*Var.VarType.PinSubCategoryObject->GetName());
		}
		if (!Var.DefaultValue.IsEmpty())
		{
			VarInfo["default"] = TCHAR_TO_UTF8(*Var.DefaultValue);
		}
		if (Var.VarType.ContainerType == EPinContainerType::Array)
			VarInfo["container"] = "array";
		else if (Var.VarType.ContainerType == EPinContainerType::Set)
			VarInfo["container"] = "set";
		else if (Var.VarType.ContainerType == EPinContainerType::Map)
		{
			VarInfo["container"] = "map";
			VarInfo["value_type"] = TCHAR_TO_UTF8(*Var.VarType.PinValueType.TerminalCategory.ToString());
			if (Var.VarType.PinValueType.TerminalSubCategoryObject.IsValid())
			{
				VarInfo["value_sub_type"] = TCHAR_TO_UTF8(*Var.VarType.PinValueType.TerminalSubCategoryObject->GetName());
			}
		}

		// Variable flags
		uint64* Flags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(BP, Var.VarName);
		if (Flags)
		{
			if (*Flags & CPF_Edit) VarInfo["edit_anywhere"] = true;
			if (*Flags & CPF_DisableEditOnInstance) VarInfo["edit_defaults_only"] = true;
			if (*Flags & CPF_DisableEditOnTemplate) VarInfo["edit_instance_only"] = true;
			if (*Flags & CPF_BlueprintReadOnly) VarInfo["blueprint_read_only"] = true;
			if (*Flags & CPF_Net) VarInfo["replicated"] = true;
			if (*Flags & CPF_RepNotify) VarInfo["rep_notify"] = true;
			if (*Flags & CPF_Transient) VarInfo["transient"] = true;
			if (*Flags & CPF_SaveGame) VarInfo["save_game"] = true;
			if (*Flags & CPF_ExposeOnSpawn) VarInfo["expose_on_spawn"] = true;
			if (*Flags & CPF_Interp) VarInfo["interp"] = true;
			if (*Flags & CPF_AdvancedDisplay) VarInfo["advanced_display"] = true;
			if (*Flags & CPF_Deprecated) VarInfo["deprecated"] = true;
		}

		// Variable metadata (category, tooltip, custom metadata)
		FText Category = FBlueprintEditorUtils::GetBlueprintVariableCategory(BP, Var.VarName, nullptr);
		if (!Category.IsEmpty())
		{
			VarInfo["category"] = TCHAR_TO_UTF8(*Category.ToString());
		}
		FString TooltipMeta;
		if (FBlueprintEditorUtils::GetBlueprintVariableMetaData(BP, Var.VarName, nullptr, FName(TEXT("tooltip")), TooltipMeta) && !TooltipMeta.IsEmpty())
		{
			VarInfo["tooltip"] = TCHAR_TO_UTF8(*TooltipMeta);
		}

		Vars[TCHAR_TO_UTF8(*Var.VarName.ToString())] = VarInfo;
		Vars[VarIdx++] = VarInfo;
	}

	return Vars;
}

// No docs — _open_blueprint is internal-only, called by open_asset for Blueprint assets
static TArray<FLuaFunctionDoc> BlueprintDocs = {};

static void BindBlueprint(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_open_blueprint", [&Session](const std::string& Path, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString FPath = UTF8_TO_TCHAR(Path.c_str());

		FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
		if (!Info.Blueprint)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] open_blueprint(\"%s\") -> not found"), *FPath));
			return sol::lua_nil;
		}

		sol::table BP = LuaView.create_table();
		BP["name"] = TCHAR_TO_UTF8(*Info.Name);
		BP["path"] = TCHAR_TO_UTF8(*Info.AssetPath);
		BP["parent_class"] = TCHAR_TO_UTF8(*Info.ParentClass);

		// Build graphs table keyed by name (and by index)
		sol::table Graphs = LuaView.create_table();
		int32 GraphIdx = 1;

		auto BuildGraph = [&](UEdGraph* Graph)
		{
			if (!Graph) return;
			std::string GName = TCHAR_TO_UTF8(*Graph->GetName());
			sol::table GraphObj = BuildGraphObject(LuaView, Session, Graph, Path, GName);
			Graphs[GName] = GraphObj;
			Graphs[GraphIdx++] = GraphObj;
		};

		for (UEdGraph* Graph : Info.Blueprint->UbergraphPages)
		{
			BuildGraph(Graph);
		}
		for (UEdGraph* Graph : Info.Blueprint->FunctionGraphs)
		{
			BuildGraph(Graph);
		}
		for (UEdGraph* Graph : Info.Blueprint->MacroGraphs)
		{
			BuildGraph(Graph);
		}

		// Recursively add composite/collapsed sub-graphs
		TFunction<void(UEdGraph*)> BuildSubGraphs = [&](UEdGraph* Parent)
		{
			for (UEdGraph* Sub : Parent->SubGraphs)
			{
				if (!Sub) continue;
				BuildGraph(Sub);
				BuildSubGraphs(Sub);
			}
		};
		for (UEdGraph* Graph : Info.Blueprint->UbergraphPages)
			if (Graph) BuildSubGraphs(Graph);
		for (UEdGraph* Graph : Info.Blueprint->FunctionGraphs)
			if (Graph) BuildSubGraphs(Graph);
		for (UEdGraph* Graph : Info.Blueprint->MacroGraphs)
			if (Graph) BuildSubGraphs(Graph);

		// AnimBP: collect nested state machine / state / transition / layer graphs
		{
			TArray<TPair<FString, UEdGraph*>> AnimGraphs;
			NeoBlueprint::CollectAnimBPGraphs(Info.Blueprint, AnimGraphs);
			for (const auto& Pair : AnimGraphs)
			{
				if (!Pair.Value) continue;
				std::string SelectorName = TCHAR_TO_UTF8(*Pair.Key);
				sol::table GraphObj = BuildGraphObject(LuaView, Session, Pair.Value, Path, SelectorName);
				Graphs[SelectorName] = GraphObj;
				Graphs[GraphIdx++] = GraphObj;
			}
		}

		BP["graphs"] = Graphs;

		// bp.variables — keyed by name
		BP["variables"] = BuildVariablesTable(LuaView, Info.Blueprint);

		// bp.components — from SCS tree (all nodes including children)
		sol::table Components = LuaView.create_table();
		if (Info.Blueprint->SimpleConstructionScript)
		{
			int32 CompIdx = 1;
			for (USCS_Node* SCSNode : Info.Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				if (!SCSNode) continue;
				sol::table CompEntry = BuildComponentTable(LuaView, SCSNode);

				// Add SCS tree parent if this is a child node (not a root)
				if (!CompEntry["parent"].valid())
				{
					USCS_Node* ParentNode = Info.Blueprint->SimpleConstructionScript->FindParentNode(SCSNode);
					if (ParentNode)
					{
						CompEntry["parent"] = TCHAR_TO_UTF8(*ParentNode->GetVariableName().ToString());
					}
				}

				Components[TCHAR_TO_UTF8(*SCSNode->GetVariableName().ToString())] = CompEntry;
				Components[CompIdx++] = CompEntry;
			}
		}
		BP["components"] = Components;

		// bp.interfaces — list of implemented interfaces
		sol::table Interfaces = LuaView.create_table();
		{
			int32 IfIdx = 1;
			for (const FBPInterfaceDescription& Desc : Info.Blueprint->ImplementedInterfaces)
			{
				if (Desc.Interface)
				{
					sol::table Entry = LuaView.create_table();
					Entry["name"] = TCHAR_TO_UTF8(*Desc.Interface->GetName());
					Entry["class_path"] = TCHAR_TO_UTF8(*Desc.Interface->GetClassPathName().ToString());

					sol::table Funcs = LuaView.create_table();
					int32 FIdx = 1;
					for (UEdGraph* FuncGraph : Desc.Graphs)
					{
						if (FuncGraph)
						{
							Funcs[FIdx++] = TCHAR_TO_UTF8(*FuncGraph->GetName());
						}
					}
					Entry["functions"] = Funcs;
					Interfaces[IfIdx++] = Entry;
				}
			}
		}
		BP["interfaces"] = Interfaces;

		// bp:find_nodes(query, max?) — context-aware search
		std::string PathStr = Path;
		BP.set_function("find_nodes", [PathStr](sol::table /*self*/,
			const std::string& Query, sol::optional<int> Max,
			sol::this_state S) -> sol::object
		{
			sol::state_view lua(S);
			sol::protected_function fn = lua["find_nodes"];
			auto result = fn(Query, PathStr, Max);
			if (result.valid()) return result;
			return sol::lua_nil;
		});

		// bp:add_variable(name, type, options?) — options table or plain default value
		BP.set_function("add_variable", [PathStr](sol::table /*self*/,
			const std::string& Name, const std::string& Type,
			sol::optional<sol::object> Options, sol::this_state S) -> sol::object
		{
			sol::state_view lua(S);
			sol::protected_function fn = lua["add_variable"];
			auto result = fn(PathStr, Name, Type, Options);
			if (result.valid()) return result;
			return sol::lua_nil;
		});

		// bp:add_component(name, class, parent?)
		BP.set_function("add_component", [&Session, FPath](sol::table /*self*/,
			const std::string& Name, const std::string& ClassName,
			sol::optional<std::string> Parent, sol::this_state S) -> sol::object
		{
			sol::state_view lua(S);
			FString FName = UTF8_TO_TCHAR(Name.c_str());
			FString FClass = UTF8_TO_TCHAR(ClassName.c_str());
			FString FParent = Parent.has_value() ? UTF8_TO_TCHAR(Parent.value().c_str()) : TEXT("");

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_component -> blueprint not found")));
				return sol::lua_nil;
			}

			USCS_Node* Node = NeoBlueprint::AddComponent(Info.Blueprint, FName, FClass, FParent);
			if (!Node)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_component(\"%s\", \"%s\") -> failed. Class not found or not a component type."),
					*FName, *FClass));
				return sol::lua_nil;
			}

			sol::table Result = BuildComponentTable(lua, Node);
			Session.Log(FString::Printf(TEXT("[OK] add_component(\"%s\", \"%s\") -> added"),
				*FName, *FClass));
			return Result;
		});

		// bp:add_function(name, options?) -> returns graph object (upsert: creates or updates)
		// Options: {pure, const_func, category, params={{name,type},...}, returns={{name,type},...}}
		BP.set_function("add_function", [&Session, FPath, PathStr](sol::table /*self*/,
			const std::string& FuncName, sol::optional<sol::table> Options, sol::this_state S) -> sol::object
		{
			sol::state_view lua(S);
			FString FFuncName = UTF8_TO_TCHAR(FuncName.c_str());

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_function -> blueprint not found")));
				return sol::lua_nil;
			}

			bool bFuncExisted = (NeoBlueprint::FindGraph(Info.Blueprint, FFuncName) != nullptr);
			UEdGraph* FuncGraph = NeoBlueprint::AddFunctionGraph(Info.Blueprint, FFuncName);
			if (!FuncGraph)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_function(\"%s\") -> failed"), *FFuncName));
				return sol::lua_nil;
			}

			// Apply options
			if (Options.has_value())
			{
				sol::table Opts = Options.value();

				// Function flags (supports both adding and removing)
				uint32 AddFlags = 0, RemoveFlags = 0;
				sol::object PureObj = Opts["pure"];
				if (PureObj.valid() && PureObj.get_type() != sol::type::lua_nil)
				{
					if (PureObj.as<bool>()) AddFlags |= FUNC_BlueprintPure;
					else RemoveFlags |= FUNC_BlueprintPure;
				}
				sol::object ConstObj = Opts["const_func"];
				if (ConstObj.valid() && ConstObj.get_type() != sol::type::lua_nil)
				{
					if (ConstObj.as<bool>()) AddFlags |= FUNC_Const;
					else RemoveFlags |= FUNC_Const;
				}
				if (AddFlags != 0 || RemoveFlags != 0)
				{
					NeoBlueprint::SetFunctionFlags(Info.Blueprint, FFuncName, AddFlags, RemoveFlags);
				}

				// Params — accept both "params" and "inputs" keys
				auto AddParamsFromTable = [&](sol::table ParamTable)
				{
					for (auto& Pair : ParamTable)
					{
						if (Pair.second.is<sol::table>())
						{
							sol::table P = Pair.second.as<sol::table>();
							std::string PName = P.get_or<std::string>("name", "");
							std::string PType = P.get_or<std::string>("type", "");
							if (!PName.empty() && !PType.empty())
							{
								FString FPName = UTF8_TO_TCHAR(PName.c_str());
								FString FPType = UTF8_TO_TCHAR(PType.c_str());
								if (NeoBlueprint::AddFunctionParam(Info.Blueprint, FFuncName, FPName, FPType))
								{
									Session.Log(FString::Printf(TEXT("[OK] add_function param \"%s\" (%s)"), *FPName, *FPType));
								}
							}
						}
					}
				};
				sol::object ParamsObj = Opts["params"];
				if (!ParamsObj.valid() || !ParamsObj.is<sol::table>()) ParamsObj = Opts["inputs"];
				if (ParamsObj.valid() && ParamsObj.is<sol::table>())
				{
					AddParamsFromTable(ParamsObj.as<sol::table>());
				}

				// Returns — accept both "returns" and "outputs" keys
				auto AddReturnsFromTable = [&](sol::table ReturnTable)
				{
					for (auto& Pair : ReturnTable)
					{
						if (Pair.second.is<sol::table>())
						{
							sol::table R = Pair.second.as<sol::table>();
							std::string RName = R.get_or<std::string>("name", "");
							std::string RType = R.get_or<std::string>("type", "");
							if (!RName.empty() && !RType.empty())
							{
								FString FRName = UTF8_TO_TCHAR(RName.c_str());
								FString FRType = UTF8_TO_TCHAR(RType.c_str());
								if (NeoBlueprint::AddFunctionReturn(Info.Blueprint, FFuncName, FRName, FRType))
								{
									Session.Log(FString::Printf(TEXT("[OK] add_function return \"%s\" (%s)"), *FRName, *FRType));
								}
							}
						}
					}
				};
				sol::object ReturnsObj = Opts["returns"];
				if (!ReturnsObj.valid() || !ReturnsObj.is<sol::table>()) ReturnsObj = Opts["outputs"];
				if (ReturnsObj.valid() && ReturnsObj.is<sol::table>())
				{
					AddReturnsFromTable(ReturnsObj.as<sol::table>());
				}

				// Function metadata (category, description, keywords, compact_title, etc.)
				FKismetUserDeclaredFunctionMetadata* FuncMeta = FBlueprintEditorUtils::GetGraphFunctionMetaData(FuncGraph);
				if (FuncMeta)
				{
					bool bMetaChanged = false;

					// Category — set directly on metadata (SetBlueprintVariableCategory only works for variables)
					std::string Cat = Opts.get_or<std::string>("category", "");
					if (!Cat.empty())
					{
						FBlueprintEditorUtils::ModifyFunctionMetaData(FuncGraph);
						FuncMeta->Category = FText::FromString(UTF8_TO_TCHAR(Cat.c_str()));
						bMetaChanged = true;
					}

					std::string DescStr = Opts.get_or<std::string>("description", "");
					if (!DescStr.empty())
					{
						FBlueprintEditorUtils::ModifyFunctionMetaData(FuncGraph);
						FuncMeta->ToolTip = FText::FromString(UTF8_TO_TCHAR(DescStr.c_str()));
						bMetaChanged = true;
					}

					std::string KeywordsStr = Opts.get_or<std::string>("keywords", "");
					if (!KeywordsStr.empty())
					{
						if (!bMetaChanged) FBlueprintEditorUtils::ModifyFunctionMetaData(FuncGraph);
						FuncMeta->Keywords = FText::FromString(UTF8_TO_TCHAR(KeywordsStr.c_str()));
						bMetaChanged = true;
					}

					std::string CompactStr = Opts.get_or<std::string>("compact_title", "");
					if (!CompactStr.empty())
					{
						if (!bMetaChanged) FBlueprintEditorUtils::ModifyFunctionMetaData(FuncGraph);
						FuncMeta->CompactNodeTitle = FText::FromString(UTF8_TO_TCHAR(CompactStr.c_str()));
						bMetaChanged = true;
					}

					sol::object CallInEditorObj = Opts["call_in_editor"];
					if (CallInEditorObj.valid() && CallInEditorObj.get_type() != sol::type::lua_nil)
					{
						if (!bMetaChanged) FBlueprintEditorUtils::ModifyFunctionMetaData(FuncGraph);
						FuncMeta->bCallInEditor = CallInEditorObj.as<bool>();
						bMetaChanged = true;
					}

					if (bMetaChanged)
					{
						FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Info.Blueprint);
					}
				}
			}

			sol::table GraphObj = BuildGraphObject(lua, Session, FuncGraph, PathStr, FuncName);
			Session.Log(FString::Printf(TEXT("[OK] add_function(\"%s\") -> %s with %d nodes"),
				*FFuncName, bFuncExisted ? TEXT("updated") : TEXT("created"), FuncGraph->Nodes.Num()));
			return GraphObj;
		});

		// bp:set_property(name, props_table) — set metadata on existing variable or function
		BP.set_function("set_property", [&Session, FPath](sol::table /*self*/,
			const std::string& TargetName, sol::table Props, sol::this_state S) -> sol::object
		{
			FString FTarget = UTF8_TO_TCHAR(TargetName.c_str());

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_property -> blueprint not found")));
				return sol::lua_nil;
			}

			// Check if target is a function (has graph) — apply function flags + metadata
			UEdGraph* Graph = NeoBlueprint::FindGraph(Info.Blueprint, FTarget);
			if (Graph)
			{
				uint32 AddFlags = 0, RemoveFlags = 0;

				sol::object PureObj = Props["pure"];
				if (PureObj.valid() && PureObj.get_type() != sol::type::lua_nil)
				{
					if (PureObj.as<bool>()) AddFlags |= FUNC_BlueprintPure;
					else RemoveFlags |= FUNC_BlueprintPure;
				}

				sol::object ConstObj = Props["const_func"];
				if (ConstObj.valid() && ConstObj.get_type() != sol::type::lua_nil)
				{
					if (ConstObj.as<bool>()) AddFlags |= FUNC_Const;
					else RemoveFlags |= FUNC_Const;
				}

				if (AddFlags != 0 || RemoveFlags != 0)
				{
					NeoBlueprint::SetFunctionFlags(Info.Blueprint, FTarget, AddFlags, RemoveFlags);
					Session.Log(FString::Printf(TEXT("[OK] set_property(\"%s\") -> function flags updated"), *FTarget));
				}

				// Function metadata (category, description, keywords, compact_title, etc.)
				FKismetUserDeclaredFunctionMetadata* FuncMeta = FBlueprintEditorUtils::GetGraphFunctionMetaData(Graph);
				if (FuncMeta)
				{
					bool bMetaChanged = false;

					// Category — set directly on metadata (SetBlueprintVariableCategory only works for variables)
					sol::object CategoryObj = Props["category"];
					if (CategoryObj.valid() && CategoryObj.is<std::string>())
					{
						FBlueprintEditorUtils::ModifyFunctionMetaData(Graph);
						FuncMeta->Category = FText::FromString(UTF8_TO_TCHAR(CategoryObj.as<std::string>().c_str()));
						bMetaChanged = true;
					}

					sol::object DescObj = Props["description"];
					if (DescObj.valid() && DescObj.is<std::string>())
					{
						FBlueprintEditorUtils::ModifyFunctionMetaData(Graph);
						FuncMeta->ToolTip = FText::FromString(UTF8_TO_TCHAR(DescObj.as<std::string>().c_str()));
						bMetaChanged = true;
					}

					sol::object KeywordsObj = Props["keywords"];
					if (KeywordsObj.valid() && KeywordsObj.is<std::string>())
					{
						if (!bMetaChanged) FBlueprintEditorUtils::ModifyFunctionMetaData(Graph);
						FuncMeta->Keywords = FText::FromString(UTF8_TO_TCHAR(KeywordsObj.as<std::string>().c_str()));
						bMetaChanged = true;
					}

					sol::object CompactObj = Props["compact_title"];
					if (CompactObj.valid() && CompactObj.is<std::string>())
					{
						if (!bMetaChanged) FBlueprintEditorUtils::ModifyFunctionMetaData(Graph);
						FuncMeta->CompactNodeTitle = FText::FromString(UTF8_TO_TCHAR(CompactObj.as<std::string>().c_str()));
						bMetaChanged = true;
					}

					sol::object DeprecatedObj = Props["deprecated"];
					if (DeprecatedObj.valid() && DeprecatedObj.get_type() != sol::type::lua_nil)
					{
						if (!bMetaChanged) FBlueprintEditorUtils::ModifyFunctionMetaData(Graph);
						FuncMeta->bIsDeprecated = DeprecatedObj.as<bool>();
						bMetaChanged = true;
					}

					sol::object DeprecationMsgObj = Props["deprecation_message"];
					if (DeprecationMsgObj.valid() && DeprecationMsgObj.is<std::string>())
					{
						if (!bMetaChanged) FBlueprintEditorUtils::ModifyFunctionMetaData(Graph);
						FuncMeta->DeprecationMessage = UTF8_TO_TCHAR(DeprecationMsgObj.as<std::string>().c_str());
						bMetaChanged = true;
					}

					sol::object CallInEditorObj = Props["call_in_editor"];
					if (CallInEditorObj.valid() && CallInEditorObj.get_type() != sol::type::lua_nil)
					{
						if (!bMetaChanged) FBlueprintEditorUtils::ModifyFunctionMetaData(Graph);
						FuncMeta->bCallInEditor = CallInEditorObj.as<bool>();
						bMetaChanged = true;
					}

					sol::object ThreadSafeObj = Props["thread_safe"];
					if (ThreadSafeObj.valid() && ThreadSafeObj.get_type() != sol::type::lua_nil)
					{
						if (!bMetaChanged) FBlueprintEditorUtils::ModifyFunctionMetaData(Graph);
						FuncMeta->bThreadSafe = ThreadSafeObj.as<bool>();
						bMetaChanged = true;
					}

					if (bMetaChanged)
					{
						FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Info.Blueprint);
						Session.Log(FString::Printf(TEXT("[OK] set_property(\"%s\") -> function metadata updated"), *FTarget));
					}
				}
			}

			// Apply variable properties (works for any variable including delegates)
			auto SolToStr = [](const sol::object& Obj) -> FString
			{
				if (Obj.is<std::string>()) return UTF8_TO_TCHAR(Obj.as<std::string>().c_str());
				if (Obj.is<bool>()) return Obj.as<bool>() ? TEXT("true") : TEXT("false");
				if (Obj.is<double>())
				{
					double V = Obj.as<double>();
					if (FMath::IsNearlyEqual(V, FMath::RoundToDouble(V)))
						return FString::Printf(TEXT("%d"), (int64)V);
					return FString::Printf(TEXT("%g"), V);
				}
				return TEXT("");
			};

			static const TArray<const char*> VarKeys = {
				"category", "tooltip", "replicated", "rep_notify", "edit_anywhere",
				"edit_defaults_only", "edit_instance_only", "blueprint_read_only",
				"save_game", "transient", "expose_on_spawn"
			};

			bool bAnySet = false;
			FName VarName(*FTarget);
			for (const char* Key : VarKeys)
			{
				sol::object Val = Props[Key];
				if (Val.valid() && Val.get_type() != sol::type::lua_nil)
				{
					FString StrVal = SolToStr(Val);
					if (NeoBlueprint::SetVariableProperty(Info.Blueprint, FTarget, FString(Key), StrVal))
					{
						Session.Log(FString::Printf(TEXT("[OK] set_property(\"%s\", \"%s\") = %s"), *FTarget, UTF8_TO_TCHAR(Key), *StrVal));
						bAnySet = true;
					}
				}
			}

			// Interp flag (expose to Sequencer/Cinematics)
			sol::object InterpObj = Props["interp"];
			if (InterpObj.valid() && InterpObj.get_type() != sol::type::lua_nil)
			{
				FBlueprintEditorUtils::SetInterpFlag(Info.Blueprint, VarName, InterpObj.as<bool>());
				Session.Log(FString::Printf(TEXT("[OK] set_property(\"%s\", \"interp\") = %s"), *FTarget, InterpObj.as<bool>() ? TEXT("true") : TEXT("false")));
				bAnySet = true;
			}

			// Advanced Display flag
			sol::object AdvancedObj = Props["advanced_display"];
			if (AdvancedObj.valid() && AdvancedObj.get_type() != sol::type::lua_nil)
			{
				FBlueprintEditorUtils::SetVariableAdvancedDisplayFlag(Info.Blueprint, VarName, AdvancedObj.as<bool>());
				Session.Log(FString::Printf(TEXT("[OK] set_property(\"%s\", \"advanced_display\") = %s"), *FTarget, AdvancedObj.as<bool>() ? TEXT("true") : TEXT("false")));
				bAnySet = true;
			}

			// Deprecated flag
			sol::object DeprecatedObj = Props["deprecated"];
			if (DeprecatedObj.valid() && DeprecatedObj.get_type() != sol::type::lua_nil)
			{
				FBlueprintEditorUtils::SetVariableDeprecatedFlag(Info.Blueprint, VarName, DeprecatedObj.as<bool>());
				Session.Log(FString::Printf(TEXT("[OK] set_property(\"%s\", \"deprecated\") = %s"), *FTarget, DeprecatedObj.as<bool>() ? TEXT("true") : TEXT("false")));
				bAnySet = true;
			}

	
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				// Variable reordering
				sol::optional<std::string> MoveBeforeOpt = Props["move_before"];
				if (MoveBeforeOpt.has_value())
				{
					FString MoveBeforeTarget = UTF8_TO_TCHAR(MoveBeforeOpt.value().c_str());
					bool bOk = FBlueprintEditorUtils::MoveVariableBeforeVariable(Info.Blueprint, nullptr, VarName, FName(*MoveBeforeTarget), false);
					if (bOk) Session.Log(FString::Printf(TEXT("[OK] set_property(\"%s\", \"move_before\") = %s"), *FTarget, *MoveBeforeTarget));
					else Session.Log(FString::Printf(TEXT("[FAIL] set_property -> move_before '%s' failed"), *MoveBeforeTarget));
				}

				sol::optional<std::string> MoveAfterOpt = Props["move_after"];
				if (MoveAfterOpt.has_value())
				{
					FString MoveAfterTarget = UTF8_TO_TCHAR(MoveAfterOpt.value().c_str());
					bool bOk = FBlueprintEditorUtils::MoveVariableAfterVariable(Info.Blueprint, nullptr, VarName, FName(*MoveAfterTarget), false);
					if (bOk) Session.Log(FString::Printf(TEXT("[OK] set_property(\"%s\", \"move_after\") = %s"), *FTarget, *MoveAfterTarget));
					else Session.Log(FString::Printf(TEXT("[FAIL] set_property -> move_after '%s' failed"), *MoveAfterTarget));
				}
#else
				sol::optional<std::string> MoveBeforeOpt = Props["move_before"];
				sol::optional<std::string> MoveAfterOpt = Props["move_after"];
				if (MoveBeforeOpt.has_value() || MoveAfterOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] variable reordering (move_before/move_after) requires UE 5.5+"));
				}
#endif

// Arbitrary metadata (set or remove)
			sol::optional<sol::table> MetaData = Props.get<sol::optional<sol::table>>("metadata");
			if (MetaData.has_value())
			{
				for (auto& KV : MetaData.value())
				{
					if (!KV.first.is<std::string>()) continue;
					FString MetaKey = UTF8_TO_TCHAR(KV.first.as<std::string>().c_str());
					if (KV.second.is<std::string>())
					{
						FString MetaValue = UTF8_TO_TCHAR(KV.second.as<std::string>().c_str());
						FBlueprintEditorUtils::SetBlueprintVariableMetaData(Info.Blueprint, VarName, nullptr, FName(*MetaKey), MetaValue);
						Session.Log(FString::Printf(TEXT("[OK] set_property(\"%s\", metadata.\"%s\") = \"%s\""), *FTarget, *MetaKey, *MetaValue));
						bAnySet = true;
					}
					else if (KV.second.is<sol::lua_nil_t>())
					{
						FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(Info.Blueprint, VarName, nullptr, FName(*MetaKey));
						Session.Log(FString::Printf(TEXT("[OK] set_property(\"%s\", metadata.\"%s\") -> removed"), *FTarget, *MetaKey));
						bAnySet = true;
					}
				}
			}

			return sol::make_object(S, bAnySet || Graph != nullptr);
		});

		// bp:set(target, property, value) or bp:set(property, value) — universal property setter via reflection
		// target: component name, "self" for CDO, or variable name (defaults to "self" when omitted)
		// value: string or table (for struct types like Vector {x=,y=,z=}, Rotator {pitch=,yaw=,roll=}, Color {r=,g=,b=,a=})
		BP.set_function("set", [&Session, FPath](sol::table /*self*/,
			const std::string& FirstArg, sol::object SecondObj, sol::optional<sol::object> ThirdObj,
			sol::this_state S) -> sol::object
		{
			FString FTarget, FProp;
			sol::object ValueObj;
			if (ThirdObj.has_value())
			{
				// 3-arg form: set(target, property, value)
				FTarget = UTF8_TO_TCHAR(FirstArg.c_str());
				FProp = SecondObj.is<std::string>() ? UTF8_TO_TCHAR(SecondObj.as<std::string>().c_str()) : TEXT("");
				ValueObj = ThirdObj.value();
			}
			else
			{
				// 2-arg form: set(property, value) — target defaults to "self"
				FTarget = TEXT("self");
				FProp = UTF8_TO_TCHAR(FirstArg.c_str());
				ValueObj = SecondObj;
			}

			// Convert value to string — handle tables for struct types
			FString FValue;
			if (ValueObj.is<std::string>())
			{
				FValue = UTF8_TO_TCHAR(ValueObj.as<std::string>().c_str());
			}
			else if (ValueObj.is<bool>())
			{
				FValue = ValueObj.as<bool>() ? TEXT("true") : TEXT("false");
			}
			else if (ValueObj.is<double>())
			{
				double V = ValueObj.as<double>();
				if (FMath::IsNearlyEqual(V, FMath::RoundToDouble(V)))
					FValue = FString::Printf(TEXT("%lld"), (long long)V);
				else
					FValue = FString::Printf(TEXT("%g"), V);
			}
			else if (ValueObj.is<sol::table>())
			{
				sol::table T = ValueObj.as<sol::table>();
				// Detect struct type from table keys — use UE parenthesized struct format
				if (T["x"].valid() || T["X"].valid())
				{
					// Vector/Vector2D: {x=,y=,z=} -> (X=...,Y=...,Z=...)
					double X = T.get_or("x", T.get_or("X", 0.0));
					double Y = T.get_or("y", T.get_or("Y", 0.0));
					double Z = T.get_or("z", T.get_or("Z", 0.0));
					FValue = FString::Printf(TEXT("(X=%g,Y=%g,Z=%g)"), X, Y, Z);
				}
				else if (T["pitch"].valid() || T["Pitch"].valid())
				{
					// Rotator: {pitch=,yaw=,roll=} -> (Pitch=...,Yaw=...,Roll=...)
					double P = T.get_or("pitch", T.get_or("Pitch", 0.0));
					double Y = T.get_or("yaw", T.get_or("Yaw", 0.0));
					double R = T.get_or("roll", T.get_or("Roll", 0.0));
					FValue = FString::Printf(TEXT("(Pitch=%g,Yaw=%g,Roll=%g)"), P, Y, R);
				}
				else if (T["r"].valid() || T["R"].valid())
				{
					// Color: {r=,g=,b=,a=} -> (R=...,G=...,B=...,A=...)
					double R = T.get_or("r", T.get_or("R", 0.0));
					double G = T.get_or("g", T.get_or("G", 0.0));
					double B = T.get_or("b", T.get_or("B", 0.0));
					double A = T.get_or("a", T.get_or("A", 1.0));
					FValue = FString::Printf(TEXT("(R=%g,G=%g,B=%g,A=%g)"), R, G, B, A);
				}
				else
				{
					Session.Log(TEXT("[FAIL] set -> table value not recognized. Use {x=,y=,z=} for Vector, {pitch=,yaw=,roll=} for Rotator, {r=,g=,b=,a=} for Color"));
					return sol::lua_nil;
				}
			}
			else
			{
				Session.Log(TEXT("[FAIL] set -> value must be a string, number, bool, or table"));
				return sol::lua_nil;
			}

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set -> blueprint not found")));
				return sol::lua_nil;
			}

			UObject* TargetObj = nullptr;
			FString TargetDesc;

			if (FTarget.ToLower() == TEXT("self"))
			{
				// Blueprint CDO
				if (Info.Blueprint->GeneratedClass)
				{
					TargetObj = Info.Blueprint->GeneratedClass->GetDefaultObject();
					TargetDesc = TEXT("CDO");
				}
			}
			else
			{
				// Try component template first
				TargetObj = NeoBlueprint::GetComponentTemplate(Info.Blueprint, FTarget);
				if (TargetObj)
				{
					TargetDesc = FString::Printf(TEXT("component %s"), *FTarget);
				}
			}

			if (!TargetObj)
			{
				// Try custom event node
				UK2Node_CustomEvent* CE = NeoBlueprint::FindCustomEvent(Info.Blueprint, FTarget);
				if (CE)
				{
					FString PropLower = FProp.ToLower();
					if (PropLower == TEXT("replicated"))
					{
						// Clear existing net flags first
						CE->FunctionFlags &= ~(FUNC_Net | FUNC_NetMulticast | FUNC_NetServer | FUNC_NetClient);
						FString ValLower = FValue.ToLower();
						if (ValLower == TEXT("multicast")) CE->FunctionFlags |= (FUNC_Net | FUNC_NetMulticast);
						else if (ValLower == TEXT("server")) CE->FunctionFlags |= (FUNC_Net | FUNC_NetServer);
						else if (ValLower == TEXT("client")) CE->FunctionFlags |= (FUNC_Net | FUNC_NetClient);
					}
					else if (PropLower == TEXT("reliable"))
					{
						if (FValue.ToLower() == TEXT("true")) CE->FunctionFlags |= FUNC_NetReliable;
						else CE->FunctionFlags &= ~FUNC_NetReliable;
					}
					else if (PropLower == TEXT("call_in_editor"))
					{
						CE->bCallInEditor = (FValue.ToLower() == TEXT("true"));
					}
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Info.Blueprint);
					Session.Log(FString::Printf(TEXT("[OK] set(\"%s\", \"%s\") = \"%s\" (custom event)"),
						*FTarget, *FProp, *FValue));
					return sol::make_object(S, true);
				}

				// Try timeline template
				UTimelineTemplate* TLTemplate = Info.Blueprint->FindTimelineTemplateByVariableName(FName(*FTarget));
				if (TLTemplate)
				{
					FString PropLower = FProp.ToLower();
					if (PropLower == TEXT("length")) TLTemplate->TimelineLength = FCString::Atof(*FValue);
					else if (PropLower == TEXT("auto_play")) TLTemplate->bAutoPlay = (FValue.ToLower() == TEXT("true"));
					else if (PropLower == TEXT("loop")) TLTemplate->bLoop = (FValue.ToLower() == TEXT("true"));
					else if (PropLower == TEXT("replicated")) TLTemplate->bReplicated = (FValue.ToLower() == TEXT("true"));
					else if (PropLower == TEXT("ignore_time_dilation")) TLTemplate->bIgnoreTimeDilation = (FValue.ToLower() == TEXT("true"));
					else
					{
						Session.Log(FString::Printf(TEXT("[FAIL] set(\"%s\", \"%s\") -> unknown timeline property. Use: length, auto_play, loop, replicated, ignore_time_dilation"),
							*FTarget, *FProp));
						return sol::lua_nil;
					}
					// Sync node flags
					UK2Node_Timeline* TLNode = NeoBlueprint::FindTimelineNode(Info.Blueprint, FTarget);
					if (TLNode)
					{
						TLNode->bAutoPlay = TLTemplate->bAutoPlay;
						TLNode->bLoop = TLTemplate->bLoop;
						TLNode->bReplicated = TLTemplate->bReplicated;
						TLNode->bIgnoreTimeDilation = TLTemplate->bIgnoreTimeDilation;
					}
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Info.Blueprint);
					Session.Log(FString::Printf(TEXT("[OK] set(\"%s\", \"%s\") = \"%s\" (timeline)"),
						*FTarget, *FProp, *FValue));
					return sol::make_object(S, true);
				}

				// Try variable metadata via set_property
				if (NeoBlueprint::SetVariableProperty(Info.Blueprint, FTarget, FProp, FValue))
				{
					Session.Log(FString::Printf(TEXT("[OK] set(\"%s\", \"%s\") = \"%s\" (variable metadata)"),
						*FTarget, *FProp, *FValue));
					return sol::make_object(S, true);
				}

				// Try treating target as a variable name — set its default value on the CDO
				if (FProp.ToLower() == TEXT("default") && Info.Blueprint->GeneratedClass)
				{
					UObject* CDO = Info.Blueprint->GeneratedClass->GetDefaultObject();
					if (CDO)
					{
						FString CDOError;
						if (NeoBlueprint::SetObjectProperty(CDO, FTarget, FValue, CDOError))
						{
							Session.Log(FString::Printf(TEXT("[OK] set(\"%s\", \"default\") = \"%s\" (variable default on CDO)"),
								*FTarget, *FValue));
							return sol::make_object(S, true);
						}
					}
				}

				Session.Log(FString::Printf(TEXT("[FAIL] set(\"%s\") -> not found as component, CDO, custom event, timeline, or variable"),
					*FTarget));
				return sol::lua_nil;
			}

			FString Error;
			if (!NeoBlueprint::SetObjectProperty(TargetObj, FProp, FValue, Error))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set(\"%s\", \"%s\") -> %s"), *FTarget, *FProp, *Error));
				return sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[OK] set(\"%s\", \"%s\") = \"%s\" (%s)"),
				*FTarget, *FProp, *FValue, *TargetDesc));
			return sol::make_object(S, true);
		});

		// bp:get(target, property) or bp:get(property) — universal property getter
		// When called with 1 arg, target defaults to "self" (class defaults)
		BP.set_function("get", [&Session, FPath](sol::table /*self*/,
			const std::string& FirstArg, sol::optional<std::string> SecondArg,
			sol::this_state S) -> sol::object
		{
			FString FTarget, FProp;
			if (SecondArg.has_value())
			{
				FTarget = UTF8_TO_TCHAR(FirstArg.c_str());
				FProp = UTF8_TO_TCHAR(SecondArg.value().c_str());
			}
			else
			{
				FTarget = TEXT("self");
				FProp = UTF8_TO_TCHAR(FirstArg.c_str());
			}

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] get -> blueprint not found")));
				return sol::lua_nil;
			}

			UObject* TargetObj = nullptr;
			if (FTarget.ToLower() == TEXT("self"))
			{
				if (Info.Blueprint->GeneratedClass)
					TargetObj = Info.Blueprint->GeneratedClass->GetDefaultObject();
			}
			else
			{
				TargetObj = NeoBlueprint::GetComponentTemplate(Info.Blueprint, FTarget);
			}

			if (!TargetObj)
			{
				// Try treating target as a variable name — get its default value from the CDO
				if (FProp.ToLower() == TEXT("default") && Info.Blueprint->GeneratedClass)
				{
					UObject* CDO = Info.Blueprint->GeneratedClass->GetDefaultObject();
					if (CDO)
					{
						FString Value;
						if (NeoBlueprint::GetObjectProperty(CDO, FTarget, Value))
						{
							return sol::make_object(S, std::string(TCHAR_TO_UTF8(*Value)));
						}
					}
				}

				Session.Log(FString::Printf(TEXT("[FAIL] get(\"%s\") -> target not found"), *FTarget));
				return sol::lua_nil;
			}

			FString Value;
			if (!NeoBlueprint::GetObjectProperty(TargetObj, FProp, Value))
			{
				FString Error = NeoBlueprint::FuzzyMatchProperty(TargetObj, FProp);
				Session.Log(FString::Printf(TEXT("[FAIL] get(\"%s\", \"%s\") -> %s"), *FTarget, *FProp, *Error));
				return sol::lua_nil;
			}

			return sol::make_object(S, std::string(TCHAR_TO_UTF8(*Value)));
		});

		// bp:list_properties(target, filter?) — discover available properties
		BP.set_function("list_properties", [&Session, FPath](sol::table /*self*/,
			const std::string& Target, sol::optional<std::string> Filter,
			sol::this_state S) -> sol::object
		{
			sol::state_view LuaView(S);
			FString FTarget = UTF8_TO_TCHAR(Target.c_str());
			FString FFilter = Filter.has_value() ? UTF8_TO_TCHAR(Filter.value().c_str()) : TEXT("");

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] list_properties -> blueprint not found"));
				return sol::lua_nil;
			}

			UObject* TargetObj = nullptr;
			if (FTarget.ToLower() == TEXT("self"))
			{
				if (Info.Blueprint->GeneratedClass)
					TargetObj = Info.Blueprint->GeneratedClass->GetDefaultObject();
			}
			else
			{
				TargetObj = NeoBlueprint::GetComponentTemplate(Info.Blueprint, FTarget);
			}

			if (!TargetObj)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] list_properties(\"%s\") -> target not found"), *FTarget));
				return sol::lua_nil;
			}

			TArray<TPair<FString, FString>> Props;
			NeoBlueprint::ListObjectProperties(TargetObj, FFilter, Props);

			sol::table Result = LuaView.create_table();
			for (int32 i = 0; i < Props.Num(); i++)
			{
				sol::table Entry = LuaView.create_table();
				Entry["property"] = TCHAR_TO_UTF8(*Props[i].Key);
				Entry["value"] = TCHAR_TO_UTF8(*Props[i].Value);
				Result[i + 1] = Entry;
			}

			Session.Log(FString::Printf(TEXT("[OK] list_properties(\"%s\"%s) -> %d properties"),
				*FTarget, FFilter.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", \"%s\""), *FFilter), Props.Num()));
			return Result;
		});

		// bp:remove(name) — unified remove: detects variable/function/component
		// bp:remove(name) or bp:remove("component", name) — unified remove
		BP.set_function("remove", [&Session, FPath](sol::table /*self*/,
			const std::string& NameOrType, sol::optional<std::string> OptName, sol::this_state S) -> sol::object
		{
			// Support two-arg format: remove("component", "MyAudio") where first arg is a type hint
			FString FName;
			if (OptName.has_value())
			{
				FName = UTF8_TO_TCHAR(OptName->c_str());
			}
			else
			{
				FName = UTF8_TO_TCHAR(NameOrType.c_str());
			}

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] remove -> blueprint not found"));
				return sol::lua_nil;
			}

			// Try component first
			if (NeoBlueprint::RemoveComponent(Info.Blueprint, FName))
			{
				Session.Log(FString::Printf(TEXT("[OK] remove(\"%s\") -> component removed"), *FName));
				return sol::make_object(S, true);
			}

			// Try function graph
			if (NeoBlueprint::RemoveFunction(Info.Blueprint, FName))
			{
				Session.Log(FString::Printf(TEXT("[OK] remove(\"%s\") -> function removed"), *FName));
				return sol::make_object(S, true);
			}

			// Try variable (handles event dispatchers too)
			if (NeoBlueprint::RemoveVariable(Info.Blueprint, FName))
			{
				Session.Log(FString::Printf(TEXT("[OK] remove(\"%s\") -> variable removed"), *FName));
				return sol::make_object(S, true);
			}

			// Try timeline
			if (NeoBlueprint::RemoveTimeline(Info.Blueprint, FName))
			{
				Session.Log(FString::Printf(TEXT("[OK] remove(\"%s\") -> timeline removed"), *FName));
				return sol::make_object(S, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> not found as component, function, variable, or timeline"), *FName));
			return sol::lua_nil;
		});

		// bp:rename(old_name, new_name) — unified rename: detects variable/function
		BP.set_function("rename", [&Session, FPath](sol::table /*self*/,
			const std::string& OldName, const std::string& NewName,
			sol::this_state S) -> sol::object
		{
			FString FOld = UTF8_TO_TCHAR(OldName.c_str());
			FString FNew = UTF8_TO_TCHAR(NewName.c_str());

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] rename -> blueprint not found"));
				return sol::lua_nil;
			}

			// Try variable rename first (handles delegates too)
			if (NeoBlueprint::RenameVariable(Info.Blueprint, FOld, FNew))
			{
				Session.Log(FString::Printf(TEXT("[OK] rename(\"%s\") -> \"%s\" (variable)"), *FOld, *FNew));
				return sol::make_object(S, true);
			}

			// Try function rename
			if (NeoBlueprint::RenameFunction(Info.Blueprint, FOld, FNew))
			{
				Session.Log(FString::Printf(TEXT("[OK] rename(\"%s\") -> \"%s\" (function)"), *FOld, *FNew));
				return sol::make_object(S, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] rename(\"%s\") -> not found as variable or function"), *FOld));
			return sol::lua_nil;
		});

		// bp:add_interface(name) — implement a Blueprint Interface
		BP.set_function("add_interface", [&Session, FPath](sol::table /*self*/,
			const std::string& InterfaceName, sol::this_state S) -> sol::object
		{
			FString FName = UTF8_TO_TCHAR(InterfaceName.c_str());

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] add_interface -> blueprint not found"));
				return sol::lua_nil;
			}

			if (!NeoBlueprint::AddInterface(Info.Blueprint, FName))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_interface(\"%s\") -> interface not found or already implemented"), *FName));
				return sol::lua_nil;
			}

			// Count the generated function stubs
			int32 FuncCount = 0;
			for (const FBPInterfaceDescription& Desc : Info.Blueprint->ImplementedInterfaces)
			{
				if (Desc.Interface && Desc.Interface->GetName().Contains(FName.Replace(TEXT("/Game/"), TEXT(""))))
				{
					FuncCount = Desc.Graphs.Num();
					break;
				}
			}

			Session.Log(FString::Printf(TEXT("[OK] add_interface(\"%s\") -> implemented with %d function stubs"),
				*FName, FuncCount));
			return sol::make_object(S, true);
		});

		// bp:remove_interface(name, preserve_functions?) — remove an implemented interface
		BP.set_function("remove_interface", [&Session, FPath](sol::table /*self*/,
			const std::string& InterfaceName, sol::optional<bool> Preserve, sol::this_state S) -> sol::object
		{
			FString FName = UTF8_TO_TCHAR(InterfaceName.c_str());
			bool bPreserve = Preserve.value_or(false);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] remove_interface -> blueprint not found"));
				return sol::lua_nil;
			}

			if (!NeoBlueprint::RemoveInterface(Info.Blueprint, FName, bPreserve))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_interface(\"%s\") -> not found or not implemented"), *FName));
				return sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[OK] remove_interface(\"%s\")%s"),
				*FName, bPreserve ? TEXT(" (functions preserved)") : TEXT("")));
			return sol::make_object(S, true);
		});

		// bp:add_macro(name) — create a macro graph
		BP.set_function("add_macro", [&Session, FPath, PathStr](sol::table /*self*/,
			const std::string& MacroName, sol::this_state S) -> sol::object
		{
			sol::state_view lua(S);
			FString FMacroName = UTF8_TO_TCHAR(MacroName.c_str());

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] add_macro -> blueprint not found"));
				return sol::lua_nil;
			}

			UEdGraph* MacroGraph = NeoBlueprint::AddMacroGraph(Info.Blueprint, FMacroName);
			if (!MacroGraph)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_macro(\"%s\") -> failed"), *FMacroName));
				return sol::lua_nil;
			}

			sol::table GraphObj = BuildGraphObject(lua, Session, MacroGraph, PathStr, MacroName);
			Session.Log(FString::Printf(TEXT("[OK] add_macro(\"%s\") -> created with %d nodes"),
				*FMacroName, MacroGraph->Nodes.Num()));
			return GraphObj;
		});

		// bp:add_custom_event(name, options?) — create a custom event node in EventGraph
		// Options: {params={{name,type},...}, replicated="multicast"|"server"|"client", reliable=bool, call_in_editor=bool}
		// Also accepts: add_custom_event(name, {{name,type},...}) — direct param array
		BP.set_function("add_custom_event", [&Session, FPath](sol::table /*self*/,
			const std::string& EventName, sol::optional<sol::table> Options, sol::this_state S) -> sol::object
		{
			sol::state_view lua(S);
			FString FEventName = UTF8_TO_TCHAR(EventName.c_str());

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] add_custom_event -> blueprint not found"));
				return sol::lua_nil;
			}

			// Helper: parse a param array table into TArray<FParamDesc>
			auto ParseParamArray = [](sol::table ParamTable, TArray<FParamDesc>& OutParams)
			{
				for (auto& Pair : ParamTable)
				{
					if (Pair.second.is<sol::table>())
					{
						sol::table P = Pair.second.as<sol::table>();
						FParamDesc Desc;
						Desc.Name = UTF8_TO_TCHAR(P.get_or<std::string>("name", "").c_str());
						Desc.Type = UTF8_TO_TCHAR(P.get_or<std::string>("type", "").c_str());
						if (!Desc.Name.IsEmpty() && !Desc.Type.IsEmpty())
						{
							OutParams.Add(MoveTemp(Desc));
						}
					}
				}
			};

			// Parse params from options
			TArray<FParamDesc> Params;
			if (Options.has_value())
			{
				sol::table Opts = Options.value();
				sol::object ParamsObj = Opts["params"];
				if (ParamsObj.valid() && ParamsObj.is<sol::table>())
				{
					// Explicit {params={{name,type},...}} format
					ParseParamArray(ParamsObj.as<sol::table>(), Params);
				}
				else
				{
					// Check if the table itself is a direct param array: {{name,type},...}
					sol::object FirstEntry = Opts[1];
					if (FirstEntry.valid() && FirstEntry.is<sol::table>())
					{
						sol::table FirstTable = FirstEntry.as<sol::table>();
						if (FirstTable["name"].valid() && FirstTable["type"].valid())
						{
							ParseParamArray(Opts, Params);
						}
					}
				}
			}

			bool bExisted = (NeoBlueprint::FindCustomEvent(Info.Blueprint, FEventName) != nullptr);
			UK2Node_CustomEvent* EventNode = NeoBlueprint::AddCustomEvent(Info.Blueprint, FEventName, Params);
			if (!EventNode)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_custom_event(\"%s\") -> failed (no EventGraph?)"), *FEventName));
				return sol::lua_nil;
			}

			// Apply options
			if (Options.has_value())
			{
				sol::table Opts = Options.value();

				// RPC replication
				std::string RepStr = Opts.get_or<std::string>("replicated", "");
				if (!RepStr.empty())
				{
					FString Rep = FString(UTF8_TO_TCHAR(RepStr.c_str())).ToLower();
					if (Rep == TEXT("multicast"))
					{
						EventNode->FunctionFlags |= (FUNC_Net | FUNC_NetMulticast);
					}
					else if (Rep == TEXT("server"))
					{
						EventNode->FunctionFlags |= (FUNC_Net | FUNC_NetServer);
					}
					else if (Rep == TEXT("client"))
					{
						EventNode->FunctionFlags |= (FUNC_Net | FUNC_NetClient);
					}
				}

				// Reliable
				if (Opts.get_or("reliable", false))
				{
					EventNode->FunctionFlags |= FUNC_NetReliable;
				}

				// Call in editor
				if (Opts.get_or("call_in_editor", false))
				{
					EventNode->bCallInEditor = true;
				}
			}

			// Register in session node map
			FString NodeGuid = EventNode->NodeGuid.ToString();
			Session.Nodes.Add(NodeGuid, EventNode);

			Session.Log(FString::Printf(TEXT("[OK] add_custom_event(\"%s\") -> %s with %d params, handle=\"%s\""),
				*FEventName, bExisted ? TEXT("updated") : TEXT("created"), Params.Num(), *NodeGuid));

			sol::table Result = lua.create_table();
			Result["handle"] = TCHAR_TO_UTF8(*NodeGuid);
			Result["name"] = EventName;
			return Result;
		});

		// bp:add_timeline(name, options?) — create a timeline node with tracks
		// Options: {length=5.0, auto_play=bool, loop=bool, replicated=bool, ignore_time_dilation=bool,
		//   tracks={{name="Alpha", type="float", keys={{0,0},{5,1}}}, {name="OnHit", type="event", keys={2.5}}}}
		BP.set_function("add_timeline", [&Session, FPath](sol::table /*self*/,
			const std::string& TimelineName, sol::optional<sol::table> Options, sol::this_state S) -> sol::object
		{
			sol::state_view lua(S);
			FString FTLName = UTF8_TO_TCHAR(TimelineName.c_str());

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] add_timeline -> blueprint not found"));
				return sol::lua_nil;
			}

			float Length = 5.0f;
			bool bAutoPlay = false;
			bool bLoop = false;
			TArray<NeoBlueprint::FTimelineTrackDesc> Tracks;

			if (Options.has_value())
			{
				sol::table Opts = Options.value();
				Length = Opts.get_or("length", 5.0f);
				bAutoPlay = Opts.get_or("auto_play", false);
				bLoop = Opts.get_or("loop", false);

				// Helper lambda: parse a track table into a FTimelineTrackDesc
				auto ParseTrackTable = [](sol::table T, const FString& DefaultType) -> NeoBlueprint::FTimelineTrackDesc
				{
					NeoBlueprint::FTimelineTrackDesc Desc;
					Desc.Name = UTF8_TO_TCHAR(T.get_or<std::string>("name", "").c_str());
					Desc.Type = UTF8_TO_TCHAR(T.get_or<std::string>("type", TCHAR_TO_UTF8(*DefaultType)).c_str());

					sol::object KeysObj = T["keys"];
					if (KeysObj.valid() && KeysObj.is<sol::table>())
					{
						for (auto& KPair : KeysObj.as<sol::table>())
						{
							if (KPair.second.is<sol::table>())
							{
								sol::table KT = KPair.second.as<sol::table>();
								float Time = KT.get_or(1, 0.0f);
								sol::object ValObj = KT[2];
								FString ValStr;
								if (ValObj.valid())
								{
									if (ValObj.is<double>())
										ValStr = FString::Printf(TEXT("%f"), ValObj.as<double>());
									else if (ValObj.is<std::string>())
										ValStr = UTF8_TO_TCHAR(ValObj.as<std::string>().c_str());
								}
								Desc.Keys.Add(TPair<float, FString>(Time, ValStr));
							}
							else if (KPair.second.is<double>())
							{
								Desc.Keys.Add(TPair<float, FString>(KPair.second.as<float>(), TEXT("0")));
							}
						}
					}
					return Desc;
				};

				auto ParseTrackArray = [&](const char* Key, const FString& DefaultType)
				{
					sol::object Obj = Opts[Key];
					if (Obj.valid() && Obj.is<sol::table>())
					{
						for (auto& Pair : Obj.as<sol::table>())
						{
							if (!Pair.second.is<sol::table>()) continue;
							auto Desc = ParseTrackTable(Pair.second.as<sol::table>(), DefaultType);
							if (!Desc.Name.IsEmpty()) Tracks.Add(MoveTemp(Desc));
						}
					}
				};

				// Parse typed track arrays (float_tracks, vector_tracks, etc.)
				ParseTrackArray("float_tracks", TEXT("float"));
				ParseTrackArray("vector_tracks", TEXT("vector"));
				ParseTrackArray("color_tracks", TEXT("color"));
				ParseTrackArray("event_tracks", TEXT("event"));

				// Parse generic "tracks" array (type specified per-track)
				ParseTrackArray("tracks", TEXT("float"));
			}

			bool bTLExisted = (Info.Blueprint->FindTimelineTemplateByVariableName(FName(*FTLName)) != nullptr);
			UK2Node_Timeline* TimelineNode = NeoBlueprint::AddTimeline(
				Info.Blueprint, FTLName, Length, bAutoPlay, bLoop, Tracks);
			if (!TimelineNode)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_timeline(\"%s\") -> failed"), *FTLName));
				return sol::lua_nil;
			}

			// Apply extra options after creation — set on template (authoritative) then sync to node
			if (Options.has_value())
			{
				sol::table Opts = Options.value();
				UTimelineTemplate* TLTemplate = Info.Blueprint->FindTimelineTemplateByVariableName(FName(*FTLName));
				if (Opts.get_or("replicated", false))
				{
					if (TLTemplate) TLTemplate->bReplicated = true;
					TimelineNode->bReplicated = true;
				}
				if (Opts.get_or("ignore_time_dilation", false))
				{
					if (TLTemplate) TLTemplate->bIgnoreTimeDilation = true;
					TimelineNode->bIgnoreTimeDilation = true;
				}
			}

			// Register in session node map
			FString NodeGuid = TimelineNode->NodeGuid.ToString();
			Session.Nodes.Add(NodeGuid, TimelineNode);

			Session.Log(FString::Printf(TEXT("[OK] add_timeline(\"%s\") -> %s (%.1fs, %d tracks), handle=\"%s\""),
				*FTLName, bTLExisted ? TEXT("updated") : TEXT("created"), Length, Tracks.Num(), *NodeGuid));

			sol::table Result = lua.create_table();
			Result["handle"] = TCHAR_TO_UTF8(*NodeGuid);
			Result["name"] = TimelineName;
			return Result;
		});

		// ================================================================
		// AnimBP: bp:add_state_machine(name), bp:add_state(sm, name), bp:add_transition(sm, from, to)
		// ================================================================

		// bp:add_state_machine(name) -> {handle, name, graph}
		BP.set_function("add_state_machine", [&Session, FPath, PathStr](sol::table BP_Table,
			const std::string& SMName, sol::this_state S) -> sol::object
		{
			sol::state_view lua(S);
			FString FSMName = UTF8_TO_TCHAR(SMName.c_str());

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] add_state_machine -> blueprint not found"));
				return sol::lua_nil;
			}

			UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Info.Blueprint);
			if (!AnimBP)
			{
				Session.Log(TEXT("[FAIL] add_state_machine -> not an Animation Blueprint"));
				return sol::lua_nil;
			}

			UEdGraph* AnimGraph = NeoBlueprint::FindAnimGraph(AnimBP);
			if (!AnimGraph)
			{
				Session.Log(TEXT("[FAIL] add_state_machine -> AnimGraph not found. Open the AnimBP in the editor first."));
				return sol::lua_nil;
			}

			// Check for duplicate
			for (UEdGraphNode* Node : AnimGraph->Nodes)
			{
				if (UAnimGraphNode_StateMachine* Existing = Cast<UAnimGraphNode_StateMachine>(Node))
				{
					if (Existing->EditorStateMachineGraph &&
						Existing->EditorStateMachineGraph->GetName().Equals(FSMName, ESearchCase::IgnoreCase))
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add_state_machine(\"%s\") -> already exists"), *FSMName));
						return sol::lua_nil;
					}
				}
			}

			AnimGraph->Modify();

			UAnimGraphNode_StateMachine* NewSMNode = NewObject<UAnimGraphNode_StateMachine>(AnimGraph);
			NewSMNode->CreateNewGuid();
			AnimGraph->AddNode(NewSMNode, false, false);
			NewSMNode->SetFlags(RF_Transactional);
			NewSMNode->PostPlacedNewNode();
			NewSMNode->AllocateDefaultPins();

			UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(NewSMNode->EditorStateMachineGraph);
			if (!SMGraph)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_state_machine(\"%s\") -> graph creation failed"), *FSMName));
				return sol::lua_nil;
			}

			// Rename to requested name
			TSharedPtr<INameValidatorInterface> NameValidator = FNameValidatorFactory::MakeValidator(NewSMNode);
			FBlueprintEditorUtils::RenameGraphWithSuggestion(SMGraph, NameValidator, FSMName);

			// Ensure subgraph registration
			if (AnimGraph->SubGraphs.Find(SMGraph) == INDEX_NONE)
			{
				AnimGraph->Modify();
				AnimGraph->SubGraphs.Add(SMGraph);
			}

			NewSMNode->NodePosX = 200;
			NewSMNode->NodePosY = 0;

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

			FString NodeGuid = NewSMNode->NodeGuid.ToString();
			Session.Nodes.Add(NodeGuid, NewSMNode);
			Session.RegisterGraphNodes(SMGraph);

			// Also register in bp.graphs
			std::string GraphKey = TCHAR_TO_UTF8(*SMGraph->GetName());
			sol::table GraphsTable = BP_Table["graphs"];
			if (GraphsTable.valid())
			{
				sol::table GraphObj = BuildGraphObject(lua, Session, SMGraph, PathStr, GraphKey);
				GraphsTable[GraphKey] = GraphObj;
			}

			Session.Log(FString::Printf(TEXT("[OK] add_state_machine(\"%s\") -> created, handle=\"%s\""),
				*FSMName, *NodeGuid));

			sol::table Result = lua.create_table();
			Result["handle"] = TCHAR_TO_UTF8(*NodeGuid);
			Result["name"] = SMName;
			Result["graph"] = GraphKey;
			return Result;
		});

		// bp:add_state(state_machine_name, state_name) -> {handle, name, graph}
		BP.set_function("add_state", [&Session, FPath, PathStr](sol::table BP_Table,
			const std::string& SMName, const std::string& StateName, sol::this_state S) -> sol::object
		{
			sol::state_view lua(S);
			FString FSMName = UTF8_TO_TCHAR(SMName.c_str());
			FString FStateName = UTF8_TO_TCHAR(StateName.c_str());

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] add_state -> blueprint not found"));
				return sol::lua_nil;
			}

			UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Info.Blueprint);
			if (!AnimBP)
			{
				Session.Log(TEXT("[FAIL] add_state -> not an Animation Blueprint"));
				return sol::lua_nil;
			}

			// Find state machine node
			UEdGraph* AnimGraph = NeoBlueprint::FindAnimGraph(AnimBP);
			UAnimGraphNode_StateMachine* SMNode = nullptr;
			if (AnimGraph)
			{
				for (UEdGraphNode* Node : AnimGraph->Nodes)
				{
					if (UAnimGraphNode_StateMachine* SM = Cast<UAnimGraphNode_StateMachine>(Node))
					{
						if (SM->EditorStateMachineGraph &&
							SM->EditorStateMachineGraph->GetName().Equals(FSMName, ESearchCase::IgnoreCase))
						{
							SMNode = SM;
							break;
						}
					}
				}
			}

			if (!SMNode)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_state(\"%s\") -> state machine \"%s\" not found"),
					*FStateName, *FSMName));
				return sol::lua_nil;
			}

			UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
			if (!SMGraph)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_state(\"%s\") -> state machine graph not found"), *FStateName));
				return sol::lua_nil;
			}

			// Check for duplicate state
			for (UEdGraphNode* Node : SMGraph->Nodes)
			{
				if (UAnimStateNode* ExistingState = Cast<UAnimStateNode>(Node))
				{
					FString ExistingName = ExistingState->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
					if (ExistingName.Equals(FStateName, ESearchCase::IgnoreCase))
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add_state(\"%s\") -> already exists in \"%s\""),
							*FStateName, *FSMName));
						return sol::lua_nil;
					}
				}
			}

			SMGraph->Modify();

			UAnimStateNode* NewState = NewObject<UAnimStateNode>(SMGraph);
			NewState->CreateNewGuid();
			SMGraph->AddNode(NewState, false, false);
			NewState->SetFlags(RF_Transactional);
			NewState->PostPlacedNewNode();
			NewState->AllocateDefaultPins();

			if (!NewState->BoundGraph)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_state(\"%s\") -> bound graph not created"), *FStateName));
				return sol::lua_nil;
			}

			// Rename
			TSharedPtr<INameValidatorInterface> NameValidator = FNameValidatorFactory::MakeValidator(NewState);
			FBlueprintEditorUtils::RenameGraphWithSuggestion(NewState->BoundGraph, NameValidator, FStateName);

			// Position below existing states
			int32 MaxY = 0;
			for (UEdGraphNode* Node : SMGraph->Nodes)
			{
				if (UAnimStateNode* ExistingState = Cast<UAnimStateNode>(Node))
				{
					MaxY = FMath::Max(MaxY, static_cast<int32>(ExistingState->NodePosY));
				}
			}
			NewState->NodePosX = 300;
			NewState->NodePosY = MaxY + 150;

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

			FString NodeGuid = NewState->NodeGuid.ToString();
			Session.Nodes.Add(NodeGuid, NewState);
			Session.RegisterGraphNodes(NewState->BoundGraph);

			// Build selector name and register in bp.graphs
			FString SelectorName = FString::Printf(TEXT("%s/%s"), *SMGraph->GetName(), *FStateName);
			std::string GraphKey = TCHAR_TO_UTF8(*SelectorName);
			sol::table GraphsTable = BP_Table["graphs"];
			if (GraphsTable.valid())
			{
				sol::table GraphObj = BuildGraphObject(lua, Session, NewState->BoundGraph, PathStr, GraphKey);
				GraphsTable[GraphKey] = GraphObj;
			}

			Session.Log(FString::Printf(TEXT("[OK] add_state(\"%s\", \"%s\") -> created, handle=\"%s\", graph=\"%s\""),
				*FSMName, *FStateName, *NodeGuid, *SelectorName));

			sol::table Result = lua.create_table();
			Result["handle"] = TCHAR_TO_UTF8(*NodeGuid);
			Result["name"] = StateName;
			Result["graph"] = GraphKey;
			return Result;
		});

		// bp:add_transition(state_machine_name, from_state, to_state) -> {handle, graph, result_handle}
		BP.set_function("add_transition", [&Session, FPath, PathStr](sol::table BP_Table,
			const std::string& SMName, const std::string& FromState, const std::string& ToState,
			sol::this_state S) -> sol::object
		{
			sol::state_view lua(S);
			FString FSMName = UTF8_TO_TCHAR(SMName.c_str());
			FString FFromState = UTF8_TO_TCHAR(FromState.c_str());
			FString FToState = UTF8_TO_TCHAR(ToState.c_str());

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] add_transition -> blueprint not found"));
				return sol::lua_nil;
			}

			UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Info.Blueprint);
			if (!AnimBP)
			{
				Session.Log(TEXT("[FAIL] add_transition -> not an Animation Blueprint"));
				return sol::lua_nil;
			}

			// Find state machine
			UEdGraph* AnimGraph = NeoBlueprint::FindAnimGraph(AnimBP);
			UAnimGraphNode_StateMachine* SMNode = nullptr;
			if (AnimGraph)
			{
				for (UEdGraphNode* Node : AnimGraph->Nodes)
				{
					if (UAnimGraphNode_StateMachine* SM = Cast<UAnimGraphNode_StateMachine>(Node))
					{
						if (SM->EditorStateMachineGraph &&
							SM->EditorStateMachineGraph->GetName().Equals(FSMName, ESearchCase::IgnoreCase))
						{
							SMNode = SM;
							break;
						}
					}
				}
			}

			if (!SMNode)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_transition -> state machine \"%s\" not found"), *FSMName));
				return sol::lua_nil;
			}

			UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
			if (!SMGraph)
			{
				Session.Log(TEXT("[FAIL] add_transition -> state machine graph not found"));
				return sol::lua_nil;
			}

			// Find source and destination nodes
			UEdGraphNode* FromNode = nullptr;
			UAnimStateNodeBase* ToStateNode = nullptr;
			bool bFromEntry = false;

			if (FFromState.Equals(TEXT("[Entry]"), ESearchCase::IgnoreCase) ||
				FFromState.Equals(TEXT("Entry"), ESearchCase::IgnoreCase))
			{
				for (UEdGraphNode* Node : SMGraph->Nodes)
				{
					if (UAnimStateEntryNode* EntryNode = Cast<UAnimStateEntryNode>(Node))
					{
						FromNode = EntryNode;
						bFromEntry = true;
						break;
					}
				}
			}
			else
			{
				for (UEdGraphNode* Node : SMGraph->Nodes)
				{
					if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node))
					{
						FString Name = StateNode->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
						if (Name.Equals(FFromState, ESearchCase::IgnoreCase))
						{
							FromNode = StateNode;
							break;
						}
					}
				}
			}

			for (UEdGraphNode* Node : SMGraph->Nodes)
			{
				if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node))
				{
					FString Name = StateNode->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
					if (Name.Equals(FToState, ESearchCase::IgnoreCase))
					{
						ToStateNode = StateNode;
						break;
					}
				}
			}

			if (!FromNode)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_transition -> source \"%s\" not found"), *FFromState));
				return sol::lua_nil;
			}
			if (!ToStateNode)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_transition -> target \"%s\" not found"), *FToState));
				return sol::lua_nil;
			}

			// Create transition node
			UAnimStateTransitionNode* TransNode = NewObject<UAnimStateTransitionNode>(SMGraph);
			TransNode->CreateNewGuid();
			SMGraph->AddNode(TransNode, false, false);
			TransNode->SetFlags(RF_Transactional);
			TransNode->PostPlacedNewNode();
			TransNode->AllocateDefaultPins();

			// Position between source and target
			TransNode->NodePosX = (FromNode->NodePosX + ToStateNode->NodePosX) / 2;
			TransNode->NodePosY = (FromNode->NodePosY + ToStateNode->NodePosY) / 2;

			UAnimationTransitionGraph* TransGraph = Cast<UAnimationTransitionGraph>(TransNode->BoundGraph);
			if (!TransGraph)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_transition(\"%s\" -> \"%s\") -> transition graph not created"),
					*FFromState, *FToState));
				return sol::lua_nil;
			}

			if (TransNode->Pins.Num() < 2)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_transition(\"%s\" -> \"%s\") -> pins not created"),
					*FFromState, *FToState));
				return sol::lua_nil;
			}

			// Wire the transition
			SMGraph->Modify();

			if (bFromEntry)
			{
				UEdGraphPin* EntryOutput = nullptr;
				for (UEdGraphPin* Pin : FromNode->Pins)
				{
					if (Pin->Direction == EGPD_Output)
					{
						EntryOutput = Pin;
						break;
					}
				}
				UEdGraphPin* TransInput = TransNode->GetInputPin();
				UEdGraphPin* TransOutput = TransNode->GetOutputPin();
				UEdGraphPin* ToInput = ToStateNode->GetInputPin();

				if (EntryOutput && TransInput)
				{
					EntryOutput->Modify();
					TransInput->Modify();
					EntryOutput->MakeLinkTo(TransInput);
					if (UEdGraphNode* EntryNode = EntryOutput->GetOwningNode()) EntryNode->PinConnectionListChanged(EntryOutput);
					if (UEdGraphNode* TransInNode = TransInput->GetOwningNode()) TransInNode->PinConnectionListChanged(TransInput);
				}
				if (TransOutput && ToInput)
				{
					TransOutput->Modify();
					ToInput->Modify();
					TransOutput->MakeLinkTo(ToInput);
					if (UEdGraphNode* TransOutNode = TransOutput->GetOwningNode()) TransOutNode->PinConnectionListChanged(TransOutput);
					if (UEdGraphNode* ToInNode = ToInput->GetOwningNode()) ToInNode->PinConnectionListChanged(ToInput);
				}
			}
			else
			{
				UAnimStateNodeBase* FromStateNode = Cast<UAnimStateNodeBase>(FromNode);
				if (!FromStateNode)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_transition -> source \"%s\" is not a state node"), *FFromState));
					return sol::lua_nil;
				}
				TransNode->CreateConnections(FromStateNode, ToStateNode);
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

			FString TransGuid = TransNode->NodeGuid.ToString();
			Session.Nodes.Add(TransGuid, TransNode);
			Session.RegisterGraphNodes(TransGraph);

			// Get result node handle
			UAnimGraphNode_TransitionResult* ResultNode = TransGraph->MyResultNode;
			FString ResultGuid = ResultNode ? ResultNode->NodeGuid.ToString() : TEXT("none");
			if (ResultNode)
			{
				Session.Nodes.Add(ResultGuid, ResultNode);
			}

			// Build selector name and register in bp.graphs
			FString SelectorName = FString::Printf(TEXT("%s/%s->%s"), *SMGraph->GetName(), *FFromState, *FToState);
			std::string GraphKey = TCHAR_TO_UTF8(*SelectorName);
			sol::table GraphsTable = BP_Table["graphs"];
			if (GraphsTable.valid())
			{
				sol::table GraphObj = BuildGraphObject(lua, Session, TransGraph, PathStr, GraphKey);
				GraphsTable[GraphKey] = GraphObj;
			}

			Session.Log(FString::Printf(TEXT("[OK] add_transition(\"%s\", \"%s\" -> \"%s\") -> handle=\"%s\", graph=\"%s\", result=\"%s\""),
				*FSMName, *FFromState, *FToState, *TransGuid, *SelectorName, *ResultGuid));

			sol::table Result = lua.create_table();
			Result["handle"] = TCHAR_TO_UTF8(*TransGuid);
			Result["graph"] = GraphKey;
			Result["result_handle"] = TCHAR_TO_UTF8(*ResultGuid);
			return Result;
		});

		// bp:rename_variable(old_name, new_name)
		BP.set_function("rename_variable", [&Session, FPath](sol::table /*self*/,
			const std::string& OldName, const std::string& NewName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *FPath);
			if (!Blueprint) return sol::lua_nil;
			FString Old = UTF8_TO_TCHAR(OldName.c_str());
			FString New = UTF8_TO_TCHAR(NewName.c_str());
			FBlueprintEditorUtils::RenameMemberVariable(Blueprint, FName(Old), FName(New));
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			Session.Log(FString::Printf(TEXT("[OK] rename_variable(\"%s\" -> \"%s\")"), *Old, *New));
			return sol::make_object(Lua, true);
		});

		// bp:remove_variable(name)
		BP.set_function("remove_variable", [&Session, FPath](sol::table /*self*/,
			const std::string& VarName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *FPath);
			if (!Blueprint) return sol::lua_nil;
			FString Name = UTF8_TO_TCHAR(VarName.c_str());
			FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, FName(Name));
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			Session.Log(FString::Printf(TEXT("[OK] remove_variable(\"%s\")"), *Name));
			return sol::make_object(Lua, true);
		});

		// bp:reparent(new_parent_class_path)
		BP.set_function("reparent", [&Session, FPath](sol::table /*self*/,
			const std::string& ParentPath, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *FPath);
			if (!Blueprint) return sol::lua_nil;
			FString FParent = UTF8_TO_TCHAR(ParentPath.c_str());
			UClass* NewParent = FindObject<UClass>(nullptr, *FParent);
			if (!NewParent) NewParent = LoadObject<UClass>(nullptr, *FParent);
			// Try common short names: "Character" -> "/Script/Engine.Character"
			if (!NewParent && !FParent.Contains(TEXT(".")))
			{
				// Try /Script/Engine.ClassName
				NewParent = LoadObject<UClass>(nullptr, *(TEXT("/Script/Engine.") + FParent));
				// Try with A prefix (AActor, ACharacter, etc.)
				if (!NewParent && !FParent.StartsWith(TEXT("A")))
					NewParent = LoadObject<UClass>(nullptr, *(TEXT("/Script/Engine.A") + FParent));
				// Try /Script/UMG.ClassName for widget types
				if (!NewParent)
					NewParent = LoadObject<UClass>(nullptr, *(TEXT("/Script/UMG.U") + FParent));
				// Try FindFirstObject as last resort
				if (!NewParent)
					NewParent = FindFirstObject<UClass>(*FParent, EFindFirstObjectOptions::NativeFirst);
				if (!NewParent && !FParent.StartsWith(TEXT("A")))
					NewParent = FindFirstObject<UClass>(*(TEXT("A") + FParent), EFindFirstObjectOptions::NativeFirst);
			}
			if (!NewParent) { Session.Log(FString::Printf(TEXT("[FAIL] reparent -> class '%s' not found"), *FParent)); return sol::lua_nil; }
			if (NewParent == Blueprint->ParentClass)
			{
				Session.Log(FString::Printf(TEXT("[OK] reparent(\"%s\") -> already current parent"), *NewParent->GetName()));
				return sol::make_object(Lua, true);
			}

			const FScopedTransaction Transaction(NSLOCTEXT("AIK", "LuaReparent", "Reparent Blueprint"));

			// Mark blueprint and SCS for undo (mirrors engine ReparentBlueprint_NewParentChosen)
			Blueprint->Modify();
			if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
			{
				SCS->Modify();
				for (USCS_Node* Node : SCS->GetAllNodes())
				{
					if (Node) Node->Modify();
				}
			}

			Blueprint->ParentClass = NewParent;

			// Purge null graphs, upgrade cosmetically stale nodes
			FBlueprintEditorUtils::PurgeNullGraphs(Blueprint);
			FKismetEditorUtilities::UpgradeCosmeticallyStaleBlueprint(Blueprint);

			// Reconstruct all nodes to pick up new parent context
			FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

			// Conform sparse class data if applicable
			if (UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass))
			{
				if (UScriptStruct* SparseStruct = NewParent->GetSparseClassDataStruct())
				{
					BPGC->PrepareToConformSparseClassData(SparseStruct);
				}
			}

			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			Session.Log(FString::Printf(TEXT("[OK] reparent(\"%s\")"), *NewParent->GetName()));
			return sol::make_object(Lua, true);
		});

		// bp:add_comment(text, x?, y?, w?, h?) OR bp:add_comment({text=, x=, y=, ...})
		BP.set_function("add_comment", [&Session, FPath](sol::table /*self*/,
			sol::object Arg1, sol::optional<double> Arg2, sol::optional<double> Arg3,
			sol::optional<double> Arg4, sol::optional<double> Arg5, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *FPath);
			if (!Blueprint) return sol::lua_nil;

			std::string GraphName = "EventGraph";
			std::string Text = "Comment";
			double PosX = 0, PosY = 0, Width = 400, Height = 200;
			std::string Color;

			if (Arg1.is<sol::table>())
			{
				// Table format: {text=, graph=, x=, y=, width=, height=, color=}
				sol::table Params = Arg1.as<sol::table>();
				GraphName = Params.get_or<std::string>("graph", "EventGraph");
				Text = Params.get_or<std::string>("text", "Comment");
				PosX = Params.get<sol::optional<double>>("x").value_or(0.0);
				PosY = Params.get<sol::optional<double>>("y").value_or(0.0);
				Width = Params.get<sol::optional<double>>("width").value_or(400.0);
				Height = Params.get<sol::optional<double>>("height").value_or(200.0);
				Color = Params.get_or<std::string>("color", "");
			}
			else if (Arg1.is<std::string>())
			{
				// Positional format: add_comment(text, x?, y?, w?, h?)
				Text = Arg1.as<std::string>();
				PosX = Arg2.value_or(0.0);
				PosY = Arg3.value_or(0.0);
				Width = Arg4.value_or(400.0);
				Height = Arg5.value_or(200.0);
			}
			else
			{
				Session.Log(TEXT("[FAIL] add_comment -> first argument must be a string (text) or table ({text=, x=, ...})"));
				return sol::lua_nil;
			}

			// Find graph
			FString FGraphName = UTF8_TO_TCHAR(GraphName.c_str());
			UEdGraph* Graph = nullptr;
			for (UEdGraph* G : Blueprint->UbergraphPages)
				if (G->GetName().Equals(FGraphName, ESearchCase::IgnoreCase)) { Graph = G; break; }
			if (!Graph) for (UEdGraph* G : Blueprint->FunctionGraphs)
				if (G->GetName().Equals(FGraphName, ESearchCase::IgnoreCase)) { Graph = G; break; }
			if (!Graph) { Session.Log(TEXT("[FAIL] add_comment -> graph not found")); return sol::lua_nil; }

			Graph->SetFlags(RF_Transactional);
			Graph->Modify();
			UEdGraphNode_Comment* Comment = NewObject<UEdGraphNode_Comment>(Graph);
			Comment->CreateNewGuid();
			Comment->SetFlags(RF_Transactional);
			Comment->PostPlacedNewNode();
			Comment->NodeComment = UTF8_TO_TCHAR(Text.c_str());
			Comment->NodePosX = (int32)PosX;
			Comment->NodePosY = (int32)PosY;
			Comment->NodeWidth = (int32)Width;
			Comment->NodeHeight = (int32)Height;
			if (!Color.empty())
				Comment->CommentColor = FColor::FromHex(UTF8_TO_TCHAR(Color.c_str())).ReinterpretAsLinear();
			Graph->AddNode(Comment, false, false);
			Blueprint->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] add_comment(\"%s\")"), UTF8_TO_TCHAR(Text.c_str())));
			return sol::make_object(Lua, TCHAR_TO_UTF8(*Comment->NodeGuid.ToString()));
		});

		// bp:break_connection(from_handle, from_pin, to_handle, to_pin)
		BP.set_function("break_connection", [&Session, FPath](sol::table /*self*/,
			const std::string& FromHandle, const std::string& FromPinName,
			const std::string& ToHandle, const std::string& ToPinName,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *FPath);
			if (!Blueprint) return sol::lua_nil;

			FGuid FromGuid, ToGuid;
			FGuid::Parse(UTF8_TO_TCHAR(FromHandle.c_str()), FromGuid);
			FGuid::Parse(UTF8_TO_TCHAR(ToHandle.c_str()), ToGuid);

			// Find nodes across all graphs
			UEdGraphNode* FromNode = nullptr;
			UEdGraphNode* ToNode = nullptr;
			TArray<UEdGraph*> AllGraphs;
			Blueprint->GetAllGraphs(AllGraphs);
			for (UEdGraph* G : AllGraphs)
			{
				for (UEdGraphNode* N : G->Nodes)
				{
					if (N->NodeGuid == FromGuid) FromNode = N;
					if (N->NodeGuid == ToGuid) ToNode = N;
					if (FromNode && ToNode) break;
				}
				if (FromNode && ToNode) break;
			}
			if (!FromNode || !ToNode) { Session.Log(TEXT("[FAIL] break_connection -> node not found")); return sol::lua_nil; }

			FString FFromPin = UTF8_TO_TCHAR(FromPinName.c_str());
			FString FToPin = UTF8_TO_TCHAR(ToPinName.c_str());

			UEdGraphPin* SrcPin = nullptr;
			for (UEdGraphPin* P : FromNode->Pins)
			{
				FString Name = P->GetDisplayName().IsEmpty() ? P->PinName.ToString() : P->GetDisplayName().ToString();
				if (Name.Equals(FFromPin, ESearchCase::IgnoreCase)) { SrcPin = P; break; }
			}
			UEdGraphPin* DstPin = nullptr;
			for (UEdGraphPin* P : ToNode->Pins)
			{
				FString Name = P->GetDisplayName().IsEmpty() ? P->PinName.ToString() : P->GetDisplayName().ToString();
				if (Name.Equals(FToPin, ESearchCase::IgnoreCase)) { DstPin = P; break; }
			}
			if (!SrcPin || !DstPin) { Session.Log(TEXT("[FAIL] break_connection -> pin not found")); return sol::lua_nil; }

			if (SrcPin->LinkedTo.Contains(DstPin))
			{
				UEdGraphNode* SrcNode = SrcPin->GetOwningNode();
				UEdGraphNode* DstNode = DstPin->GetOwningNode();
				SrcPin->BreakLinkTo(DstPin);
				if (SrcNode) SrcNode->PinConnectionListChanged(SrcPin);
				if (DstNode) DstNode->PinConnectionListChanged(DstPin);
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
				Session.Log(TEXT("[OK] break_connection"));
				return sol::make_object(Lua, true);
			}
			Session.Log(TEXT("[FAIL] break_connection -> pins not connected"));
			return sol::lua_nil;
		});

		// bp:align_nodes(handles_array, axis, mode)
		BP.set_function("align_nodes", [&Session, FPath](sol::table /*self*/,
			sol::table Handles, const std::string& Axis, const std::string& Mode,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *FPath);
			if (!Blueprint) return sol::lua_nil;

			FString FAxis = UTF8_TO_TCHAR(Axis.c_str());
			FString FMode = UTF8_TO_TCHAR(Mode.c_str());
			bool bAxisX = FAxis.Equals(TEXT("x"), ESearchCase::IgnoreCase);

			// Collect nodes
			TArray<UEdGraph*> AllGraphs;
			Blueprint->GetAllGraphs(AllGraphs);
			TArray<UEdGraphNode*> Nodes;
			for (auto& [_, val] : Handles)
			{
				if (!val.is<std::string>()) continue;
				FGuid Guid;
				FGuid::Parse(UTF8_TO_TCHAR(val.as<std::string>().c_str()), Guid);
				for (UEdGraph* G : AllGraphs)
					for (UEdGraphNode* N : G->Nodes)
						if (N->NodeGuid == Guid) { Nodes.Add(N); break; }
			}
			if (Nodes.Num() < 2) { Session.Log(TEXT("[FAIL] align_nodes -> need at least 2 nodes")); return sol::lua_nil; }

			auto GetCoord = [bAxisX](UEdGraphNode* N) { return bAxisX ? N->NodePosX : N->NodePosY; };

			int32 Target = 0;
			if (FMode.Equals(TEXT("min"), ESearchCase::IgnoreCase))
			{
				Target = INT_MAX;
				for (auto* N : Nodes) Target = FMath::Min(Target, GetCoord(N));
			}
			else if (FMode.Equals(TEXT("max"), ESearchCase::IgnoreCase))
			{
				Target = INT_MIN;
				for (auto* N : Nodes) Target = FMath::Max(Target, GetCoord(N));
			}
			else // center
			{
				int64 Sum = 0;
				for (auto* N : Nodes) Sum += GetCoord(N);
				Target = (int32)(Sum / Nodes.Num());
			}

			for (auto* N : Nodes)
			{
				N->Modify();
				if (bAxisX) N->NodePosX = Target;
				else N->NodePosY = Target;
			}
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			Session.Log(FString::Printf(TEXT("[OK] align_nodes(%d nodes, %s, %s)"), Nodes.Num(), *FAxis, *FMode));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// Widget Blueprint operations
		// ================================================================

		// bp:add_widget(type, name, parent?) — add a widget to widget tree
		BP.set_function("add_widget", [&Session, FPath](sol::table /*self*/,
			const std::string& WidgetType, const std::string& WidgetName,
			sol::optional<std::string> ParentName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(WidgetType.c_str());
			FString FWidgetName = UTF8_TO_TCHAR(WidgetName.c_str());
			FString FParent = ParentName.has_value() ? UTF8_TO_TCHAR(ParentName.value().c_str()) : TEXT("");

			UWidgetBlueprint* WidgetBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WidgetBP)
			{
				Session.Log(TEXT("[FAIL] add_widget -> not a Widget Blueprint"));
				return sol::lua_nil;
			}

			UWidgetTree* WidgetTree = WidgetBP->WidgetTree;
			if (!WidgetTree)
			{
				WidgetTree = NewObject<UWidgetTree>(WidgetBP, TEXT("WidgetTree"));
				WidgetBP->WidgetTree = WidgetTree;
			}

			// Check duplicate
			if (WidgetTree->FindWidget(FName(*FWidgetName)))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_widget(\"%s\") -> already exists"), *FWidgetName));
				return sol::lua_nil;
			}

			// Resolve widget class
			UClass* WidgetClass = nullptr;
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (It->IsChildOf(UWidget::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract))
				{
					FString ClassName = It->GetName();
					if (ClassName.Equals(FType, ESearchCase::IgnoreCase) ||
						ClassName.Equals(FType + TEXT("Widget"), ESearchCase::IgnoreCase))
					{
						WidgetClass = *It;
						break;
					}
				}
			}
			if (!WidgetClass)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_widget -> class '%s' not found"), *FType));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaAddWidget", "Add Widget"));
			WidgetTree->SetFlags(RF_Transactional);
			WidgetTree->Modify();

			UWidget* NewWidget = WidgetTree->ConstructWidget<UWidget>(WidgetClass, FName(*FWidgetName));
			if (!NewWidget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_widget(\"%s\") -> construction failed"), *FWidgetName));
				return sol::lua_nil;
			}

			NewWidget->CreatedFromPalette();

			if (FParent.IsEmpty() && !WidgetTree->RootWidget)
			{
				WidgetTree->RootWidget = NewWidget;
			}
			else
			{
				UPanelWidget* ParentWidget = nullptr;
				if (!FParent.IsEmpty())
				{
					ParentWidget = Cast<UPanelWidget>(WidgetTree->FindWidget(FName(*FParent)));
					if (!ParentWidget)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add_widget -> parent '%s' not found or not a panel"), *FParent));
						return sol::lua_nil;
					}
				}
				else
				{
					ParentWidget = Cast<UPanelWidget>(WidgetTree->RootWidget);
				}

				if (!ParentWidget)
				{
					Session.Log(TEXT("[FAIL] add_widget -> no panel parent available. Root must be a panel widget."));
					return sol::lua_nil;
				}

				ParentWidget->SetFlags(RF_Transactional);
				ParentWidget->Modify();
				UPanelSlot* Slot = ParentWidget->AddChild(NewWidget);
				if (!Slot)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_widget(\"%s\") -> AddChild failed"), *FWidgetName));
					return sol::lua_nil;
				}
			}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			WidgetBP->OnVariableAdded(NewWidget->GetFName());
#endif
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

			Session.Log(FString::Printf(TEXT("[OK] add_widget(\"%s\", type=\"%s\", parent=\"%s\")"),
				*FWidgetName, *FType, *FParent));

			sol::table Result = Lua.create_table();
			Result["name"] = WidgetName;
			Result["type"] = TCHAR_TO_UTF8(*WidgetClass->GetName());
			return Result;
		});

		// bp:remove_widget(name) — remove a widget from widget tree
		// Engine parity: matches WidgetBlueprintEditorUtils::DeleteWidgets flow
		BP.set_function("remove_widget", [&Session, FPath](sol::table /*self*/,
			const std::string& WidgetName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FWidgetName = UTF8_TO_TCHAR(WidgetName.c_str());

			UWidgetBlueprint* WidgetBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WidgetBP || !WidgetBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] remove_widget -> not a Widget Blueprint or no widget tree"));
				return sol::lua_nil;
			}

			UWidget* Widget = WidgetBP->WidgetTree->FindWidget(FName(*FWidgetName));
			if (!Widget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_widget(\"%s\") -> not found"), *FWidgetName));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaRemoveWidget", "Remove Widget"));

			// Engine: WidgetTree + Blueprint both get Modify for undo tracking
			WidgetBP->WidgetTree->SetFlags(RF_Transactional);
			WidgetBP->WidgetTree->Modify();
			WidgetBP->Modify();

			Widget->SetFlags(RF_Transactional);

			const FName WName = Widget->GetFName();

			// Engine: clean up bindings referencing this widget
			FString WidgetNameStr = WName.ToString();
			for (int32 i = WidgetBP->Bindings.Num() - 1; i >= 0; --i)
			{
				if (WidgetBP->Bindings[i].ObjectName == WidgetNameStr)
				{
					WidgetBP->Bindings.RemoveAt(i);
				}
			}

			// Engine: modify parent before RemoveWidget for undo tracking
			UPanelWidget* Parent = Widget->GetParent();
			if (Parent)
			{
				Parent->SetFlags(RF_Transactional);
				Parent->Modify();
			}

			Widget->Modify();

			if (Widget == WidgetBP->WidgetTree->RootWidget)
			{
				WidgetBP->WidgetTree->RootWidget = nullptr;
			}
			else
			{
				WidgetBP->WidgetTree->RemoveWidget(Widget);

				// Engine: if no parent, check named slots
				// FindAndRemoveNamedSlotContent is private, so check manually via INamedSlotInterface
				if (Widget->GetParent() == nullptr)
				{
					WidgetBP->WidgetTree->ForEachWidget([&](UWidget* W)
					{
						if (INamedSlotInterface* SlotHost = Cast<INamedSlotInterface>(W))
						{
							TArray<FName> SlotNames;
							SlotHost->GetSlotNames(SlotNames);
							for (const FName& SlotName : SlotNames)
							{
								if (SlotHost->GetContentForSlot(SlotName) == Widget)
								{
									SlotHost->SetContentForSlot(SlotName, nullptr);
								}
							}
						}
					});
				}
			}

			// Engine: remove graph variable nodes if widget is used in graph
			FBlueprintEditorUtils::RemoveVariableNodes(WidgetBP, WName);

			// Engine: replace desired focus references
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			FWidgetBlueprintEditorUtils::ReplaceDesiredFocus(WidgetBP, WName, FName());
#endif

			// Engine: rename to transient package to avoid name conflicts
			Widget->Rename(nullptr, GetTransientPackage());

			// Engine: only call OnVariableRemoved if no other widget has the same name
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			const bool bHasWidgetWithSameName = WidgetBP->GetAllSourceWidgets().ContainsByPredicate(
				[WName](const UWidget* W) { return WName == W->GetFName(); });
			if (!bHasWidgetWithSameName)
			{
				WidgetBP->OnVariableRemoved(WName);
			}
#endif

			// Engine: recursively clean up all child widgets
			TArray<UWidget*> ChildWidgets;
			UWidgetTree::GetChildWidgets(Widget, ChildWidgets);
			for (UWidget* ChildWidget : ChildWidgets)
			{
				const FName ChildName = ChildWidget->GetFName();
				ChildWidget->SetFlags(RF_Transactional);
				ChildWidget->Modify();

				FBlueprintEditorUtils::RemoveVariableNodes(WidgetBP, ChildName);
				ChildWidget->Rename(nullptr, GetTransientPackage());

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
				const bool bHasChildWithSameName = WidgetBP->GetAllSourceWidgets().ContainsByPredicate(
					[ChildName](const UWidget* W) { return ChildName == W->GetFName(); });
				if (!bHasChildWithSameName)
				{
					WidgetBP->OnVariableRemoved(ChildName);
				}
#endif
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

			Session.Log(FString::Printf(TEXT("[OK] remove_widget(\"%s\")"), *FWidgetName));
			return sol::make_object(S, true);
		});

		// bp:configure_widget(name, properties?, slot?) — set properties on a widget
		BP.set_function("configure_widget", [&Session, FPath](sol::table /*self*/,
			const std::string& WidgetName, sol::optional<sol::table> Properties,
			sol::optional<sol::table> SlotProps, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FWidgetName = UTF8_TO_TCHAR(WidgetName.c_str());

			UWidgetBlueprint* WidgetBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WidgetBP || !WidgetBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] configure_widget -> not a Widget Blueprint or no widget tree"));
				return sol::lua_nil;
			}

			UWidget* Widget = WidgetBP->WidgetTree->FindWidget(FName(*FWidgetName));
			if (!Widget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure_widget(\"%s\") -> not found"), *FWidgetName));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaConfigWidget", "Configure Widget"));
			Widget->Modify();
			int32 PropsSet = 0;

			auto SetPropertyFromString = [&](UObject* Obj, const FString& PropName, const FString& Value) -> bool
			{
				FProperty* Prop = nullptr;
				for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
				{
					if (It->GetName().Equals(PropName, ESearchCase::IgnoreCase))
					{
						Prop = *It;
						break;
					}
				}
				if (!Prop)
				{
					Session.Log(FString::Printf(TEXT("[WARN] configure_widget -> property '%s' not found on '%s'"),
						*PropName, *Obj->GetClass()->GetName()));
					return false;
				}
				// Engine parity: PreEditChange before ImportText, PostEditChangeProperty with actual property
				Obj->PreEditChange(Prop);
				void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Obj);
				if (Prop->ImportText_Direct(*Value, ValuePtr, Obj, PPF_None))
				{
					FPropertyChangedEvent PropEvent(Prop, EPropertyChangeType::ValueSet);
					Obj->PostEditChangeProperty(PropEvent);
					PropsSet++;
					return true;
				}
				// Must close the PreEditChange bracket even on failure
				FPropertyChangedEvent FailEvent(Prop, EPropertyChangeType::Unspecified);
				Obj->PostEditChangeProperty(FailEvent);
				Session.Log(FString::Printf(TEXT("[WARN] configure_widget -> failed to set '%s' = '%s'"),
					*PropName, *Value));
				return false;
			};

			auto SolValToStr = [](const sol::object& val) -> FString
			{
				if (val.is<std::string>()) return UTF8_TO_TCHAR(val.as<std::string>().c_str());
				if (val.is<bool>()) return val.as<bool>() ? TEXT("true") : TEXT("false");
				if (val.is<double>())
				{
					double V = val.as<double>();
					if (FMath::IsNearlyEqual(V, FMath::RoundToDouble(V)))
						return FString::Printf(TEXT("%d"), (int64)V);
					return FString::Printf(TEXT("%g"), V);
				}
				return TEXT("");
			};

			if (Properties.has_value())
			{
				// Special handling for is_variable / bIsVariable — needs OnVariableAdded/Removed callbacks
				sol::optional<bool> IsVarOpt = Properties.value()["is_variable"];
				if (IsVarOpt.has_value())
				{
					bool bNewValue = IsVarOpt.value();
					if (bNewValue != (bool)Widget->bIsVariable)
					{
						if (bNewValue)
						{
							Widget->bIsVariable = true;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
							WidgetBP->OnVariableAdded(Widget->GetFName());
#endif
						}
						else
						{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
							WidgetBP->OnVariableRemoved(Widget->GetFName());
#endif
							Widget->bIsVariable = false;
						}
						PropsSet++;
					}
				}

				for (auto& [key, val] : Properties.value())
				{
					if (!key.is<std::string>()) continue;
					FString PropName = UTF8_TO_TCHAR(key.as<std::string>().c_str());
					if (PropName.Equals(TEXT("is_variable"), ESearchCase::IgnoreCase)) continue; // handled above
					SetPropertyFromString(Widget, PropName, SolValToStr(val));
				}
			}

			if (SlotProps.has_value() && Widget->Slot)
			{
				Widget->Slot->Modify();
				for (auto& [key, val] : SlotProps.value())
				{
					if (!key.is<std::string>()) continue;
					FString PropName = UTF8_TO_TCHAR(key.as<std::string>().c_str());
					SetPropertyFromString(Widget->Slot, PropName, SolValToStr(val));
				}
				Widget->Slot->SynchronizeProperties();
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
			Session.Log(FString::Printf(TEXT("[OK] configure_widget(\"%s\") -> %d properties set"), *FWidgetName, PropsSet));
			return sol::make_object(S, true);
		});

		// bp:rename_widget(old_name, new_name) — rename a widget in the widget tree
		BP.set_function("rename_widget", [&Session, FPath](sol::table /*self*/,
			const std::string& OldName, const std::string& NewName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FOldName = UTF8_TO_TCHAR(OldName.c_str());
			FString FNewName = UTF8_TO_TCHAR(NewName.c_str());

			UWidgetBlueprint* WidgetBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WidgetBP || !WidgetBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] rename_widget -> not a Widget Blueprint or no widget tree"));
				return sol::lua_nil;
			}

			UWidget* Widget = WidgetBP->WidgetTree->FindWidget(FName(*FOldName));
			if (!Widget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] rename_widget(\"%s\") -> not found"), *FOldName));
				return sol::lua_nil;
			}

			// Sanitize new name: strip invalid characters
			FName NewFName(*FNewName);
			if (!NewFName.IsValidXName(INVALID_OBJECTNAME_CHARACTERS))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] rename_widget -> invalid name '%s'"), *FNewName));
				return sol::lua_nil;
			}

			// Check uniqueness — new name must not already exist
			FName OldFName = Widget->GetFName();
			if (NewFName == OldFName)
			{
				Session.Log(FString::Printf(TEXT("[OK] rename_widget(\"%s\") -> already has that name"), *FOldName));
				return sol::make_object(S, true);
			}

			FKismetNameValidator NameValidator(WidgetBP, OldFName);
			if (NameValidator.IsValid(NewFName) != EValidatorResult::Ok)
			{
				// Check if the name is used by a BindWidget property (allowed override)
				UClass* ParentClass = WidgetBP->ParentClass;
				FObjectPropertyBase* ExistingProp = ParentClass ? CastField<FObjectPropertyBase>(ParentClass->FindPropertyByName(NewFName)) : nullptr;
				bool bBindWidget = ExistingProp && FWidgetBlueprintEditorUtils::IsBindWidgetProperty(ExistingProp) && Widget->IsA(ExistingProp->PropertyClass);
				if (!bBindWidget)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] rename_widget -> name '%s' already in use"), *FNewName));
					return sol::lua_nil;
				}
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaRenameWidget", "Rename Widget"));

			FString NewNameStr = NewFName.ToString();
			FString OldNameStr = OldFName.ToString();

			WidgetBP->Modify();
			Widget->Modify();

			// Update variable guid map
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			WidgetBP->OnVariableRenamed(OldFName, NewFName);
#endif

			// Rename the template widget
			Widget->SetDisplayLabel(FNewName);
			Widget->Rename(*NewNameStr);

			// Replace desired focus references
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			FWidgetBlueprintEditorUtils::ReplaceDesiredFocus(WidgetBP, OldFName, NewFName);
#endif

			// Update delegate bindings
			for (FDelegateEditorBinding& Binding : WidgetBP->Bindings)
			{
				if (Binding.ObjectName == OldNameStr)
				{
					Binding.ObjectName = NewNameStr;
				}
			}

			// Update widget animation bindings
			for (UWidgetAnimation* WidgetAnimation : WidgetBP->Animations)
			{
				if (!WidgetAnimation || !WidgetAnimation->MovieScene) continue;
				for (FWidgetAnimationBinding& AnimBinding : WidgetAnimation->AnimationBindings)
				{
					if (AnimBinding.WidgetName == OldFName)
					{
						AnimBinding.WidgetName = NewFName;
						WidgetAnimation->MovieScene->Modify();
						if (AnimBinding.SlotWidgetName == NAME_None)
						{
							FMovieScenePossessable* Possessable = WidgetAnimation->MovieScene->FindPossessable(AnimBinding.AnimationGuid);
							if (Possessable)
							{
								Possessable->SetName(NewNameStr);
							}
						}
						else
						{
							break;
						}
					}
				}
			}

			// Update navigation binding references
			WidgetBP->WidgetTree->ForEachWidget([OldFName, NewFName](UWidget* W)
			{
				if (W->Navigation)
				{
					W->Navigation->SetFlags(RF_Transactional);
					W->Navigation->Modify();
					W->Navigation->TryToRenameBinding(OldFName, NewFName);
				}
			});

			// Validate child blueprint variables
			FBlueprintEditorUtils::ValidateBlueprintChildVariables(WidgetBP, NewFName);

			// Refresh references
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

			// Update variable references in graph nodes
			FBlueprintEditorUtils::ReplaceVariableReferences(WidgetBP, OldFName, NewFName);

			Session.Log(FString::Printf(TEXT("[OK] rename_widget(\"%s\" -> \"%s\")"), *FOldName, *FNewName));
			return sol::make_object(S, true);
		});

		// bp:wrap_widgets(wrapper_class, widget_names) — wrap widgets with a container
		BP.set_function("wrap_widgets", [&Session, FPath](sol::table /*self*/,
			const std::string& WrapperType, sol::table WidgetNames, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FWrapperType = UTF8_TO_TCHAR(WrapperType.c_str());

			UWidgetBlueprint* WidgetBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WidgetBP || !WidgetBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] wrap_widgets -> not a Widget Blueprint or no widget tree"));
				return sol::lua_nil;
			}

			// Resolve wrapper class (must be a panel widget)
			UClass* WrapperClass = nullptr;
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (It->IsChildOf(UPanelWidget::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract))
				{
					FString ClassName = It->GetName();
					if (ClassName.Equals(FWrapperType, ESearchCase::IgnoreCase) ||
						ClassName.Equals(FWrapperType + TEXT("Widget"), ESearchCase::IgnoreCase))
					{
						WrapperClass = *It;
						break;
					}
				}
			}
			if (!WrapperClass)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] wrap_widgets -> wrapper class '%s' not found or not a panel widget"), *FWrapperType));
				return sol::lua_nil;
			}

			// Collect widgets to wrap
			TArray<UWidget*> WidgetsToWrap;
			for (auto& Pair : WidgetNames)
			{
				std::string Name = Pair.second.as<std::string>();
				FString FName_Str = UTF8_TO_TCHAR(Name.c_str());
				UWidget* W = WidgetBP->WidgetTree->FindWidget(FName(*FName_Str));
				if (!W)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] wrap_widgets -> widget '%s' not found"), *FName_Str));
					return sol::lua_nil;
				}
				WidgetsToWrap.Add(W);
			}

			if (WidgetsToWrap.Num() == 0)
			{
				Session.Log(TEXT("[FAIL] wrap_widgets -> no widgets specified"));
				return sol::lua_nil;
			}

			// All widgets must share the same parent
			UPanelWidget* CommonParent = nullptr;
			int32 MinIndex = INT_MAX;
			UWidget* MinWidget = nullptr;
			bool bIsRoot = false;

			for (UWidget* W : WidgetsToWrap)
			{
				if (W == WidgetBP->WidgetTree->RootWidget)
				{
					bIsRoot = true;
					if (WidgetsToWrap.Num() > 1)
					{
						Session.Log(TEXT("[FAIL] wrap_widgets -> cannot wrap root widget together with other widgets"));
						return sol::lua_nil;
					}
					break;
				}
				int32 Idx;
				UPanelWidget* Parent = UWidgetTree::FindWidgetParent(W, Idx);
				if (!Parent)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] wrap_widgets -> widget '%s' has no parent panel"), *W->GetName()));
					return sol::lua_nil;
				}
				if (!CommonParent)
				{
					CommonParent = Parent;
				}
				else if (CommonParent != Parent)
				{
					Session.Log(TEXT("[FAIL] wrap_widgets -> all widgets must share the same parent"));
					return sol::lua_nil;
				}
				if (Idx < MinIndex)
				{
					MinIndex = Idx;
					MinWidget = W;
				}
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaWrapWidgets", "Wrap Widgets"));
			WidgetBP->WidgetTree->SetFlags(RF_Transactional);
			WidgetBP->WidgetTree->Modify();

			// Create the wrapper widget
			UPanelWidget* Wrapper = Cast<UPanelWidget>(WidgetBP->WidgetTree->ConstructWidget<UWidget>(WrapperClass));
			if (!Wrapper)
			{
				Session.Log(TEXT("[FAIL] wrap_widgets -> failed to construct wrapper widget"));
				return sol::lua_nil;
			}
			Wrapper->CreatedFromPalette();

			if (bIsRoot)
			{
				// Wrapping the root: new wrapper becomes root, old root becomes child
				UWidget* OldRoot = WidgetBP->WidgetTree->RootWidget;
				WidgetBP->WidgetTree->RootWidget = Wrapper;
				Wrapper->AddChild(OldRoot);
			}
			else
			{
				CommonParent->SetFlags(RF_Transactional);
				CommonParent->Modify();

				// Replace the lowest-index widget with the wrapper
				CommonParent->ReplaceChildAt(MinIndex, Wrapper);

				// Add the replaced widget first to maintain order
				Wrapper->AddChild(MinWidget);

				// Add remaining widgets (AddChild auto-removes from old parent)
				for (UWidget* W : WidgetsToWrap)
				{
					if (W == MinWidget) continue;
					W->SetFlags(RF_Transactional);
					W->Modify();
					Wrapper->AddChild(W);
				}
			}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			WidgetBP->OnVariableAdded(Wrapper->GetFName());
#endif
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

			Session.Log(FString::Printf(TEXT("[OK] wrap_widgets(\"%s\") -> wrapped %d widgets, wrapper name = \"%s\""),
				*FWrapperType, WidgetsToWrap.Num(), *Wrapper->GetName()));

			sol::table Result = Lua.create_table();
			Result["name"] = TCHAR_TO_UTF8(*Wrapper->GetName());
			Result["type"] = TCHAR_TO_UTF8(*WrapperClass->GetName());
			Result["child_count"] = WidgetsToWrap.Num();
			return Result;
		});

		// bp:replace_widget(old_name, new_class) — replace a widget with a different class
		BP.set_function("replace_widget", [&Session, FPath](sol::table /*self*/,
			const std::string& WidgetName, const std::string& NewType, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FWidgetName = UTF8_TO_TCHAR(WidgetName.c_str());
			FString FNewType = UTF8_TO_TCHAR(NewType.c_str());

			UWidgetBlueprint* WidgetBP = LoadObject<UWidgetBlueprint>(nullptr, *FPath);
			if (!WidgetBP || !WidgetBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] replace_widget -> not a Widget Blueprint or no widget tree"));
				return sol::lua_nil;
			}

			UWidget* Widget = WidgetBP->WidgetTree->FindWidget(FName(*FWidgetName));
			if (!Widget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] replace_widget(\"%s\") -> not found"), *FWidgetName));
				return sol::lua_nil;
			}

			// Resolve replacement class
			UClass* NewClass = nullptr;
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (It->IsChildOf(UWidget::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract))
				{
					FString ClassName = It->GetName();
					if (ClassName.Equals(FNewType, ESearchCase::IgnoreCase) ||
						ClassName.Equals(FNewType + TEXT("Widget"), ESearchCase::IgnoreCase))
					{
						NewClass = *It;
						break;
					}
				}
			}
			if (!NewClass)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] replace_widget -> class '%s' not found"), *FNewType));
				return sol::lua_nil;
			}

			// If widget has children and replacement is not a panel, fail
			if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
			{
				if (Panel->GetChildrenCount() > 0 && !NewClass->IsChildOf(UPanelWidget::StaticClass()))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] replace_widget(\"%s\") -> has children but '%s' is not a panel widget"),
						*FWidgetName, *FNewType));
					return sol::lua_nil;
				}
			}

			// ReplaceWidgetsWithTemplateClass is private — not available
			Session.Log(FString::Printf(TEXT("[FAIL] replace_widget(\"%s\") -> not supported (engine API is private). Remove the old widget and add a new one instead."), *FWidgetName));
			return sol::lua_nil;
		});

		// ================================================================
		// Event binding operations
		// ================================================================

		// bp:list_events(source?) — discover delegate events on a component, widget, or all components
		BP.set_function("list_events", [&Session, FPath](sol::table /*self*/,
			sol::optional<std::string> OptSourceName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] list_events -> blueprint not found"));
				return sol::lua_nil;
			}

			// Helper: collect delegate events from a UClass into the result table
			auto CollectEvents = [&Lua](UClass* TargetClass, const FString& SourceLabel, sol::table& Result, int32& Idx)
			{
				for (TFieldIterator<FMulticastDelegateProperty> It(TargetClass); It; ++It)
				{
					FMulticastDelegateProperty* Prop = *It;
					if (!Prop || !Prop->HasAnyPropertyFlags(CPF_BlueprintAssignable)) continue;

					sol::table Entry = Lua.create_table();
					Entry["name"] = TCHAR_TO_UTF8(*Prop->GetName());
					Entry["source"] = TCHAR_TO_UTF8(*SourceLabel);

					UFunction* SigFunc = Prop->SignatureFunction;
					if (SigFunc)
					{
						FString Sig;
						for (TFieldIterator<FProperty> PIt(SigFunc); PIt && PIt->HasAnyPropertyFlags(CPF_Parm) && !PIt->HasAnyPropertyFlags(CPF_ReturnParm); ++PIt)
						{
							if (!Sig.IsEmpty()) Sig += TEXT(", ");
							Sig += FString::Printf(TEXT("%s %s"), *PIt->GetCPPType(), *PIt->GetName());
						}
						Entry["signature"] = TCHAR_TO_UTF8(*Sig);
					}
					Result[Idx++] = Entry;
				}
			};

			sol::table Result = Lua.create_table();
			int32 Idx = 1;

			if (OptSourceName.has_value())
			{
				FString FSource = UTF8_TO_TCHAR(OptSourceName->c_str());
				UClass* TargetClass = nullptr;

				// Check widget
				UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(Info.Blueprint);
				if (WidgetBP && WidgetBP->WidgetTree)
				{
					UWidget* Widget = WidgetBP->WidgetTree->FindWidget(FName(*FSource));
					if (Widget) TargetClass = Widget->GetClass();
				}

				// Check component
				if (!TargetClass)
				{
					UActorComponent* Comp = NeoBlueprint::GetComponentTemplate(Info.Blueprint, FSource);
					if (Comp) TargetClass = Comp->GetClass();
				}

				if (!TargetClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] list_events(\"%s\") -> not found as widget or component"), *FSource));
					return sol::lua_nil;
				}

				CollectEvents(TargetClass, FSource, Result, Idx);
				Session.Log(FString::Printf(TEXT("[OK] list_events(\"%s\") -> %d events"), *FSource, Idx - 1));
			}
			else
			{
				// No source specified — list events from all components
				if (Info.Blueprint->SimpleConstructionScript)
				{
					for (USCS_Node* SCSNode : Info.Blueprint->SimpleConstructionScript->GetAllNodes())
					{
						if (!SCSNode || !SCSNode->ComponentTemplate) continue;
						FString CompName = SCSNode->GetVariableName().ToString();
						CollectEvents(SCSNode->ComponentTemplate->GetClass(), CompName, Result, Idx);
					}
				}
				Session.Log(FString::Printf(TEXT("[OK] list_events() -> %d events from all components"), Idx - 1));
			}

			return Result;
		});

		// bp:bind_event(source, event) — bind a component/widget delegate event
		BP.set_function("bind_event", [&Session, FPath](sol::table /*self*/,
			const std::string& SourceName, const std::string& EventName,
			sol::optional<std::string> /*HandlerName*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FSource = UTF8_TO_TCHAR(SourceName.c_str());
			FString FEvent = UTF8_TO_TCHAR(EventName.c_str());

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] bind_event -> blueprint not found"));
				return sol::lua_nil;
			}

			// Widget Blueprint path
			UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(Info.Blueprint);
			if (WidgetBP && WidgetBP->WidgetTree)
			{
				UWidget* Widget = WidgetBP->WidgetTree->FindWidget(FName(*FSource));
				if (Widget)
				{
					// Widget must be a variable for the skeleton class to have an FObjectProperty for it.
					// Auto-promote and recompile if needed.
					if (!Widget->bIsVariable)
					{
						Widget->bIsVariable = true;
						FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Info.Blueprint);
						FKismetEditorUtilities::CompileBlueprint(Info.Blueprint);
						Session.Log(FString::Printf(TEXT("[INFO] bind_event -> auto-promoted widget '%s' to variable"), *FSource));
					}

					UClass* SkeletonClass = Info.Blueprint->SkeletonGeneratedClass;
					if (!SkeletonClass) SkeletonClass = Info.Blueprint->GeneratedClass;
					FObjectProperty* WidgetProp = SkeletonClass ?
						FindFProperty<FObjectProperty>(SkeletonClass, FName(*FSource)) : nullptr;

					// If property still not found, try recompiling (widget may have been added this session)
					if (!WidgetProp)
					{
						FKismetEditorUtilities::CompileBlueprint(Info.Blueprint);
						SkeletonClass = Info.Blueprint->SkeletonGeneratedClass;
						if (!SkeletonClass) SkeletonClass = Info.Blueprint->GeneratedClass;
						WidgetProp = SkeletonClass ?
							FindFProperty<FObjectProperty>(SkeletonClass, FName(*FSource)) : nullptr;
					}

					if (WidgetProp)
					{
						const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaBindEvent", "Bind Event"));
						FKismetEditorUtilities::CreateNewBoundEventForClass(
							Widget->GetClass(), FName(*FEvent), Info.Blueprint, WidgetProp);
						FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Info.Blueprint);
						Session.Log(FString::Printf(TEXT("[OK] bind_event(\"%s\", \"%s\") -> bound (widget)"), *FSource, *FEvent));
						return sol::make_object(Lua, true);
					}

					Session.Log(FString::Printf(TEXT("[FAIL] bind_event -> could not find property for widget '%s' in skeleton class after compile"), *FSource));
					return sol::lua_nil;
				}
			}

			// Component path
			if (Info.Blueprint->SimpleConstructionScript)
			{
				USCS_Node* SCSNode = nullptr;
				for (USCS_Node* Node : Info.Blueprint->SimpleConstructionScript->GetAllNodes())
				{
					if (Node && Node->GetVariableName().ToString().Equals(FSource, ESearchCase::IgnoreCase))
					{
						SCSNode = Node;
						break;
					}
				}

				if (SCSNode && SCSNode->ComponentClass)
				{
					UClass* SkeletonClass = Info.Blueprint->SkeletonGeneratedClass;
					if (!SkeletonClass) SkeletonClass = Info.Blueprint->GeneratedClass;
					FObjectProperty* CompProp = SkeletonClass ?
						FindFProperty<FObjectProperty>(SkeletonClass, SCSNode->GetVariableName()) : nullptr;

					if (CompProp)
					{
						const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaBindCompEvent", "Bind Component Event"));
						FKismetEditorUtilities::CreateNewBoundEventForClass(
							SCSNode->ComponentClass, FName(*FEvent), Info.Blueprint, CompProp);
						FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Info.Blueprint);
						Session.Log(FString::Printf(TEXT("[OK] bind_event(\"%s\", \"%s\") -> bound (component)"), *FSource, *FEvent));
						return sol::make_object(Lua, true);
					}
				}
			}

			Session.Log(FString::Printf(TEXT("[FAIL] bind_event(\"%s\", \"%s\") -> source not found"), *FSource, *FEvent));
			return sol::lua_nil;
		});

		// bp:unbind_event(source, event) — remove an event binding
		BP.set_function("unbind_event", [&Session, FPath](sol::table /*self*/,
			const std::string& SourceName, const std::string& EventName,
			sol::this_state S) -> sol::object
		{
			FString FSource = UTF8_TO_TCHAR(SourceName.c_str());
			FString FEvent = UTF8_TO_TCHAR(EventName.c_str());

			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *FPath);
			if (!Blueprint)
			{
				Session.Log(TEXT("[FAIL] unbind_event -> blueprint not found"));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaUnbindEvent", "Unbind Event"));
			bool bRemoved = false;

			for (UEdGraph* Graph : Blueprint->UbergraphPages)
			{
				if (!Graph) continue;
				for (int32 i = Graph->Nodes.Num() - 1; i >= 0; --i)
				{
					UK2Node_ComponentBoundEvent* BoundEvent = Cast<UK2Node_ComponentBoundEvent>(Graph->Nodes[i]);
					if (BoundEvent &&
						BoundEvent->ComponentPropertyName.ToString().Equals(FSource, ESearchCase::IgnoreCase) &&
						BoundEvent->DelegatePropertyName.ToString().Equals(FEvent, ESearchCase::IgnoreCase))
					{
						FBlueprintEditorUtils::RemoveNode(Blueprint, BoundEvent, true);
						bRemoved = true;
					}
				}
			}

			if (bRemoved)
			{
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				Session.Log(FString::Printf(TEXT("[OK] unbind_event(\"%s\", \"%s\")"), *FSource, *FEvent));
				return sol::make_object(S, true);
			}

			// Also check widget delegate bindings
			UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(Blueprint);
			if (WidgetBP)
			{
				for (int32 i = WidgetBP->Bindings.Num() - 1; i >= 0; --i)
				{
					if (WidgetBP->Bindings[i].ObjectName.Equals(FSource, ESearchCase::IgnoreCase) &&
						WidgetBP->Bindings[i].PropertyName.ToString().Equals(FEvent, ESearchCase::IgnoreCase))
					{
						WidgetBP->Bindings.RemoveAt(i);
						bRemoved = true;
					}
				}
				if (bRemoved)
				{
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
					Session.Log(FString::Printf(TEXT("[OK] unbind_event(\"%s\", \"%s\") (widget binding)"), *FSource, *FEvent));
					return sol::make_object(S, true);
				}
			}

			Session.Log(FString::Printf(TEXT("[FAIL] unbind_event(\"%s\", \"%s\") -> binding not found"), *FSource, *FEvent));
			return sol::lua_nil;
		});

		// ================================================================
		// Event Dispatcher (multicast delegate variable) operations
		// ================================================================

		// bp:add_event_dispatcher(name, params?)
		BP.set_function("add_event_dispatcher", [&Session, FPath](sol::table /*self*/,
			const std::string& DispName, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FDispName = UTF8_TO_TCHAR(DispName.c_str());

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] add_event_dispatcher -> blueprint not found"));
				return sol::lua_nil;
			}

			// Check existing
			for (const FBPVariableDescription& Var : Info.Blueprint->NewVariables)
			{
				if (Var.VarName == FName(*FDispName) &&
					Var.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_event_dispatcher(\"%s\") -> already exists"), *FDispName));
					return sol::lua_nil;
				}
			}

			TArray<FParamDesc> ParamDescs;
			if (Params.has_value())
			{
				for (auto& Pair : Params.value())
				{
					if (Pair.second.is<sol::table>())
					{
						sol::table P = Pair.second.as<sol::table>();
						FParamDesc Desc;
						Desc.Name = UTF8_TO_TCHAR(P.get_or<std::string>("name", "").c_str());
						Desc.Type = UTF8_TO_TCHAR(P.get_or<std::string>("type", "").c_str());
						if (!Desc.Name.IsEmpty() && !Desc.Type.IsEmpty())
							ParamDescs.Add(MoveTemp(Desc));
					}
				}
			}

			UEdGraph* SigGraph = NeoBlueprint::AddEventDispatcher(Info.Blueprint, FDispName, ParamDescs);
			if (!SigGraph)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_event_dispatcher(\"%s\") -> failed"), *FDispName));
				return sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[OK] add_event_dispatcher(\"%s\") -> %d params"), *FDispName, ParamDescs.Num()));
			return sol::make_object(S, true);
		});

		// bp:remove_event_dispatcher(name)
		BP.set_function("remove_event_dispatcher", [&Session, FPath](sol::table /*self*/,
			const std::string& DispName, sol::this_state S) -> sol::object
		{
			FString FDispName = UTF8_TO_TCHAR(DispName.c_str());
			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *FPath);
			if (!Blueprint) { Session.Log(TEXT("[FAIL] remove_event_dispatcher -> blueprint not found")); return sol::lua_nil; }

			for (int32 i = Blueprint->NewVariables.Num() - 1; i >= 0; --i)
			{
				const FBPVariableDescription& Var = Blueprint->NewVariables[i];
				if (Var.VarName == FName(*FDispName) && Var.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
				{
					FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, Var.VarName);
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
					Session.Log(FString::Printf(TEXT("[OK] remove_event_dispatcher(\"%s\")"), *FDispName));
					return sol::make_object(S, true);
				}
			}
			Session.Log(FString::Printf(TEXT("[FAIL] remove_event_dispatcher(\"%s\") -> not found"), *FDispName));
			return sol::lua_nil;
		});

		// bp:remove_custom_event(name)
		BP.set_function("remove_custom_event", [&Session, FPath](sol::table /*self*/,
			const std::string& EventName, sol::this_state S) -> sol::object
		{
			FString FEventName = UTF8_TO_TCHAR(EventName.c_str());
			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *FPath);
			if (!Blueprint) { Session.Log(TEXT("[FAIL] remove_custom_event -> blueprint not found")); return sol::lua_nil; }

			if (Blueprint->UbergraphPages.Num() == 0) { Session.Log(TEXT("[FAIL] remove_custom_event -> no event graphs")); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaRemCE", "Remove Custom Event"));
			// Search all ubergraph pages, not just the first EventGraph
			for (UEdGraph* EventGraph : Blueprint->UbergraphPages)
			{
				if (!EventGraph) continue;
				for (int32 i = EventGraph->Nodes.Num() - 1; i >= 0; --i)
				{
					UK2Node_CustomEvent* EventNode = Cast<UK2Node_CustomEvent>(EventGraph->Nodes[i]);
					if (EventNode && EventNode->CustomFunctionName.ToString().Equals(FEventName, ESearchCase::IgnoreCase))
					{
						FBlueprintEditorUtils::RemoveNode(Blueprint, EventNode, true);
						Session.Log(FString::Printf(TEXT("[OK] remove_custom_event(\"%s\")"), *FEventName));
						return sol::make_object(S, true);
					}
				}
			}
			Session.Log(FString::Printf(TEXT("[FAIL] remove_custom_event(\"%s\") -> not found"), *FEventName));
			return sol::lua_nil;
		});

		// ================================================================
		// Override functions
		// ================================================================

		// bp:override_function(name) — create event or function override for parent class function
		BP.set_function("override_function", [&Session, FPath, PathStr](sol::table /*self*/,
			const std::string& FuncName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FFuncName = UTF8_TO_TCHAR(FuncName.c_str());

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint) { Session.Log(TEXT("[FAIL] override_function -> blueprint not found")); return sol::lua_nil; }

			UFunction* OverrideFunc = nullptr;
			UClass* OverrideFuncClass = FBlueprintEditorUtils::GetOverrideFunctionClass(
				Info.Blueprint, FName(*FFuncName), &OverrideFunc);

			// If not found by C++ name, try matching by ScriptName or DisplayName metadata.
			// Many engine functions use K2_ prefix internally but expose friendly names
			// (e.g. K2_ActivateAbility has ScriptName="ActivateAbility").
			if (!OverrideFunc || !OverrideFuncClass)
			{
				UClass* ParentClass = Info.Blueprint->SkeletonGeneratedClass
					? Info.Blueprint->SkeletonGeneratedClass->GetSuperClass()
					: nullptr;
				if (ParentClass)
				{
					for (TFieldIterator<UFunction> FuncIt(ParentClass, EFieldIteratorFlags::IncludeSuper); FuncIt; ++FuncIt)
					{
						UFunction* Func = *FuncIt;
						if (!Func) continue;
#if WITH_METADATA
						const FString& ScriptName = Func->GetMetaData(TEXT("ScriptName"));
						const FString& DisplayName = Func->GetMetaData(TEXT("DisplayName"));
						if (ScriptName.Equals(FFuncName, ESearchCase::IgnoreCase)
							|| DisplayName.Equals(FFuncName, ESearchCase::IgnoreCase))
						{
							// Retry with the real C++ function name
							FName RealName = Func->GetFName();
							OverrideFuncClass = FBlueprintEditorUtils::GetOverrideFunctionClass(
								Info.Blueprint, RealName, &OverrideFunc);
							if (OverrideFunc && OverrideFuncClass)
							{
								FFuncName = RealName.ToString();
								Session.Log(FString::Printf(TEXT("[INFO] override_function -> resolved \"%s\" to C++ name \"%s\""),
									UTF8_TO_TCHAR(FuncName.c_str()), *FFuncName));
								break;
							}
						}
#endif
					}
				}
			}

			if (!OverrideFunc || !OverrideFuncClass)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] override_function(\"%s\") -> not found in parent class"), *FFuncName));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaOverride", "Override Function"));

			if (UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(OverrideFunc))
			{
				UK2Node_Event* Existing = FBlueprintEditorUtils::FindOverrideForFunction(
					Info.Blueprint, OverrideFuncClass, FName(*FFuncName));

				// Also check for any event node matching this name (catches non-override events
				// like those placed from the default BP template)
				if (!Existing)
				{
					TArray<UK2Node_Event*> AllEvents;
					FBlueprintEditorUtils::GetAllNodesOfClass<UK2Node_Event>(Info.Blueprint, AllEvents);
					for (UK2Node_Event* Evt : AllEvents)
					{
						if (Evt && Evt->EventReference.GetMemberName() == FName(*FFuncName))
						{
							Existing = Evt;
							break;
						}
					}
				}

				if (Existing)
				{
					Session.Log(FString::Printf(TEXT("[OK] override_function(\"%s\") -> already exists (event)"), *FFuncName));
					sol::table Result = Lua.create_table();
					Result["handle"] = TCHAR_TO_UTF8(*Existing->NodeGuid.ToString());
					Result["type"] = "event";
					return Result;
				}

				UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Info.Blueprint);
				if (!EventGraph) { Session.Log(TEXT("[FAIL] override_function -> no EventGraph")); return sol::lua_nil; }

				FName EventFName = FName(*FFuncName);
				UK2Node_Event* NewEvent = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_Event>(
					EventGraph, EventGraph->GetGoodPlaceForNewNode(), EK2NewNodeFlags::None,
					[EventFName, OverrideFuncClass](UK2Node_Event* NewInstance)
					{
						NewInstance->EventReference.SetExternalMember(EventFName, OverrideFuncClass);
						NewInstance->bOverrideFunction = true;
					}
				);

				if (NewEvent)
				{
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Info.Blueprint);
					Session.Log(FString::Printf(TEXT("[OK] override_function(\"%s\") -> created as event"), *FFuncName));
					sol::table Result = Lua.create_table();
					Result["handle"] = TCHAR_TO_UTF8(*NewEvent->NodeGuid.ToString());
					Result["type"] = "event";
					return Result;
				}
			}
			else
			{
				UEdGraph* ExistingGraph = NeoBlueprint::FindGraph(Info.Blueprint, FFuncName);
				if (ExistingGraph)
				{
					Session.Log(FString::Printf(TEXT("[OK] override_function(\"%s\") -> already exists (function)"), *FFuncName));
					return BuildGraphObject(Lua, Session, ExistingGraph, PathStr, FuncName);
				}

				UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
					Info.Blueprint, FName(*FFuncName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
				if (NewGraph)
				{
					FBlueprintEditorUtils::AddFunctionGraph(Info.Blueprint, NewGraph, false, OverrideFuncClass);
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Info.Blueprint);
					Session.Log(FString::Printf(TEXT("[OK] override_function(\"%s\") -> created as function"), *FFuncName));
					return BuildGraphObject(Lua, Session, NewGraph, PathStr, FuncName);
				}
			}

			Session.Log(FString::Printf(TEXT("[FAIL] override_function(\"%s\") -> failed to create"), *FFuncName));
			return sol::lua_nil;
		});

		// ================================================================
		// Linked Anim Layer support (AnimBlueprint only)
		// ================================================================

		// bp:add_linked_anim_layer(layer_name, interface?)
		BP.set_function("add_linked_anim_layer", [&Session, FPath](sol::table /*self*/,
			const std::string& LayerName, sol::optional<std::string> InterfaceStr,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FLayerName = UTF8_TO_TCHAR(LayerName.c_str());

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint) { Session.Log(TEXT("[FAIL] add_linked_anim_layer -> blueprint not found")); return sol::lua_nil; }

			UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Info.Blueprint);
			if (!AnimBP) { Session.Log(TEXT("[FAIL] add_linked_anim_layer -> not an Animation Blueprint")); return sol::lua_nil; }

			UEdGraph* AnimGraph = NeoBlueprint::FindAnimGraph(AnimBP);
			if (!AnimGraph) { Session.Log(TEXT("[FAIL] add_linked_anim_layer -> AnimGraph not found")); return sol::lua_nil; }

			// Resolve interface class
			UClass* InterfaceClass = nullptr;
			if (InterfaceStr.has_value())
			{
				FString FInterface = UTF8_TO_TCHAR(InterfaceStr.value().c_str());
				InterfaceClass = FindObject<UClass>(nullptr, *FInterface);
				if (!InterfaceClass)
				{
					UBlueprint* InterfaceBP = LoadObject<UBlueprint>(nullptr, *FInterface);
					if (InterfaceBP && InterfaceBP->GeneratedClass)
						InterfaceClass = InterfaceBP->GeneratedClass;
				}
				if (!InterfaceClass)
				{
					for (const FBPInterfaceDescription& Desc : AnimBP->ImplementedInterfaces)
					{
						if (Desc.Interface && Desc.Interface->GetName().Contains(FInterface))
						{
							InterfaceClass = Desc.Interface;
							break;
						}
					}
				}
			}
			else
			{
				for (const FBPInterfaceDescription& Desc : AnimBP->ImplementedInterfaces)
				{
					if (Desc.Interface && Desc.Interface->IsChildOf(UAnimLayerInterface::StaticClass()))
					{
						InterfaceClass = Desc.Interface;
						break;
					}
				}
			}

			if (!InterfaceClass)
			{
				// If an explicit path was given, try loading it as a Blueprint and auto-implementing
				if (InterfaceStr.has_value())
				{
					FString FInterface = UTF8_TO_TCHAR(InterfaceStr.value().c_str());
					Session.Log(FString::Printf(TEXT("[INFO] add_linked_anim_layer -> interface '%s' not implemented, attempting auto-implement"), *FInterface));
					if (NeoBlueprint::AddInterface(AnimBP, FInterface))
					{
						// Re-resolve after implementing
						for (const FBPInterfaceDescription& Desc : AnimBP->ImplementedInterfaces)
						{
							if (Desc.Interface && Desc.Interface->IsChildOf(UAnimLayerInterface::StaticClass()))
							{
								InterfaceClass = Desc.Interface;
								break;
							}
						}
					}
				}
				if (!InterfaceClass)
				{
					Session.Log(TEXT("[FAIL] add_linked_anim_layer -> AnimLayerInterface not found. Use add_interface() first, or pass the interface asset path."));
					return sol::lua_nil;
				}
			}
			if (!InterfaceClass->IsChildOf(UAnimLayerInterface::StaticClass()))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_linked_anim_layer -> '%s' is not an AnimLayerInterface"),
					*InterfaceClass->GetName()));
				return sol::lua_nil;
			}

			UFunction* LayerFunc = InterfaceClass->FindFunctionByName(FName(*FLayerName));
			if (!LayerFunc)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_linked_anim_layer -> function '%s' not found on interface '%s'"),
					*FLayerName, *InterfaceClass->GetName()));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaAddLayer", "Add Linked Anim Layer"));
			AnimGraph->Modify();

			UAnimGraphNode_LinkedAnimLayer* LayerNode = NewObject<UAnimGraphNode_LinkedAnimLayer>(AnimGraph);
			LayerNode->Node.Interface = InterfaceClass;  // BEFORE PostPlacedNewNode
			LayerNode->CreateNewGuid();
			AnimGraph->AddNode(LayerNode, false, false);
			LayerNode->SetFlags(RF_Transactional);
			LayerNode->PostPlacedNewNode();
			LayerNode->AllocateDefaultPins();

			LayerNode->Node.Layer = FName(*FLayerName);

			// Set FunctionReference via reflection
			FProperty* FuncRefProp = LayerNode->GetClass()->FindPropertyByName(TEXT("FunctionReference"));
			if (FuncRefProp)
			{
				FMemberReference* FuncRef = FuncRefProp->ContainerPtrToValuePtr<FMemberReference>(LayerNode);
				if (FuncRef)
				{
					UClass* TargetClass = AnimBP->SkeletonGeneratedClass ? static_cast<UClass*>(AnimBP->SkeletonGeneratedClass) : InterfaceClass;
					FuncRef->SetExternalMember(FName(*FLayerName), TargetClass);
				}
			}

			// Update InterfaceGuid from matching interface graph
			FProperty* InterfaceGuidProp = LayerNode->StaticClass()->FindPropertyByName(TEXT("InterfaceGuid"));
			if (InterfaceGuidProp)
			{
				for (const FBPInterfaceDescription& Desc : AnimBP->ImplementedInterfaces)
				{
					for (UEdGraph* IGraph : Desc.Graphs)
					{
						if (IGraph && IGraph->GetFName() == FName(*FLayerName))
						{
							FGuid* GuidPtr = InterfaceGuidProp->ContainerPtrToValuePtr<FGuid>(LayerNode);
							if (GuidPtr) *GuidPtr = IGraph->GraphGuid;
							break;
						}
					}
				}
			}

			LayerNode->ReconstructNode();

			int32 MaxX = 200;
			for (UEdGraphNode* Node : AnimGraph->Nodes)
			{
				if (Node && Node != LayerNode)
					MaxX = FMath::Max(MaxX, static_cast<int32>(Node->NodePosX) + 300);
			}
			LayerNode->NodePosX = MaxX;
			LayerNode->NodePosY = 0;

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

			FString NodeGuid = LayerNode->NodeGuid.ToString();
			Session.Nodes.Add(NodeGuid, LayerNode);

			Session.Log(FString::Printf(TEXT("[OK] add_linked_anim_layer(\"%s\", interface=\"%s\") -> handle=\"%s\""),
				*FLayerName, *InterfaceClass->GetName(), *NodeGuid));

			sol::table Result = Lua.create_table();
			Result["handle"] = TCHAR_TO_UTF8(*NodeGuid);
			Result["layer"] = LayerName;
			Result["interface"] = TCHAR_TO_UTF8(*InterfaceClass->GetName());
			return Result;
		});

		// ================================================================
		// Additional component operations
		// ================================================================

		// bp:rename_component(name, new_name)
		BP.set_function("rename_component", [&Session, FPath](sol::table /*self*/,
			const std::string& OldName, const std::string& NewName, sol::this_state S) -> sol::object
		{
			FString FOld = UTF8_TO_TCHAR(OldName.c_str());
			FString FNew = UTF8_TO_TCHAR(NewName.c_str());
			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *FPath);
			if (!Blueprint || !Blueprint->SimpleConstructionScript)
			{ Session.Log(TEXT("[FAIL] rename_component -> blueprint not found or no SCS")); return sol::lua_nil; }

			USCS_Node* Node = nullptr;
			for (USCS_Node* N : Blueprint->SimpleConstructionScript->GetAllNodes())
				if (N && N->GetVariableName().ToString().Equals(FOld, ESearchCase::IgnoreCase)) { Node = N; break; }
			if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] rename_component(\"%s\") -> not found"), *FOld)); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaRenComp", "Rename Component"));
			FBlueprintEditorUtils::RenameComponentMemberVariable(Blueprint, Node, FName(*FNew));
			Session.Log(FString::Printf(TEXT("[OK] rename_component(\"%s\" -> \"%s\")"), *FOld, *FNew));
			return sol::make_object(S, true);
		});

		// bp:duplicate_component(name, new_name?)
		BP.set_function("duplicate_component", [&Session, FPath](sol::table /*self*/,
			const std::string& SourceName, sol::optional<std::string> NewNameOpt,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FSource = UTF8_TO_TCHAR(SourceName.c_str());
			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *FPath);
			if (!Blueprint || !Blueprint->SimpleConstructionScript)
			{ Session.Log(TEXT("[FAIL] duplicate_component -> blueprint not found or no SCS")); return sol::lua_nil; }

			USCS_Node* SourceNode = nullptr;
			for (USCS_Node* N : Blueprint->SimpleConstructionScript->GetAllNodes())
				if (N && N->GetVariableName().ToString().Equals(FSource, ESearchCase::IgnoreCase)) { SourceNode = N; break; }
			if (!SourceNode || !SourceNode->ComponentTemplate)
			{ Session.Log(FString::Printf(TEXT("[FAIL] duplicate_component(\"%s\") -> not found"), *FSource)); return sol::lua_nil; }

			FString FNew = NewNameOpt.has_value() ? UTF8_TO_TCHAR(NewNameOpt.value().c_str()) :
				FString::Printf(TEXT("%s_Copy"), *FSource);

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaDupComp", "Duplicate Component"));
			USCS_Node* NewNode = Blueprint->SimpleConstructionScript->CreateNode(SourceNode->ComponentTemplate->GetClass(), FName(*FNew));
			if (!NewNode) { Session.Log(TEXT("[FAIL] duplicate_component -> CreateNode failed")); return sol::lua_nil; }

			UEngine::CopyPropertiesForUnrelatedObjects(SourceNode->ComponentTemplate, NewNode->ComponentTemplate);

			USCS_Node* ParentNode = Blueprint->SimpleConstructionScript->FindParentNode(SourceNode);
			if (ParentNode) ParentNode->AddChildNode(NewNode);
			else Blueprint->SimpleConstructionScript->AddNode(NewNode);

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			Session.Log(FString::Printf(TEXT("[OK] duplicate_component(\"%s\" -> \"%s\")"), *FSource, *FNew));

			sol::table Result = Lua.create_table();
			Result["name"] = TCHAR_TO_UTF8(*NewNode->GetVariableName().ToString());
			Result["class"] = NewNode->ComponentClass ? TCHAR_TO_UTF8(*NewNode->ComponentClass->GetName()) : "None";
			return Result;
		});

		// bp:reparent_component(name, parent?) — move to new parent (empty = root)
		BP.set_function("reparent_component", [&Session, FPath](sol::table /*self*/,
			const std::string& CompName, sol::optional<std::string> ParentName, sol::this_state S) -> sol::object
		{
			FString FComp = UTF8_TO_TCHAR(CompName.c_str());
			FString FParent = ParentName.has_value() ? UTF8_TO_TCHAR(ParentName.value().c_str()) : TEXT("");
			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *FPath);
			if (!Blueprint || !Blueprint->SimpleConstructionScript)
			{ Session.Log(TEXT("[FAIL] reparent_component -> blueprint not found or no SCS")); return sol::lua_nil; }

			auto FindSCSNode = [&](const FString& Name) -> USCS_Node* {
				for (USCS_Node* N : Blueprint->SimpleConstructionScript->GetAllNodes())
					if (N && N->GetVariableName().ToString().Equals(Name, ESearchCase::IgnoreCase)) return N;
				return nullptr;
			};

			USCS_Node* Node = FindSCSNode(FComp);
			if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] reparent_component(\"%s\") -> not found"), *FComp)); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaReparentComp", "Reparent Component"));
			USCS_Node* CurrentParent = Blueprint->SimpleConstructionScript->FindParentNode(Node);
			if (CurrentParent) CurrentParent->RemoveChildNode(Node, false);
			else Blueprint->SimpleConstructionScript->RemoveNode(Node, false);

			bool bToRoot = FParent.IsEmpty() || FParent.Equals(TEXT("root"), ESearchCase::IgnoreCase);
			if (bToRoot)
			{
				Blueprint->SimpleConstructionScript->AddNode(Node);
			}
			else
			{
				USCS_Node* NewParent = FindSCSNode(FParent);
				if (!NewParent) { Blueprint->SimpleConstructionScript->AddNode(Node);
					Session.Log(FString::Printf(TEXT("[FAIL] reparent_component -> parent '%s' not found"), *FParent)); return sol::lua_nil; }
				// Circular check
				USCS_Node* Check = NewParent;
				while (Check) {
					if (Check == Node) { Blueprint->SimpleConstructionScript->AddNode(Node);
						Session.Log(TEXT("[FAIL] reparent_component -> circular reference")); return sol::lua_nil; }
					Check = Blueprint->SimpleConstructionScript->FindParentNode(Check);
				}
				NewParent->AddChildNode(Node, true);
			}
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			Session.Log(FString::Printf(TEXT("[OK] reparent_component(\"%s\" -> \"%s\")"), *FComp, bToRoot ? TEXT("root") : *FParent));
			return sol::make_object(S, true);
		});

		// bp:set_root_component(name) — promote a scene component to root
		BP.set_function("set_root_component", [&Session, FPath](sol::table /*self*/,
			const std::string& CompName, sol::this_state S) -> sol::object
		{
			FString FComp = UTF8_TO_TCHAR(CompName.c_str());
			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *FPath);
			if (!Blueprint || !Blueprint->SimpleConstructionScript)
			{ Session.Log(TEXT("[FAIL] set_root_component -> blueprint not found or no SCS")); return sol::lua_nil; }

			USCS_Node* NewRootNode = nullptr;
			for (USCS_Node* N : Blueprint->SimpleConstructionScript->GetAllNodes())
				if (N && N->GetVariableName().ToString().Equals(FComp, ESearchCase::IgnoreCase)) { NewRootNode = N; break; }
			if (!NewRootNode || !Cast<USceneComponent>(NewRootNode->ComponentTemplate))
			{ Session.Log(FString::Printf(TEXT("[FAIL] set_root_component(\"%s\") -> not found or not SceneComponent"), *FComp)); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaSetRoot", "Set Root Component"));

			USCS_Node* CurrentRoot = nullptr;
			for (USCS_Node* RN : Blueprint->SimpleConstructionScript->GetRootNodes())
				if (RN && Cast<USceneComponent>(RN->ComponentTemplate)) { CurrentRoot = RN; break; }

			if (CurrentRoot == NewRootNode) { Session.Log(FString::Printf(TEXT("[OK] set_root_component(\"%s\") -> already root"), *FComp)); return sol::make_object(S, true); }

			USCS_Node* OldParent = Blueprint->SimpleConstructionScript->FindParentNode(NewRootNode);
			if (OldParent) OldParent->RemoveChildNode(NewRootNode, false);
			else Blueprint->SimpleConstructionScript->RemoveNode(NewRootNode, false);

			if (CurrentRoot)
			{
				TArray<USCS_Node*> Children = CurrentRoot->GetChildNodes();
				for (USCS_Node* Child : Children)
					if (Child && Child != NewRootNode) { CurrentRoot->RemoveChildNode(Child, false); NewRootNode->AddChildNode(Child, false); }
				Blueprint->SimpleConstructionScript->RemoveNode(CurrentRoot, false);
				NewRootNode->AddChildNode(CurrentRoot, true);
			}
			Blueprint->SimpleConstructionScript->AddNode(NewRootNode);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			Session.Log(FString::Printf(TEXT("[OK] set_root_component(\"%s\")"), *FComp));
			return sol::make_object(S, true);
		});

		// bp:add_event_graph(name)
		BP.set_function("add_event_graph", [&Session, FPath, PathStr](sol::table /*self*/,
			const std::string& GraphName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FGraphName = UTF8_TO_TCHAR(GraphName.c_str());
			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *FPath);
			if (!Blueprint) { Session.Log(TEXT("[FAIL] add_event_graph -> blueprint not found")); return sol::lua_nil; }

			for (UEdGraph* G : Blueprint->UbergraphPages)
				if (G && G->GetName().Equals(FGraphName, ESearchCase::IgnoreCase))
				{ Session.Log(FString::Printf(TEXT("[FAIL] add_event_graph(\"%s\") -> already exists"), *FGraphName)); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaAddEG", "Add Event Graph"));
			UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
				Blueprint, FName(*FGraphName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			if (!NewGraph) { Session.Log(FString::Printf(TEXT("[FAIL] add_event_graph(\"%s\") -> failed"), *FGraphName)); return sol::lua_nil; }

			FBlueprintEditorUtils::AddUbergraphPage(Blueprint, NewGraph);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			Session.Log(FString::Printf(TEXT("[OK] add_event_graph(\"%s\")"), *FGraphName));
			return BuildGraphObject(Lua, Session, NewGraph, PathStr, GraphName);
		});

		// bp:compile()
		// bp:info() -> table with blueprint metadata
		BP.set_function("info", [&Session, FPath](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FBlueprintInfo BPInfo = NeoBlueprint::LoadBlueprint(FPath);
			if (!BPInfo.Blueprint)
			{
				Session.Log(TEXT("[FAIL] info() -> blueprint not found"));
				return sol::lua_nil;
			}
			UBlueprint* Blueprint = BPInfo.Blueprint;

			sol::table Result = Lua.create_table();
			Result["name"] = TCHAR_TO_UTF8(*BPInfo.Name);
			Result["path"] = TCHAR_TO_UTF8(*BPInfo.AssetPath);
			Result["parent_class"] = TCHAR_TO_UTF8(*BPInfo.ParentClass);
			Result["type"] = TCHAR_TO_UTF8(*Blueprint->GetClass()->GetName());

			// Counts — match exactly what open_asset populates in abp.graphs:
			// top-level pages + recursive SubGraphs + AnimBP state machine graphs
			int32 NumGraphs = Blueprint->UbergraphPages.Num() + Blueprint->FunctionGraphs.Num() + Blueprint->MacroGraphs.Num();
			int32 TotalNodes = 0;
			for (UEdGraph* G : Blueprint->UbergraphPages) if (G) TotalNodes += G->Nodes.Num();
			for (UEdGraph* G : Blueprint->FunctionGraphs) if (G) TotalNodes += G->Nodes.Num();
			for (UEdGraph* G : Blueprint->MacroGraphs) if (G) TotalNodes += G->Nodes.Num();

			// Count recursive SubGraphs (collapsed/composite nodes)
			TSet<UEdGraph*> CountedGraphs;
			TFunction<void(UEdGraph*)> CountSubGraphs = [&](UEdGraph* Parent)
			{
				for (UEdGraph* Sub : Parent->SubGraphs)
				{
					if (!Sub || CountedGraphs.Contains(Sub)) continue;
					CountedGraphs.Add(Sub);
					NumGraphs++;
					TotalNodes += Sub->Nodes.Num();
					CountSubGraphs(Sub);
				}
			};
			for (UEdGraph* G : Blueprint->UbergraphPages) if (G) CountSubGraphs(G);
			for (UEdGraph* G : Blueprint->FunctionGraphs) if (G) CountSubGraphs(G);
			for (UEdGraph* G : Blueprint->MacroGraphs) if (G) CountSubGraphs(G);

			// AnimBP: count SM sub-graphs (state machines, states, transitions, layers)
			if (Blueprint->IsA<UAnimBlueprint>())
			{
				TArray<TPair<FString, UEdGraph*>> AnimGraphs;
				NeoBlueprint::CollectAnimBPGraphs(Blueprint, AnimGraphs);
				for (const auto& Pair : AnimGraphs)
				{
					if (!Pair.Value || CountedGraphs.Contains(Pair.Value)) continue;
					CountedGraphs.Add(Pair.Value);
					NumGraphs++;
					TotalNodes += Pair.Value->Nodes.Num();
				}
			}

			Result["num_graphs"] = NumGraphs;
			Result["num_nodes"] = TotalNodes;
			Result["num_variables"] = Blueprint->NewVariables.Num();
			Result["num_components"] = Blueprint->SimpleConstructionScript
				? Blueprint->SimpleConstructionScript->GetAllNodes().Num() : 0;
			Result["num_interfaces"] = Blueprint->ImplementedInterfaces.Num();
			Result["num_functions"] = Blueprint->FunctionGraphs.Num();
			Result["num_macros"] = Blueprint->MacroGraphs.Num();
			Result["num_event_graphs"] = Blueprint->UbergraphPages.Num();

			// Blueprint flags
			Result["is_actor"] = Blueprint->ParentClass && Blueprint->ParentClass->IsChildOf(AActor::StaticClass());
			Result["is_widget"] = Blueprint->ParentClass && Blueprint->ParentClass->IsChildOf(UUserWidget::StaticClass());
			Result["is_anim_bp"] = Blueprint->IsA<UAnimBlueprint>();

			// Graphs array — list all graph names and types
			sol::table GraphsArray = Lua.create_table();
			int32 GIdx = 1;
			auto AddGraphInfo = [&](UEdGraph* G, const char* GraphType)
			{
				if (!G) return;
				sol::table GEntry = Lua.create_table();
				GEntry["name"] = TCHAR_TO_UTF8(*G->GetName());
				GEntry["type"] = GraphType;
				GEntry["num_nodes"] = G->Nodes.Num();
				GraphsArray[GIdx++] = GEntry;
			};
			for (UEdGraph* G : Blueprint->UbergraphPages) AddGraphInfo(G, "event_graph");
			for (UEdGraph* G : Blueprint->FunctionGraphs) AddGraphInfo(G, "function");
			for (UEdGraph* G : Blueprint->MacroGraphs) AddGraphInfo(G, "macro");

			// Include recursive SubGraphs
			TSet<UEdGraph*> ListedGraphs;
			TFunction<void(UEdGraph*)> ListSubGraphs = [&](UEdGraph* Parent)
			{
				for (UEdGraph* Sub : Parent->SubGraphs)
				{
					if (!Sub || ListedGraphs.Contains(Sub)) continue;
					ListedGraphs.Add(Sub);
					AddGraphInfo(Sub, "subgraph");
					ListSubGraphs(Sub);
				}
			};
			for (UEdGraph* G : Blueprint->UbergraphPages) if (G) ListSubGraphs(G);
			for (UEdGraph* G : Blueprint->FunctionGraphs) if (G) ListSubGraphs(G);
			for (UEdGraph* G : Blueprint->MacroGraphs) if (G) ListSubGraphs(G);

			// AnimBP: include SM sub-graphs in the graphs array
			if (Blueprint->IsA<UAnimBlueprint>())
			{
				TArray<TPair<FString, UEdGraph*>> AnimGraphs;
				NeoBlueprint::CollectAnimBPGraphs(Blueprint, AnimGraphs);
				for (const auto& Pair : AnimGraphs)
				{
					if (!Pair.Value || ListedGraphs.Contains(Pair.Value)) continue;
					ListedGraphs.Add(Pair.Value);
					sol::table GEntry = Lua.create_table();
					GEntry["name"] = TCHAR_TO_UTF8(*Pair.Key);
					if (Pair.Key.Contains(TEXT("->")))
						GEntry["type"] = "transition";
					else if (Pair.Key.Contains(TEXT("/")))
						GEntry["type"] = "state";
					else
						GEntry["type"] = "state_machine";
					GEntry["num_nodes"] = Pair.Value->Nodes.Num();
					GraphsArray[GIdx++] = GEntry;
				}
			}

			Result["graphs"] = GraphsArray;

			Session.Log(FString::Printf(TEXT("[OK] info() -> %s, %d graphs, %d nodes, %d vars, %d components"),
				*BPInfo.Name, NumGraphs, TotalNodes, Blueprint->NewVariables.Num(),
				Blueprint->SimpleConstructionScript ? Blueprint->SimpleConstructionScript->GetAllNodes().Num() : 0));
			return Result;
		});

		// bp:help() -> string describing available methods
		BP.set_function("help", [&Session](sol::table self, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			std::string Out =
				"=== Blueprint methods ===\n"
				"  info() — structured metadata (graphs, variables, components, etc.)\n"
				"  help() — this help text\n"
				"\n--- Variables ---\n"
				"  add_variable(name, type, opts?) — add a member variable\n"
				"    opts: {default, category, tooltip, replicated, rep_notify, edit_anywhere, edit_defaults_only,\n"
				"      edit_instance_only, blueprint_read_only, save_game, transient, expose_on_spawn,\n"
				"      interp, advanced_display, deprecated, metadata={key=val}, duplicate_from=\"VarName\"}\n"
				"  rename_variable(old, new) — rename a variable\n"
				"  remove_variable(name) — remove a variable\n"
				"\n--- Components ---\n"
				"  add_component(name, class, parent?) — add a component\n"
				"  rename_component(old, new) — rename a component\n"
				"  duplicate_component(name) — duplicate a component\n"
				"  reparent_component(name, new_parent) — reparent a component\n"
				"  set_root_component(name) — set as root component\n"
				"  remove(\"component\", name) — remove a component\n"
				"\n--- Functions & Events ---\n"
				"  add_function(name, opts?) — add a function graph\n"
				"    opts: {params/inputs={{name,type},...}, returns/outputs={{name,type},...}, pure, const_func, category}\n"
				"  override_function(name) — override a parent function\n"
				"  add_macro(name) — add a macro graph\n"
				"  add_custom_event(name, opts?) — add a custom event\n"
				"    opts: {params={{name,type},...}} or direct {{name,type},...} for params only\n"
				"  remove_custom_event(name) — remove a custom event\n"
				"  add_event_dispatcher(name, params?) — add an event dispatcher\n"
				"  remove_event_dispatcher(name) — remove an event dispatcher\n"
				"  add_event_graph(name) — add an event graph\n"
				"\n--- Events Binding ---\n"
				"  list_events(source?) — list delegate events on a component (or all if omitted)\n"
				"  bind_event(component, event) — bind a component/widget delegate event\n"
				"  unbind_event(event) — unbind an event\n"
				"\n--- Interfaces ---\n"
				"  add_interface(name) — implement an interface\n"
				"  remove_interface(name) — remove an interface\n"
				"\n--- State Machines (AnimBP) ---\n"
				"  add_state_machine(name, graph?) — add a state machine\n"
				"  add_state(state_machine, name) — add a state to a state machine\n"
				"  add_transition(state_machine, from, to) — add a transition between states\n"
				"  add_linked_anim_layer(interface) — add a linked anim layer\n"
				"\n--- Widgets (UMG) ---\n"
				"  add_widget(class, name, parent?) — add a widget\n"
				"  remove_widget(name) — remove a widget\n"
				"  configure_widget(name, props) — configure widget properties\n"
				"  rename_widget(old_name, new_name) — rename a widget\n"
				"  wrap_widgets(wrapper_class, {names}) — wrap widgets in a container\n"
				"  replace_widget(name, new_class) — replace widget with different class\n"
				"\n--- Timelines ---\n"
				"  add_timeline(name, opts?) — add a timeline\n"
				"    opts: {length, auto_play, loop, replicated, ignore_time_dilation,\n"
				"      float_tracks={{name,keys={{time,val},...}},...}, vector_tracks=..., color_tracks=..., event_tracks=...,\n"
				"      tracks={{name,type,keys=...},...}}\n"
				"\n--- Graph Operations ---\n"
				"  find_nodes(query, max?) — search for node types\n"
				"  add_comment(text, x, y, w?, h?) — add a comment node\n"
				"  break_connection(node, pin) — break connections\n"
				"  align_nodes(handles, axis) — align nodes\n"
				"\n--- Properties ---\n"
				"  set_property(name, props) — set variable props (interp, advanced_display, deprecated, metadata={...})\n"
				"    For functions: description, keywords, compact_title, deprecated, deprecation_message, call_in_editor, thread_safe\n"
				"  set(target, property, value) — set property via reflection\n"
				"  get(target, property) — read a property\n"
				"  list_properties(target, filter?) — list properties\n"
				"\n--- Other ---\n"
				"  rename(type, old, new) — rename function/variable\n"
				"  remove(type, name) — remove function/variable/component\n"
				"  reparent(new_parent_class) — change parent class\n"
				"  compile() — compile the blueprint\n"
				"  save() — save to disk\n"
				"\n--- Data Access ---\n"
				"  .graphs — table of graph objects (by name and index)\n"
				"  .variables — table of variable info (by name)\n"
				"  .components — table of component info (by name)\n"
				"  .interfaces — table of implemented interfaces\n"
				"  .name, .path, .parent_class — basic info\n"
				"\n--- Graph Object Methods (on .graphs[\"name\"]) ---\n"
				"  add_node(node, x?, y?) — add a node to this graph\n"
				"  connect(from_handle, from_pin, to_handle, to_pin) — connect pins\n"
				"  disconnect(handle, pin_name) — break all connections on a pin\n"
				"  disconnect_from(from_handle, from_pin, to_handle, to_pin) — break a specific connection\n"
				"  set_pin(handle, pin_name, value) — set pin default value\n"
				"  get_pin(handle, pin_name) — read pin default value\n"
				"  delete_node(handle) — delete a node\n"
				"  move_node(handle, x, y, relative?) — move node position\n"
				"  split_pin(handle, pin_name) — split a struct pin\n"
				"  recombine_pin(handle, pin_name) — recombine a split pin\n"
				"  add_exec_pin(handle) — add execution pin (AddPins)\n"
				"  remove_exec_pin(handle) — remove last execution pin\n"
				"  reset_pin(handle, pin_name) — reset pin to default\n"
				"  set_node_comment(handle, text, visible?) — set node comment\n"
				"  .nodes — table of all nodes in the graph\n";

			// Append enrichment-specific help (e.g., WidgetBlueprint, GameplayAbility)
			sol::optional<std::string> Extra = self.get<sol::optional<std::string>>("_help_text");
			if (Extra.has_value() && !Extra.value().empty())
			{
				Out += "\n" + Extra.value();
			}

			Session.Log(FString::Printf(TEXT("[OK] help()\n%s"), UTF8_TO_TCHAR(Out.c_str())));
			return sol::make_object(Lua, Out);
		});

		BP.set_function("compile", [&Session, FPath](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			FBlueprintInfo CompileInfo = NeoBlueprint::LoadBlueprint(FPath);
			if (!CompileInfo.Blueprint)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] compile(\"%s\") -> blueprint not found"), *FPath));
				return sol::lua_nil;
			}

			FCompileResult Result = NeoBlueprint::CompileBlueprint(CompileInfo.Blueprint);

			if (Result.bSuccess)
			{
				Session.Log(FString::Printf(TEXT("[OK] compile(\"%s\") -> success"), *FPath));
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[FAIL] compile(\"%s\") -> errors"), *FPath));
			}

			return sol::make_object(S, Result.bSuccess);
		});

		// bp:save()
		BP.set_function("save", [&Session, FPath](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			FBlueprintInfo SaveInfo = NeoBlueprint::LoadBlueprint(FPath);
			if (!SaveInfo.Blueprint)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] save(\"%s\") -> blueprint not found"), *FPath));
				return sol::lua_nil;
			}

			bool bSaved = NeoBlueprint::SaveBlueprint(SaveInfo.Blueprint);
			if (bSaved)
			{
				Session.Log(FString::Printf(TEXT("[OK] save(\"%s\") -> saved"), *FPath));
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[FAIL] save(\"%s\") -> save failed"), *FPath));
			}

			return sol::make_object(S, bSaved);
		});

		int32 TotalNodes = 0;
		for (UEdGraph* Graph : Info.Blueprint->UbergraphPages)
			if (Graph) TotalNodes += Graph->Nodes.Num();
		for (UEdGraph* Graph : Info.Blueprint->FunctionGraphs)
			if (Graph) TotalNodes += Graph->Nodes.Num();
		for (UEdGraph* Graph : Info.Blueprint->MacroGraphs)
			if (Graph) TotalNodes += Graph->Nodes.Num();

		int32 NumVars = Info.Blueprint->NewVariables.Num();
		int32 NumComps = Info.Blueprint->SimpleConstructionScript
			? Info.Blueprint->SimpleConstructionScript->GetAllNodes().Num() : 0;

		Session.Log(FString::Printf(TEXT("[OK] open_blueprint(\"%s\") -> %d graphs, %d nodes, %d variables, %d components"),
			*FPath, Info.Graphs.Num(), TotalNodes, NumVars, NumComps));

		return BP;
	});
}

REGISTER_LUA_BINDING(Blueprint, BlueprintDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindBlueprint(Lua, Session);
});
