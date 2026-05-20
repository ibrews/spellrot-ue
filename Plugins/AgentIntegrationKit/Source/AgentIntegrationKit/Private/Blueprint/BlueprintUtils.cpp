#include "Blueprint/BlueprintUtils.h"
#include "AgentIntegrationKitModule.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#include "Misc/StringOutputDevice.h"
#endif
#include "Engine/Blueprint.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetDebugUtilities.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_ComponentBoundEvent.h"
#include "UObject/SavePackage.h"
#include "AssetToolsModule.h"
#include "Factories/BlueprintFactory.h"
#include "UObject/UObjectGlobals.h"
#include "EdGraphSchema_K2.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_EditablePinBase.h"
#include "Misc/OutputDeviceRedirector.h"
#include "UObject/EnumProperty.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Timeline.h"
#include "K2Node_Variable.h"
#include "Engine/TimelineTemplate.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "Curves/CurveLinearColor.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Engine/UserDefinedEnum.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
#include "StructUtils/UserDefinedStruct.h"
#else
#include "Engine/UserDefinedStruct.h"
#endif
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"

// AnimBP support
#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimationStateMachineGraph.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimStateEntryNode.h"
#include "AnimStateConduitNode.h"
#include "AnimationGraph.h"
#include "AnimationStateGraph.h"
#include "AnimationTransitionGraph.h"
#include "AnimGraphNode_LinkedAnimLayer.h"

// Recursively collect SubGraphs (composite/collapsed node bound graphs)
static void CollectSubGraphs(UEdGraph* Graph, TArray<UEdGraph*>& OutSubGraphs)
{
	for (UEdGraph* Sub : Graph->SubGraphs)
	{
		if (Sub)
		{
			OutSubGraphs.Add(Sub);
			CollectSubGraphs(Sub, OutSubGraphs);
		}
	}
}

static UEdGraph* FindInSubGraphs(UEdGraph* Graph, const FString& GraphName)
{
	if (!Graph) return nullptr;
	for (UEdGraph* Sub : Graph->SubGraphs)
	{
		if (!Sub) continue;
		if (Sub->GetName() == GraphName) return Sub;
		UEdGraph* Found = FindInSubGraphs(Sub, GraphName);
		if (Found) return Found;
	}
	return nullptr;
}

FBlueprintInfo NeoBlueprint::LoadBlueprint(const FString& AssetPath)
{
	FBlueprintInfo Info;

	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath);

	if (!BP)
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		TArray<FAssetData> Results;
		ARM.Get().GetAssetsByPackageName(FName(*AssetPath), Results);
		if (Results.Num() > 0)
		{
			BP = Cast<UBlueprint>(Results[0].GetAsset());
		}
	}

	if (!BP)
	{
		return Info;
	}

	Info.Blueprint = BP;
	Info.Name = BP->GetName();
	Info.AssetPath = AssetPath;
	Info.ParentClass = BP->ParentClass ? BP->ParentClass->GetName() : TEXT("None");

	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (Graph)
		{
			Info.Graphs.Add(Graph->GetName());
		}
	}

	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph)
		{
			Info.Graphs.Add(Graph->GetName());
		}
	}

	for (UEdGraph* Graph : BP->MacroGraphs)
	{
		if (Graph)
		{
			Info.Graphs.Add(Graph->GetName());
		}
	}

	// Collect composite/collapsed sub-graphs recursively
	TArray<UEdGraph*> AllTopLevel;
	AllTopLevel.Append(BP->UbergraphPages);
	AllTopLevel.Append(BP->FunctionGraphs);
	AllTopLevel.Append(BP->MacroGraphs);
	for (UEdGraph* Graph : AllTopLevel)
	{
		if (!Graph) continue;
		TArray<UEdGraph*> SubGraphs;
		CollectSubGraphs(Graph, SubGraphs);
		for (UEdGraph* Sub : SubGraphs)
		{
			Info.Graphs.Add(Sub->GetName());
		}
	}

	return Info;
}

UEdGraph* NeoBlueprint::FindGraph(UBlueprint* BP, const FString& GraphName)
{
	if (!BP) return nullptr;

	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}

	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}

	for (UEdGraph* Graph : BP->MacroGraphs)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}

	// Search composite/collapsed sub-graphs recursively
	TArray<UEdGraph*> AllTopLevel;
	AllTopLevel.Append(BP->UbergraphPages);
	AllTopLevel.Append(BP->FunctionGraphs);
	AllTopLevel.Append(BP->MacroGraphs);
	for (UEdGraph* Graph : AllTopLevel)
	{
		if (!Graph) continue;
		UEdGraph* Found = FindInSubGraphs(Graph, GraphName);
		if (Found) return Found;
	}

	// AnimBP: resolve selector-style paths (e.g. "Locomotion/Idle", "Locomotion/Idle->Run")
	TArray<TPair<FString, UEdGraph*>> AnimGraphs;
	CollectAnimBPGraphs(BP, AnimGraphs);
	for (const auto& Pair : AnimGraphs)
	{
		if (Pair.Key == GraphName)
		{
			return Pair.Value;
		}
	}

	return nullptr;
}

// ============================================================================
// Animation Blueprint helpers
// ============================================================================

UEdGraph* NeoBlueprint::FindAnimGraph(UBlueprint* BP)
{
	if (!BP) return nullptr;
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph && Graph->GetFName() == TEXT("AnimGraph"))
		{
			return Graph;
		}
	}
	return nullptr;
}

// Safe helpers for transition node traversal (GetPreviousState/GetNextState crash on malformed nodes)
static UAnimStateNodeBase* SafeGetPreviousState(UAnimStateTransitionNode* TransNode)
{
	if (!TransNode || TransNode->Pins.Num() < 2) return nullptr;
	UEdGraphPin* InputPin = TransNode->GetInputPin();
	if (!InputPin || InputPin->LinkedTo.Num() == 0) return nullptr;
	return TransNode->GetPreviousState();
}

static UAnimStateNodeBase* SafeGetNextState(UAnimStateTransitionNode* TransNode)
{
	if (!TransNode || TransNode->Pins.Num() < 2) return nullptr;
	UEdGraphPin* OutputPin = TransNode->GetOutputPin();
	if (!OutputPin || OutputPin->LinkedTo.Num() == 0) return nullptr;
	return TransNode->GetNextState();
}

static FString GetStateName(UEdGraphNode* Node)
{
	if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node))
	{
		return StateNode->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
	}
	if (Cast<UAnimStateEntryNode>(Node))
	{
		return TEXT("[Entry]");
	}
	if (UAnimStateConduitNode* Conduit = Cast<UAnimStateConduitNode>(Node))
	{
		return Conduit->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
	}
	return Node->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
}

void NeoBlueprint::CollectAnimBPGraphs(UBlueprint* BP, TArray<TPair<FString, UEdGraph*>>& OutGraphs)
{
	UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(BP);
	if (!AnimBP) return;

	// Traverse all FunctionGraphs looking for UAnimationGraph instances
	for (UEdGraph* FuncGraph : AnimBP->FunctionGraphs)
	{
		UAnimationGraph* AnimGraph = Cast<UAnimationGraph>(FuncGraph);
		if (!AnimGraph) continue;

		// Find state machine nodes within this anim graph
		for (UEdGraphNode* Node : AnimGraph->Nodes)
		{
			UAnimGraphNode_StateMachineBase* SMNode = Cast<UAnimGraphNode_StateMachineBase>(Node);
			if (!SMNode || !SMNode->EditorStateMachineGraph) continue;

			UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
			if (!SMGraph) continue;

			FString SMName = SMGraph->GetName();

			// Add the state machine graph itself
			OutGraphs.Add(TPair<FString, UEdGraph*>(SMName, SMGraph));

			// Traverse nodes inside the state machine
			for (UEdGraphNode* SMChild : SMGraph->Nodes)
			{
				if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(SMChild))
				{
					if (StateNode->BoundGraph)
					{
						FString StateName = StateNode->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
						FString Selector = FString::Printf(TEXT("%s/%s"), *SMName, *StateName);
						OutGraphs.Add(TPair<FString, UEdGraph*>(Selector, StateNode->BoundGraph));
					}
				}
				else if (UAnimStateTransitionNode* TransNode = Cast<UAnimStateTransitionNode>(SMChild))
				{
					UAnimStateNodeBase* PrevState = SafeGetPreviousState(TransNode);
					UAnimStateNodeBase* NextState = SafeGetNextState(TransNode);

					FString FromName = PrevState ? GetStateName(PrevState) : TEXT("[Entry]");
					FString ToName = NextState ? GetStateName(NextState) : TEXT("?");

					if (TransNode->BoundGraph)
					{
						FString Selector = FString::Printf(TEXT("%s/%s->%s"), *SMName, *FromName, *ToName);
						OutGraphs.Add(TPair<FString, UEdGraph*>(Selector, TransNode->BoundGraph));
					}
				}
				else if (UAnimStateConduitNode* ConduitNode = Cast<UAnimStateConduitNode>(SMChild))
				{
					if (ConduitNode->BoundGraph)
					{
						FString ConduitName = ConduitNode->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
						FString Selector = FString::Printf(TEXT("%s/%s"), *SMName, *ConduitName);
						OutGraphs.Add(TPair<FString, UEdGraph*>(Selector, ConduitNode->BoundGraph));
					}
				}
			}
		}
	}

	// Collect animation layer graphs from implemented interfaces
	for (const FBPInterfaceDescription& Desc : AnimBP->ImplementedInterfaces)
	{
		for (UEdGraph* LayerGraph : Desc.Graphs)
		{
			if (LayerGraph && Cast<UAnimationGraph>(LayerGraph))
			{
				OutGraphs.Add(TPair<FString, UEdGraph*>(LayerGraph->GetName(), LayerGraph));
			}
		}
	}
}

FCompileResult NeoBlueprint::CompileBlueprint(UBlueprint* BP)
{
	FCompileResult Result;
	if (!BP) return Result;

	// Must use StructurallyModified to trigger full recompile after graph mutations
	// (MarkBlueprintAsModified only marks dirty without invalidating compiled bytecode)
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

	Result.bSuccess = (BP->Status != BS_Error);
	return Result;
}

bool NeoBlueprint::SaveBlueprint(UBlueprint* BP)
{
	if (!BP) return false;

	UPackage* Package = BP->GetOutermost();
	if (!Package) return false;

	FString PackageFileName;
	if (!FPackageName::DoesPackageExist(Package->GetName(), &PackageFileName))
	{
		PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	}

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Standalone;
	return UPackage::SavePackage(Package, BP, *PackageFileName, SaveArgs);
}

UBlueprint* NeoBlueprint::CreateBlueprint(const FString& AssetPath, const FString& ParentClassName)
{
	// Special case: creating an Interface Blueprint
	FString LowerParent = ParentClassName.ToLower();
	if (LowerParent == TEXT("interface") || LowerParent == TEXT("blueprintinterface"))
	{
		return CreateInterfaceBlueprint(AssetPath);
	}

	UClass* ParentClass = nullptr;

	// Try common names first
	static const TMap<FString, FString> ClassAliases = {
		{TEXT("actor"), TEXT("/Script/Engine.Actor")},
		{TEXT("pawn"), TEXT("/Script/Engine.Pawn")},
		{TEXT("character"), TEXT("/Script/Engine.Character")},
		{TEXT("playercontroller"), TEXT("/Script/Engine.PlayerController")},
		{TEXT("gamemode"), TEXT("/Script/Engine.GameModeBase")},
		{TEXT("gamemodebase"), TEXT("/Script/Engine.GameModeBase")},
		{TEXT("hud"), TEXT("/Script/Engine.HUD")},
		{TEXT("actorcomponent"), TEXT("/Script/Engine.ActorComponent")},
		{TEXT("scenecomponent"), TEXT("/Script/Engine.SceneComponent")},
		{TEXT("object"), TEXT("/Script/CoreUObject.Object")},
	};

	FString LowerName = ParentClassName.ToLower();
	if (const FString* FullPath = ClassAliases.Find(LowerName))
	{
		ParentClass = LoadObject<UClass>(nullptr, **FullPath);
	}

	if (!ParentClass)
	{
		ParentClass = FindFirstObject<UClass>(*ParentClassName, EFindFirstObjectOptions::ExactClass);
	}

	if (!ParentClass)
	{
		ParentClass = LoadObject<UClass>(nullptr, *ParentClassName);
	}

	if (!ParentClass)
	{
		return nullptr;
	}

	FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);
	FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
	Factory->ParentClass = ParentClass;

	UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UBlueprint::StaticClass(), Factory);
	return Cast<UBlueprint>(NewAsset);
}

// Forward declaration — resolves a single (non-container) type string into pin category/subcategory
static bool ResolveSingleType(const FString& TypeStr, FName& OutCategory, FName& OutSubCategory, TWeakObjectPtr<UObject>& OutSubCategoryObject);

static bool ResolveTypeString(const FString& VarType, FEdGraphPinType& OutPinType)
{
	FString Trimmed = VarType.TrimStartAndEnd();

	// ----------------------------------------------------------------
	// Container shorthand: Type[] → array:Type
	// ----------------------------------------------------------------
	if (Trimmed.EndsWith(TEXT("[]")))
	{
		FString Inner = Trimmed.LeftChop(2).TrimStartAndEnd();
		FEdGraphPinType InnerType;
		if (!ResolveTypeString(Inner, InnerType)) return false;
		OutPinType = InnerType;
		OutPinType.ContainerType = EPinContainerType::Array;
		return true;
	}

	// ----------------------------------------------------------------
	// Container prefixes: array:Type, set:Type, map:KeyType:ValueType
	// ----------------------------------------------------------------
	if (Trimmed.StartsWith(TEXT("array:"), ESearchCase::IgnoreCase))
	{
		FString Inner = Trimmed.Mid(6).TrimStartAndEnd();
		FEdGraphPinType InnerType;
		if (!ResolveTypeString(Inner, InnerType)) return false;
		OutPinType = InnerType;
		OutPinType.ContainerType = EPinContainerType::Array;
		return true;
	}
	if (Trimmed.StartsWith(TEXT("set:"), ESearchCase::IgnoreCase))
	{
		FString Inner = Trimmed.Mid(4).TrimStartAndEnd();
		FEdGraphPinType InnerType;
		if (!ResolveTypeString(Inner, InnerType)) return false;
		OutPinType = InnerType;
		OutPinType.ContainerType = EPinContainerType::Set;
		return true;
	}
	if (Trimmed.StartsWith(TEXT("map:"), ESearchCase::IgnoreCase))
	{
		// map:KeyType:ValueType
		FString Rest = Trimmed.Mid(4).TrimStartAndEnd();
		int32 ColonIdx = INDEX_NONE;
		Rest.FindChar(TEXT(':'), ColonIdx);
		if (ColonIdx == INDEX_NONE) return false;

		FString KeyStr = Rest.Left(ColonIdx).TrimStartAndEnd();
		FString ValStr = Rest.Mid(ColonIdx + 1).TrimStartAndEnd();
		if (KeyStr.IsEmpty() || ValStr.IsEmpty()) return false;

		// Resolve key type into the primary pin type fields
		FName KeyCat, KeySub;
		TWeakObjectPtr<UObject> KeyObj;
		if (!ResolveSingleType(KeyStr, KeyCat, KeySub, KeyObj)) return false;

		// Resolve value type into the PinValueType terminal fields
		FName ValCat, ValSub;
		TWeakObjectPtr<UObject> ValObj;
		if (!ResolveSingleType(ValStr, ValCat, ValSub, ValObj)) return false;

		OutPinType.ContainerType = EPinContainerType::Map;
		OutPinType.PinCategory = KeyCat;
		OutPinType.PinSubCategory = KeySub;
		OutPinType.PinSubCategoryObject = KeyObj;
		OutPinType.PinValueType.TerminalCategory = ValCat;
		OutPinType.PinValueType.TerminalSubCategory = ValSub;
		OutPinType.PinValueType.TerminalSubCategoryObject = ValObj;
		return true;
	}

	// ----------------------------------------------------------------
	// Single (non-container) type
	// ----------------------------------------------------------------
	FName Cat, Sub;
	TWeakObjectPtr<UObject> SubObj;
	if (!ResolveSingleType(Trimmed, Cat, Sub, SubObj)) return false;

	OutPinType.PinCategory = Cat;
	OutPinType.PinSubCategory = Sub;
	OutPinType.PinSubCategoryObject = SubObj;
	return true;
}

// Fallback class finder: iterates loaded classes by short name when TryFindTypeSlow fails
template<typename T>
static T* FindTypeFallback(const FString& Name)
{
	FString NameLower = Name.ToLower();
	for (TObjectIterator<T> It; It; ++It)
	{
		if (It->GetName().ToLower() == NameLower)
			return *It;
	}
	return nullptr;
}

static UClass* FindClassRobust(const FString& ClassName)
{
	UClass* Cls = UClass::TryFindTypeSlow<UClass>(ClassName);
	if (!Cls && !ClassName.StartsWith(TEXT("U")))
		Cls = UClass::TryFindTypeSlow<UClass>(TEXT("U") + ClassName);
	if (!Cls && !ClassName.StartsWith(TEXT("A")))
		Cls = UClass::TryFindTypeSlow<UClass>(TEXT("A") + ClassName);
	if (!Cls) Cls = FindTypeFallback<UClass>(ClassName);
	if (!Cls && !ClassName.StartsWith(TEXT("U")))
		Cls = FindTypeFallback<UClass>(TEXT("U") + ClassName);
	if (!Cls && !ClassName.StartsWith(TEXT("A")))
		Cls = FindTypeFallback<UClass>(TEXT("A") + ClassName);
	return Cls;
}

static UEnum* FindEnumRobust(const FString& EnumName)
{
	UEnum* Enum = UClass::TryFindTypeSlow<UEnum>(EnumName);
	if (!Enum && !EnumName.StartsWith(TEXT("E")))
		Enum = UClass::TryFindTypeSlow<UEnum>(TEXT("E") + EnumName);
	if (!Enum) Enum = FindTypeFallback<UEnum>(EnumName);
	if (!Enum && !EnumName.StartsWith(TEXT("E")))
		Enum = FindTypeFallback<UEnum>(TEXT("E") + EnumName);
	return Enum;
}

static UScriptStruct* FindStructRobust(const FString& StructName)
{
	UScriptStruct* Struct = UClass::TryFindTypeSlow<UScriptStruct>(StructName);
	if (!Struct && !StructName.StartsWith(TEXT("F")))
		Struct = UClass::TryFindTypeSlow<UScriptStruct>(TEXT("F") + StructName);
	if (!Struct) Struct = FindTypeFallback<UScriptStruct>(StructName);
	if (!Struct && !StructName.StartsWith(TEXT("F")))
		Struct = FindTypeFallback<UScriptStruct>(TEXT("F") + StructName);
	return Struct;
}

static bool ResolveSingleType(const FString& TypeStr, FName& OutCategory, FName& OutSubCategory, TWeakObjectPtr<UObject>& OutSubCategoryObject)
{
	// ---- Primitive types ----
	static const TMap<FString, FName> Primitives = {
		{TEXT("bool"),      UEdGraphSchema_K2::PC_Boolean},
		{TEXT("boolean"),   UEdGraphSchema_K2::PC_Boolean},
		{TEXT("byte"),      UEdGraphSchema_K2::PC_Byte},
		{TEXT("uint8"),     UEdGraphSchema_K2::PC_Byte},
		{TEXT("int"),       UEdGraphSchema_K2::PC_Int},
		{TEXT("integer"),   UEdGraphSchema_K2::PC_Int},
		{TEXT("int32"),     UEdGraphSchema_K2::PC_Int},
		{TEXT("int64"),     UEdGraphSchema_K2::PC_Int64},
		{TEXT("float"),     UEdGraphSchema_K2::PC_Real},
		{TEXT("double"),    UEdGraphSchema_K2::PC_Real},
		{TEXT("real"),      UEdGraphSchema_K2::PC_Real},
		{TEXT("string"),    UEdGraphSchema_K2::PC_String},
		{TEXT("fstring"),   UEdGraphSchema_K2::PC_String},
		{TEXT("name"),      UEdGraphSchema_K2::PC_Name},
		{TEXT("fname"),     UEdGraphSchema_K2::PC_Name},
		{TEXT("text"),      UEdGraphSchema_K2::PC_Text},
		{TEXT("ftext"),     UEdGraphSchema_K2::PC_Text},
		{TEXT("wildcard"),  UEdGraphSchema_K2::PC_Wildcard},
	};

	FString LowerType = TypeStr.ToLower();

	if (const FName* Category = Primitives.Find(LowerType))
	{
		OutCategory = *Category;
		if (*Category == UEdGraphSchema_K2::PC_Real)
			OutSubCategory = TEXT("double");
		return true;
	}

	// ---- Explicit prefix: "class:ClassName" for TSubclassOf / UClass* ----
	if (TypeStr.StartsWith(TEXT("class:"), ESearchCase::IgnoreCase))
	{
		FString ClassName = TypeStr.Mid(6).TrimStartAndEnd();
		UClass* MetaClass = FindClassRobust(ClassName);
		if (!MetaClass) return false;

		OutCategory = UEdGraphSchema_K2::PC_Class;
		OutSubCategoryObject = MetaClass;
		return true;
	}

	// ---- Explicit prefix: "softobject:ClassName" or "soft:ClassName" for TSoftObjectPtr ----
	if (TypeStr.StartsWith(TEXT("softobject:"), ESearchCase::IgnoreCase) ||
		TypeStr.StartsWith(TEXT("soft_object:"), ESearchCase::IgnoreCase) ||
		(TypeStr.StartsWith(TEXT("soft:"), ESearchCase::IgnoreCase) && !TypeStr.StartsWith(TEXT("softclass:"), ESearchCase::IgnoreCase)))
	{
		FString ClassName = TypeStr.Mid(TypeStr.Find(TEXT(":")) + 1).TrimStartAndEnd();
		UClass* ObjClass = FindClassRobust(ClassName);
		if (!ObjClass) return false;

		OutCategory = UEdGraphSchema_K2::PC_SoftObject;
		OutSubCategoryObject = ObjClass;
		return true;
	}

	// ---- Explicit prefix: "softclass:ClassName" for TSoftClassPtr ----
	if (TypeStr.StartsWith(TEXT("softclass:"), ESearchCase::IgnoreCase) ||
		TypeStr.StartsWith(TEXT("soft_class:"), ESearchCase::IgnoreCase))
	{
		FString ClassName = TypeStr.Mid(TypeStr.Find(TEXT(":")) + 1).TrimStartAndEnd();
		UClass* MetaClass = FindClassRobust(ClassName);
		if (!MetaClass) return false;

		OutCategory = UEdGraphSchema_K2::PC_SoftClass;
		OutSubCategoryObject = MetaClass;
		return true;
	}

	// ---- Explicit prefix: "interface:InterfaceName" ----
	if (TypeStr.StartsWith(TEXT("interface:"), ESearchCase::IgnoreCase))
	{
		FString InterfaceName = TypeStr.Mid(10).TrimStartAndEnd();
		UClass* InterfaceClass = FindClassRobust(InterfaceName);
		if (!InterfaceClass || !InterfaceClass->HasAnyClassFlags(CLASS_Interface)) return false;

		OutCategory = UEdGraphSchema_K2::PC_Interface;
		OutSubCategoryObject = InterfaceClass;
		return true;
	}

	// ---- Explicit prefix: "enum:EnumName" ----
	if (TypeStr.StartsWith(TEXT("enum:"), ESearchCase::IgnoreCase))
	{
		FString EnumName = TypeStr.Mid(5).TrimStartAndEnd();
		UEnum* Enum = FindEnumRobust(EnumName);
		if (!Enum) return false;

		OutCategory = UEdGraphSchema_K2::PC_Enum;
		OutSubCategoryObject = Enum;
		return true;
	}

	// ---- Explicit prefix: "struct:StructName" ----
	if (TypeStr.StartsWith(TEXT("struct:"), ESearchCase::IgnoreCase))
	{
		FString StructName = TypeStr.Mid(7).TrimStartAndEnd();
		UScriptStruct* Struct = FindStructRobust(StructName);
		if (!Struct) return false;

		OutCategory = UEdGraphSchema_K2::PC_Struct;
		OutSubCategoryObject = Struct;
		return true;
	}

	// ---- Well-known struct aliases (case-insensitive) ----
	// These ensure commonly used types resolve even before trying generic reflection
	static const TMap<FString, FString> StructAliases = {
		// Math
		{TEXT("vector"),            TEXT("FVector")},
		{TEXT("vector2d"),          TEXT("FVector2D")},
		{TEXT("vector4"),           TEXT("FVector4")},
		{TEXT("rotator"),           TEXT("FRotator")},
		{TEXT("transform"),         TEXT("FTransform")},
		{TEXT("quat"),              TEXT("FQuat")},
		{TEXT("quaternion"),        TEXT("FQuat")},
		{TEXT("intpoint"),          TEXT("FIntPoint")},
		{TEXT("intvector"),         TEXT("FIntVector")},
		// Color
		{TEXT("linearcolor"),       TEXT("FLinearColor")},
		{TEXT("color"),             TEXT("FColor")},
		// Gameplay
		{TEXT("gameplaytag"),       TEXT("FGameplayTag")},
		{TEXT("gameplay_tag"),      TEXT("FGameplayTag")},
		{TEXT("tag"),               TEXT("FGameplayTag")},
		{TEXT("gameplaytagcontainer"), TEXT("FGameplayTagContainer")},
		{TEXT("gameplay_tag_container"), TEXT("FGameplayTagContainer")},
		{TEXT("tagcontainer"),      TEXT("FGameplayTagContainer")},
		{TEXT("tag_container"),     TEXT("FGameplayTagContainer")},
		// DateTime / Timespan
		{TEXT("datetime"),          TEXT("FDateTime")},
		{TEXT("timespan"),          TEXT("FTimespan")},
		// Common types
		{TEXT("guid"),              TEXT("FGuid")},
		{TEXT("softobjectpath"),    TEXT("FSoftObjectPath")},
		{TEXT("softclasspath"),     TEXT("FSoftClassPath")},
		{TEXT("primaryassetid"),    TEXT("FPrimaryAssetId")},
		{TEXT("primaryassettype"),  TEXT("FPrimaryAssetType")},
		// Input
		{TEXT("key"),               TEXT("FKey")},
		{TEXT("inputactionvalue"),  TEXT("FInputActionValue")},
		// Hit / overlap
		{TEXT("hitresult"),         TEXT("FHitResult")},
		// Data Table
		{TEXT("datatable"),         TEXT("FDataTableRowHandle")},
		{TEXT("datatablerowhandle"),TEXT("FDataTableRowHandle")},
	};

	if (const FString* CanonicalName = StructAliases.Find(LowerType))
	{
		UScriptStruct* Struct = FindStructRobust(*CanonicalName);
		if (Struct)
		{
			OutCategory = UEdGraphSchema_K2::PC_Struct;
			OutSubCategoryObject = Struct;
			return true;
		}
	}

	// ---- Try as struct via reflection (with auto F-prefix) ----
	{
		UScriptStruct* Struct = FindStructRobust(TypeStr);
		if (Struct)
		{
			OutCategory = UEdGraphSchema_K2::PC_Struct;
			OutSubCategoryObject = Struct;
			return true;
		}
	}

	// ---- Try as class/object via reflection (with auto A/U prefix) ----
	{
		UClass* ObjClass = FindClassRobust(TypeStr);
		if (ObjClass)
		{
			if (ObjClass->HasAnyClassFlags(CLASS_Interface))
			{
				OutCategory = UEdGraphSchema_K2::PC_Interface;
			}
			else
			{
				OutCategory = UEdGraphSchema_K2::PC_Object;
			}
			OutSubCategoryObject = ObjClass;
			return true;
		}
	}

	// ---- Try as enum via reflection (with auto E-prefix) ----
	{
		UEnum* Enum = FindEnumRobust(TypeStr);
		if (Enum)
		{
			OutCategory = UEdGraphSchema_K2::PC_Enum;
			OutSubCategoryObject = Enum;
			return true;
		}
	}

	// ---- Try loading by asset path (for user-defined structs/enums) ----
	if (TypeStr.Contains(TEXT("/")) || TypeStr.Contains(TEXT(".")))
	{
		UObject* Loaded = LoadObject<UObject>(nullptr, *TypeStr);
		if (UScriptStruct* Struct = Cast<UScriptStruct>(Loaded))
		{
			OutCategory = UEdGraphSchema_K2::PC_Struct;
			OutSubCategoryObject = Struct;
			return true;
		}
		if (UEnum* Enum = Cast<UEnum>(Loaded))
		{
			OutCategory = UEdGraphSchema_K2::PC_Enum;
			OutSubCategoryObject = Enum;
			return true;
		}
		if (UClass* Cls = Cast<UClass>(Loaded))
		{
			OutCategory = UEdGraphSchema_K2::PC_Object;
			OutSubCategoryObject = Cls;
			return true;
		}
		// Also try loading as a Blueprint and getting its generated class
		if (UBlueprint* BP = Cast<UBlueprint>(Loaded))
		{
			if (BP->GeneratedClass)
			{
				OutCategory = UEdGraphSchema_K2::PC_Object;
				OutSubCategoryObject = BP->GeneratedClass;
				return true;
			}
		}
	}

	return false;
}

bool NeoBlueprint::AddVariable(UBlueprint* BP, const FString& VarName, const FString& VarType, const FString& DefaultValue)
{
	if (!BP) return false;

	FEdGraphPinType PinType;
	if (!ResolveTypeString(VarType, PinType))
	{
		return false;
	}

	FBlueprintEditorUtils::AddMemberVariable(BP, FName(*VarName), PinType, DefaultValue);
	return true;
}

bool NeoBlueprint::DeleteNode(UEdGraphNode* Node)
{
	if (!Node) return false;

	UEdGraph* Graph = Node->GetGraph();
	if (!Graph) return false;

	// Refuse to delete nodes in /Engine/Transient — they're orphaned from cross-graph
	// corruption. The schema's BreakNodeLinks calls FindBlueprintForNodeChecked which
	// triggers a fatal assert crash loop on these nodes.
	if (Graph->GetOutermost() == GetTransientPackage())
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("DeleteNode: Refusing to delete orphaned node '%s' in /Engine/Transient"),
			*Node->GetNodeTitle(ENodeTitleType::MenuTitle).ToString());
		return false;
	}

	// Use FBlueprintEditorUtils::RemoveNode for BP graphs (handles breakpoints, pin watches, schema notifications)
	UBlueprint* OwnerBP = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
	if (OwnerBP)
	{
		FBlueprintEditorUtils::RemoveNode(OwnerBP, Node, true);
		return true;
	}

	// Non-BP graphs: follow the engine flow manually
	const UEdGraphSchema* Schema = Graph->GetSchema();
	Graph->Modify();
	Node->Modify();
	if (Schema)
	{
		Schema->BreakNodeLinks(*Node);
	}
	Node->DestroyNode();
	return true;
}

USCS_Node* NeoBlueprint::AddComponent(UBlueprint* BP, const FString& ComponentName, const FString& ComponentClassName, const FString& ParentName)
{
	if (!BP || !BP->SimpleConstructionScript) return nullptr;
	if (!BP->ParentClass || !BP->ParentClass->IsChildOf(AActor::StaticClass())) return nullptr;

	// Resolve component class: try TryFindTypeSlow first, then iterate loaded classes as fallback
	UClass* ComponentClass = UClass::TryFindTypeSlow<UClass>(ComponentClassName);
	if (!ComponentClass)
	{
		FString WithU = TEXT("U") + ComponentClassName;
		ComponentClass = UClass::TryFindTypeSlow<UClass>(WithU);
	}
	// Fallback: scan all loaded component classes by short name match
	if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
	{
		ComponentClass = nullptr;
		FString TargetLower = ComponentClassName.ToLower();
		// Also try without leading 'U' for matching
		FString TargetNoU = TargetLower.StartsWith(TEXT("u")) ? TargetLower.Mid(1) : TargetLower;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (!It->IsChildOf(UActorComponent::StaticClass())) continue;
			if (It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated)) continue;

			FString ShortName = It->GetName().ToLower();
			if (ShortName == TargetLower || ShortName == TargetNoU
				|| ShortName == TargetLower + TEXT("component")
				|| ShortName == TargetNoU + TEXT("component"))
			{
				ComponentClass = *It;
				break;
			}
		}
	}
	if (!ComponentClass)
	{
		return nullptr;
	}

	USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
	FName VarName = SCS->GenerateNewComponentName(ComponentClass, FName(*ComponentName));
	USCS_Node* NewNode = SCS->CreateNode(ComponentClass, VarName);
	if (!NewNode) return nullptr;

	if (!ParentName.IsEmpty())
	{
		USCS_Node* ParentNode = SCS->FindSCSNode(FName(*ParentName));
		if (ParentNode)
		{
			ParentNode->AddChildNode(NewNode);
		}
		else
		{
			SCS->AddNode(NewNode);
		}
	}
	else
	{
		SCS->AddNode(NewNode);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return NewNode;
}

UEdGraph* NeoBlueprint::AddFunctionGraph(UBlueprint* BP, const FString& FunctionName)
{
	if (!BP) return nullptr;

	// Upsert: find existing function graph first
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == FunctionName)
		{
			return Graph;
		}
	}

	UEdGraph* FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
		BP,
		FName(*FunctionName),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass()
	);

	if (!FunctionGraph) return nullptr;

	FBlueprintEditorUtils::AddFunctionGraph<UClass>(
		BP,
		FunctionGraph,
		true,
		nullptr
	);

	return FunctionGraph;
}

bool NeoBlueprint::MoveNode(UEdGraphNode* Node, double X, double Y)
{
	if (!Node) return false;
	Node->NodePosX = static_cast<int32>(X);
	Node->NodePosY = static_cast<int32>(Y);
	return true;
}

UEdGraph* NeoBlueprint::AddEventDispatcher(UBlueprint* BP, const FString& Name, const TArray<FParamDesc>& Params)
{
	if (!BP) return nullptr;

	// Step 1: Create a member variable of type MC_Delegate
	FEdGraphPinType DelegateType;
	DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
	FBlueprintEditorUtils::AddMemberVariable(BP, FName(*Name), DelegateType);

	// Step 2: Create the delegate signature graph
	UEdGraph* SignatureGraph = FBlueprintEditorUtils::CreateNewGraph(
		BP, FName(*Name),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass()
	);
	if (!SignatureGraph) return nullptr;

	SignatureGraph->bEditable = false;

	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	K2Schema->CreateDefaultNodesForGraph(*SignatureGraph);
	K2Schema->CreateFunctionGraphTerminators(*SignatureGraph, static_cast<UClass*>(nullptr));
	K2Schema->AddExtraFunctionFlags(SignatureGraph, FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
	K2Schema->MarkFunctionEntryAsEditable(SignatureGraph, true);

	BP->DelegateSignatureGraphs.Add(SignatureGraph);

	// Step 3: Add parameters to the signature graph's entry node
	if (Params.Num() > 0)
	{
		TArray<UK2Node_FunctionEntry*> EntryNodes;
		SignatureGraph->GetNodesOfClass(EntryNodes);

		if (EntryNodes.Num() > 0)
		{
			UK2Node_FunctionEntry* EntryNode = EntryNodes[0];
			for (const FParamDesc& Param : Params)
			{
				FEdGraphPinType PinType;
				if (ResolveTypeString(Param.Type, PinType))
				{
					EntryNode->CreateUserDefinedPin(FName(*Param.Name), PinType, EGPD_Output);
				}
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return SignatureGraph;
}

bool NeoBlueprint::AddFunctionParam(UBlueprint* BP, const FString& FuncName, const FString& ParamName, const FString& ParamType)
{
	if (!BP) return false;

	UEdGraph* Graph = FindGraph(BP, FuncName);
	if (!Graph) return false;

	TArray<UK2Node_FunctionEntry*> EntryNodes;
	Graph->GetNodesOfClass(EntryNodes);
	if (EntryNodes.Num() == 0) return false;

	FEdGraphPinType PinType;
	if (!ResolveTypeString(ParamType, PinType)) return false;

	EntryNodes[0]->CreateUserDefinedPin(FName(*ParamName), PinType, EGPD_Output);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return true;
}

bool NeoBlueprint::AddFunctionReturn(UBlueprint* BP, const FString& FuncName, const FString& ReturnName, const FString& ReturnType)
{
	if (!BP) return false;

	UEdGraph* Graph = FindGraph(BP, FuncName);
	if (!Graph) return false;

	// Find or create result node
	TArray<UK2Node_FunctionResult*> ResultNodes;
	Graph->GetNodesOfClass(ResultNodes);

	UK2Node_FunctionResult* ResultNode = nullptr;
	if (ResultNodes.Num() > 0)
	{
		ResultNode = ResultNodes[0];
	}
	else
	{
		// Create a result node if one doesn't exist
		ResultNode = NewObject<UK2Node_FunctionResult>(Graph);
		ResultNode->NodePosX = 600;
		ResultNode->NodePosY = 0;
		Graph->AddNode(ResultNode, false, false);
		ResultNode->CreateNewGuid();
		ResultNode->PostPlacedNewNode();
		// Note: do NOT call AllocateDefaultPins() here — PostPlacedNewNode()
		// already syncs with the entry node and calls ReconstructNode() which
		// allocates pins. A second call would create a duplicate exec pin.
	}

	FEdGraphPinType PinType;
	if (!ResolveTypeString(ReturnType, PinType)) return false;

	ResultNode->CreateUserDefinedPin(FName(*ReturnName), PinType, EGPD_Input);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return true;
}

bool NeoBlueprint::SetFunctionFlags(UBlueprint* BP, const FString& FuncName, uint32 AddFlags, uint32 RemoveFlags)
{
	if (!BP) return false;

	UEdGraph* Graph = FindGraph(BP, FuncName);
	if (!Graph) return false;

	TArray<UK2Node_FunctionEntry*> EntryNodes;
	Graph->GetNodesOfClass(EntryNodes);
	if (EntryNodes.Num() == 0) return false;

	UK2Node_FunctionEntry* EntryNode = EntryNodes[0];

	if (AddFlags != 0)
	{
		EntryNode->AddExtraFlags(AddFlags);
	}

	if (RemoveFlags != 0)
	{
		EntryNode->ClearExtraFlags(RemoveFlags);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return true;
}

bool NeoBlueprint::SetVariableProperty(UBlueprint* BP, const FString& VarName, const FString& Key, const FString& Value)
{
	if (!BP) return false;

	FName FVarName(*VarName);
	FString KeyLower = Key.ToLower();

	// Property flags
	uint64* Flags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(BP, FVarName);
	if (!Flags) return false;

	if (KeyLower == TEXT("edit_anywhere"))
	{
		if (Value.ToBool()) *Flags |= CPF_Edit;
		else *Flags &= ~CPF_Edit;
	}
	else if (KeyLower == TEXT("edit_defaults_only"))
	{
		if (Value.ToBool())
		{
			*Flags |= CPF_Edit | CPF_DisableEditOnInstance;
		}
		else
		{
			*Flags &= ~CPF_DisableEditOnInstance;
		}
	}
	else if (KeyLower == TEXT("edit_instance_only"))
	{
		if (Value.ToBool())
		{
			*Flags |= CPF_Edit;
			*Flags &= ~CPF_DisableEditOnInstance;
			*Flags |= CPF_DisableEditOnTemplate;
		}
		else
		{
			*Flags &= ~CPF_DisableEditOnTemplate;
		}
	}
	else if (KeyLower == TEXT("blueprint_read_only"))
	{
		if (Value.ToBool())
		{
			*Flags |= CPF_BlueprintVisible | CPF_BlueprintReadOnly;
		}
		else
		{
			*Flags &= ~CPF_BlueprintReadOnly;
		}
	}
	else if (KeyLower == TEXT("replicated"))
	{
		if (Value.ToBool()) *Flags |= CPF_Net;
		else *Flags &= ~(CPF_Net | CPF_RepNotify);
	}
	else if (KeyLower == TEXT("rep_notify"))
	{
		if (!Value.IsEmpty() && Value != TEXT("false") && Value != TEXT("0"))
		{
			*Flags |= CPF_Net | CPF_RepNotify;
			FBlueprintEditorUtils::SetBlueprintVariableRepNotifyFunc(BP, FVarName, FName(*Value));
		}
	}
	else if (KeyLower == TEXT("transient"))
	{
		if (Value.ToBool()) *Flags |= CPF_Transient;
		else *Flags &= ~CPF_Transient;
	}
	else if (KeyLower == TEXT("save_game"))
	{
		if (Value.ToBool()) *Flags |= CPF_SaveGame;
		else *Flags &= ~CPF_SaveGame;
	}
	else if (KeyLower == TEXT("expose_on_spawn"))
	{
		if (Value.ToBool()) *Flags |= CPF_ExposeOnSpawn;
		else *Flags &= ~CPF_ExposeOnSpawn;
	}
	// Metadata (non-flag properties)
	else if (KeyLower == TEXT("category"))
	{
		FBlueprintEditorUtils::SetBlueprintVariableCategory(BP, FVarName, nullptr, FText::FromString(Value));
	}
	else if (KeyLower == TEXT("tooltip"))
	{
		FBlueprintEditorUtils::SetBlueprintVariableMetaData(BP, FVarName, nullptr, FName(TEXT("tooltip")), Value);
	}
	else
	{
		// Generic metadata fallback
		FBlueprintEditorUtils::SetBlueprintVariableMetaData(BP, FVarName, nullptr, FName(*Key), Value);
		return true;
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return true;
}

// ============================================================================
// Generic property access via reflection
// ============================================================================

bool NeoBlueprint::SetObjectProperty(UObject* Object, const FString& PropertyName, const FString& Value, FString& OutError)
{
	if (!Object)
	{
		OutError = TEXT("null object");
		return false;
	}

	FProperty* Prop = Object->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (!Prop)
	{
		OutError = FuzzyMatchProperty(Object, PropertyName);
		return false;
	}

	// Full edit notification sequence for proper engine integration
	// (component templates, widgets, etc. rely on PostEditChangeProperty)
	Object->Modify();
	Object->PreEditChange(Prop);

	FStringOutputDevice ErrorText;
	const TCHAR* Result = Prop->ImportText_InContainer(*Value, Object, Object, 0, &ErrorText);

	if (!Result || ErrorText.Len() > 0)
	{
		// Close the PreEditChange bracket to prevent corrupted undo state
		FPropertyChangedEvent FailEvent(Prop, EPropertyChangeType::Unspecified);
		Object->PostEditChangeProperty(FailEvent);

		// Build better error for enums — list valid values
		if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			if (UEnum* Enum = EnumProp->GetEnum())
			{
				OutError = FString::Printf(TEXT("invalid value \"%s\". Valid: "), *Value);
				for (int32 i = 0; i < Enum->NumEnums() - 1; i++)
				{
					if (i > 0) OutError += TEXT(", ");
					OutError += Enum->GetNameStringByIndex(i);
				}
				return false;
			}
		}
		else if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			if (UEnum* Enum = ByteProp->Enum)
			{
				OutError = FString::Printf(TEXT("invalid value \"%s\". Valid: "), *Value);
				for (int32 i = 0; i < Enum->NumEnums() - 1; i++)
				{
					if (i > 0) OutError += TEXT(", ");
					OutError += Enum->GetNameStringByIndex(i);
				}
				return false;
			}
		}

		OutError = FString::Printf(TEXT("ImportText failed for \"%s\" (type: %s). Value: \"%s\""),
			*PropertyName, *Prop->GetCPPType(), *Value);
		if (ErrorText.Len() > 0)
		{
			OutError += FString::Printf(TEXT(" Error: %s"), *ErrorText);
		}
		return false;
	}

	FPropertyChangedEvent PropertyEvent(Prop, EPropertyChangeType::ValueSet);
	Object->PostEditChangeProperty(PropertyEvent);
	Object->MarkPackageDirty();

	return true;
}

bool NeoBlueprint::GetObjectProperty(UObject* Object, const FString& PropertyName, FString& OutValue)
{
	if (!Object) return false;

	FProperty* Prop = Object->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (!Prop) return false;

	Prop->ExportTextItem_InContainer(OutValue, Object, nullptr, Object, 0);
	return true;
}

FString NeoBlueprint::FuzzyMatchProperty(UObject* Object, const FString& PropertyName)
{
	if (!Object) return TEXT("property not found");

	FString NameLower = PropertyName.ToLower();
	TArray<TPair<int32, FString>> Scored;

	for (TFieldIterator<FProperty> PropIt(Object->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!Prop || Prop->HasAnyPropertyFlags(CPF_Deprecated)) continue;

		FString PropName = Prop->GetName();
		FString PropLower = PropName.ToLower();

		int32 Score = 0;
		if (PropLower == NameLower) Score = 100;
		else if (PropLower.Contains(NameLower) || NameLower.Contains(PropLower)) Score = 70;
		else
		{
			// Subsequence match
			int32 Pos = 0, Matched = 0;
			for (int32 i = 0; i < NameLower.Len() && Pos < PropLower.Len(); i++)
			{
				for (int32 j = Pos; j < PropLower.Len(); j++)
				{
					if (PropLower[j] == NameLower[i]) { Pos = j + 1; Matched++; break; }
				}
			}
			if (Matched > NameLower.Len() / 2) Score = 30;
		}

		if (Score > 0)
		{
			Scored.Add(TPair<int32, FString>(Score, FString::Printf(TEXT("%s (%s)"), *PropName, *Prop->GetCPPType())));
		}
	}

	Scored.Sort([](const TPair<int32, FString>& A, const TPair<int32, FString>& B) { return A.Key > B.Key; });

	FString Suggestions;
	int32 Max = FMath::Min(Scored.Num(), 8);
	for (int32 i = 0; i < Max; i++)
	{
		if (i > 0) Suggestions += TEXT(", ");
		Suggestions += Scored[i].Value;
	}

	if (Suggestions.IsEmpty())
	{
		return FString::Printf(TEXT("property \"%s\" not found on %s"), *PropertyName, *Object->GetClass()->GetName());
	}

	return FString::Printf(TEXT("property \"%s\" not found. Similar: %s"), *PropertyName, *Suggestions);
}

void NeoBlueprint::ListObjectProperties(UObject* Object, const FString& Filter, TArray<TPair<FString, FString>>& OutProps)
{
	if (!Object) return;

	FString FilterLower = Filter.ToLower();

	for (TFieldIterator<FProperty> PropIt(Object->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!Prop) continue;
		if (Prop->HasAnyPropertyFlags(CPF_Deprecated)) continue;

		FString PropName = Prop->GetName();

		if (!FilterLower.IsEmpty() && !PropName.ToLower().Contains(FilterLower))
		{
			continue;
		}

		FString Value;
		Prop->ExportTextItem_InContainer(Value, Object, nullptr, Object, 0);

		FString TypeStr = Prop->GetCPPType();
		OutProps.Add(TPair<FString, FString>(
			FString::Printf(TEXT("%s (%s)"), *PropName, *TypeStr),
			Value
		));
	}
}

UActorComponent* NeoBlueprint::GetComponentTemplate(UBlueprint* BP, const FString& ComponentName)
{
	if (!BP || !BP->SimpleConstructionScript) return nullptr;

	USCS_Node* Node = BP->SimpleConstructionScript->FindSCSNode(FName(*ComponentName));
	if (!Node) return nullptr;

	return Node->ComponentTemplate;
}

// ============================================================================
// Remove
// ============================================================================

bool NeoBlueprint::RemoveVariable(UBlueprint* BP, const FString& VarName)
{
	if (!BP) return false;

	FName FVarName(*VarName);
	int32 Idx = FBlueprintEditorUtils::FindNewVariableIndex(BP, FVarName);
	if (Idx == INDEX_NONE) return false;

	// If it's an event dispatcher, also remove its signature graph
	if (BP->NewVariables[Idx].VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
	{
		if (UEdGraph* SigGraph = FBlueprintEditorUtils::GetDelegateSignatureGraphByName(BP, FVarName))
		{
			FBlueprintEditorUtils::RemoveGraph(BP, SigGraph);
		}
	}

	FBlueprintEditorUtils::RemoveMemberVariable(BP, FVarName);
	return true;
}

bool NeoBlueprint::RemoveFunction(UBlueprint* BP, const FString& FuncName)
{
	if (!BP) return false;

	UEdGraph* Graph = FindGraph(BP, FuncName);
	if (!Graph) return false;

	FBlueprintEditorUtils::RemoveGraph(BP, Graph);
	return true;
}

bool NeoBlueprint::RemoveComponent(UBlueprint* BP, const FString& ComponentName)
{
	if (!BP || !BP->SimpleConstructionScript) return false;

	USCS_Node* Node = BP->SimpleConstructionScript->FindSCSNode(FName(*ComponentName));
	if (!Node) return false;

	FName VarName = Node->GetVariableName();

	// Remove variable accessor nodes from BP graphs (Get/Set nodes referencing this component)
	FBlueprintEditorUtils::RemoveVariableNodes(BP, VarName);

	// Remove from SCS tree, promoting children to parent
	BP->SimpleConstructionScript->RemoveNodeAndPromoteChildren(Node);

	// Rename the template object so the name can be reused without recompiling
	// (mirrors SSCSEditor.cpp cleanup logic)
	if (Node->ComponentTemplate != nullptr)
	{
		const FName TemplateName = Node->ComponentTemplate->GetFName();
		const FString RemovedName = VarName.ToString() + TEXT("_REMOVED_") + FGuid::NewGuid().ToString();

		Node->ComponentTemplate->Modify();
		Node->ComponentTemplate->Rename(*RemovedName, nullptr, REN_DontCreateRedirectors);

		// Destroy archetype instances on world actors
		TArray<UObject*> ArchetypeInstances;
		Node->ComponentTemplate->GetArchetypeInstances(ArchetypeInstances);
		for (UObject* Instance : ArchetypeInstances)
		{
			if (!Instance->HasAllFlags(RF_ArchetypeObject | RF_InheritableComponentTemplate))
			{
				if (UActorComponent* Comp = Cast<UActorComponent>(Instance)) { Comp->DestroyComponent(); }
				Instance->Rename(*RemovedName, nullptr, REN_DontCreateRedirectors);
			}
		}

		// Clean up child class inherited component templates
		if (BP->GeneratedClass)
		{
			TArray<UClass*> ChildClasses;
			GetDerivedClasses(BP->GeneratedClass, ChildClasses);
			for (UClass* ChildClass : ChildClasses)
			{
				if (UActorComponent* ChildTemplate = Cast<UActorComponent>(static_cast<UObject*>(FindObjectWithOuter(ChildClass, UActorComponent::StaticClass(), TemplateName))))
				{
					ChildTemplate->Modify();
					ChildTemplate->Rename(*RemovedName, nullptr, REN_DontCreateRedirectors);

					ArchetypeInstances.Reset();
					ChildTemplate->GetArchetypeInstances(ArchetypeInstances);
					for (UObject* Instance : ArchetypeInstances)
					{
						if (!Instance->HasAllFlags(RF_ArchetypeObject | RF_InheritableComponentTemplate))
						{
							if (UActorComponent* Comp = Cast<UActorComponent>(Instance)) { Comp->DestroyComponent(); }
							Instance->Rename(*RemovedName, nullptr, REN_DontCreateRedirectors);
						}
					}
				}
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return true;
}

// ============================================================================
// Rename
// ============================================================================

bool NeoBlueprint::RenameVariable(UBlueprint* BP, const FString& OldName, const FString& NewName)
{
	if (!BP) return false;

	FName FOld(*OldName);
	FName FNew(*NewName);

	int32 Idx = FBlueprintEditorUtils::FindNewVariableIndex(BP, FOld);
	if (Idx == INDEX_NONE) return false;

	FBlueprintEditorUtils::RenameMemberVariable(BP, FOld, FNew);
	return true;
}

bool NeoBlueprint::RenameFunction(UBlueprint* BP, const FString& OldName, const FString& NewName)
{
	if (!BP) return false;

	UEdGraph* Graph = FindGraph(BP, OldName);
	if (!Graph) return false;

	FBlueprintEditorUtils::RenameGraph(Graph, NewName);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return true;
}

// ============================================================================
// Local variables
// ============================================================================

bool NeoBlueprint::AddLocalVariable(UBlueprint* BP, const FString& FuncName, const FString& VarName, const FString& VarType, const FString& DefaultValue)
{
	if (!BP) return false;

	UEdGraph* Graph = FindGraph(BP, FuncName);
	if (!Graph) return false;

	FEdGraphPinType PinType;
	if (!ResolveTypeString(VarType, PinType)) return false;

	return FBlueprintEditorUtils::AddLocalVariable(BP, Graph, FName(*VarName), PinType, DefaultValue);
}

bool NeoBlueprint::RemoveLocalVariable(UBlueprint* BP, const FString& FuncName, const FString& VarName)
{
	if (!BP) return false;

	UEdGraph* Graph = FindGraph(BP, FuncName);
	if (!Graph) return false;

	// Get the scope struct from the skeleton class
	UFunction* Func = BP->SkeletonGeneratedClass ? BP->SkeletonGeneratedClass->FindFunctionByName(FName(*FuncName)) : nullptr;
	if (!Func) return false;

	FBlueprintEditorUtils::RemoveLocalVariable(BP, Func, FName(*VarName));
	return true;
}

bool NeoBlueprint::RenameLocalVariable(UBlueprint* BP, const FString& FuncName, const FString& OldName, const FString& NewName)
{
	if (!BP) return false;

	UFunction* Func = BP->SkeletonGeneratedClass ? BP->SkeletonGeneratedClass->FindFunctionByName(FName(*FuncName)) : nullptr;
	if (!Func) return false;

	FBPVariableDescription* Var = FBlueprintEditorUtils::FindLocalVariable(BP, Func, FName(*OldName));
	if (!Var) return false;

	FBlueprintEditorUtils::RenameLocalVariable(BP, Func, FName(*OldName), FName(*NewName));
	return true;
}

// ============================================================================
// Change variable type
// ============================================================================

bool NeoBlueprint::ChangeVariableType(UBlueprint* BP, const FString& VarName, const FString& NewType)
{
	if (!BP) return false;

	FName VarFName(*VarName);
	int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(BP, VarFName);
	if (VarIndex == INDEX_NONE) return false;

	FEdGraphPinType NewPinType;
	if (!ResolveTypeString(NewType, NewPinType)) return false;

	FBPVariableDescription& Variable = BP->NewVariables[VarIndex];
	if (Variable.VarType == NewPinType) return true; // Already the right type

	BP->Modify();

	// Handle AActor reference flag management (matches engine ChangeMemberVariableType behavior)
	if (NewPinType.PinCategory == UEdGraphSchema_K2::PC_Object || NewPinType.PinCategory == UEdGraphSchema_K2::PC_Interface)
	{
		if (NewPinType.PinSubCategoryObject.IsValid())
		{
			const UClass* ClassObject = Cast<UClass>(NewPinType.PinSubCategoryObject.Get());
			if (ClassObject && ClassObject->IsChildOf(AActor::StaticClass()))
			{
				Variable.PropertyFlags |= CPF_DisableEditOnTemplate;
			}
			else
			{
				Variable.PropertyFlags &= ~CPF_DisableEditOnTemplate;
			}
		}
		else
		{
			return false; // Invalid object type
		}
	}
	else
	{
		Variable.PropertyFlags &= ~CPF_DisableEditOnTemplate;
	}

	// Update friendly name if transitioning to/from boolean
	const bool bBecameBoolean = Variable.VarType.PinCategory != UEdGraphSchema_K2::PC_Boolean && NewPinType.PinCategory == UEdGraphSchema_K2::PC_Boolean;
	const bool bBecameNotBoolean = Variable.VarType.PinCategory == UEdGraphSchema_K2::PC_Boolean && NewPinType.PinCategory != UEdGraphSchema_K2::PC_Boolean;
	if (bBecameBoolean || bBecameNotBoolean)
	{
		Variable.FriendlyName = FName::NameToDisplayString(Variable.VarName.ToString(), bBecameBoolean);
	}

	Variable.VarType = NewPinType;

	// Sets/maps cannot be replicated — clear replication if switching to those types
	if (Variable.VarType.IsSet() || Variable.VarType.IsMap())
	{
		if (Variable.RepNotifyFunc != NAME_None || (Variable.PropertyFlags & CPF_Net) || (Variable.PropertyFlags & CPF_RepNotify))
		{
			Variable.PropertyFlags &= ~CPF_Net;
			Variable.PropertyFlags &= ~CPF_RepNotify;
			Variable.RepNotifyFunc = NAME_None;
			Variable.ReplicationCondition = COND_None;
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	// Reconstruct variable nodes to update pins (manual iteration — GetNodesForVariable is protected)
	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		for (UEdGraphNode* GraphNode : Graph->Nodes)
		{
			if (UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(GraphNode))
			{
				if (VarNode->GetVarName() == VarFName)
				{
					K2Schema->ReconstructNode(*VarNode, true);
				}
			}
		}
	}

	return true;
}

bool NeoBlueprint::ChangeLocalVariableType(UBlueprint* BP, const FString& FuncName, const FString& VarName, const FString& NewType)
{
	if (!BP) return false;

	UFunction* Func = BP->SkeletonGeneratedClass ? BP->SkeletonGeneratedClass->FindFunctionByName(FName(*FuncName)) : nullptr;
	if (!Func) return false;

	FBPVariableDescription* Var = FBlueprintEditorUtils::FindLocalVariable(BP, Func, FName(*VarName));
	if (!Var) return false;

	FEdGraphPinType NewPinType;
	if (!ResolveTypeString(NewType, NewPinType)) return false;

	if (Var->VarType == NewPinType) return true;

	// Direct modification — the engine's ChangeLocalVariableType shows a dialog, so we replicate the core logic
	BP->Modify();
	Var->VarType = NewPinType;

	// Reconstruct local variable nodes (manual iteration — GetNodesForVariable is protected)
	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);
	const FName LocalVarFName(*VarName);
	for (UEdGraph* Graph : AllGraphs)
	{
		for (UEdGraphNode* GraphNode : Graph->Nodes)
		{
			if (UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(GraphNode))
			{
				if (VarNode->GetVarName() == LocalVarFName)
				{
					K2Schema->ReconstructNode(*VarNode, true);
				}
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return true;
}

// ============================================================================
// Reorder variables
// ============================================================================

bool NeoBlueprint::MoveVariableBefore(UBlueprint* BP, const FString& VarName, const FString& TargetVarName)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	if (!BP || !BP->SkeletonGeneratedClass) return false;
	return FBlueprintEditorUtils::MoveVariableBeforeVariable(BP, BP->SkeletonGeneratedClass, FName(*VarName), FName(*TargetVarName), false);
#else
	return false;
#endif
}

bool NeoBlueprint::MoveVariableAfter(UBlueprint* BP, const FString& VarName, const FString& TargetVarName)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	if (!BP || !BP->SkeletonGeneratedClass) return false;
	return FBlueprintEditorUtils::MoveVariableAfterVariable(BP, BP->SkeletonGeneratedClass, FName(*VarName), FName(*TargetVarName), false);
#else
	return false;
#endif
}

// ============================================================================
// Query
// ============================================================================

bool NeoBlueprint::IsVariableUsed(UBlueprint* BP, const FString& VarName)
{
	if (!BP) return false;
	return FBlueprintEditorUtils::IsVariableUsed(BP, FName(*VarName));
}

// ============================================================================
// Interfaces
// ============================================================================

UBlueprint* NeoBlueprint::CreateInterfaceBlueprint(const FString& AssetPath)
{
	FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);
	FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);

	UPackage* Package = CreatePackage(*AssetPath);
	if (!Package) return nullptr;

	UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
		UInterface::StaticClass(),
		Package,
		FName(*AssetName),
		BPTYPE_Interface,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);

	if (NewBP)
	{
		FAssetRegistryModule::AssetCreated(NewBP);
		NewBP->MarkPackageDirty();
	}

	return NewBP;
}

static UClass* FindInterfaceClass(const FString& InterfaceName)
{
	// Try common short names with U prefix for interface classes
	FString TestName = InterfaceName;

	// Try as-is first
	UClass* Found = FindFirstObject<UClass>(*TestName, EFindFirstObjectOptions::NativeFirst);
	if (Found && Found->IsChildOf(UInterface::StaticClass())) return Found;

	// Try with U prefix (UMyInterface)
	if (!TestName.StartsWith(TEXT("U")))
	{
		Found = FindFirstObject<UClass>(*(TEXT("U") + TestName), EFindFirstObjectOptions::NativeFirst);
		if (Found && Found->IsChildOf(UInterface::StaticClass())) return Found;
	}

	// Try loading as a Blueprint Interface asset path
	UBlueprint* InterfaceBP = LoadObject<UBlueprint>(nullptr, *InterfaceName);
	if (InterfaceBP && InterfaceBP->BlueprintType == BPTYPE_Interface && InterfaceBP->GeneratedClass)
	{
		return InterfaceBP->GeneratedClass;
	}

	// Try with /Game/ prefix
	if (!InterfaceName.StartsWith(TEXT("/")))
	{
		InterfaceBP = LoadObject<UBlueprint>(nullptr, *(TEXT("/Game/") + InterfaceName));
		if (InterfaceBP && InterfaceBP->BlueprintType == BPTYPE_Interface && InterfaceBP->GeneratedClass)
		{
			return InterfaceBP->GeneratedClass;
		}
	}

	// Try searching loaded objects by short name (fast, covers in-session created assets)
	if (!InterfaceName.StartsWith(TEXT("/")))
	{
		for (TObjectIterator<UBlueprint> It; It; ++It)
		{
			UBlueprint* BP = *It;
			if (BP && BP->GetName() == InterfaceName && BP->BlueprintType == BPTYPE_Interface && BP->GeneratedClass)
			{
				return BP->GeneratedClass;
			}
		}

		// Try Asset Registry for assets not yet loaded
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		AR.GetAssetsByPackageName(FName(*(TEXT("/Game/") + InterfaceName)), Assets, true);
		if (Assets.Num() == 0)
		{
			// Search all paths — the asset might be in a subdirectory
			FARFilter Filter;
			Filter.bRecursiveClasses = true;
			Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
			AR.GetAssets(Filter, Assets);
		}

		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetName.ToString() == InterfaceName)
			{
				InterfaceBP = Cast<UBlueprint>(Asset.GetAsset());
				if (InterfaceBP && InterfaceBP->BlueprintType == BPTYPE_Interface && InterfaceBP->GeneratedClass)
				{
					return InterfaceBP->GeneratedClass;
				}
			}
		}
	}

	return nullptr;
}

bool NeoBlueprint::AddInterface(UBlueprint* BP, const FString& InterfaceName)
{
	if (!BP) return false;

	UClass* InterfaceClass = FindInterfaceClass(InterfaceName);
	if (!InterfaceClass) return false;

	// Check if already implemented
	if (FBlueprintEditorUtils::ImplementsInterface(BP, false, InterfaceClass))
	{
		return false;
	}

	FTopLevelAssetPath InterfacePath = InterfaceClass->GetClassPathName();
	return FBlueprintEditorUtils::ImplementNewInterface(BP, InterfacePath);
}

bool NeoBlueprint::RemoveInterface(UBlueprint* BP, const FString& InterfaceName, bool bPreserveFunctions)
{
	if (!BP) return false;

	UClass* InterfaceClass = FindInterfaceClass(InterfaceName);
	if (!InterfaceClass) return false;

	if (!FBlueprintEditorUtils::ImplementsInterface(BP, false, InterfaceClass))
	{
		return false;
	}

	FTopLevelAssetPath InterfacePath = InterfaceClass->GetClassPathName();
	FBlueprintEditorUtils::RemoveInterface(BP, InterfacePath, bPreserveFunctions);
	return true;
}

// ============================================================================
// Macros
// ============================================================================

UEdGraph* NeoBlueprint::AddMacroGraph(UBlueprint* BP, const FString& MacroName)
{
	if (!BP) return nullptr;

	UEdGraph* MacroGraph = FBlueprintEditorUtils::CreateNewGraph(
		BP,
		FName(*MacroName),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass()
	);

	if (!MacroGraph) return nullptr;

	FBlueprintEditorUtils::AddMacroGraph(BP, MacroGraph, /*bIsUserCreated=*/true, /*SignatureFromClass=*/nullptr);
	return MacroGraph;
}

// ============================================================================
// Custom Events (upsert — find or create, then apply params/flags)
// ============================================================================

UK2Node_CustomEvent* NeoBlueprint::FindCustomEvent(UBlueprint* BP, const FString& EventName)
{
	if (!BP) return nullptr;

	FName TargetName(*EventName);
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node);
			if (CE && CE->CustomFunctionName == TargetName)
			{
				return CE;
			}
		}
	}
	return nullptr;
}

UK2Node_CustomEvent* NeoBlueprint::AddCustomEvent(UBlueprint* BP, const FString& EventName, const TArray<FParamDesc>& Params)
{
	if (!BP) return nullptr;

	// Upsert: find existing or create new
	UK2Node_CustomEvent* EventNode = FindCustomEvent(BP, EventName);
	bool bExisting = (EventNode != nullptr);

	if (!bExisting)
	{
		// Find the EventGraph (first ubergraph page)
		UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
		if (!EventGraph) return nullptr;

		EventNode = NewObject<UK2Node_CustomEvent>(EventGraph);
		EventNode->CustomFunctionName = FName(*EventName);
		EventNode->SetFlags(RF_Transactional);

		EventGraph->Modify();
		EventGraph->AddNode(EventNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		EventNode->CreateNewGuid();
		EventNode->PostPlacedNewNode();
		EventNode->AllocateDefaultPins();
	}
	else if (Params.Num() > 0)
	{
		// Remove existing user-defined pins (keep exec/delegate pins)
		TArray<TSharedPtr<FUserPinInfo>> OldPins = EventNode->UserDefinedPins;
		for (int32 i = OldPins.Num() - 1; i >= 0; i--)
		{
			EventNode->RemoveUserDefinedPin(OldPins[i]);
		}
	}

	// Add parameters as output pins
	for (const FParamDesc& Param : Params)
	{
		FEdGraphPinType PinType;
		if (ResolveTypeString(Param.Type, PinType))
		{
			EventNode->CreateUserDefinedPin(FName(*Param.Name), PinType, EGPD_Output);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return EventNode;
}

// ============================================================================
// Timelines (upsert — find or create, then apply properties/tracks)
// ============================================================================

// Shared helper: add a single track to a template
static void AddTrackToTemplate(UBlueprint* BP, UTimelineTemplate* Template, const NeoBlueprint::FTimelineTrackDesc& TrackDesc)
{
	FString TypeLower = TrackDesc.Type.ToLower();

	if (TypeLower == TEXT("float"))
	{
		UCurveFloat* Curve = NewObject<UCurveFloat>(BP, NAME_None, RF_Transactional);
		for (const auto& Key : TrackDesc.Keys)
		{
			Curve->FloatCurve.AddKey(Key.Key, FCString::Atof(*Key.Value));
		}
		Curve->FloatCurve.AutoSetTangents();

		FTTFloatTrack Track;
		Track.SetTrackName(FName(*TrackDesc.Name), Template);
		Track.CurveFloat = Curve;
		Track.bIsExternalCurve = false;
		Template->FloatTracks.Add(Track);
		Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_FloatInterp, Template->FloatTracks.Num() - 1));
	}
	else if (TypeLower == TEXT("vector"))
	{
		UCurveVector* Curve = NewObject<UCurveVector>(BP, NAME_None, RF_Transactional);
		for (const auto& Key : TrackDesc.Keys)
		{
			FVector Vec(ForceInitToZero);
			TArray<FString> Parts;
			Key.Value.ParseIntoArray(Parts, TEXT(","));
			if (Parts.Num() >= 3)
			{
				Vec.X = FCString::Atof(*Parts[0]);
				Vec.Y = FCString::Atof(*Parts[1]);
				Vec.Z = FCString::Atof(*Parts[2]);
			}
			Curve->FloatCurves[0].AddKey(Key.Key, Vec.X);
			Curve->FloatCurves[1].AddKey(Key.Key, Vec.Y);
			Curve->FloatCurves[2].AddKey(Key.Key, Vec.Z);
		}
		for (int32 i = 0; i < 3; i++) Curve->FloatCurves[i].AutoSetTangents();

		FTTVectorTrack Track;
		Track.SetTrackName(FName(*TrackDesc.Name), Template);
		Track.CurveVector = Curve;
		Track.bIsExternalCurve = false;
		Template->VectorTracks.Add(Track);
		Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_VectorInterp, Template->VectorTracks.Num() - 1));
	}
	else if (TypeLower == TEXT("color") || TypeLower == TEXT("linearcolor"))
	{
		UCurveLinearColor* Curve = NewObject<UCurveLinearColor>(BP, NAME_None, RF_Transactional);
		for (const auto& Key : TrackDesc.Keys)
		{
			TArray<FString> Parts;
			Key.Value.ParseIntoArray(Parts, TEXT(","));
			float R = Parts.Num() >= 1 ? FCString::Atof(*Parts[0]) : 0.f;
			float G = Parts.Num() >= 2 ? FCString::Atof(*Parts[1]) : 0.f;
			float B = Parts.Num() >= 3 ? FCString::Atof(*Parts[2]) : 0.f;
			float A = Parts.Num() >= 4 ? FCString::Atof(*Parts[3]) : 1.f;
			Curve->FloatCurves[0].AddKey(Key.Key, R);
			Curve->FloatCurves[1].AddKey(Key.Key, G);
			Curve->FloatCurves[2].AddKey(Key.Key, B);
			Curve->FloatCurves[3].AddKey(Key.Key, A);
		}
		for (int32 i = 0; i < 4; i++) Curve->FloatCurves[i].AutoSetTangents();

		FTTLinearColorTrack Track;
		Track.SetTrackName(FName(*TrackDesc.Name), Template);
		Track.CurveLinearColor = Curve;
		Track.bIsExternalCurve = false;
		Template->LinearColorTracks.Add(Track);
		Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_LinearColorInterp, Template->LinearColorTracks.Num() - 1));
	}
	else if (TypeLower == TEXT("event"))
	{
		UCurveFloat* Curve = NewObject<UCurveFloat>(BP, NAME_None, RF_Transactional);
		for (const auto& Key : TrackDesc.Keys)
		{
			Curve->FloatCurve.AddKey(Key.Key, 0.0f);
		}

		FTTEventTrack Track;
		Track.SetTrackName(FName(*TrackDesc.Name), Template);
		Track.CurveKeys = Curve;
		Track.bIsExternalCurve = false;
		Template->EventTracks.Add(Track);
		Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_Event, Template->EventTracks.Num() - 1));
	}
}

UK2Node_Timeline* NeoBlueprint::FindTimelineNode(UBlueprint* BP, const FString& TimelineName)
{
	if (!BP) return nullptr;

	FName TLName(*TimelineName);
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_Timeline* TLNode = Cast<UK2Node_Timeline>(Node);
			if (TLNode && TLNode->TimelineName == TLName)
			{
				return TLNode;
			}
		}
	}
	return nullptr;
}

UK2Node_Timeline* NeoBlueprint::AddTimeline(UBlueprint* BP, const FString& TimelineName, float Length,
	bool bAutoPlay, bool bLoop, const TArray<FTimelineTrackDesc>& Tracks)
{
	if (!BP) return nullptr;

	FName TLName(*TimelineName);

	// Upsert: find existing template or create new
	UTimelineTemplate* Template = BP->FindTimelineTemplateByVariableName(TLName);
	UK2Node_Timeline* TimelineNode = nullptr;
	bool bExisting = (Template != nullptr);

	if (!bExisting)
	{
		// Find EventGraph
		UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
		if (!EventGraph) return nullptr;

		Template = FBlueprintEditorUtils::AddNewTimeline(BP, TLName);
		if (!Template) return nullptr;

		// Create the timeline node in EventGraph (standard node creation flow)
		TimelineNode = NewObject<UK2Node_Timeline>(EventGraph);
		TimelineNode->CreateNewGuid();
		TimelineNode->TimelineName = TLName;
		TimelineNode->TimelineGuid = Template->TimelineGuid;
		TimelineNode->SetFlags(RF_Transactional);
		TimelineNode->PostPlacedNewNode();
		TimelineNode->AllocateDefaultPins();
		EventGraph->AddNode(TimelineNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
	}
	else
	{
		TimelineNode = FindTimelineNode(BP, TimelineName);
	}

	// Update template properties
	Template->TimelineLength = Length;
	Template->bAutoPlay = bAutoPlay;
	Template->bLoop = bLoop;

	// Add tracks (appends — doesn't remove existing tracks)
	for (const FTimelineTrackDesc& TrackDesc : Tracks)
	{
		AddTrackToTemplate(BP, Template, TrackDesc);
	}

	// Sync node properties and reconstruct pins
	if (TimelineNode)
	{
		TimelineNode->bAutoPlay = bAutoPlay;
		TimelineNode->bLoop = bLoop;
		TimelineNode->ReconstructNode();
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return TimelineNode;
}

bool NeoBlueprint::AddTimelineTrack(UBlueprint* BP, const FString& TimelineName, const FTimelineTrackDesc& Track)
{
	if (!BP) return false;

	UTimelineTemplate* Template = BP->FindTimelineTemplateByVariableName(FName(*TimelineName));
	if (!Template) return false;

	AddTrackToTemplate(BP, Template, Track);

	// Reconstruct the node to add new pins
	UK2Node_Timeline* Node = FindTimelineNode(BP, TimelineName);
	if (Node)
	{
		Node->ReconstructNode();
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return true;
}

bool NeoBlueprint::RemoveTimeline(UBlueprint* BP, const FString& TimelineName)
{
	if (!BP) return false;

	UTimelineTemplate* Template = BP->FindTimelineTemplateByVariableName(FName(*TimelineName));
	if (!Template) return false;

	FBlueprintEditorUtils::RemoveTimeline(BP, Template);
	return true;
}

// ============================================================================
// Generic Asset Creation
// ============================================================================

// Resolve a class name (short or full path) to a UClass*
static UClass* ResolveClassName(const FString& ClassName)
{
	if (ClassName.IsEmpty()) return nullptr;

	// Common aliases
	static const TMap<FString, FString> ClassAliases = {
		// Regular Blueprint parent classes
		{TEXT("actor"), TEXT("/Script/Engine.Actor")},
		{TEXT("pawn"), TEXT("/Script/Engine.Pawn")},
		{TEXT("character"), TEXT("/Script/Engine.Character")},
		{TEXT("playercontroller"), TEXT("/Script/Engine.PlayerController")},
		{TEXT("gamemode"), TEXT("/Script/Engine.GameModeBase")},
		{TEXT("gamemodebase"), TEXT("/Script/Engine.GameModeBase")},
		{TEXT("hud"), TEXT("/Script/Engine.HUD")},
		{TEXT("actorcomponent"), TEXT("/Script/Engine.ActorComponent")},
		{TEXT("scenecomponent"), TEXT("/Script/Engine.SceneComponent")},
		{TEXT("object"), TEXT("/Script/CoreUObject.Object")},
		// AnimBP parent classes
		{TEXT("animinstance"), TEXT("/Script/Engine.AnimInstance")},
		// Data assets
		{TEXT("dataasset"), TEXT("/Script/Engine.DataAsset")},
		{TEXT("primarydataasset"), TEXT("/Script/Engine.PrimaryDataAsset")},
	};

	FString LowerName = ClassName.ToLower();
	if (const FString* FullPath = ClassAliases.Find(LowerName))
	{
		UClass* Found = LoadObject<UClass>(nullptr, **FullPath);
		if (Found) return Found;
	}

	// Try exact match
	UClass* Found = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	if (Found) return Found;

	// Try loading by path
	Found = LoadObject<UClass>(nullptr, *ClassName);
	if (Found) return Found;

	// Try with common prefixes
	if (!ClassName.StartsWith(TEXT("U")) && !ClassName.StartsWith(TEXT("A")))
	{
		Found = FindFirstObject<UClass>(*(TEXT("U") + ClassName), EFindFirstObjectOptions::NativeFirst);
		if (Found) return Found;
		Found = FindFirstObject<UClass>(*(TEXT("A") + ClassName), EFindFirstObjectOptions::NativeFirst);
		if (Found) return Found;
	}

	return nullptr;
}

// Known asset type aliases -> UClass path for supported class
static const TMap<FString, FString>& GetAssetTypeAliases()
{
	static const TMap<FString, FString> Aliases = {
		// Blueprints
		{TEXT("blueprint"), TEXT("/Script/Engine.Blueprint")},
		{TEXT("bp"), TEXT("/Script/Engine.Blueprint")},
		{TEXT("animblueprint"), TEXT("/Script/Engine.AnimBlueprint")},
		{TEXT("animbp"), TEXT("/Script/Engine.AnimBlueprint")},
		{TEXT("widgetblueprint"), TEXT("/Script/UMGEditor.WidgetBlueprint")},
		{TEXT("widget"), TEXT("/Script/UMGEditor.WidgetBlueprint")},
		// Data
		{TEXT("datatable"), TEXT("/Script/Engine.DataTable")},
		{TEXT("curvetable"), TEXT("/Script/Engine.CurveTable")},
		{TEXT("dataasset"), TEXT("/Script/Engine.DataAsset")},
		{TEXT("stringtable"), TEXT("/Script/Engine.StringTable")},
		// Structures & Enums
		{TEXT("userdefinedenum"), TEXT("/Script/Engine.UserDefinedEnum")},
		{TEXT("enum"), TEXT("/Script/Engine.UserDefinedEnum")},
		{TEXT("userdefinedstruct"), TEXT("/Script/Engine.UserDefinedStruct")},
		{TEXT("struct"), TEXT("/Script/Engine.UserDefinedStruct")},
		// Animation
		{TEXT("animsequence"), TEXT("/Script/Engine.AnimSequence")},
		{TEXT("animmontage"), TEXT("/Script/Engine.AnimMontage")},
		{TEXT("animcomposite"), TEXT("/Script/Engine.AnimComposite")},
		{TEXT("blendspace"), TEXT("/Script/Engine.BlendSpace")},
		{TEXT("blendspace1d"), TEXT("/Script/Engine.BlendSpace1D")},
		{TEXT("aimoffsetblendspace"), TEXT("/Script/Engine.AimOffsetBlendSpace")},
		{TEXT("poseasset"), TEXT("/Script/Engine.PoseAsset")},
		{TEXT("skeleton"), TEXT("/Script/Engine.Skeleton")},
		// Materials
		{TEXT("material"), TEXT("/Script/Engine.Material")},
		{TEXT("materialinstance"), TEXT("/Script/Engine.MaterialInstanceConstant")},
		{TEXT("materialfunction"), TEXT("/Script/Engine.MaterialFunction")},
		// Physics
		{TEXT("physicsasset"), TEXT("/Script/Engine.PhysicsAsset")},
		{TEXT("physicsmaterial"), TEXT("/Script/PhysicsCore.PhysicalMaterial")},
		// Curves
		{TEXT("curvefloat"), TEXT("/Script/Engine.CurveFloat")},
		{TEXT("curvevector"), TEXT("/Script/Engine.CurveVector")},
		{TEXT("curvelinearcolor"), TEXT("/Script/Engine.CurveLinearColor")},
		// AI
		{TEXT("behaviortree"), TEXT("/Script/AIModule.BehaviorTree")},
		{TEXT("bt"), TEXT("/Script/AIModule.BehaviorTree")},
		{TEXT("blackboarddata"), TEXT("/Script/AIModule.BlackboardData")},
		{TEXT("blackboard"), TEXT("/Script/AIModule.BlackboardData")},
		{TEXT("statetree"), TEXT("/Script/StateTreeModule.StateTree")},
		// EQS
		{TEXT("environmentquery"), TEXT("/Script/AIModule.EnvQuery")},
		{TEXT("envquery"), TEXT("/Script/AIModule.EnvQuery")},
		{TEXT("eqs"), TEXT("/Script/AIModule.EnvQuery")},
		// Sequencer
		{TEXT("levelsequence"), TEXT("/Script/LevelSequence.LevelSequence")},
		{TEXT("materialinstanceconstant"), TEXT("/Script/Engine.MaterialInstanceConstant")},
		// IK
		{TEXT("ikrig"), TEXT("/Script/IKRig.IKRigDefinition")},
		{TEXT("ikretargeter"), TEXT("/Script/IKRig.IKRetargeter")},
		// Editor utilities
		{TEXT("editorutilityblueprint"), TEXT("/Script/Blutility.EditorUtilityBlueprint")},
		{TEXT("editorutilitybp"), TEXT("/Script/Blutility.EditorUtilityBlueprint")},
		{TEXT("editorutilitywidgetblueprint"), TEXT("/Script/Blutility.EditorUtilityWidgetBlueprint")},
		{TEXT("editorutilitywidget"), TEXT("/Script/Blutility.EditorUtilityWidgetBlueprint")},
		// ControlRig
		{TEXT("controlrigblueprint"), TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint")},
		{TEXT("controlrig"), TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint")},
		// Enhanced Input
		{TEXT("inputaction"), TEXT("/Script/EnhancedInput.InputAction")},
		{TEXT("inputmappingcontext"), TEXT("/Script/EnhancedInput.InputMappingContext")},
		// Chooser
		{TEXT("choosertable"), TEXT("/Script/Chooser.ChooserTable")},
		{TEXT("chooser"), TEXT("/Script/Chooser.ChooserTable")},
		// Sound
		{TEXT("soundcue"), TEXT("/Script/Engine.SoundCue")},
		// Paper2D
		{TEXT("papersprite"), TEXT("/Script/Paper2D.PaperSprite")},
		{TEXT("paperflipbook"), TEXT("/Script/Paper2D.PaperFlipbook")},
		{TEXT("papertileset"), TEXT("/Script/Paper2D.PaperTileSet")},
		{TEXT("papertilemap"), TEXT("/Script/Paper2D.PaperTileMap")},
		// PCG
		{TEXT("pcggraph"), TEXT("/Script/PCG.PCGGraph")},
		// MetaSound
		{TEXT("metasoundsource"), TEXT("/Script/MetasoundEngine.MetaSoundSource")},
		{TEXT("metasound"), TEXT("/Script/MetasoundEngine.MetaSoundSource")},
		// SmartObjects
		{TEXT("smartobjectdefinition"), TEXT("/Script/SmartObjectsModule.SmartObjectDefinition")},
		{TEXT("smartobject"), TEXT("/Script/SmartObjectsModule.SmartObjectDefinition")},
		// Foliage / Landscape
		{TEXT("foliagetype"), TEXT("/Script/Foliage.FoliageType")},
		{TEXT("landscapegrasstype"), TEXT("/Script/Landscape.LandscapeGrassType")},
		// Dataflow / Cloth / Outfit
		{TEXT("dataflow"), TEXT("/Script/DataflowEngine.Dataflow")},
		{TEXT("chaosclothasset"), TEXT("/Script/ChaosClothAssetEngine.ChaosClothAsset")},
		{TEXT("clothasset"), TEXT("/Script/ChaosClothAssetEngine.ChaosClothAsset")},
		{TEXT("chaosoutfitasset"), TEXT("/Script/ChaosOutfitAssetEngine.ChaosOutfitAsset")},
		{TEXT("outfitasset"), TEXT("/Script/ChaosOutfitAssetEngine.ChaosOutfitAsset")},
		// MetaHuman wardrobe
		{TEXT("metahumanwardrobeitem"), TEXT("/Script/MetaHumanCharacterPalette.MetaHumanWardrobeItem")},
		{TEXT("wardrobeitem"), TEXT("/Script/MetaHumanCharacterPalette.MetaHumanWardrobeItem")},
		// HLOD
		{TEXT("hlodlayer"), TEXT("/Script/Engine.HLODLayer")},
		// Movie Render Pipeline
		{TEXT("moviepipelineprimaryconfig"), TEXT("/Script/MovieRenderPipelineCore.MoviePipelinePrimaryConfig")},
		{TEXT("moviegraphconfig"), TEXT("/Script/MovieRenderPipelineCore.MovieGraphConfig")},
	};
	return Aliases;
}

// Known required properties for common factory types
struct FFactoryRequirement
{
	FString PropertyName;
	FString Description;
	bool bRequired;
};

static TArray<FFactoryRequirement> GetKnownRequirements(const FString& AssetTypeLower)
{
	TArray<FFactoryRequirement> Reqs;

	if (AssetTypeLower == TEXT("animblueprint") || AssetTypeLower == TEXT("animbp"))
	{
		Reqs.Add({TEXT("ParentClass"), TEXT("TSubclassOf<UAnimInstance> — e.g. \"AnimInstance\" or custom subclass"), true});
		Reqs.Add({TEXT("TargetSkeleton"), TEXT("USkeleton asset path — e.g. \"/Game/Characters/SK_Mesh\""), false});
		Reqs.Add({TEXT("bTemplate"), TEXT("bool — set true to skip TargetSkeleton"), false});
	}
	else if (AssetTypeLower == TEXT("blueprint") || AssetTypeLower == TEXT("bp"))
	{
		Reqs.Add({TEXT("ParentClass"), TEXT("Parent class — e.g. \"Actor\", \"Character\", full path. Defaults to Actor"), false});
	}
	else if (AssetTypeLower == TEXT("datatable"))
	{
		Reqs.Add({TEXT("Struct"), TEXT("UScriptStruct — row struct e.g. \"/Script/MyGame.FMyRow\""), true});
	}
	else if (AssetTypeLower == TEXT("dataasset"))
	{
		Reqs.Add({TEXT("DataAssetClass"), TEXT("TSubclassOf<UDataAsset> — e.g. \"PrimaryDataAsset\""), true});
	}
	else if (AssetTypeLower == TEXT("animsequence") || AssetTypeLower == TEXT("animmontage") ||
		AssetTypeLower == TEXT("animcomposite") || AssetTypeLower == TEXT("blendspace") ||
		AssetTypeLower == TEXT("blendspace1d") || AssetTypeLower == TEXT("poseasset"))
	{
		Reqs.Add({TEXT("TargetSkeleton"), TEXT("USkeleton asset path — e.g. \"/Game/Characters/SK_Mesh\""), true});
	}
	else if (AssetTypeLower == TEXT("physicsasset") || AssetTypeLower == TEXT("skeleton"))
	{
		Reqs.Add({TEXT("TargetSkeletalMesh"), TEXT("USkeletalMesh asset path"), true});
	}
	else if (AssetTypeLower == TEXT("materialinstance") || AssetTypeLower == TEXT("materialinstanceconstant"))
	{
		Reqs.Add({TEXT("ParentMaterial"), TEXT("Parent material path — e.g. \"/Game/Materials/M_Base\""), false});
	}
	else if (AssetTypeLower == TEXT("functionlibrary") || AssetTypeLower == TEXT("macrolibrary") ||
		AssetTypeLower == TEXT("animlayerinterface"))
	{
		// Handled as special Blueprint sub-types, no factory requirements
	}
	else if (AssetTypeLower == TEXT("editorutilityblueprint") || AssetTypeLower == TEXT("editorutilitybp"))
	{
		Reqs.Add({TEXT("ParentClass"), TEXT("Parent class — defaults to EditorUtilityObject"), false});
	}
	else if (AssetTypeLower == TEXT("editorutilitywidgetblueprint") || AssetTypeLower == TEXT("editorutilitywidget"))
	{
		Reqs.Add({TEXT("ParentClass"), TEXT("Parent class — defaults to EditorUtilityWidget"), false});
	}

	return Reqs;
}

// Find a factory that supports creating the given asset class
static UFactory* FindFactoryForClass(UClass* AssetClass)
{
	if (!AssetClass) return nullptr;

	UClass* BestFactoryClass = nullptr;
	bool bBestIsExact = false;

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* FactoryClass = *It;
		if (!FactoryClass->IsChildOf(UFactory::StaticClass()) ||
			FactoryClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
		{
			continue;
		}

		UFactory* CDO = FactoryClass->GetDefaultObject<UFactory>();
		if (!CDO->CanCreateNew()) continue;

		UClass* Supported = CDO->GetSupportedClass();
		if (!Supported) continue;

		if (AssetClass == Supported)
		{
			// Exact match — use immediately
			BestFactoryClass = FactoryClass;
			bBestIsExact = true;
			break;
		}
		else if (AssetClass->IsChildOf(Supported) && !bBestIsExact)
		{
			// Compatible match — keep first found
			if (!BestFactoryClass)
			{
				BestFactoryClass = FactoryClass;
			}
		}
	}

	if (!BestFactoryClass) return nullptr;
	return NewObject<UFactory>(GetTransientPackage(), BestFactoryClass);
}

// Collect UPROPERTY fields from a factory for error reporting
static void CollectFactoryProperties(UFactory* Factory, TArray<FString>& OutProps)
{
	if (!Factory) return;

	for (TFieldIterator<FProperty> PropIt(Factory->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;
		// Skip inherited UFactory base properties
		if (Prop->GetOwnerClass() == UFactory::StaticClass()) continue;

		FString Value;
		Prop->ExportText_InContainer(0, Value, Factory, Factory, Factory, PPF_None);

		FString TypeName = Prop->GetCPPType();
		FString Entry = FString::Printf(TEXT("  %s : %s = %s"),
			*Prop->GetName(), *TypeName, Value.IsEmpty() ? TEXT("(empty)") : *Value);
		OutProps.Add(Entry);
	}
}

void NeoBlueprint::ListFactoryProperties(const FString& AssetTypeName, TArray<TPair<FString, FString>>& OutProps)
{
	FString LowerType = AssetTypeName.ToLower();
	const TMap<FString, FString>& Aliases = GetAssetTypeAliases();

	UClass* AssetClass = nullptr;
	if (const FString* ClassPath = Aliases.Find(LowerType))
	{
		AssetClass = LoadObject<UClass>(nullptr, **ClassPath);
	}
	if (!AssetClass)
	{
		AssetClass = ResolveClassName(AssetTypeName);
	}
	if (!AssetClass) return;

	UFactory* Factory = FindFactoryForClass(AssetClass);
	if (!Factory) return;

	for (TFieldIterator<FProperty> PropIt(Factory->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;
		if (Prop->GetOwnerClass() == UFactory::StaticClass()) continue;

		OutProps.Add(TPair<FString, FString>(Prop->GetName(), Prop->GetCPPType()));
	}
}

void NeoBlueprint::ListAssetTypeAliases(TArray<TPair<FString, FString>>& OutTypes)
{
	const TMap<FString, FString>& Aliases = GetAssetTypeAliases();
	for (const auto& Pair : Aliases)
	{
		OutTypes.Add(TPair<FString, FString>(Pair.Key, Pair.Value));
	}
	// Add special types not in the alias map
	OutTypes.Add(TPair<FString, FString>(TEXT("interface"), TEXT("Blueprint Interface")));
	OutTypes.Add(TPair<FString, FString>(TEXT("functionlibrary"), TEXT("Blueprint Function Library")));
	OutTypes.Add(TPair<FString, FString>(TEXT("macrolibrary"), TEXT("Blueprint Macro Library")));
	OutTypes.Add(TPair<FString, FString>(TEXT("animlayerinterface"), TEXT("AnimLayer Interface Blueprint")));
}

NeoBlueprint::FCreateAssetResult NeoBlueprint::CreateAssetGeneric(const FString& AssetPath,
	const FString& AssetTypeName, const TMap<FString, FString>& Options)
{
	FCreateAssetResult Result;

	// ---- Validate path ----
	if (AssetPath.IsEmpty() || !AssetPath.StartsWith(TEXT("/")))
	{
		Result.Error = FString::Printf(TEXT("Invalid asset path \"%s\" — must be an absolute content path (e.g. \"/Game/MyFolder/MyAsset\")."), *AssetPath);
		return Result;
	}
	FText PathReason;
	if (!FPackageName::IsValidLongPackageName(AssetPath, false, &PathReason))
	{
		Result.Error = FString::Printf(TEXT("Invalid asset path \"%s\" — %s"), *AssetPath, *PathReason.ToString());
		return Result;
	}

	// ---- Check for existing asset ----
	if (StaticFindObject(UObject::StaticClass(), nullptr, *AssetPath))
	{
		Result.Error = FString::Printf(TEXT("Asset already exists at \"%s\". Use duplicate_asset() to copy, or choose a different path."), *AssetPath);
		return Result;
	}

	FString LowerType = AssetTypeName.ToLower();
	const TMap<FString, FString>& Aliases = GetAssetTypeAliases();

	// ---- Resolve asset type to UClass ----
	UClass* AssetClass = nullptr;
	if (const FString* ClassPath = Aliases.Find(LowerType))
	{
		AssetClass = LoadObject<UClass>(nullptr, **ClassPath);
	}
	if (!AssetClass)
	{
		AssetClass = ResolveClassName(AssetTypeName);
	}
	if (!AssetClass)
	{
		// Allow types that are handled as special cases below without needing a UClass
		static const TSet<FString> SpecialCaseTypes = {
			TEXT("interface"), TEXT("blueprintinterface"),
			TEXT("functionlibrary"), TEXT("macrolibrary"),
			TEXT("animlayerinterface")
		};
		if (!SpecialCaseTypes.Contains(LowerType))
		{
			Result.Error = FString::Printf(TEXT("Unknown asset type \"%s\". Common types: Blueprint, AnimBlueprint, DataTable, AnimSequence, AnimMontage, Material, MaterialInstance, Enum, Struct, PhysicsAsset, BlendSpace, CurveFloat, DataAsset, BehaviorTree, StateTree, LevelSequence, EQS, FunctionLibrary, MacroLibrary, AnimLayerInterface"),
				*AssetTypeName);
			return Result;
		}
	}

	// ---- Special cases: types that don't use factories ----

	// Interface Blueprint
	if (LowerType == TEXT("interface") || LowerType == TEXT("blueprintinterface"))
	{
		Result.Asset = CreateInterfaceBlueprint(AssetPath);
		if (!Result.Asset) Result.Error = TEXT("Failed to create Interface Blueprint");
		return Result;
	}

	// Material — no factory, just NewObject
	if (AssetClass && AssetClass->IsChildOf(UMaterial::StaticClass()) && LowerType == TEXT("material"))
	{
		FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		if (Package)
		{
			Result.Asset = NewObject<UMaterial>(Package, FName(*AssetName),
				RF_Public | RF_Standalone | RF_Transactional);
			if (Result.Asset)
			{
				FAssetRegistryModule::AssetCreated(Result.Asset);
				Package->MarkPackageDirty();
			}
		}
		if (!Result.Asset) Result.Error = TEXT("Failed to create Material");
		return Result;
	}

	// UserDefinedEnum — use FEnumEditorUtils for proper initialization
	if (AssetClass == UUserDefinedEnum::StaticClass())
	{
		FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		if (Package)
		{
			Result.Asset = FEnumEditorUtils::CreateUserDefinedEnum(Package, FName(*AssetName),
				RF_Public | RF_Standalone | RF_Transactional);
			if (Result.Asset)
			{
				FAssetRegistryModule::AssetCreated(Result.Asset);
				Package->MarkPackageDirty();
			}
		}
		if (!Result.Asset) Result.Error = TEXT("Failed to create UserDefinedEnum");
		return Result;
	}

	// UserDefinedStruct — no factory
	if (AssetClass == UUserDefinedStruct::StaticClass())
	{
		FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		if (Package)
		{
			Result.Asset = FStructureEditorUtils::CreateUserDefinedStruct(Package, FName(*AssetName),
				RF_Public | RF_Standalone | RF_Transactional);
			if (Result.Asset)
			{
				FAssetRegistryModule::AssetCreated(Result.Asset);
				Package->MarkPackageDirty();
			}
		}
		if (!Result.Asset) Result.Error = TEXT("Failed to create UserDefinedStruct");
		return Result;
	}

	// Generic NewObject creation for asset types that have no factory (PCGGraph, etc.)
	// These are simple UObject subclasses that just need NewObject + asset registration.
	if (AssetClass && !AssetClass->HasAnyClassFlags(CLASS_Abstract)
		&& !AssetClass->IsChildOf(UBlueprint::StaticClass())
		&& !AssetClass->IsChildOf(AActor::StaticClass()))
	{
		// Check if any factory can produce this class — if not, use NewObject
		bool bHasFactory = false;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->IsChildOf(UFactory::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract))
			{
				UFactory* FactoryCDO = It->GetDefaultObject<UFactory>();
				if (FactoryCDO && FactoryCDO->SupportedClass == AssetClass)
				{
					bHasFactory = true;
					break;
				}
			}
		}

		if (!bHasFactory)
		{
			FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
			UPackage* Package = CreatePackage(*AssetPath);
			if (Package)
			{
				Result.Asset = NewObject<UObject>(Package, AssetClass, FName(*AssetName),
					RF_Public | RF_Standalone | RF_Transactional);
				if (Result.Asset)
				{
					FAssetRegistryModule::AssetCreated(Result.Asset);
					Package->MarkPackageDirty();
				}
			}
			if (!Result.Asset) Result.Error = FString::Printf(TEXT("Failed to create %s via NewObject"), *AssetTypeName);
			return Result;
		}
	}

	// ---- Check known requirements before creating factory ----
	TArray<FFactoryRequirement> KnownReqs = GetKnownRequirements(LowerType);
	for (const FFactoryRequirement& Req : KnownReqs)
	{
		if (Req.bRequired && !Options.Contains(Req.PropertyName))
		{
			// Build helpful error with all requirements
			Result.Error = FString::Printf(TEXT("Missing required property \"%s\" (%s)."),
				*Req.PropertyName, *Req.Description);

			FString AllReqs;
			for (const FFactoryRequirement& R : KnownReqs)
			{
				AllReqs += FString::Printf(TEXT("\n  %s%s : %s"),
					*R.PropertyName, R.bRequired ? TEXT(" [REQUIRED]") : TEXT(""),
					*R.Description);
			}
			Result.Error += FString::Printf(TEXT("\nProperties for %s:%s"), *AssetTypeName, *AllReqs);
			return Result;
		}
	}

	// ---- StateTree special case — factory requires a schema class ----
	if (LowerType == TEXT("statetree"))
	{
		// Resolve schema class from options, default to UStateTreeComponentSchema (most common)
		FString SchemaStr = TEXT("StateTreeComponentSchema");
		if (const FString* Val = Options.Find(TEXT("Schema")))
		{
			SchemaStr = *Val;
		}

		UClass* SchemaClass = ResolveClassName(SchemaStr);
		if (!SchemaClass)
		{
			// Try with full path
			SchemaClass = LoadObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/GameplayStateTreeModule.%s"), *SchemaStr));
		}
		if (!SchemaClass)
		{
			Result.Error = FString::Printf(TEXT("Could not find StateTree schema class \"%s\". Common schemas: StateTreeComponentSchema, GameplayInteractionStateTreeSchema, MassStateTreeSchema"), *SchemaStr);
			return Result;
		}

		// Find and configure the factory
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->IsChildOf(UFactory::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract))
			{
				UFactory* Factory = It->GetDefaultObject<UFactory>();
				if (Factory && Factory->SupportedClass == AssetClass)
				{
					// Set the schema via the property (StateTreeSchemaClass is protected, use property reflection)
					FProperty* SchemaProp = FindFProperty<FProperty>(Factory->GetClass(), TEXT("StateTreeSchemaClass"));
					if (SchemaProp)
					{
						// Create a mutable instance for the factory
						UFactory* FactoryInstance = NewObject<UFactory>(GetTransientPackage(), *It);
						FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(SchemaProp);
						if (ObjProp)
						{
							ObjProp->SetObjectPropertyValue(SchemaProp->ContainerPtrToValuePtr<void>(FactoryInstance), SchemaClass);
						}

						FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
						UPackage* Package = CreatePackage(*AssetPath);
						if (Package)
						{
							Result.Asset = FactoryInstance->FactoryCreateNew(AssetClass, Package, FName(*AssetName),
								RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn);
							if (Result.Asset)
							{
								FAssetRegistryModule::AssetCreated(Result.Asset);
								Package->MarkPackageDirty();
							}
						}
					}
					break;
				}
			}
		}
		if (!Result.Asset) Result.Error = TEXT("Failed to create StateTree — could not configure factory with schema class");
		return Result;
	}

	// ---- Blueprint special case (uses FKismetEditorUtilities) ----
	if (LowerType == TEXT("blueprint") || LowerType == TEXT("bp"))
	{
		FString ParentStr = TEXT("Actor");
		if (const FString* Val = Options.Find(TEXT("ParentClass")))
		{
			ParentStr = *Val;
		}
		Result.Asset = CreateBlueprint(AssetPath, ParentStr);
		if (!Result.Asset)
		{
			Result.Error = FString::Printf(TEXT("Failed to create Blueprint with parent \"%s\". Check the class name."), *ParentStr);
		}
		return Result;
	}

	// ---- FunctionLibrary special case ----
	if (LowerType == TEXT("functionlibrary"))
	{
		FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		if (Package)
		{
			Result.Asset = FKismetEditorUtilities::CreateBlueprint(
				UObject::StaticClass(),
				Package,
				FName(*AssetName),
				BPTYPE_FunctionLibrary,
				UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass()
			);
			if (Result.Asset)
			{
				FAssetRegistryModule::AssetCreated(Result.Asset);
				Result.Asset->MarkPackageDirty();
			}
		}
		if (!Result.Asset) Result.Error = TEXT("Failed to create Function Library Blueprint");
		return Result;
	}

	// ---- MacroLibrary special case ----
	if (LowerType == TEXT("macrolibrary"))
	{
		FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		if (Package)
		{
			Result.Asset = FKismetEditorUtilities::CreateBlueprint(
				UObject::StaticClass(),
				Package,
				FName(*AssetName),
				BPTYPE_MacroLibrary,
				UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass()
			);
			if (Result.Asset)
			{
				FAssetRegistryModule::AssetCreated(Result.Asset);
				Result.Asset->MarkPackageDirty();
			}
		}
		if (!Result.Asset) Result.Error = TEXT("Failed to create Macro Library Blueprint");
		return Result;
	}

	// ---- AnimLayerInterface special case ----
	if (LowerType == TEXT("animlayerinterface"))
	{
		FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		if (Package)
		{
			UClass* AnimLayerClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.AnimLayerInterface"));
			if (!AnimLayerClass)
				AnimLayerClass = LoadObject<UClass>(nullptr, TEXT("/Script/Engine.AnimLayerInterface"));
			if (!AnimLayerClass)
			{
				Result.Error = TEXT("UAnimLayerInterface class not found — Engine module may not be loaded");
				return Result;
			}
			UClass* ParentClass = AnimLayerClass;

			Result.Asset = FKismetEditorUtilities::CreateBlueprint(
				ParentClass,
				Package,
				FName(*AssetName),
				BPTYPE_Interface,
				UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass()
			);
			if (Result.Asset)
			{
				FAssetRegistryModule::AssetCreated(Result.Asset);
				Result.Asset->MarkPackageDirty();
			}
		}
		if (!Result.Asset) Result.Error = TEXT("Failed to create AnimLayer Interface Blueprint");
		return Result;
	}

	// ---- MaterialInstance special case ----
	if (LowerType == TEXT("materialinstance") || LowerType == TEXT("materialinstanceconstant"))
	{
		FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		if (Package)
		{
			UMaterialInstanceConstant* MIC = NewObject<UMaterialInstanceConstant>(Package, FName(*AssetName),
				RF_Public | RF_Standalone | RF_Transactional);
			if (MIC)
			{
				// Set parent material if provided
				if (const FString* ParentPath = Options.Find(TEXT("ParentMaterial")))
				{
					UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, **ParentPath);
					if (Parent)
					{
						MIC->SetParentEditorOnly(Parent);
					}
				}
				FAssetRegistryModule::AssetCreated(MIC);
				Package->MarkPackageDirty();
				Result.Asset = MIC;
			}
		}
		if (!Result.Asset) Result.Error = TEXT("Failed to create MaterialInstanceConstant");
		return Result;
	}

	// ---- EditorUtilityBlueprint default ParentClass ----
	if ((LowerType == TEXT("editorutilityblueprint") || LowerType == TEXT("editorutilitybp"))
		&& !Options.Contains(TEXT("ParentClass")))
	{
		// Default to Actor so the factory doesn't fail with null ParentClass
		const_cast<TMap<FString, FString>&>(Options).Add(TEXT("ParentClass"), TEXT("Actor"));
	}

	// ---- DataTable: resolve Struct option (supports UserDefinedStruct asset paths) ----
	if (LowerType == TEXT("datatable") && Options.Contains(TEXT("Struct")))
	{
		const FString& StructPath = Options[TEXT("Struct")];
		// First try loading as an asset (UserDefinedStruct, etc.)
		UScriptStruct* RowStruct = LoadObject<UScriptStruct>(nullptr, *StructPath);
		if (!RowStruct)
		{
			// Try with .AssetName suffix
			FString AssetName = FPackageName::GetShortName(StructPath);
			RowStruct = LoadObject<UScriptStruct>(nullptr, *(StructPath + TEXT(".") + AssetName));
		}
		if (!RowStruct)
		{
			// Fall back to C++ struct lookup (FindStructRobust handles /Script/ paths)
			RowStruct = FindStructRobust(StructPath);
		}
		if (RowStruct)
		{
			// Directly set the factory's Struct property instead of relying on ImportText
			UFactory* DTFactory = FindFactoryForClass(AssetClass);
			if (DTFactory)
			{
				FObjectProperty* StructProp = CastField<FObjectProperty>(DTFactory->GetClass()->FindPropertyByName(TEXT("Struct")));
				if (StructProp)
				{
					StructProp->SetObjectPropertyValue(StructProp->ContainerPtrToValuePtr<void>(DTFactory), const_cast<UScriptStruct*>(RowStruct));
				}

				FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);
				FString DTAssetName = FPackageName::GetLongPackageAssetName(AssetPath);
				Result.Asset = DTFactory->FactoryCreateNew(AssetClass, CreatePackage(*AssetPath), FName(*DTAssetName),
					RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn);
				if (Result.Asset)
				{
					FAssetRegistryModule::AssetCreated(Result.Asset);
					Result.Asset->MarkPackageDirty();
				}
				else
				{
					Result.Error = FString::Printf(TEXT("FactoryCreateNew returned null for DataTable with struct \"%s\""), *StructPath);
				}
				return Result;
			}
		}
		else
		{
			Result.Error = FString::Printf(TEXT("Could not resolve struct \"%s\". For engine structs use \"/Script/Module.FStructName\", for UserDefinedStructs use \"/Game/Path/StructName\""), *StructPath);
			return Result;
		}
	}

	// ---- Find factory ----
	UFactory* Factory = FindFactoryForClass(AssetClass);
	if (!Factory)
	{
		// No factory found — try direct NewObject
		FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		if (Package)
		{
			Result.Asset = NewObject<UObject>(Package, AssetClass, FName(*AssetName),
				RF_Public | RF_Standalone | RF_Transactional);
			if (Result.Asset)
			{
				FAssetRegistryModule::AssetCreated(Result.Asset);
				Package->MarkPackageDirty();
			}
		}
		if (!Result.Asset)
		{
			Result.Error = FString::Printf(TEXT("No factory found for \"%s\" and direct creation failed."), *AssetTypeName);
		}
		return Result;
	}

	// ---- Set factory properties from options ----
	int32 PropsSet = 0;
	for (const auto& Pair : Options)
	{
		FProperty* Prop = Factory->GetClass()->FindPropertyByName(FName(*Pair.Key));
		if (!Prop)
		{
			// Try case-insensitive match
			for (TFieldIterator<FProperty> It(Factory->GetClass()); It; ++It)
			{
				if (It->GetName().Equals(Pair.Key, ESearchCase::IgnoreCase))
				{
					Prop = *It;
					break;
				}
			}
		}

		if (!Prop) continue;

		FString Value = Pair.Value;

		// Special handling for TSubclassOf — resolve short names to full class paths
		if (FClassProperty* ClassProp = CastField<FClassProperty>(Prop))
		{
			UClass* Resolved = ResolveClassName(Value);
			if (Resolved)
			{
				Value = Resolved->GetPathName();
			}
		}

		// Special handling for object properties — resolve to full asset paths
		if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
		{
			// Ensure proper format for ImportText
			if (!Value.IsEmpty() && !Value.Contains(TEXT("'")))
			{
				// Try loading directly to validate
				UObject* Loaded = StaticLoadObject(ObjProp->PropertyClass, nullptr, *Value);
				if (Loaded)
				{
					Value = Loaded->GetPathName();
				}
			}
		}

		FStringOutputDevice ErrorText;
		const TCHAR* ImportResult = Prop->ImportText_InContainer(*Value, Factory, Factory, 0, &ErrorText);
		if (ImportResult)
		{
			PropsSet++;
		}
	}

	// ---- Create the asset ----
	FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);
	FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	Result.Asset = AssetTools.CreateAsset(AssetName, PackagePath, AssetClass, Factory);

	if (!Result.Asset)
	{
		// Collect factory properties for debugging
		CollectFactoryProperties(Factory, Result.FactoryProperties);

		Result.Error = FString::Printf(TEXT("FactoryCreateNew returned null for \"%s\"."), *AssetTypeName);

		if (Result.FactoryProperties.Num() > 0)
		{
			Result.Error += TEXT("\nFactory properties after configuration:");
			for (const FString& PropInfo : Result.FactoryProperties)
			{
				Result.Error += TEXT("\n") + PropInfo;
			}
		}

		// Add known requirements hint
		if (KnownReqs.Num() > 0)
		{
			Result.Error += TEXT("\nKnown requirements:");
			for (const FFactoryRequirement& Req : KnownReqs)
			{
				const FString* ProvidedVal = Options.Find(Req.PropertyName);
				Result.Error += FString::Printf(TEXT("\n  %s%s = %s"),
					*Req.PropertyName, Req.bRequired ? TEXT(" [REQUIRED]") : TEXT(""),
					ProvidedVal ? **ProvidedVal : TEXT("(not set)"));
			}
		}
	}

	return Result;
}
