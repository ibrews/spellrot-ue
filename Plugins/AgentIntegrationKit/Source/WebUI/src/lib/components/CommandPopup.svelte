<script lang="ts">
	import type { SlashCommand } from '$lib/bridge.js';
	import Icon from '$lib/components/Icon.svelte';
	import { CommandIcon } from '@hugeicons/core-free-icons';

	let {
		query,
		visible,
		commands,
		onselect,
		ondismiss
	}: {
		query: string;
		visible: boolean;
		commands: SlashCommand[];
		onselect: (command: SlashCommand) => void;
		ondismiss: () => void;
	} = $props();

	let selectedIndex = $state(0);
	let popupEl: HTMLDivElement | undefined = $state();

	let filtered = $derived(() => {
		const q = query.toLowerCase();
		if (!q) return commands;
		return commands.filter(
			(c) => c.name.toLowerCase().includes(q) || c.description.toLowerCase().includes(q)
		);
	});

	// Reset selection when query changes
	$effect(() => {
		query;
		selectedIndex = 0;
	});

	export function handleKeydown(e: KeyboardEvent): boolean {
		const items = filtered();
		if (!visible || items.length === 0) return false;

		if (e.key === 'ArrowDown') {
			e.preventDefault();
			selectedIndex = (selectedIndex + 1) % items.length;
			scrollToSelected();
			return true;
		}
		if (e.key === 'ArrowUp') {
			e.preventDefault();
			selectedIndex = (selectedIndex - 1 + items.length) % items.length;
			scrollToSelected();
			return true;
		}
		if (e.key === 'Enter' || e.key === 'Tab') {
			e.preventDefault();
			if (items[selectedIndex]) {
				onselect(items[selectedIndex]);
			}
			return true;
		}
		if (e.key === 'Escape') {
			e.preventDefault();
			ondismiss();
			return true;
		}
		return false;
	}

	function scrollToSelected() {
		if (!popupEl) return;
		const item = popupEl.querySelector(`[data-index="${selectedIndex}"]`);
		item?.scrollIntoView({ block: 'nearest' });
	}
</script>

{#if visible}
	{@const items = filtered()}
	<div
		bind:this={popupEl}
		class="absolute bottom-full left-0 z-50 mb-1 max-h-[280px] w-[360px] overflow-y-auto rounded-xl border border-border bg-[#222] shadow-2xl"
	>
		{#if items.length === 0}
			<div class="px-3 py-3 text-[12px] text-muted-foreground/40">
				No matching commands
			</div>
		{:else}
			<div class="px-3 pt-2 pb-0.5 text-[10px] font-medium uppercase tracking-wider text-muted-foreground/40">
				Commands
			</div>
			{#each items as cmd, idx}
				<button
					data-index={idx}
					class="flex w-full items-center gap-2.5 px-3 py-1.5 text-left text-[13px] transition-colors
						{idx === selectedIndex ? 'bg-accent/40 text-foreground' : 'text-foreground/80 hover:bg-accent/20'}"
					onclick={() => onselect(cmd)}
					onmouseenter={() => (selectedIndex = idx)}
				>
					<span class="flex h-4 w-4 shrink-0 items-center justify-center text-muted-foreground/60">
						<Icon icon={CommandIcon} size={14} strokeWidth={1.5} />
					</span>
					<div class="min-w-0 flex-1">
						<div class="flex items-baseline gap-1.5">
							<span class="font-medium">/{cmd.name}</span>
							{#if cmd.inputHint}
								<span class="text-[11px] text-muted-foreground/40">{cmd.inputHint}</span>
							{/if}
						</div>
						{#if cmd.description}
							<div class="truncate text-[11px] text-muted-foreground/40">{cmd.description}</div>
						{/if}
					</div>
				</button>
			{/each}
		{/if}
	</div>
{/if}
