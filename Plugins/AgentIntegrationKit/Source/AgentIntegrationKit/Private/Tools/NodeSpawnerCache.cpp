// Copyright 2025 Betide Studio. All Rights Reserved.

#include "Tools/NodeSpawnerCache.h"
#include "AgentIntegrationKitModule.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintVariableNodeSpawner.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintEventNodeSpawner.h"
#include "BlueprintActionDatabase.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_MacroInstance.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace NodeSpawnerCacheUtils
{
	/**
	 * Checks if the spawner's associated field is stale (belongs to a TRASH or REINST class).
	 * This mirrors the engine's FBlueprintNodeSpawnerUtils::IsStaleFieldAction() logic.
	 */
	static bool IsStaleFieldAction(UBlueprintNodeSpawner const* Spawner)
	{
		if (!Spawner)
		{
			return true;
		}

		UClass* ClassOwner = nullptr;

		// Check function spawners
		if (const UBlueprintFunctionNodeSpawner* FuncSpawner = Cast<UBlueprintFunctionNodeSpawner>(Spawner))
		{
			if (const UFunction* Function = FuncSpawner->GetFunction())
			{
				ClassOwner = Function->GetOwnerClass();
			}
			else
			{
				return true;  // No function means stale
			}
		}
		// Check variable spawners
		else if (const UBlueprintVariableNodeSpawner* VarSpawner = Cast<UBlueprintVariableNodeSpawner>(Spawner))
		{
			if (const FProperty* Property = VarSpawner->GetVarProperty())
			{
				ClassOwner = Property->GetOwnerClass();
			}
			else
			{
				return true;  // No property means stale
			}
		}

		if (ClassOwner != nullptr)
		{
			// Check if the field belongs to a TRASH or REINST class (hot-reload remnant)
			return ClassOwner->HasAnyClassFlags(CLASS_NewerVersionExists) ||
			       (ClassOwner->GetOutermost() == GetTransientPackage());
		}

		return false;
	}
}

FNodeSpawnerCache& FNodeSpawnerCache::Get()
{
	static FNodeSpawnerCache Instance;
	return Instance;
}

FNodeSpawnerCache::FNodeSpawnerCache()
{
}

const TCHAR* FNodeSpawnerCache::GetTypePrefix(ENodeCacheType Type)
{
	switch (Type)
	{
	case ENodeCacheType::VariableGet:      return TEXT("VAR_GET");
	case ENodeCacheType::VariableSet:      return TEXT("VAR_SET");
	case ENodeCacheType::LocalVariableGet: return TEXT("LVAR_GET");
	case ENodeCacheType::LocalVariableSet: return TEXT("LVAR_SET");
	case ENodeCacheType::CallFunction:     return TEXT("FUNC");
	case ENodeCacheType::Event:            return TEXT("EVENT");
	case ENodeCacheType::Macro:            return TEXT("MACRO");
	case ENodeCacheType::CustomEvent:      return TEXT("CUSTOM_EVENT");
	case ENodeCacheType::MaterialExpression: return TEXT("MAT_EXPR");
	case ENodeCacheType::BTTask:           return TEXT("BT_TASK");
	case ENodeCacheType::BTComposite:      return TEXT("BT_COMP");
	case ENodeCacheType::BTDecorator:      return TEXT("BT_DEC");
	case ENodeCacheType::BTService:        return TEXT("BT_SVC");
	default:                               return TEXT("NODE");
	}
}

ENodeCacheType FNodeSpawnerCache::ParseTypePrefix(const FString& Prefix)
{
	if (Prefix == TEXT("VAR_GET"))      return ENodeCacheType::VariableGet;
	if (Prefix == TEXT("VAR_SET"))      return ENodeCacheType::VariableSet;
	if (Prefix == TEXT("LVAR_GET"))     return ENodeCacheType::LocalVariableGet;
	if (Prefix == TEXT("LVAR_SET"))     return ENodeCacheType::LocalVariableSet;
	if (Prefix == TEXT("FUNC"))         return ENodeCacheType::CallFunction;
	if (Prefix == TEXT("EVENT"))        return ENodeCacheType::Event;
	if (Prefix == TEXT("MACRO"))        return ENodeCacheType::Macro;
	if (Prefix == TEXT("CUSTOM_EVENT")) return ENodeCacheType::CustomEvent;
	if (Prefix == TEXT("MAT_EXPR"))     return ENodeCacheType::MaterialExpression;
	if (Prefix == TEXT("BT_TASK"))      return ENodeCacheType::BTTask;
	if (Prefix == TEXT("BT_COMP"))      return ENodeCacheType::BTComposite;
	if (Prefix == TEXT("BT_DEC"))       return ENodeCacheType::BTDecorator;
	if (Prefix == TEXT("BT_SVC"))       return ENodeCacheType::BTService;
	return ENodeCacheType::Other;
}

ENodeCacheType FNodeSpawnerCache::DetectNodeType(UBlueprintNodeSpawner* Spawner) const
{
	if (!Spawner || !Spawner->NodeClass)
	{
		return ENodeCacheType::Other;
	}

	// Check for variable spawner first
	if (const UBlueprintVariableNodeSpawner* VarSpawner = Cast<UBlueprintVariableNodeSpawner>(Spawner))
	{
		bool bIsLocal = VarSpawner->IsLocalVariable();
		bool bIsGetter = Spawner->NodeClass->IsChildOf(UK2Node_VariableGet::StaticClass());

		if (bIsLocal)
		{
			return bIsGetter ? ENodeCacheType::LocalVariableGet : ENodeCacheType::LocalVariableSet;
		}
		else
		{
			return bIsGetter ? ENodeCacheType::VariableGet : ENodeCacheType::VariableSet;
		}
	}

	// Check node class type
	if (Spawner->NodeClass->IsChildOf(UK2Node_CallFunction::StaticClass()))
	{
		return ENodeCacheType::CallFunction;
	}
	if (Spawner->NodeClass->IsChildOf(UK2Node_CustomEvent::StaticClass()))
	{
		return ENodeCacheType::CustomEvent;
	}
	if (Spawner->NodeClass->IsChildOf(UK2Node_Event::StaticClass()))
	{
		return ENodeCacheType::Event;
	}
	if (Spawner->NodeClass->IsChildOf(UK2Node_MacroInstance::StaticClass()))
	{
		return ENodeCacheType::Macro;
	}

	return ENodeCacheType::Other;
}

FString FNodeSpawnerCache::CacheSpawner(
	UBlueprintNodeSpawner* Spawner,
	UBlueprint* Blueprint,
	const FString& NodeName)
{
	if (!Spawner)
	{
		return FString();
	}

	FScopeLock Lock(&CacheLock);

	// Create cache entry with full metadata
	FCacheEntry Entry;
	Entry.NodeType = DetectNodeType(Spawner);
	Entry.NodeName = NodeName;
	Entry.CachedSpawner = Spawner;
	Entry.LastAccessTime = FPlatformTime::Seconds();
	Entry.AccessCount = 0;

	if (Blueprint)
	{
		Entry.BlueprintPath = Blueprint->GetPathName();
		Entry.BlueprintName = Blueprint->GetName();
	}

	// Extract type-specific metadata for recreation
	switch (Entry.NodeType)
	{
	case ENodeCacheType::VariableGet:
	case ENodeCacheType::VariableSet:
	{
		if (const UBlueprintVariableNodeSpawner* VarSpawner = Cast<UBlueprintVariableNodeSpawner>(Spawner))
		{
			if (FProperty const* VarProp = VarSpawner->GetVarProperty())
			{
				Entry.PropertyName = VarProp->GetName();
				Entry.PropertyPath = VarProp->GetPathName();
			}
		}
		break;
	}

	case ENodeCacheType::LocalVariableGet:
	case ENodeCacheType::LocalVariableSet:
	{
		if (const UBlueprintVariableNodeSpawner* VarSpawner = Cast<UBlueprintVariableNodeSpawner>(Spawner))
		{
			FFieldVariant VarOuter = VarSpawner->GetVarOuter();
			if (UEdGraph* LocalGraph = Cast<UEdGraph>(VarOuter.ToUObject()))
			{
				Entry.GraphName = LocalGraph->GetName();
			}
			Entry.SpawnerGuid = Spawner->GetSpawnerSignature().AsGuid();
		}
		break;
	}

	case ENodeCacheType::CallFunction:
	{
		if (const UBlueprintFunctionNodeSpawner* FuncSpawner = Cast<UBlueprintFunctionNodeSpawner>(Spawner))
		{
			if (UFunction const* Func = FuncSpawner->GetFunction())
			{
				Entry.FunctionName = Func->GetName();
				if (UClass* OwnerClass = Func->GetOwnerClass())
				{
					Entry.OwnerClassName = OwnerClass->GetPathName();
				}
			}
		}
		break;
	}

	case ENodeCacheType::Event:
	case ENodeCacheType::CustomEvent:
	{
		// For events, store the event name (extracted from node name)
		Entry.EventName = NodeName;
		break;
	}

	default:
	{
		// Store GUID as fallback
		Entry.SpawnerGuid = Spawner->GetSpawnerSignature().AsGuid();
		if (Spawner->NodeClass)
		{
			Entry.ClassName = Spawner->NodeClass->GetPathName();
		}
		break;
	}
	}

	// Generate unique ID and store
	int32 UniqueId = NextUniqueId++;
	Cache.Add(UniqueId, Entry);

	FString CacheId = BuildCacheId(UniqueId, Entry);

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] NodeSpawnerCache: Cached %s as %s"),
		*NodeName, *CacheId);

	return CacheId;
}

FString FNodeSpawnerCache::BuildCacheId(int32 UniqueId, const FCacheEntry& Entry) const
{
	// Format: TYPE:BlueprintName:NodeName:UniqueId
	// Sanitize node name (replace colons and special chars)
	FString SafeNodeName = Entry.NodeName;
	SafeNodeName.ReplaceInline(TEXT(":"), TEXT("_"));
	SafeNodeName.ReplaceInline(TEXT(" "), TEXT("_"));

	return FString::Printf(TEXT("%s:%s:%s:%d"),
		GetTypePrefix(Entry.NodeType),
		*Entry.BlueprintName,
		*SafeNodeName,
		UniqueId);
}

bool FNodeSpawnerCache::ParseCacheId(const FString& CacheId, FString& OutType, FString& OutBlueprint, FString& OutNode, int32& OutUniqueId) const
{
	TArray<FString> Parts;
	CacheId.ParseIntoArray(Parts, TEXT(":"));

	if (Parts.Num() < 4)
	{
		return false;
	}

	OutType = Parts[0];
	OutBlueprint = Parts[1];
	OutNode = Parts[2];
	OutUniqueId = FCString::Atoi(*Parts[3]);

	return OutUniqueId > 0;
}

bool FNodeSpawnerCache::IsCacheId(const FString& Id)
{
	// Check for known prefixes
	return Id.StartsWith(TEXT("VAR_GET:")) ||
		   Id.StartsWith(TEXT("VAR_SET:")) ||
		   Id.StartsWith(TEXT("LVAR_GET:")) ||
		   Id.StartsWith(TEXT("LVAR_SET:")) ||
		   Id.StartsWith(TEXT("FUNC:")) ||
		   Id.StartsWith(TEXT("EVENT:")) ||
		   Id.StartsWith(TEXT("MACRO:")) ||
		   Id.StartsWith(TEXT("CUSTOM_EVENT:")) ||
		   Id.StartsWith(TEXT("MAT_EXPR:")) ||
		   Id.StartsWith(TEXT("BT_TASK:")) ||
		   Id.StartsWith(TEXT("BT_COMP:")) ||
		   Id.StartsWith(TEXT("BT_DEC:")) ||
		   Id.StartsWith(TEXT("BT_SVC:")) ||
		   Id.StartsWith(TEXT("NODE:"));
}

FString FNodeSpawnerCache::GetBlueprintNameFromId(const FString& CacheId)
{
	TArray<FString> Parts;
	CacheId.ParseIntoArray(Parts, TEXT(":"));
	return Parts.Num() >= 2 ? Parts[1] : FString();
}

FString FNodeSpawnerCache::GetNodeNameFromId(const FString& CacheId)
{
	TArray<FString> Parts;
	CacheId.ParseIntoArray(Parts, TEXT(":"));
	if (Parts.Num() >= 3)
	{
		FString NodeName = Parts[2];
		NodeName.ReplaceInline(TEXT("_"), TEXT(" "));
		return NodeName;
	}
	return FString();
}

UBlueprintNodeSpawner* FNodeSpawnerCache::GetOrCreateSpawner(const FString& CacheId, UBlueprint* Blueprint)
{
	FString TypeStr, BlueprintName, NodeName;
	int32 UniqueId;

	if (!ParseCacheId(CacheId, TypeStr, BlueprintName, NodeName, UniqueId))
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("[AIK] NodeSpawnerCache: Invalid cache ID format: %s"), *CacheId);
		return nullptr;
	}

	FScopeLock Lock(&CacheLock);

	FCacheEntry* Entry = Cache.Find(UniqueId);
	if (!Entry)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("[AIK] NodeSpawnerCache: No entry found for ID %d"), UniqueId);
		return nullptr;
	}

	// Update stats
	Entry->LastAccessTime = FPlatformTime::Seconds();
	Entry->AccessCount++;

	// Fast path: cached spawner is still valid and not stale
	if (Entry->CachedSpawner.IsValid())
	{
		UBlueprintNodeSpawner* CachedSpawner = Entry->CachedSpawner.Get();

		// Check if the spawner's field has become stale (class was recompiled/hot-reloaded)
		if (!NodeSpawnerCacheUtils::IsStaleFieldAction(CachedSpawner))
		{
			StatCacheHits++;
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] NodeSpawnerCache: Cache hit for %s"), *CacheId);
			return CachedSpawner;
		}

		// Spawner is stale, need to recreate
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] NodeSpawnerCache: Cached spawner is stale for %s, recreating"), *CacheId);
		Entry->CachedSpawner.Reset();
	}

	// Slow path: recreate spawner from metadata
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] NodeSpawnerCache: Recreating spawner for %s (was GC'd or stale)"), *CacheId);
	StatRecreations++;

	UBlueprintNodeSpawner* RecreatedSpawner = RecreateSpawner(*Entry, Blueprint);
	if (RecreatedSpawner)
	{
		Entry->CachedSpawner = RecreatedSpawner;
	}

	return RecreatedSpawner;
}

UBlueprintNodeSpawner* FNodeSpawnerCache::RecreateSpawner(FCacheEntry& Entry, UBlueprint* Blueprint)
{
	switch (Entry.NodeType)
	{
	case ENodeCacheType::VariableGet:
		return RecreateVariableSpawner(Entry, Blueprint, true);

	case ENodeCacheType::VariableSet:
		return RecreateVariableSpawner(Entry, Blueprint, false);

	case ENodeCacheType::LocalVariableGet:
		return RecreateLocalVariableSpawner(Entry, Blueprint, true);

	case ENodeCacheType::LocalVariableSet:
		return RecreateLocalVariableSpawner(Entry, Blueprint, false);

	case ENodeCacheType::CallFunction:
		return RecreateFunctionSpawner(Entry, Blueprint);

	default:
		// For other types, try to find in action database by GUID
		if (Entry.SpawnerGuid.IsValid())
		{
			FBlueprintActionDatabase& ActionDB = FBlueprintActionDatabase::Get();
			const FBlueprintActionDatabase::FActionRegistry& AllActions = ActionDB.GetAllActions();

			for (const auto& ActionPair : AllActions)
			{
				for (UBlueprintNodeSpawner* Spawner : ActionPair.Value)
				{
					if (Spawner && Spawner->GetSpawnerSignature().AsGuid() == Entry.SpawnerGuid)
					{
						return Spawner;
					}
				}
			}
		}

		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("[AIK] NodeSpawnerCache: Cannot recreate spawner type %d"),
			static_cast<int32>(Entry.NodeType));
		return nullptr;
	}
}

UBlueprintNodeSpawner* FNodeSpawnerCache::RecreateVariableSpawner(FCacheEntry& Entry, UBlueprint* Blueprint, bool bIsGetter)
{
	if (!Blueprint || Entry.PropertyName.IsEmpty())
	{
		return nullptr;
	}

	// Find the property in the blueprint's generated class
	UClass* GeneratedClass = Blueprint->GeneratedClass;
	if (!GeneratedClass)
	{
		// Try skeleton class
		GeneratedClass = Blueprint->SkeletonGeneratedClass;
	}

	if (!GeneratedClass)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("[AIK] NodeSpawnerCache: No generated class for %s"), *Blueprint->GetName());
		return nullptr;
	}

	FProperty* Property = FindFProperty<FProperty>(GeneratedClass, *Entry.PropertyName);
	if (!Property)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("[AIK] NodeSpawnerCache: Property %s not found in %s"),
			*Entry.PropertyName, *GeneratedClass->GetName());
		return nullptr;
	}

	// Create the spawner
	UClass* NodeClass = bIsGetter ? UK2Node_VariableGet::StaticClass() : UK2Node_VariableSet::StaticClass();
	UBlueprintVariableNodeSpawner* Spawner = UBlueprintVariableNodeSpawner::CreateFromMemberOrParam(
		NodeClass,
		Property,
		nullptr,  // No outer
		GeneratedClass
	);

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] NodeSpawnerCache: Recreated %s spawner for %s"),
		bIsGetter ? TEXT("getter") : TEXT("setter"), *Entry.PropertyName);

	return Spawner;
}

UBlueprintNodeSpawner* FNodeSpawnerCache::RecreateLocalVariableSpawner(FCacheEntry& Entry, UBlueprint* Blueprint, bool bIsGetter)
{
	if (!Blueprint || Entry.PropertyName.IsEmpty() || Entry.GraphName.IsEmpty())
	{
		return nullptr;
	}

	// Find the graph
	UEdGraph* TargetGraph = nullptr;
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == Entry.GraphName)
		{
			TargetGraph = Graph;
			break;
		}
	}

	if (!TargetGraph)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("[AIK] NodeSpawnerCache: Graph %s not found"), *Entry.GraphName);
		return nullptr;
	}

	// Local variable recreation is complex - search action database by GUID

	FBlueprintActionDatabase& ActionDB = FBlueprintActionDatabase::Get();
	ActionDB.RefreshAssetActions(Blueprint);
	const FBlueprintActionDatabase::FActionRegistry& AllActions = ActionDB.GetAllActions();

	for (const auto& ActionPair : AllActions)
	{
		for (UBlueprintNodeSpawner* Spawner : ActionPair.Value)
		{
			if (const UBlueprintVariableNodeSpawner* VarSpawner = Cast<UBlueprintVariableNodeSpawner>(Spawner))
			{
				if (VarSpawner->IsLocalVariable() &&
					Spawner->GetSpawnerSignature().AsGuid() == Entry.SpawnerGuid)
				{
					bool bSpawnerIsGetter = Spawner->NodeClass->IsChildOf(UK2Node_VariableGet::StaticClass());
					if (bSpawnerIsGetter == bIsGetter)
					{
						return Spawner;
					}
				}
			}
		}
	}

	UE_LOG(LogAgentIntegrationKit, Warning, TEXT("[AIK] NodeSpawnerCache: Could not recreate local var spawner for %s"),
		*Entry.NodeName);
	return nullptr;
}

UBlueprintNodeSpawner* FNodeSpawnerCache::RecreateFunctionSpawner(FCacheEntry& Entry, UBlueprint* Blueprint)
{
	if (Entry.FunctionName.IsEmpty())
	{
		return nullptr;
	}

	// Find the function
	UFunction* Function = nullptr;

	if (!Entry.OwnerClassName.IsEmpty())
	{
		// Static function on a specific class
		UClass* OwnerClass = FindObject<UClass>(nullptr, *Entry.OwnerClassName);
		if (OwnerClass)
		{
			Function = OwnerClass->FindFunctionByName(*Entry.FunctionName);
		}
	}
	else if (Blueprint && Blueprint->GeneratedClass)
	{
		// Function on the blueprint itself
		Function = Blueprint->GeneratedClass->FindFunctionByName(*Entry.FunctionName);
	}

	if (!Function)
	{
		// Search common classes
		TArray<UClass*> CommonClasses = {
			UObject::StaticClass(),
			AActor::StaticClass(),
			UActorComponent::StaticClass()
		};

		for (UClass* Class : CommonClasses)
		{
			Function = Class->FindFunctionByName(*Entry.FunctionName);
			if (Function)
			{
				break;
			}
		}
	}

	if (!Function)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("[AIK] NodeSpawnerCache: Function %s not found"), *Entry.FunctionName);
		return nullptr;
	}

	// Create spawner
	UBlueprintFunctionNodeSpawner* Spawner = UBlueprintFunctionNodeSpawner::Create(Function);

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] NodeSpawnerCache: Recreated function spawner for %s"), *Entry.FunctionName);

	return Spawner;
}

UEdGraphNode* FNodeSpawnerCache::InvokeSpawner(
	const FString& CacheId,
	UEdGraph* Graph,
	const FVector2D& Location,
	const IBlueprintNodeBinder::FBindingSet& Bindings)
{
	if (!Graph)
	{
		UE_LOG(LogAgentIntegrationKit, Error, TEXT("[AIK] NodeSpawnerCache: Cannot invoke - Graph is null"));
		return nullptr;
	}

	// Validate graph is in a good state
	if (!Graph->IsValidLowLevel())
	{
		UE_LOG(LogAgentIntegrationKit, Error, TEXT("[AIK] NodeSpawnerCache: Cannot invoke - Graph is invalid"));
		return nullptr;
	}

	// Get the blueprint from graph
	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);

	UBlueprintNodeSpawner* Spawner = GetOrCreateSpawner(CacheId, Blueprint);
	if (!Spawner)
	{
		UE_LOG(LogAgentIntegrationKit, Error, TEXT("[AIK] NodeSpawnerCache: Failed to get/create spawner for %s"), *CacheId);
		return nullptr;
	}

	// Validate spawner is still valid (UObject level)
	if (!Spawner->IsValidLowLevel())
	{
		UE_LOG(LogAgentIntegrationKit, Error, TEXT("[AIK] NodeSpawnerCache: Spawner is invalid for %s"), *CacheId);
		return nullptr;
	}

	// Validate the spawner's node class
	if (!Spawner->NodeClass || !Spawner->NodeClass->IsValidLowLevel())
	{
		UE_LOG(LogAgentIntegrationKit, Error, TEXT("[AIK] NodeSpawnerCache: Spawner has invalid NodeClass for %s"), *CacheId);
		return nullptr;
	}

	// Check if the spawner's associated field is stale (belongs to TRASH/REINST class)
	// This is critical for hot-reload safety - mirrors engine's FBlueprintNodeSpawnerUtils::IsStaleFieldAction()
	if (NodeSpawnerCacheUtils::IsStaleFieldAction(Spawner))
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("[AIK] NodeSpawnerCache: Spawner has stale field for %s (class was recompiled), invalidating cache entry"), *CacheId);

		// Invalidate this cache entry so it gets recreated next time
		FScopeLock Lock(&CacheLock);
		FString TypeStr, BlueprintName, NodeName;
		int32 UniqueId;
		if (ParseCacheId(CacheId, TypeStr, BlueprintName, NodeName, UniqueId))
		{
			Cache.Remove(UniqueId);
		}
		return nullptr;
	}

	// Track node count
	int32 PreCount = Graph->Nodes.Num();

	// Invoke the spawner
	UEdGraphNode* NewNode = Spawner->Invoke(Graph, Bindings, FVector2D(Location));

	if (NewNode)
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] NodeSpawnerCache: Successfully spawned %s via %s"),
			*NewNode->GetNodeTitle(ENodeTitleType::ListView).ToString(), *CacheId);
	}
	else if (Graph->Nodes.Num() > PreCount)
	{
		NewNode = Graph->Nodes.Last();
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] NodeSpawnerCache: Spawned node (indirect) via %s"), *CacheId);
	}
	else
	{
		UE_LOG(LogAgentIntegrationKit, Error, TEXT("[AIK] NodeSpawnerCache: Spawn failed for %s"), *CacheId);
	}

	// Fix self-context for variable nodes in their own blueprint.
	// When spawned via the cache, variable nodes default to "external access" mode with explicit self pin.
	// For nodes accessing variables in their own blueprint, we want implicit self (hidden self pin).
	if (UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(NewNode))
	{
		if (VarNode->VariableReference.IsSelfContext())
		{
			VarNode->SelfContextInfo = ESelfContextInfo::Unspecified;
			VarNode->ReconstructNode();
		}
	}

	return NewNode;
}

void FNodeSpawnerCache::InvalidateForBlueprint(UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		return;
	}

	FScopeLock Lock(&CacheLock);

	FString TargetName = Blueprint->GetName();
	TArray<int32> ToRemove;

	for (const auto& Pair : Cache)
	{
		if (Pair.Value.BlueprintName == TargetName)
		{
			ToRemove.Add(Pair.Key);
		}
	}

	for (int32 Id : ToRemove)
	{
		Cache.Remove(Id);
	}

	if (ToRemove.Num() > 0)
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] NodeSpawnerCache: Invalidated %d entries for %s"),
			ToRemove.Num(), *TargetName);
	}
}

void FNodeSpawnerCache::ClearAll()
{
	FScopeLock Lock(&CacheLock);
	int32 Count = Cache.Num();
	Cache.Empty();
	NextUniqueId = 1;
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] NodeSpawnerCache: Cleared %d entries"), Count);
}

void FNodeSpawnerCache::GetStats(int32& OutTotal, int32& OutCacheHits, int32& OutRecreations) const
{
	FScopeLock Lock(&CacheLock);
	OutTotal = Cache.Num();
	OutCacheHits = StatCacheHits;
	OutRecreations = StatRecreations;
}
