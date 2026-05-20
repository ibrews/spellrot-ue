// Copyright 2026 Betide Studio. All Rights Reserved.

#include "ACPAgentManager.h"
#include "AgentIntegrationKitModule.h"
#include "AgentInstaller.h"
#include "ACPCodexHistoryReader.h"
#include "ACPSettings.h"
#include "ACPAgentQuirks.h"
#include "ACPRegistryClient.h"
#include "ACPRegistryConfigGenerator.h"
#include "ACPSessionManager.h"
#include "ChatGatewayClient.h"
#include "MCPServer.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"

namespace
{
static bool IsGeminiCliAgent(const FString& AgentName)
{
	return AgentName == TEXT("Gemini CLI");
}

static const TArray<FString>& GetKnownGeminiCliModelIds()
{
	static const TArray<FString> KnownModels = {
		TEXT("auto"),
		TEXT("pro"),
		TEXT("flash"),
		TEXT("flash-lite"),
		TEXT("gemini-2.5-pro"),
		TEXT("gemini-2.5-flash"),
		TEXT("gemini-2.5-flash-lite"),
		TEXT("gemini-3-pro-preview"),
		TEXT("gemini-3-flash-preview")
	};
	return KnownModels;
}

static bool IsKnownGeminiCliModelId(const FString& ModelId)
{
	if (ModelId.IsEmpty())
	{
		return false;
	}

	for (const FString& KnownId : GetKnownGeminiCliModelIds())
	{
		if (KnownId == ModelId)
		{
			return true;
		}
	}
	return false;
}

/**
 * Write a per-process MCP config JSON file for agents that need MCP injected
 * via CLI flag (Copilot) or env var (Gemini). Returns the temp file path,
 * or empty string on failure.
 */
static FString WriteMcpConfigTempFile(const FString& AgentId, const FString& ConfigTemplate, int32 MCPPort)
{
	FString McpUrl = FString::Printf(TEXT("http://127.0.0.1:%d/mcp"), MCPPort);
	FString ConfigJson = ConfigTemplate;
	ConfigJson.ReplaceInline(TEXT("{url}"), *McpUrl);

	const FString TempDir = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("aik-mcp-config"));
	IFileManager::Get().MakeDirectory(*TempDir, true);

	const FString FilePath = FPaths::Combine(
		TempDir,
		FString::Printf(TEXT("mcp-%s-%s.json"), *AgentId, *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

	if (FFileHelper::SaveStringToFile(ConfigJson, *FilePath))
	{
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPAgentManager: Wrote MCP config for '%s': %s"), *AgentId, *FilePath);
		return FilePath;
	}

	UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAgentManager: Failed to write MCP config for '%s'"), *AgentId);
	return FString();
}

static FString ResolveGeminiCliLaunchModelId(const FString& RequestedModelId)
{
	// On some Gemini CLI/auth combinations, "auto" resolves to a model that the
	// account cannot access and prompt calls fail with "Requested entity was not found.".
	// Use "flash" as a safe ACP launch default to keep new chats functional.
	if (RequestedModelId == TEXT("auto"))
	{
		return TEXT("flash");
	}

	return IsKnownGeminiCliModelId(RequestedModelId)
		? RequestedModelId
		: TEXT("flash");
}

static TArray<FString> StripGeminiCliModelArgs(const TArray<FString>& Arguments)
{
	TArray<FString> Result;
	Result.Reserve(Arguments.Num());

	for (int32 Index = 0; Index < Arguments.Num(); ++Index)
	{
		const FString& Arg = Arguments[Index];
		if (Arg == TEXT("--model"))
		{
			// Skip this arg + explicit value arg, if present.
			++Index;
			continue;
		}
		if (Arg.StartsWith(TEXT("--model=")))
		{
			continue;
		}
		Result.Add(Arg);
	}

	return Result;
}

static TArray<FACPModelInfo> BuildGeminiCliFallbackModels()
{
	TArray<FACPModelInfo> Models;
	Models.Reserve(9);

	auto AddModel = [&Models](const TCHAR* Id, const TCHAR* Name, const TCHAR* Description)
	{
		FACPModelInfo Model;
		Model.ModelId = Id;
		Model.Name = Name;
		Model.Description = Description;
		Models.Add(MoveTemp(Model));
	};

	// Based on the current Gemini CLI model-selection docs.
	AddModel(TEXT("auto"), TEXT("Auto"), TEXT("ACP compatibility note: currently falls back to Flash at launch to avoid model-not-found errors."));
	AddModel(TEXT("pro"), TEXT("Pro"), TEXT("Higher-capability reasoning alias. Launch-time option."));
	AddModel(TEXT("flash"), TEXT("Flash"), TEXT("Fast, balanced alias. Launch-time option."));
	AddModel(TEXT("flash-lite"), TEXT("Flash Lite"), TEXT("Lowest-latency alias. Launch-time option."));
	AddModel(TEXT("gemini-2.5-pro"), TEXT("Gemini 2.5 Pro"), TEXT("Explicit 2.5 Pro model. Launch-time option."));
	AddModel(TEXT("gemini-2.5-flash"), TEXT("Gemini 2.5 Flash"), TEXT("Explicit 2.5 Flash model. Launch-time option."));
	AddModel(TEXT("gemini-2.5-flash-lite"), TEXT("Gemini 2.5 Flash Lite"), TEXT("Explicit 2.5 Flash Lite model. Launch-time option."));
	AddModel(TEXT("gemini-3-pro-preview"), TEXT("Gemini 3 Pro Preview"), TEXT("Preview model. Launch-time option; availability depends on account/features."));
	AddModel(TEXT("gemini-3-flash-preview"), TEXT("Gemini 3 Flash Preview"), TEXT("Preview model. Launch-time option; availability depends on account/features."));

	return Models;
}

static FACPSessionModelState BuildGeminiCliFallbackModelState(const FString& PreferredModelId)
{
	FACPSessionModelState State;
	State.AvailableModels = BuildGeminiCliFallbackModels();

	if (!PreferredModelId.IsEmpty() && !IsKnownGeminiCliModelId(PreferredModelId))
	{
		FACPModelInfo Custom;
		Custom.ModelId = PreferredModelId;
		Custom.Name = PreferredModelId;
		Custom.Description = TEXT("Saved custom model ID.");
		State.AvailableModels.Insert(MoveTemp(Custom), 0);
		State.CurrentModelId = PreferredModelId;
		return State;
	}

	State.CurrentModelId = ResolveGeminiCliLaunchModelId(PreferredModelId);
	return State;
}

static FString QuoteForShellDouble(const FString& Value)
{
	FString Escaped = Value;
	Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
	return FString::Printf(TEXT("\"%s\""), *Escaped);
}

static FString BuildQuotedCommandLine(const FString& Command, const TArray<FString>& Args)
{
	FString Result = QuoteForShellDouble(Command);
	for (const FString& Arg : Args)
	{
		Result += TEXT(" ");
		Result += QuoteForShellDouble(Arg);
	}
	return Result;
}

static FString MakeSafeFileStem_AgentMgr(const FString& Value)
{
	FString Result;
	Result.Reserve(Value.Len());
	for (const TCHAR Ch : Value)
	{
		if ((Ch >= TEXT('a') && Ch <= TEXT('z')) ||
			(Ch >= TEXT('A') && Ch <= TEXT('Z')) ||
			(Ch >= TEXT('0') && Ch <= TEXT('9')))
		{
			Result.AppendChar(Ch);
		}
		else
		{
			Result.AppendChar(TEXT('-'));
		}
	}
	Result.TrimStartAndEndInline();
	if (Result.IsEmpty())
	{
		Result = TEXT("agent");
	}
	return Result;
}
}

FACPAgentManager& FACPAgentManager::Get()
{
	static FACPAgentManager Instance;
	return Instance;
}

FACPAgentManager::FACPAgentManager()
{
	InitializeDefaultAgents();
}

FACPAgentManager::~FACPAgentManager()
{
	DisconnectAll();
}

void FACPAgentManager::InitializeDefaultAgents()
{
	// Clear existing configs to avoid duplicates on re-initialization
	{
		FScopeLock Lock(&ConfigLock);
		AgentConfigs.Empty();
	}

	// Built-in agents (OpenRouter) — not ACP subprocess, always present
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		TArray<FACPAgentConfig> BuiltInConfigs = Settings->GetAgentConfigs();
		for (const FACPAgentConfig& Config : BuiltInConfigs)
		{
			if (Config.bIsBuiltIn)
			{
				AddAgentConfig(Config);
			}
		}
	}

	// Registry-installed agents — only agents user explicitly installed from the ACP registry
	TArray<FACPAgentConfig> RegistryConfigs = FACPRegistryConfigGenerator::GenerateAllConfigs();
	for (const FACPAgentConfig& Config : RegistryConfigs)
	{
		AddAgentConfig(Config);
	}

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPAgentManager: Loaded %d registry agents"), RegistryConfigs.Num());

	// Also add custom agents from settings (user-defined, not from registry)
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		for (const FACPAgentSettingsEntry& Entry : Settings->CustomAgents)
		{
			if (Entry.AgentName.IsEmpty() || Entry.ExecutablePath.FilePath.IsEmpty())
			{
				continue;
			}
			// Don't overwrite a registry agent with the same name
			if (GetAgentConfig(Entry.AgentName) != nullptr)
			{
				continue;
			}
			FACPAgentConfig Config;
			Config.AgentName = Entry.AgentName;
			Config.ExecutablePath = Entry.ExecutablePath.FilePath;
			Config.WorkingDirectory = FPaths::ProjectDir();
			Config.Status = EACPAgentStatus::Unknown;
			AddAgentConfig(Config);
		}
	}
}

void FACPAgentManager::AddAgentConfig(const FACPAgentConfig& Config)
{
	FScopeLock Lock(&ConfigLock);
	AgentConfigs.Add(Config.AgentName, Config);
}

void FACPAgentManager::RemoveAgentConfig(const FString& AgentName)
{
	// Disconnect first if connected
	DisconnectFromAgent(AgentName);

	FScopeLock Lock(&ConfigLock);
	AgentConfigs.Remove(AgentName);
}

TArray<FACPAgentConfig> FACPAgentManager::GetAllAgentConfigs() const
{
	FScopeLock Lock(&ConfigLock);

	TArray<FACPAgentConfig> Configs;
	for (const auto& Pair : AgentConfigs)
	{
		Configs.Add(Pair.Value);
	}
	return Configs;
}

FACPAgentConfig* FACPAgentManager::GetAgentConfig(const FString& AgentName)
{
	FScopeLock Lock(&ConfigLock);
	return AgentConfigs.Find(AgentName);
}

TArray<FString> FACPAgentManager::GetAvailableAgentNames() const
{
	FScopeLock Lock(&ConfigLock);

	TArray<FString> Names;
	for (const auto& Pair : AgentConfigs)
	{
		Names.Add(Pair.Key);
	}
	return Names;
}

bool FACPAgentManager::IsChatGatewayAgent(const FString& AgentName) const
{
	// Check by registry ID first (quirks-driven)
	if (const FACPAgentConfig* Config = AgentConfigs.Find(AgentName))
	{
		if (!Config->RegistryId.IsEmpty())
		{
			const FACPAgentQuirks& Quirks = FACPAgentQuirksMap::GetQuirks(Config->RegistryId);
			return Quirks.Transport == EAgentTransport::ChatGateway;
		}
	}
	// Legacy fallback for agents without registry ID
	return AgentName == TEXT("OpenRouter");
}

bool FACPAgentManager::ConnectToAgent(const FString& AgentName)
{
	// Get config
	FACPAgentConfig* Config = GetAgentConfig(AgentName);
	if (!Config)
	{
		UE_LOG(LogAgentIntegrationKit, Error, TEXT("ACPAgentManager: No configuration found for agent: %s"), *AgentName);
		return false;
	}

	// Check if this is Chat Gateway (built-in, no subprocess)
	if (IsChatGatewayAgent(AgentName))
	{
		// Check if already connected
		{
			FScopeLock Lock(&ClientLock);
			if (TSharedPtr<FChatGatewayClient>* ExistingClient = ActiveChatGatewayClients.Find(AgentName))
			{
				if ((*ExistingClient)->IsConnected())
				{
					return true;
				}
			}
		}

		// Create new Chat Gateway client — uses first enabled provider as default,
		// but fetches models from all enabled providers
		TSharedPtr<FChatGatewayClient> Client = MakeShared<FChatGatewayClient>();
		if (const UACPSettings* Settings = UACPSettings::Get())
		{
			TArray<FString> EnabledIds = Settings->GetProviderPriority();
			if (EnabledIds.Num() > 0)
			{
				TSharedPtr<IChatGatewayProvider> FirstProvider = ChatGateway::FindProvider(EnabledIds[0]);
				if (FirstProvider.IsValid())
				{
					Client->SetProvider(FirstProvider.ToSharedRef());
				}
			}
		}

		// Bind delegates
		Client->OnStateChanged.AddLambda([this, AgentName](EACPClientState State, const FString& Message)
		{
			OnChatGatewayStateChanged(AgentName, State, Message);
		});

		Client->OnSessionUpdate.AddLambda([this, AgentName](const FACPSessionUpdate& Update)
		{
			OnChatGatewaySessionUpdate(AgentName, Update);
		});

		Client->OnModelsAvailable.AddLambda([this, AgentName](const FACPSessionModelState& ModelState)
		{
			OnChatGatewayModelsAvailable(AgentName, ModelState);
		});

		Client->OnError.AddLambda([this, AgentName](int32 Code, const FString& Message)
		{
			UE_LOG(LogAgentIntegrationKit, Error, TEXT("ACPAgentManager: ChatGateway error %d: %s"), Code, *Message);
			FString SessionId;
			{
				FScopeLock Lock(&ClientLock);
				if (TSharedPtr<FChatGatewayClient>* ClientPtr = ActiveChatGatewayClients.Find(AgentName))
				{
					SessionId = (*ClientPtr)->GetUnrealSessionId();
				}
			}
			if (SessionId.IsEmpty())
			{
				SessionId = FindSessionForAgent(AgentName);
			}
			OnAgentError.Broadcast(SessionId, AgentName, Code, Message);
		});

		// Pre-apply saved model so the client starts with the right selection.
		FACPAgentConfig ChatGatewayConfig = *Config;
		if (const UACPSettings* Settings = UACPSettings::Get())
		{
			const FString SavedModel = Settings->GetSavedModelForAgent(AgentName);
			if (!SavedModel.IsEmpty())
			{
				ChatGatewayConfig.ModelId = SavedModel;
			}
		}

		// Connect
		if (!Client->Connect(ChatGatewayConfig))
		{
			UE_LOG(LogAgentIntegrationKit, Error, TEXT("ACPAgentManager: Failed to connect to Chat Gateway"));
			return false;
		}

		// Store client
		{
			FScopeLock Lock(&ClientLock);
			ActiveChatGatewayClients.Add(AgentName, Client);
		}

		return true;
	}

	// Lazy binary download: if the agent is installed (in settings) but the managed binary
	// hasn't been downloaded yet, trigger download+extract.
	// Apply quirks override so agents with BinaryArchiveUrlOverride (e.g., codex-acp)
	// download from the fork instead of upstream.
	if (!Config->RegistryId.IsEmpty())
	{
		const FACPRegistryAgent* RegistryAgent = FACPRegistryClient::Get().FindAgent(Config->RegistryId);
		const FString PlatformKey = FACPRegistryClient::GetCurrentPlatformKey();
		const FACPAgentQuirks& Quirks = FACPAgentQuirksMap::GetQuirks(Config->RegistryId);

		if (RegistryAgent && RegistryAgent->Distribution.HasBinaryForPlatform(PlatformKey))
		{
			// Apply archive URL override from quirks (e.g., betidestu fork for codex-acp)
			FACPRegistryAgent EffectiveAgent = *RegistryAgent;
			if (!Quirks.BinaryArchiveUrlOverride.IsEmpty())
			{
				for (auto& Pair : EffectiveAgent.Distribution.BinaryTargets)
				{
					FString OldUrl = Pair.Value.Archive;
					int32 LastSlash;
					if (OldUrl.FindLastChar('/', LastSlash))
					{
						FString Filename = OldUrl.Mid(LastSlash + 1);
						FString Ext;
						if (Filename.EndsWith(TEXT(".tar.gz"))) { Ext = TEXT("tar.gz"); Filename.LeftChopInline(7); }
						else if (Filename.EndsWith(TEXT(".zip"))) { Ext = TEXT("zip"); Filename.LeftChopInline(4); }
						if (!Ext.IsEmpty())
						{
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
									NewUrl = NewUrl.Replace(TEXT("{version}"), *EffectiveAgent.Version);
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

			const FACPRegistryBinaryTarget& Target = EffectiveAgent.Distribution.BinaryTargets[PlatformKey];

			if (!Target.Archive.IsEmpty() && !FAgentInstaller::IsAgentBinaryExtracted(EffectiveAgent.Id, Target.Archive, Target.Cmd))
			{
				// Guard against re-entrant download loops: if we're already downloading
				// the binary for this agent, don't start another download.
				if (AgentsDownloadingBinary.Contains(AgentName))
				{
					UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAgentManager: Binary download already in progress for '%s', skipping"), *AgentName);
					return true;
				}
				AgentsDownloadingBinary.Add(AgentName);

				UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPAgentManager: Binary not found for '%s', downloading from %s"), *AgentName, *Target.Archive);

				OnAgentStateChanged.Broadcast(FString(), AgentName, EACPClientState::Connecting, TEXT("Downloading agent binary..."));

				FACPRegistryAgent AgentCopy = EffectiveAgent;
				FString AgentNameCopy = AgentName;

				FAgentInstaller::Get().InstallRegistryAgentAsync(
					AgentCopy,
					FAgentInstaller::ERegistryInstallMethod::Binary,
					FOnInstallProgress::CreateLambda([this, AgentNameCopy](const FString& Status)
					{
						OnAgentStateChanged.Broadcast(FString(), AgentNameCopy, EACPClientState::Connecting, Status);
					}),
					FOnInstallComplete::CreateLambda([this, AgentNameCopy](bool bSuccess, const FString& Error)
					{
						AgentsDownloadingBinary.Remove(AgentNameCopy);

						if (bSuccess)
						{
							UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPAgentManager: Binary download complete for '%s', reconnecting"), *AgentNameCopy);
							InitializeDefaultAgents();
							ConnectToAgent(AgentNameCopy);
						}
						else
						{
							UE_LOG(LogAgentIntegrationKit, Error, TEXT("ACPAgentManager: Binary download failed for '%s': %s"), *AgentNameCopy, *Error);
							OnAgentStateChanged.Broadcast(FString(), AgentNameCopy, EACPClientState::Error,
								FString::Printf(TEXT("Download failed: %s"), *Error));
						}
					})
				);

				return true; // Connection will complete asynchronously after download
			}
		}
	}

	// Standard ACP agent (Claude Code, Gemini CLI, etc.)
	// Check if already connected, and extract any stale client for cleanup
	TSharedPtr<FACPClient> OldClient;
	{
		FScopeLock Lock(&ClientLock);
		if (TSharedPtr<FACPClient>* ExistingClient = ActiveClients.Find(AgentName))
		{
			if ((*ExistingClient)->IsConnected())
			{
				return true;
			}
			// Stale client (Error/Disconnected) — extract it from the map so we can
			// disconnect it safely outside the TMap operation (avoids destroying it
			// inline during TMap::Add, which races with the client's worker thread).
			OldClient = *ExistingClient;
			ActiveClients.Remove(AgentName);
		}
	}

	// Disconnect old client outside the lock — this waits for its worker thread
	if (OldClient.IsValid())
	{
		OldClient->Disconnect();
		OldClient.Reset();
	}

	// Create new client
	TSharedPtr<FACPClient> Client = MakeShared<FACPClient>();

	// Bind delegates
	Client->OnStateChanged.AddLambda([this, AgentName](EACPClientState State, const FString& Message)
	{
		OnClientStateChanged(AgentName, State, Message);
	});

	Client->OnSessionUpdate.AddLambda([this, AgentName](const FACPSessionUpdate& Update)
	{
		OnClientSessionUpdate(AgentName, Update);
	});

	Client->OnModelsAvailable.AddLambda([this, AgentName](const FACPSessionModelState& ModelState)
	{
		OnClientModelsAvailable(AgentName, ModelState);
	});

	Client->OnPermissionRequest.AddLambda([this, AgentName](const FACPPermissionRequest& Request)
	{
		OnClientPermissionRequest(AgentName, Request);
	});

	Client->OnModesAvailable.AddLambda([this, AgentName](const FACPSessionModeState& ModeState)
	{
		OnClientModesAvailable(AgentName, ModeState);
	});

	Client->OnModeChanged.AddLambda([this, AgentName](const FString& ModeId)
	{
		OnClientModeChanged(AgentName, ModeId);
	});

	Client->OnCommandsAvailable.AddLambda([this, AgentName](const TArray<FACPSlashCommand>& Commands)
	{
		OnClientCommandsAvailable(AgentName, Commands);
	});

	Client->OnError.AddLambda([this, AgentName](int32 Code, const FString& Message)
	{
		UE_LOG(LogAgentIntegrationKit, Error, TEXT("ACPAgentManager: %s error %d: %s"), *AgentName, Code, *Message);
		FString SessionId;
		{
			FScopeLock Lock(&ClientLock);
			if (TSharedPtr<FACPClient>* ClientPtr = ActiveClients.Find(AgentName))
			{
				SessionId = (*ClientPtr)->GetUnrealSessionId();
			}
		}
		if (SessionId.IsEmpty())
		{
			SessionId = FindSessionForAgent(AgentName);
		}
		OnAgentError.Broadcast(SessionId, AgentName, Code, Message);
	});

	Client->OnAuthComplete.AddLambda([this, AgentName](bool bSuccess, const FString& Error)
	{
		FString SessionId = FindSessionForAgent(AgentName);
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPAgentManager: %s auth complete (success=%d): %s"), *AgentName, bSuccess, *Error);
		OnAgentAuthComplete.Broadcast(SessionId, AgentName, bSuccess, Error);
	});

	Client->OnSessionListReceived.AddLambda([this, AgentName](const TArray<FACPRemoteSessionEntry>& Sessions)
	{
		OnClientSessionListReceived(AgentName, Sessions);
	});

	Client->OnSessionInfoUpdated.AddLambda([this, AgentName](const FString& UnrealSessionId, const FString& Title)
	{
		// The session_info_update arrives with the Unreal session ID, but the cached
		// session list uses the agent's native session ID. Resolve via SessionManager.
		FString AgentSessionId;
		{
			FACPSessionManager& SessionMgr = FACPSessionManager::Get();
			const FACPActiveSession* Active = SessionMgr.GetActiveSession(UnrealSessionId);
			if (Active)
			{
				AgentSessionId = Active->Metadata.AgentSessionId;
			}
		}

		// Update the cached session list entry
		if (TArray<FACPRemoteSessionEntry>* CachedList = CachedSessionLists.Find(AgentName))
		{
			// Match by either agent native ID or Unreal session ID
			for (FACPRemoteSessionEntry& Entry : *CachedList)
			{
				if (Entry.SessionId == AgentSessionId || Entry.SessionId == UnrealSessionId)
				{
					Entry.Title = Title;
					Entry.UpdatedAt = FDateTime::Now();
					break;
				}
			}
			// Re-broadcast so the WebUI sidebar updates in real time
			OnAgentSessionListReceived.Broadcast(AgentName, *CachedList);
		}
	});

	// Resolve launch-time config (Gemini model is selected at process startup).
	FACPAgentConfig LaunchConfig = *Config;
	{
		FScopeLock Lock(&ClientLock);
		if (const FString* LaunchResumeSessionId = PendingLaunchResumeSessions.Find(AgentName))
		{
			LaunchConfig.LaunchResumeSessionId = *LaunchResumeSessionId;
			PendingLaunchResumeSessions.Remove(AgentName);
		}
	}
	if (IsGeminiCliAgent(AgentName))
	{
		const UACPSettings* Settings = UACPSettings::Get();
		const FString SavedModelId = Settings ? Settings->GetSavedModelForAgent(AgentName) : FString();
		const FString LaunchModelId = ResolveGeminiCliLaunchModelId(SavedModelId);

		LaunchConfig.Arguments = StripGeminiCliModelArgs(LaunchConfig.Arguments);
		LaunchConfig.Arguments.Add(TEXT("--model"));
		LaunchConfig.Arguments.Add(LaunchModelId);

		UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPAgentManager: Gemini CLI launch model: %s"), *LaunchModelId);
	}

	// Apply quirks-based MCP injection: some agents (Copilot, Gemini) don't consume
	// mcpServers from ACP session/new and need the MCP server injected via CLI flag or env var.
	// We store the config path locally and set it on the client AFTER Connect() — because
	// Connect() cleans up any existing paths from a previous run and would delete our file.
	FString McpConfigTempPath;
	EMCPInjectionStrategy McpInjectionUsed = EMCPInjectionStrategy::ACPSessionParam;

	if (!LaunchConfig.RegistryId.IsEmpty() && FMCPServer::Get().IsRunning())
	{
		const FACPAgentQuirks& Quirks = FACPAgentQuirksMap::GetQuirks(LaunchConfig.RegistryId);

		if (Quirks.MCPInjection == EMCPInjectionStrategy::CliConfigFile
			&& !Quirks.MCPCliFlag.IsEmpty() && !Quirks.MCPConfigTemplate.IsEmpty())
		{
			McpConfigTempPath = WriteMcpConfigTempFile(
				LaunchConfig.RegistryId, Quirks.MCPConfigTemplate, FMCPServer::Get().GetPort());

			if (!McpConfigTempPath.IsEmpty())
			{
				FString ArgValue = Quirks.bMCPCliPrefixAt
					? FString::Printf(TEXT("@%s"), *McpConfigTempPath)
					: McpConfigTempPath;

				LaunchConfig.Arguments.Add(Quirks.MCPCliFlag);
				LaunchConfig.Arguments.Add(ArgValue);
				McpInjectionUsed = EMCPInjectionStrategy::CliConfigFile;

				UE_LOG(LogAgentIntegrationKit, Log,
					TEXT("ACPAgentManager: Injected MCP via CLI flag for '%s': %s %s"),
					*AgentName, *Quirks.MCPCliFlag, *ArgValue);
			}
		}
		else if (Quirks.MCPInjection == EMCPInjectionStrategy::EnvVarConfigFile
			&& !Quirks.MCPEnvVarName.IsEmpty() && !Quirks.MCPConfigTemplate.IsEmpty())
		{
			McpConfigTempPath = WriteMcpConfigTempFile(
				LaunchConfig.RegistryId, Quirks.MCPConfigTemplate, FMCPServer::Get().GetPort());

			if (!McpConfigTempPath.IsEmpty())
			{
				LaunchConfig.EnvironmentVariables.Add(Quirks.MCPEnvVarName, McpConfigTempPath);
				McpInjectionUsed = EMCPInjectionStrategy::EnvVarConfigFile;

				UE_LOG(LogAgentIntegrationKit, Log,
					TEXT("ACPAgentManager: Injected MCP via env var for '%s': %s=%s"),
					*AgentName, *Quirks.MCPEnvVarName, *McpConfigTempPath);
			}
		}
	}

	// Connect
	if (!Client->Connect(LaunchConfig))
	{
		// Clean up temp file on failure
		if (!McpConfigTempPath.IsEmpty())
		{
			IFileManager::Get().Delete(*McpConfigTempPath, false, true);
		}
		UE_LOG(LogAgentIntegrationKit, Error, TEXT("ACPAgentManager: Failed to connect to agent: %s"), *AgentName);
		return false;
	}

	// Store temp file path on client for cleanup on disconnect (AFTER Connect so it doesn't get deleted)
	if (!McpConfigTempPath.IsEmpty())
	{
		if (McpInjectionUsed == EMCPInjectionStrategy::CliConfigFile)
		{
			Client->CopilotAdditionalMcpConfigPath = McpConfigTempPath;
		}
		else if (McpInjectionUsed == EMCPInjectionStrategy::EnvVarConfigFile)
		{
			Client->GeminiSystemSettingsPath = McpConfigTempPath;
		}
	}

	// Store client
	{
		FScopeLock Lock(&ClientLock);
		ActiveClients.Add(AgentName, Client);
	}

	return true;
}

void FACPAgentManager::DisconnectFromAgent(const FString& AgentName)
{
	auto CleanupPendingForAgent = [this, &AgentName]()
	{
		FScopeLock Lock(&ClientLock);
		PendingNewSessions.Remove(AgentName);

		TArray<FString> PromptKeysToRemove;
		for (const auto& Pair : PendingPrompts)
		{
			const FString PendingAgent = GetSessionAgent(Pair.Key);
			if (Pair.Key == AgentName || PendingAgent == AgentName)
			{
				PromptKeysToRemove.Add(Pair.Key);
			}
		}
		for (const FString& Key : PromptKeysToRemove)
		{
			PendingPrompts.Remove(Key);
		}
	};

	// Check Chat Gateway first
	if (IsChatGatewayAgent(AgentName))
	{
		TSharedPtr<FChatGatewayClient> Client;
		{
			FScopeLock Lock(&ClientLock);
			if (TSharedPtr<FChatGatewayClient>* ClientPtr = ActiveChatGatewayClients.Find(AgentName))
			{
				Client = *ClientPtr;
				ActiveChatGatewayClients.Remove(AgentName);
			}
		}

		if (Client.IsValid())
		{
			Client->Disconnect();
		}
		CleanupPendingForAgent();
		return;
	}

	// Standard ACP client
	TSharedPtr<FACPClient> Client;
	{
		FScopeLock Lock(&ClientLock);
		if (TSharedPtr<FACPClient>* ClientPtr = ActiveClients.Find(AgentName))
		{
			Client = *ClientPtr;
			ActiveClients.Remove(AgentName);
		}
	}

	if (Client.IsValid())
	{
		Client->Disconnect();
	}

	CleanupPendingForAgent();
}

void FACPAgentManager::SetLaunchResumeSession(const FString& AgentName, const FString& SessionId)
{
	FScopeLock Lock(&ClientLock);
	if (SessionId.IsEmpty())
	{
		PendingLaunchResumeSessions.Remove(AgentName);
	}
	else
	{
		PendingLaunchResumeSessions.Add(AgentName, SessionId);
	}
}

void FACPAgentManager::DisconnectAll()
{
	// Disconnect Chat Gateway clients
	TArray<TSharedPtr<FChatGatewayClient>> ChatGatewayClients;
	{
		FScopeLock Lock(&ClientLock);
		for (const auto& Pair : ActiveChatGatewayClients)
		{
			ChatGatewayClients.Add(Pair.Value);
		}
		ActiveChatGatewayClients.Empty();
	}

	for (TSharedPtr<FChatGatewayClient>& Client : ChatGatewayClients)
	{
		if (Client.IsValid())
		{
			Client->Disconnect();
		}
	}

	// Disconnect ACP clients
	TArray<TSharedPtr<FACPClient>> Clients;
	{
		FScopeLock Lock(&ClientLock);
		for (const auto& Pair : ActiveClients)
		{
			Clients.Add(Pair.Value);
		}
		ActiveClients.Empty();
	}

	for (TSharedPtr<FACPClient>& Client : Clients)
	{
		if (Client.IsValid())
		{
			Client->Disconnect();
		}
	}
}

TSharedPtr<FACPClient> FACPAgentManager::GetClient(const FString& AgentName)
{
	FScopeLock Lock(&ClientLock);

	if (TSharedPtr<FACPClient>* Client = ActiveClients.Find(AgentName))
	{
		return *Client;
	}
	return nullptr;
}

TSharedPtr<FChatGatewayClient> FACPAgentManager::GetChatGatewayClient(const FString& AgentName)
{
	FScopeLock Lock(&ClientLock);

	if (TSharedPtr<FChatGatewayClient>* Client = ActiveChatGatewayClients.Find(AgentName))
	{
		return *Client;
	}
	return nullptr;
}

bool FACPAgentManager::IsConnectedToAgent(const FString& AgentName) const
{
	FScopeLock Lock(&ClientLock);

	// Check Chat Gateway
	if (IsChatGatewayAgent(AgentName))
	{
		if (const TSharedPtr<FChatGatewayClient>* Client = ActiveChatGatewayClients.Find(AgentName))
		{
			return (*Client)->IsConnected();
		}
		return false;
	}

	// Check ACP clients
	if (const TSharedPtr<FACPClient>* Client = ActiveClients.Find(AgentName))
	{
		return (*Client)->IsConnected();
	}
	return false;
}

void FACPAgentManager::SendPromptToAgent(const FString& AgentName, const FString& PromptText)
{
	// Handle Chat Gateway specially
	if (IsChatGatewayAgent(AgentName))
	{
		TSharedPtr<FChatGatewayClient> Client;
		{
			FScopeLock Lock(&ClientLock);
			if (TSharedPtr<FChatGatewayClient>* ClientPtr = ActiveChatGatewayClients.Find(AgentName))
			{
				Client = *ClientPtr;
			}
		}

		if (!Client.IsValid())
		{
			// Try to connect first
			if (!ConnectToAgent(AgentName))
			{
				UE_LOG(LogAgentIntegrationKit, Error, TEXT("ACPAgentManager: Cannot send prompt - failed to connect to Chat Gateway"));
				return;
			}

			FScopeLock Lock(&ClientLock);
			if (TSharedPtr<FChatGatewayClient>* ClientPtr = ActiveChatGatewayClients.Find(AgentName))
			{
				Client = *ClientPtr;
			}
		}

		if (Client.IsValid())
		{
			EACPClientState CurrentState = Client->GetState();

			if (CurrentState == EACPClientState::InSession)
			{
				// Already in session, send prompt directly
				Client->SendPrompt(PromptText);
			}
			else if (CurrentState == EACPClientState::Ready)
			{
				// Need to create session first - queue the prompt and start session
				{
					FScopeLock Lock(&ClientLock);
					PendingPrompts.Add(AgentName, PromptText);
				}
				Client->NewSession(FPaths::ProjectDir());
			}
			else
			{
				// Still initializing or in other state - queue the prompt
				FScopeLock Lock(&ClientLock);
				PendingPrompts.Add(AgentName, PromptText);
				UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Queued prompt for Chat Gateway (state: %d)"), (int32)CurrentState);
			}
		}
		return;
	}

	// Standard ACP client
	TSharedPtr<FACPClient> Client = GetClient(AgentName);
	if (!Client.IsValid())
	{
		// Try to connect first
		if (!ConnectToAgent(AgentName))
		{
			UE_LOG(LogAgentIntegrationKit, Error, TEXT("ACPAgentManager: Cannot send prompt - failed to connect to agent: %s"), *AgentName);
			return;
		}
		Client = GetClient(AgentName);
	}

	if (Client.IsValid())
	{
		EACPClientState CurrentState = Client->GetState();

		if (CurrentState == EACPClientState::InSession)
		{
			// Already in session, send prompt directly
			Client->SendPrompt(PromptText);
		}
		else if (CurrentState == EACPClientState::Ready)
		{
			// Need to create session first - queue the prompt and start session
			{
				FScopeLock Lock(&ClientLock);
				PendingPrompts.Add(AgentName, PromptText);
			}
			Client->NewSession(FPaths::ProjectDir());
		}
		else
		{
			// Still initializing or in other state - queue the prompt
			FScopeLock Lock(&ClientLock);
			PendingPrompts.Add(AgentName, PromptText);
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Queued prompt for %s (state: %d)"), *AgentName, (int32)CurrentState);
		}
	}
}

void FACPAgentManager::OnClientSessionUpdate(const FString& AgentName, const FACPSessionUpdate& Update)
{
	// Prefer session ID from ACP payload for robust routing across parallel sessions.
	FString SessionId = Update.SessionId;
	if (!SessionId.IsEmpty())
	{
		const FString KnownSessionAgent = GetSessionAgent(SessionId);
		if (KnownSessionAgent.IsEmpty())
		{
			// Resolve agent-native external ID -> Unreal session ID.
			bool bResolvedExternalId = false;
			TArray<FString> ActiveIds = FACPSessionManager::Get().GetActiveSessionIds();
			for (const FString& ActiveId : ActiveIds)
			{
				const FACPActiveSession* Active = FACPSessionManager::Get().GetActiveSession(ActiveId);
				if (Active && Active->Metadata.AgentName == AgentName && Active->Metadata.AgentSessionId == Update.SessionId)
				{
					SessionId = ActiveId;
					bResolvedExternalId = true;
					break;
				}
			}
			if (!bResolvedExternalId)
			{
				// Unknown external ID (common before SetSessionExternalId runs) — fall back to client tracking.
				SessionId.Empty();
			}
		}
		else if (KnownSessionAgent != AgentName)
		{
			// Defensive: ignore cross-agent IDs.
			SessionId.Empty();
		}
	}

	if (SessionId.IsEmpty())
	{
		FScopeLock Lock(&ClientLock);
		if (TSharedPtr<FACPClient>* ClientPtr = ActiveClients.Find(AgentName))
		{
			SessionId = (*ClientPtr)->GetUnrealSessionId();
			if (!SessionId.IsEmpty() && !Update.SessionId.IsEmpty() && Update.SessionId != SessionId)
			{
				FACPSessionManager::Get().SetSessionExternalId(SessionId, Update.SessionId);
			}
		}
	}

	if (SessionId.IsEmpty())
	{
		SessionId = FindSessionForAgent(AgentName);
	}

	// Clear streaming state on error
	if (Update.UpdateType == EACPUpdateType::Error)
	{
		FScopeLock Lock(&SessionLock);
		if (FAgentSessionContext* Context = ActiveSessions.Find(SessionId))
		{
			Context->bIsStreaming = false;
		}
	}

	// Broadcast plan updates separately for UI convenience
	if (Update.UpdateType == EACPUpdateType::Plan && Update.Plan.Entries.Num() > 0)
	{
		OnAgentPlanUpdate.Broadcast(SessionId, AgentName, Update.Plan);
	}

	OnAgentMessage.Broadcast(SessionId, AgentName, Update);
}

void FACPAgentManager::OnClientStateChanged(const FString& AgentName, EACPClientState State, const FString& Message)
{
	// Get the Unreal SessionId directly from the client for accurate routing
	FString SessionId;
	{
		FScopeLock Lock(&ClientLock);
		if (TSharedPtr<FACPClient>* ClientPtr = ActiveClients.Find(AgentName))
		{
			SessionId = (*ClientPtr)->GetUnrealSessionId();
		}
	}

	// Fall back to lookup if client doesn't have a tracked session
	if (SessionId.IsEmpty())
	{
		SessionId = FindSessionForAgent(AgentName);
	}

	// Clear streaming state when agent is no longer prompting
	if (State != EACPClientState::Prompting && !SessionId.IsEmpty())
	{
		FScopeLock Lock(&SessionLock);
		if (FAgentSessionContext* Context = ActiveSessions.Find(SessionId))
		{
			Context->bIsStreaming = false;
		}
	}

	// When agent becomes Ready (after initialize), check if we have pending prompts and create session
	if (State == EACPClientState::Ready)
	{
		bool bHasPendingPrompt = false;
		FString PendingSessionId;
		{
			FScopeLock Lock(&ClientLock);
			// Check for any pending prompt (keyed by session ID or agent name for legacy)
			for (const auto& Pair : PendingPrompts)
			{
				// Check if this pending prompt belongs to this agent
				FString PendingAgent = GetSessionAgent(Pair.Key);
				if (PendingAgent == AgentName || Pair.Key == AgentName)
				{
					bHasPendingPrompt = true;
					PendingSessionId = Pair.Key;
					break;
				}
			}
		}

		if (bHasPendingPrompt)
		{
			TSharedPtr<FACPClient> Client = GetClient(AgentName);
			if (Client.IsValid())
			{
				const bool bHasSessionId = !PendingSessionId.IsEmpty() && PendingSessionId != AgentName;
				FString WorkingDirectory = FPaths::ProjectDir();
				bool bIsResumedSession = false;
				if (bHasSessionId)
				{
					if (const FACPActiveSession* Session = FACPSessionManager::Get().GetActiveSession(PendingSessionId))
					{
						bIsResumedSession = Session->bIsLoadingHistory;
					}
				}

				Client->SetUnrealSessionId(PendingSessionId);
				if (bIsResumedSession && bHasSessionId)
				{
					// Session was resumed from ACP list — SessionId IS the agent's session ID
					const FACPAgentCapabilities& Caps = Client->GetAgentCapabilities();
					if (Caps.bSupportsResumeSession)
					{
						UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Agent ready, resuming session for pending prompt: %s"), *AgentName);
						Client->ResumeSession(PendingSessionId, WorkingDirectory);
					}
					else if (Caps.bSupportsLoadSession)
					{
						UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Agent ready, loading session for pending prompt: %s"), *AgentName);
						Client->LoadSession(PendingSessionId, WorkingDirectory);
					}
					else
					{
						UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Agent ready, creating session for pending prompt (no resume/load support): %s"), *AgentName);
						Client->NewSession(WorkingDirectory);
					}
				}
				else
				{
					UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Agent ready, creating session for pending prompt: %s"), *AgentName);
					Client->NewSession(WorkingDirectory);
				}
			}

				// If this prompt corresponds to a queued NewSession entry, remove only that one.
				if (!PendingSessionId.IsEmpty() && PendingSessionId != AgentName)
				{
					FScopeLock PNSLock(&ClientLock);
					PendingNewSessions.RemoveSingle(AgentName, PendingSessionId);
				}
		}
		else
		{
			// No pending prompt — check if a session was created while the agent was still connecting.
			// If so, we need to call NewSession now so the agent pushes models/modes/commands.
				FString QueuedSessionId;
				{
					FScopeLock Lock(&ClientLock);
					TArray<FString> QueuedIds;
					PendingNewSessions.MultiFind(AgentName, QueuedIds);
					if (QueuedIds.Num() > 0)
					{
						QueuedSessionId = QueuedIds[0];
						PendingNewSessions.RemoveSingle(AgentName, QueuedSessionId);
					}
				}

			if (!QueuedSessionId.IsEmpty())
			{
				TSharedPtr<FACPClient> Client = GetClient(AgentName);
				if (Client.IsValid())
				{
					Client->SetUnrealSessionId(QueuedSessionId);
					UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Agent ready, creating session for pending WebUI session: %s (session: %s)"), *AgentName, *QueuedSessionId);
					Client->NewSession(FPaths::ProjectDir());
				}
			}
		}
	}
	// When session is established, send any pending prompts for this agent
	else if (State == EACPClientState::InSession)
	{
		TSharedPtr<FACPClient> Client = GetClient(AgentName);
		FString CurrentClientSessionId = Client.IsValid() ? Client->GetUnrealSessionId() : FString();
		TArray<FString> KeysToRemove;
		TArray<FString> PromptsToSend;
		FString NextSessionToActivate;

		{
			FScopeLock Lock(&ClientLock);
			for (const auto& Pair : PendingPrompts)
			{
				FString PendingAgent = GetSessionAgent(Pair.Key);
				const bool bLegacyAgentQueue = Pair.Key == AgentName;
				const bool bBelongsToAgent = PendingAgent == AgentName || bLegacyAgentQueue;
				if (!bBelongsToAgent)
				{
					continue;
				}

				// Flush only prompts for the session this client is currently serving.
				if (bLegacyAgentQueue
					|| (!CurrentClientSessionId.IsEmpty() && Pair.Key == CurrentClientSessionId))
				{
					PromptsToSend.Add(Pair.Value);
					KeysToRemove.Add(Pair.Key);
				}
				else if (!bLegacyAgentQueue && NextSessionToActivate.IsEmpty())
				{
					NextSessionToActivate = Pair.Key;
				}
			}
			for (const FString& Key : KeysToRemove)
			{
				PendingPrompts.Remove(Key);
			}
		}

		if (PromptsToSend.Num() > 0)
		{
			if (Client.IsValid())
			{
				UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Sending %d queued prompt(s) for %s"), PromptsToSend.Num(), *AgentName);
				for (const FString& Prompt : PromptsToSend)
				{
					Client->SendPrompt(Prompt);
				}
			}
		}
		else if (!NextSessionToActivate.IsEmpty() && Client.IsValid())
		{
			// Prompt(s) are queued for a different session. Switch first, then the next
			// InSession state callback will flush prompts for that session.
			FString WorkingDirectory = FPaths::ProjectDir();
			bool bIsResumedSession = false;
			if (const FACPActiveSession* Session = FACPSessionManager::Get().GetActiveSession(NextSessionToActivate))
			{
				bIsResumedSession = Session->bIsLoadingHistory;
			}

			Client->SetUnrealSessionId(NextSessionToActivate);
			if (bIsResumedSession)
			{
				const FACPAgentCapabilities& Caps = Client->GetAgentCapabilities();
				if (Caps.bSupportsResumeSession)
				{
					Client->ResumeSession(NextSessionToActivate, WorkingDirectory);
				}
				else if (Caps.bSupportsLoadSession)
				{
					Client->LoadSession(NextSessionToActivate, WorkingDirectory);
				}
				else
				{
					Client->NewSession(WorkingDirectory);
				}
			}
			else
			{
				Client->NewSession(WorkingDirectory);
			}
		}
	}

	// Auto-request session list when agent becomes Ready or InSession
	if (State == EACPClientState::Ready || State == EACPClientState::InSession)
	{
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPAgentManager: State=%s for '%s', requesting session list"),
			State == EACPClientState::Ready ? TEXT("Ready") : TEXT("InSession"), *AgentName);
		RequestSessionList(AgentName);
	}

	// Re-read actual client state: sending pending prompts above may have transitioned
	// the client to Prompting. Broadcasting the stale InSession state would cause the UI
	// to show a false "finished responding" notification.
	EACPClientState ActualState = State;
	{
		TSharedPtr<FACPClient> Client = GetClient(AgentName);
		if (Client.IsValid())
		{
			ActualState = Client->GetState();
		}
	}
	OnAgentStateChanged.Broadcast(SessionId, AgentName, ActualState, Message);
}

void FACPAgentManager::RequestSessionList(const FString& AgentName)
{
	if (IsChatGatewayAgent(AgentName))
	{
		return; // Chat Gateway doesn't support session listing
	}

	TSharedPtr<FACPClient> Client = GetClient(AgentName);
	if (!Client.IsValid())
	{
		return;
	}

	if (!Client->GetAgentCapabilities().bSupportsListSessions)
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Agent '%s' does not support session listing"), *AgentName);
		return;
	}

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPAgentManager: Requesting session list from '%s'"), *AgentName);
	Client->ListSessions(FPaths::ProjectDir());
}

TArray<FACPRemoteSessionEntry> FACPAgentManager::GetCachedSessionList(const FString& AgentName) const
{
	if (const TArray<FACPRemoteSessionEntry>* List = CachedSessionLists.Find(AgentName))
	{
		return *List;
	}
	return TArray<FACPRemoteSessionEntry>();
}

void FACPAgentManager::OnClientSessionListReceived(const FString& AgentName, const TArray<FACPRemoteSessionEntry>& Sessions)
{
	// Merge ACP sessions with local history sessions (from disk readers like CodexHistoryReader).
	// Local history catches sessions that ACP's list_threads misses.
	TArray<FACPRemoteSessionEntry> Merged = Sessions;

	// Collect local history sessions
	TArray<FACPRemoteSessionEntry> LocalSessions;
	if (AgentName == TEXT("Codex CLI") || AgentName == TEXT("Codex"))
	{
		LocalSessions = FACPCodexHistoryReader::ListSessions(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	}

	// Add local sessions that aren't already in the ACP results (dedup by SessionId)
	if (LocalSessions.Num() > 0)
	{
		TSet<FString> ExistingIds;
		for (const FACPRemoteSessionEntry& Entry : Merged)
		{
			ExistingIds.Add(Entry.SessionId);
		}

		int32 AddedCount = 0;
		for (const FACPRemoteSessionEntry& LocalEntry : LocalSessions)
		{
			if (!ExistingIds.Contains(LocalEntry.SessionId))
			{
				Merged.Add(LocalEntry);
				AddedCount++;
			}
		}

		if (AddedCount > 0)
		{
			UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPAgentManager: Merged %d local history sessions into %d ACP sessions for '%s'"),
				AddedCount, Sessions.Num(), *AgentName);
		}
	}

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPAgentManager: Cached %d sessions for '%s'"), Merged.Num(), *AgentName);
	CachedSessionLists.FindOrAdd(AgentName) = Merged;
	OnAgentSessionListReceived.Broadcast(AgentName, Merged);
}

void FACPAgentManager::DeleteRemoteSession(const FString& AgentName, const FString& AgentSessionId)
{
	if (AgentName.IsEmpty() || AgentSessionId.IsEmpty())
	{
		return;
	}

	TSharedPtr<FACPClient> Client = GetClient(AgentName);
	if (!Client.IsValid())
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: No active client for '%s', skipping ACP delete"), *AgentName);
		return;
	}

	Client->DeleteSession(AgentSessionId);
}

bool FACPAgentManager::AgentSupportsDeleteSession(const FString& AgentName) const
{
	FScopeLock Lock(&ClientLock);
	if (const TSharedPtr<FACPClient>* ClientPtr = ActiveClients.Find(AgentName))
	{
		if (ClientPtr->IsValid())
		{
			return (*ClientPtr)->GetAgentCapabilities().bSupportsDeleteSession;
		}
	}
	return false;
}

void FACPAgentManager::LoadConfigFromSettings()
{
	// TODO: Load from UACPSettings
}

void FACPAgentManager::SaveConfigToSettings()
{
	// TODO: Save to UACPSettings
}

FACPSessionModelState FACPAgentManager::GetAgentModelState(const FString& AgentName) const
{
	FScopeLock Lock(&ClientLock);

	// Check Chat Gateway
	if (IsChatGatewayAgent(AgentName))
	{
		if (const TSharedPtr<FChatGatewayClient>* ClientPtr = ActiveChatGatewayClients.Find(AgentName))
		{
			if (ClientPtr->IsValid())
			{
				return (*ClientPtr)->GetModelState();
			}
		}
		return FACPSessionModelState();
	}

	// Check ACP clients
	if (const TSharedPtr<FACPClient>* ClientPtr = ActiveClients.Find(AgentName))
	{
		if (ClientPtr->IsValid())
		{
			FACPSessionModelState State = (*ClientPtr)->GetModelState();
			if (IsGeminiCliAgent(AgentName) && State.AvailableModels.Num() == 0)
			{
				const UACPSettings* Settings = UACPSettings::Get();
				const FString SavedModelId = Settings ? Settings->GetSavedModelForAgent(AgentName) : FString();
				const FString PreferredModel = !State.CurrentModelId.IsEmpty() ? State.CurrentModelId : SavedModelId;
				return BuildGeminiCliFallbackModelState(PreferredModel);
			}
			return State;
		}
	}

	// Gemini CLI ACP currently does not expose model options over ACP metadata.
	// Provide a local fallback list so WebUI can select launch-time model.
	if (IsGeminiCliAgent(AgentName))
	{
		const UACPSettings* Settings = UACPSettings::Get();
		const FString SavedModelId = Settings ? Settings->GetSavedModelForAgent(AgentName) : FString();
		return BuildGeminiCliFallbackModelState(SavedModelId);
	}

	return FACPSessionModelState();
}

TArray<FACPModelInfo> FACPAgentManager::GetAgentFullModelList(const FString& AgentName) const
{
	FScopeLock Lock(&ClientLock);

	// Only Chat Gateway supports "full list" vs "curated list" concept for now
	if (IsChatGatewayAgent(AgentName))
	{
		if (const TSharedPtr<FChatGatewayClient>* ClientPtr = ActiveChatGatewayClients.Find(AgentName))
		{
			if (ClientPtr->IsValid())
			{
				return (*ClientPtr)->GetAllCachedModels();
			}
		}
	}

	if (IsGeminiCliAgent(AgentName))
	{
		const UACPSettings* Settings = UACPSettings::Get();
		const FString SavedModelId = Settings ? Settings->GetSavedModelForAgent(AgentName) : FString();
		return BuildGeminiCliFallbackModelState(SavedModelId).AvailableModels;
	}

	return TArray<FACPModelInfo>();
}

void FACPAgentManager::AddAgentRecentModel(const FString& AgentName, const FACPModelInfo& Model)
{
	FScopeLock Lock(&ClientLock);

	if (IsChatGatewayAgent(AgentName))
	{
		if (const TSharedPtr<FChatGatewayClient>* ClientPtr = ActiveChatGatewayClients.Find(AgentName))
		{
			if (ClientPtr->IsValid())
			{
				(*ClientPtr)->AddRecentModel(Model);
			}
		}
	}
}

void FACPAgentManager::RefreshAgentModels(const FString& AgentName)
{
	FScopeLock Lock(&ClientLock);

	if (IsChatGatewayAgent(AgentName))
	{
		if (const TSharedPtr<FChatGatewayClient>* ClientPtr = ActiveChatGatewayClients.Find(AgentName))
		{
			if (ClientPtr->IsValid())
			{
				(*ClientPtr)->RefreshModels();
			}
		}
	}
}

void FACPAgentManager::SetAgentModel(const FString& AgentName, const FString& ModelId)
{
	// Check Chat Gateway
	if (IsChatGatewayAgent(AgentName))
	{
		TSharedPtr<FChatGatewayClient> Client;
		{
			FScopeLock Lock(&ClientLock);
			if (TSharedPtr<FChatGatewayClient>* ClientPtr = ActiveChatGatewayClients.Find(AgentName))
			{
				Client = *ClientPtr;
			}
		}

		if (Client.IsValid())
		{
			Client->SetModel(ModelId);

			// Track selected model in Chat Gateway recents so it appears in curated list next time.
			if (!ModelId.StartsWith(TEXT("special:")))
			{
				bool bFoundInCache = false;
				const TArray<FACPModelInfo>& CachedModels = Client->GetAllCachedModels();
				for (const FACPModelInfo& ModelInfo : CachedModels)
				{
					if (ModelInfo.ModelId == ModelId)
					{
						Client->AddRecentModel(ModelInfo);
						bFoundInCache = true;
						break;
					}
				}

				// If model isn't in the cache yet, check the curated list (includes
				// hardcoded models from custom providers) before creating a placeholder.
				if (!bFoundInCache)
				{
					bool bFoundInCurated = false;
					for (const FACPModelInfo& CuratedModel : Client->GetModelState().AvailableModels)
					{
						if (CuratedModel.ModelId == ModelId)
						{
							Client->AddRecentModel(CuratedModel);
							bFoundInCurated = true;
							break;
						}
					}

					if (!bFoundInCurated)
					{
						// Last resort: create a minimal placeholder so the model still
						// appears in the curated dropdown.
						FACPModelInfo Placeholder;
						Placeholder.ModelId = ModelId;

						// Derive a readable name from the model ID (e.g. "google/gemini-2.5-pro" → "gemini-2.5-pro")
						int32 SlashIdx = INDEX_NONE;
						if (ModelId.FindChar(TEXT('/'), SlashIdx))
						{
							Placeholder.Name = ModelId.Mid(SlashIdx + 1);
						}
						else
						{
							Placeholder.Name = ModelId;
						}

						Client->AddRecentModel(Placeholder);
					}
				}
			}

			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Set model for Chat Gateway to %s"), *ModelId);
		}
		else
		{
			UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAgentManager: Cannot set model - Chat Gateway not connected"));
		}
		return;
	}

	// ACP client
	TSharedPtr<FACPClient> Client;
	{
		FScopeLock Lock(&ClientLock);
		if (TSharedPtr<FACPClient>* ClientPtr = ActiveClients.Find(AgentName))
		{
			Client = *ClientPtr;
		}
	}

	if (Client.IsValid())
	{
		Client->SetModel(ModelId);
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Set model for %s to %s"), *AgentName, *ModelId);
	}
	else
	{
		if (IsGeminiCliAgent(AgentName))
		{
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Gemini model '%s' saved for next connection"), *ModelId);
		}
		else
		{
			UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAgentManager: Cannot set model - agent not connected: %s"), *AgentName);
		}
	}
}

void FACPAgentManager::OnClientModelsAvailable(const FString& AgentName, const FACPSessionModelState& ModelState)
{
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Models available for %s - %d models, current: %s"),
		*AgentName, ModelState.AvailableModels.Num(), *ModelState.CurrentModelId);

	// Reapply persisted model selection if available and valid.
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		const FString SavedModel = Settings->GetSavedModelForAgent(AgentName);
		if (!SavedModel.IsEmpty() && SavedModel != ModelState.CurrentModelId)
		{
			bool bSavedModelAvailable = false;
			for (const FACPModelInfo& Model : ModelState.AvailableModels)
			{
				if (Model.ModelId == SavedModel)
				{
					bSavedModelAvailable = true;
					break;
				}
			}

			if (bSavedModelAvailable)
			{
				SetAgentModel(AgentName, SavedModel);
			}
		}
	}

	FString SessionId;
	{
		FScopeLock Lock(&ClientLock);
		if (TSharedPtr<FACPClient>* ClientPtr = ActiveClients.Find(AgentName))
		{
			SessionId = (*ClientPtr)->GetUnrealSessionId();
		}
	}
	if (SessionId.IsEmpty())
	{
		SessionId = FindSessionForAgent(AgentName);
	}
	OnAgentModelsAvailable.Broadcast(SessionId, AgentName, ModelState);
}

void FACPAgentManager::OnClientPermissionRequest(const FString& AgentName, const FACPPermissionRequest& Request)
{
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Permission request from %s for tool: %s"),
		*AgentName, *Request.ToolCall.Title);

	// Prefer explicit session ID from the permission request payload.
	FString SessionId = Request.SessionId;
	if (!SessionId.IsEmpty())
	{
		const FString KnownSessionAgent = GetSessionAgent(SessionId);
		if (KnownSessionAgent.IsEmpty())
		{
			// Resolve agent-native external ID -> Unreal session ID.
			bool bResolvedExternalId = false;
			TArray<FString> ActiveIds = FACPSessionManager::Get().GetActiveSessionIds();
			for (const FString& ActiveId : ActiveIds)
			{
				const FACPActiveSession* Active = FACPSessionManager::Get().GetActiveSession(ActiveId);
				if (Active && Active->Metadata.AgentName == AgentName && Active->Metadata.AgentSessionId == Request.SessionId)
				{
					SessionId = ActiveId;
					bResolvedExternalId = true;
					break;
				}
			}
			if (!bResolvedExternalId)
			{
				SessionId.Empty();
			}
		}
		else if (KnownSessionAgent != AgentName)
		{
			SessionId.Empty();
		}
	}

	if (SessionId.IsEmpty())
	{
		FScopeLock Lock(&ClientLock);
		if (TSharedPtr<FACPClient>* ClientPtr = ActiveClients.Find(AgentName))
		{
			SessionId = (*ClientPtr)->GetUnrealSessionId();
			if (!SessionId.IsEmpty() && !Request.SessionId.IsEmpty() && Request.SessionId != SessionId)
			{
				FACPSessionManager::Get().SetSessionExternalId(SessionId, Request.SessionId);
			}
		}
	}
	if (SessionId.IsEmpty())
	{
		SessionId = FindSessionForAgent(AgentName);
	}
	OnAgentPermissionRequest.Broadcast(SessionId, AgentName, Request);
}

void FACPAgentManager::RespondToPermissionRequest(const FString& AgentName, int32 RequestId, const FString& OptionId, TSharedPtr<FJsonObject> OutcomeMeta)
{
	TSharedPtr<FACPClient> Client = GetClient(AgentName);
	if (Client.IsValid())
	{
		Client->RespondToPermissionRequest(RequestId, OptionId, OutcomeMeta);
	}
	else
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAgentManager: Cannot respond to permission - agent not connected: %s"), *AgentName);
	}
}

FACPSessionModeState FACPAgentManager::GetAgentModeState(const FString& AgentName) const
{
	FScopeLock Lock(&ClientLock);
	if (const TSharedPtr<FACPClient>* ClientPtr = ActiveClients.Find(AgentName))
	{
		if (ClientPtr->IsValid())
		{
			return (*ClientPtr)->GetModeState();
		}
	}
	return FACPSessionModeState();
}

void FACPAgentManager::SetAgentMode(const FString& AgentName, const FString& ModeId)
{
	TSharedPtr<FACPClient> Client;
	{
		FScopeLock Lock(&ClientLock);
		if (TSharedPtr<FACPClient>* ClientPtr = ActiveClients.Find(AgentName))
		{
			Client = *ClientPtr;
		}
	}

	if (Client.IsValid())
	{
		Client->SetMode(ModeId);
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Set mode for %s to %s"), *AgentName, *ModeId);
	}
	else
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAgentManager: Cannot set mode - agent not connected: %s"), *AgentName);
	}
}

void FACPAgentManager::CancelAgentPrompt(const FString& AgentName)
{
	// Check Chat Gateway
	if (IsChatGatewayAgent(AgentName))
	{
		TSharedPtr<FChatGatewayClient> Client;
		{
			FScopeLock Lock(&ClientLock);
			if (TSharedPtr<FChatGatewayClient>* ClientPtr = ActiveChatGatewayClients.Find(AgentName))
			{
				Client = *ClientPtr;
			}
		}

		if (Client.IsValid())
		{
			Client->CancelPrompt();
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Cancelled prompt for Chat Gateway"));
		}
		else
		{
			UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAgentManager: Cannot cancel - Chat Gateway not connected"));
		}
		return;
	}

	// ACP client
	TSharedPtr<FACPClient> Client;
	{
		FScopeLock Lock(&ClientLock);
		if (TSharedPtr<FACPClient>* ClientPtr = ActiveClients.Find(AgentName))
		{
			Client = *ClientPtr;
		}
	}

	if (Client.IsValid())
	{
		Client->CancelPrompt();
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Cancelled prompt for %s"), *AgentName);
	}
	else
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAgentManager: Cannot cancel - agent not connected: %s"), *AgentName);
	}
}

void FACPAgentManager::StartNewSession(const FString& AgentName)
{
	// Check Chat Gateway
	if (IsChatGatewayAgent(AgentName))
	{
		TSharedPtr<FChatGatewayClient> Client;
		{
			FScopeLock Lock(&ClientLock);
			if (TSharedPtr<FChatGatewayClient>* ClientPtr = ActiveChatGatewayClients.Find(AgentName))
			{
				Client = *ClientPtr;
			}
		}

		if (Client.IsValid())
		{
			Client->NewSession(FPaths::ProjectDir());
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Started new session for Chat Gateway"));
		}
		else
		{
			UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAgentManager: Cannot start new session - Chat Gateway not connected"));
		}
		return;
	}

	// ACP client
	TSharedPtr<FACPClient> Client;
	{
		FScopeLock Lock(&ClientLock);
		if (TSharedPtr<FACPClient>* ClientPtr = ActiveClients.Find(AgentName))
		{
			Client = *ClientPtr;
		}
	}

	if (Client.IsValid())
	{
		Client->NewSession(FPaths::ProjectDir());
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Started new session for %s"), *AgentName);
	}
	else
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAgentManager: Cannot start new session - agent not connected: %s"), *AgentName);
	}
}

void FACPAgentManager::OnClientModesAvailable(const FString& AgentName, const FACPSessionModeState& ModeState)
{
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Modes available for %s - %d modes, current: %s"),
		*AgentName, ModeState.AvailableModes.Num(), *ModeState.CurrentModeId);

	// Reapply persisted mode selection if available and valid.
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		const FString SavedMode = Settings->GetSavedModeForAgent(AgentName);
		if (!SavedMode.IsEmpty() && SavedMode != ModeState.CurrentModeId)
		{
			bool bSavedModeAvailable = false;
			for (const FACPSessionMode& Mode : ModeState.AvailableModes)
			{
				if (Mode.ModeId == SavedMode)
				{
					bSavedModeAvailable = true;
					break;
				}
			}

			if (bSavedModeAvailable)
			{
				SetAgentMode(AgentName, SavedMode);
			}
		}
	}

	FString SessionId;
	{
		FScopeLock Lock(&ClientLock);
		if (TSharedPtr<FACPClient>* ClientPtr = ActiveClients.Find(AgentName))
		{
			SessionId = (*ClientPtr)->GetUnrealSessionId();
		}
	}
	if (SessionId.IsEmpty())
	{
		SessionId = FindSessionForAgent(AgentName);
	}
	OnAgentModesAvailable.Broadcast(SessionId, AgentName, ModeState);
}

void FACPAgentManager::OnClientModeChanged(const FString& AgentName, const FString& ModeId)
{
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Mode changed for %s to %s"), *AgentName, *ModeId);

	FString SessionId;
	{
		FScopeLock Lock(&ClientLock);
		if (TSharedPtr<FACPClient>* ClientPtr = ActiveClients.Find(AgentName))
		{
			SessionId = (*ClientPtr)->GetUnrealSessionId();
		}
	}
	if (SessionId.IsEmpty())
	{
		SessionId = FindSessionForAgent(AgentName);
	}
	OnAgentModeChanged.Broadcast(SessionId, AgentName, ModeId);
}

void FACPAgentManager::OnClientCommandsAvailable(const FString& AgentName, const TArray<FACPSlashCommand>& Commands)
{
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: %d commands available for %s"), Commands.Num(), *AgentName);

	FString SessionId;
	{
		FScopeLock Lock(&ClientLock);
		if (TSharedPtr<FACPClient>* ClientPtr = ActiveClients.Find(AgentName))
		{
			SessionId = (*ClientPtr)->GetUnrealSessionId();
		}
	}
	if (SessionId.IsEmpty())
	{
		SessionId = FindSessionForAgent(AgentName);
	}
	OnAgentCommandsAvailable.Broadcast(SessionId, AgentName, Commands);
}

// ============================================================================
// Chat Gateway-specific handlers
// ============================================================================

void FACPAgentManager::OnChatGatewaySessionUpdate(const FString& AgentName, const FACPSessionUpdate& Update)
{
	// Get the Unreal SessionId directly from the client for accurate routing
	FString SessionId;
	{
		FScopeLock Lock(&ClientLock);
		if (TSharedPtr<FChatGatewayClient>* ClientPtr = ActiveChatGatewayClients.Find(AgentName))
		{
			SessionId = (*ClientPtr)->GetUnrealSessionId();
		}
	}

	// Fall back to lookup if client doesn't have a tracked session
	if (SessionId.IsEmpty())
	{
		SessionId = FindSessionForAgent(AgentName);
	}

	// Clear streaming state on error
	if (Update.UpdateType == EACPUpdateType::Error)
	{
		FScopeLock Lock(&SessionLock);
		if (FAgentSessionContext* Context = ActiveSessions.Find(SessionId))
		{
			Context->bIsStreaming = false;
		}
	}

	// Broadcast plan updates separately for UI convenience
	if (Update.UpdateType == EACPUpdateType::Plan && Update.Plan.Entries.Num() > 0)
	{
		OnAgentPlanUpdate.Broadcast(SessionId, AgentName, Update.Plan);
	}

	OnAgentMessage.Broadcast(SessionId, AgentName, Update);
}

void FACPAgentManager::OnChatGatewayStateChanged(const FString& AgentName, EACPClientState State, const FString& Message)
{
	// Get the Unreal SessionId directly from the client for accurate routing
	FString SessionId;
	{
		FScopeLock Lock(&ClientLock);
		if (TSharedPtr<FChatGatewayClient>* ClientPtr = ActiveChatGatewayClients.Find(AgentName))
		{
			SessionId = (*ClientPtr)->GetUnrealSessionId();
		}
	}

	// Fall back to lookup if client doesn't have a tracked session
	if (SessionId.IsEmpty())
	{
		SessionId = FindSessionForAgent(AgentName);
	}

	// Clear streaming state when agent is no longer prompting
	if (State != EACPClientState::Prompting && !SessionId.IsEmpty())
	{
		FScopeLock Lock(&SessionLock);
		if (FAgentSessionContext* Context = ActiveSessions.Find(SessionId))
		{
			Context->bIsStreaming = false;
		}
	}

	// When Chat Gateway becomes Ready, check if we have pending prompts and create session
	if (State == EACPClientState::Ready)
	{
		bool bHasPendingPrompt = false;
		{
			FScopeLock Lock(&ClientLock);
			for (const auto& Pair : PendingPrompts)
			{
				FString PendingAgent = GetSessionAgent(Pair.Key);
				if (PendingAgent == AgentName || Pair.Key == AgentName)
				{
					bHasPendingPrompt = true;
					break;
				}
			}
		}

		if (bHasPendingPrompt)
		{
			TSharedPtr<FChatGatewayClient> Client;
			{
				FScopeLock Lock(&ClientLock);
				if (TSharedPtr<FChatGatewayClient>* ClientPtr = ActiveChatGatewayClients.Find(AgentName))
				{
					Client = *ClientPtr;
				}
			}

			if (Client.IsValid())
			{
				UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Chat Gateway ready, creating session for pending prompt"));
				Client->NewSession(FPaths::ProjectDir());
			}
		}
	}
	// When session is established, store agent→unreal ID mapping and send pending prompts
	else if (State == EACPClientState::InSession)
	{
		// Store the agent's session ID in the active session metadata so we can
		// deduplicate against remote session lists (which use agent IDs)
			{
				FScopeLock Lock(&ClientLock);
				if (TSharedPtr<FChatGatewayClient>* ClientPtr = ActiveChatGatewayClients.Find(AgentName))
				{
					FString AgentSessId = (*ClientPtr)->GetCurrentSessionId();
					FString UnrealSessId = (*ClientPtr)->GetUnrealSessionId();
					if (!AgentSessId.IsEmpty() && !UnrealSessId.IsEmpty())
					{
						FACPSessionManager::Get().SetSessionExternalId(UnrealSessId, AgentSessId);
					}
				}
			}

		FString CurrentClientSessionId = SessionId;
		TArray<FString> KeysToRemove;
		TArray<FString> PromptsToSend;
		FString NextSessionToActivate;

		{
			FScopeLock Lock(&ClientLock);
			for (const auto& Pair : PendingPrompts)
			{
				FString PendingAgent = GetSessionAgent(Pair.Key);
				const bool bLegacyAgentQueue = Pair.Key == AgentName;
				const bool bBelongsToAgent = PendingAgent == AgentName || bLegacyAgentQueue;
				if (!bBelongsToAgent)
				{
					continue;
				}

				if (bLegacyAgentQueue
					|| (!CurrentClientSessionId.IsEmpty() && Pair.Key == CurrentClientSessionId))
				{
					PromptsToSend.Add(Pair.Value);
					KeysToRemove.Add(Pair.Key);
				}
				else if (!bLegacyAgentQueue && NextSessionToActivate.IsEmpty())
				{
					NextSessionToActivate = Pair.Key;
				}
			}
			for (const FString& Key : KeysToRemove)
			{
				PendingPrompts.Remove(Key);
			}
		}

		if (PromptsToSend.Num() > 0)
		{
			TSharedPtr<FChatGatewayClient> Client;
			{
				FScopeLock Lock(&ClientLock);
				if (TSharedPtr<FChatGatewayClient>* ClientPtr = ActiveChatGatewayClients.Find(AgentName))
				{
					Client = *ClientPtr;
				}
			}

			if (Client.IsValid())
			{
				UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Sending %d queued prompt(s) for Chat Gateway"), PromptsToSend.Num());
				for (const FString& Prompt : PromptsToSend)
				{
					Client->SendPrompt(Prompt);
				}
			}
		}
		else if (!NextSessionToActivate.IsEmpty())
		{
			TSharedPtr<FChatGatewayClient> Client;
			{
				FScopeLock Lock(&ClientLock);
				if (TSharedPtr<FChatGatewayClient>* ClientPtr = ActiveChatGatewayClients.Find(AgentName))
				{
					Client = *ClientPtr;
				}
			}
			if (Client.IsValid())
			{
				Client->SetUnrealSessionId(NextSessionToActivate);
				Client->NewSession(FPaths::ProjectDir());
			}
		}
	}

	// Re-read actual client state: sending pending prompts above may have transitioned
	// the client to Prompting. Broadcasting the stale InSession state would cause the UI
	// to show a false "finished responding" notification.
	EACPClientState ActualState = State;
	{
		FScopeLock Lock(&ClientLock);
		if (TSharedPtr<FChatGatewayClient>* ClientPtr = ActiveChatGatewayClients.Find(AgentName))
		{
			if (ClientPtr->IsValid())
			{
				ActualState = (*ClientPtr)->GetState();
			}
		}
	}
	OnAgentStateChanged.Broadcast(SessionId, AgentName, ActualState, Message);
}

void FACPAgentManager::OnChatGatewayModelsAvailable(const FString& AgentName, const FACPSessionModelState& ModelState)
{
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Chat Gateway models available - %d models, current: %s"),
		ModelState.AvailableModels.Num(), *ModelState.CurrentModelId);

	// Reapply persisted model selection.
	// Chat Gateway accepts any valid model ID at request time, so we don't
	// gate on the model being present in the curated shortlist — the saved
	// model may have been picked from "Browse all models" and won't appear
	// in the hardcoded curated list on a fresh launch.
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		const FString SavedModel = Settings->GetSavedModelForAgent(AgentName);
		if (!SavedModel.IsEmpty() && SavedModel != ModelState.CurrentModelId)
		{
			SetAgentModel(AgentName, SavedModel);
		}
	}

	FString SessionId;
	{
		FScopeLock Lock(&ClientLock);
		if (TSharedPtr<FChatGatewayClient>* ClientPtr = ActiveChatGatewayClients.Find(AgentName))
		{
			SessionId = (*ClientPtr)->GetUnrealSessionId();
		}
	}
	if (SessionId.IsEmpty())
	{
		SessionId = FindSessionForAgent(AgentName);
	}
	OnAgentModelsAvailable.Broadcast(SessionId, AgentName, ModelState);
}

// ============================================================================
// Session-aware methods for parallel chat support
// ============================================================================

void FACPAgentManager::QueuePendingNewSession(const FString& AgentName, const FString& SessionId)
{
	FScopeLock Lock(&ClientLock);
	PendingNewSessions.Add(AgentName, SessionId);
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Queued pending NewSession for %s (session: %s)"), *AgentName, *SessionId);
}

void FACPAgentManager::RegisterSession(const FString& SessionId, const FString& AgentName)
{
	FScopeLock Lock(&SessionLock);

	FAgentSessionContext Context;
	Context.SessionId = SessionId;
	Context.AgentName = AgentName;
	Context.bIsStreaming = false;

	ActiveSessions.Add(SessionId, Context);

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Registered session %s for agent %s"), *SessionId, *AgentName);
}

void FACPAgentManager::UnregisterSession(const FString& SessionId)
{
	FScopeLock Lock(&SessionLock);
	ActiveSessions.Remove(SessionId);

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPAgentManager: Unregistered session %s"), *SessionId);
}

FString FACPAgentManager::GetSessionAgent(const FString& SessionId) const
{
	FScopeLock Lock(&SessionLock);

	if (const FAgentSessionContext* Context = ActiveSessions.Find(SessionId))
	{
		return Context->AgentName;
	}
	return FString();
}

FString FACPAgentManager::GetActiveSessionForAgent(const FString& AgentName) const
{
	FScopeLock Lock(&SessionLock);

	// Find a session that's currently streaming for this agent, or return the first one
	FString FirstSession;
	for (const auto& Pair : ActiveSessions)
	{
		if (Pair.Value.AgentName == AgentName)
		{
			if (Pair.Value.bIsStreaming)
			{
				return Pair.Key;
			}
			if (FirstSession.IsEmpty())
			{
				FirstSession = Pair.Key;
			}
		}
	}
	return FirstSession;
}

FString FACPAgentManager::FindSessionForAgent(const FString& AgentName) const
{
	return GetActiveSessionForAgent(AgentName);
}

void FACPAgentManager::SendPromptToSession(const FString& SessionId, const FString& AgentName, const FString& PromptText)
{
	// Mark session as streaming
	{
		FScopeLock Lock(&SessionLock);
		if (FAgentSessionContext* Context = ActiveSessions.Find(SessionId))
		{
			Context->bIsStreaming = true;
		}
	}

	// Handle Chat Gateway specially
	if (IsChatGatewayAgent(AgentName))
	{
		TSharedPtr<FChatGatewayClient> Client;
		{
			FScopeLock Lock(&ClientLock);
			if (TSharedPtr<FChatGatewayClient>* ClientPtr = ActiveChatGatewayClients.Find(AgentName))
			{
				Client = *ClientPtr;
			}
		}

		if (!Client.IsValid())
		{
			if (!ConnectToAgent(AgentName))
			{
				UE_LOG(LogAgentIntegrationKit, Error, TEXT("ACPAgentManager: Cannot send prompt - failed to connect to Chat Gateway"));
				{
					FScopeLock Lock(&SessionLock);
					if (FAgentSessionContext* Context = ActiveSessions.Find(SessionId))
					{
						Context->bIsStreaming = false;
					}
				}
				OnAgentError.Broadcast(SessionId, AgentName, -32001, TEXT("Failed to connect to OpenRouter. Verify configuration and try again."));
				return;
			}

			FScopeLock Lock(&ClientLock);
			if (TSharedPtr<FChatGatewayClient>* ClientPtr = ActiveChatGatewayClients.Find(AgentName))
			{
				Client = *ClientPtr;
			}
		}

		if (Client.IsValid())
		{
			EACPClientState CurrentState = Client->GetState();

			// Check if client is serving a different Unreal session - if so, reset it
			FString ClientUnrealSession = Client->GetUnrealSessionId();
			bool bSessionMismatch = !ClientUnrealSession.IsEmpty() && ClientUnrealSession != SessionId;

			if (CurrentState == EACPClientState::InSession && !bSessionMismatch)
			{
				// Already in session for this Unreal session - just send prompt
				// But first ensure we track the session (in case it wasn't set)
				if (ClientUnrealSession.IsEmpty())
				{
					Client->SetUnrealSessionId(SessionId);
				}
				Client->SendPrompt(PromptText);
			}
				else if (CurrentState == EACPClientState::Ready
					|| (bSessionMismatch && CurrentState == EACPClientState::InSession))
				{
					// Need new session: either not in session yet, or different Unreal session
				{
					FScopeLock Lock(&ClientLock);
					PendingPrompts.Add(SessionId, PromptText);
				}
				Client->SetUnrealSessionId(SessionId);
				Client->NewSession(FPaths::ProjectDir());
			}
			else
			{
				FScopeLock Lock(&ClientLock);
				PendingPrompts.Add(SessionId, PromptText);
			}
		}
		return;
	}

	// Standard ACP client
	TSharedPtr<FACPClient> Client = GetClient(AgentName);
	if (!Client.IsValid())
	{
		if (!ConnectToAgent(AgentName))
		{
			UE_LOG(LogAgentIntegrationKit, Error, TEXT("ACPAgentManager: Cannot send prompt - failed to connect to agent: %s"), *AgentName);
			{
				FScopeLock Lock(&SessionLock);
				if (FAgentSessionContext* Context = ActiveSessions.Find(SessionId))
				{
					Context->bIsStreaming = false;
				}
			}
			OnAgentError.Broadcast(SessionId, AgentName, -32001, FString::Printf(
				TEXT("Failed to connect to %s. Make sure the CLI is installed and authenticated, then try again."),
				*AgentName));
			return;
		}
		Client = GetClient(AgentName);
	}

	if (Client.IsValid())
	{
		EACPClientState CurrentState = Client->GetState();

		FString WorkingDirectory = FPaths::ProjectDir();
		bool bIsResumedSession = false;
		if (const FACPActiveSession* Session = FACPSessionManager::Get().GetActiveSession(SessionId))
		{
			bIsResumedSession = Session->bIsLoadingHistory;
		}

		// Check if client is serving a different Unreal session - if so, reset it
		FString ClientUnrealSession = Client->GetUnrealSessionId();
		bool bSessionMismatch = !ClientUnrealSession.IsEmpty() && ClientUnrealSession != SessionId;

		if (CurrentState == EACPClientState::InSession && !bSessionMismatch)
		{
			// Already in session for this Unreal session - just send prompt
			// But first ensure we track the session (in case it wasn't set)
			if (ClientUnrealSession.IsEmpty())
			{
				Client->SetUnrealSessionId(SessionId);
			}
			Client->SendPrompt(PromptText);
		}
			else if (CurrentState == EACPClientState::Ready
				|| (bSessionMismatch && CurrentState == EACPClientState::InSession))
			{
				// Need new session: either not in session yet, or different Unreal session
			{
				FScopeLock Lock(&ClientLock);
				PendingPrompts.Add(SessionId, PromptText);
			}
			Client->SetUnrealSessionId(SessionId);
			if (bIsResumedSession)
			{
				// Session was resumed from ACP list — SessionId IS the agent's session ID
				const FACPAgentCapabilities& Caps = Client->GetAgentCapabilities();
				if (Caps.bSupportsResumeSession)
				{
					Client->ResumeSession(SessionId, WorkingDirectory);
				}
				else if (Caps.bSupportsLoadSession)
				{
					Client->LoadSession(SessionId, WorkingDirectory);
				}
				else
				{
					Client->NewSession(WorkingDirectory);
				}
			}
			else
			{
				Client->NewSession(WorkingDirectory);
			}
		}
		else
		{
			FScopeLock Lock(&ClientLock);
			PendingPrompts.Add(SessionId, PromptText);
		}
	}
}

void FACPAgentManager::CancelSessionPrompt(const FString& SessionId)
{
	FString AgentName;
	{
		FScopeLock Lock(&SessionLock);
		if (const FAgentSessionContext* Context = ActiveSessions.Find(SessionId))
		{
			AgentName = Context->AgentName;
		}
	}

	if (AgentName.IsEmpty())
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPAgentManager: Cannot cancel - session not found: %s"), *SessionId);
		return;
	}

	// Mark session as not streaming
	{
		FScopeLock Lock(&SessionLock);
		if (FAgentSessionContext* Context = ActiveSessions.Find(SessionId))
		{
			Context->bIsStreaming = false;
		}
	}

	CancelAgentPrompt(AgentName);
}

void FACPAgentManager::StartSessionConversation(const FString& SessionId, const FString& AgentName)
{
	// Register the session if not already registered
	{
		FScopeLock Lock(&SessionLock);
		if (!ActiveSessions.Contains(SessionId))
		{
			FAgentSessionContext Context;
			Context.SessionId = SessionId;
			Context.AgentName = AgentName;
			Context.bIsStreaming = false;
			ActiveSessions.Add(SessionId, Context);
		}
	}

	StartNewSession(AgentName);
}

// ── Authentication ──────────────────────────────────────────────────

TArray<FACPAuthMethod> FACPAgentManager::GetAuthMethods(const FString& AgentName) const
{
	FScopeLock Lock(&ClientLock);
	const TSharedPtr<FACPClient>* ClientPtr = ActiveClients.Find(AgentName);
	if (ClientPtr && ClientPtr->IsValid())
	{
		return (*ClientPtr)->GetAgentCapabilities().AuthMethods;
	}
	return TArray<FACPAuthMethod>();
}

void FACPAgentManager::AuthenticateAgent(const FString& AgentName, const FString& MethodId)
{
	// Find the auth method to determine mechanism
	TArray<FACPAuthMethod> Methods = GetAuthMethods(AgentName);
	const FACPAuthMethod* TargetMethod = nullptr;

	for (const FACPAuthMethod& M : Methods)
	{
		if (M.Id == MethodId)
		{
			TargetMethod = &M;
			break;
		}
	}

	if (!TargetMethod)
	{
		// Terminal-first auth even when the adapter did not provide terminal metadata.
		FACPAuthMethod FallbackMethod;
		FallbackMethod.Id = MethodId;
		SpawnTerminalAuth(AgentName, FallbackMethod);
		return;
	}

	// Always use terminal-first auth. This is more reliable for interactive OAuth flows.
	SpawnTerminalAuth(AgentName, *TargetMethod);
}

void FACPAgentManager::SpawnTerminalAuth(const FString& AgentName, const FACPAuthMethod& Method)
{
	// Clean up any previous auth process bookkeeping.
	if (AuthTickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(AuthTickerHandle);
		AuthTickerHandle.Reset();
	}
	if (AuthProcessHandle.IsValid())
	{
		FPlatformProcess::CloseProc(AuthProcessHandle);
	}

	AuthenticatingAgentName = AgentName;

	// Resolve terminal command from auth method metadata, then fallback to the agent's configured executable.
	FString Command = Method.TerminalAuthCommand;
	TArray<FString> Args = Method.TerminalAuthArgs;

	if (Command.IsEmpty())
	{
		if (FACPAgentConfig* Config = GetAgentConfig(AgentName))
		{
			Command = Config->ExecutablePath;
			Args = Config->Arguments;
		}
	}

	if (Command.IsEmpty())
	{
		FString SessionId = FindSessionForAgent(AgentName);
		OnAgentAuthComplete.Broadcast(SessionId, AgentName, false, TEXT("No terminal auth command available for this agent"));
		return;
	}

	FString CommandLine = BuildQuotedCommandLine(Command, Args);
	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPAgentManager: Launching terminal-auth for %s: %s"), *AgentName, *CommandLine);

	FString LaunchError;
	if (!LaunchExternalAuthTerminal(AgentName, CommandLine, LaunchError))
	{
		FString SessionId = FindSessionForAgent(AgentName);
		OnAgentAuthComplete.Broadcast(SessionId, AgentName, false,
			LaunchError.IsEmpty() ? TEXT("Failed to launch external login terminal") : LaunchError);
		return;
	}

	// External terminal ownership: report launch success and let users complete login there.
	const FString SessionId = FindSessionForAgent(AgentName);
	OnAgentAuthComplete.Broadcast(SessionId, AgentName, true, TEXT(""));
	AuthenticatingAgentName.Empty();
}

bool FACPAgentManager::LaunchExternalAuthTerminal(const FString& AgentName, const FString& CommandLine, FString& OutError) const
{
	OutError.Empty();
	if (CommandLine.IsEmpty())
	{
		OutError = TEXT("Auth command is empty.");
		return false;
	}

	const FString ScriptRootDir = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("AgentIntegrationKit"), TEXT("auth"));
	IFileManager::Get().MakeDirectory(*ScriptRootDir, true);

	const FString SafeAgent = MakeSafeFileStem_AgentMgr(AgentName.IsEmpty() ? TEXT("agent") : AgentName);
	const FString UniqueSuffix = FString::Printf(TEXT("%lld"), FDateTime::UtcNow().GetTicks());

	FString ScriptPath;
	FString ScriptContent;
	FString LaunchExe;
	FString LaunchArgs;

#if PLATFORM_WINDOWS
	ScriptPath = FPaths::Combine(ScriptRootDir, FString::Printf(TEXT("auth-%s-%s.bat"), *SafeAgent, *UniqueSuffix));
	FPaths::MakePlatformFilename(ScriptPath);
	const FString WinAgentName = AgentName.IsEmpty() ? FString(TEXT("Agent")) : AgentName;

	ScriptContent += TEXT("@echo off\r\n");
	ScriptContent += TEXT("echo.\r\n");
	ScriptContent += TEXT("echo ============================================\r\n");
	ScriptContent += TEXT("echo   Agent Integration Kit - Authentication\r\n");
	ScriptContent += TEXT("echo ============================================\r\n");
	ScriptContent += FString::Printf(TEXT("echo Agent: %s\r\n"), *WinAgentName);
	ScriptContent += TEXT("echo.\r\n");
	ScriptContent += TEXT("echo Complete sign-in in this terminal/browser window.\r\n");
	ScriptContent += TEXT("echo.\r\n");
	ScriptContent += CommandLine + TEXT("\r\n");
	ScriptContent += TEXT("echo.\r\n");
	ScriptContent += TEXT("echo Return to Unreal and continue chatting.\r\n");
	ScriptContent += TEXT("pause\r\n");

	LaunchExe = TEXT("cmd.exe");
	LaunchArgs = FString::Printf(TEXT("/c start \"AIK Auth\" cmd.exe /k \"\"%s\"\""), *ScriptPath);
#elif PLATFORM_MAC || PLATFORM_LINUX
	ScriptPath = FPaths::Combine(ScriptRootDir, FString::Printf(TEXT("auth-%s-%s.sh"), *SafeAgent, *UniqueSuffix));
	const FString UnixAgentName = AgentName.IsEmpty() ? FString(TEXT("Agent")) : AgentName;

	ScriptContent += TEXT("#!/bin/bash\n");
	ScriptContent += TEXT("echo\n");
	ScriptContent += TEXT("echo '============================================'\n");
	ScriptContent += TEXT("echo '  Agent Integration Kit - Authentication'\n");
	ScriptContent += TEXT("echo '============================================'\n");
	ScriptContent += FString::Printf(TEXT("echo 'Agent: %s'\n"), *UnixAgentName);
	ScriptContent += TEXT("echo\n");
	ScriptContent += TEXT("echo 'Complete sign-in in this terminal/browser window.'\n");
	ScriptContent += TEXT("echo\n");
	ScriptContent += CommandLine + TEXT("\n");
	ScriptContent += TEXT("echo\n");
	ScriptContent += TEXT("echo 'If sign-in did not start automatically, run the same command again here.'\n");
	ScriptContent += TEXT("echo 'Then return to Unreal and continue chatting.'\n");
	ScriptContent += TEXT("echo\n");
	ScriptContent += TEXT("read -r -p 'Press Enter to close...' _\n");

	FPaths::NormalizeFilename(ScriptPath);
#if PLATFORM_MAC
	LaunchExe = TEXT("/usr/bin/open");
	LaunchArgs = FString::Printf(TEXT("-a Terminal \"%s\""), *ScriptPath);
#else
	const FString LinuxLaunch = FString::Printf(
		TEXT("if command -v x-terminal-emulator >/dev/null 2>&1; then x-terminal-emulator -e \"%s\"; ")
		TEXT("elif command -v gnome-terminal >/dev/null 2>&1; then gnome-terminal -- \"%s\"; ")
		TEXT("elif command -v konsole >/dev/null 2>&1; then konsole -e \"%s\"; ")
		TEXT("elif command -v xterm >/dev/null 2>&1; then xterm -e \"%s\"; ")
		TEXT("else exit 127; fi"),
		*ScriptPath, *ScriptPath, *ScriptPath, *ScriptPath);
	LaunchExe = TEXT("/bin/bash");
	FString EscapedLaunch = LinuxLaunch;
	EscapedLaunch.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	EscapedLaunch.ReplaceInline(TEXT("\""), TEXT("\\\""));
	EscapedLaunch.ReplaceInline(TEXT("`"), TEXT("\\`"));
	LaunchArgs = FString::Printf(TEXT("-l -c \"%s\""), *EscapedLaunch);
#endif
#else
	OutError = TEXT("External terminal auth is not supported on this platform.");
	return false;
#endif

	if (!FFileHelper::SaveStringToFile(ScriptContent, *ScriptPath))
	{
		OutError = FString::Printf(TEXT("Failed to write auth script: %s"), *ScriptPath);
		return false;
	}

#if PLATFORM_MAC || PLATFORM_LINUX
	FPlatformProcess::ExecProcess(TEXT("/bin/chmod"), *FString::Printf(TEXT("+x \"%s\""), *ScriptPath), nullptr, nullptr, nullptr);
#endif

	FProcHandle ProcHandle = FPlatformProcess::CreateProc(
		*LaunchExe, *LaunchArgs,
		true, false, false,
		nullptr, 0, nullptr, nullptr);

	if (!ProcHandle.IsValid())
	{
		OutError = FString::Printf(TEXT("Failed to launch auth terminal: %s %s"), *LaunchExe, *LaunchArgs);
		return false;
	}

	return true;
}
