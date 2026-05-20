// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ACPSessionTypes.h"
#include "ACPTypes.h"

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnSessionCreated, const FString& /*SessionId*/, const FACPSessionMetadata& /*Metadata*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnSessionClosed, const FString& /*SessionId*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnSessionSwitched, const FString& /*OldSessionId*/, const FString& /*NewSessionId*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnSessionMessageAdded, const FString& /*SessionId*/, const FACPChatMessage& /*Message*/);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnSessionMessageUpdated, const FString& /*SessionId*/, int32 /*MessageIndex*/, const FACPChatMessage& /*Message*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnActiveSessionsChanged, const TArray<FString>& /*SessionIds*/);

/**
 * Manages active chat sessions (runtime only, no persistence)
 * Singleton accessible throughout the editor
 */
class AGENTINTEGRATIONKIT_API FACPSessionManager
{
public:
	static FACPSessionManager& Get();

	// Session lifecycle
	FString CreateSession(const FString& AgentName, const FString& ModelId = TEXT(""));
	bool CloseSession(const FString& SessionId);
	bool ResumeSession(const FString& SessionId);
	bool SwitchToSession(const FString& SessionId);

	// Active session access
	FACPActiveSession* GetActiveSession(const FString& SessionId);
	const FACPActiveSession* GetActiveSession(const FString& SessionId) const;
	FString GetCurrentSessionId() const { return CurrentSessionId; }
	FACPActiveSession* GetCurrentSession();
	TArray<FString> GetActiveSessionIds() const;
	int32 GetActiveSessionCount() const;

	// Message management
	void AddUserMessage(const FString& SessionId, const FString& Message, const TArray<FACPMessageContext>& Contexts = TArray<FACPMessageContext>());
	int32 StartAssistantMessage(const FString& SessionId);
	void AppendContentBlock(const FString& SessionId, int32 MessageIndex, const FACPContentBlock& Block);
	void AppendStreamingText(const FString& SessionId, int32 MessageIndex, EACPContentBlockType BlockType, const FString& TextChunk);
	void UpdateMessage(const FString& SessionId, int32 MessageIndex, const FACPChatMessage& Message);
	void FinishMessage(const FString& SessionId, int32 MessageIndex);
	void ClearSessionMessages(const FString& SessionId);

	// Session metadata
	void UpdateSessionTitle(const FString& SessionId, const FString& NewTitle);
	void SetCustomTitle(const FString& SessionId, const FString& NewTitle);

	/** Returns the persisted custom title for a session ID (Unreal GUID or agent ID), or nullptr if none. */
	const FString* GetPersistedCustomTitle(const FString& SessionId) const;

	// Agent coordination
	void SetSessionExternalId(const FString& SessionId, const FString& ExternalId);
	void SetSessionConnected(const FString& SessionId, bool bConnected);
	void SetSessionLoadingHistory(const FString& SessionId, bool bLoading);

	// Delegates
	FOnSessionCreated OnSessionCreated;
	FOnSessionClosed OnSessionClosed;
	FOnSessionSwitched OnSessionSwitched;
	FOnSessionMessageAdded OnSessionMessageAdded;
	FOnSessionMessageUpdated OnSessionMessageUpdated;
	FOnActiveSessionsChanged OnActiveSessionsChanged;

	// Limits
	static constexpr int32 MaxActiveSessions = 8;

private:
	FACPSessionManager();
	~FACPSessionManager();

	FACPSessionManager(const FACPSessionManager&) = delete;
	FACPSessionManager& operator=(const FACPSessionManager&) = delete;

	void BroadcastActiveSessionsChanged();
	void LoadCustomTitles();
	void SaveCustomTitles();
	FString GetCustomTitlesPath() const;

	TMap<FString, FACPActiveSession> ActiveSessions;
	FString CurrentSessionId;

	/** Persisted custom titles: sessionId (Unreal GUID or agent session ID) → title */
	TMap<FString, FString> PersistedCustomTitles;

	mutable FCriticalSection SessionLock;
};
