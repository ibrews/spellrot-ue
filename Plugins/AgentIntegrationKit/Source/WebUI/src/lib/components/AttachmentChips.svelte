<script lang="ts">
	import Icon from '$lib/components/Icon.svelte';
	import { Cancel01Icon, Image01Icon, CodeIcon } from '@hugeicons/core-free-icons';
	import { attachments, removeItem } from '$lib/stores/attachments.js';
</script>

{#if $attachments.length > 0}
	<div class="flex flex-wrap gap-1.5 px-4 pt-3 pb-1">
		{#each $attachments as att (att.id)}
			<div
				class="group flex items-center gap-1.5 rounded-lg border border-border/60 bg-card/60 px-2 py-1 text-[12px] text-foreground/80 transition-colors hover:border-border"
			>
				{#if att.type === 'image'}
					{#if att.thumbnail}
						<img
							src={`data:${att.mimeType ?? 'image/png'};base64,${att.thumbnail}`}
							alt={att.displayName}
							class="h-5 w-5 shrink-0 rounded object-cover"
						/>
					{:else}
						<Icon icon={Image01Icon} size={14} strokeWidth={1.5} class="shrink-0 text-blue-400/70" />
					{/if}
				{:else}
					<Icon icon={CodeIcon} size={14} strokeWidth={1.5} class="shrink-0 text-orange-400/70" />
				{/if}
				<span class="max-w-[140px] truncate">{att.displayName}</span>
				{#if att.type === 'image' && att.width && att.height}
					<span class="text-[10px] text-muted-foreground/50">{att.width}&times;{att.height}</span>
				{:else if att.type === 'file' && att.sizeBytes}
					<span class="text-[10px] text-muted-foreground/50">{Math.round(att.sizeBytes / 1024)} KB</span>
				{/if}
				<button
					class="ml-0.5 rounded p-0.5 text-muted-foreground/40 transition-colors hover:bg-destructive/20 hover:text-red-400"
					onclick={() => removeItem(att.id)}
					title="Remove"
				>
					<Icon icon={Cancel01Icon} size={12} strokeWidth={2} />
				</button>
			</div>
		{/each}
	</div>
{/if}
