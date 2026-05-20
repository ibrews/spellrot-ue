// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ACPTypes.h"
#include "ACPRegistryClient.h"

/**
 * Generates FACPAgentConfig entries from ACP registry manifests.
 * Replaces the hardcoded per-agent config generation in ACPSettings.
 *
 * The pipeline is:
 *   Registry manifest (distribution info)
 *   + Quirks map (agent-specific behavior)
 *   + User overrides (custom executable paths)
 *   → FACPAgentConfig ready for FACPClient::Connect()
 */
class AGENTINTEGRATIONKIT_API FACPRegistryConfigGenerator
{
public:
	/**
	 * Generate configs for all registry agents available on this platform.
	 * Checks install status and resolves executable paths.
	 */
	static TArray<FACPAgentConfig> GenerateAllConfigs();

	/**
	 * Generate a config for a single registry agent.
	 * Returns false if the agent has no usable distribution for this platform.
	 */
	static bool GenerateConfig(const FACPRegistryAgent& Agent, FACPAgentConfig& OutConfig);

	/**
	 * Check if a user has overridden the executable path for an agent.
	 * Reads from ACPSettings' generic path override map.
	 */
	static FString GetUserPathOverride(const FString& RegistryId);
};
