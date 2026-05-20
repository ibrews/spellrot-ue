// Copyright 2026 Betide Studio. All Rights Reserved.

#include "ACPRegistryConfigGenerator.h"
#include "ACPAgentQuirks.h"
#include "ACPRegistryClient.h"
#include "ACPSettings.h"
#include "AgentInstaller.h"
#include "AgentIntegrationKitModule.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"

// ============================================================================
// Helpers
// ============================================================================

/** Try to resolve an executable name to an absolute path on the system */
static bool TryResolveExecutable(const FString& ExecutableName, FString& OutPath)
{
	if (ExecutableName.IsEmpty())
	{
		return false;
	}

	// If it's already an absolute path, just check existence
	if (!FPaths::IsRelative(ExecutableName))
	{
		if (IFileManager::Get().FileExists(*ExecutableName))
		{
			OutPath = ExecutableName;
			return true;
		}
		return false;
	}

	// Use the installer's path resolution (searches PATH, homebrew, bun, etc.)
	FAgentInstaller& Installer = FAgentInstaller::Get();
	if (Installer.ResolveExecutable(ExecutableName, OutPath))
	{
		return true;
	}

	// Try login shell resolution as fallback (catches nvm, rbenv, etc.)
	if (Installer.ResolveExecutableViaLoginShell(ExecutableName, OutPath))
	{
		return true;
	}

	return false;
}

/** Resolve npx executable path */
static bool ResolveNpxPath(FString& OutPath)
{
	// Try npx first, then look for it via node
	if (TryResolveExecutable(TEXT("npx"), OutPath))
	{
		return true;
	}

	// On Windows, try npx.cmd
#if PLATFORM_WINDOWS
	if (TryResolveExecutable(TEXT("npx.cmd"), OutPath))
	{
		return true;
	}
#endif

	return false;
}

/** Resolve uvx executable path */
static bool ResolveUvxPath(FString& OutPath)
{
	if (TryResolveExecutable(TEXT("uvx"), OutPath))
	{
		return true;
	}

#if PLATFORM_WINDOWS
	if (TryResolveExecutable(TEXT("uvx.cmd"), OutPath))
	{
		return true;
	}
#endif

	return false;
}

// ============================================================================
// Config Generation
// ============================================================================

TArray<FACPAgentConfig> FACPRegistryConfigGenerator::GenerateAllConfigs()
{
	TArray<FACPAgentConfig> Configs;

	const FACPRegistryClient& Registry = FACPRegistryClient::Get();
	if (!Registry.IsLoaded())
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("RegistryConfigGen: Registry not loaded yet, returning empty configs"));
		return Configs;
	}

	// Only generate configs for agents the user has "installed" (added to their list)
	// This matches Zed's model: Install = add to settings, process spawns lazily on first use.
	const UACPSettings* Settings = UACPSettings::Get();
	if (!Settings || Settings->InstalledAgentIds.Num() == 0)
	{
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("RegistryConfigGen: No installed agents in settings"));
		return Configs;
	}

	Configs.Reserve(Settings->InstalledAgentIds.Num());

	for (const FString& AgentId : Settings->InstalledAgentIds)
	{
		const FACPRegistryAgent* Agent = Registry.FindAgent(AgentId);
		if (!Agent)
		{
			UE_LOG(LogAgentIntegrationKit, Warning, TEXT("RegistryConfigGen: Installed agent '%s' not found in registry"), *AgentId);
			continue;
		}

		FACPAgentConfig Config;
		if (GenerateConfig(*Agent, Config))
		{
			UE_LOG(LogAgentIntegrationKit, Log, TEXT("RegistryConfigGen: Agent '%s' (id=%s) status=%d msg='%s' exe='%s'"),
				*Config.AgentName, *Config.RegistryId,
				(int32)Config.Status, *Config.StatusMessage, *Config.ExecutablePath);
			Configs.Add(MoveTemp(Config));
		}
	}

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("RegistryConfigGen: Generated %d agent configs for %d installed agents"), Configs.Num(), Settings->InstalledAgentIds.Num());
	return Configs;
}

/** Extract the Rust target triple from a Zed-style archive URL.
 *  e.g., ".../codex-acp-0.10.0-aarch64-apple-darwin.tar.gz" → "aarch64-apple-darwin", "tar.gz" */
static bool ExtractTargetAndExt(const FString& ArchiveUrl, FString& OutTarget, FString& OutExt)
{
	// Find the filename: everything after the last '/'
	int32 LastSlash;
	if (!ArchiveUrl.FindLastChar('/', LastSlash))
	{
		return false;
	}
	FString Filename = ArchiveUrl.Mid(LastSlash + 1);

	// Strip extension (.tar.gz or .zip)
	if (Filename.EndsWith(TEXT(".tar.gz")))
	{
		OutExt = TEXT("tar.gz");
		Filename.LeftChopInline(7);
	}
	else if (Filename.EndsWith(TEXT(".zip")))
	{
		OutExt = TEXT("zip");
		Filename.LeftChopInline(4);
	}
	else
	{
		return false;
	}

	// Pattern: "codex-acp-{version}-{target}" — find the third hyphen-separated segment
	// that starts the target triple (arch-vendor-os). We find the version by looking
	// for a segment that starts with a digit after the agent name prefix.
	// Simpler: find "{version}-" and take everything after it.
	// The version is the agent's semver. We just need the target suffix.
	// Split on '-' and find where the target triple begins (first segment that is an arch).
	static const TArray<FString> KnownArches = { TEXT("aarch64"), TEXT("x86_64"), TEXT("i686") };
	TArray<FString> Parts;
	Filename.ParseIntoArray(Parts, TEXT("-"));
	for (int32 i = 0; i < Parts.Num(); ++i)
	{
		if (KnownArches.Contains(Parts[i]))
		{
			TArray<FString> TargetParts;
			for (int32 j = i; j < Parts.Num(); ++j)
			{
				TargetParts.Add(Parts[j]);
			}
			OutTarget = FString::Join(TargetParts, TEXT("-"));
			return true;
		}
	}
	return false;
}

/** Apply BinaryArchiveUrlOverride from quirks to a distribution's binary targets */
static FACPRegistryDistribution ApplyArchiveUrlOverride(const FACPRegistryDistribution& Original, const FString& OverrideTemplate, const FString& Version)
{
	FACPRegistryDistribution Result = Original;

	for (auto& Pair : Result.BinaryTargets)
	{
		FString Target, Ext;
		if (ExtractTargetAndExt(Pair.Value.Archive, Target, Ext))
		{
			FString NewUrl = OverrideTemplate;
			NewUrl = NewUrl.Replace(TEXT("{version}"), *Version);
			NewUrl = NewUrl.Replace(TEXT("{target}"), *Target);
			NewUrl = NewUrl.Replace(TEXT("{ext}"), *Ext);
			Pair.Value.Archive = NewUrl;
		}
	}

	return Result;
}

bool FACPRegistryConfigGenerator::GenerateConfig(const FACPRegistryAgent& Agent, FACPAgentConfig& OutConfig)
{
	const FACPAgentQuirks& Quirks = FACPAgentQuirksMap::GetQuirks(Agent.Id);
	const FString PlatformKey = FACPRegistryClient::GetCurrentPlatformKey();

	// Apply archive URL override if the agent has one (e.g., Betide fork of codex-acp).
	// Keep npx/uvx as fallback so the agent stays Available while the managed binary
	// hasn't been downloaded yet. Once the binary IS downloaded, it takes priority
	// over npx in the resolution order (Strategy 1 before Strategy 2).
	FACPRegistryDistribution EffectiveDistribution = Agent.Distribution;
	if (!Quirks.BinaryArchiveUrlOverride.IsEmpty())
	{
		EffectiveDistribution = ApplyArchiveUrlOverride(Agent.Distribution, Quirks.BinaryArchiveUrlOverride, Agent.Version);
	}

	OutConfig = FACPAgentConfig();
	OutConfig.AgentName = Agent.Name;
	OutConfig.RegistryId = Agent.Id;
	OutConfig.WorkingDirectory = FPaths::ProjectDir();
	OutConfig.InstallInstructions = Agent.Description;

	// Check for ChatGateway transport (no subprocess needed)
	if (Quirks.Transport == EAgentTransport::ChatGateway)
	{
		OutConfig.bIsBuiltIn = true;
		OutConfig.Status = EACPAgentStatus::Available;
		OutConfig.StatusMessage = TEXT("Built-in agent");
		return true;
	}

	// Check user path override first
	FString UserOverride = GetUserPathOverride(Agent.Id);
	if (!UserOverride.IsEmpty())
	{
		FString ResolvedOverride;
		if (TryResolveExecutable(UserOverride, ResolvedOverride))
		{
			OutConfig.ExecutablePath = ResolvedOverride;
			OutConfig.Status = EACPAgentStatus::Available;
			OutConfig.StatusMessage = TEXT("Ready (user path)");
		}
		else
		{
			OutConfig.ExecutablePath = UserOverride;
			OutConfig.Status = EACPAgentStatus::NotInstalled;
			OutConfig.StatusMessage = FString::Printf(TEXT("User path not found: %s"), *UserOverride);
		}
		// Still apply args from registry manifest
		goto ApplyArgs;
	}

	// Strategy 1: Binary distribution (self-contained, no runtime deps)
	// Only use managed binaries (downloaded by us) — NOT system PATH binaries.
	// System PATH binaries can be outdated versions that lack features like session/list.
	// If the agent also has npx/uvx, prefer those (pinned to exact registry version).
	if (EffectiveDistribution.HasBinaryForPlatform(PlatformKey))
	{
		const FACPRegistryBinaryTarget& Target = EffectiveDistribution.BinaryTargets[PlatformKey];
		bool bManagedBinaryFound = false;

		// Check managed install directory only (Zed pattern — our downloaded binary)
		if (!Target.Archive.IsEmpty() && FAgentInstaller::IsAgentBinaryExtracted(Agent.Id, Target.Archive, Target.Cmd))
		{
			OutConfig.ExecutablePath = FAgentInstaller::GetExtractedAgentExecutable(Agent.Id, Target.Archive, Target.Cmd);
			OutConfig.Status = EACPAgentStatus::Available;
			OutConfig.StatusMessage = TEXT("Ready");
			bManagedBinaryFound = true;
		}

		if (bManagedBinaryFound)
		{
			// Apply args and env from binary manifest
			for (const FString& Arg : Target.Args)
			{
				OutConfig.Arguments.Add(Arg);
			}
			for (const auto& EnvPair : Target.Env)
			{
				OutConfig.EnvironmentVariables.Add(EnvPair.Key, EnvPair.Value);
			}
			return true;
		}

		// Managed binary not found — fall through to npx/uvx if available.
		// If this agent is binary-ONLY (no npx/uvx), try system PATH as last resort,
		// then mark as not-installed for lazy download.
		if (!EffectiveDistribution.HasNpx() && !EffectiveDistribution.HasUvx())
		{
			// Binary-only agent: try system PATH as fallback
			FString ResolvedCmd;
			bool bResolved = !Target.Cmd.IsEmpty() && TryResolveExecutable(Target.Cmd, ResolvedCmd);
			if (!bResolved && !Target.Cmd.IsEmpty())
			{
				FString BaseName = FPaths::GetCleanFilename(Target.Cmd);
				if (BaseName != Target.Cmd)
				{
					bResolved = TryResolveExecutable(BaseName, ResolvedCmd);
				}
			}

			if (bResolved)
			{
				OutConfig.ExecutablePath = ResolvedCmd;
				OutConfig.Status = EACPAgentStatus::Available;
				OutConfig.StatusMessage = TEXT("Ready (system)");
				for (const FString& Arg : Target.Args)
				{
					OutConfig.Arguments.Add(Arg);
				}
				for (const auto& EnvPair : Target.Env)
				{
					OutConfig.EnvironmentVariables.Add(EnvPair.Key, EnvPair.Value);
				}
				return true;
			}
		}

		// No managed binary and no system fallback for binary-only agents
		if (!EffectiveDistribution.HasNpx() && !EffectiveDistribution.HasUvx())
		{
			OutConfig.ExecutablePath = Target.Cmd;
			OutConfig.Status = EACPAgentStatus::NotInstalled;
			OutConfig.StatusMessage = TEXT("Will download on first use.");

			for (const FString& Arg : Target.Args)
			{
				OutConfig.Arguments.Add(Arg);
			}
			for (const auto& EnvPair : Target.Env)
			{
				OutConfig.EnvironmentVariables.Add(EnvPair.Key, EnvPair.Value);
			}
			return true;
		}
		// else: fall through to npx/uvx
	}

	// Strategy 2: npx distribution (requires Node.js)
	// Agent is in the user's installed list — mark Available if npx exists.
	if (EffectiveDistribution.HasNpx())
	{
		FString NpxPath;
		if (ResolveNpxPath(NpxPath))
		{
			OutConfig.ExecutablePath = NpxPath;
			OutConfig.Arguments.Add(EffectiveDistribution.NpxPackage);
			for (const FString& Arg : EffectiveDistribution.NpxArgs)
			{
				OutConfig.Arguments.Add(Arg);
			}
			OutConfig.Status = EACPAgentStatus::Available;
			OutConfig.StatusMessage = TEXT("Ready (via npx)");
		}
		else
		{
			OutConfig.Status = EACPAgentStatus::NotInstalled;
			OutConfig.StatusMessage = TEXT("Requires Node.js. Install Node.js to use this agent.");
		}

		return true;
	}

	// Strategy 3: uvx distribution (requires Python/uv)
	if (EffectiveDistribution.HasUvx())
	{
		FString UvxPath;
		if (ResolveUvxPath(UvxPath))
		{
			OutConfig.ExecutablePath = UvxPath;
			OutConfig.Arguments.Add(EffectiveDistribution.UvxPackage);
			for (const FString& Arg : EffectiveDistribution.UvxArgs)
			{
				OutConfig.Arguments.Add(Arg);
			}
			OutConfig.Status = EACPAgentStatus::Available;
			OutConfig.StatusMessage = TEXT("Ready (via uvx)");
		}
		else
		{
			OutConfig.Status = EACPAgentStatus::NotInstalled;
			OutConfig.StatusMessage = TEXT("Requires uv. Install uv (Python package manager) to use this agent.");
		}

		return true;
	}

	// No distribution method available for this platform
	return false;

ApplyArgs:
	// Apply registry manifest args (from the binary target if available, or npx/uvx)
	if (EffectiveDistribution.HasBinaryForPlatform(PlatformKey))
	{
		const FACPRegistryBinaryTarget& Target = EffectiveDistribution.BinaryTargets[PlatformKey];
		for (const FString& Arg : Target.Args)
		{
			OutConfig.Arguments.Add(Arg);
		}
		for (const auto& EnvPair : Target.Env)
		{
			OutConfig.EnvironmentVariables.Add(EnvPair.Key, EnvPair.Value);
		}
	}
	else if (EffectiveDistribution.HasNpx())
	{
		// For npx with user override, the user path IS the executable — add package as first arg
		OutConfig.Arguments.Add(EffectiveDistribution.NpxPackage);
		for (const FString& Arg : EffectiveDistribution.NpxArgs)
		{
			OutConfig.Arguments.Add(Arg);
		}
	}

	return true;
}

FString FACPRegistryConfigGenerator::GetUserPathOverride(const FString& RegistryId)
{
	const UACPSettings* Settings = UACPSettings::Get();
	if (!Settings)
	{
		return FString();
	}

	// Check the generic path overrides map
	if (const FString* Override = Settings->AgentPathOverrides.Find(RegistryId))
	{
		if (!Override->IsEmpty())
		{
			return *Override;
		}
	}

	return FString();
}
