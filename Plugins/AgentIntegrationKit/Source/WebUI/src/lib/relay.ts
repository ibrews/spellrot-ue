/**
 * Relay WebSocket client for remote mode.
 * When the WebUI runs on a website (not inside UE), this module manages
 * the WebSocket connection to the AIK relay server and provides relayCall()
 * for RPC requests + event dispatch for streaming callbacks.
 */

export type RelayState = 'disconnected' | 'connecting' | 'authenticating' | 'connected' | 'error';

type PendingRpc = {
	resolve: (result: any) => void;
	reject: (error: Error) => void;
	timer: ReturnType<typeof setTimeout>;
};

type EventCallback = (data: any) => void;

let ws: WebSocket | null = null;
let state: RelayState = 'disconnected';
let instanceId: string = '';
let sessionToken: string = '';
let relayUrl: string = '';

const pendingRpcs = new Map<string, PendingRpc>();
const eventCallbacks = new Map<string, Set<EventCallback>>();
const stateListeners = new Set<(state: RelayState) => void>();

const RPC_TIMEOUT_MS = 30_000;
let reconnectAttempt = 0;
let reconnectTimer: ReturnType<typeof setTimeout> | null = null;

// ── Public API ─────────────────────────────────────────────────────

export function getRelayState(): RelayState {
	return state;
}

export function getInstanceId(): string {
	return instanceId;
}

export function onRelayStateChange(cb: (state: RelayState) => void): () => void {
	stateListeners.add(cb);
	return () => stateListeners.delete(cb);
}

/**
 * Connect to the relay server for a specific instance.
 * @param url - Relay WS URL (e.g., wss://api.neostack.cloud/ws/web/a1b2c3d4)
 * @param token - Better Auth session token for authentication
 * @param targetInstanceId - The instance ID to connect to
 */
export function connectToRelay(url: string, token: string, targetInstanceId: string): Promise<void> {
	return new Promise((resolve, reject) => {
		if (ws) {
			ws.close();
			ws = null;
		}

		relayUrl = url;
		sessionToken = token;
		instanceId = targetInstanceId;
		reconnectAttempt = 0;

		setState('connecting');

		const wsUrl = `${url}/${targetInstanceId}`;
		ws = new WebSocket(wsUrl);

		ws.onopen = () => {
			setState('authenticating');
			ws!.send(JSON.stringify({
				type: 'auth',
				sessionToken: token,
			}));
		};

		let authResolved = false;

		ws.onmessage = (e) => {
			let msg: any;
			try {
				msg = JSON.parse(e.data);
			} catch {
				return;
			}

			// Auth response
			if (msg.type === 'auth_ok' && !authResolved) {
				authResolved = true;
				setState('connected');
				reconnectAttempt = 0;
				resolve();
				return;
			}

			if (msg.type === 'auth_error' && !authResolved) {
				authResolved = true;
				setState('error');
				reject(new Error(msg.message || 'Auth failed'));
				return;
			}

			// RPC response
			if (msg.type === 'rpc_response' && msg.id) {
				const pending = pendingRpcs.get(msg.id);
				if (pending) {
					pendingRpcs.delete(msg.id);
					clearTimeout(pending.timer);
					if (msg.error) {
						pending.reject(new Error(msg.error));
					} else {
						pending.resolve(msg.result);
					}
				}
				return;
			}

			// Event push from instance
			if (msg.type === 'event' && msg.event) {
				const callbacks = eventCallbacks.get(msg.event);
				if (callbacks) {
					for (const cb of callbacks) {
						try { cb(msg.data); } catch (e) { console.error('Event callback error:', e); }
					}
				}
				return;
			}

			// Instance status
			if (msg.type === 'instance_offline') {
				setState('disconnected');
				return;
			}

			// Heartbeat
			if (msg.type === 'ping') {
				ws?.send(JSON.stringify({ type: 'pong' }));
				return;
			}
		};

		ws.onclose = () => {
			if (!authResolved) {
				authResolved = true;
				reject(new Error('Connection closed before auth'));
			}
			setState('disconnected');
			scheduleReconnect();
		};

		ws.onerror = (err) => {
			console.error('[Relay] WebSocket error:', err);
		};
	});
}

export function disconnectRelay(): void {
	if (reconnectTimer) {
		clearTimeout(reconnectTimer);
		reconnectTimer = null;
	}
	reconnectAttempt = -1; // Prevent auto-reconnect
	if (ws) {
		ws.close();
		ws = null;
	}
	setState('disconnected');

	// Reject all pending RPCs
	for (const [id, pending] of pendingRpcs) {
		clearTimeout(pending.timer);
		pending.reject(new Error('Disconnected'));
	}
	pendingRpcs.clear();
}

/**
 * Send an RPC request to the instance via the relay.
 * Returns a Promise that resolves with the instance's response.
 */
export function relayCall<T = any>(method: string, ...args: any[]): Promise<T> {
	return new Promise((resolve, reject) => {
		if (!ws || ws.readyState !== WebSocket.OPEN || state !== 'connected') {
			reject(new Error('Not connected to relay'));
			return;
		}

		const id = crypto.randomUUID();
		const timer = setTimeout(() => {
			pendingRpcs.delete(id);
			reject(new Error(`RPC timeout: ${method}`));
		}, RPC_TIMEOUT_MS);

		pendingRpcs.set(id, { resolve, reject, timer });

		ws.send(JSON.stringify({
			type: 'rpc_request',
			id,
			method,
			args,
		}));
	});
}

/**
 * Subscribe to events pushed by the instance (onMessage, onStateChanged, etc.)
 * Returns an unsubscribe function.
 */
export function onRelayEvent(event: string, callback: EventCallback): () => void {
	if (!eventCallbacks.has(event)) {
		eventCallbacks.set(event, new Set());
	}
	eventCallbacks.get(event)!.add(callback);
	return () => {
		eventCallbacks.get(event)?.delete(callback);
	};
}

// ── Internal ───────────────────────────────────────────────────────

function setState(newState: RelayState): void {
	state = newState;
	for (const cb of stateListeners) {
		try { cb(newState); } catch {}
	}
}

function scheduleReconnect(): void {
	if (reconnectAttempt < 0) return; // Explicitly disconnected
	if (!relayUrl || !sessionToken || !instanceId) return;

	const delay = Math.min(Math.pow(2, reconnectAttempt) * 1000, 30_000);
	reconnectAttempt++;

	console.log(`[Relay] Reconnecting in ${delay / 1000}s (attempt ${reconnectAttempt})`);
	reconnectTimer = setTimeout(() => {
		connectToRelay(relayUrl, sessionToken, instanceId).catch(() => {
			// Will trigger onclose → scheduleReconnect again
		});
	}, delay);
}
