// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ACPTypes.h"
#include "ACPSessionTypes.generated.h"

/**
 * Session metadata — lightweight info for listing and tracking
 */
USTRUCT()
struct AGENTINTEGRATIONKIT_API FACPSessionMetadata
{
	GENERATED_BODY()

	UPROPERTY()
	FString SessionId;

	UPROPERTY()
	FString AgentName;

	UPROPERTY()
	FString Title;

	UPROPERTY()
	FDateTime CreatedAt;

	UPROPERTY()
	FDateTime LastModifiedAt;

	UPROPERTY()
	int32 MessageCount = 0;

	UPROPERTY()
	FString ModelId;

	/** True when the user has explicitly renamed this session.
	 *  Prevents remote sync and auto-title from overwriting the custom name. */
	UPROPERTY()
	bool bHasCustomTitle = false;

	/** The external agent's own session ID (e.g., Claude's session ID).
	 *  Used to deduplicate against remote session lists which use agent IDs. */
	UPROPERTY()
	FString AgentSessionId;
};

/**
 * Active session state tracking (runtime only)
 */
struct AGENTINTEGRATIONKIT_API FACPActiveSession
{
	FACPSessionMetadata Metadata;
	TArray<FACPChatMessage> Messages;
	bool bIsConnected = false;
	bool bIsLoadingHistory = false;
	int32 CurrentStreamingMessageIndex = INDEX_NONE;
};
