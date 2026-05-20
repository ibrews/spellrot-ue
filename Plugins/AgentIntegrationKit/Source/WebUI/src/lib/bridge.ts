/**
 * TypeScript wrapper for the UE ↔ JS bridge (window.ue.bridge).
 * All calls are async (UE returns Promises from bound UFUNCTIONs).
 *
 * Transport modes:
 * - 'embedded': Running inside UE's CEF browser, uses window.ue.bridge
 * - 'remote': Running on website, uses WebSocket relay to connected instance
 * - 'standalone': No backend available (dev mode), returns mock data
 */
import { createUUID } from '$lib/utils.js';
import { relayCall, onRelayEvent, getRelayState } from './relay.js';

export type Transport = 'embedded' | 'remote' | 'standalone';

let currentTransport: Transport = 'standalone';

export function getTransport(): Transport {
	return currentTransport;
}

export function setTransport(transport: Transport): void {
	currentTransport = transport;
}

/**
 * Detect and set the transport mode.
 * Called during app initialization.
 */
export function detectTransport(): Transport {
	if (getBridge()) {
		currentTransport = 'embedded';
	} else if (getRelayState() === 'connected') {
		currentTransport = 'remote';
	} else {
		currentTransport = 'standalone';
	}
	return currentTransport;
}

export type AgentStatus = 'available' | 'not_installed' | 'missing_key' | 'unknown';

export type AgentInfo = {
	id: string;
	name: string;
	status: AgentStatus;
	statusMessage: string;
	isBuiltIn: boolean;
	isConnected: boolean;
	registryId?: string;
	iconUrl?: string;
};

export type SessionInfo = {
	sessionId: string;
	agentName: string;
	/** ACP registry agent id when configured (e.g. claude-acp); empty for bundled-only rows */
	registryId?: string;
	/** Server-computed: embedded terminal can generate a CLI resume line for this agent */
	terminalResumeSupported?: boolean;
	title: string;
	messageCount?: number;
	createdAt?: string;
	lastModifiedAt: string;
	isConnected: boolean;
	isActive?: boolean;
	/** True when the user has explicitly renamed this session — title survives remote sync */
	hasCustomTitle?: boolean;
};

export type ContinuationDraftResult = {
	success: boolean;
	sourceSessionId?: string;
	targetAgentName?: string;
	summaryMode?: 'compact' | 'detailed';
	draftPrompt?: string;
	providerUsed?: 'openrouter' | 'local';
	error?: string;
};

export type ContinuationSummarySettings = {
	provider: 'openrouter' | 'local';
	modelId: string;
	defaultDetail: 'compact' | 'detailed';
	hasOpenRouterKey: boolean;
};

export type ExportSessionResult = {
	success: boolean;
	canceled?: boolean;
	savedPath?: string;
	error?: string;
};

export type ToolResultImage = {
	base64: string;
	mimeType: string;
	width: number;
	height: number;
};

export type ContentBlock = {
	type: 'text' | 'thought' | 'tool_call' | 'tool_result' | 'image' | 'error' | 'system';
	text: string;
	isStreaming: boolean;
	toolCallId?: string;
	toolName?: string;
	toolArguments?: string;
	toolResult?: string;
	toolSuccess?: boolean;
	imageCount?: number;
	images?: ToolResultImage[];
	/** If this tool call was made inside a subagent (Task), the parent Task's toolCallId */
	parentToolCallId?: string;
	/** For system status blocks (e.g. "compacting", "compacted") */
	systemStatus?: string;
};

export type ChatMessage = {
	messageId: string;
	role: 'user' | 'assistant' | 'system';
	isStreaming: boolean;
	timestamp: string;
	contentBlocks: ContentBlock[];
};

export type ModelUsageEntry = {
	modelName: string;
	inputTokens: number;
	outputTokens: number;
	cacheReadTokens: number;
	cacheCreationTokens: number;
	costUSD: number;
	contextWindow: number;
	maxOutputTokens: number;
};

export type StreamingUpdate = {
	agentName: string;
	type: 'text_chunk' | 'thought_chunk' | 'tool_call' | 'tool_result' | 'error' | 'usage' | 'plan' | 'user_message_chunk' | 'unknown';
	text: string;
	systemStatus?: string;
	toolCallId?: string;
	toolName?: string;
	toolArguments?: string;
	toolResult?: string;
	toolSuccess?: boolean;
	images?: ToolResultImage[];
	/** If this tool call was made inside a subagent (Task), the parent Task's toolCallId */
	parentToolCallId?: string;
	errorMessage?: string;
	errorCode?: number;
	// Usage fields (present when type === 'usage')
	inputTokens?: number;
	outputTokens?: number;
	totalTokens?: number;
	cacheReadTokens?: number;
	cacheCreationTokens?: number;
	reasoningTokens?: number;
	costAmount?: number;
	costCurrency?: string;
	turnCostUSD?: number;
	contextUsed?: number;
	contextSize?: number;
	numTurns?: number;
	durationMs?: number;
	modelUsage?: ModelUsageEntry[];
};

// Check if we're running inside UE's embedded browser
function getBridge(): any | null {
	if (typeof window !== 'undefined' && (window as any).ue?.bridge) {
		return (window as any).ue.bridge;
	}
	return null;
}

/**
 * Wait for the UE bridge to become available.
 * The CEF browser starts loading the page before BindUObject() completes,
 * so window.ue.bridge may not be available when onMount fires.
 * This polls until the bridge appears or the timeout expires.
 */
export async function waitForBridge(maxWaitMs = 5000): Promise<boolean> {
	// Already available — no wait needed
	if (getBridge()) return true;
	// Not in a browser environment (SSR)
	if (typeof window === 'undefined') return false;

	const start = Date.now();
	while (Date.now() - start < maxWaitMs) {
		if ((window as any).ue?.bridge) return true;
		await new Promise((r) => setTimeout(r, 50));
	}
	console.warn('Bridge not available after', maxWaitMs, 'ms — running in standalone mode');
	return false;
}

/** Safely parse a bridge result - UE wraps returns in { ReturnValue: "json string" } */
function parseResult<T>(value: unknown): T {
	// UE bridge wraps UFUNCTION returns in { ReturnValue: ... }
	const raw = (value && typeof value === 'object' && 'ReturnValue' in (value as any))
		? (value as any).ReturnValue
		: value;
	if (typeof raw === 'string') {
		return JSON.parse(raw);
	}
	return raw as T;
}

type ContinuationStartResult = {
	success: boolean;
	pending?: boolean;
	requestId?: number;
	error?: string;
};

let continuationDraftBound = false;
const continuationDraftResolvers = new Map<number, (result: ContinuationDraftResult) => void>();

function ensureContinuationDraftBinding(): void {
	const bridge = getBridge();
	if (!bridge || continuationDraftBound) return;

	bridge.bindoncontinuationdraftready((requestId: number, resultJson: string) => {
		const resolver = continuationDraftResolvers.get(requestId);
		if (!resolver) return;
		continuationDraftResolvers.delete(requestId);
		try {
			resolver(JSON.parse(resultJson) as ContinuationDraftResult);
		} catch {
			resolver({ success: false, error: 'Failed to parse continuation draft result' });
		}
	});

	continuationDraftBound = true;
}

export function isInUnreal(): boolean {
	return getBridge() !== null || currentTransport === 'remote';
}

/** Get the last used agent name (persisted across editor sessions) */
export async function getLastUsedAgent(): Promise<string> {
	if (currentTransport === 'remote') return relayCall<string>('getLastUsedAgent');
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getlastusedagent();
		const raw = (result && typeof result === 'object' && 'ReturnValue' in result)
			? result.ReturnValue
			: result;
		return (raw as string) || '';
	}
	return '';
}

// ── Onboarding ──────────────────────────────────────────────────────

/** Check if the onboarding wizard has been completed or skipped */
export async function getOnboardingCompleted(): Promise<boolean> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getonboardingcompleted();
		const raw = (result && typeof result === 'object' && 'ReturnValue' in result)
			? result.ReturnValue
			: result;
		return !!raw;
	}
	return true; // In dev mode (no UE), skip wizard
}

/** Mark onboarding as completed. Persists across editor sessions. */
export async function setOnboardingCompleted(): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setonboardingcompleted();
	}
}

// ── Chat Handoff Summary Settings ─────────────────────────────────

export async function getContinuationSummarySettings(): Promise<ContinuationSummarySettings> {
	const bridge = getBridge();
	if (bridge) {
		const timeoutPromise = new Promise<never>((_, reject) => {
			setTimeout(() => reject(new Error('getContinuationSummarySettings timed out')), 8000);
		});
		const result = await Promise.race([bridge.getcontinuationsummarysettings(), timeoutPromise]);
		return parseResult(result as unknown);
	}
	return {
		provider: 'openrouter',
		modelId: 'x-ai/grok-4.1-fast',
		defaultDetail: 'compact',
		hasOpenRouterKey: false
	};
}

export async function setContinuationSummaryProvider(provider: 'openrouter' | 'local'): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setcontinuationsummaryprovider(provider);
	}
}

export async function setContinuationSummaryModel(modelId: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setcontinuationsummarymodel(modelId);
	}
}

export async function setContinuationSummaryDefaultDetail(detail: 'compact' | 'detailed'): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setcontinuationsummarydefaultdetail(detail);
	}
}

// ── Provider Settings ───────────────────────────────────────────────

export type CustomProviderModel = {
	id: string;
	name: string;
	description: string;
};

export type ProviderConfig = {
	id: string;
	name: string;
	description: string;
	requiresApiKey: boolean;
	hasApiKey: boolean;
	apiKeyMasked: string;
	baseUrl: string;
	defaultBaseUrl: string;
	defaultModel: string;
	supportsModelDiscovery: boolean;
	configured: boolean;
	inPriorityList: boolean;
	isUserDefined: boolean;
	enableModelDiscovery: boolean;
	models?: CustomProviderModel[];
};

export type ProviderSettings = {
	priority: string[];
	providers: ProviderConfig[];
};

export async function getProviderSettings(): Promise<ProviderSettings> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getprovidersettings();
		return parseResult(result);
	}
	return { priority: [], providers: [] };
}

export async function setProviderPriority(priority: string[]): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setproviderpriority(JSON.stringify(priority));
	}
}

export async function addProvider(providerId: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.addprovider(providerId);
	}
}

export async function removeProvider(providerId: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.removeprovider(providerId);
	}
}

export async function setProviderApiKey(providerId: string, apiKey: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setproviderapikey(providerId, apiKey);
	}
}

export async function setProviderBaseUrl(providerId: string, baseUrl: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setproviderbaseurl(providerId, baseUrl);
	}
}

export async function refreshProviderModels(): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.refreshprovidermodels();
	}
}

// ── Custom Providers ────────────────────────────────────────────────

export async function createCustomProvider(displayName: string, baseUrl: string): Promise<{ providerId: string }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.createcustomprovider(displayName, baseUrl);
		return parseResult(result);
	}
	return { providerId: '' };
}

export async function deleteCustomProvider(providerId: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.deletecustomprovider(providerId);
	}
}

export async function updateCustomProvider(providerId: string, displayName: string, baseUrl: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.updatecustomprovider(providerId, displayName, baseUrl);
	}
}

export async function addCustomProviderModel(providerId: string, modelId: string, displayName: string, description: string): Promise<{ success: boolean }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.addcustomprovidermodel(providerId, modelId, displayName, description);
		return parseResult(result);
	}
	return { success: false };
}

export async function removeCustomProviderModel(providerId: string, modelId: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.removecustomprovidermodel(providerId, modelId);
	}
}

export async function importCustomProviderModels(providerId: string, modelsJson: string): Promise<{ imported: number; errors: string[] }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.importcustomprovidermodels(providerId, modelsJson);
		return parseResult(result);
	}
	return { imported: 0, errors: ['Bridge not available'] };
}

export async function setCustomProviderModelDiscovery(providerId: string, enabled: boolean): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setcustomprovidermodeldiscovery(providerId, enabled);
	}
}

export type EnabledModelsState = {
	enabledModels: string[];
	hasCustomSelection: boolean;
};

export async function getEnabledModels(): Promise<EnabledModelsState> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getenabledmodels();
		return parseResult(result);
	}
	return { enabledModels: [], hasCustomSelection: false };
}

export async function setModelEnabled(modelId: string, enabled: boolean): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setmodelenabled(modelId, enabled);
	}
}

export async function setEnabledModels(modelIds: string[]): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setenabledmodels(JSON.stringify(modelIds));
	}
}

// ── Notification Settings ──────────────────────────────────────

export type NotificationSettings = {
	onlyWhenUnfocused: boolean;
	notifyOnComplete: boolean;
	flashTaskbar: boolean;
	playSound: boolean;
	soundVolume: number;
	completionSound: string;
	errorSound: string;
	playPermissionSound: boolean;
	permissionSoundVolume: number;
	permissionRequestSound: string;
};

const defaultNotificationSettings: NotificationSettings = {
	onlyWhenUnfocused: false,
	notifyOnComplete: true,
	flashTaskbar: true,
	playSound: true,
	soundVolume: 1.0,
	completionSound: '',
	errorSound: '',
	playPermissionSound: false,
	permissionSoundVolume: 1.0,
	permissionRequestSound: ''
};

export async function getNotificationSettings(): Promise<NotificationSettings> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getnotificationsettings();
		const parsed = parseResult<Partial<NotificationSettings>>(result);
		return { ...defaultNotificationSettings, ...parsed };
	}
	return { ...defaultNotificationSettings };
}

export async function setNotificationSetting(key: string, value: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setnotificationsetting(key, value);
	}
}

// ── Agent Discovery ─────────────────────────────────────────────────

/** Get list of available agents from the backend */
export async function getAgents(): Promise<AgentInfo[]> {
	if (currentTransport === 'remote') return relayCall<AgentInfo[]>('getAgents');
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getagents();
		return parseResult<AgentInfo[]>(result);
	}
	return [];
}

/** Create a new chat session */
export async function createSession(agentName: string): Promise<{ sessionId: string; agentName: string }> {
	if (currentTransport === 'remote') return relayCall('createSession', agentName);
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.createsession(agentName);
		return parseResult(result);
	}
	return { sessionId: createUUID(), agentName };
}

/** Get all sessions (saved + active) */
export async function getSessions(): Promise<SessionInfo[]> {
	if (currentTransport === 'remote') return relayCall<SessionInfo[]>('getSessions');
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getsessions();
		return parseResult(result);
	}
	return [];
}

/** Resume a saved session — loads from disk, connects agent, resumes external session */
export async function resumeSession(sessionId: string): Promise<{ success: boolean; agentName?: string; error?: string }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.resumesession(sessionId);
		return parseResult(result);
	}
	return { success: false, error: 'Not in UE' };
}

export type SessionTerminalResumeResult = {
	supported: boolean;
	command?: string;
	agentName?: string;
	registryId?: string;
	error?: string;
};

/** Shell command to resume this chat in the embedded terminal (Claude Code, Gemini, Copilot, Codex). */
export async function getSessionTerminalResumeCommand(
	sessionId: string
): Promise<SessionTerminalResumeResult> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getsessionterminalresumecommand(sessionId);
		return parseResult(result);
	}
	return { supported: false, error: 'Not in UE' };
}

/** Get messages for a session */
export async function getSessionMessages(sessionId: string): Promise<ChatMessage[]> {
	if (currentTransport === 'remote') return relayCall<ChatMessage[]>('getSessionMessages', sessionId);
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getsessionmessages(sessionId);
		return parseResult(result);
	}
	return [];
}

/** Build a continuation draft from a source chat for a target agent */
export async function buildContinuationDraft(
	sourceSessionId: string,
	targetAgentName: string,
	summaryMode: 'compact' | 'detailed' = 'compact'
): Promise<ContinuationDraftResult> {
	const bridge = getBridge();
	if (bridge) {
		ensureContinuationDraftBinding();
		const startRaw = await bridge.requestcontinuationdraft(sourceSessionId, targetAgentName, summaryMode);
		const start = parseResult<ContinuationStartResult>(startRaw);
		if (!start.success || !start.requestId) {
			return { success: false, error: start.error || 'Failed to start continuation draft generation' };
		}
		const requestId = start.requestId;

		return await new Promise<ContinuationDraftResult>((resolve) => {
			continuationDraftResolvers.set(requestId, resolve);
			setTimeout(() => {
				if (!continuationDraftResolvers.has(requestId)) return;
				continuationDraftResolvers.delete(requestId);
				resolve({ success: false, error: 'Continuation draft generation timed out' });
			}, 180000);
		});
	}
	return { success: false, error: 'Not in UE' };
}

/** Rename a session (sets custom title that survives remote sync) */
export async function renameSession(sessionId: string, newTitle: string): Promise<{ success: boolean }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.renamesession(sessionId, newTitle);
		return parseResult(result);
	}
	return { success: false };
}

/** Delete a session */
export async function deleteSession(sessionId: string): Promise<{ success: boolean }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.deletesession(sessionId);
		return parseResult(result);
	}
	return { success: false };
}

/** Export a loaded session to a Markdown file via native save dialog */
export async function exportSessionToMarkdown(sessionId: string): Promise<ExportSessionResult> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.exportsessiontomarkdown(sessionId);
		return parseResult(result);
	}
	return { success: false, error: 'Not in UE' };
}

/** Send a prompt to a session */
export async function sendPrompt(sessionId: string, text: string): Promise<void> {
	if (currentTransport === 'remote') {
		await relayCall('sendPrompt', sessionId, text);
		return;
	}
	const bridge = getBridge();
	if (!bridge) {
		throw new Error('UE bridge unavailable');
	}
	await bridge.sendprompt(sessionId, text);
}

/** Cancel current prompt in a session */
export async function cancelPrompt(sessionId: string): Promise<void> {
	if (currentTransport === 'remote') {
		await relayCall('cancelPrompt', sessionId);
		return;
	}
	const bridge = getBridge();
	if (bridge) {
		await bridge.cancelprompt(sessionId);
	}
}

// ── Agent Setup ─────────────────────────────────────────────────────

export type AgentInstallInfo = {
	agentName: string;
	baseExecutableName: string;
	installCommand: string;
	installUrl: string;
	requiresAdapter: boolean;
	requiresBaseCLI: boolean;
};

/** Get install info for an agent (install command, download URL, requirements) */
export async function getAgentInstallInfo(agentName: string): Promise<AgentInstallInfo> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getagentinstallinfo(agentName);
		return parseResult(result);
	}
	return { agentName, baseExecutableName: '', installCommand: '', installUrl: '', requiresAdapter: false, requiresBaseCLI: false };
}

/** Start async agent installation. Listen for progress via onInstallProgress/onInstallComplete. */
export async function installAgent(agentName: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.installagent(agentName);
	}
}

/** Register callback for install progress updates */
export function onInstallProgress(callback: (agentName: string, message: string) => void): void {
	const bridge = getBridge();
	if (bridge) {
		bridge.bindoninstallprogress(callback);
	}
}

/** Register callback for install completion */
export function onInstallComplete(callback: (agentName: string, success: boolean, errorMessage: string) => void): void {
	const bridge = getBridge();
	if (bridge) {
		bridge.bindoninstallcomplete(callback);
	}
}

/** Refresh an agent's status (invalidates cache, re-checks). Returns updated status. */
export async function refreshAgentStatus(agentName: string): Promise<{ status: AgentStatus; statusMessage: string }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.refreshagentstatus(agentName);
		return parseResult(result);
	}
	return { status: 'unknown', statusMessage: '' };
}

// ── ACP Registry ────────────────────────────────────────────────────

export type RegistryAgent = {
	id: string;
	name: string;
	version: string;
	description: string;
	license: string;
	icon: string; // SVG markup (pre-fetched by C++ backend, supports currentColor)
	repository: string;
	authors: string[];
	hasBinary: boolean;
	hasNpx: boolean;
	hasUvx: boolean;
	npxPackage?: string;
	uvxPackage?: string;
	// Install status
	isInstalled: boolean;
	installedVersion?: string;
	latestVersion?: string;
	updateAvailable?: boolean;
	installMethod: string; // "binary" | "npx" | "uvx" | ""
};

/** Get all agents from the ACP registry (cached, platform-filtered) */
export async function getRegistryAgents(): Promise<RegistryAgent[]> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getregistryagents();
		return parseResult<RegistryAgent[]>(result);
	}
	return [];
}

/** Force refresh the ACP registry from the CDN */
export async function refreshRegistry(): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.refreshregistry();
	}
}

/** Install a registry agent. Method: "binary" | "npx" | "uvx" | "auto" */
export async function installRegistryAgent(agentId: string, method: string = 'auto'): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.installregistryagent(agentId, method);
	}
}

/** Uninstall a registry agent (removes downloaded binaries) */
export async function uninstallRegistryAgent(agentId: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.uninstallregistryagent(agentId);
	}
}

/** Agent update info */
export type AgentUpdateInfo = {
	agentId: string;
	agentName: string;
	installedVersion: string;
	latestVersion: string;
	isNpx: boolean;
};

/** Get list of installed agents that have updates available */
export async function getAgentUpdates(): Promise<AgentUpdateInfo[]> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getagentupdates();
		return parseResult<AgentUpdateInfo[]>(result);
	}
	return [];
}

/** Trigger update for a binary agent (removes old version, downloads new on next use) */
export async function updateRegistryAgent(agentId: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.updateregistryagent(agentId);
	}
}

// ── Prerequisites ───────────────────────────────────────────────────

export type PrerequisiteTool = {
	found: boolean;
	path: string;
	version?: string;
};

export type PrerequisiteStatus = {
	node: PrerequisiteTool;
	npm: PrerequisiteTool;
	npx: PrerequisiteTool;
	git: PrerequisiteTool;
	uv: PrerequisiteTool;
	uvx: PrerequisiteTool;
	bun: PrerequisiteTool;
};

/** Check which prerequisite tools are installed on the system */
export async function getPrerequisiteStatus(): Promise<PrerequisiteStatus> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getprerequisitestatus();
		return parseResult<PrerequisiteStatus>(result);
	}
	const empty: PrerequisiteTool = { found: false, path: '' };
	return { node: empty, npm: empty, npx: empty, git: empty, uv: empty, uvx: empty, bun: empty };
}

/** Copy text to system clipboard */
export async function copyToClipboard(text: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.copytoclipboard(text);
	}
}

/** Read text from system clipboard */
export async function getClipboardText(): Promise<string> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getclipboardtext();
		const raw = (result && typeof result === 'object' && 'ReturnValue' in result)
			? result.ReturnValue
			: result;
		return (raw as string) || '';
	}
	return '';
}

/** Open URL in system browser */
export async function openUrl(url: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.openurl(url);
	}
}

/** Open an asset or source file in UE. Handles /Game/ paths, filesystem paths, file:line format. */
export async function openPath(path: string, line: number = 0): Promise<void> {
	const bridge = getBridge();
	if (!bridge) {
		console.warn('[AIK] openPath: bridge not available');
		return;
	}
	if (typeof bridge.openpath !== 'function') {
		console.warn('[AIK] openPath: bridge.openpath is not a function, available methods:', Object.keys(bridge));
		return;
	}
	try {
		await bridge.openpath(path, line);
	} catch (e) {
		console.error('[AIK] openPath failed:', e);
	}
}

/** Open the plugin settings panel in UE Project Settings */
export async function openPluginSettings(): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.openpluginsettings();
	}
}

/** Restart Unreal Editor (prompts to save unsaved work) */
export async function restartEditor(): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.restarteditor();
	}
}

/** Trigger an async plugin update check in UE */
export async function checkForPluginUpdate(): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.checkforpluginupdate();
	}
}

// ── Model & Reasoning ───────────────────────────────────────────────

export type ModelInfo = {
	id: string;
	name: string;
	description: string;
	supportsReasoning: boolean;
	provider?: string;
	providerDisplayName?: string;
};

export type ModelState = {
	models: ModelInfo[];
	currentModelId: string;
};

/** Get available models for an agent */
export async function getModels(agentName: string): Promise<ModelState> {
	if (currentTransport === 'remote') return relayCall<ModelState>('getModels', agentName);
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getmodels(agentName);
		return parseResult(result);
	}
	return { models: [], currentModelId: '' };
}

/** Get full model list for an agent (OpenRouter supports this). */
export async function getAllModels(agentName: string): Promise<ModelState> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getallmodels(agentName);
		return parseResult(result);
	}
	return { models: [], currentModelId: '' };
}

/** Set the active model for an agent */
export async function setModel(agentName: string, modelId: string): Promise<void> {
	if (currentTransport === 'remote') { await relayCall('setModel', agentName, modelId); return; }
	const bridge = getBridge();
	if (bridge) {
		await bridge.setmodel(agentName, modelId);
	}
}

/** Get current reasoning effort level for an agent */
export async function getReasoningLevel(agentName: string): Promise<string> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getreasoninglevel(agentName);
		// ReturnValue is a plain string, not JSON
		const raw = (result && typeof result === 'object' && 'ReturnValue' in result)
			? result.ReturnValue
			: result;
		return (raw as string) || 'medium';
	}
	return '';
}

/** Set reasoning effort level for an agent */
export async function setReasoningLevel(agentName: string, level: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setreasoninglevel(agentName, level);
	}
}

/** Register callback for streaming message updates */
export function onMessage(callback: (sessionId: string, update: StreamingUpdate) => void): void {
	if (currentTransport === 'remote') {
		onRelayEvent('onMessage', (data: any) => {
			callback(data.sessionId, data.update);
		});
		return;
	}
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonmessage((sessionId: string, updateJson: string) => {
			const update: StreamingUpdate = JSON.parse(updateJson);
			callback(sessionId, update);
		});
	}
}

/** Register callback for agent state changes */
export function onStateChanged(callback: (sessionId: string, agentName: string, state: string, message: string) => void): void {
	if (currentTransport === 'remote') {
		onRelayEvent('onStateChanged', (data: any) => {
			callback(data.sessionId, data.agentName, String(data.state), data.message);
		});
		return;
	}
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonstatechanged(callback);
	}
}

/** Register callback for MCP tool readiness status: "waiting" | "ready" | "timeout" */
export function onMcpStatus(callback: (sessionId: string, status: string) => void): void {
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonmcpstatus(callback);
	}
}

/** Register callback for session list updates from agents */
export function onSessionListUpdated(callback: (agentName: string, sessions: SessionInfo[]) => void): void {
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonsessionlistupdated((agentName: string, sessionsJson: string) => {
			const sessions: SessionInfo[] = JSON.parse(sessionsJson).map((s: any) => ({
				...s,
				agentName,
				isConnected: false
			}));
			callback(agentName, sessions);
		});
	}
}

/** Manually refresh session lists from all agents. Returns how many agents are being connected. */
export async function refreshSessionList(): Promise<{ connectingCount: number }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.refreshsessionlist();
		return parseResult(result);
	}
	return { connectingCount: 0 };
}

// ── Permissions ─────────────────────────────────────────────────────

export type PermissionOption = {
	optionId: string;
	name: string;
	kind: 'allow_always' | 'allow_once' | 'reject_once';
};

export type PermissionToolCall = {
	toolCallId: string;
	title: string;
	rawInput: string;
};

export type QuestionOption = {
	label: string;
	description: string;
};

export type Question = {
	question: string;
	header: string;
	options: QuestionOption[];
	multiSelect: boolean;
};

export type PermissionRequest = {
	agentName: string;
	requestId: number;
	options: PermissionOption[];
	toolCall: PermissionToolCall;
	isAskUserQuestion: boolean;
	questions: Question[];
};

/** Register callback for permission/consent requests */
export function onPermissionRequest(callback: (sessionId: string, request: PermissionRequest) => void): void {
	if (currentTransport === 'remote') {
		onRelayEvent('onPermissionRequest', (data: any) => {
			callback(data.sessionId, data);
		});
		return;
	}
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonpermissionrequest((sessionId: string, requestJson: string) => {
			const request: PermissionRequest = JSON.parse(requestJson);
			callback(sessionId, request);
		});
	}
}

/** Respond to a permission request */
export async function respondToPermission(
	agentName: string,
	requestId: number,
	optionId: string,
	outcomeMeta?: Record<string, unknown>
): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		const metaJson = outcomeMeta ? JSON.stringify(outcomeMeta) : '';
		await bridge.respondtopermission(agentName, requestId, optionId, metaJson);
	}
}

// ── Modes ───────────────────────────────────────────────────────────

export type ModeInfo = {
	id: string;
	name: string;
	description: string;
};

export type ModeState = {
	modes: ModeInfo[];
	currentModeId: string;
};

/** Get available modes for an agent */
export async function getModes(agentName: string): Promise<ModeState> {
	if (currentTransport === 'remote') return relayCall<ModeState>('getModes', agentName);
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getmodes(agentName);
		return parseResult(result);
	}
	return { modes: [], currentModeId: '' };
}

/** Set the active mode for an agent */
export async function setMode(agentName: string, modeId: string): Promise<void> {
	if (currentTransport === 'remote') { await relayCall('setMode', agentName, modeId); return; }
	const bridge = getBridge();
	if (bridge) {
		await bridge.setmode(agentName, modeId);
	}
}

/** Register callback for mode availability updates */
export function onModesAvailable(callback: (agentName: string, modeState: ModeState) => void): void {
	if (currentTransport === 'remote') {
		onRelayEvent('onModesAvailable', (data: any) => {
			callback(data.agentName, data);
		});
		return;
	}
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonmodesavailable((agentName: string, modesJson: string) => {
			const modeState: ModeState = JSON.parse(modesJson);
			callback(agentName, modeState);
		});
	}
}

/** Register callback for mode change notifications */
export function onModeChanged(callback: (agentName: string, modeId: string) => void): void {
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonmodechanged(callback);
	}
}

/** Register callback for model availability updates (async push from agents like Codex) */
export function onModelsAvailable(callback: (agentName: string, modelState: ModelState) => void): void {
	if (currentTransport === 'remote') {
		onRelayEvent('onModelsAvailable', (data: any) => {
			callback(data.agentName, data);
		});
		return;
	}
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonmodelsavailable((agentName: string, modelsJson: string) => {
			const modelState: ModelState = JSON.parse(modelsJson);
			callback(agentName, modelState);
		});
	}
}

// ── Slash Commands ──────────────────────────────────────────────────

export type SlashCommand = {
	name: string;
	description: string;
	inputHint: string;
};

/** Register callback for slash commands availability updates */
export function onCommandsAvailable(callback: (sessionId: string, commands: SlashCommand[]) => void): void {
	const bridge = getBridge();
	if (bridge) {
		bridge.bindoncommandsavailable((sessionId: string, commandsJson: string) => {
			const commands: SlashCommand[] = JSON.parse(commandsJson);
			callback(sessionId, commands);
		});
	}
}

// ── Plan/Todo ───────────────────────────────────────────────────────

export type PlanEntry = {
	content: string;
	activeForm: string;
	priority: 'high' | 'medium' | 'low';
	status: 'pending' | 'in_progress' | 'completed';
};

export type PlanUpdate = {
	entries: PlanEntry[];
	completedCount: number;
	totalCount: number;
};

/** Register callback for plan/todo updates */
export function onPlanUpdate(callback: (sessionId: string, plan: PlanUpdate) => void): void {
	if (currentTransport === 'remote') {
		onRelayEvent('onPlanUpdate', (data: any) => {
			callback(data.sessionId, data);
		});
		return;
	}
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonplanupdate((sessionId: string, planJson: string) => {
			const plan: PlanUpdate = JSON.parse(planJson);
			callback(sessionId, plan);
		});
	}
}

// ── Attachments ─────────────────────────────────────────────────────

export type AttachmentInfo = {
	id: string;
	type: 'blueprint_node' | 'blueprint' | 'image' | 'file';
	displayName: string;
	mimeType?: string;
	width?: number;
	height?: number;
	sizeBytes?: number;
	hasExtractedText?: boolean;
	thumbnail?: string;
};

/** Paste image from system clipboard into attachments */
export async function pasteClipboardImage(): Promise<{ success: boolean; error?: string }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.pasteclipboardimage();
		return parseResult(result);
	}
	return { success: false, error: 'Not in UE' };
}

/** Open native file picker for attachments (images + common docs) */
export async function openImagePicker(): Promise<{ success: boolean; count: number }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.openimagepicker();
		return parseResult(result);
	}
	return { success: false, count: 0 };
}

/** Add an image from base64 data (for JS-side drag-drop) */
export async function addImageFromBase64(
	base64: string, mimeType: string, width: number, height: number, displayName: string
): Promise<{ success: boolean; attachmentId?: string }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.addimagefrombase64(base64, mimeType, width, height, displayName);
		return parseResult(result);
	}
	return { success: false };
}

/** Add a generic file from base64 data (for JS-side drag-drop) */
export async function addFileFromBase64(
	base64: string, mimeType: string, displayName: string
): Promise<{ success: boolean; attachmentId?: string }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.addfilefrombase64(base64, mimeType, displayName);
		return parseResult(result);
	}
	return { success: false };
}

/** Remove an attachment by its GUID */
export async function removeAttachment(id: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.removeattachment(id);
	}
}

/** Get current attachments (metadata only) */
export async function getAttachments(): Promise<AttachmentInfo[]> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getattachments();
		return parseResult(result);
	}
	return [];
}

/** Register callback for attachment list changes */
export function onAttachmentsChanged(callback: (attachments: AttachmentInfo[]) => void): void {
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonattachmentschanged((attachmentsJson: string) => {
			const attachments: AttachmentInfo[] = JSON.parse(attachmentsJson);
			callback(attachments);
		});
	}
}

// ── Context Mentions ────────────────────────────────────────────────

export type ContextItem = {
	name: string;
	path: string;
	category: string;
	type: string;
	icon?: string; // Raw SVG string from engine (Starship class icons)
};

/** Search for assets/files to attach via @ mention */
export async function searchContextItems(query: string): Promise<ContextItem[]> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.searchcontextitems(query);
		return parseResult(result);
	}
	return [];
}

// ── Agent Authentication ────────────────────────────────────────────

export type AuthMethod = {
	id: string;
	name: string;
	description: string;
	isTerminalAuth: boolean;
};

/** Get available auth methods for an agent */
export async function getAuthMethods(agentName: string): Promise<AuthMethod[]> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getauthmethods(agentName);
		return parseResult(result);
	}
	return [];
}

/** Start agent login with a specific auth method */
export async function startAgentLogin(agentName: string, methodId: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.startagentlogin(agentName, methodId);
	}
}

/** Register callback for login completion */
export function onLoginComplete(callback: (agentName: string, success: boolean, errorMessage: string) => void): void {
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonlogincomplete(callback);
	}
}

// ── Agent Usage / Rate Limits ──────────────────────────────────────

export type RateLimitWindow = {
	usedPercent: number;
	resetsAt: string;
	windowDurationMinutes: number;
	hasData: boolean;
};

export type ExtraUsage = {
	isEnabled: boolean;
	usedAmount: number;
	limitAmount: number;
	currencyCode: string;
	hasData: boolean;
};

export type MeshyBalance = {
	configured: boolean;
	balance: number;
	isLoading: boolean;
	error: string;
};

export type AgentRateLimitData = {
	hasData: boolean;
	isLoading: boolean;
	errorMessage: string;
	agentName: string;
	planType: string;
	lastUpdated: string;
	primary: RateLimitWindow;
	secondary: RateLimitWindow;
	modelSpecific: RateLimitWindow;
	modelSpecificLabel: string;
	extraUsage: ExtraUsage;
	meshy: MeshyBalance;
};

/** Get cached rate limit data for an agent (triggers background fetch if needed) */
export async function getAgentUsage(agentName: string): Promise<AgentRateLimitData> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getagentusage(agentName);
		return parseResult(result);
	}
	return { hasData: false, isLoading: false, errorMessage: '', agentName, planType: '', lastUpdated: '', primary: { usedPercent: 0, resetsAt: '', windowDurationMinutes: 0, hasData: false }, secondary: { usedPercent: 0, resetsAt: '', windowDurationMinutes: 0, hasData: false }, modelSpecific: { usedPercent: 0, resetsAt: '', windowDurationMinutes: 0, hasData: false }, modelSpecificLabel: '', extraUsage: { isEnabled: false, usedAmount: 0, limitAmount: 0, currencyCode: '', hasData: false }, meshy: { configured: false, balance: -1, isLoading: false, error: '' } };
}

/** Force-refresh usage data for an agent */
export async function refreshAgentUsage(agentName: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.refreshagentusage(agentName);
	}
}

/** Register callback for agent usage/rate-limit updates */
export function onUsageUpdated(callback: (agentName: string, data: AgentRateLimitData) => void): void {
	const bridge = getBridge();
	if (bridge) {
		bridge.bindonusageupdated((agentName: string, usageJson: string) => {
			const data: AgentRateLimitData = JSON.parse(usageJson);
			callback(agentName, data);
		});
	}
}

// ── Tool Profiles & Settings ────────────────────────────────────────

export type ToolInfo = {
	name: string;
	displayName: string;
	description: string;
	extendedDescription: string;
	category: string;
	enabled: boolean;
	descriptionOverride: string;
};

export type ProfileInfo = {
	profileId: string;
	displayName: string;
	description: string;
	isBuiltIn: boolean;
	isActive: boolean;
	enabledToolCount: number;
};

export type ProfilesState = {
	profiles: ProfileInfo[];
	activeProfileId: string;
};

/** Get all tools with metadata and enabled state. Pass profileId to view tools for a specific profile, or empty for global. */
export async function getTools(profileId: string = ''): Promise<ToolInfo[]> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.gettools(profileId);
		return parseResult(result);
	}
	return [];
}

/** Get all profiles and the active profile ID */
export async function getProfiles(): Promise<ProfilesState> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getprofiles();
		return parseResult(result);
	}
	return { profiles: [], activeProfileId: '' };
}

/** Set the active profile (empty string = no profile) */
export async function setActiveProfile(profileId: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setactiveprofile(profileId);
	}
}

/** Toggle a tool's global enabled state */
export async function setToolEnabled(toolName: string, enabled: boolean): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.settoolenabled(toolName, enabled);
	}
}

/** Toggle a tool within a specific profile's whitelist */
export async function setProfileToolEnabled(profileId: string, toolName: string, enabled: boolean): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.setprofiletoolenabled(profileId, toolName, enabled);
	}
}

/** Create a custom profile. Returns the new profile ID. */
export async function createProfile(displayName: string, description: string): Promise<string> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.createprofile(displayName, description);
		const data = parseResult<{ profileId: string }>(result);
		return data.profileId;
	}
	return '';
}

/** Delete a custom profile */
export async function deleteProfile(profileId: string): Promise<boolean> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.deleteprofile(profileId);
		const data = parseResult<{ success: boolean }>(result);
		return data.success;
	}
	return false;
}

// ── Profile Detail & Editing ────────────────────────────────────────

export type ProfileDetail = {
	found: boolean;
	profileId: string;
	displayName: string;
	description: string;
	isBuiltIn: boolean;
	customInstructions: string;
	toolDescriptionOverrides: Record<string, string>;
};

/** Get full profile details including custom instructions and tool description overrides */
export async function getProfileDetail(profileId: string): Promise<ProfileDetail> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getprofiledetail(profileId);
		return parseResult(result);
	}
	return { found: false, profileId: '', displayName: '', description: '', isBuiltIn: false, customInstructions: '', toolDescriptionOverrides: {} };
}

/** Update profile metadata (name, description, custom instructions) */
export async function updateProfile(profileId: string, displayName: string, description: string, customInstructions: string): Promise<boolean> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.updateprofile(profileId, displayName, description, customInstructions);
		const data = parseResult<{ success: boolean }>(result);
		return data.success;
	}
	return false;
}

/** Set or clear a tool description override for a profile. Empty override = clear. */
export async function setToolDescriptionOverride(profileId: string, toolName: string, descriptionOverride: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.settooldescriptionoverride(profileId, toolName, descriptionOverride);
	}
}

// ── Project Indexing ────────────────────────────────────────────────

export type IndexingScopeBreakdown = {
	blueprints: number;
	cppFiles: number;
	assets: number;
	levels: number;
	config: number;
	documents: number;
};

export type IndexingSettings = {
	provider: 'openrouter' | 'custom';
	endpointUrl: string;
	apiKey: string;
	model: string;
	dimensions: number;
	autoIndex: boolean;
	scope: {
		blueprints: boolean;
		cppFiles: boolean;
		assets: boolean;
		levels: boolean;
		config: boolean;
		documents: boolean;
	};
	hasOpenRouterKey: boolean;
};

export type IndexingStatus = {
	state: 'idle' | 'indexing' | 'ready' | 'error';
	totalChunks: number;
	indexedChunks: number;
	lastIndexedAt: string;
	indexSizeBytes: number;
	errorMessage: string;
	breakdown: IndexingScopeBreakdown;
	embeddingModel: string;
	embeddingDimensions: number;
};

export async function getIndexingSettings(): Promise<IndexingSettings> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getindexingsettings();
		return parseResult(result);
	}
	return {
		provider: 'openrouter', endpointUrl: '', apiKey: '', model: 'google/gemini-embedding-001',
		dimensions: 768, autoIndex: false,
		scope: { blueprints: true, cppFiles: true, assets: true, levels: true, config: false, documents: true },
		hasOpenRouterKey: false
	};
}

export async function getIndexingStatus(): Promise<IndexingStatus> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getindexingstatus();
		return parseResult(result);
	}
	return { state: 'idle', totalChunks: 0, indexedChunks: 0, lastIndexedAt: '', indexSizeBytes: 0, errorMessage: '', breakdown: { blueprints: 0, cppFiles: 0, assets: 0, levels: 0, config: 0, documents: 0 }, embeddingModel: '', embeddingDimensions: 0 };
}

export async function setIndexingProvider(provider: 'openrouter' | 'custom'): Promise<void> {
	const bridge = getBridge();
	if (bridge) await bridge.setindexingprovider(provider);
}

export async function setIndexingEndpointUrl(url: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) await bridge.setindexingendpointurl(url);
}

export async function setIndexingApiKey(key: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) await bridge.setindexingapikey(key);
}

export async function setIndexingModel(model: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) await bridge.setindexingmodel(model);
}

export async function setIndexingDimensions(dims: number): Promise<void> {
	const bridge = getBridge();
	if (bridge) await bridge.setindexingdimensions(dims);
}

export async function setAutoIndex(enabled: boolean): Promise<void> {
	const bridge = getBridge();
	if (bridge) await bridge.setautoindex(enabled);
}

export async function setIndexingScopeEnabled(scope: string, enabled: boolean): Promise<void> {
	const bridge = getBridge();
	if (bridge) await bridge.setindexingscopeenabled(scope, enabled);
}

export async function startIndexing(): Promise<void> {
	const bridge = getBridge();
	if (bridge) await bridge.startindexing();
}

export async function clearIndex(): Promise<void> {
	const bridge = getBridge();
	if (bridge) await bridge.clearindex();
}

// ── Source Control ──────────────────────────────────────────────────

export type SourceControlStatus = {
	enabled: boolean;
	provider: string;
	branch: string;
	changesCount: number;
	connected: boolean;
};

/** Get current source control status (branch, changes, provider) */
export async function getSourceControlStatus(): Promise<SourceControlStatus> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getsourcecontrolstatus();
		return parseResult(result);
	}
	return { enabled: false, provider: '', branch: '', changesCount: -1, connected: false };
}

/** Open the UE source control changelists tab */
export async function openSourceControlChangelist(): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.opensourcecontrolchangelist();
	}
}

/** Open the UE check-in/submit dialog */
export async function openSourceControlSubmit(): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.opensourcecontrolsubmit();
	}
}

// ── Terminal ────────────────────────────────────────────────────────

/** Start a new terminal session. Returns the terminal ID. */
export async function startTerminal(workingDir: string = '', shell: string = ''): Promise<{ terminalId?: string; error?: string }> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.startterminal(workingDir, shell);
		return parseResult(result);
	}
	return { error: 'Not in UE' };
}

/** Write input data to a terminal (raw string from xterm.js onData) */
export async function writeTerminal(terminalId: string, data: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.writeterminal(terminalId, data);
	}
}

/** Resize terminal PTY */
export async function resizeTerminal(terminalId: string, cols: number, rows: number): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.resizeterminal(terminalId, cols, rows);
	}
}

/** Close a terminal session */
export async function closeTerminal(terminalId: string): Promise<void> {
	const bridge = getBridge();
	if (bridge) {
		await bridge.closeterminal(terminalId);
	}
}

const terminalOutputListeners = new Set<(terminalId: string, base64Data: string) => void>();
const terminalExitListeners = new Set<(terminalId: string, exitCode: number) => void>();
let terminalOutputBridgeBound = false;
let terminalExitBridgeBound = false;

function dispatchTerminalOutput(terminalId: string, base64Data: string): void {
	for (const listener of terminalOutputListeners) {
		try {
			listener(terminalId, base64Data);
		} catch (e) {
			console.warn('subscribeTerminalOutput listener error:', e);
		}
	}
}

function dispatchTerminalExit(terminalId: string, exitCode: number): void {
	for (const listener of terminalExitListeners) {
		try {
			listener(terminalId, exitCode);
		} catch (e) {
			console.warn('subscribeTerminalExit listener error:', e);
		}
	}
}

/**
 * Subscribe to PTY output for all terminal sessions. Multiple xterm instances must each subscribe —
 * UE only allows one bridge callback; this multicasts.
 * @returns Unsubscribe (call on component destroy).
 */
export function subscribeTerminalOutput(
	callback: (terminalId: string, base64Data: string) => void
): () => void {
	terminalOutputListeners.add(callback);
	const bridge = getBridge();
	if (bridge && !terminalOutputBridgeBound) {
		bridge.bindonterminaloutput(dispatchTerminalOutput);
		terminalOutputBridgeBound = true;
	}
	return () => {
		terminalOutputListeners.delete(callback);
	};
}

/** @returns Unsubscribe (call on component destroy). */
export function subscribeTerminalExit(
	callback: (terminalId: string, exitCode: number) => void
): () => void {
	terminalExitListeners.add(callback);
	const bridge = getBridge();
	if (bridge && !terminalExitBridgeBound) {
		bridge.bindonterminalexit(dispatchTerminalExit);
		terminalExitBridgeBound = true;
	}
	return () => {
		terminalExitListeners.delete(callback);
	};
}

// ── Studio / Generative Providers ──────────────────────────────────

export type GenerativeActionDescriptor = {
	actionId: string;
	description: string;
	inputHints: string[];
	outputHints: string[];
	creditCost: string;
	isSynchronous: boolean;
	paramsSchema?: {
		type: string;
		properties: Record<string, {
			type: string;
			description?: string;
			enum?: string[];
			default?: unknown;
			minimum?: number;
			maximum?: number;
		}>;
		required?: string[];
	};
};

export type GenerativeProviderInfo = {
	id: string;
	displayName: string;
	website: string;
	actions: GenerativeActionDescriptor[];
};

export type GenerativeJobInfo = {
	providerId: string;
	actionId: string;
	jobId: string;
	status: 'pending' | 'running' | 'succeeded' | 'failed' | 'cancelled';
	progress: number;
	resultUrl: string;
	thumbnailUrl: string;
	extraUrls: Record<string, string>;
	imageUrls: string[];
	error: string;
};

export type GenerativeJobResult = {
	success: boolean;
	job?: GenerativeJobInfo;
	error?: string;
};

export type GenerativeBalanceResult = {
	success: boolean;
	balance: number;
	error?: string;
};

/** Get all registered generative providers with their actions and parameter schemas */
export async function getGenerativeProviders(): Promise<GenerativeProviderInfo[]> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getgenerativeproviders();
		return parseResult<GenerativeProviderInfo[]>(result);
	}
	return [];
}

/** Submit a generation job to a provider */
export async function submitGenerativeJob(
	providerId: string,
	actionId: string,
	params: Record<string, unknown>
): Promise<GenerativeJobResult> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.submitgenerativejob(providerId, actionId, JSON.stringify(params));
		return parseResult<GenerativeJobResult>(result);
	}
	return { success: false, error: 'Bridge not available' };
}

/** Check status of a generation job */
export async function checkGenerativeJobStatus(
	providerId: string,
	jobId: string,
	actionId: string
): Promise<GenerativeJobResult> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.checkgenerativejobstatus(providerId, jobId, actionId);
		return parseResult<GenerativeJobResult>(result);
	}
	return { success: false, error: 'Bridge not available' };
}

/** Get credit balance for a generative provider */
export async function getGenerativeBalance(providerId: string): Promise<GenerativeBalanceResult> {
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getgenerativebalance(providerId);
		return parseResult<GenerativeBalanceResult>(result);
	}
	return { success: false, balance: -1, error: 'Bridge not available' };
}

// ── Crash Reporting ─────────────────────────────────────────────────

export type CrashRecord = {
	crashId: string;
	timestamp: string;
	errorMessage: string;
	crashType: string;
	callstackSummary: string;
	basicReported: boolean;
	fullLogSent: boolean;
	fullLogDeclined: boolean;
	manuallyReported: boolean;
};

/** Get crash history from local crash_history.json */
export async function getCrashHistory(): Promise<CrashRecord[]> {
	if (currentTransport === 'remote') return relayCall<CrashRecord[]>('getCrashHistory');
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.getcrashhistory();
		return parseResult<CrashRecord[]>(result);
	}
	return [];
}

/** Manually send a crash report for a previously declined crash */
export async function reportCrash(crashId: string): Promise<{ success: boolean }> {
	if (currentTransport === 'remote') return relayCall<{ success: boolean }>('reportCrash', crashId);
	const bridge = getBridge();
	if (bridge) {
		const result = await bridge.reportcrash(crashId);
		return parseResult<{ success: boolean }>(result);
	}
	return { success: false };
}
