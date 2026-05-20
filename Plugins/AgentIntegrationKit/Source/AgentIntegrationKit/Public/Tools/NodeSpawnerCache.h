// Copyright 2025 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BlueprintNodeBinder.h"
#include "UObject/WeakObjectPtr.h"

class UBlueprintNodeSpawner;
class UBlueprint;
class UEdGraph;
class UEdGraphNode;

/**
 * Node type categories for spawner recreation
 */
enum class ENodeCacheType : uint8
{
	VariableGet,      // Get variable node
	VariableSet,      // Set variable node
	LocalVariableGet, // Get local variable
	LocalVariableSet, // Set local variable
	CallFunction,     // Function call node
	Event,            // Event node
	Macro,            // Macro instance
	CustomEvent,      // Custom event
	MaterialExpression, // Material expression
	BTTask,           // Behavior tree task
	BTComposite,      // Behavior tree composite
	BTDecorator,      // Behavior tree decorator
	BTService,        // Behavior tree service
	Other             // Fallback - use GUID
};

/**
 * Production-ready cache for Blueprint node spawners.
 *
 * Stores metadata to recreate spawners on-demand, surviving:
 * - Garbage collection cycles
 * - Blueprint recompilation
 * - Long sessions (30+ minutes)
 * - Multiple blueprints (10+)
 *
 * ID Format: TYPE:BlueprintName:NodeName:UniqueId
 * Examples:
 *   VAR_GET:BP_Player:Health:1
 *   VAR_SET:BP_Player:Health:2
 *   FUNC:BP_Player:PrintString:3
 *   EVENT:BP_Player:BeginPlay:4
 */
class AGENTINTEGRATIONKIT_API FNodeSpawnerCache
{
public:
	/** Get singleton instance */
	static FNodeSpawnerCache& Get();

	/**
	 * Cache a spawner with full metadata for recreation
	 * @param Spawner The spawner to cache
	 * @param Blueprint The blueprint context
	 * @param NodeName Display name of the node
	 * @return Semantic cache ID (e.g., "VAR_GET:BP_Player:Health:1")
	 */
	FString CacheSpawner(
		UBlueprintNodeSpawner* Spawner,
		UBlueprint* Blueprint,
		const FString& NodeName
	);

	/**
	 * Get or recreate a spawner from cache ID
	 * @param CacheId The semantic cache ID
	 * @param Blueprint The target blueprint (for validation/recreation)
	 * @return The spawner (from cache or freshly created), or nullptr
	 */
	UBlueprintNodeSpawner* GetOrCreateSpawner(const FString& CacheId, UBlueprint* Blueprint);

	/**
	 * Invoke a cached/recreated spawner to create a node
	 * @param CacheId The cache ID
	 * @param Graph The graph to spawn into
	 * @param Location Node position
	 * @return The created node, or nullptr on failure
	 */
	UEdGraphNode* InvokeSpawner(
		const FString& CacheId,
		UEdGraph* Graph,
		const FVector2D& Location,
		const IBlueprintNodeBinder::FBindingSet& Bindings = IBlueprintNodeBinder::FBindingSet());

	/**
	 * Check if an ID is a cache ID
	 */
	static bool IsCacheId(const FString& Id);

	/**
	 * Parse blueprint name from cache ID
	 */
	static FString GetBlueprintNameFromId(const FString& CacheId);

	/**
	 * Parse node name from cache ID
	 */
	static FString GetNodeNameFromId(const FString& CacheId);

	/**
	 * Clear cache entries for a specific blueprint
	 */
	void InvalidateForBlueprint(UBlueprint* Blueprint);

	/**
	 * Clear all cache entries
	 */
	void ClearAll();

	/**
	 * Get cache statistics
	 */
	void GetStats(int32& OutTotal, int32& OutCacheHits, int32& OutRecreations) const;

private:
	FNodeSpawnerCache();
	~FNodeSpawnerCache() = default;

	FNodeSpawnerCache(const FNodeSpawnerCache&) = delete;
	FNodeSpawnerCache& operator=(const FNodeSpawnerCache&) = delete;

	/** Metadata needed to recreate a spawner */
	struct FCacheEntry
	{
		// Core identification
		ENodeCacheType NodeType = ENodeCacheType::Other;
		FString BlueprintPath;       // Full asset path: /Game/Blueprints/BP_Player
		FString BlueprintName;       // Short name: BP_Player
		FString NodeName;            // Display name: "Get Health", "Print String"

		// For variables
		FString PropertyName;        // Raw property name: "Health"
		FString PropertyPath;        // Full path for recreation

		// For functions
		FString FunctionName;        // Function name
		FString OwnerClassName;      // Owning class for static functions

		// For local variables
		FString GraphName;           // Graph that owns local var
		FGuid LocalVarGuid;          // Local variable GUID

		// For events
		FString EventName;           // Event name

		// For materials/BT
		FString ClassName;           // UClass path for expressions/BT nodes

		// Fallback GUID (for "Other" type)
		FGuid SpawnerGuid;

		// Cached spawner (may be GC'd - that's OK)
		TWeakObjectPtr<UBlueprintNodeSpawner> CachedSpawner;

		// Stats
		double LastAccessTime = 0.0;
		int32 AccessCount = 0;
	};

	/** Build cache ID string from entry */
	FString BuildCacheId(int32 UniqueId, const FCacheEntry& Entry) const;

	/** Parse cache ID into components */
	bool ParseCacheId(const FString& CacheId, FString& OutType, FString& OutBlueprint, FString& OutNode, int32& OutUniqueId) const;

	/** Recreate spawner from metadata */
	UBlueprintNodeSpawner* RecreateSpawner(FCacheEntry& Entry, UBlueprint* Blueprint);

	/** Recreate variable spawner */
	UBlueprintNodeSpawner* RecreateVariableSpawner(FCacheEntry& Entry, UBlueprint* Blueprint, bool bIsGetter);

	/** Recreate local variable spawner */
	UBlueprintNodeSpawner* RecreateLocalVariableSpawner(FCacheEntry& Entry, UBlueprint* Blueprint, bool bIsGetter);

	/** Recreate function call spawner */
	UBlueprintNodeSpawner* RecreateFunctionSpawner(FCacheEntry& Entry, UBlueprint* Blueprint);

	/** Detect node type from spawner */
	ENodeCacheType DetectNodeType(UBlueprintNodeSpawner* Spawner) const;

	/** Get type prefix string */
	static const TCHAR* GetTypePrefix(ENodeCacheType Type);

	/** Parse type from prefix */
	static ENodeCacheType ParseTypePrefix(const FString& Prefix);

	/** Cache storage: UniqueId -> Entry */
	TMap<int32, FCacheEntry> Cache;

	/** Next unique ID */
	int32 NextUniqueId = 1;

	/** Statistics */
	mutable int32 StatCacheHits = 0;
	mutable int32 StatRecreations = 0;

	/** Thread safety */
	mutable FCriticalSection CacheLock;
};
