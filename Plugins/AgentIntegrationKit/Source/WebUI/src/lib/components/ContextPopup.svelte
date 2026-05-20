<script lang="ts">
	import { searchContextItems, type ContextItem } from '$lib/bridge.js';

	let {
		query,
		visible,
		onselect,
		ondismiss,
		onnavigate
	}: {
		query: string;
		visible: boolean;
		onselect: (item: ContextItem) => void;
		ondismiss: () => void;
		onnavigate: (path: string) => void;
	} = $props();

	let results = $state<ContextItem[]>([]);
	let isLoading = $state(false);
	let selectedIndex = $state(0);
	let popupEl: HTMLDivElement | undefined = $state();

	let searchTimeout: ReturnType<typeof setTimeout> | undefined;

	$effect(() => {
		if (!visible) {
			results = [];
			selectedIndex = 0;
			return;
		}

		const q = query;
		clearTimeout(searchTimeout);

		const delay = q.length === 0 ? 0 : 80;
		isLoading = true;

		if (delay === 0) {
			searchContextItems(q).then((items) => {
				results = items;
				selectedIndex = 0;
				isLoading = false;
			});
		} else {
			searchTimeout = setTimeout(() => {
				searchContextItems(q).then((items) => {
					results = items;
					selectedIndex = 0;
					isLoading = false;
				});
			}, delay);
		}
	});

	let flatResults = $derived(results);

	function handleItemAction(item: ContextItem) {
		if (item.type === 'folder') {
			// Drill into folder — tell parent to update the query
			onnavigate(item.path);
		} else {
			onselect(item);
		}
	}

	export function handleKeydown(e: KeyboardEvent): boolean {
		if (!visible || results.length === 0) return false;

		if (e.key === 'ArrowDown') {
			e.preventDefault();
			selectedIndex = (selectedIndex + 1) % flatResults.length;
			scrollToSelected();
			return true;
		}
		if (e.key === 'ArrowUp') {
			e.preventDefault();
			selectedIndex = (selectedIndex - 1 + flatResults.length) % flatResults.length;
			scrollToSelected();
			return true;
		}
		if (e.key === 'Enter' || e.key === 'Tab') {
			e.preventDefault();
			if (flatResults[selectedIndex]) {
				handleItemAction(flatResults[selectedIndex]);
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
	<div
		bind:this={popupEl}
		class="absolute bottom-full left-0 z-50 mb-1 max-h-[280px] w-[380px] overflow-y-auto rounded-lg border border-border/60 bg-[#1a1a1a] py-1 shadow-xl"
	>
		{#if isLoading && results.length === 0}
			<div class="px-3 py-2 text-[13px] text-muted-foreground/40">
				Searching...
			</div>
		{:else if results.length === 0}
			<div class="px-3 py-2 text-[13px] text-muted-foreground/40">
				No results
			</div>
		{:else}
			{#each flatResults as item, idx}
				<button
					data-index={idx}
					class="flex w-full items-center gap-2 px-2.5 py-[5px] text-left text-[13px] transition-colors
						{idx === selectedIndex ? 'bg-white/[0.08] text-foreground' : 'text-foreground/70 hover:bg-white/[0.04]'}"
					onclick={() => handleItemAction(item)}
					onmouseenter={() => (selectedIndex = idx)}
				>
					<span class="context-icon flex h-4 w-4 shrink-0 items-center justify-center opacity-70">
						{#if item.type === 'folder'}
							<!-- Folder icon (minimal) -->
							<svg width="14" height="14" viewBox="0 0 16 16" fill="none" xmlns="http://www.w3.org/2000/svg">
								<path d="M1.5 3C1.5 2.44772 1.94772 2 2.5 2H6.29289C6.4255 2 6.55268 2.05268 6.64645 2.14645L8 3.5H13.5C14.0523 3.5 14.5 3.94772 14.5 4.5V13C14.5 13.5523 14.0523 14 13.5 14H2.5C1.94772 14 1.5 13.5523 1.5 13V3Z" fill="white" fill-opacity="0.5"/>
							</svg>
						{:else if item.icon}
							{@html item.icon}
						{:else}
							<!-- Generic file icon -->
							<svg width="14" height="14" viewBox="0 0 16 16" fill="none" xmlns="http://www.w3.org/2000/svg">
								<path d="M3 1.5C3 1.22386 3.22386 1 3.5 1H9.5L13 4.5V14.5C13 14.7761 12.7761 15 12.5 15H3.5C3.22386 15 3 14.7761 3 14.5V1.5Z" fill="white" fill-opacity="0.4"/>
								<path d="M9.5 1L13 4.5H10C9.72386 4.5 9.5 4.27614 9.5 4V1Z" fill="white" fill-opacity="0.6"/>
							</svg>
						{/if}
					</span>
					<span class="truncate {item.type === 'folder' ? 'text-foreground/90' : ''}">{item.name}</span>
				</button>
			{/each}
		{/if}
	</div>
{/if}

<style>
	.context-icon :global(svg) {
		width: 14px;
		height: 14px;
	}
</style>
