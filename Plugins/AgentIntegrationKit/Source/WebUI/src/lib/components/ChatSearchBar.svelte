<script lang="ts">
	import { onMount, onDestroy, tick } from 'svelte';
	import Icon from '$lib/components/Icon.svelte';
	import { ArrowUp01Icon, ArrowDown01Icon, Cancel01Icon } from '@hugeicons/core-free-icons';
	import { t } from '$lib/i18n.js';

	interface Props {
		scrollContainer?: HTMLDivElement;
		onclose: () => void;
	}

	let { scrollContainer, onclose }: Props = $props();

	let query = $state('');
	let inputEl: HTMLInputElement | undefined = $state();
	let matches = $state<Range[]>([]);
	let currentIndex = $state(-1);
	let debounceTimer: ReturnType<typeof setTimeout> | null = null;

	// Use CSS Custom Highlight API (supported in Chromium/CEF)
	const hasHighlightAPI = typeof CSS !== 'undefined' && 'highlights' in CSS;

	onMount(() => {
		tick().then(() => inputEl?.focus());
	});

	onDestroy(() => {
		clearHighlights();
		if (debounceTimer) clearTimeout(debounceTimer);
	});

	function clearHighlights() {
		if (hasHighlightAPI) {
			(CSS as any).highlights.delete('search-results');
			(CSS as any).highlights.delete('search-current');
		}
		matches = [];
		currentIndex = -1;
	}

	function findMatches() {
		clearHighlights();
		const q = query.trim();
		if (!q || !scrollContainer) return;

		const ranges: Range[] = [];
		const walker = document.createTreeWalker(scrollContainer, NodeFilter.SHOW_TEXT);
		const lowerQuery = q.toLowerCase();

		while (walker.nextNode()) {
			const node = walker.currentNode as Text;
			const text = node.textContent?.toLowerCase() ?? '';
			let startPos = 0;
			while (true) {
				const idx = text.indexOf(lowerQuery, startPos);
				if (idx === -1) break;
				const range = new Range();
				range.setStart(node, idx);
				range.setEnd(node, idx + q.length);
				ranges.push(range);
				startPos = idx + q.length;
			}
		}

		matches = ranges;
		currentIndex = ranges.length > 0 ? 0 : -1;

		if (hasHighlightAPI && ranges.length > 0) {
			const highlight = new (globalThis as any).Highlight(...ranges);
			(CSS as any).highlights.set('search-results', highlight);
			scrollToCurrentMatch();
		}
	}

	function scrollToCurrentMatch() {
		if (currentIndex < 0 || !matches[currentIndex] || !scrollContainer) return;

		if (hasHighlightAPI) {
			const current = new (globalThis as any).Highlight(matches[currentIndex]);
			(CSS as any).highlights.set('search-current', current);
		}

		try {
			const rect = matches[currentIndex].getBoundingClientRect();
			const containerRect = scrollContainer.getBoundingClientRect();
			const targetTop = rect.top - containerRect.top + scrollContainer.scrollTop - containerRect.height / 3;
			scrollContainer.scrollTo({ top: targetTop, behavior: 'smooth' });
		} catch {
			// Range may be detached if DOM changed
		}
	}

	function goNext() {
		if (matches.length === 0) return;
		currentIndex = (currentIndex + 1) % matches.length;
		scrollToCurrentMatch();
	}

	function goPrev() {
		if (matches.length === 0) return;
		currentIndex = (currentIndex - 1 + matches.length) % matches.length;
		scrollToCurrentMatch();
	}

	function handleInput() {
		if (debounceTimer) clearTimeout(debounceTimer);
		debounceTimer = setTimeout(findMatches, 150);
	}

	function handleKeydown(e: KeyboardEvent) {
		if (e.key === 'Escape') {
			onclose();
		} else if (e.key === 'Enter') {
			e.preventDefault();
			if (e.shiftKey) goPrev();
			else goNext();
		}
	}

	function handleClose() {
		clearHighlights();
		onclose();
	}
</script>

<div class="absolute right-4 top-2 z-20 flex items-center gap-1 rounded-lg border border-border bg-card px-2 py-1.5 shadow-lg" style="box-shadow: 0 2px 12px rgba(0,0,0,0.25);">
	<input
		bind:this={inputEl}
		bind:value={query}
		oninput={handleInput}
		onkeydown={handleKeydown}
		placeholder={$t('search_in_chat')}
		spellcheck="false"
		class="w-[200px] bg-transparent px-1.5 text-[13px] text-foreground placeholder:text-muted-foreground/50 focus:outline-none"
	/>
	<span class="min-w-[60px] text-center text-[11px] text-muted-foreground/60">
		{#if query.trim()}
			{#if matches.length > 0}
				{$t('n_of_m_matches', { n: currentIndex + 1, m: matches.length })}
			{:else}
				{$t('no_matches')}
			{/if}
		{/if}
	</span>
	<button
		class="flex h-6 w-6 items-center justify-center rounded text-muted-foreground/60 transition-colors hover:bg-accent hover:text-foreground disabled:opacity-30"
		onclick={goPrev}
		disabled={matches.length === 0}
	>
		<Icon icon={ArrowUp01Icon} size={14} strokeWidth={2} />
	</button>
	<button
		class="flex h-6 w-6 items-center justify-center rounded text-muted-foreground/60 transition-colors hover:bg-accent hover:text-foreground disabled:opacity-30"
		onclick={goNext}
		disabled={matches.length === 0}
	>
		<Icon icon={ArrowDown01Icon} size={14} strokeWidth={2} />
	</button>
	<button
		class="flex h-6 w-6 items-center justify-center rounded text-muted-foreground/60 transition-colors hover:bg-accent hover:text-foreground"
		onclick={handleClose}
	>
		<Icon icon={Cancel01Icon} size={14} strokeWidth={2} />
	</button>
</div>
