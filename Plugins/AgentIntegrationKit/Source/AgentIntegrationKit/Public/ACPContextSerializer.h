// Copyright 2025 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AActor;
class UEdGraphNode;
class UEdGraphPin;
class UBlueprint;

/**
 * Utility class for serializing Blueprint nodes and assets to structured text
 * suitable for AI context.
 */
class AGENTINTEGRATIONKIT_API FACPContextSerializer
{
public:
	/**
	 * Serialize any UObject:
	 * - Object name, class, and asset path
	 * - Key editable properties with current values
	 *
	 * @param Object The object to serialize
	 * @return Formatted text representation of the object
	 */
	static FString SerializeObjectOverview(const UObject* Object);

	/**
	 * Serialize an Actor from the level:
	 * - Actor label, class, and Blueprint path (if applicable)
	 * - World transform (location, rotation, scale)
	 * - Components (name, class, attached-to)
	 * - Key properties (mobility, tags, layers, visibility)
	 *
	 * @param Actor The actor to serialize
	 * @return Formatted text representation of the actor
	 */
	static FString SerializeActorOverview(const AActor* Actor);

	/**
	 * Get a short display name for an Actor.
	 * Used for UI chips.
	 *
	 * @param Actor The actor to get the name from
	 * @return Short display name
	 */
	static FString GetActorDisplayName(const AActor* Actor);

	/**
	 * Serialize a graph node with full detail:
	 * - Node name, type, GUID, position
	 * - All pins with types and default values
	 * - Connections to adjacent nodes (1 level deep when bIncludeConnections is true)
	 * - Node-specific properties (function details, variable info, etc.)
	 *
	 * @param Node The node to serialize
	 * @param bIncludeConnections Whether to include info about connected nodes
	 * @return Formatted text representation of the node
	 */
	static FString SerializeNode(const UEdGraphNode* Node, bool bIncludeConnections = true);

	/**
	 * Serialize a Blueprint structure overview:
	 * - Blueprint name and parent class
	 * - Variables (name, type, default value)
	 * - Components (name, class)
	 * - Functions (name, parameter count, node count)
	 * - Events (name, node count)
	 * - Graph overview with node counts
	 *
	 * @param Blueprint The blueprint to serialize
	 * @return Formatted text representation of the blueprint structure
	 */
	static FString SerializeBlueprintOverview(const UBlueprint* Blueprint);

	/**
	 * Get a short display name for a node.
	 * Used for UI chips.
	 *
	 * @param Node The node to get the name from
	 * @return Short display name
	 */
	static FString GetNodeDisplayName(const UEdGraphNode* Node);

	/**
	 * Get a short display name for a Blueprint.
	 * Used for UI chips.
	 *
	 * @param Blueprint The blueprint to get the name from
	 * @return Short display name
	 */
	static FString GetBlueprintDisplayName(const UBlueprint* Blueprint);

private:
	/**
	 * Serialize a single pin's information.
	 */
	static FString SerializePinForContext(const UEdGraphPin* Pin);

	/**
	 * Get a summary of nodes connected to a pin.
	 */
	static FString GetConnectedNodeSummary(const UEdGraphPin* Pin);

	/**
	 * Convert a pin type to a readable string.
	 */
	static FString PinTypeToString(const UEdGraphPin* Pin);
};
