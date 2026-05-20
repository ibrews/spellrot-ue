import { writable } from 'svelte/store';

export const settingsOpen = writable(false);
export const settingsTab = writable<string>('general');

/** Must match tab `id` values in SettingsPanel.svelte */
const SETTINGS_TAB_IDS = [
	'general',
	'indexing',
	'tools',
	'agents',
	'notifications',
	'crashes',
	'about'
] as const;

function isKnownSettingsTab(tab: string): tab is (typeof SETTINGS_TAB_IDS)[number] {
	return (SETTINGS_TAB_IDS as readonly string[]).includes(tab);
}

// Enter-to-send preference (persisted to localStorage)
const ENTER_TO_SEND_KEY = 'aik-enter-to-send';
function getInitialEnterToSend(): boolean {
	if (typeof localStorage === 'undefined') return true;
	const stored = localStorage.getItem(ENTER_TO_SEND_KEY);
	return stored === null ? true : stored === 'true';
}
export const enterToSend = writable<boolean>(getInitialEnterToSend());
enterToSend.subscribe((value) => {
	if (typeof localStorage !== 'undefined') {
		localStorage.setItem(ENTER_TO_SEND_KEY, String(value));
	}
});

export function openSettings(tab?: string) {
	// Only accept known string ids (avoids `onclick={openSettings}` passing a MouseEvent, etc.)
	if (typeof tab === 'string' && isKnownSettingsTab(tab)) {
		settingsTab.set(tab);
	} else {
		settingsTab.set('general');
	}
	settingsOpen.set(true);
}

export function closeSettings() {
	settingsOpen.set(false);
}
