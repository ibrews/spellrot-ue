// Copyright 2026 Betide Studio. All Rights Reserved.

#include "ACPSessionManager.h"
#include "AgentIntegrationKitModule.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

FACPSessionManager& FACPSessionManager::Get()
{
	static FACPSessionManager Instance;
	return Instance;
}

FACPSessionManager::FACPSessionManager()
{
	LoadCustomTitles();
}

FACPSessionManager::~FACPSessionManager()
{
}

// ── Title generation (moved from deleted ACPSessionStorage) ──────────

static FString GenerateTitleFromMessage(const FString& Message)
{
	FString Title = Message.Left(73); // 73 chars fits sidebar width at default font size without ellipsis overlap
	Title.ReplaceInline(TEXT("\n"), TEXT(" "));
	Title.ReplaceInline(TEXT("\r"), TEXT(""));
	Title.TrimStartAndEndInline();

	if (Title.Len() < Message.Len())
	{
		Title += TEXT("...");
	}

	return Title;
}

// ── Session Lifecycle ────────────────────────────────────────────────

FString FACPSessionManager::CreateSession(const FString& AgentName, const FString& ModelId)
{
	FScopeLock Lock(&SessionLock);

	FString SessionId = FGuid::NewGuid().ToString();

	FACPActiveSession NewSession;
	NewSession.Metadata.SessionId = SessionId;
	NewSession.Metadata.AgentName = AgentName;
	NewSession.Metadata.Title = TEXT("New conversation");
	NewSession.Metadata.CreatedAt = FDateTime::Now();
	NewSession.Metadata.LastModifiedAt = NewSession.Metadata.CreatedAt;
	NewSession.Metadata.MessageCount = 0;
	NewSession.Metadata.ModelId = ModelId;
	NewSession.bIsConnected = false;

	ActiveSessions.Add(SessionId, NewSession);

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPSessionManager: Created session %s for agent %s"), *SessionId, *AgentName);

	AsyncTask(ENamedThreads::GameThread, [this, SessionId, Metadata = NewSession.Metadata]() {
		OnSessionCreated.Broadcast(SessionId, Metadata);
		BroadcastActiveSessionsChanged();
	});

	return SessionId;
}

bool FACPSessionManager::CloseSession(const FString& SessionId)
{
	FScopeLock Lock(&SessionLock);

	FACPActiveSession* Session = ActiveSessions.Find(SessionId);
	if (!Session)
	{
		return false;
	}

	ActiveSessions.Remove(SessionId);

	if (CurrentSessionId == SessionId)
	{
		CurrentSessionId.Empty();

		for (const auto& Pair : ActiveSessions)
		{
			CurrentSessionId = Pair.Key;
			break;
		}
	}

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPSessionManager: Closed session %s"), *SessionId);

	AsyncTask(ENamedThreads::GameThread, [this, SessionId]() {
		OnSessionClosed.Broadcast(SessionId);
		BroadcastActiveSessionsChanged();
	});

	return true;
}

bool FACPSessionManager::ResumeSession(const FString& SessionId)
{
	FScopeLock Lock(&SessionLock);

	if (ActiveSessions.Contains(SessionId))
	{
		return SwitchToSession(SessionId);
	}

	// Create empty active session — messages will arrive via ACP session/load replay
	FACPActiveSession Active;
	Active.Metadata.SessionId = SessionId;
	Active.Metadata.CreatedAt = FDateTime::Now();
	Active.Metadata.LastModifiedAt = Active.Metadata.CreatedAt;
	Active.bIsConnected = false;
	Active.bIsLoadingHistory = true;

	ActiveSessions.Add(SessionId, Active);

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPSessionManager: Resuming session %s (loading history from agent)"), *SessionId);

	AsyncTask(ENamedThreads::GameThread, [this, SessionId, Metadata = Active.Metadata]() {
		OnSessionCreated.Broadcast(SessionId, Metadata);
		BroadcastActiveSessionsChanged();
	});

	return SwitchToSession(SessionId);
}

bool FACPSessionManager::SwitchToSession(const FString& SessionId)
{
	FScopeLock Lock(&SessionLock);

	if (!ActiveSessions.Contains(SessionId))
	{
		return false;
	}

	if (CurrentSessionId == SessionId)
	{
		return true;
	}

	FString OldSessionId = CurrentSessionId;
	CurrentSessionId = SessionId;

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPSessionManager: Switched from session %s to %s"), *OldSessionId, *SessionId);

	AsyncTask(ENamedThreads::GameThread, [this, OldSessionId, SessionId]() {
		OnSessionSwitched.Broadcast(OldSessionId, SessionId);
	});

	return true;
}

// ── Session Access ───────────────────────────────────────────────────

FACPActiveSession* FACPSessionManager::GetActiveSession(const FString& SessionId)
{
	FScopeLock Lock(&SessionLock);
	return ActiveSessions.Find(SessionId);
}

const FACPActiveSession* FACPSessionManager::GetActiveSession(const FString& SessionId) const
{
	FScopeLock Lock(&SessionLock);
	return ActiveSessions.Find(SessionId);
}

FACPActiveSession* FACPSessionManager::GetCurrentSession()
{
	if (CurrentSessionId.IsEmpty())
	{
		return nullptr;
	}
	return GetActiveSession(CurrentSessionId);
}

TArray<FString> FACPSessionManager::GetActiveSessionIds() const
{
	FScopeLock Lock(&SessionLock);
	TArray<FString> Ids;
	ActiveSessions.GetKeys(Ids);
	return Ids;
}

int32 FACPSessionManager::GetActiveSessionCount() const
{
	FScopeLock Lock(&SessionLock);
	return ActiveSessions.Num();
}

// ── Message Management ───────────────────────────────────────────────

void FACPSessionManager::AddUserMessage(const FString& SessionId, const FString& Message, const TArray<FACPMessageContext>& Contexts)
{
	FScopeLock Lock(&SessionLock);

	FACPActiveSession* Session = ActiveSessions.Find(SessionId);
	if (!Session)
	{
		return;
	}

	FACPChatMessage UserMsg;
	UserMsg.Role = EACPMessageRole::User;
	UserMsg.Timestamp = FDateTime::Now();
	UserMsg.bIsStreaming = false;
	UserMsg.Contexts = Contexts;

	FACPContentBlock TextBlock;
	TextBlock.Type = EACPContentBlockType::Text;
	TextBlock.Text = Message;
	TextBlock.Timestamp = UserMsg.Timestamp;
	UserMsg.ContentBlocks.Add(TextBlock);

	Session->Messages.Add(UserMsg);
	Session->Metadata.MessageCount = Session->Messages.Num();
	Session->Metadata.LastModifiedAt = FDateTime::Now();

	if (!Session->Metadata.bHasCustomTitle
		&& (Session->Metadata.Title == TEXT("New conversation") || Session->Metadata.Title.IsEmpty()))
	{
		Session->Metadata.Title = GenerateTitleFromMessage(Message);
	}

	FACPChatMessage MessageCopy = UserMsg;

	AsyncTask(ENamedThreads::GameThread, [this, SessionId, MessageCopy]() {
		OnSessionMessageAdded.Broadcast(SessionId, MessageCopy);
	});
}

int32 FACPSessionManager::StartAssistantMessage(const FString& SessionId)
{
	FScopeLock Lock(&SessionLock);

	FACPActiveSession* Session = ActiveSessions.Find(SessionId);
	if (!Session)
	{
		return INDEX_NONE;
	}

	FACPChatMessage AssistantMsg;
	AssistantMsg.Role = EACPMessageRole::Assistant;
	AssistantMsg.Timestamp = FDateTime::Now();
	AssistantMsg.bIsStreaming = true;

	Session->Messages.Add(AssistantMsg);
	Session->CurrentStreamingMessageIndex = Session->Messages.Num() - 1;

	return Session->CurrentStreamingMessageIndex;
}

void FACPSessionManager::AppendContentBlock(const FString& SessionId, int32 MessageIndex, const FACPContentBlock& Block)
{
	FScopeLock Lock(&SessionLock);

	FACPActiveSession* Session = ActiveSessions.Find(SessionId);
	if (!Session || !Session->Messages.IsValidIndex(MessageIndex))
	{
		return;
	}

	TArray<FACPContentBlock>& Blocks = Session->Messages[MessageIndex].ContentBlocks;

	// If a block with the same ToolCallId and type already exists, update it in-place
	if (!Block.ToolCallId.IsEmpty())
	{
		for (int32 i = Blocks.Num() - 1; i >= 0; --i)
		{
			FACPContentBlock& Existing = Blocks[i];
			if (Existing.ToolCallId == Block.ToolCallId && Existing.Type == Block.Type)
			{
				if (Block.Type == EACPContentBlockType::ToolCall)
				{
					if (!Block.ToolArguments.IsEmpty())
					{
						Existing.ToolArguments = Block.ToolArguments;
					}
					if (!Block.ToolName.IsEmpty())
					{
						Existing.ToolName = Block.ToolName;
					}
					if (!Block.ParentToolCallId.IsEmpty())
					{
						Existing.ParentToolCallId = Block.ParentToolCallId;
					}
				}
				else if (Block.Type == EACPContentBlockType::ToolResult)
				{
					if (!Block.ToolResultContent.IsEmpty())
					{
						Existing.ToolResultContent = Block.ToolResultContent;
					}
					Existing.bToolSuccess = Block.bToolSuccess;
					if (Block.ToolResultImages.Num() > 0)
					{
						Existing.ToolResultImages = Block.ToolResultImages;
					}
					if (!Block.ParentToolCallId.IsEmpty())
					{
						Existing.ParentToolCallId = Block.ParentToolCallId;
					}
				}
				Existing.bIsStreaming = Block.bIsStreaming;
				Existing.Timestamp = Block.Timestamp;

				FACPChatMessage MessageCopy = Session->Messages[MessageIndex];
				AsyncTask(ENamedThreads::GameThread, [this, SessionId, MessageIndex, MessageCopy]() {
					OnSessionMessageUpdated.Broadcast(SessionId, MessageIndex, MessageCopy);
				});
				return;
			}
		}
	}

	Blocks.Add(Block);

	FACPChatMessage MessageCopy = Session->Messages[MessageIndex];

	AsyncTask(ENamedThreads::GameThread, [this, SessionId, MessageIndex, MessageCopy]() {
		OnSessionMessageUpdated.Broadcast(SessionId, MessageIndex, MessageCopy);
	});
}

void FACPSessionManager::AppendStreamingText(const FString& SessionId, int32 MessageIndex, EACPContentBlockType BlockType, const FString& TextChunk)
{
	FScopeLock Lock(&SessionLock);

	FACPActiveSession* Session = ActiveSessions.Find(SessionId);
	if (!Session || !Session->Messages.IsValidIndex(MessageIndex))
	{
		return;
	}

	FACPChatMessage& Message = Session->Messages[MessageIndex];

	// Only append to the LAST block if it matches type and is streaming
	FACPContentBlock* ExistingBlock = nullptr;
	if (Message.ContentBlocks.Num() > 0)
	{
		FACPContentBlock& Last = Message.ContentBlocks.Last();
		if (Last.Type == BlockType && Last.bIsStreaming)
		{
			ExistingBlock = &Last;
		}
	}

	if (ExistingBlock)
	{
		ExistingBlock->Text += TextChunk;
	}
	else
	{
		FACPContentBlock NewBlock;
		NewBlock.Type = BlockType;
		NewBlock.Text = TextChunk;
		NewBlock.bIsStreaming = true;
		NewBlock.Timestamp = FDateTime::Now();
		Message.ContentBlocks.Add(NewBlock);
	}
}

void FACPSessionManager::UpdateMessage(const FString& SessionId, int32 MessageIndex, const FACPChatMessage& Message)
{
	FScopeLock Lock(&SessionLock);

	FACPActiveSession* Session = ActiveSessions.Find(SessionId);
	if (!Session || !Session->Messages.IsValidIndex(MessageIndex))
	{
		return;
	}

	Session->Messages[MessageIndex] = Message;
}

void FACPSessionManager::FinishMessage(const FString& SessionId, int32 MessageIndex)
{
	FScopeLock Lock(&SessionLock);

	FACPActiveSession* Session = ActiveSessions.Find(SessionId);
	if (!Session || !Session->Messages.IsValidIndex(MessageIndex))
	{
		return;
	}

	FACPChatMessage& Message = Session->Messages[MessageIndex];
	Message.bIsStreaming = false;

	for (FACPContentBlock& Block : Message.ContentBlocks)
	{
		Block.bIsStreaming = false;
	}

	Session->Metadata.MessageCount = Session->Messages.Num();
	Session->Metadata.LastModifiedAt = FDateTime::Now();
	Session->CurrentStreamingMessageIndex = INDEX_NONE;

	FACPChatMessage MessageCopy = Message;

	AsyncTask(ENamedThreads::GameThread, [this, SessionId, MessageIndex, MessageCopy]() {
		OnSessionMessageUpdated.Broadcast(SessionId, MessageIndex, MessageCopy);
	});
}

void FACPSessionManager::ClearSessionMessages(const FString& SessionId)
{
	FScopeLock Lock(&SessionLock);

	FACPActiveSession* Session = ActiveSessions.Find(SessionId);
	if (!Session)
	{
		return;
	}

	Session->Messages.Empty();
	Session->Metadata.MessageCount = 0;
	Session->Metadata.Title = TEXT("New conversation");
	Session->Metadata.LastModifiedAt = FDateTime::Now();
	Session->CurrentStreamingMessageIndex = INDEX_NONE;
}

// ── Session Metadata ─────────────────────────────────────────────────

void FACPSessionManager::UpdateSessionTitle(const FString& SessionId, const FString& NewTitle)
{
	FScopeLock Lock(&SessionLock);

	FACPActiveSession* Session = ActiveSessions.Find(SessionId);
	if (Session && !Session->Metadata.bHasCustomTitle)
	{
		Session->Metadata.Title = NewTitle;
		Session->Metadata.LastModifiedAt = FDateTime::Now();
	}
}

void FACPSessionManager::SetCustomTitle(const FString& SessionId, const FString& NewTitle)
{
	FScopeLock Lock(&SessionLock);

	FACPActiveSession* Session = ActiveSessions.Find(SessionId);
	if (Session)
	{
		Session->Metadata.Title = NewTitle;
		Session->Metadata.bHasCustomTitle = true;

		// Persist under both Unreal GUID and agent session ID
		PersistedCustomTitles.Add(SessionId, NewTitle);
		if (!Session->Metadata.AgentSessionId.IsEmpty())
		{
			PersistedCustomTitles.Add(Session->Metadata.AgentSessionId, NewTitle);
		}
		SaveCustomTitles();
	}
}

void FACPSessionManager::SetSessionExternalId(const FString& SessionId, const FString& ExternalId)
{
	FScopeLock Lock(&SessionLock);
	if (FACPActiveSession* Session = ActiveSessions.Find(SessionId))
	{
		Session->Metadata.AgentSessionId = ExternalId;

		// If this session has a persisted custom title, also store it under the agent ID
		if (const FString* Title = PersistedCustomTitles.Find(SessionId))
		{
			PersistedCustomTitles.Add(ExternalId, *Title);
			Session->Metadata.Title = *Title;
			Session->Metadata.bHasCustomTitle = true;
			SaveCustomTitles();
		}
	}
}

void FACPSessionManager::SetSessionConnected(const FString& SessionId, bool bConnected)
{
	FScopeLock Lock(&SessionLock);

	FACPActiveSession* Session = ActiveSessions.Find(SessionId);
	if (Session)
	{
		Session->bIsConnected = bConnected;
	}
}

void FACPSessionManager::SetSessionLoadingHistory(const FString& SessionId, bool bLoading)
{
	FScopeLock Lock(&SessionLock);

	FACPActiveSession* Session = ActiveSessions.Find(SessionId);
	if (Session)
	{
		Session->bIsLoadingHistory = bLoading;
	}
}

void FACPSessionManager::BroadcastActiveSessionsChanged()
{
	TArray<FString> Ids = GetActiveSessionIds();
	OnActiveSessionsChanged.Broadcast(Ids);
}

// ── Custom Title Persistence ──────────────────────────────────────────

const FString* FACPSessionManager::GetPersistedCustomTitle(const FString& SessionId) const
{
	return PersistedCustomTitles.Find(SessionId);
}

FString FACPSessionManager::GetCustomTitlesPath() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AgentIntegrationKit"), TEXT("custom_titles.json"));
}

void FACPSessionManager::LoadCustomTitles()
{
	const FString Path = GetCustomTitlesPath();
	FString JsonStr;
	if (!FFileHelper::LoadFileToString(JsonStr, *Path))
	{
		return;
	}

	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
	{
		for (const auto& Pair : JsonObj->Values)
		{
			FString Title;
			if (Pair.Value->TryGetString(Title))
			{
				PersistedCustomTitles.Add(Pair.Key, Title);
			}
		}
	}

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("Loaded %d persisted custom titles"), PersistedCustomTitles.Num());
}

void FACPSessionManager::SaveCustomTitles()
{
	TSharedRef<FJsonObject> JsonObj = MakeShared<FJsonObject>();
	for (const auto& Pair : PersistedCustomTitles)
	{
		JsonObj->SetStringField(Pair.Key, Pair.Value);
	}

	FString JsonStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);
	FJsonSerializer::Serialize(JsonObj, Writer);

	const FString Path = GetCustomTitlesPath();
	const FString Dir = FPaths::GetPath(Path);
	IFileManager::Get().MakeDirectory(*Dir, true);
	FFileHelper::SaveStringToFile(JsonStr, *Path);
}
