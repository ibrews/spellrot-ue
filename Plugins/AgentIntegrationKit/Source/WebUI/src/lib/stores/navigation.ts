import { writable } from 'svelte/store';

export type AppTab = 'chat' | 'studio' | 'terminal';

export const currentTab = writable<AppTab>('chat');
