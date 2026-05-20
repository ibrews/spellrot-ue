import { writable, derived, get } from 'svelte/store';
import {
	getSessions,
	createSession,
	resumeSession,
	deleteSession as bridgeDeleteSession,
	onSessionListUpdated,
	refreshSessionList as bridgeRefreshSessionList,
	type SessionInfo
} from '$lib/bridge.js';
import { agents, selectedAgent } from '$lib/stores/agents.js';

export const sessions = writable<SessionInfo[]>([]);
export const currentSessionId = writable<string | null>(null);
export const isLoadingSessions = writable(false);
/** True while agents are being connected for session listing (initial load or manual refresh) */
export const isConnectingAgents = writable(false);

// ── Date Grouping ────────────────────────────────────────────────────

export type SessionGroup = {
	label: string;
	sessions: SessionInfo[];
};

/** Group sessions by date (Today, Yesterday, This Week, This Month, Older) */
export const groupedSessions = derived(sessions, ($sessions) => {
	const groups: SessionGroup[] = [];
	const groupMap = new Map<string, SessionInfo[]>();

	const now = new Date();
	const today = new Date(now.getFullYear(), now.getMonth(), now.getDate());
	const yesterday = new Date(today.getTime() - 86400000);
	const weekAgo = new Date(today.getTime() - 7 * 86400000);
	const monthAgo = new Date(today.getTime() - 30 * 86400000);

	for (const session of $sessions) {
		const date = new Date(session.lastModifiedAt);
		let label: string;

		if (date >= today) {
			label = 'Today';
		} else if (date >= yesterday) {
			label = 'Yesterday';
		} else if (date >= weekAgo) {
			label = 'This Week';
		} else if (date >= monthAgo) {
			label = 'This Month';
		} else {
			label = 'Older';
		}

		if (!groupMap.has(label)) {
			groupMap.set(label, []);
		}
		groupMap.get(label)!.push(session);
	}

	// Maintain consistent order
	const order = ['Today', 'Yesterday', 'This Week', 'This Month', 'Older'];
	for (const label of order) {
		const items = groupMap.get(label);
		if (items && items.length > 0) {
			groups.push({ label, sessions: items });
		}
	}

	return groups;
});

// ── Session Lifecycle ────────────────────────────────────────────────

/** Load all sessions from the backend, sorted by most recent first.
 *  This pulls cached data — agents connected later will push updates. */
export async function loadSessions(): Promise<void> {
	isLoadingSessions.set(true);
	try {
		const list = await getSessions();
		list.sort((a, b) => new Date(b.lastModifiedAt).getTime() - new Date(a.lastModifiedAt).getTime());
		// Merge with existing sessions instead of replacing — push updates may
		// have already arrived from agents while this async call was in flight.
		sessions.update((current) => {
			if (current.length === 0) return list;
			const merged = [...current];
			for (const s of list)
			{
				const existingIndex = merged.findIndex((item) => item.sessionId === s.sessionId);
				if (existingIndex === -1) {
					merged.push(s);
					continue;
				}
				const existing = merged[existingIndex];
				const hasCustom = existing.hasCustomTitle || s.hasCustomTitle;
				merged[existingIndex] = {
					...existing,
					...s,
					// Preserve custom titles — never let remote sync overwrite a user rename
					title: hasCustom
						? (existing.hasCustomTitle ? existing.title : s.title)
						: (s.title || existing.title),
					hasCustomTitle: hasCustom,
					messageCount: s.messageCount ?? existing.messageCount,
					createdAt: s.createdAt ?? existing.createdAt,
					lastModifiedAt: s.lastModifiedAt ?? existing.lastModifiedAt,
					isConnected: s.isConnected ?? existing.isConnected,
					isActive: (existing.isActive ?? false) || (s.isActive ?? false)
				};
			}
			merged.sort(
				(a, b) =>
					new Date(b.lastModifiedAt || 0).getTime() -
					new Date(a.lastModifiedAt || 0).getTime()
			);
			return merged;
		});
	} catch (e) {
		console.warn('Failed to load sessions from bridge:', e);
	} finally {
		isLoadingSessions.set(false);
	}
}

/** Create a new session for the given agent and make it current */
export async function createNewSession(agentName: string): Promise<string | null> {
	try {
		const result = await createSession(agentName);
		const newSession: SessionInfo = {
			sessionId: result.sessionId,
			agentName: result.agentName,
			title: '',
			messageCount: 0,
			createdAt: new Date().toISOString(),
			lastModifiedAt: new Date().toISOString(),
			isConnected: false,
			isActive: true
		};
		// Prepend to list (most recent first)
		sessions.update(list => [newSession, ...list]);
		currentSessionId.set(result.sessionId);
		return result.sessionId;
	} catch (e) {
		console.warn('Failed to create session:', e);
		return null;
	}
}

/** Select a session — resumes from disk, connects agent, syncs selected agent.
 *  Returns the agent name on success for downstream model/mode loading. */
export async function selectSession(sessionId: string): Promise<string | null> {
	try {
		// Resume the session FIRST so messages are loaded into memory
		const result = await resumeSession(sessionId);

		if (result.success && result.agentName) {
			// Sync selectedAgent to match the session's agent
			const agentList = get(agents);
			const sessionAgent = agentList.find(a => a.name === result.agentName);
			if (sessionAgent) {
				selectedAgent.set(sessionAgent);
			}
			// Set currentSessionId after selectedAgent so downstream model/mode loads
			// target the correct agent/session pair.
			currentSessionId.set(sessionId);
			return result.agentName;
		}
		currentSessionId.set(sessionId);
	} catch (e) {
		console.warn('Failed to resume session:', e);
		// Still set the session so the UI doesn't get stuck
		currentSessionId.set(sessionId);
	}
	return null;
}

/** Delete a session, clearing current if it's the active one */
export async function removeSession(sessionId: string): Promise<boolean> {
	try {
		const result = await bridgeDeleteSession(sessionId);
		if (result.success) {
			sessions.update(list => list.filter(s => s.sessionId !== sessionId));
			if (get(currentSessionId) === sessionId) {
				currentSessionId.set(null);
			}
		}
		return result.success;
	} catch (e) {
		console.warn('Failed to delete session:', e);
		return false;
	}
}

/** Format an ISO date string to a short relative time like "2m", "6h", "3d" */
export function formatTimeAgo(isoDate?: string): string {
	if (!isoDate) return 'now';
	const now = Date.now();
	const then = new Date(isoDate).getTime();
	if (!Number.isFinite(then)) return 'now';
	const seconds = Math.floor((now - then) / 1000);

	if (seconds < 60) return 'now';
	const minutes = Math.floor(seconds / 60);
	if (minutes < 60) return `${minutes}m`;
	const hours = Math.floor(minutes / 60);
	if (hours < 24) return `${hours}h`;
	const days = Math.floor(hours / 24);
	if (days < 30) return `${days}d`;
	const months = Math.floor(days / 30);
	return `${months}mo`;
}

// ── Session List Push Updates ─────────────────────────────────────────

let sessionListBound = false;
let connectingTimeout: ReturnType<typeof setTimeout> | null = null;

/** Bind to push-based session list updates from agents. Call once on mount. */
export function bindSessionListListener(): void {
	if (sessionListBound) return;
	sessionListBound = true;

	onSessionListUpdated((agentName, remoteSessions) => {
		// Sessions arrived — clear connecting state
		isConnectingAgents.set(false);
		if (connectingTimeout) {
			clearTimeout(connectingTimeout);
			connectingTimeout = null;
		}

		sessions.update((current) => {
			// Remove old remote sessions for this agent (keep active/in-memory ones)
			const kept = current.filter(
				(s) => s.agentName !== agentName || s.isActive
			);

			// Build lookup for remote sessions
			const remoteMap = new Map(remoteSessions.map((rs) => [rs.sessionId, rs]));

			// Update local active sessions with remote data (title, messageCount, etc.)
				for (const s of kept) {
					const remote = remoteMap.get(s.sessionId);
					if (remote && s.isActive) {
						// Never let remote sync overwrite a user-renamed title
						if (remote.hasCustomTitle) {
							s.title = remote.title;
							s.hasCustomTitle = true;
						} else if (remote.title && !s.hasCustomTitle) {
							s.title = remote.title;
						}
						if (remote.messageCount !== undefined) s.messageCount = remote.messageCount;
						if (remote.lastModifiedAt) s.lastModifiedAt = remote.lastModifiedAt;
						if (remote.registryId !== undefined) s.registryId = remote.registryId;
						if (remote.terminalResumeSupported !== undefined) {
							s.terminalResumeSupported = remote.terminalResumeSupported;
						}
					}
				}

			// Merge in new remote sessions, skipping any that already exist
			const existingIds = new Set(kept.map((s) => s.sessionId));
			for (const rs of remoteSessions) {
				if (!existingIds.has(rs.sessionId)) {
					kept.push(rs);
				}
			}

			// Sort by most recent
			kept.sort(
				(a, b) =>
					new Date(b.lastModifiedAt || 0).getTime() -
					new Date(a.lastModifiedAt || 0).getTime()
			);
			return kept;
		});
	});

	// The C++ side auto-connects agents when this listener binds.
	// Show connecting state so the user sees activity.
	isConnectingAgents.set(true);

	// Safety timeout: stop showing "connecting" after 15 seconds even
	// if no sessions arrive (agents may be unavailable).
	connectingTimeout = setTimeout(() => {
		isConnectingAgents.set(false);
		connectingTimeout = null;
	}, 15000);
}

/** Manually refresh session lists from all agents */
export async function refreshSessions(): Promise<void> {
	isConnectingAgents.set(true);
	if (connectingTimeout) clearTimeout(connectingTimeout);

	try {
		await bridgeRefreshSessionList();
	} catch (e) {
		console.warn('Failed to refresh session list:', e);
	}

	// Timeout: stop showing connecting state after 15 seconds
	connectingTimeout = setTimeout(() => {
		isConnectingAgents.set(false);
		connectingTimeout = null;
	}, 15000);
}
