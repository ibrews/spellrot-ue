// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// ============================================================================
// Agent Quirks — Declarative per-agent behavior overrides
//
// These encode the small number of agent-specific behaviors that are NOT
// covered by the ACP protocol or the registry manifest. Instead of scattered
// if(AgentName == "X") checks, quirks are declared as data.
// ============================================================================

/** How the plugin injects its MCP server into the agent process */
enum class EMCPInjectionStrategy : uint8
{
	/** Injected via ACP session/new mcpServers[] param (standard, most agents) */
	ACPSessionParam,

	/** Injected via a per-process JSON config file passed as a CLI argument */
	CliConfigFile,

	/** Injected via an environment variable pointing to a JSON config file */
	EnvVarConfigFile,

	/** No injection — agent discovers MCP server on its own (e.g., reads from its own config) */
	None,
};

/** How the agent resumes a previous session */
enum class EResumeStrategy : uint8
{
	/** Standard ACP: send session/resume after initialize (default) */
	ACPMethod,

	/** Resume flag passed at process launch time (--resume <id>) */
	LaunchArg,

	/** No resume support */
	None,
};

/** Transport type for the agent */
enum class EAgentTransport : uint8
{
	/** Standard ACP: JSON-RPC over stdio (default, most agents) */
	ACPStdio,

	/** Built-in HTTP API client (OpenRouter, DeepSeek, etc.) — no subprocess */
	ChatGateway,
};

/** Declarative quirks for a specific agent */
struct FACPAgentQuirks
{
	// ── MCP Injection ──

	EMCPInjectionStrategy MCPInjection = EMCPInjectionStrategy::ACPSessionParam;

	/** For CliConfigFile: the CLI flag to pass (e.g., "--additional-mcp-config") */
	FString MCPCliFlag;

	/** For CliConfigFile: format template for the config JSON.
	 *  Placeholders: {url} = MCP server URL.
	 *  e.g., R"({"mcpServers":{"unreal-editor":{"type":"http","url":"{url}","tools":["*"]}}})" */
	FString MCPConfigTemplate;

	/** For CliConfigFile: whether to prefix the path with "@" (Copilot convention) */
	bool bMCPCliPrefixAt = false;

	/** For EnvVarConfigFile: the env var name (e.g., "GEMINI_CLI_SYSTEM_SETTINGS_PATH") */
	FString MCPEnvVarName;

	// ── Resume ──

	EResumeStrategy ResumeStrategy = EResumeStrategy::ACPMethod;

	/** For LaunchArg: the CLI flag (e.g., "--resume") */
	FString ResumeLaunchFlag;

	// ── Transport ──

	EAgentTransport Transport = EAgentTransport::ACPStdio;

	// ── System Prompt ──

	/** Override system prompt delivery method. Empty = use default (SessionMeta). */
	FString SystemPromptDeliveryOverride;

	// ── Distribution Override ──

	/** Override the binary archive URL template for this agent.
	 *  Used to redirect downloads to a fork's releases instead of the registry default.
	 *  Placeholders: {version} = registry version, {platform} = platform target suffix.
	 *  e.g., "https://github.com/betidestu/codex-acp/releases/download/v{version}/codex-acp-{version}-{platform}.tar.gz"
	 *  When set, applies to ALL platforms. The {platform} placeholder is resolved
	 *  from the registry's binary target keys (e.g., "aarch64-apple-darwin"). */
	FString BinaryArchiveUrlOverride;

	// ── Misc ──

	/** If true, model changes are local-only (not sent to agent via ACP).
	 *  Used when the agent selects model at launch time, not mid-session. */
	bool bModelChangesLocalOnly = false;
};

/**
 * Static registry of agent quirks.
 * Keyed by registry ID (e.g., "claude-acp", "gemini", "github-copilot").
 */
class AGENTINTEGRATIONKIT_API FACPAgentQuirksMap
{
public:
	/** Get quirks for an agent by registry ID. Returns default quirks if not found. */
	static const FACPAgentQuirks& GetQuirks(const FString& RegistryId);

	/** Check if an agent has any non-default quirks */
	static bool HasQuirks(const FString& RegistryId);

private:
	static TMap<FString, FACPAgentQuirks>& GetMap();
	static void InitializeMap(TMap<FString, FACPAgentQuirks>& Map);
};
