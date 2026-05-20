<script lang="ts">
	import { onDestroy, onMount } from 'svelte';
	import Icon from '$lib/components/Icon.svelte';
	import {
		Settings02Icon,
		ArrowDown01Icon,
		MessageMultiple01Icon,
		Delete02Icon,
		ReloadIcon
	} from '@hugeicons/core-free-icons';
	import * as DropdownMenu from '$lib/components/ui/dropdown-menu/index.js';
	import * as ContextMenu from '$lib/components/ui/context-menu/index.js';
	import { agents, selectedAgent, statusDotColor, type Agent } from '$lib/stores/agents.js';
	import {
		sessions,
		currentSessionId,
		isLoadingSessions,
		isConnectingAgents,
		groupedSessions,
		createNewSession,
		selectSession,
		removeSession,
		refreshSessions,
		formatTimeAgo
	} from '$lib/stores/sessions.js';
	import { sessionStates, recentlyFinished, getSessionSidebarStatus } from '$lib/stores/agentState.js';
	import { sessionsNeedingAttention } from '$lib/stores/permissions.js';
	import { loadModelsForAgent } from '$lib/stores/models.js';
	import { enterSetup, setupAgent } from '$lib/stores/setup.js';
	import { openSettings } from '$lib/stores/settings.js';
	import {
		buildContinuationDraft,
		getContinuationSummarySettings,
		exportSessionToMarkdown,
		renameSession,
		getSessionTerminalResumeCommand
	} from '$lib/bridge.js';
	import { currentTab } from '$lib/stores/navigation.js';
	import { pendingTerminalPaste } from '$lib/stores/pendingTerminalPaste.js';
	import { PencilEdit01Icon } from '@hugeicons/core-free-icons';
	import { queueComposerDraft } from '$lib/stores/composer.js';
	import { t } from '$lib/i18n.js';

	import { Search01Icon } from '@hugeicons/core-free-icons';

	let searchQuery = $state('');
	let filteredGroupedSessions = $derived.by(() => {
		const q = searchQuery.trim().toLowerCase();
		if (!q) return $groupedSessions;
		return $groupedSessions
			.map(group => ({
				...group,
				sessions: group.sessions.filter(s =>
					(s.title || '').toLowerCase().includes(q) ||
					s.agentName.toLowerCase().includes(q)
				)
			}))
			.filter(group => group.sessions.length > 0);
	});

	let isRefreshing = $state(false);
	let isContinuing = $state(false);
	let continuationSummaryMode = $state<'compact' | 'detailed'>('compact');
	let continuationProvider = $state<'openrouter' | 'local'>('openrouter');
	let continuationModelId = $state('x-ai/grok-4.1-fast');
	let continuationHasOpenRouterKey = $state(false);
	let continuationStatusMessage = $state('');
	let sessionActionStatusMessage = $state('');
	let sessionActionStatusTone = $state<'success' | 'error'>('success');
	let statusMessageTimeout: ReturnType<typeof setTimeout> | null = null;

	function setSessionActionStatus(message: string, tone: 'success' | 'error'): void {
		sessionActionStatusMessage = message;
		sessionActionStatusTone = tone;
		if (statusMessageTimeout) {
			clearTimeout(statusMessageTimeout);
		}
		statusMessageTimeout = setTimeout(() => {
			sessionActionStatusMessage = '';
			statusMessageTimeout = null;
		}, 5000);
	}

	async function loadContinuationSummarySettings() {
		try {
			const summarySettings = await getContinuationSummarySettings();
			continuationSummaryMode = summarySettings.defaultDetail;
			continuationProvider = summarySettings.provider;
			continuationModelId = summarySettings.modelId;
			continuationHasOpenRouterKey = summarySettings.hasOpenRouterKey;
		} catch (e) {
			console.warn('Failed to load continuation summary settings:', e);
		}
	}

	onMount(async () => {
		await loadContinuationSummarySettings();
	});

	onDestroy(() => {
		if (statusMessageTimeout) {
			clearTimeout(statusMessageTimeout);
		}
	});

	async function handleSelectAgent(agent: Agent) {
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

	async function handleNewChat() {
		if ($selectedAgent) {
			if ($selectedAgent.status !== 'available') {
				currentSessionId.set(null);
				enterSetup($selectedAgent);
			} else {
				setupAgent.set(null);
				await createNewSession($selectedAgent.name);
			}
		}
	}

	function getAgentForSession(agentName: string): Agent | undefined {
		return $agents.find(a => a.name === agentName);
	}

	function getContinuationTargets(sourceAgentName: string): Agent[] {
		return $agents.filter(a => a.status === 'available' && a.name !== sourceAgentName);
	}

	async function handleDelete(sessionId: string) {
		await removeSession(sessionId);
	}

	// ── Inline rename ───────────────────────────────────────────────
	let renamingSessionId = $state<string | null>(null);
	let renameValue = $state('');
	let renameInputEl = $state<HTMLInputElement | undefined>();

	function startRename(sessionId: string, currentTitle: string) {
		renamingSessionId = sessionId;
		renameValue = currentTitle;
		// Focus after DOM update
		requestAnimationFrame(() => renameInputEl?.select());
	}

	async function commitRename() {
		if (!renamingSessionId) return;
		const trimmed = renameValue.trim();
		if (trimmed) {
			const result = await renameSession(renamingSessionId, trimmed);
			if (result.success) {
				// Update local session list so UI reflects immediately
				sessions.update(list => list.map(s =>
					s.sessionId === renamingSessionId ? { ...s, title: trimmed, hasCustomTitle: true } : s
				));
			}
		}
		renamingSessionId = null;
		renameValue = '';
	}

	function cancelRename() {
		renamingSessionId = null;
		renameValue = '';
	}

	function handleRenameKeydown(e: KeyboardEvent) {
		if (e.key === 'Enter') { e.preventDefault(); commitRename(); }
		else if (e.key === 'Escape') { e.preventDefault(); cancelRename(); }
	}

	async function handleExport(sessionId: string) {
		try {
			let result = await exportSessionToMarkdown(sessionId);
			let errorText = (result.error || '').toLowerCase();
			const isLoadStateError =
				errorText.includes('not loaded in memory') || errorText.includes('still loading from acp');

			if (!result.success && !result.canceled && isLoadStateError) {
				if ($currentSessionId !== sessionId) {
					await selectSession(sessionId);
				}

				const deadline = Date.now() + 7000;
				while (!result.success && !result.canceled && Date.now() < deadline) {
					await new Promise((resolve) => setTimeout(resolve, 500));
					result = await exportSessionToMarkdown(sessionId);
					errorText = (result.error || '').toLowerCase();
					if (!errorText.includes('not loaded in memory') && !errorText.includes('still loading from acp')) {
						break;
					}
				}
			}

			if (result.success) {
				const pathLabel = result.savedPath ? ` to ${result.savedPath}` : '';
				setSessionActionStatus(`Exported chat${pathLabel}`, 'success');
				return;
			}
			if (result.canceled) {
				return;
			}
			const errorMessage = result.error || 'Failed to export session.';
			setSessionActionStatus(errorMessage, 'error');
			console.warn('Failed to export session:', errorMessage);
		} catch (e) {
			setSessionActionStatus('Failed to export session.', 'error');
			console.warn('Failed to export session:', e);
		}
	}

	async function handleContinueInTerminal(sessionId: string) {
		const result = await getSessionTerminalResumeCommand(sessionId);
		if (!result.supported || !result.command) {
			console.warn('Continue in terminal:', result.error ?? 'Unsupported');
			return;
		}
		currentTab.set('terminal');
		pendingTerminalPaste.set({ line: result.command, openInNewTab: true });
	}

	async function handleContinueSession(sessionId: string, targetAgent: Agent) {
		if (isContinuing) return;
		isContinuing = true;
		continuationStatusMessage = '';

		try {
			// Refresh provider/model display before launching the handoff generation.
			await loadContinuationSummarySettings();
			const providerLabel = continuationProvider === 'openrouter'
				? `OpenRouter AI (${continuationModelId})`
				: 'Local fallback summary';
			continuationStatusMessage = `Generating ${continuationSummaryMode} summary via ${providerLabel}...`;

			const draftResult = await buildContinuationDraft(
				sessionId,
				targetAgent.name,
				continuationSummaryMode
			);

			if (!draftResult.success || !draftResult.draftPrompt) {
				continuationStatusMessage = 'Failed to generate summary.';
				console.warn('Failed to build continuation draft:', draftResult.error ?? 'Unknown error');
				return;
			}

			const providerUsed = draftResult.providerUsed === 'openrouter' ? 'OpenRouter AI' : 'local fallback';
			continuationStatusMessage = `Summary ready (${providerUsed}). Creating new ${targetAgent.shortName} chat...`;

			const newSessionId = await createNewSession(targetAgent.name);
			if (!newSessionId) {
				continuationStatusMessage = 'Failed to create target session.';
				console.warn('Failed to create target session for continuation');
				return;
			}

			selectedAgent.set(targetAgent);
			queueComposerDraft(newSessionId, draftResult.draftPrompt);
			loadModelsForAgent(targetAgent.name);
		} catch (e) {
			continuationStatusMessage = 'Unexpected error while continuing chat.';
			console.warn('Failed to continue session:', e);
		} finally {
			isContinuing = false;
		}
	}

	async function handleRefresh() {
		isRefreshing = true;
		await refreshSessions();
		// Keep the spinner for at least 1s so it's visible
		setTimeout(() => { isRefreshing = false; }, 1000);
	}
</script>

<aside class="flex h-full w-[280px] shrink-0 flex-col border-r border-border bg-sidebar">
	<!-- New chat / Setup agents -->
	<div class="px-3 pt-2.5 pb-1">
		{#if $agents.length === 0}
			<button
				class="flex w-full items-center justify-center gap-2 rounded-lg border border-dashed border-border/80 px-3 py-2.5 text-[13px] text-muted-foreground transition-colors hover:bg-secondary hover:text-foreground"
				onclick={() => { openSettings(); }}
			>
				<span>Set up agents in Settings</span>
			</button>
		{:else}
		<div class="flex items-stretch">
			<button
				class="flex flex-1 items-center gap-2 rounded-l-lg border border-border/80 bg-secondary/50 px-3 py-1.5 text-[13px] text-sidebar-foreground transition-colors hover:bg-secondary"
				onclick={handleNewChat}
			>
				{#if $selectedAgent?.iconUrl}
					<img src={$selectedAgent.iconUrl} alt="" class="h-4 w-4 shrink-0 invert opacity-70" />
				{:else if $selectedAgent}
					<span
						class="flex h-5 w-5 items-center justify-center rounded text-[9px] font-bold text-white"
						style="background-color: {$selectedAgent.color};"
					>
						{$selectedAgent.letter}
					</span>
				{/if}
				<span class="truncate">{$selectedAgent ? $selectedAgent.shortName : "New Chat"}</span>
			</button>
			<DropdownMenu.Root>
				<DropdownMenu.Trigger
					class="flex items-center rounded-r-lg border border-l-0 border-border/80 bg-secondary/50 px-1.5 transition-colors hover:bg-secondary"
				>
					<Icon icon={ArrowDown01Icon} size={14} strokeWidth={1.5} class="text-muted-foreground" />
				</DropdownMenu.Trigger>
				<DropdownMenu.Content class="w-[248px]" side="bottom" align="start" sideOffset={4}>
					<DropdownMenu.Label class="text-[11px] text-muted-foreground">{$t('start_with')}</DropdownMenu.Label>
					{#each $agents as agent}
						<DropdownMenu.Item
							class="flex items-center gap-2.5 px-2 py-1.5"
							onclick={() => handleSelectAgent(agent)}
						>
							{#if agent.iconUrl}
								<span class="flex h-6 w-6 items-center justify-center shrink-0" style="color: {agent.color};">
									<img src={agent.iconUrl} alt="" class="h-4.5 w-4.5 invert opacity-70" />
								</span>
							{:else if agent.iconUrl}
								<img src={agent.iconUrl} alt="" class="h-5 w-5 shrink-0 invert opacity-70" />
							{:else}
								<span
									class="flex h-6 w-6 items-center justify-center rounded text-[9px] font-bold text-white"
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
									<div class="text-[11px] text-amber-400/60 truncate">{agent.statusMessage || $t('setup_click')}</div>
								{:else if agent.status === 'missing_key'}
									<div class="text-[11px] text-amber-400/60 truncate">{agent.statusMessage || $t('api_key_needed')}</div>
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
					<DropdownMenu.Separator />
					<DropdownMenu.Item
						class="flex items-center gap-2.5 px-2 py-1.5 text-[13px] text-muted-foreground/60"
						onclick={() => openSettings('agents')}
					>
						<span class="flex h-6 w-6 items-center justify-center shrink-0 text-muted-foreground/40">+</span>
						<span>Browse more agents...</span>
					</DropdownMenu.Item>
				</DropdownMenu.Content>
			</DropdownMenu.Root>
		</div>
		{/if}
	</div>

	<!-- Sessions header -->
	<div class="flex items-center justify-between px-4 pt-3 pb-1">
		<span class="text-[11px] font-medium uppercase tracking-wider text-muted-foreground">{$t('sessions')}</span>
		<button
			class="flex items-center justify-center h-5 w-5 rounded text-muted-foreground/50 transition-colors hover:text-muted-foreground hover:bg-sidebar-accent/60"
			onclick={handleRefresh}
			title={$t('refresh_session_list')}
		>
			<Icon
				icon={ReloadIcon}
				size={12}
				strokeWidth={1.5}
				class={isRefreshing || $isConnectingAgents ? 'animate-spin' : ''}
			/>
		</button>
	</div>
	{#if $sessions.length > 0}
		<div class="px-3 pb-1">
			<div class="relative">
				<span class="absolute left-2 top-1/2 -translate-y-1/2 text-muted-foreground/40">
					<Icon icon={Search01Icon} size={12} strokeWidth={1.5} />
				</span>
				<input
					type="text"
					bind:value={searchQuery}
					placeholder={$t('search_sessions_placeholder')}
					spellcheck="false"
					class="w-full rounded-md border border-border/50 bg-secondary/30 pl-7 pr-2.5 py-1 text-[12px] text-foreground placeholder:text-muted-foreground/40 focus:border-[var(--ue-accent-muted)] focus:outline-none"
				/>
			</div>
		</div>
	{/if}
	{#if sessionActionStatusMessage}
		<div class="px-4 pb-1 text-[11px] {sessionActionStatusTone === 'success' ? 'text-emerald-400/80' : 'text-red-400/85'}">
			{sessionActionStatusMessage}
		</div>
	{/if}

	<!-- Session list (scrollable, grouped by date) -->
	<div class="min-h-0 flex-1 overflow-y-auto px-2 pb-2">
		{#if $isLoadingSessions}
			<div class="flex flex-col gap-0.5 pt-0.5">
				{#each Array(4) as _}
					<div class="flex items-center gap-2 rounded-md px-2 py-1.5 animate-pulse">
						<div class="h-4 w-4 shrink-0 rounded bg-muted-foreground/10"></div>
						<div class="flex-1 h-3.5 rounded bg-muted-foreground/10"></div>
						<div class="h-3 w-6 shrink-0 rounded bg-muted-foreground/10"></div>
					</div>
				{/each}
			</div>
		{:else if $sessions.length === 0 && $isConnectingAgents}
			<!-- Agents are connecting, sessions haven't arrived yet -->
			<div class="flex flex-col items-center justify-center gap-2 py-8 text-muted-foreground/50">
				<div class="animate-spin">
					<Icon icon={ReloadIcon} size={20} strokeWidth={1.5} />
				</div>
				<span class="text-[12px]">{$t('loading_sessions')}</span>
			</div>
		{:else if $sessions.length === 0}
			<div class="flex flex-col items-center justify-center gap-3 py-8 text-muted-foreground/50">
				<Icon icon={MessageMultiple01Icon} size={24} strokeWidth={1.5} />
				<span class="text-[12px]">{$t('no_sessions_yet')}</span>
				<button
					class="text-[11px] text-muted-foreground/60 hover:text-muted-foreground transition-colors underline underline-offset-2"
					onclick={handleRefresh}
				>
					{$t('retry_loading')}
				</button>
			</div>
		{:else if searchQuery.trim() && filteredGroupedSessions.length === 0}
			<div class="flex flex-col items-center justify-center gap-2 py-8 text-muted-foreground/50">
				<Icon icon={Search01Icon} size={20} strokeWidth={1.5} />
				<span class="text-[12px]">{$t('no_matching_sessions')}</span>
			</div>
		{:else}
			{#each filteredGroupedSessions as group}
				<!-- Date group header -->
				<div class="px-2 pt-3 pb-1 first:pt-1">
					<span class="text-[11px] font-medium text-muted-foreground/50">{group.label}</span>
				</div>
				<!-- Sessions in group -->
				<div class="flex flex-col gap-0.5">
					{#each group.sessions as session}
						{@const agent = getAgentForSession(session.agentName)}
						{@const sessionStatus = getSessionSidebarStatus(session.sessionId, $sessionStates, $recentlyFinished)}
						<ContextMenu.Root>
							<ContextMenu.Trigger>
								<button
									class="flex w-full items-center gap-2 rounded-md px-2 py-1.5 text-left transition-colors {$currentSessionId === session.sessionId
										? 'bg-sidebar-accent text-sidebar-foreground'
										: 'text-sidebar-foreground/80 hover:bg-sidebar-accent/60'}"
									onclick={() => { setupAgent.set(null); selectSession(session.sessionId); }}
								>
									{#if agent?.iconUrl}
										<span class="flex h-4 w-4 items-center justify-center shrink-0" style="color: {agent.color};">
											<img src={agent.iconUrl} alt="" class="h-3.5 w-3.5 invert opacity-70" />
										</span>
									{:else if agent?.iconUrl}
										<img src={agent.iconUrl} alt="" class="h-3.5 w-3.5 shrink-0 invert opacity-70" />
									{:else}
										<span
											class="flex h-4 w-4 items-center justify-center rounded text-[7px] font-bold text-white shrink-0"
											style="background-color: {agent?.color ?? '#666'};"
										>
											{agent?.letter ?? '?'}
										</span>
									{/if}
									{#if renamingSessionId === session.sessionId}
									<!-- svelte-ignore a11y_autofocus -->
									<input
										bind:this={renameInputEl}
										bind:value={renameValue}
										class="flex-1 min-w-0 rounded bg-secondary px-1 py-0.5 text-[13px] text-foreground outline-none ring-1 ring-[var(--ue-accent)]/50"
										onkeydown={handleRenameKeydown}
										onblur={commitRename}
										onclick={(e) => e.stopPropagation()}
										autofocus
									/>
								{:else}
									<span class="flex-1 truncate text-[13px]">{session.title || $t('new_chat')}</span>
								{/if}
									<span class="flex items-center gap-1.5 ml-1 shrink-0">
								{#if $sessionsNeedingAttention.has(session.sessionId) && $currentSessionId !== session.sessionId}
									<span class="h-2 w-2 shrink-0 rounded-full bg-amber-500 animate-pulse" title="Needs permission approval"></span>
								{:else if sessionStatus === 'working'}
											<span class="h-2 w-2 shrink-0 rounded-full bg-blue-500 animate-pulse" title={$t('working')}></span>
										{:else if sessionStatus === 'finished'}
											<span class="h-2 w-2 shrink-0 rounded-full bg-emerald-500 transition-opacity" title={$t('finished')}></span>
										{/if}
										<span class="text-[11px] text-muted-foreground">{formatTimeAgo(session.lastModifiedAt)}</span>
									</span>
								</button>
							</ContextMenu.Trigger>
							<ContextMenu.Content class="w-[220px]">
								<ContextMenu.Item
									class="flex items-center gap-2 text-[13px]"
									onclick={() => startRename(session.sessionId, session.title || '')}
								>
									<Icon icon={PencilEdit01Icon} size={14} strokeWidth={1.5} />
									{$t('rename')}
								</ContextMenu.Item>
								<ContextMenu.Separator />
								<ContextMenu.Sub>
									<ContextMenu.SubTrigger class="text-[13px]" disabled={sessionStatus === 'working' || isContinuing}>
										{$t('continue_in')}
									</ContextMenu.SubTrigger>
									<ContextMenu.SubContent class="w-[240px]">
										<ContextMenu.Label class="text-[11px] text-muted-foreground">{$t('target_agent')}</ContextMenu.Label>
										<ContextMenu.Item disabled class="text-[11px] leading-relaxed">
											{#if continuationProvider === 'openrouter'}
												{$t('using_openrouter_ai', { modelId: continuationModelId })}
											{:else}
												{$t('using_local_fallback_summary')}
											{/if}
										</ContextMenu.Item>
										{#if continuationProvider === 'openrouter' && !continuationHasOpenRouterKey}
											<ContextMenu.Item disabled class="text-[11px] text-amber-300">
												{$t('no_openrouter_key_detected')}
											</ContextMenu.Item>
										{/if}
										{#if isContinuing}
											<ContextMenu.Item disabled class="flex items-center gap-2 text-[11px]">
												<Icon icon={ReloadIcon} size={12} strokeWidth={1.5} class="animate-spin" />
												<span class="truncate">{continuationStatusMessage || $t('generating_summary')}</span>
											</ContextMenu.Item>
										{/if}
										<ContextMenu.Separator />
										{@const continuationTargets = getContinuationTargets(session.agentName)}
										{#if continuationTargets.length === 0}
											<ContextMenu.Item disabled class="text-[12px]">{$t('no_available_target_agents')}</ContextMenu.Item>
										{:else}
											{#each continuationTargets as targetAgent}
												<ContextMenu.Item
													class="flex items-center gap-2 text-[13px]"
													disabled={sessionStatus === 'working' || isContinuing}
													closeOnSelect={false}
													onclick={() => handleContinueSession(session.sessionId, targetAgent)}
												>
													{#if targetAgent.iconUrl}
														<span class="flex h-4 w-4 items-center justify-center shrink-0" style="color: {targetAgent.color};">
															<img src={targetAgent.iconUrl} alt="" class="h-3 w-3 invert opacity-70" />
														</span>
													{:else}
														<span
															class="flex h-4 w-4 items-center justify-center rounded text-[7px] font-bold text-white shrink-0"
															style="background-color: {targetAgent.color};"
														>
															{targetAgent.letter}
														</span>
													{/if}
													<span class="truncate">{targetAgent.name}</span>
												</ContextMenu.Item>
											{/each}
										{/if}
										<ContextMenu.Separator />
										<ContextMenu.Label class="text-[11px] text-muted-foreground">{$t('summary_detail')}</ContextMenu.Label>
										<ContextMenu.RadioGroup bind:value={continuationSummaryMode}>
											<ContextMenu.RadioItem value="compact" class="text-[12px]" closeOnSelect={false}>{$t('compact')}</ContextMenu.RadioItem>
											<ContextMenu.RadioItem value="detailed" class="text-[12px]" closeOnSelect={false}>{$t('detailed')}</ContextMenu.RadioItem>
										</ContextMenu.RadioGroup>
									</ContextMenu.SubContent>
								</ContextMenu.Sub>
									<ContextMenu.Item
										class="text-[13px]"
										disabled={!session.terminalResumeSupported}
										onclick={() => handleContinueInTerminal(session.sessionId)}
									>
										{$t('continue_in_terminal')}
									</ContextMenu.Item>
									<ContextMenu.Separator />
									<ContextMenu.Item
										class="text-[13px]"
										disabled={sessionStatus === 'working'}
										onclick={() => handleExport(session.sessionId)}
									>
										{$t('export_as_markdown')}
									</ContextMenu.Item>
									<ContextMenu.Separator />
									<ContextMenu.Item
										class="flex items-center gap-2 text-[13px] text-red-400 data-[highlighted]:text-red-400"
										onclick={() => handleDelete(session.sessionId)}
								>
									<Icon icon={Delete02Icon} size={14} strokeWidth={1.5} />
									{$t('delete')}
								</ContextMenu.Item>
							</ContextMenu.Content>
						</ContextMenu.Root>
					{/each}
				</div>
			{/each}
		{/if}
	</div>

	<!-- Bottom: Discord + Settings -->
	<div class="border-t border-border px-3 py-2 flex flex-col gap-0.5">
		<a
			href="https://discord.gg/betide"
			target="_blank"
			rel="noopener noreferrer"
			class="flex w-full items-center gap-2.5 rounded-md px-2 py-1.5 text-[13px] text-sidebar-foreground transition-colors hover:bg-sidebar-accent"
		>
			<svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor" class="shrink-0">
				<path d="M20.317 4.37a19.791 19.791 0 0 0-4.885-1.515.074.074 0 0 0-.079.037c-.21.375-.444.864-.608 1.25a18.27 18.27 0 0 0-5.487 0 12.64 12.64 0 0 0-.617-1.25.077.077 0 0 0-.079-.037A19.736 19.736 0 0 0 3.677 4.37a.07.07 0 0 0-.032.027C.533 9.046-.32 13.58.099 18.057a.082.082 0 0 0 .031.057 19.9 19.9 0 0 0 5.993 3.03.078.078 0 0 0 .084-.028 14.09 14.09 0 0 0 1.226-1.994.076.076 0 0 0-.041-.106 13.107 13.107 0 0 1-1.872-.892.077.077 0 0 1-.008-.128 10.2 10.2 0 0 0 .372-.292.074.074 0 0 1 .077-.01c3.928 1.793 8.18 1.793 12.062 0a.074.074 0 0 1 .078.01c.12.098.246.198.373.292a.077.077 0 0 1-.006.127 12.299 12.299 0 0 1-1.873.892.077.077 0 0 0-.041.107c.36.698.772 1.362 1.225 1.993a.076.076 0 0 0 .084.028 19.839 19.839 0 0 0 6.002-3.03.077.077 0 0 0 .032-.054c.5-5.177-.838-9.674-3.549-13.66a.061.061 0 0 0-.031-.03zM8.02 15.33c-1.183 0-2.157-1.085-2.157-2.419 0-1.333.956-2.419 2.157-2.419 1.21 0 2.176 1.095 2.157 2.42 0 1.333-.956 2.418-2.157 2.418zm7.975 0c-1.183 0-2.157-1.085-2.157-2.419 0-1.333.955-2.419 2.157-2.419 1.21 0 2.176 1.095 2.157 2.42 0 1.333-.946 2.418-2.157 2.418z"/>
			</svg>
			<span>Discord</span>
		</a>
		<button
			class="flex w-full items-center gap-2.5 rounded-md px-2 py-1.5 text-[13px] text-sidebar-foreground transition-colors hover:bg-sidebar-accent"
			onclick={() => openSettings()}
		>
			<Icon icon={Settings02Icon} size={16} strokeWidth={1.5} />
			<span>{$t('settings')}</span>
		</button>
	</div>
</aside>
