<script lang="ts">
	import Icon from '$lib/components/Icon.svelte';
	import {
		CheckmarkCircle02Icon,
		Loading03Icon,
		RadioIcon,
		ArrowDown01Icon,
		ArrowUp01Icon
	} from '@hugeicons/core-free-icons';
	import { currentPlan, planProgress } from '$lib/stores/plan.js';

	let expanded = $state(false);

	let currentTask = $derived(
		$currentPlan?.entries.find((e) => e.status === 'in_progress')
	);

	let statusIcon = (status: string) => {
		switch (status) {
			case 'completed':
				return CheckmarkCircle02Icon;
			case 'in_progress':
				return Loading03Icon;
			default:
				return RadioIcon;
		}
	};

	let statusColor = (status: string) => {
		switch (status) {
			case 'completed':
				return 'text-emerald-400/60';
			case 'in_progress':
				return 'text-blue-400';
			default:
				return 'text-muted-foreground/30';
		}
	};

	let textColor = (status: string) => {
		switch (status) {
			case 'completed':
				return 'text-muted-foreground/40 line-through';
			case 'in_progress':
				return 'text-foreground';
			default:
				return 'text-muted-foreground/60';
		}
	};
</script>

{#if $currentPlan && $currentPlan.entries.length > 0}
	<div class="border-b border-border/50">
		<!-- Header — clickable to toggle -->
		<button
			class="flex w-full items-center gap-2.5 px-3 py-2 text-left text-[13px] transition-colors hover:bg-accent/20"
			onclick={() => (expanded = !expanded)}
		>
			{#if expanded}
				<!-- Expanded: show "Tasks" label + count -->
				<div class="flex-1 flex items-center gap-2">
					<span class="font-medium text-foreground/80">Tasks</span>
					<span class="text-[11px] text-muted-foreground/50">
						{$currentPlan.completedCount}/{$currentPlan.totalCount}
					</span>
				</div>
			{:else}
				<!-- Collapsed: show current task + count -->
				<div class="flex-1 flex items-center gap-2 min-w-0">
					{#if currentTask}
						<span class="text-blue-400">
							<Icon icon={Loading03Icon} size={13} strokeWidth={1.5} class="animate-spin" />
						</span>
						<span class="truncate text-[12px] text-foreground/70">
							{currentTask.activeForm || currentTask.content}
						</span>
					{:else}
						<span class="text-emerald-400/60">
							<Icon icon={CheckmarkCircle02Icon} size={13} strokeWidth={1.5} />
						</span>
						<span class="truncate text-[12px] text-muted-foreground/50">All tasks complete</span>
					{/if}
					<span class="shrink-0 text-[11px] text-muted-foreground/40">
						{$currentPlan.completedCount}/{$currentPlan.totalCount}
					</span>
				</div>
			{/if}

			<!-- Progress bar -->
			<div class="h-1.5 w-16 shrink-0 overflow-hidden rounded-full bg-muted-foreground/15">
				<div
					class="h-full rounded-full bg-emerald-500 transition-all duration-300"
					style="width: {$planProgress}%;"
				></div>
			</div>

			<span class="shrink-0 text-muted-foreground/50">
				<Icon icon={expanded ? ArrowUp01Icon : ArrowDown01Icon} size={12} strokeWidth={1.5} />
			</span>
		</button>

		<!-- Task list (expanded only) -->
		{#if expanded}
			<div class="border-t border-border/30 px-1 py-1">
				{#each $currentPlan.entries as entry, i}
					<div class="flex items-start gap-2 rounded-lg px-2 py-1">
						<span class="mt-0.5 flex h-4 w-4 shrink-0 items-center justify-center {statusColor(entry.status)}">
							<Icon
								icon={statusIcon(entry.status)}
								size={13}
								strokeWidth={1.5}
								class={entry.status === 'in_progress' ? 'animate-spin' : ''}
							/>
						</span>
						<div class="min-w-0 flex-1">
							<div class="text-[12px] leading-relaxed {textColor(entry.status)}">
								{#if entry.status === 'in_progress' && entry.activeForm}
									{entry.activeForm}
								{:else}
									{entry.content}
								{/if}
							</div>
						</div>
					</div>
				{/each}
			</div>
		{/if}
	</div>
{/if}
