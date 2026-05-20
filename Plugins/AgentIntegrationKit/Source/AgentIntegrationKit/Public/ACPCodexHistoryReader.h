// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ACPTypes.h"

/**
 * Reads Codex CLI session history directly from ~/.codex/sessions/ rollout files.
 * Bypasses codex-acp's session/list (which uses list_threads internally and can miss sessions).
 * Same pattern as FACPGeminiHistoryReader and FACPCopilotHistoryReader.
 */
class AGENTINTEGRATIONKIT_API FACPCodexHistoryReader
{
public:
	/** List all Codex sessions matching the given working directory */
	static TArray<FACPRemoteSessionEntry> ListSessions(const FString& WorkingDirectory);

	/** Check if a session rollout file exists by session ID */
	static bool SessionExists(const FString& SessionId, const FString& WorkingDirectory);

	/** Get the filesystem path to a session's rollout JSONL file */
	static FString GetSessionJsonlPath(const FString& SessionId, const FString& WorkingDirectory);

	/** Parse a rollout JSONL file into chat messages */
	static bool ParseSessionJsonl(const FString& RolloutPath, TArray<FACPChatMessage>& OutMessages);

private:
	/** Get the Codex sessions directory (~/.codex/sessions/) */
	static FString GetCodexSessionsDir();

	/** Parse session_meta from the first line of a rollout file */
	static bool ReadRolloutMeta(
		const FString& RolloutPath,
		FString& OutSessionId,
		FString& OutCwd,
		FString& OutTitle,
		FDateTime& OutUpdatedAt);

	/** Normalize a path for comparison (forward slashes, strip trailing, lowercase on Windows) */
	static FString NormalizePath(const FString& Path);
};
