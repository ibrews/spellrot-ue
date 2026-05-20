<script lang="ts">
	import Icon from '$lib/components/Icon.svelte';
	import {
		authState,
		authAgentName,
		authError,
		startLogin,
		resetAuth
	} from '$lib/stores/auth.js';
	import { agents, withAlpha } from '$lib/stores/agents.js';

	// Resolve the agent object for branding
	let agent = $derived($agents.find(a => a.name === $authAgentName));
	let agentColor = $derived(agent?.color ?? '#888');
	let agentDisplayName = $derived(agent?.shortName ?? ($authAgentName || 'Agent'));
</script>

{#if $authState !== 'none'}
	<div class="mx-auto mb-5 w-full max-w-3xl">
		<div
			class="overflow-hidden rounded-xl border border-border/40"
			style="background: linear-gradient(135deg, #252525, #1f1f1f);"
		>
			{#if $authState === 'required'}
				<div class="flex items-start gap-4 p-5">
					<!-- Agent icon -->
					<div
						class="flex h-11 w-11 shrink-0 items-center justify-center rounded-xl"
						style="background-color: {withAlpha(agentColor, 0.12)};"
					>
						{#if agent?.iconUrl}
							<span style="color: {agentColor};">
								<img src={agent.iconUrl} alt="" class="h-5.5 w-5.5 invert opacity-70" />
							</span>
						{:else}
							<span
								class="flex h-7 w-7 items-center justify-center rounded-lg text-[10px] font-bold text-white"
								style="background-color: {agentColor};"
							>
								{agent?.letter ?? '?'}
							</span>
						{/if}
					</div>
					<div class="flex-1 min-w-0">
						<h3 class="text-[14px] font-medium text-foreground">
							Sign in to {agentDisplayName}
						</h3>
						<p class="mt-1 text-[13px] leading-relaxed text-muted-foreground/50">
							Authentication is required to continue. This will open your browser to complete sign-in.
						</p>
						{#if $authError}
							<p class="mt-1 text-[12px] leading-relaxed text-muted-foreground/40">{$authError}</p>
						{/if}
						<div class="mt-3.5 flex items-center gap-2.5">
							<button
								class="rounded-lg px-4 py-2 text-[13px] font-medium text-white transition-all hover:brightness-110 active:scale-[0.98]"
								style="background-color: {agentColor};"
								onclick={() => startLogin()}
							>
								Sign in with browser
							</button>
							<button
								class="rounded-lg border border-border/40 bg-white/[0.03] px-3 py-2 text-[13px] text-muted-foreground/60 transition-colors hover:bg-white/[0.06] hover:text-muted-foreground"
								onclick={() => resetAuth()}
							>
								Dismiss
							</button>
						</div>
					</div>
				</div>

			{:else if $authState === 'logging_in'}
				<div class="flex items-center gap-3.5 px-5 py-4">
					<div
						class="h-5 w-5 animate-spin rounded-full border-2 border-t-transparent"
						style="border-color: {withAlpha(agentColor, 0.3)}; border-top-color: {agentColor};"
					></div>
					<div>
						<span class="text-[14px] font-medium text-foreground">Waiting for sign-in...</span>
						<p class="mt-0.5 text-[12px] text-muted-foreground/40">Complete authentication in your browser, then return here.</p>
					</div>
				</div>

			{:else if $authState === 'success'}
				<div class="flex items-center gap-3 px-5 py-4">
					<div class="flex h-7 w-7 items-center justify-center rounded-full bg-emerald-500/10">
						<svg class="h-4 w-4 text-emerald-400" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2.5">
							<path stroke-linecap="round" stroke-linejoin="round" d="M5 13l4 4L19 7" />
						</svg>
					</div>
					<span class="text-[14px] font-medium text-emerald-400">
						Signed in to {agentDisplayName}
					</span>
				</div>

			{:else if $authState === 'error'}
				<div class="flex items-start gap-4 p-5">
					<div class="flex h-9 w-9 shrink-0 items-center justify-center rounded-xl bg-red-500/8">
						<svg class="h-4.5 w-4.5 text-red-400/80" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2">
							<path stroke-linecap="round" stroke-linejoin="round" d="M12 9v3.75m9-.75a9 9 0 11-18 0 9 9 0 0118 0zm-9 3.75h.008v.008H12v-.008z" />
						</svg>
					</div>
					<div class="flex-1 min-w-0">
						<h3 class="text-[14px] font-medium text-red-400/90">Sign-in failed</h3>
						{#if $authError}
							<p class="mt-1 text-[12px] leading-relaxed text-muted-foreground/40">{$authError}</p>
						{/if}
						<div class="mt-3 flex items-center gap-2.5">
							<button
								class="rounded-lg px-4 py-2 text-[13px] font-medium text-white transition-all hover:brightness-110 active:scale-[0.98]"
								style="background-color: {agentColor};"
								onclick={() => startLogin()}
							>
								Try again
							</button>
							<button
								class="rounded-lg border border-border/40 bg-white/[0.03] px-3 py-2 text-[13px] text-muted-foreground/60 transition-colors hover:bg-white/[0.06] hover:text-muted-foreground"
								onclick={() => resetAuth()}
							>
								Dismiss
							</button>
						</div>
					</div>
				</div>
			{/if}
		</div>
	</div>
{/if}
