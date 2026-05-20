// Copyright 2025 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"

/**
 * Session-persistent registry mapping node names to GUIDs.
 * Allows AI to reference nodes by friendly names across multiple tool calls.
 *
 * Key format: "AssetPath|GraphName|NodeName"
 *
 * Behavior:
 * - New name: Registers name -> GUID mapping
 * - Existing name: Replaces with new GUID (handles AI retries)
 * - Lookup: Returns GUID for name, or invalid GUID if not found
 */
class AGENTINTEGRATIONKIT_API FNodeNameRegistry
{
public:
	/** Get singleton instance */
	static FNodeNameRegistry& Get();

	/**
	 * Register or replace a name -> GUID mapping
	 * @param AssetPath   Full asset path (e.g., "/Game/Blueprints/BP_Player")
	 * @param GraphName   Name of the graph (e.g., "EventGraph")
	 * @param NodeName    Friendly name assigned by user/AI
	 * @param NodeGuid    The node's actual GUID in the graph
	 */
	void Register(const FString& AssetPath, const FString& GraphName,
	              const FString& NodeName, const FGuid& NodeGuid);

	/**
	 * Resolve a name to its GUID
	 * @param AssetPath   Full asset path
	 * @param GraphName   Name of the graph
	 * @param NodeName    Friendly name to lookup
	 * @return The GUID if found, or invalid GUID if not registered
	 */
	FGuid Resolve(const FString& AssetPath, const FString& GraphName,
	              const FString& NodeName) const;

	/**
	 * Check if a name is registered
	 */
	bool IsRegistered(const FString& AssetPath, const FString& GraphName,
	                  const FString& NodeName) const;

	/**
	 * Unregister a specific name
	 */
	void Unregister(const FString& AssetPath, const FString& GraphName,
	                const FString& NodeName);

	/**
	 * Clear all registrations for a specific graph
	 */
	void ClearGraph(const FString& AssetPath, const FString& GraphName);

	/**
	 * Clear all registrations for a specific asset
	 */
	void ClearAsset(const FString& AssetPath);

	/**
	 * Clear entire registry
	 */
	void ClearAll();

	/**
	 * Get count of registered names
	 */
	int32 GetCount() const { return Registry.Num(); }

private:
	FNodeNameRegistry() = default;
	~FNodeNameRegistry() = default;

	/** Build registry key from components */
	static FString MakeKey(const FString& AssetPath, const FString& GraphName, const FString& NodeName);

	/** Parse key back to components (for iteration/clearing) */
	static bool ParseKey(const FString& Key, FString& OutAssetPath, FString& OutGraphName, FString& OutNodeName);

	/** The registry: Key -> GUID */
	TMap<FString, FGuid> Registry;
};
