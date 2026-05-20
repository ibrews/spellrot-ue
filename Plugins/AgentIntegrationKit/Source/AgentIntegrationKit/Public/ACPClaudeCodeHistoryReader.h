// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ACPTypes.h"

/**
 * Utility for reading Claude Code's native session history files
 * Claude Code stores sessions at: ~/.claude/projects/<mangle(cwd)>/<sessionId>.jsonl
 *
 * Path mangling: /Users/devesh/MyProject -> -Users-devesh-MyProject
 */
class AGENTINTEGRATIONKIT_API FACPClaudeCodeHistoryReader
{
public:
	/**
	 * Get the Claude Code sessions directory for a given working directory
	 */
	static FString GetClaudeCodeProjectsDir();

	/**
	 * Mangle a path for Claude Code's directory naming
	 * /Users/devesh/MyProject -> -Users-devesh-MyProject
	 */
	static FString ManglePath(const FString& Path);

	/**
	 * Get the full path to a Claude Code session's JSONL file
	 */
	static FString GetSessionJsonlPath(const FString& SessionId, const FString& WorkingDirectory);

	/**
	 * Parse a Claude Code JSONL file and convert to our message format
	 */
	static bool ParseSessionJsonl(const FString& FilePath, TArray<FACPChatMessage>& OutMessages);

	/**
	 * List available Claude Code sessions for a working directory
	 * Returns session IDs
	 */
	static TArray<FString> ListSessions(const FString& WorkingDirectory);

	/**
	 * Check if a session JSONL file exists
	 */
	static bool SessionExists(const FString& SessionId, const FString& WorkingDirectory);

private:
	/**
	 * Parse a single JSONL line into message updates
	 * Claude Code JSONL contains various message types that we accumulate
	 */
	static void ProcessJsonlLine(const FString& Line, TArray<FACPChatMessage>& OutMessages,
		FACPChatMessage*& CurrentAssistantMessage, bool& bInAssistantMessage);
};
