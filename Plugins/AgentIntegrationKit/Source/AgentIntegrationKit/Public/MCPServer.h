// Copyright 2025 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"

/** Delegate for tool execution results with images (for UI display when using ACP) */
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnMCPToolExecuted, const FString& /*ToolName*/, bool /*bSuccess*/, const FMCPToolResult& /*Result*/);

/** Delegate broadcast when a new MCP client completes initialization (tools/list) */
DECLARE_MULTICAST_DELEGATE(FOnMCPClientToolsDiscovered);

/**
 * MCP (Model Context Protocol) Server
 *
 * Exposes Unreal Editor tools to AI agents via HTTP.
 * Supports both transport protocols:
 * - Streamable HTTP (MCP spec 2025-03-26+): POST /mcp with MCP-Session-Id header
 * - Legacy HTTP+SSE (MCP spec 2024-11-05): GET /sse + POST /message
 *
 * Implements the MCP JSON-RPC 2.0 protocol.
 */
class AGENTINTEGRATIONKIT_API FMCPServer
{
public:
	/** Get the singleton instance */
	static FMCPServer& Get();

	/** Delegate broadcast when a tool executes (for capturing images in ACP mode) */
	FOnMCPToolExecuted OnToolExecuted;

	/** Delegate broadcast (on game thread) when a new MCP client discovers tools.
	 *  Used by the chat UI to unblock input after ACP sessions configure MCP. */
	FOnMCPClientToolsDiscovered OnClientToolsDiscovered;

	/** Start the MCP server on the specified port */
	bool Start(int32 Port);

	/** Stop the MCP server */
	void Stop();

	/** Check if the server is running */
	bool IsRunning() const { return bIsRunning; }

	/** Whether any MCP client has completed tools/list discovery */
	bool HasClientDiscoveredTools() const { return bClientDiscoveredTools; }

	/** Get the server port */
	int32 GetPort() const { return ServerPort; }

	/** Register a tool that AI agents can call */
	void RegisterTool(const FMCPToolDefinition& Tool);

	/** Unregister a tool by name */
	void UnregisterTool(const FString& ToolName);

	/** Get all registered tools */
	const TMap<FString, FMCPToolDefinition>& GetRegisteredTools() const { return RegisteredTools; }

private:
	FMCPServer();
	~FMCPServer();

	// Non-copyable
	FMCPServer(const FMCPServer&) = delete;
	FMCPServer& operator=(const FMCPServer&) = delete;

	/** Handle SSE endpoint (GET /sse) - establishes SSE connection (Legacy HTTP+SSE transport) */
	bool HandleSSERequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	/** Handle message endpoint (POST /message) - receives JSON-RPC messages (Legacy HTTP+SSE transport) */
	bool HandleMessageRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	/** Handle Streamable HTTP transport (POST /mcp) - unified endpoint for MCP spec 2025-03-26 */
	bool HandleStreamableHTTPRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	/** Handle Streamable HTTP GET (GET /mcp) - for server-to-client SSE notifications */
	bool HandleStreamableHTTPGet(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	/** Handle CORS preflight (OPTIONS /mcp) - for browser-based clients */
	bool HandleOptionsRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	/** Handle Streamable HTTP session termination (DELETE /mcp) */
	bool HandleStreamableHTTPDelete(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	/** Legacy: Handle incoming HTTP request at /mcp (POST) - redirects to StreamableHTTP handler */
	bool HandleRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	/** Parse JSON-RPC request and dispatch to handler */
	TSharedPtr<FJsonObject> ProcessJsonRpcRequest(const TSharedPtr<FJsonObject>& Request);

	/** MCP Protocol Handlers — Id is preserved as TSharedPtr<FJsonValue> per JSON-RPC 2.0 spec (string, number, or null) */
	TSharedPtr<FJsonObject> HandleInitialize(TSharedPtr<FJsonValue> Id, TSharedPtr<FJsonObject> Params);
	TSharedPtr<FJsonObject> HandleToolsList(TSharedPtr<FJsonValue> Id, TSharedPtr<FJsonObject> Params);
	TSharedPtr<FJsonObject> HandleToolsCall(TSharedPtr<FJsonValue> Id, TSharedPtr<FJsonObject> Params);

	/** Async tool execution with timeout - sends response via callback */
	bool HandleToolsCallAsync(TSharedPtr<FJsonValue> Id, TSharedPtr<FJsonObject> Params, const FHttpResultCallback& OnComplete);

	/** Create JSON-RPC response */
	TSharedPtr<FJsonObject> CreateResponse(TSharedPtr<FJsonValue> Id, TSharedPtr<FJsonObject> Result);
	TSharedPtr<FJsonObject> CreateErrorResponse(TSharedPtr<FJsonValue> Id, int32 Code, const FString& Message);

	/** Register all exposed tools for the MCP server */
	void RegisterDefaultTools();

	/** Create JSON schema for a tool */
	TSharedPtr<FJsonObject> CreateToolSchema(const FMCPToolDefinition& Tool);

	/** Generate a unique session ID */
	FString GenerateSessionId();

	/** Extract protocol version from MCP-Protocol-Version header */
	FString GetProtocolVersionFromHeader(const FHttpServerRequest& Request);

	/** Return true if protocol version is explicitly supported by this server */
	bool IsSupportedProtocolVersion(const FString& Version) const;

	/** Resolve negotiated protocol version (falls back to default if unsupported/empty) */
	FString ResolveProtocolVersion(const FString& RequestedVersion) const;

	/** Remove stale streamable sessions to avoid unbounded growth */
	void PruneExpiredStreamableSessions();

private:
	/** Registered tools */
	TMap<FString, FMCPToolDefinition> RegisteredTools;

	/** HTTP router handle */
	TSharedPtr<IHttpRouter> HttpRouter;

	/** Server port (set by Start()) */
	int32 ServerPort = 0;

	/** Server running state */
	bool bIsRunning = false;

	/** Set to true after the first tools/list response is sent to any client */
	bool bClientDiscoveredTools = false;

	/** Server info */
	FMCPServerInfo ServerInfo;

	/** Server capabilities */
	FMCPServerCapabilities Capabilities;

	/** Route handles for cleanup */
	FHttpRouteHandle SSERouteHandle;
	FHttpRouteHandle MessageRouteHandle;
	FHttpRouteHandle LegacyRouteHandle;
	FHttpRouteHandle StreamableHTTPGetHandle;
	FHttpRouteHandle OptionsRouteHandle;
	FHttpRouteHandle DeleteRouteHandle;

	/** Session counter for generating unique IDs */
	int32 SessionCounter = 0;

	/** Active sessions (for legacy SSE transport) */
	TSet<FString> ActiveSessions;

	/** Streamable HTTP sessions - maps session ID to creation time */
	TMap<FString, double> StreamableSessions;

	/** Negotiated protocol version per streamable HTTP session */
	TMap<FString, FString> StreamableSessionProtocols;

	/** Guards StreamableSessions + StreamableSessionProtocols */
	mutable FCriticalSection StreamableSessionLock;

	/** Extract session ID from MCP-Session-Id header (case-insensitive) */
	FString GetSessionIdFromHeader(const FHttpServerRequest& Request);

	/** Add CORS and common headers to response */
	void AddCommonHeaders(FHttpServerResponse& Response, const FString& SessionId = FString(), const FString& ProtocolVersion = FString());

	/** Check Origin header for browser CSRF protection. Returns true if request was rejected (403 sent). */
	bool RejectIfBrowserRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

};
