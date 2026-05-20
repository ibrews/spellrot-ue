<script lang="ts">
	import Icon from '$lib/components/Icon.svelte';
	import { MessageMultiple01Icon, Image02Icon, ComputerTerminal01Icon } from '@hugeicons/core-free-icons';
	import { currentTab, type AppTab } from '$lib/stores/navigation.js';

	const tabs: { id: AppTab; label: string; icon: typeof MessageMultiple01Icon }[] = [
		{ id: 'chat', label: 'Chat', icon: MessageMultiple01Icon },
		{ id: 'studio', label: 'Studio', icon: Image02Icon },
		{ id: 'terminal', label: 'Terminal', icon: ComputerTerminal01Icon }
	];

	function switchTab(id: AppTab) {
		currentTab.set(id);
	}
</script>

<nav class="flex h-[34px] shrink-0 items-center border-b border-border bg-[var(--sidebar)]">
	<div class="flex h-full items-center gap-0 pl-2">
		{#each tabs as tab}
			{@const active = $currentTab === tab.id}
			<button
				class="relative flex h-full items-center gap-1.5 px-3 text-[11px] font-medium transition-colors duration-100
					{active
					? 'text-[var(--foreground)]'
					: 'text-[var(--muted-foreground)] hover:text-[var(--foreground)]'}"
				onclick={() => switchTab(tab.id)}
			>
				<Icon icon={tab.icon} size={13} />
				{tab.label}
				{#if active}
					<span
						class="absolute bottom-0 left-2 right-2 h-[2px] rounded-t-full bg-[var(--ue-accent)]"
					></span>
				{/if}
			</button>
		{/each}
	</div>

	<div class="ml-auto flex items-center pr-3">
		<span class="text-[10px] font-medium tracking-wider text-[var(--muted-foreground)] opacity-60"
			>NEOSTACK</span
		>
	</div>
</nav>
