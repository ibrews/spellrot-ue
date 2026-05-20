<script lang="ts">
	import { onMount } from 'svelte';
	import { getPrerequisiteStatus, type PrerequisiteStatus, type PrerequisiteTool, openUrl } from '$lib/bridge.js';
	import { CheckmarkCircle02Icon, Alert02Icon, RefreshIcon } from '@hugeicons/core-free-icons';
	import Icon from '$lib/components/Icon.svelte';

	let status = $state<PrerequisiteStatus | null>(null);
	let isLoading = $state(false);

	const tools = [
		{ key: 'node', name: 'Node.js', required: true, description: 'Required for most ACP agents (npx)', installUrl: 'https://nodejs.org/', installTip: 'Download from nodejs.org or run: brew install node' },
		{ key: 'npm', name: 'npm', required: false, description: 'Comes with Node.js', installUrl: '', installTip: 'Included with Node.js' },
		{ key: 'npx', name: 'npx', required: false, description: 'Runs ACP agents on demand', installUrl: '', installTip: 'Included with Node.js' },
		{ key: 'git', name: 'Git', required: false, description: 'Used by some agents for source control', installUrl: 'https://git-scm.com/', installTip: 'Download from git-scm.com or run: brew install git' },
		{ key: 'uv', name: 'uv', required: false, description: 'Required for Python-based agents (uvx)', installUrl: 'https://docs.astral.sh/uv/', installTip: 'Run: curl -LsSf https://astral.sh/uv/install.sh | sh' },
		{ key: 'bun', name: 'Bun', required: false, description: 'Alternative JS runtime', installUrl: 'https://bun.sh/', installTip: 'Run: curl -fsSL https://bun.sh/install | bash' },
	] as const;

	async function checkPrerequisites() {
		isLoading = true;
		try {
			status = await getPrerequisiteStatus();
		} catch (e) {
			console.warn('Failed to check prerequisites:', e);
		} finally {
			isLoading = false;
		}
	}

	onMount(() => {
		checkPrerequisites();
	});

	function getTool(key: string): PrerequisiteTool {
		if (!status) return { found: false, path: '' };
		return (status as any)[key] ?? { found: false, path: '' };
	}
</script>

<div class="flex flex-col gap-3">
	<div class="flex items-center justify-between">
		<div>
			<h3 class="text-[14px] font-medium text-foreground">Prerequisites</h3>
			<p class="text-[12px] text-muted-foreground/60">Tools needed to run ACP agents.</p>
		</div>
		<button
			class="flex items-center gap-1.5 rounded-md border border-border/60 px-2.5 py-1 text-[11px] text-muted-foreground transition-colors hover:bg-accent/20 hover:text-foreground"
			onclick={checkPrerequisites}
			disabled={isLoading}
		>
			<Icon icon={RefreshIcon} size={12} class={isLoading ? 'animate-spin' : ''} />
			Recheck
		</button>
	</div>

	{#if !status}
		<div class="flex items-center gap-2 py-4 text-[12px] text-muted-foreground/50">
			<span class="inline-block h-3.5 w-3.5 animate-spin rounded-full border-2 border-muted-foreground/30 border-t-muted-foreground"></span>
			Checking...
		</div>
	{:else}
		<div class="rounded-lg border border-border/60 bg-card divide-y divide-border/40">
			{#each tools as tool}
				{@const info = getTool(tool.key)}
				<div class="flex items-center gap-3 px-4 py-2.5">
					<!-- Status icon -->
					{#if info.found}
						<Icon icon={CheckmarkCircle02Icon} size={16} class="text-emerald-400 shrink-0" />
					{:else}
						<Icon icon={Alert02Icon} size={16} class="{tool.required ? 'text-red-400' : 'text-muted-foreground/30'} shrink-0" />
					{/if}

					<!-- Name + description -->
					<div class="flex-1 min-w-0">
						<div class="flex items-baseline gap-2">
							<span class="text-[13px] font-medium text-foreground">{tool.name}</span>
							{#if tool.required}
								<span class="text-[10px] text-amber-400">Required</span>
							{/if}
						</div>
						<p class="text-[11px] text-muted-foreground/50 truncate">{tool.description}</p>
					</div>

					<!-- Version / Install link -->
					<div class="shrink-0 text-right">
						{#if info.found}
							<span class="text-[11px] text-emerald-400/80">{info.version || 'Found'}</span>
							{#if info.path}
								<p class="text-[10px] text-muted-foreground/30 truncate max-w-[200px]" title={info.path}>{info.path}</p>
							{/if}
						{:else if tool.installUrl}
							<button
								class="text-[11px] text-[var(--ue-accent)] hover:underline"
								onclick={() => openUrl(tool.installUrl)}
							>
								Install
							</button>
							<p class="text-[10px] text-muted-foreground/30">{tool.installTip}</p>
						{:else}
							<span class="text-[11px] text-muted-foreground/30">Not found</span>
						{/if}
					</div>
				</div>
			{/each}
		</div>
	{/if}
</div>
