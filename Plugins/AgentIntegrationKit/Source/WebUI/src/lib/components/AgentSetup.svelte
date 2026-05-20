<script lang="ts">
	import Icon from '$lib/components/Icon.svelte';
	import {
		Alert02Icon,
		Tick02Icon,
		Cancel01Icon,
		Download04Icon,
		Copy01Icon,
		Settings02Icon,
		ReloadIcon,
		Key01Icon
	} from '@hugeicons/core-free-icons';
	import { copyToClipboard, openUrl, openPluginSettings, restartEditor } from '$lib/bridge.js';
	import { type Agent } from '$lib/stores/agents.js';
	import {
		setupState,
		setupProgress,
		setupError,
		setupInstallInfo,
		startInstall,
		recheckStatus,
		exitSetup
	} from '$lib/stores/setup.js';

	let { agent }: { agent: Agent } = $props();

	let copied = $state(false);
	let copiedError = $state(false);

	function parseColorToRgb(color: string): { r: number; g: number; b: number } | null {
		const value = color.trim().toLowerCase();

		if (value.startsWith('#')) {
			const hex = value.slice(1);
			if (hex.length === 3) {
				const r = parseInt(hex[0] + hex[0], 16);
				const g = parseInt(hex[1] + hex[1], 16);
				const b = parseInt(hex[2] + hex[2], 16);
				return { r, g, b };
			}
			if (hex.length === 6) {
				const r = parseInt(hex.slice(0, 2), 16);
				const g = parseInt(hex.slice(2, 4), 16);
				const b = parseInt(hex.slice(4, 6), 16);
				return { r, g, b };
			}
		}

		const rgbMatch = value.match(/^rgba?\(([^)]+)\)$/);
		if (rgbMatch) {
			const parts = rgbMatch[1].split(',').map((part) => Number.parseFloat(part.trim()));
			if (parts.length >= 3 && parts.slice(0, 3).every((num) => Number.isFinite(num))) {
				return {
					r: Math.max(0, Math.min(255, parts[0])),
					g: Math.max(0, Math.min(255, parts[1])),
					b: Math.max(0, Math.min(255, parts[2]))
				};
			}
		}

		return null;
	}

	function isLightColor(color: string): boolean {
		const rgb = parseColorToRgb(color);
		if (!rgb) return false;
		const luminance = (0.299 * rgb.r + 0.587 * rgb.g + 0.114 * rgb.b) / 255;
		return luminance >= 0.65;
	}

	function primaryTextClass(color: string): string {
		return isLightColor(color) ? 'text-[#111827]' : 'text-white';
	}

	const openRouterCreditsHint = $derived(
		agent.name === 'OpenRouter' &&
		$setupError.toLowerCase().includes('betide api token')
	);

	async function handleCopyCommand() {
		const command = $setupInstallInfo?.installCommand;
		if (command) {
			await copyToClipboard(command);
			copied = true;
			setTimeout(() => (copied = false), 2000);
		}
	}

	async function handleCopyError() {
		if ($setupError) {
			await copyToClipboard($setupError);
			copiedError = true;
			setTimeout(() => (copiedError = false), 2000);
		}
	}

	async function handleDownload() {
		const url = $setupInstallInfo?.installUrl;
		if (url) {
			await openUrl(url);
		}
	}

	async function handleOpenSettings() {
		await openPluginSettings();
	}

	async function handleRestart() {
		await restartEditor();
	}
</script>

<div class="flex flex-1 flex-col items-center justify-center px-6">
	<div class="flex w-full max-w-md flex-col items-center gap-6">

		{#if $setupState === 'idle'}
			<!-- ─── Idle: Agent not set up ─── -->
			{#if agent.iconUrl}
				<span style="color: {agent.color}; opacity: 0.5;">
					<img src={agent.iconUrl} alt="" class="h-14 w-14 invert opacity-70" />
				</span>
			{:else}
				<span
					class="flex h-14 w-14 items-center justify-center rounded-xl text-lg font-bold text-white"
					style="background-color: {agent.color}; opacity: 0.7;"
				>
					{agent.letter}
				</span>
			{/if}

			<div class="flex flex-col items-center gap-2 text-center">
				<h2 class="text-lg font-medium text-foreground/80">{agent.name} is not set up</h2>
				<p class="max-w-xs text-[13px] leading-relaxed text-muted-foreground/50">
					{agent.name} needs to be installed before you can start a conversation.
				</p>
				{#if $setupError}
					<p class="max-w-xs text-[12px] leading-relaxed text-amber-400/70">{$setupError}</p>
				{/if}
			</div>

			{#if $setupInstallInfo?.installCommand}
				<div class="w-full max-w-sm">
					<p class="mb-2 text-[12px] text-muted-foreground/40">Install command:</p>
					<div class="relative rounded-lg bg-[#1c1c1c] p-3">
						<pre class="overflow-x-auto pr-10 text-[12px] leading-relaxed text-emerald-400/80">{$setupInstallInfo.installCommand}</pre>
						<button
							class="absolute right-2 top-2 rounded-md p-1.5 text-muted-foreground/40 transition-colors hover:bg-white/5 hover:text-muted-foreground/70"
							onclick={handleCopyCommand}
							title="Copy command"
						>
							{#if copied}
								<Icon icon={Tick02Icon} size={14} strokeWidth={2} class="text-emerald-400" />
							{:else}
								<Icon icon={Copy01Icon} size={14} strokeWidth={1.5} />
							{/if}
						</button>
					</div>
				</div>
			{/if}

				<button
					class={`rounded-xl px-6 py-2.5 text-[14px] font-medium transition-all hover:brightness-110 ${primaryTextClass(agent.color)}`}
					style="background-color: {agent.color};"
					onclick={() => startInstall(agent.name)}
				>
				Set up {agent.shortName}
			</button>

		{:else if $setupState === 'checking'}
			<!-- ─── Checking prerequisites ─── -->
			<div class="flex flex-col items-center gap-4">
				<div class="h-10 w-10 animate-spin rounded-full border-2 border-muted-foreground/20 border-t-muted-foreground/60"></div>
				<p class="text-[14px] text-muted-foreground/60">{$setupProgress || 'Checking prerequisites...'}</p>
			</div>

		{:else if $setupState === 'installing'}
			<!-- ─── Installing ─── -->
			<div class="flex flex-col items-center gap-4">
				{#if agent.iconUrl}
					<span style="color: {agent.color}; opacity: 0.4;">
						<img src={agent.iconUrl} alt="" class="h-10 w-10 invert opacity-70" />
					</span>
				{/if}
				<div class="h-10 w-10 animate-spin rounded-full border-2 border-muted-foreground/20 border-t-muted-foreground/60"></div>
				<div class="flex flex-col items-center gap-1.5">
					<p class="text-[14px] text-foreground/70">Setting up {agent.name}...</p>
					<p class="max-w-xs text-center text-[12px] text-muted-foreground/40">{$setupProgress}</p>
				</div>
			</div>

		{:else if $setupState === 'cli_missing'}
			<!-- ─── CLI Required ─── -->
			<div class="flex w-full flex-col items-center gap-5">
				<div class="flex h-12 w-12 items-center justify-center rounded-full bg-amber-500/10">
					<Icon icon={Alert02Icon} size={24} strokeWidth={1.5} class="text-amber-400" />
				</div>

				<div class="flex flex-col items-center gap-2 text-center">
					<h2 class="text-lg font-medium text-foreground/80">
						{$setupInstallInfo?.baseExecutableName || 'CLI'} Required
					</h2>
					<p class="max-w-xs text-[13px] leading-relaxed text-muted-foreground/50">
						{#if $setupInstallInfo?.requiresAdapter}
							The adapter is ready, but {agent.name} requires the
						{:else}
							{agent.name} requires the
						{/if}
						<span class="font-mono text-foreground/60">{$setupInstallInfo?.baseExecutableName}</span>
						CLI to be installed on your system.
					</p>
				</div>

				{#if $setupInstallInfo?.installCommand}
					<div class="w-full">
						<p class="mb-2 text-[12px] text-muted-foreground/40">Run this in your terminal:</p>
						<div class="relative rounded-lg bg-[#1c1c1c] p-3">
							<pre class="overflow-x-auto pr-10 text-[13px] leading-relaxed text-emerald-400/80">{$setupInstallInfo.installCommand}</pre>
							<button
								class="absolute right-2 top-2 rounded-md p-1.5 text-muted-foreground/40 transition-colors hover:bg-white/5 hover:text-muted-foreground/70"
								onclick={handleCopyCommand}
								title="Copy command"
							>
								{#if copied}
									<Icon icon={Tick02Icon} size={14} strokeWidth={2} class="text-emerald-400" />
								{:else}
									<Icon icon={Copy01Icon} size={14} strokeWidth={1.5} />
								{/if}
							</button>
						</div>
					</div>
				{/if}

				<div class="flex flex-wrap justify-center gap-2.5">
					{#if $setupInstallInfo?.installUrl}
						<button
							class="flex items-center gap-1.5 rounded-lg border border-border/50 bg-secondary/50 px-3.5 py-2 text-[13px] text-foreground/70 transition-colors hover:bg-secondary"
							onclick={handleDownload}
						>
							<Icon icon={Download04Icon} size={14} strokeWidth={1.5} />
							Download Page
						</button>
					{/if}
						<button
							class={`flex items-center gap-1.5 rounded-lg px-3.5 py-2 text-[13px] font-medium transition-all hover:brightness-110 ${primaryTextClass(agent.color)}`}
							style="background-color: {agent.color};"
							onclick={() => recheckStatus(agent.name)}
						>
						<Icon icon={ReloadIcon} size={14} strokeWidth={1.5} />
						Try Again
					</button>
				</div>
			</div>

		{:else if $setupState === 'missing_key'}
			<!-- ─── API Key Required ─── -->
			<div class="flex w-full flex-col items-center gap-5">
				<div class="flex h-12 w-12 items-center justify-center rounded-full bg-amber-500/10">
					<Icon icon={Key01Icon} size={24} strokeWidth={1.5} class="text-amber-400" />
				</div>

					<div class="flex flex-col items-center gap-2 text-center">
						<h2 class="text-lg font-medium text-foreground/80">API Key Required</h2>
						<p class="max-w-xs text-[13px] leading-relaxed text-muted-foreground/50">
							{agent.name} requires an API key. Configure it in
							<span class="text-foreground/60">Project Settings &gt; Plugins &gt; Agent Integration Kit</span>.
						</p>
						{#if $setupError}
							<p class="max-w-xs text-[12px] leading-relaxed text-amber-400/80">{$setupError}</p>
						{/if}
						{#if openRouterCreditsHint}
							<p class="max-w-xs text-[12px] leading-relaxed text-muted-foreground/55">
								If your OpenRouter key is already set, disable
								<span class="text-foreground/65">Use Betide Studio Credits</span>
								in the Auto-Update section, then click Check Again.
							</p>
						{/if}
					</div>

					<div class="flex flex-wrap justify-center gap-2.5">
					<button
						class="flex items-center gap-1.5 rounded-lg border border-border/50 bg-secondary/50 px-3.5 py-2 text-[13px] text-foreground/70 transition-colors hover:bg-secondary"
						onclick={handleOpenSettings}
					>
						<Icon icon={Settings02Icon} size={14} strokeWidth={1.5} />
						Open Settings
					</button>
						<button
							class={`flex items-center gap-1.5 rounded-lg px-3.5 py-2 text-[13px] font-medium transition-all hover:brightness-110 ${primaryTextClass(agent.color)}`}
							style="background-color: {agent.color};"
							onclick={() => recheckStatus(agent.name)}
						>
						<Icon icon={ReloadIcon} size={14} strokeWidth={1.5} />
						Check Again
					</button>
				</div>
			</div>

		{:else if $setupState === 'success'}
			<!-- ─── Success ─── -->
			<div class="flex flex-col items-center gap-5">
				<div class="flex h-14 w-14 items-center justify-center rounded-full bg-emerald-500/10">
					<Icon icon={Tick02Icon} size={28} strokeWidth={2} class="text-emerald-400" />
				</div>

				<div class="flex flex-col items-center gap-2 text-center">
					<h2 class="text-lg font-medium text-emerald-400">{agent.name} is ready!</h2>
					<p class="max-w-xs text-[13px] leading-relaxed text-muted-foreground/50">
						Please restart the editor to start using {agent.name}. The agent will be available after relaunch.
					</p>
				</div>

				<button
					class="rounded-lg border border-border/50 bg-secondary/50 px-4 py-2 text-[13px] text-foreground/70 transition-colors hover:bg-secondary"
					onclick={handleRestart}
				>
					Restart
				</button>
				</div>

		{:else if $setupState === 'error'}
			<!-- ─── Error ─── -->
			<div class="flex w-full flex-col items-center gap-5">
				<div class="flex h-12 w-12 items-center justify-center rounded-full bg-red-500/10">
					<Icon icon={Cancel01Icon} size={24} strokeWidth={1.5} class="text-red-400" />
				</div>

				<div class="flex flex-col items-center gap-2 text-center">
					<h2 class="text-lg font-medium text-red-400">Setup Failed</h2>
				</div>

				{#if $setupError}
					<div class="relative w-full rounded-lg bg-[#1c1c1c] p-3">
						<pre class="max-h-[160px] overflow-auto text-[12px] leading-relaxed text-muted-foreground/60">{$setupError}</pre>
						<button
							class="absolute right-2 top-2 rounded-md p-1.5 text-muted-foreground/40 transition-colors hover:bg-white/5 hover:text-muted-foreground/70"
							onclick={handleCopyError}
							title="Copy error"
						>
							{#if copiedError}
								<Icon icon={Tick02Icon} size={14} strokeWidth={2} class="text-emerald-400" />
							{:else}
								<Icon icon={Copy01Icon} size={14} strokeWidth={1.5} />
							{/if}
						</button>
					</div>
				{/if}

				<div class="flex flex-wrap justify-center gap-2.5">
					<button
						class="rounded-lg border border-border/50 bg-secondary/50 px-3.5 py-2 text-[13px] text-foreground/70 transition-colors hover:bg-secondary"
						onclick={exitSetup}
					>
						Cancel
					</button>
						<button
							class={`flex items-center gap-1.5 rounded-lg px-3.5 py-2 text-[13px] font-medium transition-all hover:brightness-110 ${primaryTextClass(agent.color)}`}
							style="background-color: {agent.color};"
							onclick={() => startInstall(agent.name)}
						>
						<Icon icon={ReloadIcon} size={14} strokeWidth={1.5} />
						Try Again
					</button>
				</div>
			</div>
		{/if}
	</div>
</div>
