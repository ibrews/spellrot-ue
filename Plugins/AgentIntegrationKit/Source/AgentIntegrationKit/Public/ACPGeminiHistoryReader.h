// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ACPTypes.h"

class AGENTINTEGRATIONKIT_API FACPGeminiHistoryReader
{
public:
	static TArray<FACPRemoteSessionEntry> ListSessions(const FString& WorkingDirectory);
	static bool ParseSession(const FString& SessionId, TArray<FACPChatMessage>& OutMessages, FACPRemoteSessionEntry* OutMetadata = nullptr);
	static FString FindSessionFilePath(const FString& SessionId);

private:
	static bool ReadSessionFileMetadata(const FString& FilePath, FACPRemoteSessionEntry& OutEntry);
	static FString BuildTitleFromMessages(const TArray<FACPChatMessage>& Messages);
};
