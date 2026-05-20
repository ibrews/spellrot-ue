// Copyright 2026 Betide Studio. All Rights Reserved.

#include "ACPRegistryClient.h"
#include "AgentIntegrationKitModule.h"
#include "ACPAgentManager.h"
#include "ACPSettings.h"
#include "AgentInstaller.h"
#include "HttpModule.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformMisc.h"
#include "HAL/FileManager.h"

static const TCHAR* RegistryCDNUrl = TEXT("https://cdn.agentclientprotocol.com/registry/v1/latest/registry.json");

// Default refresh interval: 24 hours
static constexpr float RegistryRefreshIntervalSeconds = 24.0f * 60.0f * 60.0f;

// ============================================================================
// Singleton
// ============================================================================

FACPRegistryClient& FACPRegistryClient::Get()
{
	static FACPRegistryClient Instance;
	return Instance;
}

// ============================================================================
// Lifecycle
// ============================================================================

void FACPRegistryClient::Initialize()
{
	if (bInitialized)
	{
		return;
	}
	bInitialized = true;

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPRegistry: Initializing..."));

	// Load all WebUI preferences from ~/.agentintegrationkit/preferences.json
	// This overrides in-memory values with the persisted ones (installed agents,
	// model selections, onboarding state, etc.)
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->LoadPreferences();
	}

	// Try loading from local cache first for instant availability
	if (LoadFromCache())
	{
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPRegistry: Loaded %d agents from cache (%d for this platform)"),
			AllAgents.Num(), FilteredAgents.Num());

		// Re-initialize agent manager now that registry data is available.
		// The manager's constructor may have run before the registry was loaded.
		FACPAgentManager::Get().InitializeDefaultAgents();
	}

	// Start a CDN fetch in the background (will update cache on success)
	FetchFromCDN();

	// Set up periodic refresh
	RefreshTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FACPRegistryClient::OnRefreshTick),
		RegistryRefreshIntervalSeconds);
}

void FACPRegistryClient::Shutdown()
{
	if (!bInitialized)
	{
		return;
	}
	bInitialized = false;

	if (RefreshTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RefreshTickerHandle);
		RefreshTickerHandle.Reset();
	}

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPRegistry: Shut down"));
}

void FACPRegistryClient::RefreshAsync()
{
	FetchFromCDN();
}

bool FACPRegistryClient::OnRefreshTick(float DeltaTime)
{
	FetchFromCDN();
	return true; // Keep ticking
}

// ============================================================================
// Platform Detection
// ============================================================================

FString FACPRegistryClient::GetCurrentPlatformKey()
{
#if PLATFORM_MAC
	#if PLATFORM_CPU_ARM_FAMILY
		return TEXT("darwin-aarch64");
	#else
		return TEXT("darwin-x86_64");
	#endif
#elif PLATFORM_WINDOWS
	#if PLATFORM_CPU_ARM_FAMILY
		return TEXT("windows-aarch64");
	#else
		return TEXT("windows-x86_64");
	#endif
#elif PLATFORM_LINUX
	#if PLATFORM_CPU_ARM_FAMILY
		return TEXT("linux-aarch64");
	#else
		return TEXT("linux-x86_64");
	#endif
#else
	return TEXT("unknown");
#endif
}

// ============================================================================
// CDN Fetch
// ============================================================================

void FACPRegistryClient::FetchFromCDN()
{
	if (bFetchInFlight)
	{
		return;
	}
	bFetchInFlight = true;

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPRegistry: Fetching from CDN..."));

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(RegistryCDNUrl);
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->SetHeader(TEXT("User-Agent"), TEXT("AgentIntegrationKit/1.0"));
	Request->OnProcessRequestComplete().BindRaw(this, &FACPRegistryClient::HandleFetchResponse);
	Request->ProcessRequest();
}

void FACPRegistryClient::HandleFetchResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess)
{
	bFetchInFlight = false;

	if (!bSuccess || !Response.IsValid())
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPRegistry: CDN fetch failed (network error)"));
		OnFetchFailed.Broadcast(TEXT("Network error — using cached registry"));
		return;
	}

	const int32 ResponseCode = Response->GetResponseCode();
	if (ResponseCode != 200)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPRegistry: CDN returned HTTP %d"), ResponseCode);
		OnFetchFailed.Broadcast(FString::Printf(TEXT("CDN returned HTTP %d"), ResponseCode));
		return;
	}

	const FString JsonString = Response->GetContentAsString();
	if (JsonString.IsEmpty())
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPRegistry: CDN returned empty response"));
		OnFetchFailed.Broadcast(TEXT("Empty response from CDN"));
		return;
	}

	if (ParseRegistryJson(JsonString))
	{
		SaveToCache(JsonString);
		LastFetchTime = FDateTime::Now();
		bIsLoaded = true;

		UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPRegistry: Fetched %d agents from CDN (v%s), %d for this platform"),
			AllAgents.Num(), *RegistryVersion, FilteredAgents.Num());

		// Re-initialize agent manager with fresh registry data
		FACPAgentManager::Get().InitializeDefaultAgents();

		// Check for agent updates
		CachedUpdates = CheckForAgentUpdates();
		if (CachedUpdates.Num() > 0)
		{
			UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPRegistry: %d agent update(s) available:"), CachedUpdates.Num());
			for (const FAgentUpdateInfo& Update : CachedUpdates)
			{
				UE_LOG(LogAgentIntegrationKit, Log, TEXT("  - %s: %s → %s"), *Update.AgentName, *Update.InstalledVersion, *Update.LatestVersion);
			}
		}

		// Fetch SVG icons for all agents
		FetchAllIcons();

		OnRegistryRefreshed.Broadcast();
	}
	else
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPRegistry: Failed to parse CDN response"));
		OnFetchFailed.Broadcast(TEXT("Failed to parse registry JSON"));
	}
}

// ============================================================================
// Agent Update Detection
// ============================================================================

TArray<FAgentUpdateInfo> FACPRegistryClient::CheckForAgentUpdates() const
{
	TArray<FAgentUpdateInfo> Updates;

	const UACPSettings* Settings = UACPSettings::Get();
	if (!Settings || Settings->InstalledAgentIds.Num() == 0)
	{
		return Updates;
	}

	const FString PlatformKey = GetCurrentPlatformKey();

	for (const FString& AgentId : Settings->InstalledAgentIds)
	{
		const FACPRegistryAgent* RegistryAgent = FindAgent(AgentId);
		if (!RegistryAgent)
		{
			continue;
		}

		// For binary agents: check if the managed directory's archive URL hash
		// differs from the current registry archive URL (means registry has a newer build)
		if (RegistryAgent->Distribution.HasBinaryForPlatform(PlatformKey))
		{
			const FACPRegistryBinaryTarget& Target = RegistryAgent->Distribution.BinaryTargets[PlatformKey];
			if (!Target.Archive.IsEmpty())
			{
				// Check if we have ANY version installed
				bool bHasInstalled = FAgentInstaller::IsAgentBinaryExtracted(AgentId, Target.Archive, Target.Cmd);

				if (!bHasInstalled)
				{
					// Check if there's an OLD version installed (different archive URL)
					FString InstalledVersion, InstalledCmd;
					if (FAgentInstaller::Get().ReadInstallManifest(AgentId, InstalledVersion, InstalledCmd))
					{
						// Old version exists but doesn't match current registry archive
						if (InstalledVersion != RegistryAgent->Version)
						{
							FAgentUpdateInfo Info;
							Info.AgentId = AgentId;
							Info.AgentName = RegistryAgent->Name;
							Info.InstalledVersion = InstalledVersion;
							Info.LatestVersion = RegistryAgent->Version;
							Info.LatestArchiveUrl = Target.Archive;
							Updates.Add(MoveTemp(Info));
						}
					}
				}
			}
		}

		// For npx agents: compare registry version vs what we last ran.
		// npx agents auto-update when using @latest, but pinned versions
		// (e.g., @0.22.2) need manual update in the registry.
		if (RegistryAgent->Distribution.HasNpx())
		{
			// npx agents with pinned versions: the version is in the package spec
			// e.g., "@zed-industries/claude-agent-acp@0.22.2"
			// We can't easily tell what version the user last ran, so we compare
			// the registry version against our preferences.json stored version.
			// For now, npx agents are considered always up-to-date since npx
			// downloads the exact pinned version from the registry.
			// TODO: Track last-used npx package version in preferences.json
		}
	}

	return Updates;
}

// ============================================================================
// JSON Parsing
// ============================================================================

bool FACPRegistryClient::ParseRegistryJson(const FString& JsonString)
{
	TSharedPtr<FJsonObject> RootObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

	if (!FJsonSerializer::Deserialize(Reader, RootObj) || !RootObj.IsValid())
	{
		return false;
	}

	// Read registry version
	RootObj->TryGetStringField(TEXT("version"), RegistryVersion);

	// Parse agents array
	const TArray<TSharedPtr<FJsonValue>>* AgentsArray = nullptr;
	if (!RootObj->TryGetArrayField(TEXT("agents"), AgentsArray) || !AgentsArray)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPRegistry: No 'agents' array in registry JSON"));
		return false;
	}

	TArray<FACPRegistryAgent> NewAgents;
	NewAgents.Reserve(AgentsArray->Num());

	for (const TSharedPtr<FJsonValue>& AgentVal : *AgentsArray)
	{
		TSharedPtr<FJsonObject> AgentObj = AgentVal->AsObject();
		if (!AgentObj.IsValid())
		{
			continue;
		}

		FACPRegistryAgent Agent;
		if (ParseAgentObject(AgentObj, Agent))
		{
			NewAgents.Add(MoveTemp(Agent));
		}
	}

	AllAgents = MoveTemp(NewAgents);
	FilterAgentsForPlatform();
	return true;
}

bool FACPRegistryClient::ParseAgentObject(const TSharedPtr<FJsonObject>& AgentObj, FACPRegistryAgent& OutAgent)
{
	// Required fields
	if (!AgentObj->TryGetStringField(TEXT("id"), OutAgent.Id) || OutAgent.Id.IsEmpty())
	{
		return false;
	}

	AgentObj->TryGetStringField(TEXT("name"), OutAgent.Name);
	AgentObj->TryGetStringField(TEXT("version"), OutAgent.Version);
	AgentObj->TryGetStringField(TEXT("description"), OutAgent.Description);
	AgentObj->TryGetStringField(TEXT("repository"), OutAgent.Repository);
	AgentObj->TryGetStringField(TEXT("website"), OutAgent.Website);
	AgentObj->TryGetStringField(TEXT("license"), OutAgent.License);
	AgentObj->TryGetStringField(TEXT("icon"), OutAgent.IconUrl);

	// Authors array
	const TArray<TSharedPtr<FJsonValue>>* AuthorsArray = nullptr;
	if (AgentObj->TryGetArrayField(TEXT("authors"), AuthorsArray) && AuthorsArray)
	{
		for (const TSharedPtr<FJsonValue>& AuthorVal : *AuthorsArray)
		{
			FString Author = AuthorVal->AsString();
			if (!Author.IsEmpty())
			{
				OutAgent.Authors.Add(MoveTemp(Author));
			}
		}
	}

	// Distribution
	const TSharedPtr<FJsonObject>* DistObj = nullptr;
	if (AgentObj->TryGetObjectField(TEXT("distribution"), DistObj) && DistObj && (*DistObj).IsValid())
	{
		// Binary targets
		const TSharedPtr<FJsonObject>* BinaryObj = nullptr;
		if ((*DistObj)->TryGetObjectField(TEXT("binary"), BinaryObj) && BinaryObj && (*BinaryObj).IsValid())
		{
			for (const auto& Pair : (*BinaryObj)->Values)
			{
				TSharedPtr<FJsonObject> TargetObj = Pair.Value->AsObject();
				if (!TargetObj.IsValid())
				{
					continue;
				}

				FACPRegistryBinaryTarget Target;
				TargetObj->TryGetStringField(TEXT("archive"), Target.Archive);
				TargetObj->TryGetStringField(TEXT("cmd"), Target.Cmd);

				// Args array
				const TArray<TSharedPtr<FJsonValue>>* ArgsArray = nullptr;
				if (TargetObj->TryGetArrayField(TEXT("args"), ArgsArray) && ArgsArray)
				{
					for (const TSharedPtr<FJsonValue>& ArgVal : *ArgsArray)
					{
						FString Arg = ArgVal->AsString();
						if (!Arg.IsEmpty())
						{
							Target.Args.Add(MoveTemp(Arg));
						}
					}
				}

				// Env map
				const TSharedPtr<FJsonObject>* EnvObj = nullptr;
				if (TargetObj->TryGetObjectField(TEXT("env"), EnvObj) && EnvObj && (*EnvObj).IsValid())
				{
					for (const auto& EnvPair : (*EnvObj)->Values)
					{
						FString EnvVal = EnvPair.Value->AsString();
						Target.Env.Add(EnvPair.Key, MoveTemp(EnvVal));
					}
				}

				if (!Target.Archive.IsEmpty() || !Target.Cmd.IsEmpty())
				{
					OutAgent.Distribution.BinaryTargets.Add(Pair.Key, MoveTemp(Target));
				}
			}
		}

		// npx distribution
		const TSharedPtr<FJsonObject>* NpxObj = nullptr;
		if ((*DistObj)->TryGetObjectField(TEXT("npx"), NpxObj) && NpxObj && (*NpxObj).IsValid())
		{
			(*NpxObj)->TryGetStringField(TEXT("package"), OutAgent.Distribution.NpxPackage);

			const TArray<TSharedPtr<FJsonValue>>* NpxArgsArr = nullptr;
			if ((*NpxObj)->TryGetArrayField(TEXT("args"), NpxArgsArr) && NpxArgsArr)
			{
				for (const TSharedPtr<FJsonValue>& ArgVal : *NpxArgsArr)
				{
					FString Arg = ArgVal->AsString();
					if (!Arg.IsEmpty())
					{
						OutAgent.Distribution.NpxArgs.Add(MoveTemp(Arg));
					}
				}
			}
		}

		// uvx distribution
		const TSharedPtr<FJsonObject>* UvxObj = nullptr;
		if ((*DistObj)->TryGetObjectField(TEXT("uvx"), UvxObj) && UvxObj && (*UvxObj).IsValid())
		{
			(*UvxObj)->TryGetStringField(TEXT("package"), OutAgent.Distribution.UvxPackage);

			const TArray<TSharedPtr<FJsonValue>>* UvxArgsArr = nullptr;
			if ((*UvxObj)->TryGetArrayField(TEXT("args"), UvxArgsArr) && UvxArgsArr)
			{
				for (const TSharedPtr<FJsonValue>& ArgVal : *UvxArgsArr)
				{
					FString Arg = ArgVal->AsString();
					if (!Arg.IsEmpty())
					{
						OutAgent.Distribution.UvxArgs.Add(MoveTemp(Arg));
					}
				}
			}
		}
	}

	// Must have at least some distribution method
	return OutAgent.Distribution.HasAnyDistribution();
}

void FACPRegistryClient::FilterAgentsForPlatform()
{
	const FString PlatformKey = GetCurrentPlatformKey();

	FilteredAgents.Reset();
	FilteredAgents.Reserve(AllAgents.Num());

	for (const FACPRegistryAgent& Agent : AllAgents)
	{
		// Agent is available if it has a binary for this platform, or npx, or uvx
		if (Agent.Distribution.HasBinaryForPlatform(PlatformKey) ||
			Agent.Distribution.HasNpx() ||
			Agent.Distribution.HasUvx())
		{
			FilteredAgents.Add(Agent);
		}
	}
}

// ============================================================================
// Lookup
// ============================================================================

const FACPRegistryAgent* FACPRegistryClient::FindAgent(const FString& RegistryId) const
{
	for (const FACPRegistryAgent& Agent : FilteredAgents)
	{
		if (Agent.Id == RegistryId)
		{
			return &Agent;
		}
	}
	return nullptr;
}

// ============================================================================
// Icon Fetching
// ============================================================================

void FACPRegistryClient::FetchAllIcons()
{
	const FString IconCacheDir = GetIconCacheDir();

	for (FACPRegistryAgent& Agent : AllAgents)
	{
		if (Agent.IconUrl.IsEmpty() || !Agent.IconUrl.StartsWith(TEXT("http")))
		{
			continue;
		}

		// Try loading from icon cache first
		if (!IconCacheDir.IsEmpty())
		{
			FString CachedIconPath = FPaths::Combine(IconCacheDir, Agent.Id + TEXT(".svg"));
			FString CachedSvg;
			if (FFileHelper::LoadFileToString(CachedSvg, *CachedIconPath) && CachedSvg.Contains(TEXT("<svg")))
			{
				Agent.IconSvgMarkup = CachedSvg;
				continue;
			}
		}

		// Fetch from CDN
		FString AgentId = Agent.Id;
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Agent.IconUrl);
		Request->SetVerb(TEXT("GET"));
		Request->SetHeader(TEXT("User-Agent"), TEXT("AgentIntegrationKit/1.0"));
		Request->OnProcessRequestComplete().BindRaw(this, &FACPRegistryClient::HandleIconResponse, AgentId);
		Request->ProcessRequest();
	}
}

void FACPRegistryClient::HandleIconResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess, FString AgentId)
{
	if (!bSuccess || !Response.IsValid() || Response->GetResponseCode() != 200)
	{
		return;
	}

	FString SvgContent = Response->GetContentAsString();
	if (!SvgContent.Contains(TEXT("<svg")))
	{
		return;
	}

	// Store in both AllAgents and FilteredAgents
	for (FACPRegistryAgent& Agent : AllAgents)
	{
		if (Agent.Id == AgentId)
		{
			Agent.IconSvgMarkup = SvgContent;
			break;
		}
	}
	for (FACPRegistryAgent& Agent : FilteredAgents)
	{
		if (Agent.Id == AgentId)
		{
			Agent.IconSvgMarkup = SvgContent;
			break;
		}
	}

	// Cache to disk
	const FString IconCacheDir = GetIconCacheDir();
	if (!IconCacheDir.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*IconCacheDir, true);
		FString CachePath = FPaths::Combine(IconCacheDir, AgentId + TEXT(".svg"));
		FFileHelper::SaveStringToFile(SvgContent, *CachePath);
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPRegistry: Fetched icon for %s (%d bytes)"), *AgentId, SvgContent.Len());
}

FString FACPRegistryClient::GetIconCacheDir() const
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
	return FPaths::Combine(HomeDir, TEXT(".agentintegrationkit"), TEXT("icon_cache"));
}

// ============================================================================
// Local Cache
// ============================================================================

FString FACPRegistryClient::GetCacheFilePath() const
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
	return FPaths::Combine(HomeDir, TEXT(".agentintegrationkit"), TEXT("registry_cache.json"));
}

bool FACPRegistryClient::LoadFromCache()
{
	const FString CachePath = GetCacheFilePath();
	if (CachePath.IsEmpty())
	{
		return false;
	}

	FString CachedJson;
	if (!FFileHelper::LoadFileToString(CachedJson, *CachePath))
	{
		return false;
	}

	if (!ParseRegistryJson(CachedJson))
	{
		return false;
	}

	// Load cached icons
	const FString IconCacheDir = GetIconCacheDir();
	if (!IconCacheDir.IsEmpty())
	{
		for (FACPRegistryAgent& Agent : AllAgents)
		{
			FString CachedIconPath = FPaths::Combine(IconCacheDir, Agent.Id + TEXT(".svg"));
			FString CachedSvg;
			if (FFileHelper::LoadFileToString(CachedSvg, *CachedIconPath) && CachedSvg.Contains(TEXT("<svg")))
			{
				Agent.IconSvgMarkup = CachedSvg;
			}
		}
		// Update filtered agents too
		FilterAgentsForPlatform();
	}

	// Read file timestamp as last fetch time
	const FDateTime FileTime = IFileManager::Get().GetTimeStamp(*CachePath);
	if (FileTime != FDateTime::MinValue())
	{
		LastFetchTime = FileTime;
	}

	bIsLoaded = true;
	return true;
}

void FACPRegistryClient::SaveToCache(const FString& JsonString)
{
	const FString CachePath = GetCacheFilePath();
	if (CachePath.IsEmpty())
	{
		return;
	}

	// Ensure directory exists
	const FString CacheDir = FPaths::GetPath(CachePath);
	IFileManager::Get().MakeDirectory(*CacheDir, true);

	if (FFileHelper::SaveStringToFile(JsonString, *CachePath))
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPRegistry: Saved cache to %s"), *CachePath);
	}
	else
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPRegistry: Failed to save cache to %s"), *CachePath);
	}
}
