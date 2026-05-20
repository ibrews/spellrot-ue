// Copyright 2026 Betide Studio. All Rights Reserved.

#include "ACPCodexHistoryReader.h"
#include "AgentIntegrationKitModule.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"

// ============================================================================
// Path Helpers
// ============================================================================

FString FACPCodexHistoryReader::GetCodexSessionsDir()
{
#if PLATFORM_WINDOWS
	FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
#else
	FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
#endif
	if (Home.IsEmpty())
	{
		return FString();
	}

	// Check CODEX_HOME env var first
	FString CodexHome = FPlatformMisc::GetEnvironmentVariable(TEXT("CODEX_HOME"));
	if (!CodexHome.IsEmpty())
	{
		return FPaths::Combine(CodexHome, TEXT("sessions"));
	}

	return FPaths::Combine(Home, TEXT(".codex"), TEXT("sessions"));
}

FString FACPCodexHistoryReader::NormalizePath(const FString& Path)
{
	FString Normalized = Path;
	Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
	// Strip trailing slash
	while (Normalized.Len() > 1 && Normalized.EndsWith(TEXT("/")))
	{
		Normalized.LeftChopInline(1);
	}
#if PLATFORM_WINDOWS
	Normalized = Normalized.ToLower();
#endif
	return Normalized;
}

// ============================================================================
// Session Meta Parsing
// ============================================================================

bool FACPCodexHistoryReader::ReadRolloutMeta(
	const FString& RolloutPath,
	FString& OutSessionId,
	FString& OutCwd,
	FString& OutTitle,
	FDateTime& OutUpdatedAt)
{
	// Read only the first line (session_meta)
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *RolloutPath))
	{
		return false;
	}
	if (Lines.Num() == 0)
	{
		return false;
	}

	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Lines[0]);
	if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
	{
		return false;
	}

	// Verify this is a session_meta line
	FString Type;
	JsonObj->TryGetStringField(TEXT("type"), Type);
	if (Type != TEXT("session_meta"))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* PayloadPtr = nullptr;
	if (!JsonObj->TryGetObjectField(TEXT("payload"), PayloadPtr) || !PayloadPtr || !(*PayloadPtr).IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>& Payload = *PayloadPtr;

	Payload->TryGetStringField(TEXT("id"), OutSessionId);
	Payload->TryGetStringField(TEXT("cwd"), OutCwd);

	// Use timestamp from the meta as updatedAt
	FString TimestampStr;
	if (Payload->TryGetStringField(TEXT("timestamp"), TimestampStr))
	{
		FDateTime::ParseIso8601(*TimestampStr, OutUpdatedAt);
	}

	// Try to extract title from the first user message in the file
	// (scan a few more lines to find the first user message)
	OutTitle.Empty();
	for (int32 i = 1; i < FMath::Min(Lines.Num(), 20); ++i)
	{
		TSharedPtr<FJsonObject> LineObj;
		TSharedRef<TJsonReader<>> LineReader = TJsonReaderFactory<>::Create(Lines[i]);
		if (!FJsonSerializer::Deserialize(LineReader, LineObj) || !LineObj.IsValid())
		{
			continue;
		}

		FString LineType;
		LineObj->TryGetStringField(TEXT("type"), LineType);

		if (LineType == TEXT("event_msg"))
		{
			const TSharedPtr<FJsonObject>* EventPayload = nullptr;
			if (LineObj->TryGetObjectField(TEXT("payload"), EventPayload) && EventPayload && (*EventPayload).IsValid())
			{
				FString EventType;
				(*EventPayload)->TryGetStringField(TEXT("type"), EventType);
				if (EventType == TEXT("user_message"))
				{
					FString Message;
					(*EventPayload)->TryGetStringField(TEXT("message"), Message);
					if (!Message.IsEmpty())
					{
						// Truncate to reasonable title length
						if (Message.Len() > 120)
						{
							Message = Message.Left(117) + TEXT("...");
						}
						OutTitle = Message;
						break;
					}
				}
			}
		}
		else if (LineType == TEXT("response_item"))
		{
			const TSharedPtr<FJsonObject>* RIPayload = nullptr;
			if (LineObj->TryGetObjectField(TEXT("payload"), RIPayload) && RIPayload && (*RIPayload).IsValid())
			{
				FString Role;
				(*RIPayload)->TryGetStringField(TEXT("role"), Role);
				if (Role == TEXT("user"))
				{
					const TArray<TSharedPtr<FJsonValue>>* ContentArr = nullptr;
					if ((*RIPayload)->TryGetArrayField(TEXT("content"), ContentArr) && ContentArr)
					{
						for (const auto& ContentVal : *ContentArr)
						{
							const TSharedPtr<FJsonObject>* ContentObj = nullptr;
							if (ContentVal.IsValid() && ContentVal->TryGetObject(ContentObj) && ContentObj && (*ContentObj).IsValid())
							{
								FString ContentType;
								(*ContentObj)->TryGetStringField(TEXT("type"), ContentType);
								if (ContentType == TEXT("input_text"))
								{
									FString Text;
									(*ContentObj)->TryGetStringField(TEXT("text"), Text);
									// Skip system/developer instructions
									if (!Text.IsEmpty() && !Text.StartsWith(TEXT("<")) && !Text.StartsWith(TEXT("#")) && Text.Len() < 500)
									{
										if (Text.Len() > 120)
										{
											Text = Text.Left(117) + TEXT("...");
										}
										OutTitle = Text;
										break;
									}
								}
							}
						}
					}
					if (!OutTitle.IsEmpty()) break;
				}
			}
		}
	}

	// Update the timestamp from file modification time if we didn't get one from the meta
	if (OutUpdatedAt == FDateTime())
	{
		OutUpdatedAt = IFileManager::Get().GetTimeStamp(*RolloutPath);
	}

	return !OutSessionId.IsEmpty();
}

// ============================================================================
// List Sessions
// ============================================================================

TArray<FACPRemoteSessionEntry> FACPCodexHistoryReader::ListSessions(const FString& WorkingDirectory)
{
	TArray<FACPRemoteSessionEntry> Sessions;

	FString SessionsDir = GetCodexSessionsDir();
	if (SessionsDir.IsEmpty() || !IFileManager::Get().DirectoryExists(*SessionsDir))
	{
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("CodexHistoryReader: Sessions dir not found: %s"), *SessionsDir);
		return Sessions;
	}

	FString NormalizedFilterCwd = NormalizePath(FPaths::ConvertRelativePathToFull(WorkingDirectory));

	// Find all rollout-*.jsonl files recursively
	TArray<FString> AllFiles;
	IFileManager::Get().FindFilesRecursive(AllFiles, *SessionsDir, TEXT("rollout-*.jsonl"), true, false);

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("CodexHistoryReader: Found %d rollout files in %s, filtering by cwd='%s'"),
		AllFiles.Num(), *SessionsDir, *NormalizedFilterCwd);

	int32 MatchCount = 0;
	int32 MismatchCount = 0;
	int32 ParseFailCount = 0;

	for (const FString& FilePath : AllFiles)
	{
		FString SessionId, Cwd, Title;
		FDateTime UpdatedAt;

		if (!ReadRolloutMeta(FilePath, SessionId, Cwd, Title, UpdatedAt))
		{
			ParseFailCount++;
			continue;
		}

		// Compare cwds (normalized)
		FString NormalizedSessionCwd = NormalizePath(Cwd);
		if (NormalizedSessionCwd != NormalizedFilterCwd)
		{
			MismatchCount++;
			continue;
		}

		MatchCount++;

		FACPRemoteSessionEntry Entry;
		Entry.SessionId = SessionId;
		Entry.Title = Title;
		Entry.Cwd = Cwd;
		Entry.UpdatedAt = UpdatedAt;
		Sessions.Add(MoveTemp(Entry));
	}

	// Sort by UpdatedAt descending (newest first)
	Sessions.Sort([](const FACPRemoteSessionEntry& A, const FACPRemoteSessionEntry& B)
	{
		return A.UpdatedAt > B.UpdatedAt;
	});

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("CodexHistoryReader: %d matching, %d cwd mismatch, %d parse failures"),
		MatchCount, MismatchCount, ParseFailCount);

	return Sessions;
}

// ============================================================================
// Session Lookup
// ============================================================================

bool FACPCodexHistoryReader::SessionExists(const FString& SessionId, const FString& WorkingDirectory)
{
	return !GetSessionJsonlPath(SessionId, WorkingDirectory).IsEmpty();
}

FString FACPCodexHistoryReader::GetSessionJsonlPath(const FString& SessionId, const FString& WorkingDirectory)
{
	FString SessionsDir = GetCodexSessionsDir();
	if (SessionsDir.IsEmpty())
	{
		return FString();
	}

	// Search for rollout file containing the session ID
	TArray<FString> FoundFiles;
	FString Pattern = FString::Printf(TEXT("*-%s.jsonl"), *SessionId);
	IFileManager::Get().FindFilesRecursive(FoundFiles, *SessionsDir, *Pattern, true, false);

	if (FoundFiles.Num() > 0)
	{
		return FoundFiles[0];
	}

	return FString();
}

// ============================================================================
// Parse Session Messages
// ============================================================================

bool FACPCodexHistoryReader::ParseSessionJsonl(const FString& RolloutPath, TArray<FACPChatMessage>& OutMessages)
{
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *RolloutPath))
	{
		return false;
	}

	for (const FString& Line : Lines)
	{
		if (Line.IsEmpty()) continue;

		TSharedPtr<FJsonObject> JsonObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
		if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
		{
			continue;
		}

		FString Type;
		JsonObj->TryGetStringField(TEXT("type"), Type);

		// Extract user messages from event_msg
		if (Type == TEXT("event_msg"))
		{
			const TSharedPtr<FJsonObject>* Payload = nullptr;
			if (!JsonObj->TryGetObjectField(TEXT("payload"), Payload) || !Payload || !(*Payload).IsValid())
			{
				continue;
			}

			FString EventType;
			(*Payload)->TryGetStringField(TEXT("type"), EventType);

			if (EventType == TEXT("user_message"))
			{
				FString Message;
				(*Payload)->TryGetStringField(TEXT("message"), Message);
				if (!Message.IsEmpty())
				{
					FACPChatMessage ChatMsg;
					ChatMsg.Role = EACPMessageRole::User;

					FACPContentBlock Block;
					Block.Type = EACPContentBlockType::Text;
					Block.Text = Message;
					ChatMsg.ContentBlocks.Add(MoveTemp(Block));

					FString TimestampStr;
					if (JsonObj->TryGetStringField(TEXT("timestamp"), TimestampStr))
					{
						FDateTime::ParseIso8601(*TimestampStr, ChatMsg.Timestamp);
					}

					OutMessages.Add(MoveTemp(ChatMsg));
				}
			}
			else if (EventType == TEXT("agent_message"))
			{
				FString Message;
				(*Payload)->TryGetStringField(TEXT("message"), Message);
				if (!Message.IsEmpty())
				{
					FACPChatMessage ChatMsg;
					ChatMsg.Role = EACPMessageRole::Assistant;

					FACPContentBlock Block;
					Block.Type = EACPContentBlockType::Text;
					Block.Text = Message;
					ChatMsg.ContentBlocks.Add(MoveTemp(Block));

					FString TimestampStr;
					if (JsonObj->TryGetStringField(TEXT("timestamp"), TimestampStr))
					{
						FDateTime::ParseIso8601(*TimestampStr, ChatMsg.Timestamp);
					}

					OutMessages.Add(MoveTemp(ChatMsg));
				}
			}
		}
		// Extract assistant messages from response_item
		else if (Type == TEXT("response_item"))
		{
			const TSharedPtr<FJsonObject>* Payload = nullptr;
			if (!JsonObj->TryGetObjectField(TEXT("payload"), Payload) || !Payload || !(*Payload).IsValid())
			{
				continue;
			}

			FString Role;
			(*Payload)->TryGetStringField(TEXT("role"), Role);

			FString Phase;
			(*Payload)->TryGetStringField(TEXT("phase"), Phase);

			if (Role == TEXT("assistant") && Phase == TEXT("final_answer"))
			{
				const TArray<TSharedPtr<FJsonValue>>* ContentArr = nullptr;
				if ((*Payload)->TryGetArrayField(TEXT("content"), ContentArr) && ContentArr)
				{
					FString FullText;
					for (const auto& ContentVal : *ContentArr)
					{
						const TSharedPtr<FJsonObject>* ContentObj = nullptr;
						if (ContentVal.IsValid() && ContentVal->TryGetObject(ContentObj) && ContentObj && (*ContentObj).IsValid())
						{
							FString ContentType;
							(*ContentObj)->TryGetStringField(TEXT("type"), ContentType);
							if (ContentType == TEXT("output_text"))
							{
								FString Text;
								(*ContentObj)->TryGetStringField(TEXT("text"), Text);
								if (!Text.IsEmpty())
								{
									if (!FullText.IsEmpty()) FullText += TEXT("\n");
									FullText += Text;
								}
							}
						}
					}

					if (!FullText.IsEmpty())
					{
						FACPChatMessage ChatMsg;
						ChatMsg.Role = EACPMessageRole::Assistant;

						FACPContentBlock Block;
						Block.Type = EACPContentBlockType::Text;
						Block.Text = FullText;
						ChatMsg.ContentBlocks.Add(MoveTemp(Block));

						FString TimestampStr;
						if (JsonObj->TryGetStringField(TEXT("timestamp"), TimestampStr))
						{
							FDateTime::ParseIso8601(*TimestampStr, ChatMsg.Timestamp);
						}

						OutMessages.Add(MoveTemp(ChatMsg));
					}
				}
			}
		}
	}

	return OutMessages.Num() > 0;
}
