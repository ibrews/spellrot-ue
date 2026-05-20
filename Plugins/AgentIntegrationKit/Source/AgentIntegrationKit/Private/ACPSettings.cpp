// Copyright 2026 Betide Studio. All Rights Reserved.

#include "ACPSettings.h"
#include "AgentInstaller.h"
#include "AgentIntegrationKitModule.h"
#include "ChatGatewayProvider.h"
#include "MCPServer.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

#define LOCTEXT_NAMESPACE "ACPSettings"

static bool IsExecutableAvailable(const FString& ExecutablePath, FString& OutResolvedPath)
{
	return FAgentInstaller::Get().ResolveExecutable(ExecutablePath, OutResolvedPath);
}

static FString NormalizeOpenRouterBaseUrl(const FString& InBaseUrl)
{
	FString BaseUrl = InBaseUrl;
	BaseUrl.TrimStartAndEndInline();
	if (BaseUrl.IsEmpty())
	{
		BaseUrl = TEXT("https://openrouter.ai/api/v1");
	}

	while (BaseUrl.EndsWith(TEXT("/")))
	{
		BaseUrl.LeftChopInline(1, EAllowShrinking::No);
	}

	if (BaseUrl.EndsWith(TEXT("/chat/completions")))
	{
		BaseUrl.LeftChopInline(FCString::Strlen(TEXT("/chat/completions")), EAllowShrinking::No);
	}
	else if (BaseUrl.EndsWith(TEXT("/models")))
	{
		BaseUrl.LeftChopInline(FCString::Strlen(TEXT("/models")), EAllowShrinking::No);
	}

	while (BaseUrl.EndsWith(TEXT("/")))
	{
		BaseUrl.LeftChopInline(1, EAllowShrinking::No);
	}

	return BaseUrl;
}

static FString BuildOpenRouterUrl(const FString& BaseUrl, const TCHAR* EndpointPath)
{
	return NormalizeOpenRouterBaseUrl(BaseUrl) + EndpointPath;
}

static bool ResolveClaudeCodeExecutableForAdapter(const FFilePath& PreferredPath, FString& OutResolvedPath)
{
	if (!PreferredPath.FilePath.IsEmpty() && IsExecutableAvailable(PreferredPath.FilePath, OutResolvedPath))
	{
		return true;
	}

	if (FAgentInstaller::Get().ResolveExecutable(TEXT("claude"), OutResolvedPath))
	{
		return true;
	}

	TArray<FString> CandidatePaths;
#if PLATFORM_WINDOWS
	const FString UserProfile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
	if (!UserProfile.IsEmpty())
	{
		CandidatePaths.Add(FPaths::Combine(UserProfile, TEXT(".local/bin/claude.exe")));
		CandidatePaths.Add(FPaths::Combine(UserProfile, TEXT(".local/bin/claude.cmd")));
		CandidatePaths.Add(FPaths::Combine(UserProfile, TEXT(".local/bin/claude")));
	}
#else
	const FString HomeDir = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
	if (!HomeDir.IsEmpty())
	{
		CandidatePaths.Add(FPaths::Combine(HomeDir, TEXT(".local/bin/claude")));
	}
#endif

	for (FString Candidate : CandidatePaths)
	{
		FPaths::NormalizeFilename(Candidate);
		if (IFileManager::Get().FileExists(*Candidate))
		{
			OutResolvedPath = Candidate;
			return true;
		}
	}

	return false;
}

UACPSettings::UACPSettings()
{
	EnsureBuiltInProfiles();
	EnsureBuiltInSystemPromptDeliveryDefaults();
}

#if WITH_EDITOR
FText UACPSettings::GetSectionText() const
{
	return LOCTEXT("SectionText", "Agent Integration Kit");
}

FText UACPSettings::GetSectionDescription() const
{
	return LOCTEXT("SectionDescription", "Configure AI agent connections and API keys for the Agent Integration Kit.");
}

void UACPSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	SaveConfig();

	// Broadcast font size change
	if (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UACPSettings, ChatFontSize))
	{
		OnChatFontSizeChanged.Broadcast();
	}

	if (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UACPSettings, MCPServerPort))
	{
		if (FMCPServer::Get().IsRunning())
		{
			const int32 RequestedPort = FMath::Clamp(MCPServerPort, 1, 65535);
			FMCPServer::Get().Stop();
			FMCPServer::Get().Start(RequestedPort);
		}
	}
}
#endif

UACPSettings* UACPSettings::Get()
{
	if (!UObjectInitialized() || IsEngineExitRequested())
	{
		return nullptr;
	}
	return GetMutableDefault<UACPSettings>();
}

TArray<FACPAgentConfig> UACPSettings::GetAgentConfigs() const
{
	TArray<FACPAgentConfig> Configs;
	FString ResolvedPath;

	// OpenRouter (built-in, native C++ - no external executable required)
	{
		FACPAgentConfig Config;
		Config.AgentName = TEXT("OpenRouter");
		Config.bIsBuiltIn = true;
		Config.InstallInstructions = TEXT("Built-in agent. Configure Betide API token (credits mode) or OpenRouter API key.");
		Config.ExecutablePath = TEXT("");
		Config.ApiKey = GetOpenRouterAuthToken();
		Config.ModelId = OpenRouterDefaultModel;
		Config.WorkingDirectory = FPaths::ProjectDir();

		if (HasOpenRouterAuth())
		{
			Config.Status = EACPAgentStatus::Available;
			Config.StatusMessage = TEXT("Ready");
		}
		else
		{
			// Even without OpenRouter/Betide auth, the ChatGateway agent is usable
			// if any other provider is configured (custom providers, Ollama, etc.)
			bool bHasAnyProvider = false;
			for (const FString& ProviderId : GetProviderPriority())
			{
				if (ProviderId == TEXT("openrouter")) continue;
				if (IsProviderConfigured(ProviderId))
				{
					bHasAnyProvider = true;
					break;
				}
			}

			if (bHasAnyProvider)
			{
				Config.Status = EACPAgentStatus::Available;
				Config.StatusMessage = TEXT("Ready (custom provider)");
			}
			else
			{
				Config.Status = EACPAgentStatus::MissingApiKey;
				Config.StatusMessage = ShouldUseBetideCredits()
					? TEXT("Betide API token not configured. Set it in Project Settings or BETIDE_API_TOKEN env var.")
					: TEXT("OpenRouter API key not configured. Set it in Project Settings > Plugins > Agent Integration Kit.");
			}
		}
		Configs.Add(Config);
	}

	// Custom agents from settings
	for (const FACPAgentSettingsEntry& Entry : CustomAgents)
	{
		if (Entry.AgentName.IsEmpty() || Entry.ExecutablePath.FilePath.IsEmpty())
		{
			continue;
		}

		FACPAgentConfig Config;
		Config.AgentName = Entry.AgentName;
		Config.ExecutablePath = Entry.ExecutablePath.FilePath;
		Config.Arguments = Entry.Arguments;
		Config.WorkingDirectory = Entry.WorkingDirectory.Path.IsEmpty() ? FPaths::ProjectDir() : Entry.WorkingDirectory.Path;
		Config.EnvironmentVariables = Entry.EnvironmentVariables;
		Config.ApiKey = Entry.ApiKey;
		Config.ModelId = Entry.ModelId;
		Config.bIsBuiltIn = false;
		Config.InstallInstructions = TEXT("Custom agent - check your configuration.");

		if (IsExecutableAvailable(Config.ExecutablePath, ResolvedPath))
		{
			Config.Status = EACPAgentStatus::Available;
			Config.StatusMessage = TEXT("Ready");
		}
		else
		{
			Config.Status = EACPAgentStatus::NotInstalled;
			Config.StatusMessage = FString::Printf(TEXT("Executable not found: %s"), *Entry.ExecutablePath.FilePath);
		}
		Configs.Add(Config);
	}

	return Configs;
}

FString UACPSettings::GetSavedModelForAgent(const FString& AgentName) const
{
	if (const FString* SavedModel = SelectedModelPerAgent.Find(AgentName))
	{
		return *SavedModel;
	}
	return FString();
}

void UACPSettings::SaveModelForAgent(const FString& AgentName, const FString& ModelId)
{
	if (ModelId.IsEmpty())
	{
		SelectedModelPerAgent.Remove(AgentName);
	}
	else
	{
		SelectedModelPerAgent.Add(AgentName, ModelId);
	}

	SavePreferences();
}

FString UACPSettings::GetSavedModeForAgent(const FString& AgentName) const
{
	if (const FString* SavedMode = SelectedModePerAgent.Find(AgentName))
	{
		return *SavedMode;
	}
	return FString();
}

void UACPSettings::SaveModeForAgent(const FString& AgentName, const FString& ModeId)
{
	if (ModeId.IsEmpty())
	{
		SelectedModePerAgent.Remove(AgentName);
	}
	else
	{
		SelectedModePerAgent.Add(AgentName, ModeId);
	}

	SavePreferences();
}

FString UACPSettings::GetSavedReasoningForAgent(const FString& AgentName) const
{
	if (const FString* SavedReasoning = SelectedReasoningPerAgent.Find(AgentName))
	{
		return *SavedReasoning;
	}
	return FString();
}

void UACPSettings::SaveReasoningForAgent(const FString& AgentName, const FString& ReasoningLevel)
{
	if (ReasoningLevel.IsEmpty())
	{
		SelectedReasoningPerAgent.Remove(AgentName);
	}
	else
	{
		SelectedReasoningPerAgent.Add(AgentName, ReasoningLevel);
	}

	SavePreferences();
}

bool UACPSettings::IsToolEnabled(const FString& ToolName) const
{
	// Global disable always wins
	if (DisabledTools.Contains(ToolName))
	{
		return false;
	}

	// If an active profile has a non-empty EnabledTools whitelist, check it
	if (const FAgentProfile* Profile = GetActiveProfile())
	{
		if (Profile->EnabledTools.Num() > 0)
		{
			return Profile->EnabledTools.Contains(ToolName);
		}
	}

	return true;
}

void UACPSettings::SetToolEnabled(const FString& ToolName, bool bEnabled)
{
	if (bEnabled)
	{
		DisabledTools.Remove(ToolName);
	}
	else
	{
		DisabledTools.Add(ToolName);
	}
	SaveConfig();
}

bool UACPSettings::ShouldUseBetideCredits() const
{
	return bUseBetideCredits;
}

FString UACPSettings::GetBetideApiToken() const
{
	FString Token = BetideApiToken.TrimStartAndEnd();
	if (!Token.IsEmpty())
	{
		return Token;
	}

	// Allow a machine/user-level token that works across projects and engine installs.
	FString EnvToken = FPlatformMisc::GetEnvironmentVariable(TEXT("BETIDE_API_TOKEN")).TrimStartAndEnd();
	if (!EnvToken.IsEmpty())
	{
		return EnvToken;
	}

	// Backward-compatible alias.
	return FPlatformMisc::GetEnvironmentVariable(TEXT("NEOSTACK_API_TOKEN")).TrimStartAndEnd();
}

bool UACPSettings::HasOpenRouterAuth() const
{
	return ShouldUseBetideCredits()
		? !GetBetideApiToken().IsEmpty()
		: !OpenRouterApiKey.TrimStartAndEnd().IsEmpty();
}

bool UACPSettings::HasMeshyAuth() const
{
	return ShouldUseBetideCredits()
		? !GetBetideApiToken().IsEmpty()
		: !MeshyApiKey.TrimStartAndEnd().IsEmpty();
}

bool UACPSettings::HasFalAuth() const
{
	return ShouldUseBetideCredits()
		? !GetBetideApiToken().IsEmpty()
		: !FalApiKey.TrimStartAndEnd().IsEmpty();
}

FString UACPSettings::GetOpenRouterAuthToken() const
{
	return ShouldUseBetideCredits() ? GetBetideApiToken() : OpenRouterApiKey;
}

FString UACPSettings::GetMeshyAuthToken() const
{
	return ShouldUseBetideCredits() ? GetBetideApiToken() : MeshyApiKey;
}

FString UACPSettings::GetFalAuthToken() const
{
	return ShouldUseBetideCredits() ? GetBetideApiToken() : FalApiKey;
}

FString UACPSettings::GetOpenRouterChatCompletionsUrl() const
{
	return ShouldUseBetideCredits()
		? TEXT("https://betide.studio/api/proxy/chat")
		: BuildOpenRouterUrl(OpenRouterBaseUrl, TEXT("/chat/completions"));
}

FString UACPSettings::GetOpenRouterImageGenerationUrl() const
{
	return ShouldUseBetideCredits()
		? TEXT("https://betide.studio/api/proxy/image")
		: BuildOpenRouterUrl(OpenRouterBaseUrl, TEXT("/chat/completions"));
}

FString UACPSettings::GetOpenRouterModelsUrl() const
{
	return ShouldUseBetideCredits()
		? TEXT("https://openrouter.ai/api/v1/models")
		: BuildOpenRouterUrl(OpenRouterBaseUrl, TEXT("/models"));
}

FString UACPSettings::GetMeshyBaseUrl() const
{
	return ShouldUseBetideCredits()
		? TEXT("https://betide.studio/api/proxy/meshy")
		: TEXT("https://api.meshy.ai");
}

FString UACPSettings::GetFalSubmitUrl() const
{
	return ShouldUseBetideCredits()
		? TEXT("https://betide.studio/api/proxy/fal/submit")
		: TEXT("https://queue.fal.run");
}

FString UACPSettings::GetFalStatusProxyUrl() const
{
	return TEXT("https://betide.studio/api/proxy/fal/status");
}

FString UACPSettings::GetFalResultProxyUrl() const
{
	return TEXT("https://betide.studio/api/proxy/fal/result");
}

FString UACPSettings::GetFalCancelProxyUrl() const
{
	return TEXT("https://betide.studio/api/proxy/fal/cancel");
}

// ============================================
// Profile Management
// ============================================

const FAgentProfile* UACPSettings::GetActiveProfile() const
{
	if (ActiveProfileId.IsEmpty())
	{
		return nullptr;
	}
	return FindProfileById(ActiveProfileId);
}

const FAgentProfile* UACPSettings::FindProfileById(const FString& ProfileId) const
{
	for (const FAgentProfile& Profile : Profiles)
	{
		if (Profile.ProfileId == ProfileId)
		{
			return &Profile;
		}
	}
	return nullptr;
}

FAgentProfile* UACPSettings::FindProfileByIdMutable(const FString& ProfileId)
{
	for (FAgentProfile& Profile : Profiles)
	{
		if (Profile.ProfileId == ProfileId)
		{
			return &Profile;
		}
	}
	return nullptr;
}

void UACPSettings::SetActiveProfile(const FString& ProfileId)
{
	ActiveProfileId = ProfileId;
	SaveConfig();
}

void UACPSettings::AddCustomProfile(const FAgentProfile& Profile)
{
	Profiles.Add(Profile);
	SaveConfig();
}

void UACPSettings::RemoveCustomProfile(const FString& ProfileId)
{
	Profiles.RemoveAll([&ProfileId](const FAgentProfile& P)
	{
		return P.ProfileId == ProfileId && !P.bIsBuiltIn;
	});

	// If the removed profile was active, clear the selection
	if (ActiveProfileId == ProfileId)
	{
		ActiveProfileId.Empty();
	}
	SaveConfig();
}

FString UACPSettings::GetEffectiveToolDescription(const FString& ToolName, const FString& DefaultDescription) const
{
	if (const FAgentProfile* Profile = GetActiveProfile())
	{
		if (const FString* Override = Profile->ToolDescriptionOverrides.Find(ToolName))
		{
			if (!Override->IsEmpty())
			{
				return *Override;
			}
		}
	}
	return DefaultDescription;
}

FString UACPSettings::GetProfileSystemPromptAppend() const
{
	FString Result = ACPSystemPromptAppend;

	if (const FAgentProfile* Profile = GetActiveProfile())
	{
		if (!Profile->CustomInstructions.IsEmpty())
		{
			if (!Result.IsEmpty())
			{
				Result += TEXT("\n\n");
			}
			Result += TEXT("=== ACTIVE PROFILE: ") + Profile->DisplayName + TEXT(" ===\n");
			Result += Profile->CustomInstructions;
		}
	}

	return Result;
}

ESystemPromptDelivery UACPSettings::GetSystemPromptDeliveryForAgent(const FString& AgentName) const
{
	if (const ESystemPromptDelivery* Found = SystemPromptDeliveryPerAgent.Find(AgentName))
	{
		return *Found;
	}
	return ESystemPromptDelivery::SessionMeta;
}

void UACPSettings::EnsureBuiltInSystemPromptDeliveryDefaults()
{
	// Pre-populate defaults for agents known to not support _meta.systemPrompt.
	// These show up in the UI so users can see and override them.
	// NOTE: Do NOT call SaveConfig() here — this runs from the constructor before
	// UE has loaded the config file, so SaveConfig() would overwrite saved user
	// settings (API keys, checkboxes, etc.) with default/empty values.

	if (!SystemPromptDeliveryPerAgent.Contains(TEXT("OpenCode")))
	{
		SystemPromptDeliveryPerAgent.Add(TEXT("OpenCode"), ESystemPromptDelivery::FirstUserMessage);
	}
}

void UACPSettings::EnsureBuiltInProfiles()
{
	auto HasProfile = [this](const FString& Id) -> bool
	{
		return FindProfileById(Id) != nullptr;
	};

	// --- Full Toolkit (all tools, no specialization) ---
	if (!HasProfile(TEXT("builtin_full")))
	{
		FAgentProfile P;
		P.ProfileId = TEXT("builtin_full");
		P.DisplayName = TEXT("Full Toolkit");
		P.Description = TEXT("All tools enabled, no specialization");
		P.bIsBuiltIn = true;
		// Empty EnabledTools = all tools
		Profiles.Add(MoveTemp(P));
	}

	// --- Animation ---
	if (!HasProfile(TEXT("builtin_animation")))
	{
		FAgentProfile P;
		P.ProfileId = TEXT("builtin_animation");
		P.DisplayName = TEXT("Animation");
		P.Description = TEXT("Motion matching, IK, retargeting, montages, anim blueprints");
		P.bIsBuiltIn = true;
		P.EnabledTools = {
			TEXT("execute_python"),
			TEXT("read_asset"),
			TEXT("edit_blueprint"),
			TEXT("edit_rigging"),
			TEXT("edit_animation_asset"),
			TEXT("edit_character_asset"),
			TEXT("edit_graph"),
			TEXT("read_logs"),
			TEXT("screenshot")
		};
		P.CustomInstructions = TEXT(
			"You are working in an ANIMATION-focused context.\n"
			"- edit_blueprint: Focus on Animation Blueprints (AnimGraphs, State Machines, Blend Spaces)\n"
			"- Use motion matching (Pose Search) for locomotion, not traditional state machines when appropriate\n"
			"- When setting up retargeting: create IK Rig first, then IK Retargeter\n"
			"- Always read_asset before modifying any animation asset to understand its current state"
		);
		P.ToolDescriptionOverrides.Add(TEXT("edit_blueprint"),
			TEXT("Edit Animation Blueprint graphs including AnimGraphs, State Machines, Blend Space players, "
				 "and locomotion logic. Add anim nodes, configure transitions, and set up motion matching choosers."));
		Profiles.Add(MoveTemp(P));
	}

	// --- Blueprint & Gameplay ---
	if (!HasProfile(TEXT("builtin_blueprint")))
	{
		FAgentProfile P;
		P.ProfileId = TEXT("builtin_blueprint");
		P.DisplayName = TEXT("Blueprint & Gameplay");
		P.Description = TEXT("Blueprint logic, components, gameplay systems, enhanced input");
		P.bIsBuiltIn = true;
		P.EnabledTools = {
			TEXT("execute_python"),
			TEXT("read_asset"),
			TEXT("edit_blueprint"),
			TEXT("edit_graph"),
			TEXT("edit_ai_tree"),
			TEXT("edit_data_structure"),
			TEXT("read_logs"),
			TEXT("screenshot")
		};
		P.CustomInstructions = TEXT(
			"You are working in a GAMEPLAY/BLUEPRINT-focused context.\n"
			"\n"
			"CRITICAL — Blueprint Creation:\n"
			"- create_asset(path, \"Blueprint\", {ParentClass=\"Character\"}) — ALWAYS specify ParentClass.\n"
			"  Default is Actor. Common parents: Character, Pawn, PlayerController, GameModeBase, ActorComponent, AnimInstance.\n"
			"- After creation, verify parent_class before adding nodes. Reparenting invalidates ALL existing nodes.\n"
			"\n"
			"CRITICAL — Node Selection:\n"
			"- find_nodes() results now show owning_class (e.g., class=Pawn vs class=Controller).\n"
			"  ALWAYS check owning_class when multiple results share the same name — pick the one matching your BP's parent.\n"
			"  Example: \"Get Control Rotation\" exists on both Controller and Pawn — pick class=Pawn for Character BPs.\n"
			"\n"
			"Blueprint Workflow:\n"
			"- Inherited C++ components (Mesh, CapsuleComponent, CharacterMovement) are NOT accessible via set() on the CDO.\n"
			"  Use Blueprint nodes in BeginPlay to configure them (Get Mesh, Get CharacterMovement, etc.).\n"
			"- For third-person characters: add SpringArmComponent + CameraComponent, set bUsePawnControlRotation on arm,\n"
			"  disable controller rotation on pawn, enable OrientRotationToMovement on CharacterMovement.\n"
			"- For Enhanced Input: Get Controller → Cast to PlayerController → Get EnhancedInputLocalPlayerSubsystem → Add Mapping Context.\n"
			"- Prioritize clean Blueprint architecture: use interfaces, components, and event dispatchers.\n"
			"- Always compile after editing and check read_logs for errors.\n"
			"- Do NOT explore the project extensively before starting. Execute the known pattern, then adapt."
		);
		Profiles.Add(MoveTemp(P));
	}

	// --- Cinematics ---
	if (!HasProfile(TEXT("builtin_cinematics")))
	{
		FAgentProfile P;
		P.ProfileId = TEXT("builtin_cinematics");
		P.DisplayName = TEXT("Cinematics");
		P.Description = TEXT("Level Sequences, camera work, animation playback");
		P.bIsBuiltIn = true;
		P.EnabledTools = {
			TEXT("execute_python"),
			TEXT("read_asset"),
			TEXT("edit_sequencer"),
			TEXT("edit_animation_asset"),
			TEXT("edit_graph"),
			TEXT("read_logs"),
			TEXT("screenshot")
		};
		P.CustomInstructions = TEXT(
			"You are working in a CINEMATICS/SEQUENCER-focused context.\n"
			"- Use edit_sequencer for Level Sequence editing (camera cuts, transforms, keyframes)\n"
			"- Use list_track_types and list_bindings to discover available options dynamically before editing\n"
			"- Build shots in passes like a human editor: rough cut pass, visual review pass, refinement pass\n"
			"- Use execute_shot_plan to block first-pass shots from beats, then refine with manual sequencer edits\n"
			"- Use analyze_camera_cuts after each pass to check pacing, gaps/overlaps, repeated angles, and review timestamps\n"
			"- For character animation in sequences, bind skeletal mesh actors and add animation tracks\n"
			"- Use screenshot at multiple moments per shot (start/middle/end), then adjust transforms/cuts and iterate\n"
			"- Avoid repetitive camera placement patterns; vary shot size, angle, and duration based on scene intent"
		);
		Profiles.Add(MoveTemp(P));
	}

	// --- VFX & Materials ---
	if (!HasProfile(TEXT("builtin_vfx")))
	{
		FAgentProfile P;
		P.ProfileId = TEXT("builtin_vfx");
		P.DisplayName = TEXT("VFX & Materials");
		P.Description = TEXT("Niagara particles, Material graphs, visual effects");
		P.bIsBuiltIn = true;
		P.EnabledTools = {
			TEXT("execute_python"),
			TEXT("read_asset"),
			TEXT("edit_niagara"),
			TEXT("edit_graph"),
			TEXT("read_logs"),
			TEXT("screenshot"),
			TEXT("generate_asset")
		};
		P.CustomInstructions = TEXT(
			"You are working in a VFX/MATERIALS-focused context.\n"
			"- edit_graph: Use for Material and PCG graph editing\n"
			"- edit_niagara: Use for particle system creation and modification\n"
			"- Use edit_graph with operation='find_nodes' to discover available Material expression and Niagara module node types\n"
			"- generate_asset with asset_type='image' can create textures for use in materials"
		);
		P.ToolDescriptionOverrides.Add(TEXT("edit_graph"),
			TEXT("Edit Material graphs and PCG graphs. Create material expressions, connect nodes for "
				 "shader logic, and build procedural generation graphs. Use operation='find_nodes' first to discover "
				 "available node types."));
		Profiles.Add(MoveTemp(P));
	}
}

// ── Provider Priority Helpers ────────────────────────────────────────

const TArray<FString>& UACPSettings::GetProviderPriority() const
{
	// Default: OpenRouter only if priority list is empty (backward compat)
	if (ProviderPriority.Num() == 0)
	{
		static const TArray<FString> Default = { TEXT("openrouter") };
		return Default;
	}
	return ProviderPriority;
}

void UACPSettings::SetProviderPriority(const TArray<FString>& Priority)
{
	ProviderPriority = Priority;
	SaveConfig();
}

void UACPSettings::AddProviderToPriority(const FString& ProviderId)
{
	if (!ProviderPriority.Contains(ProviderId))
	{
		// Ensure list is initialized from default if empty
		if (ProviderPriority.Num() == 0)
		{
			ProviderPriority.Add(TEXT("openrouter"));
		}
		ProviderPriority.Add(ProviderId);
		SaveConfig();
	}
}

void UACPSettings::RemoveProviderFromPriority(const FString& ProviderId)
{
	if (ProviderPriority.Num() == 0)
	{
		// Initialize from default so we can remove from it
		ProviderPriority.Add(TEXT("openrouter"));
	}
	ProviderPriority.Remove(ProviderId);
	SaveConfig();
}

bool UACPSettings::IsProviderConfigured(const FString& ProviderId) const
{
	const TArray<FString>& Priority = GetProviderPriority();
	if (!Priority.Contains(ProviderId))
	{
		return false;
	}

	// Check if the provider has an API key (or doesn't require one)
	TSharedPtr<IChatGatewayProvider> Prov = ChatGateway::FindProvider(ProviderId);
	if (!Prov.IsValid())
	{
		return false;
	}
	if (!Prov->RequiresApiKey())
	{
		return true;
	}
	return !GetProviderApiKey(ProviderId).IsEmpty();
}

FString UACPSettings::GetProviderApiKey(const FString& ProviderId) const
{
	if (ProviderId == TEXT("openrouter"))
	{
		const FString* MapKey = ProviderApiKeys.Find(ProviderId);
		if (MapKey && !MapKey->IsEmpty())
		{
			return *MapKey;
		}
		return GetOpenRouterAuthToken();
	}
	const FString* Key = ProviderApiKeys.Find(ProviderId);
	return Key ? *Key : FString();
}

FString UACPSettings::GetProviderBaseUrl(const FString& ProviderId) const
{
	const FString* Url = ProviderBaseUrls.Find(ProviderId);
	if (Url && !Url->IsEmpty())
	{
		return *Url;
	}
	if (ProviderId == TEXT("openrouter"))
	{
		return OpenRouterBaseUrl;
	}
	return FString();
}

void UACPSettings::SetProviderApiKey(const FString& ProviderId, const FString& Key)
{
	FString Trimmed = Key.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		ProviderApiKeys.Remove(ProviderId);
	}
	else
	{
		ProviderApiKeys.Add(ProviderId, Trimmed);
	}
	SaveConfig();
}

void UACPSettings::SetProviderBaseUrl(const FString& ProviderId, const FString& Url)
{
	FString Trimmed = Url.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		ProviderBaseUrls.Remove(ProviderId);
	}
	else
	{
		ProviderBaseUrls.Add(ProviderId, Trimmed);
	}
	SaveConfig();
}

// ── Custom Provider Helpers ──────────────────────────────────────────

FCustomProviderDefinition* UACPSettings::FindCustomProvider(const FString& ProviderId)
{
	for (FCustomProviderDefinition& Def : CustomProviders)
	{
		if (Def.ProviderId == ProviderId) return &Def;
	}
	return nullptr;
}

const FCustomProviderDefinition* UACPSettings::FindCustomProvider(const FString& ProviderId) const
{
	for (const FCustomProviderDefinition& Def : CustomProviders)
	{
		if (Def.ProviderId == ProviderId) return &Def;
	}
	return nullptr;
}

FString UACPSettings::CreateCustomProvider(const FString& DisplayName, const FString& BaseUrl)
{
	FCustomProviderDefinition Def;
	Def.ProviderId = TEXT("userprovider_") + FGuid::NewGuid().ToString(EGuidFormats::Short).ToLower();
	Def.DisplayName = DisplayName.TrimStartAndEnd();
	Def.BaseUrl = BaseUrl.TrimStartAndEnd();
	CustomProviders.Add(Def);
	// Also add to priority list
	AddProviderToPriority(Def.ProviderId);
	SaveConfig();
	return Def.ProviderId;
}

void UACPSettings::DeleteCustomProvider(const FString& ProviderId)
{
	CustomProviders.RemoveAll([&](const FCustomProviderDefinition& D) { return D.ProviderId == ProviderId; });
	RemoveProviderFromPriority(ProviderId);
	ProviderApiKeys.Remove(ProviderId);
	ProviderBaseUrls.Remove(ProviderId);
	SaveConfig();
}

void UACPSettings::UpdateCustomProvider(const FString& ProviderId, const FString& DisplayName, const FString& BaseUrl)
{
	if (FCustomProviderDefinition* Def = FindCustomProvider(ProviderId))
	{
		if (!DisplayName.IsEmpty()) Def->DisplayName = DisplayName.TrimStartAndEnd();
		if (!BaseUrl.IsEmpty()) Def->BaseUrl = BaseUrl.TrimStartAndEnd();
		SaveConfig();
	}
}

void UACPSettings::AddCustomProviderModel(const FString& ProviderId, const FString& ModelId, const FString& DisplayName, const FString& Description)
{
	if (FCustomProviderDefinition* Def = FindCustomProvider(ProviderId))
	{
		// Check for duplicate
		for (const FCustomProviderModelEntry& Existing : Def->Models)
		{
			if (Existing.ModelId == ModelId) return;
		}
		FCustomProviderModelEntry Entry;
		Entry.ModelId = ModelId;
		Entry.DisplayName = DisplayName.IsEmpty() ? ModelId : DisplayName;
		Entry.Description = Description;
		Def->Models.Add(Entry);
		SaveConfig();
	}
}

void UACPSettings::RemoveCustomProviderModel(const FString& ProviderId, const FString& ModelId)
{
	if (FCustomProviderDefinition* Def = FindCustomProvider(ProviderId))
	{
		Def->Models.RemoveAll([&](const FCustomProviderModelEntry& E) { return E.ModelId == ModelId; });
		SaveConfig();
	}
}

void UACPSettings::AddExtraProviderModel(const FString& ProviderId, const FString& ModelId, const FString& DisplayName, const FString& Description)
{
	TArray<FCustomProviderModelEntry>& Models = ExtraProviderModels.FindOrAdd(ProviderId);
	for (const FCustomProviderModelEntry& Existing : Models)
	{
		if (Existing.ModelId == ModelId) return;
	}
	FCustomProviderModelEntry Entry;
	Entry.ModelId = ModelId;
	Entry.DisplayName = DisplayName.IsEmpty() ? ModelId : DisplayName;
	Entry.Description = Description;
	Models.Add(Entry);
	SavePreferences();
}

void UACPSettings::RemoveExtraProviderModel(const FString& ProviderId, const FString& ModelId)
{
	if (TArray<FCustomProviderModelEntry>* Models = ExtraProviderModels.Find(ProviderId))
	{
		Models->RemoveAll([&](const FCustomProviderModelEntry& E) { return E.ModelId == ModelId; });
		if (Models->Num() == 0) ExtraProviderModels.Remove(ProviderId);
		SavePreferences();
	}
}

const TArray<FCustomProviderModelEntry>* UACPSettings::GetExtraProviderModels(const FString& ProviderId) const
{
	return ExtraProviderModels.Find(ProviderId);
}

int32 UACPSettings::ImportCustomProviderModels(const FString& ProviderId, const TArray<FCustomProviderModelEntry>& Models)
{
	FCustomProviderDefinition* Def = FindCustomProvider(ProviderId);
	if (!Def) return 0;

	TSet<FString> ExistingIds;
	for (const FCustomProviderModelEntry& E : Def->Models)
	{
		ExistingIds.Add(E.ModelId);
	}

	int32 Imported = 0;
	for (const FCustomProviderModelEntry& M : Models)
	{
		if (M.ModelId.IsEmpty() || ExistingIds.Contains(M.ModelId)) continue;
		Def->Models.Add(M);
		ExistingIds.Add(M.ModelId);
		++Imported;
	}

	if (Imported > 0) SaveConfig();
	return Imported;
}

void UACPSettings::SetCustomProviderModelDiscovery(const FString& ProviderId, bool bEnabled)
{
	if (FCustomProviderDefinition* Def = FindCustomProvider(ProviderId))
	{
		Def->bEnableModelDiscovery = bEnabled;
		SaveConfig();
	}
}

bool UACPSettings::IsModelEnabled(const FString& ModelId) const
{
	// Empty set = all models enabled (backward compat)
	if (EnabledModels.Num() == 0)
	{
		return true;
	}
	return EnabledModels.Contains(ModelId);
}

void UACPSettings::SetModelEnabled(const FString& ModelId, bool bEnabled)
{
	if (bEnabled)
	{
		EnabledModels.Add(ModelId);
	}
	else
	{
		EnabledModels.Remove(ModelId);
	}
	SavePreferences();
}

void UACPSettings::RefreshAgentStatus()
{
	FScopeLock Lock(&StatusCacheLock);
	CachedAgentStatus.Empty();
	LastStatusRefresh = FDateTime::UtcNow();

	TArray<FACPAgentConfig> Configs = GetAgentConfigs();
	for (const FACPAgentConfig& Config : Configs)
	{
		CachedAgentStatus.Add(Config.AgentName, Config.Status);
	}
}

void UACPSettings::InvalidateAgentStatusCache()
{
	FScopeLock Lock(&StatusCacheLock);
	CachedAgentStatus.Empty();
	LastStatusRefresh = FDateTime::MinValue();
}

bool UACPSettings::IsAgentStatusStale() const
{
	FScopeLock Lock(&StatusCacheLock);
	if (CachedAgentStatus.Num() == 0)
	{
		return true;
	}
	FDateTime Now = FDateTime::UtcNow();
	return (Now - LastStatusRefresh).GetTotalSeconds() > StatusCacheTTLSeconds;
}

// ============================================================================
// Unified Preferences Persistence (~/.agentintegrationkit/preferences.json)
// UE's SaveConfig() writes to Config/DefaultXxx.ini but the Saved config
// at Saved/Config/<Platform>/Xxx.ini takes precedence and can override with
// stale values. This JSON file is the single source of truth for all
// WebUI-mutated settings.
// ============================================================================

static FString GetPreferencesDir()
{
	FString HomeDir = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
	if (HomeDir.IsEmpty())
	{
		HomeDir = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
	}
	return FPaths::Combine(HomeDir, TEXT(".agentintegrationkit"));
}

static FString GetPreferencesFilePath()
{
	return FPaths::Combine(GetPreferencesDir(), TEXT("preferences.json"));
}

// Helpers: serialize TMap<FString,FString> ↔ FJsonObject
static TSharedRef<FJsonObject> MapToJson(const TMap<FString, FString>& Map)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	for (const auto& Pair : Map) { Obj->SetStringField(Pair.Key, Pair.Value); }
	return Obj;
}

static void JsonToMap(const TSharedPtr<FJsonObject>& Obj, TMap<FString, FString>& OutMap)
{
	OutMap.Empty();
	if (!Obj.IsValid()) return;
	for (const auto& Pair : Obj->Values)
	{
		FString Val;
		if (Pair.Value.IsValid() && Pair.Value->TryGetString(Val))
		{
			OutMap.Add(Pair.Key, Val);
		}
	}
}

// Helpers: serialize TArray/TSet<FString> ↔ JSON array
static TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Arr)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FString& S : Arr) { Out.Add(MakeShared<FJsonValueString>(S)); }
	return Out;
}

static TArray<TSharedPtr<FJsonValue>> StringSetToJson(const TSet<FString>& Set)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FString& S : Set) { Out.Add(MakeShared<FJsonValueString>(S)); }
	return Out;
}

static void JsonToStringArray(const TArray<TSharedPtr<FJsonValue>>* Arr, TArray<FString>& Out)
{
	Out.Empty();
	if (!Arr) return;
	for (const auto& Val : *Arr)
	{
		FString S;
		if (Val.IsValid() && Val->TryGetString(S) && !S.IsEmpty()) { Out.Add(S); }
	}
}

static void JsonToStringSet(const TArray<TSharedPtr<FJsonValue>>* Arr, TSet<FString>& Out)
{
	Out.Empty();
	if (!Arr) return;
	for (const auto& Val : *Arr)
	{
		FString S;
		if (Val.IsValid() && Val->TryGetString(S) && !S.IsEmpty()) { Out.Add(S); }
	}
}

// ESystemPromptDelivery ↔ string
static FString DeliveryToString(ESystemPromptDelivery D)
{
	switch (D)
	{
	case ESystemPromptDelivery::FirstUserMessage:  return TEXT("FirstUserMessage");
	case ESystemPromptDelivery::EveryUserMessage:  return TEXT("EveryUserMessage");
	default:                                       return TEXT("SessionMeta");
	}
}

static ESystemPromptDelivery StringToDelivery(const FString& S)
{
	if (S == TEXT("FirstUserMessage"))  return ESystemPromptDelivery::FirstUserMessage;
	if (S == TEXT("EveryUserMessage"))  return ESystemPromptDelivery::EveryUserMessage;
	return ESystemPromptDelivery::SessionMeta;
}

void UACPSettings::SavePreferences()
{
	const FString FilePath = GetPreferencesFilePath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

	// Installed agents
	Root->SetArrayField(TEXT("installedAgentIds"), StringArrayToJson(InstalledAgentIds));

	// Onboarding
	Root->SetBoolField(TEXT("onboardingCompleted"), bOnboardingCompleted);

	// Last used agent
	Root->SetStringField(TEXT("lastUsedAgent"), LastUsedAgentName);

	// Per-agent selections
	Root->SetObjectField(TEXT("selectedModels"), MapToJson(SelectedModelPerAgent));
	Root->SetObjectField(TEXT("selectedModes"), MapToJson(SelectedModePerAgent));
	Root->SetObjectField(TEXT("selectedReasoning"), MapToJson(SelectedReasoningPerAgent));

	// Continuation summary
	TSharedRef<FJsonObject> SummaryObj = MakeShared<FJsonObject>();
	SummaryObj->SetStringField(TEXT("provider"), ContinuationSummaryProvider);
	SummaryObj->SetStringField(TEXT("model"), ContinuationSummaryModel);
	SummaryObj->SetStringField(TEXT("defaultDetail"), ContinuationSummaryDefaultDetail);
	Root->SetObjectField(TEXT("continuationSummary"), SummaryObj);

	// Enabled models
	Root->SetArrayField(TEXT("enabledModels"), StringSetToJson(EnabledModels));

	// System prompt delivery per agent
	TSharedRef<FJsonObject> DeliveryObj = MakeShared<FJsonObject>();
	for (const auto& Pair : SystemPromptDeliveryPerAgent)
	{
		DeliveryObj->SetStringField(Pair.Key, DeliveryToString(Pair.Value));
	}
	Root->SetObjectField(TEXT("systemPromptDelivery"), DeliveryObj);

	// Provider priority
	Root->SetArrayField(TEXT("providerPriority"), StringArrayToJson(ProviderPriority));

	// Provider API keys (stored separately — not in config .ini)
	{
		TSharedRef<FJsonObject> KeysObj = MakeShared<FJsonObject>();
		for (const auto& Pair : ProviderApiKeys)
		{
			if (!Pair.Value.IsEmpty()) KeysObj->SetStringField(Pair.Key, Pair.Value);
		}
		Root->SetObjectField(TEXT("providerApiKeys"), KeysObj);
	}

	// Provider base URL overrides
	{
		TSharedRef<FJsonObject> UrlsObj = MakeShared<FJsonObject>();
		for (const auto& Pair : ProviderBaseUrls)
		{
			if (!Pair.Value.IsEmpty()) UrlsObj->SetStringField(Pair.Key, Pair.Value);
		}
		Root->SetObjectField(TEXT("providerBaseUrls"), UrlsObj);
	}

	// Extra provider models (for built-in providers like Ollama)
	{
		TSharedRef<FJsonObject> ExtraObj = MakeShared<FJsonObject>();
		for (const auto& Pair : ExtraProviderModels)
		{
			if (Pair.Value.Num() == 0) continue;
			TArray<TSharedPtr<FJsonValue>> ModelsArr;
			for (const FCustomProviderModelEntry& M : Pair.Value)
			{
				TSharedRef<FJsonObject> MObj = MakeShared<FJsonObject>();
				MObj->SetStringField(TEXT("modelId"), M.ModelId);
				MObj->SetStringField(TEXT("displayName"), M.DisplayName);
				MObj->SetStringField(TEXT("description"), M.Description);
				ModelsArr.Add(MakeShared<FJsonValueObject>(MObj));
			}
			ExtraObj->SetArrayField(Pair.Key, ModelsArr);
		}
		Root->SetObjectField(TEXT("extraProviderModels"), ExtraObj);
	}

	// Custom providers
	{
		TArray<TSharedPtr<FJsonValue>> CustomProvidersJson;
		for (const FCustomProviderDefinition& Def : CustomProviders)
		{
			TSharedRef<FJsonObject> ProvObj = MakeShared<FJsonObject>();
			ProvObj->SetStringField(TEXT("providerId"), Def.ProviderId);
			ProvObj->SetStringField(TEXT("displayName"), Def.DisplayName);
			ProvObj->SetStringField(TEXT("baseUrl"), Def.BaseUrl);
			ProvObj->SetBoolField(TEXT("requiresApiKey"), Def.bRequiresApiKey);
			ProvObj->SetBoolField(TEXT("enableModelDiscovery"), Def.bEnableModelDiscovery);

			TArray<TSharedPtr<FJsonValue>> ModelsJson;
			for (const FCustomProviderModelEntry& M : Def.Models)
			{
				TSharedRef<FJsonObject> ModelObj = MakeShared<FJsonObject>();
				ModelObj->SetStringField(TEXT("modelId"), M.ModelId);
				ModelObj->SetStringField(TEXT("displayName"), M.DisplayName);
				ModelObj->SetStringField(TEXT("description"), M.Description);
				ModelsJson.Add(MakeShared<FJsonValueObject>(ModelObj));
			}
			ProvObj->SetArrayField(TEXT("models"), ModelsJson);
			CustomProvidersJson.Add(MakeShared<FJsonValueObject>(ProvObj));
		}
		Root->SetArrayField(TEXT("customProviders"), CustomProvidersJson);
	}

	// Serialize
	FString JsonString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(Root, Writer);

	if (FFileHelper::SaveStringToFile(JsonString, *FilePath))
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPSettings: Saved preferences to %s"), *FilePath);
	}
	else
	{
		UE_LOG(LogAgentIntegrationKit, Error, TEXT("ACPSettings: Failed to save preferences to %s"), *FilePath);
	}
}

void UACPSettings::LoadPreferences()
{
	const FString FilePath = GetPreferencesFilePath();

	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
	{
		// Try migrating from legacy files
		FString LegacyDir = GetPreferencesDir();

		// Migrate installed_agents.json
		FString LegacyInstalled;
		if (FFileHelper::LoadFileToString(LegacyInstalled, *FPaths::Combine(LegacyDir, TEXT("installed_agents.json"))))
		{
			TSharedPtr<FJsonObject> Obj;
			TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(LegacyInstalled);
			if (FJsonSerializer::Deserialize(R, Obj) && Obj.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
				if (Obj->TryGetArrayField(TEXT("installed"), Arr))
				{
					JsonToStringArray(Arr, InstalledAgentIds);
				}
			}
		}

		// Migrate onboarding.json
		FString LegacyOnboarding;
		if (FFileHelper::LoadFileToString(LegacyOnboarding, *FPaths::Combine(LegacyDir, TEXT("onboarding.json"))))
		{
			TSharedPtr<FJsonObject> Obj;
			TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(LegacyOnboarding);
			if (FJsonSerializer::Deserialize(R, Obj) && Obj.IsValid())
			{
				Obj->TryGetBoolField(TEXT("completed"), bOnboardingCompleted);
			}
		}

		// Save the migrated data as preferences.json
		if (InstalledAgentIds.Num() > 0 || bOnboardingCompleted)
		{
			SavePreferences();
			UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPSettings: Migrated legacy JSON files to preferences.json"));
		}
		else
		{
			UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPSettings: No preferences.json found"));
		}
		return;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPSettings: Failed to parse preferences.json"));
		return;
	}

	// Installed agents
	const TArray<TSharedPtr<FJsonValue>>* InstalledArr = nullptr;
	if (Root->TryGetArrayField(TEXT("installedAgentIds"), InstalledArr))
	{
		JsonToStringArray(InstalledArr, InstalledAgentIds);
	}

	// Onboarding
	Root->TryGetBoolField(TEXT("onboardingCompleted"), bOnboardingCompleted);

	// Last used agent
	Root->TryGetStringField(TEXT("lastUsedAgent"), LastUsedAgentName);

	// Per-agent selections (use TryGetObjectField — GetObjectField asserts if missing)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Root->TryGetObjectField(TEXT("selectedModels"), Obj))    JsonToMap(*Obj, SelectedModelPerAgent);
		if (Root->TryGetObjectField(TEXT("selectedModes"), Obj))     JsonToMap(*Obj, SelectedModePerAgent);
		if (Root->TryGetObjectField(TEXT("selectedReasoning"), Obj)) JsonToMap(*Obj, SelectedReasoningPerAgent);
	}

	// Continuation summary
	{
		const TSharedPtr<FJsonObject>* SummaryPtr = nullptr;
		if (Root->TryGetObjectField(TEXT("continuationSummary"), SummaryPtr))
		{
			(*SummaryPtr)->TryGetStringField(TEXT("provider"), ContinuationSummaryProvider);
			(*SummaryPtr)->TryGetStringField(TEXT("model"), ContinuationSummaryModel);
			(*SummaryPtr)->TryGetStringField(TEXT("defaultDetail"), ContinuationSummaryDefaultDetail);
		}
	}

	// Enabled models
	const TArray<TSharedPtr<FJsonValue>>* ModelsArr = nullptr;
	if (Root->TryGetArrayField(TEXT("enabledModels"), ModelsArr))
	{
		JsonToStringSet(ModelsArr, EnabledModels);
	}

	// System prompt delivery per agent
	{
		const TSharedPtr<FJsonObject>* DeliveryPtr = nullptr;
		if (Root->TryGetObjectField(TEXT("systemPromptDelivery"), DeliveryPtr))
		{
			SystemPromptDeliveryPerAgent.Empty();
			for (const auto& Pair : (*DeliveryPtr)->Values)
			{
				FString Val;
				if (Pair.Value.IsValid() && Pair.Value->TryGetString(Val))
				{
					SystemPromptDeliveryPerAgent.Add(Pair.Key, StringToDelivery(Val));
				}
			}
		}
	}

	// Provider priority
	{
		const TArray<TSharedPtr<FJsonValue>>* PriorityArr = nullptr;
		if (Root->TryGetArrayField(TEXT("providerPriority"), PriorityArr))
		{
			TArray<FString> Loaded;
			JsonToStringArray(PriorityArr, Loaded);
			if (Loaded.Num() > 0) ProviderPriority = MoveTemp(Loaded);
		}
	}

	// Provider API keys
	{
		const TSharedPtr<FJsonObject>* KeysPtr = nullptr;
		if (Root->TryGetObjectField(TEXT("providerApiKeys"), KeysPtr))
		{
			for (const auto& Pair : (*KeysPtr)->Values)
			{
				FString Val;
				if (Pair.Value.IsValid() && Pair.Value->TryGetString(Val) && !Val.IsEmpty())
				{
					ProviderApiKeys.Add(Pair.Key, Val);
				}
			}
		}
	}

	// Provider base URL overrides
	{
		const TSharedPtr<FJsonObject>* UrlsPtr = nullptr;
		if (Root->TryGetObjectField(TEXT("providerBaseUrls"), UrlsPtr))
		{
			for (const auto& Pair : (*UrlsPtr)->Values)
			{
				FString Val;
				if (Pair.Value.IsValid() && Pair.Value->TryGetString(Val) && !Val.IsEmpty())
				{
					ProviderBaseUrls.Add(Pair.Key, Val);
				}
			}
		}
	}

	// Extra provider models (for built-in providers like Ollama)
	{
		const TSharedPtr<FJsonObject>* ExtraPtr = nullptr;
		if (Root->TryGetObjectField(TEXT("extraProviderModels"), ExtraPtr))
		{
			for (const auto& Pair : (*ExtraPtr)->Values)
			{
				if (!Pair.Value.IsValid()) continue;
				const TArray<TSharedPtr<FJsonValue>>* ExtraModelsArr = nullptr;
				if ((*ExtraPtr)->TryGetArrayField(Pair.Key, ExtraModelsArr))
				{
					TArray<FCustomProviderModelEntry>& Models = ExtraProviderModels.FindOrAdd(Pair.Key);
					for (const TSharedPtr<FJsonValue>& MVal : *ExtraModelsArr)
					{
						const TSharedPtr<FJsonObject> MObj = MVal->AsObject();
						if (!MObj.IsValid()) continue;
						FCustomProviderModelEntry Entry;
						MObj->TryGetStringField(TEXT("modelId"), Entry.ModelId);
						MObj->TryGetStringField(TEXT("displayName"), Entry.DisplayName);
						MObj->TryGetStringField(TEXT("description"), Entry.Description);
						if (!Entry.ModelId.IsEmpty()) Models.Add(Entry);
					}
				}
			}
		}
	}

	// Custom providers
	{
		const TArray<TSharedPtr<FJsonValue>>* CustomArr = nullptr;
		if (Root->TryGetArrayField(TEXT("customProviders"), CustomArr))
		{
			// Only load if config didn't already have them (preferences.json is backup)
			TSet<FString> ExistingIds;
			for (const FCustomProviderDefinition& Existing : CustomProviders)
			{
				ExistingIds.Add(Existing.ProviderId);
			}

			for (const TSharedPtr<FJsonValue>& Val : *CustomArr)
			{
				const TSharedPtr<FJsonObject> Obj = Val->AsObject();
				if (!Obj.IsValid()) continue;

				FCustomProviderDefinition Def;
				Obj->TryGetStringField(TEXT("providerId"), Def.ProviderId);
				if (Def.ProviderId.IsEmpty() || ExistingIds.Contains(Def.ProviderId)) continue;

				Obj->TryGetStringField(TEXT("displayName"), Def.DisplayName);
				Obj->TryGetStringField(TEXT("baseUrl"), Def.BaseUrl);
				Obj->TryGetBoolField(TEXT("requiresApiKey"), Def.bRequiresApiKey);
				Obj->TryGetBoolField(TEXT("enableModelDiscovery"), Def.bEnableModelDiscovery);

				const TArray<TSharedPtr<FJsonValue>>* ModelsArr2 = nullptr;
				if (Obj->TryGetArrayField(TEXT("models"), ModelsArr2))
				{
					for (const TSharedPtr<FJsonValue>& MVal : *ModelsArr2)
					{
						const TSharedPtr<FJsonObject> MObj = MVal->AsObject();
						if (!MObj.IsValid()) continue;
						FCustomProviderModelEntry Entry;
						MObj->TryGetStringField(TEXT("modelId"), Entry.ModelId);
						MObj->TryGetStringField(TEXT("displayName"), Entry.DisplayName);
						MObj->TryGetStringField(TEXT("description"), Entry.Description);
						if (!Entry.ModelId.IsEmpty()) Def.Models.Add(Entry);
					}
				}

				CustomProviders.Add(MoveTemp(Def));
				ExistingIds.Add(Def.ProviderId);
			}
		}
	}

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPSettings: Loaded preferences (%d installed agents, %d custom providers, lastUsed=%s, onboarding=%s)"),
		InstalledAgentIds.Num(), CustomProviders.Num(), *LastUsedAgentName, bOnboardingCompleted ? TEXT("done") : TEXT("pending"));
}

// Legacy wrappers — delegate to unified system
void UACPSettings::SaveInstalledAgentIds() { SavePreferences(); }
void UACPSettings::LoadInstalledAgentIds() { LoadPreferences(); }

#undef LOCTEXT_NAMESPACE
