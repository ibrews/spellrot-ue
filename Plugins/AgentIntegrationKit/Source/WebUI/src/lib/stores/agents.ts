import { writable, get } from 'svelte/store';
import { getAgents, getLastUsedAgent, isInUnreal, type AgentInfo } from '$lib/bridge.js';

export type AgentStatus = 'available' | 'not_installed' | 'missing_key' | 'unknown';

export type Agent = {
	id: string;
	name: string;
	shortName: string;
	provider: string;
	description: string;
	color: string;
	letter: string;
	iconUrl?: string;
	registryId?: string;
	status: AgentStatus;
	statusMessage: string;
	isConnected: boolean;
};

/** Generate a consistent color from agent name (DJB2a variant with seed 0x1A4D) */
function hashColor(name: string): string {
	let hash = 0x1A4D; // Seed chosen to spread common short names across the hue wheel
	for (let i = 0; i < name.length; i++) {
		hash = name.charCodeAt(i) + ((hash << 5) - hash);
	}
	const h = Math.abs(hash) % 360;
	return `hsl(${h}, 55%, 55%)`;
}

function agentInfoToAgent(info: AgentInfo): Agent {
	return {
		id: info.id,
		name: info.name,
		shortName: info.name,
		provider: '',
		description: '',
		color: hashColor(info.name),
		letter: info.name.slice(0, 2).toUpperCase(),
		iconUrl: info.iconUrl,
		registryId: info.registryId,
		status: info.status,
		statusMessage: info.statusMessage || '',
		isConnected: info.isConnected
	};
}

export const agents = writable<Agent[]>([]);
export const selectedAgent = writable<Agent | null>(null);
export const agentsLoaded = writable(false);

/** Fetch agents from the UE bridge and update stores, restoring last-used agent */
export async function loadAgents(): Promise<void> {
	try {
		const [infos, lastUsedName] = await Promise.all([getAgents(), getLastUsedAgent()]);
		const mapped = infos
			.map(agentInfoToAgent)
			.filter(a => a.status === 'available' || a.status === 'not_installed' || a.status === 'missing_key');
		agents.set(mapped);
		if (mapped.length > 0) {
			const current = get(selectedAgent);
			const stillExists = current && mapped.find(a => a.id === current.id);
			if (!stillExists) {
				// Restore last-used agent if available, otherwise fall back to first
				const lastUsed = lastUsedName ? mapped.find(a => a.name === lastUsedName) : null;
				selectedAgent.set(lastUsed ?? mapped[0]);
			}
		}
		agentsLoaded.set(true);
	} catch (e) {
		console.warn('Failed to load agents:', e);
		agentsLoaded.set(true);
	}
}

/** Convert a hex or hsl color to one with alpha for Chrome 90 compat (no color-mix/oklch) */
export function withAlpha(color: string, alpha: number): string {
	if (color.startsWith('#')) {
		const hex = color.replace('#', '');
		const r = parseInt(hex.substring(0, 2), 16);
		const g = parseInt(hex.substring(2, 4), 16);
		const b = parseInt(hex.substring(4, 6), 16);
		return `rgba(${r}, ${g}, ${b}, ${alpha})`;
	}
	if (color.startsWith('hsl(')) {
		return color.replace('hsl(', 'hsla(').replace(')', `, ${alpha})`);
	}
	return color;
}

export function statusDotColor(status: AgentStatus): string {
	switch (status) {
		case 'available': return 'bg-emerald-500';
		case 'missing_key': return 'bg-amber-500';
		case 'not_installed': return 'bg-red-500/60';
		default: return 'bg-zinc-500/60';
	}
}
