import { writable, get } from 'svelte/store';
import {
	getModels,
	getAllModels,
	setModel as bridgeSetModel,
	getReasoningLevel,
	setReasoningLevel as bridgeSetReasoningLevel,
	onModelsAvailable,
	type ModelInfo
} from '$lib/bridge.js';
import { selectedAgent } from '$lib/stores/agents.js';
import { currentSessionId } from '$lib/stores/sessions.js';

export type ReasoningLevel = 'none' | 'low' | 'medium' | 'high' | 'max';

export const models = writable<ModelInfo[]>([]);
export const currentModelId = writable<string>('');
export const isLoadingModels = writable(false);
export const reasoningLevel = writable<ReasoningLevel>('high');
export const modelBrowserOpen = writable(false);
export const allModels = writable<ModelInfo[]>([]);
export const isLoadingAllModels = writable(false);

/** Per-agent model cache — stores async model pushes for all agents, not just the current one */
type CachedModelState = { models: ModelInfo[]; currentModelId: string };
const modelCache = new Map<string, CachedModelState>();
const fullModelCache = new Map<string, ModelInfo[]>();
let lastModelLoadRequestId = 0;

/** Apply a model state to the active stores */
function applyModelState(state: CachedModelState): void {
	models.set(state.models);
	if (state.currentModelId) {
		currentModelId.set(state.currentModelId);
	} else if (state.models.length > 0) {
		currentModelId.set(state.models[0].id);
	}
}

/** Load models for the given agent from the backend */
export async function loadModelsForAgent(agentName: string): Promise<void> {
	const requestId = ++lastModelLoadRequestId;
	const isStillActiveTarget = () => {
		const activeAgent = get(selectedAgent);
		const activeSession = get(currentSessionId);
		return !!activeSession && !!activeAgent && activeAgent.name === agentName;
	};

	// Check cache first — if models arrived via async push while we were on another agent
	const cached = modelCache.get(agentName);
	if (cached && cached.models.length > 0) {
		if (isStillActiveTarget()) {
			applyModelState(cached);
		}
		if (requestId === lastModelLoadRequestId) {
			isLoadingModels.set(false);
		}
		return;
	}

	isLoadingModels.set(true);
	try {
		const state = await getModels(agentName);
		modelCache.set(agentName, { models: state.models, currentModelId: state.currentModelId });
		if (requestId !== lastModelLoadRequestId) return;

		if (state.models.length > 0) {
			if (isStillActiveTarget()) {
				applyModelState({ models: state.models, currentModelId: state.currentModelId });
			}
		} else {
			// Backend returned empty — models may arrive later via async push.
			// Only clear UI if this load still targets the active session/agent.
			if (isStillActiveTarget()) {
				models.set([]);
				currentModelId.set('');
			}
		}
	} catch (e) {
		if (requestId !== lastModelLoadRequestId) return;
		console.warn('Failed to load models:', e);
		if (isStillActiveTarget()) {
			models.set([]);
			currentModelId.set('');
		}
	} finally {
		if (requestId === lastModelLoadRequestId) {
			isLoadingModels.set(false);
		}
	}
}

/** Load the current reasoning level from the backend */
export async function loadReasoningLevel(agentName: string): Promise<void> {
	try {
		const level = await getReasoningLevel(agentName);
		if (level) {
			reasoningLevel.set(level as ReasoningLevel);
		}
	} catch (e) {
		console.warn('Failed to load reasoning level:', e);
	}
}

/** Change the active model for an agent */
export async function changeModel(agentName: string, modelId: string): Promise<void> {
	const previousModelId = get(currentModelId);
	const nextModelId = modelId;
	if (!nextModelId || nextModelId === 'special:browse_all') {
		return;
	}

	currentModelId.set(nextModelId);

	// Update cache
	const cached = modelCache.get(agentName);
	if (cached) {
		cached.currentModelId = nextModelId;
	}

	try {
		await bridgeSetModel(agentName, nextModelId);

		// If selected from full catalog and not in curated list, add it for immediate UX feedback.
		const curated = get(models);
		if (!curated.some(m => m.id === nextModelId)) {
			const knownFromFull = get(allModels).find(m => m.id === nextModelId);
			if (knownFromFull) {
				const nextCurated = [knownFromFull, ...curated.filter(m => m.id !== 'special:browse_all')];
				models.set(nextCurated);
				const cachedCurated = modelCache.get(agentName);
				if (cachedCurated) {
					cachedCurated.models = nextCurated;
					cachedCurated.currentModelId = nextModelId;
				}
			}
		}
	} catch (error) {
		// Restore UI state if backend update fails
		currentModelId.set(previousModelId);
		if (cached) {
			cached.currentModelId = previousModelId;
		}
		throw error;
	}
}

/** Open searchable full model browser for an agent (OpenRouter). */
export async function openModelBrowser(agentName: string): Promise<void> {
	modelBrowserOpen.set(true);

	const cached = fullModelCache.get(agentName);
	if (cached && cached.length > 0) {
		allModels.set(cached);
		return;
	}

	isLoadingAllModels.set(true);
	try {
		const state = await getAllModels(agentName);
		const list = state.models
			.filter(m => m.id && !m.id.startsWith('special:'))
			.sort((a, b) => a.name.localeCompare(b.name));
		fullModelCache.set(agentName, list);
		allModels.set(list);
	} catch (error) {
		console.warn('Failed to load full model catalog:', error);
		allModels.set([]);
	} finally {
		isLoadingAllModels.set(false);
	}
}

export function closeModelBrowser(): void {
	modelBrowserOpen.set(false);
}

/** Change the reasoning effort level */
export async function changeReasoningLevel(level: ReasoningLevel): Promise<void> {
	const agent = get(selectedAgent);
	if (!agent) return;

	reasoningLevel.set(level);
	await bridgeSetReasoningLevel(agent.name, level);
}

/** Display labels for reasoning levels */
export const reasoningLabels: Record<ReasoningLevel, string> = {
	none: 'Off',
	low: 'Low',
	medium: 'Medium',
	high: 'High',
	max: 'Max'
};

// ── Binding ──────────────────────────────────────────────────────────

let bound = false;

/** Wire up model availability callbacks. Call once on mount.
 *  Caches models per-agent so switching agents shows models immediately. */
export function bindModelsListener(): void {
	if (bound) return;
	bound = true;

	onModelsAvailable((agentName, modelState) => {
		// Always cache, regardless of which agent is currently selected
		modelCache.set(agentName, {
			models: modelState.models,
			currentModelId: modelState.currentModelId
		});

		// Only update active stores if this is for the currently selected agent
		const agent = get(selectedAgent);
		if (agent && agentName === agent.name) {
			applyModelState(modelState);
		}
	});
}
