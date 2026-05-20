import { writable } from 'svelte/store';
import {
	type RegistryAgent,
	getRegistryAgents,
	installRegistryAgent,
	uninstallRegistryAgent,
	refreshRegistry,
} from '$lib/bridge.js';
import { loadAgents } from '$lib/stores/agents.js';

// ── State ───────────────────────────────────────────────────────────

export const registryAgents = writable<RegistryAgent[]>([]);
export const registryLoaded = writable(false);
export const registryLoading = writable(false);

/** Track which agents are currently being installed */
export const installingAgents = writable<Set<string>>(new Set());

// ── Actions ─────────────────────────────────────────────────────────

export async function loadRegistry(): Promise<void> {
	registryLoading.set(true);
	try {
		const agents = await getRegistryAgents();
		registryAgents.set(agents);
		registryLoaded.set(true);
	} catch (e) {
		console.warn('Failed to load registry:', e);
	} finally {
		registryLoading.set(false);
	}
}

export async function refreshRegistryData(): Promise<void> {
	await refreshRegistry();
	// After CDN refresh, reload the data
	await loadRegistry();
}

export async function installAgent(agentId: string, method: string = 'auto'): Promise<void> {
	installingAgents.update((set) => {
		const next = new Set(set);
		next.add(agentId);
		return next;
	});

	try {
		await installRegistryAgent(agentId, method);
		// Reload registry panel + sidebar agent list
		await loadRegistry();
		await loadAgents();
	} catch (e) {
		console.warn('Install failed:', e);
	} finally {
		installingAgents.update((set) => {
			const next = new Set(set);
			next.delete(agentId);
			return next;
		});
	}
}

export async function uninstallAgent(agentId: string): Promise<void> {
	try {
		await uninstallRegistryAgent(agentId);
		await loadRegistry();
		await loadAgents();
	} catch (e) {
		console.warn('Uninstall failed:', e);
	}
}
