// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ACPTypes.h"

class AGENTINTEGRATIONKIT_API FACPCopilotHistoryReader
{
public:
	static TArray<FACPRemoteSessionEntry> ListSessions(const FString& WorkingDirectory);
	static bool ParseSession(const FString& SessionId, TArray<FACPChatMessage>& OutMessages, FACPRemoteSessionEntry* OutMetadata = nullptr);
	static FString GetSessionDirectory(const FString& SessionId);

private:
	static bool ParseWorkspaceMetadata(const FString& WorkspaceYamlPath, FACPRemoteSessionEntry& OutEntry, FString& OutGitRoot, FString& OutCwd);
	static FString BuildTitleFromMessages(const TArray<FACPChatMessage>& Messages);
};
