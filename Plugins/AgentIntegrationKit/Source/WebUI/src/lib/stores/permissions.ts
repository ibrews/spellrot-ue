import { writable, derived, get } from 'svelte/store';
import {
	onPermissionRequest,
	respondToPermission,
	type PermissionRequest
} from '$lib/bridge.js';
import { currentSessionId } from '$lib/stores/sessions.js';

/** Queue of pending permission requests (multiple can arrive from parallel tool calls) */
export const permissionQueues = writable<Record<string, PermissionRequest[]>>({});

/** Set of session IDs that have pending permission requests (for sidebar badges) */
export const sessionsNeedingAttention = derived(
	permissionQueues,
	($queues) => {
		const ids = new Set<string>();
		for (const [sid, queue] of Object.entries($queues)) {
			if (queue.length > 0) ids.add(sid);
		}
		return ids;
	}
);

/** The currently visible permission request (first in queue, null if empty) */
export const pendingPermission = derived(
	[permissionQueues, currentSessionId],
	([$queues, $activeSessionId]) => {
		if (!$activeSessionId) return null;
		const queue = $queues[$activeSessionId] ?? [];
		return queue[0] ?? null;
	}
);

/** Remove the front request from the queue (after responding) */
function dequeue(sessionId: string): void {
	permissionQueues.update((queues) => {
		const queue = queues[sessionId] ?? [];
		if (queue.length <= 1) {
			const next = { ...queues };
			delete next[sessionId];
			return next;
		}
		return {
			...queues,
			[sessionId]: queue.slice(1)
		};
	});
}

/** Respond to a standard permission request with the selected option */
export async function respondToOption(optionId: string): Promise<void> {
	const sessionId = get(currentSessionId);
	const request = get(pendingPermission);
	if (!request || !sessionId) return;

	await respondToPermission(request.agentName, request.requestId, optionId);
	dequeue(sessionId);
}

/** Submit answers to an AskUserQuestion request */
export async function respondToQuestions(answers: Record<string, string>): Promise<void> {
	const sessionId = get(currentSessionId);
	const request = get(pendingPermission);
	if (!request || !sessionId) return;

	await respondToPermission(request.agentName, request.requestId, 'submit', { answers });
	dequeue(sessionId);
}

/** Skip an AskUserQuestion request */
export async function skipQuestions(): Promise<void> {
	const sessionId = get(currentSessionId);
	const request = get(pendingPermission);
	if (!request || !sessionId) return;

	await respondToPermission(request.agentName, request.requestId, 'skip');
	dequeue(sessionId);
}

// ── Binding ──────────────────────────────────────────────────────────

let bound = false;

/** Wire up the permission request callback. Call once on mount. */
export function bindPermissionListener(): void {
	if (bound) return;
	bound = true;

	onPermissionRequest((sessionId, request) => {
		permissionQueues.update((queues) => ({
			...queues,
			[sessionId]: [...(queues[sessionId] ?? []), request]
		}));
	});
}
