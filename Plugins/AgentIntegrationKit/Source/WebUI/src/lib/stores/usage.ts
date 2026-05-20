import { writable, derived, get } from 'svelte/store';
import { currentSessionId } from '$lib/stores/sessions.js';
import type { StreamingUpdate, ModelUsageEntry } from '$lib/bridge.js';

export type UsageData = {
	inputTokens: number;
	outputTokens: number;
	totalTokens: number;
	cacheReadTokens: number;
	cacheCreationTokens: number;
	reasoningTokens: number;
	costAmount: number;
	costCurrency: string;
	turnCostUSD: number;
	contextUsed: number;
	contextSize: number;
	numTurns: number;
	durationMs: number;
	modelUsage: ModelUsageEntry[];
};

const emptyUsage: UsageData = {
	inputTokens: 0,
	outputTokens: 0,
	totalTokens: 0,
	cacheReadTokens: 0,
	cacheCreationTokens: 0,
	reasoningTokens: 0,
	costAmount: 0,
	costCurrency: 'USD',
	turnCostUSD: 0,
	contextUsed: 0,
	contextSize: 0,
	numTurns: 0,
	durationMs: 0,
	modelUsage: []
};

/** Per-session usage data (persisted across session switches) */
const usageBySession = writable<Record<string, UsageData>>({});

/** Current session usage data (derived from per-session store) */
export const sessionUsage = derived(
	[usageBySession, currentSessionId],
	([$bySession, $sid]) => $sid ? ($bySession[$sid] ?? { ...emptyUsage }) : { ...emptyUsage }
);

/** Whether we have any usage data to display */
export const hasUsage = derived(sessionUsage, (u) => u.contextSize > 0 || u.totalTokens > 0 || u.inputTokens > 0 || u.outputTokens > 0);

/** Context usage percentage (0–100) */
export const contextPercent = derived(sessionUsage, (u) =>
	u.contextSize > 0 ? Math.min(100, Math.round((u.contextUsed / u.contextSize) * 100)) : 0
);

/** Format a token count to human-readable (e.g., 11k, 258k, 1.2M) */
export function formatTokens(count: number): string {
	if (count >= 1_000_000) return `${(count / 1_000_000).toFixed(1)}M`;
	if (count >= 1_000) return `${(count / 1_000).toFixed(1)}k`;
	return `${count}`;
}

/** Format cost to display string */
export function formatCost(amount: number, currency: string = 'USD'): string {
	if (amount <= 0) return '';
	if (currency === 'USD') return `$${amount.toFixed(4)}`;
	return `${amount.toFixed(4)} ${currency}`;
}

/** Update usage from a streaming update — stores per-session */
export function handleUsageUpdate(update: StreamingUpdate, sessionId?: string): void {
	const sid = sessionId ?? get(currentSessionId);
	if (!sid) return;

	const data: UsageData = {
		inputTokens: update.inputTokens ?? 0,
		outputTokens: update.outputTokens ?? 0,
		totalTokens: update.totalTokens ?? 0,
		cacheReadTokens: update.cacheReadTokens ?? 0,
		cacheCreationTokens: update.cacheCreationTokens ?? 0,
		reasoningTokens: update.reasoningTokens ?? 0,
		costAmount: update.costAmount ?? 0,
		costCurrency: update.costCurrency ?? 'USD',
		turnCostUSD: update.turnCostUSD ?? 0,
		contextUsed: update.contextUsed ?? 0,
		contextSize: update.contextSize ?? 0,
		numTurns: update.numTurns ?? 0,
		durationMs: update.durationMs ?? 0,
		modelUsage: update.modelUsage ?? []
	};

	usageBySession.update((all) => ({ ...all, [sid]: data }));
}

/** Reset usage for a specific session (call when session is deleted) */
export function resetUsage(sessionId?: string): void {
	if (sessionId) {
		usageBySession.update((all) => {
			const next = { ...all };
			delete next[sessionId];
			return next;
		});
	}
	// No-op when called without sessionId — usage is now derived from
	// per-session store and auto-updates on session switch.
}

/** Clean up usage data for deleted sessions */
export function cleanupUsageForSession(sessionId: string): void {
	usageBySession.update((all) => {
		const next = { ...all };
		delete next[sessionId];
		return next;
	});
}
