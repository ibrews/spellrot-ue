// Copyright 2026 Betide Studio. All Rights Reserved.

#include "WebUIBridge.h"
#include "TerminalManager.h"
#include "AgentIntegrationKitModule.h"
#include "ACPAgentManager.h"
#include "ACPClient.h"
#include "ACPSessionManager.h"
#include "ACPClaudeCodeHistoryReader.h"
#include "ACPCodexHistoryReader.h"
#include "ACPCopilotHistoryReader.h"
#include "ACPGeminiHistoryReader.h"
#include "ACPSettings.h"
#include "ACPTypes.h"
#include "ACPTerminalResumeCommand.h"
#include "AgentInstaller.h"
#include "AgentUsageMonitor.h"
#include "ACPRegistryClient.h"
#include "ACPAttachmentManager.h"
#include "ProjectIndexManager.h"
#include "ACPClipboardImageReader.h"
#include "ChatGatewayClient.h"
#include "ChatGatewayProvider.h"
#include "MCPServer.h"
#include "MCPTypes.h"
#include "AIKAnalytics.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformTime.h"
#include "ISettingsModule.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "IContentBrowserSingleton.h"
#include "ContentBrowserModule.h"
#include "SourceCodeNavigation.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Lua/NeoLuaState.h"
#include "Providers/GenerativeProvider.h"
#include "Providers/GenerativeProviderRegistry.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlWindowsModule.h"
#include "SourceControlWindows.h"
#include "UnrealEdMisc.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Styling/CoreStyle.h"
#include "Editor.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundBase.h"

// Helper: serialize a FJsonObject to compact JSON string
static FString JsonToString(const TSharedRef<FJsonObject>& Obj)
{
	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Obj, Writer);
	return Out;
}

// Helper: serialize a JSON array to compact string
static FString JsonArrayToString(const TArray<TSharedPtr<FJsonValue>>& Arr)
{
	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Arr, Writer);
	return Out;
}

static TArray<FACPRemoteSessionEntry> GetLocalHistorySessionsForAgent(const FString& AgentName, const FString& WorkingDirectory)
{
	if (AgentName == TEXT("Gemini CLI") || AgentName == TEXT("Gemini"))
	{
		FString AbsWorkDir = FPaths::ConvertRelativePathToFull(WorkingDirectory);
		TArray<FACPRemoteSessionEntry> Sessions = FACPGeminiHistoryReader::ListSessions(AbsWorkDir);
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("LocalHistory: Found %d Gemini sessions for '%s'"), Sessions.Num(), *AbsWorkDir);
		return Sessions;
	}

	if (AgentName == TEXT("Copilot CLI") || AgentName == TEXT("GitHub Copilot"))
	{
		FString AbsWorkDir = FPaths::ConvertRelativePathToFull(WorkingDirectory);
		TArray<FACPRemoteSessionEntry> Sessions = FACPCopilotHistoryReader::ListSessions(AbsWorkDir);
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("LocalHistory: Found %d Copilot sessions for '%s'"), Sessions.Num(), *AbsWorkDir);
		return Sessions;
	}

	if (AgentName == TEXT("Codex CLI") || AgentName == TEXT("Codex"))
	{
		FString AbsWorkDir = FPaths::ConvertRelativePathToFull(WorkingDirectory);
		TArray<FACPRemoteSessionEntry> Sessions = FACPCodexHistoryReader::ListSessions(AbsWorkDir);
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("LocalHistory: Found %d Codex sessions for '%s'"), Sessions.Num(), *AbsWorkDir);
		return Sessions;
	}

	return TArray<FACPRemoteSessionEntry>();
}

static bool LoadLocalHistorySession(
	const FString& AgentName,
	const FString& SessionId,
	TArray<FACPChatMessage>& OutMessages,
	FACPRemoteSessionEntry* OutMetadata = nullptr)
{
	if (AgentName == TEXT("Gemini CLI"))
	{
		return FACPGeminiHistoryReader::ParseSession(SessionId, OutMessages, OutMetadata);
	}

	if (AgentName == TEXT("Copilot CLI"))
	{
		return FACPCopilotHistoryReader::ParseSession(SessionId, OutMessages, OutMetadata);
	}

	if (AgentName == TEXT("Codex CLI") || AgentName == TEXT("Codex"))
	{
		FString WorkDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FString RolloutPath = FACPCodexHistoryReader::GetSessionJsonlPath(SessionId, WorkDir);
		if (!RolloutPath.IsEmpty())
		{
			return FACPCodexHistoryReader::ParseSessionJsonl(RolloutPath, OutMessages);
		}
	}

	return false;
}

static bool AgentUsesLaunchResumeHistory(const FString& AgentName)
{
	return AgentName == TEXT("Gemini CLI") || AgentName == TEXT("Copilot CLI");
}

static void SetSessionAgentRegistryFields(
	FACPAgentManager& AgentMgr,
	FJsonObject& SessionObj,
	const FString& AgentName)
{
	FString RegistryId;
	if (FACPAgentConfig* Cfg = AgentMgr.GetAgentConfig(AgentName))
	{
		RegistryId = Cfg->RegistryId;
	}
	SessionObj.SetStringField(TEXT("registryId"), RegistryId);
	SessionObj.SetBoolField(
		TEXT("terminalResumeSupported"),
		FACPTerminalResumeCommand::IsSupported(RegistryId, AgentName));
}

// ── Agent Discovery ──────────────────────────────────────────────────

FString UWebUIBridge::GetAgents()
{
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	TArray<FACPAgentConfig> Configs = AgentMgr.GetAllAgentConfigs();

	TArray<TSharedPtr<FJsonValue>> AgentsArray;

	for (const FACPAgentConfig& Config : Configs)
	{
		TSharedPtr<FJsonObject> AgentObj = MakeShared<FJsonObject>();
		AgentObj->SetStringField(TEXT("id"), Config.AgentName);
		AgentObj->SetStringField(TEXT("name"), Config.AgentName);

		// Map status enum to string
		FString StatusStr;
		switch (Config.Status)
		{
		case EACPAgentStatus::Available:     StatusStr = TEXT("available"); break;
		case EACPAgentStatus::NotInstalled:  StatusStr = TEXT("not_installed"); break;
		case EACPAgentStatus::MissingApiKey: StatusStr = TEXT("missing_key"); break;
		default:                             StatusStr = TEXT("unknown"); break;
		}
		AgentObj->SetStringField(TEXT("status"), StatusStr);
		AgentObj->SetStringField(TEXT("statusMessage"), Config.StatusMessage);
		AgentObj->SetBoolField(TEXT("isBuiltIn"), Config.bIsBuiltIn);
		AgentObj->SetBoolField(TEXT("isConnected"), AgentMgr.IsConnectedToAgent(Config.AgentName));
		AgentObj->SetStringField(TEXT("registryId"), Config.RegistryId);

		// Include icon URL from registry if available
		if (!Config.RegistryId.IsEmpty())
		{
			const FACPRegistryAgent* RegAgent = FACPRegistryClient::Get().FindAgent(Config.RegistryId);
			if (RegAgent)
			{
				AgentObj->SetStringField(TEXT("iconUrl"), RegAgent->IconUrl);
			}
		}

		AgentsArray.Add(MakeShared<FJsonValueObject>(AgentObj));
	}

	return JsonArrayToString(AgentsArray);
}

FString UWebUIBridge::GetLastUsedAgent()
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		return Settings->LastUsedAgentName;
	}
	return FString();
}

// ── Onboarding ──────────────────────────────────────────────────────
// State persisted via unified preferences.json (not UE SaveConfig)

bool UWebUIBridge::GetOnboardingCompleted()
{
	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings)
	{
		return true;
	}

	// Already loaded from preferences.json by LoadPreferences()
	if (Settings->bOnboardingCompleted)
	{
		return true;
	}

	// Auto-upgrade: if they've used the plugin before, skip onboarding
	if (!Settings->LastUsedAgentName.IsEmpty())
	{
		SetOnboardingCompleted();
		return true;
	}

	return false;
}

void UWebUIBridge::SetOnboardingCompleted()
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->bOnboardingCompleted = true;
		Settings->SavePreferences();
	}
}

// ── Session Lifecycle ────────────────────────────────────────────────

FString UWebUIBridge::CreateSession(const FString& AgentName)
{
	FACPSessionManager& SessionMgr = FACPSessionManager::Get();
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();

	FString SessionId = SessionMgr.CreateSession(AgentName);

	// Register with agent manager so SendPrompt can route messages
	AgentMgr.RegisterSession(SessionId, AgentName);

	// Gemini CLI sessions are process-coupled in ACP mode (model/context selected at launch).
	// Force a fresh subprocess for each "new chat" so sessions are truly isolated.
	if (AgentName == TEXT("Gemini CLI") && AgentMgr.IsConnectedToAgent(AgentName))
	{
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("WebUIBridge: Forcing fresh Gemini CLI process for new chat session %s"), *SessionId);
		AgentMgr.DisconnectFromAgent(AgentName);
	}

	// Connect to the agent if not already connected
	if (!AgentMgr.IsConnectedToAgent(AgentName))
	{
		AgentMgr.ConnectToAgent(AgentName);
	}

	// If agent is ready or already in a session, create a new external session immediately.
	// Otherwise queue it so NewSession fires automatically when Ready.
	// OpenRouter uses its own native client type, so route it separately.
	if (AgentMgr.IsChatGatewayAgent(AgentName))
	{
		TSharedPtr<FChatGatewayClient> GatewayClient = AgentMgr.GetChatGatewayClient(AgentName);
		if (GatewayClient.IsValid())
		{
			// Chat Gateway NewSession is local state setup; start it immediately.
			GatewayClient->SetUnrealSessionId(SessionId);
			GatewayClient->NewSession(FPaths::ProjectDir());
		}
	}
	else
	{
		TSharedPtr<FACPClient> Client = AgentMgr.GetClient(AgentName);
		if (Client.IsValid())
		{
			const EACPClientState ClientState = Client->GetState();
			if (ClientState == EACPClientState::Ready || ClientState == EACPClientState::InSession)
			{
				Client->SetUnrealSessionId(SessionId);
				Client->NewSession(FPaths::ProjectDir());
			}
			else
			{
				// Agent still connecting/initializing — queue NewSession for when it becomes Ready
				AgentMgr.QueuePendingNewSession(AgentName, SessionId);
			}
		}
	}

	// Persist agent as last-used
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->LastUsedAgentName = AgentName;
		Settings->SavePreferences();
	}

	// Analytics: track which agent was selected
	FAIKAnalytics::Get().RecordAgentSelected(AgentName);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("sessionId"), SessionId);
	Result->SetStringField(TEXT("agentName"), AgentName);

	return JsonToString(Result);
}

FString UWebUIBridge::GetSessions()
{
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	FACPSessionManager& SessionMgr = FACPSessionManager::Get();

	TSet<FString> SeenSessionIds;
	TArray<TSharedPtr<FJsonValue>> SessionsArray;

	// Build a set of agent session IDs that correspond to active Unreal sessions.
	// This lets us deduplicate: remote lists use agent IDs, active sessions use Unreal GUIDs.
	TSet<FString> KnownAgentSessionIds;
	TMap<FString, const FACPActiveSession*> AgentIdToActiveSession;
	TArray<FString> ActiveIds = SessionMgr.GetActiveSessionIds();
	for (const FString& Id : ActiveIds)
	{
		const FACPActiveSession* Active = SessionMgr.GetActiveSession(Id);
		if (Active && !Active->Metadata.AgentSessionId.IsEmpty())
		{
			KnownAgentSessionIds.Add(Active->Metadata.AgentSessionId);
			AgentIdToActiveSession.Add(Active->Metadata.AgentSessionId, Active);
		}
	}

	// 1. Remote sessions from each agent's cached ACP list
	TArray<FString> AgentNames = AgentMgr.GetAvailableAgentNames();
	for (const FString& AgentName : AgentNames)
	{
		TArray<FACPRemoteSessionEntry> RemoteSessions = AgentMgr.GetCachedSessionList(AgentName);
		RemoteSessions.Append(GetLocalHistorySessionsForAgent(AgentName, FPaths::ProjectDir()));
		for (const FACPRemoteSessionEntry& Entry : RemoteSessions)
		{
			if (SeenSessionIds.Contains(Entry.SessionId)) continue;

			// If this remote session corresponds to an active Unreal session,
			// update the active session's title from the remote data and skip
			// the remote entry (the active session will be listed in section 2)
			if (KnownAgentSessionIds.Contains(Entry.SessionId))
			{
				if (const FACPActiveSession** ActivePtr = AgentIdToActiveSession.Find(Entry.SessionId))
				{
					if (!Entry.Title.IsEmpty())
					{
						SessionMgr.UpdateSessionTitle((*ActivePtr)->Metadata.SessionId, Entry.Title);
					}
				}
				continue;
			}

			SeenSessionIds.Add(Entry.SessionId);

			TSharedPtr<FJsonObject> SessionObj = MakeShared<FJsonObject>();
			SessionObj->SetStringField(TEXT("sessionId"), Entry.SessionId);
			SessionObj->SetStringField(TEXT("agentName"), AgentName);
			SessionObj->SetStringField(TEXT("title"), Entry.Title);
			if (Entry.UpdatedAt.GetTicks() > 0)
			{
				SessionObj->SetStringField(TEXT("lastModifiedAt"), Entry.UpdatedAt.ToIso8601());
			}

			// Apply persisted custom title if available
			if (const FString* Persisted = SessionMgr.GetPersistedCustomTitle(Entry.SessionId))
			{
				SessionObj->SetStringField(TEXT("title"), *Persisted);
				SessionObj->SetBoolField(TEXT("hasCustomTitle"), true);
			}

			const FACPActiveSession* Active = SessionMgr.GetActiveSession(Entry.SessionId);
			SessionObj->SetBoolField(TEXT("isConnected"), Active ? Active->bIsConnected : false);
			SessionObj->SetBoolField(TEXT("isActive"), Active != nullptr);

			SetSessionAgentRegistryFields(AgentMgr, *SessionObj, AgentName);

			SessionsArray.Add(MakeShared<FJsonValueObject>(SessionObj));
		}
	}

	// 2. Active in-memory sessions not in remote lists (OpenRouter, newly created)
	for (const FString& Id : ActiveIds)
	{
		if (SeenSessionIds.Contains(Id)) continue;
		SeenSessionIds.Add(Id);

		const FACPActiveSession* Active = SessionMgr.GetActiveSession(Id);
		if (!Active) continue;

		TSharedPtr<FJsonObject> SessionObj = MakeShared<FJsonObject>();
		SessionObj->SetStringField(TEXT("sessionId"), Active->Metadata.SessionId);
		SessionObj->SetStringField(TEXT("agentName"), Active->Metadata.AgentName);
		SessionObj->SetStringField(TEXT("title"), Active->Metadata.Title);
		SessionObj->SetNumberField(TEXT("messageCount"), Active->Metadata.MessageCount);
		if (Active->Metadata.CreatedAt.GetTicks() > 0)
		{
			SessionObj->SetStringField(TEXT("createdAt"), Active->Metadata.CreatedAt.ToIso8601());
		}
		if (Active->Metadata.LastModifiedAt.GetTicks() > 0)
		{
			SessionObj->SetStringField(TEXT("lastModifiedAt"), Active->Metadata.LastModifiedAt.ToIso8601());
		}
		SessionObj->SetBoolField(TEXT("isConnected"), Active->bIsConnected);
		SessionObj->SetBoolField(TEXT("isActive"), true);
		SessionObj->SetBoolField(TEXT("hasCustomTitle"), Active->Metadata.bHasCustomTitle);

		SetSessionAgentRegistryFields(AgentMgr, *SessionObj, Active->Metadata.AgentName);

		SessionsArray.Add(MakeShared<FJsonValueObject>(SessionObj));
	}

	return JsonArrayToString(SessionsArray);
}

FString UWebUIBridge::ResumeSession(const FString& SessionId)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	if (SessionId.IsEmpty())
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Empty session ID"));
		return JsonToString(Result);
	}

	FACPSessionManager& SessionMgr = FACPSessionManager::Get();
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();

	// If already active, just switch to it
	const FACPActiveSession* ActiveSession = SessionMgr.GetActiveSession(SessionId);
	if (ActiveSession)
	{
		SessionMgr.SwitchToSession(SessionId);
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("agentName"), ActiveSession->Metadata.AgentName);
		return JsonToString(Result);
	}

	// Find which agent owns this session by checking cached ACP lists
	FString AgentName;
	for (const FString& Name : AgentMgr.GetAvailableAgentNames())
	{
		TArray<FACPRemoteSessionEntry> Sessions = AgentMgr.GetCachedSessionList(Name);
		Sessions.Append(GetLocalHistorySessionsForAgent(Name, FPaths::ProjectDir()));
		for (const FACPRemoteSessionEntry& Entry : Sessions)
		{
			if (Entry.SessionId == SessionId)
			{
				AgentName = Name;
				break;
			}
		}
		if (!AgentName.IsEmpty()) break;
	}

	if (AgentName.IsEmpty())
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Session not found in any agent's session list"));
		return JsonToString(Result);
	}

	// Create empty active session — messages will arrive via ACP replay
	if (!SessionMgr.ResumeSession(SessionId))
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Failed to create active session"));
		return JsonToString(Result);
	}

	// Set agent name on the session metadata
	if (FACPActiveSession* Session = SessionMgr.GetActiveSession(SessionId))
	{
		Session->Metadata.AgentName = AgentName;
		Session->Metadata.AgentSessionId = SessionId;
		if (AgentUsesLaunchResumeHistory(AgentName))
		{
			FACPRemoteSessionEntry Metadata;
			TArray<FACPChatMessage> Messages;
			if (LoadLocalHistorySession(AgentName, SessionId, Messages, &Metadata))
			{
				Session->Messages = MoveTemp(Messages);
				Session->Metadata.MessageCount = Session->Messages.Num();
				Session->Metadata.Title = Metadata.Title;
				if (Metadata.UpdatedAt.GetTicks() > 0)
				{
					Session->Metadata.LastModifiedAt = Metadata.UpdatedAt;
				}
			}
			Session->bIsLoadingHistory = false;
		}
	}

	// Register session with agent manager
	AgentMgr.RegisterSession(SessionId, AgentName);

	// Connect to the agent if not already connected
	if (AgentUsesLaunchResumeHistory(AgentName))
	{
		// Gemini and Copilot resume old sessions at process launch, so force
		// a fresh subprocess and inject the selected session ID into Connect().
		AgentMgr.DisconnectFromAgent(AgentName);
		AgentMgr.SetLaunchResumeSession(AgentName, SessionId);
	}

	if (!AgentMgr.IsConnectedToAgent(AgentName))
	{
		AgentMgr.ConnectToAgent(AgentName);
	}

	// For Gemini/Copilot local history, the session transcript was loaded from disk
	// and the subprocess resumes at launch. No ACP session/load call here.
	if (AgentUsesLaunchResumeHistory(AgentName))
	{
		if (TSharedPtr<FACPClient> Client = AgentMgr.GetClient(AgentName))
		{
			Client->SetUnrealSessionId(SessionId);
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetBoolField(TEXT("loading"), false);
		Result->SetStringField(TEXT("agentName"), AgentName);
		return JsonToString(Result);
	}

	// If agent is ready, load the session (messages arrive via ACP replay notifications)
	TSharedPtr<FACPClient> Client = AgentMgr.GetClient(AgentName);
	if (Client.IsValid())
	{
		const EACPClientState ClientState = Client->GetState();
		if (ClientState == EACPClientState::Ready || ClientState == EACPClientState::InSession)
		{
			FString WorkingDirectory = FPaths::ProjectDir();
			Client->SetUnrealSessionId(SessionId);

			const FACPAgentCapabilities& Caps = Client->GetAgentCapabilities();
			// Prefer session/load over session/resume — load replays message history
			// as session/update notifications so the UI can display past messages.
			// session/resume only restores the agent's internal context without replay.
			if (Caps.bSupportsLoadSession)
			{
				Client->LoadSession(SessionId, WorkingDirectory);
			}
			else if (Caps.bSupportsResumeSession)
			{
				Client->ResumeSession(SessionId, WorkingDirectory);
			}
			else
			{
				Client->NewSession(WorkingDirectory);
			}
		}
		else
		{
			// Agent still connecting — queue for when it becomes Ready
			AgentMgr.QueuePendingNewSession(AgentName, SessionId);
		}
	}

	Result->SetBoolField(TEXT("success"), true);
	Result->SetBoolField(TEXT("loading"), true);
	Result->SetStringField(TEXT("agentName"), AgentName);
	return JsonToString(Result);
}

FString UWebUIBridge::GetSessionMessages(const FString& SessionId)
{
	FACPSessionManager& SessionMgr = FACPSessionManager::Get();
	const FACPActiveSession* Session = SessionMgr.GetActiveSession(SessionId);

	if (!Session)
	{
		return TEXT("[]");
	}

	TArray<TSharedPtr<FJsonValue>> MessagesArray;
	for (const FACPChatMessage& Msg : Session->Messages)
	{
		TSharedPtr<FJsonObject> MsgJson = MessageToJson(Msg);
		if (MsgJson.IsValid())
		{
			MessagesArray.Add(MakeShared<FJsonValueObject>(MsgJson));
		}
	}

	return JsonArrayToString(MessagesArray);
}

FString UWebUIBridge::GetSessionTerminalResumeCommand(const FString& SessionId)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	if (SessionId.IsEmpty())
	{
		Result->SetBoolField(TEXT("supported"), false);
		Result->SetStringField(TEXT("error"), TEXT("Empty session ID"));
		return JsonToString(Result);
	}

	FACPSessionManager& SessionMgr = FACPSessionManager::Get();
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();

	FString AgentName;
	if (const FACPActiveSession* ActiveSession = SessionMgr.GetActiveSession(SessionId))
	{
		AgentName = ActiveSession->Metadata.AgentName;
	}
	else
	{
		for (const FString& Name : AgentMgr.GetAvailableAgentNames())
		{
			TArray<FACPRemoteSessionEntry> Sessions = AgentMgr.GetCachedSessionList(Name);
			Sessions.Append(GetLocalHistorySessionsForAgent(Name, FPaths::ProjectDir()));
			for (const FACPRemoteSessionEntry& Entry : Sessions)
			{
				if (Entry.SessionId == SessionId)
				{
					AgentName = Name;
					break;
				}
			}
			if (!AgentName.IsEmpty())
			{
				break;
			}
		}
	}

	if (AgentName.IsEmpty())
	{
		Result->SetBoolField(TEXT("supported"), false);
		Result->SetStringField(TEXT("error"), TEXT("Session not found"));
		return JsonToString(Result);
	}

	FString RegistryId;
	if (FACPAgentConfig* Cfg = AgentMgr.GetAgentConfig(AgentName))
	{
		RegistryId = Cfg->RegistryId;
	}

	FString CommandLine;
	if (!FACPTerminalResumeCommand::TryBuildCommandLine(RegistryId, AgentName, SessionId, CommandLine))
	{
		Result->SetBoolField(TEXT("supported"), false);
		Result->SetStringField(TEXT("agentName"), AgentName);
		Result->SetStringField(TEXT("registryId"), RegistryId);
		Result->SetStringField(TEXT("error"), TEXT("No terminal resume command for this agent"));
		return JsonToString(Result);
	}

	Result->SetBoolField(TEXT("supported"), true);
	Result->SetStringField(TEXT("command"), CommandLine);
	Result->SetStringField(TEXT("agentName"), AgentName);
	Result->SetStringField(TEXT("registryId"), RegistryId);
	return JsonToString(Result);
}

static FString NormalizeSummaryText(const FString& Input)
{
	FString Out = Input;
	Out.ReplaceInline(TEXT("\r"), TEXT(" "));
	Out.ReplaceInline(TEXT("\n"), TEXT(" "));
	Out.ReplaceInline(TEXT("\t"), TEXT(" "));
	while (Out.ReplaceInline(TEXT("  "), TEXT(" ")) > 0) {}
	Out.TrimStartAndEndInline();
	return Out;
}

static FString NormalizeSummaryProvider(const FString& Provider)
{
	FString Out = Provider;
	Out.TrimStartAndEndInline();
	Out = Out.ToLower();
	if (Out == TEXT("openrouter"))
	{
		return TEXT("openrouter");
	}
	return TEXT("local");
}

static FString NormalizeSummaryDetail(const FString& Detail)
{
	FString Out = Detail;
	Out.TrimStartAndEndInline();
	Out = Out.ToLower();
	if (Out == TEXT("detailed"))
	{
		return TEXT("detailed");
	}
	return TEXT("compact");
}

static FString TruncateSummaryText(const FString& Input, const int32 MaxChars)
{
	if (MaxChars <= 0)
	{
		return FString();
	}
	if (Input.Len() <= MaxChars)
	{
		return Input;
	}
	if (MaxChars <= 3)
	{
		return Input.Left(MaxChars);
	}
	return Input.Left(MaxChars - 3) + TEXT("...");
}

static FString MessageRoleToSummaryLabel(const EACPMessageRole Role)
{
	switch (Role)
	{
	case EACPMessageRole::User: return TEXT("User");
	case EACPMessageRole::Assistant: return TEXT("Assistant");
	case EACPMessageRole::System: return TEXT("System");
	default: return TEXT("Unknown");
	}
}

static FString ContentBlockToSummaryText(const FACPContentBlock& Block)
{
	switch (Block.Type)
	{
	case EACPContentBlockType::Text:
	case EACPContentBlockType::System:
	case EACPContentBlockType::Error:
		return Block.Text;
	case EACPContentBlockType::ToolCall:
		if (!Block.ToolName.IsEmpty())
		{
			return FString::Printf(TEXT("[Tool call: %s]"), *Block.ToolName);
		}
		return TEXT("[Tool call]");
	case EACPContentBlockType::ToolResult:
		if (!Block.ToolResultContent.IsEmpty())
		{
			const TCHAR* ResultStatus = Block.bToolSuccess ? TEXT("success") : TEXT("error");
			return FString::Printf(TEXT("[Tool result (%s): %s]"), ResultStatus, *Block.ToolResultContent);
		}
		return TEXT("[Tool result]");
	default:
		// Skip thought/image by default to keep continuation concise.
		return FString();
	}
}

static FString ContentBlockToTranscriptText(const FACPContentBlock& Block, const bool bDetailed)
{
	switch (Block.Type)
	{
	case EACPContentBlockType::Text:
	case EACPContentBlockType::System:
	case EACPContentBlockType::Error:
		return NormalizeSummaryText(Block.Text);
	case EACPContentBlockType::Thought:
		return bDetailed ? NormalizeSummaryText(FString::Printf(TEXT("[thought] %s"), *Block.Text)) : FString();
	case EACPContentBlockType::ToolCall:
	{
		FString Line = Block.ToolName.IsEmpty()
			? TEXT("[tool_call]")
			: FString::Printf(TEXT("[tool_call %s]"), *Block.ToolName);
		if (!Block.ToolArguments.IsEmpty())
		{
			const int32 MaxArgs = bDetailed ? 1800 : 700;
			Line += FString::Printf(TEXT(" args=%s"), *TruncateSummaryText(NormalizeSummaryText(Block.ToolArguments), MaxArgs));
		}
		return Line;
	}
	case EACPContentBlockType::ToolResult:
	{
		const TCHAR* ResultStatus = Block.bToolSuccess ? TEXT("success") : TEXT("error");
		const int32 MaxResult = bDetailed ? 3200 : 1200;
		FString ResultText = NormalizeSummaryText(Block.ToolResultContent);
		if (ResultText.IsEmpty())
		{
			ResultText = TEXT("(empty)");
		}
		return FString::Printf(TEXT("[tool_result %s] %s"), ResultStatus, *TruncateSummaryText(ResultText, MaxResult));
	}
	default:
		return FString();
	}
}

static bool ValidateContinuationSourceSession(const FString& SourceSessionId, const FACPActiveSession*& OutSession, FString& OutError)
{
	OutSession = nullptr;
	if (SourceSessionId.IsEmpty())
	{
		OutError = TEXT("Source session ID is empty");
		return false;
	}

	FACPSessionManager& SessionMgr = FACPSessionManager::Get();
	const FACPActiveSession* SourceSession = SessionMgr.GetActiveSession(SourceSessionId);
	if (!SourceSession)
	{
		OutError = TEXT("Source session is not loaded. Open the chat first, then try again.");
		return false;
	}
	if (SourceSession->Messages.Num() == 0)
	{
		OutError = TEXT("Source session has no messages to summarize.");
		return false;
	}
	if (SourceSession->Messages.Last().bIsStreaming)
	{
		OutError = TEXT("Source session is still streaming. Wait for completion, then continue.");
		return false;
	}

	OutSession = SourceSession;
	return true;
}

static FString BuildContinuationTranscript(const FACPActiveSession& Session, const bool bDetailed)
{
	const int32 MaxTranscriptChars = bDetailed ? 500000 : 250000;
	FString Transcript;
	Transcript.Reserve(FMath::Min(MaxTranscriptChars, 120000));

	int32 Turn = 1;
	for (const FACPChatMessage& Message : Session.Messages)
	{
		Transcript += FString::Printf(TEXT("Turn %d (%s):\n"), Turn++, *MessageRoleToSummaryLabel(Message.Role));
		for (const FACPContentBlock& Block : Message.ContentBlocks)
		{
			const FString BlockText = ContentBlockToTranscriptText(Block, bDetailed);
			if (!BlockText.IsEmpty())
			{
				Transcript += FString::Printf(TEXT("- %s\n"), *BlockText);
			}
			if (Transcript.Len() >= MaxTranscriptChars)
			{
				Transcript += TEXT("[transcript truncated due to size]\n");
				return Transcript;
			}
		}
		Transcript += TEXT("\n");
		if (Transcript.Len() >= MaxTranscriptChars)
		{
			Transcript += TEXT("[transcript truncated due to size]\n");
			return Transcript;
		}
	}
	return Transcript;
}

static FString BuildOpenRouterHandoffPrompt(const FACPActiveSession& Session, const FString& TargetAgentName, const FString& Detail)
{
	const bool bDetailed = Detail == TEXT("detailed");
	const FString Transcript = BuildContinuationTranscript(Session, bDetailed);

	FString Prompt;
	Prompt += TEXT("Your task is to create a detailed handoff summary of the conversation so far for another coding agent.\n");
	Prompt += FString::Printf(TEXT("Source agent: %s\n"), *Session.Metadata.AgentName);
	Prompt += FString::Printf(TEXT("Target agent: %s\n"), *TargetAgentName);
	Prompt += FString::Printf(TEXT("Requested detail: %s\n\n"), bDetailed ? TEXT("detailed") : TEXT("compact"));
	Prompt += TEXT("Output requirements:\n");
	Prompt += TEXT("1) Return ONLY the handoff summary text (no meta commentary, no XML tags).\n");
	Prompt += TEXT("2) Focus on explicit user requests and the assistant's concrete actions.\n");
	Prompt += TEXT("3) Preserve technical detail: exact file paths, function names/signatures, API names, errors, and concrete edits.\n");
	Prompt += TEXT("4) If something is unknown, write \"Unknown\" rather than guessing.\n");
	Prompt += TEXT("5) Prefer chronological clarity and include the latest unfinished work.\n");
	Prompt += TEXT("6) Keep summary concise but complete; prioritize continuity for implementation work.\n");
	Prompt += TEXT("7) If detail is compact, shorten each section but keep all headings.\n\n");
	Prompt += TEXT("Use exactly these markdown sections in order:\n");
	Prompt += TEXT("## Primary Request and Intent\n");
	Prompt += TEXT("## Key Technical Concepts\n");
	Prompt += TEXT("## Files and Code Sections\n");
	Prompt += TEXT("## Problem Solving\n");
	Prompt += TEXT("## Pending Tasks\n");
	Prompt += TEXT("## Current Work\n");
	Prompt += TEXT("## Optional Next Step\n");
	Prompt += TEXT("## Recent Conversation Evidence\n\n");
	Prompt += TEXT("Section-specific rules:\n");
	Prompt += TEXT("- Key Technical Concepts: bullet list.\n");
	Prompt += TEXT("- Files and Code Sections: bullet list with absolute or workspace-relative file paths; include why each file matters and what changed.\n");
	Prompt += TEXT("- Pending Tasks: only items explicitly requested and not completed.\n");
	Prompt += TEXT("- Optional Next Step: one concrete next action directly aligned with the latest user request.\n");
	Prompt += TEXT("- Recent Conversation Evidence: include 1-3 short verbatim quotes from the latest relevant user/assistant turns.\n\n");
	Prompt += TEXT("Conversation transcript:\n");
	Prompt += Transcript;
	return Prompt;
}

static FString ExtractOpenRouterContent(const TSharedPtr<FJsonObject>& MessageObj)
{
	if (!MessageObj.IsValid())
	{
		return FString();
	}

	FString Content;
	if (MessageObj->TryGetStringField(TEXT("content"), Content))
	{
		return Content;
	}

	const TArray<TSharedPtr<FJsonValue>>* ContentArray = nullptr;
	if (MessageObj->TryGetArrayField(TEXT("content"), ContentArray) && ContentArray)
	{
		FString Combined;
		for (const TSharedPtr<FJsonValue>& Entry : *ContentArray)
		{
			if (!Entry.IsValid())
			{
				continue;
			}

			// Some providers emit string chunks directly, others emit typed objects.
			FString Chunk;
			if (Entry->TryGetString(Chunk))
			{
				Combined += Chunk;
				continue;
			}

			const TSharedPtr<FJsonObject> EntryObj = Entry->AsObject();
			if (!EntryObj.IsValid())
			{
				continue;
			}
			if (EntryObj->TryGetStringField(TEXT("text"), Chunk))
			{
				Combined += Chunk;
				continue;
			}
			if (EntryObj->TryGetStringField(TEXT("summary"), Chunk))
			{
				Combined += Chunk;
			}
		}
		return Combined;
	}

	return FString();
}

static FString MessageToSummaryText(const FACPChatMessage& Message, const int32 MaxCharsPerMessage)
{
	FString Combined;
	for (const FACPContentBlock& Block : Message.ContentBlocks)
	{
		const FString BlockText = NormalizeSummaryText(ContentBlockToSummaryText(Block));
		if (BlockText.IsEmpty())
		{
			continue;
		}

		if (!Combined.IsEmpty())
		{
			Combined += TEXT(" ");
		}
		Combined += BlockText;
	}

	Combined = NormalizeSummaryText(Combined);
	return TruncateSummaryText(Combined, MaxCharsPerMessage);
}

FString UWebUIBridge::BuildContinuationDraft(const FString& SourceSessionId, const FString& TargetAgentName, const FString& SummaryMode)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), false);

	if (TargetAgentName.IsEmpty())
	{
		Result->SetStringField(TEXT("error"), TEXT("Target agent name is empty"));
		return JsonToString(Result);
	}

	const FString NormalizedDetail = NormalizeSummaryDetail(SummaryMode);
	const bool bDetailed = NormalizedDetail == TEXT("detailed");

	const FACPActiveSession* SourceSession = nullptr;
	FString ValidationError;
	if (!ValidateContinuationSourceSession(SourceSessionId, SourceSession, ValidationError))
	{
		Result->SetStringField(TEXT("error"), ValidationError);
		return JsonToString(Result);
	}

	const int32 MaxCharsPerMessage = bDetailed ? 520 : 300;
	const int32 HeadLineCount = bDetailed ? 6 : 3;
	const int32 TailLineCount = bDetailed ? 14 : 8;
	const int32 MaxDraftChars = bDetailed ? 12000 : 7000;

	struct FSummaryLine
	{
		FString Role;
		FString Text;
	};

	TArray<FSummaryLine> Lines;
	Lines.Reserve(SourceSession->Messages.Num());

	FString FirstUserMessage;
	FString LastUserMessage;
	FString LastAssistantMessage;

	for (const FACPChatMessage& Message : SourceSession->Messages)
	{
		const FString SummaryText = MessageToSummaryText(Message, MaxCharsPerMessage);
		if (SummaryText.IsEmpty())
		{
			continue;
		}

		FSummaryLine Line;
		Line.Role = MessageRoleToSummaryLabel(Message.Role);
		Line.Text = SummaryText;
		Lines.Add(Line);

		if (Message.Role == EACPMessageRole::User)
		{
			if (FirstUserMessage.IsEmpty())
			{
				FirstUserMessage = SummaryText;
			}
			LastUserMessage = SummaryText;
		}
		else if (Message.Role == EACPMessageRole::Assistant)
		{
			LastAssistantMessage = SummaryText;
		}
	}

	if (Lines.Num() == 0)
	{
		Result->SetStringField(TEXT("error"), TEXT("No text content found in source session messages."));
		return JsonToString(Result);
	}

	TSet<int32> IncludedIndices;
	for (int32 i = 0; i < FMath::Min(HeadLineCount, Lines.Num()); ++i)
	{
		IncludedIndices.Add(i);
	}
	for (int32 i = FMath::Max(0, Lines.Num() - TailLineCount); i < Lines.Num(); ++i)
	{
		IncludedIndices.Add(i);
	}

	TArray<int32> OrderedIndices = IncludedIndices.Array();
	OrderedIndices.Sort();

	FString Draft;
	Draft += TEXT("Continue this task in the new chat.\n\n");
	Draft += FString::Printf(TEXT("Previous agent: %s\n"), *SourceSession->Metadata.AgentName);
	Draft += FString::Printf(TEXT("Target agent: %s\n"), *TargetAgentName);
	Draft += FString::Printf(TEXT("Summary style: %s\n\n"), bDetailed ? TEXT("Detailed") : TEXT("Compact"));
	Draft += FString::Printf(TEXT("Turns analyzed: %d\n\n"), Lines.Num());

	if (!FirstUserMessage.IsEmpty())
	{
		Draft += FString::Printf(TEXT("Primary objective:\n%s\n\n"), *FirstUserMessage);
	}
	if (!LastUserMessage.IsEmpty())
	{
		Draft += FString::Printf(TEXT("Latest user request:\n%s\n\n"), *LastUserMessage);
	}
	if (!LastAssistantMessage.IsEmpty())
	{
		Draft += FString::Printf(TEXT("Latest assistant output:\n%s\n\n"), *LastAssistantMessage);
	}

	Draft += TEXT("Conversation digest (selected turns):\n");
	int32 DigestIndex = 1;
	for (const int32 LineIndex : OrderedIndices)
	{
		const FSummaryLine& Line = Lines[LineIndex];
		Draft += FString::Printf(TEXT("%d. %s: %s\n"), DigestIndex++, *Line.Role, *Line.Text);
		if (Draft.Len() > MaxDraftChars)
		{
			Draft += TEXT("...\n");
			break;
		}
	}
	if (OrderedIndices.Num() < Lines.Num())
	{
		Draft += FString::Printf(TEXT("... (%d condensed turns omitted)\n"), Lines.Num() - OrderedIndices.Num());
	}

	Draft += TEXT("\nIf anything important is missing, ask me one clarifying question before continuing.");
	Draft = TruncateSummaryText(Draft, MaxDraftChars);

	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("sourceSessionId"), SourceSessionId);
	Result->SetStringField(TEXT("targetAgentName"), TargetAgentName);
	Result->SetStringField(TEXT("summaryMode"), NormalizedDetail);
	Result->SetStringField(TEXT("draftPrompt"), Draft);
	Result->SetStringField(TEXT("providerUsed"), TEXT("local"));
	return JsonToString(Result);
}

FString UWebUIBridge::RequestContinuationDraft(const FString& SourceSessionId, const FString& TargetAgentName, const FString& SummaryMode)
{
	TSharedRef<FJsonObject> StartResult = MakeShared<FJsonObject>();
	StartResult->SetBoolField(TEXT("success"), false);
	StartResult->SetBoolField(TEXT("pending"), false);

	if (TargetAgentName.IsEmpty())
	{
		StartResult->SetStringField(TEXT("error"), TEXT("Target agent name is empty"));
		return JsonToString(StartResult);
	}

	UACPSettings* Settings = UACPSettings::Get();
	const FString RequestedProvider = NormalizeSummaryProvider(Settings ? Settings->ContinuationSummaryProvider : TEXT("openrouter"));
	FString EffectiveDetail = NormalizeSummaryDetail(SummaryMode);
	if (SummaryMode.IsEmpty() && Settings)
	{
		EffectiveDetail = NormalizeSummaryDetail(Settings->ContinuationSummaryDefaultDetail);
	}

	const int32 RequestId = NextContinuationDraftRequestId++;
	StartResult->SetBoolField(TEXT("success"), true);
	StartResult->SetBoolField(TEXT("pending"), true);
	StartResult->SetNumberField(TEXT("requestId"), RequestId);
	StartResult->SetStringField(TEXT("providerRequested"), RequestedProvider);
	StartResult->SetStringField(TEXT("summaryMode"), EffectiveDetail);

	const FString CompletionModel = [Settings]() -> FString
	{
		if (Settings)
		{
			FString Model = Settings->ContinuationSummaryModel;
			Model.TrimStartAndEndInline();
			if (!Model.IsEmpty())
			{
				return Model;
			}

			Model = Settings->OpenRouterDefaultModel;
			Model.TrimStartAndEndInline();
			if (!Model.IsEmpty())
			{
				return Model;
			}
		}
		return FString(TEXT("x-ai/grok-4.1-fast"));
	}();

	const bool bCanUseOpenRouter =
		RequestedProvider == TEXT("openrouter") &&
		Settings &&
		Settings->HasOpenRouterAuth();

	// OpenRouter summarization path (primary)
	if (bCanUseOpenRouter)
	{
		const FACPActiveSession* SourceSession = nullptr;
		FString ValidationError;
		if (!ValidateContinuationSourceSession(SourceSessionId, SourceSession, ValidationError))
		{
			StartResult->SetBoolField(TEXT("success"), false);
			StartResult->SetBoolField(TEXT("pending"), false);
			StartResult->SetStringField(TEXT("error"), ValidationError);
			return JsonToString(StartResult);
		}

		const FString Prompt = BuildOpenRouterHandoffPrompt(*SourceSession, TargetAgentName, EffectiveDetail);
		const bool bDetailed = EffectiveDetail == TEXT("detailed");

		TSharedRef<FJsonObject> RequestObj = MakeShared<FJsonObject>();
		RequestObj->SetStringField(TEXT("model"), CompletionModel);
		RequestObj->SetBoolField(TEXT("stream"), false);
		RequestObj->SetNumberField(TEXT("temperature"), 0.2);
		RequestObj->SetNumberField(TEXT("max_tokens"), bDetailed ? 8192 : 4096);

		TArray<TSharedPtr<FJsonValue>> MessagesArray;
		{
			TSharedRef<FJsonObject> SystemMsg = MakeShared<FJsonObject>();
			SystemMsg->SetStringField(TEXT("role"), TEXT("system"));
			SystemMsg->SetStringField(TEXT("content"), TEXT("You generate high-accuracy coding-session handoff summaries with strict markdown structure. Preserve technical details, avoid hallucinations, and prioritize explicit user intent plus latest unfinished work."));
			MessagesArray.Add(MakeShared<FJsonValueObject>(SystemMsg));
		}
		{
			TSharedRef<FJsonObject> UserMsg = MakeShared<FJsonObject>();
			UserMsg->SetStringField(TEXT("role"), TEXT("user"));
			UserMsg->SetStringField(TEXT("content"), Prompt);
			MessagesArray.Add(MakeShared<FJsonValueObject>(UserMsg));
		}
		RequestObj->SetArrayField(TEXT("messages"), MessagesArray);

		FString RequestBody;
		{
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
			FJsonSerializer::Serialize(RequestObj, Writer);
		}

		const FString ApiKey = Settings->GetOpenRouterAuthToken();
		TWeakObjectPtr<UWebUIBridge> WeakThis(this);
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
		HttpRequest->SetURL(Settings->GetOpenRouterChatCompletionsUrl());
		HttpRequest->SetVerb(TEXT("POST"));
		HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		HttpRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
		HttpRequest->SetHeader(TEXT("HTTP-Referer"), TEXT("https://github.com/betidestudio/AgentIntegrationKit"));
		HttpRequest->SetHeader(TEXT("X-Title"), TEXT("Agent Integration Kit"));
		HttpRequest->SetContentAsString(RequestBody);
		HttpRequest->OnProcessRequestComplete().BindLambda(
			[WeakThis, RequestId, SourceSessionId, TargetAgentName, EffectiveDetail](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess)
			{
				TSharedRef<FJsonObject> FinalResult = MakeShared<FJsonObject>();
				FinalResult->SetBoolField(TEXT("success"), false);
				FinalResult->SetStringField(TEXT("sourceSessionId"), SourceSessionId);
				FinalResult->SetStringField(TEXT("targetAgentName"), TargetAgentName);
				FinalResult->SetStringField(TEXT("summaryMode"), EffectiveDetail);
				FinalResult->SetStringField(TEXT("providerUsed"), TEXT("openrouter"));

				if (!bSuccess || !Response.IsValid())
				{
					FinalResult->SetStringField(TEXT("error"), TEXT("Failed to reach OpenRouter for summary generation."));
				}
				else if (Response->GetResponseCode() != 200)
				{
					FinalResult->SetStringField(
						TEXT("error"),
						FString::Printf(TEXT("OpenRouter summary request failed (%d): %s"), Response->GetResponseCode(), *TruncateSummaryText(Response->GetContentAsString(), 1200))
					);
				}
				else
				{
					TSharedPtr<FJsonObject> RootObj;
					const FString Body = Response->GetContentAsString();
					TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
					if (!FJsonSerializer::Deserialize(Reader, RootObj) || !RootObj.IsValid())
					{
						FinalResult->SetStringField(TEXT("error"), TEXT("Failed to parse OpenRouter summary response."));
					}
					else
					{
						const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
						if (!RootObj->TryGetArrayField(TEXT("choices"), Choices) || !Choices || Choices->Num() == 0)
						{
							FinalResult->SetStringField(TEXT("error"), TEXT("OpenRouter summary response contained no choices."));
						}
						else
						{
							const TSharedPtr<FJsonObject> ChoiceObj = (*Choices)[0]->AsObject();
							if (!ChoiceObj.IsValid() || !ChoiceObj->HasField(TEXT("message")))
							{
								FinalResult->SetStringField(TEXT("error"), TEXT("OpenRouter summary response missing message content."));
							}
							else
							{
								const TSharedPtr<FJsonObject> MessageObj = ChoiceObj->GetObjectField(TEXT("message"));
								FString DraftPrompt = NormalizeSummaryText(ExtractOpenRouterContent(MessageObj));
								if (DraftPrompt.IsEmpty())
								{
									FinalResult->SetStringField(TEXT("error"), TEXT("OpenRouter returned an empty summary."));
								}
								else
								{
									FinalResult->SetBoolField(TEXT("success"), true);
									FinalResult->SetStringField(TEXT("draftPrompt"), DraftPrompt);
								}
							}
						}
					}
				}

				const FString FinalJson = JsonToString(FinalResult);
				if (UWebUIBridge* Self = WeakThis.Get())
				{
					if (Self->OnContinuationDraftReadyCallback.IsValid())
					{
						Self->OnContinuationDraftReadyCallback(RequestId, FinalJson);
					}
				}
			}
		);

		if (!HttpRequest->ProcessRequest())
		{
			StartResult->SetBoolField(TEXT("success"), false);
			StartResult->SetBoolField(TEXT("pending"), false);
			StartResult->SetStringField(TEXT("error"), TEXT("Failed to start OpenRouter summary request."));
			return JsonToString(StartResult);
		}

		StartResult->SetStringField(TEXT("providerUsed"), TEXT("openrouter"));
		StartResult->SetStringField(TEXT("modelId"), CompletionModel);
		return JsonToString(StartResult);
	}

	// Local fallback path
	FString LocalResultJson = BuildContinuationDraft(SourceSessionId, TargetAgentName, EffectiveDetail);
	StartResult->SetStringField(TEXT("providerUsed"), TEXT("local"));
	if (RequestedProvider == TEXT("openrouter"))
	{
		StartResult->SetStringField(TEXT("fallbackReason"), TEXT("OpenRouter not available or API key missing; using local summary."));
	}

	TWeakObjectPtr<UWebUIBridge> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread, [WeakThis, RequestId, LocalResultJson]()
	{
		if (UWebUIBridge* Self = WeakThis.Get())
		{
			if (Self->OnContinuationDraftReadyCallback.IsValid())
			{
				Self->OnContinuationDraftReadyCallback(RequestId, LocalResultJson);
			}
		}
	});

	return JsonToString(StartResult);
}

FString UWebUIBridge::GetContinuationSummarySettings()
{
	const UACPSettings* Settings = UACPSettings::Get();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("provider"), NormalizeSummaryProvider(Settings ? Settings->ContinuationSummaryProvider : TEXT("openrouter")));

	FString ModelId = Settings ? Settings->ContinuationSummaryModel : FString();
	ModelId.TrimStartAndEndInline();
	if (ModelId.IsEmpty() && Settings)
	{
		ModelId = Settings->OpenRouterDefaultModel;
		ModelId.TrimStartAndEndInline();
	}
	if (ModelId.IsEmpty())
	{
		ModelId = TEXT("x-ai/grok-4.1-fast");
	}
	Result->SetStringField(TEXT("modelId"), ModelId);
	Result->SetStringField(TEXT("defaultDetail"), NormalizeSummaryDetail(Settings ? Settings->ContinuationSummaryDefaultDetail : TEXT("compact")));
	Result->SetBoolField(TEXT("hasOpenRouterKey"), Settings && Settings->HasOpenRouterAuth());
	return JsonToString(Result);
}

void UWebUIBridge::SetContinuationSummaryProvider(const FString& Provider)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->ContinuationSummaryProvider = NormalizeSummaryProvider(Provider);
		Settings->SavePreferences();
	}
}

void UWebUIBridge::SetContinuationSummaryModel(const FString& ModelId)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		FString Normalized = ModelId;
		Normalized.TrimStartAndEndInline();
		if (!Normalized.IsEmpty())
		{
			Settings->ContinuationSummaryModel = Normalized;
			Settings->SavePreferences();
		}
	}
}

void UWebUIBridge::SetContinuationSummaryDefaultDetail(const FString& Detail)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->ContinuationSummaryDefaultDetail = NormalizeSummaryDetail(Detail);
		Settings->SavePreferences();
	}
}

// ── Provider Settings ────────────────────────────────────────────────

FString UWebUIBridge::GetProviderSettings()
{
	const UACPSettings* Settings = UACPSettings::Get();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	// Priority array (ordered provider IDs)
	const TArray<FString>& Priority = Settings ? Settings->GetProviderPriority() : TArray<FString>{ TEXT("openrouter") };
	TArray<TSharedPtr<FJsonValue>> PriorityArray;
	for (const FString& Pid : Priority)
	{
		PriorityArray.Add(MakeShared<FJsonValueString>(Pid));
	}
	Result->SetArrayField(TEXT("priority"), PriorityArray);

	TArray<TSharedPtr<FJsonValue>> ProvidersArray;
	for (const TSharedRef<IChatGatewayProvider>& Provider : ChatGateway::GetAllProviders())
	{
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		const FString ProviderId = Provider->GetProviderIdString();
		P->SetStringField(TEXT("id"), ProviderId);
		P->SetStringField(TEXT("name"), Provider->GetDisplayName());
		P->SetStringField(TEXT("description"), Provider->GetDescription());
		P->SetBoolField(TEXT("requiresApiKey"), Provider->RequiresApiKey());
		P->SetStringField(TEXT("defaultBaseUrl"), Provider->GetDefaultBaseUrl());
		P->SetStringField(TEXT("defaultModel"), Provider->GetDefaultModel());
		P->SetBoolField(TEXT("supportsModelDiscovery"), Provider->SupportsModelDiscovery());

		bool bInPriorityList = Priority.Contains(ProviderId);
		bool bConfigured = Settings ? Settings->IsProviderConfigured(ProviderId) : (ProviderId == TEXT("openrouter"));
		P->SetBoolField(TEXT("configured"), bConfigured);
		P->SetBoolField(TEXT("inPriorityList"), bInPriorityList);

		// Check if this is a user-defined provider
		bool bIsUserDefined = ProviderId.StartsWith(TEXT("userprovider_"));
		P->SetBoolField(TEXT("isUserDefined"), bIsUserDefined);

		// For user-defined providers, separate capability (always true) from enabled state
		if (bIsUserDefined && Settings)
		{
			P->SetBoolField(TEXT("supportsModelDiscovery"), true);
			const FCustomProviderDefinition* Def = Settings->FindCustomProvider(ProviderId);
			P->SetBoolField(TEXT("enableModelDiscovery"), Def ? Def->bEnableModelDiscovery : false);
		}
		else
		{
			P->SetBoolField(TEXT("enableModelDiscovery"), Provider->SupportsModelDiscovery());
		}

		if (Settings)
		{
			FString ApiKey = Settings->GetProviderApiKey(ProviderId);
			P->SetBoolField(TEXT("hasApiKey"), !ApiKey.IsEmpty());
			// Mask the key for display — show last 4 chars
			if (ApiKey.Len() > 4)
			{
				ApiKey = FString::ChrN(ApiKey.Len() - 4, TEXT('*')) + ApiKey.Right(4);
			}
			P->SetStringField(TEXT("apiKeyMasked"), ApiKey);

			FString BaseUrl = Settings->GetProviderBaseUrl(ProviderId);
			P->SetStringField(TEXT("baseUrl"), BaseUrl.IsEmpty() ? Provider->GetDefaultBaseUrl() : BaseUrl);

			// Include model list for user-defined providers and extra models for built-in providers
			if (bIsUserDefined)
			{
				const FCustomProviderDefinition* Def = Settings->FindCustomProvider(ProviderId);
				if (Def)
				{
					TArray<TSharedPtr<FJsonValue>> ModelsArr;
					for (const FCustomProviderModelEntry& M : Def->Models)
					{
						TSharedRef<FJsonObject> MObj = MakeShared<FJsonObject>();
						MObj->SetStringField(TEXT("id"), M.ModelId);
						MObj->SetStringField(TEXT("name"), M.DisplayName);
						MObj->SetStringField(TEXT("description"), M.Description);
						ModelsArr.Add(MakeShared<FJsonValueObject>(MObj));
					}
					P->SetArrayField(TEXT("models"), ModelsArr);
					P->SetBoolField(TEXT("requiresApiKey"), Def->bRequiresApiKey);
				}
			}
			else if (const TArray<FCustomProviderModelEntry>* Extras = Settings->GetExtraProviderModels(ProviderId))
			{
				TArray<TSharedPtr<FJsonValue>> ModelsArr;
				for (const FCustomProviderModelEntry& M : *Extras)
				{
					TSharedRef<FJsonObject> MObj = MakeShared<FJsonObject>();
					MObj->SetStringField(TEXT("id"), M.ModelId);
					MObj->SetStringField(TEXT("name"), M.DisplayName);
					MObj->SetStringField(TEXT("description"), M.Description);
					ModelsArr.Add(MakeShared<FJsonValueObject>(MObj));
				}
				P->SetArrayField(TEXT("models"), ModelsArr);
			}
		}
		else
		{
			P->SetBoolField(TEXT("hasApiKey"), false);
			P->SetStringField(TEXT("apiKeyMasked"), TEXT(""));
			P->SetStringField(TEXT("baseUrl"), Provider->GetDefaultBaseUrl());
		}

		ProvidersArray.Add(MakeShared<FJsonValueObject>(P));
	}

	Result->SetArrayField(TEXT("providers"), ProvidersArray);
	return JsonToString(Result);
}

void UWebUIBridge::SetProviderPriority(const FString& PriorityJson)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		TArray<FString> NewPriority;
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PriorityJson);
		if (FJsonSerializer::Deserialize(Reader, JsonArray))
		{
			for (const TSharedPtr<FJsonValue>& Val : JsonArray)
			{
				FString Id;
				if (Val->TryGetString(Id))
				{
					NewPriority.Add(Id.TrimStartAndEnd().ToLower());
				}
			}
		}
		Settings->SetProviderPriority(NewPriority);
	}
}

void UWebUIBridge::AddProvider(const FString& ProviderId)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->AddProviderToPriority(ProviderId.TrimStartAndEnd().ToLower());
	}
}

void UWebUIBridge::RemoveProvider(const FString& ProviderId)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->RemoveProviderFromPriority(ProviderId.TrimStartAndEnd().ToLower());
	}
}

void UWebUIBridge::SetProviderApiKey(const FString& ProviderId, const FString& ApiKey)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->SetProviderApiKey(ProviderId, ApiKey);
	}
}

void UWebUIBridge::SetProviderBaseUrl(const FString& ProviderId, const FString& BaseUrl)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->SetProviderBaseUrl(ProviderId, BaseUrl);
	}
}

// ── Custom Provider CRUD ─────────────────────────────────────────────

FString UWebUIBridge::CreateCustomProvider(const FString& DisplayName, const FString& BaseUrl)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		FString ProviderId = Settings->CreateCustomProvider(DisplayName, BaseUrl);
		ChatGateway::SyncCustomProviders();
		Result->SetStringField(TEXT("providerId"), ProviderId);
	}
	return JsonToString(Result);
}

void UWebUIBridge::DeleteCustomProvider(const FString& ProviderId)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->DeleteCustomProvider(ProviderId);
		ChatGateway::SyncCustomProviders();
	}
}

void UWebUIBridge::UpdateCustomProvider(const FString& ProviderId, const FString& DisplayName, const FString& BaseUrl)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->UpdateCustomProvider(ProviderId, DisplayName, BaseUrl);
		ChatGateway::SyncCustomProviders();
	}
}

FString UWebUIBridge::AddCustomProviderModel(const FString& ProviderId, const FString& ModelId, const FString& DisplayName, const FString& Description)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		// Route to the right storage: custom provider models vs extra models for built-in providers
		if (Settings->FindCustomProvider(ProviderId))
		{
			Settings->AddCustomProviderModel(ProviderId, ModelId, DisplayName, Description);
			ChatGateway::SyncCustomProviders();
		}
		else
		{
			// Built-in provider (e.g. Ollama) — store in extra models
			Settings->AddExtraProviderModel(ProviderId, ModelId, DisplayName, Description);
		}
		Result->SetBoolField(TEXT("success"), true);
	}
	else
	{
		Result->SetBoolField(TEXT("success"), false);
	}
	return JsonToString(Result);
}

void UWebUIBridge::RemoveCustomProviderModel(const FString& ProviderId, const FString& ModelId)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		if (Settings->FindCustomProvider(ProviderId))
		{
			Settings->RemoveCustomProviderModel(ProviderId, ModelId);
			ChatGateway::SyncCustomProviders();
		}
		else
		{
			Settings->RemoveExtraProviderModel(ProviderId, ModelId);
		}
	}
}

FString UWebUIBridge::ImportCustomProviderModels(const FString& ProviderId, const FString& ModelsJson)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ErrorsArr;

	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings)
	{
		Result->SetNumberField(TEXT("imported"), 0);
		ErrorsArr.Add(MakeShared<FJsonValueString>(TEXT("Settings not available")));
		Result->SetArrayField(TEXT("errors"), ErrorsArr);
		return JsonToString(Result);
	}

	// Try to parse the JSON — support both [{id,...}] and {"data":[{id,...}]}
	TArray<TSharedPtr<FJsonValue>> ModelsArray;

	// Try as array first
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ModelsJson);
	if (!FJsonSerializer::Deserialize(Reader, ModelsArray))
	{
		// Try as object with "data" field
		TSharedPtr<FJsonObject> Wrapper;
		TSharedRef<TJsonReader<>> Reader2 = TJsonReaderFactory<>::Create(ModelsJson);
		if (FJsonSerializer::Deserialize(Reader2, Wrapper) && Wrapper.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* DataField;
			if (Wrapper->TryGetArrayField(TEXT("data"), DataField))
			{
				ModelsArray = *DataField;
			}
		}
	}

	if (ModelsArray.Num() == 0)
	{
		Result->SetNumberField(TEXT("imported"), 0);
		ErrorsArr.Add(MakeShared<FJsonValueString>(TEXT("No valid models found in JSON. Expected [{\"id\":\"...\"}] or {\"data\":[{\"id\":\"...\"}]}")));
		Result->SetArrayField(TEXT("errors"), ErrorsArr);
		return JsonToString(Result);
	}

	// Parse into FCustomProviderModelEntry array
	TArray<FCustomProviderModelEntry> Entries;
	for (const TSharedPtr<FJsonValue>& Val : ModelsArray)
	{
		TSharedPtr<FJsonObject> Obj = Val->AsObject();
		if (!Obj.IsValid()) continue;

		FCustomProviderModelEntry Entry;
		if (!Obj->TryGetStringField(TEXT("id"), Entry.ModelId) || Entry.ModelId.IsEmpty())
		{
			continue;
		}
		Obj->TryGetStringField(TEXT("name"), Entry.DisplayName);
		if (Entry.DisplayName.IsEmpty()) Entry.DisplayName = Entry.ModelId;
		Obj->TryGetStringField(TEXT("description"), Entry.Description);
		Entries.Add(Entry);
	}

	int32 Imported = Settings->ImportCustomProviderModels(ProviderId, Entries);
	ChatGateway::SyncCustomProviders();

	Result->SetNumberField(TEXT("imported"), Imported);
	Result->SetArrayField(TEXT("errors"), ErrorsArr);
	return JsonToString(Result);
}

void UWebUIBridge::SetCustomProviderModelDiscovery(const FString& ProviderId, bool bEnabled)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->SetCustomProviderModelDiscovery(ProviderId, bEnabled);
		ChatGateway::SyncCustomProviders();

		// Trigger model re-fetch so new discovery setting takes effect immediately
		FACPAgentManager& Manager = FACPAgentManager::Get();
		TSharedPtr<FChatGatewayClient> Client = Manager.GetChatGatewayClient(TEXT("OpenRouter"));
		if (Client.IsValid())
		{
			Client->RefreshModels();
		}
	}
}

void UWebUIBridge::RefreshProviderModels()
{
	FACPAgentManager& Manager = FACPAgentManager::Get();
	TSharedPtr<FChatGatewayClient> Client = Manager.GetChatGatewayClient(TEXT("OpenRouter"));
	if (Client.IsValid())
	{
		Client->RefreshModels();
	}
}

FString UWebUIBridge::GetEnabledModels()
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	const UACPSettings* Settings = UACPSettings::Get();
	if (Settings)
	{
		const TSet<FString>& Enabled = Settings->GetEnabledModels();
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FString& Id : Enabled)
		{
			Arr.Add(MakeShared<FJsonValueString>(Id));
		}
		Root->SetArrayField(TEXT("enabledModels"), Arr);
		Root->SetBoolField(TEXT("hasCustomSelection"), Enabled.Num() > 0);
	}
	else
	{
		Root->SetArrayField(TEXT("enabledModels"), {});
		Root->SetBoolField(TEXT("hasCustomSelection"), false);
	}
	return JsonToString(Root);
}

void UWebUIBridge::SetModelEnabled(const FString& ModelId, bool bEnabled)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->SetModelEnabled(ModelId, bEnabled);

		// Refresh the curated model list so the dropdown updates
		FACPAgentManager& Manager = FACPAgentManager::Get();
		TSharedPtr<FChatGatewayClient> Client = Manager.GetChatGatewayClient(TEXT("OpenRouter"));
		if (Client.IsValid())
		{
			Client->RefreshModels();
		}
	}
}

void UWebUIBridge::SetEnabledModels(const FString& ModelIdsJson)
{
	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings) return;

	TArray<TSharedPtr<FJsonValue>> JsonArr;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ModelIdsJson);
	if (FJsonSerializer::Deserialize(Reader, JsonArr))
	{
		// Clear and rebuild
		Settings->EnabledModels.Empty();
		for (const TSharedPtr<FJsonValue>& Val : JsonArr)
		{
			FString Id;
			if (Val.IsValid() && Val->TryGetString(Id) && !Id.IsEmpty())
			{
				Settings->EnabledModels.Add(Id);
			}
		}
		Settings->SavePreferences();

		// Refresh the curated model list
		FACPAgentManager& Manager = FACPAgentManager::Get();
		TSharedPtr<FChatGatewayClient> Client = Manager.GetChatGatewayClient(TEXT("OpenRouter"));
		if (Client.IsValid())
		{
			Client->RefreshModels();
		}
	}
}

// ── Messaging ────────────────────────────────────────────────────────

namespace
{
	// Keep automatic @ mention context bounded so adapter/model limits are not exceeded.
	constexpr int32 MaxMentionPathsPerPrompt = 4;
	constexpr int32 MaxMentionItemChars = 6000;
	constexpr int32 MaxMentionContextChars = 24000;

	static FString TruncateForPromptBudget(const FString& Input, int32 MaxChars, bool& bOutTruncated)
	{
		bOutTruncated = false;
		if (MaxChars <= 0 || Input.Len() <= MaxChars)
		{
			return Input;
		}

		const FString Suffix = TEXT("\n...[truncated for prompt size]\n");
		const int32 KeepChars = FMath::Max(0, MaxChars - Suffix.Len());
		bOutTruncated = true;
		return Input.Left(KeepChars) + Suffix;
	}
}

// Helper: parse @/Game/... and @Source/... paths from message text (mirrors SAgentChatWindow::ParseAtMentionPaths)
static TArray<FString> ParseAtMentionPaths(const FString& MessageText)
{
	TArray<FString> Paths;
	int32 SearchStart = 0;
	while (SearchStart < MessageText.Len())
	{
		int32 AtIndex = MessageText.Find(TEXT("@"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchStart);
		if (AtIndex == INDEX_NONE) break;

		int32 PathStart = AtIndex + 1;
		if (PathStart >= MessageText.Len()) break;

		int32 PathEnd = PathStart;
		while (PathEnd < MessageText.Len() && !FChar::IsWhitespace(MessageText[PathEnd]))
		{
			++PathEnd;
		}

		FString Path = MessageText.Mid(PathStart, PathEnd - PathStart);
		if (Path.StartsWith(TEXT("/")) || Path.StartsWith(TEXT("Source/")))
		{
			Paths.AddUnique(Path);
		}
		SearchStart = PathEnd;
	}
	return Paths;
}

// Helper: resolve paths to context text using Lua open_asset()+info() / read_file()
static FString BuildContextForPaths(const TArray<FString>& Paths)
{
	if (Paths.Num() == 0) return FString();

	FString ContextText = TEXT("## Referenced Context\n\n");
	const int32 PathsToProcess = FMath::Min(Paths.Num(), MaxMentionPathsPerPrompt);
	int32 TruncatedItemCount = 0;
	int32 OmittedItemCount = 0;

	// Build a single Lua script that processes all paths (one state creation for all)
	FString Script = TEXT(
		"local function dump(t, indent)\n"
		"  indent = indent or \"\"\n"
		"  if type(t) ~= \"table\" then print(indent .. tostring(t)) return end\n"
		"  local keys = {}\n"
		"  for k in pairs(t) do keys[#keys+1] = tostring(k) end\n"
		"  table.sort(keys)\n"
		"  for _, k in ipairs(keys) do\n"
		"    local v = t[k]\n"
		"    if type(v) == \"table\" then\n"
		"      print(indent .. k .. \":\")\n"
		"      dump(v, indent .. \"  \")\n"
		"    else\n"
		"      print(indent .. k .. \": \" .. tostring(v))\n"
		"    end\n"
		"  end\n"
		"end\n\n"
	);

	for (int32 i = 0; i < PathsToProcess; ++i)
	{
		// Escape quotes in path for Lua string literal
		FString EscapedPath = Paths[i].Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
		Script += FString::Printf(TEXT(
			"print(\"###ASSET_START:%d\")\n"
			"do\n"
			"  local h = open_asset(\"%s\")\n"
			"  if h then\n"
			"    local i = h:info()\n"
			"    if i then dump(i) end\n"
			"  else\n"
			"    local f = read_file(\"%s\")\n"
			"    if f and f.content then\n"
			"      print(f.content)\n"
			"    else\n"
			"      print(\"Could not load asset or file\")\n"
			"    end\n"
			"  end\n"
			"end\n"
			"print(\"###ASSET_END:%d\")\n\n"
		), i, *EscapedPath, *EscapedPath, i);
	}

	FNeoLuaState LuaState;
	FScriptResult Result = LuaState.Execute(Script);

	// Parse trace output into per-path segments
	TArray<FString> PerPathOutput;
	PerPathOutput.SetNum(PathsToProcess);

	int32 CurrentIndex = -1;
	for (const FString& Line : Result.Trace)
	{
		if (Line.StartsWith(TEXT("###ASSET_START:")))
		{
			CurrentIndex = FCString::Atoi(*Line.Mid(15));
			continue;
		}
		if (Line.StartsWith(TEXT("###ASSET_END:")))
		{
			CurrentIndex = -1;
			continue;
		}
		// Skip internal [OK]/[FAIL] log lines from open_asset/info
		if (Line.StartsWith(TEXT("[OK]")) || Line.StartsWith(TEXT("[FAIL]")))
		{
			continue;
		}
		if (CurrentIndex >= 0 && CurrentIndex < PathsToProcess)
		{
			if (!PerPathOutput[CurrentIndex].IsEmpty())
			{
				PerPathOutput[CurrentIndex] += TEXT("\n");
			}
			PerPathOutput[CurrentIndex] += Line;
		}
	}

	// Build final context text from parsed segments
	for (int32 PathIndex = 0; PathIndex < PathsToProcess; ++PathIndex)
	{
		FString DisplayName = FPaths::GetCleanFilename(Paths[PathIndex]);
		FString EntryBody = PerPathOutput[PathIndex];

		if (EntryBody.IsEmpty())
		{
			EntryBody = TEXT("No info available");
		}

		bool bEntryTruncated = false;
		EntryBody = TruncateForPromptBudget(EntryBody, MaxMentionItemChars, bEntryTruncated);
		if (bEntryTruncated)
		{
			++TruncatedItemCount;
		}

		const FString EntryText = FString::Printf(TEXT("### %s\n```\n%s\n```\n\n"), *DisplayName, *EntryBody);
		const int32 RemainingBudget = MaxMentionContextChars - ContextText.Len();
		if (EntryText.Len() > RemainingBudget)
		{
			OmittedItemCount += (PathsToProcess - PathIndex);
			break;
		}

		ContextText += EntryText;
	}

	if (Paths.Num() > PathsToProcess)
	{
		OmittedItemCount += (Paths.Num() - PathsToProcess);
	}

	if (TruncatedItemCount > 0 || OmittedItemCount > 0)
	{
		const FString SizeNote = FString::Printf(
			TEXT("> Note: mention context was size-limited (truncated=%d, omitted=%d).\n\n"),
			TruncatedItemCount,
			OmittedItemCount);
		const int32 RemainingBudget = MaxMentionContextChars - ContextText.Len();
		if (RemainingBudget > 0)
		{
			bool bIgnored = false;
			ContextText += TruncateForPromptBudget(SizeNote, RemainingBudget, bIgnored);
		}
	}

	return ContextText;
}

void UWebUIBridge::SendPrompt(const FString& SessionId, const FString& Text)
{
	const FString TrimmedSessionId = SessionId.TrimStartAndEnd();
	FString TrimmedText = Text;
	TrimmedText.TrimStartAndEndInline();

	if (TrimmedSessionId.IsEmpty())
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("WebUIBridge: SendPrompt ignored - empty session ID"));
		return;
	}

	if (TrimmedText.IsEmpty())
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("WebUIBridge: SendPrompt ignored - empty text for session %s"), *TrimmedSessionId);
		return;
	}

	FACPSessionManager& SessionMgr = FACPSessionManager::Get();
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	const FACPActiveSession* Session = SessionMgr.GetActiveSession(TrimmedSessionId);
	if (!Session)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("WebUIBridge: SendPrompt ignored - session not found: %s"), *TrimmedSessionId);
		return;
	}

	// Parse @ mentions and resolve context (same as Slate UI)
	TArray<FString> MentionedPaths = ParseAtMentionPaths(TrimmedText);
	FString ContextPrefix = BuildContextForPaths(MentionedPaths);

	// Build the full prompt with context prepended
	FString FullPrompt = ContextPrefix.IsEmpty() ? TrimmedText : (ContextPrefix + TrimmedText);

	// Add user message to session (show original text, not with context)
	SessionMgr.AddUserMessage(TrimmedSessionId, TrimmedText);

	// Provisional title from first message (like Zed) — shows immediately in sidebar,
	// replaced when the agent sends session_info_update with a proper title.
	if ((Session->Metadata.MessageCount <= 1 && Session->Metadata.Title.IsEmpty()) || Session->Metadata.Title == TEXT("New conversation"))
	{
		// Take first line, truncate to 80 chars
		FString FirstLine = TrimmedText;
		int32 NewlineIdx;
		if (FirstLine.FindChar(TEXT('\n'), NewlineIdx))
		{
			FirstLine = FirstLine.Left(NewlineIdx);
		}
		FirstLine.TrimStartAndEndInline();
		if (FirstLine.Len() > 80)
		{
			FirstLine = FirstLine.Left(77) + TEXT("...");
		}
		if (!FirstLine.IsEmpty())
		{
			SessionMgr.UpdateSessionTitle(TrimmedSessionId, FirstLine);
		}
	}

	// Get the agent for this session
	FString AgentName = AgentMgr.GetSessionAgent(TrimmedSessionId);
	if (AgentName.IsEmpty())
	{
		AgentName = Session->Metadata.AgentName;
	}

	if (!AgentName.IsEmpty())
	{
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("WebUIBridge: Sending prompt for session %s via agent %s (user_len=%d, mention_context_len=%d, full_len=%d, mentions=%d)"),
			*TrimmedSessionId, *AgentName, TrimmedText.Len(), ContextPrefix.Len(), FullPrompt.Len(), MentionedPaths.Num());
		AgentMgr.SendPromptToSession(TrimmedSessionId, AgentName, FullPrompt);
	}
	else
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("WebUIBridge: SendPrompt ignored - no agent mapped for session %s"), *TrimmedSessionId);
	}
}

void UWebUIBridge::CancelPrompt(const FString& SessionId)
{
	if (!SessionId.IsEmpty())
	{
		FACPAgentManager::Get().CancelSessionPrompt(SessionId);
	}
}

// ── Model & Reasoning ───────────────────────────────────────────────

FString UWebUIBridge::GetModels(const FString& AgentName)
{
	if (AgentName.IsEmpty()) return TEXT("[]");

	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	FACPSessionModelState ModelState = AgentMgr.GetAgentModelState(AgentName);

	// Check if ACP client has reasoning effort options (from config_option_update)
	bool bACPHasReasoning = false;
	TSharedPtr<FACPClient> ACPClient = AgentMgr.GetClient(AgentName);
	if (ACPClient.IsValid() && ACPClient->SupportsReasoningEffortControl())
	{
		bACPHasReasoning = true;
	}

	TArray<TSharedPtr<FJsonValue>> ModelsArray;

	for (const FACPModelInfo& Model : ModelState.AvailableModels)
	{
		TSharedPtr<FJsonObject> ModelObj = MakeShared<FJsonObject>();
		ModelObj->SetStringField(TEXT("id"), Model.ModelId);
		ModelObj->SetStringField(TEXT("name"), Model.Name);
		ModelObj->SetStringField(TEXT("description"), Model.Description);
		ModelObj->SetBoolField(TEXT("supportsReasoning"), Model.SupportsReasoning() || bACPHasReasoning);
		if (!Model.ProviderId.IsEmpty()) ModelObj->SetStringField(TEXT("provider"), Model.ProviderId);
		if (!Model.ProviderDisplayName.IsEmpty()) ModelObj->SetStringField(TEXT("providerDisplayName"), Model.ProviderDisplayName);
		ModelsArray.Add(MakeShared<FJsonValueObject>(ModelObj));
	}

	// Include current model ID
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("models"), ModelsArray);
	Result->SetStringField(TEXT("currentModelId"), ModelState.CurrentModelId);

	return JsonToString(Result);
}

FString UWebUIBridge::GetAllModels(const FString& AgentName)
{
	if (AgentName.IsEmpty()) return TEXT("[]");

	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	FACPSessionModelState ModelState = AgentMgr.GetAgentModelState(AgentName);
	const TArray<FACPModelInfo> FullModels = AgentMgr.GetAgentFullModelList(AgentName);

	// Check if ACP client has reasoning effort options (from config_option_update)
	bool bACPHasReasoning = false;
	TSharedPtr<FACPClient> ACPClient = AgentMgr.GetClient(AgentName);
	if (ACPClient.IsValid() && ACPClient->SupportsReasoningEffortControl())
	{
		bACPHasReasoning = true;
	}

	TArray<TSharedPtr<FJsonValue>> ModelsArray;
	for (const FACPModelInfo& Model : FullModels)
	{
		TSharedPtr<FJsonObject> ModelObj = MakeShared<FJsonObject>();
		ModelObj->SetStringField(TEXT("id"), Model.ModelId);
		ModelObj->SetStringField(TEXT("name"), Model.Name);
		ModelObj->SetStringField(TEXT("description"), Model.Description);
		ModelObj->SetBoolField(TEXT("supportsReasoning"), Model.SupportsReasoning() || bACPHasReasoning);
		if (!Model.ProviderId.IsEmpty()) ModelObj->SetStringField(TEXT("provider"), Model.ProviderId);
		if (!Model.ProviderDisplayName.IsEmpty()) ModelObj->SetStringField(TEXT("providerDisplayName"), Model.ProviderDisplayName);
		ModelsArray.Add(MakeShared<FJsonValueObject>(ModelObj));
	}

	// Fallback to curated list if full list is unavailable.
	if (ModelsArray.Num() == 0)
	{
		for (const FACPModelInfo& Model : ModelState.AvailableModels)
		{
			TSharedPtr<FJsonObject> ModelObj = MakeShared<FJsonObject>();
			ModelObj->SetStringField(TEXT("id"), Model.ModelId);
			ModelObj->SetStringField(TEXT("name"), Model.Name);
			ModelObj->SetStringField(TEXT("description"), Model.Description);
			ModelObj->SetBoolField(TEXT("supportsReasoning"), Model.SupportsReasoning() || bACPHasReasoning);
			ModelsArray.Add(MakeShared<FJsonValueObject>(ModelObj));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("models"), ModelsArray);
	Result->SetStringField(TEXT("currentModelId"), ModelState.CurrentModelId);
	return JsonToString(Result);
}

void UWebUIBridge::SetModel(const FString& AgentName, const FString& ModelId)
{
	if (AgentName.IsEmpty() || ModelId.IsEmpty()) return;

	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	AgentMgr.SetAgentModel(AgentName, ModelId);

	// Persist the selection
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->SaveModelForAgent(AgentName, ModelId);
	}
}

FString UWebUIBridge::GetReasoningLevel(const FString& AgentName)
{
	if (AgentName.IsEmpty())
	{
		return TEXT("high");
	}

	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	UACPSettings* Settings = UACPSettings::Get();

	// Prefer persisted value as fallback
	const FString SavedLevel = Settings ? Settings->GetSavedReasoningForAgent(AgentName) : FString();

	// OpenRouter uses local reasoning state.
	if (AgentName == TEXT("OpenRouter"))
	{
		TSharedPtr<FChatGatewayClient> ORClient = AgentMgr.GetChatGatewayClient(TEXT("OpenRouter"));
		if (ORClient.IsValid())
		{
			if (!SavedLevel.IsEmpty())
			{
				const bool bEnableSavedReasoning = SavedLevel != TEXT("none");
				ORClient->SetReasoningEnabled(bEnableSavedReasoning);
				if (bEnableSavedReasoning)
				{
					ORClient->SetReasoningEffort(SavedLevel);
				}
			}

			if (!ORClient->IsReasoningEnabled())
			{
				return TEXT("none");
			}

			const FString Effort = ORClient->GetReasoningEffort();
			if (!Effort.IsEmpty())
			{
				return Effort;
			}
		}

		return SavedLevel.IsEmpty() ? TEXT("high") : SavedLevel;
	}

	// Check the specific ACP client for this agent.
	TSharedPtr<FACPClient> Client = AgentMgr.GetClient(AgentName);
	if (Client.IsValid() && Client->SupportsReasoningEffortControl())
	{
		const FString& Effort = Client->GetCurrentReasoningEffort();
		if (!Effort.IsEmpty())
		{
			// Map ACP thinking values to UI reasoning levels
			if (Effort == TEXT("off")) return TEXT("none");
			return Effort;
		}
	}

	return SavedLevel.IsEmpty() ? TEXT("high") : SavedLevel;
}

void UWebUIBridge::SetReasoningLevel(const FString& AgentName, const FString& Level)
{
	if (AgentName.IsEmpty() || Level.IsEmpty()) return;

	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->SaveReasoningForAgent(AgentName, Level);
	}

	// OpenRouter reasoning is configured directly on its client.
	if (AgentName == TEXT("OpenRouter"))
	{
		TSharedPtr<FChatGatewayClient> ORClient = AgentMgr.GetChatGatewayClient(TEXT("OpenRouter"));
		if (!ORClient.IsValid()) return;

		const bool bEnabled = Level != TEXT("none");
		ORClient->SetReasoningEnabled(bEnabled);
		if (bEnabled)
		{
			ORClient->SetReasoningEffort(Level);
		}
		return;
	}

	// ACP client — map UI level to thinking config option value
	FString ThinkingValue = Level == TEXT("none") ? TEXT("off") : Level;
	TSharedPtr<FACPClient> Client = AgentMgr.GetClient(AgentName);
	if (Client.IsValid() && Client->SupportsReasoningEffortControl())
	{
		Client->SetReasoningEffort(ThinkingValue);
	}
}

// ── Mode Selection ──────────────────────────────────────────────────

FString UWebUIBridge::GetModes(const FString& AgentName)
{
	if (AgentName.IsEmpty()) return TEXT("{\"modes\":[],\"currentModeId\":\"\"}");

	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	FACPSessionModeState ModeState = AgentMgr.GetAgentModeState(AgentName);

	TArray<TSharedPtr<FJsonValue>> ModesArray;
	for (const FACPSessionMode& Mode : ModeState.AvailableModes)
	{
		TSharedPtr<FJsonObject> ModeObj = MakeShared<FJsonObject>();
		ModeObj->SetStringField(TEXT("id"), Mode.ModeId);
		ModeObj->SetStringField(TEXT("name"), Mode.Name);
		ModeObj->SetStringField(TEXT("description"), Mode.Description);
		ModesArray.Add(MakeShared<FJsonValueObject>(ModeObj));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("modes"), ModesArray);
	Result->SetStringField(TEXT("currentModeId"), ModeState.CurrentModeId);

	return JsonToString(Result);
}

void UWebUIBridge::SetMode(const FString& AgentName, const FString& ModeId)
{
	if (AgentName.IsEmpty() || ModeId.IsEmpty()) return;
	FACPAgentManager::Get().SetAgentMode(AgentName, ModeId);

	// Persist the selection
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->SaveModeForAgent(AgentName, ModeId);
	}
}

// ── Tool Profiles & Settings ────────────────────────────────────────

/** Convert raw_tool_name to "Raw Tool Name" */
static FString FormatToolDisplayName(const FString& RawName)
{
	FString Result = RawName;
	Result.ReplaceInline(TEXT("_"), TEXT(" "));
	bool bCapitalizeNext = true;
	for (int32 i = 0; i < Result.Len(); ++i)
	{
		if (bCapitalizeNext && FChar::IsAlpha(Result[i]))
		{
			Result[i] = FChar::ToUpper(Result[i]);
			bCapitalizeNext = false;
		}
		else if (Result[i] == ' ')
		{
			bCapitalizeNext = true;
		}
	}
	return Result;
}

/** Hardcoded tool metadata — mirrors SSettingsPanel::BuildToolList */
struct FWebToolMeta
{
	const TCHAR* Description;
	const TCHAR* ExtendedDescription;
	const TCHAR* Category;
};

static const TMap<FString, FWebToolMeta>& GetToolMetadata()
{
	static TMap<FString, FWebToolMeta> Meta;
	if (Meta.Num() == 0)
	{
		Meta.Add(TEXT("execute_python"), { TEXT("Python scripting for UE APIs"), TEXT("Execute Python scripts with access to 1000+ Unreal Engine APIs. Can create assets, modify properties, automate tasks, and interact with the editor."), TEXT("Scripting") });
		Meta.Add(TEXT("read_asset"), { TEXT("Read and inspect assets"), TEXT("Read any asset type with deep introspection. Dedicated readers for Blueprints, Materials, Animations, Niagara, Sequences, and 30+ more types."), TEXT("Asset Reading") });
		Meta.Add(TEXT("edit_blueprint"), { TEXT("Create & Edit Blueprints"), TEXT("Create/edit Blueprints and route Enhanced Input assets via asset_domain='enhanced_input'."), TEXT("Blueprint") });
		Meta.Add(TEXT("edit_graph"), { TEXT("Edit Graph Nodes + Search"), TEXT("Create/connect nodes in Blueprint, Material, PCG, MetaSound, and BehaviorTree graphs. Includes integrated node search via operation=find_nodes/query."), TEXT("Graphs") });
		Meta.Add(TEXT("edit_ai_tree"), { TEXT("Edit AI Trees"), TEXT("Unified AI tree editor for BehaviorTree/Blackboard and StateTree assets."), TEXT("AI") });
		Meta.Add(TEXT("edit_niagara"), { TEXT("Edit Niagara VFX"), TEXT("Modify Niagara particle systems including emitters, modules, and parameters."), TEXT("VFX") });
		Meta.Add(TEXT("edit_sequencer"), { TEXT("Edit Level Sequences"), TEXT("Edit Level Sequences for cinematics. Bind actors, add tracks, and set keyframes."), TEXT("Cinematics") });
		Meta.Add(TEXT("edit_rigging"), { TEXT("Edit Rigging"), TEXT("Unified rigging editor for motion stack (IK Rig/Retargeter/Pose Search) and Control Rig."), TEXT("Animation") });
		Meta.Add(TEXT("edit_animation_asset"), { TEXT("Edit Animation Assets"), TEXT("Unified editor for Animation Montage, AnimSequence, and BlendSpace/AimOffset assets."), TEXT("Animation") });
		Meta.Add(TEXT("edit_character_asset"), { TEXT("Edit Character Assets"), TEXT("Unified editor for Skeleton and PhysicsAsset (ragdoll/collision) workflows."), TEXT("Animation") });
		Meta.Add(TEXT("configure_asset"), { TEXT("Configure asset & node properties"), TEXT("Read and set properties on any asset or graph node using UE reflection."), TEXT("Properties") });
		Meta.Add(TEXT("edit_data_structure"), { TEXT("Edit Structs, Enums, StringTables"), TEXT("Create and modify User Defined Structs, Enums, DataTables, and StringTables."), TEXT("Data") });
		Meta.Add(TEXT("generate_asset"), { TEXT("Generate Assets"), TEXT("Unified generation for images (OpenRouter) and 3D models (Meshy)."), TEXT("Generation") });
		Meta.Add(TEXT("read_logs"), { TEXT("Read editor logs"), TEXT("Read Unreal Engine output logs and Blueprint compilation errors."), TEXT("Debug") });
		Meta.Add(TEXT("screenshot"), { TEXT("Screenshot with camera control"), TEXT("Capture screenshots from viewport or asset editor with full camera control."), TEXT("Visualization") });
		Meta.Add(TEXT("explore"), { TEXT("Explore project content"), TEXT("Browse and list project assets by path, type, or search query."), TEXT("Asset Reading") });
		Meta.Add(TEXT("create_file"), { TEXT("Create files"), TEXT("Create new files in the project directory (scripts, config, text)."), TEXT("Scripting") });
	}
	return Meta;
}

FString UWebUIBridge::GetTools(const FString& ProfileId)
{
	const UACPSettings* Settings = UACPSettings::Get();
	const TMap<FString, FWebToolMeta>& ToolMeta = GetToolMetadata();

	// Determine which profile (if any) we're viewing tools for
	const FAgentProfile* Profile = nullptr;
	if (!ProfileId.IsEmpty() && Settings)
	{
		Profile = Settings->FindProfileById(ProfileId);
	}

	const TMap<FString, FMCPToolDefinition>& MCPTools = FMCPServer::Get().GetRegisteredTools();

	TArray<TSharedPtr<FJsonValue>> ToolsArray;
	for (const auto& Pair : MCPTools)
	{
		FString Description = Pair.Value.Description;
		FString ExtendedDescription;
		FString Category = TEXT("Other");

		if (const FWebToolMeta* Meta = ToolMeta.Find(Pair.Key))
		{
			Description = Meta->Description;
			ExtendedDescription = Meta->ExtendedDescription;
			Category = Meta->Category;
		}

		// Determine enabled state
		bool bEnabled;
		if (Profile)
		{
			bEnabled = Profile->EnabledTools.Num() == 0 || Profile->EnabledTools.Contains(Pair.Key);
		}
		else
		{
			bEnabled = Settings ? Settings->IsToolEnabled(Pair.Key) : true;
		}

		// Check for description override in the profile
		FString DescriptionOverride;
		if (Profile)
		{
			if (const FString* Override = Profile->ToolDescriptionOverrides.Find(Pair.Key))
			{
				DescriptionOverride = *Override;
			}
		}

		TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
		ToolObj->SetStringField(TEXT("name"), Pair.Key);
		ToolObj->SetStringField(TEXT("displayName"), FormatToolDisplayName(Pair.Key));
		ToolObj->SetStringField(TEXT("description"), Description);
		ToolObj->SetStringField(TEXT("extendedDescription"), ExtendedDescription);
		ToolObj->SetStringField(TEXT("category"), Category);
		ToolObj->SetBoolField(TEXT("enabled"), bEnabled);
		ToolObj->SetStringField(TEXT("descriptionOverride"), DescriptionOverride);
		ToolsArray.Add(MakeShared<FJsonValueObject>(ToolObj));
	}

	return JsonArrayToString(ToolsArray);
}

FString UWebUIBridge::GetProfiles()
{
	UACPSettings* Settings = UACPSettings::Get();
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> ProfilesArray;
	if (Settings)
	{
		for (const FAgentProfile& Profile : Settings->Profiles)
		{
			TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
			PObj->SetStringField(TEXT("profileId"), Profile.ProfileId);
			PObj->SetStringField(TEXT("displayName"), Profile.DisplayName);
			PObj->SetStringField(TEXT("description"), Profile.Description);
			PObj->SetBoolField(TEXT("isBuiltIn"), Profile.bIsBuiltIn);
			PObj->SetBoolField(TEXT("isActive"), Settings->ActiveProfileId == Profile.ProfileId);
			PObj->SetNumberField(TEXT("enabledToolCount"), Profile.EnabledTools.Num());
			ProfilesArray.Add(MakeShared<FJsonValueObject>(PObj));
		}
		Result->SetStringField(TEXT("activeProfileId"), Settings->ActiveProfileId);
	}
	Result->SetArrayField(TEXT("profiles"), ProfilesArray);

	return JsonToString(Result);
}

void UWebUIBridge::SetActiveProfile(const FString& ProfileId)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->SetActiveProfile(ProfileId);
	}
}

void UWebUIBridge::SetToolEnabled(const FString& ToolName, bool bEnabled)
{
	if (ToolName.IsEmpty()) return;
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->SetToolEnabled(ToolName, bEnabled);
	}
}

void UWebUIBridge::SetProfileToolEnabled(const FString& ProfileId, const FString& ToolName, bool bEnabled)
{
	if (ProfileId.IsEmpty() || ToolName.IsEmpty()) return;

	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings) return;

	FAgentProfile* Profile = Settings->FindProfileByIdMutable(ProfileId);
	if (!Profile) return;

	if (Profile->EnabledTools.Num() == 0)
	{
		// Transition from "all enabled" to explicit whitelist — populate with all tools first
		const TMap<FString, FMCPToolDefinition>& MCPTools = FMCPServer::Get().GetRegisteredTools();
		for (const auto& Pair : MCPTools)
		{
			Profile->EnabledTools.Add(Pair.Key);
		}
	}

	if (bEnabled)
	{
		Profile->EnabledTools.Add(ToolName);
	}
	else
	{
		Profile->EnabledTools.Remove(ToolName);
	}

	Settings->SavePreferences();
}

FString UWebUIBridge::CreateProfile(const FString& DisplayName, const FString& Description)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	if (DisplayName.IsEmpty())
	{
		Result->SetStringField(TEXT("profileId"), TEXT(""));
		return JsonToString(Result);
	}

	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings)
	{
		Result->SetStringField(TEXT("profileId"), TEXT(""));
		return JsonToString(Result);
	}

	FAgentProfile NewProfile;
	NewProfile.ProfileId = FGuid::NewGuid().ToString();
	NewProfile.DisplayName = DisplayName;
	NewProfile.Description = Description;
	NewProfile.bIsBuiltIn = false;

	Settings->AddCustomProfile(NewProfile);

	Result->SetStringField(TEXT("profileId"), NewProfile.ProfileId);
	return JsonToString(Result);
}

FString UWebUIBridge::DeleteProfile(const FString& ProfileId)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	if (ProfileId.IsEmpty())
	{
		Result->SetBoolField(TEXT("success"), false);
		return JsonToString(Result);
	}

	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings)
	{
		Result->SetBoolField(TEXT("success"), false);
		return JsonToString(Result);
	}

	// Check it's not built-in
	if (const FAgentProfile* Profile = Settings->FindProfileById(ProfileId))
	{
		if (Profile->bIsBuiltIn)
		{
			Result->SetBoolField(TEXT("success"), false);
			return JsonToString(Result);
		}
	}

	Settings->RemoveCustomProfile(ProfileId);
	Result->SetBoolField(TEXT("success"), true);
	return JsonToString(Result);
}

FString UWebUIBridge::GetProfileDetail(const FString& ProfileId)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings || ProfileId.IsEmpty())
	{
		Result->SetBoolField(TEXT("found"), false);
		return JsonToString(Result);
	}

	const FAgentProfile* Profile = Settings->FindProfileById(ProfileId);
	if (!Profile)
	{
		Result->SetBoolField(TEXT("found"), false);
		return JsonToString(Result);
	}

	Result->SetBoolField(TEXT("found"), true);
	Result->SetStringField(TEXT("profileId"), Profile->ProfileId);
	Result->SetStringField(TEXT("displayName"), Profile->DisplayName);
	Result->SetStringField(TEXT("description"), Profile->Description);
	Result->SetBoolField(TEXT("isBuiltIn"), Profile->bIsBuiltIn);
	Result->SetStringField(TEXT("customInstructions"), Profile->CustomInstructions);

	// Tool description overrides
	TSharedPtr<FJsonObject> OverridesObj = MakeShared<FJsonObject>();
	for (const auto& Pair : Profile->ToolDescriptionOverrides)
	{
		OverridesObj->SetStringField(Pair.Key, Pair.Value);
	}
	Result->SetObjectField(TEXT("toolDescriptionOverrides"), OverridesObj);

	return JsonToString(Result);
}

FString UWebUIBridge::UpdateProfile(const FString& ProfileId, const FString& DisplayName, const FString& Description, const FString& CustomInstructions)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings || ProfileId.IsEmpty())
	{
		Result->SetBoolField(TEXT("success"), false);
		return JsonToString(Result);
	}

	FAgentProfile* Profile = Settings->FindProfileByIdMutable(ProfileId);
	if (!Profile)
	{
		Result->SetBoolField(TEXT("success"), false);
		return JsonToString(Result);
	}

	if (!DisplayName.IsEmpty())
	{
		Profile->DisplayName = DisplayName;
	}
	Profile->Description = Description;
	Profile->CustomInstructions = CustomInstructions;
	Settings->SavePreferences();

	Result->SetBoolField(TEXT("success"), true);
	return JsonToString(Result);
}

void UWebUIBridge::SetToolDescriptionOverride(const FString& ProfileId, const FString& ToolName, const FString& DescriptionOverride)
{
	if (ProfileId.IsEmpty() || ToolName.IsEmpty()) return;

	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings) return;

	FAgentProfile* Profile = Settings->FindProfileByIdMutable(ProfileId);
	if (!Profile) return;

	if (DescriptionOverride.IsEmpty())
	{
		Profile->ToolDescriptionOverrides.Remove(ToolName);
	}
	else
	{
		Profile->ToolDescriptionOverrides.Add(ToolName, DescriptionOverride);
	}
	Settings->SavePreferences();
}

// ── Context Mentions ────────────────────────────────────────────────

FString UWebUIBridge::SearchContextItems(const FString& Query)
{
	TArray<TSharedPtr<FJsonValue>> Results;
	const int32 MaxResults = 50;

	// ── Icon cache ──────────────────────────────────────────────────
	static TMap<FName, FString> IconCache;
	auto GetClassIconSVG = [](FName ClassName) -> FString
	{
		if (const FString* Cached = IconCache.Find(ClassName))
		{
			return *Cached;
		}

		const FString SlateDir = FPaths::EngineContentDir() / TEXT("Editor/Slate");
		FString SvgContent;

		FString DirectPath = SlateDir / FString::Printf(TEXT("Starship/AssetIcons/%s_16.svg"), *ClassName.ToString());
		if (FFileHelper::LoadFileToString(SvgContent, *DirectPath))
		{
			IconCache.Add(ClassName, SvgContent);
			return SvgContent;
		}

		UClass* Class = FindFirstObject<UClass>(*ClassName.ToString(), EFindFirstObjectOptions::NativeFirst);
		if (Class)
		{
			for (const UStruct* Super = Class->GetSuperStruct(); Super; Super = Super->GetSuperStruct())
			{
				FString SuperSvgPath = SlateDir / FString::Printf(TEXT("Starship/AssetIcons/%s_16.svg"), *Super->GetName());
				if (FFileHelper::LoadFileToString(SvgContent, *SuperSvgPath))
				{
					IconCache.Add(ClassName, SvgContent);
					return SvgContent;
				}
			}
		}

		FString DefaultPath = SlateDir / TEXT("Starship/AssetIcons/Default_16.svg");
		if (FFileHelper::LoadFileToString(SvgContent, *DefaultPath))
		{
			IconCache.Add(ClassName, SvgContent);
			return SvgContent;
		}

		IconCache.Add(ClassName, FString());
		return FString();
	};

	// ── Helper: add an asset item to results ────────────────────────
	auto AddAssetItem = [&](const FString& Name, const FString& Path, const FName& ClassName)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Name);
		Item->SetStringField(TEXT("path"), Path);
		Item->SetStringField(TEXT("type"), TEXT("asset"));

		FString IconSVG = GetClassIconSVG(ClassName);
		if (!IconSVG.IsEmpty())
		{
			Item->SetStringField(TEXT("icon"), IconSVG);
		}

		Results.Add(MakeShared<FJsonValueObject>(Item));
	};

	auto AddFolderItem = [&](const FString& FolderName, const FString& FolderPath)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), FolderName + TEXT("/"));
		Item->SetStringField(TEXT("path"), FolderPath);
		Item->SetStringField(TEXT("type"), TEXT("folder"));
		Results.Add(MakeShared<FJsonValueObject>(Item));
	};

	auto AddFileItem = [&](const FString& FileName, const FString& RelPath)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), FileName);
		Item->SetStringField(TEXT("path"), RelPath);
		Item->SetStringField(TEXT("type"), TEXT("file"));
		Results.Add(MakeShared<FJsonValueObject>(Item));
	};

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	const UACPSettings* Settings = UACPSettings::Get();
	bool bIncludeEngine = Settings ? Settings->bIncludeEngineContent : false;
	bool bIncludePlugins = Settings ? Settings->bIncludePluginContent : false;

	const bool bIsPathQuery = Query.StartsWith(TEXT("/")) || Query.StartsWith(TEXT("Source/"));
	const bool bIsSearch = !bIsPathQuery && !Query.IsEmpty();

	if (bIsPathQuery)
	{
		// ── BROWSE MODE: list subfolders + assets at a specific path ────
		// Query is like "/Game/Animations" or "Source/UST"
		FString BrowsePath = Query;

		// Trim trailing filter text: "/Game/Animations/Loc" → browse "/Game/Animations", filter "Loc"
		// We detect this by checking if the last segment matches an existing sub-path
		FString ParentPath = BrowsePath;
		FString LeafFilter;

		if (BrowsePath.StartsWith(TEXT("Source")))
		{
			// ── Filesystem browse (C++ source) ──
			FString AbsDir = FPaths::ProjectDir() / BrowsePath;
			if (!FPaths::DirectoryExists(AbsDir))
			{
				// Last segment might be a partial filter
				int32 LastSlash;
				if (BrowsePath.FindLastChar(TEXT('/'), LastSlash) && LastSlash > 0)
				{
					ParentPath = BrowsePath.Left(LastSlash);
					LeafFilter = BrowsePath.Mid(LastSlash + 1).ToLower();
					AbsDir = FPaths::ProjectDir() / ParentPath;
				}
			}

			if (FPaths::DirectoryExists(AbsDir))
			{
				// List subdirectories
				TArray<FString> SubDirs;
				IFileManager::Get().FindFiles(SubDirs, *(AbsDir / TEXT("*")), false, true);
				SubDirs.Sort();
				for (const FString& Dir : SubDirs)
				{
					if (Results.Num() >= MaxResults) break;
					if (!LeafFilter.IsEmpty() && !Dir.ToLower().Contains(LeafFilter)) continue;
					AddFolderItem(Dir, ParentPath / Dir);
				}

				// List files
				TArray<FString> Files;
				IFileManager::Get().FindFiles(Files, *(AbsDir / TEXT("*")), true, false);
				Files.Sort();
				for (const FString& File : Files)
				{
					if (Results.Num() >= MaxResults) break;
					if (!LeafFilter.IsEmpty() && !File.ToLower().Contains(LeafFilter)) continue;
					FString Ext = FPaths::GetExtension(File).ToLower();
					if (Ext == TEXT("h") || Ext == TEXT("cpp") || Ext == TEXT("cs") || Ext == TEXT("build") || Ext == TEXT("target"))
					{
						AddFileItem(File, ParentPath / File);
					}
				}
			}
		}
		else
		{
			// ── Asset Registry browse ──
			// Check if path with trailing part is a valid package path
			TArray<FString> SubPaths;
			AssetRegistry.GetSubPaths(BrowsePath, SubPaths, false);

			if (SubPaths.Num() == 0)
			{
				// Might be a partial filter: "/Game/Animations/Loc" → browse "/Game/Animations", filter "Loc"
				int32 LastSlash;
				if (BrowsePath.FindLastChar(TEXT('/'), LastSlash) && LastSlash > 0)
				{
					ParentPath = BrowsePath.Left(LastSlash);
					LeafFilter = BrowsePath.Mid(LastSlash + 1).ToLower();
					AssetRegistry.GetSubPaths(ParentPath, SubPaths, false);
				}
			}
			else
			{
				ParentPath = BrowsePath;
			}

			// Subfolders
			SubPaths.Sort();
			for (const FString& Sub : SubPaths)
			{
				if (Results.Num() >= MaxResults) break;
				FString FolderName = FPaths::GetCleanFilename(Sub);
				if (!LeafFilter.IsEmpty() && !FolderName.ToLower().Contains(LeafFilter)) continue;
				AddFolderItem(FolderName, Sub);
			}

			// Assets in this folder (non-recursive)
			FARFilter Filter;
			Filter.PackagePaths.Add(FName(*ParentPath));
			Filter.bRecursivePaths = false;

			TArray<FAssetData> FolderAssets;
			AssetRegistry.GetAssets(Filter, FolderAssets);

			FolderAssets.Sort([](const FAssetData& A, const FAssetData& B)
			{
				return A.AssetName.LexicalLess(B.AssetName);
			});

			for (const FAssetData& Asset : FolderAssets)
			{
				if (Results.Num() >= MaxResults) break;
				FString Name = Asset.AssetName.ToString();
				if (!LeafFilter.IsEmpty() && !Name.ToLower().Contains(LeafFilter)) continue;
				FName ClassName = Asset.AssetClassPath.GetAssetName();
				AddAssetItem(Name, Asset.PackageName.ToString(), ClassName);
			}
		}
	}
	else if (bIsSearch)
	{
		// ── SEARCH MODE: filter across all assets + source files by name ──
		const FString LowerQuery = Query.ToLower();

		FARFilter Filter;
		Filter.PackagePaths.Add(FName(TEXT("/Game")));
		if (bIncludeEngine) Filter.PackagePaths.Add(FName(TEXT("/Engine")));
		Filter.bRecursivePaths = true;

		TArray<FAssetData> AllAssets;
		AssetRegistry.GetAssets(Filter, AllAssets);

		if (bIncludePlugins)
		{
			TArray<FAssetData> AllRegistered;
			AssetRegistry.GetAllAssets(AllRegistered, true);
			for (FAssetData& Asset : AllRegistered)
			{
				FString PkgPath = Asset.PackagePath.ToString();
				if (!PkgPath.StartsWith(TEXT("/Game")) && !PkgPath.StartsWith(TEXT("/Engine")))
				{
					AllAssets.Add(MoveTemp(Asset));
				}
			}
		}

		for (const FAssetData& Asset : AllAssets)
		{
			if (Results.Num() >= MaxResults) break;
			FString Name = Asset.AssetName.ToString();
			FString Path = Asset.PackageName.ToString();
			if (!Name.ToLower().Contains(LowerQuery) && !Path.ToLower().Contains(LowerQuery)) continue;
			FName ClassName = Asset.AssetClassPath.GetAssetName();
			AddAssetItem(Name, Path, ClassName);
		}

		// Also search C++ source files
		if (Results.Num() < MaxResults)
		{
			FString SourceDir = FPaths::ProjectDir() / TEXT("Source");
			if (FPaths::DirectoryExists(SourceDir))
			{
				TArray<FString> FoundFiles;
				IFileManager::Get().FindFilesRecursive(FoundFiles, *SourceDir, TEXT("*.h"), true, false);
				IFileManager::Get().FindFilesRecursive(FoundFiles, *SourceDir, TEXT("*.cpp"), true, false);
				for (const FString& FilePath : FoundFiles)
				{
					if (Results.Num() >= MaxResults) break;
					FString FileName = FPaths::GetCleanFilename(FilePath);
					if (!FileName.ToLower().Contains(LowerQuery) && !FilePath.ToLower().Contains(LowerQuery)) continue;
					FString RelativePath = FilePath;
					FPaths::MakePathRelativeTo(RelativePath, *FPaths::ProjectDir());
					AddFileItem(FileName, RelativePath);
				}
			}
		}
	}
	else
	{
		// ── ROOT VIEW: show top-level browsable paths ──
		// Content roots
		AddFolderItem(TEXT("Game"), TEXT("/Game"));
		if (bIncludeEngine) AddFolderItem(TEXT("Engine"), TEXT("/Engine"));

		// Plugin content roots
		if (bIncludePlugins)
		{
			TArray<FString> RootPaths;
			AssetRegistry.GetSubPaths(TEXT("/"), RootPaths, false);
			RootPaths.Sort();
			for (const FString& RootPath : RootPaths)
			{
				if (RootPath == TEXT("/Game") || RootPath == TEXT("/Engine") || RootPath == TEXT("/Script") || RootPath == TEXT("/Temp")) continue;
				FString Name = FPaths::GetCleanFilename(RootPath);
				AddFolderItem(Name, RootPath);
			}
		}

		// Source directory
		FString SourceDir = FPaths::ProjectDir() / TEXT("Source");
		if (FPaths::DirectoryExists(SourceDir))
		{
			AddFolderItem(TEXT("Source"), TEXT("Source"));
		}
	}

	return JsonArrayToString(Results);
}

static FString SanitizeExportFilename(FString Name)
{
	Name.TrimStartAndEndInline();
	if (Name.IsEmpty())
	{
		Name = TEXT("chat-session");
	}

	Name.ReplaceInline(TEXT("/"), TEXT("-"));
	Name.ReplaceInline(TEXT("\\"), TEXT("-"));
	Name.ReplaceInline(TEXT(":"), TEXT("-"));
	Name.ReplaceInline(TEXT("\""), TEXT(""));
	Name.ReplaceInline(TEXT("<"), TEXT(""));
	Name.ReplaceInline(TEXT(">"), TEXT(""));
	Name.ReplaceInline(TEXT("|"), TEXT("-"));
	Name.ReplaceInline(TEXT("*"), TEXT(""));
	Name.ReplaceInline(TEXT("?"), TEXT(""));
	Name.ReplaceInline(TEXT("\n"), TEXT(" "));
	Name.ReplaceInline(TEXT("\r"), TEXT(" "));
	Name.ReplaceInline(TEXT("\t"), TEXT(" "));
	while (Name.ReplaceInline(TEXT("  "), TEXT(" ")) > 0) {}
	Name.TrimStartAndEndInline();

	if (Name.Len() > 64)
	{
		Name = Name.Left(64);
		Name.TrimEndInline();
	}
	if (Name.IsEmpty())
	{
		Name = TEXT("chat-session");
	}
	return Name;
}

static FString BuildSessionMarkdown(const FACPActiveSession& Session)
{
	const FString SessionTitle = Session.Metadata.Title.IsEmpty() ? TEXT("New chat") : Session.Metadata.Title;
	const FString CreatedAt = Session.Metadata.CreatedAt.GetTicks() > 0
		? Session.Metadata.CreatedAt.ToString(TEXT("%Y-%m-%d %H:%M:%S"))
		: TEXT("Unknown");
	const FString LastModifiedAt = Session.Metadata.LastModifiedAt.GetTicks() > 0
		? Session.Metadata.LastModifiedAt.ToString(TEXT("%Y-%m-%d %H:%M:%S"))
		: TEXT("Unknown");

	FString Markdown;
	Markdown.Reserve(32768);
	Markdown += FString::Printf(TEXT("# %s\n\n"), *SessionTitle);
	Markdown += FString::Printf(TEXT("- Agent: `%s`\n"), *Session.Metadata.AgentName);
	Markdown += FString::Printf(TEXT("- Session ID: `%s`\n"), *Session.Metadata.SessionId);
	Markdown += FString::Printf(TEXT("- Created: `%s`\n"), *CreatedAt);
	Markdown += FString::Printf(TEXT("- Last Modified: `%s`\n"), *LastModifiedAt);
	Markdown += FString::Printf(TEXT("- Message Count: `%d`\n\n"), Session.Messages.Num());
	Markdown += TEXT("---\n\n");

	if (Session.Messages.Num() == 0)
	{
		Markdown += TEXT("_No messages in this session._\n");
		return Markdown;
	}

	for (const FACPChatMessage& Message : Session.Messages)
	{
		FString Heading = MessageRoleToSummaryLabel(Message.Role);
		if (Message.Timestamp.GetTicks() > 0)
		{
			Heading += FString::Printf(TEXT(" (%s)"), *Message.Timestamp.ToString(TEXT("%Y-%m-%d %H:%M:%S")));
		}
		Markdown += FString::Printf(TEXT("## %s\n\n"), *Heading);

		for (const FACPContentBlock& Block : Message.ContentBlocks)
		{
			switch (Block.Type)
			{
			case EACPContentBlockType::Text:
				Markdown += Block.Text + TEXT("\n\n");
				break;

			case EACPContentBlockType::Thought:
				Markdown += TEXT("<details>\n<summary>Thinking</summary>\n\n");
				Markdown += Block.Text + TEXT("\n\n");
				Markdown += TEXT("</details>\n\n");
				break;

			case EACPContentBlockType::ToolCall:
				Markdown += FString::Printf(TEXT("### Tool Call: %s\n\n"), Block.ToolName.IsEmpty() ? TEXT("tool") : *Block.ToolName);
				if (!Block.ToolArguments.IsEmpty())
				{
					Markdown += TEXT("```json\n");
					Markdown += Block.ToolArguments;
					Markdown += TEXT("\n```\n\n");
				}
				break;

			case EACPContentBlockType::ToolResult:
			{
				const TCHAR* ResultStatus = Block.bToolSuccess ? TEXT("Success") : TEXT("Error");
				Markdown += FString::Printf(TEXT("### Tool Result (%s)\n\n"), ResultStatus);
				FString ToolResult = Block.ToolResultContent;
				const int32 MaxResultChars = 120000;
				bool bTruncated = false;
				if (ToolResult.Len() > MaxResultChars)
				{
					ToolResult = ToolResult.Left(MaxResultChars);
					bTruncated = true;
				}
				if (!ToolResult.IsEmpty())
				{
					Markdown += TEXT("```\n");
					Markdown += ToolResult;
					Markdown += TEXT("\n```\n\n");
				}
				if (Block.ToolResultImages.Num() > 0)
				{
					Markdown += FString::Printf(TEXT("- Images: %d\n\n"), Block.ToolResultImages.Num());
				}
				if (bTruncated)
				{
					Markdown += TEXT("_Tool result truncated for export size limits._\n\n");
				}
				break;
			}

			case EACPContentBlockType::Error:
				Markdown += FString::Printf(TEXT("> **Error:** %s\n\n"), *Block.Text);
				break;

			case EACPContentBlockType::System:
				Markdown += FString::Printf(TEXT("> **System:** %s\n\n"), *Block.Text);
				break;

			case EACPContentBlockType::Image:
				Markdown += TEXT("_Image block omitted from markdown export._\n\n");
				break;

			default:
				break;
			}
		}
	}

	return Markdown;
}

// ── Session Management ──────────────────────────────────────────────

FString UWebUIBridge::RenameSession(const FString& SessionId, const FString& NewTitle)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	FString TrimmedTitle = NewTitle.TrimStartAndEnd();

	if (TrimmedTitle.IsEmpty())
	{
		Result->SetBoolField(TEXT("success"), false);
		return JsonToString(Result);
	}

	FACPSessionManager& SessionMgr = FACPSessionManager::Get();
	SessionMgr.SetCustomTitle(SessionId, TrimmedTitle);
	Result->SetBoolField(TEXT("success"), true);
	return JsonToString(Result);
}

FString UWebUIBridge::DeleteSession(const FString& SessionId)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	if (SessionId.IsEmpty())
	{
		Result->SetBoolField(TEXT("success"), false);
		return JsonToString(Result);
	}

	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	FACPSessionManager& SessionMgr = FACPSessionManager::Get();

	// Find the agent that owns this session and resolve the agent's native session ID.
	// The SessionId from JS may be a Unreal GUID (for sessions created through the UI)
	// or the agent's native session ID (for sessions only listed from the remote list).
	// The JSONL file on disk is named after the agent's native ID, not the Unreal GUID.
	FString AgentName = AgentMgr.GetSessionAgent(SessionId);
	FString AgentSessionId;

	// Get the agent's native session ID from the active session metadata (before closing it)
	const FACPActiveSession* ActiveSession = SessionMgr.GetActiveSession(SessionId);
	if (ActiveSession)
	{
		AgentSessionId = ActiveSession->Metadata.AgentSessionId;
	}

	// Also try to grab the session's cwd from the cached remote list
	FString SessionCwd;

	{
		// Search cached session lists — always, to grab Cwd even when AgentName is already known
		FString LookupId = AgentSessionId.IsEmpty() ? SessionId : AgentSessionId;
		for (const FString& Name : AgentMgr.GetAvailableAgentNames())
		{
			TArray<FACPRemoteSessionEntry> Sessions = AgentMgr.GetCachedSessionList(Name);
			for (const FACPRemoteSessionEntry& Entry : Sessions)
			{
				if (Entry.SessionId == LookupId || Entry.SessionId == SessionId)
				{
					if (AgentName.IsEmpty()) AgentName = Name;
					if (SessionCwd.IsEmpty()) SessionCwd = Entry.Cwd;
					break;
				}
			}
			if (!AgentName.IsEmpty() && !SessionCwd.IsEmpty()) break;
		}
	}

	// The ID to use for file deletion: prefer the agent's native ID, fall back to SessionId
	// (which is already the native ID for sessions that were never opened through the UI)
	FString FileSessionId = AgentSessionId.IsEmpty() ? SessionId : AgentSessionId;

	// Resolve the working directory for file operations:
	// 1. Cwd from the remote session entry (most accurate — recorded in the session file itself)
	// 2. Agent's configured WorkingDirectory
	// 3. FPaths::ProjectDir() as last resort
	FString WorkingDir = SessionCwd;
	if (WorkingDir.IsEmpty())
	{
		if (FACPAgentConfig* Config = AgentMgr.GetAgentConfig(AgentName))
		{
			WorkingDir = Config->WorkingDirectory;
		}
	}
	if (WorkingDir.IsEmpty())
	{
		WorkingDir = FPaths::ProjectDir();
	}

	// Close the active session
	SessionMgr.CloseSession(SessionId);
	AgentMgr.UnregisterSession(SessionId);

	// Try ACP session/delete if the agent supports it (Zed parity)
	if (!AgentName.IsEmpty() && !FileSessionId.IsEmpty())
	{
		AgentMgr.DeleteRemoteSession(AgentName, FileSessionId);
	}

	// Delete the agent's native session file on disk.
	if (!AgentName.IsEmpty())
	{
		if (AgentName.Equals(TEXT("Claude Code"), ESearchCase::IgnoreCase))
		{
			FString SessionFilePath = FACPClaudeCodeHistoryReader::GetSessionJsonlPath(FileSessionId, WorkingDir);
			if (FPaths::FileExists(SessionFilePath))
			{
				IFileManager::Get().Delete(*SessionFilePath);
				UE_LOG(LogAgentIntegrationKit, Log, TEXT("WebUIBridge: Deleted Claude Code session file: %s"), *SessionFilePath);
			}
			else
			{
				UE_LOG(LogAgentIntegrationKit, Warning, TEXT("WebUIBridge: Claude Code session file not found for deletion: %s"), *SessionFilePath);
			}

			// Also delete the session directory (contains subagent data)
			FString SessionDirPath = FPaths::GetPath(SessionFilePath) / FileSessionId;
			if (FPaths::DirectoryExists(SessionDirPath))
			{
				IFileManager::Get().DeleteDirectory(*SessionDirPath, false, true);
				UE_LOG(LogAgentIntegrationKit, Log, TEXT("WebUIBridge: Deleted Claude Code session directory: %s"), *SessionDirPath);
			}
		}
		else if (AgentName.Equals(TEXT("Codex CLI"), ESearchCase::IgnoreCase))
		{
			// Codex stores sessions as: ~/.codex/sessions/[YYYY/MM/DD/]rollout-YYYY-MM-DDThh-mm-ss-<session-uuid>.jsonl
			// The session UUID is the last component of the filename before .jsonl
			FString CodexHome = FPaths::Combine(FPlatformProcess::UserHomeDir(), TEXT(".codex"));
			FString SessionsDir = FPaths::Combine(CodexHome, TEXT("sessions"));

			if (FPaths::DirectoryExists(SessionsDir))
			{
				// Search recursively for the file containing the session UUID
				TArray<FString> FoundFiles;
				FString SearchPattern = FString::Printf(TEXT("*-%s.jsonl"), *FileSessionId);
				IFileManager::Get().FindFilesRecursive(FoundFiles, *SessionsDir, *SearchPattern, true, false);

				if (FoundFiles.Num() > 0)
				{
					IFileManager::Get().Delete(*FoundFiles[0]);
					UE_LOG(LogAgentIntegrationKit, Log, TEXT("WebUIBridge: Deleted Codex session file: %s"), *FoundFiles[0]);
				}
				else
				{
					UE_LOG(LogAgentIntegrationKit, Warning, TEXT("WebUIBridge: Codex session file not found for deletion (searched %s for *-%s.jsonl)"), *SessionsDir, *FileSessionId);
				}
			}

			// Also check archived sessions
			FString ArchivedDir = FPaths::Combine(CodexHome, TEXT("archived_sessions"));
			if (FPaths::DirectoryExists(ArchivedDir))
			{
				TArray<FString> ArchivedFiles;
				FString SearchPattern = FString::Printf(TEXT("*-%s.jsonl"), *FileSessionId);
				IFileManager::Get().FindFilesRecursive(ArchivedFiles, *ArchivedDir, *SearchPattern, true, false);

				if (ArchivedFiles.Num() > 0)
				{
					IFileManager::Get().Delete(*ArchivedFiles[0]);
					UE_LOG(LogAgentIntegrationKit, Log, TEXT("WebUIBridge: Deleted archived Codex session file: %s"), *ArchivedFiles[0]);
				}
			}
		}

		// Refresh the cached session list so the deleted session is removed
		AgentMgr.RequestSessionList(AgentName);
	}

	Result->SetBoolField(TEXT("success"), true);
	return JsonToString(Result);
}

FString UWebUIBridge::ExportSessionToMarkdown(const FString& SessionId)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), false);
	Result->SetBoolField(TEXT("canceled"), false);

	if (SessionId.IsEmpty())
	{
		Result->SetStringField(TEXT("error"), TEXT("Empty session ID"));
		return JsonToString(Result);
	}

	FACPSessionManager& SessionMgr = FACPSessionManager::Get();
	const FACPActiveSession* Session = SessionMgr.GetActiveSession(SessionId);
	if (!Session)
	{
		Result->SetStringField(
			TEXT("error"),
			TEXT("Session is not loaded in memory. Open the chat once to load ACP history, then export.")
		);
		return JsonToString(Result);
	}

	if (Session->Messages.Num() == 0 && Session->bIsLoadingHistory)
	{
		Result->SetStringField(
			TEXT("error"),
			TEXT("Session history is still loading from ACP. Wait a moment and try export again.")
		);
		return JsonToString(Result);
	}

	const FString Markdown = BuildSessionMarkdown(*Session);

	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		Result->SetStringField(TEXT("error"), TEXT("Desktop platform unavailable"));
		return JsonToString(Result);
	}

	const void* ParentWindowHandle = FSlateApplication::Get().GetActiveTopLevelWindow().IsValid()
		? FSlateApplication::Get().GetActiveTopLevelWindow()->GetNativeWindow()->GetOSWindowHandle()
		: nullptr;

	const FString DefaultFileName = SanitizeExportFilename(Session->Metadata.Title) + TEXT(".md");
	TArray<FString> SaveFilenames;
	const bool bDialogAccepted = DesktopPlatform->SaveFileDialog(
		ParentWindowHandle,
		TEXT("Export Conversation"),
		FPaths::ProjectSavedDir(),
		DefaultFileName,
		TEXT("Markdown Files (*.md)|*.md"),
		0,
		SaveFilenames
	);

	if (!bDialogAccepted || SaveFilenames.Num() == 0)
	{
		Result->SetBoolField(TEXT("canceled"), true);
		return JsonToString(Result);
	}

	const FString& SavePath = SaveFilenames[0];
	const bool bSaved = FFileHelper::SaveStringToFile(
		Markdown,
		*SavePath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM
	);
	if (!bSaved)
	{
		Result->SetStringField(TEXT("error"), TEXT("Failed to write markdown file"));
		return JsonToString(Result);
	}

	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("savedPath"), SavePath);
	return JsonToString(Result);
}

// ── Project Indexing ────────────────────────────────────────────────

FString UWebUIBridge::GetIndexingSettings()
{
	return FProjectIndexManager::Get().GetSettingsJson();
}

FString UWebUIBridge::GetIndexingStatus()
{
	return FProjectIndexManager::Get().GetStatusJson();
}

void UWebUIBridge::SetIndexingProvider(const FString& Provider)
{
	FProjectIndexManager::Get().SetProvider(Provider);
}

void UWebUIBridge::SetIndexingEndpointUrl(const FString& Url)
{
	FProjectIndexManager::Get().SetEndpointUrl(Url);
}

void UWebUIBridge::SetIndexingApiKey(const FString& Key)
{
	FProjectIndexManager::Get().SetApiKey(Key);
}

void UWebUIBridge::SetIndexingModel(const FString& Model)
{
	FProjectIndexManager::Get().SetModel(Model);
}

void UWebUIBridge::SetIndexingDimensions(int32 Dims)
{
	FProjectIndexManager::Get().SetDimensions(Dims);
}

void UWebUIBridge::SetAutoIndex(bool bEnabled)
{
	FProjectIndexManager::Get().SetAutoIndex(bEnabled);
}

void UWebUIBridge::SetIndexingScopeEnabled(const FString& ScopeKey, bool bEnabled)
{
	FProjectIndexManager::Get().SetScopeEnabled(ScopeKey, bEnabled);
}

void UWebUIBridge::StartIndexing()
{
	FProjectIndexManager::Get().StartIndexing();
}

void UWebUIBridge::ClearIndex()
{
	FProjectIndexManager::Get().ClearIndex();
}

// ── Studio / Generative Providers ──────────────────────────────────

static FString StatusToString(EGenerativeJobStatus Status)
{
	switch (Status)
	{
	case EGenerativeJobStatus::Pending:   return TEXT("pending");
	case EGenerativeJobStatus::Running:   return TEXT("running");
	case EGenerativeJobStatus::Succeeded: return TEXT("succeeded");
	case EGenerativeJobStatus::Failed:    return TEXT("failed");
	case EGenerativeJobStatus::Cancelled: return TEXT("cancelled");
	default:                              return TEXT("unknown");
	}
}

static TSharedRef<FJsonObject> JobToJson(const FGenerativeJob& Job)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("providerId"), Job.ProviderId);
	Obj->SetStringField(TEXT("actionId"), Job.ActionId);
	Obj->SetStringField(TEXT("jobId"), Job.JobId);
	Obj->SetStringField(TEXT("status"), StatusToString(Job.Status));
	Obj->SetNumberField(TEXT("progress"), Job.Progress);
	Obj->SetStringField(TEXT("resultUrl"), Job.ResultUrl);
	Obj->SetStringField(TEXT("thumbnailUrl"), Job.ThumbnailUrl);
	Obj->SetStringField(TEXT("error"), Job.ErrorMessage);

	// Extra URLs (format → url)
	TSharedRef<FJsonObject> ExtrasObj = MakeShared<FJsonObject>();
	for (const auto& Pair : Job.ExtraUrls)
	{
		ExtrasObj->SetStringField(Pair.Key, Pair.Value);
	}
	Obj->SetObjectField(TEXT("extraUrls"), ExtrasObj);

	// Image URLs
	TArray<TSharedPtr<FJsonValue>> ImagesArr;
	for (const FString& Url : Job.ImageUrls)
	{
		ImagesArr.Add(MakeShared<FJsonValueString>(Url));
	}
	Obj->SetArrayField(TEXT("imageUrls"), ImagesArr);

	return Obj;
}

FString UWebUIBridge::GetGenerativeProviders()
{
	auto& Registry = FGenerativeProviderRegistry::Get();
	TArray<TSharedPtr<FJsonValue>> ProvidersArr;

	for (const auto& Provider : Registry.GetAll())
	{
		TSharedRef<FJsonObject> ProvObj = MakeShared<FJsonObject>();
		ProvObj->SetStringField(TEXT("id"), Provider->GetId());
		ProvObj->SetStringField(TEXT("displayName"), Provider->GetDisplayName());
		ProvObj->SetStringField(TEXT("website"), Provider->GetWebsite());

		// Actions
		TArray<TSharedPtr<FJsonValue>> ActionsArr;
		for (const auto& Action : Provider->GetActions())
		{
			TSharedRef<FJsonObject> ActObj = MakeShared<FJsonObject>();
			ActObj->SetStringField(TEXT("actionId"), Action.ActionId);
			ActObj->SetStringField(TEXT("description"), Action.Description);
			ActObj->SetStringField(TEXT("creditCost"), Action.CreditCost);
			ActObj->SetBoolField(TEXT("isSynchronous"), Action.bIsSynchronous);

			// Input hints
			TArray<TSharedPtr<FJsonValue>> InHints;
			for (const FString& H : Action.InputHints) InHints.Add(MakeShared<FJsonValueString>(H));
			ActObj->SetArrayField(TEXT("inputHints"), InHints);

			// Output hints
			TArray<TSharedPtr<FJsonValue>> OutHints;
			for (const FString& H : Action.OutputHints) OutHints.Add(MakeShared<FJsonValueString>(H));
			ActObj->SetArrayField(TEXT("outputHints"), OutHints);

			// Schema (already a FJsonObject)
			if (Action.ParamsSchema.IsValid())
			{
				ActObj->SetObjectField(TEXT("paramsSchema"), Action.ParamsSchema);
			}

			ActionsArr.Add(MakeShared<FJsonValueObject>(ActObj));
		}
		ProvObj->SetArrayField(TEXT("actions"), ActionsArr);

		ProvidersArr.Add(MakeShared<FJsonValueObject>(ProvObj));
	}

	return JsonArrayToString(ProvidersArr);
}

FString UWebUIBridge::SubmitGenerativeJob(const FString& ProviderId, const FString& ActionId, const FString& ParamsJson)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	auto Provider = FGenerativeProviderRegistry::Get().Find(ProviderId);
	if (!Provider.IsValid())
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Provider '%s' not found"), *ProviderId));
		return JsonToString(Result);
	}

	// Parse params JSON
	TSharedPtr<FJsonObject> Params;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ParamsJson);
	if (!FJsonSerializer::Deserialize(Reader, Params) || !Params.IsValid())
	{
		Params = MakeShared<FJsonObject>();
	}

	FGenerativeJob Job = Provider->Submit(ActionId, Params);
	Job.ProviderId = ProviderId;
	Job.ActionId = ActionId;

	Result->SetBoolField(TEXT("success"), Job.Status != EGenerativeJobStatus::Failed);
	Result->SetObjectField(TEXT("job"), JobToJson(Job));

	return JsonToString(Result);
}

FString UWebUIBridge::CheckGenerativeJobStatus(const FString& ProviderId, const FString& JobId, const FString& ActionId)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	auto Provider = FGenerativeProviderRegistry::Get().Find(ProviderId);
	if (!Provider.IsValid())
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Provider '%s' not found"), *ProviderId));
		return JsonToString(Result);
	}

	FGenerativeJob Job = Provider->CheckStatus(JobId, ActionId);
	Job.ProviderId = ProviderId;
	Job.ActionId = ActionId;

	Result->SetBoolField(TEXT("success"), true);
	Result->SetObjectField(TEXT("job"), JobToJson(Job));

	return JsonToString(Result);
}

FString UWebUIBridge::GetGenerativeBalance(const FString& ProviderId)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	auto Provider = FGenerativeProviderRegistry::Get().Find(ProviderId);
	if (!Provider.IsValid())
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Provider '%s' not found"), *ProviderId));
		return JsonToString(Result);
	}

	int32 Balance = Provider->GetBalance();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("balance"), Balance);

	return JsonToString(Result);
}

// ── Terminal ────────────────────────────────────────────────────────

FString UWebUIBridge::StartTerminal(const FString& WorkingDir, const FString& Shell)
{
	FString EffectiveDir = WorkingDir;
	if (EffectiveDir.IsEmpty())
	{
		EffectiveDir = FPaths::ProjectDir();
	}

	FString TerminalId = FTerminalManager::Get().StartTerminal(EffectiveDir, Shell);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	if (TerminalId.IsEmpty())
	{
		Result->SetStringField(TEXT("error"), TEXT("Failed to create terminal session"));
	}
	else
	{
		Result->SetStringField(TEXT("terminalId"), TerminalId);
	}
	return JsonToString(Result);
}

void UWebUIBridge::WriteTerminal(const FString& TerminalId, const FString& Data)
{
	if (TerminalId.IsEmpty() || Data.IsEmpty()) return;
	FTerminalManager::Get().WriteTerminal(TerminalId, Data);
}

void UWebUIBridge::ResizeTerminal(const FString& TerminalId, int32 Cols, int32 Rows)
{
	if (TerminalId.IsEmpty() || Cols <= 0 || Rows <= 0) return;
	FTerminalManager::Get().ResizeTerminal(TerminalId, Cols, Rows);
}

void UWebUIBridge::CloseTerminal(const FString& TerminalId)
{
	if (TerminalId.IsEmpty()) return;
	FTerminalManager::Get().CloseTerminal(TerminalId);
}

void UWebUIBridge::BindOnTerminalOutput(FWebJSFunction Callback)
{
	OnTerminalOutputCallback = Callback;

	if (!TerminalOutputHandle.IsValid())
	{
		TWeakObjectPtr<UWebUIBridge> WeakThis(this);
		TerminalOutputHandle = FTerminalManager::Get().OnTerminalOutput.AddLambda(
			[WeakThis](const FString& TerminalId, const FString& Base64Data)
			{
				AsyncTask(ENamedThreads::GameThread, [WeakThis, TerminalId, Base64Data]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnTerminalOutputCallback.IsValid())
						{
							Self->OnTerminalOutputCallback(TerminalId, Base64Data);
						}
					}
				});
			});
	}
}

void UWebUIBridge::BindOnTerminalExit(FWebJSFunction Callback)
{
	OnTerminalExitCallback = Callback;

	if (!TerminalExitHandle.IsValid())
	{
		TWeakObjectPtr<UWebUIBridge> WeakThis(this);
		TerminalExitHandle = FTerminalManager::Get().OnTerminalExit.AddLambda(
			[WeakThis](const FString& TerminalId, int32 ExitCode)
			{
				AsyncTask(ENamedThreads::GameThread, [WeakThis, TerminalId, ExitCode]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnTerminalExitCallback.IsValid())
						{
							Self->OnTerminalExitCallback(TerminalId, ExitCode);
						}
					}
				});
			});
	}
}

// ── Source Control ──────────────────────────────────────────────────

FString UWebUIBridge::GetSourceControlStatus()
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

#if ENGINE_MINOR_VERSION >= 6
	ISourceControlModule* SCModule = ISourceControlModule::GetPtr();
#else
	ISourceControlModule* SCModule = FModuleManager::GetModulePtr<ISourceControlModule>("SourceControl");
#endif
	if (!SCModule || !SCModule->IsEnabled())
	{
		Result->SetBoolField(TEXT("enabled"), false);
		Result->SetStringField(TEXT("provider"), TEXT(""));
		Result->SetStringField(TEXT("branch"), TEXT(""));
		Result->SetNumberField(TEXT("changesCount"), -1);
		Result->SetBoolField(TEXT("connected"), false);
		return JsonToString(Result);
	}

	ISourceControlProvider& Provider = SCModule->GetProvider();
	Result->SetBoolField(TEXT("enabled"), true);
	Result->SetStringField(TEXT("provider"), Provider.GetName().ToString());
	Result->SetBoolField(TEXT("connected"), Provider.IsAvailable());

	TMap<ISourceControlProvider::EStatus, FString> Status = Provider.GetStatus();
	FString* Branch = Status.Find(ISourceControlProvider::EStatus::Branch);
	Result->SetStringField(TEXT("branch"), Branch ? *Branch : TEXT(""));

	TOptional<int> NumChanges = Provider.GetNumLocalChanges();
	Result->SetNumberField(TEXT("changesCount"), NumChanges.IsSet() ? NumChanges.GetValue() : -1);

	return JsonToString(Result);
}

void UWebUIBridge::OpenSourceControlChangelist()
{
	ISourceControlWindowsModule& SCWindows = ISourceControlWindowsModule::Get();
	if (SCWindows.CanShowChangelistsTab())
	{
		SCWindows.ShowChangelistsTab();
	}
}

void UWebUIBridge::OpenSourceControlSubmit()
{
	FSourceControlWindows::ChoosePackagesToCheckIn();
}

// ── Agent Setup ─────────────────────────────────────────────────────

FString UWebUIBridge::GetAgentInstallInfo(const FString& AgentName)
{
	// Legacy — agent info is now provided by the ACP registry
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("agentName"), AgentName);
	return JsonToString(Result);
}

void UWebUIBridge::InstallAgent(const FString& AgentName)
{
	// Legacy — agent installation is now handled via the ACP registry
	UE_LOG(LogTemp, Warning, TEXT("WebUIBridge::InstallAgent called for '%s' — use registry-based install instead"), *AgentName);
}

FString UWebUIBridge::RefreshAgentStatus(const FString& AgentName)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	if (AgentName.IsEmpty())
	{
		Result->SetStringField(TEXT("status"), TEXT("unknown"));
		Result->SetStringField(TEXT("statusMessage"), TEXT("Empty agent name"));
		return JsonToString(Result);
	}

	// Invalidate cache and re-evaluate
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->InvalidateAgentStatusCache();
	}

	// Re-fetch configs (this triggers re-evaluation of all agents)
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	TArray<FACPAgentConfig> Configs = AgentMgr.GetAllAgentConfigs();

	// Find the requested agent
	for (const FACPAgentConfig& Config : Configs)
	{
		if (Config.AgentName == AgentName)
		{
			FString StatusStr;
			switch (Config.Status)
			{
			case EACPAgentStatus::Available:     StatusStr = TEXT("available"); break;
			case EACPAgentStatus::NotInstalled:  StatusStr = TEXT("not_installed"); break;
			case EACPAgentStatus::MissingApiKey: StatusStr = TEXT("missing_key"); break;
			default:                             StatusStr = TEXT("unknown"); break;
			}
			Result->SetStringField(TEXT("status"), StatusStr);
			Result->SetStringField(TEXT("statusMessage"), Config.StatusMessage);
			return JsonToString(Result);
		}
	}

	Result->SetStringField(TEXT("status"), TEXT("unknown"));
	Result->SetStringField(TEXT("statusMessage"), TEXT("Agent not found"));
	return JsonToString(Result);
}

FString UWebUIBridge::GetRegistryAgents()
{
	const FACPRegistryClient& Registry = FACPRegistryClient::Get();
	const TArray<FACPRegistryAgent>& Agents = Registry.GetAgents();
	const FString PlatformKey = FACPRegistryClient::GetCurrentPlatformKey();

	TArray<TSharedPtr<FJsonValue>> AgentsArr;
	AgentsArr.Reserve(Agents.Num());

	for (const FACPRegistryAgent& Agent : Agents)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), Agent.Id);
		Obj->SetStringField(TEXT("name"), Agent.Name);
		Obj->SetStringField(TEXT("version"), Agent.Version);
		Obj->SetStringField(TEXT("description"), Agent.Description);
		Obj->SetStringField(TEXT("license"), Agent.License);
		Obj->SetStringField(TEXT("icon"), Agent.IconUrl);
		Obj->SetStringField(TEXT("repository"), Agent.Repository);

		// Authors
		TArray<TSharedPtr<FJsonValue>> AuthorsArr;
		for (const FString& Author : Agent.Authors)
		{
			AuthorsArr.Add(MakeShared<FJsonValueString>(Author));
		}
		Obj->SetArrayField(TEXT("authors"), AuthorsArr);

		// Distribution availability
		Obj->SetBoolField(TEXT("hasBinary"), Agent.Distribution.HasBinaryForPlatform(PlatformKey));
		Obj->SetBoolField(TEXT("hasNpx"), Agent.Distribution.HasNpx());
		Obj->SetBoolField(TEXT("hasUvx"), Agent.Distribution.HasUvx());

		if (Agent.Distribution.HasNpx())
		{
			Obj->SetStringField(TEXT("npxPackage"), Agent.Distribution.NpxPackage);
		}
		if (Agent.Distribution.HasUvx())
		{
			Obj->SetStringField(TEXT("uvxPackage"), Agent.Distribution.UvxPackage);
		}

		// Install status — check if the agent is in the user's InstalledAgentIds list
		const UACPSettings* Settings = UACPSettings::Get();
		bool bIsInstalled = Settings && Settings->InstalledAgentIds.Contains(Agent.Id);
		Obj->SetBoolField(TEXT("isInstalled"), bIsInstalled);

		// Update availability
		const TArray<FAgentUpdateInfo>& Updates = Registry.GetAgentUpdates();
		const FAgentUpdateInfo* UpdateInfo = Updates.FindByPredicate(
			[&Agent](const FAgentUpdateInfo& U) { return U.AgentId == Agent.Id; });
		if (UpdateInfo)
		{
			Obj->SetBoolField(TEXT("updateAvailable"), true);
			Obj->SetStringField(TEXT("installedVersion"), UpdateInfo->InstalledVersion);
			Obj->SetStringField(TEXT("latestVersion"), UpdateInfo->LatestVersion);
		}

		AgentsArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	return JsonArrayToString(AgentsArr);
}

void UWebUIBridge::RefreshRegistry()
{
	FACPRegistryClient::Get().RefreshAsync();
}

FString UWebUIBridge::GetAgentUpdates()
{
	const FACPRegistryClient& Registry = FACPRegistryClient::Get();
	const TArray<FAgentUpdateInfo>& Updates = Registry.GetAgentUpdates();

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FAgentUpdateInfo& Update : Updates)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("agentId"), Update.AgentId);
		Obj->SetStringField(TEXT("agentName"), Update.AgentName);
		Obj->SetStringField(TEXT("installedVersion"), Update.InstalledVersion);
		Obj->SetStringField(TEXT("latestVersion"), Update.LatestVersion);
		Obj->SetBoolField(TEXT("isNpx"), Update.bIsNpx);
		Arr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	return JsonArrayToString(Arr);
}

void UWebUIBridge::UpdateRegistryAgent(const FString& AgentId)
{
	// For binary agents: delete old version directory, re-download on next connect
	const FACPRegistryAgent* Agent = FACPRegistryClient::Get().FindAgent(AgentId);
	if (!Agent)
	{
		return;
	}

	const FString PlatformKey = FACPRegistryClient::GetCurrentPlatformKey();
	if (Agent->Distribution.HasBinaryForPlatform(PlatformKey))
	{
		// Uninstall old binary (clears managed directory)
		FAgentInstaller::Get().UninstallRegistryAgent(AgentId);

		// Re-initialize configs — next connect will trigger lazy download of new version
		FACPAgentManager::Get().InitializeDefaultAgents();

		UE_LOG(LogAgentIntegrationKit, Log, TEXT("WebUI: Agent '%s' update triggered — old binary removed, will download v%s on next use"),
			*AgentId, *Agent->Version);
	}
}

void UWebUIBridge::InstallRegistryAgent(const FString& AgentId, const FString& Method)
{
	// Zed model: "Install" = add agent ID to settings. No download, no process spawn.
	// Process spawns lazily when the user first opens a chat with this agent.
	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings)
	{
		return;
	}

	if (!Settings->InstalledAgentIds.Contains(AgentId))
	{
		Settings->InstalledAgentIds.Add(AgentId);
		Settings->SaveInstalledAgentIds();
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("WebUI: Installed agent '%s' (added to settings, total: %d)"), *AgentId, Settings->InstalledAgentIds.Num());

		// Reinitialize agent configs so the sidebar picks up the new agent
		FACPAgentManager::Get().InitializeDefaultAgents();
	}
	else
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("WebUI: Agent '%s' already installed"), *AgentId);
	}
}

void UWebUIBridge::UninstallRegistryAgent(const FString& AgentId)
{
	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings)
	{
		return;
	}

	if (Settings->InstalledAgentIds.Remove(AgentId) > 0)
	{
		Settings->SaveInstalledAgentIds();
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("WebUI: Uninstalled agent '%s' (removed from settings)"), *AgentId);

		// Disconnect if connected
		FACPAgentManager::Get().DisconnectFromAgent(AgentId);

		// Reinitialize
		FACPAgentManager::Get().InitializeDefaultAgents();
	}
}

FString UWebUIBridge::GetPrerequisiteStatus()
{
	FAgentInstaller& Installer = FAgentInstaller::Get();
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	auto CheckTool = [&](const FString& Name, const FString& ExecutableName, const FString& VersionFlag)
	{
		TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
		FString ResolvedPath;
		bool bFound = Installer.ResolveExecutable(ExecutableName, ResolvedPath)
			|| Installer.ResolveExecutableViaLoginShell(ExecutableName, ResolvedPath);

		ToolObj->SetBoolField(TEXT("found"), bFound);
		ToolObj->SetStringField(TEXT("path"), bFound ? ResolvedPath : TEXT(""));

		// Try to get version
		if (bFound)
		{
			FString StdOut, StdErr;
			int32 ReturnCode = -1;
			FPlatformProcess::ExecProcess(*ResolvedPath, *VersionFlag, &ReturnCode, &StdOut, &StdErr);
			if (ReturnCode == 0)
			{
				FString Version = StdOut.TrimStartAndEnd();
				// Take first line only
				int32 NewlineIdx;
				if (Version.FindChar(TEXT('\n'), NewlineIdx))
				{
					Version = Version.Left(NewlineIdx).TrimStartAndEnd();
				}
				ToolObj->SetStringField(TEXT("version"), Version);
			}
		}

		Result->SetObjectField(Name, ToolObj);
	};

	CheckTool(TEXT("node"), TEXT("node"), TEXT("--version"));
	CheckTool(TEXT("npm"), TEXT("npm"), TEXT("--version"));
	CheckTool(TEXT("npx"), TEXT("npx"), TEXT("--version"));
	CheckTool(TEXT("git"), TEXT("git"), TEXT("--version"));
	CheckTool(TEXT("uv"), TEXT("uv"), TEXT("--version"));
	CheckTool(TEXT("uvx"), TEXT("uvx"), TEXT("--version"));
	CheckTool(TEXT("bun"), TEXT("bun"), TEXT("--version"));

	return JsonToString(Result);
}

void UWebUIBridge::CopyToClipboard(const FString& Text)
{
	FPlatformApplicationMisc::ClipboardCopy(*Text);
}

FString UWebUIBridge::GetClipboardText()
{
	FString ClipboardContent;
	FPlatformApplicationMisc::ClipboardPaste(ClipboardContent);
	return ClipboardContent;
}

void UWebUIBridge::OpenUrl(const FString& Url)
{
	if (!Url.IsEmpty())
	{
		FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
	}
}

void UWebUIBridge::OpenPath(const FString& Path, int32 Line)
{
	if (Path.IsEmpty()) return;
	UE_LOG(LogAgentIntegrationKit, Log, TEXT("OpenPath: '%s' (line %d)"), *Path, Line);

	// Check if it's a UE asset path (/Game/, /Engine/, /Script/, or any /MountPoint/ path)
	if (Path.StartsWith(TEXT("/")))
	{
		// Try to find as an asset first
		FAssetRegistryModule& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		FAssetData AssetData = AssetRegistry.Get().GetAssetByObjectPath(FSoftObjectPath(Path));

		if (!AssetData.IsValid())
		{
			// Try with .0 suffix stripped (some paths include sub-object)
			FString CleanPath = Path;
			int32 DotIndex;
			if (CleanPath.FindLastChar('.', DotIndex))
			{
				CleanPath = CleanPath.Left(DotIndex);
				AssetData = AssetRegistry.Get().GetAssetByObjectPath(FSoftObjectPath(CleanPath));
			}
		}

		if (!AssetData.IsValid())
		{
			// Package path without .AssetName suffix (e.g. /Game/Path/BP_C7 → /Game/Path/BP_C7.BP_C7)
			FString AssetName = FPaths::GetBaseFilename(Path);
			if (!AssetName.IsEmpty())
			{
				FString FullObjectPath = Path + TEXT(".") + AssetName;
				AssetData = AssetRegistry.Get().GetAssetByObjectPath(FSoftObjectPath(FullObjectPath));
			}
		}

		if (!AssetData.IsValid())
		{
			// Fall back to package name lookup (handles any asset in the package)
			TArray<FAssetData> PackageAssets;
			AssetRegistry.Get().GetAssetsByPackageName(*Path, PackageAssets);
			if (PackageAssets.Num() > 0)
			{
				AssetData = PackageAssets[0];
			}
		}

		if (AssetData.IsValid())
		{
			// Load and open the asset in its editor
			UObject* LoadedAsset = AssetData.GetAsset();
			if (!LoadedAsset)
			{
				UE_LOG(LogAgentIntegrationKit, Warning, TEXT("OpenPath: GetAsset() returned null for '%s', trying LoadObject"), *Path);
				LoadedAsset = LoadObject<UObject>(nullptr, *AssetData.GetSoftObjectPath().ToString());
			}

			if (LoadedAsset)
			{
				UE_LOG(LogAgentIntegrationKit, Log, TEXT("OpenPath: Opening asset '%s' (%s)"), *LoadedAsset->GetName(), *LoadedAsset->GetClass()->GetName());
				if (UAssetEditorSubsystem* EditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
				{
					EditorSubsystem->OpenEditorForAsset(LoadedAsset);
				}
			}
			else
			{
				UE_LOG(LogAgentIntegrationKit, Warning, TEXT("OpenPath: Failed to load asset for '%s', falling back to Content Browser sync"), *Path);
				// At least navigate Content Browser to the asset
				TArray<FAssetData> Assets = { AssetData };
				IContentBrowserSingleton::Get().SyncBrowserToAssets(Assets, /*bAllowLockedBrowsers=*/true, /*bFocusContentBrowser=*/true);
			}
			return;
		}
	}

	// Check if it's a filesystem source file
	FString FullPath = Path;
	if (!FPaths::FileExists(FullPath))
	{
		// Try relative to project
		FullPath = FPaths::Combine(FPaths::ProjectDir(), Path);
	}

	if (FPaths::FileExists(FullPath))
	{
		FString Extension = FPaths::GetExtension(FullPath).ToLower();

		// Source/text files — open in IDE with line number
		if (Extension == TEXT("h") || Extension == TEXT("cpp") || Extension == TEXT("c") ||
			Extension == TEXT("cs") || Extension == TEXT("py") || Extension == TEXT("js") ||
			Extension == TEXT("ts") || Extension == TEXT("ini") || Extension == TEXT("txt") ||
			Extension == TEXT("md") || Extension == TEXT("json") || Extension == TEXT("yaml") ||
			Extension == TEXT("yml") || Extension == TEXT("xml") || Extension == TEXT("cfg") ||
			Extension == TEXT("lua") || Extension == TEXT("toml") || Extension == TEXT("csv") ||
			Extension == TEXT("log") || Extension == TEXT("html") || Extension == TEXT("css") ||
			Extension == TEXT("svelte") || Extension == TEXT("sh") || Extension == TEXT("bat"))
		{
			FSourceCodeNavigation::OpenSourceFile(FullPath, Line, 0);
			return;
		}

		// Other files — open with OS default application
		FPlatformProcess::LaunchFileInDefaultExternalApplication(*FullPath);
		return;
	}

	// Last resort: try loading as UObject path
	if (Path.StartsWith(TEXT("/")))
	{
		if (UObject* Obj = LoadObject<UObject>(nullptr, *Path))
		{
			if (UAssetEditorSubsystem* EditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				EditorSubsystem->OpenEditorForAsset(Obj);
			}
		}
	}
}

// ── Notification Settings ────────────────────────────────────────────

void UWebUIBridge::FireCompletionNotifications(bool bSuccess)
{
	const UACPSettings* Settings = UACPSettings::Get();
	if (!Settings) return;

	// Check "only when unfocused" gate
	if (Settings->bOnlyNotifyWhenUnfocused)
	{
		if (FPlatformApplicationMisc::IsThisApplicationForeground())
		{
			return;
		}
	}

	// Toast notification
	if (Settings->bNotifyOnTaskComplete)
	{
		AsyncTask(ENamedThreads::GameThread, [bSuccess]()
		{
			FNotificationInfo Info(
				bSuccess
					? FText::FromString(TEXT("Agent finished the task."))
					: FText::FromString(TEXT("Agent encountered an error."))
			);
			Info.bFireAndForget = true;
			Info.FadeOutDuration = 1.0f;
			Info.ExpireDuration = 4.0f;
			Info.bUseSuccessFailIcons = true;
			Info.Image = bSuccess
				? FCoreStyle::Get().GetBrush(TEXT("NotificationList.SuccessImage"))
				: FCoreStyle::Get().GetBrush(TEXT("NotificationList.FailImage"));
			FSlateNotificationManager::Get().AddNotification(Info);
		});
	}

	// Taskbar flash
	if (Settings->bFlashTaskbarOnComplete)
	{
		AsyncTask(ENamedThreads::GameThread, []()
		{
			if (FSlateApplication::IsInitialized())
			{
				TSharedPtr<SWindow> ActiveWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
				if (ActiveWindow.IsValid())
				{
					TSharedPtr<FGenericWindow> NativeWindow = ActiveWindow->GetNativeWindow();
					if (NativeWindow.IsValid())
					{
						NativeWindow->DrawAttention(FWindowDrawAttentionParameters(EWindowDrawAttentionRequestType::UntilActivated));
					}
				}
			}
		});
	}

	// Sound
	if (Settings->bPlayCompletionSound)
	{
		const float Volume = Settings->CompletionSoundVolume;
		const FSoftObjectPath SoundPath = bSuccess ? Settings->CompletionSound : Settings->ErrorSound;

		AsyncTask(ENamedThreads::GameThread, [bSuccess, Volume, SoundPath]()
		{
			if (!GEditor) return;

			USoundBase* Sound = nullptr;

			// Try custom sound first
			if (SoundPath.IsValid())
			{
				Sound = Cast<USoundBase>(SoundPath.TryLoad());
			}

			// Fall back to default editor sounds
			if (!Sound)
			{
				const FString DefaultPath = bSuccess
					? TEXT("/Engine/EditorSounds/Notifications/CompileSuccess_Cue.CompileSuccess_Cue")
					: TEXT("/Engine/EditorSounds/Notifications/CompileFailed_Cue.CompileFailed_Cue");
				Sound = Cast<USoundBase>(StaticLoadObject(USoundBase::StaticClass(), nullptr, *DefaultPath));
			}

			if (Sound)
			{
				UAudioComponent* AudioComp = GEditor->PlayPreviewSound(Sound);
				if (AudioComp)
				{
					AudioComp->VolumeMultiplier = Volume;
				}
			}
		});
	}
}

void UWebUIBridge::FirePermissionRequestNotification()
{
	const UACPSettings* Settings = UACPSettings::Get();
	if (!Settings || !Settings->bPlayPermissionRequestSound)
	{
		return;
	}

	if (Settings->bOnlyNotifyWhenUnfocused && FPlatformApplicationMisc::IsThisApplicationForeground())
	{
		return;
	}

	if (!Settings->PermissionRequestSound.IsValid())
	{
		return;
	}

	if (!GEditor)
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();
	static constexpr double PermissionSoundCooldownSeconds = 2.0;
	if (Now - LastPermissionRequestSoundTime < PermissionSoundCooldownSeconds)
	{
		return;
	}

	USoundBase* Sound = Cast<USoundBase>(Settings->PermissionRequestSound.TryLoad());
	if (!Sound)
	{
		return;
	}

	UAudioComponent* AudioComp = GEditor->PlayPreviewSound(Sound);
	if (AudioComp)
	{
		AudioComp->VolumeMultiplier = Settings->PermissionRequestSoundVolume;
	}
	LastPermissionRequestSoundTime = Now;
}

FString UWebUIBridge::GetNotificationSettings()
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	const UACPSettings* Settings = UACPSettings::Get();
	if (Settings)
	{
		Root->SetBoolField(TEXT("onlyWhenUnfocused"), Settings->bOnlyNotifyWhenUnfocused);
		Root->SetBoolField(TEXT("notifyOnComplete"), Settings->bNotifyOnTaskComplete);
		Root->SetBoolField(TEXT("flashTaskbar"), Settings->bFlashTaskbarOnComplete);
		Root->SetBoolField(TEXT("playSound"), Settings->bPlayCompletionSound);
		Root->SetNumberField(TEXT("soundVolume"), Settings->CompletionSoundVolume);
		Root->SetStringField(TEXT("completionSound"), Settings->CompletionSound.ToString());
		Root->SetStringField(TEXT("errorSound"), Settings->ErrorSound.ToString());
		Root->SetBoolField(TEXT("playPermissionSound"), Settings->bPlayPermissionRequestSound);
		Root->SetNumberField(TEXT("permissionSoundVolume"), Settings->PermissionRequestSoundVolume);
		Root->SetStringField(TEXT("permissionRequestSound"), Settings->PermissionRequestSound.ToString());
	}
	return JsonToString(Root);
}

void UWebUIBridge::SetNotificationSetting(const FString& Key, const FString& Value)
{
	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings) return;

	if (Key == TEXT("onlyWhenUnfocused"))
	{
		Settings->bOnlyNotifyWhenUnfocused = Value.ToBool();
	}
	else if (Key == TEXT("notifyOnComplete"))
	{
		Settings->bNotifyOnTaskComplete = Value.ToBool();
	}
	else if (Key == TEXT("flashTaskbar"))
	{
		Settings->bFlashTaskbarOnComplete = Value.ToBool();
	}
	else if (Key == TEXT("playSound"))
	{
		Settings->bPlayCompletionSound = Value.ToBool();
	}
	else if (Key == TEXT("soundVolume"))
	{
		Settings->CompletionSoundVolume = FMath::Clamp(FCString::Atof(*Value), 0.0f, 1.0f);
	}
	else if (Key == TEXT("playPermissionSound"))
	{
		Settings->bPlayPermissionRequestSound = Value.ToBool();
	}
	else if (Key == TEXT("permissionSoundVolume"))
	{
		Settings->PermissionRequestSoundVolume = FMath::Clamp(FCString::Atof(*Value), 0.0f, 1.0f);
	}
	else
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("WebUIBridge: Unknown notification setting key: %s"), *Key);
		return;
	}

	Settings->SavePreferences();
}

void UWebUIBridge::OpenPluginSettings()
{
	ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
	if (SettingsModule)
	{
		SettingsModule->ShowViewer(TEXT("Project"), TEXT("Plugins"), TEXT("Agent Integration Kit"));
	}
}

void UWebUIBridge::RestartEditor()
{
	FUnrealEdMisc::Get().RestartEditor(false);
}

void UWebUIBridge::CheckForPluginUpdate()
{
	FAgentIntegrationKitModule::CheckForPluginUpdate();
}

// ── Agent Authentication ────────────────────────────────────────────

FString UWebUIBridge::GetAuthMethods(const FString& AgentName)
{
	TArray<FACPAuthMethod> Methods = FACPAgentManager::Get().GetAuthMethods(AgentName);

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FACPAuthMethod& M : Methods)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), M.Id);
		Obj->SetStringField(TEXT("name"), M.Name);
		Obj->SetStringField(TEXT("description"), M.Description);
		Obj->SetBoolField(TEXT("isTerminalAuth"), M.bIsTerminalAuth);
		Arr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Arr, Writer);
	return Out;
}

void UWebUIBridge::StartAgentLogin(const FString& AgentName, const FString& MethodId)
{
	UE_LOG(LogAgentIntegrationKit, Log, TEXT("WebUIBridge: Starting login for %s (method: %s)"), *AgentName, *MethodId);
	FACPAgentManager::Get().AuthenticateAgent(AgentName, MethodId);
}

void UWebUIBridge::BindOnLoginComplete(FWebJSFunction Callback)
{
	OnLoginCompleteCallback = Callback;
	BindDelegates();
}

// ── Agent Usage / Rate Limits ────────────────────────────────────────

// Helper: serialize rate limit window to JSON
static TSharedPtr<FJsonObject> RateLimitWindowToJson(const FAgentRateLimitWindow& Window)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("usedPercent"), Window.UsedPercent);
	Obj->SetStringField(TEXT("resetsAt"), Window.ResetsAt.ToIso8601());
	Obj->SetNumberField(TEXT("windowDurationMinutes"), Window.WindowDurationMinutes);
	Obj->SetBoolField(TEXT("hasData"), Window.HasData());
	return Obj;
}

// Helper: serialize full rate limit data to JSON string (includes Meshy balance)
static FString RateLimitDataToJsonString(const FAgentRateLimitData& Data)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("hasData"), Data.bHasData);
	Obj->SetBoolField(TEXT("isLoading"), Data.bIsLoading);
	Obj->SetStringField(TEXT("errorMessage"), Data.ErrorMessage);
	Obj->SetStringField(TEXT("agentName"), Data.AgentName);
	Obj->SetStringField(TEXT("planType"), Data.PlanType);
	Obj->SetStringField(TEXT("lastUpdated"), Data.LastUpdated.ToIso8601());

	// Rate limit windows
	Obj->SetObjectField(TEXT("primary"), RateLimitWindowToJson(Data.Primary));
	Obj->SetObjectField(TEXT("secondary"), RateLimitWindowToJson(Data.Secondary));
	Obj->SetObjectField(TEXT("modelSpecific"), RateLimitWindowToJson(Data.ModelSpecific));
	Obj->SetStringField(TEXT("modelSpecificLabel"), Data.ModelSpecificLabel);

	// Extra usage (Claude Extra)
	TSharedRef<FJsonObject> ExtraObj = MakeShared<FJsonObject>();
	ExtraObj->SetBoolField(TEXT("isEnabled"), Data.ExtraUsage.bIsEnabled);
	ExtraObj->SetNumberField(TEXT("usedAmount"), Data.ExtraUsage.UsedAmount);
	ExtraObj->SetNumberField(TEXT("limitAmount"), Data.ExtraUsage.LimitAmount);
	ExtraObj->SetStringField(TEXT("currencyCode"), Data.ExtraUsage.CurrencyCode);
	ExtraObj->SetBoolField(TEXT("hasData"), Data.ExtraUsage.HasData());
	Obj->SetObjectField(TEXT("extraUsage"), ExtraObj);

	// Meshy credits (global, not per-agent)
	TSharedRef<FJsonObject> MeshyObj = MakeShared<FJsonObject>();
	UACPSettings* Settings = UACPSettings::Get();
	bool bMeshyConfigured = Settings && Settings->HasMeshyAuth();
	FAgentUsageMonitor& Monitor = FAgentUsageMonitor::Get();
	MeshyObj->SetBoolField(TEXT("configured"), bMeshyConfigured);
	MeshyObj->SetNumberField(TEXT("balance"), Monitor.GetCachedMeshyBalance());
	MeshyObj->SetBoolField(TEXT("isLoading"), Monitor.IsMeshyBalanceLoading());
	MeshyObj->SetStringField(TEXT("error"), Monitor.GetMeshyBalanceError());
	Obj->SetObjectField(TEXT("meshy"), MeshyObj);

	return JsonToString(Obj);
}

FString UWebUIBridge::GetAgentUsage(const FString& AgentName)
{
	if (AgentName.IsEmpty())
	{
		return TEXT("{\"hasData\":false}");
	}

	FAgentUsageMonitor& Monitor = FAgentUsageMonitor::Get();

	// If this agent is supported, request an update (non-blocking, will fire callback when done)
	if (FAgentUsageMonitor::IsAgentSupported(AgentName))
	{
		Monitor.RequestUsageUpdate(AgentName);
	}

	// Also trigger Meshy balance fetch if configured
	UACPSettings* Settings = UACPSettings::Get();
	if (Settings && Settings->HasMeshyAuth())
	{
		Monitor.RequestMeshyBalanceUpdate();
	}

	// Return whatever is cached (may be empty if first fetch)
	const FAgentRateLimitData& Data = Monitor.GetCachedUsage(AgentName);
	return RateLimitDataToJsonString(Data);
}

void UWebUIBridge::RefreshAgentUsage(const FString& AgentName)
{
	if (AgentName.IsEmpty()) return;

	FAgentUsageMonitor& Monitor = FAgentUsageMonitor::Get();

	if (FAgentUsageMonitor::IsAgentSupported(AgentName))
	{
		Monitor.RequestUsageUpdate(AgentName);
	}

	// Also refresh Meshy balance
	UACPSettings* Settings = UACPSettings::Get();
	if (Settings && Settings->HasMeshyAuth())
	{
		Monitor.RequestMeshyBalanceUpdate();
	}
}

void UWebUIBridge::BindOnUsageUpdated(FWebJSFunction Callback)
{
	OnUsageUpdatedCallback = Callback;
	BindDelegates();
}

void UWebUIBridge::BindOnMcpStatus(FWebJSFunction Callback)
{
	OnMcpStatusCallback = Callback;
	BindDelegates();
}

void UWebUIBridge::BindOnContinuationDraftReady(FWebJSFunction Callback)
{
	OnContinuationDraftReadyCallback = Callback;
}

void UWebUIBridge::BindOnSessionListUpdated(FWebJSFunction Callback)
{
	OnSessionListUpdatedCallback = Callback;
	BindDelegates();

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("WebUIBridge: BindOnSessionListUpdated called"));

	// Immediately push any already-cached session lists so the UI doesn't miss
	// data that arrived before this callback was bound.
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	TArray<FString> AllAgents = AgentMgr.GetAvailableAgentNames();

	for (const FString& AgentName : AllAgents)
	{
		TArray<FACPRemoteSessionEntry> Sessions = AgentMgr.GetCachedSessionList(AgentName);
		Sessions.Append(GetLocalHistorySessionsForAgent(AgentName, FPaths::ProjectDir()));
		if (Sessions.Num() > 0)
		{
			UE_LOG(LogAgentIntegrationKit, Log, TEXT("WebUIBridge: Pushing %d cached sessions for '%s'"), Sessions.Num(), *AgentName);
			AgentMgr.OnAgentSessionListReceived.Broadcast(AgentName, Sessions);
		}
	}

	// Proactively connect ACP agents that aren't connected yet so their
	// session lists get fetched. This must happen AFTER the callback is set
	// (above) so push updates reach the JS side when they arrive.
	int32 ConnectingCount = 0;
	for (const FString& AgentName : AllAgents)
	{
		if (AgentMgr.IsChatGatewayAgent(AgentName)) continue;
		if (AgentMgr.IsConnectedToAgent(AgentName))
		{
			// Already connected — just request the session list again
			AgentMgr.RequestSessionList(AgentName);
			continue;
		}

		FACPAgentConfig* Config = AgentMgr.GetAgentConfig(AgentName);
		if (Config && Config->Status == EACPAgentStatus::Available)
		{
			UE_LOG(LogAgentIntegrationKit, Log, TEXT("WebUIBridge: Auto-connecting agent '%s' for session listing"), *AgentName);
			AgentMgr.ConnectToAgent(AgentName);
			ConnectingCount++;
		}
	}
	UE_LOG(LogAgentIntegrationKit, Log, TEXT("WebUIBridge: Auto-connecting %d agents for session listing"), ConnectingCount);
}

FString UWebUIBridge::RefreshSessionList()
{
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	TArray<FString> AllAgents = AgentMgr.GetAvailableAgentNames();

	int32 ConnectingCount = 0;
	for (const FString& AgentName : AllAgents)
	{
		if (AgentMgr.IsChatGatewayAgent(AgentName)) continue;

		const TArray<FACPRemoteSessionEntry> LocalHistorySessions = GetLocalHistorySessionsForAgent(AgentName, FPaths::ProjectDir());
		if (LocalHistorySessions.Num() > 0)
		{
			AgentMgr.OnAgentSessionListReceived.Broadcast(AgentName, LocalHistorySessions);
		}

		if (AgentMgr.IsConnectedToAgent(AgentName))
		{
			// Already connected — just request the session list
			AgentMgr.RequestSessionList(AgentName);
		}
		else
		{
			FACPAgentConfig* Config = AgentMgr.GetAgentConfig(AgentName);
			if (Config && Config->Status == EACPAgentStatus::Available)
			{
				UE_LOG(LogAgentIntegrationKit, Log, TEXT("WebUIBridge: RefreshSessionList: connecting agent '%s'"), *AgentName);
				AgentMgr.ConnectToAgent(AgentName);
				ConnectingCount++;
			}
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("connectingCount"), ConnectingCount);
	return JsonToString(Result);
}

void UWebUIBridge::NotifyMcpStatus(const FString& SessionId, const FString& Status)
{
	// Copy SessionId before clearing McpWaitingSessionId — callers pass
	// McpWaitingSessionId by const ref, so clearing it would alias-invalidate SessionId.
	const FString CapturedSessionId = SessionId;

	// Clean up MCP listeners
	if (McpToolsDiscoveredHandle.IsValid())
	{
		FMCPServer::Get().OnClientToolsDiscovered.Remove(McpToolsDiscoveredHandle);
		McpToolsDiscoveredHandle.Reset();
	}
	if (McpTimeoutTickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(McpTimeoutTickerHandle);
		McpTimeoutTickerHandle.Reset();
	}
	McpWaitingSessionId.Empty();

	// Fire JS callback
	if (OnMcpStatusCallback.IsValid())
	{
		TWeakObjectPtr<UWebUIBridge> WeakThis(this);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedSessionId, Status]()
		{
			if (UWebUIBridge* Self = WeakThis.Get())
			{
				if (Self->OnMcpStatusCallback.IsValid())
				{
					Self->OnMcpStatusCallback(CapturedSessionId, Status);
				}
			}
		});
	}

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("WebUIBridge: MCP status '%s' for session %s"), *Status, *CapturedSessionId);
}

// ── Attachments ──────────────────────────────────────────────────────

// Helper: serialize attachment list to JSON string (metadata only, no base64)
static FString SerializeAttachmentList(const TArray<FACPContextAttachment>& Attachments)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FACPContextAttachment& Att : Attachments)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), Att.AttachmentId.ToString());

		FString TypeStr;
		FString DisplayName;
		switch (Att.Type)
		{
		case EACPAttachmentType::BlueprintNode:
			TypeStr = TEXT("blueprint_node");
			DisplayName = Att.NodeAttachment.NodeTitle;
			break;
		case EACPAttachmentType::Blueprint:
			TypeStr = TEXT("blueprint");
			DisplayName = Att.BlueprintAttachment.DisplayName;
			break;
		case EACPAttachmentType::ImageAsset:
			TypeStr = TEXT("image");
			DisplayName = Att.ImageAttachment.DisplayName;
			Obj->SetStringField(TEXT("mimeType"), Att.ImageAttachment.MimeType);
			Obj->SetNumberField(TEXT("width"), Att.ImageAttachment.Width);
			Obj->SetNumberField(TEXT("height"), Att.ImageAttachment.Height);
			Obj->SetStringField(TEXT("thumbnail"), Att.ImageAttachment.ImageBase64);
			break;
		case EACPAttachmentType::FileAsset:
			TypeStr = TEXT("file");
			DisplayName = Att.FileAttachment.DisplayName;
			Obj->SetStringField(TEXT("mimeType"), Att.FileAttachment.MimeType);
			Obj->SetNumberField(TEXT("sizeBytes"), static_cast<double>(Att.FileAttachment.SizeBytes));
			Obj->SetBoolField(TEXT("hasExtractedText"), Att.FileAttachment.bHasExtractedText);
			break;
		case EACPAttachmentType::Actor:
			TypeStr = TEXT("actor");
			DisplayName = Att.ActorAttachment.ActorLabel;
			Obj->SetStringField(TEXT("className"), Att.ActorAttachment.ClassName);
			break;
		case EACPAttachmentType::GenericObject:
			TypeStr = TEXT("object");
			DisplayName = Att.GenericObjectAttachment.DisplayName;
			Obj->SetStringField(TEXT("className"), Att.GenericObjectAttachment.ClassName);
			Obj->SetStringField(TEXT("assetPath"), Att.GenericObjectAttachment.AssetPath);
			break;
		}
		Obj->SetStringField(TEXT("type"), TypeStr);
		Obj->SetStringField(TEXT("displayName"), DisplayName);
		Arr.Add(MakeShared<FJsonValueObject>(Obj));
	}
	return JsonArrayToString(Arr);
}

FString UWebUIBridge::PasteClipboardImage()
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	FACPClipboardImageData ClipData = FACPClipboardImageReader::ReadImageFromClipboard();
	if (!ClipData.bIsValid)
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("No image on clipboard"));
		return JsonToString(Result);
	}

	FACPAttachmentManager& AttMgr = FACPAttachmentManager::Get();

	if (ClipData.EncodedData.Num() > 0)
	{
		// macOS path — already PNG/JPEG encoded
		AttMgr.AddImageFromEncodedData(ClipData.EncodedData, ClipData.MimeType, ClipData.Width, ClipData.Height, TEXT("Pasted Image"));
	}
	else if (ClipData.RawPixels.Num() > 0)
	{
		// Windows path — raw BGRA pixels
		AttMgr.AddImageFromRawData(ClipData.RawPixels, ClipData.Width, ClipData.Height, TEXT("Pasted Image"));
	}

	Result->SetBoolField(TEXT("success"), true);
	return JsonToString(Result);
}

FString UWebUIBridge::OpenImagePicker()
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetNumberField(TEXT("count"), 0);
		return JsonToString(Result);
	}

	TArray<FString> OutFiles;
	const void* ParentWindowHandle = FSlateApplication::Get().GetActiveTopLevelWindow().IsValid()
		? FSlateApplication::Get().GetActiveTopLevelWindow()->GetNativeWindow()->GetOSWindowHandle()
		: nullptr;

	if (DesktopPlatform->OpenFileDialog(
		ParentWindowHandle,
		TEXT("Select Attachments"),
		FPaths::ProjectDir(),
		TEXT(""),
		TEXT("Supported Files (*.png;*.jpg;*.jpeg;*.bmp;*.pdf;*.txt;*.md;*.json;*.csv;*.xml;*.yaml;*.yml;*.log;*.ini)|*.png;*.jpg;*.jpeg;*.bmp;*.pdf;*.txt;*.md;*.json;*.csv;*.xml;*.yaml;*.yml;*.log;*.ini"),
		EFileDialogFlags::Multiple,
		OutFiles))
	{
		FACPAttachmentManager& AttMgr = FACPAttachmentManager::Get();
		for (const FString& FilePath : OutFiles)
		{
			const FString Ext = FPaths::GetExtension(FilePath).ToLower();
			if (Ext == TEXT("png") || Ext == TEXT("jpg") || Ext == TEXT("jpeg") || Ext == TEXT("bmp"))
			{
				AttMgr.AddImageFromFile(FilePath);
			}
			else
			{
				AttMgr.AddFileFromPath(FilePath);
			}
		}
		Result->SetBoolField(TEXT("success"), true);
		Result->SetNumberField(TEXT("count"), OutFiles.Num());
	}
	else
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetNumberField(TEXT("count"), 0);
	}

	return JsonToString(Result);
}

FString UWebUIBridge::AddImageFromBase64(const FString& Base64Data, const FString& MimeType, int32 Width, int32 Height, const FString& DisplayName)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	if (Base64Data.IsEmpty())
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Empty base64 data"));
		return JsonToString(Result);
	}

	TArray<uint8> DecodedBytes;
	if (!FBase64::Decode(Base64Data, DecodedBytes))
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Failed to decode base64"));
		return JsonToString(Result);
	}

	FACPAttachmentManager& AttMgr = FACPAttachmentManager::Get();
	AttMgr.AddImageFromEncodedData(DecodedBytes, MimeType.IsEmpty() ? TEXT("image/png") : MimeType, Width, Height,
		DisplayName.IsEmpty() ? TEXT("Dropped Image") : DisplayName);

	// Return the ID of the last added attachment
	const TArray<FACPContextAttachment>& Atts = AttMgr.GetAttachments();
	if (Atts.Num() > 0)
	{
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("attachmentId"), Atts.Last().AttachmentId.ToString());
	}
	else
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Attachment was not added"));
	}

	return JsonToString(Result);
}

FString UWebUIBridge::AddFileFromBase64(const FString& Base64Data, const FString& MimeType, const FString& DisplayName)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	if (Base64Data.IsEmpty())
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Empty base64 data"));
		return JsonToString(Result);
	}

	TArray<uint8> DecodedBytes;
	if (!FBase64::Decode(Base64Data, DecodedBytes))
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Failed to decode base64"));
		return JsonToString(Result);
	}

	FACPAttachmentManager& AttMgr = FACPAttachmentManager::Get();
	AttMgr.AddFileFromEncodedData(
		DecodedBytes,
		MimeType.IsEmpty() ? TEXT("application/octet-stream") : MimeType,
		DisplayName.IsEmpty() ? TEXT("Dropped File") : DisplayName);

	const TArray<FACPContextAttachment>& Atts = AttMgr.GetAttachments();
	if (Atts.Num() > 0)
	{
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("attachmentId"), Atts.Last().AttachmentId.ToString());
	}
	else
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Attachment was not added"));
	}

	return JsonToString(Result);
}

void UWebUIBridge::RemoveAttachment(const FString& AttachmentId)
{
	FGuid Guid;
	if (FGuid::Parse(AttachmentId, Guid))
	{
		FACPAttachmentManager::Get().RemoveAttachment(Guid);
	}
}

FString UWebUIBridge::GetAttachments()
{
	return SerializeAttachmentList(FACPAttachmentManager::Get().GetAttachments());
}

void UWebUIBridge::BindOnAttachmentsChanged(FWebJSFunction Callback)
{
	OnAttachmentsChangedCallback = Callback;

	// Subscribe to attachment manager delegate (not part of BindDelegates since it's on AttachmentManager, not AgentManager)
	if (!AttachmentsChangedHandle.IsValid())
	{
		AttachmentsChangedHandle = FACPAttachmentManager::Get().OnAttachmentsChanged.AddLambda(
			[this]()
			{
				if (!OnAttachmentsChangedCallback.IsValid()) return;

				FString JsonStr = SerializeAttachmentList(FACPAttachmentManager::Get().GetAttachments());

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedJson = MoveTemp(JsonStr)]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnAttachmentsChangedCallback.IsValid())
						{
							Self->OnAttachmentsChangedCallback(CapturedJson);
						}
					}
				});
			}
		);
	}

}

void UWebUIBridge::BindOnInstallProgress(FWebJSFunction Callback)
{
	OnInstallProgressCallback = Callback;
}

void UWebUIBridge::BindOnInstallComplete(FWebJSFunction Callback)
{
	OnInstallCompleteCallback = Callback;
}

void UWebUIBridge::BindOnModelsAvailable(FWebJSFunction Callback)
{
	OnModelsAvailableCallback = Callback;
	BindDelegates();
}

void UWebUIBridge::BindOnCommandsAvailable(FWebJSFunction Callback)
{
	OnCommandsAvailableCallback = Callback;
	BindDelegates();
}

void UWebUIBridge::BindOnPlanUpdate(FWebJSFunction Callback)
{
	OnPlanUpdateCallback = Callback;
	BindDelegates();
}

// ── Streaming Callbacks ──────────────────────────────────────────────

void UWebUIBridge::BindOnMessage(FWebJSFunction Callback)
{
	OnMessageCallback = Callback;
	BindDelegates();
}

void UWebUIBridge::BindOnStateChanged(FWebJSFunction Callback)
{
	OnStateChangedCallback = Callback;
	BindDelegates();
}

void UWebUIBridge::BindOnPermissionRequest(FWebJSFunction Callback)
{
	OnPermissionRequestCallback = Callback;
	BindDelegates();
}

void UWebUIBridge::BindOnModesAvailable(FWebJSFunction Callback)
{
	OnModesAvailableCallback = Callback;
	BindDelegates();
}

void UWebUIBridge::BindOnModeChanged(FWebJSFunction Callback)
{
	OnModeChangedCallback = Callback;
	BindDelegates();
}

void UWebUIBridge::RespondToPermission(const FString& AgentName, int32 RequestId, const FString& OptionId, const FString& OutcomeMetaJson)
{
	if (AgentName.IsEmpty() || OptionId.IsEmpty()) return;

	TSharedPtr<FJsonObject> OutcomeMeta;
	if (!OutcomeMetaJson.IsEmpty())
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OutcomeMetaJson);
		FJsonSerializer::Deserialize(Reader, OutcomeMeta);
	}

	FACPAgentManager::Get().RespondToPermissionRequest(AgentName, RequestId, OptionId, OutcomeMeta);
}

void UWebUIBridge::BindDelegates()
{
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();

	// Only bind once
	if (!AgentMessageHandle.IsValid())
	{
		AgentMessageHandle = AgentMgr.OnAgentMessage.AddLambda(
			[this](const FString& SessionId, const FString& AgentName, const FACPSessionUpdate& Update)
			{
				// ── Persist streaming content to SessionManager (mirrors Slate UI) ──
				// Without this, switching sessions loses assistant messages because
				// GetSessionMessages() reads from the session manager, not the JS store.
				if (Update.UpdateType != EACPUpdateType::UsageUpdate && Update.UpdateType != EACPUpdateType::Plan)
				{
					FACPSessionManager& SessionMgr = FACPSessionManager::Get();

					// UserMessageChunk (from history replay) — finish any in-progress assistant message,
					// then add the user message directly
					if (Update.UpdateType == EACPUpdateType::UserMessageChunk)
					{
						int32* MsgIdxPtr = StreamingMessageIndices.Find(SessionId);
						if (MsgIdxPtr && *MsgIdxPtr != INDEX_NONE)
						{
							SessionMgr.FinishMessage(SessionId, *MsgIdxPtr);
							*MsgIdxPtr = INDEX_NONE;
						}
						SessionMgr.AddUserMessage(SessionId, Update.TextChunk);
					}
					else
					{
						// Start a new assistant message if one isn't active for this session
						int32* MsgIdxPtr = StreamingMessageIndices.Find(SessionId);
						if (!MsgIdxPtr || *MsgIdxPtr == INDEX_NONE)
						{
							int32 NewIdx = SessionMgr.StartAssistantMessage(SessionId);
							StreamingMessageIndices.Add(SessionId, NewIdx);
							MsgIdxPtr = StreamingMessageIndices.Find(SessionId);
						}

						int32 MsgIdx = *MsgIdxPtr;

							bool bFinishStreamingAfterUpdate = false;
							switch (Update.UpdateType)
							{
						case EACPUpdateType::AgentMessageChunk:
							if (Update.bIsSystemStatus)
							{
								SessionMgr.AppendStreamingText(SessionId, MsgIdx, EACPContentBlockType::System, Update.TextChunk);
							}
							else
							{
								SessionMgr.AppendStreamingText(SessionId, MsgIdx, EACPContentBlockType::Text, Update.TextChunk);
							}
							break;

						case EACPUpdateType::AgentThoughtChunk:
							SessionMgr.AppendStreamingText(SessionId, MsgIdx, EACPContentBlockType::Thought, Update.TextChunk);
							break;

						case EACPUpdateType::ToolCall:
							{
								FACPContentBlock Block;
								Block.Type = EACPContentBlockType::ToolCall;
								Block.ToolCallId = Update.ToolCallId;
								Block.ToolName = Update.ToolName;
								Block.ToolArguments = Update.ToolArguments;
								Block.ParentToolCallId = Update.ParentToolCallId;
								SessionMgr.AppendContentBlock(SessionId, MsgIdx, Block);
							}
							break;

						case EACPUpdateType::ToolCallUpdate:
							{
								FACPContentBlock Block;
								Block.Type = EACPContentBlockType::ToolResult;
								Block.ToolCallId = Update.ToolCallId;
								Block.ToolResultContent = Update.ToolResult;
								Block.bToolSuccess = Update.bToolSuccess;
								Block.ToolResultImages = Update.ToolResultImages;
								Block.ParentToolCallId = Update.ParentToolCallId;
								SessionMgr.AppendContentBlock(SessionId, MsgIdx, Block);
							}
							break;

							case EACPUpdateType::Error:
								{
									FACPContentBlock Block;
									Block.Type = EACPContentBlockType::Error;
									Block.Text = Update.ErrorMessage.IsEmpty() ? Update.TextChunk : Update.ErrorMessage;
									SessionMgr.AppendContentBlock(SessionId, MsgIdx, Block);
									bFinishStreamingAfterUpdate = true;
								}
								break;

							default:
								break;
							}

							if (bFinishStreamingAfterUpdate)
							{
								SessionMgr.FinishMessage(SessionId, MsgIdx);
								*MsgIdxPtr = INDEX_NONE;
							}
						}
					}

				if (!OnMessageCallback.IsValid()) return;

				// Serialize the update to JSON
				TSharedRef<FJsonObject> UpdateJson = MakeShared<FJsonObject>();
				UpdateJson->SetStringField(TEXT("agentName"), AgentName);

				// Map update type
				FString TypeStr;
				switch (Update.UpdateType)
				{
				case EACPUpdateType::AgentMessageChunk:  TypeStr = TEXT("text_chunk"); break;
				case EACPUpdateType::AgentThoughtChunk:  TypeStr = TEXT("thought_chunk"); break;
				case EACPUpdateType::ToolCall:            TypeStr = TEXT("tool_call"); break;
				case EACPUpdateType::ToolCallUpdate:      TypeStr = TEXT("tool_result"); break;
				case EACPUpdateType::Error:               TypeStr = TEXT("error"); break;
				case EACPUpdateType::UserMessageChunk:    TypeStr = TEXT("user_message_chunk"); break;
				case EACPUpdateType::UsageUpdate:         TypeStr = TEXT("usage"); break;
				case EACPUpdateType::Plan:                TypeStr = TEXT("plan"); break;
				default:                                  TypeStr = TEXT("unknown"); break;
				}
				UpdateJson->SetStringField(TEXT("type"), TypeStr);
				UpdateJson->SetStringField(TEXT("text"), Update.TextChunk);

				if (Update.bIsSystemStatus)
				{
					UpdateJson->SetStringField(TEXT("systemStatus"), Update.SystemStatus);
				}

				if (!Update.ToolCallId.IsEmpty())
				{
					UpdateJson->SetStringField(TEXT("toolCallId"), Update.ToolCallId);
					UpdateJson->SetStringField(TEXT("toolName"), Update.ToolName);
					UpdateJson->SetStringField(TEXT("toolArguments"), Update.ToolArguments);
					UpdateJson->SetStringField(TEXT("toolResult"), Update.ToolResult);
					UpdateJson->SetBoolField(TEXT("toolSuccess"), Update.bToolSuccess);
					if (!Update.ParentToolCallId.IsEmpty())
					{
						UpdateJson->SetStringField(TEXT("parentToolCallId"), Update.ParentToolCallId);
					}

					// Serialize tool result images
					if (Update.ToolResultImages.Num() > 0)
					{
						TArray<TSharedPtr<FJsonValue>> ImagesArr;
						for (const FACPToolResultImage& Img : Update.ToolResultImages)
						{
							TSharedRef<FJsonObject> ImgObj = MakeShared<FJsonObject>();
							ImgObj->SetStringField(TEXT("base64"), Img.Base64Data);
							ImgObj->SetStringField(TEXT("mimeType"), Img.MimeType);
							ImgObj->SetNumberField(TEXT("width"), Img.Width);
							ImgObj->SetNumberField(TEXT("height"), Img.Height);
							ImagesArr.Add(MakeShared<FJsonValueObject>(ImgObj));
						}
						UpdateJson->SetArrayField(TEXT("images"), ImagesArr);
					}
				}

				if (!Update.ErrorMessage.IsEmpty())
				{
					UpdateJson->SetStringField(TEXT("errorMessage"), Update.ErrorMessage);
					UpdateJson->SetNumberField(TEXT("errorCode"), Update.ErrorCode);
				}

				// Serialize usage data for usage updates
				if (Update.UpdateType == EACPUpdateType::UsageUpdate)
				{
					const FACPUsageData& U = Update.Usage;
					UpdateJson->SetNumberField(TEXT("inputTokens"), U.InputTokens);
					UpdateJson->SetNumberField(TEXT("outputTokens"), U.OutputTokens);
					UpdateJson->SetNumberField(TEXT("totalTokens"), U.TotalTokens);
					UpdateJson->SetNumberField(TEXT("cacheReadTokens"), U.CacheReadTokens);
					UpdateJson->SetNumberField(TEXT("cacheCreationTokens"), U.CacheCreationTokens);
					UpdateJson->SetNumberField(TEXT("reasoningTokens"), U.ReasoningTokens);
					UpdateJson->SetNumberField(TEXT("costAmount"), U.CostAmount);
					UpdateJson->SetStringField(TEXT("costCurrency"), U.CostCurrency);
					UpdateJson->SetNumberField(TEXT("turnCostUSD"), U.TurnCostUSD);
					UpdateJson->SetNumberField(TEXT("contextUsed"), U.ContextUsed);
					UpdateJson->SetNumberField(TEXT("contextSize"), U.ContextSize);
					UpdateJson->SetNumberField(TEXT("numTurns"), U.NumTurns);
					UpdateJson->SetNumberField(TEXT("durationMs"), U.DurationMs);

					if (U.ModelUsage.Num() > 0)
					{
						TArray<TSharedPtr<FJsonValue>> ModelArr;
						for (const FModelUsageEntry& M : U.ModelUsage)
						{
							TSharedRef<FJsonObject> MObj = MakeShared<FJsonObject>();
							MObj->SetStringField(TEXT("modelName"), M.ModelName);
							MObj->SetNumberField(TEXT("inputTokens"), M.InputTokens);
							MObj->SetNumberField(TEXT("outputTokens"), M.OutputTokens);
							MObj->SetNumberField(TEXT("cacheReadTokens"), M.CacheReadTokens);
							MObj->SetNumberField(TEXT("cacheCreationTokens"), M.CacheCreationTokens);
							MObj->SetNumberField(TEXT("costUSD"), M.CostUSD);
							MObj->SetNumberField(TEXT("contextWindow"), M.ContextWindow);
							MObj->SetNumberField(TEXT("maxOutputTokens"), M.MaxOutputTokens);
							ModelArr.Add(MakeShared<FJsonValueObject>(MObj));
						}
						UpdateJson->SetArrayField(TEXT("modelUsage"), ModelArr);
					}
				}

				FString JsonStr = JsonToString(UpdateJson);

				// Dispatch to game thread — FWebJSFunction calls WKWebView which requires main thread
				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedSessionId = SessionId, CapturedJson = MoveTemp(JsonStr)]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnMessageCallback.IsValid())
						{
							Self->OnMessageCallback(CapturedSessionId, CapturedJson);
						}
					}
				});
			}
		);
	}

	if (!AgentStateHandle.IsValid())
	{
		AgentStateHandle = AgentMgr.OnAgentStateChanged.AddLambda(
			[this](const FString& SessionId, const FString& AgentName, EACPClientState State, const FString& Message)
			{
				// Finalize streaming message when agent finishes prompting
				if (State == EACPClientState::Ready || State == EACPClientState::InSession)
				{
					int32* MsgIdxPtr = StreamingMessageIndices.Find(SessionId);
					if (MsgIdxPtr && *MsgIdxPtr != INDEX_NONE)
					{
						FACPSessionManager& SessionMgr = FACPSessionManager::Get();
						const FACPActiveSession* Session = SessionMgr.GetActiveSession(SessionId);
						if (Session && Session->Messages.IsValidIndex(*MsgIdxPtr))
						{
							SessionMgr.UpdateMessage(SessionId, *MsgIdxPtr, Session->Messages[*MsgIdxPtr]);
							SessionMgr.FinishMessage(SessionId, *MsgIdxPtr);
						}
						*MsgIdxPtr = INDEX_NONE;
					}
				}

				// Detect prompting → ready/in_session transition for completion notifications
				EACPClientState* PrevPtr = PreviousSessionStates.Find(SessionId);
				EACPClientState PrevState = PrevPtr ? *PrevPtr : EACPClientState::Disconnected;
				PreviousSessionStates.FindOrAdd(SessionId) = State;

				if (PrevState == EACPClientState::Prompting
					&& (State == EACPClientState::Ready || State == EACPClientState::InSession))
				{
					FireCompletionNotifications(/*bSuccess=*/ true);
				}
				else if (PrevState == EACPClientState::Prompting && State == EACPClientState::Error)
				{
					FireCompletionNotifications(/*bSuccess=*/ false);
				}

				if (!OnStateChangedCallback.IsValid()) return;

				FString StateStr;
				switch (State)
				{
				case EACPClientState::Disconnected:  StateStr = TEXT("disconnected"); break;
				case EACPClientState::Connecting:     StateStr = TEXT("connecting"); break;
				case EACPClientState::Initializing:   StateStr = TEXT("initializing"); break;
				case EACPClientState::Ready:          StateStr = TEXT("ready"); break;
				case EACPClientState::InSession:      StateStr = TEXT("in_session"); break;
				case EACPClientState::Prompting:      StateStr = TEXT("prompting"); break;
				case EACPClientState::Error:          StateStr = TEXT("error"); break;
				default:                              StateStr = TEXT("unknown"); break;
				}

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, SessionId, AgentName, StateStr, Message, State]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnStateChangedCallback.IsValid())
						{
							Self->OnStateChangedCallback(SessionId, AgentName, StateStr, Message);
						}

						// Gate chat input until MCP tools are discovered by the ACP agent.
						// ACP agents get MCP config in session/new but discover tools asynchronously.
						if (State == EACPClientState::InSession
							&& FMCPServer::Get().IsRunning()
							&& !FACPAgentManager::Get().IsChatGatewayAgent(AgentName)
							&& Self->McpWaitingSessionId.IsEmpty())  // Not already waiting
						{
							Self->McpWaitingSessionId = SessionId;

							// Fire "waiting" status to JS
							if (Self->OnMcpStatusCallback.IsValid())
							{
								Self->OnMcpStatusCallback(SessionId, TEXT("waiting"));
							}

							// Check if MCP tools were already discovered (race: tools/list
							// can complete before we register the listener)
							if (FMCPServer::Get().HasClientDiscoveredTools())
							{
								Self->NotifyMcpStatus(Self->McpWaitingSessionId, TEXT("ready"));
								// NotifyMcpStatus clears McpWaitingSessionId, skip listener/timeout
							}
							else
							{
							// Listen for MCP client tools/list completion
							if (!Self->McpToolsDiscoveredHandle.IsValid())
							{
								Self->McpToolsDiscoveredHandle = FMCPServer::Get().OnClientToolsDiscovered.AddLambda(
									[WeakThis]()
									{
										if (UWebUIBridge* S = WeakThis.Get())
										{
											if (!S->McpWaitingSessionId.IsEmpty())
											{
												S->NotifyMcpStatus(S->McpWaitingSessionId, TEXT("ready"));
											}
										}
									});
							}

							// Timeout fallback — unblock after 15 seconds
							Self->McpTimeoutTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
								FTickerDelegate::CreateLambda([WeakThis](float) -> bool
								{
									if (UWebUIBridge* S = WeakThis.Get())
									{
										if (!S->McpWaitingSessionId.IsEmpty())
										{
											S->NotifyMcpStatus(S->McpWaitingSessionId, TEXT("timeout"));
										}
									}
									return false; // one-shot
								}), 5.0f);

							UE_LOG(LogAgentIntegrationKit, Log,
								TEXT("WebUIBridge: Waiting for MCP tools discovery (session %s, agent %s)"),
								*SessionId, *AgentName);
							} // else (tools not yet discovered)
						}
					}
				});
			}
		);
	}

		if (!AgentErrorHandle.IsValid())
		{
			AgentErrorHandle = AgentMgr.OnAgentError.AddLambda(
				[this](const FString& SessionId, const FString& AgentName, int32 ErrorCode, const FString& ErrorMessage)
				{
					FString RoutedSessionId = SessionId;
					if (RoutedSessionId.IsEmpty())
					{
						RoutedSessionId = FACPAgentManager::Get().GetActiveSessionForAgent(AgentName);
					}

					// Finalize persisted streaming message on hard errors so reloading
					// old sessions does not leave messages marked as still streaming.
					int32* MsgIdxPtr = StreamingMessageIndices.Find(RoutedSessionId);
					if (MsgIdxPtr && *MsgIdxPtr != INDEX_NONE)
					{
						FACPSessionManager& SessionMgr = FACPSessionManager::Get();
						SessionMgr.FinishMessage(RoutedSessionId, *MsgIdxPtr);
						*MsgIdxPtr = INDEX_NONE;
					}

					if (!OnMessageCallback.IsValid()) return;

				// Construct a JSON error update matching the streaming update format
				TSharedRef<FJsonObject> UpdateJson = MakeShared<FJsonObject>();
				UpdateJson->SetStringField(TEXT("agentName"), AgentName);
				UpdateJson->SetStringField(TEXT("type"), TEXT("error"));
				UpdateJson->SetStringField(TEXT("text"), ErrorMessage);
				UpdateJson->SetStringField(TEXT("errorMessage"), ErrorMessage);
				UpdateJson->SetNumberField(TEXT("errorCode"), ErrorCode);

				FString JsonStr = JsonToString(UpdateJson);

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedSessionId = RoutedSessionId, CapturedJson = MoveTemp(JsonStr)]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnMessageCallback.IsValid())
						{
							Self->OnMessageCallback(CapturedSessionId, CapturedJson);
						}
					}
				});
			}
		);
	}

	if (!PermissionRequestHandle.IsValid())
	{
		PermissionRequestHandle = AgentMgr.OnAgentPermissionRequest.AddLambda(
			[this](const FString& SessionId, const FString& AgentName, const FACPPermissionRequest& Request)
			{
				if (!OnPermissionRequestCallback.IsValid()) return;

				TSharedRef<FJsonObject> ReqJson = MakeShared<FJsonObject>();
				ReqJson->SetStringField(TEXT("agentName"), AgentName);
				ReqJson->SetNumberField(TEXT("requestId"), Request.RequestId);
				ReqJson->SetBoolField(TEXT("isAskUserQuestion"), Request.bIsAskUserQuestion);

				// Tool call info
				TSharedPtr<FJsonObject> ToolCallObj = MakeShared<FJsonObject>();
				ToolCallObj->SetStringField(TEXT("toolCallId"), Request.ToolCall.ToolCallId);
				ToolCallObj->SetStringField(TEXT("title"), Request.ToolCall.Title);
				ToolCallObj->SetStringField(TEXT("rawInput"), Request.ToolCall.RawInput);
				ReqJson->SetObjectField(TEXT("toolCall"), ToolCallObj);

				// Permission options
				TArray<TSharedPtr<FJsonValue>> OptionsArr;
				for (const FACPPermissionOption& Opt : Request.Options)
				{
					TSharedPtr<FJsonObject> OptObj = MakeShared<FJsonObject>();
					OptObj->SetStringField(TEXT("optionId"), Opt.OptionId);
					OptObj->SetStringField(TEXT("name"), Opt.Name);
					OptObj->SetStringField(TEXT("kind"), Opt.Kind);
					OptionsArr.Add(MakeShared<FJsonValueObject>(OptObj));
				}
				ReqJson->SetArrayField(TEXT("options"), OptionsArr);

				// Questions (for AskUserQuestion)
				TArray<TSharedPtr<FJsonValue>> QuestionsArr;
				for (const FACPQuestion& Q : Request.Questions)
				{
					TSharedPtr<FJsonObject> QObj = MakeShared<FJsonObject>();
					QObj->SetStringField(TEXT("question"), Q.Question);
					QObj->SetStringField(TEXT("header"), Q.Header);
					QObj->SetBoolField(TEXT("multiSelect"), Q.bMultiSelect);

					TArray<TSharedPtr<FJsonValue>> QOptsArr;
					for (const FACPQuestionOption& QOpt : Q.Options)
					{
						TSharedPtr<FJsonObject> QOptObj = MakeShared<FJsonObject>();
						QOptObj->SetStringField(TEXT("label"), QOpt.Label);
						QOptObj->SetStringField(TEXT("description"), QOpt.Description);
						QOptsArr.Add(MakeShared<FJsonValueObject>(QOptObj));
					}
					QObj->SetArrayField(TEXT("options"), QOptsArr);
					QuestionsArr.Add(MakeShared<FJsonValueObject>(QObj));
				}
				ReqJson->SetArrayField(TEXT("questions"), QuestionsArr);

				FString JsonStr = JsonToString(ReqJson);

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedSessionId = SessionId, CapturedJson = MoveTemp(JsonStr)]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						Self->FirePermissionRequestNotification();
						if (Self->OnPermissionRequestCallback.IsValid())
						{
							Self->OnPermissionRequestCallback(CapturedSessionId, CapturedJson);
						}
					}
				});
			}
		);
	}

	if (!ModesAvailableHandle.IsValid())
	{
		ModesAvailableHandle = AgentMgr.OnAgentModesAvailable.AddLambda(
			[this](const FString& SessionId, const FString& AgentName, const FACPSessionModeState& ModeState)
			{
				if (!OnModesAvailableCallback.IsValid()) return;

				TArray<TSharedPtr<FJsonValue>> ModesArr;
				for (const FACPSessionMode& Mode : ModeState.AvailableModes)
				{
					TSharedPtr<FJsonObject> ModeObj = MakeShared<FJsonObject>();
					ModeObj->SetStringField(TEXT("id"), Mode.ModeId);
					ModeObj->SetStringField(TEXT("name"), Mode.Name);
					ModeObj->SetStringField(TEXT("description"), Mode.Description);
					ModesArr.Add(MakeShared<FJsonValueObject>(ModeObj));
				}

				TSharedRef<FJsonObject> ResultJson = MakeShared<FJsonObject>();
				ResultJson->SetArrayField(TEXT("modes"), ModesArr);
				ResultJson->SetStringField(TEXT("currentModeId"), ModeState.CurrentModeId);

				FString JsonStr = JsonToString(ResultJson);

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedAgentName = AgentName, CapturedJson = MoveTemp(JsonStr)]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnModesAvailableCallback.IsValid())
						{
							Self->OnModesAvailableCallback(CapturedAgentName, CapturedJson);
						}
					}
				});
			}
		);
	}

	if (!ModeChangedHandle.IsValid())
	{
		ModeChangedHandle = AgentMgr.OnAgentModeChanged.AddLambda(
			[this](const FString& SessionId, const FString& AgentName, const FString& ModeId)
			{
				if (!OnModeChangedCallback.IsValid()) return;

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, AgentName, ModeId]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnModeChangedCallback.IsValid())
						{
							Self->OnModeChangedCallback(AgentName, ModeId);
						}
					}
				});
			}
		);
	}

	if (!ModelsAvailableHandle.IsValid())
	{
		ModelsAvailableHandle = AgentMgr.OnAgentModelsAvailable.AddLambda(
			[this](const FString& SessionId, const FString& AgentName, const FACPSessionModelState& ModelState)
			{
				if (!OnModelsAvailableCallback.IsValid()) return;

				// Check if ACP client has reasoning effort options
					bool bACPHasReasoning = false;
					FACPAgentManager& Mgr = FACPAgentManager::Get();
					TSharedPtr<FACPClient> ACPClient = Mgr.GetClient(AgentName);
					if (ACPClient.IsValid() && ACPClient->SupportsReasoningEffortControl())
					{
						bACPHasReasoning = true;
					}

				TSharedRef<FJsonObject> ResultJson = MakeShared<FJsonObject>();

				TArray<TSharedPtr<FJsonValue>> ModelsArr;
				for (const FACPModelInfo& Model : ModelState.AvailableModels)
				{
					TSharedRef<FJsonObject> MObj = MakeShared<FJsonObject>();
					MObj->SetStringField(TEXT("id"), Model.ModelId);
					MObj->SetStringField(TEXT("name"), Model.Name);
					MObj->SetStringField(TEXT("description"), Model.Description);
					MObj->SetBoolField(TEXT("supportsReasoning"), Model.SupportsReasoning() || bACPHasReasoning);
					if (!Model.ProviderId.IsEmpty()) MObj->SetStringField(TEXT("provider"), Model.ProviderId);
					if (!Model.ProviderDisplayName.IsEmpty()) MObj->SetStringField(TEXT("providerDisplayName"), Model.ProviderDisplayName);
					ModelsArr.Add(MakeShared<FJsonValueObject>(MObj));
				}
				ResultJson->SetArrayField(TEXT("models"), ModelsArr);
				ResultJson->SetStringField(TEXT("currentModelId"), ModelState.CurrentModelId);

				FString JsonStr = JsonToString(ResultJson);

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedAgentName = AgentName, CapturedJson = MoveTemp(JsonStr)]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnModelsAvailableCallback.IsValid())
						{
							Self->OnModelsAvailableCallback(CapturedAgentName, CapturedJson);
						}
					}
				});
			}
		);
	}

	if (!CommandsAvailableHandle.IsValid())
	{
		CommandsAvailableHandle = AgentMgr.OnAgentCommandsAvailable.AddLambda(
			[this](const FString& SessionId, const FString& AgentName, const TArray<FACPSlashCommand>& Commands)
			{
				if (!OnCommandsAvailableCallback.IsValid()) return;

				TArray<TSharedPtr<FJsonValue>> CmdsArr;
				for (const FACPSlashCommand& Cmd : Commands)
				{
					TSharedRef<FJsonObject> CmdObj = MakeShared<FJsonObject>();
					CmdObj->SetStringField(TEXT("name"), Cmd.Name);
					CmdObj->SetStringField(TEXT("description"), Cmd.Description);
					CmdObj->SetStringField(TEXT("inputHint"), Cmd.InputHint);
					CmdsArr.Add(MakeShared<FJsonValueObject>(CmdObj));
				}

				FString JsonStr = JsonArrayToString(CmdsArr);

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedSessionId = SessionId, CapturedJson = MoveTemp(JsonStr)]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnCommandsAvailableCallback.IsValid())
						{
							Self->OnCommandsAvailableCallback(CapturedSessionId, CapturedJson);
						}
					}
				});
			}
		);
	}

	if (!PlanUpdateHandle.IsValid())
	{
		PlanUpdateHandle = AgentMgr.OnAgentPlanUpdate.AddLambda(
			[this](const FString& SessionId, const FString& AgentName, const FACPPlan& Plan)
			{
				if (!OnPlanUpdateCallback.IsValid()) return;

				TSharedRef<FJsonObject> PlanJson = MakeShared<FJsonObject>();

				TArray<TSharedPtr<FJsonValue>> EntriesArr;
				for (const FACPPlanEntry& Entry : Plan.Entries)
				{
					TSharedRef<FJsonObject> EntryObj = MakeShared<FJsonObject>();
					EntryObj->SetStringField(TEXT("content"), Entry.Content);
					EntryObj->SetStringField(TEXT("activeForm"), Entry.ActiveForm);

					FString PriorityStr;
					switch (Entry.Priority)
					{
					case EACPPlanEntryPriority::High:   PriorityStr = TEXT("high"); break;
					case EACPPlanEntryPriority::Medium: PriorityStr = TEXT("medium"); break;
					case EACPPlanEntryPriority::Low:    PriorityStr = TEXT("low"); break;
					}
					EntryObj->SetStringField(TEXT("priority"), PriorityStr);

					FString StatusStr;
					switch (Entry.Status)
					{
					case EACPPlanEntryStatus::Pending:    StatusStr = TEXT("pending"); break;
					case EACPPlanEntryStatus::InProgress: StatusStr = TEXT("in_progress"); break;
					case EACPPlanEntryStatus::Completed:  StatusStr = TEXT("completed"); break;
					}
					EntryObj->SetStringField(TEXT("status"), StatusStr);

					EntriesArr.Add(MakeShared<FJsonValueObject>(EntryObj));
				}
				PlanJson->SetArrayField(TEXT("entries"), EntriesArr);
				PlanJson->SetNumberField(TEXT("completedCount"), Plan.GetCompletedCount());
				PlanJson->SetNumberField(TEXT("totalCount"), Plan.Entries.Num());

				FString JsonStr = JsonToString(PlanJson);

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedSessionId = SessionId, CapturedJson = MoveTemp(JsonStr)]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnPlanUpdateCallback.IsValid())
						{
							Self->OnPlanUpdateCallback(CapturedSessionId, CapturedJson);
						}
					}
				});
			}
		);
	}

	// Usage data updates (from FAgentUsageMonitor, not FACPAgentManager)
	if (!UsageUpdatedHandle.IsValid())
	{
		UsageUpdatedHandle = FAgentUsageMonitor::Get().OnUsageDataUpdated.AddLambda(
			[this](const FString& AgentName, const FAgentRateLimitData& Data)
			{
				if (!OnUsageUpdatedCallback.IsValid()) return;

				FString JsonStr = RateLimitDataToJsonString(Data);

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedAgentName = AgentName, CapturedJson = MoveTemp(JsonStr)]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnUsageUpdatedCallback.IsValid())
						{
							Self->OnUsageUpdatedCallback(CapturedAgentName, CapturedJson);
						}
					}
				});
			}
		);
	}

	// Meshy balance updates — re-push usage data so UI sees updated Meshy fields
	if (!MeshyBalanceHandle.IsValid())
	{
		MeshyBalanceHandle = FAgentUsageMonitor::Get().OnMeshyBalanceUpdated.AddLambda(
			[this](bool /*bSuccess*/, int32 /*Balance*/)
			{
				if (!OnUsageUpdatedCallback.IsValid()) return;

				// Re-serialize usage data for all cached agents so Meshy fields are fresh.
				// We push an update with agent name "_meshy" so the UI knows to refresh its cached data.
				// The RateLimitDataToJsonString reads Meshy state from the monitor singleton.
				FAgentRateLimitData DummyData;
				DummyData.AgentName = TEXT("_meshy");
				FString JsonStr = RateLimitDataToJsonString(DummyData);

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedJson = MoveTemp(JsonStr)]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnUsageUpdatedCallback.IsValid())
						{
							Self->OnUsageUpdatedCallback(TEXT("_meshy"), CapturedJson);
						}
					}
				});
			}
		);
	}

	if (!AgentAuthCompleteHandle.IsValid())
	{
		AgentAuthCompleteHandle = AgentMgr.OnAgentAuthComplete.AddLambda(
			[this](const FString& SessionId, const FString& AgentName, bool bSuccess, const FString& Error)
			{
				if (!OnLoginCompleteCallback.IsValid()) return;

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, AgentName, bSuccess, Error]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnLoginCompleteCallback.IsValid())
						{
							Self->OnLoginCompleteCallback(AgentName, bSuccess, Error);
						}
					}
				});
			}
		);
	}

	if (!SessionListUpdatedHandle.IsValid())
	{
		SessionListUpdatedHandle = AgentMgr.OnAgentSessionListReceived.AddLambda(
			[this](const FString& AgentName, const TArray<FACPRemoteSessionEntry>& Sessions)
			{
				if (!OnSessionListUpdatedCallback.IsValid()) return;

				// Build a map from agent session ID → Unreal session ID for active sessions.
				// When a remote session matches an active session, we replace the agent ID
				// with the Unreal ID so the JS merge logic can match and update titles.
				FACPSessionManager& SessionMgr = FACPSessionManager::Get();
				TMap<FString, FString> AgentIdToUnrealId;
				TArray<FString> ActiveIds = SessionMgr.GetActiveSessionIds();
				for (const FString& Id : ActiveIds)
				{
					const FACPActiveSession* Active = SessionMgr.GetActiveSession(Id);
					if (Active && !Active->Metadata.AgentSessionId.IsEmpty())
					{
						AgentIdToUnrealId.Add(Active->Metadata.AgentSessionId, Id);
					}
				}

				// Serialize the session list to JSON
				TArray<TSharedPtr<FJsonValue>> SessionsArray;
				FACPAgentManager& AgentMgrForReg = FACPAgentManager::Get();
				for (const FACPRemoteSessionEntry& Entry : Sessions)
				{
					TSharedRef<FJsonObject> SessionObj = MakeShared<FJsonObject>();

					// If this remote session maps to an active Unreal session,
					// use the Unreal ID so JS dedup/merge works correctly
					FString UseSessionId = Entry.SessionId;
					if (const FString* UnrealId = AgentIdToUnrealId.Find(Entry.SessionId))
					{
						UseSessionId = *UnrealId;

						// Also update the active session's title in the session manager
						if (!Entry.Title.IsEmpty())
						{
							SessionMgr.UpdateSessionTitle(*UnrealId, Entry.Title);
						}
					}

					SessionObj->SetStringField(TEXT("sessionId"), UseSessionId);
					SessionObj->SetStringField(TEXT("title"), Entry.Title);

					// Apply persisted custom title if available (survives editor restarts)
					if (const FString* Persisted = SessionMgr.GetPersistedCustomTitle(UseSessionId))
					{
						SessionObj->SetStringField(TEXT("title"), *Persisted);
						SessionObj->SetBoolField(TEXT("hasCustomTitle"), true);
					}
					else if (const FString* PersistedByAgent = SessionMgr.GetPersistedCustomTitle(Entry.SessionId))
					{
						SessionObj->SetStringField(TEXT("title"), *PersistedByAgent);
						SessionObj->SetBoolField(TEXT("hasCustomTitle"), true);
					}

					if (Entry.UpdatedAt.GetTicks() > 0)
					{
						SessionObj->SetStringField(TEXT("lastModifiedAt"), Entry.UpdatedAt.ToIso8601());
					}

					SetSessionAgentRegistryFields(AgentMgrForReg, *SessionObj, AgentName);

					SessionsArray.Add(MakeShared<FJsonValueObject>(SessionObj));
				}
				FString SessionsJson = JsonArrayToString(SessionsArray);

				// This delegate fires on the game thread (ProcessLine dispatches there).
				// Call the JS callback directly — no need for a second AsyncTask dispatch
				// which can race with GC and cause the callback to silently drop.
				OnSessionListUpdatedCallback(AgentName, SessionsJson);
			}
		);
	}
}

void UWebUIBridge::UnbindDelegates()
{
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();

	if (AgentMessageHandle.IsValid())
	{
		AgentMgr.OnAgentMessage.Remove(AgentMessageHandle);
		AgentMessageHandle.Reset();
	}
	if (AgentStateHandle.IsValid())
	{
		AgentMgr.OnAgentStateChanged.Remove(AgentStateHandle);
		AgentStateHandle.Reset();
	}
	if (AgentErrorHandle.IsValid())
	{
		AgentMgr.OnAgentError.Remove(AgentErrorHandle);
		AgentErrorHandle.Reset();
	}
	if (PermissionRequestHandle.IsValid())
	{
		AgentMgr.OnAgentPermissionRequest.Remove(PermissionRequestHandle);
		PermissionRequestHandle.Reset();
	}
	if (ModesAvailableHandle.IsValid())
	{
		AgentMgr.OnAgentModesAvailable.Remove(ModesAvailableHandle);
		ModesAvailableHandle.Reset();
	}
	if (ModeChangedHandle.IsValid())
	{
		AgentMgr.OnAgentModeChanged.Remove(ModeChangedHandle);
		ModeChangedHandle.Reset();
	}
	if (CommandsAvailableHandle.IsValid())
	{
		AgentMgr.OnAgentCommandsAvailable.Remove(CommandsAvailableHandle);
		CommandsAvailableHandle.Reset();
	}
	if (PlanUpdateHandle.IsValid())
	{
		AgentMgr.OnAgentPlanUpdate.Remove(PlanUpdateHandle);
		PlanUpdateHandle.Reset();
	}
	if (ModelsAvailableHandle.IsValid())
	{
		AgentMgr.OnAgentModelsAvailable.Remove(ModelsAvailableHandle);
		ModelsAvailableHandle.Reset();
	}
	if (UsageUpdatedHandle.IsValid())
	{
		FAgentUsageMonitor::Get().OnUsageDataUpdated.Remove(UsageUpdatedHandle);
		UsageUpdatedHandle.Reset();
	}
	if (MeshyBalanceHandle.IsValid())
	{
		FAgentUsageMonitor::Get().OnMeshyBalanceUpdated.Remove(MeshyBalanceHandle);
		MeshyBalanceHandle.Reset();
	}
	if (AttachmentsChangedHandle.IsValid())
	{
		FACPAttachmentManager::Get().OnAttachmentsChanged.Remove(AttachmentsChangedHandle);
		AttachmentsChangedHandle.Reset();
	}
	if (AgentAuthCompleteHandle.IsValid())
	{
		AgentMgr.OnAgentAuthComplete.Remove(AgentAuthCompleteHandle);
		AgentAuthCompleteHandle.Reset();
	}
	if (McpToolsDiscoveredHandle.IsValid())
	{
		FMCPServer::Get().OnClientToolsDiscovered.Remove(McpToolsDiscoveredHandle);
		McpToolsDiscoveredHandle.Reset();
	}
	if (McpTimeoutTickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(McpTimeoutTickerHandle);
		McpTimeoutTickerHandle.Reset();
	}
	if (SessionListUpdatedHandle.IsValid())
	{
		AgentMgr.OnAgentSessionListReceived.Remove(SessionListUpdatedHandle);
		SessionListUpdatedHandle.Reset();
	}

	// Terminal delegates
	if (TerminalOutputHandle.IsValid())
	{
		FTerminalManager::Get().OnTerminalOutput.Remove(TerminalOutputHandle);
		TerminalOutputHandle.Reset();
	}
	if (TerminalExitHandle.IsValid())
	{
		FTerminalManager::Get().OnTerminalExit.Remove(TerminalExitHandle);
		TerminalExitHandle.Reset();
	}
}

// ── JSON Serialization Helpers ───────────────────────────────────────

TSharedPtr<FJsonObject> UWebUIBridge::ContentBlockToJson(const FACPContentBlock& Block)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

	FString TypeStr;
	switch (Block.Type)
	{
	case EACPContentBlockType::Text:       TypeStr = TEXT("text"); break;
	case EACPContentBlockType::Thought:    TypeStr = TEXT("thought"); break;
	case EACPContentBlockType::ToolCall:   TypeStr = TEXT("tool_call"); break;
	case EACPContentBlockType::ToolResult: TypeStr = TEXT("tool_result"); break;
	case EACPContentBlockType::Image:      TypeStr = TEXT("image"); break;
	case EACPContentBlockType::Error:      TypeStr = TEXT("error"); break;
	case EACPContentBlockType::System:     TypeStr = TEXT("system"); break;
	default:                               TypeStr = TEXT("unknown"); break;
	}
	Obj->SetStringField(TEXT("type"), TypeStr);
	Obj->SetStringField(TEXT("text"), Block.Text);
	Obj->SetBoolField(TEXT("isStreaming"), Block.bIsStreaming);

	if (Block.Type == EACPContentBlockType::ToolCall)
	{
		Obj->SetStringField(TEXT("toolCallId"), Block.ToolCallId);
		Obj->SetStringField(TEXT("toolName"), Block.ToolName);
		Obj->SetStringField(TEXT("toolArguments"), Block.ToolArguments);
		if (!Block.ParentToolCallId.IsEmpty())
		{
			Obj->SetStringField(TEXT("parentToolCallId"), Block.ParentToolCallId);
		}
	}

	if (Block.Type == EACPContentBlockType::ToolResult)
	{
		Obj->SetStringField(TEXT("toolCallId"), Block.ToolCallId);
		Obj->SetStringField(TEXT("toolResult"), Block.ToolResultContent);
		Obj->SetBoolField(TEXT("toolSuccess"), Block.bToolSuccess);
		if (!Block.ParentToolCallId.IsEmpty())
		{
			Obj->SetStringField(TEXT("parentToolCallId"), Block.ParentToolCallId);
		}

		// Serialize tool result images (base64 + metadata)
		if (Block.ToolResultImages.Num() > 0)
		{
			Obj->SetNumberField(TEXT("imageCount"), Block.ToolResultImages.Num());

			TArray<TSharedPtr<FJsonValue>> ImagesArr;
			for (const FACPToolResultImage& Img : Block.ToolResultImages)
			{
				TSharedRef<FJsonObject> ImgObj = MakeShared<FJsonObject>();
				ImgObj->SetStringField(TEXT("base64"), Img.Base64Data);
				ImgObj->SetStringField(TEXT("mimeType"), Img.MimeType);
				ImgObj->SetNumberField(TEXT("width"), Img.Width);
				ImgObj->SetNumberField(TEXT("height"), Img.Height);
				ImagesArr.Add(MakeShared<FJsonValueObject>(ImgObj));
			}
			Obj->SetArrayField(TEXT("images"), ImagesArr);
		}
	}

	return Obj;
}

TSharedPtr<FJsonObject> UWebUIBridge::MessageToJson(const FACPChatMessage& Message)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

	Obj->SetStringField(TEXT("messageId"), Message.MessageId.ToString());

	FString RoleStr;
	switch (Message.Role)
	{
	case EACPMessageRole::User:      RoleStr = TEXT("user"); break;
	case EACPMessageRole::Assistant: RoleStr = TEXT("assistant"); break;
	case EACPMessageRole::System:    RoleStr = TEXT("system"); break;
	default:                         RoleStr = TEXT("unknown"); break;
	}
	Obj->SetStringField(TEXT("role"), RoleStr);
	Obj->SetBoolField(TEXT("isStreaming"), Message.bIsStreaming);
	Obj->SetStringField(TEXT("timestamp"), Message.Timestamp.ToIso8601());

	// Content blocks
	TArray<TSharedPtr<FJsonValue>> BlocksArray;
	for (const FACPContentBlock& Block : Message.ContentBlocks)
	{
		TSharedPtr<FJsonObject> BlockJson = ContentBlockToJson(Block);
		if (BlockJson.IsValid())
		{
			BlocksArray.Add(MakeShared<FJsonValueObject>(BlockJson));
		}
	}
	Obj->SetArrayField(TEXT("contentBlocks"), BlocksArray);

	return Obj;
}
