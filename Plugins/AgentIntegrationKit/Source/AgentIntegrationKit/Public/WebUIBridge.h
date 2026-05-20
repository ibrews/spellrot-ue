// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "WebJSFunction.h"
#include "ACPTypes.h"
#include "WebUIBridge.generated.h"

/**
 * Bridge object exposed to the WebUI via SWebBrowser::BindUObject.
 * JavaScript accesses this as window.ue.bridge.*
 * All UFUNCTION methods become callable from JS and return Promises.
 */
UCLASS()
class AGENTINTEGRATIONKIT_API UWebUIBridge : public UObject
{
	GENERATED_BODY()

public:

	// ── Agent Discovery ──────────────────────────────────────────────

	/** Returns JSON array of available agents with their config/status */
	UFUNCTION()
	FString GetAgents();

	/** Returns the last used agent name (persisted across sessions) */
	UFUNCTION()
	FString GetLastUsedAgent();

	// ── Onboarding ──────────────────────────────────────────────────

	/** Returns whether the onboarding wizard has been completed or skipped */
	UFUNCTION()
	bool GetOnboardingCompleted();

	/** Mark the onboarding wizard as completed (or skipped). Persists to config. */
	UFUNCTION()
	void SetOnboardingCompleted();

	// ── Session Lifecycle ────────────────────────────────────────────

	/** Create a new session. Returns JSON: {sessionId, agentName, title} */
	UFUNCTION()
	FString CreateSession(const FString& AgentName);

	/** Returns JSON array of all session metadata (saved + active) */
	UFUNCTION()
	FString GetSessions();

	/** Resume a saved session — loads from disk into memory. Returns JSON: {success, error?} */
	UFUNCTION()
	FString ResumeSession(const FString& SessionId);

	/** Returns JSON with full session data including messages */
	UFUNCTION()
	FString GetSessionMessages(const FString& SessionId);

	/** Shell line to resume this session in the embedded terminal (supported CLIs only).
	 *  Returns JSON: {supported, command?, agentName?, error?} */
	UFUNCTION()
	FString GetSessionTerminalResumeCommand(const FString& SessionId);

	/** Build a continuation draft from one session for a different target agent.
	 *  Returns JSON: {success, draftPrompt?, error?} */
	UFUNCTION()
	FString BuildContinuationDraft(const FString& SourceSessionId, const FString& TargetAgentName, const FString& SummaryMode);

	/** Start async continuation draft generation. Returns JSON: {success, requestId?, pending, error?} */
	UFUNCTION()
	FString RequestContinuationDraft(const FString& SourceSessionId, const FString& TargetAgentName, const FString& SummaryMode);

	/** Returns JSON: {provider, modelId, defaultDetail, hasOpenRouterKey} */
	UFUNCTION()
	FString GetContinuationSummarySettings();

	/** Set continuation summary provider ("openrouter" or "local") */
	UFUNCTION()
	void SetContinuationSummaryProvider(const FString& Provider);

	/** Set continuation summary model ID (OpenRouter model id) */
	UFUNCTION()
	void SetContinuationSummaryModel(const FString& ModelId);

	/** Set default continuation summary detail ("compact" or "detailed") */
	UFUNCTION()
	void SetContinuationSummaryDefaultDetail(const FString& Detail);

	// ── Messaging ────────────────────────────────────────────────────

	/** Send a user prompt to a session. Streaming updates arrive via the OnMessage callback. */
	UFUNCTION()
	void SendPrompt(const FString& SessionId, const FString& Text);

	/** Cancel the current streaming prompt in a session */
	UFUNCTION()
	void CancelPrompt(const FString& SessionId);

	// ── Model & Reasoning ───────────────────────────────────────────

	/** Returns JSON: {models: [{id, name, description, supportsReasoning}], currentModelId: "..."} */
	UFUNCTION()
	FString GetModels(const FString& AgentName);

	/** Returns JSON with full model list for agents that support it (OpenRouter). */
	UFUNCTION()
	FString GetAllModels(const FString& AgentName);

	/** Set the active model for an agent */
	UFUNCTION()
	void SetModel(const FString& AgentName, const FString& ModelId);

	/** Returns the current reasoning effort level for an agent: "none", "low", "medium", "high", "max" */
	UFUNCTION()
	FString GetReasoningLevel(const FString& AgentName);

	/** Set reasoning effort level for an agent: "none", "low", "medium", "high", "max" */
	UFUNCTION()
	void SetReasoningLevel(const FString& AgentName, const FString& Level);

	// ── Mode Selection ──────────────────────────────────────────────

	/** Returns JSON: {modes: [{id, name, description}], currentModeId: "..."} */
	UFUNCTION()
	FString GetModes(const FString& AgentName);

	/** Set the active mode for an agent session */
	UFUNCTION()
	void SetMode(const FString& AgentName, const FString& ModeId);

	// ── Agent Setup ─────────────────────────────────────────────────

	/** Returns JSON with install info: {agentName, baseExecutableName, installCommand, installUrl, requiresAdapter, requiresBaseCLI} */
	UFUNCTION()
	FString GetAgentInstallInfo(const FString& AgentName);

	/** Start async agent installation. Progress/completion arrive via OnInstallProgress/OnInstallComplete callbacks. */
	UFUNCTION()
	void InstallAgent(const FString& AgentName);

	/** Re-check and return current status for an agent (invalidates cache). Returns JSON: {status, statusMessage} */
	UFUNCTION()
	FString RefreshAgentStatus(const FString& AgentName);

	/** Returns JSON array of all ACP registry agents with install status, version, icon, distribution info */
	UFUNCTION()
	FString GetRegistryAgents();

	/** Force a refresh of the ACP agent registry from the CDN */
	UFUNCTION()
	void RefreshRegistry();

	/** Returns JSON array of agents with available updates [{agentId, agentName, installedVersion, latestVersion}] */
	UFUNCTION()
	FString GetAgentUpdates();

	/** Trigger an update for a binary agent (removes old version, re-downloads on next use) */
	UFUNCTION()
	void UpdateRegistryAgent(const FString& AgentId);

	/** Install a registry agent by ID. Method: "binary", "npx", "uvx", or "auto" */
	UFUNCTION()
	void InstallRegistryAgent(const FString& AgentId, const FString& Method);

	/** Uninstall a registry agent by ID (removes from settings) */
	UFUNCTION()
	void UninstallRegistryAgent(const FString& AgentId);

	/** Returns JSON with prerequisite status: node, npm, git, uv, bun — each with version + path */
	UFUNCTION()
	FString GetPrerequisiteStatus();

	/** Copy text to the system clipboard */
	UFUNCTION()
	void CopyToClipboard(const FString& Text);

	/** Read text from the system clipboard */
	UFUNCTION()
	FString GetClipboardText();

	/** Open a URL in the system default browser */
	UFUNCTION()
	void OpenUrl(const FString& Url);

	/** Open an asset or source file. Handles /Game/ paths, filesystem paths, and file:line format. */
	UFUNCTION()
	void OpenPath(const FString& Path, int32 Line);

	// ── Notification Settings ───────────────────────────────────────

	/** Returns JSON with all notification settings */
	UFUNCTION()
	FString GetNotificationSettings();

	/** Set a notification setting by key. Value is a string representation (bools as "true"/"false", floats as "0.5"). */
	UFUNCTION()
	void SetNotificationSetting(const FString& Key, const FString& Value);

	/** Open the Agent Integration Kit settings in the UE Project Settings panel */
	UFUNCTION()
	void OpenPluginSettings();

	/** Restart the Unreal Editor (prompts to save if needed) */
	UFUNCTION()
	void RestartEditor();

	/** Trigger the plugin's async update check flow */
	UFUNCTION()
	void CheckForPluginUpdate();

	// ── Agent Authentication ────────────────────────────────────────

	/** Get available auth methods for an agent. Returns JSON array: [{id, name, description, isTerminalAuth}] */
	UFUNCTION()
	FString GetAuthMethods(const FString& AgentName);

	/** Start agent login with a specific auth method. Completion arrives via OnLoginComplete callback. */
	UFUNCTION()
	void StartAgentLogin(const FString& AgentName, const FString& MethodId);

	/** Register a JS callback for login completion: callback(agentName: string, success: bool, errorMessage: string) */
	UFUNCTION()
	void BindOnLoginComplete(FWebJSFunction Callback);

	// ── Agent Usage / Rate Limits ───────────────────────────────────

	/** Returns JSON with rate limit data for an agent. Triggers a fetch if no cached data. */
	UFUNCTION()
	FString GetAgentUsage(const FString& AgentName);

	/** Force-refresh usage data for an agent. Result arrives via OnUsageUpdated callback. */
	UFUNCTION()
	void RefreshAgentUsage(const FString& AgentName);

	// ── Streaming Callbacks (JS → C++ → JS) ─────────────────────────

	/** Register a JS callback for streaming message updates: callback(sessionId, updateJson) */
	UFUNCTION()
	void BindOnMessage(FWebJSFunction Callback);

	/** Register a JS callback for agent state changes: callback(sessionId, agentName, state, message) */
	UFUNCTION()
	void BindOnStateChanged(FWebJSFunction Callback);

	/** Register a JS callback for permission requests: callback(sessionId, requestJson) */
	UFUNCTION()
	void BindOnPermissionRequest(FWebJSFunction Callback);

	/** Register a JS callback for mode availability: callback(agentName, modesJson) */
	UFUNCTION()
	void BindOnModesAvailable(FWebJSFunction Callback);

	/** Register a JS callback for mode changes: callback(agentName, modeId) */
	UFUNCTION()
	void BindOnModeChanged(FWebJSFunction Callback);

	/** Register a JS callback for install progress: callback(agentName, message) */
	UFUNCTION()
	void BindOnInstallProgress(FWebJSFunction Callback);

	/** Register a JS callback for install completion: callback(agentName, success, errorMessage) */
	UFUNCTION()
	void BindOnInstallComplete(FWebJSFunction Callback);

	/** Register a JS callback for model availability: callback(agentName, modelsJson) */
	UFUNCTION()
	void BindOnModelsAvailable(FWebJSFunction Callback);

	/** Register a JS callback for slash commands availability: callback(sessionId, commandsJson) */
	UFUNCTION()
	void BindOnCommandsAvailable(FWebJSFunction Callback);

	/** Register a JS callback for plan/todo updates: callback(sessionId, planJson) */
	UFUNCTION()
	void BindOnPlanUpdate(FWebJSFunction Callback);

	/** Register a JS callback for agent usage/rate-limit updates: callback(agentName, usageJson) */
	UFUNCTION()
	void BindOnUsageUpdated(FWebJSFunction Callback);

	/** Register a JS callback for MCP tool readiness: callback(sessionId, status) where status is "waiting"|"ready"|"timeout" */
	UFUNCTION()
	void BindOnMcpStatus(FWebJSFunction Callback);

	/** Register a JS callback for continuation draft completion: callback(requestId, resultJson) */
	UFUNCTION()
	void BindOnContinuationDraftReady(FWebJSFunction Callback);

	/** Respond to a permission request. OutcomeMetaJson is optional JSON for AskUserQuestion answers. */
	UFUNCTION()
	void RespondToPermission(const FString& AgentName, int32 RequestId, const FString& OptionId, const FString& OutcomeMetaJson);

	// ── Attachments ─────────────────────────────────────────────────

	/** Paste image from system clipboard into attachments. Returns JSON: {success, error?} */
	UFUNCTION()
	FString PasteClipboardImage();

	/** Open native file picker for attachments (images + common docs). Returns JSON: {success, count} */
	UFUNCTION()
	FString OpenImagePicker();

	/** Add an image from base64 data (JS drag-drop). Returns JSON: {success, attachmentId?} */
	UFUNCTION()
	FString AddImageFromBase64(const FString& Base64Data, const FString& MimeType, int32 Width, int32 Height, const FString& DisplayName);

	/** Add a generic file from base64 data (JS drag-drop). Returns JSON: {success, attachmentId?} */
	UFUNCTION()
	FString AddFileFromBase64(const FString& Base64Data, const FString& MimeType, const FString& DisplayName);

	/** Remove an attachment by its GUID string */
	UFUNCTION()
	void RemoveAttachment(const FString& AttachmentId);

	/** Get current attachments as JSON array (metadata only, no base64) */
	UFUNCTION()
	FString GetAttachments();

	/** Register a JS callback for attachment changes: callback(attachmentsJson) */
	UFUNCTION()
	void BindOnAttachmentsChanged(FWebJSFunction Callback);

	// ── Tool Profiles & Settings ────────────────────────────────────

	/** Returns JSON array of all tools with enabled state and any description overrides for the given profile */
	UFUNCTION()
	FString GetTools(const FString& ProfileId);

	/** Returns JSON: {profiles: [...], activeProfileId: "..."} */
	UFUNCTION()
	FString GetProfiles();

	/** Returns JSON with full profile detail including customInstructions and toolDescriptionOverrides */
	UFUNCTION()
	FString GetProfileDetail(const FString& ProfileId);

	/** Set the active profile. Empty string = no profile (all tools enabled). */
	UFUNCTION()
	void SetActiveProfile(const FString& ProfileId);

	/** Toggle a tool's global enabled state (DisabledTools set) */
	UFUNCTION()
	void SetToolEnabled(const FString& ToolName, bool bEnabled);

	/** Toggle a tool within a specific profile's EnabledTools whitelist */
	UFUNCTION()
	void SetProfileToolEnabled(const FString& ProfileId, const FString& ToolName, bool bEnabled);

	/** Create a custom profile. Returns JSON: {profileId: "..."} */
	UFUNCTION()
	FString CreateProfile(const FString& DisplayName, const FString& Description);

	/** Delete a custom profile. Returns JSON: {success: bool} */
	UFUNCTION()
	FString DeleteProfile(const FString& ProfileId);

	/** Update profile metadata (name, description, customInstructions). Returns JSON: {success: bool} */
	UFUNCTION()
	FString UpdateProfile(const FString& ProfileId, const FString& DisplayName, const FString& Description, const FString& CustomInstructions);

	/** Set or clear a tool description override for a profile. Empty override = clear. */
	UFUNCTION()
	void SetToolDescriptionOverride(const FString& ProfileId, const FString& ToolName, const FString& DescriptionOverride);

	// ── Provider Settings ───────────────────────────────────────────

	/** Returns JSON with all provider configs: {priority: [...], providers: [...]} */
	UFUNCTION()
	FString GetProviderSettings();

	/** Set the full provider priority order. Accepts a JSON array of provider ID strings. */
	UFUNCTION()
	void SetProviderPriority(const FString& PriorityJson);

	/** Add a provider to the end of the priority list */
	UFUNCTION()
	void AddProvider(const FString& ProviderId);

	/** Remove a provider from the priority list */
	UFUNCTION()
	void RemoveProvider(const FString& ProviderId);

	/** Set API key for a specific provider */
	UFUNCTION()
	void SetProviderApiKey(const FString& ProviderId, const FString& ApiKey);

	/** Set base URL override for a specific provider */
	UFUNCTION()
	void SetProviderBaseUrl(const FString& ProviderId, const FString& BaseUrl);

	/** Refresh the model list for the built-in agent (re-fetches from all providers in priority) */
	UFUNCTION()
	void RefreshProviderModels();

	/** Returns JSON: {enabledModels: string[], hasCustomSelection: bool} */
	UFUNCTION()
	FString GetEnabledModels();

	/** Toggle a model's enabled/disabled state for the dropdown */
	UFUNCTION()
	void SetModelEnabled(const FString& ModelId, bool bEnabled);

	/** Bulk-set enabled models from a JSON array of model IDs. Empty array = show all (clear custom selection). */
	UFUNCTION()
	void SetEnabledModels(const FString& ModelIdsJson);

	// ── Custom Providers ────────────────────────────────────────────

	/** Create a new custom provider. Returns JSON: {providerId: "userprovider_..."} */
	UFUNCTION()
	FString CreateCustomProvider(const FString& DisplayName, const FString& BaseUrl);

	/** Delete a custom provider and clean up its keys/priority */
	UFUNCTION()
	void DeleteCustomProvider(const FString& ProviderId);

	/** Update a custom provider's display name and/or base URL */
	UFUNCTION()
	void UpdateCustomProvider(const FString& ProviderId, const FString& DisplayName, const FString& BaseUrl);

	/** Add a model to a custom provider. Returns JSON: {success: bool} */
	UFUNCTION()
	FString AddCustomProviderModel(const FString& ProviderId, const FString& ModelId, const FString& DisplayName, const FString& Description);

	/** Remove a model from a custom provider */
	UFUNCTION()
	void RemoveCustomProviderModel(const FString& ProviderId, const FString& ModelId);

	/** Import models from JSON into a custom provider. Accepts [{id, name?, description?}] or {data:[...]}. Returns JSON: {imported: number, errors: []} */
	UFUNCTION()
	FString ImportCustomProviderModels(const FString& ProviderId, const FString& ModelsJson);

	/** Toggle model discovery for a custom provider */
	UFUNCTION()
	void SetCustomProviderModelDiscovery(const FString& ProviderId, bool bEnabled);

	// ── Context Mentions ────────────────────────────────────────────

	/** Search for assets/files to attach via @ mention. Returns JSON array of {name, path, category, type} */
	UFUNCTION()
	FString SearchContextItems(const FString& Query);

	// ── Session Management ──────────────────────────────────────────

	/** Delete a session (closes active + removes agent's native file). Returns JSON: {success: bool} */
	UFUNCTION()
	FString DeleteSession(const FString& SessionId);

	/** Rename a session (sets custom title that survives remote sync). Returns JSON: {success: bool} */
	UFUNCTION()
	FString RenameSession(const FString& SessionId, const FString& NewTitle);

	/** Export a loaded session to a Markdown file. Returns JSON: {success, canceled?, savedPath?, error?} */
	UFUNCTION()
	FString ExportSessionToMarkdown(const FString& SessionId);

	/** Register a JS callback for session list updates: callback(agentName, sessionsJson) */
	UFUNCTION()
	void BindOnSessionListUpdated(FWebJSFunction Callback);

	/** Manually refresh session lists from all connected (or connectable) agents.
	 *  Returns JSON: {connectingCount: number} — how many agents are being connected for listing. */
	UFUNCTION()
	FString RefreshSessionList();

	// ── Project Indexing ────────────────────────────────────────────

	/** Returns JSON with indexing settings (provider, model, dimensions, scope, etc.) */
	UFUNCTION()
	FString GetIndexingSettings();

	/** Returns JSON with current indexing status (state, chunks, breakdown, etc.) */
	UFUNCTION()
	FString GetIndexingStatus();

	/** Set the embedding provider ("openrouter" or "custom") */
	UFUNCTION()
	void SetIndexingProvider(const FString& Provider);

	/** Set the custom embedding endpoint URL */
	UFUNCTION()
	void SetIndexingEndpointUrl(const FString& Url);

	/** Set the custom embedding API key */
	UFUNCTION()
	void SetIndexingApiKey(const FString& Key);

	/** Set the embedding model name */
	UFUNCTION()
	void SetIndexingModel(const FString& Model);

	/** Set the embedding dimensions */
	UFUNCTION()
	void SetIndexingDimensions(int32 Dims);

	/** Toggle auto-indexing on file changes */
	UFUNCTION()
	void SetAutoIndex(bool bEnabled);

	/** Enable/disable a specific indexing scope (blueprints, cppFiles, assets, levels, config) */
	UFUNCTION()
	void SetIndexingScopeEnabled(const FString& ScopeKey, bool bEnabled);

	/** Start the indexing pipeline (async — returns immediately, poll status for progress) */
	UFUNCTION()
	void StartIndexing();

	/** Clear all index data */
	UFUNCTION()
	void ClearIndex();

	// ── Studio / Generative Providers ──────────────────────────────

	/** Returns JSON array of all generative providers with their actions and schemas.
	 *  [{id, displayName, website, actions: [{actionId, description, inputHints, outputHints, creditCost, isSynchronous, paramsSchema}]}] */
	UFUNCTION()
	FString GetGenerativeProviders();

	/** Submit a job to a generative provider. Returns JSON: {success, job: {providerId, actionId, jobId, status, progress, error}} */
	UFUNCTION()
	FString SubmitGenerativeJob(const FString& ProviderId, const FString& ActionId, const FString& ParamsJson);

	/** Check status of a generative job. Returns JSON: {success, job: {jobId, status, progress, resultUrl, thumbnailUrl, extraUrls, imageUrls, error}} */
	UFUNCTION()
	FString CheckGenerativeJobStatus(const FString& ProviderId, const FString& JobId, const FString& ActionId);

	/** Get credit balance for a provider. Returns JSON: {success, balance: number} (-1 = not supported) */
	UFUNCTION()
	FString GetGenerativeBalance(const FString& ProviderId);

	// ── Terminal ────────────────────────────────────────────────────

	/** Start a new terminal session. Returns JSON: {terminalId} or {error} */
	UFUNCTION()
	FString StartTerminal(const FString& WorkingDir, const FString& Shell);

	/** Write input data to a terminal (raw string from xterm.js onData) */
	UFUNCTION()
	void WriteTerminal(const FString& TerminalId, const FString& Data);

	/** Resize terminal PTY to given columns and rows */
	UFUNCTION()
	void ResizeTerminal(const FString& TerminalId, int32 Cols, int32 Rows);

	/** Close a terminal session */
	UFUNCTION()
	void CloseTerminal(const FString& TerminalId);

	/** Register a JS callback for terminal output: callback(terminalId, base64Data) */
	UFUNCTION()
	void BindOnTerminalOutput(FWebJSFunction Callback);

	/** Register a JS callback for terminal exit: callback(terminalId, exitCode) */
	UFUNCTION()
	void BindOnTerminalExit(FWebJSFunction Callback);

	// ── Source Control ──────────────────────────────────────────────

	/** Returns JSON: {enabled, provider, branch, changesCount, connected} */
	UFUNCTION()
	FString GetSourceControlStatus();

	/** Open the UE source control changelists tab */
	UFUNCTION()
	void OpenSourceControlChangelist();

	/** Open the UE check-in/submit dialog */
	UFUNCTION()
	void OpenSourceControlSubmit();

private:
	/** Stored JS callbacks for streaming */
	FWebJSFunction OnMessageCallback;
	FWebJSFunction OnStateChangedCallback;
	FWebJSFunction OnPermissionRequestCallback;
	FWebJSFunction OnModesAvailableCallback;
	FWebJSFunction OnModeChangedCallback;
	FWebJSFunction OnInstallProgressCallback;
	FWebJSFunction OnInstallCompleteCallback;
	FWebJSFunction OnCommandsAvailableCallback;
	FWebJSFunction OnPlanUpdateCallback;
	FWebJSFunction OnModelsAvailableCallback;
	FWebJSFunction OnUsageUpdatedCallback;
	FWebJSFunction OnAttachmentsChangedCallback;
	FWebJSFunction OnLoginCompleteCallback;
	FWebJSFunction OnMcpStatusCallback;
	FWebJSFunction OnSessionListUpdatedCallback;
	FWebJSFunction OnContinuationDraftReadyCallback;
	FWebJSFunction OnTerminalOutputCallback;
	FWebJSFunction OnTerminalExitCallback;

	/** Delegate handles for terminal events */
	FDelegateHandle TerminalOutputHandle;
	FDelegateHandle TerminalExitHandle;

	/** Monotonic request id for async continuation draft jobs */
	int32 NextContinuationDraftRequestId = 1;

	/** Delegate handles for cleanup */
	FDelegateHandle AgentMessageHandle;
	FDelegateHandle AgentStateHandle;
	FDelegateHandle AgentErrorHandle;
	FDelegateHandle AgentAuthCompleteHandle;
	FDelegateHandle PermissionRequestHandle;
	FDelegateHandle ModesAvailableHandle;
	FDelegateHandle ModeChangedHandle;
	FDelegateHandle CommandsAvailableHandle;
	FDelegateHandle PlanUpdateHandle;
	FDelegateHandle ModelsAvailableHandle;
	FDelegateHandle UsageUpdatedHandle;
	FDelegateHandle MeshyBalanceHandle;
	FDelegateHandle AttachmentsChangedHandle;
	FDelegateHandle McpToolsDiscoveredHandle;
	FDelegateHandle SessionListUpdatedHandle;

	/** MCP tools discovery timeout */
	FTSTicker::FDelegateHandle McpTimeoutTickerHandle;
	FString McpWaitingSessionId;  // Session ID waiting for MCP tools

	/** Fire MCP status callback and clean up listeners */
	void NotifyMcpStatus(const FString& SessionId, const FString& Status);

	/** Bind to agent manager delegates */
	void BindDelegates();
	void UnbindDelegates();

	/** Per-session previous state — used to detect prompting→ready transitions for notifications */
	TMap<FString, EACPClientState> PreviousSessionStates;

	/** Fire desktop notifications (toast, taskbar flash, sound) when agent finishes */
	void FireCompletionNotifications(bool bSuccess);

	/** Play optional editor preview sound when a tool permission / Ask User prompt is shown */
	void FirePermissionRequestNotification();

	/** Cooldown so parallel tool calls don't stack the same alert */
	double LastPermissionRequestSoundTime = 0.0;

	/** Per-session streaming message index (mirrors Slate UI's CurrentStreamingMessageIndex) */
	TMap<FString, int32> StreamingMessageIndices;

	/** Serialize a single message to JSON */
	static TSharedPtr<FJsonObject> MessageToJson(const struct FACPChatMessage& Message);
	static TSharedPtr<FJsonObject> ContentBlockToJson(const struct FACPContentBlock& Block);
};
