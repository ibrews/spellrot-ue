<script lang="ts">
	import Icon from '$lib/components/Icon.svelte';
	import {
		ArrowDown01Icon,
		ArrowUp01Icon,
		Loading03Icon,
		CheckmarkCircle02Icon,
		Cancel01Icon,
		Image01Icon,
		ThreeDMoveIcon
	} from '@hugeicons/core-free-icons';
	import type { ContentBlock } from '$lib/bridge.js';

	let {
		block,
		resultBlock
	}: {
		block: ContentBlock;
		resultBlock?: ContentBlock;
	} = $props();

	let expanded = $state(false);

	let status = $derived(
		!resultBlock
			? 'running'
			: resultBlock.toolSuccess !== false
				? 'success'
				: 'error'
	);

	// ── Parse arguments ─────────────────────────────────────────────

	let args = $derived.by(() => {
		if (!block.toolArguments) return null;
		try {
			return JSON.parse(block.toolArguments) as {
				action?: string;
				asset_type?: string;
				prompt?: string;
				negative_prompt?: string;
				art_style?: string;
				ai_model?: string;
				job_id?: string;
				job_type?: string;
				wait?: boolean;
				timeout?: number;
				target_polycount?: number;
				preview_task_id?: string;
				import_path?: string;
				aspect_ratio?: string;
				model?: string;
			};
		} catch {
			return null;
		}
	});

	let action = $derived(args?.action ?? '');
	let assetType = $derived(args?.asset_type ?? args?.job_type ?? '');
	let prompt = $derived(args?.prompt ?? '');
	let artStyle = $derived(args?.art_style ?? '');
	let jobId = $derived(args?.job_id ?? '');

	let is3D = $derived(assetType === 'model_3d' || assetType === 'text_to_3d' || assetType === 'image_to_3d');
	let isImage = $derived(assetType === 'image' || (!is3D && !assetType));

	// ── Parse result ────────────────────────────────────────────────

	let resultText = $derived(resultBlock?.toolResult ?? '');

	let hasImages = $derived(
		(resultBlock?.images && resultBlock.images.length > 0) ?? false
	);

	let primaryImage = $derived(resultBlock?.images?.[0] ?? null);

	/** Extract progress percentage from result like "99% complete" or "SUCCEEDED (100%)" */
	let progress = $derived.by(() => {
		if (!resultText) return null;
		const match = resultText.match(/(\d+)%/);
		return match ? parseInt(match[1]) : null;
	});

	/** Check if job succeeded */
	let jobSucceeded = $derived(resultText.includes('SUCCEEDED'));

	/** Check if job is still in progress */
	let jobInProgress = $derived(resultText.includes('IN_PROGRESS') || resultText.includes('PENDING'));

	/** Extract job stage from result */
	let jobStage = $derived.by(() => {
		if (!resultText) return '';
		const match = resultText.match(/stage:\s*(\S+)/);
		return match ? match[1] : '';
	});

	/** Extract job type from result */
	let resultJobType = $derived.by(() => {
		if (!resultText) return '';
		const match = resultText.match(/job_type:\s*(\S+)/);
		return match ? match[1] : '';
	});

	// ── Display logic ───────────────────────────────────────────────

	/** Heading text based on action and asset type */
	let heading = $derived.by(() => {
		if (action === 'create') {
			return is3D ? 'Generate 3D Model' : 'Generate Image';
		}
		if (action === 'check') {
			if (jobSucceeded) return is3D ? '3D Model Ready' : 'Image Ready';
			if (jobInProgress) return is3D ? '3D Model Status' : 'Image Status';
			return 'Check Status';
		}
		if (action === 'import') return 'Import Asset';
		return is3D ? 'Generate 3D Model' : 'Generate Image';
	});

	let headingIcon = $derived(is3D ? ThreeDMoveIcon : Image01Icon);

	/** Status badge */
	let actionBadge = $derived.by(() => {
		if (action === 'create') return { text: 'Create', color: 'text-blue-400 bg-blue-500/15' };
		if (action === 'check' && jobSucceeded) return { text: 'Complete', color: 'text-emerald-400 bg-emerald-500/15' };
		if (action === 'check' && jobInProgress) return { text: progress ? `${progress}%` : 'In Progress', color: 'text-amber-400 bg-amber-500/15' };
		if (action === 'check') return { text: 'Check', color: 'text-sky-400 bg-sky-500/15' };
		if (action === 'import') return { text: 'Import', color: 'text-violet-400 bg-violet-500/15' };
		return { text: action || 'Generate', color: 'text-neutral-400 bg-neutral-500/15' };
	});

	let statusColor = $derived(
		status === 'running'
			? 'text-blue-400'
			: status === 'success'
				? 'text-emerald-400'
				: 'text-red-400'
	);

	/** Parse NEXT STEPS section from result */
	let nextSteps = $derived.by(() => {
		if (!resultText) return '';
		const idx = resultText.indexOf('NEXT STEPS:');
		if (idx === -1) {
			const nextIdx = resultText.indexOf('NEXT:');
			if (nextIdx === -1) return '';
			return resultText.substring(nextIdx).trim();
		}
		return resultText.substring(idx).trim();
	});

	/** Main result text (without NEXT STEPS) */
	let mainResult = $derived.by(() => {
		if (!resultText) return '';
		let text = resultText;
		const idx = text.indexOf('NEXT STEPS:');
		if (idx !== -1) text = text.substring(0, idx);
		const nextIdx = text.indexOf('NEXT:');
		if (nextIdx !== -1) text = text.substring(0, nextIdx);
		return text.trim();
	});

	/** Auto-expand when there's an image to show */
	let shouldAutoExpand = $derived(hasImages && jobSucceeded);

	function imageDataUrl(base64: string, mimeType: string): string {
		return `data:${mimeType};base64,${base64}`;
	}
</script>

<div class="my-2.5 w-full overflow-hidden rounded-xl border border-border bg-card/50">
	<!-- Header -->
	<div class="flex w-full items-center gap-2.5 px-3.5 py-2.5 text-[13px]">
		<!-- Status icon -->
		<span class="flex h-4 w-4 shrink-0 items-center justify-center {statusColor}">
			<Icon
				icon={status === 'running' ? Loading03Icon : headingIcon}
				size={15}
				strokeWidth={1.5}
				class={status === 'running' ? 'animate-spin' : ''}
			/>
		</span>

		<!-- Title -->
		<span class="flex-1 min-w-0 truncate font-medium text-foreground">
			{heading}
		</span>

		<!-- Action badge -->
		<span class="shrink-0 rounded-md px-2 py-0.5 text-[10px] font-medium {actionBadge.color}">
			{actionBadge.text}
		</span>

		<!-- Art style badge (create only) -->
		{#if action === 'create' && artStyle}
			<span class="shrink-0 rounded-md bg-secondary/60 px-1.5 py-0.5 text-[10px] text-muted-foreground/70">
				{artStyle}
			</span>
		{/if}

		{#if status === 'error'}
			<span class="shrink-0 rounded-md bg-red-500/15 px-1.5 py-0.5 text-[10px] font-medium text-red-400">
				failed
			</span>
		{/if}

		<!-- Expand/collapse -->
		<button
			class="shrink-0 rounded p-1 text-muted-foreground/60 transition-colors hover:bg-accent/30 hover:text-foreground"
			onclick={() => (expanded = !expanded)}
		>
			<Icon icon={expanded || shouldAutoExpand ? ArrowUp01Icon : ArrowDown01Icon} size={12} strokeWidth={1.5} />
		</button>
	</div>

	<!-- Prompt (shown for create action) -->
	{#if action === 'create' && prompt}
		<div class="border-t border-border/50 px-3.5 py-2">
			<p class="text-[12px] leading-relaxed text-foreground/70 italic">"{prompt}"</p>
		</div>
	{/if}

	<!-- Status info for completed check that found job still running -->
	{#if status === 'success' && jobInProgress && action === 'check'}
		<div class="border-t border-border/50 px-3.5 py-2">
			<div class="flex items-center gap-2">
				<span class="font-mono text-[11px] text-muted-foreground/70">
					Status polled — {progress !== null ? `${progress}% complete` : 'still processing'}
				</span>
				{#if jobStage}
					<span class="text-[10px] text-muted-foreground/50">({jobStage})</span>
				{/if}
			</div>
		</div>
	{:else if status === 'running'}
		<div class="border-t border-border/50 px-3.5 py-3 flex items-center justify-center">
			<span class="font-mono text-[11px] text-muted-foreground animate-pulse">
				{action === 'create' ? 'Starting generation...' : action === 'check' ? 'Polling status...' : action === 'import' ? 'Importing asset...' : 'Processing...'}
			</span>
		</div>
	{/if}

	<!-- Image preview (auto-shown when job succeeds with image) -->
	{#if hasImages && primaryImage && (shouldAutoExpand || expanded)}
		<div class="border-t border-border/50">
			<div class="relative bg-black/30">
				<img
					src={imageDataUrl(primaryImage.base64, primaryImage.mimeType)}
					alt={prompt || 'Generated asset'}
					class="w-full h-auto object-contain"
					style="max-height: 440px;"
				/>
			</div>
		</div>
	{:else if hasImages && primaryImage && !expanded}
		<!-- Collapsed thumbnail -->
		<button class="block w-full border-t border-border/50" onclick={() => (expanded = true)}>
			<div class="relative bg-black/30">
				<img
					src={imageDataUrl(primaryImage.base64, primaryImage.mimeType)}
					alt={prompt || 'Generated asset'}
					class="w-full h-auto object-contain"
					style="max-height: 120px;"
				/>
			</div>
		</button>
	{/if}

	<!-- Result text (expanded or always for non-image results) -->
	{#if expanded || shouldAutoExpand}
		{#if mainResult && status !== 'running'}
			<div class="border-t border-border/50 px-3.5 py-2">
				<pre class="font-mono text-[11px] leading-relaxed text-foreground/60 whitespace-pre-wrap">{mainResult}</pre>
			</div>
		{/if}
	{/if}

	<!-- Error state -->
	{#if status === 'error' && resultText}
		<div class="border-t border-border/50 px-3.5 py-2.5">
			<pre class="font-mono text-[11px] leading-relaxed text-red-400/80 whitespace-pre-wrap">{resultText}</pre>
		</div>
	{/if}
</div>
