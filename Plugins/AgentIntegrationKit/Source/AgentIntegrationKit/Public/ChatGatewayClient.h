// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ACPTypes.h"
#include "ChatGatewayProvider.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

// Forward declarations
struct FACPAgentConfig;
struct FMCPToolDefinition;

// Delegate types for Chat Gateway client
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnChatGatewayStateChanged, EACPClientState, const FString&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnChatGatewaySessionUpdate, const FACPSessionUpdate&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnChatGatewayResponse, const TSharedPtr<FJsonObject>&);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnChatGatewayError, int32, const FString&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnChatGatewayModelsAvailable, const FACPSessionModelState&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnChatGatewayPermissionRequest, const FACPPermissionRequest&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnChatGatewayModesAvailable, const FACPSessionModeState&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnChatGatewayModeChanged, const FString&);

// Backward compat aliases (old code may reference FOnOpenRouter*)
using FOnOpenRouterStateChanged = FOnChatGatewayStateChanged;
using FOnOpenRouterSessionUpdate = FOnChatGatewaySessionUpdate;
using FOnOpenRouterResponse = FOnChatGatewayResponse;
using FOnOpenRouterError = FOnChatGatewayError;
using FOnOpenRouterModelsAvailable = FOnChatGatewayModelsAvailable;
using FOnOpenRouterPermissionRequest = FOnChatGatewayPermissionRequest;
using FOnOpenRouterModesAvailable = FOnChatGatewayModesAvailable;
using FOnOpenRouterModeChanged = FOnChatGatewayModeChanged;

/**
 * Tool call from an OpenAI-compatible API response
 */
struct FChatGatewayToolCall
{
	FString Id;
	FString Name;
	FString Arguments; // JSON string
};

/**
 * Message structure for conversation history (OpenAI chat format)
 */
struct FChatGatewayMessage
{
	FString Role;
	FString Content;

	// For assistant messages with tool calls
	TArray<FChatGatewayToolCall> ToolCalls;

	// For tool response messages
	FString ToolCallId;
};

// Backward compat aliases
using FOpenRouterToolCall = FChatGatewayToolCall;
using FOpenRouterMessage = FChatGatewayMessage;

/**
 * Native C++ client for OpenAI-compatible chat APIs.
 * Supports multiple providers (OpenRouter, OpenAI, DeepSeek, Groq, Ollama, etc.)
 * via the IChatGatewayProvider interface.
 *
 * Uses direct HTTP calls instead of spawning a subprocess, making it Fab-compliant.
 */
class AGENTINTEGRATIONKIT_API FChatGatewayClient
{
public:
	/** Construct with default provider (OpenRouter) */
	FChatGatewayClient();

	/** Construct with a specific provider */
	explicit FChatGatewayClient(TSharedRef<IChatGatewayProvider> InProvider);

	~FChatGatewayClient();

	// Connect (validates config, doesn't spawn process)
	bool Connect(const FACPAgentConfig& Config);

	// Disconnect
	void Disconnect();

	// Check connection status
	bool IsConnected() const { return State != EACPClientState::Disconnected && State != EACPClientState::Error; }
	EACPClientState GetState() const { return State; }

	// Session management (matching FACPClient interface)
	void NewSession(const FString& WorkingDirectory);
	void LoadSession(const FString& SessionId);
	void SendPrompt(const FString& PromptText);
	void CancelPrompt();
	void SetMode(const FString& ModeId);
	void SetModel(const FString& ModelId);

	// Permission response (no-op for most providers, but needed for interface compatibility)
	void RespondToPermissionRequest(int32 RequestId, const FString& OptionId);

	// Delegates
	FOnChatGatewayStateChanged OnStateChanged;
	FOnChatGatewaySessionUpdate OnSessionUpdate;
	FOnChatGatewayResponse OnResponse;
	FOnChatGatewayError OnError;
	FOnChatGatewayModelsAvailable OnModelsAvailable;
	FOnChatGatewayPermissionRequest OnPermissionRequest;
	FOnChatGatewayModesAvailable OnModesAvailable;
	FOnChatGatewayModeChanged OnModeChanged;

	// Get capabilities (matching FACPClient interface)
	const FACPAgentCapabilities& GetAgentCapabilities() const { return AgentCapabilities; }

	// Get available models for the current session
	const FACPSessionModelState& GetModelState() const { return SessionModelState; }

	// Get available modes for the current session
	const FACPSessionModeState& GetModeState() const { return SessionModeState; }

	// Get all cached models (for the picker UI)
	const TArray<FACPModelInfo>& GetAllCachedModels() const { return CachedModels; }

	// Get curated models + "More..." option (if provider supports browsing)
	TArray<FACPModelInfo> GetCuratedModels();

	// Add a model to recent list
	void AddRecentModel(const FACPModelInfo& RecentModel);

	// Get model info by ID (returns nullptr if not found)
	const FACPModelInfo* GetModelInfo(const FString& ModelId) const;

	// Check if current model supports reasoning
	bool CurrentModelSupportsReasoning() const;

	// Reasoning configuration
	void SetReasoningEnabled(bool bEnabled) { bReasoningEnabled = bEnabled; }
	bool IsReasoningEnabled() const { return bReasoningEnabled; }
	void SetReasoningEffort(const FString& Effort) { ReasoningEffort = Effort; }
	const FString& GetReasoningEffort() const { return ReasoningEffort; }

	// Session history management (for persistence/resume)
	TArray<FChatGatewayMessage> GetConversationHistory() const;
	void RestoreConversationHistory(const TArray<FChatGatewayMessage>& History);
	FString GetCurrentSessionId() const { return CurrentSessionId; }

	// Unreal session tracking (for multi-chat support)
	void SetUnrealSessionId(const FString& SessionId) { UnrealSessionId = SessionId; }
	FString GetUnrealSessionId() const { return UnrealSessionId; }

	// Usage tracking
	const FACPUsageData& GetSessionUsage() const { return SessionUsage; }
	void ResetSessionUsage() { SessionUsage = FACPUsageData(); }

	// Refresh model list from all enabled providers (invalidates cache)
	void RefreshModels();

	// Provider access
	TSharedRef<IChatGatewayProvider> GetProvider() const { return Provider; }
	void SetProvider(TSharedRef<IChatGatewayProvider> InProvider);

private:
	// Set state and broadcast
	void SetState(EACPClientState NewState, const FString& Message = TEXT(""));

	// Initialize default capabilities and models
	void InitializeCapabilities();

	// Fetch models from provider API (if supported)
	void FetchModels();

	// Callback for models fetch
	void OnModelsFetchComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess);

	// Build API request body (with tools)
	FString BuildRequestBody();

	// Build tools array from registered MCP tools
	TArray<TSharedPtr<FJsonValue>> BuildToolsArray();

	// Build single tool definition JSON
	TSharedPtr<FJsonObject> BuildToolDefinition(const FString& ToolName, const FString& Description, TSharedPtr<FJsonObject> InputSchema);

	// Configure HTTP request with provider headers
	void ConfigureRequest(TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request);

	// HTTP callbacks
	void OnRequestProgress64(FHttpRequestPtr Request, uint64 BytesSent, uint64 BytesReceived);
	void OnRequestComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess);

	// Process SSE stream data
	void ProcessStreamBuffer();
	void ProcessSSELine(const FString& Line);

	// Resolve the best provider for a model based on priority + configuration
	TSharedPtr<IChatGatewayProvider> ResolveProviderForModel(const FString& ModelId);

	// Tool calling support
	void ProcessToolCalls(const TArray<FChatGatewayToolCall>& ToolCalls);
	void ExecuteToolCall(const FChatGatewayToolCall& ToolCall);
	void ContinueAfterToolExecution();

	// Broadcast tool call UI updates
	void BroadcastToolCallStart(const FChatGatewayToolCall& ToolCall);
	void BroadcastToolCallResult(const FString& ToolCallId, bool bSuccess, const FString& Result, const TArray<FACPToolResultImage>& Images = TArray<FACPToolResultImage>());

private:
	// The active provider (determines endpoint, headers, models, etc.)
	TSharedRef<IChatGatewayProvider> Provider;

	// Configuration
	FString ApiKey;
	FString Model;
	FString BaseUrl; // Resolved from provider + settings overrides

	// State
	EACPClientState State = EACPClientState::Disconnected;
	FACPAgentConfig CurrentConfig;
	FACPAgentCapabilities AgentCapabilities;

	// Current session
	FString CurrentSessionId;

	// Unreal session ID this client is currently serving (for multi-chat support)
	FString UnrealSessionId;

	// Model/Mode state
	FACPSessionModelState SessionModelState;
	FACPSessionModeState SessionModeState;

	// Model cache
	TArray<FACPModelInfo> CachedModels;
	TArray<FACPModelInfo> RecentModels;
	TSet<FString> ProvidersWithDiscoveredModels; // Providers whose /models fetch succeeded
	FDateTime LastModelsFetch;
	static constexpr double ModelsCacheTTLHours = 23.47; // Slightly under 24h to avoid stale cache across daily sessions

	// Conversation history
	TArray<FChatGatewayMessage> ConversationHistory;

	// Current streaming state
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> CurrentRequest;
	FString StreamBuffer;
	FString CurrentResponseText;
	FString CurrentReasoningText; // Accumulated reasoning content
	int32 LastProcessedLength = 0;
	TAtomic<bool> bIsCancelled;

	// Tool calling state
	TArray<FChatGatewayToolCall> PendingToolCalls;
	TArray<FChatGatewayToolCall> CurrentToolCalls; // Tool calls being accumulated from stream
	int32 CurrentToolCallIndex = 0;
	bool bIsProcessingTools = false;

	// Reasoning configuration
	bool bReasoningEnabled = false;
	FString ReasoningEffort = TEXT("medium"); // none, low, medium, high

	// Usage tracking (cumulative for session)
	FACPUsageData SessionUsage;

	// Thread safety
	mutable FCriticalSection StateLock;

	// Guard against use-after-free in async lambdas that capture `this`
	TSharedRef<TAtomic<bool>> bAlive = MakeShared<TAtomic<bool>>(true);
};

// Backward compat alias — old code can still use FOpenRouterClient
using FOpenRouterClient = FChatGatewayClient;
