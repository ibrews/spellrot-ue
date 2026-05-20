// Copyright 2026 Betide Studio. All Rights Reserved.

#include "AgentInstaller.h"
#include "ACPAgentQuirks.h"
#include "AgentIntegrationKitModule.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Async/Async.h"

namespace
{
static bool EnsureUnixBinaryExecutable(const FString& BinaryPath, const TCHAR* BinaryLabel, bool bClearMacQuarantine)
{
#if PLATFORM_MAC || PLATFORM_LINUX
	if (BinaryPath.IsEmpty() || !IFileManager::Get().FileExists(*BinaryPath))
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("AgentInstaller: %s path is missing: %s"), BinaryLabel, *BinaryPath);
		return false;
	}

	FString StdOut, StdErr;
	int32 ReturnCode = -1;
	FPlatformProcess::ExecProcess(
		TEXT("/bin/chmod"),
		*FString::Printf(TEXT("+x \"%s\""), *BinaryPath),
		&ReturnCode, &StdOut, &StdErr);
	if (ReturnCode != 0)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("AgentInstaller: Failed to set execute permission on %s: %s"), BinaryLabel, *StdErr);
	}

#if PLATFORM_MAC
	if (bClearMacQuarantine)
	{
		StdOut.Empty();
		StdErr.Empty();
		ReturnCode = -1;
		FPlatformProcess::ExecProcess(
			TEXT("/usr/bin/xattr"),
			*FString::Printf(TEXT("-d com.apple.quarantine \"%s\""), *BinaryPath),
			&ReturnCode, &StdOut, &StdErr);
		if (ReturnCode != 0 && !StdErr.Contains(TEXT("No such xattr")))
		{
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("AgentInstaller: Could not clear quarantine for %s: %s"), BinaryLabel, *StdErr);
		}
	}
#endif

	StdOut.Empty();
	StdErr.Empty();
	ReturnCode = -1;
	FPlatformProcess::ExecProcess(
		TEXT("/bin/test"),
		*FString::Printf(TEXT("-x \"%s\""), *BinaryPath),
		&ReturnCode, &StdOut, &StdErr);
	if (ReturnCode != 0)
	{
		UE_LOG(LogAgentIntegrationKit, Error, TEXT("AgentInstaller: %s is not executable: %s"), BinaryLabel, *BinaryPath);
		return false;
	}

	return true;
#else
	return true;
#endif
}
} // anonymous namespace

FAgentInstaller& FAgentInstaller::Get()
{
	static FAgentInstaller Instance;
	return Instance;
}

bool FAgentInstaller::EnsureNativeAdapterExecutable(const FString& BinaryPath)
{
#if PLATFORM_WINDOWS
	return !BinaryPath.IsEmpty() && IFileManager::Get().FileExists(*BinaryPath);
#else
	return EnsureUnixBinaryExecutable(BinaryPath, TEXT("native adapter binary"), true);
#endif
}

// ============================================
// Executable Resolution (for base CLIs)
// ============================================

TArray<FString> FAgentInstaller::GetExtendedPaths() const
{
	TArray<FString> Paths;

#if PLATFORM_MAC
	FString HomeDir = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
	if (!HomeDir.IsEmpty())
	{
		Paths.Add(FPaths::Combine(HomeDir, TEXT("bin")));
		Paths.Add(FPaths::Combine(HomeDir, TEXT(".bun/bin")));
		Paths.Add(FPaths::Combine(HomeDir, TEXT(".npm-global/bin")));
		Paths.Add(FPaths::Combine(HomeDir, TEXT(".nvm/current/bin")));
		Paths.Add(FPaths::Combine(HomeDir, TEXT("n/bin")));
		Paths.Add(FPaths::Combine(HomeDir, TEXT(".local/share/pnpm")));
		Paths.Add(FPaths::Combine(HomeDir, TEXT(".local/bin")));
		Paths.Add(FPaths::Combine(HomeDir, TEXT(".opencode/bin")));
		Paths.Add(FPaths::Combine(HomeDir, TEXT(".asdf/shims")));
		Paths.Add(FPaths::Combine(HomeDir, TEXT(".volta/bin")));
		Paths.Add(FPaths::Combine(HomeDir, TEXT(".fnm/current/bin")));
		Paths.Add(FPaths::Combine(HomeDir, TEXT(".proto/shims")));
		Paths.Add(FPaths::Combine(HomeDir, TEXT(".local/share/mise/shims")));
	}
	Paths.Add(TEXT("/usr/local/bin"));
	Paths.Add(TEXT("/opt/homebrew/bin"));
	Paths.Add(TEXT("/usr/bin"));
#elif PLATFORM_WINDOWS
	FString UserProfile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
	if (!UserProfile.IsEmpty())
	{
		// Claude Code installs here via winget/install.ps1 (often not added to PATH)
		Paths.Add(FPaths::Combine(UserProfile, TEXT(".local/bin")));
		// Codex CLI
		Paths.Add(FPaths::Combine(UserProfile, TEXT(".codex/bin")));
		Paths.Add(FPaths::Combine(UserProfile, TEXT(".bun/bin")));
		Paths.Add(FPaths::Combine(UserProfile, TEXT(".volta/bin")));
		Paths.Add(FPaths::Combine(UserProfile, TEXT(".fnm/current/bin")));
		Paths.Add(FPaths::Combine(UserProfile, TEXT(".proto/shims")));
	}
	FString AppData = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));
	if (!AppData.IsEmpty())
	{
		Paths.Add(FPaths::Combine(AppData, TEXT("npm")));
	}
	FString LocalAppData = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
	if (!LocalAppData.IsEmpty())
	{
		Paths.Add(FPaths::Combine(LocalAppData, TEXT("Volta/bin")));
		Paths.Add(FPaths::Combine(LocalAppData, TEXT("fnm_multishells")));
	}
	FString ProgramFiles = FPlatformMisc::GetEnvironmentVariable(TEXT("ProgramFiles"));
	if (!ProgramFiles.IsEmpty())
	{
		Paths.Add(FPaths::Combine(ProgramFiles, TEXT("nodejs")));
	}
#else
	// Linux
	FString HomeDir = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
	if (!HomeDir.IsEmpty())
	{
		Paths.Add(FPaths::Combine(HomeDir, TEXT("bin")));
		Paths.Add(FPaths::Combine(HomeDir, TEXT(".bun/bin")));
		Paths.Add(FPaths::Combine(HomeDir, TEXT(".npm-global/bin")));
		Paths.Add(FPaths::Combine(HomeDir, TEXT(".local/bin")));
		Paths.Add(FPaths::Combine(HomeDir, TEXT(".opencode/bin")));
		Paths.Add(FPaths::Combine(HomeDir, TEXT(".nvm/current/bin")));
		Paths.Add(FPaths::Combine(HomeDir, TEXT(".asdf/shims")));
		Paths.Add(FPaths::Combine(HomeDir, TEXT(".volta/bin")));
		Paths.Add(FPaths::Combine(HomeDir, TEXT(".fnm/current/bin")));
		Paths.Add(FPaths::Combine(HomeDir, TEXT(".proto/shims")));
		Paths.Add(FPaths::Combine(HomeDir, TEXT(".local/share/mise/shims")));
	}
	Paths.Add(TEXT("/usr/local/bin"));
	Paths.Add(TEXT("/usr/bin"));
#endif

	return Paths;
}

bool FAgentInstaller::ResolveExecutable(const FString& ExecutableName, FString& OutResolvedPath) const
{
	// Check cache first
	{
		FScopeLock Lock(&CacheLock);
		FDateTime Now = FDateTime::UtcNow();
		if ((Now - LastCacheRefresh).GetTotalSeconds() < CacheTTLSeconds)
		{
			if (const FString* CachedPath = ResolvedPathCache.Find(ExecutableName))
			{
				if (!CachedPath->IsEmpty() && IFileManager::Get().FileExists(**CachedPath))
				{
					OutResolvedPath = *CachedPath;
					return true;
				}
			}
		}
		else
		{
			ResolvedPathCache.Empty();
			LastCacheRefresh = Now;
		}
	}

	// If it's an absolute path, check if file exists
	if (!FPaths::IsRelative(ExecutableName) || ExecutableName.Contains(TEXT("/")) || ExecutableName.Contains(TEXT("\\")))
	{
		FString NormalizedPath = ExecutableName;
		FPaths::NormalizeFilename(NormalizedPath);

		if (IFileManager::Get().FileExists(*NormalizedPath))
		{
			OutResolvedPath = NormalizedPath;
			FScopeLock Lock(&CacheLock);
			ResolvedPathCache.Add(ExecutableName, NormalizedPath);
			return true;
		}
		return false;
	}

	// Check extended paths
	TArray<FString> SearchPaths = GetExtendedPaths();
	for (const FString& BasePath : SearchPaths)
	{
		FString FullPath = FPaths::Combine(BasePath, ExecutableName);
		if (IFileManager::Get().FileExists(*FullPath))
		{
			OutResolvedPath = FullPath;
			FScopeLock Lock(&CacheLock);
			ResolvedPathCache.Add(ExecutableName, FullPath);
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("AgentInstaller: Found %s at %s"), *ExecutableName, *FullPath);
			return true;
		}

#if PLATFORM_WINDOWS
		// Try with .cmd extension
		FString CmdPath = FullPath + TEXT(".cmd");
		if (IFileManager::Get().FileExists(*CmdPath))
		{
			OutResolvedPath = CmdPath;
			FScopeLock Lock(&CacheLock);
			ResolvedPathCache.Add(ExecutableName, CmdPath);
			return true;
		}
		// Try with .exe extension
		FString ExePath = FullPath + TEXT(".exe");
		if (IFileManager::Get().FileExists(*ExePath))
		{
			OutResolvedPath = ExePath;
			FScopeLock Lock(&CacheLock);
			ResolvedPathCache.Add(ExecutableName, ExePath);
			return true;
		}
#endif
	}

	// Try login shell resolution as fallback
	if (ResolveExecutableViaLoginShell(ExecutableName, OutResolvedPath))
	{
		FScopeLock Lock(&CacheLock);
		ResolvedPathCache.Add(ExecutableName, OutResolvedPath);
		return true;
	}

	return false;
}

bool FAgentInstaller::ResolveExecutableViaLoginShell(const FString& ExecutableName, FString& OutResolvedPath) const
{
#if PLATFORM_WINDOWS
	FString StdOut, StdErr;
	int32 ReturnCode = -1;
	FPlatformProcess::ExecProcess(TEXT("where"), *ExecutableName, &ReturnCode, &StdOut, &StdErr);
	if (ReturnCode == 0 && !StdOut.IsEmpty())
	{
		StdOut.TrimStartAndEndInline();

		TArray<FString> Results;
		StdOut.ParseIntoArrayLines(Results, true);

		if (Results.Num() == 0)
		{
			return false;
		}

		if (Results.Num() == 1)
		{
			OutResolvedPath = Results[0];
			OutResolvedPath.TrimStartAndEndInline();
			return true;
		}

		// Multiple results - prefer CLI wrappers (.cmd, .bat) over desktop apps (.exe)
		FString BestCmdPath;
		FString BestExePath;

		for (const FString& Path : Results)
		{
			FString TrimmedPath = Path;
			TrimmedPath.TrimStartAndEndInline();

			if (TrimmedPath.EndsWith(TEXT(".cmd"), ESearchCase::IgnoreCase) ||
			    TrimmedPath.EndsWith(TEXT(".bat"), ESearchCase::IgnoreCase))
			{
				if (BestCmdPath.IsEmpty())
				{
					BestCmdPath = TrimmedPath;
				}
			}
			else if (TrimmedPath.EndsWith(TEXT(".exe"), ESearchCase::IgnoreCase))
			{
				if (BestExePath.IsEmpty())
				{
					BestExePath = TrimmedPath;
				}
			}
		}

		if (!BestCmdPath.IsEmpty())
		{
			OutResolvedPath = BestCmdPath;
			return true;
		}
		if (!BestExePath.IsEmpty())
		{
			OutResolvedPath = BestExePath;
			return true;
		}

		OutResolvedPath = Results[0];
		OutResolvedPath.TrimStartAndEndInline();
		return true;
	}
	return false;
#else
	FString ShellPath = GetLoginShellPath();
	if (ShellPath.IsEmpty())
	{
		ShellPath = TEXT("/bin/bash");
	}

	FString Command = FString::Printf(TEXT("which %s"), *ExecutableName);
	FString StdOut, StdErr;
	int32 ReturnCode = -1;

	FPlatformProcess::ExecProcess(*ShellPath, *BuildShellCommand(Command), &ReturnCode, &StdOut, &StdErr);

	if (ReturnCode == 0 && !StdOut.IsEmpty())
	{
		StdOut.TrimStartAndEndInline();
		int32 NewlineIndex;
		if (StdOut.FindChar(TEXT('\n'), NewlineIndex))
		{
			OutResolvedPath = StdOut.Left(NewlineIndex);
		}
		else
		{
			OutResolvedPath = StdOut;
		}
		OutResolvedPath.TrimStartAndEndInline();

		if (!OutResolvedPath.IsEmpty() && !OutResolvedPath.Contains(TEXT("not found")))
		{
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("AgentInstaller: Login shell resolved %s to %s"), *ExecutableName, *OutResolvedPath);
			return true;
		}
	}

	return false;
#endif
}

// ============================================
// Utilities
// ============================================

FString FAgentInstaller::GetLoginShellPath() const
{
#if PLATFORM_MAC || PLATFORM_LINUX
	FString Shell = FPlatformMisc::GetEnvironmentVariable(TEXT("SHELL"));
	if (!Shell.IsEmpty())
	{
		return Shell;
	}
#endif
	return FString();
}

FString FAgentInstaller::BuildShellCommand(const FString& Command) const
{
#if PLATFORM_WINDOWS
	return FString::Printf(TEXT("/c %s"), *Command);
#else
	// Ensure user-local install locations are immediately discoverable by installer scripts.
	const FString CommandWithPathBootstrap = FString::Printf(
		TEXT("export PATH=\"$HOME/.local/bin:$HOME/.cargo/bin:$HOME/.bun/bin:$PATH\"; %s"),
		*Command
	);

	FString Escaped = CommandWithPathBootstrap;
	Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
	Escaped.ReplaceInline(TEXT("`"), TEXT("\\`"));
	return FString::Printf(TEXT("-l -c \"%s\""), *Escaped);
#endif
}

// ============================================================================
// Registry-Based Agent Installation
// ============================================================================

FString FAgentInstaller::GetManagedAgentsDir()
{
	FString HomeDir;
#if PLATFORM_WINDOWS
	HomeDir = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
#else
	HomeDir = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
#endif
	if (HomeDir.IsEmpty())
	{
		return FString();
	}
	return FPaths::Combine(HomeDir, TEXT(".agentintegrationkit"), TEXT("agents"));
}

FString FAgentInstaller::GetAgentInstallDir(const FString& AgentId, const FString& Version)
{
	FString BaseDir = GetManagedAgentsDir();
	if (BaseDir.IsEmpty())
	{
		return FString();
	}
	return FPaths::Combine(BaseDir, AgentId, Version);
}

FString FAgentInstaller::GetAgentVersionDir(const FString& AgentId, const FString& ArchiveUrl)
{
	FString BaseDir = GetManagedAgentsDir();
	if (BaseDir.IsEmpty() || ArchiveUrl.IsEmpty())
	{
		return FString();
	}

	// Hash the archive URL to create a stable version directory (Zed pattern).
	// When the registry updates the archive URL, a new directory is created automatically.
	FString UrlHash = FMD5::HashAnsiString(*ArchiveUrl);
	return FPaths::Combine(BaseDir, AgentId, FString::Printf(TEXT("v_%s"), *UrlHash));
}

bool FAgentInstaller::IsAgentBinaryExtracted(const FString& AgentId, const FString& ArchiveUrl, const FString& Cmd)
{
	FString VersionDir = GetAgentVersionDir(AgentId, ArchiveUrl);
	if (VersionDir.IsEmpty())
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("AgentInstaller: IsAgentBinaryExtracted('%s') — VersionDir is empty (AgentId='%s', ArchiveUrl='%s')"),
			*Cmd, *AgentId, *ArchiveUrl);
		return false;
	}

	if (!IFileManager::Get().DirectoryExists(*VersionDir))
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("AgentInstaller: IsAgentBinaryExtracted('%s') — directory does not exist: %s"),
			*AgentId, *VersionDir);
		return false;
	}

	// If cmd is specified, verify the executable exists
	if (!Cmd.IsEmpty())
	{
		FString CmdPath = Cmd;
		if (CmdPath.StartsWith(TEXT("./")))  CmdPath.RightChopInline(2);
		if (CmdPath.StartsWith(TEXT(".\\"))) CmdPath.RightChopInline(2);
		FString FullPath = FPaths::Combine(VersionDir, CmdPath);
		FPaths::NormalizeFilename(FullPath);
		bool bExists = IFileManager::Get().FileExists(*FullPath);
		if (!bExists)
		{
			UE_LOG(LogAgentIntegrationKit, Warning, TEXT("AgentInstaller: IsAgentBinaryExtracted('%s') — file not found: %s"),
				*AgentId, *FullPath);
			// List what IS in the directory to help diagnose
			TArray<FString> FoundFiles;
			IFileManager::Get().FindFilesRecursive(FoundFiles, *VersionDir, TEXT("*"), true, false);
			for (const FString& F : FoundFiles)
			{
				UE_LOG(LogAgentIntegrationKit, Warning, TEXT("AgentInstaller:   found: %s"), *F);
			}
		}
		return bExists;
	}

	return true;
}

FString FAgentInstaller::GetExtractedAgentExecutable(const FString& AgentId, const FString& ArchiveUrl, const FString& Cmd)
{
	FString VersionDir = GetAgentVersionDir(AgentId, ArchiveUrl);
	if (VersionDir.IsEmpty() || Cmd.IsEmpty())
	{
		return FString();
	}

	FString CmdPath = Cmd;
	if (CmdPath.StartsWith(TEXT("./")))  CmdPath.RightChopInline(2);
	if (CmdPath.StartsWith(TEXT(".\\"))) CmdPath.RightChopInline(2);
	FString FullPath = FPaths::Combine(VersionDir, CmdPath);
	FPaths::NormalizeFilename(FullPath);
	return FullPath;
}

FAgentInstaller::FRegistryInstallStatus FAgentInstaller::GetRegistryInstallStatus(const FACPRegistryAgent& Agent) const
{
	FRegistryInstallStatus Status;
	Status.AgentId = Agent.Id;

	// Check for installed binary
	FString InstalledVersion, InstalledCmd;
	if (ReadInstallManifest(Agent.Id, InstalledVersion, InstalledCmd))
	{
		FString InstallDir = GetAgentInstallDir(Agent.Id, InstalledVersion);
		FString FullPath = FPaths::Combine(InstallDir, InstalledCmd);
		FPaths::NormalizeFilename(FullPath);

		if (IFileManager::Get().FileExists(*FullPath))
		{
			Status.bIsInstalled = true;
			Status.InstalledVersion = InstalledVersion;
			Status.InstalledPath = FullPath;
			Status.Method = ERegistryInstallMethod::Binary;
			Status.bUpdateAvailable = (InstalledVersion != Agent.Version);
			return Status;
		}
	}

	// npx/uvx agents are NOT considered "installed" just because the runtime exists.
	// They're launchable on demand but require auth setup. The registry UI shows
	// "Available via npx/uvx" separately from the "Installed" badge.

	return Status;
}

void FAgentInstaller::InstallRegistryAgentAsync(
	const FACPRegistryAgent& Agent,
	ERegistryInstallMethod PreferredMethod,
	FOnInstallProgress OnProgress,
	FOnInstallComplete OnComplete)
{
	const FString PlatformKey = FACPRegistryClient::GetCurrentPlatformKey();

	// Resolve method if Auto
	ERegistryInstallMethod Method = PreferredMethod;
	if (Method == ERegistryInstallMethod::Auto)
	{
		if (Agent.Distribution.HasBinaryForPlatform(PlatformKey))
		{
			Method = ERegistryInstallMethod::Binary;
		}
		else if (Agent.Distribution.HasNpx())
		{
			Method = ERegistryInstallMethod::Npx;
		}
		else if (Agent.Distribution.HasUvx())
		{
			Method = ERegistryInstallMethod::Uvx;
		}
		else
		{
			OnComplete.ExecuteIfBound(false, TEXT("No distribution available for this platform."));
			return;
		}
	}

	// npx/uvx don't need actual installation — just verify runtime is available
	if (Method == ERegistryInstallMethod::Npx)
	{
		FString NpxPath;
		if (ResolveExecutable(TEXT("npx"), NpxPath) || ResolveExecutableViaLoginShell(TEXT("npx"), NpxPath))
		{
			OnProgress.ExecuteIfBound(TEXT("npx is available — no download needed."));
			OnComplete.ExecuteIfBound(true, TEXT(""));
		}
		else
		{
			OnComplete.ExecuteIfBound(false, TEXT("Node.js/npx not found. Please install Node.js first."));
		}
		return;
	}

	if (Method == ERegistryInstallMethod::Uvx)
	{
		FString UvxPath;
		if (ResolveExecutable(TEXT("uvx"), UvxPath) || ResolveExecutableViaLoginShell(TEXT("uvx"), UvxPath))
		{
			OnProgress.ExecuteIfBound(TEXT("uvx is available — no download needed."));
			OnComplete.ExecuteIfBound(true, TEXT(""));
		}
		else
		{
			OnComplete.ExecuteIfBound(false, TEXT("uv/uvx not found. Please install uv first."));
		}
		return;
	}

	// Binary install — needs download + extract on background thread
	if (Method == ERegistryInstallMethod::Binary)
	{
		if (!Agent.Distribution.HasBinaryForPlatform(PlatformKey))
		{
			OnComplete.ExecuteIfBound(false, TEXT("No binary available for this platform."));
			return;
		}

		// Copy agent data for the background thread, applying archive URL overrides from quirks
		FACPRegistryAgent AgentCopy = Agent;
		const FACPAgentQuirks& Quirks = FACPAgentQuirksMap::GetQuirks(Agent.Id);
		if (!Quirks.BinaryArchiveUrlOverride.IsEmpty())
		{
			for (auto& Pair : AgentCopy.Distribution.BinaryTargets)
			{
				FString OldUrl = Pair.Value.Archive;
				// Extract target triple and extension from original URL filename
				// e.g., "codex-acp-0.10.0-aarch64-apple-darwin.tar.gz" → target="aarch64-apple-darwin", ext="tar.gz"
				int32 LastSlash;
				if (OldUrl.FindLastChar('/', LastSlash))
				{
					FString Filename = OldUrl.Mid(LastSlash + 1);
					FString Ext;
					if (Filename.EndsWith(TEXT(".tar.gz"))) { Ext = TEXT("tar.gz"); Filename.LeftChopInline(7); }
					else if (Filename.EndsWith(TEXT(".zip"))) { Ext = TEXT("zip"); Filename.LeftChopInline(4); }

					if (!Ext.IsEmpty())
					{
						// Find arch in filename segments
						static const TArray<FString> Arches = { TEXT("aarch64"), TEXT("x86_64"), TEXT("i686") };
						TArray<FString> Parts;
						Filename.ParseIntoArray(Parts, TEXT("-"));
						for (int32 i = 0; i < Parts.Num(); ++i)
						{
							if (Arches.Contains(Parts[i]))
							{
								TArray<FString> TargetParts;
								for (int32 j = i; j < Parts.Num(); ++j) TargetParts.Add(Parts[j]);
								FString Target = FString::Join(TargetParts, TEXT("-"));

								FString NewUrl = Quirks.BinaryArchiveUrlOverride;
								NewUrl = NewUrl.Replace(TEXT("{version}"), *AgentCopy.Version);
								NewUrl = NewUrl.Replace(TEXT("{target}"), *Target);
								NewUrl = NewUrl.Replace(TEXT("{ext}"), *Ext);
								Pair.Value.Archive = NewUrl;
								break;
							}
						}
					}
				}
			}
		}
		Async(EAsyncExecution::Thread, [this, AgentCopy, OnProgress, OnComplete, PlatformKey]()
		{
			const FACPRegistryBinaryTarget& Target = AgentCopy.Distribution.BinaryTargets[PlatformKey];
			FString Error;

			auto NotifyProgress = [OnProgress](const FString& Msg)
			{
				AsyncTask(ENamedThreads::GameThread, [OnProgress, Msg]()
				{
					OnProgress.ExecuteIfBound(Msg);
				});
			};

			auto NotifyComplete = [OnComplete](bool bSuccess, const FString& Msg)
			{
				AsyncTask(ENamedThreads::GameThread, [OnComplete, bSuccess, Msg]()
				{
					OnComplete.ExecuteIfBound(bSuccess, Msg);
				});
			};

			if (DownloadAndExtractBinary(Target, AgentCopy.Id, AgentCopy.Version, OnProgress, Error))
			{
				WriteInstallManifest(AgentCopy.Id, AgentCopy.Version, ERegistryInstallMethod::Binary, Target.Cmd);
				NotifyComplete(true, TEXT(""));
			}
			else
			{
				NotifyComplete(false, Error);
			}
		});
	}
}

bool FAgentInstaller::DownloadAndExtractBinary(
	const FACPRegistryBinaryTarget& Target,
	const FString& AgentId,
	const FString& Version,
	FOnInstallProgress OnProgress,
	FString& OutError)
{
	// Download synchronously on background thread using FHttpModule
	const FString ArchiveUrl = Target.Archive;
	if (ArchiveUrl.IsEmpty())
	{
		OutError = TEXT("No archive URL for this platform.");
		return false;
	}

	AsyncTask(ENamedThreads::GameThread, [OnProgress]()
	{
		OnProgress.ExecuteIfBound(TEXT("Downloading agent binary..."));
	});

	// Synchronous download (we're on a background thread)
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(ArchiveUrl);
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("User-Agent"), TEXT("AgentIntegrationKit/1.0"));

	// Use a blocking event to wait for completion
	FEvent* DoneEvent = FPlatformProcess::GetSynchEventFromPool();
	bool bDownloadSuccess = false;
	TArray<uint8> DownloadedData;

	Request->OnProcessRequestComplete().BindLambda(
		[&bDownloadSuccess, &DownloadedData, DoneEvent](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSuccess)
		{
			if (bSuccess && Resp.IsValid() && Resp->GetResponseCode() == 200)
			{
				DownloadedData = Resp->GetContent();
				bDownloadSuccess = true;
			}
			DoneEvent->Trigger();
		});

	Request->ProcessRequest();
	DoneEvent->Wait();
	FPlatformProcess::ReturnSynchEventToPool(DoneEvent);

	if (!bDownloadSuccess || DownloadedData.Num() == 0)
	{
		OutError = FString::Printf(TEXT("Failed to download %s"), *ArchiveUrl);
		return false;
	}

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("AgentInstaller: Downloaded %d bytes from %s"), DownloadedData.Num(), *ArchiveUrl);

	// Write to temp file
	const FString TempDir = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("aik-registry-install"));
	IFileManager::Get().MakeDirectory(*TempDir, true);

	// Determine extension from URL
	FString Extension;
	if (ArchiveUrl.EndsWith(TEXT(".tar.gz")) || ArchiveUrl.EndsWith(TEXT(".tgz")))
	{
		Extension = TEXT(".tar.gz");
	}
	else if (ArchiveUrl.EndsWith(TEXT(".zip")))
	{
		Extension = TEXT(".zip");
	}
	else if (ArchiveUrl.EndsWith(TEXT(".tar.bz2")) || ArchiveUrl.EndsWith(TEXT(".tbz2")))
	{
		Extension = TEXT(".tar.bz2");
	}
	else
	{
		// Assume raw binary
		Extension = TEXT("");
	}

	const FString TempFilePath = FPaths::Combine(TempDir, FString::Printf(TEXT("%s-%s%s"), *AgentId, *Version, *Extension));
	if (!FFileHelper::SaveArrayToFile(DownloadedData, *TempFilePath))
	{
		OutError = TEXT("Failed to write downloaded archive to temp file.");
		return false;
	}

	// Create install directory using URL hash (Zed pattern)
	const FString InstallDir = GetAgentVersionDir(AgentId, ArchiveUrl);
	UE_LOG(LogAgentIntegrationKit, Log, TEXT("AgentInstaller: Extracting to %s (ArchiveUrl=%s)"), *InstallDir, *ArchiveUrl);
	if (InstallDir.IsEmpty())
	{
		OutError = TEXT("Could not determine install directory.");
		IFileManager::Get().Delete(*TempFilePath);
		return false;
	}
	IFileManager::Get().MakeDirectory(*InstallDir, true);

	AsyncTask(ENamedThreads::GameThread, [OnProgress]()
	{
		OnProgress.ExecuteIfBound(TEXT("Extracting..."));
	});

	// Extract
	FString ExtractError;
	if (Extension.IsEmpty())
	{
		// Raw binary — just copy to install dir
		FString DestPath = FPaths::Combine(InstallDir, FPaths::GetCleanFilename(Target.Cmd.IsEmpty() ? AgentId : Target.Cmd));
		if (!IFileManager::Get().Move(*DestPath, *TempFilePath))
		{
			OutError = TEXT("Failed to copy binary to install directory.");
			return false;
		}
		EnsureNativeAdapterExecutable(DestPath);
	}
	else if (!ExtractArchive(TempFilePath, InstallDir, ExtractError))
	{
		OutError = FString::Printf(TEXT("Failed to extract archive: %s"), *ExtractError);
		IFileManager::Get().Delete(*TempFilePath);
		return false;
	}

	// Verify the expected binary actually exists after extraction.
	// On Windows, antivirus (e.g., Windows Defender) may silently quarantine unsigned executables
	// immediately after extraction, causing Expand-Archive to report success but the file to be missing.
	if (!Target.Cmd.IsEmpty())
	{
		FString CmdPath = Target.Cmd;
		if (CmdPath.StartsWith(TEXT("./")))  CmdPath.RightChopInline(2);
		if (CmdPath.StartsWith(TEXT(".\\"))) CmdPath.RightChopInline(2);
		FString FullCmdPath = FPaths::Combine(InstallDir, CmdPath);
		FPaths::NormalizeFilename(FullCmdPath);

		if (!IFileManager::Get().FileExists(*FullCmdPath))
		{
			OutError = FString::Printf(TEXT("Extraction succeeded but '%s' was not found. "
				"This is typically caused by antivirus software (e.g., Windows Defender) quarantining the executable. "
				"Try adding an exclusion for: %s"), *CmdPath, *InstallDir);
			IFileManager::Get().Delete(*TempFilePath);
			return false;
		}

		EnsureNativeAdapterExecutable(FullCmdPath);
	}

	// Clean up temp file
	IFileManager::Get().Delete(*TempFilePath);

	AsyncTask(ENamedThreads::GameThread, [OnProgress]()
	{
		OnProgress.ExecuteIfBound(TEXT("Installation complete."));
	});

	return true;
}

bool FAgentInstaller::ExtractArchive(const FString& ArchivePath, const FString& DestDir, FString& OutError)
{
#if PLATFORM_MAC || PLATFORM_LINUX
	FString StdOut, StdErr;
	int32 ReturnCode = -1;

	if (ArchivePath.EndsWith(TEXT(".zip")))
	{
		FPlatformProcess::ExecProcess(
			TEXT("/usr/bin/unzip"),
			*FString::Printf(TEXT("-o \"%s\" -d \"%s\""), *ArchivePath, *DestDir),
			&ReturnCode, &StdOut, &StdErr);
	}
	else if (ArchivePath.EndsWith(TEXT(".tar.gz")) || ArchivePath.EndsWith(TEXT(".tgz")))
	{
		FPlatformProcess::ExecProcess(
			TEXT("/usr/bin/tar"),
			*FString::Printf(TEXT("xzf \"%s\" -C \"%s\""), *ArchivePath, *DestDir),
			&ReturnCode, &StdOut, &StdErr);
	}
	else if (ArchivePath.EndsWith(TEXT(".tar.bz2")) || ArchivePath.EndsWith(TEXT(".tbz2")))
	{
		FPlatformProcess::ExecProcess(
			TEXT("/usr/bin/tar"),
			*FString::Printf(TEXT("xjf \"%s\" -C \"%s\""), *ArchivePath, *DestDir),
			&ReturnCode, &StdOut, &StdErr);
	}
	else
	{
		OutError = FString::Printf(TEXT("Unsupported archive format: %s"), *FPaths::GetExtension(ArchivePath));
		return false;
	}

	if (ReturnCode != 0)
	{
		OutError = StdErr.IsEmpty() ? FString::Printf(TEXT("Extraction failed (exit code %d)"), ReturnCode) : StdErr;
		return false;
	}
	return true;

#elif PLATFORM_WINDOWS
	if (ArchivePath.EndsWith(TEXT(".zip")))
	{
		// Use PowerShell to extract zip on Windows
		FString StdOut, StdErr;
		int32 ReturnCode = -1;
		FString Cmd = FString::Printf(
			TEXT("-NoProfile -Command \"Expand-Archive -Path '%s' -DestinationPath '%s' -Force\""),
			*ArchivePath, *DestDir);
		FPlatformProcess::ExecProcess(TEXT("powershell.exe"), *Cmd, &ReturnCode, &StdOut, &StdErr);

		if (ReturnCode != 0)
		{
			OutError = StdErr.IsEmpty() ? TEXT("PowerShell Expand-Archive failed") : StdErr;
			return false;
		}
		return true;
	}
	else if (ArchivePath.EndsWith(TEXT(".tar.gz")) || ArchivePath.EndsWith(TEXT(".tgz")))
	{
		// Windows 10+ has built-in tar
		FString StdOut, StdErr;
		int32 ReturnCode = -1;
		FPlatformProcess::ExecProcess(
			TEXT("tar.exe"),
			*FString::Printf(TEXT("xzf \"%s\" -C \"%s\""), *ArchivePath, *DestDir),
			&ReturnCode, &StdOut, &StdErr);
		if (ReturnCode != 0)
		{
			OutError = StdErr.IsEmpty() ? TEXT("tar extraction failed") : StdErr;
			return false;
		}
		return true;
	}

	OutError = FString::Printf(TEXT("Unsupported archive format on Windows: %s"), *FPaths::GetExtension(ArchivePath));
	return false;
#else
	OutError = TEXT("Archive extraction not supported on this platform.");
	return false;
#endif
}

bool FAgentInstaller::WriteInstallManifest(const FString& AgentId, const FString& Version, ERegistryInstallMethod Method, const FString& Cmd)
{
	// Write manifest to the URL-hash directory if available (where DownloadAndExtractBinary extracts to)
	// Also write to legacy version directory for backward compat
	FString MethodStr;
	switch (Method)
	{
	case ERegistryInstallMethod::Binary: MethodStr = TEXT("binary"); break;
	case ERegistryInstallMethod::Npx:    MethodStr = TEXT("npx"); break;
	case ERegistryInstallMethod::Uvx:    MethodStr = TEXT("uvx"); break;
	default:                             MethodStr = TEXT("auto"); break;
	}

	FString ManifestJson = FString::Printf(
		TEXT("{\"agentId\":\"%s\",\"version\":\"%s\",\"method\":\"%s\",\"cmd\":\"%s\",\"installedAt\":\"%s\"}"),
		*AgentId, *Version, *MethodStr, *Cmd, *FDateTime::Now().ToIso8601());

	// Try to find the URL-hash directory that was just created by DownloadAndExtractBinary
	FString AgentsDir = GetManagedAgentsDir();
	FString AgentDir = FPaths::Combine(AgentsDir, AgentId);
	TArray<FString> SubDirs;
	IFileManager::Get().FindFiles(SubDirs, *FPaths::Combine(AgentDir, TEXT("v_*")), false, true);
	for (const FString& SubDir : SubDirs)
	{
		FString ManifestPath = FPaths::Combine(AgentDir, SubDir, TEXT("manifest.json"));
		FFileHelper::SaveStringToFile(ManifestJson, *ManifestPath);
	}

	// Also write to legacy version dir
	FString InstallDir = GetAgentInstallDir(AgentId, Version);
	if (!InstallDir.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*InstallDir, true);
		FString ManifestPath = FPaths::Combine(InstallDir, TEXT("manifest.json"));
		return FFileHelper::SaveStringToFile(ManifestJson, *ManifestPath);
	}

	return SubDirs.Num() > 0;
}

bool FAgentInstaller::ReadInstallManifest(const FString& AgentId, FString& OutVersion, FString& OutCmd) const
{
	// Scan the agent directory for installed versions
	FString AgentsDir = GetManagedAgentsDir();
	if (AgentsDir.IsEmpty())
	{
		return false;
	}

	FString AgentDir = FPaths::Combine(AgentsDir, AgentId);
	if (!IFileManager::Get().DirectoryExists(*AgentDir))
	{
		return false;
	}

	// Find the newest version directory with a manifest
	TArray<FString> VersionDirs;
	IFileManager::Get().FindFiles(VersionDirs, *FPaths::Combine(AgentDir, TEXT("*")), false, true);

	FString BestVersion;
	FString BestCmd;

	for (const FString& VersionDir : VersionDirs)
	{
		FString ManifestPath = FPaths::Combine(AgentDir, VersionDir, TEXT("manifest.json"));
		FString ManifestJson;
		if (!FFileHelper::LoadFileToString(ManifestJson, *ManifestPath))
		{
			continue;
		}

		TSharedPtr<FJsonObject> ManifestObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ManifestJson);
		if (!FJsonSerializer::Deserialize(Reader, ManifestObj) || !ManifestObj.IsValid())
		{
			continue;
		}

		FString Version, Cmd;
		ManifestObj->TryGetStringField(TEXT("version"), Version);
		ManifestObj->TryGetStringField(TEXT("cmd"), Cmd);

		// Take the latest version found
		if (BestVersion.IsEmpty() || Version > BestVersion)
		{
			BestVersion = Version;
			BestCmd = Cmd;
		}
	}

	if (!BestVersion.IsEmpty())
	{
		OutVersion = BestVersion;
		OutCmd = BestCmd;
		return true;
	}

	return false;
}

bool FAgentInstaller::UninstallRegistryAgent(const FString& AgentId)
{
	FString AgentsDir = GetManagedAgentsDir();
	if (AgentsDir.IsEmpty())
	{
		return false;
	}

	FString AgentDir = FPaths::Combine(AgentsDir, AgentId);
	if (!IFileManager::Get().DirectoryExists(*AgentDir))
	{
		return false;
	}

	bool bDeleted = IFileManager::Get().DeleteDirectory(*AgentDir, false, true);
	if (bDeleted)
	{
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("AgentInstaller: Uninstalled registry agent: %s"), *AgentId);
	}
	return bDeleted;
}
