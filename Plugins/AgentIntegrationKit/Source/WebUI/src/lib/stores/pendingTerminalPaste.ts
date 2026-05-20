import { writable } from 'svelte/store';

/** One-shot line to inject into the embedded PTY (as if the user typed it + Enter). */
export type PendingTerminalLine = {
	line: string;
	newline?: string;
	/** If set, write to this UE terminal id regardless of active tab */
	targetTerminalId?: string;
	/** Open a new terminal tab first, then paste after the PTY connects */
	openInNewTab?: boolean;
};

export const pendingTerminalPaste = writable<PendingTerminalLine | null>(null);
