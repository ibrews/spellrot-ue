<script lang="ts">
	import Icon from '$lib/components/Icon.svelte';
	import {
		Shield02Icon,
		Tick02Icon,
		Cancel01Icon,
		ArrowDown01Icon,
		ArrowUp01Icon,
		CheckmarkBadge01Icon
	} from '@hugeicons/core-free-icons';
	import type { PermissionRequest } from '$lib/bridge.js';
	import { respondToOption } from '$lib/stores/permissions.js';

	let { request }: { request: PermissionRequest } = $props();

	let argsExpanded = $state(false);
	let responding = $state(false);
	let activeOptionIndex = $state(0);
	let rootEl: HTMLDivElement | undefined = $state();

	// Detect ExitPlanMode special case
	let isExitPlanMode = $derived(() => {
		const id = request.toolCall.toolCallId.toLowerCase();
		const title = request.toolCall.title.toLowerCase();
		return (
			id.includes('exitplanmode') ||
			title.includes('ready to code') ||
			title.includes('exit plan')
		);
	});

	// Parse raw input for display
	let formattedArgs = $derived(() => {
		try {
			const parsed = JSON.parse(request.toolCall.rawInput);
			return JSON.stringify(parsed, null, 2);
		} catch {
			return request.toolCall.rawInput;
		}
	});

	let argsLong = $derived(request.toolCall.rawInput.length > 120);

	$effect(() => {
		request.requestId;
		argsExpanded = false;
		responding = false;
		activeOptionIndex = 0;
		requestAnimationFrame(() => {
			const firstButton = rootEl?.querySelector<HTMLButtonElement>('button[data-option-index="0"]');
			firstButton?.focus();
		});
	});

	function buttonColor(kind: string): string {
		if (kind === 'allow_always' || kind === 'allow_once') {
			return 'border-emerald-500/35 bg-emerald-500/10 text-emerald-200';
		}
		if (kind === 'reject_once') {
			return 'border-red-500/35 bg-red-500/10 text-red-200';
		}
		return 'border-border/40 bg-secondary/30 text-foreground';
	}

	function buttonIcon(kind: string) {
		if (kind === 'allow_always' || kind === 'allow_once') return Tick02Icon;
		if (kind === 'reject_once') return Cancel01Icon;
		return null;
	}

	function moveSelection(delta: number) {
		const count = request.options.length;
		if (count <= 0) return;
		activeOptionIndex = (activeOptionIndex + delta + count) % count;
	}

	async function handleOption(optionId: string) {
		if (responding) return;
		responding = true;
		await respondToOption(optionId);
	}

	function handleKeydown(e: KeyboardEvent) {
		if (!e.metaKey && !e.ctrlKey && !e.altKey) {
			e.stopPropagation();
		}
		if (responding || request.options.length === 0) return;

		if (e.key === 'ArrowDown' || e.key === 'ArrowRight') {
			e.preventDefault();
			moveSelection(1);
			return;
		}

		if (e.key === 'ArrowUp' || e.key === 'ArrowLeft') {
			e.preventDefault();
			moveSelection(-1);
			return;
		}

		if (e.key === 'Enter') {
			e.preventDefault();
			const option = request.options[activeOptionIndex];
			if (option) {
				handleOption(option.optionId);
			}
		}
	}
</script>

<div
	bind:this={rootEl}
	role="listbox"
	aria-label="Permission options"
	tabindex="0"
	onkeydown={handleKeydown}
	class="rounded-xl border border-amber-500/30 bg-amber-500/5 p-3 focus:outline-none focus:ring-2 focus:ring-[var(--ue-accent-muted)]"
>
	<div
		class="rounded-lg border {isExitPlanMode()
			? 'border-emerald-500/30 bg-emerald-500/5'
			: 'border-amber-500/30 bg-amber-500/5'}"
	>
		<!-- Header -->
		<div class="flex items-center justify-between gap-2 px-4 pt-3 pb-2">
			<div class="flex items-center gap-2">
				{#if isExitPlanMode()}
					<Icon icon={CheckmarkBadge01Icon} size={18} strokeWidth={1.5} class="text-emerald-400" />
					<span class="text-[14px] font-semibold text-emerald-400">Ready to code?</span>
				{:else}
					<Icon icon={Shield02Icon} size={18} strokeWidth={1.5} class="text-amber-400" />
					<span class="text-[14px] font-semibold text-amber-400">Permission Required</span>
				{/if}
			</div>
			<span class="text-[11px] text-muted-foreground/75">↑/↓ select • Enter confirm</span>
		</div>

		<!-- Tool info -->
		<div class="px-4 pb-2">
			<div class="text-[13px] text-foreground">{request.toolCall.title || 'Tool request'}</div>
		</div>

		<!-- Arguments (collapsible) -->
		{#if request.toolCall.rawInput}
			<div class="px-4 pb-3">
				{#if argsLong}
					<button
						class="flex items-center gap-1 text-[11px] text-muted-foreground/60 transition-colors hover:text-muted-foreground"
						onclick={() => (argsExpanded = !argsExpanded)}
					>
						<Icon
							icon={argsExpanded ? ArrowUp01Icon : ArrowDown01Icon}
							size={10}
							strokeWidth={1.5}
						/>
						{argsExpanded ? 'Hide' : 'Show'} arguments
					</button>
				{/if}
				{#if !argsLong || argsExpanded}
					<pre
						class="mt-1.5 max-h-[200px] overflow-auto rounded-lg bg-[#1c1c1c] p-3 text-[12px] leading-relaxed text-muted-foreground/70"
					>{formattedArgs()}</pre>
				{/if}
			</div>
		{/if}

		<!-- Option buttons -->
		<div class="flex flex-col gap-1.5 border-t border-border/20 px-4 py-3">
			{#each request.options as option, index}
				{@const icon = buttonIcon(option.kind)}
				<button
					role="option"
					aria-selected={index === activeOptionIndex}
					data-option-index={index}
					class="flex items-center gap-1.5 rounded-lg border px-3 py-2 text-[13px] font-medium transition-all {buttonColor(option.kind)} {index ===
					activeOptionIndex
						? 'ring-2 ring-[var(--ue-accent-muted)]'
						: 'hover:border-border/70'} {responding ? 'opacity-50 cursor-not-allowed' : ''}"
					onclick={() => handleOption(option.optionId)}
					onmouseenter={() => (activeOptionIndex = index)}
					disabled={responding}
				>
					{#if icon}
						<Icon {icon} size={14} strokeWidth={2} />
					{/if}
					{option.name}
				</button>
			{/each}
		</div>
	</div>
</div>
