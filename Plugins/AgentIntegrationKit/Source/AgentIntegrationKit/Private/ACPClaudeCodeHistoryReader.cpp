// Copyright 2026 Betide Studio. All Rights Reserved.

#include "ACPClaudeCodeHistoryReader.h"
#include "AgentIntegrationKitModule.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"

FString FACPClaudeCodeHistoryReader::GetClaudeCodeProjectsDir()
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

	return FPaths::Combine(Home, TEXT(".claude"), TEXT("projects"));
}

FString FACPClaudeCodeHistoryReader::ManglePath(const FString& Path)
{
	FString Mangled = Path;

	// Strip trailing slash before encoding
	while (Mangled.Len() > 1 && (Mangled.EndsWith(TEXT("/")) || Mangled.EndsWith(TEXT("\\"))))
	{
		Mangled.LeftChopInline(1);
	}

	// Claude Code encodes slashes, backslashes, colons, AND spaces as hyphens
	Mangled.ReplaceInline(TEXT("\\"), TEXT("-"));
	Mangled.ReplaceInline(TEXT("/"), TEXT("-"));
	Mangled.ReplaceInline(TEXT(":"), TEXT("-"));
	Mangled.ReplaceInline(TEXT(" "), TEXT("-"));

	if (!Mangled.StartsWith(TEXT("-")))
	{
		Mangled = TEXT("-") + Mangled;
	}

	return Mangled;
}

FString FACPClaudeCodeHistoryReader::GetSessionJsonlPath(const FString& SessionId, const FString& WorkingDirectory)
{
	FString ProjectsDir = GetClaudeCodeProjectsDir();
	if (ProjectsDir.IsEmpty())
	{
		return FString();
	}

	FString MangledDir = ManglePath(FPaths::ConvertRelativePathToFull(WorkingDirectory));
	return FPaths::Combine(ProjectsDir, MangledDir, SessionId + TEXT(".jsonl"));
}

bool FACPClaudeCodeHistoryReader::SessionExists(const FString& SessionId, const FString& WorkingDirectory)
{
	FString Path = GetSessionJsonlPath(SessionId, WorkingDirectory);
	return !Path.IsEmpty() && IFileManager::Get().FileExists(*Path);
}

TArray<FString> FACPClaudeCodeHistoryReader::ListSessions(const FString& WorkingDirectory)
{
	TArray<FString> SessionIds;

	FString ProjectsDir = GetClaudeCodeProjectsDir();
	if (ProjectsDir.IsEmpty())
	{
		return SessionIds;
	}

	FString MangledDir = ManglePath(FPaths::ConvertRelativePathToFull(WorkingDirectory));
	FString SessionDir = FPaths::Combine(ProjectsDir, MangledDir);

	if (!IFileManager::Get().DirectoryExists(*SessionDir))
	{
		return SessionIds;
	}

	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *SessionDir, TEXT("*.jsonl"));

	for (const FString& File : Files)
	{
		SessionIds.Add(FPaths::GetBaseFilename(File));
	}

	return SessionIds;
}

bool FACPClaudeCodeHistoryReader::ParseSessionJsonl(const FString& FilePath, TArray<FACPChatMessage>& OutMessages)
{
	if (!IFileManager::Get().FileExists(*FilePath))
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPClaudeCodeHistoryReader: File not found: %s"), *FilePath);
		return false;
	}

	FString FileContent;
	if (!FFileHelper::LoadFileToString(FileContent, *FilePath))
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPClaudeCodeHistoryReader: Failed to load file: %s"), *FilePath);
		return false;
	}

	OutMessages.Empty();
	FACPChatMessage* CurrentAssistantMessage = nullptr;
	bool bInAssistantMessage = false;

	TArray<FString> Lines;
	FileContent.ParseIntoArrayLines(Lines);

	for (const FString& Line : Lines)
	{
		if (Line.TrimStartAndEnd().IsEmpty())
		{
			continue;
		}

		ProcessJsonlLine(Line, OutMessages, CurrentAssistantMessage, bInAssistantMessage);
	}

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClaudeCodeHistoryReader: Parsed %d messages from %s"), OutMessages.Num(), *FilePath);
	return true;
}

void FACPClaudeCodeHistoryReader::ProcessJsonlLine(const FString& Line, TArray<FACPChatMessage>& OutMessages,
	FACPChatMessage*& CurrentAssistantMessage, bool& bInAssistantMessage)
{
	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);

	if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
	{
		return;
	}

	FString Type;
	JsonObj->TryGetStringField(TEXT("type"), Type);

	// ── System events (compact boundaries, turn duration, etc.) ──────
	if (Type == TEXT("system"))
	{
		FString Subtype;
		JsonObj->TryGetStringField(TEXT("subtype"), Subtype);

		if (Subtype == TEXT("compact_boundary") || Subtype == TEXT("microcompact_boundary"))
		{
			// Close any in-progress assistant message
			if (bInAssistantMessage && CurrentAssistantMessage)
			{
				CurrentAssistantMessage->bIsStreaming = false;
				CurrentAssistantMessage = nullptr;
			}
			bInAssistantMessage = false;

			FString Content;
			JsonObj->TryGetStringField(TEXT("content"), Content);
			if (Content.IsEmpty())
			{
				Content = (Subtype == TEXT("compact_boundary"))
					? TEXT("Conversation compacted")
					: TEXT("Context optimized");
			}

			FACPChatMessage SystemMsg;
			SystemMsg.Role = EACPMessageRole::System;
			SystemMsg.Timestamp = FDateTime::Now();
			SystemMsg.bIsStreaming = false;

			FACPContentBlock SystemBlock;
			SystemBlock.Type = EACPContentBlockType::System;
			SystemBlock.Text = Content;
			SystemMsg.ContentBlocks.Add(SystemBlock);
			OutMessages.Add(SystemMsg);
		}
		// Skip turn_duration, stop_hook_summary, and other system subtypes
		return;
	}

	// ── Skip non-message types (queue-operation, summary, file-history-snapshot, progress) ──
	if (Type != TEXT("user") && Type != TEXT("human") && Type != TEXT("assistant") && Type != TEXT("tool_result"))
	{
		return;
	}

	if (Type == TEXT("user") || Type == TEXT("human"))
	{
		// Skip internal meta messages (local command caveats, etc.)
		bool bIsMeta = false;
		JsonObj->TryGetBoolField(TEXT("isMeta"), bIsMeta);
		if (bIsMeta)
		{
			return;
		}

		// Compact summaries → render as system divider, not user message
		bool bIsCompactSummary = false;
		JsonObj->TryGetBoolField(TEXT("isCompactSummary"), bIsCompactSummary);

		if (bInAssistantMessage && CurrentAssistantMessage)
		{
			CurrentAssistantMessage->bIsStreaming = false;
			CurrentAssistantMessage = nullptr;
		}
		bInAssistantMessage = false;

		if (bIsCompactSummary)
		{
			// Extract the summary text
			FString SummaryText;
			TSharedPtr<FJsonObject> MessageObj = JsonObj->GetObjectField(TEXT("message"));
			if (MessageObj.IsValid())
			{
				MessageObj->TryGetStringField(TEXT("content"), SummaryText);
			}

			if (!SummaryText.IsEmpty())
			{
				FACPChatMessage SystemMsg;
				SystemMsg.Role = EACPMessageRole::System;
				SystemMsg.Timestamp = FDateTime::Now();
				SystemMsg.bIsStreaming = false;

				FACPContentBlock SystemBlock;
				SystemBlock.Type = EACPContentBlockType::System;
				SystemBlock.Text = TEXT("Previous context was summarized");
				SystemMsg.ContentBlocks.Add(SystemBlock);
				OutMessages.Add(SystemMsg);
			}
			return;
		}

		FACPChatMessage UserMsg;
		UserMsg.Role = EACPMessageRole::User;
		UserMsg.Timestamp = FDateTime::Now();

		FString UserText;

		const TArray<TSharedPtr<FJsonValue>>* MessageArray;
		if (JsonObj->TryGetArrayField(TEXT("message"), MessageArray))
		{
			for (const TSharedPtr<FJsonValue>& Val : *MessageArray)
			{
				TSharedPtr<FJsonObject> ContentObj = Val->AsObject();
				if (ContentObj.IsValid())
				{
					FString ContentType;
					ContentObj->TryGetStringField(TEXT("type"), ContentType);

					if (ContentType == TEXT("text"))
					{
						FString Text;
						ContentObj->TryGetStringField(TEXT("text"), Text);
						UserText += Text;
					}
				}
			}
		}
		else
		{
			TSharedPtr<FJsonObject> MessageObj = JsonObj->GetObjectField(TEXT("message"));
			if (MessageObj.IsValid())
			{
				MessageObj->TryGetStringField(TEXT("content"), UserText);
			}
		}

		if (!UserText.IsEmpty())
		{
			FACPContentBlock TextBlock;
			TextBlock.Type = EACPContentBlockType::Text;
			TextBlock.Text = UserText;
			UserMsg.ContentBlocks.Add(TextBlock);
			OutMessages.Add(UserMsg);
		}
	}
	else if (Type == TEXT("assistant"))
	{
		if (!bInAssistantMessage)
		{
			FACPChatMessage AssistantMsg;
			AssistantMsg.Role = EACPMessageRole::Assistant;
			AssistantMsg.Timestamp = FDateTime::Now();
			AssistantMsg.bIsStreaming = false;

			OutMessages.Add(AssistantMsg);
			CurrentAssistantMessage = &OutMessages.Last();
			bInAssistantMessage = true;
		}

		const TArray<TSharedPtr<FJsonValue>>* MessageArray;
		if (JsonObj->TryGetArrayField(TEXT("message"), MessageArray) && CurrentAssistantMessage)
		{
			for (const TSharedPtr<FJsonValue>& Val : *MessageArray)
			{
				TSharedPtr<FJsonObject> ContentObj = Val->AsObject();
				if (!ContentObj.IsValid())
				{
					continue;
				}

				FString ContentType;
				ContentObj->TryGetStringField(TEXT("type"), ContentType);

				if (ContentType == TEXT("text"))
				{
					FString Text;
					ContentObj->TryGetStringField(TEXT("text"), Text);

					FACPContentBlock TextBlock;
					TextBlock.Type = EACPContentBlockType::Text;
					TextBlock.Text = Text;
					CurrentAssistantMessage->ContentBlocks.Add(TextBlock);
				}
				else if (ContentType == TEXT("tool_use"))
				{
					FACPContentBlock ToolBlock;
					ToolBlock.Type = EACPContentBlockType::ToolCall;
					ContentObj->TryGetStringField(TEXT("id"), ToolBlock.ToolCallId);
					ContentObj->TryGetStringField(TEXT("name"), ToolBlock.ToolName);

					TSharedPtr<FJsonObject> InputObj = ContentObj->GetObjectField(TEXT("input"));
					if (InputObj.IsValid())
					{
						FString InputStr;
						TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&InputStr);
						FJsonSerializer::Serialize(InputObj.ToSharedRef(), Writer);
						ToolBlock.ToolArguments = InputStr;
					}

					CurrentAssistantMessage->ContentBlocks.Add(ToolBlock);
				}
				else if (ContentType == TEXT("thinking"))
				{
					FString ThinkingText;
					ContentObj->TryGetStringField(TEXT("thinking"), ThinkingText);

					if (!ThinkingText.IsEmpty())
					{
						FACPContentBlock ThoughtBlock;
						ThoughtBlock.Type = EACPContentBlockType::Thought;
						ThoughtBlock.Text = ThinkingText;
						CurrentAssistantMessage->ContentBlocks.Add(ThoughtBlock);
					}
				}
			}
		}
	}
	else if (Type == TEXT("tool_result"))
	{
		if (CurrentAssistantMessage)
		{
			FString ToolUseId;
			JsonObj->TryGetStringField(TEXT("tool_use_id"), ToolUseId);

			for (FACPContentBlock& Block : CurrentAssistantMessage->ContentBlocks)
			{
				if (Block.Type == EACPContentBlockType::ToolCall && Block.ToolCallId == ToolUseId)
				{
					FACPContentBlock ResultBlock;
					ResultBlock.Type = EACPContentBlockType::ToolResult;
					ResultBlock.ToolCallId = ToolUseId;

					const TArray<TSharedPtr<FJsonValue>>* ContentArray;
					if (JsonObj->TryGetArrayField(TEXT("content"), ContentArray))
					{
						for (const TSharedPtr<FJsonValue>& Val : *ContentArray)
						{
							TSharedPtr<FJsonObject> ContentObj = Val->AsObject();
							if (ContentObj.IsValid())
							{
								FString ContentType;
								ContentObj->TryGetStringField(TEXT("type"), ContentType);

								if (ContentType == TEXT("text"))
								{
									FString Text;
									ContentObj->TryGetStringField(TEXT("text"), Text);
									ResultBlock.ToolResultContent += Text;
								}
							}
						}
					}
					else
					{
						FString Content;
						JsonObj->TryGetStringField(TEXT("content"), Content);
						ResultBlock.ToolResultContent = Content;
					}

					bool bIsError = false;
					JsonObj->TryGetBoolField(TEXT("is_error"), bIsError);
					ResultBlock.bToolSuccess = !bIsError;

					CurrentAssistantMessage->ContentBlocks.Add(ResultBlock);
					break;
				}
			}
		}
	}
}
