import { writable } from 'svelte/store';

export type ComposerDraft = {
	sessionId: string;
	text: string;
};

export const pendingComposerDraft = writable<ComposerDraft | null>(null);

export function queueComposerDraft(sessionId: string, text: string): void {
	if (!sessionId) return;
	const trimmed = text.trim();
	if (!trimmed) return;
	pendingComposerDraft.set({ sessionId, text: trimmed });
}

export function clearComposerDraft(): void {
	pendingComposerDraft.set(null);
}
