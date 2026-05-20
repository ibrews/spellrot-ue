import { writable, derived } from 'svelte/store';
import { getSourceControlStatus, type SourceControlStatus } from '$lib/bridge.js';

const emptyStatus: SourceControlStatus = {
	enabled: false,
	provider: '',
	branch: '',
	changesCount: -1,
	connected: false
};

/** Current source control status */
export const scStatus = writable<SourceControlStatus>({ ...emptyStatus });

/** Whether source control is enabled */
export const scEnabled = derived(scStatus, (s) => s.enabled);

/** Current branch name (empty if no VCS) */
export const branchName = derived(scStatus, (s) => s.branch || '');

/** Whether there are known pending changes (changesCount > 0) */
export const hasChanges = derived(scStatus, (s) => s.changesCount > 0);

/** Display label for change count, e.g. "+3" — empty if unknown or zero */
export const changesLabel = derived(scStatus, (s) =>
	s.changesCount > 0 ? `+${s.changesCount}` : ''
);

let pollInterval: ReturnType<typeof setInterval> | null = null;

/** Fetch source control status from C++ bridge */
async function fetchStatus(): Promise<void> {
	const status = await getSourceControlStatus();
	scStatus.set(status);
}

/** Load source control status once and start polling every 30s */
export function loadSourceControlStatus(): void {
	fetchStatus();
	if (!pollInterval) {
		pollInterval = setInterval(fetchStatus, 30_000);
	}
}

/** Stop polling (call on unmount if needed) */
export function stopSourceControlPolling(): void {
	if (pollInterval) {
		clearInterval(pollInterval);
		pollInterval = null;
	}
}
