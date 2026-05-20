// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ACPTypes.h"
#include "Containers/Ticker.h"

/**
 * FAgentService — Headless service layer for AI agent orchestration.
 *
 * This singleton owns the critical behaviors that were previously coupled to
 * UWebUIBridge and the embedded WebUI tab:
 *
 *   1. Message persistence — subscribes to FACPAgentManager delegates and
 *      persists streaming chunks to FACPSessionManager in real-time.
 *   2. Agent lifecycle — proactively connects available agents and fetches
 *      session lists at startup, not on WebUI tab open.
 *   3. Session discovery — merges active sessions, ACP session lists, and
 *      local history (Claude Code JSONL, Gemini, Copilot) into a unified list.
 *   4. Direct history reading — reads session messages from disk without
 *      requiring ACP session/load replay.
 *
 * Any frontend (WebUI, relay, future IDE, code review tool) subscribes to
 * this service's delegates and calls its API. The service is initialized at
 * module startup and runs for the lifetime of the editor process.
 *
 * FAgentService does NOT depend on UWebUIBridge, SWebBrowser, or any UI.
 */

DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnServiceMessage, const FString& /*SessionId*/, const FString& /*AgentName*/, const FString& /*UpdateJson*/);
DECLARE_MULTICAST_DELEGATE_FourParams(FOnServiceStateChanged, const FString& /*SessionId*/, const FString& /*AgentName*/, const FString& /*StateStr*/, const FString& /*Message*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnServicePermissionRequest, const FString& /*SessionId*/, const FString& /*RequestJson*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnServiceModelsAvailable, const FString& /*AgentName*/, const FString& /*ModelsJson*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnServiceModesAvailable, const FString& /*AgentName*/, const FString& /*ModesJson*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnServiceModeChanged, const FString& /*AgentName*/, const FString& /*ModeId*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnServicePlanUpdate, const FString& /*SessionId*/, const FString& /*PlanJson*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnServiceCommandsAvailable, const FString& /*SessionId*/, const FString& /*CommandsJson*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnServiceSessionListUpdated, const FString& /*AgentName*/, const FString& /*SessionsJson*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnServiceUsageUpdated, const FString& /*UsageJson*/);

class AGENTINTEGRATIONKIT_API FAgentService
{
public:
	static FAgentService& Get();

	/** Initialize the service — binds delegates, connects agents, fetches session lists.
	 *  Called once at module startup. Safe to call multiple times (no-op after first). */
	void Initialize();

	/** Shutdown — unbind delegates, clean up. */
	void Shutdown();

	/** Whether Initialize() has been called. */
	bool IsInitialized() const { return bInitialized; }

	// ── Session Discovery ───────────────────────────────────────────

	/** Get all sessions — merges active, ACP cached, and local history.
	 *  Returns JSON array string. */
	FString GetAllSessions() const;

	/** Get messages for a session — tries active memory first, then disk history.
	 *  Returns JSON array string. */
	FString GetSessionMessages(const FString& SessionId) const;

	/** Resume a session for chatting — loads from disk, connects agent, starts ACP load.
	 *  Pre-populates messages from disk so they're immediately available via GetSessionMessages().
	 *  Returns JSON object string with { success, agentName, loading }. */
	FString ResumeSession(const FString& SessionId);

	// ── Agent Discovery ─────────────────────────────────────────────

	/** Get all available agents. Returns JSON array string. */
	FString GetAgents() const;

	/** Get last used agent name. */
	FString GetLastUsedAgent() const;

	// ── Session Lifecycle ───────────────────────────────────────────

	/** Create a new session. Returns JSON { sessionId, agentName }. */
	FString CreateSession(const FString& AgentName);

	/** Send a prompt. Returns "ok" or error. */
	FString SendPrompt(const FString& SessionId, const FString& Text);

	/** Cancel current prompt. */
	void CancelPrompt(const FString& SessionId);

	/** Delete a session. */
	FString DeleteSession(const FString& SessionId);

	/** Rename a session. */
	FString RenameSession(const FString& SessionId, const FString& NewTitle);

	// ── Models & Modes ──────────────────────────────────────────────

	/** Get models for an agent. Returns JSON { models, currentModelId }. */
	FString GetModels(const FString& AgentName) const;

	/** Set model for an agent. */
	void SetModel(const FString& AgentName, const FString& ModelId);

	/** Get modes for an agent. Returns JSON { modes, currentModeId }. */
	FString GetModes(const FString& AgentName) const;

	/** Set mode for an agent. */
	void SetMode(const FString& AgentName, const FString& ModeId);

	// ── Permissions ─────────────────────────────────────────────────

	/** Respond to a permission request. */
	void RespondToPermission(const FString& AgentName, int32 RequestId, const FString& OptionId, TSharedPtr<FJsonObject> OutcomeMeta = nullptr);

	// ── Delegates (for any frontend to subscribe) ───────────────────

	/** Fired when a streaming update arrives (text chunk, tool call, error, usage, etc.)
	 *  UpdateJson is pre-serialized for efficient forwarding. */
	FOnServiceMessage OnMessage;

	/** Fired when agent state changes (connecting, ready, prompting, error, etc.) */
	FOnServiceStateChanged OnStateChanged;

	/** Fired when agent requests permission (tool approval, AskUserQuestion) */
	FOnServicePermissionRequest OnPermissionRequest;

	/** Fired when agent's model list becomes available */
	FOnServiceModelsAvailable OnModelsAvailable;

	/** Fired when agent's mode list becomes available */
	FOnServiceModesAvailable OnModesAvailable;

	/** Fired when agent's mode changes */
	FOnServiceModeChanged OnModeChanged;

	/** Fired when plan/todo updates arrive */
	FOnServicePlanUpdate OnPlanUpdate;

	/** Fired when slash commands become available */
	FOnServiceCommandsAvailable OnCommandsAvailable;

	/** Fired when an agent's session list is received (from ACP or local history) */
	FOnServiceSessionListUpdated OnSessionListUpdated;

	/** Fired when usage/rate limit data updates */
	FOnServiceUsageUpdated OnUsageUpdated;

private:
	FAgentService() = default;
	~FAgentService();

	FAgentService(const FAgentService&) = delete;
	FAgentService& operator=(const FAgentService&) = delete;

	/** Subscribe to all FACPAgentManager delegates. */
	void BindDelegates();
	void UnbindDelegates();

	/** Connect available agents and fetch session lists. */
	void DiscoverAgents();

	/** Serialize a message array to JSON. */
	static FString SerializeMessages(const TArray<FACPChatMessage>& Messages);

	/** Serialize a single update to JSON string (for OnMessage delegate). */
	static FString SerializeUpdate(const FString& AgentName, const FACPSessionUpdate& Update);

	/** Try to read messages for a session from disk (Claude Code, Gemini, Copilot).
	 *  Returns true if found and parsed. */
	bool ReadMessagesFromDisk(const FString& SessionId, TArray<FACPChatMessage>& OutMessages) const;

	/** Find which agent owns a session by checking cached lists and local history. */
	FString FindAgentForSession(const FString& SessionId) const;

	/** Get local history sessions for an agent (same logic as WebUIBridge). */
	static TArray<FACPRemoteSessionEntry> GetLocalHistorySessions(const FString& AgentName, const FString& WorkDir);

	/** Check if an agent uses launch-time resume (Gemini/Copilot). */
	static bool IsLaunchResumeAgent(const FString& AgentName);

	/** Finalize any in-progress streaming message for a session. */
	void FinalizeStreamingMessage(const FString& SessionId);

	// ── JSON Helpers ────────────────────────────────────────────────

	static FString JsonToCompactString(const TSharedRef<FJsonObject>& Obj);
	static FString JsonArrayToCompactString(const TArray<TSharedPtr<FJsonValue>>& Arr);

private:
	bool bInitialized = false;

	/** Per-session streaming message index — tracks which assistant message is being streamed. */
	TMap<FString, int32> StreamingMessageIndices;

	/** Per-session previous state — used to detect transitions. */
	TMap<FString, EACPClientState> PreviousSessionStates;

	/** Delegate handles for cleanup. */
	FDelegateHandle AgentMessageHandle;
	FDelegateHandle AgentStateHandle;
	FDelegateHandle AgentErrorHandle;
	FDelegateHandle PermissionRequestHandle;
	FDelegateHandle ModesAvailableHandle;
	FDelegateHandle ModeChangedHandle;
	FDelegateHandle ModelsAvailableHandle;
	FDelegateHandle CommandsAvailableHandle;
	FDelegateHandle PlanUpdateHandle;
	FDelegateHandle AuthCompleteHandle;
	FDelegateHandle SessionListHandle;
};
