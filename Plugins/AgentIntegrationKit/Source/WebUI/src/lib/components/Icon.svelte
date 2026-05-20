<script lang="ts">
	type IconElement = readonly [string, Readonly<Record<string, string | number>>];
	type IconData = readonly IconElement[];

	interface Props {
		icon: IconData;
		size?: number;
		strokeWidth?: number;
		class?: string;
	}

	let { icon, size = 24, strokeWidth, class: className = '' }: Props = $props();

	// Convert camelCase SVG attrs to kebab-case (strokeWidth -> stroke-width)
	function processAttrs(attrs: Readonly<Record<string, string | number>>): string {
		return Object.entries(attrs)
			.filter(([key]) => key !== 'key')
			.map(([key, value]) => {
				const kebab = key.replace(/([a-z0-9])([A-Z])/g, '$1-$2').toLowerCase();
				// Override stroke-width if explicitly set
				if (kebab === 'stroke-width' && strokeWidth !== undefined) {
					return `${kebab}="${strokeWidth}"`;
				}
				return `${kebab}="${value}"`;
			})
			.join(' ');
	}

	function buildSvgContent(elements: IconData): string {
		return elements
			.map(([tag, attrs]) => `<${tag} ${processAttrs(attrs)}/>`)
			.join('');
	}
</script>

<svg
	xmlns="http://www.w3.org/2000/svg"
	width={size}
	height={size}
	viewBox="0 0 24 24"
	fill="none"
	color="currentColor"
	style="display: inline-block; vertical-align: middle;"
	class={className}
>
	{@html buildSvgContent(icon)}
</svg>
