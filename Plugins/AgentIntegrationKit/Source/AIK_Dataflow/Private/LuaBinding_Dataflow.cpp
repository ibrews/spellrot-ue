// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include <sol/sol.hpp>
#include "Tools/NeoStackToolUtils.h"

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
#include "Dataflow/DataflowObject.h"
#include "Dataflow/DataflowGraph.h"
#include "Dataflow/DataflowNode.h"
#include "Dataflow/DataflowNodeFactory.h"
#include "Dataflow/DataflowTerminalNode.h"
#include "Dataflow/DataflowEdNode.h"
#if ENGINE_MINOR_VERSION >= 6
#include "Dataflow/DataflowEditorBlueprintLibrary.h"
#endif
#include "Dataflow/DataflowBlueprintLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/UObjectGlobals.h"
#endif // ENGINE_MINOR_VERSION >= 5

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5

// ─── Documentation ───

static TArray<FLuaFunctionDoc> DataflowDocs = {
	{ TEXT("dataflow_list_node_types(filter?)"), TEXT("List all registered Dataflow node types with optional filter"), TEXT("table[]") },
	{ TEXT("dataflow_get_graph(asset_path)"), TEXT("Get graph info — nodes and connections for a Dataflow asset"), TEXT("table or nil") },
	{ TEXT("dataflow_add_node(asset_path, type_name, name?, x?, y?)"), TEXT("Add a node to a Dataflow graph"), TEXT("string or nil") },
	{ TEXT("dataflow_connect(asset_path, from_node, output_pin, to_node, input_pin)"), TEXT("Connect output pin to input pin between Dataflow nodes"), TEXT("true or nil") },
	{ TEXT("dataflow_set_property(asset_path, node_name, property_name, value)"), TEXT("Set a property on a Dataflow node"), TEXT("true or nil") },
	{ TEXT("dataflow_get_property(asset_path, node_name, property_name)"), TEXT("Get a property value from a Dataflow node"), TEXT("string or nil") },
	{ TEXT("dataflow_remove_node(asset_path, node_name)"), TEXT("Remove a node from a Dataflow graph"), TEXT("true or nil") },
	{ TEXT("dataflow_get_node_info(asset_path, node_name)"), TEXT("Get detailed info about a Dataflow node: type, pins, properties"), TEXT("table or nil") },
	{ TEXT("dataflow_disconnect(asset_path, from_node, output_pin, to_node, input_pin)"), TEXT("Disconnect pins between Dataflow nodes"), TEXT("true or nil") },
	{ TEXT("dataflow_set_frozen(asset_path, node_name, is_frozen)"), TEXT("Set the frozen state of a Dataflow node"), TEXT("true or nil") },
	{ TEXT("dataflow_set_active(asset_path, node_name, is_active)"), TEXT("Set the active state of a Dataflow node"), TEXT("true or nil") },
	{ TEXT("dataflow_rename_node(asset_path, node_name, new_name)"), TEXT("Rename a Dataflow node"), TEXT("string (new name) or nil") },
	{ TEXT("dataflow_can_connect(asset_path, from_node, output_pin, to_node, input_pin)"), TEXT("Check if two pins can be connected"), TEXT("true or false") },
	{ TEXT("dataflow_regenerate(asset_path, regenerate_dependents?)"), TEXT("Regenerate/evaluate a Dataflow asset by running all terminal nodes. If the Dataflow is owned by another asset (e.g. ChaosClothAsset), pass that asset path instead. regenerate_dependents defaults to false."), TEXT("true or nil") },
	{ TEXT("dataflow_evaluate_terminal(asset_path, terminal_node_name)"), TEXT("Evaluate a specific terminal node in a Dataflow graph by name"), TEXT("true or nil") },
};

// ─── Helpers ───

static UDataflow* LoadDataflowAsset(const std::string& AssetPath, FLuaSessionData& Session)
{
	FString FPath = UTF8_TO_TCHAR(AssetPath.c_str());
	if (!FPath.StartsWith(TEXT("/")))
		FPath = TEXT("/Game/") + FPath;

	UObject* Loaded = StaticLoadObject(UDataflow::StaticClass(), nullptr, *FPath);
	UDataflow* Dataflow = Cast<UDataflow>(Loaded);
	if (!Dataflow)
	{
		Session.Log(FString::Printf(TEXT("[FAIL] Dataflow asset not found: %s"), *FPath));
	}
	return Dataflow;
}

// Helper: collect all UPROPERTY fields from a node's struct and populate a sol table
static void CollectNodeProperties(const FDataflowNode* Node, sol::state_view& Lua, sol::table& PropsTable)
{
	const UScriptStruct* ScriptStruct = Node->TypedScriptStruct();
	if (!ScriptStruct) return;

	const void* NodeMemory = Node;
	int32 PIdx = 1;
	for (TFieldIterator<FProperty> PropIt(ScriptStruct); PropIt; ++PropIt)
	{
		const FProperty* Prop = *PropIt;
		if (!Prop) continue;
		// Skip internal/hidden properties
		if (Prop->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient))
			continue;

		sol::table PEntry = Lua.create_table();
		PEntry["name"] = std::string(TCHAR_TO_UTF8(*Prop->GetName()));
		PEntry["type"] = std::string(TCHAR_TO_UTF8(*Prop->GetCPPType()));

		// Export current value as string
		FString ValueStr;
		Prop->ExportText_Direct(ValueStr, Prop->ContainerPtrToValuePtr<void>(NodeMemory), nullptr, nullptr, PPF_None);
		PEntry["value"] = std::string(TCHAR_TO_UTF8(*ValueStr));

		if (Prop->HasAnyPropertyFlags(CPF_EditConst))
			PEntry["read_only"] = true;

		PropsTable[PIdx++] = PEntry;
	}
}

// ─── Binding ───

static void BindDataflow(sol::state& Lua, FLuaSessionData& Session)
{
	// ---- dataflow_list_node_types(filter?) ----
	Lua.set_function("dataflow_list_node_types", [&Session](sol::optional<std::string> Filter, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);

		UE::Dataflow::FNodeFactory* Factory = UE::Dataflow::FNodeFactory::GetInstance();
		if (!Factory)
		{
			Session.Log(TEXT("[FAIL] dataflow_list_node_types -> node factory not available"));
			return sol::lua_nil;
		}

		TArray<UE::Dataflow::FFactoryParameters> AllParams = Factory->RegisteredParameters();
		FString FilterStr = Filter.has_value() ? UTF8_TO_TCHAR(Filter.value().c_str()) : FString();

		sol::table Result = Lua.create_table();
		int32 Idx = 1;
		for (const auto& Param : AllParams)
		{
			if (!FilterStr.IsEmpty())
			{
				FString TypeStr = Param.TypeName.ToString();
				FString DisplayStr = Param.DisplayName.ToString();
				FString CatStr = Param.Category.ToString();
				if (!TypeStr.Contains(FilterStr) && !DisplayStr.Contains(FilterStr) && !CatStr.Contains(FilterStr))
					continue;
			}

			sol::table Entry = Lua.create_table();
			Entry["type"] = std::string(TCHAR_TO_UTF8(*Param.TypeName.ToString()));
			Entry["display_name"] = std::string(TCHAR_TO_UTF8(*Param.DisplayName.ToString()));
			Entry["category"] = std::string(TCHAR_TO_UTF8(*Param.Category.ToString()));
			Entry["tags"] = std::string(TCHAR_TO_UTF8(*Param.Tags));
			Entry["deprecated"] = Param.IsDeprecated();
			Entry["experimental"] = Param.IsExperimental();
			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] dataflow_list_node_types -> %d types%s"),
			Idx - 1, FilterStr.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" matching '%s'"), *FilterStr)));
		return sol::make_object(Lua, Result);
	});

	// ---- dataflow_get_graph(asset_path) ----
	Lua.set_function("dataflow_get_graph", [&Session](const std::string& AssetPath, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDataflow* Dataflow = LoadDataflowAsset(AssetPath, Session);
		if (!Dataflow) return sol::lua_nil;

		TSharedPtr<const UE::Dataflow::FGraph> Graph = Dataflow->GetDataflow();
		if (!Graph)
		{
			Session.Log(TEXT("[FAIL] dataflow_get_graph -> graph data is null"));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();
		Result["active"] = Dataflow->bActive;

		// Nodes
		const TArray<TSharedPtr<FDataflowNode>>& Nodes = Graph->GetNodes();
		sol::table NodeList = Lua.create_table();
		int32 Idx = 1;
		for (const auto& Node : Nodes)
		{
			if (!Node) continue;
			sol::table NEntry = Lua.create_table();
			NEntry["name"] = std::string(TCHAR_TO_UTF8(*Node->GetName().ToString()));
			NEntry["type"] = std::string(TCHAR_TO_UTF8(*Node->GetType().ToString()));
			NEntry["guid"] = std::string(TCHAR_TO_UTF8(*Node->GetGuid().ToString()));

			// Inputs — try GetInputs() first, fall back to GetPins() if empty
			TArray<FDataflowInput*> Inputs = Node->GetInputs();
			TArray<FDataflowOutput*> Outputs = Node->GetOutputs();

			// If both are empty, try GetPins() as fallback (some node types
			// only populate pins through GetPins, not via ExpandedInputs/ExpandedOutputs)
			TArray<UE::Dataflow::FPin> FallbackPins;
			if (Inputs.Num() == 0 && Outputs.Num() == 0)
			{
				FallbackPins = Node->GetPins();
			}

			sol::table InputList = Lua.create_table();
			sol::table OutputList = Lua.create_table();
			int32 IIdx = 1;
			int32 OIdx = 1;

			if (FallbackPins.Num() > 0)
			{
				// Use GetPins() data
				for (const auto& Pin : FallbackPins)
				{
					sol::table PEntry = Lua.create_table();
					PEntry["name"] = std::string(TCHAR_TO_UTF8(*Pin.Name.ToString()));
					PEntry["type"] = std::string(TCHAR_TO_UTF8(*Pin.Type.ToString()));
					if (Pin.Direction == UE::Dataflow::FPin::EDirection::INPUT)
					{
						InputList[IIdx++] = PEntry;
					}
					else
					{
						OutputList[OIdx++] = PEntry;
					}
				}
			}
			else
			{
				// Use GetInputs()/GetOutputs() data
				for (FDataflowInput* Input : Inputs)
				{
					if (!Input) continue;
					sol::table IEntry = Lua.create_table();
					IEntry["name"] = std::string(TCHAR_TO_UTF8(*Input->GetName().ToString()));
					IEntry["type"] = std::string(TCHAR_TO_UTF8(*Input->GetType().ToString()));
					IEntry["connected"] = Input->GetConnectedOutputs().Num() > 0;
					InputList[IIdx++] = IEntry;
				}
				for (FDataflowOutput* Output : Outputs)
				{
					if (!Output) continue;
					sol::table OEntry = Lua.create_table();
					OEntry["name"] = std::string(TCHAR_TO_UTF8(*Output->GetName().ToString()));
					OEntry["type"] = std::string(TCHAR_TO_UTF8(*Output->GetType().ToString()));
					OutputList[OIdx++] = OEntry;
				}
			}
			NEntry["inputs"] = InputList;
			NEntry["outputs"] = OutputList;

			NodeList[Idx++] = NEntry;
		}
		Result["nodes"] = NodeList;
		Result["node_count"] = Nodes.Num();

		// Connections
		const TArray<UE::Dataflow::FLink>& Links = Graph->GetConnections();
		sol::table ConnList = Lua.create_table();
		int32 CIdx = 1;
		for (const auto& Link : Links)
		{
			sol::table CEntry = Lua.create_table();

			// Resolve node names and pin names from GUIDs
			TSharedPtr<const FDataflowNode> FromNode = Graph->FindBaseNode(Link.OutputNode);
			TSharedPtr<const FDataflowNode> ToNode = Graph->FindBaseNode(Link.InputNode);
			if (FromNode)
			{
				CEntry["from_node"] = std::string(TCHAR_TO_UTF8(*FromNode->GetName().ToString()));
				const FDataflowOutput* OutputPin = FromNode->FindOutput(Link.Output);
				if (OutputPin)
				{
					CEntry["output_pin"] = std::string(TCHAR_TO_UTF8(*OutputPin->GetName().ToString()));
				}
				else if (FromNode->NumOutputs() == 1)
				{
					// Single-output node: use its only output pin name
					TArray<FDataflowOutput*> AllOutputs = FromNode->GetOutputs();
					if (AllOutputs.Num() > 0 && AllOutputs[0])
					{
						CEntry["output_pin"] = std::string(TCHAR_TO_UTF8(*AllOutputs[0]->GetName().ToString()));
					}
				}
			}
			if (ToNode)
			{
				CEntry["to_node"] = std::string(TCHAR_TO_UTF8(*ToNode->GetName().ToString()));
				const FDataflowInput* InputPin = ToNode->FindInput(Link.Input);
				if (InputPin)
				{
					CEntry["input_pin"] = std::string(TCHAR_TO_UTF8(*InputPin->GetName().ToString()));
				}
				else if (ToNode->GetNumInputs() == 1)
				{
					// Single-input node: use its only input pin name
					TArray<FDataflowInput*> AllInputs = ToNode->GetInputs();
					if (AllInputs.Num() > 0 && AllInputs[0])
					{
						CEntry["input_pin"] = std::string(TCHAR_TO_UTF8(*AllInputs[0]->GetName().ToString()));
					}
				}
			}

			ConnList[CIdx++] = CEntry;
		}
		Result["connections"] = ConnList;
		Result["connection_count"] = Links.Num();

		Session.Log(FString::Printf(TEXT("[OK] dataflow_get_graph -> %d nodes, %d connections"), Nodes.Num(), Links.Num()));
		return sol::make_object(Lua, Result);
	});

	// ---- dataflow_add_node(asset_path, type_name, name?, x?, y?) ----
	Lua.set_function("dataflow_add_node", [&Session](const std::string& AssetPath, const std::string& TypeName,
		sol::optional<std::string> Name, sol::optional<double> X, sol::optional<double> Y, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDataflow* Dataflow = LoadDataflowAsset(AssetPath, Session);
		if (!Dataflow) return sol::lua_nil;

		FName FTypeName = FName(UTF8_TO_TCHAR(TypeName.c_str()));
		FName FBaseName = Name.has_value() ? FName(UTF8_TO_TCHAR(Name.value().c_str())) : FTypeName;
		FVector2D Location(X.value_or(0.0), Y.value_or(0.0));

	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
		FName ResultName = UDataflowEditorBlueprintLibrary::AddDataflowNode(Dataflow, FTypeName, FBaseName, Location);
#else
		FName ResultName = NAME_None;
		Session.Log(TEXT("[FAIL] dataflow_add_node -> requires UE 5.6+"));
#endif
		if (ResultName.IsNone())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] dataflow_add_node -> failed to add node of type '%s'"), *FTypeName.ToString()));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] dataflow_add_node -> added '%s' (type: %s)"), *ResultName.ToString(), *FTypeName.ToString()));
		return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*ResultName.ToString())));
	});

	// ---- dataflow_connect(asset_path, from_node, output_pin, to_node, input_pin) ----
	Lua.set_function("dataflow_connect", [&Session](const std::string& AssetPath,
		const std::string& FromNode, const std::string& OutputPin,
		const std::string& ToNode, const std::string& InputPin, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDataflow* Dataflow = LoadDataflowAsset(AssetPath, Session);
		if (!Dataflow) return sol::lua_nil;

		FName FFrom = FName(UTF8_TO_TCHAR(FromNode.c_str()));
		FName FOutput = FName(UTF8_TO_TCHAR(OutputPin.c_str()));
		FName FTo = FName(UTF8_TO_TCHAR(ToNode.c_str()));
		FName FInput = FName(UTF8_TO_TCHAR(InputPin.c_str()));

	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
		bool bConnected = UDataflowEditorBlueprintLibrary::ConnectDataflowNodes(Dataflow, FFrom, FOutput, FTo, FInput);
#else
		bool bConnected = false;
#endif
		if (!bConnected)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] dataflow_connect -> failed to connect %s.%s -> %s.%s"),
				*FFrom.ToString(), *FOutput.ToString(), *FTo.ToString(), *FInput.ToString()));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] dataflow_connect -> %s.%s -> %s.%s"),
			*FFrom.ToString(), *FOutput.ToString(), *FTo.ToString(), *FInput.ToString()));
		return sol::make_object(Lua, true);
	});

	// ---- dataflow_set_property(asset_path, node_name, property_name, value) ----
	// NOTE: UDataflowEditorBlueprintLibrary::SetDataflowNodeProperty has a bug in UE 5.5-5.7:
	// it creates a temporary FStructOnScope copy, sets the property on the COPY, then discards it.
	// The actual node is never modified. We bypass this by setting properties directly on the node memory.
	Lua.set_function("dataflow_set_property", [&Session](const std::string& AssetPath,
		const std::string& NodeName, const std::string& PropName, const std::string& Value, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDataflow* Dataflow = LoadDataflowAsset(AssetPath, Session);
		if (!Dataflow) return sol::lua_nil;

		TSharedPtr<UE::Dataflow::FGraph> Graph = Dataflow->GetDataflow();
		if (!Graph)
		{
			Session.Log(TEXT("[FAIL] dataflow_set_property -> graph data is null"));
			return sol::lua_nil;
		}

		FName FNodeName = FName(UTF8_TO_TCHAR(NodeName.c_str()));
		FName FPropName = FName(UTF8_TO_TCHAR(PropName.c_str()));
		FString FValue = UTF8_TO_TCHAR(Value.c_str());

		TSharedPtr<FDataflowNode> Node = Graph->FindBaseNode(FNodeName);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] dataflow_set_property -> node '%s' not found"), *FNodeName.ToString()));
			return sol::lua_nil;
		}

		const UScriptStruct* ScriptStruct = Node->TypedScriptStruct();
		if (!ScriptStruct)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] dataflow_set_property -> node '%s' has no script struct"), *FNodeName.ToString()));
			return sol::lua_nil;
		}

		const FProperty* Property = ScriptStruct->FindPropertyByName(FPropName);
		if (!Property)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] dataflow_set_property -> property '%s' not found on node '%s'"),
				*FPropName.ToString(), *FNodeName.ToString()));
			return sol::lua_nil;
		}

		// Get pointer to the ACTUAL node memory (the node IS the struct instance)
		void* NodeMemory = Node.Get();
		bool bSet = false;

		FScopedTransaction Transaction(FText::FromString(TEXT("Dataflow Set Property")));
		Dataflow->Modify();

		// For object properties, try loading the asset and setting directly
		if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Property))
		{
			if (!FValue.IsEmpty())
			{
				UObject* LoadedAsset = StaticLoadObject(UObject::StaticClass(), nullptr, *FValue);
				if (LoadedAsset)
				{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
					// Try the virtual SetAssetProperty first (handles type-specific logic, available 5.6+)
					if (Node->SupportsAssetProperty(LoadedAsset))
					{
						Node->SetAssetProperty(LoadedAsset);
						bSet = true;
					}
					else
#endif
					{
						// Direct property assignment via reflection on the node's own memory
						ObjProp->SetObjectPropertyValue(Property->ContainerPtrToValuePtr<void>(NodeMemory), LoadedAsset);
						bSet = true;
					}
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] dataflow_set_property -> could not load asset '%s'"), *FValue));
					return sol::lua_nil;
				}
			}
			else
			{
				// Empty value = clear the reference
				ObjProp->SetObjectPropertyValue(Property->ContainerPtrToValuePtr<void>(NodeMemory), nullptr);
				bSet = true;
			}
		}
		else
		{
			// For non-object properties, use ImportText directly on node memory
			const TCHAR* ImportResult = Property->ImportText_Direct(*FValue,
				Property->ContainerPtrToValuePtr<void>(NodeMemory), nullptr, PPF_None);
			bSet = (ImportResult != nullptr);

			if (!bSet)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] dataflow_set_property -> ImportText failed for %s.%s = '%s'"),
					*FNodeName.ToString(), *FPropName.ToString(), *FValue));
				return sol::lua_nil;
			}
		}

		if (bSet)
		{
			// Invalidate the node hash so the dataflow knows to re-evaluate
			Node->Invalidate();
			Dataflow->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] dataflow_set_property -> %s.%s = %s"),
				*FNodeName.ToString(), *FPropName.ToString(), *FValue));
			return sol::make_object(Lua, true);
		}

		Session.Log(FString::Printf(TEXT("[FAIL] dataflow_set_property -> failed to set %s.%s"),
			*FNodeName.ToString(), *FPropName.ToString()));
		return sol::lua_nil;
	});

	// ---- dataflow_get_property(asset_path, node_name, property_name) ----
	Lua.set_function("dataflow_get_property", [&Session](const std::string& AssetPath,
		const std::string& NodeName, const std::string& PropName, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDataflow* Dataflow = LoadDataflowAsset(AssetPath, Session);
		if (!Dataflow) return sol::lua_nil;

		TSharedPtr<const UE::Dataflow::FGraph> Graph = Dataflow->GetDataflow();
		if (!Graph)
		{
			Session.Log(TEXT("[FAIL] dataflow_get_property -> graph data is null"));
			return sol::lua_nil;
		}

		FName FNodeName = FName(UTF8_TO_TCHAR(NodeName.c_str()));
		FName FPropName = FName(UTF8_TO_TCHAR(PropName.c_str()));

		TSharedPtr<const FDataflowNode> Node = Graph->FindBaseNode(FNodeName);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] dataflow_get_property -> node '%s' not found"), *FNodeName.ToString()));
			return sol::lua_nil;
		}

		const UScriptStruct* ScriptStruct = Node->TypedScriptStruct();
		if (!ScriptStruct)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] dataflow_get_property -> node '%s' has no script struct"), *FNodeName.ToString()));
			return sol::lua_nil;
		}

		const FProperty* Property = ScriptStruct->FindPropertyByName(FPropName);
		if (!Property)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] dataflow_get_property -> property '%s' not found on node '%s'"),
				*FPropName.ToString(), *FNodeName.ToString()));
			return sol::lua_nil;
		}

		const void* NodeMemory = Node.Get();
		FString ValueStr;
		Property->ExportText_Direct(ValueStr, Property->ContainerPtrToValuePtr<void>(NodeMemory), nullptr, nullptr, PPF_None);

		Session.Log(FString::Printf(TEXT("[OK] dataflow_get_property -> %s.%s = %s"),
			*FNodeName.ToString(), *FPropName.ToString(), *ValueStr));
		return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*ValueStr)));
	});

	// ---- dataflow_remove_node(asset_path, node_name) ----
	// Follows engine pattern from DataflowAssetEditUtils::DeleteNodesNoTransaction:
	// EdGraph->RemoveNode(EdNode) -> DataflowGraph->RemoveNode(DataflowNode) -> EdNode->Rename()
	Lua.set_function("dataflow_remove_node", [&Session](const std::string& AssetPath, const std::string& NodeName, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDataflow* Dataflow = LoadDataflowAsset(AssetPath, Session);
		if (!Dataflow) return sol::lua_nil;

		TSharedPtr<UE::Dataflow::FGraph> Graph = Dataflow->GetDataflow();
		if (!Graph)
		{
			Session.Log(TEXT("[FAIL] dataflow_remove_node -> graph data is null"));
			return sol::lua_nil;
		}

		FName FNodeName = FName(UTF8_TO_TCHAR(NodeName.c_str()));
		TSharedPtr<FDataflowNode> Node = Graph->FindBaseNode(FNodeName);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] dataflow_remove_node -> node '%s' not found"), *FNodeName.ToString()));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Dataflow Remove Node")));
		Dataflow->Modify();

		// Remove the corresponding EdNode first (engine pattern)
		if (UDataflowEdNode* EdNode = Dataflow->FindEdNodeByDataflowNodeGuid(Node->GetGuid()))
		{
			EdNode->Modify();
			Dataflow->RemoveNode(EdNode);
			// Free the name so it can be reused before GC
			EdNode->Rename(nullptr, GetTransientPackage());
		}

		Graph->RemoveNode(Node);
		Dataflow->MarkPackageDirty();

		Session.Log(FString::Printf(TEXT("[OK] dataflow_remove_node -> removed '%s'"), *FNodeName.ToString()));
		return sol::make_object(Lua, true);
	});

	// ---- dataflow_get_node_info(asset_path, node_name) ----
	Lua.set_function("dataflow_get_node_info", [&Session](const std::string& AssetPath, const std::string& NodeName, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDataflow* Dataflow = LoadDataflowAsset(AssetPath, Session);
		if (!Dataflow) return sol::lua_nil;

		TSharedPtr<const UE::Dataflow::FGraph> Graph = Dataflow->GetDataflow();
		if (!Graph)
		{
			Session.Log(TEXT("[FAIL] dataflow_get_node_info -> graph data is null"));
			return sol::lua_nil;
		}

		FName FNodeName = FName(UTF8_TO_TCHAR(NodeName.c_str()));
		TSharedPtr<const FDataflowNode> Node = Graph->FindBaseNode(FNodeName);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] dataflow_get_node_info -> node '%s' not found"), *FNodeName.ToString()));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();
		Result["name"] = std::string(TCHAR_TO_UTF8(*Node->GetName().ToString()));
		Result["type"] = std::string(TCHAR_TO_UTF8(*Node->GetType().ToString()));
		Result["display_name"] = std::string(TCHAR_TO_UTF8(*Node->GetDisplayName().ToString()));
		Result["category"] = std::string(TCHAR_TO_UTF8(*Node->GetCategory().ToString()));
		Result["tags"] = std::string(TCHAR_TO_UTF8(*Node->GetTags()));
		Result["tooltip"] = std::string(TCHAR_TO_UTF8(*Node->GetToolTip()));
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
		Result["frozen"] = Node->IsFrozen();
		Result["active"] = Node->IsActive();
#else
		Result["active"] = Node->bActive;
#endif

		// Node position from EdNode
		if (TObjectPtr<const UDataflowEdNode> EdNode = Dataflow->FindEdNodeByDataflowNodeGuid(Node->GetGuid()))
		{
			Result["x"] = EdNode->NodePosX;
			Result["y"] = EdNode->NodePosY;
		}

		// Pins — try GetPins() first as it's most reliable, also provide separate inputs/outputs
		TArray<UE::Dataflow::FPin> Pins = Node->GetPins();

		// If GetPins() is empty, try GetInputs()/GetOutputs() directly
		if (Pins.Num() == 0)
		{
			TArray<FDataflowInput*> DirectInputs = Node->GetInputs();
			TArray<FDataflowOutput*> DirectOutputs = Node->GetOutputs();
			for (FDataflowInput* In : DirectInputs)
			{
				if (!In) continue;
				UE::Dataflow::FPin P;
				P.Direction = UE::Dataflow::FPin::EDirection::INPUT;
				P.Name = In->GetName();
				P.Type = In->GetType();
				P.bHidden = false;
				Pins.Add(P);
			}
			for (FDataflowOutput* Out : DirectOutputs)
			{
				if (!Out) continue;
				UE::Dataflow::FPin P;
				P.Direction = UE::Dataflow::FPin::EDirection::OUTPUT;
				P.Name = Out->GetName();
				P.Type = Out->GetType();
				P.bHidden = false;
				Pins.Add(P);
			}
		}

		sol::table PinList = Lua.create_table();
		sol::table InputList = Lua.create_table();
		sol::table OutputList = Lua.create_table();
		int32 PIdx = 1, IIdx = 1, OIdx = 1;
		for (const auto& Pin : Pins)
		{
			sol::table PEntry = Lua.create_table();
			PEntry["name"] = std::string(TCHAR_TO_UTF8(*Pin.Name.ToString()));
			PEntry["direction"] = (Pin.Direction == UE::Dataflow::FPin::EDirection::INPUT) ? "input" : "output";
			PEntry["type"] = std::string(TCHAR_TO_UTF8(*Pin.Type.ToString()));
			PEntry["hidden"] = Pin.bHidden;
			PinList[PIdx++] = PEntry;

			// Also populate separate input/output lists
			sol::table NameEntry = Lua.create_table();
			NameEntry["name"] = PEntry["name"];
			NameEntry["type"] = PEntry["type"];
			if (Pin.Direction == UE::Dataflow::FPin::EDirection::INPUT)
				InputList[IIdx++] = NameEntry;
			else
				OutputList[OIdx++] = NameEntry;
		}
		Result["pins"] = PinList;
		Result["inputs"] = InputList;
		Result["outputs"] = OutputList;

		// Properties — list all editable UPROPERTYs with current values
		sol::table PropsTable = Lua.create_table();
		CollectNodeProperties(Node.Get(), Lua, PropsTable);
		Result["properties"] = PropsTable;

		Session.Log(FString::Printf(TEXT("[OK] dataflow_get_node_info -> '%s' (%s)"), *Node->GetName().ToString(), *Node->GetType().ToString()));
		return sol::make_object(Lua, Result);
	});

	// ---- dataflow_disconnect(asset_path, from_node, output_pin, to_node, input_pin) ----
	Lua.set_function("dataflow_disconnect", [&Session](const std::string& AssetPath,
		const std::string& FromNode, const std::string& OutputPin,
		const std::string& ToNode, const std::string& InputPin, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDataflow* Dataflow = LoadDataflowAsset(AssetPath, Session);
		if (!Dataflow) return sol::lua_nil;

		TSharedPtr<UE::Dataflow::FGraph> Graph = Dataflow->GetDataflow();
		if (!Graph)
		{
			Session.Log(TEXT("[FAIL] dataflow_disconnect -> graph data is null"));
			return sol::lua_nil;
		}

		FName FFrom = FName(UTF8_TO_TCHAR(FromNode.c_str()));
		FName FOutput = FName(UTF8_TO_TCHAR(OutputPin.c_str()));
		FName FTo = FName(UTF8_TO_TCHAR(ToNode.c_str()));
		FName FInput = FName(UTF8_TO_TCHAR(InputPin.c_str()));

		TSharedPtr<FDataflowNode> FromNodePtr = Graph->FindBaseNode(FFrom);
		TSharedPtr<FDataflowNode> ToNodePtr = Graph->FindBaseNode(FTo);
		if (!FromNodePtr || !ToNodePtr)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] dataflow_disconnect -> node not found (%s or %s)"),
				*FFrom.ToString(), *FTo.ToString()));
			return sol::lua_nil;
		}

		FDataflowOutput* Output = FromNodePtr->FindOutput(FOutput);
		FDataflowInput* Input = ToNodePtr->FindInput(FInput);
		if (!Output || !Input)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] dataflow_disconnect -> pin not found (%s.%s or %s.%s)"),
				*FFrom.ToString(), *FOutput.ToString(), *FTo.ToString(), *FInput.ToString()));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Dataflow Disconnect")));
		Dataflow->Modify();
		Graph->Disconnect(Output, Input);

		// Refresh EdNodes to update visual representation (matches ConnectDataflowNodes pattern)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
		Dataflow->RefreshEdNodeByGuid(FromNodePtr->GetGuid());
		Dataflow->RefreshEdNodeByGuid(ToNodePtr->GetGuid());
#endif

		Dataflow->MarkPackageDirty();

		Session.Log(FString::Printf(TEXT("[OK] dataflow_disconnect -> %s.%s -x- %s.%s"),
			*FFrom.ToString(), *FOutput.ToString(), *FTo.ToString(), *FInput.ToString()));
		return sol::make_object(Lua, true);
	});

	// ---- dataflow_set_frozen(asset_path, node_name, is_frozen) ----
	Lua.set_function("dataflow_set_frozen", [&Session](const std::string& AssetPath,
		const std::string& NodeName, bool bFrozen, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDataflow* Dataflow = LoadDataflowAsset(AssetPath, Session);
		if (!Dataflow) return sol::lua_nil;

		TSharedPtr<UE::Dataflow::FGraph> Graph = Dataflow->GetDataflow();
		if (!Graph)
		{
			Session.Log(TEXT("[FAIL] dataflow_set_frozen -> graph data is null"));
			return sol::lua_nil;
		}

		FName FNodeName = FName(UTF8_TO_TCHAR(NodeName.c_str()));
		TSharedPtr<FDataflowNode> Node = Graph->FindBaseNode(FNodeName);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] dataflow_set_frozen -> node '%s' not found"), *FNodeName.ToString()));
			return sol::lua_nil;
		}

		// bIsFrozen is a private UPROPERTY — use reflection to set it
		const FBoolProperty* FrozenProp = CastField<FBoolProperty>(
			FDataflowNode::StaticStruct()->FindPropertyByName(TEXT("bIsFrozen")));
		if (!FrozenProp)
		{
			Session.Log(TEXT("[FAIL] dataflow_set_frozen -> bIsFrozen property not found"));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Dataflow Set Frozen")));
		Dataflow->Modify();
		FrozenProp->SetPropertyValue_InContainer(Node.Get(), bFrozen);
		Node->Invalidate();
		Dataflow->MarkPackageDirty();

		Session.Log(FString::Printf(TEXT("[OK] dataflow_set_frozen -> '%s' frozen=%s"),
			*FNodeName.ToString(), bFrozen ? TEXT("true") : TEXT("false")));
		return sol::make_object(Lua, true);
	});

	// ---- dataflow_set_active(asset_path, node_name, is_active) ----
	Lua.set_function("dataflow_set_active", [&Session](const std::string& AssetPath,
		const std::string& NodeName, bool bActive, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDataflow* Dataflow = LoadDataflowAsset(AssetPath, Session);
		if (!Dataflow) return sol::lua_nil;

		TSharedPtr<UE::Dataflow::FGraph> Graph = Dataflow->GetDataflow();
		if (!Graph)
		{
			Session.Log(TEXT("[FAIL] dataflow_set_active -> graph data is null"));
			return sol::lua_nil;
		}

		FName FNodeName = FName(UTF8_TO_TCHAR(NodeName.c_str()));
		TSharedPtr<FDataflowNode> Node = Graph->FindBaseNode(FNodeName);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] dataflow_set_active -> node '%s' not found"), *FNodeName.ToString()));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Dataflow Set Active")));
		Dataflow->Modify();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
		// bActive is a private UPROPERTY in 5.6+ — use reflection to set it
		const FBoolProperty* ActiveProp = CastField<FBoolProperty>(
			FDataflowNode::StaticStruct()->FindPropertyByName(FDataflowNode::GetActivePropertyName()));
		if (!ActiveProp)
		{
			Session.Log(TEXT("[FAIL] dataflow_set_active -> bActive property not found"));
			return sol::lua_nil;
		}
		ActiveProp->SetPropertyValue_InContainer(Node.Get(), bActive);
#else
		// bActive is public in 5.5
		Node->bActive = bActive;
#endif
		Node->Invalidate();
		Dataflow->MarkPackageDirty();

		Session.Log(FString::Printf(TEXT("[OK] dataflow_set_active -> '%s' active=%s"),
			*FNodeName.ToString(), bActive ? TEXT("true") : TEXT("false")));
		return sol::make_object(Lua, true);
	});

	// ---- dataflow_rename_node(asset_path, node_name, new_name) ----
	Lua.set_function("dataflow_rename_node", [&Session](const std::string& AssetPath,
		const std::string& NodeName, const std::string& NewName, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDataflow* Dataflow = LoadDataflowAsset(AssetPath, Session);
		if (!Dataflow) return sol::lua_nil;

		TSharedPtr<UE::Dataflow::FGraph> Graph = Dataflow->GetDataflow();
		if (!Graph)
		{
			Session.Log(TEXT("[FAIL] dataflow_rename_node -> graph data is null"));
			return sol::lua_nil;
		}

		FName FNodeName = FName(UTF8_TO_TCHAR(NodeName.c_str()));
		FName FNewName = FName(UTF8_TO_TCHAR(NewName.c_str()));

		TSharedPtr<FDataflowNode> Node = Graph->FindBaseNode(FNodeName);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] dataflow_rename_node -> node '%s' not found"), *FNodeName.ToString()));
			return sol::lua_nil;
		}

		// Check that the new name doesn't conflict with an existing node
		if (Graph->FindBaseNode(FNewName))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] dataflow_rename_node -> name '%s' already in use"), *FNewName.ToString()));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Dataflow Rename Node")));
		Dataflow->Modify();
		Node->SetName(FNewName);
		Dataflow->MarkPackageDirty();

		Session.Log(FString::Printf(TEXT("[OK] dataflow_rename_node -> '%s' -> '%s'"),
			*FNodeName.ToString(), *FNewName.ToString()));
		return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*FNewName.ToString())));
	});

	// ---- dataflow_can_connect(asset_path, from_node, output_pin, to_node, input_pin) ----
	Lua.set_function("dataflow_can_connect", [&Session](const std::string& AssetPath,
		const std::string& FromNode, const std::string& OutputPin,
		const std::string& ToNode, const std::string& InputPin, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDataflow* Dataflow = LoadDataflowAsset(AssetPath, Session);
		if (!Dataflow) return sol::make_object(Lua, false);

		TSharedPtr<UE::Dataflow::FGraph> Graph = Dataflow->GetDataflow();
		if (!Graph) return sol::make_object(Lua, false);

		FName FFrom = FName(UTF8_TO_TCHAR(FromNode.c_str()));
		FName FOutput = FName(UTF8_TO_TCHAR(OutputPin.c_str()));
		FName FTo = FName(UTF8_TO_TCHAR(ToNode.c_str()));
		FName FInput = FName(UTF8_TO_TCHAR(InputPin.c_str()));

		TSharedPtr<FDataflowNode> FromNodePtr = Graph->FindBaseNode(FFrom);
		TSharedPtr<FDataflowNode> ToNodePtr = Graph->FindBaseNode(FTo);
		if (!FromNodePtr || !ToNodePtr) return sol::make_object(Lua, false);

		FDataflowOutput* Output = FromNodePtr->FindOutput(FOutput);
		FDataflowInput* Input = ToNodePtr->FindInput(FInput);
		if (!Output || !Input) return sol::make_object(Lua, false);

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
		bool bCanConnect = Graph->CanConnect(*Output, *Input);
		return sol::make_object(Lua, bCanConnect);
#else
		return sol::make_object(Lua, true);
#endif
	});

	// ---- dataflow_regenerate(asset_path, regenerate_dependents?) ----
	Lua.set_function("dataflow_regenerate", [&Session](const std::string& AssetPath,
		sol::optional<bool> RegenDependentsOpt, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FPath = UTF8_TO_TCHAR(AssetPath.c_str());
		if (!FPath.StartsWith(TEXT("/")))
			FPath = TEXT("/Game/") + FPath;

		bool bRegenerateDependents = RegenDependentsOpt.value_or(false);

		// First try loading as UDataflow directly
		UObject* Loaded = StaticLoadObject(UObject::StaticClass(), nullptr, *FPath);
		if (!Loaded)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] dataflow_regenerate -> asset not found: %s"), *FPath));
			return sol::lua_nil;
		}

		// If this is a UDataflow, evaluate all terminal nodes
		UDataflow* Dataflow = Cast<UDataflow>(Loaded);
		if (Dataflow)
		{
			TSharedPtr<UE::Dataflow::FGraph> Graph = Dataflow->GetDataflow();
			if (!Graph)
			{
				Session.Log(TEXT("[FAIL] dataflow_regenerate -> graph data is null"));
				return sol::lua_nil;
			}

			// Evaluate all terminal nodes via Blueprint library (non-deprecated path)
			int32 TerminalCount = 0;
			const TArray<TSharedPtr<FDataflowNode>>& TerminalNodes = Graph->GetFilteredNodes(FDataflowTerminalNode::StaticType());
			for (const TSharedPtr<FDataflowNode>& Node : TerminalNodes)
			{
				if (Node)
				{
					UDataflowBlueprintLibrary::EvaluateTerminalNodeByName(Dataflow, Node->GetName(), Dataflow);
					TerminalCount++;
				}
			}

			Dataflow->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] dataflow_regenerate -> evaluated %d terminal node(s) on '%s'"),
				TerminalCount, *Dataflow->GetName()));
			return sol::make_object(Lua, true);
		}

		// Otherwise try RegenerateAssetFromDataflow (works for ChaosClothAsset, etc.
		// that implement IDataflowInstanceInterface) — available in 5.6+
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
		bool bRegenerated = UDataflowBlueprintLibrary::RegenerateAssetFromDataflow(Loaded, bRegenerateDependents);
		if (bRegenerated)
		{
			Loaded->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] dataflow_regenerate -> regenerated '%s' (dependents=%s)"),
				*Loaded->GetName(), bRegenerateDependents ? TEXT("true") : TEXT("false")));
			return sol::make_object(Lua, true);
		}

		Session.Log(FString::Printf(TEXT("[FAIL] dataflow_regenerate -> '%s' does not support Dataflow regeneration (not a UDataflow or IDataflowInstanceInterface)"),
			*Loaded->GetName()));
#else
		Session.Log(FString::Printf(TEXT("[FAIL] dataflow_regenerate -> RegenerateAssetFromDataflow requires UE 5.6+. Asset '%s' is not a UDataflow."),
			*Loaded->GetName()));
#endif
		return sol::lua_nil;
	});

	// ---- dataflow_evaluate_terminal(asset_path, terminal_node_name) ----
	Lua.set_function("dataflow_evaluate_terminal", [&Session](const std::string& AssetPath,
		const std::string& TerminalName, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDataflow* Dataflow = LoadDataflowAsset(AssetPath, Session);
		if (!Dataflow) return sol::lua_nil;

		FName FTerminalName = FName(UTF8_TO_TCHAR(TerminalName.c_str()));

		TSharedPtr<UE::Dataflow::FGraph> Graph = Dataflow->GetDataflow();
		if (!Graph)
		{
			Session.Log(TEXT("[FAIL] dataflow_evaluate_terminal -> graph data is null"));
			return sol::lua_nil;
		}

		// Verify the terminal node exists
		TSharedPtr<FDataflowNode> Node = Graph->FindBaseNode(FTerminalName);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] dataflow_evaluate_terminal -> node '%s' not found"), *FTerminalName.ToString()));
			return sol::lua_nil;
		}

		const FDataflowTerminalNode* TerminalNode = Node->AsType<FDataflowTerminalNode>();
		if (!TerminalNode)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] dataflow_evaluate_terminal -> '%s' is not a terminal node"), *FTerminalName.ToString()));
			return sol::lua_nil;
		}

		UDataflowBlueprintLibrary::EvaluateTerminalNodeByName(Dataflow, FTerminalName, Dataflow);
		Dataflow->MarkPackageDirty();

		Session.Log(FString::Printf(TEXT("[OK] dataflow_evaluate_terminal -> evaluated '%s'"), *FTerminalName.ToString()));
		return sol::make_object(Lua, true);
	});
}

REGISTER_LUA_BINDING(Dataflow, DataflowDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindDataflow(Lua, Session);
});

#else
// Dataflow binding not supported on UE 5.4 — API was completely rewritten in 5.5

static TArray<FLuaFunctionDoc> DataflowDocs = {};

REGISTER_LUA_BINDING(Dataflow, DataflowDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	// No-op: Dataflow support requires UE 5.5+
});
#endif // ENGINE_MINOR_VERSION >= 5
