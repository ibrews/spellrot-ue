// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ACPTypes.h"
#include "Containers/Ticker.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnAgentUsageDataUpdated, const FString& /*AgentName*/, const FAgentRateLimitData& /*Data*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnMeshyBalanceUpdated, bool /*bSuccess*/, int32 /*Balance*/);

/**
 * Singleton service that reads OAuth credentials from CLI config files on disk
 * and polls usage/rate-limit APIs for Claude Code and Codex.
 *
 * Does NOT perform token refresh — the CLI tools own their tokens.
 * If a token is expired, we surface a "re-authenticate" message.
 */
class AGENTINTEGRATIONKIT_API FAgentUsageMonitor
{
public:
	static FAgentUsageMonitor& Get();

	/** Start periodic polling (call once from module startup) */
	void Initialize();

	/** Stop polling and clean up (call from module shutdown) */
	void Shutdown();

	/** Trigger an immediate usage fetch for the given agent (e.g. on agent switch) */
	void RequestUsageUpdate(const FString& AgentName);

	/** Get the most recently cached usage data for an agent */
	const FAgentRateLimitData& GetCachedUsage(const FString& AgentName) const;

	/** Returns true if we support usage monitoring for this agent name */
	static bool IsAgentSupported(const FString& AgentName);

	/** Broadcast when usage data changes (UI subscribes to this) */
	FOnAgentUsageDataUpdated OnUsageDataUpdated;

	/** Broadcast when Meshy balance is updated */
	FOnMeshyBalanceUpdated OnMeshyBalanceUpdated;

	/** Trigger an immediate Meshy balance fetch */
	void RequestMeshyBalanceUpdate();

	/** Get cached Meshy balance. Returns -1 if not fetched. */
	int32 GetCachedMeshyBalance() const { return CachedMeshyBalance; }
	bool IsMeshyBalanceLoading() const { return bMeshyFetchInFlight; }
	const FString& GetMeshyBalanceError() const { return MeshyBalanceError; }

private:
	FAgentUsageMonitor() = default;
	~FAgentUsageMonitor() = default;

	// Non-copyable
	FAgentUsageMonitor(const FAgentUsageMonitor&) = delete;
	FAgentUsageMonitor& operator=(const FAgentUsageMonitor&) = delete;

	// ---- Credential reading (file-based, no Keychain) ----

	struct FClaudeCredentials
	{
		FString AccessToken;
		FString RateLimitTier;
		double ExpiresAtEpochMs = 0.0; // milliseconds since epoch
		bool bIsValid = false;
		FString ErrorMessage;
	};

	struct FCodexCredentials
	{
		FString AccessToken;
		FString AccountId;
		bool bIsApiKey = false;
		bool bIsValid = false;
		FString ErrorMessage;
	};

	FClaudeCredentials ReadClaudeCredentials() const;
	FCodexCredentials ReadCodexCredentials() const;

	// ---- HTTP fetching ----

	void FetchClaudeUsage(const FString& AccessToken);
	void FetchCodexUsage(const FString& AccessToken, const FString& AccountId);

	void HandleClaudeResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess);
	void HandleCodexResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess);

	FAgentRateLimitData ParseClaudeUsageResponse(const TSharedPtr<FJsonObject>& Json) const;
	FAgentRateLimitData ParseCodexUsageResponse(const TSharedPtr<FJsonObject>& Json) const;

	/** Infer plan name from rateLimitTier credential field */
	static FString InferPlanFromTier(const FString& Tier);

	// ---- Polling ----

	bool Tick(float DeltaTime);
	FTSTicker::FDelegateHandle TickerHandle;

	/** Currently monitored agent name (set by RequestUsageUpdate) */
	FString ActiveAgentName;

	/** Cached usage data per agent */
	TMap<FString, FAgentRateLimitData> CachedData;

	/** Default empty data for GetCachedUsage when no data exists */
	static const FAgentRateLimitData EmptyData;

	/** Time of last fetch attempt */
	double LastFetchTimeSeconds = 0.0;

	/** Polling interval in seconds */
	static constexpr double PollIntervalSeconds = 90.0;

	/** Retry interval after error */
	static constexpr double RetryIntervalSeconds = 30.0;

	/** Whether a fetch is currently in-flight */
	bool bFetchInFlight = false;

	/** Cached rate limit tier from Claude credentials (used during response parsing) */
	FString CachedClaudeRateLimitTier;

	bool bInitialized = false;

	// ---- Meshy balance ----

	void FetchMeshyBalance();
	void HandleMeshyBalanceResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess);

	int32 CachedMeshyBalance = -1; // -1 = not fetched
	bool bMeshyFetchInFlight = false;
	FString MeshyBalanceError;
	double LastMeshyFetchTimeSeconds = 0.0;
};
