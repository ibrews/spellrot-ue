import typography from '@tailwindcss/typography';
import plugin from 'tailwindcss/plugin';

/** @type {import('tailwindcss').Config} */
export default {
	darkMode: ['selector', '.dark'],
	content: [
		'./src/**/*.{html,js,svelte,ts}',
		'./node_modules/svelte-streamdown/**/*.{html,js,svelte,ts}'
	],
	theme: {
		extend: {
			fontFamily: {
				sans: ['"Geist"', 'ui-sans-serif', 'system-ui', 'sans-serif'],
				mono: ['"Geist Mono"', 'ui-monospace', 'monospace']
			},
			borderRadius: {
				sm: 'calc(var(--radius) - 4px)',
				md: 'calc(var(--radius) - 2px)',
				lg: 'var(--radius)',
				xl: 'calc(var(--radius) + 4px)'
			},
			colors: {
				background: 'var(--background)',
				foreground: 'var(--foreground)',
				card: {
					DEFAULT: 'var(--card)',
					foreground: 'var(--card-foreground)'
				},
				popover: {
					DEFAULT: 'var(--popover)',
					foreground: 'var(--popover-foreground)'
				},
				primary: {
					DEFAULT: 'var(--primary)',
					foreground: 'var(--primary-foreground)'
				},
				secondary: {
					DEFAULT: 'var(--secondary)',
					foreground: 'var(--secondary-foreground)'
				},
				muted: {
					DEFAULT: 'var(--muted)',
					foreground: 'var(--muted-foreground)'
				},
				accent: {
					DEFAULT: 'var(--accent)',
					foreground: 'var(--accent-foreground)'
				},
				destructive: {
					DEFAULT: 'var(--destructive)'
				},
				border: 'var(--border)',
				input: 'var(--input)',
				ring: 'var(--ring)',
				sidebar: {
					DEFAULT: 'var(--sidebar)',
					foreground: 'var(--sidebar-foreground)',
					primary: {
						DEFAULT: 'var(--sidebar-primary)',
						foreground: 'var(--sidebar-primary-foreground)'
					},
					accent: {
						DEFAULT: 'var(--sidebar-accent)',
						foreground: 'var(--sidebar-accent-foreground)'
					},
					border: 'var(--sidebar-border)',
					ring: 'var(--sidebar-ring)'
				}
			}
		}
	},
	plugins: [
		typography,
		// Recreate the dark variant from v4's @custom-variant
		plugin(function ({ addVariant }) {
			addVariant('dark', '&:is(.dark *)');
		})
	]
};
