import { get, writable } from 'svelte/store';

export type ComposerHistoryEntry = {
	text: string;
	kind: 'submitted' | 'draft';
	timestamp: string;
};

const MAX_HISTORY_ENTRIES = 100;

export const composerHistory = writable<ComposerHistoryEntry[]>([]);

function pushHistoryEntry(text: string, kind: ComposerHistoryEntry['kind']): void {
	const normalized = text.replace(/\r\n/g, '\n');
	if (!normalized.trim()) return;

	composerHistory.update((entries) => {
		const last = entries[entries.length - 1];
		if (last?.text === normalized) {
			if (last.kind === kind) {
				return entries;
			}
			return [
				...entries.slice(0, -1),
				{
					text: normalized,
					kind,
					timestamp: new Date().toISOString()
				}
			];
		}

		const next = [
			...entries,
			{
				text: normalized,
				kind,
				timestamp: new Date().toISOString()
			}
		];

		return next.length > MAX_HISTORY_ENTRIES ? next.slice(next.length - MAX_HISTORY_ENTRIES) : next;
	});
}

export function addSubmittedPromptToHistory(text: string): void {
	pushHistoryEntry(text, 'submitted');
}

export function addDraftToHistory(text: string): void {
	pushHistoryEntry(text, 'draft');
}

export function getComposerHistoryEntries(): ComposerHistoryEntry[] {
	return get(composerHistory);
}
