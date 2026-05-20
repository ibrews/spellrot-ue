// Copyright 2026 Betide Studio. All Rights Reserved.

#include "LocalClient.h"
#include "AgentService.h"
#include "ACPSettings.h"
#include "MCPServer.h"
#include "Tools/NeoStackToolRegistry.h"
#include "Tools/NeoStackToolBase.h"
#include "WebSocketsModule.h"
#include "Json.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/EngineVersion.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/PlatformProcess.h"
#include "Editor.h"

DECLARE_LOG_CATEGORY_EXTERN(LogLocalClient, Log, All);
DEFINE_LOG_CATEGORY(LogLocalClient);

// ── Singleton ──────────────────────────────────────────────────────────

FLocalClient& FLocalClient::Get()
{
	static FLocalClient Instance;
	return Instance;
}

FLocalClient::~FLocalClient()
{
	Shutdown();
}

// ── Discovery ──────────────────────────────────────────────────────────

FString FLocalClient::GetDiscoveryFilePath()
{
	const UACPSettings* Settings = UACPSettings::Get();
	if (Settings && !Settings->IDEDiscoveryPathOverride.IsEmpty())
	{
		return Settings->IDEDiscoveryPathOverride;
	}

#if PLATFORM_WINDOWS
	FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
	if (Home.IsEmpty()) Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
#else
	FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
#endif

	return FPaths::Combine(Home, TEXT(".neostack"), TEXT("server.json"));
}

uint16 FLocalClient::DiscoverIdePort() const
{
	const FString Path = GetDiscoveryFilePath();

	// Check file exists and is fresh (modified within last 15 seconds)
	FFileStatData Stat = IFileManager::Get().GetStatData(*Path);
	if (!Stat.bIsValid || Stat.bIsDirectory)
	{
		return 0;
	}

	const FTimespan Age = FDateTime::UtcNow() - Stat.ModificationTime;
	if (Age.GetTotalSeconds() > 15.0)
	{
		return 0;
	}

	// Read and parse
	FString Contents;
	if (!FFileHelper::LoadFileToString(Contents, *Path))
	{
		return 0;
	}

	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Contents);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		return 0;
	}

	int32 Port = 0;
	if (Json->TryGetNumberField(TEXT("wsPort"), Port) && Port > 0 && Port <= 65535)
	{
		return static_cast<uint16>(Port);
	}

	return 0;
}

// ── Lifecycle ──────────────────────────────────────────────────────────

void FLocalClient::Initialize()
{
	const UACPSettings* Settings = UACPSettings::Get();
	if (Settings && !Settings->bEnableIDEConnection)
	{
		return;
	}

	bEnabled = true;
	UE_LOG(LogLocalClient, Log, TEXT("Initializing IDE connection..."));

	FModuleManager::Get().LoadModuleChecked<FWebSocketsModule>("WebSockets");

	// Try to connect immediately if IDE is running
	uint16 Port = DiscoverIdePort();
	if (Port > 0)
	{
		Connect(Port);
	}
	else
	{
		UE_LOG(LogLocalClient, Log, TEXT("IDE not detected. Will poll for discovery file..."));
	}

	// Poll for IDE discovery file every 5 seconds
	DiscoveryTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([this](float) -> bool
		{
			if (!bEnabled) return false;
			if (IsConnected()) return true; // Already connected

			uint16 Port = DiscoverIdePort();
			if (Port > 0 && Port != LastKnownPort)
			{
				UE_LOG(LogLocalClient, Log, TEXT("IDE discovered on port %d"), Port);
				Connect(Port);
			}
			return true;
		}),
		5.0f);
}

void FLocalClient::Shutdown()
{
	bEnabled = false;
	bAuthenticated = false;
	LastKnownPort = 0;

	UnbindDelegates();

	if (DiscoveryTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(DiscoveryTickerHandle);
		DiscoveryTickerHandle.Reset();
	}
	if (ReconnectTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ReconnectTickerHandle);
		ReconnectTickerHandle.Reset();
	}
	if (HeartbeatTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(HeartbeatTickerHandle);
		HeartbeatTickerHandle.Reset();
	}

	Disconnect();
}

bool FLocalClient::IsConnected() const
{
	return WebSocket.IsValid() && WebSocket->IsConnected() && bAuthenticated;
}

// ── Connection ─────────────────────────────────────────────────────────

void FLocalClient::Connect(uint16 Port)
{
	if (WebSocket.IsValid()) Disconnect();

	LastKnownPort = Port;
	FString Url = FString::Printf(TEXT("ws://127.0.0.1:%d"), Port);
	UE_LOG(LogLocalClient, Log, TEXT("Connecting to IDE at %s"), *Url);

	WebSocket = FWebSocketsModule::Get().CreateWebSocket(Url, TEXT(""), TMap<FString, FString>());
	WebSocket->OnConnected().AddRaw(this, &FLocalClient::OnConnected);
	WebSocket->OnConnectionError().AddRaw(this, &FLocalClient::OnConnectionError);
	WebSocket->OnClosed().AddRaw(this, &FLocalClient::OnClosed);
	WebSocket->OnMessage().AddRaw(this, &FLocalClient::OnMessage);
	WebSocket->Connect();
}

void FLocalClient::Disconnect()
{
	if (WebSocket.IsValid())
	{
		if (WebSocket->IsConnected()) WebSocket->Close();
		WebSocket.Reset();
	}
}

void FLocalClient::ScheduleReconnect()
{
	if (!bEnabled) return;

	float Delay = FMath::Min(FMath::Pow(2.0f, static_cast<float>(ReconnectAttempt)), 30.0f);
	ReconnectAttempt++;

	UE_LOG(LogLocalClient, Log, TEXT("Reconnecting in %.0fs (attempt %d)"), Delay, ReconnectAttempt);

	if (ReconnectTickerHandle.IsValid())
		FTSTicker::GetCoreTicker().RemoveTicker(ReconnectTickerHandle);

	ReconnectTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([this](float) -> bool
		{
			// Re-discover port in case IDE restarted on a different port
			uint16 Port = DiscoverIdePort();
			if (Port > 0)
			{
				Connect(Port);
			}
			return false;
		}),
		Delay);
}

// ── Auth ───────────────────────────────────────────────────────────────

void FLocalClient::SendAuth()
{
	TSharedRef<FJsonObject> AuthMsg = MakeShared<FJsonObject>();
	AuthMsg->SetStringField(TEXT("type"), TEXT("auth"));

	// Instance metadata (same fields as relay, minus API key)
	FString InstanceId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	AuthMsg->SetStringField(TEXT("instanceId"), InstanceId);
	AuthMsg->SetStringField(TEXT("projectPath"),
		FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	AuthMsg->SetStringField(TEXT("projectName"), FApp::GetProjectName());
	AuthMsg->SetStringField(TEXT("engineVersion"),
		FEngineVersion::Current().ToString(EVersionComponent::Minor));

#if PLATFORM_MAC
	AuthMsg->SetStringField(TEXT("platform"), TEXT("Mac"));
#elif PLATFORM_WINDOWS
	AuthMsg->SetStringField(TEXT("platform"), TEXT("Win64"));
#elif PLATFORM_LINUX
	AuthMsg->SetStringField(TEXT("platform"), TEXT("Linux"));
#else
	AuthMsg->SetStringField(TEXT("platform"), TEXT("Unknown"));
#endif

	if (IPluginManager::Get().FindPlugin(TEXT("AgentIntegrationKit")))
	{
		AuthMsg->SetStringField(TEXT("pluginVersion"),
			IPluginManager::Get().FindPlugin(TEXT("AgentIntegrationKit"))->GetDescriptor().VersionName);
	}

	SendMessage(AuthMsg);
}

// ── WebSocket Events ───────────────────────────────────────────────────

void FLocalClient::OnConnected()
{
	UE_LOG(LogLocalClient, Log, TEXT("WebSocket connected to IDE, sending auth..."));
	ReconnectAttempt = 0;
	SendAuth();
}

void FLocalClient::OnConnectionError(const FString& Error)
{
	UE_LOG(LogLocalClient, Warning, TEXT("IDE connection error: %s"), *Error);
	bAuthenticated = false;
	ScheduleReconnect();
}

void FLocalClient::OnClosed(int32 StatusCode, const FString& Reason, bool bWasClean)
{
	UE_LOG(LogLocalClient, Log, TEXT("IDE connection closed (code=%d, reason=%s)"), StatusCode, *Reason);
	bAuthenticated = false;
	UnbindDelegates();
	ScheduleReconnect();
}

void FLocalClient::OnMessage(const FString& Message)
{
	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid()) return;

	FString Type;
	if (!Json->TryGetStringField(TEXT("type"), Type)) return;

	if (Type == TEXT("auth_ok"))
	{
		bAuthenticated = true;
		UE_LOG(LogLocalClient, Log, TEXT("Authenticated with IDE!"));

		BindDelegates();

		// Start heartbeat
		if (HeartbeatTickerHandle.IsValid())
			FTSTicker::GetCoreTicker().RemoveTicker(HeartbeatTickerHandle);
		HeartbeatTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([this](float) -> bool
			{
				if (!IsConnected()) return true;
				TSharedRef<FJsonObject> Pong = MakeShared<FJsonObject>();
				Pong->SetStringField(TEXT("type"), TEXT("pong"));
				SendMessage(Pong);
				return true;
			}),
			30.0f);
		return;
	}

	if (Type == TEXT("auth_error"))
	{
		FString Msg;
		Json->TryGetStringField(TEXT("message"), Msg);
		UE_LOG(LogLocalClient, Error, TEXT("IDE auth failed: %s"), *Msg);
		return;
	}

	if (Type == TEXT("ping"))
	{
		TSharedRef<FJsonObject> Pong = MakeShared<FJsonObject>();
		Pong->SetStringField(TEXT("type"), TEXT("pong"));
		SendMessage(Pong);
		return;
	}

	if (Type == TEXT("rpc_request"))
	{
		HandleRpcRequest(Json);
		return;
	}
}

// ── Message Sending ────────────────────────────────────────────────────

void FLocalClient::SendMessage(const TSharedRef<FJsonObject>& Message)
{
	FString JsonString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(Message, Writer);
	SendRawMessage(JsonString);
}

void FLocalClient::SendRawMessage(const FString& JsonString)
{
	if (!WebSocket.IsValid() || !WebSocket->IsConnected()) return;

	if (IsInGameThread())
	{
		WebSocket->Send(JsonString);
	}
	else
	{
		FString Copy = JsonString;
		AsyncTask(ENamedThreads::GameThread, [this, Copy]()
		{
			if (WebSocket.IsValid() && WebSocket->IsConnected())
				WebSocket->Send(Copy);
		});
	}
}

void FLocalClient::SendEvent(const FString& EventName, const TSharedRef<FJsonObject>& Data)
{
	if (!IsConnected()) return;
	TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("type"), TEXT("event"));
	Msg->SetStringField(TEXT("event"), EventName);
	Msg->SetObjectField(TEXT("data"), Data);
	SendMessage(Msg);
}

// ── RPC Response Helpers ───────────────────────────────────────────────

void FLocalClient::SendRpcResponse(const FString& RequestId, const FString& ResultJson)
{
	TSharedPtr<FJsonValue> ResultValue;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);
	FJsonSerializer::Deserialize(Reader, ResultValue);

	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("type"), TEXT("rpc_response"));
	Response->SetStringField(TEXT("id"), RequestId);
	if (ResultValue.IsValid())
		Response->SetField(TEXT("result"), ResultValue);
	else
		Response->SetStringField(TEXT("result"), ResultJson);
	SendMessage(Response);
}

void FLocalClient::SendRpcError(const FString& RequestId, const FString& Error)
{
	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("type"), TEXT("rpc_response"));
	Response->SetStringField(TEXT("id"), RequestId);
	Response->SetStringField(TEXT("error"), Error);
	SendMessage(Response);
}

// ── RPC Dispatch ───────────────────────────────────────────────────────

void FLocalClient::HandleRpcRequest(const TSharedPtr<FJsonObject>& Request)
{
	FString RequestId, Method;
	Request->TryGetStringField(TEXT("id"), RequestId);
	Request->TryGetStringField(TEXT("method"), Method);
	if (RequestId.IsEmpty() || Method.IsEmpty()) return;

	const TArray<TSharedPtr<FJsonValue>>* Args = nullptr;
	Request->TryGetArrayField(TEXT("args"), Args);

	auto GetStringArg = [&](int32 Index) -> FString
	{
		if (Args && Args->IsValidIndex(Index) && (*Args)[Index]->Type == EJson::String)
			return (*Args)[Index]->AsString();
		return FString();
	};

	auto GetObjectArg = [&](int32 Index) -> TSharedPtr<FJsonObject>
	{
		if (Args && Args->IsValidIndex(Index) && (*Args)[Index]->Type == EJson::Object)
			return (*Args)[Index]->AsObject();
		return nullptr;
	};

	FAgentService& Svc = FAgentService::Get();

	// ── Standard relay methods (same as FRelayClient) ──────────────────
	if (Method == TEXT("getAgents"))           { SendRpcResponse(RequestId, Svc.GetAgents()); return; }
	if (Method == TEXT("getLastUsedAgent"))     { SendRpcResponse(RequestId, FString::Printf(TEXT("\"%s\""), *Svc.GetLastUsedAgent())); return; }
	if (Method == TEXT("getSessions"))          { SendRpcResponse(RequestId, Svc.GetAllSessions()); return; }
	if (Method == TEXT("getSessionMessages"))   { SendRpcResponse(RequestId, Svc.GetSessionMessages(GetStringArg(0))); return; }
	if (Method == TEXT("resumeSession"))        { SendRpcResponse(RequestId, Svc.ResumeSession(GetStringArg(0))); return; }
	if (Method == TEXT("deleteSession"))        { SendRpcResponse(RequestId, Svc.DeleteSession(GetStringArg(0))); return; }
	if (Method == TEXT("renameSession"))        { SendRpcResponse(RequestId, Svc.RenameSession(GetStringArg(0), GetStringArg(1))); return; }
	if (Method == TEXT("getModels"))            { SendRpcResponse(RequestId, Svc.GetModels(GetStringArg(0))); return; }
	if (Method == TEXT("getModes"))             { SendRpcResponse(RequestId, Svc.GetModes(GetStringArg(0))); return; }
	if (Method == TEXT("cancelPrompt"))         { Svc.CancelPrompt(GetStringArg(0)); SendRpcResponse(RequestId, TEXT("\"ok\"")); return; }

	if (Method == TEXT("createSession"))
	{
		FString AgentName = GetStringArg(0);
		if (AgentName.IsEmpty()) { SendRpcError(RequestId, TEXT("Missing agentName")); return; }
		SendRpcResponse(RequestId, Svc.CreateSession(AgentName));
		return;
	}

	if (Method == TEXT("sendPrompt"))
	{
		FString SessionId = GetStringArg(0), Text = GetStringArg(1);
		if (SessionId.IsEmpty() || Text.IsEmpty()) { SendRpcError(RequestId, TEXT("Missing sessionId or text")); return; }
		SendRpcResponse(RequestId, Svc.SendPrompt(SessionId, Text));
		return;
	}

	if (Method == TEXT("setModel"))  { Svc.SetModel(GetStringArg(0), GetStringArg(1)); SendRpcResponse(RequestId, TEXT("\"ok\"")); return; }
	if (Method == TEXT("setMode"))   { Svc.SetMode(GetStringArg(0), GetStringArg(1)); SendRpcResponse(RequestId, TEXT("\"ok\"")); return; }

	if (Method == TEXT("respondToPermission"))
	{
		int32 PermReqId = 0;
		if (Args && Args->IsValidIndex(1)) PermReqId = static_cast<int32>((*Args)[1]->AsNumber());
		Svc.RespondToPermission(GetStringArg(0), PermReqId, GetStringArg(2));
		SendRpcResponse(RequestId, TEXT("\"ok\""));
		return;
	}

	// ── IDE-specific methods (new for LocalClient) ─────────────────────

	if (Method == TEXT("executeScript"))
	{
		FString Script = GetStringArg(0);
		if (Script.IsEmpty()) { SendRpcError(RequestId, TEXT("Missing script argument")); return; }

		// Execute via the MCP tool registry (same path as MCP server tools/call)
		FNeoStackToolBase* Tool = FNeoStackToolRegistry::Get().GetTool(TEXT("execute_script"));
		if (!Tool)
		{
			SendRpcError(RequestId, TEXT("execute_script tool not registered"));
			return;
		}

		TSharedPtr<FJsonObject> ToolArgs = MakeShared<FJsonObject>();
		ToolArgs->SetStringField(TEXT("script"), Script);

		FToolResult Result = FNeoStackToolRegistry::Get().Execute(TEXT("execute_script"), ToolArgs);

		// Build MCP-style response
		TSharedRef<FJsonObject> ResultObj = MakeShared<FJsonObject>();
		ResultObj->SetBoolField(TEXT("success"), Result.bSuccess);
		ResultObj->SetBoolField(TEXT("isError"), !Result.bSuccess);

		TArray<TSharedPtr<FJsonValue>> ContentArray;
		TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
		TextContent->SetStringField(TEXT("type"), TEXT("text"));
		TextContent->SetStringField(TEXT("text"), Result.Output);
		ContentArray.Add(MakeShared<FJsonValueObject>(TextContent));

		// Add images if any
		for (const auto& Img : Result.Images)
		{
			TSharedPtr<FJsonObject> ImgContent = MakeShared<FJsonObject>();
			ImgContent->SetStringField(TEXT("type"), TEXT("image"));
			ImgContent->SetStringField(TEXT("data"), Img.Base64Data);
			ImgContent->SetStringField(TEXT("mimeType"), Img.MimeType);
			ContentArray.Add(MakeShared<FJsonValueObject>(ImgContent));
		}

		ResultObj->SetArrayField(TEXT("content"), ContentArray);

		FString ResponseJson;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseJson);
		FJsonSerializer::Serialize(ResultObj, Writer);
		SendRpcResponse(RequestId, ResponseJson);
		return;
	}

	if (Method == TEXT("getEditorState"))
	{
		TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
		State->SetStringField(TEXT("projectName"), FApp::GetProjectName());
		State->SetStringField(TEXT("projectPath"),
			FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		State->SetStringField(TEXT("engineVersion"),
			FEngineVersion::Current().ToString());

		// Current level
		if (GEditor && GEditor->GetEditorWorldContext().World())
		{
			State->SetStringField(TEXT("currentLevel"),
				GEditor->GetEditorWorldContext().World()->GetMapName());
		}

		// PIE state
		State->SetBoolField(TEXT("isPlaying"), GEditor ? GEditor->IsPlaySessionInProgress() : false);

		FString StateJson;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&StateJson);
		FJsonSerializer::Serialize(State, Writer);
		SendRpcResponse(RequestId, StateJson);
		return;
	}

	if (Method == TEXT("getToolsList"))
	{
		TArray<TSharedPtr<FJsonValue>> ToolsArray;
		for (const FString& ToolName : FNeoStackToolRegistry::Get().GetToolNames())
		{
			FNeoStackToolBase* ToolDef = FNeoStackToolRegistry::Get().GetTool(ToolName);
			if (!ToolDef) continue;

			TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
			ToolObj->SetStringField(TEXT("name"), ToolDef->GetName());
			ToolObj->SetStringField(TEXT("description"), ToolDef->GetDescription());

			TSharedPtr<FJsonObject> Schema = ToolDef->GetInputSchema();
			if (Schema.IsValid())
				ToolObj->SetObjectField(TEXT("inputSchema"), Schema);

			ToolsArray.Add(MakeShared<FJsonValueObject>(ToolObj));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("tools"), ToolsArray);

		FString ResultJson;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultJson);
		FJsonSerializer::Serialize(Result, Writer);
		SendRpcResponse(RequestId, ResultJson);
		return;
	}

	if (Method == TEXT("executeTool"))
	{
		FString ToolName = GetStringArg(0);
		TSharedPtr<FJsonObject> ToolArgs = GetObjectArg(1);
		if (ToolName.IsEmpty()) { SendRpcError(RequestId, TEXT("Missing tool name")); return; }
		if (!ToolArgs.IsValid()) ToolArgs = MakeShared<FJsonObject>();

		FToolResult Result = FNeoStackToolRegistry::Get().Execute(ToolName, ToolArgs);

		TSharedRef<FJsonObject> ResultObj = MakeShared<FJsonObject>();
		ResultObj->SetBoolField(TEXT("success"), Result.bSuccess);
		ResultObj->SetBoolField(TEXT("isError"), !Result.bSuccess);

		TArray<TSharedPtr<FJsonValue>> ContentArray;
		TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
		TextContent->SetStringField(TEXT("type"), TEXT("text"));
		TextContent->SetStringField(TEXT("text"), Result.Output);
		ContentArray.Add(MakeShared<FJsonValueObject>(TextContent));

		for (const auto& Img : Result.Images)
		{
			TSharedPtr<FJsonObject> ImgContent = MakeShared<FJsonObject>();
			ImgContent->SetStringField(TEXT("type"), TEXT("image"));
			ImgContent->SetStringField(TEXT("data"), Img.Base64Data);
			ImgContent->SetStringField(TEXT("mimeType"), Img.MimeType);
			ContentArray.Add(MakeShared<FJsonValueObject>(ImgContent));
		}

		ResultObj->SetArrayField(TEXT("content"), ContentArray);

		FString ResponseJson;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseJson);
		FJsonSerializer::Serialize(ResultObj, Writer);
		SendRpcResponse(RequestId, ResponseJson);
		return;
	}

	SendRpcError(RequestId, FString::Printf(TEXT("Method '%s' not implemented"), *Method));
}

// ── Delegate Binding ───────────────────────────────────────────────────

void FLocalClient::BindDelegates()
{
	UnbindDelegates();

	FAgentService& Svc = FAgentService::Get();

	OnAgentMessageHandle = Svc.OnMessage.AddLambda(
		[this](const FString& SessionId, const FString& AgentName, const FString& UpdateJson)
		{
			if (!IsConnected()) return;
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("sessionId"), SessionId);

			TSharedPtr<FJsonObject> UpdateObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(UpdateJson);
			if (FJsonSerializer::Deserialize(Reader, UpdateObj) && UpdateObj.IsValid())
				Data->SetObjectField(TEXT("update"), UpdateObj);

			SendEvent(TEXT("onMessage"), Data);
		});

	OnAgentStateChangedHandle = Svc.OnStateChanged.AddLambda(
		[this](const FString& SessionId, const FString& AgentName, const FString& StateStr, const FString& Message)
		{
			if (!IsConnected()) return;
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("sessionId"), SessionId);
			Data->SetStringField(TEXT("agentName"), AgentName);
			Data->SetStringField(TEXT("state"), StateStr);
			Data->SetStringField(TEXT("message"), Message);
			SendEvent(TEXT("onStateChanged"), Data);
		});

	OnPermissionRequestHandle = Svc.OnPermissionRequest.AddLambda(
		[this](const FString& SessionId, const FString& RequestJson)
		{
			if (!IsConnected()) return;
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("sessionId"), SessionId);

			TSharedPtr<FJsonObject> ReqObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestJson);
			if (FJsonSerializer::Deserialize(Reader, ReqObj) && ReqObj.IsValid())
			{
				for (const auto& Pair : ReqObj->Values)
					Data->SetField(Pair.Key, Pair.Value);
			}

			SendEvent(TEXT("onPermissionRequest"), Data);
		});

	OnModelsAvailableHandle = Svc.OnModelsAvailable.AddLambda(
		[this](const FString& AgentName, const FString& ModelsJson)
		{
			if (!IsConnected()) return;
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("agentName"), AgentName);
			Data->SetStringField(TEXT("modelsJson"), ModelsJson);
			SendEvent(TEXT("onModelsAvailable"), Data);
		});

	OnPlanUpdateHandle = Svc.OnPlanUpdate.AddLambda(
		[this](const FString& SessionId, const FString& PlanJson)
		{
			if (!IsConnected()) return;
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("sessionId"), SessionId);
			Data->SetStringField(TEXT("planJson"), PlanJson);
			SendEvent(TEXT("onPlanUpdate"), Data);
		});

	UE_LOG(LogLocalClient, Log, TEXT("Bound to FAgentService delegates for IDE event forwarding"));
}

void FLocalClient::UnbindDelegates()
{
	FAgentService& Svc = FAgentService::Get();

	if (OnAgentMessageHandle.IsValid()) { Svc.OnMessage.Remove(OnAgentMessageHandle); OnAgentMessageHandle.Reset(); }
	if (OnAgentStateChangedHandle.IsValid()) { Svc.OnStateChanged.Remove(OnAgentStateChangedHandle); OnAgentStateChangedHandle.Reset(); }
	if (OnPermissionRequestHandle.IsValid()) { Svc.OnPermissionRequest.Remove(OnPermissionRequestHandle); OnPermissionRequestHandle.Reset(); }
	if (OnModelsAvailableHandle.IsValid()) { Svc.OnModelsAvailable.Remove(OnModelsAvailableHandle); OnModelsAvailableHandle.Reset(); }
	if (OnPlanUpdateHandle.IsValid()) { Svc.OnPlanUpdate.Remove(OnPlanUpdateHandle); OnPlanUpdateHandle.Reset(); }
}
