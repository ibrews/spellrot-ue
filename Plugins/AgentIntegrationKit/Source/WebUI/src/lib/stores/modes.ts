import { writable, get } from 'svelte/store';
import {
	getModes,
	setMode,
	onModesAvailable,
	onModeChanged,
	type ModeInfo,
	type ModeState
} from '$lib/bridge.js';
import { selectedAgent } from '$lib/stores/agents.js';
import { currentSessionId } from '$lib/stores/sessions.js';

/** Available modes for the current agent */
export const availableModes = writable<ModeInfo[]>([]);

/** Currently selected mode ID */
export const currentModeId = writable<string>('');
let lastModeLoadRequestId = 0;

// Cache of last-received modes per agent — push callbacks may arrive before
// selectedAgent is set (e.g. during session/load). We store them here and
// replay when the agent becomes active.
const pendingModesByAgent = new Map<string, ModeState>();

/** Load modes for an agent (called when agent changes) */
export async function loadModesForAgent(agentName: string): Promise<void> {
	const requestId = ++lastModeLoadRequestId;
	const isStillActiveTarget = () => {
		const activeAgent = get(selectedAgent);
		const activeSession = get(currentSessionId);
		return !!activeSession && !!activeAgent && activeAgent.name === agentName;
	};

	// Check if we already have modes cached from a push that arrived early
	const cached = pendingModesByAgent.get(agentName);
	if (cached && cached.modes.length > 0) {
		pendingModesByAgent.delete(agentName);
		availableModes.set(cached.modes);
		currentModeId.set(cached.currentModeId);
		return;
	}

	try {
		const state = await getModes(agentName);
		if (requestId !== lastModeLoadRequestId) return;
		if (!isStillActiveTarget()) return;
		if (state.modes.length > 0) {
			availableModes.set(state.modes);
			currentModeId.set(state.currentModeId);
		}
		// If pull returned empty, modes may not have arrived yet from ACP.
		// The push callback will handle it when they do.
	} catch (e) {
		if (requestId !== lastModeLoadRequestId) return;
		console.warn('Failed to load modes:', e);
		if (isStillActiveTarget()) {
			availableModes.set([]);
			currentModeId.set('');
		}
	}
}

/** Change the active mode */
export async function changeMode(agentName: string, modeId: string): Promise<void> {
	currentModeId.set(modeId);
	await setMode(agentName, modeId);
}

/** Check if currently in plan mode */
export function isInPlanMode(modeId: string): boolean {
	const lower = modeId.toLowerCase();
	return lower === 'plan' || lower === 'architect' || lower.includes('plan');
}

// ── Binding ──────────────────────────────────────────────────────────

let bound = false;

/** Wire up mode callbacks. Call once on mount. */
export function bindModeListener(): void {
	if (bound) return;
	bound = true;

	onModesAvailable((agentName, modeState) => {
		const agent = get(selectedAgent);
		if (agent && agentName === agent.name) {
			// Agent is currently selected — apply immediately
			availableModes.set(modeState.modes);
			if (modeState.currentModeId) {
				currentModeId.set(modeState.currentModeId);
			}
		} else {
			// Agent not selected yet (modes arrived before selectedAgent was set,
			// e.g. during session/load). Cache for when the agent becomes active.
			pendingModesByAgent.set(agentName, modeState);
		}
	});

	onModeChanged((agentName, modeId) => {
		const agent = get(selectedAgent);
		if (agent && agentName === agent.name) {
			currentModeId.set(modeId);
		}
	});
}
