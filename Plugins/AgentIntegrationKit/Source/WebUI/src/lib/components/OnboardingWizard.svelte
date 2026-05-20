<script lang="ts">
	import Icon from '$lib/components/Icon.svelte';
	import { SparklesIcon, Settings02Icon, ArrowRight01Icon } from '@hugeicons/core-free-icons';
	import { skipOnboarding, completeOnboarding } from '$lib/stores/onboarding.js';
	import { openSettings } from '$lib/stores/settings.js';

	function goToSettings() {
		skipOnboarding();
		openSettings();
	}
</script>

<div class="flex flex-1 flex-col items-center justify-center px-6">
	<div class="flex w-full max-w-md flex-col items-center gap-8">
		<!-- Icon -->
		<div class="relative flex items-center justify-center">
			<div class="absolute h-24 w-24 rounded-full bg-gradient-to-br from-violet-500/8 to-amber-500/8 blur-xl"></div>
			<div class="relative flex h-20 w-20 items-center justify-center rounded-2xl border border-border/30 bg-card/40 shadow-lg shadow-black/20 backdrop-blur-sm">
				<Icon icon={SparklesIcon} size={36} strokeWidth={1.2} class="text-foreground/50" />
			</div>
		</div>

		<!-- Text -->
		<div class="flex flex-col items-center gap-3 text-center">
			<h1 class="text-[22px] font-semibold tracking-tight text-foreground/90">
				Welcome to Agent Integration Kit
			</h1>
			<p class="max-w-xs text-[13.5px] leading-relaxed text-muted-foreground/50">
				Connect AI coding agents to Unreal Engine. Install agents from the ACP Registry to get started.
			</p>
		</div>

		<!-- Actions -->
		<div class="flex flex-col items-center gap-3">
			<button
				class="group flex items-center gap-2 rounded-xl bg-[var(--ue-accent)] px-7 py-2.5 text-[14px] font-medium text-white transition-all hover:opacity-90"
				onclick={goToSettings}
			>
				<Icon icon={Settings02Icon} size={16} strokeWidth={1.5} />
				Set Up Agents
				<Icon icon={ArrowRight01Icon} size={16} strokeWidth={2} class="transition-transform group-hover:translate-x-0.5" />
			</button>
			<button
				class="text-[12px] text-muted-foreground/30 transition-colors hover:text-muted-foreground/50"
				onclick={skipOnboarding}
			>
				Skip for now
			</button>
		</div>
	</div>
</div>
