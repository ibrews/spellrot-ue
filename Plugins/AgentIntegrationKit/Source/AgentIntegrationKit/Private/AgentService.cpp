// Copyright 2026 Betide Studio. All Rights Reserved.

#include "AgentService.h"
#include "ACPAgentManager.h"
#include "ACPSessionManager.h"
#include "ACPSettings.h"
#include "ACPRegistryClient.h"
#include "ACPClaudeCodeHistoryReader.h"
#include "ACPCodexHistoryReader.h"
#include "ACPGeminiHistoryReader.h"
#include "ACPCopilotHistoryReader.h"
#include "ACPTerminalResumeCommand.h"
#include "ChatGatewayClient.h"
#include "AIKAnalytics.h"
#include "Json.h"
#include "Misc/App.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogAgentService, Log, All);

// ── Singleton ──────────────────────────────────────────────────────────

FAgentService& FAgentService::Get()
{
	static FAgentService Instance;
	return Instance;
}

FAgentService::~FAgentService()
{
	Shutdown();
}

// ── Lifecycle ──────────────────────────────────────────────────────────

void FAgentService::Initialize()
{
	if (bInitialized) return;
	bInitialized = true;

	UE_LOG(LogAgentService, Log, TEXT("Initializing agent service..."));

	BindDelegates();
	DiscoverAgents();

	UE_LOG(LogAgentService, Log, TEXT("Agent service initialized"));
}

void FAgentService::Shutdown()
{
	if (!bInitialized) return;

	UnbindDelegates();
	StreamingMessageIndices.Empty();
	PreviousSessionStates.Empty();
	bInitialized = false;
}

// ── Agent Discovery (proactive, no WebUI needed) ───────────────────────

void FAgentService::DiscoverAgents()
{
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	TArray<FACPAgentConfig> Configs = AgentMgr.GetAllAgentConfigs();

	for (const FACPAgentConfig& Config : Configs)
	{
		if (Config.Status != EACPAgentStatus::Available) continue;
		if (AgentMgr.IsChatGatewayAgent(Config.AgentName)) continue;
		if (AgentMgr.IsConnectedToAgent(Config.AgentName)) continue;

		UE_LOG(LogAgentService, Log, TEXT("Proactively connecting agent: %s"), *Config.AgentName);
		AgentMgr.ConnectToAgent(Config.AgentName);
	}
}

// ── JSON Helpers ───────────────────────────────────────────────────────

FString FAgentService::JsonToCompactString(const TSharedRef<FJsonObject>& Obj)
{
	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Obj, Writer);
	return Out;
}

FString FAgentService::JsonArrayToCompactString(const TArray<TSharedPtr<FJsonValue>>& Arr)
{
	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Arr, Writer);
	return Out;
}

// ── Local History ──────────────────────────────────────────────────────

TArray<FACPRemoteSessionEntry> FAgentService::GetLocalHistorySessions(const FString& AgentName, const FString& WorkDir)
{
	if (AgentName == TEXT("Gemini CLI") || AgentName == TEXT("Gemini"))
	{
		return FACPGeminiHistoryReader::ListSessions(FPaths::ConvertRelativePathToFull(WorkDir));
	}
	if (AgentName == TEXT("Copilot CLI") || AgentName == TEXT("GitHub Copilot"))
	{
		return FACPCopilotHistoryReader::ListSessions(FPaths::ConvertRelativePathToFull(WorkDir));
	}
	if (AgentName == TEXT("Codex CLI") || AgentName == TEXT("Codex"))
	{
		return FACPCodexHistoryReader::ListSessions(FPaths::ConvertRelativePathToFull(WorkDir));
	}
	return {};
}

bool FAgentService::IsLaunchResumeAgent(const FString& AgentName)
{
	return AgentName == TEXT("Gemini CLI") || AgentName == TEXT("Copilot CLI");
}

bool FAgentService::ReadMessagesFromDisk(const FString& SessionId, TArray<FACPChatMessage>& OutMessages) const
{
	const FString WorkDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	// Try Claude Code JSONL
	if (FACPClaudeCodeHistoryReader::SessionExists(SessionId, WorkDir))
	{
		FString JsonlPath = FACPClaudeCodeHistoryReader::GetSessionJsonlPath(SessionId, WorkDir);
		if (FACPClaudeCodeHistoryReader::ParseSessionJsonl(JsonlPath, OutMessages))
		{
			return true;
		}
	}

	// Try Gemini
	if (FACPGeminiHistoryReader::ParseSession(SessionId, OutMessages))
	{
		return true;
	}

	// Try Copilot
	if (FACPCopilotHistoryReader::ParseSession(SessionId, OutMessages))
	{
		return true;
	}

	// Try Codex CLI (rollout files in ~/.codex/sessions/)
	{
		FString CodexPath = FACPCodexHistoryReader::GetSessionJsonlPath(SessionId, WorkDir);
		if (!CodexPath.IsEmpty() && FACPCodexHistoryReader::ParseSessionJsonl(CodexPath, OutMessages))
		{
			return true;
		}
	}

	return false;
}

FString FAgentService::FindAgentForSession(const FString& SessionId) const
{
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	const FString WorkDir = FPaths::ProjectDir();

	for (const FString& AgentName : AgentMgr.GetAvailableAgentNames())
	{
		// Check ACP cached lists
		for (const FACPRemoteSessionEntry& Entry : AgentMgr.GetCachedSessionList(AgentName))
		{
			if (Entry.SessionId == SessionId) return AgentName;
		}

		// Check local history
		for (const FACPRemoteSessionEntry& Entry : GetLocalHistorySessions(AgentName, WorkDir))
		{
			if (Entry.SessionId == SessionId) return AgentName;
		}
	}

	return FString();
}

// ── Streaming State ────────────────────────────────────────────────────

void FAgentService::FinalizeStreamingMessage(const FString& SessionId)
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

// ── Delegate Binding (message persistence + event forwarding) ──────────

void FAgentService::BindDelegates()
{
	UnbindDelegates();

	FACPAgentManager& AgentMgr = FACPAgentManager::Get();

	// ── OnAgentMessage: persist streaming chunks + forward to clients ──
	AgentMessageHandle = AgentMgr.OnAgentMessage.AddLambda(
		[this](const FString& SessionId, const FString& AgentName, const FACPSessionUpdate& Update)
		{
			// ── 1. Persist to FACPSessionManager ──
			if (Update.UpdateType != EACPUpdateType::UsageUpdate && Update.UpdateType != EACPUpdateType::Plan)
			{
				FACPSessionManager& SessionMgr = FACPSessionManager::Get();

				if (Update.UpdateType == EACPUpdateType::UserMessageChunk)
				{
					// History replay: user message arrived. Finalize any in-progress assistant message.
					FinalizeStreamingMessage(SessionId);
					SessionMgr.AddUserMessage(SessionId, Update.TextChunk);
				}
				else
				{
					// Ensure an assistant message is active for this session
					int32* MsgIdxPtr = StreamingMessageIndices.Find(SessionId);
					if (!MsgIdxPtr || *MsgIdxPtr == INDEX_NONE)
					{
						int32 NewIdx = SessionMgr.StartAssistantMessage(SessionId);
						StreamingMessageIndices.Add(SessionId, NewIdx);
						MsgIdxPtr = StreamingMessageIndices.Find(SessionId);
					}

					int32 MsgIdx = *MsgIdxPtr;
					bool bFinish = false;

					switch (Update.UpdateType)
					{
					case EACPUpdateType::AgentMessageChunk:
						SessionMgr.AppendStreamingText(SessionId, MsgIdx,
							Update.bIsSystemStatus ? EACPContentBlockType::System : EACPContentBlockType::Text,
							Update.TextChunk);
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
							bFinish = true;
						}
						break;

					default:
						break;
					}

					if (bFinish)
					{
						SessionMgr.FinishMessage(SessionId, MsgIdx);
						*MsgIdxPtr = INDEX_NONE;
					}
				}
			}

			// ── 2. Serialize and broadcast to all clients ──
			FString UpdateJson = SerializeUpdate(AgentName, Update);
			OnMessage.Broadcast(SessionId, AgentName, UpdateJson);
		});

	// ── OnAgentStateChanged: finalize streaming + broadcast ──
	AgentStateHandle = AgentMgr.OnAgentStateChanged.AddLambda(
		[this](const FString& SessionId, const FString& AgentName, EACPClientState State, const FString& Message)
		{
			// Finalize streaming message when agent returns to ready
			if (State == EACPClientState::Ready || State == EACPClientState::InSession)
			{
				FinalizeStreamingMessage(SessionId);
			}

			PreviousSessionStates.FindOrAdd(SessionId) = State;

			// Map state to string
			FString StateStr;
			switch (State)
			{
			case EACPClientState::Disconnected:   StateStr = TEXT("disconnected"); break;
			case EACPClientState::Connecting:      StateStr = TEXT("connecting"); break;
			case EACPClientState::Ready:           StateStr = TEXT("ready"); break;
			case EACPClientState::InSession:       StateStr = TEXT("in_session"); break;
			case EACPClientState::Prompting:       StateStr = TEXT("prompting"); break;
			default:                               StateStr = FString::Printf(TEXT("state_%d"), static_cast<int32>(State)); break;
			}

			OnStateChanged.Broadcast(SessionId, AgentName, StateStr, Message);
		});

	// ── OnAgentError: finalize streaming + broadcast ──
	AgentErrorHandle = AgentMgr.OnAgentError.AddLambda(
		[this](const FString& SessionId, const FString& AgentName, int32 ErrorCode, const FString& ErrorMessage)
		{
			FinalizeStreamingMessage(SessionId);

			TSharedRef<FJsonObject> ErrorJson = MakeShared<FJsonObject>();
			ErrorJson->SetStringField(TEXT("agentName"), AgentName);
			ErrorJson->SetStringField(TEXT("type"), TEXT("error"));
			ErrorJson->SetStringField(TEXT("errorMessage"), ErrorMessage);
			ErrorJson->SetNumberField(TEXT("errorCode"), ErrorCode);
			OnMessage.Broadcast(SessionId, AgentName, JsonToCompactString(ErrorJson));
		});

	// ── OnPermissionRequest: serialize + broadcast ──
	PermissionRequestHandle = AgentMgr.OnAgentPermissionRequest.AddLambda(
		[this](const FString& SessionId, const FString& AgentName, const FACPPermissionRequest& Request)
		{
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("agentName"), AgentName);
			Json->SetNumberField(TEXT("requestId"), Request.RequestId);
			Json->SetStringField(TEXT("toolName"), Request.ToolCall.Title);
			Json->SetStringField(TEXT("toolCallId"), Request.ToolCall.ToolCallId);
			Json->SetStringField(TEXT("toolInput"), Request.ToolCall.RawInput);
			Json->SetBoolField(TEXT("isAskUserQuestion"), Request.bIsAskUserQuestion);

			TArray<TSharedPtr<FJsonValue>> OptionsArr;
			for (const FACPPermissionOption& Opt : Request.Options)
			{
				TSharedRef<FJsonObject> OptJson = MakeShared<FJsonObject>();
				OptJson->SetStringField(TEXT("id"), Opt.OptionId);
				OptJson->SetStringField(TEXT("name"), Opt.Name);
				OptJson->SetStringField(TEXT("kind"), Opt.Kind);
				OptionsArr.Add(MakeShared<FJsonValueObject>(OptJson));
			}
			Json->SetArrayField(TEXT("options"), OptionsArr);

			if (Request.bIsAskUserQuestion)
			{
				TArray<TSharedPtr<FJsonValue>> QuestionsArr;
				for (const FACPQuestion& Q : Request.Questions)
				{
					TSharedRef<FJsonObject> QJson = MakeShared<FJsonObject>();
					QJson->SetStringField(TEXT("question"), Q.Question);
					QJson->SetStringField(TEXT("header"), Q.Header);
					QJson->SetBoolField(TEXT("multiSelect"), Q.bMultiSelect);
					TArray<TSharedPtr<FJsonValue>> QOptsArr;
					for (const FACPQuestionOption& QOpt : Q.Options)
					{
						TSharedRef<FJsonObject> QOptJson = MakeShared<FJsonObject>();
						QOptJson->SetStringField(TEXT("label"), QOpt.Label);
						QOptJson->SetStringField(TEXT("description"), QOpt.Description);
						QOptsArr.Add(MakeShared<FJsonValueObject>(QOptJson));
					}
					QJson->SetArrayField(TEXT("options"), QOptsArr);
					QuestionsArr.Add(MakeShared<FJsonValueObject>(QJson));
				}
				Json->SetArrayField(TEXT("questions"), QuestionsArr);
			}

			OnPermissionRequest.Broadcast(SessionId, JsonToCompactString(Json));
		});

	// ── OnModelsAvailable ──
	ModelsAvailableHandle = AgentMgr.OnAgentModelsAvailable.AddLambda(
		[this](const FString& SessionId, const FString& AgentName, const FACPSessionModelState& ModelState)
		{
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("currentModelId"), ModelState.CurrentModelId);
			TArray<TSharedPtr<FJsonValue>> ModelsArr;
			for (const FACPModelInfo& M : ModelState.AvailableModels)
			{
				TSharedRef<FJsonObject> MJson = MakeShared<FJsonObject>();
				MJson->SetStringField(TEXT("id"), M.ModelId);
				MJson->SetStringField(TEXT("name"), M.Name);
				MJson->SetStringField(TEXT("description"), M.Description);
				ModelsArr.Add(MakeShared<FJsonValueObject>(MJson));
			}
			Json->SetArrayField(TEXT("models"), ModelsArr);
			OnModelsAvailable.Broadcast(AgentName, JsonToCompactString(Json));
		});

	// ── OnModesAvailable ──
	ModesAvailableHandle = AgentMgr.OnAgentModesAvailable.AddLambda(
		[this](const FString& SessionId, const FString& AgentName, const FACPSessionModeState& ModeState)
		{
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("currentModeId"), ModeState.CurrentModeId);
			TArray<TSharedPtr<FJsonValue>> ModesArr;
			for (const FACPSessionMode& M : ModeState.AvailableModes)
			{
				TSharedRef<FJsonObject> MJson = MakeShared<FJsonObject>();
				MJson->SetStringField(TEXT("id"), M.ModeId);
				MJson->SetStringField(TEXT("name"), M.Name);
				MJson->SetStringField(TEXT("description"), M.Description);
				ModesArr.Add(MakeShared<FJsonValueObject>(MJson));
			}
			Json->SetArrayField(TEXT("modes"), ModesArr);
			OnModesAvailable.Broadcast(AgentName, JsonToCompactString(Json));
		});

	// ── OnModeChanged ──
	ModeChangedHandle = AgentMgr.OnAgentModeChanged.AddLambda(
		[this](const FString& SessionId, const FString& AgentName, const FString& ModeId)
		{
			OnModeChanged.Broadcast(AgentName, ModeId);
		});

	// ── OnPlanUpdate ──
	PlanUpdateHandle = AgentMgr.OnAgentPlanUpdate.AddLambda(
		[this](const FString& SessionId, const FString& AgentName, const FACPPlan& Plan)
		{
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> EntriesArr;
			int32 CompletedCount = 0;
			for (const FACPPlanEntry& E : Plan.Entries)
			{
				TSharedRef<FJsonObject> EJson = MakeShared<FJsonObject>();
				EJson->SetStringField(TEXT("content"), E.Content);
				EJson->SetStringField(TEXT("activeForm"), E.ActiveForm);
				FString StatusStr;
				switch (E.Status)
				{
				case EACPPlanEntryStatus::Pending:     StatusStr = TEXT("pending"); break;
				case EACPPlanEntryStatus::InProgress:   StatusStr = TEXT("in_progress"); break;
				case EACPPlanEntryStatus::Completed:    StatusStr = TEXT("completed"); CompletedCount++; break;
				default:                                StatusStr = TEXT("pending"); break;
				}
				EJson->SetStringField(TEXT("status"), StatusStr);
				EntriesArr.Add(MakeShared<FJsonValueObject>(EJson));
			}
			Json->SetArrayField(TEXT("entries"), EntriesArr);
			Json->SetNumberField(TEXT("completedCount"), CompletedCount);
			Json->SetNumberField(TEXT("totalCount"), Plan.Entries.Num());
			OnPlanUpdate.Broadcast(SessionId, JsonToCompactString(Json));
		});

	// ── OnCommandsAvailable ──
	CommandsAvailableHandle = AgentMgr.OnAgentCommandsAvailable.AddLambda(
		[this](const FString& SessionId, const FString& AgentName, const TArray<FACPSlashCommand>& Commands)
		{
			TArray<TSharedPtr<FJsonValue>> CmdsArr;
			for (const FACPSlashCommand& Cmd : Commands)
			{
				TSharedRef<FJsonObject> CmdJson = MakeShared<FJsonObject>();
				CmdJson->SetStringField(TEXT("name"), Cmd.Name);
				CmdJson->SetStringField(TEXT("description"), Cmd.Description);
				CmdsArr.Add(MakeShared<FJsonValueObject>(CmdJson));
			}
			OnCommandsAvailable.Broadcast(SessionId, JsonArrayToCompactString(CmdsArr));
		});

	// ── OnSessionListReceived ──
	SessionListHandle = AgentMgr.OnAgentSessionListReceived.AddLambda(
		[this](const FString& AgentName, const TArray<FACPRemoteSessionEntry>& Sessions)
		{
			TArray<TSharedPtr<FJsonValue>> SessionsArr;
			for (const FACPRemoteSessionEntry& S : Sessions)
			{
				TSharedRef<FJsonObject> SJson = MakeShared<FJsonObject>();
				SJson->SetStringField(TEXT("sessionId"), S.SessionId);
				SJson->SetStringField(TEXT("title"), S.Title);
				if (S.UpdatedAt.GetTicks() > 0)
				{
					SJson->SetStringField(TEXT("lastModifiedAt"), S.UpdatedAt.ToIso8601());
				}
				SessionsArr.Add(MakeShared<FJsonValueObject>(SJson));
			}
			OnSessionListUpdated.Broadcast(AgentName, JsonArrayToCompactString(SessionsArr));
		});

	UE_LOG(LogAgentService, Log, TEXT("Bound to agent manager delegates"));
}

void FAgentService::UnbindDelegates()
{
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();

	if (AgentMessageHandle.IsValid()) { AgentMgr.OnAgentMessage.Remove(AgentMessageHandle); AgentMessageHandle.Reset(); }
	if (AgentStateHandle.IsValid()) { AgentMgr.OnAgentStateChanged.Remove(AgentStateHandle); AgentStateHandle.Reset(); }
	if (AgentErrorHandle.IsValid()) { AgentMgr.OnAgentError.Remove(AgentErrorHandle); AgentErrorHandle.Reset(); }
	if (PermissionRequestHandle.IsValid()) { AgentMgr.OnAgentPermissionRequest.Remove(PermissionRequestHandle); PermissionRequestHandle.Reset(); }
	if (ModesAvailableHandle.IsValid()) { AgentMgr.OnAgentModesAvailable.Remove(ModesAvailableHandle); ModesAvailableHandle.Reset(); }
	if (ModeChangedHandle.IsValid()) { AgentMgr.OnAgentModeChanged.Remove(ModeChangedHandle); ModeChangedHandle.Reset(); }
	if (ModelsAvailableHandle.IsValid()) { AgentMgr.OnAgentModelsAvailable.Remove(ModelsAvailableHandle); ModelsAvailableHandle.Reset(); }
	if (CommandsAvailableHandle.IsValid()) { AgentMgr.OnAgentCommandsAvailable.Remove(CommandsAvailableHandle); CommandsAvailableHandle.Reset(); }
	if (PlanUpdateHandle.IsValid()) { AgentMgr.OnAgentPlanUpdate.Remove(PlanUpdateHandle); PlanUpdateHandle.Reset(); }
	if (SessionListHandle.IsValid()) { AgentMgr.OnAgentSessionListReceived.Remove(SessionListHandle); SessionListHandle.Reset(); }
}

// ── Update Serialization ───────────────────────────────────────────────

FString FAgentService::SerializeUpdate(const FString& AgentName, const FACPSessionUpdate& Update)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("agentName"), AgentName);

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
	Json->SetStringField(TEXT("type"), TypeStr);
	Json->SetStringField(TEXT("text"), Update.TextChunk);

	if (Update.bIsSystemStatus)
	{
		Json->SetStringField(TEXT("systemStatus"), Update.SystemStatus);
	}

	if (!Update.ToolCallId.IsEmpty())
	{
		Json->SetStringField(TEXT("toolCallId"), Update.ToolCallId);
		Json->SetStringField(TEXT("toolName"), Update.ToolName);
		Json->SetStringField(TEXT("toolArguments"), Update.ToolArguments);
		Json->SetStringField(TEXT("toolResult"), Update.ToolResult);
		Json->SetBoolField(TEXT("toolSuccess"), Update.bToolSuccess);
		if (!Update.ParentToolCallId.IsEmpty())
		{
			Json->SetStringField(TEXT("parentToolCallId"), Update.ParentToolCallId);
		}

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
			Json->SetArrayField(TEXT("images"), ImagesArr);
		}
	}

	if (!Update.ErrorMessage.IsEmpty())
	{
		Json->SetStringField(TEXT("errorMessage"), Update.ErrorMessage);
		Json->SetNumberField(TEXT("errorCode"), Update.ErrorCode);
	}

	if (Update.UpdateType == EACPUpdateType::UsageUpdate)
	{
		const FACPUsageData& U = Update.Usage;
		Json->SetNumberField(TEXT("inputTokens"), U.InputTokens);
		Json->SetNumberField(TEXT("outputTokens"), U.OutputTokens);
		Json->SetNumberField(TEXT("totalTokens"), U.TotalTokens);
		Json->SetNumberField(TEXT("cacheReadTokens"), U.CacheReadTokens);
		Json->SetNumberField(TEXT("cacheCreationTokens"), U.CacheCreationTokens);
		Json->SetNumberField(TEXT("reasoningTokens"), U.ReasoningTokens);
		Json->SetNumberField(TEXT("costAmount"), U.CostAmount);
		Json->SetStringField(TEXT("costCurrency"), U.CostCurrency);
		Json->SetNumberField(TEXT("turnCostUSD"), U.TurnCostUSD);
		Json->SetNumberField(TEXT("contextUsed"), U.ContextUsed);
		Json->SetNumberField(TEXT("contextSize"), U.ContextSize);
		Json->SetNumberField(TEXT("numTurns"), U.NumTurns);
		Json->SetNumberField(TEXT("durationMs"), U.DurationMs);

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
			Json->SetArrayField(TEXT("modelUsage"), ModelArr);
		}
	}

	return JsonToCompactString(Json);
}

// ── Message Serialization ──────────────────────────────────────────────

FString FAgentService::SerializeMessages(const TArray<FACPChatMessage>& Messages)
{
	TArray<TSharedPtr<FJsonValue>> MessagesArr;

	for (const FACPChatMessage& Msg : Messages)
	{
		TSharedRef<FJsonObject> MsgObj = MakeShared<FJsonObject>();

		FString RoleStr;
		switch (Msg.Role)
		{
		case EACPMessageRole::User:      RoleStr = TEXT("user"); break;
		case EACPMessageRole::Assistant:  RoleStr = TEXT("assistant"); break;
		case EACPMessageRole::System:     RoleStr = TEXT("system"); break;
		default:                          RoleStr = TEXT("assistant"); break;
		}
		MsgObj->SetStringField(TEXT("role"), RoleStr);
		MsgObj->SetStringField(TEXT("timestamp"), Msg.Timestamp.ToIso8601());
		MsgObj->SetBoolField(TEXT("isStreaming"), Msg.bIsStreaming);

		TArray<TSharedPtr<FJsonValue>> BlocksArr;
		for (const FACPContentBlock& Block : Msg.ContentBlocks)
		{
			TSharedRef<FJsonObject> BlockObj = MakeShared<FJsonObject>();

			FString BlockTypeStr;
			switch (Block.Type)
			{
			case EACPContentBlockType::Text:       BlockTypeStr = TEXT("text"); break;
			case EACPContentBlockType::Thought:    BlockTypeStr = TEXT("thought"); break;
			case EACPContentBlockType::ToolCall:   BlockTypeStr = TEXT("tool_call"); break;
			case EACPContentBlockType::ToolResult: BlockTypeStr = TEXT("tool_result"); break;
			case EACPContentBlockType::Image:      BlockTypeStr = TEXT("image"); break;
			case EACPContentBlockType::Error:      BlockTypeStr = TEXT("error"); break;
			case EACPContentBlockType::System:     BlockTypeStr = TEXT("system"); break;
			default:                               BlockTypeStr = TEXT("text"); break;
			}
			BlockObj->SetStringField(TEXT("type"), BlockTypeStr);
			BlockObj->SetStringField(TEXT("text"), Block.Text);
			BlockObj->SetBoolField(TEXT("isStreaming"), Block.bIsStreaming);

			if (!Block.ToolCallId.IsEmpty()) BlockObj->SetStringField(TEXT("toolCallId"), Block.ToolCallId);
			if (!Block.ToolName.IsEmpty()) BlockObj->SetStringField(TEXT("toolName"), Block.ToolName);
			if (!Block.ToolArguments.IsEmpty()) BlockObj->SetStringField(TEXT("toolArguments"), Block.ToolArguments);
			if (!Block.ToolResultContent.IsEmpty()) BlockObj->SetStringField(TEXT("toolResult"), Block.ToolResultContent);
			if (!Block.ParentToolCallId.IsEmpty()) BlockObj->SetStringField(TEXT("parentToolCallId"), Block.ParentToolCallId);

			BlocksArr.Add(MakeShared<FJsonValueObject>(BlockObj));
		}
		MsgObj->SetArrayField(TEXT("contentBlocks"), BlocksArr);
		MessagesArr.Add(MakeShared<FJsonValueObject>(MsgObj));
	}

	return JsonArrayToCompactString(MessagesArr);
}

// ── Public API ─────────────────────────────────────────────────────────

FString FAgentService::GetAgents() const
{
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	TArray<FACPAgentConfig> Configs = AgentMgr.GetAllAgentConfigs();
	TArray<TSharedPtr<FJsonValue>> Arr;

	for (const FACPAgentConfig& C : Configs)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), C.AgentName);
		Obj->SetStringField(TEXT("name"), C.AgentName);

		FString StatusStr;
		switch (C.Status)
		{
		case EACPAgentStatus::Available:     StatusStr = TEXT("available"); break;
		case EACPAgentStatus::NotInstalled:  StatusStr = TEXT("not_installed"); break;
		case EACPAgentStatus::MissingApiKey: StatusStr = TEXT("missing_key"); break;
		default:                             StatusStr = TEXT("unknown"); break;
		}
		Obj->SetStringField(TEXT("status"), StatusStr);
		Obj->SetStringField(TEXT("statusMessage"), C.StatusMessage);
		Obj->SetBoolField(TEXT("isBuiltIn"), C.bIsBuiltIn);
		Obj->SetBoolField(TEXT("isConnected"), AgentMgr.IsConnectedToAgent(C.AgentName));
		Obj->SetStringField(TEXT("registryId"), C.RegistryId);

		if (!C.RegistryId.IsEmpty())
		{
			if (const FACPRegistryAgent* Reg = FACPRegistryClient::Get().FindAgent(C.RegistryId))
			{
				Obj->SetStringField(TEXT("iconUrl"), Reg->IconUrl);
			}
		}
		Arr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	return JsonArrayToCompactString(Arr);
}

FString FAgentService::GetLastUsedAgent() const
{
	if (const UACPSettings* Settings = UACPSettings::Get())
	{
		return Settings->LastUsedAgentName;
	}
	return FString();
}

FString FAgentService::GetAllSessions() const
{
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	FACPSessionManager& SessionMgr = FACPSessionManager::Get();
	const FString WorkDir = FPaths::ProjectDir();

	TSet<FString> Seen;
	TArray<TSharedPtr<FJsonValue>> Arr;

	// Map active session agent IDs for deduplication
	TSet<FString> KnownAgentIds;
	TMap<FString, const FACPActiveSession*> AgentIdToActive;
	for (const FString& Id : SessionMgr.GetActiveSessionIds())
	{
		if (const FACPActiveSession* A = SessionMgr.GetActiveSession(Id))
		{
			if (!A->Metadata.AgentSessionId.IsEmpty())
			{
				KnownAgentIds.Add(A->Metadata.AgentSessionId);
				AgentIdToActive.Add(A->Metadata.AgentSessionId, A);
			}
		}
	}

	// All agent configs for registry ID lookup
	TArray<FACPAgentConfig> AllConfigs = AgentMgr.GetAllAgentConfigs();
	auto GetRegistryId = [&](const FString& AgentName) -> FString
	{
		for (const FACPAgentConfig& C : AllConfigs)
		{
			if (C.AgentName == AgentName) return C.RegistryId;
		}
		return FString();
	};

	// 1. Remote + local history sessions
	for (const FString& AgentName : AgentMgr.GetAvailableAgentNames())
	{
		TArray<FACPRemoteSessionEntry> Sessions = AgentMgr.GetCachedSessionList(AgentName);
		Sessions.Append(GetLocalHistorySessions(AgentName, WorkDir));

		for (const FACPRemoteSessionEntry& S : Sessions)
		{
			if (Seen.Contains(S.SessionId)) continue;

			// Deduplicate: if this is also an active session, update its title and skip
			if (KnownAgentIds.Contains(S.SessionId))
			{
				if (const FACPActiveSession** Ptr = AgentIdToActive.Find(S.SessionId))
				{
					if (!S.Title.IsEmpty())
					{
						SessionMgr.UpdateSessionTitle((*Ptr)->Metadata.SessionId, S.Title);
					}
				}
				continue;
			}

			Seen.Add(S.SessionId);

			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("sessionId"), S.SessionId);
			Obj->SetStringField(TEXT("agentName"), AgentName);
			Obj->SetStringField(TEXT("title"), S.Title);
			if (S.UpdatedAt.GetTicks() > 0)
			{
				Obj->SetStringField(TEXT("lastModifiedAt"), S.UpdatedAt.ToIso8601());
			}

			if (const FString* CustomTitle = SessionMgr.GetPersistedCustomTitle(S.SessionId))
			{
				Obj->SetStringField(TEXT("title"), *CustomTitle);
				Obj->SetBoolField(TEXT("hasCustomTitle"), true);
			}

			const FACPActiveSession* Active = SessionMgr.GetActiveSession(S.SessionId);
			Obj->SetBoolField(TEXT("isConnected"), Active && Active->bIsConnected);
			Obj->SetBoolField(TEXT("isActive"), Active != nullptr);
			Obj->SetStringField(TEXT("registryId"), GetRegistryId(AgentName));

			Arr.Add(MakeShared<FJsonValueObject>(Obj));
		}
	}

	// 2. Active sessions not in remote lists
	for (const FString& Id : SessionMgr.GetActiveSessionIds())
	{
		if (Seen.Contains(Id)) continue;
		Seen.Add(Id);

		const FACPActiveSession* A = SessionMgr.GetActiveSession(Id);
		if (!A) continue;

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("sessionId"), A->Metadata.SessionId);
		Obj->SetStringField(TEXT("agentName"), A->Metadata.AgentName);
		Obj->SetStringField(TEXT("title"), A->Metadata.Title);
		Obj->SetNumberField(TEXT("messageCount"), A->Metadata.MessageCount);
		if (A->Metadata.CreatedAt.GetTicks() > 0) Obj->SetStringField(TEXT("createdAt"), A->Metadata.CreatedAt.ToIso8601());
		if (A->Metadata.LastModifiedAt.GetTicks() > 0) Obj->SetStringField(TEXT("lastModifiedAt"), A->Metadata.LastModifiedAt.ToIso8601());
		Obj->SetBoolField(TEXT("isConnected"), A->bIsConnected);
		Obj->SetBoolField(TEXT("isActive"), true);
		Obj->SetBoolField(TEXT("hasCustomTitle"), A->Metadata.bHasCustomTitle);
		Obj->SetStringField(TEXT("registryId"), GetRegistryId(A->Metadata.AgentName));

		Arr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	return JsonArrayToCompactString(Arr);
}

FString FAgentService::GetSessionMessages(const FString& SessionId) const
{
	// 1. Try active session (messages in memory — current or recently resumed)
	FACPSessionManager& SessionMgr = FACPSessionManager::Get();
	if (const FACPActiveSession* Active = SessionMgr.GetActiveSession(SessionId))
	{
		if (Active->Messages.Num() > 0)
		{
			return SerializeMessages(Active->Messages);
		}
	}

	// 2. Try reading directly from disk (no ACP subprocess needed)
	TArray<FACPChatMessage> DiskMessages;
	if (ReadMessagesFromDisk(SessionId, DiskMessages))
	{
		return SerializeMessages(DiskMessages);
	}

	return TEXT("[]");
}

FString FAgentService::ResumeSession(const FString& SessionId)
{
	FACPSessionManager& SessionMgr = FACPSessionManager::Get();
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	// Already active?
	if (const FACPActiveSession* Existing = SessionMgr.GetActiveSession(SessionId))
	{
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("agentName"), Existing->Metadata.AgentName);
		Result->SetBoolField(TEXT("loading"), false);
		return JsonToCompactString(Result);
	}

	// Find owning agent
	FString AgentName = FindAgentForSession(SessionId);
	if (AgentName.IsEmpty())
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Session not found in any agent"));
		return JsonToCompactString(Result);
	}

	// Create active session
	if (!SessionMgr.ResumeSession(SessionId))
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Failed to create active session"));
		return JsonToCompactString(Result);
	}

	// Set metadata + pre-populate messages from disk
	FACPActiveSession* Session = SessionMgr.GetActiveSession(SessionId);
	if (Session)
	{
		Session->Metadata.AgentName = AgentName;
		Session->Metadata.AgentSessionId = SessionId;

		// Pre-populate from disk so GetSessionMessages returns immediately
		TArray<FACPChatMessage> DiskMessages;
		if (ReadMessagesFromDisk(SessionId, DiskMessages))
		{
			Session->Messages = MoveTemp(DiskMessages);
			Session->Metadata.MessageCount = Session->Messages.Num();
		}
		Session->bIsLoadingHistory = false;
	}

	// Register and connect
	AgentMgr.RegisterSession(SessionId, AgentName);

	bool bIsLocal = IsLaunchResumeAgent(AgentName);
	if (bIsLocal)
	{
		AgentMgr.DisconnectFromAgent(AgentName);
		AgentMgr.SetLaunchResumeSession(AgentName, SessionId);
	}

	if (!AgentMgr.IsConnectedToAgent(AgentName))
	{
		AgentMgr.ConnectToAgent(AgentName);
	}

	// For ACP agents: trigger session/load to restore agent context
	if (!bIsLocal)
	{
		TSharedPtr<FACPClient> Client = AgentMgr.GetClient(AgentName);
		if (Client.IsValid())
		{
			const EACPClientState State = Client->GetState();
			if (State == EACPClientState::Ready || State == EACPClientState::InSession)
			{
				Client->SetUnrealSessionId(SessionId);
				const FACPAgentCapabilities& Caps = Client->GetAgentCapabilities();
				if (Caps.bSupportsLoadSession)
					Client->LoadSession(SessionId, FPaths::ProjectDir());
				else if (Caps.bSupportsResumeSession)
					Client->ResumeSession(SessionId, FPaths::ProjectDir());
				else
					Client->NewSession(FPaths::ProjectDir());
			}
			else
			{
				AgentMgr.QueuePendingNewSession(AgentName, SessionId);
			}
		}
	}
	else
	{
		if (TSharedPtr<FACPClient> Client = AgentMgr.GetClient(AgentName))
		{
			Client->SetUnrealSessionId(SessionId);
		}
	}

	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("agentName"), AgentName);
	Result->SetBoolField(TEXT("loading"), false); // Messages pre-populated from disk
	return JsonToCompactString(Result);
}

FString FAgentService::CreateSession(const FString& AgentName)
{
	FACPSessionManager& SessionMgr = FACPSessionManager::Get();
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();

	FString SessionId = SessionMgr.CreateSession(AgentName);
	AgentMgr.RegisterSession(SessionId, AgentName);

	// Gemini: force fresh process per session
	if (AgentName == TEXT("Gemini CLI") && AgentMgr.IsConnectedToAgent(AgentName))
	{
		AgentMgr.DisconnectFromAgent(AgentName);
	}

	if (!AgentMgr.IsConnectedToAgent(AgentName))
	{
		AgentMgr.ConnectToAgent(AgentName);
	}

	if (AgentMgr.IsChatGatewayAgent(AgentName))
	{
		TSharedPtr<FChatGatewayClient> Client = AgentMgr.GetChatGatewayClient(AgentName);
		if (Client.IsValid())
		{
			Client->SetUnrealSessionId(SessionId);
			Client->NewSession(FPaths::ProjectDir());
		}
	}
	else
	{
		TSharedPtr<FACPClient> Client = AgentMgr.GetClient(AgentName);
		if (Client.IsValid())
		{
			const EACPClientState State = Client->GetState();
			if (State == EACPClientState::Ready || State == EACPClientState::InSession)
			{
				Client->SetUnrealSessionId(SessionId);
				Client->NewSession(FPaths::ProjectDir());
			}
			else
			{
				AgentMgr.QueuePendingNewSession(AgentName, SessionId);
			}
		}
	}

	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->LastUsedAgentName = AgentName;
		Settings->SavePreferences();
	}

	FAIKAnalytics::Get().RecordAgentSelected(AgentName);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("sessionId"), SessionId);
	Result->SetStringField(TEXT("agentName"), AgentName);
	return JsonToCompactString(Result);
}

FString FAgentService::SendPrompt(const FString& SessionId, const FString& Text)
{
	FACPSessionManager& SessionMgr = FACPSessionManager::Get();
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();

	const FACPActiveSession* Session = SessionMgr.GetActiveSession(SessionId);
	if (!Session)
	{
		return TEXT("{\"error\":\"Session not found\"}");
	}

	// Add user message to session
	SessionMgr.AddUserMessage(SessionId, Text);

	// Auto-title from first message
	if (Session->Metadata.MessageCount <= 1 &&
		(Session->Metadata.Title.IsEmpty() || Session->Metadata.Title == TEXT("New conversation")))
	{
		FString Title = Text;
		int32 Idx;
		if (Title.FindChar(TEXT('\n'), Idx)) Title = Title.Left(Idx);
		Title.TrimStartAndEndInline();
		if (Title.Len() > 80) Title = Title.Left(77) + TEXT("...");
		if (!Title.IsEmpty()) SessionMgr.UpdateSessionTitle(SessionId, Title);
	}

	// Route to agent
	FString AgentName = AgentMgr.GetSessionAgent(SessionId);
	if (AgentName.IsEmpty()) AgentName = Session->Metadata.AgentName;

	if (!AgentName.IsEmpty())
	{
		AgentMgr.SendPromptToSession(SessionId, AgentName, Text);
	}

	return TEXT("\"ok\"");
}

void FAgentService::CancelPrompt(const FString& SessionId)
{
	if (!SessionId.IsEmpty())
	{
		FACPAgentManager::Get().CancelSessionPrompt(SessionId);
	}
}

FString FAgentService::DeleteSession(const FString& SessionId)
{
	FACPSessionManager::Get().CloseSession(SessionId);
	return TEXT("{\"success\":true}");
}

FString FAgentService::RenameSession(const FString& SessionId, const FString& NewTitle)
{
	FACPSessionManager::Get().UpdateSessionTitle(SessionId, NewTitle);
	return TEXT("{\"success\":true}");
}

FString FAgentService::GetModels(const FString& AgentName) const
{
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	FACPSessionModelState ModelState;

	if (AgentMgr.IsChatGatewayAgent(AgentName))
	{
		TSharedPtr<FChatGatewayClient> Client = AgentMgr.GetChatGatewayClient(AgentName);
		if (Client.IsValid()) ModelState = Client->GetModelState();
	}
	else
	{
		TSharedPtr<FACPClient> Client = AgentMgr.GetClient(AgentName);
		if (Client.IsValid()) ModelState = Client->GetModelState();
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("currentModelId"), ModelState.CurrentModelId);
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FACPModelInfo& M : ModelState.AvailableModels)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), M.ModelId);
		Obj->SetStringField(TEXT("name"), M.Name);
		Obj->SetStringField(TEXT("description"), M.Description);
		Arr.Add(MakeShared<FJsonValueObject>(Obj));
	}
	Result->SetArrayField(TEXT("models"), Arr);
	return JsonToCompactString(Result);
}

void FAgentService::SetModel(const FString& AgentName, const FString& ModelId)
{
	if (AgentName.IsEmpty() || ModelId.IsEmpty()) return;
	FACPAgentManager::Get().SetAgentModel(AgentName, ModelId);
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->SaveModelForAgent(AgentName, ModelId);
	}
}

FString FAgentService::GetModes(const FString& AgentName) const
{
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	TSharedPtr<FACPClient> Client = AgentMgr.GetClient(AgentName);
	if (Client.IsValid())
	{
		const FACPSessionModeState& ModeState = Client->GetModeState();
		Result->SetStringField(TEXT("currentModeId"), ModeState.CurrentModeId);
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FACPSessionMode& M : ModeState.AvailableModes)
		{
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("id"), M.ModeId);
			Obj->SetStringField(TEXT("name"), M.Name);
			Obj->SetStringField(TEXT("description"), M.Description);
			Arr.Add(MakeShared<FJsonValueObject>(Obj));
		}
		Result->SetArrayField(TEXT("modes"), Arr);
	}
	return JsonToCompactString(Result);
}

void FAgentService::SetMode(const FString& AgentName, const FString& ModeId)
{
	if (AgentName.IsEmpty() || ModeId.IsEmpty()) return;
	FACPAgentManager::Get().SetAgentMode(AgentName, ModeId);
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->SaveModeForAgent(AgentName, ModeId);
	}
}

void FAgentService::RespondToPermission(const FString& AgentName, int32 RequestId, const FString& OptionId, TSharedPtr<FJsonObject> OutcomeMeta)
{
	FACPAgentManager::Get().RespondToPermissionRequest(AgentName, RequestId, OptionId, OutcomeMeta);
}
