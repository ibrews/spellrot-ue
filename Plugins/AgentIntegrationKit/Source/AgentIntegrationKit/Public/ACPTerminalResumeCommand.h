// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Builds a shell line to resume an agent CLI session in an interactive terminal.
 * Prefers ACP registry IDs (stable); falls back to bundled/legacy display names when RegistryId is empty.
 */
class AGENTINTEGRATIONKIT_API FACPTerminalResumeCommand
{
public:
	/** True when TryBuildCommandLine would succeed for this agent (session ID may still be invalid). */
	static bool IsSupported(const FString& RegistryId, const FString& AgentName);

	static bool TryBuildCommandLine(
		const FString& RegistryId,
		const FString& AgentName,
		const FString& SessionId,
		FString& OutCommandLine);

private:
	enum class EKind : uint8
	{
		None,
		Claude,
		Gemini,
		Copilot,
		Codex,
	};

	static EKind Classify(const FString& RegistryId, const FString& AgentName);
	static FString QuoteSessionIdForShell(const FString& SessionId);
};
