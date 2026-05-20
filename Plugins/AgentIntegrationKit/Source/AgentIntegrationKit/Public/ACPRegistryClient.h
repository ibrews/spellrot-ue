// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

// ============================================================================
// Registry Data Types (plain C++ — not USTRUCTs, these are transient network data)
// ============================================================================

/** A single platform-specific binary distribution entry */
struct FACPRegistryBinaryTarget
{
	/** URL to the archive (.zip, .tar.gz, etc.) */
	FString Archive;

	/** Path to the executable inside the extracted archive */
	FString Cmd;

	/** Arguments to pass to the executable */
	TArray<FString> Args;

	/** Environment variables to set */
	TMap<FString, FString> Env;
};

/** Distribution info for a registry agent */
struct FACPRegistryDistribution
{
	/** Binary targets keyed by platform: "darwin-aarch64", "windows-x86_64", etc. */
	TMap<FString, FACPRegistryBinaryTarget> BinaryTargets;

	/** npx distribution: package spec (e.g., "@anthropic-ai/claude-code-acp@0.22.2") */
	FString NpxPackage;
	TArray<FString> NpxArgs;

	/** uvx distribution: package spec (e.g., "some-agent==1.0.0") */
	FString UvxPackage;
	TArray<FString> UvxArgs;

	bool HasBinaryForPlatform(const FString& PlatformKey) const { return BinaryTargets.Contains(PlatformKey); }
	bool HasNpx() const { return !NpxPackage.IsEmpty(); }
	bool HasUvx() const { return !UvxPackage.IsEmpty(); }
	bool HasAnyDistribution() const { return BinaryTargets.Num() > 0 || HasNpx() || HasUvx(); }
};

/** A single agent entry from the ACP registry */
struct FACPRegistryAgent
{
	/** Unique ID (lowercase + hyphens, e.g., "claude-acp") */
	FString Id;

	/** Display name (e.g., "Claude Agent") */
	FString Name;

	/** Semver version (e.g., "0.22.2") */
	FString Version;

	/** Brief description */
	FString Description;

	/** Source code repository URL */
	FString Repository;

	/** Website URL */
	FString Website;

	/** Author names */
	TArray<FString> Authors;

	/** License (SPDX identifier or "proprietary") */
	FString License;

	/** Icon URL from registry (e.g., "https://cdn.../agent.svg") */
	FString IconUrl;

	/** Fetched SVG markup (populated after registry load) */
	FString IconSvgMarkup;

	/** Distribution methods */
	FACPRegistryDistribution Distribution;
};

// ============================================================================
// Registry Client
// ============================================================================

/** Info about an available update for an installed agent */
struct FAgentUpdateInfo
{
	FString AgentId;
	FString AgentName;
	FString InstalledVersion;   // Currently installed (from manifest or archive URL hash)
	FString LatestVersion;      // From registry
	FString LatestArchiveUrl;   // For binary agents
	bool bIsNpx = false;        // npx agents auto-update via npx
};

DECLARE_MULTICAST_DELEGATE(FOnRegistryRefreshed);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnRegistryFetchFailed, const FString& /*ErrorMessage*/);

/**
 * Singleton client for the ACP Agent Registry.
 * Fetches and caches the registry JSON from the CDN.
 * Agents are filtered by the current platform.
 */
class AGENTINTEGRATIONKIT_API FACPRegistryClient
{
public:
	static FACPRegistryClient& Get();

	/** Start periodic refresh (call from module startup) */
	void Initialize();

	/** Stop and clean up (call from module shutdown) */
	void Shutdown();

	/** Force an immediate registry refresh */
	void RefreshAsync();

	/** Get all agents available for the current platform */
	const TArray<FACPRegistryAgent>& GetAgents() const { return FilteredAgents; }

	/** Find a specific agent by registry ID */
	const FACPRegistryAgent* FindAgent(const FString& RegistryId) const;

	/** Whether the registry has been loaded (from cache or CDN) */
	bool IsLoaded() const { return bIsLoaded; }

	/** Whether a fetch is currently in-flight */
	bool IsFetching() const { return bFetchInFlight; }

	/** When the registry was last successfully fetched/loaded */
	FDateTime GetLastFetchTime() const { return LastFetchTime; }

	/** Registry JSON version string */
	const FString& GetRegistryVersion() const { return RegistryVersion; }

	/** Get the platform key for the current platform (e.g., "darwin-aarch64") */
	static FString GetCurrentPlatformKey();

	/** Check for updates to installed agents (called after CDN refresh) */
	TArray<FAgentUpdateInfo> CheckForAgentUpdates() const;

	/** Whether any installed agents have updates available */
	bool HasAgentUpdates() const { return CachedUpdates.Num() > 0; }

	/** Get cached update info */
	const TArray<FAgentUpdateInfo>& GetAgentUpdates() const { return CachedUpdates; }

	/** Broadcast when registry data is refreshed (success) */
	FOnRegistryRefreshed OnRegistryRefreshed;

	/** Broadcast when a fetch attempt fails */
	FOnRegistryFetchFailed OnFetchFailed;

private:
	FACPRegistryClient() = default;
	~FACPRegistryClient() = default;

	FACPRegistryClient(const FACPRegistryClient&) = delete;
	FACPRegistryClient& operator=(const FACPRegistryClient&) = delete;

	// CDN fetch
	void FetchFromCDN();
	void HandleFetchResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess);

	// JSON parsing
	bool ParseRegistryJson(const FString& JsonString);
	bool ParseAgentObject(const TSharedPtr<FJsonObject>& AgentObj, FACPRegistryAgent& OutAgent);
	void FilterAgentsForPlatform();

	// Icon fetching (SVGs from CDN)
	void FetchAllIcons();
	void HandleIconResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess, FString AgentId);

	// Local cache
	FString GetCacheFilePath() const;
	FString GetIconCacheDir() const;
	bool LoadFromCache();
	void SaveToCache(const FString& JsonString);

	// Periodic refresh
	bool OnRefreshTick(float DeltaTime);
	FTSTicker::FDelegateHandle RefreshTickerHandle;

	// State
	TArray<FACPRegistryAgent> AllAgents;       // All agents from registry
	TArray<FACPRegistryAgent> FilteredAgents;  // Agents available for current platform
	TArray<FAgentUpdateInfo> CachedUpdates;    // Available updates for installed agents
	FString RegistryVersion;
	FDateTime LastFetchTime;
	bool bIsLoaded = false;
	bool bFetchInFlight = false;
	bool bInitialized = false;
};
