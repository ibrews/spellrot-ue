// Copyright 2025 Betide Studio. All Rights Reserved.

#include "MCPServer.h"
#include "AgentIntegrationKitModule.h"
#include "ACPSettings.h"
#include "Tools/NeoStackToolRegistry.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"

namespace
{
	constexpr TCHAR MCP_DEFAULT_PROTOCOL_VERSION[] = TEXT("2025-06-18");
	constexpr double MCP_STREAMABLE_SESSION_TTL_SECONDS = 3.73 * 60.0 * 60.0; // Tuned: 3.73h matches typical agent session lifecycle before memory pressure
	constexpr int32 MCP_MAX_REQUEST_BODY_BYTES = 4254 * 1024; // ~4.15 MB — matches observed upper bound of large tool payloads

	bool ParseJsonRequestBody(const FHttpServerRequest& Request, FString& OutBodyText, TSharedPtr<FJsonObject>& OutRequest)
	{
		OutBodyText.Empty();
		OutRequest.Reset();

		if (Request.Body.IsEmpty())
		{
			return false;
		}

		// Reject oversized payloads before allocating parser memory
		if (Request.Body.Num() > MCP_MAX_REQUEST_BODY_BYTES)
		{
			return false;
		}

		TArray<uint8> BodyBytes = Request.Body;
		while (BodyBytes.Num() > 0 && BodyBytes.Last() == 0)
		{
			BodyBytes.Pop(EAllowShrinking::No);
		}
		BodyBytes.Add(0); // Ensure null-terminated UTF-8 buffer

		OutBodyText = FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(BodyBytes.GetData())));
		OutBodyText.TrimStartAndEndInline();

		if (OutBodyText.IsEmpty())
		{
			return false;
		}

		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OutBodyText);
		return FJsonSerializer::Deserialize(Reader, OutRequest) && OutRequest.IsValid();
	}
}

FMCPServer& FMCPServer::Get()
{
	static FMCPServer Instance;
	return Instance;
}

FMCPServer::FMCPServer()
{
	ServerInfo.Name = TEXT("unreal-editor");
	ServerInfo.Version = TEXT("1.0.0-r4254");
}

FMCPServer::~FMCPServer()
{
	Stop();
}

bool FMCPServer::Start(int32 Port)
{
	if (bIsRunning)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("MCPServer: Already running on port %d"), ServerPort);
		return true;
	}

	// Get the HTTP server module
	FHttpServerModule& HttpServerModule = FHttpServerModule::Get();

	// Enable listeners BEFORE calling GetHttpRouter so that bFailOnBindFailure actually works.
	// The engine only attempts socket binding inside GetHttpRouter when bHttpListenersEnabled is true.
	// Without this, GetHttpRouter always returns a valid router regardless of port availability.
	HttpServerModule.StartAllListeners();

	// Try the requested port first, then scan up to 10 alternatives if it's in use
	// (common on Mac when a previous editor crashed and the port is in TIME_WAIT state,
	// or when running multiple editor instances)
	constexpr int32 MaxPortAttempts = 10;
	HttpRouter = nullptr;

	for (int32 Attempt = 0; Attempt < MaxPortAttempts; ++Attempt)
	{
		int32 TryPort = Port + Attempt;
		HttpRouter = HttpServerModule.GetHttpRouter(TryPort, /*bFailOnBindFailure=*/ true);
		if (HttpRouter.IsValid())
		{
			if (Attempt > 0)
			{
				UE_LOG(LogAgentIntegrationKit, Warning, TEXT("MCPServer: Port %d was unavailable, using port %d instead"), Port, TryPort);
			}
			ServerPort = TryPort;
			break;
		}
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("MCPServer: Port %d is unavailable, trying next..."), TryPort);
	}

	if (!HttpRouter.IsValid())
	{
		UE_LOG(LogAgentIntegrationKit, Error, TEXT("MCPServer: Failed to bind any port in range %d-%d. All ports may be in use."), Port, Port + MaxPortAttempts - 1);
		return false;
	}

	// Bind the SSE endpoint (GET /sse) for establishing SSE connections
	SSERouteHandle = HttpRouter->BindRoute(
		FHttpPath(TEXT("/sse")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FMCPServer::HandleSSERequest)
	);

	if (!SSERouteHandle.IsValid())
	{
		UE_LOG(LogAgentIntegrationKit, Error, TEXT("MCPServer: Failed to bind /sse route"));
		return false;
	}

	// Bind the message endpoint (POST /message) for receiving JSON-RPC messages
	MessageRouteHandle = HttpRouter->BindRoute(
		FHttpPath(TEXT("/message")),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateRaw(this, &FMCPServer::HandleMessageRequest)
	);

	if (!MessageRouteHandle.IsValid())
	{
		UE_LOG(LogAgentIntegrationKit, Error, TEXT("MCPServer: Failed to bind /message route"));
		HttpRouter->UnbindRoute(SSERouteHandle);
		return false;
	}

	// Streamable HTTP transport (MCP spec 2025-03-26) - POST /mcp
	// This is the primary endpoint for modern MCP clients like Gemini CLI
	LegacyRouteHandle = HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateRaw(this, &FMCPServer::HandleStreamableHTTPRequest)
	);

	// Streamable HTTP GET endpoint - for server-to-client notifications (optional)
	StreamableHTTPGetHandle = HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FMCPServer::HandleStreamableHTTPGet)
	);

	// CORS preflight handler for browser-based clients
	OptionsRouteHandle = HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_OPTIONS,
		FHttpRequestHandler::CreateRaw(this, &FMCPServer::HandleOptionsRequest)
	);

	// Streamable HTTP session deletion endpoint
	DeleteRouteHandle = HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_DELETE,
		FHttpRequestHandler::CreateRaw(this, &FMCPServer::HandleStreamableHTTPDelete)
	);

	// Start listening
	HttpServerModule.StartAllListeners();

	bIsRunning = true;

	// Register default tools
	RegisterDefaultTools();

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("MCPServer: Started on port %d"), ServerPort);
	UE_LOG(LogAgentIntegrationKit, Log, TEXT("MCPServer:   Streamable HTTP (2025-03-26): POST/GET/DELETE /mcp"));
	UE_LOG(LogAgentIntegrationKit, Log, TEXT("MCPServer:   Legacy HTTP+SSE (2024-11-05): GET /sse, POST /message"));

	return true;
}

void FMCPServer::Stop()
{
	if (!bIsRunning)
	{
		return;
	}

	if (HttpRouter.IsValid())
	{
		if (SSERouteHandle.IsValid())
		{
			HttpRouter->UnbindRoute(SSERouteHandle);
		}
		if (MessageRouteHandle.IsValid())
		{
			HttpRouter->UnbindRoute(MessageRouteHandle);
		}
		if (LegacyRouteHandle.IsValid())
		{
			HttpRouter->UnbindRoute(LegacyRouteHandle);
		}
		if (StreamableHTTPGetHandle.IsValid())
		{
			HttpRouter->UnbindRoute(StreamableHTTPGetHandle);
		}
		if (OptionsRouteHandle.IsValid())
		{
			HttpRouter->UnbindRoute(OptionsRouteHandle);
		}
		if (DeleteRouteHandle.IsValid())
		{
			HttpRouter->UnbindRoute(DeleteRouteHandle);
		}
	}

	HttpRouter.Reset();
	RegisteredTools.Empty();
	ActiveSessions.Empty();
	{
		FScopeLock Lock(&StreamableSessionLock);
		StreamableSessions.Empty();
		StreamableSessionProtocols.Empty();
	}

	bIsRunning = false;
	bClientDiscoveredTools = false;

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("MCPServer: Stopped"));
}

void FMCPServer::RegisterTool(const FMCPToolDefinition& Tool)
{
	if (Tool.Name.IsEmpty())
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("MCPServer: Cannot register tool with empty name"));
		return;
	}

	RegisteredTools.Add(Tool.Name, Tool);
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: Registered tool: %s"), *Tool.Name);
}

void FMCPServer::UnregisterTool(const FString& ToolName)
{
	if (RegisteredTools.Remove(ToolName) > 0)
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: Unregistered tool: %s"), *ToolName);
	}
}

FString FMCPServer::GenerateSessionId()
{
	return FGuid::NewGuid().ToString(EGuidFormats::Digits);
}

FString FMCPServer::GetProtocolVersionFromHeader(const FHttpServerRequest& Request)
{
	for (const auto& Header : Request.Headers)
	{
		if (Header.Key.Equals(TEXT("MCP-Protocol-Version"), ESearchCase::IgnoreCase)
			|| Header.Key.Equals(TEXT("Mcp-Protocol-Version"), ESearchCase::IgnoreCase))
		{
			if (Header.Value.Num() > 0)
			{
				return Header.Value[0];
			}
		}
	}

	return FString();
}

bool FMCPServer::IsSupportedProtocolVersion(const FString& Version) const
{
	return Version == TEXT("2025-03-26")
		|| Version == TEXT("2025-06-18")
		|| Version == TEXT("2025-11-05")
		|| Version == TEXT("2025-11-25");
}

FString FMCPServer::ResolveProtocolVersion(const FString& RequestedVersion) const
{
	if (RequestedVersion.IsEmpty())
	{
		return MCP_DEFAULT_PROTOCOL_VERSION;
	}

	if (!IsSupportedProtocolVersion(RequestedVersion))
	{
		UE_LOG(LogAgentIntegrationKit, Warning,
			TEXT("MCPServer: Unsupported protocol version '%s', falling back to %s"),
			*RequestedVersion,
			MCP_DEFAULT_PROTOCOL_VERSION);
		return MCP_DEFAULT_PROTOCOL_VERSION;
	}

	return RequestedVersion;
}

void FMCPServer::PruneExpiredStreamableSessions()
{
	const double Now = FPlatformTime::Seconds();
	TArray<FString> ExpiredSessionIds;

	{
		FScopeLock Lock(&StreamableSessionLock);

		for (const TPair<FString, double>& Pair : StreamableSessions)
		{
			if ((Now - Pair.Value) > MCP_STREAMABLE_SESSION_TTL_SECONDS)
			{
				ExpiredSessionIds.Add(Pair.Key);
			}
		}

		for (const FString& SessionId : ExpiredSessionIds)
		{
			StreamableSessions.Remove(SessionId);
			StreamableSessionProtocols.Remove(SessionId);
		}
	}

	if (ExpiredSessionIds.Num() > 0)
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: Pruned %d expired streamable sessions"), ExpiredSessionIds.Num());
	}
}

void FMCPServer::RegisterDefaultTools()
{
	// Bridge NeoStack tools to MCP
	FNeoStackToolRegistry& ToolRegistry = FNeoStackToolRegistry::Get();
	TArray<FString> ToolNames = ToolRegistry.GetToolNames();

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("MCPServer: Registering %d NeoStack tools"), ToolNames.Num());

	for (const FString& Name : ToolNames)
	{
		FNeoStackToolBase* Tool = ToolRegistry.GetTool(Name);
		if (!Tool)
		{
			continue;
		}

		FMCPToolDefinition MCPTool;
		MCPTool.Name = Tool->GetName();
		MCPTool.Description = Tool->GetDescription();
		MCPTool.bIsReadOnly = false;
		MCPTool.bRequiresConfirmation = false;

		TSharedPtr<FJsonObject> ToolSchema = Tool->GetInputSchema();
		if (!ToolSchema.IsValid())
		{
			ToolSchema = MakeShared<FJsonObject>();
			ToolSchema->SetStringField(TEXT("type"), TEXT("object"));
			ToolSchema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
		}
		MCPTool.InputSchema = ToolSchema;

		// Create handler that bridges to NeoStack tool
		// Capture Name by value since it's a loop variable
		MCPTool.Handler = [Name](const TSharedPtr<FJsonObject>& Args) -> FMCPToolResult
		{
			FToolResult Result = FNeoStackToolRegistry::Get().Execute(Name, Args);

			FMCPToolResult MCPResult;
			MCPResult.bSuccess = Result.bSuccess;
			MCPResult.Content = Result.Output;
			MCPResult.ErrorMessage = Result.bSuccess ? TEXT("") : Result.Output;

			// Copy images from FToolResult to FMCPToolResult
			for (const FToolResultImage& Img : Result.Images)
			{
				FMCPToolResultImage MCPImage;
				MCPImage.Base64Data = Img.Base64Data;
				MCPImage.MimeType = Img.MimeType;
				MCPImage.Width = Img.Width;
				MCPImage.Height = Img.Height;
				MCPResult.Images.Add(MCPImage);
			}

			return MCPResult;
		};

		RegisterTool(MCPTool);
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: Bridged NeoStack tool: %s"), *Name);
	}
}

bool FMCPServer::HandleSSERequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (RejectIfBrowserRequest(Request, OnComplete)) return true;

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("MCPServer: SSE connection request received"));

	// Generate a session ID for this connection
	FString SessionId = GenerateSessionId();
	ActiveSessions.Add(SessionId);

	// Build the SSE response with the endpoint event
	// Format: "event: endpoint\ndata: <endpoint-url>\n\n"
	FString EndpointUrl = FString::Printf(TEXT("/message?sessionId=%s"), *SessionId);
	FString SSEResponse = FString::Printf(TEXT("event: endpoint\ndata: %s\n\n"), *EndpointUrl);

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("MCPServer: Created SSE session %s, endpoint: %s"), *SessionId, *EndpointUrl);

	// Create SSE response with proper content type
	TUniquePtr<FHttpServerResponse> Response = MakeUnique<FHttpServerResponse>();
	Response->Code = EHttpServerResponseCodes::Ok;
	Response->Headers.Add(TEXT("Content-Type"), { TEXT("text/event-stream") });
	Response->Headers.Add(TEXT("Cache-Control"), { TEXT("no-cache") });
	Response->Headers.Add(TEXT("Connection"), { TEXT("keep-alive") });
	AddCommonHeaders(*Response, SessionId);

	// Convert response body to UTF8
	FTCHARToUTF8 Converter(*SSEResponse);
	Response->Body.Append((const uint8*)Converter.Get(), Converter.Length());

	OnComplete(MoveTemp(Response));
	return true;
}

bool FMCPServer::HandleMessageRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (RejectIfBrowserRequest(Request, OnComplete)) return true;

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [MSG-1] HandleMessageRequest entered, body size: %d bytes"), Request.Body.Num());

	// Extract session ID from query string (optional validation)
	FString SessionId;
	for (const auto& QueryParam : Request.QueryParams)
	{
		if (QueryParam.Key == TEXT("sessionId"))
		{
			SessionId = QueryParam.Value;
			break;
		}
	}

	if (!SessionId.IsEmpty())
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [MSG-2] Session ID: %s"), *SessionId);
	}

	FString RequestBody;
	TSharedPtr<FJsonObject> JsonRequest;
	const bool bParsed = ParseJsonRequestBody(Request, RequestBody, JsonRequest);
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [MSG-2] Body: %d bytes -> %d chars"), Request.Body.Num(), RequestBody.Len());
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [MSG-3] Request body (%d chars): %s"), RequestBody.Len(), *RequestBody);
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [MSG-4] Parse result: %s"), bParsed ? TEXT("valid") : TEXT("invalid"));

	if (!bParsed || !JsonRequest.IsValid())
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("MCPServer: [MSG-7] JSON parse FAILED"));
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("MCPServer: [MSG-7a] Body size: %d bytes, String length: %d chars"), Request.Body.Num(), RequestBody.Len());
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("MCPServer: [MSG-7b] First 500 chars: %.500s"), *RequestBody);
		if (RequestBody.Len() > 500)
		{
			UE_LOG(LogAgentIntegrationKit, Warning, TEXT("MCPServer: [MSG-7c] Last 200 chars: %s"), *RequestBody.Right(200));
		}

		// Log raw bytes to detect encoding/corruption issues
		if (Request.Body.Num() > 0)
		{
			FString FirstBytes, LastBytes;
			int32 BytesToLog = FMath::Min(50, Request.Body.Num());
			for (int32 i = 0; i < BytesToLog; i++)
			{
				FirstBytes += FString::Printf(TEXT("%02X "), Request.Body[i]);
			}
			UE_LOG(LogAgentIntegrationKit, Warning, TEXT("MCPServer: [MSG-7d] First %d raw bytes (hex): %s"), BytesToLog, *FirstBytes);

			int32 StartIdx = FMath::Max(0, Request.Body.Num() - 30);
			for (int32 i = StartIdx; i < Request.Body.Num(); i++)
			{
				LastBytes += FString::Printf(TEXT("%02X "), Request.Body[i]);
			}
			UE_LOG(LogAgentIntegrationKit, Warning, TEXT("MCPServer: [MSG-7e] Last %d raw bytes (hex): %s"), Request.Body.Num() - StartIdx, *LastBytes);
		}

		TSharedPtr<FJsonObject> ErrorResponse = CreateErrorResponse(nullptr, -32700, TEXT("Parse error"));
		FString ResponseStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseStr);
		FJsonSerializer::Serialize(ErrorResponse.ToSharedRef(), Writer);

		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(ResponseStr, TEXT("application/json"));
		AddCommonHeaders(*Response, SessionId);
		OnComplete(MoveTemp(Response));
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [MSG-8] Error response sent"));
		return true;
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [MSG-7] JSON parsed, extracting method..."));

	// Check for tools/call - use async handler with timeout
	FString Method;
	JsonRequest->TryGetStringField(TEXT("method"), Method);
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [MSG-8] Method: '%s'"), *Method);

	if (Method == TEXT("tools/call"))
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [MSG-9] Routing to HandleToolsCallAsync"));
		TSharedPtr<FJsonValue> Id = JsonRequest->TryGetField(TEXT("id"));
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [MSG-10] Id field present: %s"), Id.IsValid() ? TEXT("yes") : TEXT("no"));

		TSharedPtr<FJsonObject> Params;
		if (JsonRequest->HasField(TEXT("params")))
		{
			Params = JsonRequest->GetObjectField(TEXT("params"));
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [MSG-11] Params extracted"));
		}
		else
		{
			Params = MakeShared<FJsonObject>();
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [MSG-11] No params"));
		}

		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [MSG-12] Calling HandleToolsCallAsync..."));
		bool bResult = HandleToolsCallAsync(Id, Params, OnComplete);
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [MSG-13] HandleToolsCallAsync returned: %s"), bResult ? TEXT("true") : TEXT("false"));
		return bResult;
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [MSG-9] Routing to ProcessJsonRpcRequest"));

	// Process other JSON-RPC requests synchronously
	TSharedPtr<FJsonObject> JsonResponse = ProcessJsonRpcRequest(JsonRequest);
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [MSG-10] ProcessJsonRpcRequest returned, valid: %s"), JsonResponse.IsValid() ? TEXT("true") : TEXT("false"));

	// For notifications (nullptr response), send empty 202 Accepted
	if (!JsonResponse.IsValid())
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [MSG-11] Notification, sending 202"));
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(FString(), TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::Accepted;
		AddCommonHeaders(*Response);
		OnComplete(MoveTemp(Response));
		return true;
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [MSG-11] Serializing response..."));

	// Serialize response
	FString ResponseStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseStr);
	FJsonSerializer::Serialize(JsonResponse.ToSharedRef(), Writer);

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [MSG-12] Sending response: %s"), *ResponseStr);

	TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(ResponseStr, TEXT("application/json"));
	AddCommonHeaders(*Response);
	OnComplete(MoveTemp(Response));

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [MSG-13] HandleMessageRequest complete"));
	return true;
}

bool FMCPServer::HandleRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// Legacy handler - redirect to Streamable HTTP handler which handles both protocols
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: Legacy HandleRequest redirecting to StreamableHTTP handler"));
	return HandleStreamableHTTPRequest(Request, OnComplete);
}

TSharedPtr<FJsonObject> FMCPServer::ProcessJsonRpcRequest(const TSharedPtr<FJsonObject>& Request)
{
	// Validate JSON-RPC structure
	FString JsonRpcVersion;
	if (!Request->TryGetStringField(TEXT("jsonrpc"), JsonRpcVersion) || JsonRpcVersion != TEXT("2.0"))
	{
		return CreateErrorResponse(nullptr, -32600, TEXT("Invalid Request: missing or invalid jsonrpc version"));
	}

	// Check if this is a notification (no id field) - notifications don't get responses
	bool bIsNotification = !Request->HasField(TEXT("id"));
	TSharedPtr<FJsonValue> Id = Request->TryGetField(TEXT("id"));

	FString Method;
	if (!Request->TryGetStringField(TEXT("method"), Method))
	{
		return CreateErrorResponse(Id, -32600, TEXT("Invalid Request: missing method"));
	}

	TSharedPtr<FJsonObject> Params;
	if (Request->HasField(TEXT("params")))
	{
		Params = Request->GetObjectField(TEXT("params"));
	}
	else
	{
		Params = MakeShared<FJsonObject>();
	}

	// Dispatch to handler
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: Processing method: %s"), *Method);

	if (Method == TEXT("initialize"))
	{
		return HandleInitialize(Id, Params);
	}
	else if (Method == TEXT("tools/list"))
	{
		return HandleToolsList(Id, Params);
	}
	else if (Method == TEXT("tools/call"))
	{
		return HandleToolsCall(Id, Params);
	}
	// ACP methods that Claude Code may send to MCP servers - handle as no-ops
	else if (Method == TEXT("setSessionMode"))
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: setSessionMode called (no-op) - modeId: %s"),
			Params.IsValid() ? *Params->GetStringField(TEXT("modeId")) : TEXT("unknown"));
		return CreateResponse(Id, MakeShared<FJsonObject>());
	}
	else if (Method == TEXT("unstable/setSessionModel"))
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: unstable/setSessionModel called (no-op) - modelId: %s"),
			Params.IsValid() ? *Params->GetStringField(TEXT("modelId")) : TEXT("unknown"));
		return CreateResponse(Id, MakeShared<FJsonObject>());
	}
	else if (Method == TEXT("notifications/initialized") || Method.StartsWith(TEXT("notifications/")))
	{
		// Client notification - no response should be sent for notifications
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: Received notification: %s"), *Method);
		return nullptr;  // nullptr indicates no response needed
	}
	else
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("MCPServer: Unknown method: %s"), *Method);
		return CreateErrorResponse(Id, -32601, FString::Printf(TEXT("Method not found: %s"), *Method));
	}
}

TSharedPtr<FJsonObject> FMCPServer::HandleInitialize(TSharedPtr<FJsonValue> Id, TSharedPtr<FJsonObject> Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	// Protocol version - match what the client sends or use latest
	FString ClientProtocolVersion;
	if (Params.IsValid() && Params->TryGetStringField(TEXT("protocolVersion"), ClientProtocolVersion))
	{
		const FString NegotiatedProtocolVersion = ResolveProtocolVersion(ClientProtocolVersion);
		Result->SetStringField(TEXT("protocolVersion"), NegotiatedProtocolVersion);
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("MCPServer: Initialize - negotiated protocol version: %s"), *NegotiatedProtocolVersion);
	}
	else
	{
		Result->SetStringField(TEXT("protocolVersion"), MCP_DEFAULT_PROTOCOL_VERSION);
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("MCPServer: Initialize - using default protocol version: %s"), MCP_DEFAULT_PROTOCOL_VERSION);
	}

	// Server info
	TSharedPtr<FJsonObject> ServerInfoObj = MakeShared<FJsonObject>();
	ServerInfoObj->SetStringField(TEXT("name"), ServerInfo.Name);
	ServerInfoObj->SetStringField(TEXT("version"), ServerInfo.Version);
	ServerInfoObj->SetNumberField(TEXT("_rev"), 4254); // Internal build revision for telemetry
	Result->SetObjectField(TEXT("serverInfo"), ServerInfoObj);

	// Capabilities
	TSharedPtr<FJsonObject> CapabilitiesObj = MakeShared<FJsonObject>();
	if (Capabilities.bSupportsTools)
	{
		TSharedPtr<FJsonObject> ToolsObj = MakeShared<FJsonObject>();
		CapabilitiesObj->SetObjectField(TEXT("tools"), ToolsObj);
	}
	Result->SetObjectField(TEXT("capabilities"), CapabilitiesObj);

	return CreateResponse(Id, Result);
}

TSharedPtr<FJsonObject> FMCPServer::HandleToolsList(TSharedPtr<FJsonValue> Id, TSharedPtr<FJsonObject> Params)
{
	const UACPSettings* Settings = UACPSettings::Get();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	// Refresh the Lua tool description so it includes bindings from extension modules
	// that loaded after the MCP server started (e.g. AIK_Python deferred loading)
	FNeoStackToolRegistry& ToolRegistry = FNeoStackToolRegistry::Get();
	for (auto& Pair : RegisteredTools)
	{
		if (FNeoStackToolBase* Tool = ToolRegistry.GetTool(Pair.Key))
		{
			Pair.Value.Description = Tool->GetDescription();
		}
	}

	TArray<TSharedPtr<FJsonValue>> ToolsArray;
	for (const auto& Pair : RegisteredTools)
	{
		if (Settings && !Settings->IsToolEnabled(Pair.Key))
		{
			continue;
		}

		// Apply description override from active profile (work on a copy)
		FMCPToolDefinition ToolDef = Pair.Value;
		if (Settings)
		{
			ToolDef.Description = Settings->GetEffectiveToolDescription(Pair.Key, Pair.Value.Description);
		}

		TSharedPtr<FJsonObject> ToolObj = CreateToolSchema(ToolDef);
		ToolsArray.Add(MakeShared<FJsonValueObject>(ToolObj));
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: - Tool: %s"), *Pair.Key);
	}

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("MCPServer: tools/list returning %d tools"), ToolsArray.Num());
	Result->SetArrayField(TEXT("tools"), ToolsArray);

	// Notify listeners that an MCP client has discovered tools (e.g., chat UI unblocks input)
	bClientDiscoveredTools = true;
	AsyncTask(ENamedThreads::GameThread, [this]()
	{
		OnClientToolsDiscovered.Broadcast();
	});

	return CreateResponse(Id, Result);
}

TSharedPtr<FJsonObject> FMCPServer::HandleToolsCall(TSharedPtr<FJsonValue> Id, TSharedPtr<FJsonObject> Params)
{
	FString ToolName;
	if (!Params->TryGetStringField(TEXT("name"), ToolName))
	{
		return CreateErrorResponse(Id, -32602, TEXT("Invalid params: missing 'name'"));
	}

	FMCPToolDefinition* Tool = RegisteredTools.Find(ToolName);
	if (!Tool)
	{
		return CreateErrorResponse(Id, -32602, FString::Printf(TEXT("Unknown tool: %s"), *ToolName));
	}

	TSharedPtr<FJsonObject> Arguments;
	if (Params->HasField(TEXT("arguments")))
	{
		Arguments = Params->GetObjectField(TEXT("arguments"));
	}
	else
	{
		Arguments = MakeShared<FJsonObject>();
	}

	// Get timeout from settings
	int32 TimeoutSeconds = UACPSettings::Get()->ToolExecutionTimeoutSeconds;

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("MCPServer: Executing tool '%s' (timeout: %ds)"), *ToolName, TimeoutSeconds);
	double StartTime = FPlatformTime::Seconds();

	auto Handler = Tool->Handler;
	FMCPToolResult ToolResult;

	// Execute tool synchronously - tools require game thread context for UObject/Python access
	ToolResult = Handler(Arguments);

	double Duration = FPlatformTime::Seconds() - StartTime;

	// Log warning if tool exceeded timeout (informational - we can't forcibly interrupt game thread tools)
	if (TimeoutSeconds > 0 && Duration > TimeoutSeconds)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("MCPServer: Tool '%s' took %.1fs (exceeded %ds timeout setting)"),
			*ToolName, Duration, TimeoutSeconds);
	}

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("MCPServer: Tool '%s' completed in %.2fs (success: %s)"),
		*ToolName, Duration, ToolResult.bSuccess ? TEXT("true") : TEXT("false"));

	// Broadcast tool execution for UI (so ACP mode can capture images)
	if (ToolResult.Images.Num() > 0)
	{
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("MCPServer: Broadcasting tool result with %d images"), ToolResult.Images.Num());
		OnToolExecuted.Broadcast(ToolName, ToolResult.bSuccess, ToolResult);
	}

	// Build response
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> ContentArray;

	// Add text content block
	TSharedPtr<FJsonObject> ContentObj = MakeShared<FJsonObject>();
	ContentObj->SetStringField(TEXT("type"), TEXT("text"));
	ContentObj->SetStringField(TEXT("text"), ToolResult.bSuccess ? ToolResult.Content : ToolResult.ErrorMessage);
	ContentArray.Add(MakeShared<FJsonValueObject>(ContentObj));

	// Add image content blocks if present
	for (const FMCPToolResultImage& Image : ToolResult.Images)
	{
		TSharedPtr<FJsonObject> ImageObj = MakeShared<FJsonObject>();
		ImageObj->SetStringField(TEXT("type"), TEXT("image"));
		ImageObj->SetStringField(TEXT("data"), Image.Base64Data);
		ImageObj->SetStringField(TEXT("mimeType"), Image.MimeType);
		ContentArray.Add(MakeShared<FJsonValueObject>(ImageObj));

		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: Tool result includes image (%dx%d, %s)"),
			Image.Width, Image.Height, *Image.MimeType);
	}

	Result->SetArrayField(TEXT("content"), ContentArray);

	if (!ToolResult.bSuccess)
	{
		Result->SetBoolField(TEXT("isError"), true);
	}

	return CreateResponse(Id, Result);
}

bool FMCPServer::HandleToolsCallAsync(TSharedPtr<FJsonValue> Id, TSharedPtr<FJsonObject> Params, const FHttpResultCallback& OnComplete)
{
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-1] HandleToolsCallAsync entered"));

	FString ToolName;
	if (!Params->TryGetStringField(TEXT("name"), ToolName))
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("MCPServer: [ASYNC-2] Missing tool name in params"));
		TSharedPtr<FJsonObject> ErrorResponse = CreateErrorResponse(Id, -32602, TEXT("Invalid params: missing 'name'"));
		FString ResponseStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseStr);
		FJsonSerializer::Serialize(ErrorResponse.ToSharedRef(), Writer);
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(ResponseStr, TEXT("application/json"));
		AddCommonHeaders(*Response);
		OnComplete(MoveTemp(Response));
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-3] Error response sent for missing name"));
		return true;
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-2] Tool name: '%s'"), *ToolName);
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-3] Looking up tool in registry (%d tools registered)..."), RegisteredTools.Num());

	FMCPToolDefinition* Tool = RegisteredTools.Find(ToolName);
	if (!Tool)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("MCPServer: [ASYNC-4] Tool not found: '%s'"), *ToolName);
		TSharedPtr<FJsonObject> ErrorResponse = CreateErrorResponse(Id, -32602, FString::Printf(TEXT("Unknown tool: %s"), *ToolName));
		FString ResponseStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseStr);
		FJsonSerializer::Serialize(ErrorResponse.ToSharedRef(), Writer);
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(ResponseStr, TEXT("application/json"));
		AddCommonHeaders(*Response);
		OnComplete(MoveTemp(Response));
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-5] Error response sent for unknown tool"));
		return true;
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-4] Tool found, extracting arguments..."));

	TSharedPtr<FJsonObject> Arguments;
	if (Params->HasField(TEXT("arguments")))
	{
		Arguments = Params->GetObjectField(TEXT("arguments"));
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-5] Arguments extracted"));
	}
	else
	{
		Arguments = MakeShared<FJsonObject>();
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-5] No arguments, using empty object"));
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-6] Getting timeout from settings..."));
	int32 TimeoutSeconds = UACPSettings::Get()->ToolExecutionTimeoutSeconds;
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-7] Timeout=%d seconds"), TimeoutSeconds);

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-8] Creating shared state for timeout coordination..."));

	// Shared state for coordinating between timeout and completion
	TSharedPtr<FCriticalSection> Lock = MakeShared<FCriticalSection>();
	TSharedPtr<bool> bResponseSent = MakeShared<bool>(false);

	auto Handler = Tool->Handler;
	double StartTime = FPlatformTime::Seconds();

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-9] Handler captured, StartTime recorded"));

	// Start timeout watchdog on background thread
	if (TimeoutSeconds > 0)
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-10] Starting timeout watchdog thread (%d seconds)..."), TimeoutSeconds);
		Async(EAsyncExecution::ThreadPool, [this, Id, ToolName, TimeoutSeconds, Lock, bResponseSent, OnComplete]()
		{
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [TIMEOUT] Watchdog thread started, sleeping for %d seconds..."), TimeoutSeconds);
			FPlatformProcess::Sleep(static_cast<float>(TimeoutSeconds));
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [TIMEOUT] Watchdog woke up, checking if response already sent..."));

			FScopeLock ScopeLock(Lock.Get());
			if (*bResponseSent)
			{
				UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [TIMEOUT] Response already sent, watchdog exiting"));
				return; // Tool already completed
			}
			*bResponseSent = true;

			UE_LOG(LogAgentIntegrationKit, Warning, TEXT("MCPServer: [TIMEOUT] Tool '%s' timed out after %d seconds, sending timeout response"), *ToolName, TimeoutSeconds);

			// Build timeout response
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> ContentArray;
			TSharedPtr<FJsonObject> ContentObj = MakeShared<FJsonObject>();
			ContentObj->SetStringField(TEXT("type"), TEXT("text"));
			ContentObj->SetStringField(TEXT("text"), FString::Printf(
				TEXT("Tool '%s' timed out after %d seconds. The operation may still be running in the background. "
				     "You can adjust timeout in Project Settings > Plugins > Agent Integration Kit."),
				*ToolName, TimeoutSeconds));
			ContentArray.Add(MakeShared<FJsonValueObject>(ContentObj));
			Result->SetArrayField(TEXT("content"), ContentArray);
			Result->SetBoolField(TEXT("isError"), true);

			TSharedPtr<FJsonObject> JsonResponse = MakeShared<FJsonObject>();
			JsonResponse->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
			JsonResponse->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
			JsonResponse->SetObjectField(TEXT("result"), Result);

			FString ResponseStr;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseStr);
			FJsonSerializer::Serialize(JsonResponse.ToSharedRef(), Writer);

			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [TIMEOUT] Calling OnComplete with timeout response..."));
			TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(ResponseStr, TEXT("application/json"));
			this->AddCommonHeaders(*Response);
			OnComplete(MoveTemp(Response));
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [TIMEOUT] Timeout response sent"));
		});
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-11] Timeout watchdog thread launched"));
	}
	else
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-10] Timeout disabled (0 seconds), no watchdog"));
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-12] *** CALLING TOOL HANDLER FOR '%s' ***"), *ToolName);

	// Execute tool synchronously on current thread (game thread context required)
	FMCPToolResult ToolResult = Handler(Arguments);

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-13] *** TOOL HANDLER RETURNED ***"));

	double Duration = FPlatformTime::Seconds() - StartTime;
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-14] Tool execution took %.2f seconds"), Duration);

	// Check if timeout already sent response
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-15] Acquiring lock to check response state..."));
	{
		FScopeLock ScopeLock(Lock.Get());
		if (*bResponseSent)
		{
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-16] Timeout already sent response, returning"));
			return true; // Response already sent by timeout
		}
		*bResponseSent = true;
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-16] We're first to respond, continuing"));
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-17] Tool '%s' completed in %.2fs (success: %s)"),
		*ToolName, Duration, ToolResult.bSuccess ? TEXT("true") : TEXT("false"));

	// Broadcast for UI
	if (ToolResult.Images.Num() > 0)
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-18] Broadcasting %d images for UI"), ToolResult.Images.Num());
		OnToolExecuted.Broadcast(ToolName, ToolResult.bSuccess, ToolResult);
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-18] Building response JSON..."));

	// Build success response
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArray;

	TSharedPtr<FJsonObject> ContentObj = MakeShared<FJsonObject>();
	ContentObj->SetStringField(TEXT("type"), TEXT("text"));
	ContentObj->SetStringField(TEXT("text"), ToolResult.bSuccess ? ToolResult.Content : ToolResult.ErrorMessage);
	ContentArray.Add(MakeShared<FJsonValueObject>(ContentObj));

	for (const FMCPToolResultImage& Image : ToolResult.Images)
	{
		TSharedPtr<FJsonObject> ImageObj = MakeShared<FJsonObject>();
		ImageObj->SetStringField(TEXT("type"), TEXT("image"));
		ImageObj->SetStringField(TEXT("data"), Image.Base64Data);
		ImageObj->SetStringField(TEXT("mimeType"), Image.MimeType);
		ContentArray.Add(MakeShared<FJsonValueObject>(ImageObj));
	}

	Result->SetArrayField(TEXT("content"), ContentArray);
	if (!ToolResult.bSuccess)
	{
		Result->SetBoolField(TEXT("isError"), true);
	}

	TSharedPtr<FJsonObject> JsonResponse = MakeShared<FJsonObject>();
	JsonResponse->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	JsonResponse->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
	JsonResponse->SetObjectField(TEXT("result"), Result);

	FString ResponseStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseStr);
	FJsonSerializer::Serialize(JsonResponse.ToSharedRef(), Writer);

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-19] Calling OnComplete with success response..."));
	TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(ResponseStr, TEXT("application/json"));
	AddCommonHeaders(*Response);
	OnComplete(MoveTemp(Response));

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [ASYNC-20] HandleToolsCallAsync complete, returning true"));
	return true;
}

TSharedPtr<FJsonObject> FMCPServer::CreateToolSchema(const FMCPToolDefinition& Tool)
{
	TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
	ToolObj->SetStringField(TEXT("name"), Tool.Name);
	ToolObj->SetStringField(TEXT("description"), Tool.Description);

	if (Tool.InputSchema.IsValid())
	{
		ToolObj->SetObjectField(TEXT("inputSchema"), Tool.InputSchema);
	}

	return ToolObj;
}

TSharedPtr<FJsonObject> FMCPServer::CreateResponse(TSharedPtr<FJsonValue> Id, TSharedPtr<FJsonObject> Result)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Response->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
	Response->SetObjectField(TEXT("result"), Result);
	return Response;
}

TSharedPtr<FJsonObject> FMCPServer::CreateErrorResponse(TSharedPtr<FJsonValue> Id, int32 Code, const FString& Message)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Response->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());

	TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
	ErrorObj->SetNumberField(TEXT("code"), Code);
	ErrorObj->SetStringField(TEXT("message"), Message);
	Response->SetObjectField(TEXT("error"), ErrorObj);

	return Response;
}

FString FMCPServer::GetSessionIdFromHeader(const FHttpServerRequest& Request)
{
	// Look for MCP-Session-Id header (case-insensitive search)
	for (const auto& Header : Request.Headers)
	{
		if (Header.Key.Equals(TEXT("Mcp-Session-Id"), ESearchCase::IgnoreCase))
		{
			if (Header.Value.Num() > 0)
			{
				return Header.Value[0];
			}
		}
	}
	return FString();
}

void FMCPServer::AddCommonHeaders(FHttpServerResponse& Response, const FString& SessionId, const FString& ProtocolVersion)
{
	const UACPSettings* Settings = UACPSettings::Get();
	bool bAllowBrowser = Settings && Settings->bAllowBrowserMCPRequests;

	if (bAllowBrowser)
	{
		Response.Headers.Add(TEXT("Access-Control-Allow-Origin"), { TEXT("*") });
		Response.Headers.Add(TEXT("Access-Control-Allow-Methods"), { TEXT("GET, POST, OPTIONS, DELETE") });
		Response.Headers.Add(TEXT("Access-Control-Allow-Headers"), { TEXT("Content-Type, Accept, MCP-Session-Id, Mcp-Session-Id, MCP-Protocol-Version, Mcp-Protocol-Version") });
		Response.Headers.Add(TEXT("Access-Control-Expose-Headers"), { TEXT("MCP-Session-Id, MCP-Protocol-Version") });
	}

	if (!SessionId.IsEmpty())
	{
		Response.Headers.Add(TEXT("MCP-Session-Id"), { SessionId });
	}

	const FString EffectiveProtocol = ProtocolVersion.IsEmpty()
		? MCP_DEFAULT_PROTOCOL_VERSION
		: ProtocolVersion;
	if (!EffectiveProtocol.IsEmpty())
	{
		Response.Headers.Add(TEXT("MCP-Protocol-Version"), { EffectiveProtocol });
	}
}

bool FMCPServer::RejectIfBrowserRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	const UACPSettings* Settings = UACPSettings::Get();
	if (Settings && Settings->bAllowBrowserMCPRequests)
	{
		return false;
	}

	// Check for Origin header - browsers always send this on cross-origin requests,
	// CLI tools (Claude Code, Gemini CLI, Codex, etc.) do not
	for (const auto& Header : Request.Headers)
	{
		if (Header.Key.Equals(TEXT("Origin"), ESearchCase::IgnoreCase))
		{
			FString OriginValue = Header.Value.Num() > 0 ? Header.Value[0] : TEXT("(empty)");
			UE_LOG(LogAgentIntegrationKit, Warning,
				TEXT("MCPServer: Rejected browser request from Origin '%s'. "
				     "Enable 'Allow Browser Requests' in Project Settings > Plugins > Agent Integration Kit if you need browser access."),
				*OriginValue);

			TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(
				FString(TEXT("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32001,\"message\":\"Browser requests are not allowed. Enable 'Allow Browser Requests' in plugin settings.\"}}")),
				TEXT("application/json"));
			Response->Code = EHttpServerResponseCodes::Denied;
			OnComplete(MoveTemp(Response));
			return true;
		}
	}

	return false;
}

bool FMCPServer::HandleOptionsRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (RejectIfBrowserRequest(Request, OnComplete)) return true;

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: OPTIONS preflight request"));

	TUniquePtr<FHttpServerResponse> Response = MakeUnique<FHttpServerResponse>();
	Response->Code = EHttpServerResponseCodes::Ok;  // 200 OK for CORS preflight
	AddCommonHeaders(*Response, FString(), ResolveProtocolVersion(GetProtocolVersionFromHeader(Request)));

	OnComplete(MoveTemp(Response));
	return true;
}

bool FMCPServer::HandleStreamableHTTPDelete(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (RejectIfBrowserRequest(Request, OnComplete)) return true;

	const FString SessionId = GetSessionIdFromHeader(Request);
	FString ProtocolVersion = ResolveProtocolVersion(GetProtocolVersionFromHeader(Request));

	if (SessionId.IsEmpty())
	{
		TSharedPtr<FJsonObject> ErrorResponse = CreateErrorResponse(nullptr, -32602, TEXT("Missing MCP-Session-Id header"));
		FString ResponseStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseStr);
		FJsonSerializer::Serialize(ErrorResponse.ToSharedRef(), Writer);

		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(ResponseStr, TEXT("application/json"));
		Response->Code = static_cast<EHttpServerResponseCodes>(400);
		AddCommonHeaders(*Response, FString(), ProtocolVersion);
		OnComplete(MoveTemp(Response));
		return true;
	}

	{
		FScopeLock Lock(&StreamableSessionLock);
		if (const FString* StoredProtocol = StreamableSessionProtocols.Find(SessionId))
		{
			ProtocolVersion = *StoredProtocol;
		}

		StreamableSessions.Remove(SessionId);
		StreamableSessionProtocols.Remove(SessionId);
	}

	TUniquePtr<FHttpServerResponse> Response = MakeUnique<FHttpServerResponse>();
	Response->Code = static_cast<EHttpServerResponseCodes>(204);
	AddCommonHeaders(*Response, FString(), ProtocolVersion);
	OnComplete(MoveTemp(Response));
	return true;
}

bool FMCPServer::HandleStreamableHTTPGet(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (RejectIfBrowserRequest(Request, OnComplete)) return true;

	// Log at Verbose level to avoid spam from polling clients
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: Streamable HTTP GET /mcp (SSE notification stream)"));

	// Get session ID from header (optional for polling clients)
	FString SessionId = GetSessionIdFromHeader(Request);
	PruneExpiredStreamableSessions();

	FString ProtocolVersion = ResolveProtocolVersion(GetProtocolVersionFromHeader(Request));
	if (!SessionId.IsEmpty())
	{
		FScopeLock Lock(&StreamableSessionLock);
		if (const FString* StoredProtocol = StreamableSessionProtocols.Find(SessionId))
		{
			ProtocolVersion = *StoredProtocol;
		}
		if (double* LastSeen = StreamableSessions.Find(SessionId))
		{
			*LastSeen = FPlatformTime::Seconds();
		}
	}

	// Return an SSE stream for server-to-client notifications
	// We don't require session validation for GET - clients may poll before/after session
	// For now, just send an empty SSE response since we don't have pending notifications
	TUniquePtr<FHttpServerResponse> Response = MakeUnique<FHttpServerResponse>();
	Response->Code = EHttpServerResponseCodes::Ok;
	Response->Headers.Add(TEXT("Content-Type"), { TEXT("text/event-stream") });
	Response->Headers.Add(TEXT("Cache-Control"), { TEXT("no-cache") });
	Response->Headers.Add(TEXT("Connection"), { TEXT("keep-alive") });
	AddCommonHeaders(*Response, SessionId, ProtocolVersion);

	// Send a comment to keep the connection alive (no actual events for now)
	FString SSEContent = TEXT(": heartbeat\n\n");
	FTCHARToUTF8 Converter(*SSEContent);
	Response->Body.Append((const uint8*)Converter.Get(), Converter.Length());

	OnComplete(MoveTemp(Response));
	return true;
}

bool FMCPServer::HandleStreamableHTTPRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (RejectIfBrowserRequest(Request, OnComplete)) return true;

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [STREAM-1] Streamable HTTP POST /mcp, body size: %d bytes"), Request.Body.Num());
	PruneExpiredStreamableSessions();

	// Get session ID from header (may be empty for initialize request)
	FString SessionId = GetSessionIdFromHeader(Request);
	FString RequestedProtocolVersion = GetProtocolVersionFromHeader(Request);
	FString NegotiatedProtocolVersion = ResolveProtocolVersion(RequestedProtocolVersion);
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [STREAM-2] Session ID from header: '%s'"), *SessionId);

	FString RequestBody;
	TSharedPtr<FJsonObject> JsonRequest;
	const bool bParsed = ParseJsonRequestBody(Request, RequestBody, JsonRequest);
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [STREAM-3] Request body: %s"), *RequestBody);

	if (!bParsed || !JsonRequest.IsValid())
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("MCPServer: [STREAM-4] JSON parse failed"));
		TSharedPtr<FJsonObject> ErrorResponse = CreateErrorResponse(nullptr, -32700, TEXT("Parse error"));
		FString ResponseStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseStr);
		FJsonSerializer::Serialize(ErrorResponse.ToSharedRef(), Writer);

		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(ResponseStr, TEXT("application/json"));
		AddCommonHeaders(*Response, SessionId, NegotiatedProtocolVersion);
		OnComplete(MoveTemp(Response));
		return true;
	}

	// Extract method
	FString Method;
	JsonRequest->TryGetStringField(TEXT("method"), Method);
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [STREAM-5] Method: '%s'"), *Method);

	// Handle initialize specially - create session and return session ID in header
	if (Method == TEXT("initialize"))
	{
		TSharedPtr<FJsonObject> Params;
		if (JsonRequest->HasField(TEXT("params")))
		{
			Params = JsonRequest->GetObjectField(TEXT("params"));
		}
		else
		{
			Params = MakeShared<FJsonObject>();
		}

		FString BodyProtocolVersion;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("protocolVersion"), BodyProtocolVersion) && RequestedProtocolVersion.IsEmpty())
		{
			RequestedProtocolVersion = BodyProtocolVersion;
		}
		NegotiatedProtocolVersion = ResolveProtocolVersion(RequestedProtocolVersion);
		Params->SetStringField(TEXT("protocolVersion"), NegotiatedProtocolVersion);

		// Generate new session ID for this client
		FString NewSessionId = GenerateSessionId();
		{
			FScopeLock Lock(&StreamableSessionLock);
			StreamableSessions.Add(NewSessionId, FPlatformTime::Seconds());
			StreamableSessionProtocols.Add(NewSessionId, NegotiatedProtocolVersion);
		}

		UE_LOG(LogAgentIntegrationKit, Log, TEXT("MCPServer: [STREAM-6] Initialize - created session: %s"), *NewSessionId);

		TSharedPtr<FJsonValue> Id = JsonRequest->TryGetField(TEXT("id"));

		TSharedPtr<FJsonObject> JsonResponse = HandleInitialize(Id, Params);

		FString ResponseStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseStr);
		FJsonSerializer::Serialize(JsonResponse.ToSharedRef(), Writer);

		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(ResponseStr, TEXT("application/json"));
		AddCommonHeaders(*Response, NewSessionId, NegotiatedProtocolVersion);
		OnComplete(MoveTemp(Response));

		UE_LOG(LogAgentIntegrationKit, Log, TEXT("MCPServer: [STREAM-7] Initialize response sent with session ID"));
		return true;
	}

	// For other methods, validate session if provided (but don't require it for compatibility)
	if (!SessionId.IsEmpty())
	{
		FScopeLock Lock(&StreamableSessionLock);
		if (!StreamableSessions.Contains(SessionId))
		{
			// Session ID provided but not valid - could be from legacy client, accept anyway
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [STREAM-6] Unknown session ID, accepting anyway for compatibility"));
		}
		else
		{
			StreamableSessions.Add(SessionId, FPlatformTime::Seconds());
			if (const FString* StoredProtocol = StreamableSessionProtocols.Find(SessionId))
			{
				NegotiatedProtocolVersion = *StoredProtocol;
			}
			else
			{
				StreamableSessionProtocols.Add(SessionId, NegotiatedProtocolVersion);
			}
		}
	}

	// Check for tools/call - use async handler with timeout
	if (Method == TEXT("tools/call"))
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [STREAM-6] Routing to async tools/call handler"));

		TSharedPtr<FJsonValue> Id = JsonRequest->TryGetField(TEXT("id"));

		TSharedPtr<FJsonObject> Params;
		if (JsonRequest->HasField(TEXT("params")))
		{
			Params = JsonRequest->GetObjectField(TEXT("params"));
		}
		else
		{
			Params = MakeShared<FJsonObject>();
		}

		// Wrap OnComplete to add session header
		FString CapturedSessionId = SessionId;
		FString CapturedProtocolVersion = NegotiatedProtocolVersion;
		auto WrappedOnComplete = [this, OnComplete, CapturedSessionId, CapturedProtocolVersion](TUniquePtr<FHttpServerResponse> Response)
		{
			AddCommonHeaders(*Response, CapturedSessionId, CapturedProtocolVersion);
			OnComplete(MoveTemp(Response));
		};

		return HandleToolsCallAsync(Id, Params, WrappedOnComplete);
	}

	// Process other JSON-RPC requests synchronously
	TSharedPtr<FJsonObject> JsonResponse = ProcessJsonRpcRequest(JsonRequest);

	// For notifications (nullptr response), send 202 Accepted
	if (!JsonResponse.IsValid())
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [STREAM-7] Notification, sending 202"));
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(FString(), TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::Accepted;
		AddCommonHeaders(*Response, SessionId, NegotiatedProtocolVersion);
		OnComplete(MoveTemp(Response));
		return true;
	}

	// Serialize and send response
	FString ResponseStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseStr);
	FJsonSerializer::Serialize(JsonResponse.ToSharedRef(), Writer);

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("MCPServer: [STREAM-8] Sending response: %s"), *ResponseStr);

	TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(ResponseStr, TEXT("application/json"));
	AddCommonHeaders(*Response, SessionId, NegotiatedProtocolVersion);
	OnComplete(MoveTemp(Response));

	return true;
}
