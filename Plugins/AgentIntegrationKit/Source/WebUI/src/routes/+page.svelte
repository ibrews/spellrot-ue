<script lang="ts">
	import { tick } from 'svelte';
	import { PaneGroup, Pane, PaneResizer } from 'paneforge';
	import Sidebar from '$lib/components/Sidebar.svelte';
	import ChatPane from '$lib/components/ChatPane.svelte';
	import StudioPage from '$lib/components/StudioPage.svelte';
	import TerminalPane from '$lib/components/TerminalPane.svelte';
	import { currentTab } from '$lib/stores/navigation.js';
	import ChatMessageComponent from '$lib/components/ChatMessage.svelte';
	import PermissionDialog from '$lib/components/PermissionDialog.svelte';
	import { paneManager } from '$lib/stores/panes.svelte.js';
	import AskUserDialog from '$lib/components/AskUserDialog.svelte';
	import ContextPopup from '$lib/components/ContextPopup.svelte';
	import CommandPopup from '$lib/components/CommandPopup.svelte';
	import PlanPanel from '$lib/components/PlanPanel.svelte';
	import AgentSetup from '$lib/components/AgentSetup.svelte';
	import OnboardingWizard from '$lib/components/OnboardingWizard.svelte';
	import AuthBanner from '$lib/components/AuthBanner.svelte';
	import UsageBar from '$lib/components/UsageBar.svelte';
	import AttachmentChips from '$lib/components/AttachmentChips.svelte';
	import SettingsPanel from '$lib/components/SettingsPanel.svelte';
	import Icon from '$lib/components/Icon.svelte';
	import {
		MoreHorizontalIcon,
		Add01Icon,
		GitBranchIcon,
		SidebarLeftIcon,
		ArrowDown01Icon,
		ChangeScreenModeIcon,
		ArrowUp01Icon,
		StopIcon,
		LayoutLeftIcon,
		Cancel01Icon
	} from '@hugeicons/core-free-icons';
	import * as DropdownMenu from '$lib/components/ui/dropdown-menu/index.js';
	import { agents, selectedAgent, agentsLoaded, statusDotColor, type Agent } from '$lib/stores/agents.js';
	import { showOnboarding, onboardingLoading, checkOnboarding } from '$lib/stores/onboarding.js';
	import { sessions, currentSessionId, createNewSession } from '$lib/stores/sessions.js';
	import {
		models,
		currentModelId,
		isLoadingModels,
		reasoningLevel,
		modelBrowserOpen,
		allModels,
		isLoadingAllModels,
		changeModel,
		changeReasoningLevel,
		openModelBrowser,
		closeModelBrowser,
		loadModelsForAgent,
		loadReasoningLevel,
		reasoningLabels,
		type ReasoningLevel
	} from '$lib/stores/models.js';
	import { currentState, activeSessionId, stateDisplay, currentMcpStatus } from '$lib/stores/agentState.js';
	import {
		messages,
		isStreaming,
		loadMessages,
		sendMessage,
		cancelCurrentPrompt,
		finishStreaming
	} from '$lib/stores/messages.js';
	import { pendingPermission } from '$lib/stores/permissions.js';
	import { availableModes, currentModeId, loadModesForAgent, changeMode, isInPlanMode } from '$lib/stores/modes.js';
	import { sessionUsage, hasUsage, contextPercent, formatTokens, formatCost } from '$lib/stores/usage.js';
	import { setupAgent, enterSetup } from '$lib/stores/setup.js';
	import { pendingComposerDraft, clearComposerDraft } from '$lib/stores/composer.js';
	import { addDraftToHistory, getComposerHistoryEntries } from '$lib/stores/composerHistory.js';
	import { loadAgentUsage } from '$lib/stores/rateLimits.js';
	import { availableCommands } from '$lib/stores/commands.js';
	import { currentPlan, hasPlan } from '$lib/stores/plan.js';
	import { hasAttachments, pasteImage, pickAttachments, addDroppedFile } from '$lib/stores/attachments.js';
	import { resetAuth } from '$lib/stores/auth.js';
	import { settingsOpen } from '$lib/stores/settings.js';
	import { scEnabled, branchName, hasChanges, changesLabel } from '$lib/stores/sourceControl.js';
	import {
		openSourceControlChangelist,
		openSourceControlSubmit,
		copyToClipboard,
		getClipboardText,
		type SlashCommand
	} from '$lib/bridge.js';
	import * as Tooltip from '$lib/components/ui/tooltip/index.js';
	import { t } from '$lib/i18n.js';

	let sidebarOpen = $state((() => {
		try { const v = localStorage.getItem('sidebar_open'); return v !== null ? v === 'true' : true; }
		catch { return true; }
	})());
	let sidebarMounted = $state(false);
	$effect(() => { requestAnimationFrame(() => { sidebarMounted = true; }); });
	let inputText = $state('');
	let messageContainer: HTMLDivElement | undefined = $state();
	let textareaEl: HTMLTextAreaElement | undefined = $state();
	let isTextComposing = $state(false);
	let historyIndex = $state<number | null>(null);
	let draftBeforeHistory = $state('');
	let lastEscapeAt = $state(0);
	let userNearBottom = $state(true);

	// Per-session state that survives session switches
	const draftBySession = new Map<string, string>();
	const scrollBySession = new Map<string, number>();
	const nearBottomBySession = new Map<string, boolean>();

	// @ mention popup state
	let contextPopupVisible = $state(false);
	let contextQuery = $state('');
	let contextPopupRef: ContextPopup | undefined = $state();
	let mentionStartPos = $state(-1);

	// / command popup state
	let commandPopupVisible = $state(false);
	let commandQuery = $state('');
	let commandPopupRef: CommandPopup | undefined = $state();
	let commandStartPos = $state(-1);

	// Drag-drop state
	let dragOver = $state(false);
	const COMMON_FILESYSTEM_ROOTS = new Set([
		'Applications',
		'Users',
		'private',
		'tmp',
		'var',
		'etc',
		'System',
		'Volumes',
		'home',
		'opt',
		'usr',
		'bin',
		'sbin',
		'dev'
	]);
	const DOUBLE_ESCAPE_WINDOW_MS = 900;

	// Check onboarding status once agents are loaded
	$effect(() => {
		if ($agentsLoaded) {
			checkOnboarding();
		}
	});

	// Sync activeSessionId for agent state tracking
	$effect(() => {
		activeSessionId.set($currentSessionId);
	});

	// Initialize pane manager when first session is selected.
	// Only sync focused pane when currentSessionId actually changes (not on focus switch).
	let prevSyncedSessionId: string | null = null;
	$effect(() => {
		const sid = $currentSessionId;
		if (!sid) return;
		if (paneManager.paneCount === 0) {
			paneManager.init(sid);
			prevSyncedSessionId = sid;
		} else if (sid !== prevSyncedSessionId) {
			// currentSessionId changed externally (sidebar click, new chat) — update focused pane
			prevSyncedSessionId = sid;
			const focusedPane = paneManager.focusedPane;
			if (focusedPane && focusedPane.sessionId !== sid) {
				paneManager.openInFocused(sid);
			}
		}
	});

	function handleSplitPane() {
		if (paneManager.canSplit) {
			paneManager.split(null); // New empty pane, user picks a session
		}
	}

	function handleUnsplit() {
		paneManager.unsplit();
	}

	function handlePaneFocus(index: number) {
		paneManager.setFocus(index);
		// Sync currentSessionId to focused pane's session
		const pane = paneManager.panes[index];
		if (pane?.sessionId && pane.sessionId !== $currentSessionId) {
			currentSessionId.set(pane.sessionId);
		}
	}

	function handlePaneClose(index: number) {
		paneManager.closePane(index);
		// Sync currentSessionId to the new focused pane
		const focused = paneManager.focusedPane;
		if (focused?.sessionId) {
			currentSessionId.set(focused.sessionId);
		}
	}

	// Save/restore per-session state on session switch
	let prevSessionId: string | null = null;
	$effect(() => {
		const sid = $currentSessionId;

		// Save state for previous session
		if (prevSessionId) {
			draftBySession.set(prevSessionId, inputText);
			if (messageContainer) {
				scrollBySession.set(prevSessionId, messageContainer.scrollTop);
				nearBottomBySession.set(prevSessionId, userNearBottom);
			}
		}

		// Restore state for new session
		if (sid) {
			inputText = draftBySession.get(sid) ?? '';
			userNearBottom = nearBottomBySession.get(sid) ?? true;
			// Scroll restoration is deferred until after messages render
			tick().then(() => {
				if (messageContainer && sid === $currentSessionId) {
					const saved = scrollBySession.get(sid);
					if (saved !== undefined && !userNearBottom) {
						messageContainer.scrollTop = saved;
					} else {
						messageContainer.scrollTop = messageContainer.scrollHeight;
					}
				}
				resizeTextarea();
			});
		} else {
			inputText = '';
		}

		prevSessionId = sid;
	});

	// Load models and modes when session is active (requires both agent + session)
	$effect(() => {
		const agent = $selectedAgent;
		const sid = $currentSessionId;
			if (agent && sid) {
				loadModelsForAgent(agent.name);
				loadReasoningLevel(agent.name);
				loadModesForAgent(agent.name);
			} else {
			// No session — clear agent-specific UI state
			models.set([]);
			currentModelId.set('');
			availableModes.set([]);
			currentModeId.set('');
		}
	});

	// Load rate limit data when selected agent changes
	$effect(() => {
		const agent = $selectedAgent;
		if (agent && agent.status === 'available') {
			loadAgentUsage(agent.name);
		}
	});

	// Load messages when session changes
	$effect(() => {
		const sid = $currentSessionId;
		historyIndex = null;
		draftBeforeHistory = '';
		lastEscapeAt = 0;
		if (sid) {
			loadMessages(sid);
		}
		// Usage and plan are now per-session derived stores — no reset needed.
		resetAuth();
	});

	// Prefill composer for continuation drafts (do not auto-send).
	$effect(() => {
		const sid = $currentSessionId;
		const draft = $pendingComposerDraft;
		if (!sid || !draft || draft.sessionId !== sid) return;

		inputText = draft.text;
		clearComposerDraft();
		tick().then(() => resizeTextarea());
	});

	// Clean up per-session caches for deleted sessions.
	$effect(() => {
		const valid = new Set($sessions.map((s) => s.sessionId));
		for (const sid of draftBySession.keys()) {
			if (!valid.has(sid)) {
				draftBySession.delete(sid);
				scrollBySession.delete(sid);
				nearBottomBySession.delete(sid);
			}
		}
	});

	// Auto-scroll to bottom on new messages or permission dialogs
	$effect(() => {
		const _msgs = $messages;
		const _perm = $pendingPermission;
		if (userNearBottom && messageContainer) {
			tick().then(() => {
				if (messageContainer) {
					messageContainer.scrollTop = messageContainer.scrollHeight;
				}
			});
		}
	});

	// Current session title for the header
	let currentSession = $derived($sessions.find(s => s.sessionId === $currentSessionId));
	let headerTitle = $derived($currentSessionId ? (currentSession?.title || $t('new_chat')) : $t('agent_chat'));
	let headerAgent = $derived($currentSessionId ? (currentSession?.agentName ?? $selectedAgent?.name ?? '') : '');

	// Model picker
	let hasModels = $derived($models.length > 0);
	let currentModel = $derived($models.find(m => m.id === $currentModelId));
	let modelDisplayName = $derived(currentModel?.name ?? $currentModelId ?? 'Model');
	let currentModelSupportsReasoning = $derived(currentModel?.supportsReasoning ?? false);

	// Connection state
	let connectionInfo = $derived($currentState ? stateDisplay[$currentState.state] : null);

	// Mode selector
	let hasModes = $derived($availableModes.length > 0);
	let currentMode = $derived($availableModes.find(m => m.id === $currentModeId));
	let modeDisplayName = $derived(currentMode?.name ?? $currentModeId ?? $t('mode_default'));
	let isPlanMode = $derived(isInPlanMode($currentModeId));

	// Usage display
	let usageHasContext = $derived($sessionUsage.contextSize > 0);
	let usageHasTokens = $derived($sessionUsage.inputTokens > 0 || $sessionUsage.outputTokens > 0);
	let usageHasCost = $derived($sessionUsage.costAmount > 0);
	let usageContextLabel = $derived(
		usageHasContext
			? `${$contextPercent}% context`
			: usageHasTokens
				? `${formatTokens($sessionUsage.inputTokens + $sessionUsage.outputTokens)} tokens`
				: ''
	);

	// Input state
	let waitingForMcp = $derived($currentMcpStatus === 'waiting');
	let modelSearchQuery = $state('');
	let modelOptions = $derived($models.filter(m => m.id !== 'special:browse_all'));
	let hasBrowseAllModelsAction = $derived($models.some(m => m.id === 'special:browse_all'));
	let filteredAllModels = $derived.by(() => {
		const q = modelSearchQuery.trim().toLowerCase();
		if (!q) return $allModels;
		return $allModels.filter(model =>
			model.name.toLowerCase().includes(q) ||
			model.id.toLowerCase().includes(q) ||
			(model.description ?? '').toLowerCase().includes(q)
		);
	});
	// Agent is ready when state says so, OR when we're switching sessions on
	// the same agent and models are already loaded (state callback hasn't arrived yet)
	let agentReady = $derived.by(() => {
		if (waitingForMcp) return false;
		const state = $currentState?.state;
		if (state === 'ready' || state === 'in_session' || state === 'prompting' || state === 'error') return true;
		// If state is null/undefined (switching sessions), check if models are
		// already loaded for this agent — means the agent is connected, just
		// the per-session state callback hasn't fired yet
		if (!state && $currentSessionId && $models.length > 0) return true;
		return false;
	});
	let canSend = $derived(inputText.trim().length > 0 && !!$currentSessionId && !$isStreaming && agentReady);
	let inputPlaceholder = $derived(
		waitingForMcp
			? $t('connecting_tools')
			: !agentReady
				? ($currentState?.state === 'connecting' || $currentState?.state === 'initializing'
					? $t('waiting_for_agent_connect')
					: $currentState?.state === 'ready'
						? $t('ready_to_send')
						: $currentState?.state === 'error'
							? ($currentState?.message || $t('agent_error_check_connection'))
							: $t('waiting_for_agent'))
				: $messages.length > 0
					? $t('ask_follow_up')
					: $t('describe_task')
	);

	async function handleBrowseAllModels() {
		if (!$selectedAgent) return;
		modelSearchQuery = '';
		await openModelBrowser($selectedAgent.name);
	}

	async function handleSelectModelFromBrowser(modelId: string) {
		if (!$selectedAgent) return;
		await changeModel($selectedAgent.name, modelId);
		closeModelBrowser();
	}

	const reasoningLevels: ReasoningLevel[] = ['none', 'low', 'medium', 'high', 'max'];

	function toggleSidebar() {
		sidebarOpen = !sidebarOpen;
		try { localStorage.setItem('sidebar_open', String(sidebarOpen)); } catch {}
	}

	async function selectAgent(agent: Agent) {
		if (agent.status !== 'available') {
			currentSessionId.set(null);
			enterSetup(agent);
		} else {
			setupAgent.set(null);
			selectedAgent.set(agent);
			await createNewSession(agent.name);
			loadModelsForAgent(agent.name);
		}
	}

	async function handlePaneStartSession(agentName: string) {
		const agent = $agents.find(a => a.name === agentName);
		if (!agent || agent.status !== 'available') return;
		setupAgent.set(null);
		selectedAgent.set(agent);
		await createNewSession(agent.name);
		loadModelsForAgent(agent.name);
	}

	async function handleNewChat() {
		if ($selectedAgent) {
			if ($selectedAgent.status !== 'available') {
				currentSessionId.set(null);
				enterSetup($selectedAgent);
			} else {
				setupAgent.set(null);
				await createNewSession($selectedAgent.name);
				loadModelsForAgent($selectedAgent.name);
			}
		}
	}

	async function handleSend() {
		if (!canSend || !$currentSessionId) return;
		const text = inputText.trim();
		historyIndex = null;
		draftBeforeHistory = '';
		lastEscapeAt = 0;
		inputText = '';
		// Always scroll to bottom when user sends a message
		userNearBottom = true;
		await tick();
		resizeTextarea();
		const bSent = await sendMessage($currentSessionId, text);
		if (!bSent) {
			inputText = text;
			await tick();
			resizeTextarea();
		}
	}

	function handleCancel() {
		if ($currentSessionId) {
			cancelCurrentPrompt($currentSessionId);
		}
	}

	function clearComposerPopups() {
		contextPopupVisible = false;
		mentionStartPos = -1;
		commandPopupVisible = false;
		commandStartPos = -1;
	}

	function placeCaret(position: 'start' | 'end') {
		if (!textareaEl) return;
		const offset = position === 'start' ? 0 : inputText.length;
		textareaEl.selectionStart = offset;
		textareaEl.selectionEnd = offset;
		textareaEl.focus();
	}

	function updateComposerText(text: string, caretPosition: 'start' | 'end') {
		inputText = text;
		clearComposerPopups();
		tick().then(() => {
			placeCaret(caretPosition);
			resizeTextarea();
			detectAtMention();
		});
	}

	function isCaretAtStart(): boolean {
		if (!textareaEl) return inputText.length === 0;
		const start = textareaEl.selectionStart ?? 0;
		const end = textareaEl.selectionEnd ?? 0;
		return start === 0 && end === 0;
	}

	function isCaretAtEnd(): boolean {
		if (!textareaEl) return true;
		const start = textareaEl.selectionStart ?? inputText.length;
		const end = textareaEl.selectionEnd ?? inputText.length;
		return start === inputText.length && end === inputText.length;
	}

	function navigateHistory(direction: -1 | 1) {
		const entries = getComposerHistoryEntries();
		if (entries.length === 0) return;

		if (direction === -1) {
			if (historyIndex === null) {
				draftBeforeHistory = inputText;
				const nextIndex = entries.length - 1;
				historyIndex = nextIndex;
				updateComposerText(entries[nextIndex].text, 'start');
				return;
			}

			if (historyIndex === 0) return;
			const nextIndex = historyIndex - 1;
			historyIndex = nextIndex;
			updateComposerText(entries[nextIndex].text, 'start');
			return;
		}

		if (historyIndex === null) return;

		if (historyIndex >= entries.length - 1) {
			const draft = draftBeforeHistory;
			historyIndex = null;
			draftBeforeHistory = '';
			updateComposerText(draft, 'end');
			return;
		}

		const nextIndex = historyIndex + 1;
		historyIndex = nextIndex;
		updateComposerText(entries[nextIndex].text, 'start');
	}

	function handleDoubleEscape(): boolean {
		const now = Date.now();
		if (now - lastEscapeAt > DOUBLE_ESCAPE_WINDOW_MS) {
			lastEscapeAt = now;
			return true;
		}

		lastEscapeAt = 0;
		if (inputText.trim()) {
			addDraftToHistory(inputText);
		}
		historyIndex = null;
		draftBeforeHistory = '';
		updateComposerText('', 'start');
		return true;
	}

	function isImeCompositionEvent(e: KeyboardEvent): boolean {
		// Some embedded webviews report IME keys as "Process" / keyCode 229.
		return isTextComposing || e.isComposing || e.key === 'Process' || e.keyCode === 229;
	}

	function handleKeydown(e: KeyboardEvent) {
		if (isImeCompositionEvent(e)) {
			return;
		}

		// Prevent non-modifier typing keys from bubbling to Unreal editor hotkeys
		// (e.g. Shift+2 => Landscape mode, P => viewport toggles) while typing in chat.
		if (!e.metaKey && !e.ctrlKey && !e.altKey) {
			e.stopPropagation();
		}

		// Let active popups handle navigation keys first
		if (contextPopupVisible && contextPopupRef?.handleKeydown(e)) {
			return;
		}
		if (commandPopupVisible && commandPopupRef?.handleKeydown(e)) {
			return;
		}
		if (e.key === 'Escape') {
			e.preventDefault();
			handleDoubleEscape();
			return;
		}
		lastEscapeAt = 0;
		if (!e.metaKey && !e.ctrlKey && !e.altKey && !e.shiftKey) {
			if (e.key === 'ArrowUp') {
				e.preventDefault();
				if (isCaretAtStart()) {
					navigateHistory(-1);
				} else {
					placeCaret('start');
				}
				return;
			}
			if (e.key === 'ArrowDown') {
				e.preventDefault();
				if (isCaretAtEnd()) {
					navigateHistory(1);
				} else {
					placeCaret('end');
				}
				return;
			}
		}
		if (e.key === 'Enter' && !e.shiftKey) {
			e.preventDefault();
			handleSend();
		}
	}

	function handleKeyup(e: KeyboardEvent) {
		if (isImeCompositionEvent(e)) {
			return;
		}

		// Match keydown behavior so key-up events don't leak to editor hotkeys.
		if (!e.metaKey && !e.ctrlKey && !e.altKey) {
			e.stopPropagation();
		}
	}

	function handleCompositionStart() {
		isTextComposing = true;
	}

	function handleCompositionEnd() {
		isTextComposing = false;
		detectAtMention();
	}

	function handleInput() {
		lastEscapeAt = 0;
		if (historyIndex !== null) {
			historyIndex = null;
			draftBeforeHistory = '';
		}
		resizeTextarea();
		detectAtMention();
	}

	function detectAtMention() {
		if (!textareaEl) return;

		const cursorPos = textareaEl.selectionStart;
		const text = inputText;

		// Detect / command at start of input
		if (text.startsWith('/') && $availableCommands.length > 0) {
			// Only trigger if cursor is still in the first "word" (no spaces yet)
			const firstSpace = text.indexOf(' ');
			if (firstSpace === -1 || cursorPos <= firstSpace) {
				commandStartPos = 0;
				commandQuery = text.slice(1, cursorPos);
				commandPopupVisible = true;
				contextPopupVisible = false;
				return;
			}
		}
		commandPopupVisible = false;
		commandStartPos = -1;

		// Search backwards from cursor for an unmatched @
		let atPos = -1;
		for (let i = cursorPos - 1; i >= 0; i--) {
			const ch = text[i];
			if (ch === '@') {
				// Check if this @ is at start of text or preceded by whitespace
				if (i === 0 || /\s/.test(text[i - 1])) {
					atPos = i;
				}
				break;
			}
			// Stop searching if we hit whitespace (means no @ in this "word")
			if (/\s/.test(ch)) break;
		}

		if (atPos >= 0) {
			mentionStartPos = atPos;
			contextQuery = text.slice(atPos + 1, cursorPos);
			contextPopupVisible = true;
		} else {
			contextPopupVisible = false;
			mentionStartPos = -1;
		}
	}

	function handleContextSelect(item: import('$lib/bridge.js').ContextItem) {
		if (!textareaEl || mentionStartPos < 0) return;

		const cursorPos = textareaEl.selectionStart;
		const before = inputText.slice(0, mentionStartPos);
		const after = inputText.slice(cursorPos);
		const mention = `@${item.path} `;

		inputText = before + mention + after;
		contextPopupVisible = false;
		mentionStartPos = -1;

		// Restore cursor position after the inserted mention
		tick().then(() => {
			if (textareaEl) {
				const newPos = before.length + mention.length;
				textareaEl.selectionStart = newPos;
				textareaEl.selectionEnd = newPos;
				textareaEl.focus();
				resizeTextarea();
			}
		});
	}

	function handleContextDismiss() {
		contextPopupVisible = false;
		mentionStartPos = -1;
	}

	function handleCommandSelect(cmd: SlashCommand) {
		if (!textareaEl) return;

		const replacement = `/${cmd.name}${cmd.inputHint ? ' ' : ''}`;
		inputText = replacement;
		commandPopupVisible = false;
		commandStartPos = -1;

		tick().then(() => {
			if (textareaEl) {
				const pos = replacement.length;
				textareaEl.selectionStart = pos;
				textareaEl.selectionEnd = pos;
				textareaEl.focus();
				resizeTextarea();
			}
		});
	}

	function handleCommandDismiss() {
		commandPopupVisible = false;
		commandStartPos = -1;
	}

	function handleScroll() {
		if (!messageContainer) return;
		const { scrollTop, scrollHeight, clientHeight } = messageContainer;
		userNearBottom = scrollHeight - scrollTop - clientHeight < 80;
	}

	// Custom right-click context menu (CEF doesn't provide native copy/paste on Windows,
	// and on Mac the native menu should be suppressed via OnSuppressContextMenu in C++)
	let ctxMenuVisible = $state(false);
	let ctxMenuX = $state(0);
	let ctxMenuY = $state(0);
	let ctxMenuHasSelection = $state(false);
	let ctxMenuIsInput = $state(false);

	const CTX_MENU_WIDTH = 200;
	const CTX_MENU_HEIGHT = 160;
	function clampMenuPosition(x: number, y: number): [number, number] {
		const vw = window.innerWidth;
		const vh = window.innerHeight;
		return [
			Math.min(x, vw - CTX_MENU_WIDTH),
			Math.min(y, vh - CTX_MENU_HEIGHT)
		];
	}

	function handleChatContextMenu(e: MouseEvent) {
		e.preventDefault();
		e.stopPropagation();
		ctxMenuHasSelection = !!(window.getSelection()?.toString());
		ctxMenuIsInput = false;
		[ctxMenuX, ctxMenuY] = clampMenuPosition(e.clientX, e.clientY);
		ctxMenuVisible = true;
	}

	function handleInputContextMenu(e: MouseEvent) {
		e.preventDefault();
		e.stopPropagation();
		if (!textareaEl) return;
		const start = textareaEl.selectionStart ?? 0;
		const end = textareaEl.selectionEnd ?? 0;
		ctxMenuHasSelection = start !== end;
		ctxMenuIsInput = true;
		[ctxMenuX, ctxMenuY] = clampMenuPosition(e.clientX, e.clientY);
		ctxMenuVisible = true;
	}

	function handleCtxCopy() {
		if (ctxMenuIsInput && textareaEl) {
			const start = textareaEl.selectionStart ?? 0;
			const end = textareaEl.selectionEnd ?? 0;
			const selection = textareaEl.value.substring(start, end);
			if (selection) copyToClipboard(selection);
		} else {
			const selection = window.getSelection()?.toString() ?? '';
			if (selection) copyToClipboard(selection);
		}
		ctxMenuVisible = false;
	}

	function handleCtxCut() {
		if (!ctxMenuIsInput || !textareaEl) return;
		const start = textareaEl.selectionStart ?? 0;
		const end = textareaEl.selectionEnd ?? 0;
		const selection = textareaEl.value.substring(start, end);
		if (selection) {
			copyToClipboard(selection);
			textareaEl.setRangeText('', start, end, 'end');
			textareaEl.dispatchEvent(new Event('input', { bubbles: true }));
		}
		ctxMenuVisible = false;
	}

	async function handleCtxPaste() {
		if (!textareaEl) return;
		const text = await getClipboardText();
		if (text) {
			const start = textareaEl.selectionStart ?? 0;
			const end = textareaEl.selectionEnd ?? 0;
			textareaEl.setRangeText(text, start, end, 'end');
			textareaEl.dispatchEvent(new Event('input', { bubbles: true }));
		}
		ctxMenuVisible = false;
	}

	function handleCtxSelectAll() {
		if (ctxMenuIsInput && textareaEl) {
			textareaEl.select();
		} else if (messageContainer) {
			const range = document.createRange();
			range.selectNodeContents(messageContainer);
			const sel = window.getSelection();
			sel?.removeAllRanges();
			sel?.addRange(range);
		}
		ctxMenuVisible = false;
	}

	function closeCtxMenu() {
		ctxMenuVisible = false;
	}

	function handlePaste(e: ClipboardEvent) {
		const items = e.clipboardData?.items;
		if (items) {
			for (const item of items) {
				if (item.type.startsWith('image/')) {
					e.preventDefault();
					pasteImage();
					return;
				}
			}
		}
		// Normal text paste — let browser handle it
	}

	function handleDragOver(e: DragEvent) {
		e.preventDefault();
		dragOver = true;
	}

	function handleDragLeave() {
		dragOver = false;
	}

	function handleDrop(e: DragEvent) {
		e.preventDefault();
		dragOver = false;
		const dataTransfer = e.dataTransfer;
		if (!dataTransfer) return;

		const files = dataTransfer.files;
		if (files) {
			for (const file of files) {
				addDroppedFile(file);
			}
		}

		const droppedMentionPaths = extractDroppedMentionPaths(dataTransfer);
		if (droppedMentionPaths.length > 0) {
			insertMentionTags(droppedMentionPaths);
		}
	}

	function resizeTextarea() {
		if (!textareaEl) return;
		textareaEl.style.height = 'auto';
		textareaEl.style.height = Math.min(textareaEl.scrollHeight, 200) + 'px';
	}

	function extractDroppedMentionPaths(dataTransfer: DataTransfer): string[] {
		const found = new Set<string>();

		const addFromText = (text: string) => {
			if (!text) return;
			const matches = text.match(/(?:Source\/[^\s,'"()]+|\/[^\s,'"()]+)/g);
			if (!matches) return;

			for (const match of matches) {
				const normalized = normalizeDroppedMentionPath(match);
				if (normalized) {
					found.add(normalized);
				}
			}
		};

		for (let i = 0; i < dataTransfer.types.length; i++) {
			const type = dataTransfer.types[i];
			const raw = dataTransfer.getData(type);
			addFromText(raw);
		}

		return Array.from(found);
	}

	function normalizeDroppedMentionPath(rawPath: string): string | null {
		let path = rawPath.trim();
		if (!path) return null;

		// Strip wrapping punctuation/quotes often present in asset drag payloads.
		path = path.replace(/^[([{"']+/, '').replace(/[)\]},"';:.]+$/, '');

		const sourceIndex = path.indexOf('Source/');
		if (sourceIndex > 0) {
			path = path.slice(sourceIndex);
		}

		path = path.replace(/\\/g, '/');
		if (path.startsWith('Source/')) {
			return path;
		}

		if (!path.startsWith('/')) {
			return null;
		}

		const firstSegment = path.split('/')[1];
		if (!firstSegment || COMMON_FILESYSTEM_ROOTS.has(firstSegment)) {
			return null;
		}

		return path;
	}

	function insertMentionTags(paths: string[]) {
		if (paths.length === 0) return;

		const mentionText = `${paths.map((path) => `@${path}`).join(' ')} `;
		if (!textareaEl) {
			const needsSpace = inputText.length > 0 && !/\s$/.test(inputText);
			inputText = `${inputText}${needsSpace ? ' ' : ''}${mentionText}`;
			return;
		}

		const start = textareaEl.selectionStart ?? inputText.length;
		const end = textareaEl.selectionEnd ?? start;
		const before = inputText.slice(0, start);
		const after = inputText.slice(end);
		const needsSpace = before.length > 0 && !/\s$/.test(before);
		inputText = `${before}${needsSpace ? ' ' : ''}${mentionText}${after}`;

		tick().then(() => {
			if (!textareaEl) return;
			const cursorPos = before.length + (needsSpace ? 1 : 0) + mentionText.length;
			textareaEl.selectionStart = cursorPos;
			textareaEl.selectionEnd = cursorPos;
			textareaEl.focus();
			resizeTextarea();
		});
	}


</script>

{#if $settingsOpen}
<SettingsPanel />
{:else}
<!-- Studio page — rendered always, hidden when chat is active (preserves state) -->
<div class="flex min-w-0 flex-1" class:hidden={$currentTab !== 'studio'}>
	<StudioPage />
</div>

<!-- Terminal page — rendered always, hidden when not active (preserves PTY session) -->
<div class="flex min-w-0 flex-1" class:hidden={$currentTab !== 'terminal'}>
	<TerminalPane />
</div>

<!-- Chat view — rendered always, hidden when not active (preserves state) -->
<div class="flex min-w-0 flex-1" class:hidden={$currentTab !== 'chat'}>
<!-- Sidebar with animated width -->
<div
	class="shrink-0 overflow-hidden {sidebarMounted ? 'transition-all duration-250 ease-out' : ''}"
	style="width: {sidebarOpen ? '280px' : '0px'};"
>
	<div class="h-full w-[280px]">
		<Sidebar />
	</div>
</div>

<!-- Main chat area -->
	<main class="flex min-w-0 flex-1 flex-col">
		<!-- Top header bar — matches UE toolbar style -->
		<header class="flex h-10 shrink-0 items-center justify-between border-b border-border bg-[#222] px-3">
			<div class="flex min-w-0 flex-1 items-center gap-2 text-[13px]">
				<!-- Sidebar toggle -->
				<button
					class="shrink-0 rounded p-1 text-muted-foreground transition-colors hover:bg-accent hover:text-foreground"
					onclick={toggleSidebar}
					title={sidebarOpen ? $t('hide_sidebar') : $t('show_sidebar')}
				>
					<Icon icon={SidebarLeftIcon} size={16} strokeWidth={1.5} />
				</button>
				<div class="min-w-0 flex flex-1 items-center gap-2">
					<span class="min-w-0 flex-1 truncate font-medium text-foreground" title={headerTitle}>
						{headerTitle}
					</span>
					{#if headerAgent}
						<span class="max-w-[120px] truncate text-muted-foreground" title={headerAgent}>
							{headerAgent}
						</span>
					{/if}
				</div>
				{#if paneManager.isMultiPane}
					<button
						class="shrink-0 rounded p-0.5 text-muted-foreground hover:text-foreground"
						onclick={handleUnsplit}
						title="Close split panes"
					>
						<Icon icon={Cancel01Icon} size={14} strokeWidth={1.5} />
					</button>
				{/if}
				{#if paneManager.canSplit && $currentSessionId}
					<button
						class="shrink-0 rounded p-0.5 text-muted-foreground hover:text-foreground"
						onclick={handleSplitPane}
						title="Split pane"
					>
						<Icon icon={LayoutLeftIcon} size={16} strokeWidth={1.5} />
					</button>
				{/if}
				<button class="shrink-0 rounded p-0.5 text-muted-foreground hover:text-foreground">
					<Icon icon={MoreHorizontalIcon} size={16} strokeWidth={1.5} />
				</button>
			</div>
			<div class="ml-2 flex shrink-0 items-center gap-2">
				<UsageBar />
				{#if connectionInfo}
					<span
					class="flex items-center gap-1.5 rounded-md border border-border/80 bg-secondary/30 px-2 py-1 text-[11px] text-muted-foreground"
				>
					<span class="h-2 w-2 shrink-0 rounded-full {connectionInfo.dotClass} {connectionInfo.pulse ? 'animate-pulse' : ''}"></span>
					{connectionInfo.label}
				</span>
			{/if}
			<!-- New thread button (visible when sidebar is collapsed) -->
			{#if !sidebarOpen}
				<div class="flex items-stretch">
					<button
						class="flex items-center gap-1.5 rounded-l-md border border-border/80 bg-secondary/50 px-2 py-1 text-[12px] text-sidebar-foreground transition-colors hover:bg-secondary"
						title={$t('new_chat_agent', { agentName: $selectedAgent?.shortName ?? '' })}
						onclick={handleNewChat}
					>
						{#if $selectedAgent?.iconUrl}
							<span class="flex h-4 w-4 items-center justify-center shrink-0" style="color: {$selectedAgent.color};">
								<img src={$selectedAgent.iconUrl} alt="" class="h-3.5 w-3.5 invert opacity-70" />
							</span>
						{:else if $selectedAgent}
							<span
								class="flex h-4 w-4 items-center justify-center rounded text-[7px] font-bold text-white"
								style="background-color: {$selectedAgent.color};"
							>
								{$selectedAgent.letter}
							</span>
						{/if}
						<span>{$t('new_short')}</span>
					</button>
					<DropdownMenu.Root>
						<DropdownMenu.Trigger
							class="flex items-center rounded-r-md border border-l-0 border-border/80 bg-secondary/50 px-1 transition-colors hover:bg-secondary"
						>
							<Icon icon={ArrowDown01Icon} size={12} strokeWidth={1.5} class="text-muted-foreground" />
						</DropdownMenu.Trigger>
						<DropdownMenu.Content class="w-[220px]" side="bottom" align="end" sideOffset={4}>
							<DropdownMenu.Label class="text-[11px] text-muted-foreground">{$t('start_with')}</DropdownMenu.Label>
							{#each $agents as agent}
								<DropdownMenu.Item
									class="flex items-center gap-2.5 px-2 py-1.5"
									onclick={() => selectAgent(agent)}
								>
									{#if agent.iconUrl}
										<span class="flex h-5 w-5 items-center justify-center shrink-0" style="color: {agent.color};">
											<img src={agent.iconUrl} alt="" class="h-4 w-4 invert opacity-70" />
										</span>
									{:else}
										<span
											class="flex h-5 w-5 items-center justify-center rounded text-[8px] font-bold text-white"
											style="background-color: {agent.color};"
										>
											{agent.letter}
										</span>
									{/if}
									<div class="flex-1 min-w-0">
										<div class="flex items-baseline gap-1.5">
											<span class="text-[13px] truncate">{agent.name}</span>
											{#if agent.provider}
												<span class="text-[11px] text-muted-foreground/50 shrink-0">{agent.provider}</span>
											{/if}
										</div>
										{#if agent.status === 'not_installed'}
											<div class="text-[11px] text-amber-400/60 truncate">{$t('setup_click')}</div>
										{:else if agent.status === 'missing_key'}
											<div class="text-[11px] text-amber-400/60 truncate">{$t('api_key_needed')}</div>
										{:else if agent.description}
											<div class="text-[11px] text-muted-foreground/40 truncate">{agent.description}</div>
										{/if}
									</div>
									<span class="h-2 w-2 shrink-0 rounded-full {statusDotColor(agent.status)}"></span>
								</DropdownMenu.Item>
								{#if agent.id === 'OpenRouter'}
									<DropdownMenu.Separator />
								{/if}
							{/each}
						</DropdownMenu.Content>
					</DropdownMenu.Root>
				</div>
			{/if}
		</div>
	</header>

	{#if $currentSessionId}
	<!-- Content area — each pane has its own messages + composer -->
	<div class="flex-1 overflow-hidden">
		{#if paneManager.paneCount <= 1}
			<!-- Single pane mode — no PaneForge overhead -->
			<ChatPane sessionId={$currentSessionId} isFocused={true} />
		{:else}
			<!-- Multi-pane mode — PaneForge resizable split -->
			<PaneGroup direction="horizontal" autoSaveId="chat-panes">
				{#each paneManager.panes as pane, idx (pane.paneId)}
					<Pane
						defaultSize={Math.round(100 / paneManager.paneCount)}
						minSize={20}
						order={idx + 1}
					>
						<!-- svelte-ignore a11y_click_events_have_key_events -->
						<!-- svelte-ignore a11y_no_static_element_interactions -->
						<div
							class="h-full {paneManager.focusedIndex === idx ? 'ring-1 ring-inset ring-[var(--ue-accent)]/20' : ''}"
							onclick={() => handlePaneFocus(idx)}
							onfocusin={() => handlePaneFocus(idx)}
							role="region"
							tabindex="-1"
						>
							<ChatPane
								sessionId={pane.sessionId}
								isFocused={paneManager.focusedIndex === idx}
								showPaneHeader={true}
								onStartSession={handlePaneStartSession}
							/>
						</div>
					</Pane>
					{#if idx < paneManager.paneCount - 1}
						<PaneResizer>
							<div class="w-1 h-full bg-border/40 hover:bg-[var(--ue-accent)]/40 transition-colors cursor-col-resize"></div>
						</PaneResizer>
					{/if}
				{/each}
			</PaneGroup>
		{/if}
	</div>
	{:else if !$onboardingLoading && $showOnboarding}
	<!-- First-launch onboarding wizard -->
	<OnboardingWizard />

	{:else if $setupAgent}
	<!-- Agent setup flow — agent not installed or missing key -->
	<AgentSetup agent={$setupAgent} />

	{:else}
	<!-- Welcome screen — no active session -->
	<div class="flex flex-1 flex-col items-center justify-center gap-8 px-6">
		<div class="flex flex-col items-center gap-3">
			{#if $selectedAgent?.iconUrl}
				<span style="color: {$selectedAgent.color}; opacity: 0.4;">
					<img src={$selectedAgent.iconUrl} alt="" class="h-12 w-12 invert opacity-70" />
				</span>
			{/if}
			<h2 class="text-lg font-light text-foreground/70">{$t('start_new_conversation')}</h2>
			<p class="text-[13px] text-muted-foreground/50">{$t('choose_agent_below')}</p>
		</div>

		<div class="flex flex-wrap justify-center gap-2.5">
			{#each $agents.filter(a => a.status === 'available') as agent}
				<button
					class="flex items-center gap-2.5 rounded-xl border border-border bg-card/40 px-4 py-2.5 text-[13px] text-foreground/80 transition-all hover:border-[var(--ue-accent-muted)] hover:bg-card/70 active:scale-[0.98]"
					onclick={async () => { selectedAgent.set(agent); await createNewSession(agent.name); }}
				>
					{#if agent.iconUrl}
						<span class="flex h-5 w-5 items-center justify-center" style="color: {agent.color};">
							<img src={agent.iconUrl} alt="" class="h-4.5 w-4.5 invert opacity-70" />
						</span>
					{:else}
						<span
							class="flex h-5 w-5 items-center justify-center rounded text-[8px] font-bold text-white"
							style="background-color: {agent.color};"
						>
							{agent.letter}
						</span>
					{/if}
					{agent.shortName}
				</button>
			{/each}
		</div>

		{#if $agents.some(a => a.status !== 'available')}
			<div class="flex flex-wrap justify-center gap-2">
				{#each $agents.filter(a => a.status !== 'available') as agent}
					<button
						class="flex items-center gap-1.5 rounded-lg px-3 py-1.5 text-[12px] text-muted-foreground/40 transition-colors hover:bg-card/30 hover:text-muted-foreground/60 cursor-pointer"
						onclick={() => enterSetup(agent)}
					>
						{#if agent.iconUrl}
							<span style="color: {agent.color}; opacity: 0.35;">
								<img src={agent.iconUrl} alt="" class="h-3.5 w-3.5 invert opacity-70" />
							</span>
						{/if}
						{agent.shortName}
						<span class="text-[10px]">({agent.status === 'not_installed' ? $t('set_up') : agent.status === 'missing_key' ? $t('configure') : $t('unavailable')})</span>
					</button>
				{/each}
			</div>
		{/if}
	</div>
	{/if}

	{#if $modelBrowserOpen}
		<div class="fixed inset-0 z-50 flex items-center justify-center p-4">
			<button
				class="absolute inset-0 bg-black/55 backdrop-blur-[1px]"
				onclick={closeModelBrowser}
				aria-label={$t('close_model_browser')}
			></button>

			<div class="relative z-10 flex h-[min(78vh,720px)] w-full max-w-3xl flex-col overflow-hidden rounded-2xl border border-border bg-card shadow-2xl">
				<div class="flex items-center justify-between border-b border-border px-4 py-3">
					<div>
						<div class="text-[15px] font-medium text-foreground">{$t('browse_openrouter_models')}</div>
						<div class="text-[12px] text-muted-foreground">{$t('search_full_catalog')}</div>
					</div>
					<button
						class="rounded-md px-2 py-1 text-[12px] text-muted-foreground transition-colors hover:bg-accent hover:text-foreground"
						onclick={closeModelBrowser}
					>
						{$t('close')}
					</button>
				</div>

				<div class="border-b border-border px-4 py-3">
					<input
						type="text"
						bind:value={modelSearchQuery}
						placeholder={$t('search_models_placeholder')}
						class="w-full rounded-lg border border-border bg-secondary/40 px-3 py-2 text-[13px] text-foreground placeholder:text-muted-foreground/55 focus:border-[var(--ue-accent-muted)] focus:outline-none"
					/>
				</div>

				<div class="min-h-0 flex-1 overflow-y-auto px-2 py-2">
					{#if $isLoadingAllModels}
						<div class="flex items-center justify-center py-10 text-[13px] text-muted-foreground/70">{$t('loading_models')}</div>
					{:else if filteredAllModels.length === 0}
						<div class="flex items-center justify-center py-10 text-[13px] text-muted-foreground/70">{$t('no_models_match_search')}</div>
					{:else}
						{#each filteredAllModels as model}
							<button
								class="flex w-full items-center gap-3 rounded-lg px-3 py-2 text-left transition-colors hover:bg-accent/60"
								onclick={() => handleSelectModelFromBrowser(model.id)}
							>
								<div class="min-w-0 flex-1">
									<div class="flex items-center gap-2 truncate">
										<span class="text-[13px] text-foreground">{model.name}</span>
										{#if model.providerDisplayName}
											<span class="shrink-0 rounded bg-foreground/5 px-1 py-0.5 text-[9px] uppercase tracking-wider text-muted-foreground/40">{model.providerDisplayName}</span>
										{/if}
									</div>
									<div class="truncate font-mono text-[11px] text-muted-foreground/70">{model.id}</div>
									{#if model.description}
										<div class="truncate text-[11px] text-muted-foreground/55">{model.description}</div>
									{/if}
								</div>
								{#if model.id === $currentModelId}
									<span class="h-2 w-2 shrink-0 rounded-full bg-foreground"></span>
								{/if}
							</button>
						{/each}
					{/if}
				</div>
			</div>
		</div>
	{/if}
</main>
</div><!-- /chat wrapper -->
{/if}
