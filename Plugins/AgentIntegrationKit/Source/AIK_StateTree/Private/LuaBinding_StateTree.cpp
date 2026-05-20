#include "Lua/LuaBindingRegistry.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreeTypes.h"
#include "StateTreeSchema.h"
#include "StateTreeEditorNode.h"
#include "StateTreeTaskBase.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreeConditionBase.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
#include "StateTreeConsiderationBase.h"
#endif
#include "StateTreeCompiler.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeEditorPropertyBindings.h"
#include "PropertyBindingPath.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectIterator.h"
#include "GameplayTagsManager.h"

#include "PropertyBag.h"
#include "Modules/ModuleManager.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// StateTree editor bindings require UE 5.5+ (many types/members added in 5.5)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5

// ============================================================================
// Helpers
// ============================================================================

namespace
{

UStateTreeState* FindStateRecursive(UStateTreeState* State, const FString& Name)
{
	if (!State) return nullptr;
	if (State->Name.ToString().Equals(Name, ESearchCase::IgnoreCase)) return State;
	for (UStateTreeState* Child : State->Children)
	{
		UStateTreeState* Found = FindStateRecursive(Child, Name);
		if (Found) return Found;
	}
	return nullptr;
}

UStateTreeState* FindStateInEditor(UStateTreeEditorData* ED, const FString& Name)
{
	if (!ED || Name.IsEmpty()) return nullptr;
	for (UStateTreeState* Sub : ED->SubTrees)
	{
		UStateTreeState* Found = FindStateRecursive(Sub, Name);
		if (Found) return Found;
	}
	return nullptr;
}

const UScriptStruct* FindSTNodeStruct(const FString& TypeName)
{
	TArray<FString> Search;
	Search.Add(TypeName);
	if (!TypeName.StartsWith(TEXT("F"))) Search.Add(TEXT("F") + TypeName);
	if (!TypeName.Contains(TEXT("StateTree")))
	{
		Search.Add(TEXT("FStateTree") + TypeName);
		Search.Add(TEXT("FStateTree") + TypeName + TEXT("Task"));
		Search.Add(TEXT("FStateTree") + TypeName + TEXT("Evaluator"));
		Search.Add(TEXT("FStateTree") + TypeName + TEXT("Condition"));
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		Search.Add(TEXT("FStateTree") + TypeName + TEXT("Consideration"));
#endif // ENGINE_MINOR_VERSION >= 5
	}

	const UScriptStruct* TaskBase = FStateTreeTaskBase::StaticStruct();
	const UScriptStruct* EvalBase = FStateTreeEvaluatorBase::StaticStruct();
	const UScriptStruct* CondBase = FStateTreeConditionBase::StaticStruct();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	const UScriptStruct* ConsBase = FStateTreeConsiderationBase::StaticStruct();
#endif // ENGINE_MINOR_VERSION >= 5
	const UScriptStruct* NodeBase = FStateTreeNodeBase::StaticStruct();
	const UScriptStruct* Fallback = nullptr;

	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		UScriptStruct* S = *It;
		if (S->HasMetaData(TEXT("Hidden"))) continue;
		bool bIsSTNode = S->IsChildOf(TaskBase) || S->IsChildOf(EvalBase) || S->IsChildOf(CondBase)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			|| S->IsChildOf(ConsBase)
#endif // ENGINE_MINOR_VERSION >= 5
			;
		if (!bIsSTNode) continue;
		if (S == TaskBase || S == EvalBase || S == CondBase
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			|| S == ConsBase
#endif // ENGINE_MINOR_VERSION >= 5
			|| S == NodeBase) continue;

		const FString SName = S->GetName();
		for (const FString& Q : Search)
		{
			if (SName.Equals(Q, ESearchCase::IgnoreCase)) return S;
		}
		if (!Fallback && SName.Contains(TypeName, ESearchCase::IgnoreCase))
			Fallback = S;
	}
	return Fallback;
}

EStateTreeStateType ParseStateType(const FString& T)
{
	if (T.Equals(TEXT("Group"), ESearchCase::IgnoreCase)) return EStateTreeStateType::Group;
	if (T.Equals(TEXT("Linked"), ESearchCase::IgnoreCase)) return EStateTreeStateType::Linked;
	if (T.Equals(TEXT("LinkedAsset"), ESearchCase::IgnoreCase)) return EStateTreeStateType::LinkedAsset;
	if (T.Equals(TEXT("Subtree"), ESearchCase::IgnoreCase)) return EStateTreeStateType::Subtree;
	return EStateTreeStateType::State;
}

EStateTreeStateSelectionBehavior ParseSelBehavior(const FString& B)
{
	if (B.Equals(TEXT("None"), ESearchCase::IgnoreCase)) return EStateTreeStateSelectionBehavior::None;
	if (B.Equals(TEXT("TryEnterState"), ESearchCase::IgnoreCase) || B.Equals(TEXT("TryEnter"), ESearchCase::IgnoreCase))
		return EStateTreeStateSelectionBehavior::TryEnterState;
	// Check weighted-by-utility BEFORE generic "Random" and "Utility" checks
	if (B.Contains(TEXT("RandomWeighted")) || B.Contains(TEXT("WeightedByUtility")) ||
		B.Equals(TEXT("TrySelectChildrenAtRandomWeightedByUtility"), ESearchCase::IgnoreCase))
		return EStateTreeStateSelectionBehavior::TrySelectChildrenAtRandomWeightedByUtility;
	if (B.Equals(TEXT("Random"), ESearchCase::IgnoreCase) || B.Equals(TEXT("TrySelectChildrenAtRandom"), ESearchCase::IgnoreCase))
		return EStateTreeStateSelectionBehavior::TrySelectChildrenAtRandom;
	if (B.Contains(TEXT("Utility")) || B.Equals(TEXT("TrySelectChildrenWithHighestUtility"), ESearchCase::IgnoreCase))
		return EStateTreeStateSelectionBehavior::TrySelectChildrenWithHighestUtility;
	if (B.Contains(TEXT("Transition")) || B.Equals(TEXT("TryFollowTransitions"), ESearchCase::IgnoreCase))
		return EStateTreeStateSelectionBehavior::TryFollowTransitions;
	return EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder;
}

EStateTreeTransitionTrigger ParseTrigger(const FString& T)
{
	if (T.Equals(TEXT("OnStateSucceeded"), ESearchCase::IgnoreCase) || T.Equals(TEXT("Succeeded"), ESearchCase::IgnoreCase))
		return EStateTreeTransitionTrigger::OnStateSucceeded;
	if (T.Equals(TEXT("OnStateFailed"), ESearchCase::IgnoreCase) || T.Equals(TEXT("Failed"), ESearchCase::IgnoreCase))
		return EStateTreeTransitionTrigger::OnStateFailed;
	if (T.Equals(TEXT("OnTick"), ESearchCase::IgnoreCase) || T.Equals(TEXT("Tick"), ESearchCase::IgnoreCase))
		return EStateTreeTransitionTrigger::OnTick;
	if (T.Equals(TEXT("OnEvent"), ESearchCase::IgnoreCase) || T.Equals(TEXT("Event"), ESearchCase::IgnoreCase))
		return EStateTreeTransitionTrigger::OnEvent;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	if (T.Equals(TEXT("OnDelegate"), ESearchCase::IgnoreCase) || T.Equals(TEXT("Delegate"), ESearchCase::IgnoreCase))
		return EStateTreeTransitionTrigger::OnDelegate;
#endif
	return EStateTreeTransitionTrigger::OnStateCompleted;
}

EStateTreeTransitionType ParseTransType(const FString& T)
{
	if (T.Equals(TEXT("Succeeded"), ESearchCase::IgnoreCase)) return EStateTreeTransitionType::Succeeded;
	if (T.Equals(TEXT("Failed"), ESearchCase::IgnoreCase)) return EStateTreeTransitionType::Failed;
	if (T.Equals(TEXT("NextState"), ESearchCase::IgnoreCase) || T.Equals(TEXT("Next"), ESearchCase::IgnoreCase))
		return EStateTreeTransitionType::NextState;
	if (T.Equals(TEXT("None"), ESearchCase::IgnoreCase)) return EStateTreeTransitionType::None;
	return EStateTreeTransitionType::GotoState;
}

EStateTreeTransitionPriority ParsePriority(const FString& P)
{
	if (P.Equals(TEXT("Low"), ESearchCase::IgnoreCase)) return EStateTreeTransitionPriority::Low;
	if (P.Equals(TEXT("Medium"), ESearchCase::IgnoreCase)) return EStateTreeTransitionPriority::Medium;
	if (P.Equals(TEXT("High"), ESearchCase::IgnoreCase)) return EStateTreeTransitionPriority::High;
	if (P.Equals(TEXT("Critical"), ESearchCase::IgnoreCase)) return EStateTreeTransitionPriority::Critical;
	return EStateTreeTransitionPriority::Normal;
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
EStateTreeTaskCompletionType ParseTasksCompletion(const FString& C)
{
	if (C.Equals(TEXT("All"), ESearchCase::IgnoreCase)) return EStateTreeTaskCompletionType::All;
	return EStateTreeTaskCompletionType::Any;
}
#endif

void InitEditorNode(FStateTreeEditorNode& Node, const UScriptStruct* Struct)
{
	Node.ID = FGuid::NewGuid();
	Node.Node.InitializeAs(Struct);
	if (const FStateTreeNodeBase* Base = Node.Node.GetPtr<FStateTreeNodeBase>())
	{
		if (const UScriptStruct* InstType = Cast<const UScriptStruct>(Base->GetInstanceDataType()))
			Node.Instance.InitializeAs(InstType);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		if (const UScriptStruct* RTType = Cast<const UScriptStruct>(Base->GetExecutionRuntimeDataType()))
			Node.ExecutionRuntimeData.InitializeAs(RTType);
#endif
	}
}

void CollectStatesRecursive(UStateTreeState* State, sol::state_view& Lua, sol::table& Result, int32& Idx)
{
	if (!State) return;
	sol::table Entry = Lua.create_table();
	Entry["name"] = TCHAR_TO_UTF8(*State->Name.ToString());
	Entry["type"] = TCHAR_TO_UTF8(*UEnum::GetValueAsString(State->Type));
	Entry["enabled"] = State->bEnabled;
	Entry["selection_behavior"] = TCHAR_TO_UTF8(*UEnum::GetValueAsString(State->SelectionBehavior));
	Entry["tasks"] = State->Tasks.Num();
	Entry["transitions"] = State->Transitions.Num();
	Entry["enter_conditions"] = State->EnterConditions.Num();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	Entry["considerations"] = State->Considerations.Num();
#endif // ENGINE_MINOR_VERSION >= 5
	Entry["children"] = State->Children.Num();
	if (State->Tag.IsValid())
		Entry["tag"] = TCHAR_TO_UTF8(*State->Tag.ToString());
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	if (!State->Description.IsEmpty())
		Entry["description"] = TCHAR_TO_UTF8(*State->Description);
#endif
	if (State->Weight != 1.0f)
		Entry["weight"] = State->Weight;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	if (State->bHasCustomTickRate)
		Entry["custom_tick_rate"] = State->CustomTickRate;
#endif
	if (State->bHasRequiredEventToEnter && State->RequiredEventToEnter.IsValid())
		Entry["required_event"] = TCHAR_TO_UTF8(*State->RequiredEventToEnter.Tag.ToString());
	Entry["check_prerequisites_for_child"] = State->bCheckPrerequisitesWhenActivatingChildDirectly;
	if (State->Parent)
		Entry["parent"] = TCHAR_TO_UTF8(*State->Parent->Name.ToString());
	Entry["id"] = TCHAR_TO_UTF8(*State->ID.ToString());
	Result[Idx++] = Entry;
	for (UStateTreeState* Child : State->Children)
		CollectStatesRecursive(Child, Lua, Result, Idx);
}

bool CompileST(UStateTree* ST, FString& OutMsg)
{
	FStateTreeCompilerLog Log;
	FStateTreeCompiler Compiler(Log);
	bool bOK = Compiler.Compile(*ST);
	if (!bOK)
	{
		OutMsg = TEXT("Compile FAILED");
		// Log messages are tokenized, just report success/fail
	}
	else
	{
		OutMsg = TEXT("Compile OK");
	}
	return bOK;
}

/** Find an editor node by state name, node type, and index (mirrors MCP tool FindEditorNode) */
FStateTreeEditorNode* FindEditorNodeInST(UStateTreeEditorData* ED, const FString& StateName, const FString& NodeType, int32 Index)
{
	if (!ED || NodeType.IsEmpty()) return nullptr;

	if (NodeType.Equals(TEXT("evaluator"), ESearchCase::IgnoreCase))
	{
		if (Index >= 0 && Index < ED->Evaluators.Num())
			return &ED->Evaluators[Index];
		return nullptr;
	}
	if (NodeType.Equals(TEXT("global_task"), ESearchCase::IgnoreCase))
	{
		if (Index >= 0 && Index < ED->GlobalTasks.Num())
			return &ED->GlobalTasks[Index];
		return nullptr;
	}

	UStateTreeState* State = FindStateInEditor(ED, StateName);
	if (!State) return nullptr;

	if (NodeType.Equals(TEXT("task"), ESearchCase::IgnoreCase))
	{
		if (Index >= 0 && Index < State->Tasks.Num())
			return &State->Tasks[Index];
	}
	else if (NodeType.Equals(TEXT("condition"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("enter_condition"), ESearchCase::IgnoreCase))
	{
		if (Index >= 0 && Index < State->EnterConditions.Num())
			return &State->EnterConditions[Index];
	}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	else if (NodeType.Equals(TEXT("consideration"), ESearchCase::IgnoreCase))
	{
		if (Index >= 0 && Index < State->Considerations.Num())
			return &State->Considerations[Index];
	}
#endif // ENGINE_MINOR_VERSION >= 5
	return nullptr;
}

EPropertyBagPropertyType ParsePropertyBagType(const FString& TypeStr)
{
	if (TypeStr.Equals(TEXT("Bool"), ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::Bool;
	if (TypeStr.Equals(TEXT("Byte"), ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::Byte;
	if (TypeStr.Equals(TEXT("Int32"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("Int"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("Integer"), ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::Int32;
	if (TypeStr.Equals(TEXT("Int64"), ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::Int64;
	if (TypeStr.Equals(TEXT("Float"), ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::Float;
	if (TypeStr.Equals(TEXT("Double"), ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::Double;
	if (TypeStr.Equals(TEXT("Name"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("FName"), ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::Name;
	if (TypeStr.Equals(TEXT("String"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("FString"), ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::String;
	if (TypeStr.Equals(TEXT("Text"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("FText"), ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::Text;
	if (TypeStr.Equals(TEXT("UInt32"), ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::UInt32;
	if (TypeStr.Equals(TEXT("UInt64"), ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::UInt64;
	return EPropertyBagPropertyType::None;
}

void SetParameterValueOnBag(FInstancedPropertyBag* Bag, const FString& ParamName, EPropertyBagPropertyType Type, const FString& Value)
{
	if (!Bag || Value.IsEmpty()) return;
	FName PropName(*ParamName);

	switch (Type)
	{
	case EPropertyBagPropertyType::Bool:
		Bag->SetValueBool(PropName, Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("1")));
		break;
	case EPropertyBagPropertyType::Byte:
		Bag->SetValueByte(PropName, static_cast<uint8>(FCString::Atoi(*Value)));
		break;
	case EPropertyBagPropertyType::Int32:
		Bag->SetValueInt32(PropName, FCString::Atoi(*Value));
		break;
	case EPropertyBagPropertyType::Int64:
		Bag->SetValueInt64(PropName, FCString::Atoi64(*Value));
		break;
	case EPropertyBagPropertyType::Float:
		Bag->SetValueFloat(PropName, FCString::Atof(*Value));
		break;
	case EPropertyBagPropertyType::Double:
		Bag->SetValueDouble(PropName, FCString::Atod(*Value));
		break;
	case EPropertyBagPropertyType::Name:
		Bag->SetValueName(PropName, FName(*Value));
		break;
	case EPropertyBagPropertyType::String:
		Bag->SetValueString(PropName, Value);
		break;
	case EPropertyBagPropertyType::Text:
		Bag->SetValueText(PropName, FText::FromString(Value));
		break;
	case EPropertyBagPropertyType::UInt32:
		Bag->SetValueUInt32(PropName, static_cast<uint32>(FCString::Strtoui64(*Value, nullptr, 10)));
		break;
	case EPropertyBagPropertyType::UInt64:
		Bag->SetValueUInt64(PropName, FCString::Strtoui64(*Value, nullptr, 10));
		break;
	default:
		break;
	}
}

FInstancedPropertyBag* GetParameterBag(UStateTreeEditorData* ED, const FString& StateName, FString& OutError)
{
	if (!ED)
	{
		OutError = TEXT("EditorData is null");
		return nullptr;
	}

	if (StateName.IsEmpty())
	{
		// Global parameters
		FProperty* Prop = ED->GetClass()->FindPropertyByName(TEXT("RootParameterPropertyBag"));
		if (!Prop)
		{
			OutError = TEXT("Could not find RootParameterPropertyBag property on editor data");
			return nullptr;
		}
		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(ED);
		if (!ValuePtr)
		{
			OutError = TEXT("Could not get RootParameterPropertyBag value pointer");
			return nullptr;
		}
		return static_cast<FInstancedPropertyBag*>(ValuePtr);
	}
	else
	{
		UStateTreeState* State = FindStateInEditor(ED, StateName);
		if (!State)
		{
			OutError = FString::Printf(TEXT("State '%s' not found"), *StateName);
			return nullptr;
		}
		return &State->Parameters.Parameters;
	}
}

/** Try to set a property on a struct instance via reflection, returning true if property was found */
bool TrySetPropertyOnStructInstance(const UScriptStruct* ScriptStruct, uint8* Memory, const FString& PropName, const FString& TextValue, FString& OutResult)
{
	if (!ScriptStruct || !Memory) return false;

	FProperty* Property = nullptr;
	for (TFieldIterator<FProperty> It(ScriptStruct); It; ++It)
	{
		if ((*It)->GetName().Equals(PropName, ESearchCase::IgnoreCase))
		{
			Property = *It;
			break;
		}
	}
	if (!Property) return false;

	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Memory);
	if (!ValuePtr)
	{
		OutResult = FString::Printf(TEXT("  ! %s: could not get value pointer"), *PropName);
		return true;
	}

	// Handle enum properties with string values
	if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
	{
		UEnum* Enum = EnumProp->GetEnum();
		if (Enum)
		{
			int64 EnumVal = Enum->GetValueByNameString(TextValue);
			if (EnumVal == INDEX_NONE)
				EnumVal = Enum->GetValueByNameString(Enum->GetName() + TEXT("::") + TextValue);
			if (EnumVal != INDEX_NONE)
			{
				EnumProp->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, EnumVal);
				OutResult = FString::Printf(TEXT("  = %s -> %s"), *PropName, *TextValue);
			}
			else
			{
				OutResult = FString::Printf(TEXT("  ! %s: invalid enum value '%s'"), *PropName, *TextValue);
			}
		}
		return true;
	}

	// Handle TEnumAsByte
	if (FByteProperty* ByteProp = CastField<FByteProperty>(Property))
	{
		if (UEnum* Enum = ByteProp->GetIntPropertyEnum())
		{
			int64 EnumVal = Enum->GetValueByNameString(TextValue);
			if (EnumVal == INDEX_NONE)
				EnumVal = Enum->GetValueByNameString(Enum->GetName() + TEXT("::") + TextValue);
			if (EnumVal != INDEX_NONE)
			{
				ByteProp->SetIntPropertyValue(ValuePtr, EnumVal);
				OutResult = FString::Printf(TEXT("  = %s -> %s"), *PropName, *TextValue);
			}
			else
			{
				OutResult = FString::Printf(TEXT("  ! %s: invalid enum value '%s'"), *PropName, *TextValue);
			}
			return true;
		}
	}

	// ImportText for everything else
	const TCHAR* TextPtr = *TextValue;
	if (Property->ImportText_Direct(TextPtr, ValuePtr, nullptr, PPF_None))
	{
		OutResult = FString::Printf(TEXT("  = %s -> %s"), *PropName, *TextValue);
	}
	else
	{
		OutResult = FString::Printf(TEXT("  ! %s: failed to set '%s'"), *PropName, *TextValue);
	}
	return true;
}

bool TryAddStateTreeParameter(FInstancedPropertyBag* Bag, FName ParamName, EPropertyBagPropertyType PropType, FString& OutError)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	const EPropertyBagAlterationResult Result = Bag->AddProperty(ParamName, PropType, nullptr, false);
	if (Result == EPropertyBagAlterationResult::Success)
	{
		return true;
	}

	switch (Result)
	{
	case EPropertyBagAlterationResult::PropertyNameEmpty:
		OutError = TEXT("property name is empty");
		break;
	case EPropertyBagAlterationResult::PropertyNameInvalidCharacters:
		OutError = TEXT("property name has invalid characters");
		break;
	case EPropertyBagAlterationResult::TargetPropertyAlreadyExists:
		OutError = TEXT("property already exists");
		break;
	default:
		OutError = TEXT("internal error");
		break;
	}
	return false;
#else
	Bag->AddProperty(ParamName, PropType);
	(void)OutError;
	return true;
#endif
}

bool BuildStateTreeBindingTargetPath(const FGuid& StructID, const FString& PropertyPath, FPropertyBindingPath& OutTargetPath, FString& OutError)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	OutTargetPath.SetStructID(StructID);
	if (!OutTargetPath.FromString(PropertyPath))
	{
		OutError = FString::Printf(TEXT("invalid property path '%s'"), *PropertyPath);
		return false;
	}
	return true;
#else
	(void)StructID;
	(void)PropertyPath;
	(void)OutTargetPath;
	OutError = TEXT("not supported in UE 5.5 (requires 5.6+)");
	return false;
#endif
}

bool TryRemoveStateTreeParameter(FInstancedPropertyBag* Bag, FName ParamName, FString& OutError)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	const EPropertyBagAlterationResult Result = Bag->RemovePropertyByName(ParamName);
	if (Result == EPropertyBagAlterationResult::Success)
	{
		return true;
	}

	switch (Result)
	{
	case EPropertyBagAlterationResult::SourcePropertyNotFound:
	case EPropertyBagAlterationResult::TargetPropertyNotFound:
		OutError = TEXT("property not found");
		break;
	default:
		OutError = TEXT("internal error");
		break;
	}
	return false;
#else
	Bag->RemovePropertyByName(ParamName);
	(void)OutError;
	return true;
#endif
}

} // namespace

// ============================================================================
// Lua Binding
// ============================================================================

static TArray<FLuaFunctionDoc> StateTreeDocs = {};

static void BindStateTree(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_state_tree", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UStateTree* ST = LoadObject<UStateTree>(nullptr, *FPath);
		if (!ST) return;

		UStateTreeEditorData* ED = Cast<UStateTreeEditorData>(ST->EditorData.Get());
		if (!ED) return;

		AssetObj["_help_text"] =
			"Element types for add/remove/list:\n"
			"  state              — hierarchical state (State/Group/Linked/Subtree)\n"
			"  task               — task on a state (e.g. StateTreeDelayTask)\n"
			"  evaluator          — global evaluator\n"
			"  global_task        — global task (runs every tick)\n"
			"  transition         — state transition (trigger + target)\n"
			"  condition          — enter condition on a state\n"
			"  transition_condition — condition on a transition\n"
			"  consideration      — utility AI consideration\n"
			"  binding            — property binding between nodes\n"
			"  parameter          — state or global parameter\n"
			"\n"
			"add(type, params):\n"
			"  add(\"state\", {name=\"Idle\", parent=\"Root\", type=\"State\",\n"
			"    description=\"..\", selection_behavior=\"TrySelectChildrenInOrder\",\n"
			"    tasks_completion=\"Any\", tag=\"AI.State\", weight=1.0, custom_tick_rate=0.5})\n"
			"  add(\"task\", {state=\"Idle\", type=\"StateTreeDelayTask\"})\n"
			"  add(\"evaluator\", {type=\"MyEvaluator\"})\n"
			"  add(\"global_task\", {type=\"MyGlobalTask\"})\n"
			"  add(\"transition\", {state=\"Idle\", trigger=\"OnStateCompleted\", target=\"Patrol\",\n"
			"    priority=\"Normal\", event_tag=\"Tag.Name\", delay=2.0})\n"
			"    triggers: OnStateCompleted, OnStateSucceeded, OnStateFailed, OnTick, OnEvent, OnDelegate\n"
			"  add(\"condition\", {state=\"Idle\", type=\"StateTreeCompareBoolCondition\"})\n"
			"  add(\"transition_condition\", {state=\"Idle\", transition_index=0, type=\"...\"})\n"
			"  add(\"consideration\", {state=\"Idle\", type=\"MyConsideration\"})\n"
			"  add(\"binding\", {source_state=\"..\", source_node_type=\"task\", source_index=0,\n"
			"       source_property=\"Duration\", target_state=\"..\", target_node_type=\"task\",\n"
			"       target_index=1, target_property=\"Delay\"})\n"
			"  add(\"parameter\", {state=\"\" (empty=global), name=\"Health\", type=\"Float\", value=\"100\"})\n"
			"\n"
			"remove(type, id):\n"
			"  remove(\"state\", \"Idle\")\n"
			"  remove(\"task\", {state=\"Idle\", index=0})\n"
			"  remove(\"evaluator\", 0)  — by global index\n"
			"  remove(\"global_task\", 0)\n"
			"  remove(\"transition\", {state=\"Idle\", index=0})\n"
			"  remove(\"condition\", {state=\"Idle\", index=0})\n"
			"  remove(\"transition_condition\", {state=\"Idle\", transition_index=0, condition_index=0})\n"
			"  remove(\"consideration\", {state=\"Idle\", index=0})\n"
			"  remove(\"binding\", {state=\"Idle\", node_type=\"task\", index=0, property=\"Delay\"})\n"
			"  remove(\"parameter\", {state=\"\" (empty=global), name=\"Health\"})\n"
			"\n"
			"list(type, opts?):\n"
			"  list(\"states\"), list(\"evaluators\"), list(\"global_tasks\")\n"
			"  list(\"parameters\")                    — global parameters\n"
			"  list(\"parameters\", {state=\"Idle\"})    — state parameters\n"
			"  list(\"task_types\"), list(\"condition_types\"), list(\"evaluator_types\")\n"
			"  list(\"consideration_types\"), list(\"schemas\")\n"
			"  list(\"tasks\", {state=\"Idle\"})       — per-state task listing\n"
			"  list(\"transitions\", {state=\"Idle\"})  — per-state transition listing\n"
			"  list(\"enter_conditions\", {state=\"Idle\"}) — per-state enter conditions\n"
			"  list(\"considerations\", {state=\"Idle\"})   — per-state considerations\n"
			"\n"
			"configure(type, params):\n"
			"  configure(\"state\", {name=\"Idle\", enabled=true, description=\"..\",\n"
			"    selection_behavior=\"TrySelectChildrenInOrder\", tasks_completion=\"Any\",\n"
			"    tag=\"AI.State.Idle\", weight=1.0, custom_tick_rate=0.5,\n"
			"    linked_asset=\"/Game/Path/ToTree\", linked_subtree=\"SubtreeName\",\n"
			"    rename=\"NewName\", required_event=\"Tag.Name\" (or \"none\" to clear),\n"
			"    check_prerequisites_for_child=true})\n"
			"  configure(\"transition\", {state=\"Idle\", index=0, transition_enabled=true,\n"
			"    delay=2.0, delay_random_variance=0.5, priority=\"High\",\n"
			"    trigger=\"OnStateCompleted\", target=\"StateName\", event_tag=\"Tag.Name\"})\n"
			"  configure(\"task\", {state=\"Idle\", index=0, Duration=5.0})  — set task properties\n"
			"  configure(\"condition\", {state=\"Combat\", index=0, Invert=true})  — set condition properties\n"
			"  configure(\"global_tasks_completion\", {value=\"Any\"})  — Any or All\n"
			"\n"
			"Action methods:\n"
			"  compile()         — compile StateTree, returns {success, message}\n"
			"  info()            — summary of states, tasks, transitions\n"
			"  set_properties({state=\"..\", node_type=\"task\", index=0, properties={Duration=5.0}})\n"
			"  set_schema(\"StateTreeComponentSchema\")\n"
			"  move_state({name=\"Idle\", parent=\"Root\", index=0})  — reparent/reorder state\n";

		// ---- add(type, params) ----
		AssetObj.set_function("add", [ST, ED, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("state"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"state\") -> {name=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string Name = P.get_or<std::string>("name", "");
				if (Name.empty()) { Session.Log(TEXT("[FAIL] add(\"state\") -> name required")); return sol::lua_nil; }

				FString FName = UTF8_TO_TCHAR(Name.c_str());
				if (FindStateInEditor(ED, FName)) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"state\") -> '%s' already exists"), *FName)); return sol::lua_nil; }

				std::string ParentStr = P.get_or<std::string>("parent", "");
				std::string TypeStr = P.get_or<std::string>("type", "State");
				std::string SelStr = P.get_or<std::string>("selection_behavior", "TrySelectChildrenInOrder");
				bool bEnabled = P.get_or("enabled", true);

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddSTState", "Add StateTree State"));
				ED->Modify();

				FString FParent = UTF8_TO_TCHAR(ParentStr.c_str());
				UObject* Outer = static_cast<UObject*>(ED);
				UStateTreeState* ParentState = nullptr;
				if (!FParent.IsEmpty())
				{
					ParentState = FindStateInEditor(ED, FParent);
					if (!ParentState) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"state\") -> parent '%s' not found"), *FParent)); return sol::lua_nil; }
					Outer = ParentState;
				}

				UStateTreeState* NewState = NewObject<UStateTreeState>(Outer, ::NAME_None, RF_Transactional);
				NewState->Name = ::FName(*FName);
				NewState->Type = ParseStateType(UTF8_TO_TCHAR(TypeStr.c_str()));
				NewState->SelectionBehavior = ParseSelBehavior(UTF8_TO_TCHAR(SelStr.c_str()));
				NewState->bEnabled = bEnabled;
				NewState->ID = FGuid::NewGuid();

				std::string TagStr = P.get_or<std::string>("tag", "");
				if (!TagStr.empty())
					NewState->Tag = FGameplayTag::RequestGameplayTag(::FName(UTF8_TO_TCHAR(TagStr.c_str())), false);

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
				std::string DescStr = P.get_or<std::string>("description", "");
				if (!DescStr.empty())
					NewState->Description = UTF8_TO_TCHAR(DescStr.c_str());

				std::string TasksCompStr = P.get_or<std::string>("tasks_completion", "");
				if (!TasksCompStr.empty())
					NewState->TasksCompletion = ParseTasksCompletion(UTF8_TO_TCHAR(TasksCompStr.c_str()));
#endif

				double WeightVal = P.get_or("weight", 0.0);
				if (WeightVal > 0.0)
					NewState->Weight = static_cast<float>(WeightVal);

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
				double CustomTickVal = P.get_or("custom_tick_rate", 0.0);
				if (CustomTickVal > 0.0)
				{
					NewState->bHasCustomTickRate = true;
					NewState->CustomTickRate = static_cast<float>(CustomTickVal);
				}
#endif

				std::string LinkedAssetStr = P.get_or<std::string>("linked_asset", "");
				if (!LinkedAssetStr.empty())
				{
					FString FLinkedAsset = UTF8_TO_TCHAR(LinkedAssetStr.c_str());
					UStateTree* LinkedAsset = LoadObject<UStateTree>(nullptr, *FLinkedAsset);
					if (LinkedAsset)
					{
						NewState->Type = EStateTreeStateType::LinkedAsset;
						NewState->SetLinkedStateAsset(LinkedAsset);
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] add(\"state\") -> linked_asset '%s' not found, skipping"), *FLinkedAsset));
					}
				}

				if (ParentState)
				{
					ParentState->Modify();
					NewState->Parent = ParentState;
					ParentState->Children.Add(NewState);
				}
				else
				{
					ED->SubTrees.Add(NewState);
				}

				ST->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"state\", name=\"%s\", parent=\"%s\")"), *FName, *FParent));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("task"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"task\") -> {state=.., type=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string StateName = P.get_or<std::string>("state", "");
				std::string NodeType = P.get_or<std::string>("type", "");
				if (StateName.empty() || NodeType.empty()) { Session.Log(TEXT("[FAIL] add(\"task\") -> state and type required")); return sol::lua_nil; }

				FString FState = UTF8_TO_TCHAR(StateName.c_str());
				UStateTreeState* State = FindStateInEditor(ED, FState);
				if (!State) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"task\") -> state '%s' not found"), *FState)); return sol::lua_nil; }

				FString FNodeType = UTF8_TO_TCHAR(NodeType.c_str());
				const UScriptStruct* Struct = FindSTNodeStruct(FNodeType);
				if (!Struct) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"task\") -> type '%s' not found"), *FNodeType)); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddSTTask", "Add StateTree Task"));
				ED->Modify();
				State->Modify();
				FStateTreeEditorNode& Node = State->Tasks.AddDefaulted_GetRef();
				InitEditorNode(Node, Struct);
				ST->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add(\"task\", state=\"%s\", type=\"%s\")"), *FState, *Struct->GetName()));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("evaluator"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"evaluator\") -> {type=..} required")); return sol::lua_nil; }
				std::string NodeType = Params.value().get_or<std::string>("type", "");
				if (NodeType.empty()) { Session.Log(TEXT("[FAIL] add(\"evaluator\") -> type required")); return sol::lua_nil; }

				FString FNodeType = UTF8_TO_TCHAR(NodeType.c_str());
				const UScriptStruct* Struct = FindSTNodeStruct(FNodeType);
				if (!Struct) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"evaluator\") -> type '%s' not found"), *FNodeType)); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddSTEval", "Add StateTree Evaluator"));
				ED->Modify();
				FStateTreeEditorNode& Node = ED->Evaluators.AddDefaulted_GetRef();
				InitEditorNode(Node, Struct);
				ST->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add(\"evaluator\", type=\"%s\", index=%d)"), *Struct->GetName(), ED->Evaluators.Num() - 1));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("global_task"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"global_task\") -> {type=..} required")); return sol::lua_nil; }
				std::string NodeType = Params.value().get_or<std::string>("type", "");
				if (NodeType.empty()) { Session.Log(TEXT("[FAIL] add(\"global_task\") -> type required")); return sol::lua_nil; }

				FString FNodeType = UTF8_TO_TCHAR(NodeType.c_str());
				const UScriptStruct* Struct = FindSTNodeStruct(FNodeType);
				if (!Struct) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"global_task\") -> type '%s' not found"), *FNodeType)); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddSTGTask", "Add StateTree Global Task"));
				ED->Modify();
				FStateTreeEditorNode& Node = ED->GlobalTasks.AddDefaulted_GetRef();
				InitEditorNode(Node, Struct);
				ST->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add(\"global_task\", type=\"%s\", index=%d)"), *Struct->GetName(), ED->GlobalTasks.Num() - 1));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("transition"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"transition\") -> {state=.., trigger=.., target=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string StateName = P.get_or<std::string>("state", "");
				if (StateName.empty()) { Session.Log(TEXT("[FAIL] add(\"transition\") -> state required")); return sol::lua_nil; }

				FString FState = UTF8_TO_TCHAR(StateName.c_str());
				UStateTreeState* State = FindStateInEditor(ED, FState);
				if (!State) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"transition\") -> state '%s' not found"), *FState)); return sol::lua_nil; }

				std::string TriggerStr = P.get_or<std::string>("trigger", "OnStateCompleted");
				std::string TargetStr = P.get_or<std::string>("target", "");
				std::string PriorityStr = P.get_or<std::string>("priority", "Normal");
				std::string TypeStr = P.get_or<std::string>("type", "");

				EStateTreeTransitionTrigger Trigger = ParseTrigger(UTF8_TO_TCHAR(TriggerStr.c_str()));
				// Determine transition type: explicit "type" param, or infer from target
				// (if target is a state name → GotoState, if empty and no type → GotoState default)
				EStateTreeTransitionType TransType = !TypeStr.empty()
					? ParseTransType(UTF8_TO_TCHAR(TypeStr.c_str()))
					: (!TargetStr.empty() ? EStateTreeTransitionType::GotoState : EStateTreeTransitionType::GotoState);
				EStateTreeTransitionPriority Priority = ParsePriority(UTF8_TO_TCHAR(PriorityStr.c_str()));

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddSTTrans", "Add StateTree Transition"));
				ED->Modify();
				State->Modify();
				FStateTreeTransition& Trans = State->Transitions.AddDefaulted_GetRef();
				Trans.ID = FGuid::NewGuid();
				Trans.Trigger = Trigger;
				Trans.Priority = Priority;
				Trans.State.LinkType = TransType;

				if (TransType == EStateTreeTransitionType::GotoState && !TargetStr.empty())
				{
					FString FTarget = UTF8_TO_TCHAR(TargetStr.c_str());
					UStateTreeState* Target = FindStateInEditor(ED, FTarget);
					if (Target)
					{
						Trans.State.ID = Target->ID;
						Trans.State.Name = Target->Name;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"transition\") -> target '%s' not found"), *FTarget));
						return sol::lua_nil;
					}
				}

				// Event tag
				if (Trigger == EStateTreeTransitionTrigger::OnEvent)
				{
					std::string EventTag = P.get_or<std::string>("event_tag", "");
					if (EventTag.empty()) { Session.Log(TEXT("[FAIL] add(\"transition\") -> OnEvent trigger requires event_tag")); return sol::lua_nil; }
					Trans.RequiredEvent.Tag = FGameplayTag::RequestGameplayTag(::FName(UTF8_TO_TCHAR(EventTag.c_str())), false);
				}

				// Delay
				double Delay = P.get_or("delay", 0.0);
				if (Delay > 0.0)
				{
					Trans.bDelayTransition = true;
					Trans.DelayDuration = static_cast<float>(Delay);
				}

				ST->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"transition\", state=\"%s\", trigger=\"%s\", target=\"%s\")"),
					*FState, UTF8_TO_TCHAR(TriggerStr.c_str()), UTF8_TO_TCHAR(TargetStr.c_str())));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("condition"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"condition\") -> {state=.., type=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string StateName = P.get_or<std::string>("state", "");
				std::string NodeType = P.get_or<std::string>("type", "");
				if (StateName.empty() || NodeType.empty()) { Session.Log(TEXT("[FAIL] add(\"condition\") -> state and type required")); return sol::lua_nil; }

				FString FState = UTF8_TO_TCHAR(StateName.c_str());
				UStateTreeState* State = FindStateInEditor(ED, FState);
				if (!State) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"condition\") -> state '%s' not found"), *FState)); return sol::lua_nil; }

				FString FNodeType = UTF8_TO_TCHAR(NodeType.c_str());
				const UScriptStruct* Struct = FindSTNodeStruct(FNodeType);
				if (!Struct) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"condition\") -> type '%s' not found"), *FNodeType)); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddSTCond", "Add StateTree Condition"));
				ED->Modify();
				State->Modify();
				FStateTreeEditorNode& Node = State->EnterConditions.AddDefaulted_GetRef();
				InitEditorNode(Node, Struct);
				ST->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add(\"condition\", state=\"%s\", type=\"%s\")"), *FState, *Struct->GetName()));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("transition_condition"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"transition_condition\") -> {state=.., transition_index=.., type=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string StateName = P.get_or<std::string>("state", "");
				std::string NodeType = P.get_or<std::string>("type", "");
				int32 TransIdx = P.get_or("transition_index", -1);
				if (StateName.empty() || NodeType.empty() || TransIdx < 0) { Session.Log(TEXT("[FAIL] add(\"transition_condition\") -> state, transition_index, and type required")); return sol::lua_nil; }

				FString FState = UTF8_TO_TCHAR(StateName.c_str());
				UStateTreeState* State = FindStateInEditor(ED, FState);
				if (!State) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"transition_condition\") -> state '%s' not found"), *FState)); return sol::lua_nil; }

				if (TransIdx >= State->Transitions.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"transition_condition\") -> transition_index %d out of range (%d)"), TransIdx, State->Transitions.Num()));
					return sol::lua_nil;
				}

				FString FNodeType = UTF8_TO_TCHAR(NodeType.c_str());
				const UScriptStruct* Struct = FindSTNodeStruct(FNodeType);
				if (!Struct) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"transition_condition\") -> type '%s' not found"), *FNodeType)); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddSTTransCond", "Add StateTree Transition Condition"));
				ED->Modify();
				State->Modify();
				FStateTreeTransition& Trans = State->Transitions[TransIdx];
				FStateTreeEditorNode& Node = Trans.Conditions.AddDefaulted_GetRef();
				InitEditorNode(Node, Struct);
				ST->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add(\"transition_condition\", state=\"%s\", transition_index=%d, type=\"%s\")"), *FState, TransIdx, *Struct->GetName()));
				return sol::make_object(Lua, true);
			}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			else if (FType.Equals(TEXT("consideration"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"consideration\") -> {state=.., type=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string StateName = P.get_or<std::string>("state", "");
				std::string NodeType = P.get_or<std::string>("type", "");
				if (StateName.empty() || NodeType.empty()) { Session.Log(TEXT("[FAIL] add(\"consideration\") -> state and type required")); return sol::lua_nil; }

				FString FState = UTF8_TO_TCHAR(StateName.c_str());
				UStateTreeState* State = FindStateInEditor(ED, FState);
				if (!State) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"consideration\") -> state '%s' not found"), *FState)); return sol::lua_nil; }

				FString FNodeType = UTF8_TO_TCHAR(NodeType.c_str());
				const UScriptStruct* Struct = FindSTNodeStruct(FNodeType);
				if (!Struct) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"consideration\") -> type '%s' not found"), *FNodeType)); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddSTCons", "Add StateTree Consideration"));
				ED->Modify();
				State->Modify();
				FStateTreeEditorNode& Node = State->Considerations.AddDefaulted_GetRef();
				InitEditorNode(Node, Struct);
				ST->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add(\"consideration\", state=\"%s\", type=\"%s\")"), *FState, *Struct->GetName()));
				return sol::make_object(Lua, true);
			}
#endif // ENGINE_MINOR_VERSION >= 5
			else if (FType.Equals(TEXT("binding"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"binding\") -> {source_state=.., source_node_type=.., source_index=.., source_property=.., target_state=.., target_node_type=.., target_index=.., target_property=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string SrcState = P.get_or<std::string>("source_state", "");
				std::string SrcNodeType = P.get_or<std::string>("source_node_type", "");
				int32 SrcIndex = P.get_or("source_index", 0);
				std::string SrcProperty = P.get_or<std::string>("source_property", "");
				std::string TgtState = P.get_or<std::string>("target_state", "");
				std::string TgtNodeType = P.get_or<std::string>("target_node_type", "");
				int32 TgtIndex = P.get_or("target_index", 0);
				std::string TgtProperty = P.get_or<std::string>("target_property", "");

				if (SrcNodeType.empty() || SrcProperty.empty() || TgtNodeType.empty() || TgtProperty.empty())
				{
					Session.Log(TEXT("[FAIL] add(\"binding\") -> source_node_type, source_property, target_node_type, target_property required"));
					return sol::lua_nil;
				}

				FString FSrcState = UTF8_TO_TCHAR(SrcState.c_str());
				FString FSrcNodeType = UTF8_TO_TCHAR(SrcNodeType.c_str());
				FString FSrcProp = UTF8_TO_TCHAR(SrcProperty.c_str());
				FString FTgtState = UTF8_TO_TCHAR(TgtState.c_str());
				FString FTgtNodeType = UTF8_TO_TCHAR(TgtNodeType.c_str());
				FString FTgtProp = UTF8_TO_TCHAR(TgtProperty.c_str());

				FStateTreeEditorNode* SourceNode = FindEditorNodeInST(ED, FSrcState, FSrcNodeType, SrcIndex);
				if (!SourceNode)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"binding\") -> source node not found (%s %s[%d])"), *FSrcState, *FSrcNodeType, SrcIndex));
					return sol::lua_nil;
				}

				FStateTreeEditorNode* TargetNode = FindEditorNodeInST(ED, FTgtState, FTgtNodeType, TgtIndex);
				if (!TargetNode)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"binding\") -> target node not found (%s %s[%d])"), *FTgtState, *FTgtNodeType, TgtIndex));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddSTBinding", "Add StateTree Property Binding"));
				ED->Modify();

				bool bSuccess = ED->AddPropertyBinding(*SourceNode, FSrcProp, *TargetNode, FTgtProp);
				if (bSuccess)
				{
					ST->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] add(\"binding\", %s[%d].%s -> %s[%d].%s)"),
						*FSrcNodeType, SrcIndex, *FSrcProp, *FTgtNodeType, TgtIndex, *FTgtProp));
					return sol::make_object(Lua, true);
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"binding\") -> failed to bind %s.%s -> %s.%s (check property paths are valid)"),
						*FSrcNodeType, *FSrcProp, *FTgtNodeType, *FTgtProp));
					return sol::lua_nil;
				}
			}
			else if (FType.Equals(TEXT("parameter"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"parameter\") -> {name=.., type=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string ParamName = P.get_or<std::string>("name", "");
				std::string ParamType = P.get_or<std::string>("type", "");
				std::string ParamValue = P.get_or<std::string>("value", "");
				std::string StateName = P.get_or<std::string>("state", "");

				if (ParamName.empty() || ParamType.empty()) { Session.Log(TEXT("[FAIL] add(\"parameter\") -> name and type required")); return sol::lua_nil; }

				FString FParamName = UTF8_TO_TCHAR(ParamName.c_str());
				FString FParamType = UTF8_TO_TCHAR(ParamType.c_str());
				FString FParamValue = UTF8_TO_TCHAR(ParamValue.c_str());
				FString FStateName = UTF8_TO_TCHAR(StateName.c_str());

				EPropertyBagPropertyType PropType = ParsePropertyBagType(FParamType);
				if (PropType == EPropertyBagPropertyType::None)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"parameter\") -> unknown type '%s'. Use: Bool, Byte, Int32, Int64, Float, Double, Name, String, Text, UInt32, UInt64"), *FParamType));
					return sol::lua_nil;
				}

				FString BagError;
				FInstancedPropertyBag* Bag = GetParameterBag(ED, FStateName, BagError);
				if (!Bag)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"parameter\") -> %s"), *BagError));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddSTParam", "Add StateTree Parameter"));
				ED->Modify();

				FString ParameterError;
				if (!TryAddStateTreeParameter(Bag, FName(*FParamName), PropType, ParameterError))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"parameter\") -> failed to add '%s' (%s)"), *FParamName, *ParameterError));
					return sol::lua_nil;
				}

				if (!FParamValue.IsEmpty())
				{
					SetParameterValueOnBag(Bag, FParamName, PropType, FParamValue);
				}

				ST->MarkPackageDirty();
				FString Location = FStateName.IsEmpty() ? TEXT("global") : FString::Printf(TEXT("state %s"), *FStateName);
				Session.Log(FString::Printf(TEXT("[OK] add(\"parameter\", name=\"%s\", type=\"%s\", location=%s)"), *FParamName, *FParamType, *Location));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: state, task, evaluator, global_task, transition, condition, transition_condition, consideration, binding, parameter"), *FType));
			return sol::lua_nil;
		});

		// ---- remove(type, id) ----
		AssetObj.set_function("remove", [ST, ED, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("state"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"state\") -> name required")); return sol::lua_nil; }
				FString Name = UTF8_TO_TCHAR(Id.as<std::string>().c_str());

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemSTState", "Remove StateTree State"));
				ED->Modify();

				// Check root subtrees first
				for (int32 i = ED->SubTrees.Num() - 1; i >= 0; i--)
				{
					if (ED->SubTrees[i] && ED->SubTrees[i]->Name.ToString().Equals(Name, ESearchCase::IgnoreCase))
					{
						UStateTreeState* Removed = ED->SubTrees[i];
						ED->SubTrees.RemoveAt(i);
						Removed->Parent = nullptr;
						ST->MarkPackageDirty();
						Session.Log(FString::Printf(TEXT("[OK] remove(\"state\", \"%s\")"), *Name));
						return sol::make_object(Lua, true);
					}
				}

				UStateTreeState* State = FindStateInEditor(ED, Name);
				if (!State) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"state\") -> '%s' not found"), *Name)); return sol::lua_nil; }
				if (State->Parent)
				{
					State->Parent->Modify();
					State->Parent->Children.Remove(State);
					State->Parent = nullptr;
					ST->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] remove(\"state\", \"%s\")"), *Name));
					return sol::make_object(Lua, true);
				}
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"state\") -> '%s' could not be removed"), *Name));
				return sol::lua_nil;
			}
			else if (FType.Equals(TEXT("evaluator"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<int>()) { Session.Log(TEXT("[FAIL] remove(\"evaluator\") -> index required")); return sol::lua_nil; }
				int32 Idx = Id.as<int>();
				if (Idx < 0 || Idx >= ED->Evaluators.Num()) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"evaluator\") -> index %d out of range (%d)"), Idx, ED->Evaluators.Num())); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemSTEval", "Remove StateTree Evaluator"));
				ED->Modify();
				ED->Evaluators.RemoveAt(Idx);
				ST->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"evaluator\", %d)"), Idx));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("global_task"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<int>()) { Session.Log(TEXT("[FAIL] remove(\"global_task\") -> index required")); return sol::lua_nil; }
				int32 Idx = Id.as<int>();
				if (Idx < 0 || Idx >= ED->GlobalTasks.Num()) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"global_task\") -> index %d out of range (%d)"), Idx, ED->GlobalTasks.Num())); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemSTGTask", "Remove StateTree Global Task"));
				ED->Modify();
				ED->GlobalTasks.RemoveAt(Idx);
				ST->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"global_task\", %d)"), Idx));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("transition_condition"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<sol::table>()) { Session.Log(TEXT("[FAIL] remove(\"transition_condition\") -> {state=.., transition_index=.., condition_index=..} required")); return sol::lua_nil; }
				sol::table T = Id.as<sol::table>();
				std::string StateName = T.get_or<std::string>("state", "");
				int32 TransIdx = T.get_or("transition_index", -1);
				int32 CondIdx = T.get_or("condition_index", -1);
				if (StateName.empty() || TransIdx < 0 || CondIdx < 0) { Session.Log(TEXT("[FAIL] remove(\"transition_condition\") -> state, transition_index, and condition_index required")); return sol::lua_nil; }

				FString FState = UTF8_TO_TCHAR(StateName.c_str());
				UStateTreeState* State = FindStateInEditor(ED, FState);
				if (!State) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"transition_condition\") -> state '%s' not found"), *FState)); return sol::lua_nil; }

				if (TransIdx >= State->Transitions.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"transition_condition\") -> transition_index %d out of range (%d)"), TransIdx, State->Transitions.Num()));
					return sol::lua_nil;
				}

				FStateTreeTransition& Trans = State->Transitions[TransIdx];
				if (CondIdx >= Trans.Conditions.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"transition_condition\") -> condition_index %d out of range (%d)"), CondIdx, Trans.Conditions.Num()));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemSTTransCond", "Remove StateTree Transition Condition"));
				ED->Modify();
				Trans.Conditions.RemoveAt(CondIdx);
				ST->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"transition_condition\", state=\"%s\", transition_index=%d, condition_index=%d)"), *FState, TransIdx, CondIdx));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("binding"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<sol::table>()) { Session.Log(TEXT("[FAIL] remove(\"binding\") -> {state=.., node_type=.., index=.., property=..} required")); return sol::lua_nil; }
				sol::table T = Id.as<sol::table>();
				std::string StateName = T.get_or<std::string>("state", "");
				std::string NodeType = T.get_or<std::string>("node_type", "");
				int32 Idx = T.get_or("index", 0);
				std::string Property = T.get_or<std::string>("property", "");

				if (NodeType.empty() || Property.empty()) { Session.Log(TEXT("[FAIL] remove(\"binding\") -> node_type and property required")); return sol::lua_nil; }

				FString FStateName = UTF8_TO_TCHAR(StateName.c_str());
				FString FNodeType = UTF8_TO_TCHAR(NodeType.c_str());
				FString FProperty = UTF8_TO_TCHAR(Property.c_str());

				FStateTreeEditorNode* Node = FindEditorNodeInST(ED, FStateName, FNodeType, Idx);
				if (!Node)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"binding\") -> node not found (%s %s[%d])"), *FStateName, *FNodeType, Idx));
					return sol::lua_nil;
				}

				FStateTreeEditorPropertyBindings* Bindings = ED->GetPropertyEditorBindings();
				if (!Bindings)
				{
					Session.Log(TEXT("[FAIL] remove(\"binding\") -> no property bindings available on this StateTree"));
					return sol::lua_nil;
				}

				FPropertyBindingPath TargetPath;
				FString BindingError;
				if (!BuildStateTreeBindingTargetPath(Node->ID, FProperty, TargetPath, BindingError))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"binding\") -> %s"), *BindingError));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemSTBinding", "Remove StateTree Property Binding"));
				ED->Modify();

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
				bool bHadBinding = Bindings->HasBinding(TargetPath);
				Bindings->RemoveBindings(TargetPath);
#else
				bool bHadBinding = false;
#endif

				if (bHadBinding)
				{
					ST->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] remove(\"binding\", %s[%d].%s)"), *FNodeType, Idx, *FProperty));
					return sol::make_object(Lua, true);
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"binding\") -> no binding found for %s[%d].%s"), *FNodeType, Idx, *FProperty));
					return sol::lua_nil;
				}
			}
			else if (FType.Equals(TEXT("parameter"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<sol::table>()) { Session.Log(TEXT("[FAIL] remove(\"parameter\") -> {name=..} required")); return sol::lua_nil; }
				sol::table T = Id.as<sol::table>();
				std::string ParamName = T.get_or<std::string>("name", "");
				std::string StateName = T.get_or<std::string>("state", "");

				if (ParamName.empty()) { Session.Log(TEXT("[FAIL] remove(\"parameter\") -> name required")); return sol::lua_nil; }

				FString FParamName = UTF8_TO_TCHAR(ParamName.c_str());
				FString FStateName = UTF8_TO_TCHAR(StateName.c_str());

				FString BagError;
				FInstancedPropertyBag* Bag = GetParameterBag(ED, FStateName, BagError);
				if (!Bag)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"parameter\") -> %s"), *BagError));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemSTParam", "Remove StateTree Parameter"));
				ED->Modify();

				FString ParameterError;
				if (!TryRemoveStateTreeParameter(Bag, FName(*FParamName), ParameterError))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"parameter\") -> failed to remove '%s' (%s)"), *FParamName, *ParameterError));
					return sol::lua_nil;
				}

				ST->MarkPackageDirty();
				FString Location = FStateName.IsEmpty() ? TEXT("global") : FString::Printf(TEXT("state %s"), *FStateName);
				Session.Log(FString::Printf(TEXT("[OK] remove(\"parameter\", name=\"%s\", location=%s)"), *FParamName, *Location));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("task"), ESearchCase::IgnoreCase) ||
					 FType.Equals(TEXT("transition"), ESearchCase::IgnoreCase) ||
					 FType.Equals(TEXT("condition"), ESearchCase::IgnoreCase) ||
					 FType.Equals(TEXT("consideration"), ESearchCase::IgnoreCase))
			{
				// These require {state, index} table
				if (!Id.is<sol::table>()) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> {state=.., index=..} required"), *FType)); return sol::lua_nil; }
				sol::table T = Id.as<sol::table>();
				std::string StateName = T.get_or<std::string>("state", "");
				int32 Idx = T.get_or("index", -1);
				if (StateName.empty() || Idx < 0) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> state and index required"), *FType)); return sol::lua_nil; }

				FString FState = UTF8_TO_TCHAR(StateName.c_str());
				UStateTreeState* State = FindStateInEditor(ED, FState);
				if (!State) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> state '%s' not found"), *FType, *FState)); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemSTNode", "Remove StateTree Node"));
				ED->Modify();

				TArray<FStateTreeEditorNode>* Arr = nullptr;
				TArray<FStateTreeTransition>* TransArr = nullptr;

				if (FType.Equals(TEXT("task"), ESearchCase::IgnoreCase)) Arr = &State->Tasks;
				else if (FType.Equals(TEXT("condition"), ESearchCase::IgnoreCase)) Arr = &State->EnterConditions;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				else if (FType.Equals(TEXT("consideration"), ESearchCase::IgnoreCase)) Arr = &State->Considerations;
#endif // ENGINE_MINOR_VERSION >= 5
				else if (FType.Equals(TEXT("transition"), ESearchCase::IgnoreCase)) TransArr = &State->Transitions;

				if (Arr)
				{
					if (Idx >= Arr->Num()) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> index %d out of range (%d)"), *FType, Idx, Arr->Num())); return sol::lua_nil; }
					Arr->RemoveAt(Idx);
				}
				else if (TransArr)
				{
					if (Idx >= TransArr->Num()) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> index %d out of range (%d)"), *FType, Idx, TransArr->Num())); return sol::lua_nil; }
					TransArr->RemoveAt(Idx);
				}

				ST->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"%s\", state=\"%s\", index=%d)"), *FType, *FState, Idx));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type"), *FType));
			return sol::lua_nil;
		});

		// ---- list(type?, opts?) ----
		AssetObj.set_function("list", [ST, ED, &Session](sol::table self,
			sol::optional<std::string> TypeOpt, sol::optional<sol::table> OptsOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			// Extract optional state filter
			FString StateFilter;
			if (OptsOpt.has_value())
			{
				std::string SF = OptsOpt.value().get_or<std::string>("state", "");
				if (!SF.empty()) StateFilter = UTF8_TO_TCHAR(SF.c_str());
			}

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = self["info"];
				if (InfoFn.valid()) return InfoFn(self);
				return sol::lua_nil;
			}

			if (FType.Contains(TEXT("state"), ESearchCase::IgnoreCase) && !FType.Contains(TEXT("type")))
			{
				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (UStateTreeState* Sub : ED->SubTrees)
					CollectStatesRecursive(Sub, Lua, Result, Idx);
				Session.Log(FString::Printf(TEXT("[OK] list(\"states\") -> %d"), Idx - 1));
				return Result;
			}

			if (FType.Contains(TEXT("evaluator"), ESearchCase::IgnoreCase) && !FType.Contains(TEXT("type")))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < ED->Evaluators.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["index"] = i;
					const UScriptStruct* S2 = ED->Evaluators[i].Node.GetScriptStruct();
					E["type"] = S2 ? TCHAR_TO_UTF8(*S2->GetName()) : "Unknown";
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"evaluators\") -> %d"), ED->Evaluators.Num()));
				return Result;
			}

			if (FType.Contains(TEXT("global_task"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < ED->GlobalTasks.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["index"] = i;
					const UScriptStruct* S2 = ED->GlobalTasks[i].Node.GetScriptStruct();
					E["type"] = S2 ? TCHAR_TO_UTF8(*S2->GetName()) : "Unknown";
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"global_tasks\") -> %d"), ED->GlobalTasks.Num()));
				return Result;
			}

			// Schema discovery
			if (FType.Equals(TEXT("schemas"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (TObjectIterator<UClass> It; It; ++It)
				{
					UClass* Class = *It;
					if (!Class->IsChildOf(UStateTreeSchema::StaticClass())) continue;
					if (Class->HasAnyClassFlags(CLASS_Abstract)) continue;
					if (Class == UStateTreeSchema::StaticClass()) continue;

					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*Class->GetName());
					FString DisplayName = Class->GetDisplayNameText().ToString();
					if (!DisplayName.IsEmpty() && !DisplayName.Equals(Class->GetName()))
						E["display_name"] = TCHAR_TO_UTF8(*DisplayName);
					Result[Idx++] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"schemas\") -> %d"), Idx - 1));
				return Result;
			}

			// Type discovery: task_types, condition_types, evaluator_types, consideration_types
			if (FType.Contains(TEXT("type"), ESearchCase::IgnoreCase))
			{
				const UScriptStruct* BaseStruct = nullptr;
				if (FType.Contains(TEXT("task"))) BaseStruct = FStateTreeTaskBase::StaticStruct();
				else if (FType.Contains(TEXT("condition"))) BaseStruct = FStateTreeConditionBase::StaticStruct();
				else if (FType.Contains(TEXT("evaluator"))) BaseStruct = FStateTreeEvaluatorBase::StaticStruct();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				else if (FType.Contains(TEXT("consideration"))) BaseStruct = FStateTreeConsiderationBase::StaticStruct();
#endif // ENGINE_MINOR_VERSION >= 5

				if (!BaseStruct) { Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown discovery type"), *FType)); return sol::lua_nil; }

				const UStateTreeSchema* Schema = ED->Schema;
				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (TObjectIterator<UScriptStruct> It; It; ++It)
				{
					UScriptStruct* Struct = *It;
					if (!Struct->IsChildOf(BaseStruct)) continue;
					if (Struct == BaseStruct || Struct == FStateTreeNodeBase::StaticStruct()) continue;
					if (Struct->HasMetaData(TEXT("Hidden"))) continue;
					if (Schema && !Schema->IsStructAllowed(Struct)) continue;

					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*Struct->GetName());
					FString Desc;
					if (Struct->HasMetaData(TEXT("DisplayName")))
						Desc = Struct->GetMetaData(TEXT("DisplayName"));
					if (!Desc.IsEmpty()) E["display_name"] = TCHAR_TO_UTF8(*Desc);
					Result[Idx++] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"%s\") -> %d types"), *FType, Idx - 1));
				return Result;
			}

			// Per-state listing: tasks, transitions, enter_conditions, considerations
			if (!StateFilter.IsEmpty() &&
				(FType.Equals(TEXT("tasks"), ESearchCase::IgnoreCase) ||
				 FType.Equals(TEXT("transitions"), ESearchCase::IgnoreCase) ||
				 FType.Equals(TEXT("enter_conditions"), ESearchCase::IgnoreCase) ||
				 FType.Equals(TEXT("conditions"), ESearchCase::IgnoreCase) ||
				 FType.Equals(TEXT("considerations"), ESearchCase::IgnoreCase)))
			{
				UStateTreeState* State = FindStateInEditor(ED, StateFilter);
				if (!State)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> state '%s' not found"), *FType, *StateFilter));
					return sol::lua_nil;
				}

				sol::table Result = Lua.create_table();

				if (FType.Equals(TEXT("tasks"), ESearchCase::IgnoreCase))
				{
					for (int32 i = 0; i < State->Tasks.Num(); i++)
					{
						sol::table E = Lua.create_table();
						E["index"] = i;
						const UScriptStruct* S2 = State->Tasks[i].Node.GetScriptStruct();
						E["type"] = S2 ? TCHAR_TO_UTF8(*S2->GetName()) : "Unknown";
						E["id"] = TCHAR_TO_UTF8(*State->Tasks[i].ID.ToString());
						Result[i + 1] = E;
					}
					Session.Log(FString::Printf(TEXT("[OK] list(\"tasks\", state=\"%s\") -> %d"), *StateFilter, State->Tasks.Num()));
				}
				else if (FType.Equals(TEXT("transitions"), ESearchCase::IgnoreCase))
				{
					for (int32 i = 0; i < State->Transitions.Num(); i++)
					{
						sol::table E = Lua.create_table();
						E["index"] = i;
						E["trigger"] = TCHAR_TO_UTF8(*UEnum::GetValueAsString(State->Transitions[i].Trigger));
						E["priority"] = TCHAR_TO_UTF8(*UEnum::GetValueAsString(State->Transitions[i].Priority));
						E["enabled"] = State->Transitions[i].bTransitionEnabled;
						E["has_delay"] = State->Transitions[i].bDelayTransition;
						if (State->Transitions[i].bDelayTransition)
						{
							E["delay_duration"] = State->Transitions[i].DelayDuration;
							E["delay_random_variance"] = State->Transitions[i].DelayRandomVariance;
						}
						E["conditions"] = State->Transitions[i].Conditions.Num();
						E["target_type"] = TCHAR_TO_UTF8(*UEnum::GetValueAsString(State->Transitions[i].State.LinkType));
						if (State->Transitions[i].State.Name != NAME_None)
						{
							FString TargetName = State->Transitions[i].State.Name.ToString();
							E["target_name"] = TCHAR_TO_UTF8(*TargetName);
							E["target"] = TCHAR_TO_UTF8(*TargetName); // alias for convenience
						}
						Result[i + 1] = E;
					}
					Session.Log(FString::Printf(TEXT("[OK] list(\"transitions\", state=\"%s\") -> %d"), *StateFilter, State->Transitions.Num()));
				}
				else if (FType.Equals(TEXT("enter_conditions"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("conditions"), ESearchCase::IgnoreCase))
				{
					for (int32 i = 0; i < State->EnterConditions.Num(); i++)
					{
						sol::table E = Lua.create_table();
						E["index"] = i;
						const UScriptStruct* S2 = State->EnterConditions[i].Node.GetScriptStruct();
						E["type"] = S2 ? TCHAR_TO_UTF8(*S2->GetName()) : "Unknown";
						E["id"] = TCHAR_TO_UTF8(*State->EnterConditions[i].ID.ToString());
						E["operand"] = TCHAR_TO_UTF8(*UEnum::GetValueAsString(State->EnterConditions[i].ExpressionOperand));
						Result[i + 1] = E;
					}
					Session.Log(FString::Printf(TEXT("[OK] list(\"enter_conditions\", state=\"%s\") -> %d"), *StateFilter, State->EnterConditions.Num()));
				}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				else if (FType.Equals(TEXT("considerations"), ESearchCase::IgnoreCase))
				{
					for (int32 i = 0; i < State->Considerations.Num(); i++)
					{
						sol::table E = Lua.create_table();
						E["index"] = i;
						const UScriptStruct* S2 = State->Considerations[i].Node.GetScriptStruct();
						E["type"] = S2 ? TCHAR_TO_UTF8(*S2->GetName()) : "Unknown";
						E["id"] = TCHAR_TO_UTF8(*State->Considerations[i].ID.ToString());
						Result[i + 1] = E;
					}
					Session.Log(FString::Printf(TEXT("[OK] list(\"considerations\", state=\"%s\") -> %d"), *StateFilter, State->Considerations.Num()));
				}
#endif // ENGINE_MINOR_VERSION >= 5

				return Result;
			}

			// Parameter listing
			if (FType.Equals(TEXT("parameters"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("params"), ESearchCase::IgnoreCase))
			{
				FString BagError;
				FInstancedPropertyBag* Bag = GetParameterBag(ED, StateFilter, BagError);
				if (!Bag)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] list(\"parameters\") -> %s"), *BagError));
					return sol::lua_nil;
				}

				sol::table Result = Lua.create_table();
				const UPropertyBag* BagStruct = Bag->GetPropertyBagStruct();
				if (BagStruct)
				{
					int32 Idx = 1;
					for (const FPropertyBagPropertyDesc& Desc : BagStruct->GetPropertyDescs())
					{
						sol::table E = Lua.create_table();
						E["name"] = TCHAR_TO_UTF8(*Desc.Name.ToString());
						E["type"] = TCHAR_TO_UTF8(*UEnum::GetValueAsString(Desc.ValueType));

						// Export current value as string
						const uint8* BagMemory = Bag->GetValue().GetMemory();
						if (BagMemory)
						{
							const FProperty* Prop = BagStruct->FindPropertyByName(Desc.Name);
							if (Prop)
							{
								FString ValueStr;
								const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(BagMemory);
								if (ValuePtr)
								{
									Prop->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, nullptr, PPF_None);
									E["value"] = TCHAR_TO_UTF8(*ValueStr);
								}
							}
						}
						Result[Idx++] = E;
					}
					FString Location = StateFilter.IsEmpty() ? TEXT("global") : FString::Printf(TEXT("state %s"), *StateFilter);
					Session.Log(FString::Printf(TEXT("[OK] list(\"parameters\", %s) -> %d"), *Location, Idx - 1));
				}
				else
				{
					FString Location = StateFilter.IsEmpty() ? TEXT("global") : FString::Printf(TEXT("state %s"), *StateFilter);
					Session.Log(FString::Printf(TEXT("[OK] list(\"parameters\", %s) -> 0 (no parameters defined)"), *Location));
				}
				return Result;
			}

			// ---- list("bindings") ----
			if (FType.Equals(TEXT("bindings"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				const FStateTreeEditorPropertyBindings* Bindings = ED->GetPropertyEditorBindings();
				if (Bindings)
				{
					TConstArrayView<FStateTreePropertyPathBinding> AllBindings = Bindings->GetBindings();
					int32 Idx = 1;
					for (const FStateTreePropertyPathBinding& B : AllBindings)
					{
						sol::table Entry = Lua.create_table();
						Entry["source"] = TCHAR_TO_UTF8(*B.GetSourcePath().ToString());
						Entry["target"] = TCHAR_TO_UTF8(*B.GetTargetPath().ToString());
						Entry["source_struct_id"] = TCHAR_TO_UTF8(*B.GetSourcePath().GetStructID().ToString());
						Entry["target_struct_id"] = TCHAR_TO_UTF8(*B.GetTargetPath().GetStructID().ToString());
						Result[Idx++] = Entry;
					}
					Session.Log(FString::Printf(TEXT("[OK] list(\"bindings\") -> %d property bindings"), AllBindings.Num()));
				}
				else
				{
					Session.Log(TEXT("[OK] list(\"bindings\") -> 0 (no bindings)"));
				}
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: states, evaluators, global_tasks, parameters, bindings, task_types, condition_types, evaluator_types, consideration_types, schemas. With {state=\"..\"}: tasks, transitions, enter_conditions, considerations, parameters"), *FType));
			return sol::lua_nil;
		});

		// ---- set_properties({state=.., node_type=.., index=.., properties={...}}) ----
		AssetObj.set_function("set_properties", [ST, ED, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			std::string NodeType = Params.get_or<std::string>("node_type", "");
			int32 Index = Params.get_or("index", 0);
			std::string StateName = Params.get_or<std::string>("state", "");

			if (NodeType.empty()) { Session.Log(TEXT("[FAIL] set_properties() -> node_type required")); return sol::lua_nil; }

			sol::optional<sol::table> PropsOpt = Params.get<sol::optional<sol::table>>("properties");
			if (!PropsOpt.has_value()) { Session.Log(TEXT("[FAIL] set_properties() -> properties table required")); return sol::lua_nil; }
			sol::table Props = PropsOpt.value();

			FString FNodeType = UTF8_TO_TCHAR(NodeType.c_str());
			FString FStateName = UTF8_TO_TCHAR(StateName.c_str());

			// Locate the FStateTreeEditorNode
			FStateTreeEditorNode* EditorNode = nullptr;
			FString NodeLocation;

			if (FNodeType.Equals(TEXT("evaluator"), ESearchCase::IgnoreCase))
			{
				if (Index < 0 || Index >= ED->Evaluators.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set_properties() -> evaluator index %d out of range (%d)"), Index, ED->Evaluators.Num()));
					return sol::lua_nil;
				}
				EditorNode = &ED->Evaluators[Index];
				NodeLocation = FString::Printf(TEXT("evaluator[%d]"), Index);
			}
			else if (FNodeType.Equals(TEXT("global_task"), ESearchCase::IgnoreCase))
			{
				if (Index < 0 || Index >= ED->GlobalTasks.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set_properties() -> global_task index %d out of range (%d)"), Index, ED->GlobalTasks.Num()));
					return sol::lua_nil;
				}
				EditorNode = &ED->GlobalTasks[Index];
				NodeLocation = FString::Printf(TEXT("global_task[%d]"), Index);
			}
			else
			{
				if (FStateName.IsEmpty())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set_properties() -> '%s' requires a state name"), *FNodeType));
					return sol::lua_nil;
				}
				UStateTreeState* State = FindStateInEditor(ED, FStateName);
				if (!State)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set_properties() -> state '%s' not found"), *FStateName));
					return sol::lua_nil;
				}

				if (FNodeType.Equals(TEXT("task"), ESearchCase::IgnoreCase))
				{
					if (Index < 0 || Index >= State->Tasks.Num())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] set_properties() -> task index %d out of range (%d)"), Index, State->Tasks.Num()));
						return sol::lua_nil;
					}
					EditorNode = &State->Tasks[Index];
					NodeLocation = FString::Printf(TEXT("task[%d] in state %s"), Index, *FStateName);
				}
				else if (FNodeType.Equals(TEXT("condition"), ESearchCase::IgnoreCase))
				{
					if (Index < 0 || Index >= State->EnterConditions.Num())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] set_properties() -> condition index %d out of range (%d)"), Index, State->EnterConditions.Num()));
						return sol::lua_nil;
					}
					EditorNode = &State->EnterConditions[Index];
					NodeLocation = FString::Printf(TEXT("condition[%d] in state %s"), Index, *FStateName);
				}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				else if (FNodeType.Equals(TEXT("consideration"), ESearchCase::IgnoreCase))
				{
					if (Index < 0 || Index >= State->Considerations.Num())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] set_properties() -> consideration index %d out of range (%d)"), Index, State->Considerations.Num()));
						return sol::lua_nil;
					}
					EditorNode = &State->Considerations[Index];
					NodeLocation = FString::Printf(TEXT("consideration[%d] in state %s"), Index, *FStateName);
				}
#endif // ENGINE_MINOR_VERSION >= 5
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set_properties() -> unknown node_type '%s'. Use: task, condition, evaluator, global_task, consideration"), *FNodeType));
					return sol::lua_nil;
				}
			}

			if (!EditorNode) { Session.Log(TEXT("[FAIL] set_properties() -> could not locate editor node")); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "SetSTProps", "Set StateTree Node Properties"));
			ED->Modify();

			int32 SetCount = 0;
			TArray<FString> PropResults;

			for (auto& [Key, Value] : Props)
			{
				if (!Key.is<std::string>()) continue;
				std::string KeyStr = Key.as<std::string>();
				FString PropName = UTF8_TO_TCHAR(KeyStr.c_str());

				// Convert sol value to string representation
				FString TextValue;
				if (Value.is<std::string>())
				{
					TextValue = UTF8_TO_TCHAR(Value.as<std::string>().c_str());
				}
				else if (Value.is<double>())
				{
					double NumValue = Value.as<double>();
					if (FMath::IsNearlyEqual(NumValue, FMath::RoundToDouble(NumValue)))
						TextValue = FString::Printf(TEXT("%d"), static_cast<int64>(NumValue));
					else
						TextValue = FString::SanitizeFloat(NumValue);
				}
				else if (Value.is<bool>())
				{
					TextValue = Value.as<bool>() ? TEXT("true") : TEXT("false");
				}
				else
				{
					PropResults.Add(FString::Printf(TEXT("  ! %s: unsupported value type"), *PropName));
					continue;
				}

				// Special handling for ExpressionOperand on the editor node itself
				if (PropName.Equals(TEXT("operand"), ESearchCase::IgnoreCase) ||
					PropName.Equals(TEXT("expression_operand"), ESearchCase::IgnoreCase) ||
					PropName.Equals(TEXT("ExpressionOperand"), ESearchCase::IgnoreCase))
				{
					if (TextValue.Equals(TEXT("And"), ESearchCase::IgnoreCase))
					{
						EditorNode->ExpressionOperand = EStateTreeExpressionOperand::And;
						PropResults.Add(TEXT("  = ExpressionOperand -> And"));
						SetCount++;
					}
					else if (TextValue.Equals(TEXT("Or"), ESearchCase::IgnoreCase))
					{
						EditorNode->ExpressionOperand = EStateTreeExpressionOperand::Or;
						PropResults.Add(TEXT("  = ExpressionOperand -> Or"));
						SetCount++;
					}
					else if (TextValue.Equals(TEXT("Copy"), ESearchCase::IgnoreCase))
					{
						EditorNode->ExpressionOperand = EStateTreeExpressionOperand::Copy;
						PropResults.Add(TEXT("  = ExpressionOperand -> Copy"));
						SetCount++;
					}
					else
					{
						PropResults.Add(FString::Printf(TEXT("  ! ExpressionOperand: invalid value '%s' (use And, Or, Copy)"), *TextValue));
					}
					continue;
				}

				// Try Node struct first, then Instance struct
				bool bSet = false;
				FString SetResult;

				if (EditorNode->Node.IsValid())
				{
					bSet = TrySetPropertyOnStructInstance(EditorNode->Node.GetScriptStruct(), EditorNode->Node.GetMutableMemory(), PropName, TextValue, SetResult);
				}

				if (!bSet && EditorNode->Instance.IsValid())
				{
					bSet = TrySetPropertyOnStructInstance(EditorNode->Instance.GetScriptStruct(), EditorNode->Instance.GetMutableMemory(), PropName, TextValue, SetResult);
				}

				// Try InstanceObject (UObject instance data)
				if (!bSet && EditorNode->InstanceObject)
				{
					FProperty* Property = nullptr;
					for (TFieldIterator<FProperty> It(EditorNode->InstanceObject->GetClass()); It; ++It)
					{
						if ((*It)->GetName().Equals(PropName, ESearchCase::IgnoreCase))
						{
							Property = *It;
							break;
						}
					}
					if (Property)
					{
						void* ValuePtr = Property->ContainerPtrToValuePtr<void>(EditorNode->InstanceObject);
						if (ValuePtr)
						{
							const TCHAR* TextPtr = *TextValue;
							if (Property->ImportText_Direct(TextPtr, ValuePtr, EditorNode->InstanceObject, PPF_None))
							{
								SetResult = FString::Printf(TEXT("  = %s -> %s"), *PropName, *TextValue);
							}
							else
							{
								SetResult = FString::Printf(TEXT("  ! %s: failed to set '%s'"), *PropName, *TextValue);
							}
						}
						bSet = true;
					}
				}

				if (bSet)
				{
					if (!SetResult.IsEmpty())
					{
						PropResults.Add(SetResult);
						if (SetResult.Contains(TEXT("  = "))) SetCount++;
					}
				}
				else
				{
					PropResults.Add(FString::Printf(TEXT("  ! %s: property not found on node or instance"), *PropName));
				}
			}

			if (SetCount > 0)
				ST->MarkPackageDirty();
			FString Output = FString::Printf(TEXT("Properties on %s: %d set"), *NodeLocation, SetCount);
			for (const FString& R : PropResults)
			{
				Output += TEXT("\n") + R;
			}
			Session.Log(FString::Printf(TEXT("[OK] set_properties() -> %s"), *Output));
			return sol::make_object(Lua, true);
		});

		// ---- set_schema(class_name) ----
		AssetObj.set_function("set_schema", [ST, ED, &Session](sol::table /*self*/,
			const std::string& ClassName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FClassName = UTF8_TO_TCHAR(ClassName.c_str());

			FString SearchName = FClassName;
			if (!SearchName.StartsWith(TEXT("U")))
				SearchName = TEXT("U") + SearchName;

			UClass* SchemaClass = nullptr;
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (It->IsChildOf(UStateTreeSchema::StaticClass()) &&
					!It->HasAnyClassFlags(CLASS_Abstract))
				{
					if (It->GetName().Equals(SearchName, ESearchCase::IgnoreCase) ||
						It->GetName().Equals(FClassName, ESearchCase::IgnoreCase))
					{
						SchemaClass = *It;
						break;
					}
				}
			}

			if (!SchemaClass)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_schema(\"%s\") -> unknown schema class"), *FClassName));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "SetSTSchema", "Set StateTree Schema"));
			ED->Modify();

			UStateTreeSchema* NewSchema = NewObject<UStateTreeSchema>(ED, SchemaClass);
			ED->Schema = NewSchema;

			ST->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] set_schema(\"%s\")"), *SchemaClass->GetName()));
			return sol::make_object(Lua, true);
		});

		// ---- configure(type, params) ----
		AssetObj.set_function("configure", [ST, ED, &Session](sol::table /*self*/,
			const std::string& Type, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("state"), ESearchCase::IgnoreCase))
			{
				std::string StateName = Params.get_or<std::string>("name", "");
				if (StateName.empty()) { Session.Log(TEXT("[FAIL] configure(\"state\") -> name required")); return sol::lua_nil; }

				FString FStateName = UTF8_TO_TCHAR(StateName.c_str());
				UStateTreeState* State = FindStateInEditor(ED, FStateName);
				if (!State) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"state\") -> '%s' not found"), *FStateName)); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "ConfigSTState", "Configure StateTree State"));
				ED->Modify();
				State->Modify();

				int32 SetCount = 0;
				TArray<FString> Results;

				sol::optional<bool> EnabledOpt = Params.get<sol::optional<bool>>("enabled");
				if (EnabledOpt.has_value())
				{
					State->bEnabled = EnabledOpt.value();
					Results.Add(FString::Printf(TEXT("  = enabled -> %s"), State->bEnabled ? TEXT("true") : TEXT("false")));
					SetCount++;
				}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
				std::string DescStr = Params.get_or<std::string>("description", "");
				if (!DescStr.empty())
				{
					State->Description = UTF8_TO_TCHAR(DescStr.c_str());
					Results.Add(FString::Printf(TEXT("  = description -> %s"), *State->Description));
					SetCount++;
				}
#endif

				std::string SelStr = Params.get_or<std::string>("selection_behavior", "");
				if (!SelStr.empty())
				{
					State->SelectionBehavior = ParseSelBehavior(UTF8_TO_TCHAR(SelStr.c_str()));
					Results.Add(FString::Printf(TEXT("  = selection_behavior -> %s"), *UEnum::GetValueAsString(State->SelectionBehavior)));
					SetCount++;
				}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
				std::string TasksCompStr = Params.get_or<std::string>("tasks_completion", "");
				if (!TasksCompStr.empty())
				{
					State->TasksCompletion = ParseTasksCompletion(UTF8_TO_TCHAR(TasksCompStr.c_str()));
					Results.Add(FString::Printf(TEXT("  = tasks_completion -> %s"), *UEnum::GetValueAsString(State->TasksCompletion)));
					SetCount++;
				}
#endif

				std::string TagStr = Params.get_or<std::string>("tag", "");
				if (!TagStr.empty())
				{
					State->Tag = FGameplayTag::RequestGameplayTag(::FName(UTF8_TO_TCHAR(TagStr.c_str())), false);
					Results.Add(FString::Printf(TEXT("  = tag -> %s"), *State->Tag.ToString()));
					SetCount++;
				}

				double WeightVal = Params.get_or("weight", -1.0);
				if (WeightVal >= 0.0)
				{
					State->Weight = static_cast<float>(WeightVal);
					Results.Add(FString::Printf(TEXT("  = weight -> %f"), State->Weight));
					SetCount++;
				}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
				double CustomTickVal = Params.get_or("custom_tick_rate", -1.0);
				if (CustomTickVal >= 0.0)
				{
					if (CustomTickVal > 0.0)
					{
						State->bHasCustomTickRate = true;
						State->CustomTickRate = static_cast<float>(CustomTickVal);
						Results.Add(FString::Printf(TEXT("  = custom_tick_rate -> %f"), State->CustomTickRate));
					}
					else
					{
						State->bHasCustomTickRate = false;
						State->CustomTickRate = 0.0f;
						Results.Add(TEXT("  = custom_tick_rate -> disabled"));
					}
					SetCount++;
				}
#endif

				std::string LinkedAssetStr = Params.get_or<std::string>("linked_asset", "");
				if (!LinkedAssetStr.empty())
				{
					FString FLinkedAsset = UTF8_TO_TCHAR(LinkedAssetStr.c_str());
					UStateTree* LinkedAsset = LoadObject<UStateTree>(nullptr, *FLinkedAsset);
					if (LinkedAsset)
					{
						State->Type = EStateTreeStateType::LinkedAsset;
						State->SetLinkedStateAsset(LinkedAsset);
						Results.Add(FString::Printf(TEXT("  = linked_asset -> %s"), *FLinkedAsset));
						SetCount++;
					}
					else
					{
						Results.Add(FString::Printf(TEXT("  ! linked_asset: '%s' not found"), *FLinkedAsset));
					}
				}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
				std::string LinkedSubtreeStr = Params.get_or<std::string>("linked_subtree", "");
				if (!LinkedSubtreeStr.empty())
				{
					FString FLinkedSubtree = UTF8_TO_TCHAR(LinkedSubtreeStr.c_str());
					UStateTreeState* LinkedState = FindStateInEditor(ED, FLinkedSubtree);
					if (LinkedState)
					{
						State->Type = EStateTreeStateType::Linked;
						State->SetLinkedState(LinkedState->GetLinkToState());
						Results.Add(FString::Printf(TEXT("  = linked_subtree -> %s"), *FLinkedSubtree));
						SetCount++;
					}
					else
					{
						Results.Add(FString::Printf(TEXT("  ! linked_subtree: state '%s' not found"), *FLinkedSubtree));
					}
				}
#endif

				// Rename state
				std::string RenameStr = Params.get_or<std::string>("rename", "");
				if (!RenameStr.empty())
				{
					FString FNewName = UTF8_TO_TCHAR(RenameStr.c_str());
					State->Name = ::FName(*FNewName);
					Results.Add(FString::Printf(TEXT("  = name -> %s"), *FNewName));
					SetCount++;
				}

				// Required event to enter
				std::string RequiredEventStr = Params.get_or<std::string>("required_event", "");
				if (!RequiredEventStr.empty())
				{
					FString FReqEvent = UTF8_TO_TCHAR(RequiredEventStr.c_str());
					if (FReqEvent.Equals(TEXT("none"), ESearchCase::IgnoreCase) || FReqEvent.Equals(TEXT("clear"), ESearchCase::IgnoreCase))
					{
						State->bHasRequiredEventToEnter = false;
						State->RequiredEventToEnter = FStateTreeEventDesc();
						Results.Add(TEXT("  = required_event -> cleared"));
					}
					else
					{
						State->bHasRequiredEventToEnter = true;
						State->RequiredEventToEnter.Tag = FGameplayTag::RequestGameplayTag(::FName(*FReqEvent), false);
						Results.Add(FString::Printf(TEXT("  = required_event -> %s"), *State->RequiredEventToEnter.Tag.ToString()));
					}
					SetCount++;
				}

				// Check prerequisites when activating child directly
				sol::optional<bool> CheckPrereqOpt = Params.get<sol::optional<bool>>("check_prerequisites_for_child");
				if (CheckPrereqOpt.has_value())
				{
					State->bCheckPrerequisitesWhenActivatingChildDirectly = CheckPrereqOpt.value();
					Results.Add(FString::Printf(TEXT("  = check_prerequisites_for_child -> %s"),
						State->bCheckPrerequisitesWhenActivatingChildDirectly ? TEXT("true") : TEXT("false")));
					SetCount++;
				}

				ST->MarkPackageDirty();
				FString Output = FString::Printf(TEXT("State '%s': %d properties set"), *FStateName, SetCount);
				for (const FString& R : Results) Output += TEXT("\n") + R;
				Session.Log(FString::Printf(TEXT("[OK] configure(\"state\") -> %s"), *Output));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("transition"), ESearchCase::IgnoreCase))
			{
				std::string StateName = Params.get_or<std::string>("state", "");
				int32 TransIdx = Params.get_or("index", -1);
				if (StateName.empty() || TransIdx < 0) { Session.Log(TEXT("[FAIL] configure(\"transition\") -> state and index required")); return sol::lua_nil; }

				FString FStateName = UTF8_TO_TCHAR(StateName.c_str());
				UStateTreeState* State = FindStateInEditor(ED, FStateName);
				if (!State) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"transition\") -> state '%s' not found"), *FStateName)); return sol::lua_nil; }

				if (TransIdx >= State->Transitions.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"transition\") -> index %d out of range (%d)"), TransIdx, State->Transitions.Num()));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "ConfigSTTrans", "Configure StateTree Transition"));
				ED->Modify();
				State->Modify();
				FStateTreeTransition& Trans = State->Transitions[TransIdx];

				int32 SetCount = 0;
				TArray<FString> Results;

				sol::optional<bool> EnabledOpt = Params.get<sol::optional<bool>>("transition_enabled");
				if (EnabledOpt.has_value())
				{
					Trans.bTransitionEnabled = EnabledOpt.value();
					Results.Add(FString::Printf(TEXT("  = transition_enabled -> %s"), Trans.bTransitionEnabled ? TEXT("true") : TEXT("false")));
					SetCount++;
				}

				double DelayVariance = Params.get_or("delay_random_variance", -1.0);
				if (DelayVariance >= 0.0)
				{
					Trans.DelayRandomVariance = static_cast<float>(DelayVariance);
					Results.Add(FString::Printf(TEXT("  = delay_random_variance -> %f"), Trans.DelayRandomVariance));
					SetCount++;
				}

				double DelayVal = Params.get_or("delay", -1.0);
				if (DelayVal >= 0.0)
				{
					if (DelayVal > 0.0)
					{
						Trans.bDelayTransition = true;
						Trans.DelayDuration = static_cast<float>(DelayVal);
						Results.Add(FString::Printf(TEXT("  = delay -> %f"), Trans.DelayDuration));
					}
					else
					{
						Trans.bDelayTransition = false;
						Trans.DelayDuration = 0.0f;
						Results.Add(TEXT("  = delay -> disabled"));
					}
					SetCount++;
				}

				std::string PriorityStr = Params.get_or<std::string>("priority", "");
				if (!PriorityStr.empty())
				{
					Trans.Priority = ParsePriority(UTF8_TO_TCHAR(PriorityStr.c_str()));
					Results.Add(FString::Printf(TEXT("  = priority -> %s"), *UEnum::GetValueAsString(Trans.Priority)));
					SetCount++;
				}

				// Change trigger
				std::string TriggerStr = Params.get_or<std::string>("trigger", "");
				if (!TriggerStr.empty())
				{
					Trans.Trigger = ParseTrigger(UTF8_TO_TCHAR(TriggerStr.c_str()));
					Results.Add(FString::Printf(TEXT("  = trigger -> %s"), *UEnum::GetValueAsString(Trans.Trigger)));
					SetCount++;
				}

				// Change target
				std::string TargetStr = Params.get_or<std::string>("target", "");
				if (!TargetStr.empty())
				{
					FString FTarget = UTF8_TO_TCHAR(TargetStr.c_str());
					EStateTreeTransitionType TransType = ParseTransType(FTarget);
					Trans.State.LinkType = TransType;
					if (TransType == EStateTreeTransitionType::GotoState)
					{
						UStateTreeState* TargetState = FindStateInEditor(ED, FTarget);
						if (TargetState)
						{
							Trans.State.ID = TargetState->ID;
							Trans.State.Name = TargetState->Name;
							Results.Add(FString::Printf(TEXT("  = target -> %s"), *FTarget));
							SetCount++;
						}
						else
						{
							Results.Add(FString::Printf(TEXT("  ! target: state '%s' not found"), *FTarget));
						}
					}
					else
					{
						Trans.State.ID = FGuid();
						Trans.State.Name = NAME_None;
						Results.Add(FString::Printf(TEXT("  = target -> %s"), *UEnum::GetValueAsString(TransType)));
						SetCount++;
					}
				}

				// Change event tag
				std::string EventTagStr = Params.get_or<std::string>("event_tag", "");
				if (!EventTagStr.empty())
				{
					Trans.RequiredEvent.Tag = FGameplayTag::RequestGameplayTag(::FName(UTF8_TO_TCHAR(EventTagStr.c_str())), false);
					Results.Add(FString::Printf(TEXT("  = event_tag -> %s"), *Trans.RequiredEvent.Tag.ToString()));
					SetCount++;
				}

				ST->MarkPackageDirty();
				FString Output = FString::Printf(TEXT("Transition[%d] in '%s': %d properties set"), TransIdx, *FStateName, SetCount);
				for (const FString& R : Results) Output += TEXT("\n") + R;
				Session.Log(FString::Printf(TEXT("[OK] configure(\"transition\") -> %s"), *Output));
				return sol::make_object(Lua, true);
			}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			else if (FType.Equals(TEXT("global_tasks_completion"), ESearchCase::IgnoreCase))
			{
				std::string CompStr = Params.get_or<std::string>("value", "");
				if (CompStr.empty()) CompStr = Params.get_or<std::string>("completion", "");
				if (CompStr.empty()) { Session.Log(TEXT("[FAIL] configure(\"global_tasks_completion\") -> value required (Any or All)")); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "ConfigSTGTC", "Configure StateTree Global Tasks Completion"));
				ED->Modify();
				ED->GlobalTasksCompletion = ParseTasksCompletion(UTF8_TO_TCHAR(CompStr.c_str()));
				ST->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"global_tasks_completion\") -> %s"), *UEnum::GetValueAsString(ED->GlobalTasksCompletion)));
				return sol::make_object(Lua, true);
			}
#endif

			// ---- configure("task") / configure("condition") / configure("consideration") ----
			// Delegates to the same property-setting logic as set_properties()
			if (FType.Equals(TEXT("task"), ESearchCase::IgnoreCase) ||
				FType.Equals(TEXT("condition"), ESearchCase::IgnoreCase) ||
				FType.Equals(TEXT("consideration"), ESearchCase::IgnoreCase))
			{
				std::string StateName = Params.get_or<std::string>("state", "");
				int32 Index = Params.get_or("index", 0);
				if (StateName.empty())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> state required"), *FType));
					return sol::lua_nil;
				}

				FString FStateName = UTF8_TO_TCHAR(StateName.c_str());
				UStateTreeState* State = FindStateInEditor(ED, FStateName);
				if (!State)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> state '%s' not found"), *FType, *FStateName));
					return sol::lua_nil;
				}

				TArray<FStateTreeEditorNode>* Arr = nullptr;
				if (FType.Equals(TEXT("task"), ESearchCase::IgnoreCase)) Arr = &State->Tasks;
				else if (FType.Equals(TEXT("condition"), ESearchCase::IgnoreCase)) Arr = &State->EnterConditions;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				else Arr = &State->Considerations;
#endif // ENGINE_MINOR_VERSION >= 5

				if (Index < 0 || Index >= Arr->Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> index %d out of range (%d)"), *FType, Index, Arr->Num()));
					return sol::lua_nil;
				}

				FStateTreeEditorNode& EditorNode = (*Arr)[Index];
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "ConfigSTNode", "Configure StateTree Node"));
				ED->Modify();
				State->Modify();

				int32 SetCount = 0;
				TArray<FString> PropResults;

				for (auto& [Key, Value] : Params)
				{
					if (!Key.is<std::string>()) continue;
					std::string KeyStr = Key.as<std::string>();
					// Skip control params
					if (KeyStr == "state" || KeyStr == "index") continue;

					FString PropName = UTF8_TO_TCHAR(KeyStr.c_str());
					FString TextValue;
					if (Value.is<std::string>()) TextValue = UTF8_TO_TCHAR(Value.as<std::string>().c_str());
					else if (Value.is<bool>()) TextValue = Value.as<bool>() ? TEXT("true") : TEXT("false");
					else if (Value.is<double>())
					{
						double D = Value.as<double>();
						if (FMath::IsNearlyEqual(D, FMath::RoundToDouble(D)))
							TextValue = FString::Printf(TEXT("%d"), static_cast<int64>(D));
						else
							TextValue = FString::SanitizeFloat(D);
					}
					else continue;

					// Handle ExpressionOperand specially
					if (PropName.Equals(TEXT("operand"), ESearchCase::IgnoreCase) ||
						PropName.Equals(TEXT("ExpressionOperand"), ESearchCase::IgnoreCase))
					{
						if (TextValue.Equals(TEXT("And"), ESearchCase::IgnoreCase))
							EditorNode.ExpressionOperand = EStateTreeExpressionOperand::And;
						else if (TextValue.Equals(TEXT("Or"), ESearchCase::IgnoreCase))
							EditorNode.ExpressionOperand = EStateTreeExpressionOperand::Or;
						else if (TextValue.Equals(TEXT("Copy"), ESearchCase::IgnoreCase))
							EditorNode.ExpressionOperand = EStateTreeExpressionOperand::Copy;
						PropResults.Add(FString::Printf(TEXT("  = ExpressionOperand -> %s"), *TextValue));
						SetCount++;
						continue;
					}

					bool bSet = false;
					FString SetResult;
					if (EditorNode.Node.IsValid())
						bSet = TrySetPropertyOnStructInstance(EditorNode.Node.GetScriptStruct(), EditorNode.Node.GetMutableMemory(), PropName, TextValue, SetResult);
					if (!bSet && EditorNode.Instance.IsValid())
						bSet = TrySetPropertyOnStructInstance(EditorNode.Instance.GetScriptStruct(), EditorNode.Instance.GetMutableMemory(), PropName, TextValue, SetResult);
					if (!bSet && EditorNode.InstanceObject)
					{
						FProperty* Property = nullptr;
						for (TFieldIterator<FProperty> It(EditorNode.InstanceObject->GetClass()); It; ++It)
						{
							if ((*It)->GetName().Equals(PropName, ESearchCase::IgnoreCase))
							{ Property = *It; break; }
						}
						if (Property)
						{
							void* ValuePtr = Property->ContainerPtrToValuePtr<void>(EditorNode.InstanceObject);
							if (ValuePtr && Property->ImportText_Direct(*TextValue, ValuePtr, EditorNode.InstanceObject, PPF_None))
								SetResult = FString::Printf(TEXT("  = %s -> %s"), *PropName, *TextValue);
							else
								SetResult = FString::Printf(TEXT("  ! %s: failed to set '%s'"), *PropName, *TextValue);
							bSet = true;
						}
					}

					if (bSet)
					{
						if (!SetResult.IsEmpty())
						{
							PropResults.Add(SetResult);
							if (SetResult.Contains(TEXT("  = "))) SetCount++;
						}
					}
					else
					{
						PropResults.Add(FString::Printf(TEXT("  ! %s: property not found"), *PropName));
					}
				}

				ST->MarkPackageDirty();
				FString Output = FString::Join(PropResults, TEXT("\n"));
				Session.Log(FString::Printf(TEXT("[OK] configure(\"%s\", state=\"%s\", index=%d) -> %d properties set\n%s"),
					*FType, *FStateName, Index, SetCount, *Output));
				return sol::make_object(Lua, SetCount > 0);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: state, transition, task, condition, consideration, global_tasks_completion"), *FType));
			return sol::lua_nil;
		});

		// ---- move_state({name=.., parent=.., index=..}) ----
		AssetObj.set_function("move_state", [ST, ED, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			std::string NameStr = Params.get_or<std::string>("name", "");
			if (NameStr.empty()) { Session.Log(TEXT("[FAIL] move_state() -> name required")); return sol::lua_nil; }

			FString FNameStr = UTF8_TO_TCHAR(NameStr.c_str());
			UStateTreeState* State = FindStateInEditor(ED, FNameStr);
			if (!State) { Session.Log(FString::Printf(TEXT("[FAIL] move_state() -> state '%s' not found"), *FNameStr)); return sol::lua_nil; }

			std::string ParentStr = Params.get_or<std::string>("parent", "");
			int32 InsertIdx = Params.get_or("index", -1);

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "MoveSTState", "Move StateTree State"));
			ED->Modify();

			// Remove from current location
			if (State->Parent)
			{
				State->Parent->Modify();
				State->Parent->Children.Remove(State);
			}
			else
			{
				ED->SubTrees.Remove(State);
			}

			FString FParent = UTF8_TO_TCHAR(ParentStr.c_str());
			if (!FParent.IsEmpty())
			{
				UStateTreeState* NewParent = FindStateInEditor(ED, FParent);
				if (!NewParent)
				{
					// Restore to root if new parent not found
					ED->SubTrees.Add(State);
					State->Parent = nullptr;
					Session.Log(FString::Printf(TEXT("[FAIL] move_state() -> new parent '%s' not found, state restored to root"), *FParent));
					return sol::lua_nil;
				}
				NewParent->Modify();
				State->Parent = NewParent;
				if (InsertIdx >= 0 && InsertIdx < NewParent->Children.Num())
					NewParent->Children.Insert(State, InsertIdx);
				else
					NewParent->Children.Add(State);
			}
			else
			{
				// Move to root level
				State->Parent = nullptr;
				if (InsertIdx >= 0 && InsertIdx < ED->SubTrees.Num())
					ED->SubTrees.Insert(State, InsertIdx);
				else
					ED->SubTrees.Add(State);
			}

			ST->MarkPackageDirty();
			FString Location = FParent.IsEmpty() ? TEXT("root") : FString::Printf(TEXT("under '%s'"), *FParent);
			Session.Log(FString::Printf(TEXT("[OK] move_state(\"%s\") -> moved %s%s"),
				*FNameStr, *Location,
				InsertIdx >= 0 ? *FString::Printf(TEXT(" at index %d"), InsertIdx) : TEXT("")));
			return sol::make_object(Lua, true);
		});

		// ---- compile() ----
		AssetObj.set_function("compile", [ST, ED, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			ST->Modify();
			ED->Modify();

			FString Msg;
			bool bOK = CompileST(ST, Msg);

			sol::table Result = Lua.create_table();
			Result["success"] = bOK;
			Result["message"] = TCHAR_TO_UTF8(*Msg);

			Session.Log(FString::Printf(TEXT("[%s] compile() -> %s"), bOK ? TEXT("OK") : TEXT("FAIL"), *Msg));
			return Result;
		});

		// ---- info() ----
		AssetObj.set_function("info", [ST, ED, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();

			// Count states
			int32 TotalStates = 0;
			int32 TotalTasks = 0;
			int32 TotalTransitions = 0;
			auto CountRecursive = [&](auto& Self, UStateTreeState* State) -> void
			{
				if (!State) return;
				TotalStates++;
				TotalTasks += State->Tasks.Num();
				TotalTransitions += State->Transitions.Num();
				for (UStateTreeState* Child : State->Children)
					Self(Self, Child);
			};
			for (UStateTreeState* Sub : ED->SubTrees)
				CountRecursive(CountRecursive, Sub);

			Result["states"] = TotalStates;
			Result["tasks"] = TotalTasks;
			Result["transitions"] = TotalTransitions;
			Result["evaluators"] = ED->Evaluators.Num();
			Result["global_tasks"] = ED->GlobalTasks.Num();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			Result["global_tasks_completion"] = TCHAR_TO_UTF8(*UEnum::GetValueAsString(ED->GlobalTasksCompletion));
#endif
			Result["subtrees"] = ED->SubTrees.Num();

			if (ED->Schema)
				Result["schema"] = TCHAR_TO_UTF8(*ED->Schema->GetClass()->GetName());

			Session.Log(FString::Printf(TEXT("[OK] info() -> %d states, %d tasks, %d transitions, %d evaluators"),
				TotalStates, TotalTasks, TotalTransitions, ED->Evaluators.Num()));
			return Result;
		});
	});
}

#else // ENGINE_MINOR_VERSION < 5

static TArray<FLuaFunctionDoc> StateTreeDocs = {};

#endif // ENGINE_MINOR_VERSION >= 5

static void StateTree_TryBind(sol::state& Lua, FLuaSessionData& Session)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	BindStateTree(Lua, Session);
#else
	Session.Log(TEXT("[WARN] StateTree editing requires UE 5.5+. Binding not available in this engine version."));
#endif
}

REGISTER_LUA_BINDING(StateTree, StateTreeDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("StateTreeModule")))
	{
		Session.Log(TEXT("[WARN] StateTree plugin is not loaded. Enable it in Edit > Plugins to use this feature."));
		return;
	}
	StateTree_TryBind(Lua, Session);
});
