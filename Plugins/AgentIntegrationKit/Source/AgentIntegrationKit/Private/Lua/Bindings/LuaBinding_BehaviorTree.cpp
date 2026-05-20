#include "Lua/LuaBindingRegistry.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
#include "BehaviorTree/Blackboard/BlackboardKeyType_Struct.h"
#endif
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_SubtreeTask.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTree/Composites/BTComposite_SimpleParallel.h"
#include "BehaviorTree/Tasks/BTTask_RunBehavior.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "BehaviorTreeEditorTypes.h"
#include "AIGraphNode.h"
#include "AIGraphTypes.h"
#include "UObject/UnrealType.h"
#include "ScopedTransaction.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Framework/Application/SlateApplication.h"
#include "RenderingThread.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

namespace
{

UClass* FindBBKeyTypeClass(const FString& TypeName)
{
	if (TypeName.Equals(TEXT("Bool"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("Boolean"), ESearchCase::IgnoreCase))
		return UBlackboardKeyType_Bool::StaticClass();
	if (TypeName.Equals(TEXT("Int"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("Integer"), ESearchCase::IgnoreCase))
		return UBlackboardKeyType_Int::StaticClass();
	if (TypeName.Equals(TEXT("Float"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("Double"), ESearchCase::IgnoreCase))
		return UBlackboardKeyType_Float::StaticClass();
	if (TypeName.Equals(TEXT("String"), ESearchCase::IgnoreCase))
		return UBlackboardKeyType_String::StaticClass();
	if (TypeName.Equals(TEXT("Name"), ESearchCase::IgnoreCase))
		return UBlackboardKeyType_Name::StaticClass();
	if (TypeName.Equals(TEXT("Vector"), ESearchCase::IgnoreCase))
		return UBlackboardKeyType_Vector::StaticClass();
	if (TypeName.Equals(TEXT("Rotator"), ESearchCase::IgnoreCase))
		return UBlackboardKeyType_Rotator::StaticClass();
	if (TypeName.Equals(TEXT("Object"), ESearchCase::IgnoreCase))
		return UBlackboardKeyType_Object::StaticClass();
	if (TypeName.Equals(TEXT("Class"), ESearchCase::IgnoreCase))
		return UBlackboardKeyType_Class::StaticClass();
	if (TypeName.Equals(TEXT("Enum"), ESearchCase::IgnoreCase))
		return UBlackboardKeyType_Enum::StaticClass();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	if (TypeName.Equals(TEXT("Struct"), ESearchCase::IgnoreCase))
		return UBlackboardKeyType_Struct::StaticClass();
#endif // ENGINE_MINOR_VERSION >= 5
	return nullptr;
}

UBTCompositeNode* FindCompositeByNameRecursive(UBTCompositeNode* Root, const FString& Name, int32 Depth = 0)
{
	if (!Root) return nullptr;
	if (Depth > 64)
	{
		UE_LOG(LogTemp, Warning, TEXT("FindCompositeByNameRecursive: max depth (64) exceeded, aborting recursion"));
		return nullptr;
	}
	if (Root->GetNodeName().Equals(Name, ESearchCase::IgnoreCase)) return Root;
	for (int32 i = 0; i < Root->GetChildrenNum(); i++)
	{
		if (Root->Children[i].ChildComposite)
		{
			UBTCompositeNode* Found = FindCompositeByNameRecursive(Root->Children[i].ChildComposite, Name, Depth + 1);
			if (Found) return Found;
		}
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// BuildGuidMap — walk BT->BTGraph to map runtime node instances to graph GUIDs and positions
// ---------------------------------------------------------------------------
void BuildGuidMap(UBehaviorTree* BT, TMap<UObject*, FGuid>& OutGuidMap, TMap<UObject*, FVector2D>& OutPosMap)
{
	if (!BT || !BT->BTGraph) return;

	UBehaviorTreeGraph* BTGraph = Cast<UBehaviorTreeGraph>(BT->BTGraph);
	if (!BTGraph) return;

	for (UEdGraphNode* Node : BTGraph->Nodes)
	{
		UAIGraphNode* AINode = Cast<UAIGraphNode>(Node);
		if (!AINode) continue;

		if (AINode->NodeInstance)
		{
			OutGuidMap.Add(AINode->NodeInstance, AINode->NodeGuid);
			OutPosMap.Add(AINode->NodeInstance, FVector2D(AINode->NodePosX, AINode->NodePosY));
		}

		// Sub-nodes (decorators/services) — use parent position since they don't have independent positions
		for (UAIGraphNode* SubNode : AINode->SubNodes)
		{
			if (SubNode && SubNode->NodeInstance)
			{
				OutGuidMap.Add(SubNode->NodeInstance, SubNode->NodeGuid);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// GetShortGuid — first 8 chars of GUID from map
// ---------------------------------------------------------------------------
FString GetShortGuid(const UObject* RuntimeNode, const TMap<UObject*, FGuid>& GuidMap)
{
	if (!RuntimeNode) return TEXT("");
	const FGuid* Found = GuidMap.Find(const_cast<UObject*>(RuntimeNode));
	if (Found) return Found->ToString().Left(8);
	return TEXT("");
}

// ---------------------------------------------------------------------------
// ReconstructDecoratorLogicStr — postfix stack -> "And(A, B)" expression string
// ---------------------------------------------------------------------------
FString ReconstructDecoratorLogicStr(const TArray<UBTDecorator*>& Decorators, const TArray<FBTDecoratorLogic>& DecoratorOps)
{
	if (DecoratorOps.Num() == 0) return TEXT("");
	if (Decorators.Num() <= 1 && DecoratorOps.Num() <= 1) return TEXT("");

	TArray<FString> Stack;
	for (const FBTDecoratorLogic& Op : DecoratorOps)
	{
		switch (Op.Operation)
		{
		case EBTDecoratorLogic::Test:
		{
			int32 Idx = Op.Number;
			if (Decorators.IsValidIndex(Idx) && Decorators[Idx])
			{
				FString DecName = Decorators[Idx]->GetClass()->GetName();
				DecName.RemoveFromStart(TEXT("BTDecorator_"));
				Stack.Push(DecName);
			}
			else
			{
				Stack.Push(FString::Printf(TEXT("Test[%d]"), Idx));
			}
			break;
		}
		case EBTDecoratorLogic::And:
		case EBTDecoratorLogic::Or:
		{
			FString OpName = (Op.Operation == EBTDecoratorLogic::And) ? TEXT("And") : TEXT("Or");
			int32 ChildCount = Op.Number;
			if (ChildCount <= 0 || ChildCount > Stack.Num())
			{
				Stack.Push(FString::Printf(TEXT("%s(?)"), *OpName));
				break;
			}
			TArray<FString> Children;
			for (int32 c = 0; c < ChildCount; c++)
			{
				Children.Insert(Stack.Pop(), 0);
			}
			Stack.Push(FString::Printf(TEXT("%s(%s)"), *OpName, *FString::Join(Children, TEXT(", "))));
			break;
		}
		case EBTDecoratorLogic::Not:
		{
			if (Stack.Num() > 0)
			{
				FString Child = Stack.Pop();
				Stack.Push(FString::Printf(TEXT("Not(%s)"), *Child));
			}
			else
			{
				Stack.Push(TEXT("Not(?)"));
			}
			break;
		}
		default:
			break;
		}
	}

	if (Stack.Num() == 1) return Stack[0];
	if (Stack.Num() > 1) return FString::Printf(TEXT("And(%s)"), *FString::Join(Stack, TEXT(", ")));
	return TEXT("");
}

static void ApplyBlackboardEntryEditorFields(FBlackboardEntry& Entry, const sol::table& Params)
{
#if WITH_EDITORONLY_DATA
	std::string Category = Params.get_or("category", std::string());
	if (!Category.empty())
	{
		Entry.EntryCategory = FName(UTF8_TO_TCHAR(Category.c_str()));
	}

	std::string Description = Params.get_or("description", std::string());
	if (!Description.empty())
	{
		Entry.EntryDescription = UTF8_TO_TCHAR(Description.c_str());
	}
#else
	(void)Entry;
	(void)Params;
#endif
}

static void AddBlackboardEntryEditorFields(sol::table& KeyEntry, const FBlackboardEntry& Entry)
{
#if WITH_EDITORONLY_DATA
	if (!Entry.EntryCategory.IsNone())
	{
		KeyEntry["category"] = TCHAR_TO_UTF8(*Entry.EntryCategory.ToString());
	}
#else
	(void)KeyEntry;
	(void)Entry;
#endif
}

// ---------------------------------------------------------------------------
// BuildKeyEntryTable — build a Lua table from a single FBlackboardEntry
// ---------------------------------------------------------------------------
void BuildKeyEntryTable(sol::state_view& Lua, sol::table& KeyEntry, const FBlackboardEntry& Entry, bool bIncludeInstanceSynced = true)
{
	KeyEntry["name"] = TCHAR_TO_UTF8(*Entry.EntryName.ToString());
	KeyEntry["type"] = Entry.KeyType ? TCHAR_TO_UTF8(*Entry.KeyType->GetClass()->GetName().Replace(TEXT("BlackboardKeyType_"), TEXT(""))) : "Unknown";
	if (bIncludeInstanceSynced) KeyEntry["instance_synced"] = Entry.bInstanceSynced;
	AddBlackboardEntryEditorFields(KeyEntry, Entry);

	if (Entry.KeyType)
	{
		if (const UBlackboardKeyType_Object* ObjKey = Cast<UBlackboardKeyType_Object>(Entry.KeyType))
		{
			KeyEntry["base_class"] = ObjKey->BaseClass ? TCHAR_TO_UTF8(*ObjKey->BaseClass->GetName()) : "None";
		}
		else if (const UBlackboardKeyType_Class* ClsKey = Cast<UBlackboardKeyType_Class>(Entry.KeyType))
		{
			KeyEntry["base_class"] = ClsKey->BaseClass ? TCHAR_TO_UTF8(*ClsKey->BaseClass->GetName()) : "None";
		}
		else if (const UBlackboardKeyType_Enum* EnumKey = Cast<UBlackboardKeyType_Enum>(Entry.KeyType))
		{
			KeyEntry["enum_type"] = EnumKey->EnumType ? TCHAR_TO_UTF8(*EnumKey->EnumType->GetName()) : "None";
			KeyEntry["enum_name"] = TCHAR_TO_UTF8(*EnumKey->EnumName);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			KeyEntry["default_value"] = static_cast<int>(EnumKey->DefaultValue);
#endif
		}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		else if (const UBlackboardKeyType_Struct* StructKey = Cast<UBlackboardKeyType_Struct>(Entry.KeyType))
		{
			if (StructKey->DefaultValue.IsValid())
				KeyEntry["struct_type"] = TCHAR_TO_UTF8(*StructKey->DefaultValue.GetScriptStruct()->GetName());
		}
#endif // ENGINE_MINOR_VERSION >= 5
		else if (const UBlackboardKeyType_Bool* BoolKey = Cast<UBlackboardKeyType_Bool>(Entry.KeyType))
		{
			KeyEntry["default_value"] = BoolKey->bDefaultValue;
		}
		else if (const UBlackboardKeyType_Int* IntKey = Cast<UBlackboardKeyType_Int>(Entry.KeyType))
		{
			KeyEntry["default_value"] = IntKey->DefaultValue;
		}
		else if (const UBlackboardKeyType_Float* FloatKey = Cast<UBlackboardKeyType_Float>(Entry.KeyType))
		{
			KeyEntry["default_value"] = FloatKey->DefaultValue;
		}
		else if (const UBlackboardKeyType_String* StrKey = Cast<UBlackboardKeyType_String>(Entry.KeyType))
		{
			KeyEntry["default_value"] = TCHAR_TO_UTF8(*StrKey->DefaultValue);
		}
		else if (const UBlackboardKeyType_Name* NameKey = Cast<UBlackboardKeyType_Name>(Entry.KeyType))
		{
			KeyEntry["default_value"] = TCHAR_TO_UTF8(*NameKey->DefaultValue.ToString());
		}
		else if (const UBlackboardKeyType_Vector* VecKey = Cast<UBlackboardKeyType_Vector>(Entry.KeyType))
		{
			if (VecKey->bUseDefaultValue)
			{
				sol::table VT = Lua.create_table();
				VT["x"] = VecKey->DefaultValue.X;
				VT["y"] = VecKey->DefaultValue.Y;
				VT["z"] = VecKey->DefaultValue.Z;
				KeyEntry["default_value"] = VT;
			}
		}
		else if (const UBlackboardKeyType_Rotator* RotKey = Cast<UBlackboardKeyType_Rotator>(Entry.KeyType))
		{
			if (RotKey->bUseDefaultValue)
			{
				sol::table RT = Lua.create_table();
				RT["pitch"] = RotKey->DefaultValue.Pitch;
				RT["yaw"] = RotKey->DefaultValue.Yaw;
				RT["roll"] = RotKey->DefaultValue.Roll;
				KeyEntry["default_value"] = RT;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// BuildAllKeysTable — build Lua table of all keys from a blackboard (including parent chain)
// ---------------------------------------------------------------------------
sol::table BuildAllKeysTable(sol::state_view& Lua, UBlackboardData* BB)
{
	sol::table Result = Lua.create_table();
	int32 Idx = 1;

	// Walk parent chain first (inherited keys come first, just like engine does)
	TArray<UBlackboardData*> Chain;
	for (UBlackboardData* Cur = BB; Cur; Cur = Cur->Parent)
	{
		Chain.Insert(Cur, 0);
	}
	for (UBlackboardData* BBData : Chain)
	{
		for (int32 i = 0; i < BBData->Keys.Num(); i++)
		{
			const FBlackboardEntry& Entry = BBData->Keys[i];
			sol::table KeyEntry = Lua.create_table();
			BuildKeyEntryTable(Lua, KeyEntry, Entry);
			if (BBData != BB) KeyEntry["inherited"] = true;
			Result[Idx++] = KeyEntry;
		}
	}
	return Result;
}

// ---------------------------------------------------------------------------
// BuildPropertyTable — CPF_Edit properties on a BT node, skipping UBTNode base
// ---------------------------------------------------------------------------
sol::table BuildPropertyTable(sol::state_view& Lua, UBTNode* Node)
{
	sol::table Props = Lua.create_table();
	if (!Node) return Props;

	UClass* BaseNodeClass = UBTNode::StaticClass();
	int32 Idx = 1;

	for (TFieldIterator<FProperty> PropIt(Node->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Property = *PropIt;
		if (!Property->HasAnyPropertyFlags(CPF_Edit)) continue;
		if (Property->HasAnyPropertyFlags(CPF_Deprecated)) continue;
		if (Property->GetOwnerClass() == BaseNodeClass) continue;
		if (Property->IsA<FDelegateProperty>() || Property->IsA<FMulticastDelegateProperty>()) continue;

		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Node);
		if (!ValuePtr) continue;

		FString PropName = Property->GetName();
		FString PropType = Property->GetCPPType();
		FString ValueStr;

		// FBlackboardKeySelector — show selected key name
		if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
		{
			if (StructProp->Struct && StructProp->Struct->GetName() == TEXT("BlackboardKeySelector"))
			{
				const FBlackboardKeySelector* KeySelector = StructProp->ContainerPtrToValuePtr<FBlackboardKeySelector>(Node);
				if (KeySelector)
				{
					ValueStr = KeySelector->SelectedKeyName.IsNone() ? TEXT("(none)") : KeySelector->SelectedKeyName.ToString();
					PropType = TEXT("BlackboardKeySelector");
				}
			}
		}

		// FEnumProperty
		if (ValueStr.IsEmpty())
		{
			if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
			{
				if (UEnum* Enum = EnumProp->GetEnum())
				{
					FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
					int64 EnumValue = UnderlyingProp->GetSignedIntPropertyValue(
						EnumProp->ContainerPtrToValuePtr<void>(Node));
					ValueStr = Enum->GetNameStringByValue(EnumValue);
					PropType = Enum->GetName();
				}
			}
		}

		// TEnumAsByte (e.g. FlowAbortMode)
		if (ValueStr.IsEmpty())
		{
			if (FByteProperty* ByteProp = CastField<FByteProperty>(Property))
			{
				if (UEnum* Enum = ByteProp->GetIntPropertyEnum())
				{
					uint8 ByteValue = ByteProp->GetPropertyValue_InContainer(Node);
					ValueStr = Enum->GetNameStringByValue(ByteValue);
					PropType = Enum->GetName();
				}
			}
		}

		// Generic export for all other types
		if (ValueStr.IsEmpty())
		{
			Property->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, nullptr, PPF_None);
		}

		// Skip empty/default values
		if (ValueStr.IsEmpty() || ValueStr == TEXT("None") || ValueStr == TEXT("0") || ValueStr == TEXT("0.000000"))
			continue;

		if (ValueStr.Len() > 200) ValueStr = ValueStr.Left(197) + TEXT("...");

		sol::table P = Lua.create_table();
		P["name"] = TCHAR_TO_UTF8(*PropName);
		P["type"] = TCHAR_TO_UTF8(*PropType);
		P["value"] = TCHAR_TO_UTF8(*ValueStr);
		Props[Idx++] = P;
	}
	return Props;
}

// ---------------------------------------------------------------------------
// BuildServiceTable — array of service tables for a node
// ---------------------------------------------------------------------------
sol::table BuildServiceTable(sol::state_view& Lua, const TArray<TObjectPtr<UBTService>>& Services, const TMap<UObject*, FGuid>& GuidMap)
{
	sol::table Result = Lua.create_table();
	int32 Idx = 1;
	for (UBTService* Service : Services)
	{
		if (!Service) continue;
		sol::table S = Lua.create_table();
		S["class"] = TCHAR_TO_UTF8(*Service->GetClass()->GetName());
		S["name"] = TCHAR_TO_UTF8(*Service->GetNodeName());
		FString Guid = GetShortGuid(Service, GuidMap);
		if (!Guid.IsEmpty()) S["guid"] = TCHAR_TO_UTF8(*Guid);
		S["execution_index"] = static_cast<int>(Service->GetExecutionIndex());
		S["static_description"] = TCHAR_TO_UTF8(*Service->GetStaticDescription());
		S["properties"] = BuildPropertyTable(Lua, Service);
		Result[Idx++] = S;
	}
	return Result;
}

// ---------------------------------------------------------------------------
// BuildTaskNodeTable — leaf task node table
// ---------------------------------------------------------------------------
sol::table BuildTaskNodeTable(sol::state_view& Lua, UBTTaskNode* Task,
	const TMap<UObject*, FGuid>& GuidMap, const TMap<UObject*, FVector2D>& PosMap)
{
	sol::table T = Lua.create_table();
	if (!Task) return T;

	T["class"] = TCHAR_TO_UTF8(*Task->GetClass()->GetName());
	T["name"] = TCHAR_TO_UTF8(*Task->GetNodeName());
	FString Guid = GetShortGuid(Task, GuidMap);
	if (!Guid.IsEmpty()) T["guid"] = TCHAR_TO_UTF8(*Guid);

	const FVector2D* Pos = PosMap.Find(Task);
	if (Pos)
	{
		sol::table PosT = Lua.create_table();
		PosT["x"] = Pos->X;
		PosT["y"] = Pos->Y;
		T["position"] = PosT;
	}

	T["execution_index"] = static_cast<int>(Task->GetExecutionIndex());
	T["static_description"] = TCHAR_TO_UTF8(*Task->GetStaticDescription());
	T["properties"] = BuildPropertyTable(Lua, Task);
	T["services"] = BuildServiceTable(Lua, Task->Services, GuidMap);

	return T;
}

// ---------------------------------------------------------------------------
// BuildNodeTable — recursive composite node -> Lua table
// ---------------------------------------------------------------------------
sol::table BuildNodeTable(sol::state_view& Lua, UBTCompositeNode* Node,
	const TMap<UObject*, FGuid>& GuidMap, const TMap<UObject*, FVector2D>& PosMap, int32 Depth = 0)
{
	sol::table T = Lua.create_table();
	if (!Node) return T;
	if (Depth > 64)
	{
		UE_LOG(LogTemp, Warning, TEXT("BuildNodeTable: max depth (64) exceeded, aborting recursion"));
		return T;
	}

	T["class"] = TCHAR_TO_UTF8(*Node->GetClass()->GetName());
	T["name"] = TCHAR_TO_UTF8(*Node->GetNodeName());
	FString Guid = GetShortGuid(Node, GuidMap);
	if (!Guid.IsEmpty()) T["guid"] = TCHAR_TO_UTF8(*Guid);

	const FVector2D* Pos = PosMap.Find(Node);
	if (Pos)
	{
		sol::table PosT = Lua.create_table();
		PosT["x"] = Pos->X;
		PosT["y"] = Pos->Y;
		T["position"] = PosT;
	}

	T["execution_index"] = static_cast<int>(Node->GetExecutionIndex());
	T["static_description"] = TCHAR_TO_UTF8(*Node->GetStaticDescription());
	T["properties"] = BuildPropertyTable(Lua, Node);
	T["services"] = BuildServiceTable(Lua, Node->Services, GuidMap);

	// Children
	sol::table ChildrenTable = Lua.create_table();
	for (int32 i = 0; i < Node->GetChildrenNum(); i++)
	{
		const FBTCompositeChild& Child = Node->Children[i];
		sol::table ChildEntry = Lua.create_table();

		// Decorators on this child link
		sol::table DecsTable = Lua.create_table();
		int32 DecIdx = 1;
		for (UBTDecorator* Decorator : Child.Decorators)
		{
			if (!Decorator) continue;
			sol::table D = Lua.create_table();
			D["class"] = TCHAR_TO_UTF8(*Decorator->GetClass()->GetName());
			D["name"] = TCHAR_TO_UTF8(*Decorator->GetNodeName());
			FString DecGuid = GetShortGuid(Decorator, GuidMap);
			if (!DecGuid.IsEmpty()) D["guid"] = TCHAR_TO_UTF8(*DecGuid);
			D["execution_index"] = static_cast<int>(Decorator->GetExecutionIndex());

			// Flow abort mode
			EBTFlowAbortMode::Type AbortMode = Decorator->GetFlowAbortMode();
			switch (AbortMode)
			{
			case EBTFlowAbortMode::None:          D["flow_abort_mode"] = "None"; break;
			case EBTFlowAbortMode::LowerPriority: D["flow_abort_mode"] = "LowerPriority"; break;
			case EBTFlowAbortMode::Self:           D["flow_abort_mode"] = "Self"; break;
			case EBTFlowAbortMode::Both:           D["flow_abort_mode"] = "Both"; break;
			}
			D["is_inversed"] = Decorator->IsInversed();
			D["static_description"] = TCHAR_TO_UTF8(*Decorator->GetStaticDescription());
			D["properties"] = BuildPropertyTable(Lua, Decorator);
			DecsTable[DecIdx++] = D;
		}
		ChildEntry["decorators"] = DecsTable;

		// Decorator logic expression
		if (Child.DecoratorOps.Num() > 0 && Child.Decorators.Num() > 1)
		{
			// Need to convert TArray<TObjectPtr<UBTDecorator>> to TArray<UBTDecorator*>
			TArray<UBTDecorator*> RawDecs;
			for (const TObjectPtr<UBTDecorator>& DecPtr : Child.Decorators)
			{
				RawDecs.Add(DecPtr.Get());
			}
			FString LogicExpr = ReconstructDecoratorLogicStr(RawDecs, Child.DecoratorOps);
			if (!LogicExpr.IsEmpty())
			{
				ChildEntry["decorator_logic"] = TCHAR_TO_UTF8(*LogicExpr);
			}
		}

		// Child node (composite or task)
		if (Child.ChildComposite)
		{
			ChildEntry["node"] = BuildNodeTable(Lua, Child.ChildComposite, GuidMap, PosMap, Depth + 1);
			ChildEntry["name"] = TCHAR_TO_UTF8(*Child.ChildComposite->GetNodeName());
			ChildEntry["node_type"] = "composite";
			FString CGuid = GetShortGuid(Child.ChildComposite, GuidMap);
			if (!CGuid.IsEmpty()) ChildEntry["guid"] = TCHAR_TO_UTF8(*CGuid);
		}
		else if (Child.ChildTask)
		{
			ChildEntry["node"] = BuildTaskNodeTable(Lua, Child.ChildTask, GuidMap, PosMap);
			ChildEntry["name"] = TCHAR_TO_UTF8(*Child.ChildTask->GetNodeName());
			ChildEntry["node_type"] = "task";
			FString TGuid = GetShortGuid(Child.ChildTask, GuidMap);
			if (!TGuid.IsEmpty()) ChildEntry["guid"] = TCHAR_TO_UTF8(*TGuid);
		}

		ChildrenTable[i + 1] = ChildEntry;
	}
	T["children"] = ChildrenTable;

	return T;
}

// ---------------------------------------------------------------------------
// CountBTNodes — recursive counting
// ---------------------------------------------------------------------------
void CountBTNodes(UBTCompositeNode* Node, int32& OutComposites, int32& OutTasks, int32& OutDecorators, int32& OutServices, int32 Depth = 0)
{
	if (!Node) return;
	if (Depth > 64)
	{
		UE_LOG(LogTemp, Warning, TEXT("CountBTNodes: max depth (64) exceeded, aborting recursion"));
		return;
	}
	OutComposites++;
	OutServices += Node->Services.Num();

	for (int32 i = 0; i < Node->GetChildrenNum(); i++)
	{
		const FBTCompositeChild& Child = Node->Children[i];
		OutDecorators += Child.Decorators.Num();

		if (Child.ChildComposite)
		{
			CountBTNodes(Child.ChildComposite, OutComposites, OutTasks, OutDecorators, OutServices, Depth + 1);
		}
		else if (Child.ChildTask)
		{
			OutTasks++;
			OutServices += Child.ChildTask->Services.Num();
		}
	}
}

// ---------------------------------------------------------------------------
// CollectFlatNodes — collect all nodes into flat array for list("nodes")
// ---------------------------------------------------------------------------
void CollectFlatNodes(sol::state_view& Lua, sol::table& Result, int32& Idx,
	UBTCompositeNode* Node, const TMap<UObject*, FGuid>& GuidMap, int32 Depth = 0)
{
	if (!Node) return;
	if (Depth > 64)
	{
		UE_LOG(LogTemp, Warning, TEXT("CollectFlatNodes: max depth (64) exceeded, aborting recursion"));
		return;
	}

	// Add composite node
	{
		sol::table E = Lua.create_table();
		E["class"] = TCHAR_TO_UTF8(*Node->GetClass()->GetName());
		E["name"] = TCHAR_TO_UTF8(*Node->GetNodeName());
		FString Guid = GetShortGuid(Node, GuidMap);
		if (!Guid.IsEmpty()) E["guid"] = TCHAR_TO_UTF8(*Guid);
		E["node_type"] = "composite";
		E["execution_index"] = static_cast<int>(Node->GetExecutionIndex());
		Result[Idx++] = E;
	}

	// Services on composite
	for (UBTService* Service : Node->Services)
	{
		if (!Service) continue;
		sol::table E = Lua.create_table();
		E["class"] = TCHAR_TO_UTF8(*Service->GetClass()->GetName());
		E["name"] = TCHAR_TO_UTF8(*Service->GetNodeName());
		FString Guid = GetShortGuid(Service, GuidMap);
		if (!Guid.IsEmpty()) E["guid"] = TCHAR_TO_UTF8(*Guid);
		E["node_type"] = "service";
		E["execution_index"] = static_cast<int>(Service->GetExecutionIndex());
		Result[Idx++] = E;
	}

	// Children
	for (int32 i = 0; i < Node->GetChildrenNum(); i++)
	{
		const FBTCompositeChild& Child = Node->Children[i];

		// Decorators on the child link
		for (UBTDecorator* Decorator : Child.Decorators)
		{
			if (!Decorator) continue;
			sol::table E = Lua.create_table();
			E["class"] = TCHAR_TO_UTF8(*Decorator->GetClass()->GetName());
			E["name"] = TCHAR_TO_UTF8(*Decorator->GetNodeName());
			FString Guid = GetShortGuid(Decorator, GuidMap);
			if (!Guid.IsEmpty()) E["guid"] = TCHAR_TO_UTF8(*Guid);
			E["node_type"] = "decorator";
			E["execution_index"] = static_cast<int>(Decorator->GetExecutionIndex());
			Result[Idx++] = E;
		}

		if (Child.ChildComposite)
		{
			CollectFlatNodes(Lua, Result, Idx, Child.ChildComposite, GuidMap, Depth + 1);
		}
		else if (Child.ChildTask)
		{
			sol::table E = Lua.create_table();
			E["class"] = TCHAR_TO_UTF8(*Child.ChildTask->GetClass()->GetName());
			E["name"] = TCHAR_TO_UTF8(*Child.ChildTask->GetNodeName());
			FString Guid = GetShortGuid(Child.ChildTask, GuidMap);
			if (!Guid.IsEmpty()) E["guid"] = TCHAR_TO_UTF8(*Guid);
			E["node_type"] = "task";
			E["execution_index"] = static_cast<int>(Child.ChildTask->GetExecutionIndex());
			Result[Idx++] = E;

			// Services on task
			for (UBTService* Service : Child.ChildTask->Services)
			{
				if (!Service) continue;
				sol::table SE = Lua.create_table();
				SE["class"] = TCHAR_TO_UTF8(*Service->GetClass()->GetName());
				SE["name"] = TCHAR_TO_UTF8(*Service->GetNodeName());
				FString SGuid = GetShortGuid(Service, GuidMap);
				if (!SGuid.IsEmpty()) SE["guid"] = TCHAR_TO_UTF8(*SGuid);
				SE["node_type"] = "service";
				SE["execution_index"] = static_cast<int>(Service->GetExecutionIndex());
				Result[Idx++] = SE;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// FindGraphNodeByGuid — find editor graph node by short GUID prefix
// Searches both top-level nodes AND sub-nodes (decorators/services)
// ---------------------------------------------------------------------------
UBehaviorTreeGraphNode* FindGraphNodeByGuid(UBehaviorTree* BT, const FString& GuidPrefix)
{
	if (!BT || !BT->BTGraph) return nullptr;
	for (UEdGraphNode* Node : BT->BTGraph->Nodes)
	{
		UBehaviorTreeGraphNode* BTNode = Cast<UBehaviorTreeGraphNode>(Node);
		if (!BTNode) continue;
		FString NodeGuid = BTNode->NodeGuid.ToString().Left(GuidPrefix.Len());
		if (NodeGuid.Equals(GuidPrefix, ESearchCase::IgnoreCase))
			return BTNode;
		// Search sub-nodes (decorators/services are stored here, NOT in BTGraph->Nodes)
		for (UAIGraphNode* SubNode : BTNode->SubNodes)
		{
			UBehaviorTreeGraphNode* BTSubNode = Cast<UBehaviorTreeGraphNode>(SubNode);
			if (!BTSubNode) continue;
			FString SubGuid = BTSubNode->NodeGuid.ToString().Left(GuidPrefix.Len());
			if (SubGuid.Equals(GuidPrefix, ESearchCase::IgnoreCase))
				return BTSubNode;
		}
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// FindGraphNodeByRuntimeNode — find graph node that wraps a given runtime node instance
// Searches both top-level nodes AND sub-nodes
// ---------------------------------------------------------------------------
UBehaviorTreeGraphNode* FindGraphNodeByRuntimeNode(UBehaviorTree* BT, UBTNode* RuntimeNode)
{
	if (!BT || !BT->BTGraph || !RuntimeNode) return nullptr;
	for (UEdGraphNode* Node : BT->BTGraph->Nodes)
	{
		UBehaviorTreeGraphNode* BTNode = Cast<UBehaviorTreeGraphNode>(Node);
		if (!BTNode) continue;
		if (BTNode->NodeInstance == RuntimeNode)
			return BTNode;
		for (UAIGraphNode* SubNode : BTNode->SubNodes)
		{
			UBehaviorTreeGraphNode* BTSubNode = Cast<UBehaviorTreeGraphNode>(SubNode);
			if (BTSubNode && BTSubNode->NodeInstance == RuntimeNode)
				return BTSubNode;
		}
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// FindGraphNodeByName — find graph node whose runtime instance matches name
// Searches both top-level nodes AND sub-nodes
// ---------------------------------------------------------------------------
UBehaviorTreeGraphNode* FindGraphNodeByName(UBehaviorTree* BT, const FString& NodeName)
{
	if (!BT || !BT->BTGraph) return nullptr;
	for (UEdGraphNode* Node : BT->BTGraph->Nodes)
	{
		UBehaviorTreeGraphNode* BTNode = Cast<UBehaviorTreeGraphNode>(Node);
		if (!BTNode) continue;

		// Match by runtime node name (composites, tasks)
		if (BTNode->NodeInstance)
		{
			UBTNode* RuntimeNode = Cast<UBTNode>(BTNode->NodeInstance);
			if (RuntimeNode && RuntimeNode->GetNodeName().Equals(NodeName, ESearchCase::IgnoreCase))
				return BTNode;
		}
		else
		{
			// Root node has no NodeInstance — match by node title ("ROOT")
			FString Title = BTNode->GetNodeTitle(ENodeTitleType::MenuTitle).ToString();
			if (Title.Equals(NodeName, ESearchCase::IgnoreCase))
				return BTNode;
		}

		// Search sub-nodes (decorators, services)
		for (UAIGraphNode* SubNode : BTNode->SubNodes)
		{
			UBehaviorTreeGraphNode* BTSubNode = Cast<UBehaviorTreeGraphNode>(SubNode);
			if (!BTSubNode || !BTSubNode->NodeInstance) continue;
			UBTNode* SubRuntime = Cast<UBTNode>(BTSubNode->NodeInstance);
			if (SubRuntime && SubRuntime->GetNodeName().Equals(NodeName, ESearchCase::IgnoreCase))
				return BTSubNode;
		}
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// ResolveParentGraphNode — find parent graph node by guid prefix or name
// ---------------------------------------------------------------------------
UBehaviorTreeGraphNode* ResolveParentGraphNode(UBehaviorTree* BT, const FString& ParentId)
{
	// Try GUID first (hex chars)
	UBehaviorTreeGraphNode* Result = FindGraphNodeByGuid(BT, ParentId);
	if (Result) return Result;
	// Fall back to name
	return FindGraphNodeByName(BT, ParentId);
}

// ---------------------------------------------------------------------------
// EnsureBTEditorOpen — open the BT editor if not already open (needed for class cache)
// ---------------------------------------------------------------------------
void EnsureBTEditorOpen(UBehaviorTree* BT)
{
	if (!BT || !GEditor) return;
	UAssetEditorSubsystem* Sub = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!Sub) return;
	if (Sub->FindEditorForAsset(BT, false)) return;
	Sub->OpenEditorForAsset(BT);
	// Tick Slate instead of sleeping — Sleep blocks the game thread and prevents the editor from actually opening
	for (int32 i = 0; i < 40; ++i)
	{
		if (Sub->FindEditorForAsset(BT, false)) break;
		FSlateApplication::Get().Tick();
		FlushRenderingCommands();
	}
}

// ---------------------------------------------------------------------------
// GatherSubNodeClasses — get available decorator or service classes
// ---------------------------------------------------------------------------
void GatherSubNodeClasses(UBehaviorTree* BT, int32 SubNodeFlags,
	TArray<FGraphNodeClassData>& OutClasses, UClass*& OutGraphNodeClass)
{
	if (!BT || !BT->BTGraph) return;
	const UEdGraphSchema* Schema = BT->BTGraph->GetSchema();
	if (!Schema) return;
	const UAIGraphSchema* AISchema = Cast<UAIGraphSchema>(Schema);
	if (!AISchema) return;
	AISchema->GetSubNodeClasses(SubNodeFlags, OutClasses, OutGraphNodeClass);
}

} // namespace

static TArray<FLuaFunctionDoc> BehaviorTreeDocs = {};

static void BindBehaviorTree(sol::state& Lua, FLuaSessionData& Session)
{
	// Enriches BehaviorTree assets (also works on standalone BlackboardData)
	Lua.set_function("_enrich_behavior_tree", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());

		UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *FPath);
		UBlackboardData* BB = nullptr;
		if (!BT)
		{
			BB = LoadObject<UBlackboardData>(nullptr, *FPath);
			if (!BB) return;
		}

		// Get the blackboard to work with (from BT or standalone)
		UBlackboardData* TargetBB = BB ? BB : (BT ? BT->BlackboardAsset.Get() : nullptr);

		AssetObj["_help_text"] =
			"Element types for add/remove/list:\n"
			"  key — blackboard key (Bool/Int/Float/String/Name/Vector/Rotator/Object/Class/Enum/Struct)\n"
			"  task — BT task node\n"
			"  composite — BT composite node (Selector/Sequence/SimpleParallel)\n"
			"  decorator — BT decorator (attach to composite/task node)\n"
			"  service — BT service (attach to composite/task node)\n"
			"\n"
			"add(type, params):\n"
			"  add(\"key\", {name=\"Health\", type=\"Float\"})  — add blackboard key\n"
			"  add(\"key\", {name=\"Target\", type=\"Object\", base_class=\"Actor\"})  — object key\n"
			"  add(\"task\", {parent=\"guid_or_name\", class=\"BTTask_Wait\"})  — add task node\n"
			"  add(\"composite\", {parent=\"guid_or_name\", class=\"BTComposite_Sequence\"})  — add composite node\n"
			"  add(\"decorator\", {parent=\"guid_or_name\", class=\"BTDecorator_Blackboard\"})  — attach decorator\n"
			"  add(\"service\", {parent=\"guid_or_name\", class=\"BTService_DefaultFocus\"})  — attach service\n"
			"\n"
			"remove(type, id):\n"
			"  remove(\"key\", \"Health\") — remove blackboard key by name\n"
			"  remove(\"node\", \"guid_or_name\") — remove any BT node\n"
			"  remove(\"decorator\", \"guid_or_name\") — remove decorator\n"
			"  remove(\"service\", \"guid_or_name\") — remove service\n"
			"\n"
			"list(type):\n"
			"  list(\"keys\") — list all blackboard keys (includes inherited from parent chain)\n"
			"  list(\"nodes\") — flat array of all BT nodes (composites, tasks, decorators, services)\n"
			"  list(\"task_types\") — available task classes\n"
			"  list(\"composite_types\") — available composite classes\n"
			"  list(\"decorator_types\") — available decorator classes\n"
			"  list(\"service_types\") — available service classes\n"
			"\n"
			"configure(type, id, params):\n"
			"  configure(\"key\", \"Health\", {description=\"HP\"})  — configure blackboard key\n"
			"  configure(\"blackboard\", \"/Game/AI/BB_Enemy\")  — set blackboard asset on BT\n"
			"  configure(\"node\", \"guid_or_name\", {WaitTime=5.0})  — set node properties\n"
			"  configure(\"decorator\", \"guid_or_name\", {FlowAbortMode=\"Both\"})  — set decorator properties\n"
			"\n"
			"Action methods:\n"
			"  reorder_children(parent_name, {\"Child1\", \"Child2\", ...}) — reorder composite children\n"
			"  info() — full recursive tree structure, node properties, blackboard keys, node counts\n";

		// ---- add(type, params) ----
		AssetObj.set_function("add", [BT, TargetBB, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("key"), ESearchCase::IgnoreCase))
			{
				if (!TargetBB) { Session.Log(TEXT("[FAIL] add(\"key\") -> no blackboard available")); return sol::lua_nil; }
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"key\") -> {name=.., type=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string KeyName = P.get_or("name", std::string());
				std::string KeyType = P.get_or("type", std::string());
				if (KeyName.empty() || KeyType.empty()) { Session.Log(TEXT("[FAIL] add(\"key\") -> name and type required")); return sol::lua_nil; }

				FString FKeyName = UTF8_TO_TCHAR(KeyName.c_str());
				FString FKeyType = UTF8_TO_TCHAR(KeyType.c_str());

				// Check duplicate in this BB and parent chain
				FName FKeyFName(*FKeyName);
				for (UBlackboardData* Cur = TargetBB; Cur; Cur = Cur->Parent)
				{
					for (const FBlackboardEntry& Entry : Cur->Keys)
					{
						if (Entry.EntryName.ToString().Equals(FKeyName, ESearchCase::IgnoreCase))
						{
							FString Where = (Cur == TargetBB) ? TEXT("") : FString::Printf(TEXT(" (inherited from %s)"), *Cur->GetName());
							Session.Log(FString::Printf(TEXT("[FAIL] add(\"key\") -> '%s' already exists%s"), *FKeyName, *Where));
							return sol::lua_nil;
						}
					}
				}

				UClass* KeyTypeClass = FindBBKeyTypeClass(FKeyType);
				if (!KeyTypeClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"key\") -> unknown type '%s'. Valid: Bool, Int, Float, String, Name, Vector, Rotator, Object, Class, Enum, Struct"), *FKeyType));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddBBKey", "Add Blackboard Key"));
				TargetBB->SetFlags(RF_Transactional);
				TargetBB->Modify();

				UBlackboardKeyType* KeyTypeInst = NewObject<UBlackboardKeyType>(TargetBB, KeyTypeClass);

				// Configure type-specific properties
				if (UBlackboardKeyType_Object* ObjKey = Cast<UBlackboardKeyType_Object>(KeyTypeInst))
				{
					std::string BaseClassName = P.get_or("base_class", std::string());
					if (!BaseClassName.empty())
					{
						FString FBaseClass = UTF8_TO_TCHAR(BaseClassName.c_str());
						UClass* Cls = UClass::TryFindTypeSlow<UClass>(*FBaseClass, EFindFirstObjectOptions::ExactClass);
						if (!Cls) Cls = StaticLoadClass(UObject::StaticClass(), nullptr, *FBaseClass);
						if (Cls) ObjKey->BaseClass = Cls;
						else Session.Log(FString::Printf(TEXT("[WARN] add(\"key\") -> base_class '%s' not found, defaulting to UObject"), *FBaseClass));
					}
					if (!ObjKey->BaseClass) ObjKey->BaseClass = UObject::StaticClass();
				}
				else if (UBlackboardKeyType_Class* ClsKey = Cast<UBlackboardKeyType_Class>(KeyTypeInst))
				{
					std::string BaseClassName = P.get_or("base_class", std::string());
					if (!BaseClassName.empty())
					{
						FString FBaseClass = UTF8_TO_TCHAR(BaseClassName.c_str());
						UClass* Cls = UClass::TryFindTypeSlow<UClass>(*FBaseClass, EFindFirstObjectOptions::ExactClass);
						if (!Cls) Cls = StaticLoadClass(UObject::StaticClass(), nullptr, *FBaseClass);
						if (Cls) ClsKey->BaseClass = Cls;
						else Session.Log(FString::Printf(TEXT("[WARN] add(\"key\") -> base_class '%s' not found, defaulting to UObject"), *FBaseClass));
					}
					if (!ClsKey->BaseClass) ClsKey->BaseClass = UObject::StaticClass();
				}
				else if (UBlackboardKeyType_Enum* EnumKey = Cast<UBlackboardKeyType_Enum>(KeyTypeInst))
				{
					std::string EnumTypeName = P.get_or("enum_type", P.get_or("enum_name", std::string()));
					if (!EnumTypeName.empty())
					{
						FString FEnumName = UTF8_TO_TCHAR(EnumTypeName.c_str());
						UEnum* FoundEnum = UClass::TryFindTypeSlow<UEnum>(*FEnumName, EFindFirstObjectOptions::ExactClass);
						if (FoundEnum)
						{
							EnumKey->EnumType = FoundEnum;
							EnumKey->EnumName = FoundEnum->GetName();
							EnumKey->bIsEnumNameValid = true;
						}
						else
						{
							Session.Log(FString::Printf(TEXT("[WARN] add(\"key\") -> enum_type '%s' not found"), *FEnumName));
						}
					}
					sol::optional<int> DefVal = P.get<sol::optional<int>>("default_value");
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
					if (DefVal.has_value()) EnumKey->DefaultValue = static_cast<uint8>(DefVal.value());
#endif
				}
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			else if (UBlackboardKeyType_Struct* StructKey = Cast<UBlackboardKeyType_Struct>(KeyTypeInst))
				{
					std::string StructTypeName = P.get_or<std::string>("struct_type", "");
					if (!StructTypeName.empty())
					{
						FString FStructName = UTF8_TO_TCHAR(StructTypeName.c_str());
						UScriptStruct* FoundStruct = FindFirstObject<UScriptStruct>(*FStructName, EFindFirstObjectOptions::NativeFirst);
						if (!FoundStruct)
							FoundStruct = FindFirstObject<UScriptStruct>(*(TEXT("F") + FStructName), EFindFirstObjectOptions::NativeFirst);
						if (FoundStruct)
						{
							StructKey->DefaultValue.InitializeAs(FoundStruct);
						}
						else
						{
							Session.Log(FString::Printf(TEXT("[WARN] add(\"key\") -> struct_type '%s' not found"), *FStructName));
						}
					}
				}
#endif // ENGINE_MINOR_VERSION >= 5
				else if (UBlackboardKeyType_Bool* BoolKey = Cast<UBlackboardKeyType_Bool>(KeyTypeInst))
				{
					sol::optional<bool> DefVal = P.get<sol::optional<bool>>("default_value");
					if (DefVal.has_value()) BoolKey->bDefaultValue = DefVal.value();
				}
				else if (UBlackboardKeyType_Int* IntKey = Cast<UBlackboardKeyType_Int>(KeyTypeInst))
				{
					sol::optional<int> DefVal = P.get<sol::optional<int>>("default_value");
					if (DefVal.has_value()) IntKey->DefaultValue = DefVal.value();
				}
				else if (UBlackboardKeyType_Float* FloatKey = Cast<UBlackboardKeyType_Float>(KeyTypeInst))
				{
					sol::optional<double> DefVal = P.get<sol::optional<double>>("default_value");
					if (DefVal.has_value()) FloatKey->DefaultValue = static_cast<float>(DefVal.value());
				}
				else if (UBlackboardKeyType_String* StrKey = Cast<UBlackboardKeyType_String>(KeyTypeInst))
				{
					std::string DefVal = P.get_or("default_value", std::string());
					if (!DefVal.empty()) StrKey->DefaultValue = UTF8_TO_TCHAR(DefVal.c_str());
				}
				else if (UBlackboardKeyType_Name* NameKey = Cast<UBlackboardKeyType_Name>(KeyTypeInst))
				{
					std::string DefVal = P.get_or("default_value", std::string());
					if (!DefVal.empty()) NameKey->DefaultValue = FName(UTF8_TO_TCHAR(DefVal.c_str()));
				}
				else if (UBlackboardKeyType_Vector* VecKey = Cast<UBlackboardKeyType_Vector>(KeyTypeInst))
				{
					sol::optional<sol::table> DefVal = P.get<sol::optional<sol::table>>("default_value");
					if (DefVal.has_value())
					{
						sol::table VT = DefVal.value();
						VecKey->DefaultValue = FVector(
							VT.get_or("x", 0.0),
							VT.get_or("y", 0.0),
							VT.get_or("z", 0.0));
						VecKey->bUseDefaultValue = true;
					}
				}
				else if (UBlackboardKeyType_Rotator* RotKey = Cast<UBlackboardKeyType_Rotator>(KeyTypeInst))
				{
					sol::optional<sol::table> DefVal = P.get<sol::optional<sol::table>>("default_value");
					if (DefVal.has_value())
					{
						sol::table RT = DefVal.value();
						RotKey->DefaultValue = FRotator(
							RT.get_or("pitch", 0.0),
							RT.get_or("yaw", 0.0),
							RT.get_or("roll", 0.0));
						RotKey->bUseDefaultValue = true;
					}
				}

				FBlackboardEntry NewEntry;
				NewEntry.EntryName = FName(*FKeyName);
				NewEntry.KeyType = KeyTypeInst;
				NewEntry.bInstanceSynced = P.get_or("instance_synced", false);
				ApplyBlackboardEntryEditorFields(NewEntry, P);

				TargetBB->Keys.Add(NewEntry);

				// Trigger propagation
				FProperty* KeysProp = TargetBB->GetClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys));
				if (KeysProp)
				{
					FPropertyChangedEvent Event(KeysProp, EPropertyChangeType::ArrayAdd);
					TargetBB->PostEditChangeProperty(Event);
				}
				// Refresh BT graph blackboard references if available
				if (BT && BT->BTGraph)
				{
					UBehaviorTreeGraph* BTGraphTyped = Cast<UBehaviorTreeGraph>(BT->BTGraph);
					if (BTGraphTyped) BTGraphTyped->UpdateBlackboardChange();
				}
				TargetBB->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add(\"key\", name=\"%s\", type=\"%s\")"), *FKeyName, *FKeyType));
				return sol::make_object(Lua, true);
			}

			// ---- add("decorator", {parent=..., class=...}) / add("service", {parent=..., class=...}) ----
			bool bIsDecorator = FType.Equals(TEXT("decorator"), ESearchCase::IgnoreCase);
			bool bIsService = FType.Equals(TEXT("service"), ESearchCase::IgnoreCase);
			if ((bIsDecorator || bIsService) && BT)
			{
				if (!Params.has_value())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> {parent=.., class=..} required"), *FType));
					return sol::lua_nil;
				}
				sol::table P = Params.value();
				std::string ParentStr = P.get_or("parent", std::string());
				std::string ClassStr = P.get_or("class", std::string());
				if (ParentStr.empty() || ClassStr.empty())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> parent and class required"), *FType));
					return sol::lua_nil;
				}

				FString FParent = UTF8_TO_TCHAR(ParentStr.c_str());
				FString FClass = UTF8_TO_TCHAR(ClassStr.c_str());

				// Find the parent graph node
				UBehaviorTreeGraphNode* ParentGraphNode = ResolveParentGraphNode(BT, FParent);
				if (!ParentGraphNode)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> parent node \"%s\" not found. Use guid or node name."), *FType, *FParent));
					return sol::lua_nil;
				}

				// Ensure editor is open for class cache
				EnsureBTEditorOpen(BT);

				// Gather available sub-node classes
				int32 SubNodeFlag = bIsDecorator ? ESubNode::Decorator : ESubNode::Service;
				TArray<FGraphNodeClassData> ClassData;
				UClass* GraphNodeClass = nullptr;
				GatherSubNodeClasses(BT, SubNodeFlag, ClassData, GraphNodeClass);

				if (!GraphNodeClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> could not resolve graph node class from schema"), *FType));
					return sol::lua_nil;
				}

				// Find matching class data
				FString FClassLower = FClass.ToLower();
				const FGraphNodeClassData* MatchedClass = nullptr;
				for (const FGraphNodeClassData& CD : ClassData)
				{
					if (CD.IsAbstract() || CD.IsDeprecated()) continue;
					FString ClassName = CD.GetClassName();
					FString ClassNameLower = ClassName.ToLower();
					if (ClassNameLower == FClassLower
						|| ClassNameLower == (TEXT("bt") + FClassLower)
						|| ClassNameLower == (TEXT("btdecorator_") + FClassLower)
						|| ClassNameLower == (TEXT("btservice_") + FClassLower))
					{
						MatchedClass = &CD;
						break;
					}
					// Also match display name
					FString DisplayName = FName::NameToDisplayString(CD.ToString(), false);
					if (DisplayName.ToLower() == FClassLower)
					{
						MatchedClass = &CD;
						break;
					}
				}

				if (!MatchedClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> class \"%s\" not found. Use list(\"%s_types\") to see available types."),
						*FType, *FClass, *FType));
					return sol::lua_nil;
				}

				// Create the sub-node graph node and attach it
				UEdGraph* BTGraph = BT->BTGraph;
				UAIGraphNode* NewSubNode = NewObject<UAIGraphNode>(BTGraph, GraphNodeClass);
				NewSubNode->ClassData = *MatchedClass;

				ParentGraphNode->AddSubNode(NewSubNode, BTGraph);
				BT->MarkPackageDirty();

				// Build result
				sol::table Result = Lua.create_table();
				Result["class"] = TCHAR_TO_UTF8(*MatchedClass->GetClassName());
				Result["name"] = TCHAR_TO_UTF8(*FName::NameToDisplayString(MatchedClass->ToString(), false));
				FString NewGuid = NewSubNode->NodeGuid.ToString().Left(8);
				Result["guid"] = TCHAR_TO_UTF8(*NewGuid);
				Result["parent"] = TCHAR_TO_UTF8(*FParent);

				Session.Log(FString::Printf(TEXT("[OK] add(\"%s\", class=\"%s\", parent=\"%s\") -> guid=%s"),
					*FType, *FClass, *FParent, *NewGuid));
				return Result;
			}

			// ---- add("task", {parent=..., class=...}) / add("composite", {parent=..., class=...}) ----
			bool bIsTask = FType.Equals(TEXT("task"), ESearchCase::IgnoreCase);
			bool bIsComposite = FType.Equals(TEXT("composite"), ESearchCase::IgnoreCase)
				|| FType.Equals(TEXT("node"), ESearchCase::IgnoreCase);
			if ((bIsTask || bIsComposite) && BT)
			{
				if (!Params.has_value())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> {parent=.., class=..} required"), *FType));
					return sol::lua_nil;
				}
				sol::table P = Params.value();
				std::string ParentStr = P.get_or("parent", std::string());
				std::string ClassStr = P.get_or("class", std::string());
				if (ParentStr.empty() || ClassStr.empty())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> parent and class required"), *FType));
					return sol::lua_nil;
				}

				FString FParent = UTF8_TO_TCHAR(ParentStr.c_str());
				FString FClass = UTF8_TO_TCHAR(ClassStr.c_str());

				// Find parent graph node to connect to
				UBehaviorTreeGraphNode* ParentGraphNode = ResolveParentGraphNode(BT, FParent);
				if (!ParentGraphNode)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> parent node \"%s\" not found"), *FType, *FParent));
					return sol::lua_nil;
				}

				EnsureBTEditorOpen(BT);

				// Gather available classes
				FGraphNodeClassHelper ClassHelper(UBTNode::StaticClass());
				TArray<FGraphNodeClassData> ClassData;
				UClass* BaseClass = bIsTask ? UBTTaskNode::StaticClass() : UBTCompositeNode::StaticClass();
				ClassHelper.GatherClasses(BaseClass, ClassData);

				// Find matching class
				FString FClassLower = FClass.ToLower();
				const FGraphNodeClassData* MatchedClass = nullptr;
				for (const FGraphNodeClassData& CD : ClassData)
				{
					if (CD.IsAbstract() || CD.IsDeprecated()) continue;
					FString ClassName = CD.GetClassName();
					FString ClassNameLower = ClassName.ToLower();
					if (ClassNameLower == FClassLower
						|| ClassNameLower == (TEXT("bt") + FClassLower)
						|| ClassNameLower == (TEXT("btcomposite_") + FClassLower)
						|| ClassNameLower == (TEXT("bttasknode_") + FClassLower)
						|| ClassNameLower == (TEXT("bttask_") + FClassLower))
					{
						MatchedClass = &CD;
						break;
					}
					FString DisplayName = FName::NameToDisplayString(CD.ToString(), false);
					if (DisplayName.ToLower() == FClassLower)
					{
						MatchedClass = &CD;
						break;
					}
				}
				if (!MatchedClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> class \"%s\" not found. Use list(\"%s_types\") to see available."),
						*FType, *FClass, bIsTask ? TEXT("task") : TEXT("composite")));
					return sol::lua_nil;
				}

				// Determine the graph node class to use
				UClass* GraphNodeClass = nullptr;
				if (bIsTask)
				{
					// Check for subtree task (same logic as UEdGraphSchema_BehaviorTree::IsNodeSubtreeTask)
					if (MatchedClass->GetClassName() == UBTTask_RunBehavior::StaticClass()->GetName())
						GraphNodeClass = UBehaviorTreeGraphNode_SubtreeTask::StaticClass();
					else
						GraphNodeClass = UBehaviorTreeGraphNode_Task::StaticClass();
				}
				else
				{
					// Check for simple parallel — use dynamic class lookup since the graph node class is not exported
					FString ParallelName = UBTComposite_SimpleParallel::StaticClass()->GetName();
					if (MatchedClass->GetClassName() == ParallelName)
					{
						UClass* SimpleParallelGraphClass = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_SimpleParallel"));
						GraphNodeClass = SimpleParallelGraphClass ? SimpleParallelGraphClass : UBehaviorTreeGraphNode_Composite::StaticClass();
					}
					else
						GraphNodeClass = UBehaviorTreeGraphNode_Composite::StaticClass();
				}

				UEdGraph* BTGraph = BT->BTGraph;
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddBTNode", "Add BT Node"));
				BTGraph->Modify();

				// Create graph node using the engine flow
				UBehaviorTreeGraphNode* NewGraphNode = NewObject<UBehaviorTreeGraphNode>(BTGraph, GraphNodeClass);
				NewGraphNode->ClassData = *MatchedClass;
				NewGraphNode->SetFlags(RF_Transactional);
				NewGraphNode->Rename(nullptr, BTGraph, REN_NonTransactional);
				BTGraph->AddNode(NewGraphNode, true);
				NewGraphNode->CreateNewGuid();
				NewGraphNode->PostPlacedNewNode();
				NewGraphNode->AllocateDefaultPins();

				// Position below parent
				double PosX = P.get_or("x", static_cast<double>(ParentGraphNode->NodePosX));
				double PosY = P.get_or("y", static_cast<double>(ParentGraphNode->NodePosY + 200));
				NewGraphNode->NodePosX = static_cast<int32>(PosX);
				NewGraphNode->NodePosY = static_cast<int32>(PosY);

				// Connect to parent: parent's output pin -> new node's input pin
				UEdGraphPin* ParentOutput = ParentGraphNode->GetOutputPin();
				UEdGraphPin* NewInput = NewGraphNode->GetInputPin();
				if (ParentOutput && NewInput)
				{
					ParentOutput->Modify();
					ParentOutput->LinkedTo.Add(NewInput);
					NewInput->LinkedTo.Add(ParentOutput);
					ParentGraphNode->NodeConnectionListChanged();
					NewGraphNode->NodeConnectionListChanged();
				}

				// Rebuild runtime tree from graph
				UBehaviorTreeGraph* BTGraphTyped = Cast<UBehaviorTreeGraph>(BTGraph);
				if (BTGraphTyped)
				{
					BTGraphTyped->UpdateAsset();
				}
				BT->MarkPackageDirty();

				sol::table Result = Lua.create_table();
				Result["class"] = TCHAR_TO_UTF8(*MatchedClass->GetClassName());
				Result["name"] = TCHAR_TO_UTF8(*FName::NameToDisplayString(MatchedClass->ToString(), false));
				FString NewGuid = NewGraphNode->NodeGuid.ToString().Left(8);
				Result["guid"] = TCHAR_TO_UTF8(*NewGuid);
				Result["parent"] = TCHAR_TO_UTF8(*FParent);

				Session.Log(FString::Printf(TEXT("[OK] add(\"%s\", class=\"%s\", parent=\"%s\") -> guid=%s"),
					*FType, *FClass, *FParent, *NewGuid));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: key, decorator, service, task, composite"), *FType));
			return sol::lua_nil;
		});

		// ---- remove(type, id) ----
		AssetObj.set_function("remove", [BT, TargetBB, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("key"), ESearchCase::IgnoreCase))
			{
				if (!TargetBB) { Session.Log(TEXT("[FAIL] remove(\"key\") -> no blackboard available")); return sol::lua_nil; }
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"key\") -> name required")); return sol::lua_nil; }
				FString KeyName = UTF8_TO_TCHAR(Id.as<std::string>().c_str());

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemBBKey", "Remove Blackboard Key"));
				TargetBB->SetFlags(RF_Transactional);
				TargetBB->Modify();

				for (int32 i = TargetBB->Keys.Num() - 1; i >= 0; i--)
				{
					if (TargetBB->Keys[i].EntryName.ToString().Equals(KeyName, ESearchCase::IgnoreCase))
					{
						TargetBB->Keys.RemoveAt(i);
						FProperty* KeysProp = TargetBB->GetClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys));
						if (KeysProp)
						{
							FPropertyChangedEvent Event(KeysProp, EPropertyChangeType::ArrayRemove);
							TargetBB->PostEditChangeProperty(Event);
						}
						// Refresh BT graph blackboard references if available
						if (BT && BT->BTGraph)
						{
							UBehaviorTreeGraph* BTGraphTyped = Cast<UBehaviorTreeGraph>(BT->BTGraph);
							if (BTGraphTyped) BTGraphTyped->UpdateBlackboardChange();
						}
						TargetBB->MarkPackageDirty();
						Session.Log(FString::Printf(TEXT("[OK] remove(\"key\", \"%s\")"), *KeyName));
						return sol::make_object(Lua, true);
					}
				}

				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"key\") -> '%s' not found"), *KeyName));
				return sol::lua_nil;
			}

			// ---- remove("node"/"decorator"/"service", guid_or_name) ----
			if (FType.Equals(TEXT("node"), ESearchCase::IgnoreCase)
				|| FType.Equals(TEXT("decorator"), ESearchCase::IgnoreCase)
				|| FType.Equals(TEXT("service"), ESearchCase::IgnoreCase))
			{
				if (!BT)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> only available on BehaviorTree assets"), *FType));
					return sol::lua_nil;
				}
				if (!Id.is<std::string>())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> guid or node name required"), *FType));
					return sol::lua_nil;
				}
				FString NodeId = UTF8_TO_TCHAR(Id.as<std::string>().c_str());

				UBehaviorTreeGraphNode* GraphNode = ResolveParentGraphNode(BT, NodeId);
				if (!GraphNode)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\", \"%s\") -> node not found"), *FType, *NodeId));
					return sol::lua_nil;
				}

				// Prevent removing root node
				if (Cast<UBehaviorTreeGraphNode_Root>(GraphNode))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\", \"%s\") -> cannot remove root node"), *FType, *NodeId));
					return sol::lua_nil;
				}

				UEdGraph* BTGraph = BT->BTGraph;
				if (!BTGraph)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> no graph available"), *FType));
					return sol::lua_nil;
				}

				FString NodeName = GraphNode->GetNodeTitle(ENodeTitleType::MenuTitle).ToString();

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemBTNode", "Remove BT Node"));
				BTGraph->Modify();
				GraphNode->Modify();

				// If this is a sub-node (decorator/service), remove from parent
				if (GraphNode->ParentNode)
				{
					GraphNode->ParentNode->RemoveSubNode(GraphNode);
				}

				// Break links and destroy
				const UEdGraphSchema* Schema = BTGraph->GetSchema();
				if (Schema)
				{
					Schema->BreakNodeLinks(*GraphNode);
				}
				GraphNode->DestroyNode();

				// Rebuild the runtime tree from the graph
				UBehaviorTreeGraph* BTGraphTyped = Cast<UBehaviorTreeGraph>(BTGraph);
				if (BTGraphTyped)
				{
					BTGraphTyped->UpdateAsset();
				}
				BT->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"%s\", \"%s\") -> removed \"%s\""), *FType, *NodeId, *NodeName));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: key, node, decorator, service"), *FType));
			return sol::lua_nil;
		});

		// ---- list(type?) ----
		AssetObj.set_function("list", [BT, TargetBB, &Session](sol::table self,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = self["info"];
				if (InfoFn.valid()) return InfoFn(self);
				return sol::lua_nil;
			}

			if (FType.Contains(TEXT("key"), ESearchCase::IgnoreCase))
			{
				if (!TargetBB)
				{
					Session.Log(TEXT("[FAIL] list(\"keys\") -> no blackboard available"));
					return sol::lua_nil;
				}

				sol::table Result = BuildAllKeysTable(Lua, TargetBB);
				int32 TotalKeys = 0;
				for (UBlackboardData* Cur = TargetBB; Cur; Cur = Cur->Parent) TotalKeys += Cur->Keys.Num();
				Session.Log(FString::Printf(TEXT("[OK] list(\"keys\") -> %d (%d own, %d inherited)"),
					TotalKeys, TargetBB->Keys.Num(), TotalKeys - TargetBB->Keys.Num()));
				return Result;
			}

			// ---- list("nodes") — flat array of all nodes ----
			if (FType.Contains(TEXT("node"), ESearchCase::IgnoreCase))
			{
				if (!BT || !BT->RootNode)
				{
					Session.Log(TEXT("[FAIL] list(\"nodes\") -> no behavior tree or no root node"));
					return sol::lua_nil;
				}

				TMap<UObject*, FGuid> GuidMap;
				TMap<UObject*, FVector2D> PosMap;
				BuildGuidMap(BT, GuidMap, PosMap);

				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				CollectFlatNodes(Lua, Result, Idx, BT->RootNode, GuidMap);

				Session.Log(FString::Printf(TEXT("[OK] list(\"nodes\") -> %d"), Idx - 1));
				return Result;
			}

			// ---- list("decorator_types") / list("service_types") ----
			if (FType.Contains(TEXT("decorator"), ESearchCase::IgnoreCase) && FType.Contains(TEXT("type"), ESearchCase::IgnoreCase))
			{
				if (!BT)
				{
					Session.Log(TEXT("[FAIL] list(\"decorator_types\") -> only available on BehaviorTree assets"));
					return sol::lua_nil;
				}
				EnsureBTEditorOpen(BT);
				TArray<FGraphNodeClassData> ClassData;
				UClass* GraphNodeClass = nullptr;
				GatherSubNodeClasses(BT, ESubNode::Decorator, ClassData, GraphNodeClass);

				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (const FGraphNodeClassData& CD : ClassData)
				{
					if (CD.IsAbstract() || CD.IsDeprecated()) continue;
					sol::table Entry = Lua.create_table();
					Entry["class"] = TCHAR_TO_UTF8(*CD.GetClassName());
					Entry["name"] = TCHAR_TO_UTF8(*FName::NameToDisplayString(CD.ToString(), false));
					FText Cat = CD.GetCategory();
					if (!Cat.IsEmpty()) Entry["category"] = TCHAR_TO_UTF8(*Cat.ToString());
					Idx++;
					Result[Idx - 1] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"decorator_types\") -> %d"), Idx - 1));
				return Result;
			}

			if (FType.Contains(TEXT("service"), ESearchCase::IgnoreCase) && FType.Contains(TEXT("type"), ESearchCase::IgnoreCase))
			{
				if (!BT)
				{
					Session.Log(TEXT("[FAIL] list(\"service_types\") -> only available on BehaviorTree assets"));
					return sol::lua_nil;
				}
				EnsureBTEditorOpen(BT);
				TArray<FGraphNodeClassData> ClassData;
				UClass* GraphNodeClass = nullptr;
				GatherSubNodeClasses(BT, ESubNode::Service, ClassData, GraphNodeClass);

				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (const FGraphNodeClassData& CD : ClassData)
				{
					if (CD.IsAbstract() || CD.IsDeprecated()) continue;
					sol::table Entry = Lua.create_table();
					Entry["class"] = TCHAR_TO_UTF8(*CD.GetClassName());
					Entry["name"] = TCHAR_TO_UTF8(*FName::NameToDisplayString(CD.ToString(), false));
					FText Cat = CD.GetCategory();
					if (!Cat.IsEmpty()) Entry["category"] = TCHAR_TO_UTF8(*Cat.ToString());
					Idx++;
					Result[Idx - 1] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"service_types\") -> %d"), Idx - 1));
				return Result;
			}

			// ---- list("task_types") ----
			if (FType.Contains(TEXT("task"), ESearchCase::IgnoreCase) && FType.Contains(TEXT("type"), ESearchCase::IgnoreCase))
			{
				if (!BT)
				{
					Session.Log(TEXT("[FAIL] list(\"task_types\") -> only available on BehaviorTree assets"));
					return sol::lua_nil;
				}
				EnsureBTEditorOpen(BT);
				FGraphNodeClassHelper ClassHelper(UBTNode::StaticClass());
				TArray<FGraphNodeClassData> NodeClasses;
				ClassHelper.GatherClasses(UBTTaskNode::StaticClass(), NodeClasses);

				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (const FGraphNodeClassData& CD : NodeClasses)
				{
					if (CD.IsAbstract() || CD.IsDeprecated()) continue;
					sol::table Entry = Lua.create_table();
					Entry["class"] = TCHAR_TO_UTF8(*CD.GetClassName());
					Entry["name"] = TCHAR_TO_UTF8(*FName::NameToDisplayString(CD.ToString(), false));
					FText Cat = CD.GetCategory();
					if (!Cat.IsEmpty()) Entry["category"] = TCHAR_TO_UTF8(*Cat.ToString());
					Result[Idx++] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"task_types\") -> %d"), Idx - 1));
				return Result;
			}

			// ---- list("composite_types") ----
			if (FType.Contains(TEXT("composite"), ESearchCase::IgnoreCase) && FType.Contains(TEXT("type"), ESearchCase::IgnoreCase))
			{
				if (!BT)
				{
					Session.Log(TEXT("[FAIL] list(\"composite_types\") -> only available on BehaviorTree assets"));
					return sol::lua_nil;
				}
				EnsureBTEditorOpen(BT);
				FGraphNodeClassHelper ClassHelper(UBTNode::StaticClass());
				TArray<FGraphNodeClassData> NodeClasses;
				ClassHelper.GatherClasses(UBTCompositeNode::StaticClass(), NodeClasses);

				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (const FGraphNodeClassData& CD : NodeClasses)
				{
					if (CD.IsAbstract() || CD.IsDeprecated()) continue;
					sol::table Entry = Lua.create_table();
					Entry["class"] = TCHAR_TO_UTF8(*CD.GetClassName());
					Entry["name"] = TCHAR_TO_UTF8(*FName::NameToDisplayString(CD.ToString(), false));
					FText Cat = CD.GetCategory();
					if (!Cat.IsEmpty()) Entry["category"] = TCHAR_TO_UTF8(*Cat.ToString());
					Result[Idx++] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"composite_types\") -> %d"), Idx - 1));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: keys, nodes, decorator_types, service_types, task_types, composite_types"), *FType));
			return sol::lua_nil;
		});

		// ---- reorder_children(parent, order_table) ----
		AssetObj.set_function("reorder_children", [BT, &Session](sol::table /*self*/,
			const std::string& ParentName, sol::table OrderTable, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!BT || !BT->RootNode)
			{
				Session.Log(TEXT("[FAIL] reorder_children -> no behavior tree or tree is empty"));
				return sol::lua_nil;
			}
			if (!BT->BTGraph)
			{
				Session.Log(TEXT("[FAIL] reorder_children -> no BT graph available"));
				return sol::lua_nil;
			}

			FString FParent = UTF8_TO_TCHAR(ParentName.c_str());

			// Find the parent graph node (source of truth for child ordering)
			UBehaviorTreeGraphNode* ParentGraphNode = nullptr;
			for (UEdGraphNode* Node : BT->BTGraph->Nodes)
			{
				UBehaviorTreeGraphNode* BTNode = Cast<UBehaviorTreeGraphNode>(Node);
				if (!BTNode || !BTNode->NodeInstance) continue;
				UBTNode* RuntimeNode = Cast<UBTNode>(BTNode->NodeInstance);
				if (RuntimeNode && RuntimeNode->GetNodeName().Equals(FParent, ESearchCase::IgnoreCase))
				{
					ParentGraphNode = BTNode;
					break;
				}
			}
			if (!ParentGraphNode)
			{
				// Try GUID match
				ParentGraphNode = FindGraphNodeByGuid(BT, FParent);
			}
			if (!ParentGraphNode)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] reorder_children -> parent '%s' not found"), *FParent));
				return sol::lua_nil;
			}

			// Find the output pin that connects to children
			UEdGraphPin* OutputPin = nullptr;
			for (UEdGraphPin* Pin : ParentGraphNode->Pins)
			{
				if (Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0)
				{
					OutputPin = Pin;
					break;
				}
			}
			if (!OutputPin || OutputPin->LinkedTo.Num() == 0)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] reorder_children -> '%s' has no children"), *FParent));
				return sol::lua_nil;
			}

			// Build order list from Lua table
			TArray<FString> Order;
			for (auto& kv : OrderTable)
			{
				if (kv.second.is<std::string>())
					Order.Add(UTF8_TO_TCHAR(kv.second.as<std::string>().c_str()));
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "ReorderBT", "Reorder BT Children"));
			BT->BTGraph->Modify();
			ParentGraphNode->Modify();

			// Reorder graph pin connections (the source of truth for child order)
			TArray<UEdGraphPin*> OldLinked = OutputPin->LinkedTo;
			TArray<UEdGraphPin*> NewLinked;
			TArray<bool> Used;
			Used.SetNumZeroed(OldLinked.Num());

			for (const FString& ChildName : Order)
			{
				for (int32 i = 0; i < OldLinked.Num(); i++)
				{
					if (Used[i]) continue;
					UBehaviorTreeGraphNode* ChildGraphNode = Cast<UBehaviorTreeGraphNode>(OldLinked[i]->GetOwningNode());
					if (!ChildGraphNode || !ChildGraphNode->NodeInstance) continue;
					UBTNode* ChildRuntime = Cast<UBTNode>(ChildGraphNode->NodeInstance);
					if (ChildRuntime && ChildRuntime->GetNodeName().Equals(ChildName, ESearchCase::IgnoreCase))
					{
						NewLinked.Add(OldLinked[i]);
						Used[i] = true;
						break;
					}
				}
			}

			// Append unmentioned children at end
			for (int32 i = 0; i < OldLinked.Num(); i++)
			{
				if (!Used[i]) NewLinked.Add(OldLinked[i]);
			}

			OutputPin->LinkedTo = NewLinked;

			// Rebuild runtime tree from graph — this is the canonical flow
			UBehaviorTreeGraph* BTGraphTyped = Cast<UBehaviorTreeGraph>(BT->BTGraph);
			if (BTGraphTyped)
			{
				BTGraphTyped->UpdateAsset();
			}
			BT->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] reorder_children(\"%s\", %d children)"), *FParent, NewLinked.Num()));
			return sol::make_object(Lua, true);
		});

		// ---- info() — full recursive tree structure ----
		AssetObj.set_function("info", [BT, TargetBB, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();

			if (BT)
			{
				Result["has_root"] = (BT->RootNode != nullptr);

				if (BT->RootNode)
				{
					// Build GUID + position maps from editor graph
					TMap<UObject*, FGuid> GuidMap;
					TMap<UObject*, FVector2D> PosMap;
					BuildGuidMap(BT, GuidMap, PosMap);

					// Full recursive tree
					Result["root"] = BuildNodeTable(Lua, BT->RootNode, GuidMap, PosMap);

					// Node counts
					int32 Composites = 0, Tasks = 0, Decorators = 0, Services = 0;
					CountBTNodes(BT->RootNode, Composites, Tasks, Decorators, Services);
					sol::table Counts = Lua.create_table();
					Counts["composites"] = Composites;
					Counts["tasks"] = Tasks;
					Counts["decorators"] = Decorators;
					Counts["services"] = Services;
					Result["node_counts"] = Counts;
				}
			}

			if (TargetBB)
			{
				Result["blackboard_asset"] = TCHAR_TO_UTF8(*TargetBB->GetName());
				Result["blackboard_keys"] = BuildAllKeysTable(Lua, TargetBB);
				if (TargetBB->Parent)
				{
					Result["parent_blackboard"] = TCHAR_TO_UTF8(*TargetBB->Parent->GetName());
				}
			}

			Session.Log(FString::Printf(TEXT("[OK] info() -> BT=%s, root=%s, %d blackboard keys"),
				BT ? TEXT("yes") : TEXT("no"),
				(BT && BT->RootNode) ? TEXT("yes") : TEXT("no"),
				TargetBB ? TargetBB->Keys.Num() : 0));
			return Result;
		});

		// ---- help() ----
		// help() is handled by OpenAsset's help() which reads _help_text
		// and includes generic methods (get/set/list_properties/save/etc.)

		// ---- configure(type, id, params) ----
		AssetObj.set_function("configure", [BT, TargetBB, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::optional<sol::table> ParamsOpt,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- configure("key", "KeyName", {type?, default_value?, base_class?, enum_name?, description?}) ----
			if (FType.Equals(TEXT("key"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>())
				{
					Session.Log(TEXT("[FAIL] configure(\"key\") -> key name required"));
					return sol::lua_nil;
				}
				if (!ParamsOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"key\") -> params table required"));
					return sol::lua_nil;
				}
				if (!TargetBB)
				{
					Session.Log(TEXT("[FAIL] configure(\"key\") -> no blackboard data"));
					return sol::lua_nil;
				}

				FString KeyName = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				sol::table P = ParamsOpt.value();

				// Find the key by name (direct match on EntryName)
				int32 KeyIndex = -1;
				for (int32 i = 0; i < TargetBB->Keys.Num(); i++)
				{
					if (TargetBB->Keys[i].EntryName.ToString().Equals(KeyName, ESearchCase::IgnoreCase))
					{
						KeyIndex = i;
						break;
					}
				}

				if (KeyIndex < 0)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"key\", \"%s\") -> key not found"), *KeyName));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("BT: Configure Key")));
				TargetBB->Modify();

				FBlackboardEntry& Entry = TargetBB->Keys[KeyIndex];

				// Description
#if WITH_EDITORONLY_DATA
				std::string DescStr = P.get_or<std::string>("description", "");
				if (!DescStr.empty())
				{
					Entry.EntryDescription = UTF8_TO_TCHAR(DescStr.c_str());
				}
				std::string CategoryStr = P.get_or<std::string>("category", "");
				if (!CategoryStr.empty())
				{
					Entry.EntryCategory = FName(UTF8_TO_TCHAR(CategoryStr.c_str()));
				}
#endif

				// Instance synced
				sol::optional<bool> InstanceSynced = P.get<sol::optional<bool>>("instance_synced");
				if (InstanceSynced.has_value())
				{
					Entry.bInstanceSynced = InstanceSynced.value();
				}

				// Type-specific configuration
				if (Entry.KeyType)
				{
					if (UBlackboardKeyType_Object* ObjKey = Cast<UBlackboardKeyType_Object>(Entry.KeyType))
					{
						std::string BaseClassName = P.get_or("base_class", std::string());
						if (!BaseClassName.empty())
						{
							FString FBaseClass = UTF8_TO_TCHAR(BaseClassName.c_str());
							UClass* Cls = UClass::TryFindTypeSlow<UClass>(*FBaseClass, EFindFirstObjectOptions::ExactClass);
							if (!Cls) Cls = StaticLoadClass(UObject::StaticClass(), nullptr, *FBaseClass);
							if (Cls) ObjKey->BaseClass = Cls;
						}
					}
					else if (UBlackboardKeyType_Class* ClsKey = Cast<UBlackboardKeyType_Class>(Entry.KeyType))
					{
						std::string BaseClassName = P.get_or("base_class", std::string());
						if (!BaseClassName.empty())
						{
							FString FBaseClass = UTF8_TO_TCHAR(BaseClassName.c_str());
							UClass* Cls = UClass::TryFindTypeSlow<UClass>(*FBaseClass, EFindFirstObjectOptions::ExactClass);
							if (!Cls) Cls = StaticLoadClass(UObject::StaticClass(), nullptr, *FBaseClass);
							if (Cls) ClsKey->BaseClass = Cls;
						}
					}
					else if (UBlackboardKeyType_Enum* EnumKey = Cast<UBlackboardKeyType_Enum>(Entry.KeyType))
					{
						std::string EnumTypeName = P.get_or("enum_type", std::string());
						if (!EnumTypeName.empty())
						{
							FString FEnumName = UTF8_TO_TCHAR(EnumTypeName.c_str());
							UEnum* FoundEnum = UClass::TryFindTypeSlow<UEnum>(*FEnumName, EFindFirstObjectOptions::ExactClass);
							if (FoundEnum)
							{
								EnumKey->EnumType = FoundEnum;
								EnumKey->EnumName = FoundEnum->GetName();
								EnumKey->bIsEnumNameValid = true;
							}
						}
						sol::optional<int> DefVal = P.get<sol::optional<int>>("default_value");
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
						if (DefVal.has_value()) EnumKey->DefaultValue = static_cast<uint8>(DefVal.value());
#endif
					}
					else if (UBlackboardKeyType_Bool* BoolKey = Cast<UBlackboardKeyType_Bool>(Entry.KeyType))
					{
						sol::optional<bool> DefVal = P.get<sol::optional<bool>>("default_value");
						if (DefVal.has_value()) BoolKey->bDefaultValue = DefVal.value();
					}
					else if (UBlackboardKeyType_Int* IntKey = Cast<UBlackboardKeyType_Int>(Entry.KeyType))
					{
						sol::optional<int> DefVal = P.get<sol::optional<int>>("default_value");
						if (DefVal.has_value()) IntKey->DefaultValue = DefVal.value();
					}
					else if (UBlackboardKeyType_Float* FloatKey = Cast<UBlackboardKeyType_Float>(Entry.KeyType))
					{
						sol::optional<double> DefVal = P.get<sol::optional<double>>("default_value");
						if (DefVal.has_value()) FloatKey->DefaultValue = static_cast<float>(DefVal.value());
					}
					else if (UBlackboardKeyType_String* StrKey = Cast<UBlackboardKeyType_String>(Entry.KeyType))
					{
						std::string DefVal = P.get_or("default_value", std::string());
						if (!DefVal.empty()) StrKey->DefaultValue = UTF8_TO_TCHAR(DefVal.c_str());
					}
					else if (UBlackboardKeyType_Name* NameKey = Cast<UBlackboardKeyType_Name>(Entry.KeyType))
					{
						std::string DefVal = P.get_or("default_value", std::string());
						if (!DefVal.empty()) NameKey->DefaultValue = FName(UTF8_TO_TCHAR(DefVal.c_str()));
					}
				}

				FProperty* KeysProp = TargetBB->GetClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys));
				FPropertyChangedEvent KeysEvt(KeysProp, EPropertyChangeType::ValueSet);
				TargetBB->PostEditChangeProperty(KeysEvt);
				if (BT && BT->BTGraph)
				{
					UBehaviorTreeGraph* BTGraphTyped = Cast<UBehaviorTreeGraph>(BT->BTGraph);
					if (BTGraphTyped) BTGraphTyped->UpdateBlackboardChange();
				}
				TargetBB->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"key\", \"%s\")"), *KeyName));
				return sol::make_object(Lua, true);
			}

			// ---- configure("blackboard", path) — set blackboard asset on BT ----
			if (FType.Equals(TEXT("blackboard"), ESearchCase::IgnoreCase))
			{
				if (!BT)
				{
					Session.Log(TEXT("[FAIL] configure(\"blackboard\") -> only available on BehaviorTree assets"));
					return sol::lua_nil;
				}
				if (!Id.is<std::string>())
				{
					Session.Log(TEXT("[FAIL] configure(\"blackboard\") -> blackboard asset path required"));
					return sol::lua_nil;
				}

				FString BBPath = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				if (!BBPath.StartsWith(TEXT("/")))
					BBPath = TEXT("/Game/") + BBPath;

				UBlackboardData* NewBB = LoadObject<UBlackboardData>(nullptr, *BBPath);
				if (!NewBB)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"blackboard\", \"%s\") -> blackboard asset not found"), *BBPath));
					return sol::lua_nil;
				}

				// Find the root graph node — that's where the editor stores the blackboard reference
				if (!BT->BTGraph)
				{
					// Graph is created on first editor open — force it
					EnsureBTEditorOpen(BT);
				}
				if (!BT->BTGraph)
				{
					Session.Log(TEXT("[FAIL] configure(\"blackboard\") -> no BT graph available (try opening the BT editor first)"));
					return sol::lua_nil;
				}

				UBehaviorTreeGraphNode_Root* RootGraphNode = nullptr;
				for (UEdGraphNode* Node : BT->BTGraph->Nodes)
				{
					RootGraphNode = Cast<UBehaviorTreeGraphNode_Root>(Node);
					if (RootGraphNode) break;
				}

				if (!RootGraphNode)
				{
					Session.Log(TEXT("[FAIL] configure(\"blackboard\") -> root graph node not found"));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SetBB", "Set Blackboard Asset"));
				RootGraphNode->Modify();
				RootGraphNode->BlackboardAsset = NewBB;
				RootGraphNode->UpdateBlackboard();
				BT->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"blackboard\", \"%s\")"), *NewBB->GetName()));
				return sol::make_object(Lua, true);
			}

			// ---- configure("node", guid_or_name, {prop=val, ...}) — set properties on a BT node ----
			if (FType.Equals(TEXT("node"), ESearchCase::IgnoreCase)
				|| FType.Equals(TEXT("decorator"), ESearchCase::IgnoreCase)
				|| FType.Equals(TEXT("service"), ESearchCase::IgnoreCase)
				|| FType.Equals(TEXT("task"), ESearchCase::IgnoreCase)
				|| FType.Equals(TEXT("composite"), ESearchCase::IgnoreCase))
			{
				if (!BT)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> only available on BehaviorTree assets"), *FType));
					return sol::lua_nil;
				}
				if (!Id.is<std::string>())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> guid or node name required"), *FType));
					return sol::lua_nil;
				}
				if (!ParamsOpt.has_value())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> params table required"), *FType));
					return sol::lua_nil;
				}

				FString NodeId = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				sol::table P = ParamsOpt.value();

				// Find the graph node
				UBehaviorTreeGraphNode* GraphNode = ResolveParentGraphNode(BT, NodeId);
				if (!GraphNode)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\", \"%s\") -> node not found"), *FType, *NodeId));
					return sol::lua_nil;
				}

				UObject* NodeInstance = GraphNode->NodeInstance;
				if (!NodeInstance)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\", \"%s\") -> node has no runtime instance"), *FType, *NodeId));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "ConfigBTNode", "Configure BT Node"));
				NodeInstance->SetFlags(RF_Transactional);
				NodeInstance->Modify();

				int32 SetCount = 0;
				TArray<FString> Errors;

				for (auto& kv : P)
				{
					if (!kv.first.is<std::string>()) continue;

					// Convert value to string for ImportText — accept string, number, or boolean
					FString PropName = UTF8_TO_TCHAR(kv.first.as<std::string>().c_str());
					FString PropValue;
					if (kv.second.is<std::string>())
					{
						PropValue = UTF8_TO_TCHAR(kv.second.as<std::string>().c_str());
					}
					else if (kv.second.is<double>())
					{
						double NumVal = kv.second.as<double>();
						if (FMath::IsNearlyEqual(NumVal, FMath::RoundToDouble(NumVal)))
							PropValue = FString::Printf(TEXT("%d"), static_cast<int64>(NumVal));
						else
							PropValue = FString::SanitizeFloat(NumVal);
					}
					else if (kv.second.is<bool>())
					{
						PropValue = kv.second.as<bool>() ? TEXT("True") : TEXT("False");
					}
					else
					{
						continue;
					}

					// Find property on the node instance
					FProperty* Prop = nullptr;
					for (TFieldIterator<FProperty> PropIt(NodeInstance->GetClass()); PropIt; ++PropIt)
					{
						if (PropIt->GetName().Equals(PropName, ESearchCase::IgnoreCase))
						{
							Prop = *PropIt;
							break;
						}
					}

					if (!Prop)
					{
						Errors.Add(FString::Printf(TEXT("property '%s' not found"), *PropName));
						continue;
					}

					NodeInstance->PreEditChange(Prop);

					const TCHAR* ImportResult = Prop->ImportText_InContainer(*PropValue, NodeInstance, NodeInstance, PPF_None);
					if (!ImportResult)
					{
						// Try boolean fixup
						FString Transformed = PropValue;
						if (PropValue.Equals(TEXT("true"), ESearchCase::IgnoreCase)) Transformed = TEXT("True");
						else if (PropValue.Equals(TEXT("false"), ESearchCase::IgnoreCase)) Transformed = TEXT("False");
						ImportResult = Prop->ImportText_InContainer(*Transformed, NodeInstance, NodeInstance, PPF_None);
					}

					// For struct wrapper types (FValueOrBBKey_Float, etc.), try setting
					// the inner DefaultValue sub-property when direct ImportText fails
					if (!ImportResult)
					{
						if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
						{
							void* StructData = StructProp->ContainerPtrToValuePtr<void>(NodeInstance);
							FProperty* DefaultValueProp = StructProp->Struct->FindPropertyByName(TEXT("DefaultValue"));
							if (DefaultValueProp && StructData)
							{
								ImportResult = DefaultValueProp->ImportText_Direct(*PropValue, DefaultValueProp->ContainerPtrToValuePtr<void>(StructData), NodeInstance, PPF_None);
							}
						}
					}

					FPropertyChangedEvent Event(Prop, EPropertyChangeType::ValueSet);
					NodeInstance->PostEditChangeProperty(Event);

					if (ImportResult)
					{
						SetCount++;
					}
					else
					{
						Errors.Add(FString::Printf(TEXT("failed to set '%s' = '%s'"), *PropName, *PropValue));
					}
				}

				// Rebuild the runtime tree to pick up changes
				UBehaviorTreeGraph* BTGraphTyped = Cast<UBehaviorTreeGraph>(BT->BTGraph);
				if (BTGraphTyped)
				{
					BTGraphTyped->UpdateAsset();
				}
				BT->MarkPackageDirty();

				if (Errors.Num() > 0)
				{
					Session.Log(FString::Printf(TEXT("[WARN] configure(\"%s\", \"%s\") -> %d set, %d errors: %s"),
						*FType, *NodeId, SetCount, Errors.Num(), *FString::Join(Errors, TEXT("; "))));
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[OK] configure(\"%s\", \"%s\") -> %d properties set"),
						*FType, *NodeId, SetCount));
				}
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: key, blackboard, node, decorator, service, task, composite"), *FType));
			return sol::lua_nil;
		});
	});
}

REGISTER_LUA_BINDING(BehaviorTree, BehaviorTreeDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindBehaviorTree(Lua, Session);
});
