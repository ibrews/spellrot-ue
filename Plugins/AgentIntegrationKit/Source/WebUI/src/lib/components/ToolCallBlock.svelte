<script lang="ts">
	import Icon from '$lib/components/Icon.svelte';
	import AssetReadBlock from '$lib/components/AssetReadBlock.svelte';
	import ScreenshotBlock from '$lib/components/ScreenshotBlock.svelte';
	import GenerateAssetBlock from '$lib/components/GenerateAssetBlock.svelte';
	import {
		ArrowDown01Icon,
		ArrowUp01Icon,
		CheckmarkCircle02Icon,
		Cancel01Icon,
		Loading03Icon,
		CommandLineIcon,
		File01Icon
	} from '@hugeicons/core-free-icons';
	import type { ContentBlock } from '$lib/bridge.js';
	import { copyToClipboard } from '$lib/bridge.js';

	let {
		block,
		resultBlock,
		childBlocks = [],
		allBlocks = [],
		depth = 0
	}: {
		block: ContentBlock;
		resultBlock?: ContentBlock;
		childBlocks?: ContentBlock[];
		allBlocks?: ContentBlock[];
		depth?: number;
	} = $props();

	let expanded = $state(false);

	/** Stripped tool name — removes MCP prefix like mcp__neostack__read_asset → read_asset */
	let toolName = $derived(
		block.toolName?.replace(/^mcp__[^_]+__/, '') ?? 'tool'
	);

	let isTask = $derived(block.toolName === 'Task');
	let isBash = $derived(block.toolName === 'Bash');
	let isScreenshot = $derived(toolName === 'screenshot');
	let isGenerateAsset = $derived(
		toolName === 'generate_asset' || toolName === 'generate_3d_model' || toolName === 'generate_image'
	);
	let isAssetRead = $derived(toolName === 'read_asset');
	let isReadLike = $derived(
		toolName === 'Read' || toolName === 'read_file' || toolName === 'ReadFile'
	);

	let status = $derived(
		!resultBlock
			? 'running'
			: resultBlock.toolSuccess !== false
				? 'success'
				: 'error'
	);

	let statusIcon = $derived(
		status === 'running'
			? Loading03Icon
			: status === 'success'
				? CheckmarkCircle02Icon
				: Cancel01Icon
	);

	let statusColor = $derived(
		status === 'running'
			? 'text-blue-400'
			: status === 'success'
				? 'text-emerald-400'
				: 'text-red-400'
	);

	// ── Task-specific parsed fields ──────────────────────────────────

	let taskArgs = $derived.by(() => {
		if (!isTask || !block.toolArguments) return null;
		try {
			return JSON.parse(block.toolArguments) as {
				description?: string;
				subagent_type?: string;
				prompt?: string;
				model?: string;
				max_turns?: number;
			};
		} catch {
			return null;
		}
	});

	let subagentType = $derived(taskArgs?.subagent_type || 'Task');
	let taskDescription = $derived(taskArgs?.description || '');
	let taskPrompt = $derived(taskArgs?.prompt || '');
	let taskModel = $derived(
		taskArgs?.model ? taskArgs.model.charAt(0).toUpperCase() + taskArgs.model.slice(1) : ''
	);

	/** "Explore(Check SC windows)" style heading */
	let taskHeading = $derived(
		taskDescription ? `${subagentType}(${taskDescription})` : subagentType
	);

	// ── Bash-specific parsed fields ─────────────────────────────────

	let bashArgs = $derived.by(() => {
		if (!isBash || !block.toolArguments) return null;
		try {
			return JSON.parse(block.toolArguments) as {
				command?: string;
				description?: string;
				timeout?: number;
			};
		} catch {
			return null;
		}
	});

	let bashCommand = $derived(bashArgs?.command ?? '');
	let bashDescription = $derived(bashArgs?.description ?? '');
	let bashOutput = $derived(resultBlock?.toolResult ?? '');
	let bashOutputLines = $derived(bashOutput ? bashOutput.split('\n') : []);
	let bashOutputLineCount = $derived(bashOutputLines.length);

	const BASH_PREVIEW_LINES = 4;
	let bashPreview = $derived(
		bashOutputLines.length > BASH_PREVIEW_LINES
			? bashOutputLines.slice(0, BASH_PREVIEW_LINES).join('\n')
			: bashOutput
	);
	let bashHasMoreLines = $derived(bashOutputLines.length > BASH_PREVIEW_LINES);
	let bashRemainingLines = $derived(Math.max(0, bashOutputLines.length - BASH_PREVIEW_LINES));
	let bashAccentColor = $derived(
		status === 'error' ? '#ef4444' : status === 'running' ? '#3b82f6' : '#10b981'
	);

	// ── Read-specific parsed fields ─────────────────────────────────

	let readArgs = $derived.by(() => {
		if (!isReadLike || !block.toolArguments) return null;
		try {
			const parsed = JSON.parse(block.toolArguments);
			// Try common field names across different ACPs
			const filePath = parsed.file_path || parsed.path || parsed.file || parsed.filename || '';
			if (!filePath) return null;
			return {
				filePath: filePath as string,
				offset: (parsed.offset ?? parsed.start_line ?? parsed.line) as number | undefined,
				limit: (parsed.limit ?? parsed.end_line ?? parsed.lines) as number | undefined
			};
		} catch {
			return null;
		}
	});

	/** If we parsed Read args successfully, use custom UI; otherwise fall back to generic */
	let isRead = $derived(isReadLike && readArgs !== null);

	let readFilePath = $derived(readArgs?.filePath ?? '');
	let readFileName = $derived(() => {
		const p = readFilePath;
		if (!p) return '';
		const parts = p.replace(/\\/g, '/').split('/');
		return parts[parts.length - 1] || p;
	});
	let readDirPath = $derived(() => {
		const p = readFilePath;
		if (!p) return '';
		const normalized = p.replace(/\\/g, '/');
		const lastSlash = normalized.lastIndexOf('/');
		return lastSlash >= 0 ? normalized.substring(0, lastSlash) : '';
	});
	let readLineRange = $derived(() => {
		if (!readArgs) return '';
		const { offset, limit } = readArgs;
		if (offset && limit) return `Lines ${offset}–${offset + limit - 1}`;
		if (offset) return `From line ${offset}`;
		if (limit) return `${limit} lines`;
		return '';
	});

	/** Strip leading/trailing "..." truncation markers from Read results */
	let readOutput = $derived.by(() => {
		let raw = resultBlock?.toolResult ?? '';
		if (!raw) return '';
		// Strip leading "...\n" and trailing "\n..."
		raw = raw.replace(/^\.\.\.\n?/, '').replace(/\n?\.\.\.$/, '');
		return raw;
	});
	let readOutputLines = $derived(readOutput ? readOutput.split('\n') : []);
	let readOutputLineCount = $derived(readOutputLines.length);

	const READ_PREVIEW_LINES = 6;
	let readPreview = $derived(
		readOutputLines.length > READ_PREVIEW_LINES
			? readOutputLines.slice(0, READ_PREVIEW_LINES).join('\n')
			: readOutput
	);
	let readHasMoreLines = $derived(readOutputLines.length > READ_PREVIEW_LINES);
	let readRemainingLines = $derived(Math.max(0, readOutputLines.length - READ_PREVIEW_LINES));

	/** Guess language from file extension for potential future syntax highlighting */
	let readFileExt = $derived(() => {
		const name = readFileName();
		const dot = name.lastIndexOf('.');
		return dot >= 0 ? name.substring(dot + 1).toLowerCase() : '';
	});

	let hasImages = $derived(
		(resultBlock?.images && resultBlock.images.length > 0) ?? false
	);

	let hasChildren = $derived(childBlocks.length > 0);

	// ── Task result parsing ──────────────────────────────────────────
	// The raw result contains metadata like:
	//   "4\nagentId: abc123 (for resuming...)\n<usage>total_tokens: 40876\ntool_uses: 0\nduration_ms: 2216</usage>"
	// We extract the clean content, usage stats, and agentId separately.

	let taskResultParsed = $derived.by(() => {
		const raw = resultBlock?.toolResult ?? '';
		if (!raw || !isTask) return { content: raw, tokens: '', tools: '', duration: '', agentId: '' };

		let text = raw;

		// Extract <usage>...</usage> block
		let tokens = '';
		let tools = '';
		let duration = '';
		const usageMatch = text.match(/<usage>([\s\S]*?)<\/usage>/);
		if (usageMatch) {
			const usageBlock = usageMatch[1];
			const tokensMatch = usageBlock.match(/total_tokens:\s*(\d+)/);
			const toolsMatch = usageBlock.match(/tool_uses:\s*(\d+)/);
			const durationMatch = usageBlock.match(/duration_ms:\s*(\d+)/);
			if (tokensMatch) {
				const n = parseInt(tokensMatch[1]);
				tokens = n >= 1000 ? `${(n / 1000).toFixed(1)}k` : `${n}`;
			}
			if (toolsMatch) tools = toolsMatch[1];
			if (durationMatch) {
				const ms = parseInt(durationMatch[1]);
				duration = ms >= 1000 ? `${(ms / 1000).toFixed(1)}s` : `${ms}ms`;
			}
			text = text.replace(/<usage>[\s\S]*?<\/usage>/, '');
		}

		// Extract agentId line
		let agentId = '';
		const agentMatch = text.match(/^agentId:\s*([a-f0-9-]+).*$/m);
		if (agentMatch) {
			agentId = agentMatch[1];
			text = text.replace(/^agentId:.*$/m, '');
		}

		// Clean up leftover whitespace
		const content = text.trim();

		return { content, tokens, tools, duration, agentId };
	});

	function findChildResult(toolCallId: string | undefined): ContentBlock | undefined {
		if (!toolCallId) return undefined;
		return allBlocks.find(
			(b) => b.type === 'tool_result' && b.toolCallId === toolCallId
		);
	}

	function getGrandchildren(childToolCallId: string | undefined): ContentBlock[] {
		if (!childToolCallId) return [];
		return allBlocks.filter(
			(b) => b.type === 'tool_call' && b.parentToolCallId === childToolCallId
		);
	}

	function formatArgs(args: string | undefined): string {
		if (!args) return '';
		try {
			return JSON.stringify(JSON.parse(args), null, 2);
		} catch {
			return args;
		}
	}

	function imageDataUrl(base64: string, mimeType: string): string {
		return `data:${mimeType};base64,${base64}`;
	}
</script>

{#if isTask}
	<!-- ════════════════════════════════════════════════════════════════
	     Task / Subagent block
	     ════════════════════════════════════════════════════════════════ -->
	<div class="my-1.5 w-full rounded-lg border border-border bg-card/40">
		<!-- Header: status · heading · model badge · tool count · chevron -->
		<button
			class="flex w-full items-center gap-2 px-3 py-2 text-left text-[13px] transition-colors hover:bg-accent/30"
			onclick={() => (expanded = !expanded)}
		>
			<span class="flex h-4 w-4 shrink-0 items-center justify-center {statusColor}">
				<Icon
					icon={statusIcon}
					size={14}
					strokeWidth={1.5}
					class={status === 'running' ? 'animate-spin' : ''}
				/>
			</span>
			<span class="flex-1 min-w-0 truncate font-medium text-foreground/80">
				{taskHeading}
			</span>
			{#if taskModel}
				<span class="shrink-0 rounded bg-secondary/60 px-1.5 py-0.5 text-[10px] font-medium text-muted-foreground/60">
					{taskModel}
				</span>
			{/if}
			{#if hasChildren}
				<span class="shrink-0 text-[11px] tabular-nums text-muted-foreground/50">
					{childBlocks.length} tool{childBlocks.length !== 1 ? 's' : ''}
				</span>
			{/if}
			<span class="shrink-0 text-muted-foreground/50">
				<Icon icon={expanded ? ArrowUp01Icon : ArrowDown01Icon} size={12} strokeWidth={1.5} />
			</span>
		</button>

		<!-- Usage stats bar (always visible when done) -->
		{#if status !== 'running' && (taskResultParsed.tokens || taskResultParsed.duration || taskResultParsed.tools)}
			<div class="flex items-center gap-2 border-t border-border/15 px-3 py-1">
				{#if taskResultParsed.tokens}
					<span class="text-[10px] text-muted-foreground/40">{taskResultParsed.tokens} tokens</span>
				{/if}
				{#if taskResultParsed.tools && taskResultParsed.tools !== '0'}
					<span class="text-[10px] text-muted-foreground/25">·</span>
					<span class="text-[10px] text-muted-foreground/40">{taskResultParsed.tools} tool uses</span>
				{/if}
				{#if taskResultParsed.duration}
					<span class="text-[10px] text-muted-foreground/25">·</span>
					<span class="text-[10px] text-muted-foreground/40">{taskResultParsed.duration}</span>
				{/if}
			</div>
		{/if}

		<!-- Expanded content -->
		{#if expanded}
			<!-- Prompt -->
			{#if taskPrompt}
				<div class="border-t border-border/20 px-3 py-2">
					<div class="mb-1 text-[10px] font-medium uppercase tracking-wider text-muted-foreground/40">
						Prompt
					</div>
					<div class="text-[12px] leading-relaxed text-muted-foreground/70 whitespace-pre-wrap break-words">
						{taskPrompt}
					</div>
				</div>
			{/if}

			<!-- Clean result content -->
			{#if taskResultParsed.content}
				<div class="border-t border-border/20 px-3 py-2">
					<div class="mb-1 text-[10px] font-medium uppercase tracking-wider text-muted-foreground/40">
						Result
					</div>
					<pre class="max-h-[300px] overflow-auto rounded bg-secondary/50 p-2 font-mono text-[11px] leading-relaxed text-foreground/70">{taskResultParsed.content}</pre>
				</div>
			{/if}

			<!-- Nested child tool calls -->
			{#if hasChildren}
				<div class="border-t border-border/20 pl-3 pr-1 py-1">
					{#each childBlocks as child (child.toolCallId)}
						<svelte:self
							block={child}
							resultBlock={findChildResult(child.toolCallId)}
							childBlocks={getGrandchildren(child.toolCallId)}
							{allBlocks}
							depth={depth + 1}
						/>
					{/each}
				</div>
			{/if}
		{/if}
	</div>

{:else if isBash}
	<!-- ════════════════════════════════════════════════════════════════
	     Bash / Terminal block
	     ════════════════════════════════════════════════════════════════ -->
	<div class="my-1 w-full overflow-hidden rounded-lg border border-border bg-card/40">
		<!-- Header -->
		<button
			class="flex w-full items-center gap-2 px-3 py-2 text-left text-[13px] transition-colors hover:bg-accent/30"
			onclick={() => (expanded = !expanded)}
		>
			<span class="flex h-4 w-4 shrink-0 items-center justify-center {statusColor}">
				{#if status === 'running'}
					<Icon icon={Loading03Icon} size={14} strokeWidth={1.5} class="animate-spin" />
				{:else}
					<Icon icon={CommandLineIcon} size={14} strokeWidth={1.5} />
				{/if}
			</span>

			<span class="flex-1 min-w-0 flex items-center gap-1.5 truncate">
				<span class="shrink-0 font-medium text-foreground/80">Bash</span>
				{#if bashDescription}
					<span class="text-muted-foreground/30">&middot;</span>
					<span class="truncate text-muted-foreground/60">{bashDescription}</span>
				{/if}
			</span>

			{#if status === 'error'}
				<span class="shrink-0 rounded bg-red-500/15 px-1.5 py-0.5 text-[10px] font-medium text-red-400/80">
					failed
				</span>
			{/if}
			<span class="shrink-0 text-muted-foreground/50">
				<Icon icon={expanded ? ArrowUp01Icon : ArrowDown01Icon} size={12} strokeWidth={1.5} />
			</span>
		</button>

		<!-- Command line (always visible) -->
		<div class="border-t border-border/50 px-3 py-1.5">
			<pre class="overflow-x-auto font-mono text-[11px] leading-relaxed text-foreground/50"><span class="text-emerald-500/50 select-none">$</span> {bashCommand}</pre>
		</div>

		<!-- Output preview / full output -->
		{#if bashOutput}
			<div class="border-t border-border/50">
				{#if expanded}
					<pre class="max-h-[400px] overflow-auto px-3 py-2 font-mono text-[11px] leading-relaxed {status === 'error' ? 'text-red-400/70' : 'text-muted-foreground/60'}">{bashOutput}</pre>
				{:else}
					<!-- Preview: first few lines with fade -->
					<div class="relative">
						<pre class="px-3 py-2 font-mono text-[11px] leading-relaxed {status === 'error' ? 'text-red-400/70' : 'text-muted-foreground/60'}">{bashPreview}</pre>
						{#if bashHasMoreLines}
							<div class="absolute inset-x-0 bottom-0 flex h-8 items-end justify-center" style="background: linear-gradient(to top, hsl(var(--card)) 30%, transparent);">
								<span class="pb-1 text-[10px] text-muted-foreground/40">
									{bashRemainingLines} more line{bashRemainingLines !== 1 ? 's' : ''}
								</span>
							</div>
						{/if}
					</div>
				{/if}
			</div>
		{:else if status === 'running'}
			<div class="border-t border-border/50 px-3 py-2">
				<span class="font-mono text-[11px] text-muted-foreground/40 animate-pulse">Running...</span>
			</div>
		{/if}
	</div>

{:else if isRead}
	<!-- ════════════════════════════════════════════════════════════════
	     Read / File block
	     ════════════════════════════════════════════════════════════════ -->
	<div class="my-1 w-full overflow-hidden rounded-lg border border-border bg-card/40">
		<!-- Header: file icon + filename -->
		<button
			class="flex w-full items-center gap-2 px-3 py-2 text-left text-[13px] transition-colors hover:bg-accent/30"
			onclick={() => (expanded = !expanded)}
		>
			<span class="flex h-4 w-4 shrink-0 items-center justify-center {statusColor}">
				{#if status === 'running'}
					<Icon icon={Loading03Icon} size={14} strokeWidth={1.5} class="animate-spin" />
				{:else}
					<Icon icon={File01Icon} size={14} strokeWidth={1.5} />
				{/if}
			</span>

			<span class="flex-1 min-w-0 flex items-center gap-1.5 truncate">
				<span class="shrink-0 font-medium text-foreground/80">Read</span>
				<span class="text-muted-foreground/30">&middot;</span>
				<span class="truncate text-muted-foreground/60">{readFileName()}</span>
			</span>

			{#if readLineRange()}
				<span class="shrink-0 text-[10px] text-muted-foreground/40">
					{readLineRange()}
				</span>
			{:else if status !== 'running' && readOutputLineCount > 0}
				<span class="shrink-0 text-[10px] tabular-nums text-muted-foreground/40">
					{readOutputLineCount} line{readOutputLineCount !== 1 ? 's' : ''}
				</span>
			{/if}
			{#if status === 'error'}
				<span class="shrink-0 rounded bg-red-500/15 px-1.5 py-0.5 text-[10px] font-medium text-red-400/80">
					failed
				</span>
			{/if}
			<span class="shrink-0 text-muted-foreground/50">
				<Icon icon={expanded ? ArrowUp01Icon : ArrowDown01Icon} size={12} strokeWidth={1.5} />
			</span>
		</button>

		<!-- Path (always visible, muted) -->
		<div class="border-t border-border/50 px-3 py-1">
			<span class="block truncate font-mono text-[10px] text-muted-foreground/35">{readDirPath()}/</span>
		</div>

		<!-- Content preview / full content -->
		{#if readOutput}
			<div class="border-t border-border/50">
				{#if expanded}
					<pre class="max-h-[400px] overflow-auto px-2 py-1.5 font-mono text-[11px] leading-snug text-muted-foreground/60">{readOutput}</pre>
				{:else}
					<div class="relative">
						<pre class="px-2 py-1.5 font-mono text-[11px] leading-snug text-muted-foreground/60">{readPreview}</pre>
						{#if readHasMoreLines}
							<div class="absolute inset-x-0 bottom-0 flex h-8 items-end justify-center" style="background: linear-gradient(to top, hsl(var(--card)) 30%, transparent);">
								<span class="pb-1 text-[10px] text-muted-foreground/40">
									{readRemainingLines} more line{readRemainingLines !== 1 ? 's' : ''}
								</span>
							</div>
						{/if}
					</div>
				{/if}
			</div>
		{:else if status === 'running'}
			<div class="border-t border-border/50 px-3 py-2">
				<span class="font-mono text-[11px] text-muted-foreground/40 animate-pulse">Reading...</span>
			</div>
		{:else if status === 'error'}
			<div class="border-t border-border/50 px-3 py-1.5">
				<pre class="font-mono text-[11px] leading-relaxed text-red-400/70">{resultBlock?.toolResult ?? 'File not found'}</pre>
			</div>
		{/if}
	</div>

{:else if isScreenshot}
	<!-- ════════════════════════════════════════════════════════════════
	     Screenshot block
	     ════════════════════════════════════════════════════════════════ -->
	<ScreenshotBlock {block} {resultBlock} />

{:else if isGenerateAsset}
	<!-- ════════════════════════════════════════════════════════════════
	     Generate Asset block (3D model / image generation)
	     ════════════════════════════════════════════════════════════════ -->
	<GenerateAssetBlock {block} {resultBlock} />

{:else if isAssetRead}
	<!-- ════════════════════════════════════════════════════════════════
	     Asset Read block (read_asset)
	     ════════════════════════════════════════════════════════════════ -->
	<AssetReadBlock {block} {resultBlock} />

{:else}
	<!-- ════════════════════════════════════════════════════════════════
	     Regular tool call block (non-Task)
	     ════════════════════════════════════════════════════════════════ -->
	<div class="my-1 w-full rounded-lg border border-border bg-card/40">
		<button
			class="flex w-full items-center gap-2 px-3 py-2 text-left text-[13px] transition-colors hover:bg-accent/30"
			onclick={() => (expanded = !expanded)}
		>
			<span class="flex h-4 w-4 shrink-0 items-center justify-center {statusColor}">
				<Icon
					icon={statusIcon}
					size={14}
					strokeWidth={1.5}
					class={status === 'running' ? 'animate-spin' : ''}
				/>
			</span>
			<span class="flex-1 truncate font-medium text-foreground/80">{toolName}</span>
			<span class="text-muted-foreground/50">
				<Icon icon={expanded ? ArrowUp01Icon : ArrowDown01Icon} size={12} strokeWidth={1.5} />
			</span>
		</button>

		<!-- Inline image preview when collapsed -->
		{#if !expanded && hasImages && resultBlock?.images}
			<div class="flex gap-2 overflow-x-auto border-t border-border/50 px-3 py-2">
				{#each resultBlock.images as img}
					<button class="shrink-0 overflow-hidden rounded-md border border-border/50" onclick={() => (expanded = true)}>
						<img
							src={imageDataUrl(img.base64, img.mimeType)}
							alt="Tool result"
							class="h-20 w-auto object-contain"
							style="max-width: 160px;"
						/>
					</button>
				{/each}
			</div>
		{/if}

		{#if expanded}
			<div class="border-t border-border/50 px-3 py-2 text-[12px]">
				{#if block.toolArguments}
					<div class="mb-2">
						<div class="mb-1 text-[11px] font-medium uppercase tracking-wider text-muted-foreground/50">
							Arguments
						</div>
						<pre
							class="max-h-[200px] overflow-auto rounded bg-secondary/50 p-2 font-mono text-[11px] leading-relaxed text-foreground/70">{formatArgs(block.toolArguments)}</pre>
					</div>
				{/if}
				{#if hasImages && resultBlock?.images}
					<div class="mb-2">
						<div class="mb-1 text-[11px] font-medium uppercase tracking-wider text-muted-foreground/50">
							Images
						</div>
						<div class="flex flex-wrap gap-2">
							{#each resultBlock.images as img}
								<div class="overflow-hidden rounded-md border border-border/50 bg-black/20">
									<img
										src={imageDataUrl(img.base64, img.mimeType)}
										alt="Tool result"
										class="max-h-[400px] w-auto max-w-full object-contain"
									/>
									{#if img.width > 0 && img.height > 0}
										<div class="px-2 py-1 text-[10px] text-muted-foreground/40">
											{img.width}&times;{img.height}
										</div>
									{/if}
								</div>
							{/each}
						</div>
					</div>
				{/if}
				{#if resultBlock?.toolResult}
					<div>
						<div class="mb-1 text-[11px] font-medium uppercase tracking-wider text-muted-foreground/50">
							Result
						</div>
						<pre
							class="max-h-[300px] overflow-auto rounded bg-secondary/50 p-2 font-mono text-[11px] leading-relaxed text-foreground/70">{resultBlock.toolResult}</pre>
					</div>
				{/if}
			</div>
		{/if}
	</div>
{/if}
