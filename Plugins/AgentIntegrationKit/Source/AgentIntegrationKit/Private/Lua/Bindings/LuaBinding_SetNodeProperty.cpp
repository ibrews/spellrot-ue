#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaPinHelper.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "UObject/UnrealType.h"
#include "UObject/EnumProperty.h"

// Material graph node -> UMaterialExpression
#include "MaterialGraph/MaterialGraphNode.h"
#include "Materials/MaterialExpression.h"

// AI graph node (BT, EQS) -> NodeInstance (UBTNode, etc.)
#include "AIGraphNode.h"

// Sound Cue graph node -> USoundNode (Modulator, Delay, etc.)
#include "SoundCueGraph/SoundCueGraphNode.h"
#include "Sound/SoundNode.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// Resolve the "real" object whose properties the user likely wants to set.
// For most graph types this is the node itself, but some graph types wrap
// a separate runtime object (material expressions, BT tasks, etc.).
static UObject* ResolvePropertyTarget(UEdGraphNode* Node)
{
	// Material: UMaterialGraphNode wraps a UMaterialExpression
	if (UMaterialGraphNode* MatNode = Cast<UMaterialGraphNode>(Node))
	{
		if (MatNode->MaterialExpression)
			return MatNode->MaterialExpression;
	}

	// BT / EQS: UAIGraphNode wraps a NodeInstance (UBTTask, UBTDecorator, UEnvQueryTest, etc.)
	if (UAIGraphNode* AINode = Cast<UAIGraphNode>(Node))
	{
		if (AINode->NodeInstance)
			return AINode->NodeInstance;
	}

	// Sound Cue: USoundCueGraphNode wraps a USoundNode (Modulator, Delay, etc.)
	if (USoundCueGraphNode* SCNode = Cast<USoundCueGraphNode>(Node))
	{
		if (SCNode->SoundNode)
			return SCNode->SoundNode;
	}

	return Node;
}

// Find a property on a UObject by name, with flexible matching:
// exact -> bool "b" prefix -> case-insensitive
static FProperty* FindPropertyFlexible(UObject* Obj, const FString& Name)
{
	UClass* Class = Obj->GetClass();

	// Exact match
	if (FProperty* Prop = Class->FindPropertyByName(FName(*Name)))
		return Prop;

	// Bool prefix: "TwoSided" -> "bTwoSided"
	if (!Name.StartsWith(TEXT("b")))
	{
		FString BoolName = TEXT("b") + Name;
		if (FProperty* Prop = Class->FindPropertyByName(FName(*BoolName)))
			return Prop;
	}

	// Case-insensitive scan
	FString NameLower = Name.ToLower();
	for (TFieldIterator<FProperty> It(Class); It; ++It)
	{
		if (It->GetName().ToLower() == NameLower)
			return *It;
	}

	return nullptr;
}

// Resolve a nested property path like "DefaultValue.R" or "Settings.bEnabled".
// Returns the leaf FProperty and sets OutContainer to the address of the struct containing it.
static FProperty* ResolvePropertyPath(UObject* Obj, const FString& Path, void*& OutContainer)
{
	TArray<FString> Parts;
	Path.ParseIntoArray(Parts, TEXT("."));
	if (Parts.Num() == 0)
		return nullptr;

	UStruct* CurrentStruct = Obj->GetClass();
	void* CurrentContainer = Obj;

	for (int32 i = 0; i < Parts.Num(); ++i)
	{
		FProperty* Prop = nullptr;
		FName PartName(*Parts[i]);

		// Try exact match first
		Prop = CurrentStruct->FindPropertyByName(PartName);

		// Try bool prefix
		if (!Prop && !Parts[i].StartsWith(TEXT("b")))
		{
			FString BoolName = TEXT("b") + Parts[i];
			Prop = CurrentStruct->FindPropertyByName(FName(*BoolName));
		}

		// Case-insensitive fallback
		if (!Prop)
		{
			FString LowerPart = Parts[i].ToLower();
			for (TFieldIterator<FProperty> It(CurrentStruct); It; ++It)
			{
				if (It->GetName().ToLower() == LowerPart)
				{
					Prop = *It;
					break;
				}
			}
		}

		if (!Prop)
			return nullptr;

		// If this is the last segment, we're done
		if (i == Parts.Num() - 1)
		{
			OutContainer = CurrentContainer;
			return Prop;
		}

		// Must be a struct property to descend further
		FStructProperty* StructProp = CastField<FStructProperty>(Prop);
		if (!StructProp || !StructProp->Struct)
			return nullptr;

		CurrentContainer = StructProp->ContainerPtrToValuePtr<void>(CurrentContainer);
		CurrentStruct = StructProp->Struct;
	}

	return nullptr;
}

// List settable properties on an object (for error messages)
static FString ListProperties(UObject* Obj, bool bEditableOnly = true)
{
	FString List;
	for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
	{
		FProperty* Prop = *It;
		if (bEditableOnly && !Prop->HasAnyPropertyFlags(CPF_Edit))
			continue;
		if (Prop->HasAnyPropertyFlags(CPF_Deprecated))
			continue;

		if (List.Len() > 0) List += TEXT(", ");
		List += FString::Printf(TEXT("%s (%s)"), *Prop->GetName(), *Prop->GetCPPType());
	}
	return List;
}

// Get the current value of a property as a string
static FString GetPropertyValueString(void* Container, FProperty* Prop)
{
	FString Value;
	const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Container);
	Prop->ExportTextItem_Direct(Value, ValuePtr, nullptr, nullptr, PPF_None);
	return Value;
}

// Overload for UObject* (most common case)
static FString GetPropertyValueString(UObject* Obj, FProperty* Prop)
{
	FString Value;
	const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Obj);
	Prop->ExportTextItem_Direct(Value, ValuePtr, nullptr, Obj, PPF_None);
	return Value;
}

// Collect properties from an object into a lua table, returns next index
static int32 CollectProperties(UObject* Obj, sol::state_view& LuaView, sol::table& Result, int32 StartIdx, const FString& Prefix)
{
	int32 Idx = StartIdx;
	for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit))
			continue;
		if (Prop->HasAnyPropertyFlags(CPF_Deprecated))
			continue;

		FString DisplayName = Prefix + Prop->GetName();

		sol::table Entry = LuaView.create_table();
		Entry["name"] = TCHAR_TO_UTF8(*DisplayName);
		Entry["type"] = TCHAR_TO_UTF8(*Prop->GetCPPType());
		Entry["value"] = TCHAR_TO_UTF8(*GetPropertyValueString(Obj, Prop));

		// Add metadata if available
#if WITH_EDITORONLY_DATA
		if (const FString* Tooltip = Prop->FindMetaData(TEXT("ToolTip")))
		{
			if (!Tooltip->IsEmpty())
				Entry["tooltip"] = TCHAR_TO_UTF8(**Tooltip);
		}

		if (const FString* UIMin = Prop->FindMetaData(TEXT("UIMin")))
			Entry["min"] = TCHAR_TO_UTF8(**UIMin);
		if (const FString* UIMax = Prop->FindMetaData(TEXT("UIMax")))
			Entry["max"] = TCHAR_TO_UTF8(**UIMax);
		if (const FString* ClampMin = Prop->FindMetaData(TEXT("ClampMin")))
		{
			if (!Entry["min"].valid()) Entry["min"] = TCHAR_TO_UTF8(**ClampMin);
		}
		if (const FString* ClampMax = Prop->FindMetaData(TEXT("ClampMax")))
		{
			if (!Entry["max"].valid()) Entry["max"] = TCHAR_TO_UTF8(**ClampMax);
		}
#endif

		// Enum values
		if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			if (UEnum* Enum = EnumProp->GetEnum())
			{
				sol::table EnumValues = LuaView.create_table();
				for (int32 i = 0; i < Enum->NumEnums() - 1; ++i) // -1 to skip _MAX
				{
					EnumValues[i + 1] = TCHAR_TO_UTF8(*Enum->GetNameStringByIndex(i));
				}
				Entry["enum_values"] = EnumValues;
			}
		}
		else if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			if (ByteProp->Enum)
			{
				sol::table EnumValues = LuaView.create_table();
				for (int32 i = 0; i < ByteProp->Enum->NumEnums() - 1; ++i)
				{
					EnumValues[i + 1] = TCHAR_TO_UTF8(*ByteProp->Enum->GetNameStringByIndex(i));
				}
				Entry["enum_values"] = EnumValues;
			}
		}

		// Object reference class
		if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
		{
			if (ObjProp->PropertyClass)
			{
				Entry["object_class"] = TCHAR_TO_UTF8(*ObjProp->PropertyClass->GetName());
			}
		}

		// Struct sub-properties hint
		if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			if (StructProp->Struct)
			{
				Entry["struct_type"] = TCHAR_TO_UTF8(*StructProp->Struct->GetName());
			}
		}

		// Array inner type
		if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
		{
			if (ArrayProp->Inner)
			{
				Entry["array_element_type"] = TCHAR_TO_UTF8(*ArrayProp->Inner->GetCPPType());
			}
		}

		Result[Idx++] = Entry;
	}
	return Idx;
}

// Convert sol::object value to FString for ImportText
static bool ValueToString(sol::object& Value, FString& OutValue)
{
	if (Value.is<bool>())
	{
		OutValue = Value.as<bool>() ? TEXT("True") : TEXT("False");
		return true;
	}
	if (Value.is<std::string>())
	{
		OutValue = UTF8_TO_TCHAR(Value.as<std::string>().c_str());
		return true;
	}
	if (Value.is<double>())
	{
		double D = Value.as<double>();
		if (FMath::IsNearlyEqual(D, FMath::RoundToDouble(D)))
			OutValue = FString::Printf(TEXT("%lld"), (int64)D);
		else
			OutValue = FString::Printf(TEXT("%g"), D);
		return true;
	}
	return false;
}

static TArray<FLuaFunctionDoc> SetNodePropertyDocs = {
	{
		TEXT("set_node_property(handle, property, value)"),
		TEXT("Set a UPROPERTY on a graph node (or its underlying object like UMaterialExpression, UBTTask, etc.).\n"
			"  Supports dot-paths for struct members: set_node_property(h, 'DefaultValue.R', 0.5)\n"
			"  Examples: set_node_property(h, 'ParameterName', 'BaseColor')\n"
			"            set_node_property(h, 'DefaultValue', '(R=0.0,G=0.3,B=1.0,A=1.0)')\n"
			"            set_node_property(h, 'Code', 'return 1.0;')"),
		TEXT("true on success, nil on failure")
	},
	{
		TEXT("set_node_properties(handle, {prop=value, ...})"),
		TEXT("Set multiple UPROPERTYs on a graph node in one call. Avoids redundant node reconstruction.\n"
			"  Example: set_node_properties(h, {ParameterName='BaseColor', bTwoSided=true})"),
		TEXT("table of {property=true/false} results")
	},
	{
		TEXT("get_node_property(handle, property)"),
		TEXT("Get a UPROPERTY value from a graph node (or its underlying object). Supports dot-paths for struct members."),
		TEXT("string value or nil")
	},
	{
		TEXT("list_node_properties(handle)"),
		TEXT("List all editable UPROPERTYs on a graph node's underlying object.\n"
			"  Returns metadata: name, type, value, tooltip, min, max, enum_values, struct_type, object_class, array_element_type."),
		TEXT("table of property info tables or nil")
	}
};

REGISTER_LUA_BINDING(SetNodeProperty, SetNodePropertyDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	// Shared helper: resolve target + property (flat or dot-path), with fallback to node
	auto ResolveTargetAndProp = [](UEdGraphNode* Node, const FString& PropName,
		UObject*& OutTarget, FProperty*& OutProp, void*& OutContainer) -> bool
	{
		OutTarget = ResolvePropertyTarget(Node);
		OutContainer = OutTarget;

		// Try dot-path first (on resolved target)
		if (PropName.Contains(TEXT(".")))
		{
			OutProp = ResolvePropertyPath(OutTarget, PropName, OutContainer);
			if (OutProp) return true;

			// Try dot-path on the node itself
			if (OutTarget != Node)
			{
				OutTarget = Node;
				OutContainer = Node;
				OutProp = ResolvePropertyPath(OutTarget, PropName, OutContainer);
				if (OutProp) return true;
			}
			return false;
		}

		// Flat property lookup
		OutProp = FindPropertyFlexible(OutTarget, PropName);
		if (OutProp) return true;

		if (OutTarget != Node)
		{
			OutTarget = Node;
			OutContainer = Node;
			OutProp = FindPropertyFlexible(OutTarget, PropName);
			if (OutProp) return true;
		}

		return false;
	};

	// Shared helper: perform the actual property set on a target
	// Returns true on success. Does NOT call ReconstructNode (caller's responsibility).
	auto SetPropertyOnTarget = [&Session](
		UEdGraphNode* Node, UObject* Target, FProperty* Prop, void* Container,
		sol::object& Value, const FString& Handle, const FString& PropName) -> bool
	{
		FString FValue;
		if (!ValueToString(Value, FValue))
		{
			Session.Log(TEXT("[FAIL] set_node_property -> value must be a string, number, or boolean"));
			return false;
		}

		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Container);

		// Snapshot current value — ImportText_Direct can partially write to the
		// destination buffer before failing, leaving the property in a corrupt
		// state that crashes PostEditChangeProperty.
		TArray<uint8> Backup;
		Backup.SetNumUninitialized(Prop->GetSize());
		Prop->CopyCompleteValue(Backup.GetData(), ValuePtr);

		Target->Modify();
		Target->PreEditChange(Prop);
		const TCHAR* Result = Prop->ImportText_Direct(*FValue, ValuePtr, Target, PPF_None);

		if (!Result)
		{
			// For struct wrapper types (FValueOrBBKey_Float, etc.), try setting
			// the inner DefaultValue sub-property instead
			if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
			{
				FProperty* DefaultValueProp = StructProp->Struct->FindPropertyByName(TEXT("DefaultValue"));
				if (DefaultValueProp)
				{
					void* InnerPtr = DefaultValueProp->ContainerPtrToValuePtr<void>(ValuePtr);
					Result = DefaultValueProp->ImportText_Direct(*FValue, InnerPtr, Target, PPF_None);
				}
			}
		}

		if (!Result)
		{
			// Restore original value before PostEditChange sees corrupted memory
			Prop->CopyCompleteValue(ValuePtr, Backup.GetData());

			FPropertyChangedEvent FailEvent(Prop, EPropertyChangeType::Unspecified);
			Target->PostEditChangeProperty(FailEvent);
			Session.Log(FString::Printf(TEXT("[FAIL] set_node_property(%s, \"%s\") -> failed to parse value \"%s\" for type %s"),
				*Handle, *PropName, *FValue, *Prop->GetCPPType()));
			return false;
		}

		FPropertyChangedEvent ChangeEvent(Prop);
		Target->PostEditChangeProperty(ChangeEvent);

		// If we set a property on a sub-object (e.g. UMaterialExpression), also notify
		// the owning graph node so its overridden PostEditChangeProperty fires
		if (Target != Node)
		{
			FPropertyChangedEvent NodeEvent(nullptr);
			Node->PostEditChangeProperty(NodeEvent);
		}

		return true;
	};

	Lua.set_function("set_node_property", [&Session, ResolveTargetAndProp, SetPropertyOnTarget](
		const std::string& Handle, const std::string& PropertyName,
		sol::object Value, sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		FString FPropName = UTF8_TO_TCHAR(PropertyName.c_str());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] set_node_property -> node \"%s\" not found"), *FHandle));
			return sol::lua_nil;
		}

		UObject* Target = nullptr;
		FProperty* Prop = nullptr;
		void* Container = nullptr;

		if (!ResolveTargetAndProp(Node, FPropName, Target, Prop, Container))
		{
			UObject* PrimaryTarget = ResolvePropertyTarget(Node);
			Session.Log(FString::Printf(TEXT("[FAIL] set_node_property -> property \"%s\" not found on %s. Editable: %s"),
				*FPropName, *PrimaryTarget->GetClass()->GetName(), *ListProperties(PrimaryTarget)));
			return sol::lua_nil;
		}

		if (!SetPropertyOnTarget(Node, Target, Prop, Container, Value, FHandle, FPropName))
			return sol::lua_nil;

		// Only reconstruct if the property was set on the node itself (pin-affecting),
		// not on a sub-object like UMaterialExpression where pins are expression-driven
		if (Target == Node)
			Node->ReconstructNode();

		if (UEdGraph* Graph = Node->GetGraph())
			Session.MarkGraphDirty(Graph);

		FString NewValue = GetPropertyValueString(Container, Prop);
		Session.Log(FString::Printf(TEXT("[OK] set_node_property(%s, \"%s\") = \"%s\""),
			*FHandle, *Prop->GetName(), *NewValue));
		return sol::make_object(S, true);
	});

	Lua.set_function("set_node_properties", [&Session, ResolveTargetAndProp, SetPropertyOnTarget](
		const std::string& Handle, sol::table Properties, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] set_node_properties -> node \"%s\" not found"), *FHandle));
			return sol::lua_nil;
		}

		sol::table Results = LuaView.create_table();
		int32 SuccessCount = 0;
		int32 FailCount = 0;
		bool bAnyNodePropSet = false;

		for (auto& [Key, Val] : Properties)
		{
			if (!Key.is<std::string>())
				continue;

			std::string PropNameStd = Key.as<std::string>();
			FString FPropName = UTF8_TO_TCHAR(PropNameStd.c_str());

			UObject* Target = nullptr;
			FProperty* Prop = nullptr;
			void* Container = nullptr;

			if (!ResolveTargetAndProp(Node, FPropName, Target, Prop, Container))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_node_properties -> property \"%s\" not found"), *FPropName));
				Results[PropNameStd] = false;
				FailCount++;
				continue;
			}

			sol::object ValObj = Val;
			if (SetPropertyOnTarget(Node, Target, Prop, Container, ValObj, FHandle, FPropName))
			{
				Results[PropNameStd] = true;
				SuccessCount++;
				if (Target == Node)
					bAnyNodePropSet = true;
			}
			else
			{
				Results[PropNameStd] = false;
				FailCount++;
			}
		}

		// Single reconstruct after all properties are set
		if (bAnyNodePropSet)
			Node->ReconstructNode();

		if (UEdGraph* Graph = Node->GetGraph())
			Session.MarkGraphDirty(Graph);

		Session.Log(FString::Printf(TEXT("[OK] set_node_properties(%s) -> %d succeeded, %d failed"),
			*FHandle, SuccessCount, FailCount));
		return Results;
	});

	Lua.set_function("get_node_property", [&Session, ResolveTargetAndProp](
		const std::string& Handle, const std::string& PropertyName,
		sol::this_state S) -> sol::object
	{
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());
		FString FPropName = UTF8_TO_TCHAR(PropertyName.c_str());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] get_node_property -> node \"%s\" not found"), *FHandle));
			return sol::lua_nil;
		}

		UObject* Target = nullptr;
		FProperty* Prop = nullptr;
		void* Container = nullptr;

		if (!ResolveTargetAndProp(Node, FPropName, Target, Prop, Container))
		{
			UObject* PrimaryTarget = ResolvePropertyTarget(Node);
			Session.Log(FString::Printf(TEXT("[FAIL] get_node_property -> property \"%s\" not found on %s"),
				*FPropName, *PrimaryTarget->GetClass()->GetName()));
			return sol::lua_nil;
		}

		FString Value = GetPropertyValueString(Container, Prop);
		return sol::make_object(S, std::string(TCHAR_TO_UTF8(*Value)));
	});

	Lua.set_function("list_node_properties", [&Session](const std::string& Handle, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString FHandle = UTF8_TO_TCHAR(Handle.c_str());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] list_node_properties -> node \"%s\" not found"), *FHandle));
			return sol::lua_nil;
		}

		UObject* Target = ResolvePropertyTarget(Node);

		sol::table Result = LuaView.create_table();
		int32 Idx = CollectProperties(Target, LuaView, Result, 1, TEXT(""));

		// Also include node-level properties if target differs
		if (Target != Node)
		{
			Idx = CollectProperties(Node, LuaView, Result, Idx, TEXT(""));
		}

		Session.Log(FString::Printf(TEXT("[OK] list_node_properties(%s) -> %d editable properties on %s%s"),
			*FHandle, Idx - 1, *Target->GetClass()->GetName(),
			Target != Node ? *FString::Printf(TEXT(" + %s"), *Node->GetClass()->GetName()) : TEXT("")));
		return Result;
	});
});
