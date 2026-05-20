import { writable, derived, get } from 'svelte/store';
import { getAgentUsage, refreshAgentUsage, onUsageUpdated, type AgentRateLimitData } from '$lib/bridge.js';
import { selectedAgent } from '$lib/stores/agents.js';

/** Per-agent rate limit data cache */
export const rateLimitCache = writable<Record<string, AgentRateLimitData>>({});

/** Rate limit data for the currently selected agent */
export const currentRateLimits = derived(
	[rateLimitCache, selectedAgent],
	([$cache, $agent]) => $agent ? $cache[$agent.name] ?? null : null
);

/** Whether we have data OR are loading for the current agent */
export const hasRateLimits = derived(currentRateLimits, (d) => d !== null && (d.hasData || d.isLoading));

/** Whether rate limit data has actual data to show */
export const hasRateLimitData = derived(currentRateLimits, (d) => d !== null && d.hasData);

/** The highest usage percentage across all windows (for the compact bar) */
export const peakUsagePercent = derived(currentRateLimits, (d) => {
	if (!d || !d.hasData) return 0;
	let peak = 0;
	if (d.primary.hasData) peak = Math.max(peak, d.primary.usedPercent);
	if (d.secondary.hasData) peak = Math.max(peak, d.secondary.usedPercent);
	if (d.modelSpecific.hasData) peak = Math.max(peak, d.modelSpecific.usedPercent);
	return Math.round(peak);
});

/** Load usage data for an agent (fires background fetch) */
export async function loadAgentUsage(agentName: string): Promise<void> {
	try {
		const data = await getAgentUsage(agentName);
		rateLimitCache.update((cache) => ({ ...cache, [agentName]: data }));
	} catch (e) {
		console.warn('Failed to load agent usage:', e);
	}
}

/** Force-refresh usage data for the selected agent */
export async function refreshCurrentUsage(): Promise<void> {
	const agent = get(selectedAgent);
	if (!agent) return;
	await refreshAgentUsage(agent.name);
}

/** Bind the push callback from C++ — call once on mount */
export function bindUsageListener(): void {
	onUsageUpdated((agentName, data) => {
		if (agentName === '_meshy') {
			// Meshy balance updated — update meshy fields in all cached entries
			rateLimitCache.update((cache) => {
				const updated = { ...cache };
				for (const key of Object.keys(updated)) {
					updated[key] = { ...updated[key], meshy: data.meshy };
				}
				return updated;
			});
		} else {
			rateLimitCache.update((cache) => ({ ...cache, [agentName]: data }));
		}
	});
}

/** Format a reset time to a human-readable countdown */
export function formatResetTime(isoDate: string): string {
	if (!isoDate) return '';
	const resetAt = new Date(isoDate);
	const now = new Date();
	const diffMs = resetAt.getTime() - now.getTime();
	if (diffMs <= 0) return 'now';
	const diffMin = Math.floor(diffMs / 60000);
	if (diffMin < 60) return `${diffMin}m`;
	const diffH = Math.floor(diffMin / 60);
	if (diffH < 24) return `${diffH}h ${diffMin % 60}m`;
	const diffD = Math.floor(diffH / 24);
	return `${diffD}d ${diffH % 24}h`;
}

/** Format window duration to human label */
export function formatWindowDuration(minutes: number): string {
	if (minutes <= 0) return '';
	if (minutes < 60) return `${minutes}m`;
	const h = Math.floor(minutes / 60);
	if (h < 24) return `${h}h`;
	const d = Math.floor(h / 24);
	return `${d}d`;
}

/** Format last-updated timestamp to relative string */
export function formatLastUpdated(isoDate: string): string {
	if (!isoDate) return '';
	const updated = new Date(isoDate);
	const now = new Date();
	const diffSec = Math.floor((now.getTime() - updated.getTime()) / 1000);
	if (diffSec < 10) return 'Updated just now';
	if (diffSec < 60) return `Updated ${diffSec}s ago`;
	const diffMin = Math.floor(diffSec / 60);
	return `Updated ${diffMin}m ago`;
}

/** Currency symbol from code */
export function currencySymbol(code: string): string {
	switch (code.toUpperCase()) {
		case 'USD': return '$';
		case 'EUR': return '€';
		case 'GBP': return '£';
		default: return code + ' ';
	}
}

/** Color for a usage percentage (matches Slate thresholds: green < 60%, yellow 60-85%, red >= 85%) */
export function usageColor(pct: number): string {
	if (pct < 60) return '#22c55e';
	if (pct < 85) return '#eab308';
	return '#ef4444';
}
