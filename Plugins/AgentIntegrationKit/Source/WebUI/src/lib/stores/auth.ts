import { writable, get } from 'svelte/store';
import { getAuthMethods, startAgentLogin, onLoginComplete } from '$lib/bridge.js';

export type AuthState = 'none' | 'required' | 'logging_in' | 'success' | 'error';

/** Whether the current agent requires authentication */
export const authState = writable<AuthState>('none');

/** Which agent needs authentication */
export const authAgentName = writable<string>('');

/** Error message from failed login */
export const authError = writable<string>('');

/** Called when we detect error code -32000 from the adapter */
export function setAuthRequired(agentName: string, reason = ''): void {
	authAgentName.set(agentName);
	authError.set(reason);
	authState.set('required');
}

/** Start the agent login flow — fetches auth methods and picks the first one */
export async function startLogin(): Promise<void> {
	const agentName = get(authAgentName);
	if (!agentName) return;

	authState.set('logging_in');
	authError.set('');

	try {
		const methods = await getAuthMethods(agentName);
		if (methods.length === 0) {
			authState.set('error');
			authError.set('No authentication methods available for this agent.');
			return;
		}
		// Pick the first available method — the backend handles terminal-auth vs ACP
		await startAgentLogin(agentName, methods[0].id);
	} catch (e) {
		authState.set('error');
		authError.set(e instanceof Error ? e.message : 'Failed to start login');
	}
}

/** Bind the login completion callback — call once on mount */
export function bindLoginListener(): void {
	onLoginComplete((agentName, success, errorMessage) => {
		// Only process if this is for the agent we're currently authenticating
		const currentAgent = get(authAgentName);
		if (currentAgent && agentName !== currentAgent) return;

		if (success) {
			authState.set('success');
			authError.set('');
			// Auto-dismiss after a moment so the UI returns to normal
			setTimeout(() => {
				authState.set('none');
				authAgentName.set('');
			}, 2500);
		} else {
			authState.set('error');
			authError.set(errorMessage || 'Login failed');
		}
	});
}

/** Reset auth state (call when switching sessions/agents) */
export function resetAuth(): void {
	authState.set('none');
	authAgentName.set('');
	authError.set('');
}
