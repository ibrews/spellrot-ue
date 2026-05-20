// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ACPRegistryClient.h"

DECLARE_DELEGATE_OneParam(FOnInstallProgress, const FString& /* StatusMessage */);
DECLARE_DELEGATE_TwoParams(FOnInstallComplete, bool /* bSuccess */, const FString& /* ErrorMessage */);

class AGENTINTEGRATIONKIT_API FAgentInstaller
{
public:
	static FAgentInstaller& Get();

	// Binary permissions (used by registry binary installs)
	static bool EnsureNativeAdapterExecutable(const FString& BinaryPath);

	// Executable resolution (for base CLIs like claude, codex, cursor)
	TArray<FString> GetExtendedPaths() const;
	bool ResolveExecutable(const FString& ExecutableName, FString& OutResolvedPath) const;
	bool ResolveExecutableViaLoginShell(const FString& ExecutableName, FString& OutResolvedPath) const;

	// ── Registry-based Installation ─────────────────────────────────

	/** Install method preference for registry agents */
	enum class ERegistryInstallMethod : uint8
	{
		Binary,  // Download platform-specific archive
		Npx,     // Use npx (requires Node.js)
		Uvx,     // Use uvx (requires uv/Python)
		Auto,    // Best available: Binary > Npx > Uvx
	};

	/** Status of a registry agent's installation */
	struct FRegistryInstallStatus
	{
		FString AgentId;
		FString InstalledVersion;
		FString InstalledPath;   // Path to binary or resolved command
		ERegistryInstallMethod Method = ERegistryInstallMethod::Auto;
		bool bIsInstalled = false;
		bool bUpdateAvailable = false; // Registry version > installed version
	};

	/** Install directory for registry agents: ~/.agentintegrationkit/agents/ */
	static FString GetManagedAgentsDir();

	/** Get the install directory for a specific agent version */
	static FString GetAgentInstallDir(const FString& AgentId, const FString& Version);

	/** Get the install directory based on archive URL hash (Zed pattern) */
	static FString GetAgentVersionDir(const FString& AgentId, const FString& ArchiveUrl);

	/** Check if an agent binary has been downloaded and extracted */
	static bool IsAgentBinaryExtracted(const FString& AgentId, const FString& ArchiveUrl, const FString& Cmd);

	/** Get the resolved executable path for an extracted agent binary */
	static FString GetExtractedAgentExecutable(const FString& AgentId, const FString& ArchiveUrl, const FString& Cmd);

	/** Check install status for a registry agent */
	FRegistryInstallStatus GetRegistryInstallStatus(const FACPRegistryAgent& Agent) const;

	/** Install a registry agent (async — downloads archive, extracts, sets permissions) */
	void InstallRegistryAgentAsync(
		const FACPRegistryAgent& Agent,
		ERegistryInstallMethod PreferredMethod,
		FOnInstallProgress OnProgress,
		FOnInstallComplete OnComplete
	);

	/** Uninstall a registry agent (removes downloaded binaries) */
	bool UninstallRegistryAgent(const FString& AgentId);

private:
	FAgentInstaller() = default;

	FString GetLoginShellPath() const;
	FString BuildShellCommand(const FString& Command) const;

	// Registry install helpers
	void RunRegistryInstallOnBackgroundThread(
		const FACPRegistryAgent& Agent,
		ERegistryInstallMethod Method,
		FOnInstallProgress OnProgress,
		FOnInstallComplete OnComplete
	);
	bool DownloadAndExtractBinary(const FACPRegistryBinaryTarget& Target, const FString& AgentId, const FString& Version, FOnInstallProgress OnProgress, FString& OutError);
	bool ExtractArchive(const FString& ArchivePath, const FString& DestDir, FString& OutError);
	bool WriteInstallManifest(const FString& AgentId, const FString& Version, ERegistryInstallMethod Method, const FString& Cmd);
public:
	bool ReadInstallManifest(const FString& AgentId, FString& OutVersion, FString& OutCmd) const;
private:

	mutable FCriticalSection CacheLock;
	mutable TMap<FString, FString> ResolvedPathCache;
	mutable FDateTime LastCacheRefresh;
	static constexpr double CacheTTLSeconds = 300.0;
};
