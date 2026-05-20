import { writable, derived, get } from 'svelte/store';
import { onCommandsAvailable, type SlashCommand } from '$lib/bridge.js';
import { currentSessionId } from '$lib/stores/sessions.js';

/** Per-session command lists (cached so they survive session switches) */
const commandsBySession = writable<Record<string, SlashCommand[]>>({});

/** Current session's available commands (derived) */
export const availableCommands = derived(
	[commandsBySession, currentSessionId],
	([$bySession, $sid]) => $sid ? ($bySession[$sid] ?? []) : []
);

let commandsBound = false;

/** Wire up commands availability callback. Call once on mount. */
export function bindCommandsListener(): void {
	if (commandsBound) return;
	commandsBound = true;

	onCommandsAvailable((sessionId, commands) => {
		// Store for ALL sessions — fixes lost commands for background sessions
		commandsBySession.update((all) => ({ ...all, [sessionId]: commands }));
	});
}

/** Clean up command data for deleted sessions */
export function cleanupCommandsForSession(sessionId: string): void {
	commandsBySession.update((all) => {
		const next = { ...all };
		delete next[sessionId];
		return next;
	});
}
