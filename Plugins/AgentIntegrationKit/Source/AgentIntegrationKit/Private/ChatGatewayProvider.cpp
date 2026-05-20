// Copyright 2026 Betide Studio. All Rights Reserved.

#include "ChatGatewayProvider.h"
#include "ACPSettings.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Interfaces/IHttpRequest.h"

// ============================================================================
// IChatGatewayProvider defaults
// ============================================================================

TArray<FACPModelInfo> IChatGatewayProvider::GetCuratedModels(
	const FString& CurrentModelId,
	const TArray<FACPModelInfo>& RecentModels,
	const TArray<FACPModelInfo>& CachedModels) const
{
	// Default: just return hardcoded models
	return GetHardcodedModels();
}

FString IChatGatewayProvider::FormatErrorMessage(int32 ResponseCode, const FString& ResponseBody, bool bUsingBetideCredits) const
{
	// Try to extract error message from JSON response
	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
	if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
	{
		FString Message;
		if (Json->TryGetStringField(TEXT("error"), Message) && !Message.IsEmpty())
		{
			return FString::Printf(TEXT("%s API error %d: %s"), *GetDisplayName(), ResponseCode, *Message);
		}
		if (Json->TryGetStringField(TEXT("message"), Message) && !Message.IsEmpty())
		{
			return FString::Printf(TEXT("%s API error %d: %s"), *GetDisplayName(), ResponseCode, *Message);
		}
		const TSharedPtr<FJsonObject>* ErrorObj = nullptr;
		if (Json->TryGetObjectField(TEXT("error"), ErrorObj) && ErrorObj && ErrorObj->IsValid())
		{
			if ((*ErrorObj)->TryGetStringField(TEXT("message"), Message) && !Message.IsEmpty())
			{
				return FString::Printf(TEXT("%s API error %d: %s"), *GetDisplayName(), ResponseCode, *Message);
			}
		}
	}

	return FString::Printf(TEXT("%s API error %d: %s"), *GetDisplayName(), ResponseCode, *ResponseBody.Left(800));
}

// ============================================================================
// OpenRouter Provider
// ============================================================================

TArray<FACPModelInfo> FOpenRouterProvider::GetHardcodedModels() const
{
	TArray<FACPModelInfo> Models;

	{ FACPModelInfo M; M.ModelId = TEXT("anthropic/claude-sonnet-4"); M.Name = TEXT("Claude Sonnet 4"); M.Description = TEXT("Anthropic's Claude Sonnet 4"); Models.Add(M); }
	{ FACPModelInfo M; M.ModelId = TEXT("anthropic/claude-3.5-sonnet"); M.Name = TEXT("Claude 3.5 Sonnet"); M.Description = TEXT("Anthropic's Claude 3.5 Sonnet"); Models.Add(M); }
	{ FACPModelInfo M; M.ModelId = TEXT("openai/gpt-4o"); M.Name = TEXT("GPT-4o"); M.Description = TEXT("OpenAI's GPT-4o"); Models.Add(M); }
	{ FACPModelInfo M; M.ModelId = TEXT("google/gemini-2.0-flash-001"); M.Name = TEXT("Gemini 2.0 Flash"); M.Description = TEXT("Google's Gemini 2.0 Flash"); Models.Add(M); }
	{ FACPModelInfo M; M.ModelId = TEXT("deepseek/deepseek-chat"); M.Name = TEXT("DeepSeek Chat"); M.Description = TEXT("DeepSeek's Chat model"); Models.Add(M); }

	return Models;
}

TArray<FACPModelInfo> FOpenRouterProvider::ParseModelsResponse(const FString& ResponseBody) const
{
	TArray<FACPModelInfo> AllModels;

	TSharedPtr<FJsonObject> JsonRoot;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
	if (!FJsonSerializer::Deserialize(Reader, JsonRoot) || !JsonRoot.IsValid())
	{
		return AllModels;
	}

	const TArray<TSharedPtr<FJsonValue>>* DataArray;
	if (!JsonRoot->TryGetArrayField(TEXT("data"), DataArray))
	{
		return AllModels;
	}

	for (const TSharedPtr<FJsonValue>& ModelVal : *DataArray)
	{
		TSharedPtr<FJsonObject> ModelObj = ModelVal->AsObject();
		if (!ModelObj.IsValid()) continue;

		FACPModelInfo ModelInfo;
		ModelObj->TryGetStringField(TEXT("id"), ModelInfo.ModelId);
		ModelObj->TryGetStringField(TEXT("name"), ModelInfo.Name);
		ModelObj->TryGetStringField(TEXT("description"), ModelInfo.Description);

		const TArray<TSharedPtr<FJsonValue>>* SupportedParamsArray;
		if (ModelObj->TryGetArrayField(TEXT("supported_parameters"), SupportedParamsArray))
		{
			for (const TSharedPtr<FJsonValue>& ParamVal : *SupportedParamsArray)
			{
				FString ParamStr;
				if (ParamVal->TryGetString(ParamStr))
				{
					ModelInfo.SupportedParameters.Add(ParamStr);
				}
			}
		}

		if (ModelInfo.ModelId.IsEmpty()) continue;
		AllModels.Add(ModelInfo);
	}

	AllModels.Sort([](const FACPModelInfo& A, const FACPModelInfo& B) { return A.Name < B.Name; });
	return AllModels;
}

TArray<FACPModelInfo> FOpenRouterProvider::GetCuratedModels(
	const FString& CurrentModelId,
	const TArray<FACPModelInfo>& RecentModels,
	const TArray<FACPModelInfo>& CachedModels) const
{
	TArray<FACPModelInfo> Curated;
	TSet<FString> AddedIds;

	// 0a. Current active model
	if (!CurrentModelId.IsEmpty() && !CurrentModelId.StartsWith(TEXT("special:")))
	{
		const FACPModelInfo* CachedInfo = nullptr;
		for (const FACPModelInfo& M : CachedModels)
		{
			if (M.ModelId == CurrentModelId) { CachedInfo = &M; break; }
		}

		if (CachedInfo)
		{
			Curated.Add(*CachedInfo);
		}
		else
		{
			FACPModelInfo ActiveModel;
			ActiveModel.ModelId = CurrentModelId;
			int32 SlashIdx = INDEX_NONE;
			if (CurrentModelId.FindChar(TEXT('/'), SlashIdx))
			{
				ActiveModel.Name = CurrentModelId.Mid(SlashIdx + 1);
			}
			else
			{
				ActiveModel.Name = CurrentModelId;
			}
			Curated.Add(ActiveModel);
		}
		AddedIds.Add(CurrentModelId);
	}

	// 0b. Recent models
	for (const FACPModelInfo& Recent : RecentModels)
	{
		if (!AddedIds.Contains(Recent.ModelId))
		{
			Curated.Add(Recent);
			AddedIds.Add(Recent.ModelId);
		}
	}

	// Featured models
	struct FeaturedModel { const TCHAR* Id; const TCHAR* Name; const TCHAR* Desc; };
	static const FeaturedModel Featured[] = {
		{ TEXT("anthropic/claude-sonnet-4.5"), TEXT("Claude Sonnet 4.5"), TEXT("Anthropic's balanced model") },
		{ TEXT("anthropic/claude-opus-4.5"), TEXT("Claude Opus 4.5"), TEXT("Anthropic's high-performance model") },
		{ TEXT("google/gemini-3-pro-preview"), TEXT("Gemini 3 Pro Preview"), TEXT("Google's advanced multimodal model") },
		{ TEXT("google/gemini-3-flash-preview"), TEXT("Gemini 3 Flash Preview"), TEXT("Google's fast multimodal model") },
		{ TEXT("z-ai/glm-4.7"), TEXT("GLM 4.7"), TEXT("Zhipu AI's general purpose model") },
	};

	for (const auto& F : Featured)
	{
		if (!AddedIds.Contains(F.Id))
		{
			FACPModelInfo M;
			M.ModelId = F.Id;
			M.Name = F.Name;
			M.Description = F.Desc;
			Curated.Add(M);
			AddedIds.Add(M.ModelId);
		}
	}

	// "Browse all" sentinel
	{
		FACPModelInfo M;
		M.ModelId = TEXT("special:browse_all");
		M.Name = TEXT("Browse all models...");
		M.Description = TEXT("Click to search 400+ available models");
		Curated.Add(M);
	}

	return Curated;
}

void FOpenRouterProvider::ConfigureRequestHeaders(TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request, const FString& ApiKey) const
{
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
	Request->SetHeader(TEXT("HTTP-Referer"), TEXT("https://github.com/betidestudio/AgentIntegrationKit"));
	Request->SetHeader(TEXT("X-Title"), TEXT("Agent Integration Kit"));
}

bool FOpenRouterProvider::ModelSupportsReasoning(const FString& ModelId) const
{
	FString LowerModel = ModelId.ToLower();
	return LowerModel.Contains(TEXT("anthropic/")) ||
	       LowerModel.Contains(TEXT("openai/")) ||
	       LowerModel.Contains(TEXT("deepseek/"));
}

FString FOpenRouterProvider::FormatErrorMessage(int32 ResponseCode, const FString& ResponseBody, bool bUsingBetideCredits) const
{
	if (bUsingBetideCredits && ResponseCode == 402 && ResponseBody.Contains(TEXT("insufficient credits"), ESearchCase::IgnoreCase))
	{
		return FString::Printf(
			TEXT("OpenRouter API error %d: Insufficient NeoStack credits. Top up at https://betide.studio/dashboard/neostack"),
			ResponseCode);
	}

	return IChatGatewayProvider::FormatErrorMessage(ResponseCode, ResponseBody, bUsingBetideCredits);
}

FString FOpenRouterProvider::GetProviderFromModelId(const FString& ModelId) const
{
	int32 SlashIndex = ModelId.Find(TEXT("/"));
	if (SlashIndex != INDEX_NONE)
	{
		return ModelId.Left(SlashIndex).ToLower();
	}
	return TEXT("unknown");
}

// ============================================================================
// OpenAI Direct Provider
// ============================================================================

TArray<FACPModelInfo> FOpenAIDirectProvider::GetHardcodedModels() const
{
	TArray<FACPModelInfo> Models;

	{ FACPModelInfo M; M.ModelId = TEXT("gpt-4o"); M.Name = TEXT("GPT-4o"); M.Description = TEXT("Most capable GPT-4 model"); Models.Add(M); }
	{ FACPModelInfo M; M.ModelId = TEXT("gpt-4o-mini"); M.Name = TEXT("GPT-4o Mini"); M.Description = TEXT("Fast and affordable"); Models.Add(M); }
	{ FACPModelInfo M; M.ModelId = TEXT("o3"); M.Name = TEXT("o3"); M.Description = TEXT("Advanced reasoning model"); Models.Add(M); }
	{ FACPModelInfo M; M.ModelId = TEXT("o3-mini"); M.Name = TEXT("o3 Mini"); M.Description = TEXT("Fast reasoning model"); Models.Add(M); }
	{ FACPModelInfo M; M.ModelId = TEXT("o4-mini"); M.Name = TEXT("o4 Mini"); M.Description = TEXT("Latest reasoning model"); Models.Add(M); }
	{ FACPModelInfo M; M.ModelId = TEXT("gpt-4.1"); M.Name = TEXT("GPT-4.1"); M.Description = TEXT("Latest GPT model"); Models.Add(M); }
	{ FACPModelInfo M; M.ModelId = TEXT("gpt-4.1-mini"); M.Name = TEXT("GPT-4.1 Mini"); M.Description = TEXT("Fast latest GPT model"); Models.Add(M); }

	return Models;
}

TArray<FACPModelInfo> FOpenAIDirectProvider::ParseModelsResponse(const FString& ResponseBody) const
{
	TArray<FACPModelInfo> Models;

	TSharedPtr<FJsonObject> JsonRoot;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
	if (!FJsonSerializer::Deserialize(Reader, JsonRoot) || !JsonRoot.IsValid())
	{
		return Models;
	}

	const TArray<TSharedPtr<FJsonValue>>* DataArray;
	if (!JsonRoot->TryGetArrayField(TEXT("data"), DataArray))
	{
		return Models;
	}

	for (const TSharedPtr<FJsonValue>& ModelVal : *DataArray)
	{
		TSharedPtr<FJsonObject> ModelObj = ModelVal->AsObject();
		if (!ModelObj.IsValid()) continue;

		FACPModelInfo ModelInfo;
		ModelObj->TryGetStringField(TEXT("id"), ModelInfo.ModelId);
		ModelInfo.Name = ModelInfo.ModelId; // OpenAI API doesn't return display names

		// Filter to only chat models (skip embeddings, tts, whisper, dall-e, etc.)
		FString Id = ModelInfo.ModelId.ToLower();
		if (Id.StartsWith(TEXT("gpt")) || Id.StartsWith(TEXT("o1")) || Id.StartsWith(TEXT("o3")) || Id.StartsWith(TEXT("o4")) || Id.StartsWith(TEXT("chatgpt")))
		{
			Models.Add(ModelInfo);
		}
	}

	Models.Sort([](const FACPModelInfo& A, const FACPModelInfo& B) { return A.Name < B.Name; });
	return Models;
}

bool FOpenAIDirectProvider::ModelSupportsReasoning(const FString& ModelId) const
{
	FString Lower = ModelId.ToLower();
	return Lower.StartsWith(TEXT("o1")) || Lower.StartsWith(TEXT("o3")) || Lower.StartsWith(TEXT("o4"));
}

// ============================================================================
// DeepSeek Provider
// ============================================================================

TArray<FACPModelInfo> FDeepSeekProvider::GetHardcodedModels() const
{
	TArray<FACPModelInfo> Models;

	{ FACPModelInfo M; M.ModelId = TEXT("deepseek-chat"); M.Name = TEXT("DeepSeek Chat (V3)"); M.Description = TEXT("General-purpose chat model"); Models.Add(M); }
	{ FACPModelInfo M; M.ModelId = TEXT("deepseek-reasoner"); M.Name = TEXT("DeepSeek Reasoner (R1)"); M.Description = TEXT("Advanced reasoning model"); Models.Add(M); }

	return Models;
}

bool FDeepSeekProvider::ModelSupportsReasoning(const FString& ModelId) const
{
	return ModelId.Contains(TEXT("reasoner"));
}

// ============================================================================
// Google Gemini Provider
// ============================================================================

TArray<FACPModelInfo> FGoogleGeminiProvider::GetHardcodedModels() const
{
	TArray<FACPModelInfo> Models;

	{ FACPModelInfo M; M.ModelId = TEXT("gemini-2.5-flash"); M.Name = TEXT("Gemini 2.5 Flash"); M.Description = TEXT("Fast and capable"); Models.Add(M); }
	{ FACPModelInfo M; M.ModelId = TEXT("gemini-2.5-pro"); M.Name = TEXT("Gemini 2.5 Pro"); M.Description = TEXT("Advanced reasoning"); Models.Add(M); }
	{ FACPModelInfo M; M.ModelId = TEXT("gemini-2.0-flash"); M.Name = TEXT("Gemini 2.0 Flash"); M.Description = TEXT("Previous generation fast model"); Models.Add(M); }

	return Models;
}

bool FGoogleGeminiProvider::ModelSupportsReasoning(const FString& ModelId) const
{
	return ModelId.Contains(TEXT("pro")) || ModelId.Contains(TEXT("2.5"));
}

// ============================================================================
// Groq Provider
// ============================================================================

TArray<FACPModelInfo> FGroqProvider::GetHardcodedModels() const
{
	TArray<FACPModelInfo> Models;

	{ FACPModelInfo M; M.ModelId = TEXT("llama-3.3-70b-versatile"); M.Name = TEXT("Llama 3.3 70B"); M.Description = TEXT("Meta's versatile large model"); Models.Add(M); }
	{ FACPModelInfo M; M.ModelId = TEXT("llama-3.1-8b-instant"); M.Name = TEXT("Llama 3.1 8B"); M.Description = TEXT("Ultra-fast small model"); Models.Add(M); }
	{ FACPModelInfo M; M.ModelId = TEXT("mixtral-8x7b-32768"); M.Name = TEXT("Mixtral 8x7B"); M.Description = TEXT("Mistral's MoE model"); Models.Add(M); }
	{ FACPModelInfo M; M.ModelId = TEXT("gemma2-9b-it"); M.Name = TEXT("Gemma 2 9B"); M.Description = TEXT("Google's open model"); Models.Add(M); }

	return Models;
}

TArray<FACPModelInfo> FGroqProvider::ParseModelsResponse(const FString& ResponseBody) const
{
	// Same format as OpenAI
	TArray<FACPModelInfo> Models;

	TSharedPtr<FJsonObject> JsonRoot;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
	if (!FJsonSerializer::Deserialize(Reader, JsonRoot) || !JsonRoot.IsValid())
	{
		return Models;
	}

	const TArray<TSharedPtr<FJsonValue>>* DataArray;
	if (!JsonRoot->TryGetArrayField(TEXT("data"), DataArray))
	{
		return Models;
	}

	for (const TSharedPtr<FJsonValue>& ModelVal : *DataArray)
	{
		TSharedPtr<FJsonObject> ModelObj = ModelVal->AsObject();
		if (!ModelObj.IsValid()) continue;

		FACPModelInfo ModelInfo;
		ModelObj->TryGetStringField(TEXT("id"), ModelInfo.ModelId);
		ModelInfo.Name = ModelInfo.ModelId;

		if (!ModelInfo.ModelId.IsEmpty())
		{
			Models.Add(ModelInfo);
		}
	}

	Models.Sort([](const FACPModelInfo& A, const FACPModelInfo& B) { return A.Name < B.Name; });
	return Models;
}

// ============================================================================
// Ollama Provider
// ============================================================================

TArray<FACPModelInfo> FOllamaProvider::GetHardcodedModels() const
{
	TArray<FACPModelInfo> Models;

	{ FACPModelInfo M; M.ModelId = TEXT("llama3.2"); M.Name = TEXT("Llama 3.2"); M.Description = TEXT("Meta's latest compact model"); Models.Add(M); }
	{ FACPModelInfo M; M.ModelId = TEXT("qwen2.5-coder"); M.Name = TEXT("Qwen 2.5 Coder"); M.Description = TEXT("Alibaba's coding model"); Models.Add(M); }
	{ FACPModelInfo M; M.ModelId = TEXT("deepseek-r1"); M.Name = TEXT("DeepSeek R1"); M.Description = TEXT("Reasoning model"); Models.Add(M); }
	{ FACPModelInfo M; M.ModelId = TEXT("mistral"); M.Name = TEXT("Mistral"); M.Description = TEXT("Mistral 7B"); Models.Add(M); }

	return Models;
}

TArray<FACPModelInfo> FOllamaProvider::ParseModelsResponse(const FString& ResponseBody) const
{
	TArray<FACPModelInfo> Models;

	TSharedPtr<FJsonObject> JsonRoot;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
	if (!FJsonSerializer::Deserialize(Reader, JsonRoot) || !JsonRoot.IsValid())
	{
		return Models;
	}

	// Ollama uses "models" array (not "data")
	const TArray<TSharedPtr<FJsonValue>>* ModelsArray;
	if (JsonRoot->TryGetArrayField(TEXT("models"), ModelsArray))
	{
		for (const TSharedPtr<FJsonValue>& ModelVal : *ModelsArray)
		{
			TSharedPtr<FJsonObject> ModelObj = ModelVal->AsObject();
			if (!ModelObj.IsValid()) continue;

			FACPModelInfo ModelInfo;
			ModelObj->TryGetStringField(TEXT("name"), ModelInfo.ModelId);
			ModelObj->TryGetStringField(TEXT("name"), ModelInfo.Name);

			if (!ModelInfo.ModelId.IsEmpty())
			{
				Models.Add(ModelInfo);
			}
		}
	}
	// Also support OpenAI-compat format (Ollama v1 API)
	else if (JsonRoot->TryGetArrayField(TEXT("data"), ModelsArray))
	{
		for (const TSharedPtr<FJsonValue>& ModelVal : *ModelsArray)
		{
			TSharedPtr<FJsonObject> ModelObj = ModelVal->AsObject();
			if (!ModelObj.IsValid()) continue;

			FACPModelInfo ModelInfo;
			ModelObj->TryGetStringField(TEXT("id"), ModelInfo.ModelId);
			ModelInfo.Name = ModelInfo.ModelId;

			if (!ModelInfo.ModelId.IsEmpty())
			{
				Models.Add(ModelInfo);
			}
		}
	}

	return Models;
}

void FOllamaProvider::ConfigureRequestHeaders(TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request, const FString& ApiKey) const
{
	// Ollama doesn't need auth, but if user provided a key, use it (for remote Ollama setups)
	if (!ApiKey.IsEmpty())
	{
		Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
	}
}

// ============================================================================
// Vercel AI Gateway Provider
// ============================================================================

TArray<FACPModelInfo> FVercelAIGatewayProvider::GetHardcodedModels() const
{
	TArray<FACPModelInfo> Models;

	{ FACPModelInfo M; M.ModelId = TEXT("gpt-4o"); M.Name = TEXT("GPT-4o"); M.Description = TEXT("Via Vercel AI Gateway"); Models.Add(M); }
	{ FACPModelInfo M; M.ModelId = TEXT("claude-sonnet-4"); M.Name = TEXT("Claude Sonnet 4"); M.Description = TEXT("Via Vercel AI Gateway"); Models.Add(M); }

	return Models;
}

// ============================================================================
// Custom Provider
// ============================================================================

TArray<FACPModelInfo> FCustomChatProvider::ParseModelsResponse(const FString& ResponseBody) const
{
	// Try standard OpenAI format
	TArray<FACPModelInfo> Models;

	TSharedPtr<FJsonObject> JsonRoot;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
	if (!FJsonSerializer::Deserialize(Reader, JsonRoot) || !JsonRoot.IsValid())
	{
		return Models;
	}

	const TArray<TSharedPtr<FJsonValue>>* DataArray;
	if (!JsonRoot->TryGetArrayField(TEXT("data"), DataArray))
	{
		return Models;
	}

	for (const TSharedPtr<FJsonValue>& ModelVal : *DataArray)
	{
		TSharedPtr<FJsonObject> ModelObj = ModelVal->AsObject();
		if (!ModelObj.IsValid()) continue;

		FACPModelInfo ModelInfo;
		ModelObj->TryGetStringField(TEXT("id"), ModelInfo.ModelId);
		ModelObj->TryGetStringField(TEXT("name"), ModelInfo.Name);
		if (ModelInfo.Name.IsEmpty()) ModelInfo.Name = ModelInfo.ModelId;

		if (!ModelInfo.ModelId.IsEmpty())
		{
			Models.Add(ModelInfo);
		}
	}

	Models.Sort([](const FACPModelInfo& A, const FACPModelInfo& B) { return A.Name < B.Name; });
	return Models;
}

// ============================================================================
// User-Defined Provider
// ============================================================================

FUserDefinedProvider::FUserDefinedProvider(const FCustomProviderDefinition& InDefinition)
	: Definition(InDefinition)
{
}

FString FUserDefinedProvider::GetProviderIdString() const { return Definition.ProviderId; }
FString FUserDefinedProvider::GetDisplayName() const { return Definition.DisplayName; }
FString FUserDefinedProvider::GetDescription() const { return FString::Printf(TEXT("Custom: %s"), *Definition.BaseUrl); }
FString FUserDefinedProvider::GetDefaultBaseUrl() const { return Definition.BaseUrl; }
bool FUserDefinedProvider::SupportsModelDiscovery() const { return Definition.bEnableModelDiscovery; }
bool FUserDefinedProvider::RequiresApiKey() const { return Definition.bRequiresApiKey; }

FString FUserDefinedProvider::GetDefaultModel() const
{
	if (Definition.Models.Num() > 0)
	{
		return Definition.Models[0].ModelId;
	}
	return TEXT("");
}

TArray<FACPModelInfo> FUserDefinedProvider::GetHardcodedModels() const
{
	TArray<FACPModelInfo> Models;
	for (const FCustomProviderModelEntry& Entry : Definition.Models)
	{
		FACPModelInfo M;
		M.ModelId = Entry.ModelId;
		M.Name = Entry.DisplayName.IsEmpty() ? Entry.ModelId : Entry.DisplayName;
		M.Description = Entry.Description;
		Models.Add(M);
	}
	return Models;
}

TArray<FACPModelInfo> FUserDefinedProvider::ParseModelsResponse(const FString& ResponseBody) const
{
	// Reuse the same OpenAI-compatible format parsing as FCustomChatProvider
	TArray<FACPModelInfo> Models;

	TSharedPtr<FJsonObject> JsonRoot;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
	if (!FJsonSerializer::Deserialize(Reader, JsonRoot) || !JsonRoot.IsValid())
	{
		return Models;
	}

	const TArray<TSharedPtr<FJsonValue>>* DataArray;
	if (!JsonRoot->TryGetArrayField(TEXT("data"), DataArray))
	{
		return Models;
	}

	for (const TSharedPtr<FJsonValue>& ModelVal : *DataArray)
	{
		TSharedPtr<FJsonObject> ModelObj = ModelVal->AsObject();
		if (!ModelObj.IsValid()) continue;

		FACPModelInfo ModelInfo;
		ModelObj->TryGetStringField(TEXT("id"), ModelInfo.ModelId);
		ModelObj->TryGetStringField(TEXT("name"), ModelInfo.Name);
		if (ModelInfo.Name.IsEmpty()) ModelInfo.Name = ModelInfo.ModelId;

		if (!ModelInfo.ModelId.IsEmpty())
		{
			Models.Add(ModelInfo);
		}
	}

	Models.Sort([](const FACPModelInfo& A, const FACPModelInfo& B) { return A.Name < B.Name; });
	return Models;
}

void FUserDefinedProvider::UpdateDefinition(const FCustomProviderDefinition& NewDefinition)
{
	Definition = NewDefinition;
}

// ============================================================================
// Provider Registry
// ============================================================================

namespace ChatGateway
{
	static TArray<TSharedRef<IChatGatewayProvider>> ProviderInstances;
	static bool bInitialized = false;
	static FCriticalSection ProviderLock;

	static void EnsureInitialized()
	{
		if (bInitialized) return;
		bInitialized = true;

		ProviderInstances.Add(MakeShared<FOpenRouterProvider>());
		ProviderInstances.Add(MakeShared<FOpenAIDirectProvider>());
		ProviderInstances.Add(MakeShared<FDeepSeekProvider>());
		ProviderInstances.Add(MakeShared<FGoogleGeminiProvider>());
		ProviderInstances.Add(MakeShared<FGroqProvider>());
		ProviderInstances.Add(MakeShared<FOllamaProvider>());
		ProviderInstances.Add(MakeShared<FVercelAIGatewayProvider>());
		ProviderInstances.Add(MakeShared<FCustomChatProvider>());
	}

	// Internal sync — caller must hold ProviderLock
	static void SyncCustomProvidersInternal()
	{
		const UACPSettings* Settings = UACPSettings::Get();
		if (!Settings) return;

		TSet<FString> DesiredIds;
		for (const FCustomProviderDefinition& Def : Settings->CustomProviders)
		{
			DesiredIds.Add(Def.ProviderId);
		}

		// Remove stale user-defined providers
		ProviderInstances.RemoveAll([&](const TSharedRef<IChatGatewayProvider>& Prov)
		{
			const FString& Id = Prov->GetProviderIdString();
			return Id.StartsWith(TEXT("userprovider_")) && !DesiredIds.Contains(Id);
		});

		// Add or update
		for (const FCustomProviderDefinition& Def : Settings->CustomProviders)
		{
			bool bFound = false;
			for (const TSharedRef<IChatGatewayProvider>& Prov : ProviderInstances)
			{
				if (Prov->GetProviderIdString() == Def.ProviderId)
				{
					static_cast<FUserDefinedProvider*>(&Prov.Get())->UpdateDefinition(Def);
					bFound = true;
					break;
				}
			}
			if (!bFound)
			{
				ProviderInstances.Add(MakeShared<FUserDefinedProvider>(Def));
			}
		}
	}

	void SyncCustomProviders()
	{
		FScopeLock Lock(&ProviderLock);
		EnsureInitialized();
		SyncCustomProvidersInternal();
	}

	TArray<TSharedRef<IChatGatewayProvider>> GetAllProviders()
	{
		FScopeLock Lock(&ProviderLock);
		EnsureInitialized();
		SyncCustomProvidersInternal();
		return ProviderInstances;
	}

	TSharedPtr<IChatGatewayProvider> FindProvider(const FString& ProviderId)
	{
		FScopeLock Lock(&ProviderLock);
		EnsureInitialized();
		SyncCustomProvidersInternal();
		for (const auto& Provider : ProviderInstances)
		{
			if (Provider->GetProviderIdString() == ProviderId)
			{
				return Provider;
			}
		}
		return nullptr;
	}

	TSharedPtr<IChatGatewayProvider> FindProvider(EChatGatewayProviderId ProviderId)
	{
		FScopeLock Lock(&ProviderLock);
		EnsureInitialized();
		for (const auto& Provider : ProviderInstances)
		{
			if (Provider->GetProviderId() == ProviderId)
			{
				return Provider;
			}
		}
		return nullptr;
	}

	TSharedRef<IChatGatewayProvider> GetDefaultProvider()
	{
		EnsureInitialized();
		return ProviderInstances[0]; // OpenRouter
	}
}
