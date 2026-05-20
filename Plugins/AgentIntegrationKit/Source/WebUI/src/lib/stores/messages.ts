import { writable, get } from 'svelte/store';
import {
	getSessionMessages,
	sendPrompt,
	cancelPrompt,
	onMessage,
	type ChatMessage,
	type ContentBlock,
	type StreamingUpdate
} from '$lib/bridge.js';
import { currentSessionId } from '$lib/stores/sessions.js';
import { sessionStates } from '$lib/stores/agentState.js';
import { handleUsageUpdate } from '$lib/stores/usage.js';
import { setAuthRequired } from '$lib/stores/auth.js';
import { sessions } from '$lib/stores/sessions.js';
import { addSubmittedPromptToHistory } from '$lib/stores/composerHistory.js';
import { createUUID } from '$lib/utils.js';

export const messages = writable<ChatMessage[]>([]);
export const isStreaming = writable(false);
export const messagesBySession = writable<Record<string, ChatMessage[]>>({});
const streamingBySession = writable<Record<string, boolean>>({});
// Per-session load counters so concurrent loads for different sessions
// don't cancel each other. Only same-session loads should cancel prior ones.
const loadRequestIds = new Map<string, number>();
let loadRequestCounter = 0;

// ── Streaming gating ─────────────────────────────────────────────────
const promptActiveForSession = new Set<string>();

// ── Store helpers ────────────────────────────────────────────────────

function setSessionMessages(sessionId: string, msgs: ChatMessage[]): void {
	messagesBySession.update((all) => ({ ...all, [sessionId]: msgs }));
	if (get(currentSessionId) === sessionId) {
		messages.set(msgs);
	}
}

function updateSessionMessages(sessionId: string, updater: (msgs: ChatMessage[]) => ChatMessage[]): void {
	let nextMsgs: ChatMessage[] = [];
	messagesBySession.update((all) => {
		nextMsgs = updater(all[sessionId] ?? []);
		return { ...all, [sessionId]: nextMsgs };
	});
	if (get(currentSessionId) === sessionId) {
		messages.set(nextMsgs);
	}
}

function setSessionStreaming(sessionId: string, streaming: boolean): void {
	streamingBySession.update((all) => ({ ...all, [sessionId]: streaming }));
	if (get(currentSessionId) === sessionId) {
		isStreaming.set(streaming);
	}
}

// ── Auth / error helpers ─────────────────────────────────────────────

function isLikelyAuthError(update: StreamingUpdate, agentName: string): string | null {
	if (update.errorCode === -32000) {
		return 'Authentication is required to continue.';
	}
	const lowerText = (update.errorMessage ?? update.text ?? '').toLowerCase();
	if (!lowerText) return null;
	const isClaudeOrCodex = agentName === 'Claude Code' || agentName === 'Codex CLI';
	if (!isClaudeOrCodex) return null;
	if (lowerText.includes('query closed before response received')) {
		return `Your ${agentName === 'Claude Code' ? 'Claude' : 'Codex'} CLI session may be signed out. Sign in and try again.`;
	}
	if (lowerText.includes('not authenticated') || lowerText.includes('authentication required')) {
		return `Your ${agentName === 'Claude Code' ? 'Claude' : 'Codex'} CLI needs authentication.`;
	}
	if (lowerText.includes("run 'claude'") || lowerText.includes("run 'codex'")) {
		return 'Run the CLI sign-in flow and return to the editor.';
	}
	return null;
}

function formatAgentError(update: StreamingUpdate, agentName: string): string {
	const raw = (update.errorMessage ?? update.text ?? '').trim();
	const lower = raw.toLowerCase();
	const codeSuffix = typeof update.errorCode === 'number' ? ` (code ${update.errorCode})` : '';
	if (!raw || lower === 'unknown error' || lower === 'agent error') {
		return `Agent error${codeSuffix}. Please retry, and if it repeats open setup for ${agentName || 'this agent'} to verify installation/authentication.`;
	}
	if (lower.includes('failed to connect') || lower.includes('cannot send prompt')) {
		return `${raw}${codeSuffix}. The agent process failed to start or reconnect. Open setup and verify the CLI is installed and authenticated.`;
	}
	return codeSuffix ? `${raw}${codeSuffix}` : raw;
}

export function finishStreamingForSession(sessionId: string): void {
	promptActiveForSession.delete(sessionId);
	updateSessionMessages(sessionId, (msgs) => {
		const last = msgs[msgs.length - 1];
		if (last?.isStreaming) {
			const finished: ChatMessage = {
				...last,
				isStreaming: false,
				contentBlocks: last.contentBlocks.map((b) => ({ ...b, isStreaming: false }))
			};
			return [...msgs.slice(0, -1), finished];
		}
		return msgs;
	});
	setSessionStreaming(sessionId, false);
}

// Keep active-view stores in sync when current session changes.
currentSessionId.subscribe((sid) => {
	if (!sid) {
		messages.set([]);
		isStreaming.set(false);
		return;
	}
	const allMessages = get(messagesBySession);
	messages.set(allMessages[sid] ?? []);
	const allStreaming = get(streamingBySession);
	isStreaming.set(allStreaming[sid] ?? false);
});

/** Load saved messages for a session */
export async function loadMessages(sessionId: string): Promise<void> {
	const requestId = ++loadRequestCounter;
	loadRequestIds.set(sessionId, requestId);
	try {
		const msgs = await getSessionMessages(sessionId);
		if (loadRequestIds.get(sessionId) !== requestId) return;
		for (const msg of msgs) {
			if (msg.isStreaming) {
				msg.isStreaming = false;
				for (const b of msg.contentBlocks) {
					if (b.isStreaming) b.isStreaming = false;
				}
			}
		}
		setSessionMessages(sessionId, msgs);
		setSessionStreaming(sessionId, promptActiveForSession.has(sessionId));
	} catch (e) {
		if (loadRequestIds.get(sessionId) !== requestId) return;
		console.warn('Failed to load messages:', e);
		setSessionMessages(sessionId, []);
		setSessionStreaming(sessionId, false);
	}
}

/** Send a user message and trigger agent prompt */
export async function sendMessage(sessionId: string, text: string): Promise<boolean> {
	if (!sessionId || !text.trim()) return false;
	const userMsg: ChatMessage = {
		messageId: createUUID(),
		role: 'user',
		isStreaming: false,
		timestamp: new Date().toISOString(),
		contentBlocks: [{ type: 'text', text: text.trim(), isStreaming: false }]
	};
	updateSessionMessages(sessionId, (msgs) => [...msgs, userMsg]);
	try {
		await sendPrompt(sessionId, text.trim());
		addSubmittedPromptToHistory(text.trim());
		promptActiveForSession.add(sessionId);
		setSessionStreaming(sessionId, true);
		return true;
	} catch (e) {
		console.warn('Failed to send prompt:', e);
		setSessionStreaming(sessionId, false);
		updateSessionMessages(sessionId, (msgs) =>
			msgs.filter((m) => m.messageId !== userMsg.messageId)
		);
		return false;
	}
}

/** Cancel the current streaming prompt */
export async function cancelCurrentPrompt(sessionId: string): Promise<void> {
	if (!sessionId) return;
	try {
		await cancelPrompt(sessionId);
	} catch (e) {
		console.warn('Failed to cancel prompt:', e);
	}
	finishStreamingForSession(sessionId);
}

/** Mark the current streaming message as complete */
export function finishStreaming(): void {
	const sid = get(currentSessionId);
	if (!sid) {
		isStreaming.set(false);
		return;
	}
	finishStreamingForSession(sid);
}

// ── Streaming Update Mutation ────────────────────────────────────────

function getOrCreateAssistantMessage(msgs: ChatMessage[]): ChatMessage {
	const last = msgs[msgs.length - 1];
	if (last?.role === 'assistant') {
		last.isStreaming = true;
		return last;
	}
	const newMsg: ChatMessage = {
		messageId: createUUID(),
		role: 'assistant',
		isStreaming: true,
		timestamp: new Date().toISOString(),
		contentBlocks: []
	};
	msgs.push(newMsg);
	return newMsg;
}

function appendToBlock(msg: ChatMessage, blockType: 'text' | 'thought', chunk: string): void {
	const blocks = msg.contentBlocks;
	const lastBlock = blocks[blocks.length - 1];
	if (lastBlock?.type === blockType) {
		lastBlock.text += chunk;
		lastBlock.isStreaming = true;
	} else {
		blocks.push({ type: blockType, text: chunk, isStreaming: true });
	}
}

function stopStreamingOnMessage(msg: ChatMessage): void {
	msg.isStreaming = false;
	for (const b of msg.contentBlocks) {
		if (b.isStreaming) b.isStreaming = false;
	}
}

/**
 * Apply a single streaming update to a working copy of the message array.
 * Mutates `msgs` in place. Returns the modified assistant message.
 */
function applyStreamingUpdate(
	msgs: ChatMessage[],
	sessionId: string,
	update: StreamingUpdate,
	isActivePrompt: boolean
): { msg: ChatMessage; shouldStream: boolean } {
	const msg = getOrCreateAssistantMessage(msgs);
	let shouldStream = isActivePrompt;

	switch (update.type) {
		case 'text_chunk':
			if (update.systemStatus) {
				const existingSystem = msg.contentBlocks.find(
					(b) => b.type === 'system' && b.systemStatus === 'compacting'
				);
				if (update.systemStatus === 'compacted' && existingSystem) {
					existingSystem.text = update.text;
					existingSystem.systemStatus = 'compacted';
					existingSystem.isStreaming = false;
				} else {
					msg.contentBlocks.push({
						type: 'system', text: update.text,
						isStreaming: update.systemStatus === 'compacting',
						systemStatus: update.systemStatus
					});
				}
			} else {
				appendToBlock(msg, 'text', update.text);
			}
			break;

		case 'thought_chunk':
			appendToBlock(msg, 'thought', update.text);
			break;

		case 'tool_call': {
			const tcId = update.toolCallId || `gen_${createUUID()}`;
			const existing = msg.contentBlocks.find(
				(b) => b.type === 'tool_call' && b.toolCallId === tcId
			);
			if (existing) {
				if (update.toolArguments) existing.toolArguments = update.toolArguments;
				if (update.toolName && !existing.toolName) existing.toolName = update.toolName;
				if (update.parentToolCallId && !existing.parentToolCallId) existing.parentToolCallId = update.parentToolCallId;
			} else {
				for (const b of msg.contentBlocks) {
					if ((b.type === 'text' || b.type === 'thought') && b.isStreaming) b.isStreaming = false;
				}
				msg.contentBlocks.push({
					type: 'tool_call', text: '', isStreaming: isActivePrompt,
					toolCallId: tcId, toolName: update.toolName,
					toolArguments: update.toolArguments, parentToolCallId: update.parentToolCallId
				});
			}
			break;
		}

		case 'tool_result': {
			const resultTcId = update.toolCallId;
			let toolCall = msg.contentBlocks.find(
				(b) => b.type === 'tool_call' && b.toolCallId === resultTcId
			);
			if (!toolCall && resultTcId) {
				toolCall = {
					type: 'tool_call', text: '', isStreaming: false,
					toolCallId: resultTcId, toolName: update.toolName || 'tool',
					toolArguments: '', parentToolCallId: update.parentToolCallId
				};
				msg.contentBlocks.push(toolCall);
			}
			if (toolCall) toolCall.isStreaming = false;
			const existingResult = msg.contentBlocks.find(
				(b) => b.type === 'tool_result' && b.toolCallId === resultTcId
			);
			if (existingResult) {
				existingResult.toolResult = update.toolResult;
				existingResult.toolSuccess = update.toolSuccess;
				if (update.images) existingResult.images = update.images;
			} else {
				msg.contentBlocks.push({
					type: 'tool_result', text: '', isStreaming: false,
					toolCallId: resultTcId, toolResult: update.toolResult,
					toolSuccess: update.toolSuccess, images: update.images,
					parentToolCallId: update.parentToolCallId
				});
			}
			break;
		}

		case 'error': {
			const sessionAgent = update.agentName
				|| get(sessions).find(s => s.sessionId === sessionId)?.agentName || '';
			const authReason = isLikelyAuthError(update, sessionAgent);
			if (authReason) {
				setAuthRequired(sessionAgent, authReason);
				stopStreamingOnMessage(msg);
				shouldStream = false;
				break;
			}
			msg.contentBlocks.push({
				type: 'error', text: formatAgentError(update, sessionAgent), isStreaming: false
			});
			stopStreamingOnMessage(msg);
			shouldStream = false;
			break;
		}

		default:
			break;
	}

	if (!isActivePrompt) stopStreamingOnMessage(msg);
	return { msg, shouldStream };
}

/** Clone a message: new message ref + new contentBlocks array. Reuses block objects. */
function cloneMessage(msg: ChatMessage): ChatMessage {
	return { ...msg, contentBlocks: msg.contentBlocks.map((b) => ({ ...b })) };
}

// ── RAF Batching ─────────────────────────────────────────────────────
// Accumulate streaming updates and flush once per animation frame.
// All updates for the same session are applied in a single mutation pass
// with ONE store write at the end — not one per update.

let pendingUpdates: Array<{ sessionId: string; update: StreamingUpdate }> = [];
let rafId: number | null = null;

function queueStreamingUpdate(sessionId: string, update: StreamingUpdate): void {
	// Usage updates bypass RAF (low frequency, no DOM impact)
	if (update.type === 'usage') {
		handleUsageUpdate(update, sessionId);
		return;
	}
	// Errors and user_message_chunk are important — process immediately
	if (update.type === 'error' || update.type === 'user_message_chunk') {
		handleImmediateUpdate(sessionId, update);
		return;
	}
	pendingUpdates.push({ sessionId, update });
	if (rafId === null) {
		rafId = requestAnimationFrame(flushPendingUpdates);
	}
}

function handleImmediateUpdate(sessionId: string, update: StreamingUpdate): void {
	if (update.type === 'user_message_chunk') {
		finishStreamingForSession(sessionId);
		const userMsg: ChatMessage = {
			messageId: createUUID(),
			role: 'user',
			isStreaming: false,
			timestamp: new Date().toISOString(),
			contentBlocks: [{ type: 'text', text: update.text, isStreaming: false }]
		};
		updateSessionMessages(sessionId, (msgs) => [...msgs, userMsg]);
		return;
	}

	// Error: apply via standard updateSessionMessages so store properly spreads
	const isActivePrompt = promptActiveForSession.has(sessionId);
	updateSessionMessages(sessionId, (msgs) => {
		const working = [...msgs];
		const { msg } = applyStreamingUpdate(working, sessionId, update, isActivePrompt);
		const idx = working.indexOf(msg);
		if (idx >= 0) working[idx] = cloneMessage(msg);
		return working;
	});
	setSessionStreaming(sessionId, false); // errors always stop streaming
}

function flushPendingUpdates(): void {
	rafId = null;
	const batch = pendingUpdates;
	pendingUpdates = [];

	// Group by sessionId to do one store write per session
	const bySession = new Map<string, StreamingUpdate[]>();
	for (const { sessionId, update } of batch) {
		let arr = bySession.get(sessionId);
		if (!arr) { arr = []; bySession.set(sessionId, arr); }
		arr.push(update);
	}

	for (const [sessionId, updates] of bySession) {
		const isActivePrompt = promptActiveForSession.has(sessionId);

		// ONE store write per session per frame
		updateSessionMessages(sessionId, (msgs) => {
			const working = [...msgs];
			let modifiedMsg: ChatMessage | null = null;
			let shouldStream = isActivePrompt;

			for (const update of updates) {
				const result = applyStreamingUpdate(working, sessionId, update, isActivePrompt);
				modifiedMsg = result.msg;
				shouldStream = result.shouldStream;
			}

			// Clone the modified message so Svelte's keyed {#each} detects the change
			if (modifiedMsg) {
				const idx = working.indexOf(modifiedMsg);
				if (idx >= 0) working[idx] = cloneMessage(modifiedMsg);
			}

			return working;
		});

		// Set streaming state once per session per frame
		setSessionStreaming(sessionId, promptActiveForSession.has(sessionId));
	}
}

// ── Binding ──────────────────────────────────────────────────────────

let messageBound = false;
let stateUnsubscribe: (() => void) | null = null;

/** Wire up streaming callbacks. Call once on mount. */
export function bindMessageListener(): void {
	if (messageBound) return;
	messageBound = true;

	onMessage(queueStreamingUpdate);

	const prevStates: Record<string, string> = {};
	stateUnsubscribe = sessionStates.subscribe((states) => {
		for (const [sessionId, sessionState] of Object.entries(states)) {
			const prev = prevStates[sessionId];
			const cur = sessionState.state;
			if (prev === 'prompting' && cur !== 'prompting') {
				finishStreamingForSession(sessionId);
			}
			if ((cur === 'ready' || cur === 'in_session') && promptActiveForSession.has(sessionId)) {
				if (prev && prev !== 'prompting' && prev !== 'connecting' && prev !== 'initializing') {
					finishStreamingForSession(sessionId);
				}
			}
			prevStates[sessionId] = cur;
		}
	});
}
