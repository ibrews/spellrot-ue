/**
 * PaneManager — manages split-pane chat layout.
 *
 * Each pane displays a different session's messages. The focused pane
 * receives input from the shared composer. Uses Svelte 5 runes for
 * fine-grained reactivity.
 */

import { createUUID } from '$lib/utils.js';

const MAX_PANES = 4;

export interface ChatPane {
	paneId: string;
	sessionId: string | null;
}

function createPane(sessionId: string | null): ChatPane {
	return {
		paneId: createUUID(),
		sessionId
	};
}

class PaneManager {
	panes = $state<ChatPane[]>([]);
	focusedIndex = $state<number>(0);

	get focusedPane(): ChatPane | undefined {
		return this.panes[this.focusedIndex];
	}

	get focusedSessionId(): string | null {
		return this.focusedPane?.sessionId ?? null;
	}

	get isMultiPane(): boolean {
		return this.panes.length > 1;
	}

	get canSplit(): boolean {
		return this.panes.length < MAX_PANES;
	}

	get paneCount(): number {
		return this.panes.length;
	}

	/** Initialize with a single pane for the given session */
	init(sessionId: string | null) {
		if (this.panes.length === 0) {
			this.panes = [createPane(sessionId)];
			this.focusedIndex = 0;
		}
	}

	/** Open a session in the focused pane */
	openInFocused(sessionId: string) {
		if (this.panes.length === 0) {
			this.panes = [createPane(sessionId)];
			this.focusedIndex = 0;
			return;
		}
		this.panes[this.focusedIndex] = {
			...this.panes[this.focusedIndex],
			sessionId
		};
	}

	/** Split: add a new pane next to the focused one */
	split(sessionId: string | null = null) {
		if (!this.canSplit) return;
		const insertAt = this.focusedIndex + 1;
		this.panes.splice(insertAt, 0, createPane(sessionId));
		this.focusedIndex = insertAt;
	}

	/** Remove a pane by index */
	closePane(index: number) {
		if (this.panes.length <= 1) return; // Always keep at least one
		this.panes.splice(index, 1);
		// Adjust focus
		if (this.focusedIndex >= this.panes.length) {
			this.focusedIndex = this.panes.length - 1;
		} else if (this.focusedIndex > index) {
			this.focusedIndex--;
		}
	}

	/** Collapse to single pane (keep focused) */
	unsplit() {
		if (this.panes.length <= 1) return;
		const kept = this.panes[this.focusedIndex];
		this.panes = [kept];
		this.focusedIndex = 0;
	}

	/** Set focus to a specific pane */
	setFocus(index: number) {
		if (index >= 0 && index < this.panes.length) {
			this.focusedIndex = index;
		}
	}

	/** Update a pane's session (e.g., from sidebar click while pane is focused) */
	updatePaneSession(paneIndex: number, sessionId: string) {
		if (paneIndex >= 0 && paneIndex < this.panes.length) {
			this.panes[paneIndex] = {
				...this.panes[paneIndex],
				sessionId
			};
		}
	}

	/** Find which pane (if any) has a given session open */
	findPaneWithSession(sessionId: string): number {
		return this.panes.findIndex((p) => p.sessionId === sessionId);
	}

	/** Clean up panes referencing deleted sessions */
	cleanupDeletedSession(sessionId: string) {
		for (let i = 0; i < this.panes.length; i++) {
			if (this.panes[i].sessionId === sessionId) {
				this.panes[i] = { ...this.panes[i], sessionId: null };
			}
		}
	}
}

export const paneManager = new PaneManager();
