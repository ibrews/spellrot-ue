<script lang="ts">
	import { cn } from "$lib/utils";

	interface Props {
		class?: string;
		children?: import("svelte").Snippet;
	}

	let { class: className = "", children, ...props }: Props = $props();
</script>

<div class={cn("max-w-none", className)} style="color: #a0a0a0;" {...props}>
	{@render children?.()}
</div>
