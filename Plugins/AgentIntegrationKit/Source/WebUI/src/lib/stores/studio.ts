import { writable, derived, get } from 'svelte/store';
import {
	getGenerativeProviders,
	submitGenerativeJob,
	checkGenerativeJobStatus,
	getGenerativeBalance,
	type GenerativeProviderInfo,
	type GenerativeActionDescriptor,
	type GenerativeJobInfo
} from '$lib/bridge.js';

// ── Provider & Action Discovery ────────────────────────────────────

export const providers = writable<GenerativeProviderInfo[]>([]);
export const providersLoaded = writable(false);

/** Flattened list of all actions across all providers, tagged with provider info */
export type ActionWithProvider = GenerativeActionDescriptor & {
	providerId: string;
	providerName: string;
};

export const allActions = derived(providers, ($providers) => {
	const actions: ActionWithProvider[] = [];
	for (const p of $providers) {
		for (const a of p.actions) {
			actions.push({
				...a,
				providerId: p.id,
				providerName: p.displayName
			});
		}
	}
	return actions;
});

/** Get all providers that support a given action ID */
export function getProvidersForAction(actionId: string): { id: string; name: string }[] {
	const $providers = get(providers);
	return $providers
		.filter(p => p.actions.some(a => a.actionId === actionId))
		.map(p => ({ id: p.id, name: p.displayName }));
}

/** Get the action descriptor for a specific provider + action combo */
export function getActionDescriptor(providerId: string, actionId: string): GenerativeActionDescriptor | undefined {
	const $providers = get(providers);
	const provider = $providers.find(p => p.id === providerId);
	return provider?.actions.find(a => a.actionId === actionId);
}

/** Deduplicated action list — groups same actionId across providers */
export type UniqueAction = {
	actionId: string;
	description: string;
	inputHints: string[];
	outputHints: string[];
	providers: { id: string; name: string; creditCost: string }[];
};

export const uniqueActions = derived(providers, ($providers) => {
	const map = new Map<string, UniqueAction>();
	for (const p of $providers) {
		for (const a of p.actions) {
			if (!map.has(a.actionId)) {
				map.set(a.actionId, {
					actionId: a.actionId,
					description: a.description,
					inputHints: a.inputHints,
					outputHints: a.outputHints,
					providers: []
				});
			}
			map.get(a.actionId)!.providers.push({
				id: p.id,
				name: p.displayName,
				creditCost: a.creditCost
			});
		}
	}
	return Array.from(map.values());
});

/** Load all generative providers from the C++ registry */
export async function loadProviders(): Promise<void> {
	const result = await getGenerativeProviders();
	providers.set(result);
	providersLoaded.set(true);
}

// ── Job Management ─────────────────────────────────────────────────

export type StudioJob = GenerativeJobInfo & {
	prompt: string;
	createdAt: number;
	pollTimer?: ReturnType<typeof setInterval>;
};

export const jobs = writable<StudioJob[]>([]);

/** Submit a new generation job */
export async function submitJob(
	providerId: string,
	actionId: string,
	params: Record<string, unknown>,
	prompt: string
): Promise<StudioJob | null> {
	const result = await submitGenerativeJob(providerId, actionId, params);
	if (!result.success || !result.job) {
		console.error('Failed to submit job:', result.error);
		return null;
	}

	const job: StudioJob = {
		...result.job,
		prompt,
		createdAt: Date.now()
	};

	jobs.update(j => [job, ...j]);

	// Start polling if async
	if (!job.status || job.status === 'pending' || job.status === 'running') {
		startPolling(job);
	}

	return job;
}

/** Poll a job until it reaches a terminal state */
function startPolling(job: StudioJob): void {
	const timer = setInterval(async () => {
		const result = await checkGenerativeJobStatus(job.providerId, job.jobId, job.actionId);
		if (!result.success || !result.job) return;

		const updated = result.job;
		jobs.update(all => all.map(j => {
			if (j.jobId !== job.jobId) return j;
			return { ...j, ...updated };
		}));

		// Stop polling on terminal state
		if (updated.status === 'succeeded' || updated.status === 'failed' || updated.status === 'cancelled') {
			clearInterval(timer);
		}
	}, 3000); // Poll every 3 seconds

	// Store timer reference for cleanup
	jobs.update(all => all.map(j => {
		if (j.jobId !== job.jobId) return j;
		return { ...j, pollTimer: timer };
	}));
}

/** Cancel all active poll timers (cleanup on unmount) */
export function stopAllPolling(): void {
	const currentJobs = get(jobs);
	for (const job of currentJobs) {
		if (job.pollTimer) clearInterval(job.pollTimer);
	}
}

// ── Balance ────────────────────────────────────────────────────────

export const balances = writable<Record<string, number>>({});

export async function loadBalance(providerId: string): Promise<void> {
	const result = await getGenerativeBalance(providerId);
	if (result.success && result.balance >= 0) {
		balances.update(b => ({ ...b, [providerId]: result.balance }));
	}
}
