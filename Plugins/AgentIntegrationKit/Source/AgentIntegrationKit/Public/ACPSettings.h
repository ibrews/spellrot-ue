// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "ACPTypes.h"
#include "ACPSettings.generated.h"

DECLARE_MULTICAST_DELEGATE(FOnChatFontSizeChanged);

/** Where the chat WebUI is loaded from */
UENUM()
enum class EWebUISource : uint8
{
	/** Load from hosted CDN (faster updates, requires internet) */
	Hosted		UMETA(DisplayName = "Hosted (Recommended)"),
	/** Load from the local build bundled with the plugin */
	Local		UMETA(DisplayName = "Local"),
};

/**
 * Agent configuration stored in settings
 */
USTRUCT(BlueprintType)
struct FACPAgentSettingsEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Agent")
	FString AgentName;

	UPROPERTY(EditAnywhere, Category = "Agent", meta = (FilePathFilter = "Executable files (*.exe)|*.exe|All files (*.*)|*.*"))
	FFilePath ExecutablePath;

	UPROPERTY(EditAnywhere, Category = "Agent")
	TArray<FString> Arguments;

	UPROPERTY(EditAnywhere, Category = "Agent", meta = (RelativeToGameDir))
	FDirectoryPath WorkingDirectory;

	UPROPERTY(EditAnywhere, Category = "Agent")
	TMap<FString, FString> EnvironmentVariables;

	// For agents that need API keys
	UPROPERTY(EditAnywhere, Category = "Agent", meta = (PasswordField = true))
	FString ApiKey;

	// Model ID for agents that support model selection
	UPROPERTY(EditAnywhere, Category = "Agent")
	FString ModelId;
};

/**
 * Agent profile — a named configuration that controls which tools are exposed
 * and provides specialized instructions/description overrides per domain.
 */
USTRUCT()
struct FAgentProfile
{
	GENERATED_BODY()

	/** Stable internal key (e.g., "builtin_animation" or a GUID for custom profiles) */
	UPROPERTY(config)
	FString ProfileId;

	/** User-facing display name */
	UPROPERTY(config)
	FString DisplayName;

	/** Short description shown in tooltips */
	UPROPERTY(config)
	FString Description;

	/** Built-in profiles can be edited but not deleted */
	UPROPERTY(config)
	bool bIsBuiltIn = false;

	/** Whitelist of enabled tool names. Empty set = all tools enabled. */
	UPROPERTY(config)
	TSet<FString> EnabledTools;

	/** Custom instructions appended to the system prompt when this profile is active */
	UPROPERTY(config)
	FString CustomInstructions;

	/** Per-tool description overrides. Key = tool name, Value = replacement description. */
	UPROPERTY(config)
	TMap<FString, FString> ToolDescriptionOverrides;
};

/**
 * A single model entry within a user-defined custom provider.
 */
USTRUCT()
struct FCustomProviderModelEntry
{
	GENERATED_BODY()

	UPROPERTY(config)
	FString ModelId;

	UPROPERTY(config)
	FString DisplayName;

	UPROPERTY(config)
	FString Description;
};

/**
 * A user-defined custom provider (any OpenAI-compatible API endpoint).
 * Users can create multiple of these, each with its own base URL, API key, and model list.
 */
USTRUCT()
struct FCustomProviderDefinition
{
	GENERATED_BODY()

	/** Unique stable ID, auto-generated as "userprovider_<guid>" on creation */
	UPROPERTY(config)
	FString ProviderId;

	/** User-facing display name (e.g., "My Azure Endpoint") */
	UPROPERTY(config)
	FString DisplayName;

	/** Base URL for OpenAI-compatible API (e.g., "https://my-endpoint.openai.azure.com/v1") */
	UPROPERTY(config)
	FString BaseUrl;

	/** Whether this provider requires an API key (most do, local servers may not) */
	UPROPERTY(config)
	bool bRequiresApiKey = true;

	/** Whether to also attempt /models endpoint discovery in addition to the manual model list */
	UPROPERTY(config)
	bool bEnableModelDiscovery = false;

	/** User-defined model list (acts as the "hardcoded" models for this provider) */
	UPROPERTY(config)
	TArray<FCustomProviderModelEntry> Models;
};

/**
 * Settings for the Agent Integration Kit plugin
 * Accessible via Project Settings > Plugins > Agent Integration Kit
 */
UCLASS(config = AgentIntegrationKit, defaultconfig, meta = (DisplayName = "Agent Integration Kit"))
class AGENTINTEGRATIONKIT_API UACPSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UACPSettings();

	// UDeveloperSettings interface
	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }
	virtual FName GetSectionName() const override { return FName(TEXT("Agent Integration Kit")); }

#if WITH_EDITOR
	virtual FText GetSectionText() const override;
	virtual FText GetSectionDescription() const override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	// Get singleton instance
	static UACPSettings* Get();

	// ============================================
	// General
	// ============================================

	/** Last agent used — automatically saved when creating a session, used as default next time */
	UPROPERTY(config)
	FString LastUsedAgentName;

	/** Automatically connect to the default agent when the editor starts */
	UPROPERTY(config, EditAnywhere, Category = "General", meta = (DisplayName = "Auto-Connect on Startup"))
	bool bAutoConnectOnStartup = false;

	/** Where to load the chat UI from. Hosted loads from CDN and receives UI updates without plugin rebuilds.
	 *  Local loads from the build bundled inside the plugin folder. */
	UPROPERTY(config, EditAnywhere, Category = "General", meta = (DisplayName = "Chat UI Source",
		ToolTip = "Hosted loads the chat UI from a CDN (gets UI fixes/improvements automatically). Local uses the build shipped with the plugin."))
	EWebUISource WebUISource = EWebUISource::Local;

	/** Custom URL for the hosted WebUI. Only used when Chat UI Source is set to Hosted. */
	UPROPERTY(config, EditAnywhere, Category = "General", meta = (DisplayName = "Hosted UI URL",
		EditCondition = "WebUISource == EWebUISource::Hosted && !bUseDevServer", EditConditionHides))
	FString HostedWebUIUrl = TEXT("https://neo-stack-xq8m.vercel.app/");

	/** Use a local Vite dev server instead of the hosted/built UI. Enables hot-reload for UI development. */
	UPROPERTY(config, EditAnywhere, Category = "General", meta = (DisplayName = "Use Live Dev Server",
		ToolTip = "When enabled, loads the chat UI from a local Vite dev server (npm run dev) for live debugging. Overrides the Chat UI Source setting."))
	bool bUseDevServer = false;

	/** Port for the local Vite dev server. */
	UPROPERTY(config, EditAnywhere, Category = "General", meta = (DisplayName = "Dev Server Port",
		EditCondition = "bUseDevServer", EditConditionHides, ClampMin = "1024", ClampMax = "65535"))
	int32 DevServerPort = 5173;

	/** Check for newer versions of Agent Integration Kit when the editor starts */
	UPROPERTY(config, EditAnywhere, Category = "General", meta = (DisplayName = "Check for Updates",
		ToolTip = "Automatically check for newer versions of Agent Integration Kit when the editor starts. Shows a banner in the chat window if an update is available."))
	bool bCheckForUpdates = true;

	// ============================================
	// Auto-Update
	// ============================================

	/** API token from betide.studio for downloading updates directly.
	 *  Generate one at https://betide.studio/dashboard/neostack (API Tokens section).
	 *  This bypasses Fab marketplace review delays so you get updates faster.
	 *  If empty, the plugin falls back to BETIDE_API_TOKEN (or NEOSTACK_API_TOKEN) environment variables. */
	UPROPERTY(config, EditAnywhere, Category = "Auto-Update", meta = (PasswordField = true, DisplayName = "Betide API Token",
		ToolTip = "Your betide.studio API token for direct plugin updates. Generate one in your NeoStack dashboard at betide.studio after verifying your Fab purchase. This bypasses Fab review delays. If left empty, BETIDE_API_TOKEN (or NEOSTACK_API_TOKEN) environment variable is used."))
	FString BetideApiToken;

	/** Route OpenRouter and Meshy calls through betide.studio proxies and charge NeoStack credits. */
	UPROPERTY(config, EditAnywhere, Category = "Auto-Update", meta = (DisplayName = "Use Betide Studio Credits (Recommended)",
		ToolTip = "When enabled, OpenRouter and Meshy requests are sent through betide.studio proxy endpoints and charged against your NeoStack credits using the Betide API token."))
	bool bUseBetideCredits = true;

	/** Automatically download updates when available (still requires manual editor restart to install) */
	UPROPERTY(config, EditAnywhere, Category = "Auto-Update", meta = (DisplayName = "Auto-Download Updates",
		ToolTip = "When enabled and an API token is set, updates are automatically downloaded in the background. You will still be prompted before installation."))
	bool bAutoDownloadUpdates = false;

	/** Opt into beta channel to receive pre-release updates before they go to stable */
	UPROPERTY(config, EditAnywhere, Category = "Auto-Update", meta = (DisplayName = "Beta Channel",
		ToolTip = "When enabled, version checks will include beta/pre-release versions. Useful for testing updates before they go live to all users."))
	bool bUseBetaChannel = false;

	// ============================================
	// Analytics (anonymous, privacy-first)
	// ============================================

	/** Send anonymous usage analytics to help improve the plugin.
	 *  No personal data is collected — only tool/agent usage counts and error rates.
	 *  A random anonymous ID is generated per install (no machine fingerprint). */
	UPROPERTY(config, EditAnywhere, Category = "Analytics", meta = (DisplayName = "Enable Anonymous Analytics",
		ToolTip = "Help improve Agent Integration Kit by sending anonymous usage data (which tools and agents are used, error rates). No personal information, file paths, or code content is ever sent. You can disable this at any time."))
	bool bEnableAnalytics = true;

	// ============================================
	// Crash Reporting
	// ============================================

	/** When enabled and a crash involving AIK is detected on next launch, a basic crash
	 *  report (error message, crash type, callstack summary) is sent automatically.
	 *  This respects the Enable Analytics toggle above — if analytics are off, no crash
	 *  data is sent either. */
	UPROPERTY(config, EditAnywhere, Category = "Crash Reporting", meta = (DisplayName = "Enable Crash Reporting",
		ToolTip = "Automatically send a basic crash report (error type and callstack summary) when a crash involving Agent Integration Kit is detected. No log files or project content are included unless you explicitly choose to send them."))
	bool bEnableCrashReporting = true;

	/** When enabled, the full editor log is sent along with crash reports without prompting.
	 *  If disabled, you will be asked each time a crash is detected whether to include the log. */
	UPROPERTY(config, EditAnywhere, Category = "Crash Reporting", meta = (DisplayName = "Always Send Full Logs",
		ToolTip = "Skip the crash report prompt and always include the full editor log with crash reports. You can change this at any time."))
	bool bAlwaysSendCrashLogs = false;

	// ============================================
	// Chat (managed via Web UI Settings panel)
	// ============================================

	UPROPERTY(config)
	int32 ChatFontSize = 12;

	FOnChatFontSizeChanged OnChatFontSizeChanged;

	UPROPERTY(config)
	bool bIncludeEngineContent = false;

	UPROPERTY(config)
	bool bIncludePluginContent = false;

	UPROPERTY(config)
	FString ContinuationSummaryProvider = TEXT("openrouter");

	UPROPERTY(config)
	FString ContinuationSummaryModel = TEXT("x-ai/grok-4.1-fast");

	UPROPERTY(config)
	FString ContinuationSummaryDefaultDetail = TEXT("compact");

	// ============================================
	// Notifications (managed via Web UI Settings panel)
	// ============================================

	UPROPERTY(config)
	bool bOnlyNotifyWhenUnfocused = false;

	UPROPERTY(config)
	bool bNotifyOnTaskComplete = true;

	UPROPERTY(config)
	bool bFlashTaskbarOnComplete = true;

	UPROPERTY(config)
	bool bPlayCompletionSound = true;

	UPROPERTY(config)
	FSoftObjectPath CompletionSound;

	UPROPERTY(config)
	FSoftObjectPath ErrorSound;

	UPROPERTY(config)
	float CompletionSoundVolume = 1.0f;

	/** When the agent needs allow/deny (or Ask User), play a preview sound so you're not waiting unseen. Uses Project Settings sound asset below. */
	UPROPERTY(config, EditAnywhere, Category = "Notifications", meta = (DisplayName = "Play Sound on Tool Permission Request"))
	bool bPlayPermissionRequestSound = false;

	/** SoundWave or SoundCue under /Game/... (editor preview via PlayPreviewSound). */
	UPROPERTY(config, EditAnywhere, Category = "Notifications", meta = (DisplayName = "Permission Request Sound", AllowedClasses = "/Script/Engine.SoundWave,/Script/Engine.SoundCue"))
	FSoftObjectPath PermissionRequestSound;

	UPROPERTY(config, EditAnywhere, Category = "Notifications", meta = (DisplayName = "Permission Request Sound Volume", ClampMin = "0.0", ClampMax = "1.0"))
	float PermissionRequestSoundVolume = 1.0f;

	// ============================================
	// OpenRouter (managed via Web UI Settings panel — provider management)
	// ============================================

	UPROPERTY(config)
	FString OpenRouterApiKey;

	UPROPERTY(config)
	FString OpenRouterDefaultModel = TEXT("anthropic/claude-sonnet-4");

	UPROPERTY(config)
	FString OpenRouterBaseUrl = TEXT("https://openrouter.ai/api/v1");

	// ============================================
	// ACP Agents (External CLI Agents)
	// ============================================

	/** Custom instructions appended to the system prompt for all ACP agents (Claude Code, Gemini CLI, Codex, etc.)
	 *  This text is injected into the agent's context alongside the tool definitions. */
	UPROPERTY(config, EditAnywhere, Category = "ACP Agents", meta = (DisplayName = "Custom System Prompt", MultiLine = true,
		ToolTip = "Extra instructions appended to the system prompt for ACP-based agents (Claude Code, Gemini CLI, Codex, OpenCode, Cursor Agent). Use this to enforce project-specific workflows or safety rules."))
	FString ACPSystemPromptAppend = TEXT(
		"## HOW THIS WORKS\n"
		"You control the Unreal Editor through Lua scripts. Every asset type in the engine is supported.\n"
		"The core model: open_asset(path) returns an enriched table with domain-specific methods.\n"
		"Call help() to see all domains. Call help('DomainName') for signatures. Call asset:help() for asset-specific methods.\n\n"

		"## DISCOVERY — USE IT, DON'T GUESS\n"
		"- help() — lists all global functions and asset enrichments\n"
		"- help('CreateBlueprint') — shows create_asset signatures, options, and supported types\n"
		"- list_asset_types() — all 50+ creatable types with their aliases\n"
		"- find_nodes('query', asset_path?, graph?) — search for node types before adding. Returns owning_class to disambiguate.\n"
		"- local a = open_asset(path); a:help() — see all methods available for that asset type\n"
		"- a:info() — structured read of asset contents\n"
		"ALWAYS discover before acting. NEVER guess node names, property names, or API calls.\n\n"

		"## CRITICAL — ASSET CREATION\n"
		"create_asset(path, type, opts) — type is a string alias (e.g., 'Blueprint', 'AnimMontage', 'DataTable', 'Material').\n"
		"- Blueprint: ALWAYS pass {ParentClass='Character'} or whatever the correct parent is. Default is Actor.\n"
		"  Common parents: Character, Pawn, PlayerController, GameModeBase, ActorComponent, SceneComponent, AnimInstance.\n"
		"- DataTable: MUST pass {Struct='/Script/Module.FRowStruct'} — there is no default row struct.\n"
		"- AnimBlueprint: Pass {Skeleton='/Game/Path/Skeleton'} to bind to a skeleton.\n"
		"- After creation, open_asset() the result to verify the type/parent before adding content.\n\n"

		"## CRITICAL — NODE SELECTION (find_nodes)\n"
		"find_nodes() returns owning_class for each result. When multiple results share a name, CHECK owning_class.\n"
		"Pick the one matching your asset's parent class. Example:\n"
		"  'Get Control Rotation' → class=Pawn (use in Character BPs) vs class=Controller (use in PlayerController BPs)\n"
		"  'Get Actor Location' → class=Actor (works everywhere) vs class=Kismet (static function version)\n"
		"If zero results: broaden query, try partial names, or use help('FindNodes') for search tips.\n\n"

		"## UNDERSTAND BEFORE YOU TOUCH\n"
		"Before ANY modification to an existing asset:\n"
		"1. open_asset(path) then read its structure (info(), list graphs, list components).\n"
		"2. For graph-based assets (Blueprints, Materials, BTs, Niagara): read EACH relevant graph.\n"
		"3. TRACE THE EXECUTION FLOW. Follow the logic from trigger to outcome.\n"
		"   If the user says 'make bots respawn continuously' and you see a respawn function, don't just tweak it.\n"
		"   Read the function, find what calls it, find what limits it (a counter? a boolean? a max-respawn variable?),\n"
		"   and fix the ACTUAL constraint — not a surface-level symptom.\n"
		"4. EXPLAIN what you found before making changes. This catches misunderstandings BEFORE you edit.\n\n"
		"DO NOT read an asset, see it 'has X logic', and add MORE X logic.\n"
		"The fix is usually changing a value, removing a condition, or rewiring existing nodes — NOT adding new parallel logic.\n\n"

		"## WORKFLOW\n"
		"1. READ — open_asset + info/read_graph before modifying anything. Skipping this is the #1 cause of bad edits.\n"
		"2. DISCOVER — find_nodes / help() before adding nodes. Never claim 'node doesn't exist' without searching.\n"
		"3. REUSE, DON'T DUPLICATE:\n"
		"   - If CalculateDamage exists, call it — don't create CalculateDamage2.\n"
		"   - Wire into existing event handlers instead of creating parallel ones.\n"
		"   - Check if existing logic already handles part of what the user wants.\n"
		"4. LAYOUT — Execution (white wire) flows left-to-right; data nodes go LEFT of and slightly ABOVE/BELOW consumers;\n"
		"   stack branches vertically (~300px apart); keep wire crossings minimal.\n"
		"5. COMPILE — After modifying Blueprints: read_log('compile', {Asset='/Game/Path'}). Required — graph edits only mark dirty.\n"
		"6. SAVE — execute_python: unreal.EditorAssetLibrary.save_asset('/Game/Path')\n"
		"7. VERIFY — Re-read the modified asset/graph to confirm correctness.\n\n"

		"## DOMAIN-SPECIFIC PATTERNS\n"
		"These are the major asset domains. Each has its own verbs discovered via help().\n"
		"- Blueprints: add/remove components, variables, functions, events, widgets. Graph ops for node wiring.\n"
		"- Animation: AnimSequence (curves, notifies, sync markers), AnimMontage (slots, sections, segments),\n"
		"  BlendSpace (samples, parameters), PoseAsset, PoseSearch (databases, schemas).\n"
		"- Rigging: IKRig (goals, solvers, chains), IKRetargeter (chain mapping, op settings), ControlRig (hierarchy + graph).\n"
		"- Materials: Material graphs (expressions, connections), MaterialInstance (parameter overrides),\n"
		"  MaterialFunction, MaterialParamCollection.\n"
		"- VFX: Niagara (emitters, modules, parameters, value modes — static/dynamic/linked/HLSL).\n"
		"- Sequencer: Level Sequences (bindings, tracks, keyframes, camera cuts, bulk transforms).\n"
		"  Use list_track_types() and list_bindings() to discover what's available.\n"
		"- Audio: SoundCue (node graphs), SoundWave, SoundClass/Mix/Attenuation/Concurrency, Dialogue.\n"
		"- AI: BehaviorTree (tasks, decorators, services), StateTree, GameplayAbility/Effect, EQS.\n"
		"- World: LevelDesign, FoliageType, Water, HLOD, ChaosFracture, ActorMerging.\n"
		"- Data: DataTable, CurveTable, UserDefinedStruct/Enum, ChooserTable, Localization.\n"
		"- Input: EnhancedInput (InputAction, InputMappingContext), SmartObject.\n"
		"Do NOT memorize this list. Use help() at runtime to discover what's available for any asset.\n\n"

		"## WHEN YOUR EDIT DIDN'T WORK\n"
		"- Don't retry the same approach. Re-read the asset and trace the logic again.\n"
		"- The bug is almost always in existing logic you didn't fully understand, not missing logic you need to add.\n"
		"- Look for: variable defaults, branch conditions, counters/limits, function call chains, event bindings.\n"
		"- Ask the user for clarification if the intent is ambiguous.\n\n"

		"## SAFETY\n"
		"- Never delete assets, nodes, or components without explicit user confirmation.\n"
		"- Warn before destructive operations (clearing graphs, removing functions).\n"
		"- Don't overwrite existing assets without asking.\n"
		"- SAVE BEFORE BIG CHANGES: Before large-scale modifications, remind the user to save. Not all users use version control.\n\n"

		"## IMPORTANT RULES\n"
		"- NEVER ASSUME PROJECT STRUCTURE: Every project is different. Use explore/find_assets to discover actual paths.\n"
		"- MATCH THE USER'S LANGUAGE: Respond in the same language the user writes in.\n"
		"- Reference assets with UE paths like /Game/Blueprints/BP_Character — they are clickable in the UI.\n"
		"- execute_python is available for anything Lua can't do (level actors, editor utilities, batch operations).\n"
		"- screenshot captures viewports and asset editors — use it to verify visual results.\n"
		"- generate_asset creates AI-generated textures (image) and 3D models (mesh) via external APIs.\n\n"

		"## ABOUT THIS PLUGIN\n"
		"You are operating through Agent Integration Kit by Betide Studio — an Unreal Engine editor plugin that lets\n"
		"AI agents control the editor via Lua tools. It covers the full engine: every asset type, every graph type,\n"
		"every editor workflow. If the user asks what plugin or tool this is, tell them.\n\n"

		"## TOOL LIMITATIONS & REPORTING ISSUES\n"
		"If a tool crashes, returns unexpected errors, or cannot accomplish the task, be honest.\n"
		"Do not ask the user to perform manual editor steps without first stating this may be a tool limitation.\n"
		"Tell the user: 'This appears to be a limitation of the current toolset. Please report this on the Betide Studio\n"
		"Discord server so the developers can address it: https://discord.gg/Fcj68FJzAj'"
	);

	/** Registry agent IDs that the user has "installed" (managed via Web UI agent registry) */
	UPROPERTY(config)
	TArray<FString> InstalledAgentIds;

	/** Custom ACP agent definitions. Each entry spawns an external process that communicates via ACP (JSON-RPC over stdio). */
	UPROPERTY(config, EditAnywhere, Category = "ACP Agents", meta = (DisplayName = "Custom Agents",
		ToolTip = "Define custom ACP-compatible agents. Each agent is an external process that communicates over stdin/stdout using the Agent Client Protocol."))
	TArray<FACPAgentSettingsEntry> CustomAgents;

	// ============================================
	// ACP Agents | Agent Process Overrides (Advanced)
	// ============================================
	// These advanced overrides replace the process spawned for an agent session.
	// Leave empty for normal setup behavior (bundled adapters + automatic executable resolution).

	/** Advanced: override the spawned process for Claude Code agent sessions.
	 *  Leave empty to use the default bundled claude-code-acp adapter flow. */
	UPROPERTY(config, EditAnywhere, Category = "ACP Agents | Agent Process Overrides (Advanced)", meta = (DisplayName = "Claude Agent Process Override",
		ToolTip = "Advanced override for the process spawned for Claude sessions. Leave empty to use the default bundled claude-code-acp adapter flow.",
		AdvancedDisplay,
		FilePathFilter = "Executable files (*.exe)|*.exe|All files (*.*)|*.*"))
	FFilePath ClaudeCodePath;

	/** Advanced: override the spawned process for Gemini sessions. */
	UPROPERTY(config, EditAnywhere, Category = "ACP Agents | Agent Process Overrides (Advanced)", meta = (DisplayName = "Gemini Agent Process Override",
		ToolTip = "Advanced override for the process spawned for Gemini sessions. Leave empty for default auto-detection.",
		AdvancedDisplay,
		FilePathFilter = "Executable files (*.exe)|*.exe|All files (*.*)|*.*"))
	FFilePath GeminiCliPath;

	/** Advanced: override the spawned process for Codex sessions. */
	UPROPERTY(config, EditAnywhere, Category = "ACP Agents | Agent Process Overrides (Advanced)", meta = (DisplayName = "Codex Agent Process Override",
		ToolTip = "Advanced override for the process spawned for Codex sessions. Leave empty for default auto-detection.",
		AdvancedDisplay,
		FilePathFilter = "Executable files (*.exe)|*.exe|All files (*.*)|*.*"))
	FFilePath CodexCliPath;

	/** Advanced: override the spawned process for OpenCode sessions. */
	UPROPERTY(config, EditAnywhere, Category = "ACP Agents | Agent Process Overrides (Advanced)", meta = (DisplayName = "OpenCode Agent Process Override",
		ToolTip = "Advanced override for the process spawned for OpenCode sessions. Leave empty for default auto-detection.",
		AdvancedDisplay,
		FilePathFilter = "Executable files (*.exe)|*.exe|All files (*.*)|*.*"))
	FFilePath OpenCodePath;

	/** Advanced: override the spawned process for Cursor sessions. */
	UPROPERTY(config, EditAnywhere, Category = "ACP Agents | Agent Process Overrides (Advanced)", meta = (DisplayName = "Cursor Agent Process Override",
		ToolTip = "Advanced override for the process spawned for Cursor sessions. Leave empty for default auto-detection.",
		AdvancedDisplay,
		FilePathFilter = "Executable files (*.exe)|*.exe|All files (*.*)|*.*"))
	FFilePath CursorAgentPath;

	/** Advanced: override the spawned process for Kimi sessions. */
	UPROPERTY(config, EditAnywhere, Category = "ACP Agents | Agent Process Overrides (Advanced)", meta = (DisplayName = "Kimi Agent Process Override",
		ToolTip = "Advanced override for the process spawned for Kimi sessions. Leave empty for default auto-detection.",
		AdvancedDisplay,
		FilePathFilter = "Executable files (*.exe)|*.exe|All files (*.*)|*.*"))
	FFilePath KimiCliPath;

	/** Advanced: override the spawned process for Copilot sessions. */
	UPROPERTY(config, EditAnywhere, Category = "ACP Agents | Agent Process Overrides (Advanced)", meta = (DisplayName = "Copilot Agent Process Override",
		ToolTip = "Advanced override for the process spawned for Copilot sessions. Leave empty for default auto-detection.",
		AdvancedDisplay,
		FilePathFilter = "Executable files (*.exe)|*.exe|All files (*.*)|*.*"))
	FFilePath CopilotCliPath;

	/** Override path to the Bun runtime used to run ACP adapters. The plugin bundles Bun, so this is only needed
	 *  if you want to use your own Bun/Node installation instead of the bundled one. */
	UPROPERTY(config, EditAnywhere, Category = "ACP Agents | Agent Process Overrides (Advanced)", meta = (DisplayName = "Bun Runtime Override",
		ToolTip = "Path to a custom Bun (or Node.js) runtime. The plugin bundles Bun for running ACP adapters. Only set this if you want to use your own runtime instead of the bundled one. Leave empty to use the bundled Bun.",
		AdvancedDisplay,
		FilePathFilter = "Executable files (*.exe)|*.exe|All files (*.*)|*.*"))
	FFilePath BunOverridePath;

	/** Generic agent executable path overrides, keyed by ACP registry ID (e.g., "claude-acp", "gemini").
	 *  Used by the registry-based config generator. Takes precedence over auto-detection. */
	UPROPERTY(config, EditAnywhere, Category = "ACP Agents | Agent Process Overrides (Advanced)", meta = (DisplayName = "Agent Path Overrides (Registry ID → Path)",
		ToolTip = "Custom executable paths for specific agents. Key = registry agent ID (e.g., 'claude-acp', 'gemini'), Value = path to executable.",
		AdvancedDisplay))
	TMap<FString, FString> AgentPathOverrides;

	// ============================================
	// ACP Agents | Claude Setup
	// ============================================

	/** Optional path to the Claude CLI executable used by the bundled claude-code-acp adapter.
	 *  This avoids relying on shell PATH updates in a running editor process. */
	UPROPERTY(config, EditAnywhere, Category = "ACP Agents | Claude Setup", meta = (DisplayName = "Claude CLI Executable (for bundled adapter)",
		ToolTip = "Optional explicit path to the Claude CLI executable used by the bundled claude-code-acp adapter via CLAUDE_CODE_EXECUTABLE. Leave empty to auto-detect."))
	FFilePath ClaudeCodeExecutablePath;

	/** Try running Claude's official installer in-process before falling back to external terminal setup. */
	UPROPERTY(config, EditAnywhere, Category = "ACP Agents | Claude Setup", meta = (DisplayName = "Install Claude In-Process First",
		ToolTip = "When enabled, setup first runs Claude's installer in the background installer thread. If that fails, it falls back to launching an external terminal installer."))
	bool bInstallClaudeInProcessFirst = true;

	/** Automatically persist detected Claude executable path after successful install to avoid PATH/session issues. */
	UPROPERTY(config, EditAnywhere, Category = "ACP Agents | Claude Setup", meta = (DisplayName = "Auto-Save Detected Claude Executable Path",
		ToolTip = "When enabled, after successful Claude installation the detected executable path is saved into 'Claude CLI Executable (for bundled adapter)' so setup works immediately without editor restart."))
	bool bAutoSaveClaudeCodeExecutablePathAfterInstall = true;

	// ============================================
	// AI Generation
	// ============================================

	/** Default model for AI image generation via OpenRouter (used by generate_asset with asset_type=image) */
	UPROPERTY(config, EditAnywhere, Category = "AI Generation | Images", meta = (DisplayName = "Default Image Model",
		ToolTip = "The OpenRouter model ID used for AI image generation. Must be a vision/image-capable model. Images are saved as Texture2D assets in the project."))
	FString ImageGenerationDefaultModel = TEXT("black-forest-labs/flux.2-flex");

	/** Meshy API key for AI 3D model generation (used by generate_asset with asset_type=model_3d). Get one at meshy.ai */
	UPROPERTY(config, EditAnywhere, Category = "AI Generation | 3D Models (Meshy)", meta = (PasswordField = true, DisplayName = "Meshy API Key",
		ToolTip = "Your Meshy API key for text-to-3D model generation. Generated models are imported as StaticMesh assets. Get a key at https://meshy.ai"))
	FString MeshyApiKey;

	/** Tripo API key for AI 3D model generation. Get one at tripo3d.ai */
	UPROPERTY(config, EditAnywhere, Category = "AI Generation | 3D Models (Tripo)", meta = (PasswordField = true, DisplayName = "Tripo API Key",
		ToolTip = "Your Tripo API key for text/image-to-3D generation, rigging, animation, and stylization. Get a key at https://tripo3d.ai"))
	FString TripoApiKey;

	/** ElevenLabs API key for AI audio generation (TTS, sound effects, music, STT). Get one at elevenlabs.io */
	UPROPERTY(config, EditAnywhere, Category = "AI Generation | Audio (ElevenLabs)", meta = (PasswordField = true, DisplayName = "ElevenLabs API Key",
		ToolTip = "Your ElevenLabs API key for text-to-speech, sound effects, music generation, and speech-to-text. Get a key at https://elevenlabs.io"))
	FString ElevenLabsApiKey;

	/** fal.ai API key for AI 3D model generation (used by generate_3d_model with provider='fal'). */
	UPROPERTY(config, EditAnywhere, Category = "AI Generation | 3D Models (fal.ai)", meta = (PasswordField = true, DisplayName = "fal.ai API Key",
		ToolTip = "Your fal.ai API key for direct BYOK 3D generation (for example Hunyuan3D models). In NeoStack Credits mode, Betide token/proxy is used instead."))
	FString FalApiKey;

	/** Default art style for Meshy text-to-3D generation (e.g. realistic, cartoon, low-poly) */
	UPROPERTY(config, EditAnywhere, Category = "AI Generation | 3D Models (Meshy)", meta = (DisplayName = "Default Art Style",
		ToolTip = "Default art style preset for Meshy generation. Options include: realistic, cartoon, low-poly, sculpture, pbr. The AI agent can override this per-request."))
	FString MeshyDefaultArtStyle = TEXT("realistic");

	/** Maximum time to wait for a Meshy 3D generation job to complete (seconds) */
	UPROPERTY(config, EditAnywhere, Category = "AI Generation | 3D Models (Meshy)", meta = (DisplayName = "Generation Timeout", ClampMin = 60, ClampMax = 600,
		ToolTip = "How long to wait for Meshy to finish generating a 3D model before timing out. Complex models may take several minutes."))
	int32 MeshyTimeoutSeconds = 300;

	// ============================================
	// MCP Server
	// ============================================

	/** Allow browser-based access to the MCP server. When disabled (default), requests with an Origin header
	 *  are rejected. Browsers always send Origin on cross-origin requests; CLI tools do not.
	 *  This prevents malicious websites from calling your MCP server while you browse the web. */
	UPROPERTY(config, EditAnywhere, Category = "MCP Server", meta = (DisplayName = "Allow Browser Requests",
		ToolTip = "When disabled, HTTP requests with an Origin header are rejected. This blocks browser-based cross-origin requests (CSRF protection). CLI agents (Claude Code, Gemini, Codex) never send Origin headers and are unaffected. Only enable if you use a browser-based MCP client."))
	bool bAllowBrowserMCPRequests = false;

	/** Preferred MCP server port. If occupied, the server automatically tries subsequent ports. */
	UPROPERTY(config, EditAnywhere, Category = "MCP Server", meta = (DisplayName = "Server Port", ClampMin = 1, ClampMax = 65535,
		ToolTip = "Preferred local TCP port for the built-in MCP server. If this port is already in use, Agent Integration Kit will scan a few higher ports automatically."))
	int32 MCPServerPort = 9315;

	// ============================================
	// Remote Access
	// ============================================

	/** Enable remote access to this instance via the relay server.
	 *  When enabled, the editor connects to the relay and becomes accessible from neostack.dev. */
	UPROPERTY(config, EditAnywhere, Category = "Remote Access", meta = (DisplayName = "Enable Remote Access",
		ToolTip = "Connect this instance to the relay server so you can control it remotely from neostack.dev. Requires a NeoStack API key (neostack_...)."))
	bool bEnableRemoteAccess = false;

	/** NeoStack API key for remote access authentication. Generate one at neostack.dev/dashboard.
	 *  This is separate from the Betide API token used for plugin updates/credits. */
	UPROPERTY(config, EditAnywhere, Category = "Remote Access", meta = (PasswordField = true, DisplayName = "NeoStack API Key",
		ToolTip = "Your NeoStack API key (neostack_...) for authenticating with the relay server. Generate one at neostack.dev/dashboard. Separate from the Betide API token."))
	FString NeoStackApiKey;

	/** A friendly name for this instance (shown on the website when picking which instance to control). */
	UPROPERTY(config, EditAnywhere, Category = "Remote Access", meta = (DisplayName = "Instance Name",
		ToolTip = "Human-readable name shown on neostack.dev when you have multiple instances connected. Defaults to 'ComputerName - ProjectName' if left empty."))
	FString InstanceName;

	/** The relay server URL. Only change this if you're running a custom relay. */
	UPROPERTY(config, EditAnywhere, Category = "Remote Access", AdvancedDisplay, meta = (DisplayName = "Relay Server URL",
		ToolTip = "WebSocket URL of the AIK relay server. Default connects to the hosted relay."))
	FString RelayServerUrl = TEXT("wss://api.neostack.cloud/ws/instance");

	// ============================================
	// IDE Connection
	// ============================================

	/** Automatically connect to the NeoStack IDE when it's running locally.
	 *  The plugin discovers the IDE via ~/.neostack/server.json. */
	UPROPERTY(config, EditAnywhere, Category = "IDE Connection", meta = (DisplayName = "Enable IDE Connection",
		ToolTip = "Automatically connect to the NeoStack IDE desktop app when it's running on the same machine. No API key required — works offline."))
	bool bEnableIDEConnection = true;

	/** Override the default discovery file path (~/.neostack/server.json). */
	UPROPERTY(config, EditAnywhere, Category = "IDE Connection", AdvancedDisplay, meta = (DisplayName = "Discovery File Path Override",
		ToolTip = "Override the default path to the IDE discovery file. Leave empty to use ~/.neostack/server.json."))
	FString IDEDiscoveryPathOverride;

	// ============================================
	// Tools
	// ============================================

	/** Tool execution timeout in seconds (0 = no timeout). If a tool takes longer than this, a timeout error is sent to the AI while the tool continues running in the background. */
	UPROPERTY(config, EditAnywhere, Category = "Tools", meta = (DisplayName = "Execution Timeout (seconds)", ClampMin = 0, ClampMax = 600,
		ToolTip = "Maximum seconds a tool can run before the agent receives a timeout error. The tool itself keeps running in the background. Set to 0 to disable the timeout."))
	int32 ToolExecutionTimeoutSeconds = 60;

	/** Names of tools that have been disabled by the user (managed via the Settings panel in the chat window) */
	UPROPERTY(config)
	TSet<FString> DisabledTools;

	// ============================================
	// Profiles
	// ============================================

	/** All agent profiles (built-in presets + user-created) */
	UPROPERTY(config)
	TArray<FAgentProfile> Profiles;

	/** ID of the currently active profile. Empty = no profile (all tools enabled). */
	UPROPERTY(config)
	FString ActiveProfileId;

	// ============================================
	// Debug
	// ============================================

	/** Enable verbose logging for ACP/MCP communication (logged to Output Log under LogAgentIntegrationKit) */
	UPROPERTY(config, EditAnywhere, Category = "Debug", meta = (DisplayName = "Verbose Logging",
		ToolTip = "Logs all ACP/MCP JSON messages to the Output Log. Useful for debugging agent communication issues. Can produce a lot of output."))
	bool bVerboseLogging = false;

	// ============================================
	// Internal (not shown in UI)
	// ============================================

	/** Ordered list of provider IDs for the built-in agent. Index 0 = highest priority.
	 *  When a model is servable by multiple providers, the first configured provider in this list wins. */
	UPROPERTY(config)
	TArray<FString> ProviderPriority;

	/** Per-provider API keys. Key = provider ID (e.g. "openai"), Value = API key. OpenRouter uses OpenRouterApiKey for compat. */
	UPROPERTY(config)
	TMap<FString, FString> ProviderApiKeys;

	/** Per-provider base URL overrides. Empty = provider default. */
	UPROPERTY(config)
	TMap<FString, FString> ProviderBaseUrls;

	/** User-defined custom providers. Each becomes a selectable provider with its own models. */
	UPROPERTY(config)
	TArray<FCustomProviderDefinition> CustomProviders;

	/** Extra user-added models for built-in providers (e.g. Ollama models not in hardcoded list).
	 *  Key = provider ID ("ollama"), Value = array of model entries. Merged with discovered models. */
	TMap<FString, TArray<FCustomProviderModelEntry>> ExtraProviderModels;

	/** Set of enabled model IDs for the built-in agent's dropdown.
	 *  Empty set = all models shown (backward compat). */
	UPROPERTY(config)
	TSet<FString> EnabledModels;

	/** Per-agent saved model selections (persisted across editor sessions) */
	UPROPERTY(config)
	TMap<FString, FString> SelectedModelPerAgent;

	/** Per-agent saved mode selections (persisted across editor sessions) */
	UPROPERTY(config)
	TMap<FString, FString> SelectedModePerAgent;

	/** Per-agent saved reasoning effort selections (persisted across editor sessions) */
	UPROPERTY(config)
	TMap<FString, FString> SelectedReasoningPerAgent;

	/** Per-agent system prompt delivery method. Key = agent name (e.g. "Open Code", "Codex CLI").
	 *  Agents not listed here default to SessionMeta. */
	UPROPERTY(config, EditAnywhere, Category = "ACP Agents", meta = (DisplayName = "System Prompt Delivery Per Agent",
		ToolTip = "How to deliver the custom system prompt to each agent. SessionMeta uses _meta.systemPrompt (Claude Code). FirstUserMessage/EveryUserMessage prepend it to user messages (for agents like Open Code that ignore _meta)."))
	TMap<FString, ESystemPromptDelivery> SystemPromptDeliveryPerAgent;

	/** Whether the first-launch onboarding wizard has been completed or skipped */
	UPROPERTY(config)
	bool bOnboardingCompleted = false;

	// Convert settings to agent configs
	TArray<FACPAgentConfig> GetAgentConfigs() const;

	// Model selection persistence helpers
	FString GetSavedModelForAgent(const FString& AgentName) const;
	void SaveModelForAgent(const FString& AgentName, const FString& ModelId);

	// Mode selection persistence helpers
	FString GetSavedModeForAgent(const FString& AgentName) const;
	void SaveModeForAgent(const FString& AgentName, const FString& ModeId);

	// Reasoning selection persistence helpers
	FString GetSavedReasoningForAgent(const FString& AgentName) const;
	void SaveReasoningForAgent(const FString& AgentName, const FString& ReasoningLevel);

	// Tool enable/disable helpers
	bool IsToolEnabled(const FString& ToolName) const;
	void SetToolEnabled(const FString& ToolName, bool bEnabled);

	// Credits/proxy routing helpers
	bool ShouldUseBetideCredits() const;
	FString GetBetideApiToken() const;
	bool HasOpenRouterAuth() const;
	bool HasMeshyAuth() const;
	bool HasFalAuth() const;
	FString GetOpenRouterAuthToken() const;
	FString GetMeshyAuthToken() const;
	FString GetFalAuthToken() const;
	FString GetOpenRouterChatCompletionsUrl() const;
	FString GetOpenRouterImageGenerationUrl() const;
	FString GetOpenRouterModelsUrl() const;
	FString GetMeshyBaseUrl() const;
	FString GetFalSubmitUrl() const;
	FString GetFalStatusProxyUrl() const;
	FString GetFalResultProxyUrl() const;
	FString GetFalCancelProxyUrl() const;

	// Profile management
	const FAgentProfile* GetActiveProfile() const;
	const FAgentProfile* FindProfileById(const FString& ProfileId) const;
	FAgentProfile* FindProfileByIdMutable(const FString& ProfileId);
	void SetActiveProfile(const FString& ProfileId);
	void AddCustomProfile(const FAgentProfile& Profile);
	void RemoveCustomProfile(const FString& ProfileId);
	void EnsureBuiltInProfiles();

	/** Returns the tool description, applying the active profile's override if one exists */
	FString GetEffectiveToolDescription(const FString& ToolName, const FString& DefaultDescription) const;

	/** Returns ACPSystemPromptAppend + active profile's CustomInstructions */
	FString GetProfileSystemPromptAppend() const;

	/** Returns the system prompt delivery method for a given agent */
	ESystemPromptDelivery GetSystemPromptDeliveryForAgent(const FString& AgentName) const;

	/** Ensures built-in defaults exist in SystemPromptDeliveryPerAgent so they're visible in the UI */
	void EnsureBuiltInSystemPromptDeliveryDefaults();

	// Provider priority helpers (for built-in agent)
	const TArray<FString>& GetProviderPriority() const;
	void SetProviderPriority(const TArray<FString>& Priority);
	void AddProviderToPriority(const FString& ProviderId);
	void RemoveProviderFromPriority(const FString& ProviderId);
	bool IsProviderConfigured(const FString& ProviderId) const;
	FString GetProviderApiKey(const FString& ProviderId) const;
	FString GetProviderBaseUrl(const FString& ProviderId) const;
	void SetProviderApiKey(const FString& ProviderId, const FString& Key);
	void SetProviderBaseUrl(const FString& ProviderId, const FString& Url);

	// Custom provider helpers
	FCustomProviderDefinition* FindCustomProvider(const FString& ProviderId);
	const FCustomProviderDefinition* FindCustomProvider(const FString& ProviderId) const;
	FString CreateCustomProvider(const FString& DisplayName, const FString& BaseUrl);
	void DeleteCustomProvider(const FString& ProviderId);
	void UpdateCustomProvider(const FString& ProviderId, const FString& DisplayName, const FString& BaseUrl);
	void AddCustomProviderModel(const FString& ProviderId, const FString& ModelId, const FString& DisplayName, const FString& Description);
	void RemoveCustomProviderModel(const FString& ProviderId, const FString& ModelId);

	// Extra models for built-in providers (e.g. Ollama)
	void AddExtraProviderModel(const FString& ProviderId, const FString& ModelId, const FString& DisplayName, const FString& Description);
	void RemoveExtraProviderModel(const FString& ProviderId, const FString& ModelId);
	const TArray<FCustomProviderModelEntry>* GetExtraProviderModels(const FString& ProviderId) const;
	int32 ImportCustomProviderModels(const FString& ProviderId, const TArray<FCustomProviderModelEntry>& Models);
	void SetCustomProviderModelDiscovery(const FString& ProviderId, bool bEnabled);

	// Model enable/disable helpers (for built-in agent dropdown)
	bool IsModelEnabled(const FString& ModelId) const;
	void SetModelEnabled(const FString& ModelId, bool bEnabled);
	const TSet<FString>& GetEnabledModels() const { return EnabledModels; }

	// Unified preferences persistence — writes to ~/.agentintegrationkit/preferences.json
	// Bypasses UE's broken SaveConfig() which writes to the wrong INI file.
	void SavePreferences();
	void LoadPreferences();

	// Legacy wrappers (delegate to SavePreferences/LoadPreferences)
	void SaveInstalledAgentIds();
	void LoadInstalledAgentIds();

	// Agent status cache management
	void RefreshAgentStatus();
	void InvalidateAgentStatusCache();
	bool IsAgentStatusStale() const;

private:
	mutable TMap<FString, EACPAgentStatus> CachedAgentStatus;
	mutable FDateTime LastStatusRefresh;
	mutable FCriticalSection StatusCacheLock;
	static constexpr double StatusCacheTTLSeconds = 300.0;
};
