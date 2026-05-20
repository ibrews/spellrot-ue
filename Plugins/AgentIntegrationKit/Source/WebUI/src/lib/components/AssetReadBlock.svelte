<script lang="ts">
	import Icon from '$lib/components/Icon.svelte';
	import {
		ArrowDown01Icon,
		ArrowUp01Icon,
		CheckmarkCircle02Icon,
		Cancel01Icon,
		Loading03Icon,
		Layers01Icon,
		Copy01Icon
	} from '@hugeicons/core-free-icons';
	import type { ContentBlock } from '$lib/bridge.js';
	import { copyToClipboard, openPath } from '$lib/bridge.js';

	let {
		block,
		resultBlock
	}: {
		block: ContentBlock;
		resultBlock?: ContentBlock;
	} = $props();

	let expanded = $state(false);
	let copied = $state(false);

	let status = $derived(
		!resultBlock
			? 'running'
			: resultBlock.toolSuccess !== false
				? 'success'
				: 'error'
	);

	let statusColor = $derived(
		status === 'running'
			? 'text-blue-400'
			: status === 'success'
				? 'text-emerald-400'
				: 'text-red-400'
	);

	// ── Parse arguments ─────────────────────────────────────────────

	let args = $derived.by(() => {
		if (!block.toolArguments) return null;
		try {
			return JSON.parse(block.toolArguments) as {
				name?: string;
				path?: string;
				graph?: string;
				component?: string;
				offset?: number;
				limit?: number;
				include?: string[];
			};
		} catch {
			return null;
		}
	});

	let assetName = $derived(args?.name ?? 'Unknown');
	let assetPath = $derived(args?.path ?? '');
	let graphTarget = $derived(args?.graph ?? '');
	let componentTarget = $derived(args?.component ?? '');

	/** Full asset path for opening — if name is already a /Game/ path use it, otherwise combine path + name */
	let openAssetPath = $derived(
		assetName.startsWith('/') ? assetName : (assetPath ? `${assetPath}/${assetName}` : assetName)
	);

	/** Full path for display */
	let fullPath = $derived(
		assetPath ? `${assetPath}/${assetName}` : assetName
	);

	/** Short display name — just the last part */
	let displayName = $derived(() => {
		const n = assetName;
		if (!n) return '';
		const parts = n.replace(/\\/g, '/').split('/');
		return parts[parts.length - 1] || n;
	});

	// ── Parse result for asset type + sections ──────────────────────

	let resultText = $derived(resultBlock?.toolResult ?? '');

	/** Extract asset type from first # header (e.g. "# NIAGARA_SYSTEM NS_TestVFX" → "NIAGARA_SYSTEM") */
	let assetType = $derived.by(() => {
		if (!resultText) return '';
		const match = resultText.match(/^#\s+(\S+)/m);
		return match ? match[1] : '';
	});

	/** Map asset types to display labels + accent colors */
	const ASSET_TYPE_MAP: Record<string, { label: string; color: string; bg: string }> = {
		BLUEPRINT: { label: 'Blueprint', color: 'text-blue-400', bg: 'bg-blue-500/15' },
		ANIM_BLUEPRINT: { label: 'Anim BP', color: 'text-violet-400', bg: 'bg-violet-500/15' },
		WIDGET_BLUEPRINT: { label: 'Widget BP', color: 'text-teal-400', bg: 'bg-teal-500/15' },
		NIAGARA_SYSTEM: { label: 'Niagara', color: 'text-orange-400', bg: 'bg-orange-500/15' },
		MATERIAL: { label: 'Material', color: 'text-emerald-400', bg: 'bg-emerald-500/15' },
		MATERIAL_INSTANCE: { label: 'Mat Instance', color: 'text-emerald-400', bg: 'bg-emerald-500/15' },
		STATIC_MESH: { label: 'Static Mesh', color: 'text-cyan-400', bg: 'bg-cyan-500/15' },
		SKELETON: { label: 'Skeleton', color: 'text-amber-400', bg: 'bg-amber-500/15' },
		BEHAVIOR_TREE: { label: 'BT', color: 'text-rose-400', bg: 'bg-rose-500/15' },
		STATE_TREE: { label: 'State Tree', color: 'text-pink-400', bg: 'bg-pink-500/15' },
		LEVEL_SEQUENCE: { label: 'Sequencer', color: 'text-indigo-400', bg: 'bg-indigo-500/15' },
		CONTROL_RIG: { label: 'Control Rig', color: 'text-yellow-400', bg: 'bg-yellow-500/15' },
		SOUND_CUE: { label: 'Sound', color: 'text-lime-400', bg: 'bg-lime-500/15' },
		DATATABLE: { label: 'DataTable', color: 'text-sky-400', bg: 'bg-sky-500/15' },
		STRUCT: { label: 'Struct', color: 'text-stone-400', bg: 'bg-stone-500/15' },
		ENUM: { label: 'Enum', color: 'text-stone-400', bg: 'bg-stone-500/15' },
		BLEND_SPACE: { label: 'BlendSpace', color: 'text-fuchsia-400', bg: 'bg-fuchsia-500/15' },
		ANIM_MONTAGE: { label: 'Montage', color: 'text-purple-400', bg: 'bg-purple-500/15' },
		GRAPH: { label: 'Graph', color: 'text-blue-400', bg: 'bg-blue-500/15' },
		FILE: { label: 'File', color: 'text-neutral-400', bg: 'bg-neutral-500/15' },
		ENVIRONMENT_QUERY: { label: 'EQS', color: 'text-lime-400', bg: 'bg-lime-500/15' },
		PHYSICS_ASSET: { label: 'PhysAsset', color: 'text-red-400', bg: 'bg-red-500/15' },
		GAMEPLAY_EFFECT: { label: 'GE', color: 'text-amber-400', bg: 'bg-amber-500/15' }
	};

	let typeInfo = $derived(
		ASSET_TYPE_MAP[assetType] ?? { label: assetType || 'Asset', color: 'text-muted-foreground', bg: 'bg-secondary/50' }
	);

	/** Parse result into sections for structured display */
	let sections = $derived.by(() => {
		if (!resultText) return [];
		const result: { header: string; level: number; lines: string[] }[] = [];
		let current: { header: string; level: number; lines: string[] } | null = null;

		for (const line of resultText.split('\n')) {
			const headerMatch = line.match(/^(#{1,3})\s+(.+)/);
			if (headerMatch) {
				if (current) result.push(current);
				current = { header: headerMatch[2], level: headerMatch[1].length, lines: [] };
			} else {
				if (!current) {
					current = { header: '', level: 0, lines: [] };
				}
				current.lines.push(line);
			}
		}
		if (current) result.push(current);
		return result;
	});

	/** Preview: first section header + a few body lines */
	const PREVIEW_LINES = 6;
	let resultLines = $derived(resultText ? resultText.split('\n') : []);
	let totalLines = $derived(resultLines.length);
	let previewText = $derived(
		resultLines.length > PREVIEW_LINES
			? resultLines.slice(0, PREVIEW_LINES).join('\n')
			: resultText
	);
	let hasMoreLines = $derived(resultLines.length > PREVIEW_LINES);
	let remainingLines = $derived(Math.max(0, resultLines.length - PREVIEW_LINES));

	/** Context badge — shows graph or component target if specified */
	let contextBadge = $derived.by(() => {
		if (graphTarget) return graphTarget;
		if (componentTarget) return componentTarget;
		return '';
	});

	let hasImages = $derived(
		(resultBlock?.images && resultBlock.images.length > 0) ?? false
	);

	function handleCopy() {
		if (resultText) {
			copyToClipboard(resultText);
			copied = true;
			setTimeout(() => { copied = false; }, 1500);
		}
	}

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
				icon={status === 'running' ? Loading03Icon : Layers01Icon}
				size={15}
				strokeWidth={1.5}
				class={status === 'running' ? 'animate-spin' : ''}
			/>
		</span>

		<!-- Asset name (clickable — opens in editor) -->
		<button
			class="flex-1 min-w-0 truncate text-left font-medium text-foreground underline decoration-foreground/20 underline-offset-2 transition-colors hover:text-blue-400 hover:decoration-blue-400/40"
			onclick={() => openPath(openAssetPath)}
			title="Open in editor"
		>{displayName()}</button>

		<!-- Context badge (graph or component) -->
		{#if contextBadge}
			<span class="shrink-0 rounded-md bg-secondary/60 px-1.5 py-0.5 text-[10px] font-mono text-muted-foreground">
				{contextBadge}
			</span>
		{/if}

		<!-- Asset type badge -->
		{#if assetType}
			<span class="shrink-0 rounded-md px-2 py-0.5 text-[10px] font-medium {typeInfo.color} {typeInfo.bg}">
				{typeInfo.label}
			</span>
		{/if}

		<!-- Status badges -->
		{#if status === 'error'}
			<span class="shrink-0 rounded-md bg-red-500/15 px-1.5 py-0.5 text-[10px] font-medium text-red-400">
				failed
			</span>
		{/if}

		{#if status !== 'running' && totalLines > 0}
			<span class="shrink-0 text-[10px] tabular-nums text-muted-foreground/60">
				{totalLines}L
			</span>
		{/if}

		<!-- Expand/collapse toggle -->
		<button
			class="shrink-0 rounded p-1 text-muted-foreground/60 transition-colors hover:bg-accent/30 hover:text-foreground"
			onclick={() => (expanded = !expanded)}
			title={expanded ? 'Collapse' : 'Expand'}
		>
			<Icon icon={expanded ? ArrowUp01Icon : ArrowDown01Icon} size={12} strokeWidth={1.5} />
		</button>
	</div>

	<!-- Asset path (always visible) -->
	<div class="flex items-center gap-2 border-t border-border/50 px-3.5 py-1.5">
		<button
			class="flex-1 min-w-0 truncate text-left font-mono text-[10px] text-muted-foreground/60 transition-colors hover:text-blue-400"
			onclick={(e: MouseEvent) => { e.stopPropagation(); openPath(openAssetPath); }}
			title="Open {fullPath}"
		>{fullPath}</button>
		{#if status !== 'running' && resultText}
			<button
				class="shrink-0 rounded p-0.5 text-muted-foreground/50 transition-colors hover:text-foreground"
				onclick={(e: MouseEvent) => { e.stopPropagation(); handleCopy(); }}
				title="Copy result"
			>
				{#if copied}
					<Icon icon={CheckmarkCircle02Icon} size={12} strokeWidth={1.5} class="text-emerald-400" />
				{:else}
					<Icon icon={Copy01Icon} size={12} strokeWidth={1.5} />
				{/if}
			</button>
		{/if}
	</div>

	<!-- Inline image preview when collapsed -->
	{#if !expanded && hasImages && resultBlock?.images}
		<div class="flex gap-2 overflow-x-auto border-t border-border/50 px-3.5 py-2">
			{#each resultBlock.images as img}
				<button class="shrink-0 overflow-hidden rounded-lg border border-border/50" onclick={() => (expanded = true)}>
					<img
						src={imageDataUrl(img.base64, img.mimeType)}
						alt="Asset preview"
						class="h-24 w-auto object-contain"
						style="max-width: 200px;"
					/>
				</button>
			{/each}
		</div>
	{/if}

	<!-- Result content -->
	{#if resultText}
		<div class="border-t border-border/50">
			{#if expanded}
				<!-- Full structured view -->
				<div class="max-h-[500px] overflow-auto px-3.5 py-2.5">
					{#each sections as section, i}
						{#if section.header}
							<div class="flex items-center gap-2 {i > 0 ? 'mt-3' : ''} mb-1">
								{#if section.level === 1}
									<span class="text-[11px] font-semibold uppercase tracking-wider {typeInfo.color}">{section.header}</span>
								{:else if section.level === 2}
									<span class="text-[11px] font-medium text-foreground/70">{section.header}</span>
								{:else}
									<span class="text-[11px] text-foreground/55">{section.header}</span>
								{/if}
							</div>
						{/if}
						{#if section.lines.length > 0}
							<pre class="font-mono text-[11px] leading-relaxed text-foreground/60">{section.lines.join('\n')}</pre>
						{/if}
					{/each}

					{#if hasImages && resultBlock?.images}
						<div class="mt-3 flex flex-wrap gap-2">
							{#each resultBlock.images as img}
								<div class="overflow-hidden rounded-lg border border-border/50 bg-black/20">
									<img
										src={imageDataUrl(img.base64, img.mimeType)}
										alt="Asset preview"
										class="max-h-[400px] w-auto max-w-full object-contain"
									/>
									{#if img.width > 0 && img.height > 0}
										<div class="px-2 py-1 text-[10px] text-muted-foreground/60">
											{img.width}&times;{img.height}
										</div>
									{/if}
								</div>
							{/each}
						</div>
					{/if}
				</div>
			{:else}
				<!-- Collapsed preview -->
				<div class="relative">
					<pre class="px-3.5 py-2 font-mono text-[11px] leading-relaxed text-foreground/60">{previewText}</pre>
					{#if hasMoreLines}
						<div class="absolute inset-x-0 bottom-0 flex h-8 items-end justify-center" style="background: linear-gradient(to top, #353535 20%, transparent);">
							<span class="pb-1 text-[10px] text-muted-foreground/60">
								{remainingLines} more line{remainingLines !== 1 ? 's' : ''}
							</span>
						</div>
					{/if}
				</div>
			{/if}
		</div>
	{:else if status === 'running'}
		<div class="border-t border-border/50 px-3.5 py-2.5">
			<span class="font-mono text-[11px] text-muted-foreground animate-pulse">Reading asset...</span>
		</div>
	{:else if status === 'error'}
		<div class="border-t border-border/50 px-3.5 py-2">
			<pre class="font-mono text-[11px] leading-relaxed text-red-400/80">{resultBlock?.toolResult ?? 'Asset not found'}</pre>
		</div>
	{/if}
</div>
