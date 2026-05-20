import { writable, derived, get } from 'svelte/store';
import { onPlanUpdate, type PlanUpdate } from '$lib/bridge.js';
import { currentSessionId } from '$lib/stores/sessions.js';

/** Per-session plan data (persisted across session switches) */
const planBySession = writable<Record<string, PlanUpdate>>({});

/** Current plan (derived from per-session store) */
export const currentPlan = derived(
	[planBySession, currentSessionId],
	([$bySession, $sid]) => $sid ? ($bySession[$sid] ?? null) : null
);

export const hasPlan = derived(currentPlan, ($plan) => $plan !== null && $plan.entries.length > 0);

export const planProgress = derived(currentPlan, ($plan) => {
	if (!$plan || $plan.totalCount === 0) return 0;
	return Math.round(($plan.completedCount / $plan.totalCount) * 100);
});

let planBound = false;

/** Wire up plan update callback. Call once on mount. */
export function bindPlanListener(): void {
	if (planBound) return;
	planBound = true;

	onPlanUpdate((sessionId, plan) => {
		// Store for ALL sessions, not just active — fixes lost plans for background sessions
		planBySession.update((all) => ({ ...all, [sessionId]: plan }));
	});
}

/** Clear plan state for a specific session (e.g., on session delete) */
export function clearPlan(sessionId?: string): void {
	if (sessionId) {
		planBySession.update((all) => {
			const next = { ...all };
			delete next[sessionId];
			return next;
		});
	}
	// No-op without sessionId — plan is now derived from per-session store
}

/** Clean up plan data for deleted sessions */
export function cleanupPlanForSession(sessionId: string): void {
	planBySession.update((all) => {
		const next = { ...all };
		delete next[sessionId];
		return next;
	});
}
