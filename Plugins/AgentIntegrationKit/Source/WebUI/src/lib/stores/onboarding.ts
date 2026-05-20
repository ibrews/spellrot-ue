import { writable, get } from 'svelte/store';
import { getOnboardingCompleted, setOnboardingCompleted } from '$lib/bridge.js';
import { agents, selectedAgent, loadAgents, type Agent } from '$lib/stores/agents.js';
import { installAgent, loadRegistry } from '$lib/stores/registry.js';
import { enterSetup } from '$lib/stores/setup.js';
import { createNewSession } from '$lib/stores/sessions.js';

// ── Types ───────────────────────────────────────────────────────────

export type OnboardingStep = 'welcome' | 'subscriptions' | 'recommendation';

export type SubscriptionId =
	| 'claude'
	| 'chatgpt'
	| 'gemini'
	| 'copilot'
	| 'cursor'
	| 'none';

export type SubscriptionOption = {
	id: SubscriptionId;
	label: string;
	sublabel: string;
	registryAgentId: string; // ACP registry ID
};

// ── Subscription → Registry Agent Mapping ──────────────────────────

export const subscriptionOptions: SubscriptionOption[] = [
	{ id: 'claude',  label: 'Claude Pro or Max',   sublabel: 'Uses your Anthropic account',    registryAgentId: 'claude-acp' },
	{ id: 'chatgpt', label: 'ChatGPT Plus or Pro', sublabel: 'Uses your OpenAI account',       registryAgentId: 'codex-acp' },
	{ id: 'gemini',  label: 'Google Account',      sublabel: 'Free — works with any Google account', registryAgentId: 'gemini' },
	{ id: 'copilot', label: 'GitHub Copilot',      sublabel: 'GitHub subscription',            registryAgentId: 'github-copilot-cli' },
	{ id: 'cursor',  label: 'Cursor IDE',          sublabel: 'Cursor subscription',            registryAgentId: 'cursor' },
	{ id: 'none',    label: 'None of these',       sublabel: "We'll recommend free options",   registryAgentId: '' },
];

// Priority: higher index = preferred when multiple subs selected
const agentPriority: string[] = [
	'github-copilot-cli',
	'cursor',
	'codex-acp',
	'gemini',
	'claude-acp',
];

// ── Stores ──────────────────────────────────────────────────────────

export const showOnboarding = writable<boolean>(false);
export const onboardingStep = writable<OnboardingStep>('welcome');
export const selectedSubscriptions = writable<Set<SubscriptionId>>(new Set());
export const recommendedAgentId = writable<string>('');
export const alternativeAgentIds = writable<string[]>([]);
export const onboardingLoading = writable<boolean>(true);

// Keep old names for backward compat with components that reference them
export const recommendedAgentName = recommendedAgentId;
export const alternativeAgentNames = alternativeAgentIds;

// ── Recommendation Engine ───────────────────────────────────────────

const subscriptionToRegistryId: Record<SubscriptionId, string> = {
	claude:  'claude-acp',
	chatgpt: 'codex-acp',
	gemini:  'gemini',
	copilot: 'github-copilot-cli',
	cursor:  'cursor',
	none:    '',
};

function computeRecommendation(): void {
	const subs = get(selectedSubscriptions);

	if (subs.size === 0 || subs.has('none')) {
		// No subscriptions: recommend Gemini (free)
		recommendedAgentId.set('gemini');
		alternativeAgentIds.set([]);
		return;
	}

	const candidateIds = [...subs]
		.filter(s => s !== 'none')
		.map(s => subscriptionToRegistryId[s])
		.filter(Boolean);

	// Sort by priority
	const sorted = [...candidateIds].sort((a, b) =>
		agentPriority.indexOf(b) - agentPriority.indexOf(a)
	);

	const primary = sorted[0];
	const alts = candidateIds.filter(id => id !== primary);

	recommendedAgentId.set(primary);
	alternativeAgentIds.set(alts);
}

// ── Navigation ──────────────────────────────────────────────────────

export async function checkOnboarding(): Promise<void> {
	onboardingLoading.set(true);
	try {
		const completed = await getOnboardingCompleted();
		showOnboarding.set(!completed);
	} catch {
		showOnboarding.set(false);
	} finally {
		onboardingLoading.set(false);
	}
}

export function goNext(): void {
	const current = get(onboardingStep);
	if (current === 'welcome') {
		onboardingStep.set('subscriptions');
	} else if (current === 'subscriptions') {
		computeRecommendation();
		onboardingStep.set('recommendation');
	}
}

export function goBack(): void {
	const current = get(onboardingStep);
	if (current === 'subscriptions') {
		onboardingStep.set('welcome');
	} else if (current === 'recommendation') {
		onboardingStep.set('subscriptions');
	}
}

export function toggleSubscription(id: SubscriptionId): void {
	selectedSubscriptions.update(subs => {
		const next = new Set(subs);
		if (id === 'none') {
			next.clear();
			next.add('none');
		} else {
			next.delete('none');
			if (next.has(id)) {
				next.delete(id);
			} else {
				next.add(id);
			}
		}
		return next;
	});
}

// ── Completion ──────────────────────────────────────────────────────

export async function completeOnboarding(agentRegistryId?: string): Promise<void> {
	await setOnboardingCompleted();

	const registryId = agentRegistryId || get(recommendedAgentId);

	// Install the recommended agent + all alternatives from subscriptions
	const allIds = [registryId, ...get(alternativeAgentIds)].filter(Boolean);
	for (const id of allIds) {
		await installAgent(id, 'auto');
	}

	// Reload agents so the sidebar picks them up
	await loadAgents();

	showOnboarding.set(false);
	onboardingStep.set('welcome');
	selectedSubscriptions.set(new Set());

	// Select the recommended agent and start a session
	const allAgents = get(agents);
	// Find by registryId or by name containing the registry ID
	const agent = allAgents.find(a => a.registryId === registryId);
	if (agent) {
		selectedAgent.set(agent);
		if (agent.status === 'available') {
			await createNewSession(agent.name);
		} else {
			enterSetup(agent);
		}
	}
}

export async function skipOnboarding(): Promise<void> {
	await setOnboardingCompleted();
	showOnboarding.set(false);
	onboardingStep.set('welcome');
	selectedSubscriptions.set(new Set());
}
