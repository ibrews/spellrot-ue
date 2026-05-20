import { writable, derived, get } from 'svelte/store';
import { onStateChanged, onMcpStatus } from '$lib/bridge.js';
import { toast } from 'svelte-sonner';

export type AgentConnectionState =
	| 'disconnected'
	| 'connecting'
	| 'initializing'
	| 'ready'
	| 'in_session'
	| 'prompting'
	| 'error';

export type McpToolsStatus = 'none' | 'waiting' | 'ready' | 'timeout';

type SessionState = {
	agentName: string;
	state: AgentConnectionState;
	message: string;
};

/** Per-session agent state map */
export const sessionStates = writable<Record<string, SessionState>>({});

/** Current session ID being tracked (set by the page) */
export const activeSessionId = writable<string | null>(null);

/** Per-session MCP tools readiness status */
export const mcpStatusMap = writable<Record<string, McpToolsStatus>>({});

/** Sessions that recently finished working (prompting → idle). Auto-cleared after 5s. */
export const recentlyFinished = writable<Set<string>>(new Set());

// Track previous states per session for transition detection
const prevSessionStates: Record<string, AgentConnectionState> = {};
// Track auto-clear timeouts per session
const finishedTimers: Record<string, ReturnType<typeof setTimeout>> = {};

/** Derived: state of the currently active session */
export const currentState = derived(
	[sessionStates, activeSessionId],
	([$states, $activeId]) => {
		if (!$activeId) return null;
		return $states[$activeId] ?? null;
	}
);

/** Derived: MCP tools status for the currently active session */
export const currentMcpStatus = derived(
	[mcpStatusMap, activeSessionId],
	([$map, $activeId]) => {
		if (!$activeId) return 'none' as McpToolsStatus;
		return $map[$activeId] ?? 'none';
	}
);

/** Display info for each state */
export const stateDisplay: Record<AgentConnectionState, { label: string; dotClass: string; pulse: boolean }> = {
	disconnected:  { label: 'Disconnected', dotClass: 'bg-zinc-500',    pulse: false },
	connecting:    { label: 'Connecting',   dotClass: 'bg-amber-500',   pulse: true },
	initializing:  { label: 'Initializing', dotClass: 'bg-amber-500',   pulse: true },
	ready:         { label: 'Ready',        dotClass: 'bg-emerald-500', pulse: false },
	in_session:    { label: 'Connected',    dotClass: 'bg-emerald-500', pulse: false },
	prompting:     { label: 'Working',      dotClass: 'bg-blue-500',    pulse: true },
	error:         { label: 'Error',        dotClass: 'bg-red-500',     pulse: false }
};

/** Get sidebar status for a session: 'working', 'finished', or 'idle' */
export function getSessionSidebarStatus(
	sessionId: string,
	states: Record<string, SessionState>,
	finished: Set<string>
): 'working' | 'finished' | 'idle' {
	if (states[sessionId]?.state === 'prompting') return 'working';
	if (finished.has(sessionId)) return 'finished';
	return 'idle';
}

let bound = false;

/** Wire up the bridge callback. Call once on mount. */
export function bindAgentStateListener(): void {
	if (bound) return;
	bound = true;

	onStateChanged((sessionId, agentName, state, message) => {
		const newState = state as AgentConnectionState;
		const prevState = prevSessionStates[sessionId];

		// Toast for background session events
		const isBackground = sessionId !== get(activeSessionId);
		if (isBackground) {
			if (prevState === 'prompting' && (newState === 'ready' || newState === 'in_session')) {
				toast.success(`${agentName} finished`, { description: 'Background session completed' });
			} else if (newState === 'error') {
				toast.error(`${agentName} error`, { description: message || 'Background session encountered an error' });
			}
		}

		// Detect prompting → idle transition (session just finished working)
		if (prevState === 'prompting' && (newState === 'ready' || newState === 'in_session')) {
			// Clear any existing timer for this session
			if (finishedTimers[sessionId]) {
				clearTimeout(finishedTimers[sessionId]);
			}

			// Add to recently finished
			recentlyFinished.update(set => {
				const next = new Set(set);
				next.add(sessionId);
				return next;
			});

			// Auto-clear after 5 seconds
			finishedTimers[sessionId] = setTimeout(() => {
				recentlyFinished.update(set => {
					const next = new Set(set);
					next.delete(sessionId);
					return next;
				});
				delete finishedTimers[sessionId];
			}, 5000);
		}

		// Track state for next transition
		prevSessionStates[sessionId] = newState;

		sessionStates.update(states => ({
			...states,
			[sessionId]: {
				agentName,
				state: newState,
				message
			}
		}));
	});

	onMcpStatus((sessionId, status) => {
		mcpStatusMap.update(map => ({
			...map,
			[sessionId]: status as McpToolsStatus
		}));
	});
}
