// Copyright 2026 Betide Studio. All Rights Reserved.

#include "ChatGatewayClient.h"
#include "AgentIntegrationKitModule.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/Guid.h"
#include "Async/Async.h"
#include "MCPServer.h"
#include "MCPTypes.h"
#include "Tools/NeoStackToolRegistry.h"
#include "ACPAttachmentManager.h"
#include "ACPSettings.h"

FChatGatewayClient::FChatGatewayClient()
	: Provider(ChatGateway::GetDefaultProvider())
	, bIsCancelled(false)
{
	InitializeCapabilities();
}

FChatGatewayClient::FChatGatewayClient(TSharedRef<IChatGatewayProvider> InProvider)
	: Provider(InProvider)
	, bIsCancelled(false)
{
	InitializeCapabilities();
}

FChatGatewayClient::~FChatGatewayClient()
{
	bAlive->Store(false);
	Disconnect();
}

void FChatGatewayClient::SetProvider(TSharedRef<IChatGatewayProvider> InProvider)
{
	Provider = InProvider;
}

void FChatGatewayClient::RefreshModels()
{
	// Invalidate cache and re-collect hardcoded models from enabled providers
	CachedModels.Empty();
	ProvidersWithDiscoveredModels.Empty();
	LastModelsFetch = FDateTime();

	// Re-initialize hardcoded models from all providers in priority order
	SessionModelState.AvailableModels.Empty();
	TMap<FString, int32> ModelIdToIndex; // for merging ServableByProviders
	if (const UACPSettings* Settings = UACPSettings::Get())
	{
		for (const FString& ProviderId : Settings->GetProviderPriority())
		{
			TSharedPtr<IChatGatewayProvider> Prov = ChatGateway::FindProvider(ProviderId);
			if (!Prov.IsValid()) continue;
			if (Prov->RequiresApiKey() && Settings->GetProviderApiKey(ProviderId).IsEmpty()) continue;

			for (FACPModelInfo M : Prov->GetHardcodedModels())
			{
				if (int32* ExistingIdx = ModelIdToIndex.Find(M.ModelId))
				{
					// Model already exists — merge ServableByProviders
					FACPModelInfo& Existing = SessionModelState.AvailableModels[*ExistingIdx];
					Existing.ServableByProviders.AddUnique(ProviderId);
				}
				else
				{
					M.ProviderId = ProviderId;
					M.ProviderDisplayName = Prov->GetDisplayName();
					M.ServableByProviders.AddUnique(ProviderId);
					ModelIdToIndex.Add(M.ModelId, SessionModelState.AvailableModels.Num());
					SessionModelState.AvailableModels.Add(MoveTemp(M));
				}
			}

			// Include user-added extra models for this built-in provider
			if (const TArray<FCustomProviderModelEntry>* Extras = Settings->GetExtraProviderModels(ProviderId))
			{
				for (const FCustomProviderModelEntry& E : *Extras)
				{
					if (!ModelIdToIndex.Contains(E.ModelId))
					{
						FACPModelInfo M;
						M.ModelId = E.ModelId;
						M.Name = E.DisplayName;
						M.Description = E.Description;
						M.ProviderId = ProviderId;
						M.ProviderDisplayName = Prov->GetDisplayName();
						M.ServableByProviders.AddUnique(ProviderId);
						ModelIdToIndex.Add(M.ModelId, SessionModelState.AvailableModels.Num());
						SessionModelState.AvailableModels.Add(MoveTemp(M));
					}
				}
			}
		}
	}

	// Broadcast immediately with hardcoded models, then fetch discovery models async
	OnModelsAvailable.Broadcast(SessionModelState);
	FetchModels();
}

void FChatGatewayClient::ConfigureRequest(TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request)
{
	Request->SetURL(BaseUrl);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Provider->ConfigureRequestHeaders(Request, ApiKey);
}

void FChatGatewayClient::InitializeCapabilities()
{
	AgentCapabilities.bSupportsNewSession = true;
	AgentCapabilities.bSupportsLoadSession = false;
	AgentCapabilities.bSupportsResumeSession = false;
	AgentCapabilities.bSupportsAudio = false;
	AgentCapabilities.bSupportsImage = false;

	// Collect hardcoded models from all providers in priority order, merging ServableByProviders
	SessionModelState.AvailableModels.Empty();
	TMap<FString, int32> ModelIdToIndex;
	if (const UACPSettings* Settings = UACPSettings::Get())
	{
		for (const FString& ProviderId : Settings->GetProviderPriority())
		{
			TSharedPtr<IChatGatewayProvider> Prov = ChatGateway::FindProvider(ProviderId);
			if (!Prov.IsValid()) continue;

			// Only include if API key is set (or not required)
			if (Prov->RequiresApiKey() && Settings->GetProviderApiKey(ProviderId).IsEmpty()) continue;

			for (FACPModelInfo M : Prov->GetHardcodedModels())
			{
				if (int32* ExistingIdx = ModelIdToIndex.Find(M.ModelId))
				{
					// Model already exists — merge ServableByProviders
					FACPModelInfo& Existing = SessionModelState.AvailableModels[*ExistingIdx];
					Existing.ServableByProviders.AddUnique(ProviderId);
				}
				else
				{
					M.ProviderId = ProviderId;
					M.ProviderDisplayName = Prov->GetDisplayName();
					M.ServableByProviders.AddUnique(ProviderId);
					ModelIdToIndex.Add(M.ModelId, SessionModelState.AvailableModels.Num());
					SessionModelState.AvailableModels.Add(MoveTemp(M));
				}
			}
		}
	}
	else
	{
		SessionModelState.AvailableModels = Provider->GetHardcodedModels();
	}
}

void FChatGatewayClient::FetchModels()
{
	const UACPSettings* Settings = UACPSettings::Get();

	// Check cache — applies across all providers
	if (CachedModels.Num() > 0 && LastModelsFetch.GetTicks() > 0)
	{
		FDateTime Now = FDateTime::Now();
		if ((Now - LastModelsFetch).GetTotalHours() < ModelsCacheTTLHours)
		{
			SessionModelState.AvailableModels = GetCuratedModels();
			OnModelsAvailable.Broadcast(SessionModelState);
			return;
		}
	}

	// Collect providers in priority order that support model discovery and have keys
	TArray<TSharedRef<IChatGatewayProvider>> ProvidersToFetch;
	if (Settings)
	{
		for (const FString& ProviderId : Settings->GetProviderPriority())
		{
			TSharedPtr<IChatGatewayProvider> Prov = ChatGateway::FindProvider(ProviderId);
			if (!Prov.IsValid()) continue;
			if (Prov->RequiresApiKey() && Settings->GetProviderApiKey(ProviderId).IsEmpty()) continue;
			if (Prov->SupportsModelDiscovery())
			{
				ProvidersToFetch.Add(Prov.ToSharedRef());
			}
		}
	}
	else if (Provider->SupportsModelDiscovery())
	{
		ProvidersToFetch.Add(Provider);
	}

	if (ProvidersToFetch.Num() == 0)
	{
		// No providers support discovery — use hardcoded models from InitializeCapabilities
		SessionModelState.AvailableModels = GetCuratedModels();
		OnModelsAvailable.Broadcast(SessionModelState);
		return;
	}

	// Use a shared counter to know when all requests are done
	struct FMultiProviderFetchState
	{
		TAtomic<int32> PendingCount;
		FCriticalSection Lock;
		TArray<FACPModelInfo> AllModels;
		TSet<FString> SucceededProviders; // Track which providers returned models

		FMultiProviderFetchState(int32 Count) : PendingCount(Count) {}
	};

	TSharedRef<FMultiProviderFetchState> FetchState = MakeShared<FMultiProviderFetchState>(ProvidersToFetch.Num());

	for (const TSharedRef<IChatGatewayProvider>& Prov : ProvidersToFetch)
	{
		FString ProviderId = Prov->GetProviderIdString();
		FString ProviderName = Prov->GetDisplayName();

		UE_LOG(LogAgentIntegrationKit, Log, TEXT("ChatGateway: Fetching models from %s..."), *ProviderName);

		// Build models URL for this provider
		FString ModelsUrl;
		if (Settings && Prov->GetProviderId() == EChatGatewayProviderId::OpenRouter)
		{
			ModelsUrl = Settings->GetOpenRouterModelsUrl();
		}
		if (ModelsUrl.IsEmpty())
		{
			FString ProviderBaseUrl = Prov->GetDefaultBaseUrl();
			if (Settings)
			{
				FString Override = Settings->GetProviderBaseUrl(ProviderId);
				if (!Override.IsEmpty()) ProviderBaseUrl = Override;
			}
			ModelsUrl = ProviderBaseUrl + Prov->GetModelsPath();
		}

		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(ModelsUrl);
		Request->SetVerb(TEXT("GET"));
		Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

		// Set auth header
		FString ProvApiKey;
		if (Settings) ProvApiKey = Settings->GetProviderApiKey(ProviderId);
		if (!ProvApiKey.IsEmpty())
		{
			Prov->ConfigureRequestHeaders(Request, ProvApiKey);
		}

		// Capture provider info for the callback
		TSharedRef<TAtomic<bool>> FetchAliveFlag = bAlive;
		Request->OnProcessRequestComplete().BindLambda(
			[this, FetchAliveFlag, FetchState, Prov, ProviderId, ProviderName](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bOk)
			{
				if (!FetchAliveFlag->Load()) return;
				if (bOk && Resp.IsValid() && Resp->GetResponseCode() == 200)
				{
					TArray<FACPModelInfo> Models = Prov->ParseModelsResponse(Resp->GetContentAsString());
					// Tag each model with provider info and ServableByProviders
					for (FACPModelInfo& M : Models)
					{
						M.ProviderId = ProviderId;
						M.ProviderDisplayName = ProviderName;
						M.ServableByProviders.AddUnique(ProviderId);
					}

					FScopeLock Lock(&FetchState->Lock);
					// Only mark discovery as succeeded if models were actually returned;
					// empty response should fall back to hardcoded models
					UE_LOG(LogAgentIntegrationKit, Log, TEXT("ChatGateway: %s returned %d models"), *ProviderName, Models.Num());
					if (Models.Num() > 0)
					{
						FetchState->SucceededProviders.Add(ProviderId);
					}
					FetchState->AllModels.Append(MoveTemp(Models));
				}
				else
				{
					UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ChatGateway: Models fetch failed for %s (code %d)"),
						*ProviderName, Resp.IsValid() ? Resp->GetResponseCode() : -1);
				}

				int32 Remaining = --FetchState->PendingCount;
				if (Remaining == 0)
				{
					// All providers done — deduplicate, merge ServableByProviders, and broadcast
					FScopeLock Lock(&FetchState->Lock);

					// Deduplicate by ModelId, merging ServableByProviders arrays
					TMap<FString, int32> IdToIdx;
					TArray<FACPModelInfo> Merged;
					for (FACPModelInfo& M : FetchState->AllModels)
					{
						if (int32* Idx = IdToIdx.Find(M.ModelId))
						{
							// Merge provider into existing entry
							for (const FString& Pid : M.ServableByProviders)
							{
								Merged[*Idx].ServableByProviders.AddUnique(Pid);
							}
						}
						else
						{
							IdToIdx.Add(M.ModelId, Merged.Num());
							Merged.Add(MoveTemp(M));
						}
					}

					// OpenRouter can serve models from providers that ALSO use OpenRouter-compatible IDs
					// (e.g. "anthropic/claude-sonnet-4"). But it should NOT claim it can serve
					// local provider models (e.g. Ollama's "llama3.2") — those must stay with
					// their original provider to ensure correct routing and UI grouping.
					// Only add OpenRouter as servable for models that were discovered FROM OpenRouter.
					// (Models already have their original provider in ServableByProviders from the fetch.)

					Merged.Sort([](const FACPModelInfo& A, const FACPModelInfo& B)
					{
						return A.Name < B.Name;
					});

					CachedModels = MoveTemp(Merged);
					ProvidersWithDiscoveredModels = MoveTemp(FetchState->SucceededProviders);
					LastModelsFetch = FDateTime::Now();

					// Log per-provider results for diagnostics
					for (const FString& SucceededId : ProvidersWithDiscoveredModels)
					{
						int32 Count = 0;
						for (const FACPModelInfo& CM : CachedModels)
						{
							if (CM.ProviderId == SucceededId) ++Count;
						}
						UE_LOG(LogAgentIntegrationKit, Log, TEXT("ChatGateway: Provider '%s' contributed %d models to cache"), *SucceededId, Count);
					}

					SessionModelState.AvailableModels = GetCuratedModels();

					// Log provider grouping in curated list
					{
						TMap<FString, int32> ProviderCounts;
						for (const FACPModelInfo& CM : SessionModelState.AvailableModels)
						{
							FString Key = CM.ProviderDisplayName.IsEmpty() ? CM.ProviderId : CM.ProviderDisplayName;
							ProviderCounts.FindOrAdd(Key)++;
						}
						for (const auto& Pair : ProviderCounts)
						{
							UE_LOG(LogAgentIntegrationKit, Log, TEXT("ChatGateway: Curated models from '%s': %d"), *Pair.Key, Pair.Value);
						}
					}

					UE_LOG(LogAgentIntegrationKit, Log, TEXT("ChatGateway: Fetched and cached %d models across all providers. Showing %d curated."),
						CachedModels.Num(), SessionModelState.AvailableModels.Num());

					OnModelsAvailable.Broadcast(SessionModelState);
				}
			});

		if (!Request->ProcessRequest())
		{
			UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ChatGateway: Failed to initiate models request for %s"), *ProviderName);
			int32 Remaining = --FetchState->PendingCount;
			if (Remaining == 0)
			{
				SessionModelState.AvailableModels = GetCuratedModels();
				OnModelsAvailable.Broadcast(SessionModelState);
			}
		}
	}
}

TArray<FACPModelInfo> FChatGatewayClient::GetCuratedModels()
{
	const UACPSettings* Settings = UACPSettings::Get();
	if (!Settings)
	{
		return Provider->GetCuratedModels(Model, RecentModels, CachedModels);
	}

	TArray<FACPModelInfo> AllCurated;
	TSet<FString> AddedIds;

	// Recent models first (across providers)
	for (const FACPModelInfo& M : RecentModels)
	{
		if (!AddedIds.Contains(M.ModelId))
		{
			// Resolve display name to the provider that would actually serve this model
			FACPModelInfo Resolved = M;
			TSharedPtr<IChatGatewayProvider> ResolvedProv = ResolveProviderForModel(M.ModelId);
			if (ResolvedProv.IsValid())
			{
				Resolved.ProviderDisplayName = ResolvedProv->GetDisplayName();
			}
			AllCurated.Add(MoveTemp(Resolved));
			AddedIds.Add(M.ModelId);
		}
	}

	// Flat list: subset of cached models sorted by name
	for (const FACPModelInfo& M : CachedModels)
	{
		if (AddedIds.Contains(M.ModelId)) continue;

		FACPModelInfo Resolved = M;
		TSharedPtr<IChatGatewayProvider> ResolvedProv = ResolveProviderForModel(M.ModelId);
		if (ResolvedProv.IsValid())
		{
			Resolved.ProviderDisplayName = ResolvedProv->GetDisplayName();
		}
		AllCurated.Add(MoveTemp(Resolved));
		AddedIds.Add(M.ModelId);
	}

	// Include hardcoded models from providers that don't use model discovery
	// (e.g. custom providers with manually-added models), or from ALL providers
	// if no cached models have been fetched yet.
	for (const FString& ProviderId : Settings->GetProviderPriority())
	{
		TSharedPtr<IChatGatewayProvider> Prov = ChatGateway::FindProvider(ProviderId);
		if (!Prov.IsValid()) continue;
		if (Prov->RequiresApiKey() && Settings->GetProviderApiKey(ProviderId).IsEmpty()) continue;

		// Skip hardcoded models only for providers whose discovery actually succeeded
		if (ProvidersWithDiscoveredModels.Contains(ProviderId)) continue;

		for (const FACPModelInfo& M : Prov->GetHardcodedModels())
		{
			if (AddedIds.Contains(M.ModelId)) continue;

			FACPModelInfo Copy = M;
			Copy.ProviderId = ProviderId;
			Copy.ProviderDisplayName = Prov->GetDisplayName();
			Copy.ServableByProviders.AddUnique(ProviderId);
			AllCurated.Add(MoveTemp(Copy));
			AddedIds.Add(M.ModelId);
		}
	}

	// Filter by enabled models (if user has configured any)
	const TSet<FString>& EnabledModels = Settings->GetEnabledModels();
	if (EnabledModels.Num() > 0)
	{
		AllCurated.RemoveAll([&](const FACPModelInfo& M)
		{
			// Always keep the currently active model even if not in enabled set
			if (M.ModelId == Model) return false;
			return !EnabledModels.Contains(M.ModelId);
		});
	}

	// Add a single "Browse All" at the end if any provider supports browsing
	for (const FString& ProviderId : Settings->GetProviderPriority())
	{
		TSharedPtr<IChatGatewayProvider> Prov = ChatGateway::FindProvider(ProviderId);
		if (Prov.IsValid() && Prov->SupportsModelBrowsing())
		{
			FACPModelInfo BrowseAll;
			BrowseAll.ModelId = TEXT("special:browse_all");
			BrowseAll.Name = TEXT("Browse All Models...");
			AllCurated.Add(MoveTemp(BrowseAll));
			break;
		}
	}

	return AllCurated;
}

void FChatGatewayClient::AddRecentModel(const FACPModelInfo& RecentModel)
{
	// Remove if exists (to move to top)
	RecentModels.RemoveAll([&](const FACPModelInfo& M) { return M.ModelId == RecentModel.ModelId; });

	// Add to top
	RecentModels.Insert(RecentModel, 0);

	// Limit size
	if (RecentModels.Num() > 5)
	{
		RecentModels.SetNum(5);
	}

	// Update available models and broadcast
	SessionModelState.AvailableModels = GetCuratedModels();
	OnModelsAvailable.Broadcast(SessionModelState);
}

const FACPModelInfo* FChatGatewayClient::GetModelInfo(const FString& ModelId) const
{
	// First check cached models
	for (const FACPModelInfo& M : CachedModels)
	{
		if (M.ModelId == ModelId)
		{
			return &M;
		}
	}

	// Check recent models
	for (const FACPModelInfo& M : RecentModels)
	{
		if (M.ModelId == ModelId)
		{
			return &M;
		}
	}

	return nullptr;
}

bool FChatGatewayClient::CurrentModelSupportsReasoning() const
{
	const FACPModelInfo* ModelInfo = GetModelInfo(Model);
	if (ModelInfo)
	{
		return ModelInfo->SupportsReasoning();
	}

	// Fall back to provider-level check
	return Provider->ModelSupportsReasoning(Model);
}

void FChatGatewayClient::OnModelsFetchComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess)
{
	if (!bSuccess || !Response.IsValid() || Response->GetResponseCode() != 200)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ChatGateway [%s]: Models fetch failed (code %d), using fallback"),
			*Provider->GetDisplayName(),
			Response.IsValid() ? Response->GetResponseCode() : -1);
		SessionModelState.AvailableModels = GetCuratedModels();
		OnModelsAvailable.Broadcast(SessionModelState);
		return;
	}

	TArray<FACPModelInfo> AllModels = Provider->ParseModelsResponse(Response->GetContentAsString());

	// Sort alphabetically
	AllModels.Sort([](const FACPModelInfo& A, const FACPModelInfo& B)
	{
		return A.Name < B.Name;
	});

	// Store full list in cache
	CachedModels = AllModels;
	LastModelsFetch = FDateTime::Now();

	// Expose only curated list to dropdown
	SessionModelState.AvailableModels = GetCuratedModels();

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ChatGateway [%s]: Fetched and cached %d models. Showing %d curated."),
		*Provider->GetDisplayName(), AllModels.Num(), SessionModelState.AvailableModels.Num());

	OnModelsAvailable.Broadcast(SessionModelState);
}

bool FChatGatewayClient::Connect(const FACPAgentConfig& Config)
{
	if (IsConnected())
	{
		Disconnect();
	}

	CurrentConfig = Config;
	SetState(EACPClientState::Connecting, FString::Printf(TEXT("Connecting to %s..."), *Provider->GetDisplayName()));

	// Resolve auth token, base URL, and model from per-provider settings
	ApiKey = Config.ApiKey;
	BaseUrl = Provider->GetDefaultBaseUrl() + Provider->GetChatCompletionsPath();
	Model = Config.ModelId.IsEmpty() ? Provider->GetDefaultModel() : Config.ModelId;

	if (UACPSettings* Settings = UACPSettings::Get())
	{
		const FString ProviderId = Provider->GetProviderIdString();

		// OpenRouter with Betide credits has special proxy routing
		if (Provider->GetProviderId() == EChatGatewayProviderId::OpenRouter)
		{
			ApiKey = Settings->GetOpenRouterAuthToken();
			BaseUrl = Settings->GetOpenRouterChatCompletionsUrl();
		}
		else
		{
			// All other providers: read API key and base URL from per-provider settings
			FString ProviderKey = Settings->GetProviderApiKey(ProviderId);
			if (!ProviderKey.IsEmpty())
			{
				ApiKey = ProviderKey;
			}

			FString ProviderUrl = Settings->GetProviderBaseUrl(ProviderId);
			if (!ProviderUrl.IsEmpty())
			{
				BaseUrl = ProviderUrl + Provider->GetChatCompletionsPath();
			}
		}

		// If no model specified, use the provider's default model
		if (Config.ModelId.IsEmpty())
		{
			Model = Provider->GetDefaultModel();
		}
	}

	if (ApiKey.IsEmpty() && Provider->RequiresApiKey())
	{
		// Primary provider has no auth — try to fall back to any configured provider
		bool bFoundFallback = false;
		if (const UACPSettings* FallbackSettings = UACPSettings::Get())
		{
			for (const FString& FallbackId : FallbackSettings->GetProviderPriority())
			{
				if (FallbackId == Provider->GetProviderIdString()) continue;
				TSharedPtr<IChatGatewayProvider> FallbackProv = ChatGateway::FindProvider(FallbackId);
				if (!FallbackProv.IsValid()) continue;
				if (FallbackProv->RequiresApiKey() && FallbackSettings->GetProviderApiKey(FallbackId).IsEmpty()) continue;

				UE_LOG(LogAgentIntegrationKit, Log, TEXT("ChatGateway: %s has no auth, falling back to %s"),
					*Provider->GetDisplayName(), *FallbackProv->GetDisplayName());

				Provider = FallbackProv.ToSharedRef();
				ApiKey = FallbackSettings->GetProviderApiKey(FallbackId);
				FString FallbackUrl = FallbackSettings->GetProviderBaseUrl(FallbackId);
				BaseUrl = (FallbackUrl.IsEmpty() ? Provider->GetDefaultBaseUrl() : FallbackUrl)
					+ Provider->GetChatCompletionsPath();
				// Keep user's saved model if set (it may belong to this fallback provider);
				// otherwise use fallback provider's default
				if (Config.ModelId.IsEmpty())
				{
					Model = Provider->GetDefaultModel();
				}
				bFoundFallback = true;
				break;
			}
		}

		if (!bFoundFallback)
		{
			SetState(EACPClientState::Error, FString::Printf(TEXT("%s credentials are required"), *Provider->GetDisplayName()));
			return false;
		}
	}

	// Apply resolved model
	Model = Model.IsEmpty() ? Provider->GetDefaultModel() : Model;
	SessionModelState.CurrentModelId = Model;

	// Re-resolve provider based on the actual model (e.g. saved model may belong to a custom provider)
	{
		TSharedPtr<IChatGatewayProvider> ResolvedProvider = ResolveProviderForModel(Model);
		if (ResolvedProvider.IsValid() && ResolvedProvider->GetProviderIdString() != Provider->GetProviderIdString())
		{
			UE_LOG(LogAgentIntegrationKit, Log, TEXT("ChatGateway: Initial model %s belongs to %s, switching from %s"),
				*Model, *ResolvedProvider->GetDisplayName(), *Provider->GetDisplayName());

			Provider = ResolvedProvider.ToSharedRef();

			if (UACPSettings* ResolveSettings = UACPSettings::Get())
			{
				const FString ResolvedId = Provider->GetProviderIdString();
				if (Provider->GetProviderId() == EChatGatewayProviderId::OpenRouter)
				{
					ApiKey = ResolveSettings->GetOpenRouterAuthToken();
					BaseUrl = ResolveSettings->GetOpenRouterChatCompletionsUrl();
				}
				else
				{
					FString ProviderKey = ResolveSettings->GetProviderApiKey(ResolvedId);
					if (!ProviderKey.IsEmpty()) ApiKey = ProviderKey;

					FString ProviderUrl = ResolveSettings->GetProviderBaseUrl(ResolvedId);
					BaseUrl = (ProviderUrl.IsEmpty() ? Provider->GetDefaultBaseUrl() : ProviderUrl)
						+ Provider->GetChatCompletionsPath();
				}
			}
		}
	}

	// Go directly to Ready state
	SetState(EACPClientState::Ready, FString::Printf(TEXT("Connected to %s"), *Provider->GetDisplayName()));

	FetchModels();

	return true;
}

void FChatGatewayClient::Disconnect()
{
	// Cancel any pending request
	if (CurrentRequest.IsValid())
	{
		CurrentRequest->CancelRequest();
		CurrentRequest.Reset();
	}

	// Clear state
	{
		FScopeLock Lock(&StateLock);
		ConversationHistory.Empty();
		CurrentSessionId.Empty();
		StreamBuffer.Empty();
		CurrentResponseText.Empty();
		LastProcessedLength = 0;
	}

	SetState(EACPClientState::Disconnected, TEXT("Disconnected"));
}

void FChatGatewayClient::NewSession(const FString& WorkingDirectory)
{
	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ChatGateway [%s]: Creating new session"), *Provider->GetDisplayName());

	// Generate a session ID
	CurrentSessionId = FGuid::NewGuid().ToString();

	// Clear conversation history and reset usage
	{
		FScopeLock Lock(&StateLock);
		ConversationHistory.Empty();
		SessionUsage = FACPUsageData();
	}

	// Add system message
	FChatGatewayMessage SystemMessage;
	SystemMessage.Role = TEXT("system");
	FString SystemPrompt = TEXT("You are a helpful AI assistant integrated into Unreal Engine. Help the user with their game development tasks.");

	// Append custom system prompt + active profile instructions
	UACPSettings* Settings = UACPSettings::Get();
	if (Settings)
	{
		FString EffectivePrompt = Settings->GetProfileSystemPromptAppend();
		if (!EffectivePrompt.IsEmpty())
		{
			SystemPrompt += TEXT("\n\n") + EffectivePrompt;
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ChatGateway [%s]: Added custom system prompt (with profile instructions)"), *Provider->GetDisplayName());
		}
	}

	SystemMessage.Content = SystemPrompt;
	ConversationHistory.Add(SystemMessage);

	SetState(EACPClientState::InSession, TEXT("Session started"));

	// Broadcast models available now that we're in session
	OnModelsAvailable.Broadcast(SessionModelState);
}

void FChatGatewayClient::LoadSession(const FString& SessionId)
{
	// Most providers don't support loading sessions
	UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ChatGateway [%s]: LoadSession not supported"), *Provider->GetDisplayName());
}

void FChatGatewayClient::SendPrompt(const FString& PromptText)
{
	if (ApiKey.IsEmpty() && Provider->RequiresApiKey())
	{
		OnError.Broadcast(-1, TEXT("API key not configured"));
		return;
	}

	SetState(EACPClientState::Prompting, TEXT("Processing..."));

	// Reset streaming state
	{
		FScopeLock Lock(&StateLock);
		StreamBuffer.Empty();
		CurrentResponseText.Empty();
		CurrentReasoningText.Empty();
		LastProcessedLength = 0;
		bIsCancelled = false;
		PendingToolCalls.Empty();
		CurrentToolCalls.Empty();
		CurrentToolCallIndex = 0;
		bIsProcessingTools = false;
	}

	// Build user message with attachment context prepended
	FString MessageContent = PromptText;

	// If there are attachments, prepend them as markdown context
	if (FACPAttachmentManager::Get().HasAttachments())
	{
		FString ContextMarkdown = FACPAttachmentManager::Get().SerializeAsMarkdown();
		MessageContent = ContextMarkdown + TEXT("## User Request\n\n") + PromptText;

		// Clear attachments after incorporating (one-shot context)
		FACPAttachmentManager::Get().ClearAllAttachments();
	}

	// Add user message to history
	FChatGatewayMessage UserMessage;
	UserMessage.Role = TEXT("user");
	UserMessage.Content = MessageContent;
	ConversationHistory.Add(UserMessage);

	// Build request body (includes tools)
	FString RequestBody = BuildRequestBody();

	// Create HTTP request
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	CurrentRequest = Request;

	ConfigureRequest(Request);
	Request->SetContentAsString(RequestBody);

	Request->OnProcessRequestComplete().BindRaw(this, &FChatGatewayClient::OnRequestComplete);
	Request->OnRequestProgress64().BindRaw(this, &FChatGatewayClient::OnRequestProgress64);

	// Send request
	if (!Request->ProcessRequest())
	{
		SetState(EACPClientState::InSession, TEXT("Ready"));
		OnError.Broadcast(-1, FString::Printf(TEXT("Failed to send request to %s"), *Provider->GetDisplayName()));
	}
}

void FChatGatewayClient::CancelPrompt()
{
	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ChatGateway [%s]: Cancelling prompt"), *Provider->GetDisplayName());

	bIsCancelled = true;

	if (CurrentRequest.IsValid())
	{
		CurrentRequest->CancelRequest();
		CurrentRequest.Reset();
	}

	SetState(EACPClientState::InSession, TEXT("Cancelled"));
}

void FChatGatewayClient::SetModel(const FString& ModelId)
{
	// UI sentinel action, not a real model ID.
	if (ModelId.StartsWith(TEXT("special:")))
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ChatGateway [%s]: Ignoring special model action '%s'"), *Provider->GetDisplayName(), *ModelId);
		return;
	}

	// Resolve the best provider for this model using priority-based routing
	TSharedPtr<IChatGatewayProvider> ResolvedProvider = ResolveProviderForModel(ModelId);
	if (ResolvedProvider.IsValid() && ResolvedProvider->GetProviderIdString() != Provider->GetProviderIdString())
	{
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("ChatGateway: Switching provider from %s to %s for model %s"),
			*Provider->GetDisplayName(), *ResolvedProvider->GetDisplayName(), *ModelId);

		Provider = ResolvedProvider.ToSharedRef();

		// Re-resolve API key and base URL for the new provider
		if (UACPSettings* Settings = UACPSettings::Get())
		{
			const FString ProviderId = Provider->GetProviderIdString();
			if (Provider->GetProviderId() == EChatGatewayProviderId::OpenRouter)
			{
				ApiKey = Settings->GetOpenRouterAuthToken();
				BaseUrl = Settings->GetOpenRouterChatCompletionsUrl();
			}
			else
			{
				FString ProviderKey = Settings->GetProviderApiKey(ProviderId);
				if (!ProviderKey.IsEmpty()) ApiKey = ProviderKey;

				FString ProviderUrl = Settings->GetProviderBaseUrl(ProviderId);
				BaseUrl = (ProviderUrl.IsEmpty() ? Provider->GetDefaultBaseUrl() : ProviderUrl)
					+ Provider->GetChatCompletionsPath();
			}
		}
	}

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ChatGateway [%s]: Setting model to %s"), *Provider->GetDisplayName(), *ModelId);

	Model = ModelId;
	SessionModelState.CurrentModelId = ModelId;
}

TSharedPtr<IChatGatewayProvider> FChatGatewayClient::ResolveProviderForModel(const FString& ModelId)
{
	const UACPSettings* Settings = UACPSettings::Get();
	if (!Settings)
	{
		return Provider;
	}

	// Look up the model's ServableByProviders from cached/recent models
	const FACPModelInfo* ModelInfo = GetModelInfo(ModelId);
	if (ModelInfo && ModelInfo->ServableByProviders.Num() > 0)
	{
		// Iterate ProviderPriority — first configured match wins
		for (const FString& PriorityId : Settings->GetProviderPriority())
		{
			if (ModelInfo->ServableByProviders.Contains(PriorityId) && Settings->IsProviderConfigured(PriorityId))
			{
				TSharedPtr<IChatGatewayProvider> Prov = ChatGateway::FindProvider(PriorityId);
				if (Prov.IsValid())
				{
					return Prov;
				}
			}
		}
	}

	// Model not in cache — check which provider's hardcoded list contains it
	// (handles custom providers with manually-added models)
	for (const FString& PriorityId : Settings->GetProviderPriority())
	{
		TSharedPtr<IChatGatewayProvider> Prov = ChatGateway::FindProvider(PriorityId);
		if (!Prov.IsValid()) continue;
		if (!Settings->IsProviderConfigured(PriorityId)) continue;

		for (const FACPModelInfo& M : Prov->GetHardcodedModels())
		{
			if (M.ModelId == ModelId)
			{
				return Prov;
			}
		}
	}

	// Fallback: OpenRouter can serve anything (it's a meta-provider)
	if (Settings->IsProviderConfigured(TEXT("openrouter")))
	{
		TSharedPtr<IChatGatewayProvider> OpenRouter = ChatGateway::FindProvider(TEXT("openrouter"));
		if (OpenRouter.IsValid())
		{
			return OpenRouter;
		}
	}

	// Last resort: current provider
	return Provider;
}

void FChatGatewayClient::SetMode(const FString& ModeId)
{
	// Most providers don't have modes, but we support the interface
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ChatGateway [%s]: SetMode called with %s (no-op)"), *Provider->GetDisplayName(), *ModeId);
}

void FChatGatewayClient::RespondToPermissionRequest(int32 RequestId, const FString& OptionId)
{
	// Most providers don't have permission requests
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ChatGateway [%s]: RespondToPermissionRequest called (no-op)"), *Provider->GetDisplayName());
}

TArray<FChatGatewayMessage> FChatGatewayClient::GetConversationHistory() const
{
	FScopeLock Lock(&StateLock);
	return ConversationHistory;
}

void FChatGatewayClient::RestoreConversationHistory(const TArray<FChatGatewayMessage>& History)
{
	FScopeLock Lock(&StateLock);
	ConversationHistory = History;
	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ChatGateway [%s]: Restored conversation history with %d messages"), *Provider->GetDisplayName(), History.Num());

	if (State != EACPClientState::InSession && State != EACPClientState::Prompting)
	{
		SetState(EACPClientState::InSession, TEXT("Session restored"));
	}
}

void FChatGatewayClient::SetState(EACPClientState NewState, const FString& Message)
{
	{
		FScopeLock Lock(&StateLock);
		State = NewState;
	}

	// Don't broadcast during engine shutdown - the task system may be torn down
	if (IsEngineExitRequested())
	{
		return;
	}

	// Broadcast on game thread — capture alive flag by value to prevent use-after-free
	TSharedRef<TAtomic<bool>> AliveFlag = bAlive;
	AsyncTask(ENamedThreads::GameThread, [this, AliveFlag, NewState, Message]()
	{
		if (!AliveFlag->Load()) return;
		OnStateChanged.Broadcast(NewState, Message);
	});
}

FString FChatGatewayClient::BuildRequestBody()
{
	TSharedRef<FJsonObject> RequestObj = MakeShared<FJsonObject>();

	RequestObj->SetStringField(TEXT("model"), Model);
	RequestObj->SetBoolField(TEXT("stream"), true);

	// Build messages array
	TArray<TSharedPtr<FJsonValue>> MessagesArray;
	for (const FChatGatewayMessage& Msg : ConversationHistory)
	{
		TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
		MsgObj->SetStringField(TEXT("role"), Msg.Role);

		// Handle tool response messages
		if (Msg.Role == TEXT("tool"))
		{
			MsgObj->SetStringField(TEXT("tool_call_id"), Msg.ToolCallId);
			MsgObj->SetStringField(TEXT("content"), Msg.Content);
		}
		// Handle assistant messages with tool calls
		else if (Msg.Role == TEXT("assistant") && Msg.ToolCalls.Num() > 0)
		{
			// Content can be null when there are tool calls
			if (!Msg.Content.IsEmpty())
			{
				MsgObj->SetStringField(TEXT("content"), Msg.Content);
			}
			else
			{
				MsgObj->SetField(TEXT("content"), MakeShared<FJsonValueNull>());
			}

			// Add tool_calls array
			TArray<TSharedPtr<FJsonValue>> ToolCallsArray;
			for (const FChatGatewayToolCall& TC : Msg.ToolCalls)
			{
				TSharedPtr<FJsonObject> TCObj = MakeShared<FJsonObject>();
				TCObj->SetStringField(TEXT("id"), TC.Id);
				TCObj->SetStringField(TEXT("type"), TEXT("function"));

				TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
				FuncObj->SetStringField(TEXT("name"), TC.Name);
				FuncObj->SetStringField(TEXT("arguments"), TC.Arguments);
				TCObj->SetObjectField(TEXT("function"), FuncObj);

				ToolCallsArray.Add(MakeShared<FJsonValueObject>(TCObj));
			}
			MsgObj->SetArrayField(TEXT("tool_calls"), ToolCallsArray);
		}
		// Regular message
		else
		{
			MsgObj->SetStringField(TEXT("content"), Msg.Content);
		}

		MessagesArray.Add(MakeShared<FJsonValueObject>(MsgObj));
	}
	RequestObj->SetArrayField(TEXT("messages"), MessagesArray);

	// Build tools array from MCP registered tools
	TArray<TSharedPtr<FJsonValue>> ToolsArray = BuildToolsArray();
	if (ToolsArray.Num() > 0)
	{
		RequestObj->SetArrayField(TEXT("tools"), ToolsArray);
	}

	// Optional parameters
	RequestObj->SetNumberField(TEXT("temperature"), 0.7);
	RequestObj->SetNumberField(TEXT("max_tokens"), 16384);

	// Add reasoning support if enabled and model supports it
	if (bReasoningEnabled && CurrentModelSupportsReasoning())
	{
		TSharedPtr<FJsonObject> ReasoningObj = MakeShared<FJsonObject>();
		ReasoningObj->SetStringField(TEXT("effort"), ReasoningEffort);
		RequestObj->SetObjectField(TEXT("reasoning"), ReasoningObj);

		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ChatGateway [%s]: Reasoning enabled with effort '%s'"), *Provider->GetDisplayName(), *ReasoningEffort);
	}

	// Serialize to string
	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(RequestObj, Writer);

	return OutputString;
}

TArray<TSharedPtr<FJsonValue>> FChatGatewayClient::BuildToolsArray()
{
	const UACPSettings* Settings = UACPSettings::Get();
	TArray<TSharedPtr<FJsonValue>> ToolsArray;

	// Get tools from MCP server
	const TMap<FString, FMCPToolDefinition>& MCPTools = FMCPServer::Get().GetRegisteredTools();

	for (const auto& Pair : MCPTools)
	{
		if (Settings && !Settings->IsToolEnabled(Pair.Key))
		{
			continue;
		}

		const FMCPToolDefinition& Tool = Pair.Value;

		// Apply description override from active profile
		FString EffectiveDesc = Tool.Description;
		if (Settings)
		{
			EffectiveDesc = Settings->GetEffectiveToolDescription(Tool.Name, Tool.Description);
		}

		TSharedPtr<FJsonObject> ToolDef = BuildToolDefinition(Tool.Name, EffectiveDesc, Tool.InputSchema);
		if (ToolDef.IsValid())
		{
			ToolsArray.Add(MakeShared<FJsonValueObject>(ToolDef));
		}
	}

	// Also get tools from NeoStack registry
	FNeoStackToolRegistry& Registry = FNeoStackToolRegistry::Get();
	TArray<FString> ToolNames = Registry.GetToolNames();

	for (const FString& ToolName : ToolNames)
	{
		// Skip if already added from MCP or disabled
		if (MCPTools.Contains(ToolName))
		{
			continue;
		}
		if (Settings && !Settings->IsToolEnabled(ToolName))
		{
			continue;
		}

		FNeoStackToolBase* Tool = Registry.GetTool(ToolName);
		if (Tool)
		{
			TSharedPtr<FJsonObject> Schema = Tool->GetInputSchema();
			if (!Schema.IsValid())
			{
				Schema = MakeShared<FJsonObject>();
				Schema->SetStringField(TEXT("type"), TEXT("object"));
				Schema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
			}

			// Apply description override from active profile
			FString EffectiveDesc = Tool->GetDescription();
			if (Settings)
			{
				EffectiveDesc = Settings->GetEffectiveToolDescription(Tool->GetName(), Tool->GetDescription());
			}

			TSharedPtr<FJsonObject> ToolDef = BuildToolDefinition(Tool->GetName(), EffectiveDesc, Schema);
			if (ToolDef.IsValid())
			{
				ToolsArray.Add(MakeShared<FJsonValueObject>(ToolDef));
			}
		}
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ChatGateway [%s]: Built %d tools for API request"), *Provider->GetDisplayName(), ToolsArray.Num());

	return ToolsArray;
}

TSharedPtr<FJsonObject> FChatGatewayClient::BuildToolDefinition(const FString& ToolName, const FString& Description, TSharedPtr<FJsonObject> InputSchema)
{
	TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
	ToolObj->SetStringField(TEXT("type"), TEXT("function"));

	TSharedPtr<FJsonObject> FunctionObj = MakeShared<FJsonObject>();
	FunctionObj->SetStringField(TEXT("name"), ToolName);
	FunctionObj->SetStringField(TEXT("description"), Description);

	// Use provided schema or create empty one
	if (InputSchema.IsValid())
	{
		FunctionObj->SetObjectField(TEXT("parameters"), InputSchema);
	}
	else
	{
		TSharedPtr<FJsonObject> EmptySchema = MakeShared<FJsonObject>();
		EmptySchema->SetStringField(TEXT("type"), TEXT("object"));
		EmptySchema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
		FunctionObj->SetObjectField(TEXT("parameters"), EmptySchema);
	}

	ToolObj->SetObjectField(TEXT("function"), FunctionObj);

	return ToolObj;
}

void FChatGatewayClient::OnRequestProgress64(FHttpRequestPtr Request, uint64 BytesSent, uint64 BytesReceived)
{
	if (bIsCancelled)
	{
		return;
	}

	// Get response content so far
	FHttpResponsePtr Response = Request->GetResponse();
	if (!Response.IsValid())
	{
		return;
	}

	FString Content = Response->GetContentAsString();

	// Only process new data
	if (Content.Len() > LastProcessedLength)
	{
		FString NewData = Content.Mid(LastProcessedLength);
		LastProcessedLength = Content.Len();

		// Add to buffer and process
		StreamBuffer += NewData;
		ProcessStreamBuffer();
	}
}

void FChatGatewayClient::OnRequestComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess)
{
	CurrentRequest.Reset();

	if (bIsCancelled)
	{
		return;
	}

	if (!bSuccess || !Response.IsValid())
	{
		FString ErrorMsg = TEXT("Request failed");
		if (Response.IsValid())
		{
			ErrorMsg = FString::Printf(TEXT("HTTP %d: %s"), Response->GetResponseCode(), *Response->GetContentAsString());
		}

		UE_LOG(LogAgentIntegrationKit, Error, TEXT("ChatGateway [%s]: %s"), *Provider->GetDisplayName(), *ErrorMsg);

		{
			TSharedRef<TAtomic<bool>> AliveFlag = bAlive;
			AsyncTask(ENamedThreads::GameThread, [this, AliveFlag, ErrorMsg]()
			{
				if (!AliveFlag->Load()) return;
				SetState(EACPClientState::InSession, TEXT("Ready"));
				OnError.Broadcast(-1, ErrorMsg);
			});
		}
		return;
	}

	int32 ResponseCode = Response->GetResponseCode();
	if (ResponseCode != 200)
	{
		const FString ResponseBody = Response->GetContentAsString();
		bool bUsingNeoStackCredits = false;
		if (const UACPSettings* Settings = UACPSettings::Get())
		{
			bUsingNeoStackCredits = Settings->ShouldUseBetideCredits();
		}

		const FString ErrorMsg = Provider->FormatErrorMessage(ResponseCode, ResponseBody, bUsingNeoStackCredits);

		UE_LOG(LogAgentIntegrationKit, Error, TEXT("ChatGateway [%s]: %s"), *Provider->GetDisplayName(), *ErrorMsg);

		{
			TSharedRef<TAtomic<bool>> AliveFlag = bAlive;
			AsyncTask(ENamedThreads::GameThread, [this, AliveFlag, ResponseCode, ErrorMsg]()
			{
				if (!AliveFlag->Load()) return;
				SetState(EACPClientState::InSession, TEXT("Ready"));
				OnError.Broadcast(ResponseCode, ErrorMsg);
			});
		}
		return;
	}

	// Process any remaining data in buffer
	FString FinalContent = Response->GetContentAsString();
	if (FinalContent.Len() > LastProcessedLength)
	{
		StreamBuffer += FinalContent.Mid(LastProcessedLength);
		ProcessStreamBuffer();
	}

	// Check if we have tool calls to process
	if (CurrentToolCalls.Num() > 0)
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ChatGateway [%s]: Response includes %d tool calls"), *Provider->GetDisplayName(), CurrentToolCalls.Num());

		// Add assistant message with tool calls to history
		FChatGatewayMessage AssistantMessage;
		AssistantMessage.Role = TEXT("assistant");
		AssistantMessage.Content = CurrentResponseText;
		AssistantMessage.ToolCalls = CurrentToolCalls;
		ConversationHistory.Add(AssistantMessage);

		// Copy tool calls to pending and process them
		PendingToolCalls = CurrentToolCalls;
		CurrentToolCalls.Empty();
		CurrentToolCallIndex = 0;
		bIsProcessingTools = true;

		// Start processing tool calls on game thread
		{
			TSharedRef<TAtomic<bool>> AliveFlag = bAlive;
			AsyncTask(ENamedThreads::GameThread, [this, AliveFlag]()
			{
				if (!AliveFlag->Load()) return;
				ProcessToolCalls(PendingToolCalls);
			});
		}
	}
	else
	{
		// No tool calls - this is a final response
		// Add assistant response to history
		if (!CurrentResponseText.IsEmpty())
		{
			FChatGatewayMessage AssistantMessage;
			AssistantMessage.Role = TEXT("assistant");
			AssistantMessage.Content = CurrentResponseText;
			ConversationHistory.Add(AssistantMessage);
		}

		// Broadcast completion response
		{
			TSharedRef<TAtomic<bool>> AliveFlag = bAlive;
			AsyncTask(ENamedThreads::GameThread, [this, AliveFlag]()
			{
				if (!AliveFlag->Load()) return;
				TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("text"), CurrentResponseText);
				OnResponse.Broadcast(Result);

				SetState(EACPClientState::InSession, TEXT("Ready"));
			});
		}
	}
}

void FChatGatewayClient::ProcessStreamBuffer()
{
	// Process complete SSE lines (double newline separated)
	int32 Pos;
	while (StreamBuffer.FindChar(TEXT('\n'), Pos))
	{
		FString Line = StreamBuffer.Left(Pos);
		StreamBuffer = StreamBuffer.Mid(Pos + 1);

		Line.TrimStartAndEndInline();
		if (!Line.IsEmpty())
		{
			ProcessSSELine(Line);
		}
	}
}

void FChatGatewayClient::ProcessSSELine(const FString& Line)
{
	// SSE format: "data: {json}"
	if (!Line.StartsWith(TEXT("data: ")))
	{
		return;
	}

	FString JsonStr = Line.Mid(6); // Skip "data: "

	// Check for stream end
	if (JsonStr == TEXT("[DONE]"))
	{
		return;
	}

	// Parse JSON
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ChatGateway [%s]: Failed to parse SSE JSON: %s"), *Provider->GetDisplayName(), *JsonStr);
		return;
	}

	// Check for usage object (present in the final SSE chunk)
	if (JsonObject->HasField(TEXT("usage")))
	{
		TSharedPtr<FJsonObject> UsageObj = JsonObject->GetObjectField(TEXT("usage"));
		if (UsageObj.IsValid())
		{
			FACPUsageData TurnUsage;

			// Parse token counts
			int32 PromptTokens = 0, CompletionTokens = 0, TotalTokens = 0;
			UsageObj->TryGetNumberField(TEXT("prompt_tokens"), PromptTokens);
			UsageObj->TryGetNumberField(TEXT("completion_tokens"), CompletionTokens);
			UsageObj->TryGetNumberField(TEXT("total_tokens"), TotalTokens);

			TurnUsage.InputTokens = PromptTokens;
			TurnUsage.OutputTokens = CompletionTokens;
			TurnUsage.TotalTokens = TotalTokens;

			// Parse cached tokens if available
			if (UsageObj->HasField(TEXT("prompt_tokens_details")))
			{
				TSharedPtr<FJsonObject> PromptDetails = UsageObj->GetObjectField(TEXT("prompt_tokens_details"));
				if (PromptDetails.IsValid())
				{
					int32 CachedTokens = 0;
					PromptDetails->TryGetNumberField(TEXT("cached_tokens"), CachedTokens);
					TurnUsage.CachedTokens = CachedTokens;
				}
			}

			// Parse reasoning tokens if available
			if (UsageObj->HasField(TEXT("completion_tokens_details")))
			{
				TSharedPtr<FJsonObject> CompletionDetails = UsageObj->GetObjectField(TEXT("completion_tokens_details"));
				if (CompletionDetails.IsValid())
				{
					int32 ReasoningTokens = 0;
					CompletionDetails->TryGetNumberField(TEXT("reasoning_tokens"), ReasoningTokens);
					TurnUsage.ReasoningTokens = ReasoningTokens;
				}
			}

			// Parse cost (OpenRouter specific)
			// Try regular cost first, fall back to upstream_inference_cost for BYOK users
			double Cost = 0.0;
			if (UsageObj->TryGetNumberField(TEXT("cost"), Cost) && Cost > 0.0)
			{
				TurnUsage.CostAmount = Cost;
				TurnUsage.CostCurrency = TEXT("USD");
			}
			else if (UsageObj->HasField(TEXT("cost_details")))
			{
				TSharedPtr<FJsonObject> CostDetails = UsageObj->GetObjectField(TEXT("cost_details"));
				if (CostDetails.IsValid())
				{
					double UpstreamCost = 0.0;
					if (CostDetails->TryGetNumberField(TEXT("upstream_inference_cost"), UpstreamCost) && UpstreamCost > 0.0)
					{
						TurnUsage.CostAmount = UpstreamCost;
						TurnUsage.CostCurrency = TEXT("USD");
					}
				}
			}

			// Accumulate to session totals
			SessionUsage.InputTokens += TurnUsage.InputTokens;
			SessionUsage.OutputTokens += TurnUsage.OutputTokens;
			SessionUsage.TotalTokens += TurnUsage.TotalTokens;
			SessionUsage.CachedTokens += TurnUsage.CachedTokens;
			SessionUsage.ReasoningTokens += TurnUsage.ReasoningTokens;
			SessionUsage.CostAmount += TurnUsage.CostAmount;
			SessionUsage.CostCurrency = TurnUsage.CostCurrency;

			UE_LOG(LogAgentIntegrationKit, Log, TEXT("ChatGateway [%s]: Usage - Turn: %d tokens, $%.6f | Session: %d tokens, $%.6f"),
				*Provider->GetDisplayName(), TurnUsage.TotalTokens, TurnUsage.CostAmount, SessionUsage.TotalTokens, SessionUsage.CostAmount);

			// Broadcast usage update
			FACPSessionUpdate UsageUpdate;
			UsageUpdate.UpdateType = EACPUpdateType::UsageUpdate;
			UsageUpdate.Usage = SessionUsage;

			{
				TSharedRef<TAtomic<bool>> AliveFlag = bAlive;
				AsyncTask(ENamedThreads::GameThread, [this, AliveFlag, UsageUpdate]()
				{
					if (!AliveFlag->Load()) return;
					OnSessionUpdate.Broadcast(UsageUpdate);
				});
			}
		}
	}

	// Extract content from choices[0].delta
	const TArray<TSharedPtr<FJsonValue>>* ChoicesArray;
	if (!JsonObject->TryGetArrayField(TEXT("choices"), ChoicesArray) || ChoicesArray->Num() == 0)
	{
		return;
	}

	TSharedPtr<FJsonObject> ChoiceObj = (*ChoicesArray)[0]->AsObject();
	if (!ChoiceObj.IsValid())
	{
		return;
	}

	TSharedPtr<FJsonObject> DeltaObj = ChoiceObj->GetObjectField(TEXT("delta"));
	if (!DeltaObj.IsValid())
	{
		return;
	}

	// Check for tool calls in delta
	const TArray<TSharedPtr<FJsonValue>>* ToolCallsArray;
	if (DeltaObj->TryGetArrayField(TEXT("tool_calls"), ToolCallsArray))
	{
		for (const TSharedPtr<FJsonValue>& TCValue : *ToolCallsArray)
		{
			TSharedPtr<FJsonObject> TCObj = TCValue->AsObject();
			if (!TCObj.IsValid())
			{
				continue;
			}

			int32 Index = 0;
			TCObj->TryGetNumberField(TEXT("index"), Index);

			// Ensure CurrentToolCalls has enough elements
			while (CurrentToolCalls.Num() <= Index)
			{
				CurrentToolCalls.Add(FChatGatewayToolCall());
			}

			// Get or create the tool call at this index
			FChatGatewayToolCall& ToolCall = CurrentToolCalls[Index];

			// Extract id if present
			FString Id;
			if (TCObj->TryGetStringField(TEXT("id"), Id))
			{
				ToolCall.Id = Id;
			}

			// Extract function info if present
			TSharedPtr<FJsonObject> FuncObj = TCObj->GetObjectField(TEXT("function"));
			if (FuncObj.IsValid())
			{
				FString Name;
				if (FuncObj->TryGetStringField(TEXT("name"), Name))
				{
					ToolCall.Name = Name;
				}

				FString Args;
				if (FuncObj->TryGetStringField(TEXT("arguments"), Args))
				{
					ToolCall.Arguments += Args; // Arguments stream in chunks
				}
			}
		}
	}

	// Check for reasoning_details (reasoning tokens from OpenRouter)
	const TArray<TSharedPtr<FJsonValue>>* ReasoningDetailsArray;
	if (DeltaObj->TryGetArrayField(TEXT("reasoning_details"), ReasoningDetailsArray))
	{
		for (const TSharedPtr<FJsonValue>& DetailValue : *ReasoningDetailsArray)
		{
			TSharedPtr<FJsonObject> DetailObj = DetailValue->AsObject();
			if (!DetailObj.IsValid())
			{
				continue;
			}

			FString ReasoningType;
			DetailObj->TryGetStringField(TEXT("type"), ReasoningType);

			FString ReasoningText;
			if (ReasoningType == TEXT("reasoning.text"))
			{
				DetailObj->TryGetStringField(TEXT("text"), ReasoningText);
			}
			else if (ReasoningType == TEXT("reasoning.summary"))
			{
				DetailObj->TryGetStringField(TEXT("summary"), ReasoningText);
			}

			if (!ReasoningText.IsEmpty())
			{
				CurrentReasoningText += ReasoningText;

				// Send reasoning as thought chunk
				FACPSessionUpdate ReasoningUpdate;
				ReasoningUpdate.UpdateType = EACPUpdateType::AgentThoughtChunk;
				ReasoningUpdate.TextChunk = ReasoningText;

				{
					TSharedRef<TAtomic<bool>> AliveFlag = bAlive;
					AsyncTask(ENamedThreads::GameThread, [this, AliveFlag, ReasoningUpdate]()
					{
						if (!AliveFlag->Load()) return;
						OnSessionUpdate.Broadcast(ReasoningUpdate);
					});
				}
			}
		}
	}

	// Check for regular content
	FString Content;
	if (DeltaObj->TryGetStringField(TEXT("content"), Content) && !Content.IsEmpty())
	{
		// Accumulate response
		CurrentResponseText += Content;

		// Send streaming update
		FACPSessionUpdate Update;
		Update.UpdateType = EACPUpdateType::AgentMessageChunk;
		Update.TextChunk = Content;

		{
			TSharedRef<TAtomic<bool>> AliveFlag = bAlive;
			AsyncTask(ENamedThreads::GameThread, [this, AliveFlag, Update]()
			{
				if (!AliveFlag->Load()) return;
				OnSessionUpdate.Broadcast(Update);
			});
		}
	}
}

// ============================================================================
// Tool Calling Implementation
// ============================================================================

void FChatGatewayClient::ProcessToolCalls(const TArray<FChatGatewayToolCall>& ToolCalls)
{
	if (bIsCancelled)
	{
		SetState(EACPClientState::InSession, TEXT("Cancelled"));
		return;
	}

	if (ToolCalls.Num() == 0)
	{
		// No more tool calls - continue conversation
		ContinueAfterToolExecution();
		return;
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ChatGateway [%s]: Processing %d tool calls"), *Provider->GetDisplayName(), ToolCalls.Num());

	// Execute each tool call sequentially
	for (const FChatGatewayToolCall& ToolCall : ToolCalls)
	{
		if (bIsCancelled)
		{
			break;
		}

		ExecuteToolCall(ToolCall);
	}

	// After all tools are executed, continue the conversation
	if (!bIsCancelled)
	{
		ContinueAfterToolExecution();
	}
}

void FChatGatewayClient::ExecuteToolCall(const FChatGatewayToolCall& ToolCall)
{
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ChatGateway [%s]: Executing tool '%s' (id: %s)"), *Provider->GetDisplayName(), *ToolCall.Name, *ToolCall.Id);

	// Broadcast tool call start to UI
	BroadcastToolCallStart(ToolCall);

	FString ResultContent;
	bool bSuccess = false;
	TArray<FACPToolResultImage> ResultImages;

	// Try to execute via NeoStack registry first
	FNeoStackToolRegistry& Registry = FNeoStackToolRegistry::Get();
	if (Registry.HasTool(ToolCall.Name))
	{
		FToolResult Result = Registry.Execute(ToolCall.Name, ToolCall.Arguments);
		bSuccess = Result.bSuccess;
		ResultContent = Result.Output;

		// Copy images from tool result
		for (const FToolResultImage& Img : Result.Images)
		{
			FACPToolResultImage ACPImage;
			ACPImage.Base64Data = Img.Base64Data;
			ACPImage.MimeType = Img.MimeType;
			ACPImage.Width = Img.Width;
			ACPImage.Height = Img.Height;
			ResultImages.Add(ACPImage);
		}
	}
	// Try MCP server registered tools
	else
	{
		const TMap<FString, FMCPToolDefinition>& MCPTools = FMCPServer::Get().GetRegisteredTools();
		if (const FMCPToolDefinition* Tool = MCPTools.Find(ToolCall.Name))
		{
			// Parse arguments JSON
			TSharedPtr<FJsonObject> ArgsObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ToolCall.Arguments);
			if (FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid())
			{
				FMCPToolResult Result = Tool->Handler(ArgsObj);
				bSuccess = Result.bSuccess;
				ResultContent = bSuccess ? Result.Content : Result.ErrorMessage;

				// Copy images from MCP tool result
				for (const FMCPToolResultImage& Img : Result.Images)
				{
					FACPToolResultImage ACPImage;
					ACPImage.Base64Data = Img.Base64Data;
					ACPImage.MimeType = Img.MimeType;
					ACPImage.Width = Img.Width;
					ACPImage.Height = Img.Height;
					ResultImages.Add(ACPImage);
				}
			}
			else
			{
				bSuccess = false;
				ResultContent = TEXT("Failed to parse tool arguments as JSON");
			}
		}
		else
		{
			bSuccess = false;
			ResultContent = FString::Printf(TEXT("Tool '%s' not found"), *ToolCall.Name);
		}
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ChatGateway [%s]: Tool '%s' result: %s (success: %d, images: %d)"),
		*Provider->GetDisplayName(), *ToolCall.Name, *ResultContent.Left(200), bSuccess, ResultImages.Num());

	// Broadcast tool result to UI
	BroadcastToolCallResult(ToolCall.Id, bSuccess, ResultContent, ResultImages);

	// Add tool result message to conversation history
	FChatGatewayMessage ToolResultMessage;
	ToolResultMessage.Role = TEXT("tool");
	ToolResultMessage.ToolCallId = ToolCall.Id;
	ToolResultMessage.Content = ResultContent;
	ConversationHistory.Add(ToolResultMessage);
}

void FChatGatewayClient::ContinueAfterToolExecution()
{
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ChatGateway [%s]: Continuing conversation after tool execution"), *Provider->GetDisplayName());

	// Reset streaming state for next request
	{
		FScopeLock Lock(&StateLock);
		StreamBuffer.Empty();
		CurrentResponseText.Empty();
		CurrentReasoningText.Empty();
		LastProcessedLength = 0;
		CurrentToolCalls.Empty();
		bIsProcessingTools = false;
	}

	// Build new request body (will include tool results in conversation)
	FString RequestBody = BuildRequestBody();

	// Create HTTP request
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	CurrentRequest = Request;

	ConfigureRequest(Request);
	Request->SetContentAsString(RequestBody);

	Request->OnProcessRequestComplete().BindRaw(this, &FChatGatewayClient::OnRequestComplete);
	Request->OnRequestProgress64().BindRaw(this, &FChatGatewayClient::OnRequestProgress64);

	// Send request
	if (!Request->ProcessRequest())
	{
		SetState(EACPClientState::InSession, TEXT("Ready"));
		OnError.Broadcast(-1, TEXT("Failed to continue conversation after tool execution"));
	}
}

void FChatGatewayClient::BroadcastToolCallStart(const FChatGatewayToolCall& ToolCall)
{
	FACPSessionUpdate Update;
	Update.UpdateType = EACPUpdateType::ToolCall;
	Update.ToolCallId = ToolCall.Id;
	Update.ToolName = ToolCall.Name;
	Update.ToolArguments = ToolCall.Arguments;

	OnSessionUpdate.Broadcast(Update);
}

void FChatGatewayClient::BroadcastToolCallResult(const FString& ToolCallId, bool bSuccess, const FString& Result, const TArray<FACPToolResultImage>& Images)
{
	FACPSessionUpdate Update;
	Update.UpdateType = EACPUpdateType::ToolCallUpdate;
	Update.ToolCallId = ToolCallId;
	Update.ToolResult = Result;
	Update.bToolSuccess = bSuccess;
	Update.ToolResultImages = Images;
	OnSessionUpdate.Broadcast(Update);
}
