<script lang="ts">
	import { onMount } from 'svelte';
	import Icon from '$lib/components/Icon.svelte';
	import {
		Image02Icon,
		CubeIcon,
		PaintBrushIcon,
		BodyPartMuscleIcon,
		RunningShoesIcon,
		RepeatIcon,
		ArrowRight01Icon,
		RefreshIcon,
		Loading03Icon
	} from '@hugeicons/core-free-icons';
	import {
		providers,
		providersLoaded,
		uniqueActions,
		jobs,
		loadProviders,
		submitJob,
		getProvidersForAction,
		getActionDescriptor,
		loadBalance,
		balances,
		type UniqueAction,
		type StudioJob
	} from '$lib/stores/studio.js';

	// ── Icon mapping for known action types ──────────────────────────
	const actionIcons: Record<string, { icon: typeof Image02Icon; color: string }> = {
		text_to_image:     { icon: Image02Icon, color: '#f59e0b' },
		image_to_image:    { icon: Image02Icon, color: '#e67e22' },
		text_to_3d:        { icon: CubeIcon, color: '#3b82f6' },
		image_to_3d:       { icon: CubeIcon, color: '#8b5cf6' },
		multi_image_to_3d: { icon: CubeIcon, color: '#7c3aed' },
		refine:            { icon: PaintBrushIcon, color: '#10b981' },
		retexture:         { icon: PaintBrushIcon, color: '#14b8a6' },
		remesh:            { icon: RepeatIcon, color: '#6366f1' },
		rig:               { icon: BodyPartMuscleIcon, color: '#ef4444' },
		animate:           { icon: RunningShoesIcon, color: '#ec4899' },
		balance:           { icon: RefreshIcon, color: '#6b7280' }
	};

	function getActionVisual(actionId: string) {
		return actionIcons[actionId] ?? { icon: CubeIcon, color: '#6b7280' };
	}

	/** Categorize by output hints */
	function getCategory(action: UniqueAction): 'image' | '3d' | 'pipeline' | 'utility' {
		if (action.outputHints.includes('image')) return 'image';
		if (action.outputHints.includes('animation')) return 'pipeline';
		if (action.outputHints.includes('model')) return '3d';
		if (action.actionId === 'rig') return 'pipeline';
		if (action.actionId === 'balance') return 'utility';
		return '3d';
	}

	// ── State ────────────────────────────────────────────────────────
	let selectedAction: UniqueAction | null = $state(null);
	let selectedProviderId = $state('');
	let filterCategory: 'all' | 'image' | '3d' | 'pipeline' = $state('all');
	let formValues: Record<string, unknown> = $state({});
	let isSubmitting = $state(false);

	let filteredActions = $derived(
		$uniqueActions.filter(a => {
			if (a.actionId === 'balance') return false; // hide utility actions
			if (filterCategory === 'all') return true;
			return getCategory(a) === filterCategory;
		})
	);

	let activeSchema = $derived.by(() => {
		if (!selectedAction || !selectedProviderId) return null;
		const desc = getActionDescriptor(selectedProviderId, selectedAction.actionId);
		return desc?.paramsSchema ?? null;
	});

	let activeJobs = $derived($jobs.filter(j => j.status === 'running' || j.status === 'pending'));
	let completedJobs = $derived($jobs.filter(j => j.status === 'succeeded' || j.status === 'failed'));

	// ── Lifecycle ────────────────────────────────────────────────────
	onMount(() => {
		if (!$providersLoaded) {
			loadProviders();
		}
		// Load balances for all known providers
		if ($providersLoaded) {
			for (const p of $providers) {
				loadBalance(p.id);
			}
		}
	});

	$effect(() => {
		if ($providersLoaded && $providers.length > 0) {
			for (const p of $providers) {
				loadBalance(p.id);
			}
		}
	});

	// ── Actions ──────────────────────────────────────────────────────
	function openAction(action: UniqueAction) {
		selectedAction = action;
		selectedProviderId = action.providers[0]?.id ?? '';
		formValues = {};
		// Set defaults from schema
		if (selectedProviderId) {
			const desc = getActionDescriptor(selectedProviderId, action.actionId);
			if (desc?.paramsSchema?.properties) {
				for (const [key, prop] of Object.entries(desc.paramsSchema.properties)) {
					if (prop.default !== undefined) {
						formValues[key] = prop.default;
					}
				}
			}
		}
	}

	function closeAction() {
		selectedAction = null;
		selectedProviderId = '';
		formValues = {};
	}

	function switchProvider(providerId: string) {
		selectedProviderId = providerId;
		formValues = {};
		const desc = getActionDescriptor(providerId, selectedAction?.actionId ?? '');
		if (desc?.paramsSchema?.properties) {
			for (const [key, prop] of Object.entries(desc.paramsSchema.properties)) {
				if (prop.default !== undefined) {
					formValues[key] = prop.default;
				}
			}
		}
	}

	async function handleSubmit() {
		if (!selectedAction || !selectedProviderId || isSubmitting) return;
		isSubmitting = true;
		const prompt = (formValues['prompt'] as string) ?? (formValues['text_prompt'] as string) ?? '';
		await submitJob(selectedProviderId, selectedAction.actionId, { ...formValues }, prompt);
		isSubmitting = false;
	}

	// ── Helpers ──────────────────────────────────────────────────────
	function statusColor(status: string): string {
		switch (status) {
			case 'succeeded': return 'bg-emerald-400';
			case 'running': return 'bg-blue-400 animate-pulse';
			case 'pending': return 'bg-muted-foreground/40';
			case 'failed': return 'bg-red-400';
			case 'cancelled': return 'bg-muted-foreground/30';
			default: return 'bg-muted-foreground/30';
		}
	}

	function statusLabel(status: string): string {
		switch (status) {
			case 'succeeded': return 'Complete';
			case 'running': return 'Generating';
			case 'pending': return 'Queued';
			case 'failed': return 'Failed';
			case 'cancelled': return 'Cancelled';
			default: return status;
		}
	}

	function timeAgo(ts: number): string {
		const diff = Date.now() - ts;
		const mins = Math.floor(diff / 60000);
		if (mins < 1) return 'just now';
		if (mins < 60) return `${mins}m ago`;
		const hrs = Math.floor(mins / 60);
		if (hrs < 24) return `${hrs}h ago`;
		return `${Math.floor(hrs / 24)}d ago`;
	}

	function formatActionId(id: string): string {
		return id.replace(/_/g, ' ').replace(/\b\w/g, c => c.toUpperCase());
	}

	/** Render a schema property as a form field */
	function getFieldType(prop: { type: string; enum?: string[] }): 'select' | 'text' | 'number' | 'checkbox' | 'textarea' {
		if (prop.enum && prop.enum.length > 0) return 'select';
		if (prop.type === 'boolean') return 'checkbox';
		if (prop.type === 'integer' || prop.type === 'number') return 'number';
		// Heuristic: long text fields
		if (prop.type === 'string') return 'text';
		return 'text';
	}

	function isPromptField(key: string): boolean {
		return key === 'prompt' || key === 'text_prompt' || key === 'negative_prompt' || key === 'texture_prompt';
	}
</script>

<div class="flex h-full w-full flex-col overflow-hidden bg-background">
	<!-- Studio header -->
	<header class="flex h-10 shrink-0 items-center justify-between border-b border-border bg-[#222] px-4">
		<div class="flex items-center gap-3">
			<h1 class="text-[13px] font-medium text-foreground">Studio</h1>
			<div class="h-4 w-px bg-border/60"></div>
			<!-- Category filters -->
			<div class="flex items-center gap-0.5">
				{#each [
					{ id: 'all', label: 'All' },
					{ id: 'image', label: 'Images' },
					{ id: '3d', label: '3D Models' },
					{ id: 'pipeline', label: 'Pipeline' }
				] as cat}
					<button
						class="rounded-md px-2 py-1 text-[11px] font-medium transition-colors
							{filterCategory === cat.id
								? 'bg-accent text-foreground'
								: 'text-muted-foreground hover:text-foreground'}"
						onclick={() => filterCategory = cat.id as typeof filterCategory}
					>
						{cat.label}
					</button>
				{/each}
			</div>
		</div>
		<div class="flex items-center gap-3 text-[11px] text-muted-foreground/60">
			{#each Object.entries($balances) as [pid, bal]}
				{@const provider = $providers.find(p => p.id === pid)}
				{#if bal >= 0 && provider}
					<span class="rounded bg-secondary/50 px-1.5 py-0.5 text-[10px]">
						{provider.displayName}: {bal} credits
					</span>
				{/if}
			{/each}
			<span>{activeJobs.length} active</span>
			<span class="text-border">|</span>
			<span>{completedJobs.length} completed</span>
		</div>
	</header>

	<div class="flex min-h-0 flex-1 overflow-hidden">
		<div class="flex-1 overflow-y-auto px-6 py-5">
			{#if !$providersLoaded}
				<!-- Loading state -->
				<div class="flex items-center justify-center py-20">
					<div class="flex items-center gap-2 text-[13px] text-muted-foreground/60">
						<Icon icon={Loading03Icon} size={16} class="animate-spin" />
						<span>Loading providers...</span>
					</div>
				</div>
			{:else if !selectedAction}
				<!-- Action cards grid -->
				<div class="mx-auto max-w-4xl">
					{#if filteredActions.length === 0}
						<div class="flex flex-col items-center justify-center py-20 text-center">
							<p class="text-[13px] text-muted-foreground/60">No generative providers registered.</p>
							<p class="mt-1 text-[11px] text-muted-foreground/40">Configure API keys in Settings to enable generation.</p>
						</div>
					{:else}
						<div class="mb-3 flex items-center gap-2">
							<span class="text-[11px] font-semibold uppercase tracking-widest text-muted-foreground/50">Actions</span>
							<div class="h-px flex-1 bg-border/30"></div>
							<span class="text-[10px] text-muted-foreground/40">{filteredActions.length} available</span>
						</div>

						<div class="grid grid-cols-1 gap-3 sm:grid-cols-2 lg:grid-cols-3">
							{#each filteredActions as action (action.actionId)}
								{@const visual = getActionVisual(action.actionId)}
								<button
									class="group relative flex flex-col rounded-xl border border-border/60 bg-card/40 p-4 text-left transition-all duration-150 hover:border-border hover:bg-card/80 active:scale-[0.98]"
									onclick={() => openAction(action)}
								>
									<div class="mb-2.5 flex items-start justify-between">
										<div
											class="flex h-9 w-9 items-center justify-center rounded-lg"
											style="background-color: {visual.color}15; color: {visual.color};"
										>
											<Icon icon={visual.icon} size={18} strokeWidth={1.5} />
										</div>
										<Icon
											icon={ArrowRight01Icon}
											size={14}
											strokeWidth={1.5}
											class="mt-1 text-muted-foreground/0 transition-all duration-150 group-hover:text-muted-foreground/60 group-hover:translate-x-0.5"
										/>
									</div>

									<h3 class="mb-1 text-[13px] font-medium text-foreground">{formatActionId(action.actionId)}</h3>
									<p class="mb-3 line-clamp-2 text-[11px] leading-relaxed text-muted-foreground/70">{action.description}</p>

									<div class="mt-auto flex items-center gap-1.5">
										{#each action.providers as provider}
											<span class="rounded bg-secondary/60 px-1.5 py-0.5 text-[9px] font-medium text-muted-foreground/60">{provider.name}</span>
										{/each}
										{#if action.providers[0]?.creditCost}
											<span class="ml-auto text-[9px] text-muted-foreground/40">{action.providers[0].creditCost}</span>
										{/if}
									</div>
								</button>
							{/each}
						</div>
					{/if}

					<!-- Jobs section -->
					{#if $jobs.length > 0}
						<div class="mt-8">
							<div class="mb-3 flex items-center gap-2">
								<span class="text-[11px] font-semibold uppercase tracking-widest text-muted-foreground/50">Jobs</span>
								<div class="h-px flex-1 bg-border/30"></div>
								<span class="text-[10px] text-muted-foreground/40">{$jobs.length} total</span>
							</div>

							<div class="space-y-2">
								{#each $jobs as job (job.jobId)}
									<div class="flex items-center gap-3 rounded-lg border border-border/40 bg-card/30 px-4 py-3 transition-colors hover:bg-card/50">
										<span class="h-2 w-2 shrink-0 rounded-full {statusColor(job.status)}"></span>
										<div class="min-w-0 flex-1">
											<div class="flex items-center gap-2">
												<span class="text-[12px] font-medium text-foreground">{formatActionId(job.actionId)}</span>
												<span class="rounded bg-secondary/40 px-1.5 py-0.5 text-[9px] text-muted-foreground/50">{job.providerId}</span>
											</div>
											<p class="truncate text-[11px] text-muted-foreground/60">{job.prompt || job.jobId}</p>
										</div>
										<div class="flex shrink-0 items-center gap-3">
											{#if job.status === 'running'}
												<div class="flex items-center gap-2">
													<div class="h-1 w-16 overflow-hidden rounded-full bg-secondary/60">
														<div class="h-full rounded-full bg-blue-400 transition-all" style="width: {job.progress}%"></div>
													</div>
													<span class="text-[10px] tabular-nums text-blue-400">{job.progress}%</span>
												</div>
											{:else}
												<span class="text-[10px] text-muted-foreground/50">{statusLabel(job.status)}</span>
											{/if}
											<span class="text-[10px] text-muted-foreground/30">{timeAgo(job.createdAt)}</span>
										</div>
									</div>
								{/each}
							</div>
						</div>
					{/if}
				</div>
			{:else}
				<!-- Dynamic action form -->
				<div class="mx-auto max-w-2xl">
					<button
						class="mb-5 flex items-center gap-1.5 text-[12px] text-muted-foreground/60 transition-colors hover:text-foreground"
						onclick={closeAction}
					>
						<span class="rotate-180"><Icon icon={ArrowRight01Icon} size={12} strokeWidth={2} /></span>
						Back to actions
					</button>

					<div class="rounded-xl border border-border/60 bg-card/40 p-6">
						<!-- Action header -->
						<div class="mb-6 flex items-start gap-4">
							<div
								class="flex h-11 w-11 items-center justify-center rounded-xl"
								style="background-color: {getActionVisual(selectedAction.actionId).color}15; color: {getActionVisual(selectedAction.actionId).color};"
							>
								<Icon icon={getActionVisual(selectedAction.actionId).icon} size={22} strokeWidth={1.5} />
							</div>
							<div>
								<h2 class="text-[16px] font-medium text-foreground">{formatActionId(selectedAction.actionId)}</h2>
								<p class="text-[12px] text-muted-foreground/60">{selectedAction.description}</p>
							</div>
						</div>

						<!-- Provider selector -->
						{#if selectedAction.providers.length > 1}
							<div class="mb-5">
								<label class="mb-1.5 block text-[11px] font-medium uppercase tracking-wider text-muted-foreground/50">Provider</label>
								<div class="flex gap-2">
									{#each selectedAction.providers as provider}
										<button
											class="rounded-lg border px-3 py-2 text-[12px] font-medium transition-colors
												{selectedProviderId === provider.id
													? 'border-[var(--ue-accent)]/40 bg-[var(--ue-accent)]/10 text-foreground'
													: 'border-border/60 text-muted-foreground hover:border-border hover:text-foreground'}"
											onclick={() => switchProvider(provider.id)}
										>
											{provider.name}
											{#if provider.creditCost}
												<span class="ml-1 text-[10px] opacity-50">({provider.creditCost})</span>
											{/if}
										</button>
									{/each}
								</div>
							</div>
						{:else if selectedAction.providers.length === 1}
							<div class="mb-4 flex items-center gap-2 text-[11px] text-muted-foreground/50">
								<span>Provider:</span>
								<span class="rounded bg-secondary/60 px-1.5 py-0.5 font-medium">{selectedAction.providers[0].name}</span>
								{#if selectedAction.providers[0].creditCost}
									<span class="text-[10px] opacity-60">{selectedAction.providers[0].creditCost}</span>
								{/if}
							</div>
						{/if}

						<!-- Dynamic form fields from schema -->
						{#if activeSchema?.properties}
							{@const properties = Object.entries(activeSchema.properties)}
							{@const required = new Set(activeSchema.required ?? [])}

							{#each properties as [key, prop]}
								{@const fieldType = getFieldType(prop)}
								{@const isRequired = required.has(key)}
								{@const isPrompt = isPromptField(key)}

								<div class="mb-4">
									<label class="mb-1.5 flex items-baseline gap-1 text-[11px] font-medium uppercase tracking-wider text-muted-foreground/50">
										{key.replace(/_/g, ' ')}
										{#if isRequired}
											<span class="text-[var(--ue-accent)] normal-case">*</span>
										{/if}
									</label>

									{#if prop.description}
										<p class="mb-1.5 text-[10px] text-muted-foreground/40">{prop.description}</p>
									{/if}

									{#if isPrompt}
										<textarea
											class="w-full resize-none rounded-lg border border-border/60 bg-input px-3 py-2.5 text-[13px] text-foreground placeholder:text-muted-foreground/40 focus:border-[var(--ue-accent-muted)] focus:outline-none"
											rows="3"
											placeholder="Describe what you want to create..."
											value={formValues[key] as string ?? ''}
											oninput={(e) => formValues[key] = (e.target as HTMLTextAreaElement).value}
										></textarea>
									{:else if fieldType === 'select'}
										<div class="flex flex-wrap gap-2">
											{#each prop.enum ?? [] as option}
												<button
													class="rounded-lg border px-3 py-1.5 text-[11px] font-medium transition-colors
														{(formValues[key] ?? prop.default) === option
															? 'border-[var(--ue-accent)]/40 bg-[var(--ue-accent)]/10 text-foreground'
															: 'border-border/60 text-muted-foreground hover:border-border hover:text-foreground'}"
													onclick={() => formValues[key] = option}
												>
													{option}
												</button>
											{/each}
										</div>
									{:else if fieldType === 'checkbox'}
										<label class="flex items-center gap-2 text-[12px] text-muted-foreground/70">
											<input
												type="checkbox"
												checked={formValues[key] as boolean ?? prop.default ?? false}
												onchange={(e) => formValues[key] = (e.target as HTMLInputElement).checked}
												class="rounded border-border"
											/>
											{prop.description ?? key.replace(/_/g, ' ')}
										</label>
									{:else if fieldType === 'number'}
										<input
											type="number"
											value={formValues[key] as number ?? prop.default ?? ''}
											min={prop.minimum}
											max={prop.maximum}
											oninput={(e) => formValues[key] = parseInt((e.target as HTMLInputElement).value)}
											class="w-full rounded-lg border border-border/60 bg-input px-3 py-2 text-[13px] text-foreground focus:border-[var(--ue-accent-muted)] focus:outline-none"
										/>
									{:else}
										<input
											type="text"
											value={formValues[key] as string ?? prop.default ?? ''}
											oninput={(e) => formValues[key] = (e.target as HTMLInputElement).value}
											class="w-full rounded-lg border border-border/60 bg-input px-3 py-2 text-[13px] text-foreground placeholder:text-muted-foreground/40 focus:border-[var(--ue-accent-muted)] focus:outline-none"
										/>
									{/if}
								</div>
							{/each}
						{:else}
							<p class="my-4 text-[12px] italic text-muted-foreground/40">No configurable parameters for this action.</p>
						{/if}

						<!-- Generate button -->
						<button
							class="mt-2 w-full rounded-lg bg-[var(--ue-accent)] px-4 py-2.5 text-[13px] font-medium text-white shadow-[0_0_12px_rgba(43,140,235,0.25)] transition-all hover:bg-[var(--ue-accent-hover)] hover:shadow-[0_0_16px_rgba(43,140,235,0.35)] active:scale-[0.99] disabled:opacity-40 disabled:cursor-not-allowed"
							onclick={handleSubmit}
							disabled={isSubmitting}
						>
							{#if isSubmitting}
								<span class="flex items-center justify-center gap-2">
									<Icon icon={Loading03Icon} size={14} class="animate-spin" />
									Submitting...
								</span>
							{:else}
								Generate
							{/if}
						</button>
					</div>
				</div>
			{/if}
		</div>
	</div>
</div>
