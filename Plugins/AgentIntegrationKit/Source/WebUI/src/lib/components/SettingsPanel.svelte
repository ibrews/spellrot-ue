<script lang="ts">
	import Icon from '$lib/components/Icon.svelte';
	import {
		Settings02Icon,
		ArrowLeft02Icon,
		Wrench01Icon,
		UserIcon,
		Notification03Icon,
		InformationCircleIcon,
		Add01Icon,
		Delete02Icon,
		Tick02Icon,
		ArrowDown01Icon,
		ArrowRight01Icon,
		Edit02Icon,
		Cancel01Icon,
		TextIcon,
		Database02Icon,
		Alert02Icon
	} from '@hugeicons/core-free-icons';
	import CustomSelect from '$lib/components/ui/custom-select/CustomSelect.svelte';
	import ProjectIndexPanel from '$lib/components/ProjectIndexPanel.svelte';
	import NotificationsPanel from '$lib/components/NotificationsPanel.svelte';
	import AgentRegistry from '$lib/components/AgentRegistry.svelte';
	import PrerequisitesPanel from '$lib/components/PrerequisitesPanel.svelte';
	import { locale, localeNames, locales, setLocale, t, type Locale } from '$lib/i18n.js';
	import { settingsTab, closeSettings, enterToSend } from '$lib/stores/settings.js';
	import {
		getTools,
		getProfiles,
		getProfileDetail,
		setActiveProfile,
		setToolEnabled,
		setProfileToolEnabled,
		createProfile,
		deleteProfile,
		updateProfile,
		setToolDescriptionOverride,
		getContinuationSummarySettings,
		setContinuationSummaryProvider,
		setContinuationSummaryModel,
		setContinuationSummaryDefaultDetail,
		checkForPluginUpdate,
		getProviderSettings,
		setProviderPriority,
		addProvider,
		removeProvider,
		setProviderApiKey,
		setProviderBaseUrl,
		refreshProviderModels,
		createCustomProvider,
		deleteCustomProvider,
		updateCustomProvider,
		addCustomProviderModel,
		removeCustomProviderModel,
		importCustomProviderModels,
		setCustomProviderModelDiscovery,
		type ToolInfo,
		type ProfileInfo,
		type ProfileDetail,
		type ContinuationSummarySettings,
		type ProviderSettings,
		type ProviderConfig,
		type CustomProviderModel,
		getAllModels,
		getEnabledModels,
		setModelEnabled,
		setEnabledModels,
		type ModelInfo,
		type EnabledModelsState,
		getCrashHistory,
		reportCrash,
		type CrashRecord
	} from '$lib/bridge.js';

	let tabs = $derived([
		{ id: 'general', label: $t('tab_general'), icon: Settings02Icon },
		{ id: 'indexing', label: 'Project Index', icon: Database02Icon },
		{ id: 'tools', label: $t('tab_tools'), icon: Wrench01Icon },
		{ id: 'agents', label: $t('tab_agents'), icon: UserIcon },
		{ id: 'notifications', label: $t('tab_notifications'), icon: Notification03Icon },
		{ id: 'crashes', label: 'Crashes', icon: Alert02Icon },
		{ id: 'about', label: $t('tab_about'), icon: InformationCircleIcon }
	]);

	// ── Crash History State ─────────────────────────────────────────
	let crashRecords = $state<CrashRecord[]>([]);
	let isLoadingCrashes = $state(false);
	let reportingCrashId = $state('');

	async function loadCrashHistory() {
		isLoadingCrashes = true;
		try {
			crashRecords = await getCrashHistory();
		} catch {
			crashRecords = [];
		}
		isLoadingCrashes = false;
	}

	async function handleReportCrash(crashId: string) {
		reportingCrashId = crashId;
		try {
			const result = await reportCrash(crashId);
			if (result.success) {
				await loadCrashHistory();
			}
		} catch { /* ignore */ }
		reportingCrashId = '';
	}

	// ── Tool Profiles State ──────────────────────────────────────────
	let profiles = $state<ProfileInfo[]>([]);
	let activeProfileId = $state('');
	let tools = $state<ToolInfo[]>([]);
	let isLoadingTools = $state(false);

	// Which profile's tools are we viewing? Empty = global (DisabledTools)
	let viewingProfileId = $state('');

	// Profile detail editor
	let profileDetail = $state<ProfileDetail | null>(null);
	let isEditingProfile = $state(false);
	let editName = $state('');
	let editDescription = $state('');
	let editInstructions = $state('');
	let showProfileEditor = $state(false);

	// Tool description override editing
	let editingToolOverride = $state(''); // tool name being edited
	let overrideText = $state('');

	// Grouped tools by category
	let toolsByCategory = $derived(
		tools.reduce<Record<string, ToolInfo[]>>((acc, tool) => {
			if (!acc[tool.category]) acc[tool.category] = [];
			acc[tool.category].push(tool);
			return acc;
		}, {})
	);
	let categoryNames = $derived(Object.keys(toolsByCategory).sort());

	// Collapsed categories
	let collapsedCategories = $state<Set<string>>(new Set());

	// New profile input
	let showNewProfile = $state(false);
	let newProfileName = $state('');
	let newProfileInputEl = $state<HTMLInputElement>();

	// Stats
	let enabledCount = $derived(tools.filter(t => t.enabled).length);
	let totalCount = $derived(tools.length);

	// Is the current viewing profile a custom (editable) one?
	let isCustomProfile = $derived(() => {
		if (!viewingProfileId) return false;
		const p = profiles.find(p => p.profileId === viewingProfileId);
		return p ? !p.isBuiltIn : false;
	});

	// Search / filter
	let searchQuery = $state('');
	let filteredTools = $derived(
		searchQuery.trim()
			? tools.filter(t =>
				t.displayName.toLowerCase().includes(searchQuery.toLowerCase()) ||
				t.name.toLowerCase().includes(searchQuery.toLowerCase()) ||
				t.description.toLowerCase().includes(searchQuery.toLowerCase())
			)
			: tools
	);
	let filteredToolsByCategory = $derived(
		filteredTools.reduce<Record<string, ToolInfo[]>>((acc, tool) => {
			if (!acc[tool.category]) acc[tool.category] = [];
			acc[tool.category].push(tool);
			return acc;
		}, {})
	);
	let filteredCategoryNames = $derived(Object.keys(filteredToolsByCategory).sort());

	// ── General Settings: Chat Handoff Summary ──────────────────────
	let continuationSummarySettings = $state<ContinuationSummarySettings>({
		provider: 'openrouter',
		modelId: 'x-ai/grok-4.1-fast',
		defaultDetail: 'compact',
		hasOpenRouterKey: false
	});
	let isLoadingGeneralSettings = $state(false);
	let generalSettingsError = $state('');
	let isCheckingForUpdates = $state(false);
	let updateCheckMessage = $state('');

	// ── Provider Settings ────────────────────────────────────────────
	let providerSettings = $state<ProviderSettings>({ priority: [], providers: [] });
	let isLoadingProviders = $state(false);
	let selectedProviderId = $state('openrouter');
	let providerApiKeyInput = $state('');
	let providerSaveTimeout: ReturnType<typeof setTimeout> | undefined;

	let selectedProvider = $derived(providerSettings.providers.find(p => p.id === selectedProviderId));
	// Providers in priority order (configured ones)
	let priorityProviders = $derived(
		providerSettings.priority
			.map(id => providerSettings.providers.find(p => p.id === id))
			.filter((p): p is ProviderConfig => !!p)
	);
	// Providers not yet in priority list
	let availableToAdd = $derived(
		providerSettings.providers.filter(p => !p.inPriorityList)
	);

	async function loadProviderSettings() {
		if (isLoadingProviders) return;
		isLoadingProviders = true;
		try {
			providerSettings = await getProviderSettings();
			if (providerSettings.priority.length > 0) {
				selectedProviderId = providerSettings.priority[0];
			}
			providerApiKeyInput = '';
		} catch (e) {
			console.warn('Failed to load provider settings:', e);
		} finally {
			isLoadingProviders = false;
		}
	}

	async function handleAddProvider(providerId: string) {
		try {
			await addProvider(providerId);
			providerSettings = await getProviderSettings();
			selectedProviderId = providerId;
		} catch (e) {
			console.warn('Failed to add provider:', e);
		}
	}

	async function handleRemoveProvider(providerId: string) {
		try {
			// For custom providers, fully delete (removes config + models + priority)
			const prov = providerSettings.providers.find(p => p.id === providerId);
			if (prov?.isUserDefined) {
				await deleteCustomProvider(providerId);
			} else {
				await removeProvider(providerId);
			}
			providerSettings = await getProviderSettings();
			if (selectedProviderId === providerId && providerSettings.priority.length > 0) {
				selectedProviderId = providerSettings.priority[0];
			}
		} catch (e) {
			console.warn('Failed to remove provider:', e);
		}
	}

	async function handleMoveProvider(providerId: string, direction: 'up' | 'down') {
		const idx = providerSettings.priority.indexOf(providerId);
		if (idx < 0) return;
		const newIdx = direction === 'up' ? idx - 1 : idx + 1;
		if (newIdx < 0 || newIdx >= providerSettings.priority.length) return;
		const newPriority = [...providerSettings.priority];
		[newPriority[idx], newPriority[newIdx]] = [newPriority[newIdx], newPriority[idx]];
		providerSettings = { ...providerSettings, priority: newPriority };
		try {
			await setProviderPriority(newPriority);
		} catch (e) {
			console.warn('Failed to reorder providers:', e);
		}
	}

	async function handleProviderApiKeySave(providerId: string, key: string) {
		try {
			await setProviderApiKey(providerId, key);
			providerSettings = await getProviderSettings();
			providerApiKeyInput = '';
			// Refresh models since new key may enable a provider
			await refreshProviderModels();
		} catch (e) {
			console.warn('Failed to save provider API key:', e);
		}
	}

	function debouncedProviderBaseUrl(providerId: string, url: string) {
		clearTimeout(providerSaveTimeout);
		providerSaveTimeout = setTimeout(async () => {
			try {
				await setProviderBaseUrl(providerId, url);
			} catch (e) {
				console.warn('Failed to save provider base URL:', e);
			}
		}, 600);
	}

	// ── Custom Provider State ────────────────────────────────────
	let showNewCustomProviderForm = $state(false);
	let newCustomProviderName = $state('');
	let newCustomProviderUrl = $state('');
	let showAddModelForm = $state(false);
	let newModelId = $state('');
	let newModelName = $state('');
	let newModelDesc = $state('');
	let showImportModal = $state(false);
	let importJsonText = $state('');
	let importResult = $state<{ imported: number; errors: string[] } | null>(null);
	let isImporting = $state(false);
	let showDeleteConfirm = $state('');
	let customProviderUpdateTimeout: ReturnType<typeof setTimeout> | undefined;

	// Only show built-in providers in the "available to add" buttons (not custom ones)
	let builtinAvailableToAdd = $derived(
		availableToAdd.filter(p => !p.isUserDefined)
	);

	async function handleCreateCustomProvider() {
		if (!newCustomProviderName.trim()) return;
		try {
			const result = await createCustomProvider(newCustomProviderName.trim(), newCustomProviderUrl.trim());
			providerSettings = await getProviderSettings();
			selectedProviderId = result.providerId;
			newCustomProviderName = '';
			newCustomProviderUrl = '';
			showNewCustomProviderForm = false;
		} catch (e) {
			console.warn('Failed to create custom provider:', e);
		}
	}

	async function handleDeleteCustomProvider(providerId: string) {
		try {
			await deleteCustomProvider(providerId);
			providerSettings = await getProviderSettings();
			showDeleteConfirm = '';
			if (selectedProviderId === providerId && providerSettings.priority.length > 0) {
				selectedProviderId = providerSettings.priority[0];
			}
		} catch (e) {
			console.warn('Failed to delete custom provider:', e);
		}
	}

	function debouncedCustomProviderUpdate(providerId: string, name: string, baseUrl: string) {
		clearTimeout(customProviderUpdateTimeout);
		customProviderUpdateTimeout = setTimeout(async () => {
			try {
				await updateCustomProvider(providerId, name, baseUrl);
				// Don't reload full settings for debounced updates — just sync
			} catch (e) {
				console.warn('Failed to update custom provider:', e);
			}
		}, 600);
	}

	async function handleAddModel(providerId: string) {
		if (!newModelId.trim()) return;
		try {
			await addCustomProviderModel(providerId, newModelId.trim(), newModelName.trim(), newModelDesc.trim());
			providerSettings = await getProviderSettings();
			newModelId = '';
			newModelName = '';
			newModelDesc = '';
			showAddModelForm = false;
		} catch (e) {
			console.warn('Failed to add model:', e);
		}
	}

	async function handleRemoveModel(providerId: string, modelId: string) {
		try {
			await removeCustomProviderModel(providerId, modelId);
			providerSettings = await getProviderSettings();
		} catch (e) {
			console.warn('Failed to remove model:', e);
		}
	}

	async function handleImportModels(providerId: string) {
		if (!importJsonText.trim()) return;
		isImporting = true;
		importResult = null;
		try {
			importResult = await importCustomProviderModels(providerId, importJsonText.trim());
			if (importResult.imported > 0) {
				providerSettings = await getProviderSettings();
			}
		} catch (e) {
			importResult = { imported: 0, errors: ['Import failed: ' + String(e)] };
		} finally {
			isImporting = false;
		}
	}

	async function handleToggleModelDiscovery(providerId: string, enabled: boolean) {
		try {
			await setCustomProviderModelDiscovery(providerId, enabled);
			providerSettings = await getProviderSettings();
		} catch (e) {
			console.warn('Failed to toggle model discovery:', e);
		}
	}

	// ── Model Enable/Disable ────────────────────────────────────
	let settingsModels = $state<ModelInfo[]>([]);
	let enabledModelIds = $state<Set<string>>(new Set());
	let hasCustomModelSelection = $state(false);
	let isLoadingModels = $state(false);
	let modelSearchQuery = $state('');
	let filteredSettingsModels = $derived(
		modelSearchQuery.trim()
			? settingsModels.filter(m =>
				m.name.toLowerCase().includes(modelSearchQuery.toLowerCase()) ||
				m.id.toLowerCase().includes(modelSearchQuery.toLowerCase())
			)
			: settingsModels
	);
	let enabledModelCount = $derived(hasCustomModelSelection ? enabledModelIds.size : settingsModels.length);

	async function loadModelsForSettings() {
		if (isLoadingModels) return;
		isLoadingModels = true;
		try {
			const [allModelsState, enabledState] = await Promise.all([
				getAllModels('OpenRouter'),
				getEnabledModels()
			]);
			settingsModels = allModelsState.models.filter(m => m.id && !m.id.startsWith('special:'));
			settingsModels.sort((a, b) => a.name.localeCompare(b.name));
			hasCustomModelSelection = enabledState.hasCustomSelection;
			enabledModelIds = new Set(enabledState.enabledModels);
		} catch (e) {
			console.warn('Failed to load models for settings:', e);
		} finally {
			isLoadingModels = false;
		}
	}

	async function handleToggleModel(modelId: string, enabled: boolean) {
		if (!hasCustomModelSelection) {
			// First toggle: initialize enabled set with all models, then apply the change
			const allIds = new Set(settingsModels.map(m => m.id));
			if (!enabled) allIds.delete(modelId);
			enabledModelIds = allIds;
			hasCustomModelSelection = true;
			try {
				await setEnabledModels([...allIds]);
			} catch (e) {
				console.warn('Failed to initialize model selection:', e);
			}
			return;
		}
		const next = new Set(enabledModelIds);
		if (enabled) {
			next.add(modelId);
		} else {
			next.delete(modelId);
		}
		enabledModelIds = next;
		try {
			await setModelEnabled(modelId, enabled);
		} catch (e) {
			console.warn('Failed to toggle model:', e);
		}
	}

	function isModelEnabled(modelId: string): boolean {
		if (!hasCustomModelSelection) return true;
		return enabledModelIds.has(modelId);
	}

	async function handleEnableAllModels() {
		const allIds = new Set(settingsModels.map(m => m.id));
		enabledModelIds = allIds;
		hasCustomModelSelection = true;
		try {
			await setEnabledModels([...allIds]);
		} catch (e) {
			console.warn('Failed to enable all models:', e);
		}
	}

	async function handleDisableAllModels() {
		enabledModelIds = new Set();
		hasCustomModelSelection = true;
		try {
			await setEnabledModels([]);
		} catch (e) {
			console.warn('Failed to disable all models:', e);
		}
	}

	async function handleShowAllModels() {
		enabledModelIds = new Set();
		hasCustomModelSelection = false;
		try {
			await setEnabledModels([]);
		} catch (e) {
			console.warn('Failed to reset model selection:', e);
		}
	}

	// ── Indexing Panel ref ───────────────────────────────────────────
	let indexPanel: ProjectIndexPanel | undefined = $state();
	let notifPanel: NotificationsPanel | undefined = $state();

	$effect(() => {
		const tab = $settingsTab;
		queueMicrotask(() => {
			if (tab === 'tools') {
				void loadProfilesAndTools();
			} else if (tab === 'general') {
				void loadGeneralSettings();
			} else if (tab === 'agents') {
				void loadProviderSettings();
				void loadModelsForSettings();
			} else if (tab === 'notifications') {
				void notifPanel?.load();
			} else if (tab === 'indexing') {
				void indexPanel?.load();
			} else if (tab === 'crashes') {
				void loadCrashHistory();
			}
		});
	});

	async function loadProfilesAndTools() {
		isLoadingTools = true;
		try {
			const profileState = await getProfiles();
			profiles = profileState.profiles;
			activeProfileId = profileState.activeProfileId;

			viewingProfileId = activeProfileId;
			tools = await getTools(viewingProfileId);
			tools.sort((a, b) => a.category.localeCompare(b.category) || a.displayName.localeCompare(b.displayName));

			// Load detail if viewing a profile
			if (viewingProfileId) {
				await loadProfileDetail(viewingProfileId);
			} else {
				profileDetail = null;
				showProfileEditor = false;
			}
		} catch (e) {
			console.warn('Failed to load tools/profiles:', e);
		} finally {
			isLoadingTools = false;
		}
	}

	async function loadGeneralSettings() {
		if (isLoadingGeneralSettings) return;
		isLoadingGeneralSettings = true;
		generalSettingsError = '';
		try {
			continuationSummarySettings = await getContinuationSummarySettings();
		} catch (e) {
			console.warn('Failed to load continuation summary settings:', e);
			generalSettingsError = $t('summary_settings_error');
		} finally {
			isLoadingGeneralSettings = false;
		}
	}

	async function handleSummaryProviderChange(provider: 'openrouter' | 'local') {
		continuationSummarySettings = { ...continuationSummarySettings, provider };
		try {
			await setContinuationSummaryProvider(provider);
		} catch (e) {
			console.warn('Failed to save summary provider:', e);
		}
	}

	async function handleSummaryModelChange(modelId: string) {
		continuationSummarySettings = { ...continuationSummarySettings, modelId };
		try {
			await setContinuationSummaryModel(modelId);
		} catch (e) {
			console.warn('Failed to save summary model:', e);
		}
	}

	async function handleSummaryDetailChange(detail: 'compact' | 'detailed') {
		continuationSummarySettings = { ...continuationSummarySettings, defaultDetail: detail };
		try {
			await setContinuationSummaryDefaultDetail(detail);
		} catch (e) {
			console.warn('Failed to save summary detail:', e);
		}
	}

	async function handleCheckForUpdates() {
		if (isCheckingForUpdates) return;
		isCheckingForUpdates = true;
		updateCheckMessage = '';
		try {
			await checkForPluginUpdate();
			updateCheckMessage = $t('update_check_started');
		} catch (e) {
			console.warn('Failed to trigger update check:', e);
			updateCheckMessage = $t('update_check_failed');
		} finally {
			isCheckingForUpdates = false;
		}
	}

	function handleLocaleChange(nextLocale: Locale) {
		setLocale(nextLocale);
	}

	async function loadProfileDetail(profileId: string) {
		if (!profileId) {
			profileDetail = null;
			return;
		}
		const detail = await getProfileDetail(profileId);
		if (detail.found) {
			profileDetail = detail;
			editName = detail.displayName;
			editDescription = detail.description;
			editInstructions = detail.customInstructions;
		} else {
			profileDetail = null;
		}
	}

	async function handleActivateProfile(profileId: string) {
		const newId = profileId === activeProfileId ? '' : profileId;
		activeProfileId = newId;
		viewingProfileId = newId;
		await setActiveProfile(newId);
		tools = await getTools(newId);
		tools.sort((a, b) => a.category.localeCompare(b.category) || a.displayName.localeCompare(b.displayName));

		if (newId) {
			await loadProfileDetail(newId);
		} else {
			profileDetail = null;
			showProfileEditor = false;
		}
	}

	async function handleToggleTool(toolName: string, enabled: boolean) {
		tools = tools.map(t => t.name === toolName ? { ...t, enabled } : t);
		if (viewingProfileId) {
			await setProfileToolEnabled(viewingProfileId, toolName, enabled);
		} else {
			await setToolEnabled(toolName, enabled);
		}
	}

	async function handleEnableAll() {
		tools = tools.map(t => ({ ...t, enabled: true }));
		for (const tool of tools) {
			if (viewingProfileId) {
				await setProfileToolEnabled(viewingProfileId, tool.name, true);
			} else {
				await setToolEnabled(tool.name, true);
			}
		}
	}

	async function handleDisableAll() {
		tools = tools.map(t => ({ ...t, enabled: false }));
		for (const tool of tools) {
			if (viewingProfileId) {
				await setProfileToolEnabled(viewingProfileId, tool.name, false);
			} else {
				await setToolEnabled(tool.name, false);
			}
		}
	}

	async function handleCreateProfile() {
		if (!newProfileName.trim()) return;
		const id = await createProfile(newProfileName.trim(), '');
		if (id) {
			showNewProfile = false;
			newProfileName = '';
			// Activate the newly created profile
			activeProfileId = id;
			viewingProfileId = id;
			await setActiveProfile(id);
			await loadProfilesAndTools();
			showProfileEditor = true;
		}
	}

	async function handleDeleteProfile(profileId: string) {
		const ok = await deleteProfile(profileId);
		if (ok) {
			showProfileEditor = false;
			profileDetail = null;
			await loadProfilesAndTools();
		}
	}

	async function handleSaveProfile() {
		if (!viewingProfileId || !profileDetail) return;
		const ok = await updateProfile(viewingProfileId, editName, editDescription, editInstructions);
		if (ok) {
			// Reload profiles to reflect name changes
			const profileState = await getProfiles();
			profiles = profileState.profiles;
			profileDetail = { ...profileDetail, displayName: editName, description: editDescription, customInstructions: editInstructions };
		}
	}

	async function handleSaveToolOverride(toolName: string) {
		if (!viewingProfileId) return;
		await setToolDescriptionOverride(viewingProfileId, toolName, overrideText.trim());
		// Update local state
		tools = tools.map(t => t.name === toolName ? { ...t, descriptionOverride: overrideText.trim() } : t);
		editingToolOverride = '';
		overrideText = '';
	}

	function startEditingOverride(tool: ToolInfo) {
		editingToolOverride = tool.name;
		overrideText = tool.descriptionOverride || tool.description;
	}

	function cancelEditingOverride() {
		editingToolOverride = '';
		overrideText = '';
	}

	async function clearToolOverride(toolName: string) {
		if (!viewingProfileId) return;
		await setToolDescriptionOverride(viewingProfileId, toolName, '');
		tools = tools.map(t => t.name === toolName ? { ...t, descriptionOverride: '' } : t);
	}

	function toggleCategory(cat: string) {
		const next = new Set(collapsedCategories);
		if (next.has(cat)) next.delete(cat);
		else next.add(cat);
		collapsedCategories = next;
	}

	// Debounce helper for auto-save
	let saveTimeout: ReturnType<typeof setTimeout> | undefined;
	function debouncedSaveProfile() {
		clearTimeout(saveTimeout);
		saveTimeout = setTimeout(() => handleSaveProfile(), 600);
	}
</script>

<div class="flex h-full w-full">
	<!-- Left tab nav -->
	<nav class="flex w-[200px] shrink-0 flex-col border-r border-border bg-sidebar">
		<div class="flex items-center gap-2 px-4 pt-4 pb-3">
			<button
				class="rounded p-1 text-muted-foreground transition-colors hover:bg-sidebar-accent hover:text-foreground"
				onclick={closeSettings}
				title={$t('back_to_chat')}
			>
				<Icon icon={ArrowLeft02Icon} size={16} strokeWidth={1.5} />
			</button>
			<span class="text-[14px] font-medium text-foreground">{$t('settings')}</span>
		</div>

		<div class="flex flex-col gap-0.5 px-2">
			{#each tabs as tab}
				<button
					class="flex items-center gap-2.5 rounded-md px-3 py-2 text-[13px] transition-colors {$settingsTab === tab.id
						? 'bg-sidebar-accent text-foreground'
						: 'text-muted-foreground hover:bg-sidebar-accent/60 hover:text-foreground'}"
					onclick={() => settingsTab.set(tab.id)}
				>
					<Icon icon={tab.icon} size={15} strokeWidth={1.5} />
					{tab.label}
				</button>
			{/each}
		</div>
	</nav>

	<!-- Right content area -->
	<div class="flex-1 overflow-y-auto p-8">
		<div class="mx-auto max-w-3xl">
			{#if $settingsTab === 'tools'}
				<!-- ── Tool Profiles Tab ──────────────────────────── -->
				<div class="mb-6">
					<h2 class="mb-1 text-[18px] font-medium text-foreground">{$t('tool_profiles_heading')}</h2>
					<p class="text-[13px] text-muted-foreground/60">{$t('tool_profiles_desc')}</p>
				</div>

				{#if isLoadingTools}
					<div class="flex items-center gap-2 py-8 text-muted-foreground/50">
						<span class="inline-block h-4 w-4 animate-spin rounded-full border-2 border-muted-foreground/30 border-t-muted-foreground"></span>
						{$t('loading_tools')}
					</div>
				{:else}
					<!-- Profile cards -->
					<div class="mb-6 flex flex-wrap gap-2">
						<!-- "All Tools" card (no profile) -->
						<button
							class="flex items-center gap-2 rounded-lg border px-3 py-2 text-[13px] transition-colors {activeProfileId === ''
								? 'border-foreground/30 bg-foreground/5 text-foreground'
								: 'border-border/60 text-muted-foreground hover:border-border hover:text-foreground'}"
							onclick={() => handleActivateProfile('')}
						>
							{#if activeProfileId === ''}
								<Icon icon={Tick02Icon} size={14} strokeWidth={2} class="text-emerald-500" />
							{/if}
							{$t('all_tools')}
						</button>
						{#each profiles as profile}
							<button
								class="group flex items-center gap-2 rounded-lg border px-3 py-2 text-[13px] transition-colors {profile.isActive
									? 'border-foreground/30 bg-foreground/5 text-foreground'
									: 'border-border/60 text-muted-foreground hover:border-border hover:text-foreground'}"
								onclick={() => handleActivateProfile(profile.profileId)}
							>
								{#if profile.isActive}
									<Icon icon={Tick02Icon} size={14} strokeWidth={2} class="text-emerald-500" />
								{/if}
								{profile.displayName}
								{#if profile.enabledToolCount > 0}
									<span class="text-[11px] text-muted-foreground/40">{profile.enabledToolCount}</span>
								{/if}
								{#if !profile.isBuiltIn}
									<!-- svelte-ignore a11y_no_static_element_interactions -->
									<span
										role="button"
										tabindex="-1"
										class="ml-0.5 rounded p-0.5 text-muted-foreground/30 opacity-0 transition-all hover:text-red-400 group-hover:opacity-100"
										onclick={(e) => { e.stopPropagation(); handleDeleteProfile(profile.profileId); }}
										onkeydown={(e) => { if (e.key === 'Enter') { e.stopPropagation(); handleDeleteProfile(profile.profileId); } }}
										title={$t('delete_profile')}
									>
										<Icon icon={Delete02Icon} size={12} strokeWidth={1.5} />
									</span>
								{/if}
							</button>
						{/each}
						<!-- New profile button -->
						{#if showNewProfile}
							<div class="flex items-center gap-1">
								<input
									bind:this={newProfileInputEl}
									bind:value={newProfileName}
									class="h-[36px] w-[140px] rounded-lg border border-border bg-transparent px-2.5 text-[13px] text-foreground placeholder:text-muted-foreground/40 focus:border-foreground/30 focus:outline-none"
									placeholder={$t('profile_name')}
									onkeydown={(e) => { if (e.key === 'Enter') handleCreateProfile(); if (e.key === 'Escape') { showNewProfile = false; newProfileName = ''; } }}
								/>
								<button
									class="rounded-md px-2 py-1.5 text-[12px] text-foreground transition-colors hover:bg-accent"
									onclick={handleCreateProfile}
								>{$t('save')}</button>
							</div>
						{:else}
							<button
								class="flex items-center gap-1.5 rounded-lg border border-dashed border-border/60 px-3 py-2 text-[13px] text-muted-foreground/50 transition-colors hover:border-border hover:text-muted-foreground"
								onclick={() => { showNewProfile = true; requestAnimationFrame(() => newProfileInputEl?.focus()); }}
							>
								<Icon icon={Add01Icon} size={14} strokeWidth={1.5} />
								{$t('new_profile')}
							</button>
						{/if}
					</div>

					<!-- Profile editor (only for custom profiles) -->
					{#if profileDetail && !profileDetail.isBuiltIn && viewingProfileId}
						<div class="mb-6 rounded-lg border border-border/60 bg-card">
							<button
								class="flex w-full items-center justify-between px-4 py-3 text-left"
								onclick={() => showProfileEditor = !showProfileEditor}
							>
								<div class="flex items-center gap-2">
									<Icon icon={Edit02Icon} size={14} strokeWidth={1.5} class="text-muted-foreground" />
									<span class="text-[13px] font-medium text-foreground">{$t('profile_settings')}</span>
									{#if profileDetail.customInstructions || Object.keys(profileDetail.toolDescriptionOverrides).length > 0}
										<span class="rounded-full bg-foreground/10 px-1.5 py-0.5 text-[10px] text-muted-foreground">{$t('customized')}</span>
									{/if}
								</div>
								<Icon icon={showProfileEditor ? ArrowDown01Icon : ArrowRight01Icon} size={14} strokeWidth={1.5} class="text-muted-foreground/50" />
							</button>

							{#if showProfileEditor}
								<div class="border-t border-border/40 px-4 py-4">
									<div class="flex flex-col gap-4">
										<!-- Profile name -->
										<div>
											<span class="mb-1.5 block text-[12px] font-medium text-muted-foreground">{$t('profile_name')}</span>
											<input
												type="text"
												bind:value={editName}
												oninput={debouncedSaveProfile}
												class="w-full rounded-md border border-border/60 bg-transparent px-3 py-2 text-[13px] text-foreground placeholder:text-muted-foreground/40 focus:border-foreground/30 focus:outline-none"
												placeholder={$t('profile_name')}
											/>
										</div>

										<!-- Description -->
										<div>
											<span class="mb-1.5 block text-[12px] font-medium text-muted-foreground">{$t('description')}</span>
											<input
												type="text"
												bind:value={editDescription}
												oninput={debouncedSaveProfile}
												class="w-full rounded-md border border-border/60 bg-transparent px-3 py-2 text-[13px] text-foreground placeholder:text-muted-foreground/40 focus:border-foreground/30 focus:outline-none"
												placeholder={$t('profile_description_placeholder')}
											/>
										</div>

										<!-- Custom Instructions -->
										<div>
											<span class="mb-1.5 block text-[12px] font-medium text-muted-foreground">{$t('custom_system_instructions')}</span>
											<p class="mb-2 text-[11px] text-muted-foreground/50">{$t('custom_system_instructions_desc')}</p>
											<textarea
												bind:value={editInstructions}
												oninput={debouncedSaveProfile}
												class="w-full resize-y rounded-md border border-border/60 bg-transparent px-3 py-2 text-[13px] leading-relaxed text-foreground placeholder:text-muted-foreground/40 focus:border-foreground/30 focus:outline-none"
												placeholder={$t('custom_instructions_placeholder')}
												rows={4}
											></textarea>
										</div>

										<!-- Overrides summary -->
										{#if tools.some(t => t.descriptionOverride)}
											{@const overrideCount = tools.filter(t => t.descriptionOverride).length}
											<div class="flex items-center gap-2 text-[12px] text-muted-foreground/50">
												<Icon icon={TextIcon} size={13} strokeWidth={1.5} />
												{overrideCount} {overrideCount === 1 ? $t('tool_override_active_one') : $t('tool_override_active_many')}
											</div>
										{/if}
									</div>
								</div>
							{/if}
						</div>
					{/if}

					<!-- Viewing indicator + bulk actions + search -->
					<div class="mb-4 flex flex-col gap-3">
						<div class="flex items-center justify-between">
							<div class="flex items-center gap-3">
								<span class="text-[13px] text-muted-foreground">
									{#if viewingProfileId}
										{@const vp = profiles.find(p => p.profileId === viewingProfileId)}
										{$t('showing_tools_for')} <span class="font-medium text-foreground">{vp?.displayName ?? $t('profile')}</span>
									{:else}
										{$t('showing_global_tool_settings')}
									{/if}
								</span>
								<span class="text-[12px] text-muted-foreground/40">{$t('enabled_fraction', { enabled: enabledCount, total: totalCount })}</span>
							</div>
							<div class="flex items-center gap-1.5">
								<button
									class="rounded-md px-2 py-1 text-[12px] text-muted-foreground transition-colors hover:bg-accent hover:text-foreground"
									onclick={handleEnableAll}
								>{$t('enable_all')}</button>
								<button
									class="rounded-md px-2 py-1 text-[12px] text-muted-foreground transition-colors hover:bg-accent hover:text-foreground"
									onclick={handleDisableAll}
								>{$t('disable_all')}</button>
							</div>
						</div>
						<!-- Search bar -->
						<input
							type="text"
							bind:value={searchQuery}
							class="w-full rounded-md border border-border/40 bg-transparent px-3 py-1.5 text-[13px] text-foreground placeholder:text-muted-foreground/30 focus:border-foreground/20 focus:outline-none"
							placeholder={$t('search_tools')}
						/>
					</div>

					<!-- Tools by category -->
					<div class="flex flex-col gap-1">
						{#each filteredCategoryNames as category}
							{@const catTools = filteredToolsByCategory[category]}
							{@const catEnabled = catTools.filter(t => t.enabled).length}
							{@const isCollapsed = collapsedCategories.has(category)}
							<!-- Category header -->
							<button
								class="flex items-center gap-2 rounded-md px-2 py-1.5 text-[12px] font-medium uppercase tracking-wider text-muted-foreground/60 transition-colors hover:bg-accent/40"
								onclick={() => toggleCategory(category)}
							>
								<Icon icon={isCollapsed ? ArrowRight01Icon : ArrowDown01Icon} size={12} strokeWidth={1.5} />
								{category}
								<span class="font-normal normal-case tracking-normal text-muted-foreground/30">{catEnabled}/{catTools.length}</span>
							</button>
							{#if !isCollapsed}
								<div class="mb-2 flex flex-col">
									{#each catTools as tool}
										<div class="group rounded-md px-3 py-2 transition-colors hover:bg-accent/30">
											<div class="flex items-start gap-3">
												<input
													type="checkbox"
													checked={tool.enabled}
													onchange={() => handleToggleTool(tool.name, !tool.enabled)}
													class="mt-0.5 h-4 w-4 shrink-0 cursor-pointer rounded border-border accent-foreground"
												/>
												<div class="min-w-0 flex-1">
													<div class="flex items-baseline gap-2">
														<span class="text-[13px] font-medium text-foreground">{tool.displayName}</span>
														<span class="text-[11px] text-muted-foreground/40">{tool.name}</span>
														{#if tool.descriptionOverride}
															<span class="rounded bg-blue-500/10 px-1 py-0.5 text-[10px] text-blue-400">{$t('overridden')}</span>
														{/if}
													</div>
													<!-- Show override if present, else default description -->
													{#if tool.descriptionOverride}
														<p class="mt-0.5 text-[12px] leading-relaxed text-blue-300/70">{tool.descriptionOverride}</p>
														<p class="mt-0.5 text-[11px] leading-relaxed text-muted-foreground/30 line-through">{tool.description}</p>
													{:else}
														<p class="mt-0.5 text-[12px] leading-relaxed text-muted-foreground/60">{tool.description}</p>
													{/if}

													<!-- Description override editor (only for custom profiles) -->
													{#if viewingProfileId && profileDetail && !profileDetail.isBuiltIn}
														{#if editingToolOverride === tool.name}
															<div class="mt-2 flex flex-col gap-1.5">
																<textarea
																	bind:value={overrideText}
																	class="w-full resize-y rounded border border-border/60 bg-transparent px-2.5 py-1.5 text-[12px] leading-relaxed text-foreground placeholder:text-muted-foreground/40 focus:border-foreground/30 focus:outline-none"
																	placeholder={$t('custom_description_placeholder')}
																	rows={2}
																	onkeydown={(e) => { if (e.key === 'Escape') cancelEditingOverride(); }}
																></textarea>
																<div class="flex gap-1.5">
																	<button
																		class="rounded px-2 py-1 text-[11px] font-medium text-foreground transition-colors hover:bg-accent"
																		onclick={() => handleSaveToolOverride(tool.name)}
																	>{$t('save')}</button>
																	<button
																		class="rounded px-2 py-1 text-[11px] text-muted-foreground transition-colors hover:bg-accent"
																		onclick={cancelEditingOverride}
																	>{$t('cancel')}</button>
																</div>
															</div>
														{:else}
															<div class="mt-1 flex items-center gap-1.5 opacity-0 transition-opacity group-hover:opacity-100">
																<button
																	class="flex items-center gap-1 rounded px-1.5 py-0.5 text-[11px] text-muted-foreground/50 transition-colors hover:bg-accent hover:text-foreground"
																	onclick={() => startEditingOverride(tool)}
																>
																	<Icon icon={Edit02Icon} size={11} strokeWidth={1.5} />
																	{tool.descriptionOverride ? $t('edit_override') : $t('override_description')}
																</button>
																{#if tool.descriptionOverride}
																	<button
																		class="flex items-center gap-1 rounded px-1.5 py-0.5 text-[11px] text-muted-foreground/50 transition-colors hover:bg-red-500/10 hover:text-red-400"
																		onclick={() => clearToolOverride(tool.name)}
																	>
																		<Icon icon={Cancel01Icon} size={11} strokeWidth={1.5} />
																		{$t('clear')}
																	</button>
																{/if}
															</div>
														{/if}
													{/if}
												</div>
											</div>
										</div>
									{/each}
								</div>
							{/if}
						{/each}
					</div>
				{/if}
			{:else if $settingsTab === 'general'}
				<div class="mb-6">
					<h2 class="mb-1 text-[18px] font-medium text-foreground">{$t('general_heading')}</h2>
					<p class="text-[13px] text-muted-foreground/60">{$t('general_desc')}</p>
				</div>

				{#if isLoadingGeneralSettings}
					<div class="flex items-center gap-2 py-8 text-muted-foreground/50">
						<span class="inline-block h-4 w-4 animate-spin rounded-full border-2 border-muted-foreground/30 border-t-muted-foreground"></span>
						{$t('loading_summary_settings')}
					</div>
				{:else}
					<div class="mb-4 rounded-lg border border-border/60 bg-card p-4">
						<h3 class="mb-1 text-[14px] font-medium text-foreground">{$t('language_heading')}</h3>
						<p class="mb-3 text-[12px] text-muted-foreground/60">{$t('language_desc')}</p>
						<div class="flex flex-wrap gap-2">
							{#each locales as currentLocale}
								<button
									class="rounded-md border px-3 py-1.5 text-[13px] transition-colors {$locale === currentLocale
										? 'border-[var(--ue-accent)] bg-[var(--ue-accent)]/10 text-foreground'
										: 'border-border/60 text-muted-foreground hover:border-border hover:text-foreground'}"
									onclick={() => handleLocaleChange(currentLocale)}
								>
									{localeNames[currentLocale]}
								</button>
							{/each}
						</div>
					</div>

					<div class="mb-4 rounded-lg border border-border/60 bg-card p-4">
						<div class="flex items-center justify-between">
							<div>
								<h3 class="text-[14px] font-medium text-foreground">Enter to send</h3>
								<p class="mt-0.5 text-[12px] text-muted-foreground/60">
									{$enterToSend
										? 'Enter sends message, Shift+Enter for new line'
										: 'Enter for new line, Cmd/Ctrl+Enter sends message'}
								</p>
							</div>
							<button
								role="switch"
								aria-checked={$enterToSend}
								class="relative inline-flex h-5 w-9 shrink-0 cursor-pointer items-center rounded-full transition-colors {$enterToSend ? 'bg-[var(--ue-accent)]' : 'bg-muted-foreground/30'}"
								onclick={() => enterToSend.set(!$enterToSend)}
							>
								<span class="pointer-events-none inline-block h-3.5 w-3.5 rounded-full bg-white shadow-sm transition-transform {$enterToSend ? 'translate-x-[18px]' : 'translate-x-[3px]'}"></span>
							</button>
						</div>
					</div>

					<div class="rounded-lg border border-border/60 bg-card p-4">
						<div class="mb-4">
							<h3 class="text-[14px] font-medium text-foreground">{$t('handoff_heading')}</h3>
							<p class="mt-1 text-[12px] text-muted-foreground/60">{$t('handoff_desc')}</p>
						</div>

						<div class="grid gap-4">
							<div>
								<label for="continuation-provider" class="mb-1.5 block text-[12px] font-medium text-muted-foreground">{$t('provider_label')}</label>
								<CustomSelect
									id="continuation-provider"
									value={continuationSummarySettings.provider}
									options={[
										{ value: 'openrouter', label: $t('provider_openrouter') },
										{ value: 'local', label: $t('provider_local') }
									]}
									onchange={(v) => handleSummaryProviderChange(v as 'openrouter' | 'local')}
								/>
								<p class="mt-1 text-[11px] text-muted-foreground/50">
									{$t('provider_help')}
								</p>
							</div>

							<div>
								<label for="continuation-model" class="mb-1.5 block text-[12px] font-medium text-muted-foreground">{$t('openrouter_model_label')}</label>
								<input
									id="continuation-model"
									type="text"
									value={continuationSummarySettings.modelId}
									onchange={(e) => handleSummaryModelChange((e.currentTarget as HTMLInputElement).value)}
									class="w-full rounded-md border border-border/60 bg-transparent px-3 py-2 text-[13px] text-foreground placeholder:text-muted-foreground/40 focus:border-foreground/30 focus:outline-none"
									placeholder="x-ai/grok-4.1-fast"
								/>
								<p class="mt-1 text-[11px] text-muted-foreground/50">
									{$t('openrouter_model_help')}
								</p>
							</div>

							<div>
								<label for="continuation-detail" class="mb-1.5 block text-[12px] font-medium text-muted-foreground">{$t('default_detail_label')}</label>
								<CustomSelect
									id="continuation-detail"
									value={continuationSummarySettings.defaultDetail}
									options={[
										{ value: 'compact', label: $t('compact') },
										{ value: 'detailed', label: $t('detailed') }
									]}
									onchange={(v) => handleSummaryDetailChange(v as 'compact' | 'detailed')}
								/>
							</div>
						</div>

						{#if continuationSummarySettings.provider === 'openrouter' && !continuationSummarySettings.hasOpenRouterKey}
							<div class="mt-4 rounded-md border border-amber-500/30 bg-amber-500/10 px-3 py-2 text-[12px] text-amber-300">
								{$t('openrouter_missing_key')}
							</div>
						{/if}

						{#if generalSettingsError}
							<div class="mt-3 text-[12px] text-red-400">{generalSettingsError}</div>
						{/if}
					</div>
				{/if}
			{:else if $settingsTab === 'indexing'}
				<ProjectIndexPanel bind:this={indexPanel} />
			{:else if $settingsTab === 'about'}
				<div class="mb-6">
					<h2 class="mb-1 text-[18px] font-medium text-foreground">{$t('about_heading')}</h2>
					<p class="text-[13px] text-muted-foreground/60">{$t('about_desc')}</p>
				</div>

				<div class="rounded-lg border border-border/60 bg-card p-4">
					<div class="mb-3">
						<h3 class="text-[14px] font-medium text-foreground">{$t('updates_heading')}</h3>
						<p class="mt-1 text-[12px] text-muted-foreground/60">{$t('updates_desc')}</p>
					</div>

					<button
						class="rounded-md border border-border/60 px-3 py-2 text-[13px] text-foreground transition-colors hover:bg-accent disabled:cursor-not-allowed disabled:opacity-60"
						onclick={handleCheckForUpdates}
						disabled={isCheckingForUpdates}
					>
						{isCheckingForUpdates ? $t('checking') : $t('check_for_updates')}
					</button>

					{#if updateCheckMessage}
						<p class="mt-2 text-[12px] text-muted-foreground/70">{updateCheckMessage}</p>
					{/if}
				</div>
			{:else if $settingsTab === 'agents'}
				<!-- ACP Agent Registry -->
				<div class="mb-6">
					<AgentRegistry />
				</div>

				<div class="border-t border-border/40 pt-6 mb-6">
					<h2 class="mb-1 text-[18px] font-medium text-foreground">{$t('providers_heading')}</h2>
					<p class="text-[13px] text-muted-foreground/60">{$t('providers_desc')}</p>
				</div>

				{#if isLoadingProviders}
					<div class="flex items-center gap-2 py-8 text-muted-foreground/50">
						<span class="inline-block h-4 w-4 animate-spin rounded-full border-2 border-muted-foreground/30 border-t-muted-foreground"></span>
						Loading...
					</div>
				{:else}
					<!-- Provider priority list -->
					<div class="mb-4 rounded-lg border border-border/60 bg-card p-4">
						<h3 class="mb-1 text-[14px] font-medium text-foreground">{$t('active_provider_label')}</h3>
						<p class="mb-3 text-[12px] text-muted-foreground/60">{$t('active_provider_desc')}</p>

						{#if priorityProviders.length === 0}
							<p class="py-3 text-[12px] text-muted-foreground/40">No providers configured. Add one below.</p>
						{:else}
							<div class="flex flex-col gap-1">
								{#each priorityProviders as provider, i}
									<div class="group flex items-center gap-2 rounded-md border border-border/40 px-3 py-2 transition-colors hover:bg-accent/20 {selectedProviderId === provider.id ? 'border-[var(--ue-accent)]/30 bg-[var(--ue-accent)]/5' : ''}">
										<!-- Priority number -->
										<span class="w-5 text-center text-[11px] font-medium text-muted-foreground/40">{i + 1}</span>
										<!-- Up/Down arrows -->
										<div class="flex flex-col gap-0">
											<button class="rounded px-0.5 text-[10px] text-muted-foreground/30 transition-colors hover:text-foreground disabled:opacity-20" disabled={i === 0} onclick={() => handleMoveProvider(provider.id, 'up')}>&uarr;</button>
											<button class="rounded px-0.5 text-[10px] text-muted-foreground/30 transition-colors hover:text-foreground disabled:opacity-20" disabled={i === priorityProviders.length - 1} onclick={() => handleMoveProvider(provider.id, 'down')}>&darr;</button>
										</div>
										<!-- Provider info -->
										<button class="flex-1 text-left" onclick={() => { selectedProviderId = provider.id; providerApiKeyInput = ''; showAddModelForm = false; showImportModal = false; showDeleteConfirm = ''; }}>
											<span class="text-[13px] font-medium text-foreground">{provider.name}</span>
											{#if provider.isUserDefined}
												<span class="ml-1 rounded bg-[var(--ue-accent)]/10 px-1 py-0.5 text-[9px] text-[var(--ue-accent)]/70">custom</span>
											{/if}
											{#if provider.configured}
												<span class="ml-1.5 text-[10px] text-emerald-400">&#x2022; ready</span>
											{:else if provider.requiresApiKey && !provider.hasApiKey}
												<span class="ml-1.5 text-[10px] text-amber-400">&#x2022; needs key</span>
											{/if}
										</button>
										<!-- Remove button -->
										<button class="rounded p-1 text-muted-foreground/20 opacity-0 transition-all hover:text-red-400 group-hover:opacity-100" onclick={() => handleRemoveProvider(provider.id)} title="Remove">&#x2715;</button>
									</div>
								{/each}
							</div>
						{/if}

						<!-- Add built-in provider -->
						{#if builtinAvailableToAdd.length > 0}
							<div class="mt-3 flex flex-wrap gap-1.5">
								{#each builtinAvailableToAdd as provider}
									<button
										class="rounded-md border border-dashed border-border/40 px-2.5 py-1 text-[12px] text-muted-foreground/40 transition-colors hover:border-border hover:text-muted-foreground"
										onclick={() => handleAddProvider(provider.id)}
									>+ {provider.name}</button>
								{/each}
							</div>
						{/if}

						<!-- Add custom provider -->
						<div class="mt-3 border-t border-border/20 pt-3">
							{#if showNewCustomProviderForm}
								<div class="flex flex-col gap-2">
									<input type="text" bind:value={newCustomProviderName} class="w-full rounded-md border border-border/60 bg-transparent px-3 py-2 text-[13px] text-foreground placeholder:text-muted-foreground/40 focus:border-foreground/30 focus:outline-none" placeholder="Provider name (e.g. My vLLM Server)" />
									<input type="text" bind:value={newCustomProviderUrl} class="w-full rounded-md border border-border/60 bg-transparent px-3 py-2 text-[13px] text-foreground placeholder:text-muted-foreground/40 focus:border-foreground/30 focus:outline-none" placeholder="Base URL (e.g. http://localhost:8000/v1)" />
									<div class="flex gap-2">
										<button class="rounded-md border border-border/60 px-3 py-1.5 text-[12px] text-foreground transition-colors hover:bg-accent disabled:cursor-not-allowed disabled:opacity-40" onclick={handleCreateCustomProvider} disabled={!newCustomProviderName.trim()}>Create</button>
										<button class="rounded-md px-3 py-1.5 text-[12px] text-muted-foreground/60 transition-colors hover:text-foreground" onclick={() => { showNewCustomProviderForm = false; newCustomProviderName = ''; newCustomProviderUrl = ''; }}>Cancel</button>
									</div>
								</div>
							{:else}
								<button
									class="rounded-md border border-dashed border-[var(--ue-accent)]/30 px-2.5 py-1 text-[12px] text-[var(--ue-accent)]/60 transition-colors hover:border-[var(--ue-accent)]/60 hover:text-[var(--ue-accent)]"
									onclick={() => { showNewCustomProviderForm = true; }}
								>+ Add Custom Provider</button>
							{/if}
						</div>
					</div>

					<!-- Selected provider config -->
					{#if selectedProvider}
						<div class="mb-4 rounded-lg border border-border/60 bg-card p-4">
							<div class="mb-3 flex items-start justify-between">
								<div>
									<div class="flex items-center gap-2">
										<h4 class="text-[14px] font-medium text-foreground">{selectedProvider.name}</h4>
										{#if selectedProvider.isUserDefined}
											<span class="rounded bg-[var(--ue-accent)]/10 px-1.5 py-0.5 text-[9px] font-medium text-[var(--ue-accent)]">custom</span>
										{/if}
									</div>
									<p class="mt-0.5 text-[12px] text-muted-foreground/60">{selectedProvider.description}</p>
								</div>
								{#if selectedProvider.isUserDefined}
									{#if showDeleteConfirm === selectedProvider.id}
										<div class="flex items-center gap-1.5">
											<span class="text-[11px] text-red-400">Delete?</span>
											<button class="rounded px-2 py-0.5 text-[11px] text-red-400 transition-colors hover:bg-red-500/10" onclick={() => handleDeleteCustomProvider(selectedProvider.id)}>Yes</button>
											<button class="rounded px-2 py-0.5 text-[11px] text-muted-foreground transition-colors hover:bg-accent" onclick={() => { showDeleteConfirm = ''; }}>No</button>
										</div>
									{:else}
										<button class="rounded p-1 text-muted-foreground/30 transition-colors hover:text-red-400" onclick={() => { showDeleteConfirm = selectedProvider.id; }} title="Delete provider">&#x2715;</button>
									{/if}
								{/if}
							</div>

							<div class="grid gap-4">
								<!-- API Key -->
								{#if selectedProvider.requiresApiKey}
									<div>
										<span class="mb-1.5 block text-[12px] font-medium text-muted-foreground">{$t('provider_api_key_label')}</span>
										{#if selectedProvider.hasApiKey}
											<div class="mb-1.5 flex items-center gap-2">
												<span class="font-mono text-[12px] text-muted-foreground/50">····{selectedProvider.apiKeyMasked.slice(-4)}</span>
												<span class="rounded-full bg-emerald-500/10 px-1.5 py-0.5 text-[10px] text-emerald-400">{$t('provider_key_set')}</span>
											</div>
										{/if}
										<div class="flex gap-2">
											<input type="password" bind:value={providerApiKeyInput} class="flex-1 rounded-md border border-border/60 bg-transparent px-3 py-2 text-[13px] text-foreground placeholder:text-muted-foreground/40 focus:border-foreground/30 focus:outline-none" placeholder={selectedProvider.hasApiKey ? $t('provider_api_key_replace') : $t('provider_api_key_enter')} />
											<button class="rounded-md border border-border/60 px-3 py-2 text-[13px] text-foreground transition-colors hover:bg-accent disabled:cursor-not-allowed disabled:opacity-40" onclick={() => handleProviderApiKeySave(selectedProviderId, providerApiKeyInput)} disabled={!providerApiKeyInput.trim()}>{$t('save')}</button>
										</div>
									</div>
								{:else}
									<div class="rounded-md bg-emerald-500/5 px-3 py-2 text-[12px] text-emerald-400/80">{$t('provider_no_key_needed')}</div>
								{/if}

								<!-- Base URL -->
								<div>
									<span class="mb-1.5 block text-[12px] font-medium text-muted-foreground">{$t('provider_base_url_label')}</span>
									{#if selectedProvider.isUserDefined}
										<input type="text" value={selectedProvider.baseUrl} oninput={(e) => { const url = (e.currentTarget as HTMLInputElement).value; providerSettings = { ...providerSettings, providers: providerSettings.providers.map(p => p.id === selectedProviderId ? { ...p, baseUrl: url } : p) }; debouncedCustomProviderUpdate(selectedProviderId, '', url); }} class="w-full rounded-md border border-border/60 bg-transparent px-3 py-2 text-[13px] text-foreground placeholder:text-muted-foreground/40 focus:border-foreground/30 focus:outline-none" placeholder="https://api.example.com/v1" />
									{:else}
										<input type="text" value={selectedProvider.baseUrl} oninput={(e) => { const url = (e.currentTarget as HTMLInputElement).value; providerSettings = { ...providerSettings, providers: providerSettings.providers.map(p => p.id === selectedProviderId ? { ...p, baseUrl: url } : p) }; debouncedProviderBaseUrl(selectedProviderId, url); }} class="w-full rounded-md border border-border/60 bg-transparent px-3 py-2 text-[13px] text-foreground placeholder:text-muted-foreground/40 focus:border-foreground/30 focus:outline-none" placeholder={selectedProvider.defaultBaseUrl} />
									{/if}
									<p class="mt-1 text-[11px] text-muted-foreground/50">{$t('provider_base_url_help')}</p>
								</div>

								<!-- Provider: Model Management -->
								{#if selectedProvider.isUserDefined || selectedProvider.supportsModelDiscovery}
									<div class="border-t border-border/30 pt-3">
										<div class="mb-2 flex items-center justify-between">
											<span class="text-[12px] font-medium text-muted-foreground">Models</span>
											<div class="flex items-center gap-2">
												{#if !selectedProvider.isUserDefined && selectedProvider.supportsModelDiscovery}
													<button class="rounded-md border border-border/60 px-2 py-0.5 text-[11px] text-muted-foreground transition-colors hover:bg-accent hover:text-foreground" onclick={async () => { await refreshProviderModels(); }}>Refresh</button>
												{/if}
												{#if selectedProvider.isUserDefined}
													<button class="rounded-md border border-border/60 px-2 py-0.5 text-[11px] text-muted-foreground transition-colors hover:bg-accent hover:text-foreground" onclick={() => { importJsonText = ''; importResult = null; showImportModal = true; }}>Import JSON</button>
												{/if}
												<button class="rounded-md border border-border/60 px-2 py-0.5 text-[11px] text-muted-foreground transition-colors hover:bg-accent hover:text-foreground" onclick={() => { newModelId = ''; newModelName = ''; newModelDesc = ''; showAddModelForm = !showAddModelForm; }}>+ Add</button>
											</div>
										</div>
										{#if !selectedProvider.isUserDefined && selectedProvider.supportsModelDiscovery}
											<p class="mb-2 text-[11px] text-muted-foreground/50">Models are auto-discovered from the provider's /models endpoint. Use + Add for models not yet discovered.</p>
										{/if}

										<!-- Add model form -->
										{#if showAddModelForm}
											<div class="mb-2 flex flex-col gap-1.5 rounded-md border border-border/40 bg-background/50 p-2">
												<input type="text" bind:value={newModelId} class="w-full rounded border border-border/40 bg-transparent px-2 py-1 text-[12px] text-foreground placeholder:text-muted-foreground/40 focus:border-foreground/30 focus:outline-none" placeholder="Model ID (required, e.g. gpt-4o)" />
												<input type="text" bind:value={newModelName} class="w-full rounded border border-border/40 bg-transparent px-2 py-1 text-[12px] text-foreground placeholder:text-muted-foreground/40 focus:border-foreground/30 focus:outline-none" placeholder="Display name (optional)" />
												<input type="text" bind:value={newModelDesc} class="w-full rounded border border-border/40 bg-transparent px-2 py-1 text-[12px] text-foreground placeholder:text-muted-foreground/40 focus:border-foreground/30 focus:outline-none" placeholder="Description (optional)" />
												<div class="flex gap-1.5">
													<button class="rounded border border-border/60 px-2 py-0.5 text-[11px] text-foreground transition-colors hover:bg-accent disabled:opacity-40" onclick={() => handleAddModel(selectedProviderId)} disabled={!newModelId.trim()}>Add</button>
													<button class="rounded px-2 py-0.5 text-[11px] text-muted-foreground/60 transition-colors hover:text-foreground" onclick={() => { showAddModelForm = false; }}>Cancel</button>
												</div>
											</div>
										{/if}

										<!-- Model list -->
										{#if selectedProvider.models && selectedProvider.models.length > 0}
											<div class="max-h-[200px] overflow-y-auto rounded-md border border-border/30">
												{#each selectedProvider.models as model}
													<div class="group flex items-center gap-2 border-b border-border/20 px-3 py-1.5 last:border-b-0">
														<div class="min-w-0 flex-1">
															<span class="text-[12px] text-foreground">{model.name || model.id}</span>
															<span class="ml-1.5 text-[10px] text-muted-foreground/40">{model.id}</span>
														</div>
														<button class="rounded p-0.5 text-muted-foreground/20 opacity-0 transition-all hover:text-red-400 group-hover:opacity-100" onclick={() => handleRemoveModel(selectedProviderId, model.id)} title="Remove">&#x2715;</button>
													</div>
												{/each}
											</div>
										{:else if selectedProvider.isUserDefined}
											<p class="py-2 text-[11px] text-muted-foreground/40">No models defined. Add manually or import from JSON.</p>
										{:else}
											<p class="py-2 text-[11px] text-muted-foreground/40">No extra models added. Discovered models appear in the model picker.</p>
										{/if}

										<!-- Model discovery toggle (custom providers only) -->
										{#if selectedProvider.isUserDefined}
											<label class="mt-2 flex cursor-pointer items-center gap-2">
												<input type="checkbox" checked={selectedProvider.enableModelDiscovery} onchange={(e) => handleToggleModelDiscovery(selectedProviderId, (e.currentTarget as HTMLInputElement).checked)} class="h-3.5 w-3.5 rounded border-border accent-[var(--ue-accent)]" />
												<span class="text-[12px] text-muted-foreground">Auto-discover models from /models endpoint</span>
											</label>
										{/if}
									</div>

									<!-- Import Modal (custom providers only) -->
									{#if showImportModal && selectedProvider.isUserDefined}
										<div class="border-t border-border/30 pt-3">
											<span class="mb-1.5 block text-[12px] font-medium text-muted-foreground">Import Models from JSON</span>
											<p class="mb-2 text-[11px] text-muted-foreground/50">Paste a JSON array of models. Format: [&#123;"id": "model-id", "name": "Display Name"&#125;] or an OpenAI /models response.</p>
											<textarea bind:value={importJsonText} class="h-[120px] w-full resize-y rounded-md border border-border/60 bg-transparent px-3 py-2 font-mono text-[11px] text-foreground placeholder:text-muted-foreground/40 focus:border-foreground/30 focus:outline-none" placeholder={'[{"id": "my-model", "name": "My Model"}]'}></textarea>
											{#if importResult}
												<div class="mt-1.5 rounded-md px-2 py-1 text-[11px] {importResult.errors.length > 0 ? 'bg-red-500/5 text-red-400' : 'bg-emerald-500/5 text-emerald-400'}">
													{importResult.imported} model{importResult.imported !== 1 ? 's' : ''} imported.
													{#each importResult.errors as error}
														<div class="mt-0.5 text-red-400/80">{error}</div>
													{/each}
												</div>
											{/if}
											<div class="mt-2 flex gap-2">
												<button class="rounded-md border border-border/60 px-3 py-1 text-[12px] text-foreground transition-colors hover:bg-accent disabled:cursor-not-allowed disabled:opacity-40" onclick={() => handleImportModels(selectedProviderId)} disabled={!importJsonText.trim() || isImporting}>
													{isImporting ? 'Importing...' : 'Import'}
												</button>
												<button class="rounded-md px-3 py-1 text-[12px] text-muted-foreground/60 transition-colors hover:text-foreground" onclick={() => { showImportModal = false; importResult = null; }}>Close</button>
											</div>
										</div>
									{/if}
								{/if}
							</div>
						</div>
					{/if}

					<div class="rounded-md bg-foreground/5 px-3 py-2 text-[11px] text-muted-foreground/50">{$t('provider_reconnect_note')}</div>

					<!-- Models enable/disable -->
					<div class="mt-6 mb-4">
						<h2 class="mb-1 text-[18px] font-medium text-foreground">Models</h2>
						<p class="text-[13px] text-muted-foreground/60">Choose which models appear in the model selector. {#if hasCustomModelSelection}<span class="text-[var(--ue-accent)]">{enabledModelCount} enabled</span>{:else}All models shown{/if}</p>
					</div>

					{#if isLoadingModels}
						<div class="flex items-center gap-2 py-4 text-muted-foreground/50">
							<span class="inline-block h-3 w-3 animate-spin rounded-full border-2 border-muted-foreground/30 border-t-muted-foreground"></span>
							Loading models...
						</div>
					{:else if settingsModels.length > 0}
						<!-- Search + actions -->
						<div class="mb-3 flex items-center gap-2">
							<input type="text" bind:value={modelSearchQuery} class="flex-1 rounded-md border border-border/60 bg-transparent px-3 py-2 text-[13px] text-foreground placeholder:text-muted-foreground/40 focus:border-foreground/30 focus:outline-none" placeholder="Search models..." />
						</div>
						<div class="mb-3 flex items-center gap-2">
							<button class="rounded-md border border-border/60 px-2.5 py-1 text-[11px] text-muted-foreground transition-colors hover:bg-accent hover:text-foreground" onclick={handleEnableAllModels}>Enable All</button>
							<button class="rounded-md border border-border/60 px-2.5 py-1 text-[11px] text-muted-foreground transition-colors hover:bg-accent hover:text-foreground" onclick={handleDisableAllModels}>Disable All</button>
							{#if hasCustomModelSelection}
								<button class="rounded-md border border-dashed border-border/60 px-2.5 py-1 text-[11px] text-muted-foreground/60 transition-colors hover:border-border hover:text-muted-foreground" onclick={handleShowAllModels}>Reset (Show All)</button>
							{/if}
							<span class="ml-auto text-[11px] text-muted-foreground/40">{filteredSettingsModels.length} models</span>
						</div>

						<!-- Model list -->
						<div class="rounded-lg border border-border/60 bg-card">
							<div class="max-h-[400px] overflow-y-auto">
								{#each filteredSettingsModels as model}
									<label class="flex cursor-pointer items-center gap-3 border-b border-border/20 px-4 py-2.5 transition-colors last:border-b-0 hover:bg-accent/20">
										<input type="checkbox" checked={isModelEnabled(model.id)} onchange={(e) => handleToggleModel(model.id, (e.currentTarget as HTMLInputElement).checked)} class="h-3.5 w-3.5 shrink-0 rounded border-border accent-[var(--ue-accent)]" />
										<div class="min-w-0 flex-1">
											<div class="flex items-center gap-1.5">
												<span class="truncate text-[13px] text-foreground">{model.name}</span>
												{#if model.providerDisplayName}
													<span class="shrink-0 rounded bg-foreground/5 px-1 py-0.5 text-[9px] text-muted-foreground/40">{model.providerDisplayName}</span>
												{/if}
											</div>
											<div class="truncate text-[11px] text-muted-foreground/40">{model.id}</div>
										</div>
									</label>
								{/each}
							</div>
						</div>
						{#if filteredSettingsModels.length === 0 && modelSearchQuery.trim()}
							<p class="py-4 text-center text-[12px] text-muted-foreground/40">No models match your search.</p>
						{/if}
					{:else}
						<p class="py-4 text-[12px] text-muted-foreground/40">No models available. Configure a provider above first.</p>
					{/if}
				{/if}

				<!-- Prerequisites -->
				<div class="mt-8 border-t border-border/40 pt-6">
					<PrerequisitesPanel />
				</div>

			{:else if $settingsTab === 'crashes'}
				<div class="mb-6">
					<h2 class="mb-1 text-[18px] font-medium text-foreground">Crash History</h2>
					<p class="text-[13px] text-muted-foreground/60">
						Recent crashes detected by Agent Integration Kit. You can send crash reports to help us fix issues.
					</p>
				</div>

				{#if isLoadingCrashes}
					<div class="flex items-center gap-2 py-8 text-muted-foreground/50">
						<span class="inline-block h-4 w-4 animate-spin rounded-full border-2 border-muted-foreground/30 border-t-muted-foreground"></span>
						Loading crash history...
					</div>
				{:else if crashRecords.length === 0}
					<div class="rounded-lg border border-border/60 bg-card p-6 text-center">
						<p class="text-[13px] text-muted-foreground/60">No crashes recorded. That's good!</p>
					</div>
				{:else}
					<div class="space-y-3">
						{#each crashRecords as crash}
							{@const status = crash.fullLogSent || crash.manuallyReported
								? 'sent'
								: crash.fullLogDeclined
									? 'declined'
									: crash.basicReported
										? 'basic'
										: 'none'}
							<div class="rounded-lg border border-border/60 bg-card overflow-hidden">
								<div class="p-4">
									<!-- Header -->
									<div class="flex items-center justify-between mb-2">
										<div class="flex items-center gap-2">
											<span class="inline-block h-2 w-2 rounded-full {status === 'sent' ? 'bg-emerald-400' : status === 'basic' ? 'bg-amber-400' : status === 'declined' ? 'bg-red-400/60' : 'bg-muted-foreground/30'}"></span>
											<span class="text-[12px] font-mono text-muted-foreground/70">{crash.crashType || 'Crash'}</span>
										</div>
										<span class="text-[11px] text-muted-foreground/50">
											{new Date(crash.timestamp).toLocaleDateString(undefined, { month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' })}
										</span>
									</div>

									<!-- Error message -->
									<p class="text-[12px] font-mono text-red-400/80 break-all leading-relaxed mb-3 line-clamp-3">
										{crash.errorMessage}
									</p>

									<!-- Status & Action -->
									<div class="flex items-center justify-between">
										<span class="text-[11px] text-muted-foreground/50">
											{#if status === 'sent'}
												Full report sent
											{:else if status === 'basic'}
												Basic report sent
											{:else if status === 'declined'}
												Full log not sent
											{:else}
												Not reported
											{/if}
										</span>

										{#if status !== 'sent'}
											<button
												class="rounded-md border border-border/60 px-3 py-1.5 text-[12px] text-foreground transition-colors hover:bg-accent disabled:opacity-50 disabled:cursor-not-allowed"
												onclick={() => handleReportCrash(crash.crashId)}
												disabled={reportingCrashId === crash.crashId}
											>
												{#if reportingCrashId === crash.crashId}
													Sending...
												{:else}
													Send Full Report
												{/if}
											</button>
										{/if}
									</div>
								</div>
							</div>
						{/each}
					</div>

					<p class="mt-4 text-[11px] text-muted-foreground/40">
						Can't send from here? Share crash details on our <a href="https://discord.gg/betide" target="_blank" rel="noopener" class="underline hover:text-foreground">Discord</a>.
					</p>
				{/if}
			{:else if $settingsTab === 'notifications'}
				<NotificationsPanel bind:this={notifPanel} />
			{:else}
				<h2 class="mb-1 text-[18px] font-medium text-foreground">
					{tabs.find(t => t.id === $settingsTab)?.label ?? $t('settings')}
				</h2>
				<p class="text-[13px] text-muted-foreground/60">{$t('coming_soon')}</p>
			{/if}
		</div>
	</div>
</div>
