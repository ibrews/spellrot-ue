// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IWebSocket.h"
#include "Containers/Ticker.h"

/**
 * FLocalClient — Singleton WebSocket client that connects this UE instance
 * to the NeoStack IDE running on the same machine.
 *
 * Discovers the IDE via ~/.neostack/server.json (written by the Tauri app).
 * Uses the same JSON protocol as FRelayClient (auth, rpc_request, rpc_response,
 * event, ping/pong) so the IDE can reuse the same message handling.
 *
 * When connected, the IDE becomes the primary MCP host — agents talk to the IDE,
 * and the IDE forwards tool calls to this plugin over the WebSocket.
 */
class AGENTINTEGRATIONKIT_API FLocalClient
{
public:
	static FLocalClient& Get();

	/** Start polling for IDE discovery file and connect when found. */
	void Initialize();

	/** Disconnect and clean up. */
	void Shutdown();

	/** Whether we're currently connected and authenticated with the IDE. */
	bool IsConnected() const;

	/** Send a JSON message to the IDE. Thread-safe. */
	void SendMessage(const TSharedRef<FJsonObject>& Message);

	/** Send a raw JSON string to the IDE. */
	void SendRawMessage(const FString& JsonString);

	/** Forward an event to the IDE. */
	void SendEvent(const FString& EventName, const TSharedRef<FJsonObject>& Data);

private:
	FLocalClient() = default;
	~FLocalClient();

	FLocalClient(const FLocalClient&) = delete;
	FLocalClient& operator=(const FLocalClient&) = delete;

	/** Read ~/.neostack/server.json and return the WebSocket port, or 0 if unavailable. */
	uint16 DiscoverIdePort() const;

	/** Get the path to the IDE discovery file. */
	static FString GetDiscoveryFilePath();

	/** Create and connect the WebSocket. */
	void Connect(uint16 Port);

	/** Disconnect the WebSocket. */
	void Disconnect();

	/** Schedule a reconnection attempt with exponential backoff. */
	void ScheduleReconnect();

	/** Send the auth message with instance metadata. */
	void SendAuth();

	/** WebSocket event handlers. */
	void OnConnected();
	void OnConnectionError(const FString& Error);
	void OnClosed(int32 StatusCode, const FString& Reason, bool bWasClean);
	void OnMessage(const FString& Message);

	/** Handle an incoming RPC request from the IDE. */
	void HandleRpcRequest(const TSharedPtr<FJsonObject>& Request);

	/** Send an RPC response with a JSON result. */
	void SendRpcResponse(const FString& RequestId, const FString& ResultJson);

	/** Send an RPC error response. */
	void SendRpcError(const FString& RequestId, const FString& Error);

	/** Bind to FAgentService delegates for event forwarding. */
	void BindDelegates();
	void UnbindDelegates();

private:
	TSharedPtr<IWebSocket> WebSocket;

	/** Whether we've successfully authenticated with the IDE. */
	bool bAuthenticated = false;

	/** Whether Initialize() has been called. */
	bool bEnabled = false;

	/** Reconnection state. */
	int32 ReconnectAttempt = 0;
	FTSTicker::FDelegateHandle ReconnectTickerHandle;
	FTSTicker::FDelegateHandle HeartbeatTickerHandle;
	FTSTicker::FDelegateHandle DiscoveryTickerHandle;

	/** Last known IDE port (for detecting port changes on reconnect). */
	uint16 LastKnownPort = 0;

	/** Delegate handles for cleanup. */
	FDelegateHandle OnAgentMessageHandle;
	FDelegateHandle OnAgentStateChangedHandle;
	FDelegateHandle OnPermissionRequestHandle;
	FDelegateHandle OnModelsAvailableHandle;
	FDelegateHandle OnPlanUpdateHandle;
};
