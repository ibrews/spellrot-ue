// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// ── Job status (universal across all providers) ──────────────────────

enum class EGenerativeJobStatus : uint8
{
	Pending,
	Running,
	Succeeded,
	Failed,
	Cancelled
};

// ── A job submitted to any provider ──────────────────────────────────

struct AGENTINTEGRATIONKIT_API FGenerativeJob
{
	FString ProviderId;
	FString ActionId;
	FString JobId;
	EGenerativeJobStatus Status = EGenerativeJobStatus::Pending;
	int32 Progress = 0; // 0-100

	// Results (populated on success)
	FString ResultUrl;      // primary download URL (GLB, FBX, PNG, WAV, etc.)
	FString ThumbnailUrl;
	TMap<FString, FString> ExtraUrls; // format→url ("fbx"→url, "glb"→url, "walking_fbx"→url, etc.)
	TArray<FString> ImageUrls;        // for text-to-image/image-to-image (multiple output images)

	FString ErrorMessage;
	TSharedPtr<FJsonObject> RawResponse; // full provider response for advanced access

	bool IsTerminal() const
	{
		return Status == EGenerativeJobStatus::Succeeded
			|| Status == EGenerativeJobStatus::Failed
			|| Status == EGenerativeJobStatus::Cancelled;
	}

	bool IsSuccess() const { return Status == EGenerativeJobStatus::Succeeded; }

	static FGenerativeJob MakePending(const FString& InJobId)
	{
		FGenerativeJob Job;
		Job.JobId = InJobId;
		Job.Status = EGenerativeJobStatus::Pending;
		return Job;
	}

	static FGenerativeJob MakeSuccess(const FString& InJobId, const FString& InResultUrl)
	{
		FGenerativeJob Job;
		Job.JobId = InJobId;
		Job.ResultUrl = InResultUrl;
		Job.Status = EGenerativeJobStatus::Succeeded;
		Job.Progress = 100;
		return Job;
	}

	static FGenerativeJob MakeFail(const FString& InError)
	{
		FGenerativeJob Job;
		Job.ErrorMessage = InError;
		Job.Status = EGenerativeJobStatus::Failed;
		return Job;
	}
};

// ── Describes one action a provider can perform ──────────────────────

struct AGENTINTEGRATIONKIT_API FProviderActionDescriptor
{
	FString ActionId;      // "text_to_3d", "rig", "retexture", "tts"
	FString Description;   // Agent-readable description

	// Loose hints for agent discovery (NOT enforced, just for routing/display)
	TArray<FString> InputHints;   // ["text"], ["image"], ["model","text"], ["job_ref"]
	TArray<FString> OutputHints;  // ["model"], ["animation"], ["image"], ["audio"]

	// Full JSON Schema for this action's parameters
	TSharedPtr<FJsonObject> ParamsSchema;

	// Informational
	FString CreditCost;             // "20 credits" (display only)
	bool bIsSynchronous = false;    // true = no polling needed (e.g., balance check, sync TTS)
};

// ── The provider interface ───────────────────────────────────────────

class AGENTINTEGRATIONKIT_API IGenerativeProvider : public TSharedFromThis<IGenerativeProvider>
{
public:
	virtual ~IGenerativeProvider() = default;

	// Identity
	virtual FString GetId() const = 0;            // "meshy", "tripo", "elevenlabs"
	virtual FString GetDisplayName() const = 0;   // "Meshy AI"
	virtual FString GetWebsite() const { return TEXT(""); }

	// Auth & routing
	virtual FString GetDirectBaseUrl() const = 0;       // "https://api.meshy.ai"
	virtual FString GetProxyPath() const = 0;           // "/api/proxy/meshy"
	virtual FString GetApiKeySettingName() const = 0;   // "MeshyApiKey"

	// What can this provider do?
	virtual TArray<FProviderActionDescriptor> GetActions() const = 0;

	// Check if this provider supports a specific action
	bool SupportsAction(const FString& ActionId) const;

	// Find a specific action descriptor
	const FProviderActionDescriptor* FindAction(const FString& ActionId) const;

	// Execute
	virtual FGenerativeJob Submit(const FString& ActionId,
		const TSharedPtr<FJsonObject>& Params) = 0;

	virtual FGenerativeJob CheckStatus(const FString& JobId,
		const FString& ActionId = TEXT("")) = 0;

	// Optional: some providers need a separate result-fetch call
	virtual FGenerativeJob GetResult(const FString& JobId,
		const FString& ActionId = TEXT(""))
	{
		return CheckStatus(JobId, ActionId);
	}

	// Optional: cancel a running job
	virtual bool CancelJob(const FString& JobId) { return false; }

	// Optional: check credit balance (-1 = not supported)
	virtual int32 GetBalance() { return -1; }

protected:
	// Cache action list for SupportsAction/FindAction lookups
	mutable TArray<FProviderActionDescriptor> CachedActions;
	mutable bool bActionsCached = false;

	const TArray<FProviderActionDescriptor>& GetCachedActions() const;
};

// ── Base class with HTTP/auth/download helpers ───────────────────────

class AGENTINTEGRATIONKIT_API FGenerativeProviderBase : public IGenerativeProvider
{
public:
	virtual ~FGenerativeProviderBase() = default;

	// Parse status strings (SUCCEEDED, FAILED, etc.) to EGenerativeJobStatus
	static EGenerativeJobStatus ParseStatus(const FString& StatusStr);

protected:
	// Auth routing: returns Betide token or provider-specific key
	FString GetAuthToken() const;

	// URL routing: returns proxy URL or direct base URL
	FString GetBaseUrl() const;

	// Whether we're routing through Betide proxy
	bool IsUsingProxy() const;

	/** Override to customize how auth headers are set on requests.
	 *  Default: "Authorization: Bearer <token>". ElevenLabs uses "xi-api-key: <token>". */
	virtual void SetAuthHeaders(const TSharedRef<class IHttpRequest, ESPMode::ThreadSafe>& Request) const;

	// HTTP helpers (synchronous, safe from game thread dispatch)
	TSharedPtr<FJsonObject> HttpPost(const FString& Path,
		const TSharedPtr<FJsonObject>& Body,
		float TimeoutSeconds = 60.0f) const;

	TSharedPtr<FJsonObject> HttpGet(const FString& Path,
		float TimeoutSeconds = 60.0f) const;

	TSharedPtr<FJsonObject> HttpDelete(const FString& Path,
		float TimeoutSeconds = 30.0f) const;

	// Raw HTTP request (for non-JSON responses like file downloads)
	bool HttpDownload(const FString& Url, const FString& OutputPath,
		FString& OutError, float TimeoutSeconds = 300.0f) const;

	/** POST with JSON body, receive raw binary response (e.g. audio bytes from TTS).
	 *  Returns the response content as bytes. OutError set on failure. */
	TArray<uint8> HttpPostRaw(const FString& Path,
		const TSharedPtr<FJsonObject>& Body,
		FString& OutError,
		FString& OutContentType,
		float TimeoutSeconds = 120.0f) const;

	// Build a JSON Schema object for a single property
	static TSharedPtr<FJsonObject> SchemaString(const FString& Desc,
		const TArray<FString>& Enum = {}, const FString& Default = TEXT(""));
	static TSharedPtr<FJsonObject> SchemaInt(const FString& Desc,
		int32 Min = 0, int32 Max = 0, int32 Default = 0);
	static TSharedPtr<FJsonObject> SchemaBool(const FString& Desc, bool Default = false);
	static TSharedPtr<FJsonObject> SchemaStringArray(const FString& Desc);

	// Build a complete JSON Schema with properties map and required list
	static TSharedPtr<FJsonObject> BuildSchema(
		const TMap<FString, TSharedPtr<FJsonObject>>& Properties,
		const TArray<FString>& Required = {});

private:
	// Internal HTTP dispatch
	TSharedPtr<FJsonObject> MakeJsonRequest(const FString& Verb,
		const FString& FullUrl,
		const TSharedPtr<FJsonObject>& Body,
		FString& OutError,
		float TimeoutSeconds) const;
};

// ── Provider auto-registration macro ─────────────────────────────────
// Uses the same pattern as REGISTER_LUA_BINDING — static constructor calls into registry directly.

#define REGISTER_GENERATIVE_PROVIDER(ProviderClass) \
	static struct FAutoReg_Provider_##ProviderClass { \
		FAutoReg_Provider_##ProviderClass() { \
			FDeferredProviderRegistration::Get().Add([]() { \
				FGenerativeProviderRegistry::Get().Register(MakeShared<ProviderClass>()); \
			}); \
		} \
	} GAutoReg_Provider_##ProviderClass;

// Deferred registration helper — collects provider registrations during static init,
// executes them once during module startup (after UACPSettings is available).
class AGENTINTEGRATIONKIT_API FDeferredProviderRegistration
{
public:
	static FDeferredProviderRegistration& Get();
	void Add(TFunction<void()> Func);
	void ExecuteAll();

private:
	TArray<TFunction<void()>> PendingRegistrations;
	bool bExecuted = false;
};
