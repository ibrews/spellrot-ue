// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "PCGComponent.h"
#include "PCGEdge.h"

#include "UObject/UnrealType.h"
#include "UObject/PropertyAccessUtil.h"
#include "Editor.h"
#include "EngineUtils.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

namespace
{

const char* PropertyBagTypeToString(EPropertyBagPropertyType Type)
{
	switch (Type)
	{
	case EPropertyBagPropertyType::Bool:       return "Bool";
	case EPropertyBagPropertyType::Byte:       return "Byte";
	case EPropertyBagPropertyType::Int32:      return "Int32";
	case EPropertyBagPropertyType::Int64:      return "Int64";
	case EPropertyBagPropertyType::Float:      return "Float";
	case EPropertyBagPropertyType::Double:     return "Double";
	case EPropertyBagPropertyType::Name:       return "Name";
	case EPropertyBagPropertyType::String:     return "String";
	case EPropertyBagPropertyType::Text:       return "Text";
	case EPropertyBagPropertyType::Enum:       return "Enum";
	case EPropertyBagPropertyType::Struct:     return "Struct";
	case EPropertyBagPropertyType::Object:     return "Object";
	case EPropertyBagPropertyType::SoftObject: return "SoftObject";
	case EPropertyBagPropertyType::Class:      return "Class";
	case EPropertyBagPropertyType::SoftClass:  return "SoftClass";
	case EPropertyBagPropertyType::UInt32:     return "UInt32";
	case EPropertyBagPropertyType::UInt64:     return "UInt64";
	default: return "Unknown";
	}
}

// Read a parameter value from the property bag as a string for display
FString GetParameterValueAsString(const FInstancedPropertyBag& Bag, const FPropertyBagPropertyDesc& Desc)
{
	switch (Desc.ValueType)
	{
	case EPropertyBagPropertyType::Bool:
	{
		auto Val = Bag.GetValueBool(Desc.Name);
		return Val.IsValid() ? (Val.GetValue() ? TEXT("true") : TEXT("false")) : TEXT("?");
	}
	case EPropertyBagPropertyType::Int32:
	{
		auto Val = Bag.GetValueInt32(Desc.Name);
		return Val.IsValid() ? FString::FromInt(Val.GetValue()) : TEXT("?");
	}
	case EPropertyBagPropertyType::Float:
	{
		auto Val = Bag.GetValueFloat(Desc.Name);
		return Val.IsValid() ? FString::SanitizeFloat(Val.GetValue()) : TEXT("?");
	}
	case EPropertyBagPropertyType::Double:
	{
		auto Val = Bag.GetValueDouble(Desc.Name);
		return Val.IsValid() ? FString::SanitizeFloat(Val.GetValue()) : TEXT("?");
	}
	case EPropertyBagPropertyType::Name:
	{
		auto Val = Bag.GetValueName(Desc.Name);
		return Val.IsValid() ? Val.GetValue().ToString() : TEXT("?");
	}
	case EPropertyBagPropertyType::String:
	{
		auto Val = Bag.GetValueString(Desc.Name);
		return Val.IsValid() ? Val.GetValue() : TEXT("?");
	}
	default:
	{
		// For complex types, use ExportText via the property bag's underlying struct
		const UPropertyBag* BagStruct = Bag.GetPropertyBagStruct();
		if (!BagStruct) return TEXT("?");
		const FProperty* Prop = BagStruct->FindPropertyByName(Desc.Name);
		if (!Prop) return TEXT("?");
		const uint8* Memory = Bag.GetValue().GetMemory();
		if (!Memory) return TEXT("?");
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Memory);
		if (!ValuePtr) return TEXT("?");
		FString ValueStr;
		Prop->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, nullptr, PPF_None);
		if (ValueStr.Len() > 120) ValueStr = ValueStr.Left(117) + TEXT("...");
		return ValueStr;
	}
	}
}

// Set a PCG graph parameter from a Lua value, dispatching by type
bool SetParameterFromLuaValue(UPCGGraphInterface* GraphInterface, const FName& ParamName,
	EPropertyBagPropertyType ParamType, const sol::object& Value)
{
	FInstancedPropertyBag* Bag = GraphInterface->GetMutableUserParametersStruct_Unsafe();
	if (!Bag) return false;

	EPropertyBagResult Result = EPropertyBagResult::PropertyNotFound;

	switch (ParamType)
	{
	case EPropertyBagPropertyType::Bool:
		if (Value.is<bool>())
			Result = Bag->SetValueBool(ParamName, Value.as<bool>());
		break;
	case EPropertyBagPropertyType::Int32:
		if (Value.is<int>())
			Result = Bag->SetValueInt32(ParamName, Value.as<int>());
		else if (Value.is<double>())
			Result = Bag->SetValueInt32(ParamName, static_cast<int32>(Value.as<double>()));
		break;
	case EPropertyBagPropertyType::Int64:
		if (Value.is<int>())
			Result = Bag->SetValueInt64(ParamName, static_cast<int64>(Value.as<int>()));
		else if (Value.is<double>())
			Result = Bag->SetValueInt64(ParamName, static_cast<int64>(Value.as<double>()));
		break;
	case EPropertyBagPropertyType::Float:
		if (Value.is<double>())
			Result = Bag->SetValueFloat(ParamName, static_cast<float>(Value.as<double>()));
		else if (Value.is<int>())
			Result = Bag->SetValueFloat(ParamName, static_cast<float>(Value.as<int>()));
		break;
	case EPropertyBagPropertyType::Double:
		if (Value.is<double>())
			Result = Bag->SetValueDouble(ParamName, Value.as<double>());
		else if (Value.is<int>())
			Result = Bag->SetValueDouble(ParamName, static_cast<double>(Value.as<int>()));
		break;
	case EPropertyBagPropertyType::Name:
		if (Value.is<std::string>())
			Result = Bag->SetValueName(ParamName, FName(UTF8_TO_TCHAR(Value.as<std::string>().c_str())));
		break;
	case EPropertyBagPropertyType::String:
		if (Value.is<std::string>())
			Result = Bag->SetValueString(ParamName, UTF8_TO_TCHAR(Value.as<std::string>().c_str()));
		break;
	case EPropertyBagPropertyType::SoftObject:
		if (Value.is<std::string>())
			Result = Bag->SetValueSoftPath(ParamName, FSoftObjectPath(UTF8_TO_TCHAR(Value.as<std::string>().c_str())));
		break;
	case EPropertyBagPropertyType::Object:
		if (Value.is<std::string>())
		{
			FString Path = UTF8_TO_TCHAR(Value.as<std::string>().c_str());
			UObject* Obj = LoadObject<UObject>(nullptr, *Path);
			if (Obj) Result = Bag->SetValueObject(ParamName, Obj);
		}
		break;
	default:
		// For other types, try ImportText via the property bag's underlying struct
		if (Value.is<std::string>())
		{
			const UPropertyBag* BagStruct = Bag->GetPropertyBagStruct();
			if (BagStruct)
			{
				const FProperty* Prop = BagStruct->FindPropertyByName(ParamName);
				if (Prop)
				{
					void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Bag->GetMutableValue().GetMemory());
					if (ValuePtr)
					{
						FString ValueStr = UTF8_TO_TCHAR(Value.as<std::string>().c_str());
						Prop->ImportText_Direct(*ValueStr, ValuePtr, nullptr, PPF_None);
						Result = EPropertyBagResult::Success;
					}
				}
			}
		}
		break;
	}

	if (Result == EPropertyBagResult::Success)
	{
		GraphInterface->OnGraphParametersChanged(EPCGGraphParameterEvent::ValueModifiedLocally, ParamName);
	}

	return Result == EPropertyBagResult::Success;
}

// Set a property by name on a UObject via reflection (same pattern as EQS)
bool SetPropertyByReflection(UObject* Obj, const FString& PropName, const FString& Value)
{
	if (!Obj) return false;

	FProperty* Prop = nullptr;
	for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
	{
		if (It->GetName().Equals(PropName, ESearchCase::IgnoreCase))
		{
			Prop = *It;
			break;
		}
	}
	if (!Prop) return false;

	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Obj);
	if (!ValuePtr) return false;
	Prop->ImportText_Direct(*Value, ValuePtr, Obj, PPF_None);

#if WITH_EDITOR
	FPropertyChangedEvent ChangedEvent(Prop);
	Obj->PostEditChangeProperty(ChangedEvent);
#endif

	return true;
}

// Get editable properties on a UPCGSettings subclass (skipping base class props)
sol::table GetSettingsProperties(UPCGSettings* Settings, sol::state_view& Lua)
{
	sol::table Props = Lua.create_table();
	if (!Settings) return Props;

	int32 Idx = 1;
	for (TFieldIterator<FProperty> PropIt(Settings->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Property = *PropIt;
		if (!Property->HasAnyPropertyFlags(CPF_Edit)) continue;
		if (Property->HasAnyPropertyFlags(CPF_Deprecated)) continue;
		if (Property->GetOwnerClass() == UPCGSettings::StaticClass()) continue;
		if (Property->IsA<FDelegateProperty>() || Property->IsA<FMulticastDelegateProperty>()) continue;

		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Settings);
		if (!ValuePtr) continue;

		sol::table PropEntry = Lua.create_table();
		PropEntry["name"] = TCHAR_TO_UTF8(*Property->GetName());

		// Enum
		if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
		{
			if (UEnum* Enum = EnumProp->GetEnum())
			{
				FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
				int64 EnumValue = UnderlyingProp->GetSignedIntPropertyValue(
					EnumProp->ContainerPtrToValuePtr<void>(Settings));
				PropEntry["type"] = "Enum";
				PropEntry["value"] = TCHAR_TO_UTF8(*Enum->GetNameStringByValue(EnumValue));
				Props[Idx++] = PropEntry;
				continue;
			}
		}

		// TEnumAsByte
		if (FByteProperty* ByteProp = CastField<FByteProperty>(Property))
		{
			if (UEnum* Enum = ByteProp->GetIntPropertyEnum())
			{
				uint8 ByteValue = ByteProp->GetPropertyValue_InContainer(Settings);
				PropEntry["type"] = "Enum";
				PropEntry["value"] = TCHAR_TO_UTF8(*Enum->GetNameStringByValue(ByteValue));
				Props[Idx++] = PropEntry;
				continue;
			}
		}

		// Generic export
		FString ValueStr;
		Property->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, nullptr, PPF_None);
		if (ValueStr.Len() > 120) ValueStr = ValueStr.Left(117) + TEXT("...");

		PropEntry["type"] = TCHAR_TO_UTF8(*Property->GetCPPType());
		PropEntry["value"] = TCHAR_TO_UTF8(*ValueStr);
		Props[Idx++] = PropEntry;
	}
	return Props;
}

} // anonymous namespace

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> PCGDocs = {};

static void BindPCG(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_pcg", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UPCGGraph* PCGGraph = LoadObject<UPCGGraph>(nullptr, *FPath);
		if (!PCGGraph) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"PCG Graph enrichment.\n"
			"Graph editing (add_node/connect/set_pin/read_graph/find_nodes/delete_node) works via existing graph tools.\n"
			"\n"
			"Element types for list/configure:\n"
			"  parameter     — graph user parameter (density, seed, mesh refs, etc.)\n"
			"  node          — PCG runtime node with its settings\n"
			"  node_type     — available PCG settings classes\n"
			"  edge          — connections between nodes\n"
			"\n"
			"list(type):\n"
			"  list(\"parameters\")                         — all graph user parameters with values\n"
			"  list(\"nodes\")                               — all runtime nodes with settings type\n"
			"  list(\"node_types\", {query=\"sampler\"})       — search available PCG settings classes\n"
			"  list(\"edges\")                               — all connections between nodes\n"
			"\n"
			"configure(type, params):\n"
			"  configure(\"parameter\", {name=\"Density\", value=5.0})   — set a graph parameter\n"
			"  configure(\"node\", {index=0, PointsPerSquaredMeter=100}) — set node settings properties\n"
			"  configure(\"node\", {name=\"Surface Sampler\", Looseness=1.0}) — find node by name\n"
			"  configure(\"node\", {index=0, title=\"My Sampler\"})       — rename a node\n"
			"  configure(\"node\", {index=0, enabled=false})            — enable/disable a node\n"
			"  configure(\"node\", {index=0, x=100, y=200})             — set node position\n"
			"  configure(\"graph\", {seed=42})                          — set graph-level properties\n"
			"\n"
			"add(type, params):\n"
			"  add(\"parameter\", {name=\"Density\", type=\"Float\", value=1.0}) — add graph parameter\n"
			"\n"
			"remove(type, params):\n"
			"  remove(\"parameter\", {name=\"Density\"})     — remove graph parameter\n"
			"\n"
			"Action methods:\n"
			"  info()       — summary of graph parameters, nodes, and settings\n"
			"  generate()   — trigger PCG generation on all components using this graph\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [PCGGraph, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();

			Result["name"] = TCHAR_TO_UTF8(*PCGGraph->GetName());
			Result["path"] = TCHAR_TO_UTF8(*PCGGraph->GetPathName());

			// Graph settings
			Result["hierarchical_generation"] = PCGGraph->IsHierarchicalGenerationEnabled();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			Result["use_2d_grid"] = PCGGraph->Use2DGrid();
#endif
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			Result["has_default_constructed_inputs"] = PCGGraph->HasDefaultConstructedInputs();
#endif

#if WITH_EDITORONLY_DATA
			if (!PCGGraph->Category.IsEmpty())
				Result["category"] = TCHAR_TO_UTF8(*PCGGraph->Category.ToString());
			if (!PCGGraph->Description.IsEmpty())
				Result["description"] = TCHAR_TO_UTF8(*PCGGraph->Description.ToString());
#endif

			// Parameters
			const FInstancedPropertyBag* UserParams = PCGGraph->GetUserParametersStruct();
			int32 ParamCount = UserParams ? UserParams->GetNumPropertiesInBag() : 0;
			Result["parameter_count"] = ParamCount;

			if (UserParams && ParamCount > 0)
			{
				sol::table ParamsTable = Lua.create_table();
				const UPropertyBag* BagStruct = UserParams->GetPropertyBagStruct();
				if (BagStruct)
				{
					TConstArrayView<FPropertyBagPropertyDesc> Descs = BagStruct->GetPropertyDescs();
					for (int32 i = 0; i < Descs.Num(); i++)
					{
						const FPropertyBagPropertyDesc& Desc = Descs[i];
						sol::table Entry = Lua.create_table();
						Entry["name"] = TCHAR_TO_UTF8(*Desc.Name.ToString());
						Entry["type"] = PropertyBagTypeToString(Desc.ValueType);
						Entry["value"] = TCHAR_TO_UTF8(*GetParameterValueAsString(*UserParams, Desc));
						ParamsTable[i + 1] = Entry;
					}
				}
				Result["parameters"] = ParamsTable;
			}

			// Nodes
			const TArray<UPCGNode*>& Nodes = PCGGraph->GetNodes();
			Result["node_count"] = static_cast<int>(Nodes.Num());

			sol::table NodesTable = Lua.create_table();
			for (int32 i = 0; i < Nodes.Num(); i++)
			{
				const UPCGNode* Node = Nodes[i];
				if (!Node) continue;

				sol::table Entry = Lua.create_table();
				Entry["index"] = i;
				Entry["title"] = TCHAR_TO_UTF8(*Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());

				UPCGSettings* Settings = Node->GetSettings();
				if (Settings)
				{
					Entry["settings_class"] = TCHAR_TO_UTF8(*Settings->GetClass()->GetName());
					Entry["enabled"] = Settings->bEnabled;
					Entry["debug"] = Settings->bDebug;
				}

				if (Node->HasAuthoredTitle())
				{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
					Entry["authored_title"] = TCHAR_TO_UTF8(*Node->GetAuthoredTitleName().ToString());
#else
					Entry["authored_title"] = TCHAR_TO_UTF8(*Node->NodeTitle.ToString());
#endif
				}

#if WITH_EDITOR
				int32 PosX = 0, PosY = 0;
				Node->GetNodePosition(PosX, PosY);
				Entry["x"] = PosX;
				Entry["y"] = PosY;
#endif

#if WITH_EDITORONLY_DATA
				if (!Node->NodeComment.IsEmpty())
					Entry["comment"] = TCHAR_TO_UTF8(*Node->NodeComment);
#endif

				// Pins
				const TArray<TObjectPtr<UPCGPin>>& InputPins = Node->GetInputPins();
				const TArray<TObjectPtr<UPCGPin>>& OutputPins = Node->GetOutputPins();
				Entry["input_pin_count"] = static_cast<int>(InputPins.Num());
				Entry["output_pin_count"] = static_cast<int>(OutputPins.Num());

				NodesTable[i + 1] = Entry;
			}
			Result["nodes"] = NodesTable;

			Session.Log(FString::Printf(TEXT("[OK] info() -> %s, %d params, %d nodes"),
				*PCGGraph->GetName(), ParamCount, Nodes.Num()));
			return Result;
		});

		// ================================================================
		// list(type, params?)
		// ================================================================
		AssetObj.set_function("list", [PCGGraph, &Session](sol::table Self,
			sol::optional<std::string> TypeOpt, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = Self["info"];
				if (InfoFn.valid()) return InfoFn(Self);
				return sol::lua_nil;
			}

			// ---- list("parameters") ----
			if (FType.Equals(TEXT("parameters"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("parameter"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("params"), ESearchCase::IgnoreCase))
			{
				const FInstancedPropertyBag* UserParams = PCGGraph->GetUserParametersStruct();
				if (!UserParams || UserParams->GetNumPropertiesInBag() == 0)
				{
					Session.Log(TEXT("[OK] list(\"parameters\") -> 0 parameters"));
					return sol::make_object(Lua, Lua.create_table());
				}

				sol::table Result = Lua.create_table();
				const UPropertyBag* BagStruct = UserParams->GetPropertyBagStruct();
				if (BagStruct)
				{
					TConstArrayView<FPropertyBagPropertyDesc> Descs = BagStruct->GetPropertyDescs();
					for (int32 i = 0; i < Descs.Num(); i++)
					{
						const FPropertyBagPropertyDesc& Desc = Descs[i];
						sol::table Entry = Lua.create_table();
						Entry["name"] = TCHAR_TO_UTF8(*Desc.Name.ToString());
						Entry["type"] = PropertyBagTypeToString(Desc.ValueType);
						Entry["value"] = TCHAR_TO_UTF8(*GetParameterValueAsString(*UserParams, Desc));

						if (Desc.ValueTypeObject)
						{
							Entry["value_type_object"] = TCHAR_TO_UTF8(*Desc.ValueTypeObject->GetPathName());
						}

						Result[i + 1] = Entry;
					}
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"parameters\") -> %d"), UserParams->GetNumPropertiesInBag()));
				return Result;
			}

			// ---- list("nodes") ----
			if (FType.Equals(TEXT("nodes"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("node"), ESearchCase::IgnoreCase))
			{
				const TArray<UPCGNode*>& Nodes = PCGGraph->GetNodes();
				sol::table Result = Lua.create_table();

				for (int32 i = 0; i < Nodes.Num(); i++)
				{
					const UPCGNode* Node = Nodes[i];
					if (!Node) continue;

					sol::table Entry = Lua.create_table();
					Entry["index"] = i;
					Entry["title"] = TCHAR_TO_UTF8(*Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());

					UPCGSettings* Settings = Node->GetSettings();
					if (Settings)
					{
						Entry["settings_class"] = TCHAR_TO_UTF8(*Settings->GetClass()->GetName());
						Entry["enabled"] = Settings->bEnabled;
						Entry["debug"] = Settings->bDebug;
						Entry["properties"] = GetSettingsProperties(Settings, Lua);
					}

#if WITH_EDITOR
					int32 PosX = 0, PosY = 0;
					Node->GetNodePosition(PosX, PosY);
					Entry["x"] = PosX;
					Entry["y"] = PosY;
#endif

#if WITH_EDITORONLY_DATA
					if (!Node->NodeComment.IsEmpty())
						Entry["comment"] = TCHAR_TO_UTF8(*Node->NodeComment);
#endif

					// Input pins
					sol::table InPins = Lua.create_table();
					const TArray<TObjectPtr<UPCGPin>>& InputPins = Node->GetInputPins();
					for (int32 p = 0; p < InputPins.Num(); p++)
					{
						if (!InputPins[p]) continue;
						sol::table PinEntry = Lua.create_table();
						PinEntry["label"] = TCHAR_TO_UTF8(*InputPins[p]->Properties.Label.ToString());
						PinEntry["connected"] = Node->IsInputPinConnected(InputPins[p]->Properties.Label);
						InPins[p + 1] = PinEntry;
					}
					Entry["input_pins"] = InPins;

					// Output pins
					sol::table OutPins = Lua.create_table();
					const TArray<TObjectPtr<UPCGPin>>& OutputPins = Node->GetOutputPins();
					for (int32 p = 0; p < OutputPins.Num(); p++)
					{
						if (!OutputPins[p]) continue;
						sol::table PinEntry = Lua.create_table();
						PinEntry["label"] = TCHAR_TO_UTF8(*OutputPins[p]->Properties.Label.ToString());
						PinEntry["connected"] = Node->IsOutputPinConnected(OutputPins[p]->Properties.Label);
						OutPins[p + 1] = PinEntry;
					}
					Entry["output_pins"] = OutPins;

					Result[i + 1] = Entry;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"nodes\") -> %d"), Nodes.Num()));
				return Result;
			}

			// ---- list("node_types", {query="sampler"}) ----
			if (FType.Equals(TEXT("node_types"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("node_type"), ESearchCase::IgnoreCase))
			{
				FString Query;
				if (Params.has_value())
				{
					std::string QueryStr = Params.value().get_or<std::string>("query", "");
					Query = UTF8_TO_TCHAR(QueryStr.c_str());
				}

				TArray<UClass*> DerivedClasses;
				GetDerivedClasses(UPCGSettings::StaticClass(), DerivedClasses);

				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (UClass* Cls : DerivedClasses)
				{
					if (Cls->HasAnyClassFlags(CLASS_Abstract)) continue;

					FString ClassName = Cls->GetName();
					FString DisplayName = ClassName;
					DisplayName.RemoveFromStart(TEXT("PCG"));
					DisplayName.RemoveFromEnd(TEXT("Settings"));

					// Apply query filter
					if (!Query.IsEmpty())
					{
						if (!ClassName.Contains(Query, ESearchCase::IgnoreCase) &&
							!DisplayName.Contains(Query, ESearchCase::IgnoreCase))
						{
							continue;
						}
					}

					sol::table Entry = Lua.create_table();
					Entry["class_name"] = TCHAR_TO_UTF8(*ClassName);
					Entry["display_name"] = TCHAR_TO_UTF8(*DisplayName);

#if WITH_EDITOR
					FString Tooltip = Cls->GetMetaData(TEXT("ToolTip"));
					if (!Tooltip.IsEmpty())
					{
						if (Tooltip.Len() > 150) Tooltip = Tooltip.Left(147) + TEXT("...");
						Entry["description"] = TCHAR_TO_UTF8(*Tooltip);
					}
#endif

					Result[Idx++] = Entry;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"node_types\") -> %d"), Idx - 1));
				return Result;
			}

			// ---- list("edges") ----
			if (FType.Equals(TEXT("edges"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("edge"), ESearchCase::IgnoreCase))
			{
				const TArray<UPCGNode*>& Nodes = PCGGraph->GetNodes();
				sol::table Result = Lua.create_table();
				int32 Idx = 1;

				// Also include input/output nodes
				TArray<UPCGNode*> AllNodes;
				if (PCGGraph->GetInputNode()) AllNodes.Add(PCGGraph->GetInputNode());
				if (PCGGraph->GetOutputNode()) AllNodes.Add(PCGGraph->GetOutputNode());
				AllNodes.Append(Nodes);

				for (UPCGNode* Node : AllNodes)
				{
					if (!Node) continue;

					for (const TObjectPtr<UPCGPin>& OutPin : Node->GetOutputPins())
					{
						if (!OutPin) continue;

						for (const TObjectPtr<UPCGEdge>& Edge : OutPin->Edges)
						{
							if (!Edge || !Edge->IsValid()) continue;
							UPCGPin* TargetPin = Edge->GetOtherPin(OutPin);
							if (!TargetPin || !TargetPin->Node) continue;

							sol::table Entry = Lua.create_table();
							Entry["from_node"] = TCHAR_TO_UTF8(*Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());
							Entry["from_pin"] = TCHAR_TO_UTF8(*OutPin->Properties.Label.ToString());
							Entry["to_node"] = TCHAR_TO_UTF8(*TargetPin->Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());
							Entry["to_pin"] = TCHAR_TO_UTF8(*TargetPin->Properties.Label.ToString());
							Result[Idx++] = Entry;
						}
					}
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"edges\") -> %d"), Idx - 1));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: parameters, nodes, node_types, edges"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// configure(type, params)
		// ================================================================
		AssetObj.set_function("configure", [PCGGraph, &Session](sol::table /*self*/,
			const std::string& Type, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- configure("parameter", {name="Density", value=5.0}) ----
			if (FType.Equals(TEXT("parameter"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("param"), ESearchCase::IgnoreCase))
			{
				std::string NameStr = Params.get_or<std::string>("name", "");
				if (NameStr.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"parameter\") -> 'name' required"));
					return sol::lua_nil;
				}

				FName ParamName = FName(UTF8_TO_TCHAR(NameStr.c_str()));

				sol::object ValueObj = Params["value"];
				if (!ValueObj.valid() || ValueObj.is<sol::lua_nil_t>())
				{
					Session.Log(TEXT("[FAIL] configure(\"parameter\") -> 'value' required"));
					return sol::lua_nil;
				}

				// Find the parameter type
				const FInstancedPropertyBag* UserParams = PCGGraph->GetUserParametersStruct();
				if (!UserParams)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"parameter\") -> no parameters on graph")));
					return sol::lua_nil;
				}

				const UPropertyBag* BagStruct = UserParams->GetPropertyBagStruct();
				if (!BagStruct)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"parameter\") -> parameter bag struct is null")));
					return sol::lua_nil;
				}

				// Find the desc by name
				EPropertyBagPropertyType ParamType = EPropertyBagPropertyType::None;
				TConstArrayView<FPropertyBagPropertyDesc> Descs = BagStruct->GetPropertyDescs();
				for (const FPropertyBagPropertyDesc& Desc : Descs)
				{
					if (Desc.Name == ParamName)
					{
						ParamType = Desc.ValueType;
						break;
					}
				}

				if (ParamType == EPropertyBagPropertyType::None)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"parameter\") -> parameter '%s' not found. Use list(\"parameters\") to see available."),
						*ParamName.ToString()));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("PCG: Set Parameter")));
				PCGGraph->Modify();

				if (SetParameterFromLuaValue(PCGGraph, ParamName, ParamType, ValueObj))
				{
					PCGGraph->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] configure(\"parameter\", name=\"%s\")"), *ParamName.ToString()));
					return sol::make_object(Lua, true);
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"parameter\") -> could not set '%s'. Type mismatch?"), *ParamName.ToString()));
					return sol::lua_nil;
				}
			}

			// ---- configure("node", {index=N, ...}) or configure("node", {name="...", ...}) ----
			if (FType.Equals(TEXT("node"), ESearchCase::IgnoreCase))
			{
				const TArray<UPCGNode*>& Nodes = PCGGraph->GetNodes();
				UPCGNode* TargetNode = nullptr;
				int32 FoundIndex = -1;

				// Find by index
				sol::optional<int> NodeIdx = Params.get<sol::optional<int>>("index");
				if (NodeIdx.has_value())
				{
					int32 Idx = NodeIdx.value();
					if (Idx >= 0 && Idx < Nodes.Num())
					{
						TargetNode = Nodes[Idx];
						FoundIndex = Idx;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"node\") -> index %d out of range (0-%d)"), Idx, Nodes.Num() - 1));
						return sol::lua_nil;
					}
				}
				else
				{
					// Find by name/title
					std::string NameStr = Params.get_or<std::string>("name", "");
					if (NameStr.empty())
					{
						Session.Log(TEXT("[FAIL] configure(\"node\") -> 'index' or 'name' required"));
						return sol::lua_nil;
					}

					FString SearchName = UTF8_TO_TCHAR(NameStr.c_str());
					for (int32 i = 0; i < Nodes.Num(); i++)
					{
						if (!Nodes[i]) continue;
						FString Title = Nodes[i]->GetNodeTitle(EPCGNodeTitleType::ListView).ToString();
						if (Title.Contains(SearchName, ESearchCase::IgnoreCase))
						{
							TargetNode = Nodes[i];
							FoundIndex = i;
							break;
						}
					}

					if (!TargetNode)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"node\") -> no node matching '%s'. Use list(\"nodes\") to see available."), *SearchName));
						return sol::lua_nil;
					}
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("PCG: Configure Node")));
				PCGGraph->Modify();
				TargetNode->Modify();

				// Keys handled specially at the node level (not on settings)
				const TSet<FString> ReservedKeys = { TEXT("index"), TEXT("name"), TEXT("title"), TEXT("x"), TEXT("y"), TEXT("enabled"), TEXT("debug"), TEXT("comment") };
				int32 SetCount = 0;

				// --- Handle node-level special keys ---

#if WITH_EDITOR
				// Title
				sol::optional<std::string> TitleOpt = Params.get<sol::optional<std::string>>("title");
				if (TitleOpt.has_value())
				{
					FName NewTitle = FName(UTF8_TO_TCHAR(TitleOpt.value().c_str()));
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
					TargetNode->SetNodeTitle(NewTitle);
#else
					TargetNode->NodeTitle = NewTitle;
#endif
					SetCount++;
				}

				// Position
				sol::optional<int> XOpt = Params.get<sol::optional<int>>("x");
				sol::optional<int> YOpt = Params.get<sol::optional<int>>("y");
				if (XOpt.has_value() || YOpt.has_value())
				{
					int32 CurX = 0, CurY = 0;
					TargetNode->GetNodePosition(CurX, CurY);
					TargetNode->SetNodePosition(XOpt.value_or(CurX), YOpt.value_or(CurY));
					SetCount++;
				}
#endif

#if WITH_EDITORONLY_DATA
				// Comment
				sol::optional<std::string> CommentOpt = Params.get<sol::optional<std::string>>("comment");
				if (CommentOpt.has_value())
				{
					TargetNode->NodeComment = UTF8_TO_TCHAR(CommentOpt.value().c_str());
					SetCount++;
				}
#endif

				// Enabled / Debug (on settings)
				UPCGSettings* Settings = TargetNode->GetSettings();
				sol::optional<bool> EnabledOpt = Params.get<sol::optional<bool>>("enabled");
				if (EnabledOpt.has_value() && Settings)
				{
					Settings->Modify();
					Settings->bEnabled = EnabledOpt.value();
					SetCount++;
				}

				sol::optional<bool> DebugOpt = Params.get<sol::optional<bool>>("debug");
				if (DebugOpt.has_value() && Settings)
				{
					Settings->Modify();
					Settings->bDebug = DebugOpt.value();
					SetCount++;
				}

				// --- Handle settings properties via reflection ---
				if (Settings)
				{
					bool bSettingsModified = false;
					for (auto& kv : Params)
					{
						if (!kv.first.is<std::string>()) continue;
						FString Key = UTF8_TO_TCHAR(kv.first.as<std::string>().c_str());
						if (ReservedKeys.Contains(Key)) continue;

						FString ValueStr;
						if (kv.second.is<std::string>())
							ValueStr = UTF8_TO_TCHAR(kv.second.as<std::string>().c_str());
						else if (kv.second.is<double>())
							ValueStr = FString::SanitizeFloat(kv.second.as<double>());
						else if (kv.second.is<int>())
							ValueStr = FString::FromInt(kv.second.as<int>());
						else if (kv.second.is<bool>())
							ValueStr = kv.second.as<bool>() ? TEXT("true") : TEXT("false");
						else
							continue;

						if (!bSettingsModified)
						{
							Settings->Modify();
							bSettingsModified = true;
						}

						if (SetPropertyByReflection(Settings, Key, ValueStr))
						{
							SetCount++;
						}
						else
						{
							Session.Log(FString::Printf(TEXT("[WARN] configure(\"node\") -> property '%s' not found or could not be set"), *Key));
						}
					}
				}

#if WITH_EDITOR
				PCGGraph->ForceNotificationForEditor(EPCGChangeType::Settings);
#endif
				PCGGraph->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"node\", index=%d) -> %d properties set"), FoundIndex, SetCount));
				return sol::make_object(Lua, true);
			}

			// ---- configure("graph", {seed=42, use_hierarchical_generation=true, ...}) ----
			if (FType.Equals(TEXT("graph"), ESearchCase::IgnoreCase))
			{
				const FScopedTransaction Transaction(FText::FromString(TEXT("PCG: Configure Graph")));
				PCGGraph->Modify();

				int32 SetCount = 0;

				for (auto& kv : Params)
				{
					if (!kv.first.is<std::string>()) continue;
					FString Key = UTF8_TO_TCHAR(kv.first.as<std::string>().c_str());

					FString ValueStr;
					if (kv.second.is<std::string>())
						ValueStr = UTF8_TO_TCHAR(kv.second.as<std::string>().c_str());
					else if (kv.second.is<double>())
						ValueStr = FString::SanitizeFloat(kv.second.as<double>());
					else if (kv.second.is<int>())
						ValueStr = FString::FromInt(kv.second.as<int>());
					else if (kv.second.is<bool>())
						ValueStr = kv.second.as<bool>() ? TEXT("true") : TEXT("false");
					else
						continue;

					if (SetPropertyByReflection(PCGGraph, Key, ValueStr))
					{
						SetCount++;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure(\"graph\") -> property '%s' not found"), *Key));
					}
				}

#if WITH_EDITOR
				PCGGraph->ForceNotificationForEditor(EPCGChangeType::Structural);
#endif
				PCGGraph->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"graph\") -> %d properties set"), SetCount));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: parameter, node, graph"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// add("parameter", {name="Density", type="Float", value=1.0})
		// ================================================================
		AssetObj.set_function("add", [PCGGraph, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("parameter"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("param"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"parameter\") -> {name=.., type=.., value=..} required"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();

				std::string NameStr = P.get_or<std::string>("name", "");
				if (NameStr.empty())
				{
					Session.Log(TEXT("[FAIL] add(\"parameter\") -> 'name' required"));
					return sol::lua_nil;
				}

				std::string TypeStr = P.get_or<std::string>("type", "Double");
				FString FTypeStr = UTF8_TO_TCHAR(TypeStr.c_str());

				EPropertyBagPropertyType BagType = EPropertyBagPropertyType::Double;
				if (FTypeStr.Equals(TEXT("Bool"), ESearchCase::IgnoreCase)) BagType = EPropertyBagPropertyType::Bool;
				else if (FTypeStr.Equals(TEXT("Int32"), ESearchCase::IgnoreCase) || FTypeStr.Equals(TEXT("Int"), ESearchCase::IgnoreCase) || FTypeStr.Equals(TEXT("Integer"), ESearchCase::IgnoreCase)) BagType = EPropertyBagPropertyType::Int32;
				else if (FTypeStr.Equals(TEXT("Int64"), ESearchCase::IgnoreCase)) BagType = EPropertyBagPropertyType::Int64;
				else if (FTypeStr.Equals(TEXT("Float"), ESearchCase::IgnoreCase)) BagType = EPropertyBagPropertyType::Float;
				else if (FTypeStr.Equals(TEXT("Double"), ESearchCase::IgnoreCase)) BagType = EPropertyBagPropertyType::Double;
				else if (FTypeStr.Equals(TEXT("Name"), ESearchCase::IgnoreCase)) BagType = EPropertyBagPropertyType::Name;
				else if (FTypeStr.Equals(TEXT("String"), ESearchCase::IgnoreCase)) BagType = EPropertyBagPropertyType::String;
				else if (FTypeStr.Equals(TEXT("SoftObject"), ESearchCase::IgnoreCase) || FTypeStr.Equals(TEXT("SoftObjectPath"), ESearchCase::IgnoreCase)) BagType = EPropertyBagPropertyType::SoftObject;

				FName ParamName = FName(UTF8_TO_TCHAR(NameStr.c_str()));

				const FScopedTransaction Transaction(FText::FromString(TEXT("PCG: Add Parameter")));
				PCGGraph->Modify();

				TArray<FPropertyBagPropertyDesc> NewDescs;
				NewDescs.Add(FPropertyBagPropertyDesc(ParamName, BagType));

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				EPropertyBagAlterationResult AddResult = PCGGraph->AddUserParameters(NewDescs);
				if (AddResult != EPropertyBagAlterationResult::Success)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"parameter\") -> failed to add '%s' (may already exist)"), *ParamName.ToString()));
					return sol::lua_nil;
				}
#else
				PCGGraph->AddUserParameters(NewDescs);
#endif

				// Set initial value if provided
				sol::object ValueObj = P["value"];
				if (ValueObj.valid() && !ValueObj.is<sol::lua_nil_t>())
				{
					SetParameterFromLuaValue(PCGGraph, ParamName, BagType, ValueObj);
				}

				PCGGraph->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"parameter\", name=\"%s\", type=\"%s\")"), *ParamName.ToString(), *FTypeStr));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: parameter"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// remove("parameter", {name="Density"})
		// ================================================================
		AssetObj.set_function("remove", [PCGGraph, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("parameter"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("param"), ESearchCase::IgnoreCase))
			{
				FName ParamName;
				if (Id.is<sol::table>())
				{
					sol::table P = Id.as<sol::table>();
					std::string NameStr = P.get_or<std::string>("name", "");
					if (NameStr.empty())
					{
						Session.Log(TEXT("[FAIL] remove(\"parameter\") -> {name=..} required"));
						return sol::lua_nil;
					}
					ParamName = FName(UTF8_TO_TCHAR(NameStr.c_str()));
				}
				else if (Id.is<std::string>())
				{
					ParamName = FName(UTF8_TO_TCHAR(Id.as<std::string>().c_str()));
				}
				else
				{
					Session.Log(TEXT("[FAIL] remove(\"parameter\") -> {name=..} or name string required"));
					return sol::lua_nil;
				}

				// Verify the parameter exists before removing
				const FInstancedPropertyBag* UserParams = PCGGraph->GetUserParametersStruct();
				if (!UserParams || UserParams->GetNumPropertiesInBag() == 0)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"parameter\") -> no parameters on graph")));
					return sol::lua_nil;
				}

				bool bFound = false;
				const UPropertyBag* BagStruct = UserParams->GetPropertyBagStruct();
				if (BagStruct)
				{
					TConstArrayView<FPropertyBagPropertyDesc> Descs = BagStruct->GetPropertyDescs();
					for (const FPropertyBagPropertyDesc& Desc : Descs)
					{
						if (Desc.Name == ParamName) { bFound = true; break; }
					}
				}

				if (!bFound)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"parameter\") -> '%s' not found. Use list(\"parameters\") to see available."), *ParamName.ToString()));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("PCG: Remove Parameter")));
				PCGGraph->Modify();

				// Use RemovePropertyByName which preserves other parameter values
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				PCGGraph->UpdateUserParametersStruct([&ParamName](FInstancedPropertyBag& Bag)
				{
					Bag.RemovePropertyByName(ParamName);
				});
#else
				// UpdateUserParametersStruct not available in 5.4 — modify bag directly
				if (FInstancedPropertyBag* Bag = const_cast<FInstancedPropertyBag*>(PCGGraph->GetUserParametersStruct()))
				{
					Bag->RemovePropertyByName(ParamName);
				}
#endif

				PCGGraph->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"parameter\", name=\"%s\")"), *ParamName.ToString()));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: parameter"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// generate() — trigger generation on all level components using this graph
		// ================================================================
		AssetObj.set_function("generate", [PCGGraph, &Session](sol::table /*self*/,
			sol::optional<bool> ForceOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			bool bForce = ForceOpt.value_or(true);

			if (!GEditor)
			{
				Session.Log(TEXT("[FAIL] generate() -> no editor available"));
				return sol::lua_nil;
			}

			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World)
			{
				Session.Log(TEXT("[FAIL] generate() -> no editor world"));
				return sol::lua_nil;
			}

			int32 Count = 0;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (!Actor) continue;

				TArray<UPCGComponent*> PCGComponents;
				Actor->GetComponents<UPCGComponent>(PCGComponents);

				for (UPCGComponent* Comp : PCGComponents)
				{
					if (!Comp) continue;

					if (Comp->GetGraph() == PCGGraph)
					{
						Comp->Generate(bForce);
						Count++;
					}
				}
			}

			if (Count > 0)
			{
				Session.Log(FString::Printf(TEXT("[OK] generate(force=%s) -> triggered on %d components"), bForce ? TEXT("true") : TEXT("false"), Count));
			}
			else
			{
				Session.Log(TEXT("[OK] generate() -> no level components found using this graph. Place a PCG component first."));
			}

			return sol::make_object(Lua, Count);
		});
	});
}

REGISTER_LUA_BINDING(PCG, PCGDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindPCG(Lua, Session);
});

