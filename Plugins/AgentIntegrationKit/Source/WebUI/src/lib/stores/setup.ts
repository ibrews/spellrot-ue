import { writable, get } from 'svelte/store';
import {
	onInstallProgress,
	onInstallComplete,
	refreshAgentStatus,
	getAgentInstallInfo,
	type AgentInstallInfo
} from '$lib/bridge.js';
import { selectedAgent, loadAgents, type Agent } from '$lib/stores/agents.js';
import { installAgent as installRegistryAgent } from '$lib/stores/registry.js';

export type SetupState =
	| 'idle'
	| 'checking'
	| 'installing'
	| 'cli_missing'
	| 'missing_key'
	| 'success'
	| 'error';

/** The agent currently being set up (null = not in setup mode) */
export const setupAgent = writable<Agent | null>(null);

/** Current setup state */
export const setupState = writable<SetupState>('idle');

/** Progress message from the install process */
export const setupProgress = writable<string>('');

/** Error message if install failed */
export const setupError = writable<string>('');

/** Install info for the current agent */
export const setupInstallInfo = writable<AgentInstallInfo | null>(null);

/** Enter setup mode for an agent */
export function enterSetup(agent: Agent): void {
	setupAgent.set(agent);
	selectedAgent.set(agent);
	setupError.set(agent.statusMessage || '');
	setupProgress.set('');
	setupInstallInfo.set(null);

	// Preload install metadata so we can show copy-paste commands immediately.
	void getAgentInstallInfo(agent.name).then((info) => {
		setupInstallInfo.set(info);
	}).catch(() => {
		// Ignore bridge errors here; setup can still continue with install button flow.
	});

	if (agent.status === 'missing_key') {
		setupState.set('missing_key');
	} else {
		setupState.set('idle');
	}
}

/** Exit setup mode */
export function exitSetup(): void {
	setupAgent.set(null);
	setupState.set('idle');
	setupProgress.set('');
	setupError.set('');
	setupInstallInfo.set(null);
}

/** Start the install process for the current agent */
export async function startInstall(agentName: string): Promise<void> {
	setupState.set('checking');
	setupProgress.set('Checking prerequisites...');

	// Fetch install info so we can show CLI commands if needed
	const info = await getAgentInstallInfo(agentName);
	setupInstallInfo.set(info);

	setupState.set('installing');
	setupProgress.set('Setting up...');

	// Use registry-based install (adds agent ID to settings, lazy download on first use)
	const agent = get(setupAgent);
	const registryId = agent?.registryId;
	if (registryId) {
		try {
			await installRegistryAgent(registryId, 'auto');
			setupState.set('success');
			await loadAgents();
		} catch (e) {
			setupState.set('error');
			setupError.set(e instanceof Error ? e.message : 'Installation failed');
		}
	} else {
		setupState.set('error');
		setupError.set('Agent has no registry ID');
	}
}

/** Re-check agent status (for missing_key retry and cli_missing retry) */
export async function recheckStatus(agentName: string): Promise<void> {
	setupState.set('checking');
	setupProgress.set('Checking...');

	const result = await refreshAgentStatus(agentName);

	if (result.status === 'available') {
		setupState.set('success');
		await loadAgents();
	} else if (result.status === 'missing_key') {
		setupState.set('missing_key');
		setupError.set(result.statusMessage || '');
	} else {
		// Still not available — go back to idle so user can try again
		setupState.set('idle');
		setupProgress.set('');
		setupError.set(result.statusMessage || '');
	}
}

// ── Install Callback Binding ────────────────────────────────────────

let bound = false;

/** Wire up install callbacks. Call once on mount. */
export function bindInstallListeners(): void {
	if (bound) return;
	bound = true;

	onInstallProgress((agentName: string, message: string) => {
		const current = get(setupAgent);
		if (current && current.name === agentName) {
			setupProgress.set(message);
		}
	});

	onInstallComplete((agentName: string, success: boolean, errorMessage: string) => {
		const current = get(setupAgent);
		if (!current || current.name !== agentName) return;

		if (success) {
			setupState.set('success');
			loadAgents();
		} else if (
			errorMessage.includes('CLI is required') ||
			errorMessage.includes('is required but was not found') ||
			errorMessage.toLowerCase().includes('not installed')
		) {
			setupState.set('cli_missing');
			setupError.set(errorMessage);
		} else {
			setupState.set('error');
			setupError.set(errorMessage);
		}
	});
}
