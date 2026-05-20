// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ACPTypes.h"

#include "Interfaces/IHttpRequest.h"

/**
 * Provider identifier enum for quick switching
 */
enum class EChatGatewayProviderId : uint8
{
	OpenRouter,
	OpenAI,
	DeepSeek,
	Google,
	Anthropic,
	Groq,
	Ollama,
	Vercel,
	Custom
};

/**
 * Abstract interface for Chat Gateway API providers.
 * Each provider knows how to talk to a specific OpenAI-compatible API endpoint.
 */
class AGENTINTEGRATIONKIT_API IChatGatewayProvider
{
public:
	virtual ~IChatGatewayProvider() = default;

	// ---- Identity ----
	virtual EChatGatewayProviderId GetProviderId() const = 0;
	virtual FString GetProviderIdString() const = 0;     // "openrouter", "deepseek", etc.
	virtual FString GetDisplayName() const = 0;           // "OpenRouter", "DeepSeek", etc.
	virtual FString GetDescription() const = 0;           // Short description for UI

	// ---- Connection ----
	virtual FString GetDefaultBaseUrl() const = 0;
	virtual FString GetChatCompletionsPath() const { return TEXT("/chat/completions"); }
	virtual FString GetModelsPath() const { return TEXT("/models"); }

	/** Configure provider-specific headers on the HTTP request (e.g. X-Title for OpenRouter) */
	virtual void ConfigureRequestHeaders(TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request, const FString& ApiKey) const
	{
		Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
	}

	// ---- Models ----
	virtual FString GetDefaultModel() const = 0;
	virtual TArray<FACPModelInfo> GetHardcodedModels() const = 0;
	virtual bool SupportsModelDiscovery() const { return false; }

	/** Parse the response from the models endpoint into FACPModelInfo array */
	virtual TArray<FACPModelInfo> ParseModelsResponse(const FString& ResponseBody) const { return {}; }

	/** Get curated/featured models to show in the dropdown (subset of all models) */
	virtual TArray<FACPModelInfo> GetCuratedModels(
		const FString& CurrentModelId,
		const TArray<FACPModelInfo>& RecentModels,
		const TArray<FACPModelInfo>& CachedModels) const;

	// ---- Model capabilities ----
	virtual bool ModelSupportsReasoning(const FString& ModelId) const { return false; }
	virtual bool SupportsModelBrowsing() const { return false; }

	// ---- Request customization ----
	/** Allow provider to add custom fields to the request body (e.g. reasoning params) */
	virtual void CustomizeRequestBody(TSharedRef<FJsonObject> RequestBody, const FString& ModelId) const {}

	// ---- Error handling ----
	virtual FString FormatErrorMessage(int32 ResponseCode, const FString& ResponseBody, bool bUsingBetideCredits) const;

	// ---- Model ID handling ----
	/** Some providers use provider/model format (OpenRouter), others just model name */
	virtual FString GetProviderFromModelId(const FString& ModelId) const { return TEXT(""); }

	// ---- Auth requirement ----
	virtual bool RequiresApiKey() const { return true; }
};

// ============================================================================
// Concrete Provider Implementations
// ============================================================================

/**
 * OpenRouter — 400+ models, full model discovery, Betide credits support
 */
class AGENTINTEGRATIONKIT_API FOpenRouterProvider : public IChatGatewayProvider
{
public:
	virtual EChatGatewayProviderId GetProviderId() const override { return EChatGatewayProviderId::OpenRouter; }
	virtual FString GetProviderIdString() const override { return TEXT("openrouter"); }
	virtual FString GetDisplayName() const override { return TEXT("OpenRouter"); }
	virtual FString GetDescription() const override { return TEXT("400+ models from all major providers via OpenRouter"); }

	virtual FString GetDefaultBaseUrl() const override { return TEXT("https://openrouter.ai/api/v1"); }
	virtual FString GetDefaultModel() const override { return TEXT("anthropic/claude-sonnet-4"); }
	virtual TArray<FACPModelInfo> GetHardcodedModels() const override;
	virtual bool SupportsModelDiscovery() const override { return true; }
	virtual bool SupportsModelBrowsing() const override { return true; }
	virtual TArray<FACPModelInfo> ParseModelsResponse(const FString& ResponseBody) const override;
	virtual TArray<FACPModelInfo> GetCuratedModels(const FString& CurrentModelId, const TArray<FACPModelInfo>& RecentModels, const TArray<FACPModelInfo>& CachedModels) const override;

	virtual void ConfigureRequestHeaders(TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request, const FString& ApiKey) const override;
	virtual bool ModelSupportsReasoning(const FString& ModelId) const override;
	virtual FString FormatErrorMessage(int32 ResponseCode, const FString& ResponseBody, bool bUsingBetideCredits) const override;
	virtual FString GetProviderFromModelId(const FString& ModelId) const override;
};

/**
 * OpenAI — Direct OpenAI API access
 */
class AGENTINTEGRATIONKIT_API FOpenAIDirectProvider : public IChatGatewayProvider
{
public:
	virtual EChatGatewayProviderId GetProviderId() const override { return EChatGatewayProviderId::OpenAI; }
	virtual FString GetProviderIdString() const override { return TEXT("openai"); }
	virtual FString GetDisplayName() const override { return TEXT("OpenAI"); }
	virtual FString GetDescription() const override { return TEXT("Direct access to OpenAI models (GPT-4o, o3, etc.)"); }

	virtual FString GetDefaultBaseUrl() const override { return TEXT("https://api.openai.com/v1"); }
	virtual FString GetDefaultModel() const override { return TEXT("gpt-4o"); }
	virtual TArray<FACPModelInfo> GetHardcodedModels() const override;
	virtual bool SupportsModelDiscovery() const override { return true; }
	virtual TArray<FACPModelInfo> ParseModelsResponse(const FString& ResponseBody) const override;

	virtual bool ModelSupportsReasoning(const FString& ModelId) const override;
};

/**
 * DeepSeek — Direct DeepSeek API
 */
class AGENTINTEGRATIONKIT_API FDeepSeekProvider : public IChatGatewayProvider
{
public:
	virtual EChatGatewayProviderId GetProviderId() const override { return EChatGatewayProviderId::DeepSeek; }
	virtual FString GetProviderIdString() const override { return TEXT("deepseek"); }
	virtual FString GetDisplayName() const override { return TEXT("DeepSeek"); }
	virtual FString GetDescription() const override { return TEXT("Direct access to DeepSeek models"); }

	virtual FString GetDefaultBaseUrl() const override { return TEXT("https://api.deepseek.com/v1"); }
	virtual FString GetDefaultModel() const override { return TEXT("deepseek-chat"); }
	virtual TArray<FACPModelInfo> GetHardcodedModels() const override;
	virtual bool ModelSupportsReasoning(const FString& ModelId) const override;
};

/**
 * Google Gemini — Via OpenAI-compatible endpoint
 */
class AGENTINTEGRATIONKIT_API FGoogleGeminiProvider : public IChatGatewayProvider
{
public:
	virtual EChatGatewayProviderId GetProviderId() const override { return EChatGatewayProviderId::Google; }
	virtual FString GetProviderIdString() const override { return TEXT("google"); }
	virtual FString GetDisplayName() const override { return TEXT("Google Gemini"); }
	virtual FString GetDescription() const override { return TEXT("Google Gemini models via OpenAI-compatible API"); }

	virtual FString GetDefaultBaseUrl() const override { return TEXT("https://generativelanguage.googleapis.com/v1beta/openai"); }
	virtual FString GetChatCompletionsPath() const override { return TEXT("/chat/completions"); }
	virtual FString GetDefaultModel() const override { return TEXT("gemini-2.5-flash"); }
	virtual TArray<FACPModelInfo> GetHardcodedModels() const override;
	virtual bool SupportsModelDiscovery() const override { return false; }

	virtual bool ModelSupportsReasoning(const FString& ModelId) const override;
};

/**
 * Groq — Fast inference
 */
class AGENTINTEGRATIONKIT_API FGroqProvider : public IChatGatewayProvider
{
public:
	virtual EChatGatewayProviderId GetProviderId() const override { return EChatGatewayProviderId::Groq; }
	virtual FString GetProviderIdString() const override { return TEXT("groq"); }
	virtual FString GetDisplayName() const override { return TEXT("Groq"); }
	virtual FString GetDescription() const override { return TEXT("Ultra-fast inference via Groq"); }

	virtual FString GetDefaultBaseUrl() const override { return TEXT("https://api.groq.com/openai/v1"); }
	virtual FString GetDefaultModel() const override { return TEXT("llama-3.3-70b-versatile"); }
	virtual TArray<FACPModelInfo> GetHardcodedModels() const override;
	virtual bool SupportsModelDiscovery() const override { return true; }
	virtual TArray<FACPModelInfo> ParseModelsResponse(const FString& ResponseBody) const override;
};

/**
 * Ollama — Local inference, no API key required
 */
class AGENTINTEGRATIONKIT_API FOllamaProvider : public IChatGatewayProvider
{
public:
	virtual EChatGatewayProviderId GetProviderId() const override { return EChatGatewayProviderId::Ollama; }
	virtual FString GetProviderIdString() const override { return TEXT("ollama"); }
	virtual FString GetDisplayName() const override { return TEXT("Ollama (Local)"); }
	virtual FString GetDescription() const override { return TEXT("Run models locally via Ollama"); }

	virtual FString GetDefaultBaseUrl() const override { return TEXT("http://localhost:11434/v1"); }
	virtual FString GetDefaultModel() const override { return TEXT("llama3.2"); }
	virtual TArray<FACPModelInfo> GetHardcodedModels() const override;
	virtual bool SupportsModelDiscovery() const override { return true; }
	virtual TArray<FACPModelInfo> ParseModelsResponse(const FString& ResponseBody) const override;
	virtual bool RequiresApiKey() const override { return false; }

	virtual void ConfigureRequestHeaders(TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request, const FString& ApiKey) const override;
};

/**
 * Vercel AI Gateway
 */
class AGENTINTEGRATIONKIT_API FVercelAIGatewayProvider : public IChatGatewayProvider
{
public:
	virtual EChatGatewayProviderId GetProviderId() const override { return EChatGatewayProviderId::Vercel; }
	virtual FString GetProviderIdString() const override { return TEXT("vercel"); }
	virtual FString GetDisplayName() const override { return TEXT("Vercel AI Gateway"); }
	virtual FString GetDescription() const override { return TEXT("Route through Vercel AI Gateway"); }

	virtual FString GetDefaultBaseUrl() const override { return TEXT("https://gateway.ai.vercel.app/v1"); }
	virtual FString GetDefaultModel() const override { return TEXT("gpt-4o"); }
	virtual TArray<FACPModelInfo> GetHardcodedModels() const override;
};

/**
 * Custom — User provides URL, key, model
 */
class AGENTINTEGRATIONKIT_API FCustomChatProvider : public IChatGatewayProvider
{
public:
	virtual EChatGatewayProviderId GetProviderId() const override { return EChatGatewayProviderId::Custom; }
	virtual FString GetProviderIdString() const override { return TEXT("custom"); }
	virtual FString GetDisplayName() const override { return TEXT("Custom API"); }
	virtual FString GetDescription() const override { return TEXT("Any OpenAI-compatible API endpoint"); }

	virtual FString GetDefaultBaseUrl() const override { return TEXT(""); }
	virtual FString GetDefaultModel() const override { return TEXT(""); }
	virtual TArray<FACPModelInfo> GetHardcodedModels() const override { return {}; }
	virtual bool SupportsModelDiscovery() const override { return true; }
	virtual TArray<FACPModelInfo> ParseModelsResponse(const FString& ResponseBody) const override;
};

// Need full definition for FCustomProviderDefinition member
#include "ACPSettings.h"

/**
 * User-Defined Provider — instantiated from FCustomProviderDefinition.
 * Users can create multiple of these, each with unique ID, base URL, and model list.
 */
class AGENTINTEGRATIONKIT_API FUserDefinedProvider : public IChatGatewayProvider
{
public:
	explicit FUserDefinedProvider(const FCustomProviderDefinition& InDefinition);

	virtual EChatGatewayProviderId GetProviderId() const override { return EChatGatewayProviderId::Custom; }
	virtual FString GetProviderIdString() const override;
	virtual FString GetDisplayName() const override;
	virtual FString GetDescription() const override;

	virtual FString GetDefaultBaseUrl() const override;
	virtual FString GetDefaultModel() const override;
	virtual TArray<FACPModelInfo> GetHardcodedModels() const override;
	virtual bool SupportsModelDiscovery() const override;
	virtual TArray<FACPModelInfo> ParseModelsResponse(const FString& ResponseBody) const override;
	virtual bool RequiresApiKey() const override;

	/** Update the definition (when user edits models or settings) */
	void UpdateDefinition(const FCustomProviderDefinition& NewDefinition);

	/** Check if this is a user-defined provider instance */
	bool IsUserDefined() const { return true; }

private:
	FCustomProviderDefinition Definition;
};

// ============================================================================
// Provider Registry — static helpers for provider management
// ============================================================================

namespace ChatGateway
{
	/** Get all available provider instances (singleton pattern per provider) */
	AGENTINTEGRATIONKIT_API TArray<TSharedRef<IChatGatewayProvider>> GetAllProviders();

	/** Find provider by string ID */
	AGENTINTEGRATIONKIT_API TSharedPtr<IChatGatewayProvider> FindProvider(const FString& ProviderId);

	/** Find provider by enum ID */
	AGENTINTEGRATIONKIT_API TSharedPtr<IChatGatewayProvider> FindProvider(EChatGatewayProviderId ProviderId);

	/** Get the default provider (OpenRouter) */
	AGENTINTEGRATIONKIT_API TSharedRef<IChatGatewayProvider> GetDefaultProvider();

	/** Sync custom providers from settings — adds/removes/updates FUserDefinedProvider instances */
	AGENTINTEGRATIONKIT_API void SyncCustomProviders();
}
