<script lang="ts">
	import { tick, onMount, onDestroy } from 'svelte';
	import { get } from 'svelte/store';
	import ChatMessageComponent from '$lib/components/ChatMessage.svelte';
	import PermissionDialog from '$lib/components/PermissionDialog.svelte';
	import AskUserDialog from '$lib/components/AskUserDialog.svelte';
	import ContextPopup from '$lib/components/ContextPopup.svelte';
	import CommandPopup from '$lib/components/CommandPopup.svelte';
	import PlanPanel from '$lib/components/PlanPanel.svelte';
	import AuthBanner from '$lib/components/AuthBanner.svelte';
	import AttachmentChips from '$lib/components/AttachmentChips.svelte';
	import ChatSearchBar from '$lib/components/ChatSearchBar.svelte';
	import { Shimmer } from '$lib/components/ai-elements/shimmer/index.js';
	import Icon from '$lib/components/Icon.svelte';
	import {
		Add01Icon,
		ArrowUp01Icon,
		ArrowDown01Icon,
		StopIcon,
		GitBranchIcon,
		ChangeScreenModeIcon
	} from '@hugeicons/core-free-icons';
	import * as DropdownMenu from '$lib/components/ui/dropdown-menu/index.js';
	import * as Tooltip from '$lib/components/ui/tooltip/index.js';
	import {
		messagesBySession,
		isStreaming as globalIsStreaming,
		sendMessage,
		cancelCurrentPrompt,
		loadMessages
	} from '$lib/stores/messages.js';
	import { currentSessionId, sessions } from '$lib/stores/sessions.js';
	import { agents, selectedAgent, statusDotColor, type Agent } from '$lib/stores/agents.js';
	import { sessionStates, currentMcpStatus } from '$lib/stores/agentState.js';
	import { permissionQueues, sessionsNeedingAttention } from '$lib/stores/permissions.js';
	import { respondToOption, respondToQuestions, skipQuestions } from '$lib/stores/permissions.js';
	import { availableCommands } from '$lib/stores/commands.js';
	import { currentPlan, hasPlan } from '$lib/stores/plan.js';
	import {
		models,
		currentModelId,
		isLoadingModels,
		reasoningLevel,
		changeModel,
		changeReasoningLevel,
		reasoningLabels,
		type ReasoningLevel
	} from '$lib/stores/models.js';
	import { availableModes, currentModeId, changeMode, isInPlanMode } from '$lib/stores/modes.js';
	import { sessionUsage, hasUsage, contextPercent, formatTokens, formatCost } from '$lib/stores/usage.js';
	import { scEnabled, branchName, hasChanges, changesLabel } from '$lib/stores/sourceControl.js';
	import { openSourceControlChangelist, openSourceControlSubmit } from '$lib/bridge.js';
	import { hasAttachments, pasteImage, pickAttachments, addDroppedFile } from '$lib/stores/attachments.js';
	import { addDraftToHistory, getComposerHistoryEntries } from '$lib/stores/composerHistory.js';
	import { enterToSend } from '$lib/stores/settings.js';
	import { copyToClipboard, getClipboardText, type SlashCommand, type PermissionRequest } from '$lib/bridge.js';
	import { t } from '$lib/i18n.js';

	interface Props {
		sessionId: string | null;
		isFocused: boolean;
		showPaneHeader?: boolean;
		onStartSession?: (agentName: string) => void;
	}

	let { sessionId, isFocused, showPaneHeader = false, onStartSession }: Props = $props();

	// ── Per-pane state ──────────────────────────────────────────────────
	let scrollContainer: HTMLDivElement | undefined = $state();
	let textareaEl: HTMLTextAreaElement | undefined = $state();
	let inputText = $state('');
	let userNearBottom = $state(true);
	let isTextComposing = $state(false);
	let historyIndex = $state<number | null>(null);
	let draftBeforeHistory = $state('');
	let lastEscapeAt = $state(0);
	let dragOver = $state(false);

	// Popup state
	let contextPopupVisible = $state(false);
	let contextQuery = $state('');
	let contextPopupRef: ContextPopup | undefined = $state();
	let mentionStartPos = $state(-1);
	let commandPopupVisible = $state(false);
	let commandQuery = $state('');
	let commandPopupRef: CommandPopup | undefined = $state();
	let commandStartPos = $state(-1);

	// In-chat search
	let chatSearchVisible = $state(false);

	function handleGlobalKeydown(e: KeyboardEvent) {
		if ((e.metaKey || e.ctrlKey) && e.key === 'f') {
			e.preventDefault();
			chatSearchVisible = true;
		}
	}

	onMount(() => {
		window.addEventListener('keydown', handleGlobalKeydown);
	});

	onDestroy(() => {
		window.removeEventListener('keydown', handleGlobalKeydown);
	});

	// Context menu
	let ctxMenuVisible = $state(false);
	let ctxMenuX = $state(0);
	let ctxMenuY = $state(0);
	let ctxMenuHasSelection = $state(false);
	let ctxMenuIsInput = $state(false);

	const DOUBLE_ESCAPE_WINDOW_MS = 900;
	const COMMON_FILESYSTEM_ROOTS = new Set([
		'Applications', 'Users', 'private', 'tmp', 'var', 'etc', 'System',
		'Volumes', 'home', 'opt', 'usr', 'bin', 'sbin', 'dev'
	]);

	// ── Derived state ───────────────────────────────────────────────────
	let paneMessages = $derived(sessionId ? ($messagesBySession[sessionId] ?? []) : []);
	let paneIsStreaming = $derived(sessionId === $currentSessionId ? $globalIsStreaming : false);
	let sessionInfo = $derived($sessions.find((s) => s.sessionId === sessionId));
	let sessionAgent = $derived(sessionInfo?.agentName ?? '');
	let paneState = $derived(sessionId ? $sessionStates[sessionId] : undefined);
	let isWorking = $derived(paneState?.state === 'prompting');
	let needsAttention = $derived(sessionId ? $sessionsNeedingAttention.has(sessionId) : false);
	let waitingForMcp = $derived($currentMcpStatus === 'waiting' && isFocused);

	// Permission for THIS pane's session
	let panePermission = $derived.by((): PermissionRequest | null => {
		if (!sessionId) return null;
		const queue = $permissionQueues[sessionId] ?? [];
		return queue[0] ?? null;
	});

	// Commands for this pane
	let paneCommands = $derived<SlashCommand[]>($availableCommands);

	// Plan for this pane (reads from per-session plan store)
	let panePlan = $derived($currentPlan);
	let paneHasPlan = $derived($hasPlan);

	// Model/reasoning for this pane's composer
	let hasModels = $derived($models.length > 0);
	let modelOptions = $derived($models.filter(m => m.id !== 'special:browse_all'));
	let currentModel = $derived($models.find(m => m.id === $currentModelId));
	let modelDisplayName = $derived(currentModel?.name ?? $currentModelId ?? 'Model');
	let groupedModelOptions = $derived.by(() => {
		const groups: { provider: string; models: typeof modelOptions }[] = [];
		const map = new Map<string, typeof modelOptions>();
		for (const m of modelOptions) {
			const key = m.providerDisplayName || m.provider || '';
			if (!map.has(key)) map.set(key, []);
			map.get(key)!.push(m);
		}
		// Custom/non-OpenRouter providers first, then OpenRouter
		for (const [provider, models] of map) {
			if (provider.toLowerCase() !== 'openrouter') {
				groups.push({ provider, models });
			}
		}
		const orModels = map.get('OpenRouter') ?? map.get('openrouter');
		if (orModels) groups.push({ provider: 'OpenRouter', models: orModels });
		// If only one group or no provider names, return flat
		return groups;
	});
	let currentModelSupportsReasoning = $derived(currentModel?.supportsReasoning ?? false);
	const reasoningLevels: ReasoningLevel[] = ['none', 'low', 'medium', 'high', 'max'];

	// Mode selector
	let hasModes = $derived($availableModes.length > 0);
	let currentMode = $derived($availableModes.find(m => m.id === $currentModeId));
	let modeDisplayName = $derived(currentMode?.name ?? $currentModeId ?? 'Default');
	let isPlanMode = $derived(isInPlanMode($currentModeId));

	// Usage display
	let usageHasContext = $derived($sessionUsage.contextSize > 0);
	let usageHasTokens = $derived($sessionUsage.inputTokens > 0 || $sessionUsage.outputTokens > 0);
	let usageHasCost = $derived($sessionUsage.costAmount > 0);
	let usageContextLabel = $derived.by(() => {
		if (usageHasContext) return `${$contextPercent}%`;
		if (usageHasTokens) return `${formatTokens($sessionUsage.inputTokens + $sessionUsage.outputTokens)}`;
		return '';
	});

	let agentReady = $derived.by(() => {
		if (waitingForMcp) return false;
		const state = paneState?.state;
		if (state === 'ready' || state === 'in_session' || state === 'prompting') return true;
		if (!state && sessionId) return true;
		return false;
	});

	let canSend = $derived(inputText.trim().length > 0 && !!sessionId && !paneIsStreaming && agentReady);

	let inputPlaceholder = $derived(
		waitingForMcp ? $t('connecting_tools')
			: !agentReady ? $t('waiting_for_agent')
			: paneMessages.length > 0 ? $t('ask_follow_up')
			: $t('describe_task')
	);

	// ── Message windowing ───────────────────────────────────────────────
	// Only render the last N messages to keep the DOM light for long chats.
	// User can scroll up to load more (or click "Show earlier messages").
	const INITIAL_WINDOW = 40;
	const WINDOW_INCREMENT = 30;
	let messageWindow = $state(INITIAL_WINDOW);

	// Reset window and scroll state when session actually changes (not every render)
	let prevWindowSessionId: string | null = null;
	$effect(() => {
		if (sessionId !== prevWindowSessionId) {
			prevWindowSessionId = sessionId;
			messageWindow = INITIAL_WINDOW;
			// Ensure we scroll to bottom when switching to a new/different session
			userNearBottom = true;
		}
	});

	let visibleMessages = $derived(
		paneMessages.length <= messageWindow
			? paneMessages
			: paneMessages.slice(-messageWindow)
	);
	let hasHiddenMessages = $derived(paneMessages.length > messageWindow);

	function loadEarlierMessages() {
		messageWindow += WINDOW_INCREMENT;
	}

	// ── Auto-scroll (RAF-throttled) ─────────────────────────────────────
	let scrollRafId: number | null = null;

	$effect(() => {
		const _msgs = paneMessages;
		const _perm = panePermission;
		if (userNearBottom && scrollContainer && scrollRafId === null) {
			scrollRafId = requestAnimationFrame(() => {
				scrollRafId = null;
				if (scrollContainer) {
					scrollContainer.scrollTop = scrollContainer.scrollHeight;
				}
			});
		}
	});

	// Load messages when sessionId changes, then scroll to bottom
	$effect(() => {
		if (sessionId) {
			loadMessages(sessionId).then(() => {
				// After history loads, force scroll to bottom on next frame.
				// The auto-scroll $effect may have missed it if onscroll fired
				// during DOM insertion and set userNearBottom = false.
				userNearBottom = true;
				tick().then(() => {
					if (scrollContainer) {
						scrollContainer.scrollTop = scrollContainer.scrollHeight;
					}
				});
			});
		}
	});

	// ── Composer functions ───────────────────────────────────────────────
	async function handleSend() {
		if (!canSend || !sessionId) return;
		const text = inputText.trim();
		historyIndex = null;
		draftBeforeHistory = '';
		lastEscapeAt = 0;
		inputText = '';
		// Always scroll to bottom when user sends a message
		userNearBottom = true;
		await tick();
		resizeTextarea();
		const bSent = await sendMessage(sessionId, text);
		if (!bSent) {
			inputText = text;
			await tick();
			resizeTextarea();
		}
	}

	function handleCancel() {
		if (sessionId) {
			cancelCurrentPrompt(sessionId);
		}
	}

	function resizeTextarea() {
		if (!textareaEl) return;
		textareaEl.style.height = 'auto';
		textareaEl.style.height = Math.min(textareaEl.scrollHeight, 200) + 'px';
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
		return (textareaEl.selectionStart ?? 0) === 0 && (textareaEl.selectionEnd ?? 0) === 0;
	}

	function isCaretAtEnd(): boolean {
		if (!textareaEl) return true;
		return (textareaEl.selectionStart ?? inputText.length) === inputText.length &&
			(textareaEl.selectionEnd ?? inputText.length) === inputText.length;
	}

	function navigateHistory(direction: -1 | 1) {
		const entries = getComposerHistoryEntries();
		if (entries.length === 0) return;
		if (direction === -1) {
			if (historyIndex === null) {
				draftBeforeHistory = inputText;
				historyIndex = entries.length - 1;
				updateComposerText(entries[historyIndex].text, 'start');
				return;
			}
			if (historyIndex === 0) return;
			historyIndex = historyIndex - 1;
			updateComposerText(entries[historyIndex].text, 'start');
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
		historyIndex = historyIndex + 1;
		updateComposerText(entries[historyIndex].text, 'start');
	}

	function handleDoubleEscape(): boolean {
		const now = Date.now();
		if (now - lastEscapeAt > DOUBLE_ESCAPE_WINDOW_MS) {
			lastEscapeAt = now;
			return true;
		}
		lastEscapeAt = 0;
		if (inputText.trim()) addDraftToHistory(inputText);
		historyIndex = null;
		draftBeforeHistory = '';
		updateComposerText('', 'start');
		return true;
	}

	function isImeCompositionEvent(e: KeyboardEvent): boolean {
		return isTextComposing || e.isComposing || e.key === 'Process' || e.keyCode === 229;
	}

	function handleKeydown(e: KeyboardEvent) {
		if (isImeCompositionEvent(e)) return;
		if (!e.metaKey && !e.ctrlKey && !e.altKey) e.stopPropagation();
		if (contextPopupVisible && contextPopupRef?.handleKeydown(e)) return;
		if (commandPopupVisible && commandPopupRef?.handleKeydown(e)) return;
		if (e.key === 'Escape') { e.preventDefault(); handleDoubleEscape(); return; }
		lastEscapeAt = 0;
		if (!e.metaKey && !e.ctrlKey && !e.altKey && !e.shiftKey) {
			if (e.key === 'ArrowUp') {
				e.preventDefault();
				if (isCaretAtStart()) navigateHistory(-1); else placeCaret('start');
				return;
			}
			if (e.key === 'ArrowDown') {
				e.preventDefault();
				if (isCaretAtEnd()) navigateHistory(1); else placeCaret('end');
				return;
			}
		}
		if (e.key === 'Enter') {
			if ($enterToSend) {
				// Enter sends, Shift+Enter for newline (default)
				if (!e.shiftKey) { e.preventDefault(); handleSend(); }
			} else {
				// Enter for newline, Ctrl/Cmd+Enter sends
				if (e.metaKey || e.ctrlKey) { e.preventDefault(); handleSend(); }
			}
		}
	}

	function handleKeyup(e: KeyboardEvent) {
		if (isImeCompositionEvent(e)) return;
		if (!e.metaKey && !e.ctrlKey && !e.altKey) e.stopPropagation();
	}

	function handleInput() {
		lastEscapeAt = 0;
		if (historyIndex !== null) { historyIndex = null; draftBeforeHistory = ''; }
		resizeTextarea();
		detectAtMention();
	}

	function detectAtMention() {
		if (!textareaEl) return;
		const cursorPos = textareaEl.selectionStart;
		const text = inputText;

		if (text.startsWith('/') && paneCommands.length > 0) {
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

		// Scan backward for nearest @ (at start or after whitespace). Allow spaces in query
		// so multi-word asset names work (e.g. "@My Character"). Only @ terminates the scan.
		let atPos = -1;
		for (let i = cursorPos - 1; i >= 0; i--) {
			if (text[i] === '@') {
				if (i === 0 || /\s/.test(text[i - 1])) atPos = i;
				break;
			}
		}

		if (atPos >= 0) {
			const q = text.slice(atPos + 1, cursorPos);
			// Dismiss if query contains a space — the mention was completed (reference selected
			// or user typed a space after a path). Multi-word asset names still work because
			// selecting from the popup inserts the full path with trailing space.
			if (q.includes(' ')) {
				contextPopupVisible = false;
				mentionStartPos = -1;
				return;
			}
			mentionStartPos = atPos;
			contextQuery = q;
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

	function handleContextNavigate(folderPath: string) {
		// Drill into folder: replace the text after @ with the folder path + /
		if (!textareaEl || mentionStartPos < 0) return;
		const cursorPos = textareaEl.selectionStart;
		const before = inputText.slice(0, mentionStartPos + 1); // keep the @
		const after = inputText.slice(cursorPos);
		const nav = `${folderPath}/`;
		inputText = before + nav + after;
		// contextQuery will be updated by detectAtMention on next tick
		tick().then(() => {
			if (textareaEl) {
				const newPos = mentionStartPos + 1 + nav.length;
				textareaEl.selectionStart = newPos;
				textareaEl.selectionEnd = newPos;
				textareaEl.focus();
				detectAtMention();
			}
		});
	}

	function handleCommandSelect(cmd: SlashCommand) {
		if (!textareaEl) return;
		const replacement = `/${cmd.name}${cmd.inputHint ? ' ' : ''}`;
		inputText = replacement;
		commandPopupVisible = false;
		commandStartPos = -1;
		tick().then(() => {
			if (textareaEl) {
				textareaEl.selectionStart = replacement.length;
				textareaEl.selectionEnd = replacement.length;
				textareaEl.focus();
				resizeTextarea();
			}
		});
	}

	function handleScroll() {
		if (!scrollContainer) return;
		const { scrollTop, scrollHeight, clientHeight } = scrollContainer;
		userNearBottom = scrollHeight - scrollTop - clientHeight < 80;
	}

	function handlePaste(e: ClipboardEvent) {
		const items = e.clipboardData?.items;
		if (items) {
			for (const item of items) {
				if (item.type.startsWith('image/')) { e.preventDefault(); pasteImage(); return; }
			}
		}
	}

	function handleDragOver(e: DragEvent) { e.preventDefault(); dragOver = true; }
	function handleDragLeave() { dragOver = false; }
	function handleDrop(e: DragEvent) {
		e.preventDefault();
		dragOver = false;
		if (e.dataTransfer?.files) {
			for (const file of e.dataTransfer.files) addDroppedFile(file);
		}
		const paths = extractDroppedMentionPaths(e.dataTransfer!);
		if (paths.length > 0) insertMentionTags(paths);
	}

	function extractDroppedMentionPaths(dataTransfer: DataTransfer): string[] {
		const found = new Set<string>();
		for (let i = 0; i < dataTransfer.types.length; i++) {
			const raw = dataTransfer.getData(dataTransfer.types[i]);
			const matches = raw.match(/(?:Source\/[^\s,'"()]+|\/[^\s,'"()]+)/g);
			if (matches) {
				for (const m of matches) {
					let path = m.trim().replace(/^[([{"']+/, '').replace(/[)\]},"';:.]+$/, '');
					const si = path.indexOf('Source/');
					if (si > 0) path = path.slice(si);
					path = path.replace(/\\/g, '/');
					if (path.startsWith('Source/') || (path.startsWith('/') && !COMMON_FILESYSTEM_ROOTS.has(path.split('/')[1] ?? ''))) {
						found.add(path);
					}
				}
			}
		}
		return Array.from(found);
	}

	function insertMentionTags(paths: string[]) {
		if (paths.length === 0) return;
		const mentionText = `${paths.map((p) => `@${p}`).join(' ')} `;
		if (!textareaEl) {
			inputText += (inputText.length > 0 && !/\s$/.test(inputText) ? ' ' : '') + mentionText;
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

	// Context menu
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
		e.preventDefault(); e.stopPropagation();
		ctxMenuHasSelection = !!(window.getSelection()?.toString());
		ctxMenuIsInput = false;
		[ctxMenuX, ctxMenuY] = clampMenuPosition(e.clientX, e.clientY);
		ctxMenuVisible = true;
	}
	function handleInputContextMenu(e: MouseEvent) {
		e.preventDefault(); e.stopPropagation();
		if (!textareaEl) return;
		ctxMenuHasSelection = (textareaEl.selectionStart ?? 0) !== (textareaEl.selectionEnd ?? 0);
		ctxMenuIsInput = true;
		[ctxMenuX, ctxMenuY] = clampMenuPosition(e.clientX, e.clientY);
		ctxMenuVisible = true;
	}
	function handleCtxCopy() {
		if (ctxMenuIsInput && textareaEl) {
			const sel = textareaEl.value.substring(textareaEl.selectionStart ?? 0, textareaEl.selectionEnd ?? 0);
			if (sel) copyToClipboard(sel);
		} else {
			const sel = window.getSelection()?.toString() ?? '';
			if (sel) copyToClipboard(sel);
		}
		ctxMenuVisible = false;
	}
	function handleCtxCut() {
		if (!ctxMenuIsInput || !textareaEl) return;
		const s = textareaEl.selectionStart ?? 0, e = textareaEl.selectionEnd ?? 0;
		const sel = textareaEl.value.substring(s, e);
		if (sel) { copyToClipboard(sel); textareaEl.setRangeText('', s, e, 'end'); textareaEl.dispatchEvent(new Event('input', { bubbles: true })); }
		ctxMenuVisible = false;
	}
	async function handleCtxPaste() {
		if (!textareaEl) return;
		const text = await getClipboardText();
		if (text) { const s = textareaEl.selectionStart ?? 0, e = textareaEl.selectionEnd ?? 0; textareaEl.setRangeText(text, s, e, 'end'); textareaEl.dispatchEvent(new Event('input', { bubbles: true })); }
		ctxMenuVisible = false;
	}
	function handleCtxSelectAll() {
		if (ctxMenuIsInput && textareaEl) textareaEl.select();
		else if (scrollContainer) { const r = document.createRange(); r.selectNodeContents(scrollContainer); const s = window.getSelection(); s?.removeAllRanges(); s?.addRange(r); }
		ctxMenuVisible = false;
	}

	// Export for parent
	export function getInputText(): string { return inputText; }
	export function setInputText(text: string) { inputText = text; tick().then(() => resizeTextarea()); }
</script>

<div class="flex h-full flex-col overflow-hidden">
	{#if !sessionId}
		<div class="flex h-full flex-col items-center justify-center gap-4 px-6">
			<span class="text-muted-foreground/40 text-[13px]">{$t('start_with')}</span>
			<div class="flex flex-wrap items-center justify-center gap-2">
				{#each $agents.filter(a => a.status === 'available') as agent}
					<button
						class="flex items-center gap-2 rounded-lg border border-border/60 bg-secondary/40 px-3 py-2 text-[12px] text-foreground transition-colors hover:bg-secondary hover:border-border"
						onclick={() => onStartSession?.(agent.name)}
					>
						{#if agent.iconUrl}
							<span class="flex h-4 w-4 items-center justify-center shrink-0" style="color: {agent.color};">
								<img src={agent.iconUrl} alt="" class="h-4 w-4 invert opacity-70" />
							</span>
						{:else}
							<span
								class="flex h-4 w-4 items-center justify-center rounded text-[7px] font-bold text-white shrink-0"
								style="background-color: {agent.color};"
							>{agent.letter}</span>
						{/if}
						<span>{agent.shortName}</span>
					</button>
				{/each}
			</div>
			<span class="text-muted-foreground/25 text-[11px]">or select a session from the sidebar</span>
		</div>
	{:else}
		<!-- Pane header (only in multi-pane mode) -->
		{#if showPaneHeader}
			<div class="flex h-7 shrink-0 items-center justify-between border-b border-border/40 px-3 text-[11px]">
				<div class="flex items-center gap-1.5 min-w-0">
					{#if needsAttention}
						<span class="h-2 w-2 shrink-0 rounded-full bg-amber-500 animate-pulse" title="Needs permission"></span>
					{:else if isWorking}
						<span class="h-2 w-2 shrink-0 rounded-full bg-blue-500 animate-pulse"></span>
					{/if}
					<span class="truncate text-muted-foreground">{sessionInfo?.title || $t('new_chat')}</span>
					<span class="shrink-0 text-muted-foreground/40">{sessionAgent}</span>
				</div>
			</div>
		{/if}

		<!-- Content area with floating composer -->
		<div class="relative flex-1 overflow-hidden">
			{#if chatSearchVisible}
				<ChatSearchBar
					{scrollContainer}
					onclose={() => { chatSearchVisible = false; }}
				/>
			{/if}
			<!-- Messages (scrollable) -->
			<div
				class="chat-scroll-area h-full overflow-y-auto px-6 pt-4 pb-52"
				bind:this={scrollContainer}
				onscroll={handleScroll}
				oncontextmenu={handleChatContextMenu}
			>
				<AuthBanner />
				{#if paneMessages.length === 0 && !paneIsStreaming}
					<div class="flex h-full flex-col items-center justify-center gap-3 text-muted-foreground/40">
						{#if $selectedAgent?.iconUrl}
							<span style="color: {$selectedAgent.color}; opacity: 0.3;">
								<img src={$selectedAgent.iconUrl} alt="" class="h-10 w-10 invert opacity-70" />
							</span>
						{/if}
						<span class="text-[14px]">{$t('send_message_to_get_started')}</span>
					</div>
				{:else}
					{#if hasHiddenMessages}
						<div class="flex justify-center py-2">
							<button
								class="rounded-md border border-border/50 bg-secondary/40 px-3 py-1 text-[11px] text-muted-foreground/60 transition-colors hover:bg-secondary hover:text-muted-foreground"
								onclick={loadEarlierMessages}
							>
								Show {Math.min(WINDOW_INCREMENT, paneMessages.length - messageWindow)} earlier messages
							</button>
						</div>
					{/if}
					{#each visibleMessages as message (message.messageId)}
						<ChatMessageComponent {message} />
					{/each}
				{/if}
				{#if paneIsStreaming && paneMessages[paneMessages.length - 1]?.role !== 'assistant'}
					<div class="mb-4 flex justify-start">
						<div class="flex items-center gap-2 text-[12px]">
							<Shimmer><span>{$t('generating')}</span></Shimmer>
						</div>
					</div>
				{/if}
			</div>

			<!-- Context menu -->
			{#if ctxMenuVisible}
				<!-- svelte-ignore a11y_no_static_element_interactions -->
				<div class="fixed inset-0 z-[100]" onclick={() => ctxMenuVisible = false} oncontextmenu={(e) => { e.preventDefault(); ctxMenuVisible = false; }}></div>
				<div class="fixed z-[101] min-w-[180px] rounded-lg border border-border bg-popover p-1 shadow-lg" style="left: {ctxMenuX}px; top: {ctxMenuY}px;">
					{#if ctxMenuIsInput}
						<button class="flex w-full items-center justify-between rounded-md px-3 py-1.5 text-[13px] text-popover-foreground {ctxMenuHasSelection ? 'hover:bg-accent cursor-default' : 'opacity-40 cursor-not-allowed'}" onclick={handleCtxCut} disabled={!ctxMenuHasSelection}>{$t('cut')}<span class="ml-auto text-[11px] text-muted-foreground/60">⌘X</span></button>
					{/if}
					<button class="flex w-full items-center justify-between rounded-md px-3 py-1.5 text-[13px] text-popover-foreground {ctxMenuHasSelection ? 'hover:bg-accent cursor-default' : 'opacity-40 cursor-not-allowed'}" onclick={handleCtxCopy} disabled={!ctxMenuHasSelection}>{$t('copy')}<span class="ml-auto text-[11px] text-muted-foreground/60">⌘C</span></button>
					{#if ctxMenuIsInput}
						<button class="flex w-full items-center justify-between rounded-md px-3 py-1.5 text-[13px] text-popover-foreground hover:bg-accent cursor-default" onclick={handleCtxPaste}>{$t('paste')}<span class="ml-auto text-[11px] text-muted-foreground/60">⌘V</span></button>
					{/if}
					<div class="my-1 h-px bg-border"></div>
					<button class="flex w-full items-center justify-between rounded-md px-3 py-1.5 text-[13px] text-popover-foreground hover:bg-accent cursor-default" onclick={handleCtxSelectAll}>{$t('select_all')}<span class="ml-auto text-[11px] text-muted-foreground/60">⌘A</span></button>
				</div>
			{/if}

			<!-- Floating composer -->
			<div class="pointer-events-none absolute inset-x-0 bottom-0 z-10 px-6 pb-3 pt-8" style="background: linear-gradient(to bottom, transparent, var(--background) 40%);">
				<div class="pointer-events-auto mx-auto max-w-3xl">
					<!-- svelte-ignore a11y_no_static_element_interactions -->
					<div
						class="relative rounded-2xl border transition-colors focus-within:border-[var(--ue-accent-muted)] {dragOver ? 'border-dashed border-blue-400/60 bg-blue-500/5' : 'border-border bg-card'}"
						style="box-shadow: 0 0 0 1px rgba(0,0,0,0.2), 0 2px 8px rgba(0,0,0,0.15);"
						role="region"
						aria-label="Message input"
						ondragover={handleDragOver}
						ondragleave={handleDragLeave}
						ondrop={handleDrop}
					>
						{#if paneHasPlan && isFocused}
							<PlanPanel />
						{/if}
						{#if panePermission}
							<div class="p-3">
								{#if panePermission.isAskUserQuestion}
									<AskUserDialog request={panePermission} />
								{:else}
									<PermissionDialog request={panePermission} />
								{/if}
							</div>
						{:else}
							<ContextPopup bind:this={contextPopupRef} query={contextQuery} visible={contextPopupVisible} onselect={handleContextSelect} onnavigate={handleContextNavigate} ondismiss={() => { contextPopupVisible = false; mentionStartPos = -1; }} />
							<CommandPopup bind:this={commandPopupRef} query={commandQuery} visible={commandPopupVisible} commands={paneCommands} onselect={handleCommandSelect} ondismiss={() => { commandPopupVisible = false; commandStartPos = -1; }} />
							{#if waitingForMcp}
								<div class="flex items-center gap-2 px-4 pt-3 pb-1">
									<span class="inline-block h-2 w-2 animate-pulse rounded-full bg-amber-500"></span>
									<span class="text-[13px] text-amber-400/80">{$t('connecting_tools')}</span>
								</div>
							{/if}
							<AttachmentChips />
							<textarea
								bind:this={textareaEl}
								bind:value={inputText}
								placeholder={inputPlaceholder}
								disabled={!sessionId || !agentReady}
								rows={1}
								spellcheck="false"
								autocomplete="off"
								autocorrect="off"
								class="w-full resize-none bg-transparent px-4 pt-3.5 pb-3 text-[14px] leading-normal text-foreground placeholder:text-muted-foreground/60 focus:outline-none disabled:opacity-50"
								onkeydown={handleKeydown}
								onkeyup={handleKeyup}
								oncompositionstart={() => isTextComposing = true}
								oncompositionend={() => { isTextComposing = false; detectAtMention(); }}
								oninput={handleInput}
								onpaste={handlePaste}
								oncontextmenu={handleInputContextMenu}
							></textarea>
							<div class="flex items-center justify-between px-3 pb-2.5">
								<div class="flex items-center gap-1.5">
									<button class="rounded-lg p-1.5 text-muted-foreground transition-colors hover:bg-accent hover:text-foreground" onclick={pickAttachments} title={$t('attach_file')}>
										<Icon icon={Add01Icon} size={18} strokeWidth={1.5} />
									</button>
									<!-- Model picker -->
									{#if hasModels || $isLoadingModels}
										<DropdownMenu.Root>
											<DropdownMenu.Trigger
												class="flex items-center gap-1 rounded-lg px-2 py-1 text-[12px] text-muted-foreground transition-colors hover:bg-accent hover:text-foreground"
												disabled={$isLoadingModels}
											>
												{#if $isLoadingModels}
													<span class="inline-block h-3 w-3 animate-spin rounded-full border-2 border-muted-foreground/30 border-t-muted-foreground"></span>
												{:else}
													{modelDisplayName}
													<Icon icon={ArrowDown01Icon} size={10} strokeWidth={1.5} class="opacity-50" />
												{/if}
											</DropdownMenu.Trigger>
											<DropdownMenu.Content class="max-h-[300px] w-[280px] overflow-y-auto" side="top" align="start" sideOffset={4}>
												{#each groupedModelOptions as group, gi}
													{#if gi > 0}
														<DropdownMenu.Separator />
													{/if}
													<DropdownMenu.Label class="text-[11px] text-muted-foreground">{group.provider || $t('model_label')}</DropdownMenu.Label>
													{#each group.models as model}
														<DropdownMenu.Item
															class="flex items-center gap-2 px-2 py-1.5"
															onclick={() => $selectedAgent && changeModel($selectedAgent.name, model.id)}
														>
															<div class="flex-1 min-w-0">
																<div class="text-[13px] truncate">{model.name}</div>
																{#if model.description}
																	<div class="text-[11px] text-muted-foreground/40 truncate">{model.description}</div>
																{/if}
															</div>
															{#if model.id === $currentModelId}
																<span class="h-1.5 w-1.5 shrink-0 rounded-full bg-foreground"></span>
															{/if}
														</DropdownMenu.Item>
													{/each}
												{/each}
											</DropdownMenu.Content>
										</DropdownMenu.Root>
									{/if}
									<!-- Reasoning level picker -->
									{#if hasModels && !$isLoadingModels && currentModelSupportsReasoning}
										<DropdownMenu.Root>
											<DropdownMenu.Trigger
												class="flex items-center gap-1 rounded-lg px-2 py-1 text-[12px] transition-colors {$reasoningLevel !== 'none'
													? 'text-[var(--ue-accent)] hover:text-[var(--ue-accent-hover)]'
													: 'text-muted-foreground hover:bg-accent hover:text-foreground'}"
											>
												{reasoningLabels[$reasoningLevel]}
												<Icon icon={ArrowDown01Icon} size={10} strokeWidth={1.5} class="opacity-50" />
											</DropdownMenu.Trigger>
											<DropdownMenu.Content class="w-[160px]" side="top" align="start" sideOffset={4}>
												<DropdownMenu.Label class="text-[11px] text-muted-foreground">{$t('effort')}</DropdownMenu.Label>
												{#each reasoningLevels as level}
													<DropdownMenu.Item
														class="flex items-center justify-between px-2 py-1.5"
														onclick={() => changeReasoningLevel(level)}
													>
														<span class="text-[13px]">{reasoningLabels[level]}</span>
														{#if level === $reasoningLevel}
															<span class="h-1.5 w-1.5 shrink-0 rounded-full bg-foreground"></span>
														{/if}
													</DropdownMenu.Item>
												{/each}
											</DropdownMenu.Content>
										</DropdownMenu.Root>
									{/if}
								</div>
								<div class="flex items-center gap-1.5">
									{#if paneIsStreaming}
										<button
											class="flex h-8 w-8 items-center justify-center rounded-full bg-red-500/90 text-white transition-all hover:bg-red-500 hover:scale-105 active:scale-95"
											style="box-shadow: 0 0 10px rgba(220,80,40,0.3);"
											onclick={handleCancel}
											title={$t('stop_generating')}
										>
											<Icon icon={StopIcon} size={16} strokeWidth={2.5} />
										</button>
									{:else}
										<button
											class="flex h-8 w-8 items-center justify-center rounded-full transition-all {canSend ? 'text-white hover:scale-105 active:scale-95' : 'bg-muted-foreground/20 text-muted-foreground/40 cursor-not-allowed'}"
											style={canSend ? 'background: var(--ue-accent); box-shadow: 0 0 12px rgba(50,130,230,0.35);' : ''}
											onclick={handleSend}
											disabled={!canSend}
										>
											<Icon icon={ArrowUp01Icon} size={18} strokeWidth={2.5} />
										</button>
									{/if}
								</div>
							</div>
						{/if}
					</div>
					<!-- Bottom status bar -->
					<div class="mt-2.5 flex items-center justify-between px-1 text-[12px] text-muted-foreground/70">
						<div class="flex items-center gap-4">
							{#if hasModes}
								<DropdownMenu.Root>
									<DropdownMenu.Trigger
										class="flex items-center gap-1.5 transition-colors hover:text-foreground {isPlanMode ? 'text-blue-400' : ''}"
									>
										<Icon icon={ChangeScreenModeIcon} size={14} strokeWidth={1.5} />
										{modeDisplayName}
										<Icon icon={ArrowDown01Icon} size={10} strokeWidth={1.5} class="opacity-50" />
									</DropdownMenu.Trigger>
									<DropdownMenu.Content class="w-[220px]" side="top" align="start" sideOffset={4}>
										<DropdownMenu.Label class="text-[11px] text-muted-foreground">{$t('mode_label')}</DropdownMenu.Label>
										{#each $availableModes as mode}
											<DropdownMenu.Item
												class="flex items-center gap-2 px-2 py-1.5"
												onclick={() => $selectedAgent && changeMode($selectedAgent.name, mode.id)}
											>
												<div class="flex-1 min-w-0">
													<div class="text-[13px] truncate">{mode.name}</div>
													{#if mode.description}
														<div class="text-[11px] text-muted-foreground/40 truncate">{mode.description}</div>
													{/if}
												</div>
												{#if mode.id === $currentModeId}
													<span class="h-1.5 w-1.5 shrink-0 rounded-full bg-foreground"></span>
												{/if}
											</DropdownMenu.Item>
										{/each}
									</DropdownMenu.Content>
								</DropdownMenu.Root>
							{/if}
						</div>
						<div class="flex items-center gap-3">
							{#if $hasUsage}
								<Tooltip.Root>
									<Tooltip.Trigger class="flex items-center gap-1.5 transition-colors hover:text-foreground">
										{@const pct = $contextPercent}
										{@const strokeColor = pct < 50 ? '#22c55e' : pct < 80 ? '#eab308' : '#ef4444'}
										{@const circumference = 2 * Math.PI * 7}
										{@const dashOffset = circumference * (1 - pct / 100)}
										<svg width="16" height="16" viewBox="0 0 18 18">
											<circle cx="9" cy="9" r="7" fill="none" stroke="currentColor" stroke-width="2.5" opacity="0.15" />
											<circle cx="9" cy="9" r="7" fill="none" stroke={strokeColor} stroke-width="2.5"
												stroke-dasharray={circumference} stroke-dashoffset={dashOffset}
												stroke-linecap="round" transform="rotate(-90 9 9)" />
										</svg>
										<span class="text-[12px]">{usageContextLabel}</span>
									</Tooltip.Trigger>
									<Tooltip.Content side="top" sideOffset={6} class="w-[260px] !bg-popover !text-popover-foreground p-3 text-[12px]" arrowClasses="!bg-popover">
										<div class="mb-1.5 text-[13px] font-medium">{$t('context_window')}</div>
										<div class="mb-2.5 text-[11px] leading-relaxed text-muted-foreground/50">{$t('context_window_desc')}</div>
										{#if usageHasContext}
											{@const pctVal = $contextPercent}
											{@const remaining = 100 - pctVal}
											<div class="mb-1 text-muted-foreground">{$t('used_left_pct', { used: pctVal, left: remaining })}</div>
											<div class="mb-2 h-1.5 w-full overflow-hidden rounded-full bg-muted-foreground/15">
												<div class="h-full rounded-full transition-all duration-300" style="width: {pctVal}%; background-color: {pctVal < 50 ? '#22c55e' : pctVal < 80 ? '#eab308' : '#ef4444'};"></div>
											</div>
											<div class="text-muted-foreground">{$t('tokens_used', { used: formatTokens($sessionUsage.contextUsed), total: formatTokens($sessionUsage.contextSize) })}</div>
										{:else if usageHasTokens}
											<div class="text-muted-foreground">{formatTokens($sessionUsage.inputTokens)} input, {formatTokens($sessionUsage.outputTokens)} output</div>
										{/if}
										{#if $sessionUsage.cacheReadTokens > 0}
											<div class="mt-1 text-muted-foreground/60">{$t('cached', { count: formatTokens($sessionUsage.cacheReadTokens) })}</div>
										{/if}
										{#if usageHasCost}
											<div class="mt-1.5 border-t border-border/40 pt-1.5 text-muted-foreground">
												{$t('session_cost')}: {formatCost($sessionUsage.costAmount, $sessionUsage.costCurrency)}
												{#if $sessionUsage.turnCostUSD > 0}
													<span class="text-muted-foreground/50">{$t('last_turn_cost', { cost: formatCost($sessionUsage.turnCostUSD) })}</span>
												{/if}
											</div>
										{/if}
										{#if $sessionUsage.numTurns > 0}
											<div class="mt-1 text-muted-foreground/50">
												{$t('turns', { count: $sessionUsage.numTurns })}
												{#if $sessionUsage.durationMs > 0}
													&middot; {($sessionUsage.durationMs / 1000).toFixed(1)}s
												{/if}
											</div>
										{/if}
									</Tooltip.Content>
								</Tooltip.Root>
							{/if}
							{#if $scEnabled}
								<DropdownMenu.Root>
									<DropdownMenu.Trigger class="flex items-center gap-1.5 transition-colors hover:text-foreground">
										<Icon icon={GitBranchIcon} size={14} strokeWidth={1.5} />
										{$branchName || '...'}
										{#if $hasChanges}
											<span class="rounded-full bg-amber-500/20 px-1.5 text-[10px] font-medium text-amber-400">{$changesLabel}</span>
										{/if}
										<Icon icon={ArrowDown01Icon} size={10} strokeWidth={1.5} class="opacity-50" />
									</DropdownMenu.Trigger>
									<DropdownMenu.Content class="w-[200px]" side="top" align="end" sideOffset={4}>
										<DropdownMenu.Label class="text-[11px] text-muted-foreground">{$t('source_control')}</DropdownMenu.Label>
										<DropdownMenu.Item class="flex items-center gap-2 px-2 py-1.5" onclick={() => openSourceControlChangelist()}>
											<span class="text-[13px]">{$t('view_changelists')}</span>
										</DropdownMenu.Item>
										<DropdownMenu.Item class="flex items-center gap-2 px-2 py-1.5" onclick={() => openSourceControlSubmit()}>
											<span class="text-[13px]">{$t('submit_changes')}</span>
										</DropdownMenu.Item>
									</DropdownMenu.Content>
								</DropdownMenu.Root>
							{:else}
								<span class="flex items-center gap-1.5 text-muted-foreground/40">
									<Icon icon={GitBranchIcon} size={14} strokeWidth={1.5} />
									{$t('no_vcs')}
								</span>
							{/if}
						</div>
					</div>
				</div>
			</div>
		</div>
	{/if}
</div>
