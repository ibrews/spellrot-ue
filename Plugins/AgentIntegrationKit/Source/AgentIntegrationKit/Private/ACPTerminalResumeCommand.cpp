// Copyright 2026 Betide Studio. All Rights Reserved.

#include "ACPTerminalResumeCommand.h"
#include "MCPServer.h"
#include "Dom/JsonObject.h"
#include "HAL/Platform.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

static FString ShellSingleQuoteUnix(const FString& S)
{
	FString Out = TEXT("'");
	for (int32 i = 0; i < S.Len(); ++i)
	{
		const TCHAR C = S[i];
		if (C == TEXT('\''))
		{
			Out += TEXT("'\\''");
		}
		else
		{
			Out.AppendChar(C);
		}
	}
	Out += TEXT("'");
	return Out;
}

static FString CmdDoubleQuoteWindows(const FString& S)
{
	FString Escaped = S.Replace(TEXT("\""), TEXT("\\\""));
	return FString::Printf(TEXT("\"%s\""), *Escaped);
}

FString FACPTerminalResumeCommand::QuoteSessionIdForShell(const FString& SessionId)
{
#if PLATFORM_WINDOWS
	return CmdDoubleQuoteWindows(SessionId);
#else
	return ShellSingleQuoteUnix(SessionId);
#endif
}

/** Same rules as session id — safe for paths with spaces (macOS/Linux). */
static FString QuotePathForShell(const FString& Path)
{
#if PLATFORM_WINDOWS
	return CmdDoubleQuoteWindows(Path);
#else
	return ShellSingleQuoteUnix(Path);
#endif
}

/**
 * Writes MCP config for external `claude` CLI (--mcp-config), matching FACPClient::NewSession:
 * Streamable HTTP at http://127.0.0.1:<port>/mcp (IPv4; avoids Node localhost/IPv6 issues).
 */
static bool WriteClaudeTerminalMcpConfigFile(FString& OutAbsolutePath)
{
	if (!FMCPServer::Get().IsRunning())
	{
		return false;
	}

	const int32 Port = FMCPServer::Get().GetPort();
	const FString Url = FString::Printf(TEXT("http://127.0.0.1:%d/mcp"), Port);

	const FString Dir = FPaths::ProjectSavedDir() / TEXT("AgentIntegrationKit");
	FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*Dir);
	OutAbsolutePath = FPaths::ConvertRelativePathToFull(Dir / TEXT("terminal-unreal-mcp.json"));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> McpServers = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Unreal = MakeShared<FJsonObject>();
	Unreal->SetStringField(TEXT("type"), TEXT("http"));
	Unreal->SetStringField(TEXT("url"), Url);
	McpServers->SetObjectField(TEXT("unreal-editor"), Unreal);
	Root->SetObjectField(TEXT("mcpServers"), McpServers);

	FString JsonOut;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonOut);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	return FFileHelper::SaveStringToFile(JsonOut, *OutAbsolutePath);
}

FACPTerminalResumeCommand::EKind FACPTerminalResumeCommand::Classify(
	const FString& RegistryId,
	const FString& AgentName)
{
	FString Rid = RegistryId;
	Rid.TrimStartAndEndInline();
	Rid = Rid.ToLower();

	if (Rid == TEXT("claude-acp"))
	{
		return EKind::Claude;
	}
	if (Rid == TEXT("gemini"))
	{
		return EKind::Gemini;
	}
	if (Rid == TEXT("github-copilot") || Rid == TEXT("github-copilot-cli"))
	{
		return EKind::Copilot;
	}
	if (Rid == TEXT("codex-acp"))
	{
		return EKind::Codex;
	}

	// Bundled / legacy configs often leave RegistryId empty — match stable display names.
	if (AgentName.Equals(TEXT("Claude Code"), ESearchCase::IgnoreCase))
	{
		return EKind::Claude;
	}
	if (AgentName.Equals(TEXT("Gemini CLI"), ESearchCase::IgnoreCase)
		|| AgentName.Equals(TEXT("Gemini"), ESearchCase::IgnoreCase))
	{
		return EKind::Gemini;
	}
	if (AgentName.Equals(TEXT("Copilot CLI"), ESearchCase::IgnoreCase)
		|| AgentName.Equals(TEXT("GitHub Copilot"), ESearchCase::IgnoreCase))
	{
		return EKind::Copilot;
	}
	if (AgentName.Equals(TEXT("Codex CLI"), ESearchCase::IgnoreCase))
	{
		return EKind::Codex;
	}

	return EKind::None;
}

bool FACPTerminalResumeCommand::IsSupported(const FString& RegistryId, const FString& AgentName)
{
	return Classify(RegistryId, AgentName) != EKind::None;
}

bool FACPTerminalResumeCommand::TryBuildCommandLine(
	const FString& RegistryId,
	const FString& AgentName,
	const FString& SessionId,
	FString& OutCommandLine)
{
	if (SessionId.IsEmpty())
	{
		return false;
	}

	const EKind Kind = Classify(RegistryId, AgentName);
	if (Kind == EKind::None)
	{
		return false;
	}

	const FString Q = QuoteSessionIdForShell(SessionId);

	switch (Kind)
	{
	case EKind::Claude:
	{
		FString McpPath;
		if (WriteClaudeTerminalMcpConfigFile(McpPath))
		{
			OutCommandLine = FString::Printf(
				TEXT("claude --mcp-config %s --dangerously-skip-permissions -r %s"),
				*QuotePathForShell(McpPath),
				*Q);
		}
		else
		{
			OutCommandLine = FString::Printf(
				TEXT("claude --dangerously-skip-permissions -r %s"),
				*Q);
		}
		return true;
	}
	case EKind::Gemini:
		OutCommandLine = FString::Printf(TEXT("gemini --resume %s"), *Q);
		return true;
	case EKind::Copilot:
		OutCommandLine = FString::Printf(TEXT("copilot --resume %s"), *Q);
		return true;
	case EKind::Codex:
		OutCommandLine = FString::Printf(TEXT("codex resume %s"), *Q);
		return true;
	default:
		return false;
	}
}
