<script lang="ts">
	import { ArrowDown01Icon } from '@hugeicons/core-free-icons';
	import Icon from '$lib/components/Icon.svelte';

	type Option = { value: string; label: string };

	let {
		options,
		value = $bindable(''),
		onchange,
		id = ''
	}: {
		options: Option[];
		value: string;
		onchange?: (value: string) => void;
		id?: string;
	} = $props();

	let open = $state(false);
	let containerEl: HTMLDivElement | undefined = $state();

	const selectedLabel = $derived(options.find(o => o.value === value)?.label ?? value);

	function toggle() {
		open = !open;
	}

	function select(opt: Option) {
		value = opt.value;
		open = false;
		onchange?.(opt.value);
	}

	function handleKeydown(e: KeyboardEvent) {
		if (e.key === 'Escape') {
			open = false;
		}
	}

	function handleClickOutside(e: MouseEvent) {
		if (containerEl && !containerEl.contains(e.target as Node)) {
			open = false;
		}
	}

	$effect(() => {
		if (open) {
			document.addEventListener('click', handleClickOutside, true);
			return () => document.removeEventListener('click', handleClickOutside, true);
		}
	});
</script>

<div bind:this={containerEl} class="relative" {id} onkeydown={handleKeydown}>
	<button
		type="button"
		class="flex w-full items-center justify-between rounded-md border border-border/60 bg-transparent px-3 py-2 text-left text-[13px] text-foreground transition-colors hover:border-foreground/20 focus:border-foreground/30 focus:outline-none"
		onclick={toggle}
	>
		<span>{selectedLabel}</span>
		<span class="ml-2 text-muted-foreground/60 transition-transform" class:rotate-180={open}>
			<Icon icon={ArrowDown01Icon} size={14} />
		</span>
	</button>

	{#if open}
		<div class="absolute left-0 right-0 top-full z-50 mt-1 overflow-hidden rounded-md border border-border/60 bg-[#1a1a1a] shadow-lg">
			{#each options as opt}
				<button
					type="button"
					class="flex w-full items-center px-3 py-2 text-left text-[13px] transition-colors hover:bg-foreground/5"
					class:text-foreground={opt.value === value}
					class:text-muted-foreground={opt.value !== value}
					onclick={() => select(opt)}
				>
					{opt.label}
				</button>
			{/each}
		</div>
	{/if}
</div>
