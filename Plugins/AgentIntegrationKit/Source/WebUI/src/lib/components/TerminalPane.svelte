<script lang="ts">
	import '@xterm/xterm/css/xterm.css';
	import TerminalInstance from '$lib/components/TerminalInstance.svelte';
	import Icon from '$lib/components/Icon.svelte';
	import { Add01Icon, Cancel01Icon } from '@hugeicons/core-free-icons';
	import { pendingTerminalPaste } from '$lib/stores/pendingTerminalPaste.js';
	import { writeTerminal } from '$lib/bridge.js';
	import { createUUID } from '$lib/utils.js';
	import { t } from '$lib/i18n.js';

	/** Matches FTerminalManager::MaxTerminals */
	const MAX_TERMINALS = 8;

	type Tab = { localId: string };

	const initialTabId = createUUID();
	let tabs = $state<Tab[]>([{ localId: initialTabId }]);
	let activeTabId = $state(initialTabId);
	let bridgeByTab = $state<Record<string, string>>({});

	/** Lines to inject when a new tab's PTY gets a bridge id (Continue in terminal). */
	let injectQueue = $state<{ localId: string; line: string; nl: string }[]>([]);

	function setBridge(localId: string, bridgeId: string) {
		bridgeByTab = { ...bridgeByTab, [localId]: bridgeId };
		const match = injectQueue.find((q) => q.localId === localId);
		if (match) {
			injectQueue = injectQueue.filter((q) => q.localId !== localId);
			void writeTerminal(bridgeId, match.line + match.nl);
		}
	}

	function clearBridge(localId: string) {
		if (!bridgeByTab[localId]) return;
		const next = { ...bridgeByTab };
		delete next[localId];
		bridgeByTab = next;
	}

	/** @returns New tab's local id, or null if at the session limit */
	function addTab(): string | null {
		if (tabs.length >= MAX_TERMINALS) return null;
		const id = createUUID();
		tabs = [...tabs, { localId: id }];
		activeTabId = id;
		return id;
	}

	function closeTab(e: MouseEvent, localId: string) {
		e.stopPropagation();
		if (tabs.length <= 1) return;
		injectQueue = injectQueue.filter((q) => q.localId !== localId);
		const idx = tabs.findIndex((x) => x.localId === localId);
		if (idx < 0) return;
		if (activeTabId === localId) {
			const neighbor = tabs[idx === 0 ? 1 : idx - 1];
			activeTabId = neighbor.localId;
		}
		tabs = tabs.filter((x) => x.localId !== localId);
	}

	$effect(() => {
		const p = $pendingTerminalPaste;
		if (!p?.line) return;

		if (p.openInNewTab) {
			pendingTerminalPaste.set(null);
			const newId = addTab();
			const nl = p.newline ?? '\r';
			if (newId) {
				injectQueue = [...injectQueue, { localId: newId, line: p.line, nl }];
				return;
			}
			console.warn(
				'Terminal limit reached; continuing in the active tab instead of opening a new one.'
			);
			const bid = p.targetTerminalId ?? bridgeByTab[activeTabId];
			if (bid) void writeTerminal(bid, p.line + nl);
			return;
		}

		const bid = p.targetTerminalId ?? bridgeByTab[activeTabId];
		if (!bid) return;
		pendingTerminalPaste.set(null);
		void writeTerminal(bid, p.line + (p.newline ?? '\r'));
	});
</script>

<div class="flex h-full w-full flex-col bg-[#1a1a1a]">
	<div
		class="flex h-8 shrink-0 items-center gap-1 border-b border-border/50 bg-[#222] px-2"
	>
		<div class="flex min-w-0 flex-1 items-center gap-0.5 overflow-x-auto">
			{#each tabs as tab, i (tab.localId)}
				<div
					class="flex max-w-[140px] shrink-0 items-center rounded-md transition-colors {tab.localId ===
					activeTabId
						? 'bg-sidebar-accent text-sidebar-foreground'
						: 'text-muted-foreground hover:bg-sidebar-accent/50'}"
				>
					<button
						type="button"
						class="truncate px-2 py-1 text-left text-[11px] font-medium"
						onclick={() => {
							activeTabId = tab.localId;
						}}
					>
						{$t('terminal_tab', { n: i + 1 })}
					</button>
					{#if tabs.length > 1}
						<button
							type="button"
							class="mr-0.5 flex h-5 w-5 shrink-0 items-center justify-center rounded text-muted-foreground hover:bg-destructive/20 hover:text-destructive"
							onclick={(e) => closeTab(e, tab.localId)}
							aria-label={$t('close_terminal_tab')}
						>
							<Icon icon={Cancel01Icon} size={12} strokeWidth={2} />
						</button>
					{/if}
				</div>
			{/each}
		</div>
		<button
			type="button"
			class="flex h-6 w-6 shrink-0 items-center justify-center rounded text-muted-foreground hover:bg-accent hover:text-foreground disabled:cursor-not-allowed disabled:opacity-40"
			disabled={tabs.length >= MAX_TERMINALS}
			onclick={() => {
				addTab();
			}}
			title={$t('new_terminal')}
			aria-label={$t('new_terminal')}
		>
			<Icon icon={Add01Icon} size={16} strokeWidth={1.5} />
		</button>
	</div>

	<div class="relative min-h-0 flex-1">
		{#each tabs as tab (tab.localId)}
			<div
				class="absolute inset-0 flex min-h-0 min-w-0 flex-col"
				class:hidden={tab.localId !== activeTabId}
			>
				<TerminalInstance
					isActiveTab={tab.localId === activeTabId}
					onBridgeId={(id) => setBridge(tab.localId, id)}
					onBridgeLost={() => clearBridge(tab.localId)}
				/>
			</div>
		{/each}
	</div>
</div>
