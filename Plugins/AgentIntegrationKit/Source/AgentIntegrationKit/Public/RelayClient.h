// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IWebSocket.h"
#include "Containers/Ticker.h"

/**
 * FRelayClient — Singleton WebSocket client that connects this instance to the
 * AIK relay server for remote access from neostack.dev.
 *
 * Uses UE's IWebSocket (WebSockets module). Authenticates with the user's
 * NeoStack API key (neostack_...). Receives RPC requests from website
 * clients and dispatches them locally. Forwards streaming events (agent
 * messages, state changes, etc.) back through the relay to the website.
 */
class AGENTINTEGRATIONKIT_API FRelayClient
{
public:
	static FRelayClient& Get();

	/** Start the relay connection if remote access is enabled in settings. */
	void Initialize();

	/** Disconnect and clean up. */
	void Shutdown();

	/** Whether we're currently connected and authenticated with the relay. */
	bool IsConnected() const;

	/** Get the instance ID assigned by the relay (empty if not connected). */
	FString GetInstanceId() const;

	/** Send a JSON message to the relay. Thread-safe (dispatches to game thread if needed). */
	void SendMessage(const TSharedRef<FJsonObject>& Message);

	/** Send a raw JSON string to the relay. */
	void SendRawMessage(const FString& JsonString);

	/** Forward an event to all connected web clients via the relay. */
	void SendEvent(const FString& EventName, const TSharedRef<FJsonObject>& Data);

private:
	FRelayClient() = default;
	~FRelayClient();

	FRelayClient(const FRelayClient&) = delete;
	FRelayClient& operator=(const FRelayClient&) = delete;

	/** Create and connect the WebSocket. */
	void Connect();

	/** Disconnect the WebSocket. */
	void Disconnect();

	/** Schedule a reconnection attempt with exponential backoff. */
	void ScheduleReconnect();

	/** Send the auth message with neostack_ API key and instance metadata. */
	void SendAuth();

	/** WebSocket event handlers. */
	void OnConnected();
	void OnConnectionError(const FString& Error);
	void OnClosed(int32 StatusCode, const FString& Reason, bool bWasClean);
	void OnMessage(const FString& Message);

	/** Handle an incoming RPC request from a website client. */
	void HandleRpcRequest(const TSharedPtr<FJsonObject>& Request);

	/** Send an RPC response with a JSON result. */
	void SendRpcResponse(const FString& RequestId, const FString& ResultJson);

	/** Send an RPC error response. */
	void SendRpcError(const FString& RequestId, const FString& Error);

	/** Send heartbeat ping. */
	void SendPing();

	/** Bind to FACPAgentManager and FACPSessionManager delegates for event forwarding. */
	void BindDelegates();
	void UnbindDelegates();

	/** Get instance metadata for the auth message. */
	TSharedRef<FJsonObject> GetInstanceMetadata() const;

	/** Build the default instance name. */
	static FString GetDefaultInstanceName();

private:
	TSharedPtr<IWebSocket> WebSocket;

	/** Instance ID assigned by the relay on auth success. */
	FString InstanceId;

	/** Whether we've successfully authenticated. */
	bool bAuthenticated = false;

	/** Whether Initialize() has been called and we should try to stay connected. */
	bool bEnabled = false;

	/** Reconnection state. */
	int32 ReconnectAttempt = 0;
	FTSTicker::FDelegateHandle ReconnectTickerHandle;
	FTSTicker::FDelegateHandle HeartbeatTickerHandle;

	/** Delegate handles for cleanup. */
	FDelegateHandle OnAgentMessageHandle;
	FDelegateHandle OnAgentStateChangedHandle;
	FDelegateHandle OnPermissionRequestHandle;
	FDelegateHandle OnModelsAvailableHandle;
	FDelegateHandle OnModeChangedHandle;
	FDelegateHandle OnPlanUpdateHandle;
	FDelegateHandle OnCommandsAvailableHandle;
	FDelegateHandle OnSessionCreatedHandle;
	FDelegateHandle OnSessionClosedHandle;
	FDelegateHandle OnUsageUpdatedHandle;
};
