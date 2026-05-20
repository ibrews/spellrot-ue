<script lang="ts">
	import {
		getNotificationSettings,
		setNotificationSetting,
		openPluginSettings,
		type NotificationSettings
	} from '$lib/bridge.js';
	import { t } from '$lib/i18n.js';

	// ── State ──────────────────────────────────────────────────────────
	let settings = $state<NotificationSettings>({
		onlyWhenUnfocused: false,
		notifyOnComplete: true,
		flashTaskbar: true,
		playSound: true,
		soundVolume: 1.0,
		completionSound: '',
		errorSound: '',
		playPermissionSound: false,
		permissionSoundVolume: 1.0,
		permissionRequestSound: ''
	});
	let isLoading = $state(false);

	export async function load() {
		if (isLoading) return;
		isLoading = true;
		try {
			settings = await getNotificationSettings();
		} catch (e) {
			console.warn('Failed to load notification settings:', e);
		} finally {
			isLoading = false;
		}
	}

	async function toggle(key: keyof NotificationSettings, value: boolean) {
		(settings as any)[key] = value;
		try {
			await setNotificationSetting(key, String(value));
		} catch (e) {
			console.warn('Failed to save notification setting:', e);
		}
	}

	async function setVolume(value: number) {
		settings.soundVolume = value;
		try {
			await setNotificationSetting('soundVolume', String(value));
		} catch (e) {
			console.warn('Failed to save sound volume:', e);
		}
	}

	async function setPermissionVolume(value: number) {
		settings.permissionSoundVolume = value;
		try {
			await setNotificationSetting('permissionSoundVolume', String(value));
		} catch (e) {
			console.warn('Failed to save permission sound volume:', e);
		}
	}

	let volumePercent = $derived(Math.round(settings.soundVolume * 100));
	let permissionVolumePercent = $derived(Math.round(settings.permissionSoundVolume * 100));
</script>

<div class="mb-6">
	<h2 class="mb-1 text-[18px] font-medium text-foreground">{$t('tab_notifications')}</h2>
	<p class="text-[13px] text-muted-foreground/60">{$t('notif_desc')}</p>
	<p class="mt-1 text-[12px] text-muted-foreground/45">{$t('notif_permission_gate_note')}</p>
</div>

{#if isLoading}
	<div class="flex items-center gap-2 py-8 text-muted-foreground/50">
		<span class="inline-block h-4 w-4 animate-spin rounded-full border-2 border-muted-foreground/30 border-t-muted-foreground"></span>
		Loading...
	</div>
{:else}
	<!-- When to notify -->
	<div class="mb-4 rounded-lg border border-border/60 bg-card p-4">
		<h3 class="mb-3 text-[14px] font-medium text-foreground">{$t('notif_when_heading')}</h3>

		<label class="flex cursor-pointer items-center justify-between rounded-md px-1 py-2.5 transition-colors hover:bg-accent/20">
			<div>
				<span class="text-[13px] text-foreground">{$t('notif_only_unfocused')}</span>
				<p class="mt-0.5 text-[11px] text-muted-foreground/50">{$t('notif_only_unfocused_desc')}</p>
			</div>
			<input
				type="checkbox"
				checked={settings.onlyWhenUnfocused}
				onchange={(e) => toggle('onlyWhenUnfocused', (e.currentTarget as HTMLInputElement).checked)}
				class="h-4 w-4 shrink-0 rounded border-border accent-[var(--ue-accent)]"
			/>
		</label>
	</div>

	<!-- Notification types -->
	<div class="mb-4 rounded-lg border border-border/60 bg-card p-4">
		<h3 class="mb-3 text-[14px] font-medium text-foreground">{$t('notif_types_heading')}</h3>

		<div class="flex flex-col">
			<!-- Editor toast -->
			<label class="flex cursor-pointer items-center justify-between rounded-md px-1 py-2.5 transition-colors hover:bg-accent/20">
				<div>
					<span class="text-[13px] text-foreground">{$t('notif_toast')}</span>
					<p class="mt-0.5 text-[11px] text-muted-foreground/50">{$t('notif_toast_desc')}</p>
				</div>
				<input
					type="checkbox"
					checked={settings.notifyOnComplete}
					onchange={(e) => toggle('notifyOnComplete', (e.currentTarget as HTMLInputElement).checked)}
					class="h-4 w-4 shrink-0 rounded border-border accent-[var(--ue-accent)]"
				/>
			</label>

			<!-- Taskbar flash -->
			<label class="flex cursor-pointer items-center justify-between rounded-md px-1 py-2.5 transition-colors hover:bg-accent/20">
				<div>
					<span class="text-[13px] text-foreground">{$t('notif_flash')}</span>
					<p class="mt-0.5 text-[11px] text-muted-foreground/50">{$t('notif_flash_desc')}</p>
				</div>
				<input
					type="checkbox"
					checked={settings.flashTaskbar}
					onchange={(e) => toggle('flashTaskbar', (e.currentTarget as HTMLInputElement).checked)}
					class="h-4 w-4 shrink-0 rounded border-border accent-[var(--ue-accent)]"
				/>
			</label>

			<!-- Sound -->
			<label class="flex cursor-pointer items-center justify-between rounded-md px-1 py-2.5 transition-colors hover:bg-accent/20">
				<div>
					<span class="text-[13px] text-foreground">{$t('notif_sound')}</span>
					<p class="mt-0.5 text-[11px] text-muted-foreground/50">{$t('notif_sound_desc')}</p>
				</div>
				<input
					type="checkbox"
					checked={settings.playSound}
					onchange={(e) => toggle('playSound', (e.currentTarget as HTMLInputElement).checked)}
					class="h-4 w-4 shrink-0 rounded border-border accent-[var(--ue-accent)]"
				/>
			</label>

			<!-- Permission / Ask User prompt sound -->
			<label class="flex cursor-pointer items-center justify-between rounded-md px-1 py-2.5 transition-colors hover:bg-accent/20">
				<div>
					<span class="text-[13px] text-foreground">{$t('notif_permission_sound')}</span>
					<p class="mt-0.5 text-[11px] text-muted-foreground/50">{$t('notif_permission_sound_desc')}</p>
				</div>
				<input
					type="checkbox"
					checked={settings.playPermissionSound}
					onchange={(e) => toggle('playPermissionSound', (e.currentTarget as HTMLInputElement).checked)}
					class="h-4 w-4 shrink-0 rounded border-border accent-[var(--ue-accent)]"
				/>
			</label>
		</div>
	</div>

	<!-- Volume slider (only when sound enabled) -->
	{#if settings.playSound}
		<div class="mb-4 rounded-lg border border-border/60 bg-card p-4">
			<h3 class="mb-3 text-[14px] font-medium text-foreground">{$t('notif_volume_heading')}</h3>
			<div class="flex items-center gap-3">
				<input
					type="range"
					min="0"
					max="1"
					step="0.05"
					value={settings.soundVolume}
					oninput={(e) => setVolume(parseFloat((e.currentTarget as HTMLInputElement).value))}
					class="h-1.5 flex-1 cursor-pointer appearance-none rounded-full bg-border/60 accent-[var(--ue-accent)] [&::-webkit-slider-thumb]:h-3.5 [&::-webkit-slider-thumb]:w-3.5 [&::-webkit-slider-thumb]:appearance-none [&::-webkit-slider-thumb]:rounded-full [&::-webkit-slider-thumb]:bg-foreground"
				/>
				<span class="w-10 text-right text-[13px] text-muted-foreground">{volumePercent}%</span>
			</div>

			<!-- Sound asset note -->
			{#if settings.completionSound || settings.errorSound}
				<div class="mt-3 rounded-md bg-foreground/5 px-3 py-2 text-[11px] text-muted-foreground/50">
					{$t('notif_custom_sounds_set')}
				</div>
			{/if}
		</div>
	{/if}

	{#if settings.playPermissionSound}
		<div class="mb-4 rounded-lg border border-border/60 bg-card p-4">
			<h3 class="mb-3 text-[14px] font-medium text-foreground">{$t('notif_permission_volume_heading')}</h3>
			<div class="flex items-center gap-3">
				<input
					type="range"
					min="0"
					max="1"
					step="0.05"
					value={settings.permissionSoundVolume}
					oninput={(e) => setPermissionVolume(parseFloat((e.currentTarget as HTMLInputElement).value))}
					class="h-1.5 flex-1 cursor-pointer appearance-none rounded-full bg-border/60 accent-[var(--ue-accent)] [&::-webkit-slider-thumb]:h-3.5 [&::-webkit-slider-thumb]:w-3.5 [&::-webkit-slider-thumb]:appearance-none [&::-webkit-slider-thumb]:rounded-full [&::-webkit-slider-thumb]:bg-foreground"
				/>
				<span class="w-10 text-right text-[13px] text-muted-foreground">{permissionVolumePercent}%</span>
			</div>
			{#if settings.permissionRequestSound}
				<div class="mt-3 rounded-md bg-foreground/5 px-3 py-2 text-[11px] text-muted-foreground/50">
					{$t('notif_permission_custom_sound_set')}
				</div>
			{/if}
		</div>
	{/if}

	<!-- Link to Project Settings for sound assets -->
	<div class="rounded-md bg-foreground/5 px-3 py-2 text-[11px] text-muted-foreground/50">
		{$t('notif_sound_assets_note')}
		<button
			class="ml-1 text-[var(--ue-accent)] underline-offset-2 hover:underline"
			onclick={openPluginSettings}
		>{$t('notif_open_project_settings')}</button>
	</div>
{/if}
