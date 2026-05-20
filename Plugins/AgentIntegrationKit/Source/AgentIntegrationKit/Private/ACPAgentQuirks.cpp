// Copyright 2026 Betide Studio. All Rights Reserved.

#include "ACPAgentQuirks.h"

// ============================================================================
// Default quirks (no special behavior — pure ACP)
// ============================================================================

static const FACPAgentQuirks GDefaultQuirks;

// ============================================================================
// Quirks Map
// ============================================================================

TMap<FString, FACPAgentQuirks>& FACPAgentQuirksMap::GetMap()
{
	static TMap<FString, FACPAgentQuirks> Map;
	static bool bInitialized = false;
	if (!bInitialized)
	{
		InitializeMap(Map);
		bInitialized = true;
	}
	return Map;
}

void FACPAgentQuirksMap::InitializeMap(TMap<FString, FACPAgentQuirks>& Map)
{
	// ────────────────────────────────────────────────────────────────
	// GitHub Copilot CLI
	// Needs MCP server injected via --additional-mcp-config @<path>
	// Resumes sessions at process launch via --resume flag
	// ────────────────────────────────────────────────────────────────
	{
		FACPAgentQuirks Q;
		Q.MCPInjection = EMCPInjectionStrategy::CliConfigFile;
		Q.MCPCliFlag = TEXT("--additional-mcp-config");
		Q.bMCPCliPrefixAt = true;
		Q.MCPConfigTemplate = TEXT(R"({"mcpServers":{"unreal-editor":{"type":"http","url":"{url}","tools":["*"]}}})");
		Q.ResumeStrategy = EResumeStrategy::LaunchArg;
		Q.ResumeLaunchFlag = TEXT("--resume");
		Map.Add(TEXT("github-copilot"), Q);
		Map.Add(TEXT("github-copilot-cli"), Q);
	}

	// ────────────────────────────────────────────────────────────────
	// Gemini CLI
	// Needs MCP server injected via GEMINI_CLI_SYSTEM_SETTINGS_PATH env var
	// Resumes sessions at process launch via --resume flag
	// Model changes are local-only (model selected at launch time)
	// ────────────────────────────────────────────────────────────────
	{
		FACPAgentQuirks Q;
		Q.MCPInjection = EMCPInjectionStrategy::EnvVarConfigFile;
		Q.MCPEnvVarName = TEXT("GEMINI_CLI_SYSTEM_SETTINGS_PATH");
		Q.MCPConfigTemplate = TEXT(R"({"mcpServers":{"unreal-editor":{"httpUrl":"{url}"}}})");
		Q.ResumeStrategy = EResumeStrategy::LaunchArg;
		Q.ResumeLaunchFlag = TEXT("--resume");
		Q.bModelChangesLocalOnly = true;
		Map.Add(TEXT("gemini"), Q);
	}

	// ────────────────────────────────────────────────────────────────
	// OpenCode
	// System prompt must be delivered via first user message
	// (it ignores _meta.systemPrompt in session/new)
	// ────────────────────────────────────────────────────────────────
	{
		FACPAgentQuirks Q;
		Q.SystemPromptDeliveryOverride = TEXT("FirstUserMessage");
		Map.Add(TEXT("opencode"), Q);
	}

	// ────────────────────────────────────────────────────────────────
	// OpenRouter (built-in Chat Gateway, not ACP subprocess)
	// ────────────────────────────────────────────────────────────────
	{
		FACPAgentQuirks Q;
		Q.Transport = EAgentTransport::ChatGateway;
		Q.MCPInjection = EMCPInjectionStrategy::None;
		Map.Add(TEXT("openrouter"), Q);
	}

	// All other agents (codex-acp, claude-acp, cursor, cline, goose, kimi,
	// mistral-vibe, qwen-code, etc.) use pure ACP defaults — no quirks needed.
}

const FACPAgentQuirks& FACPAgentQuirksMap::GetQuirks(const FString& RegistryId)
{
	const TMap<FString, FACPAgentQuirks>& Map = GetMap();
	if (const FACPAgentQuirks* Found = Map.Find(RegistryId))
	{
		return *Found;
	}
	return GDefaultQuirks;
}

bool FACPAgentQuirksMap::HasQuirks(const FString& RegistryId)
{
	return GetMap().Contains(RegistryId);
}
