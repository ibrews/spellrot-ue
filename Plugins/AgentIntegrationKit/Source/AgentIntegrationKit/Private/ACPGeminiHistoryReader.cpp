// Copyright 2026 Betide Studio. All Rights Reserved.

#include "ACPGeminiHistoryReader.h"
#include "AgentIntegrationKitModule.h"
#include "AgentInstaller.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
static FString QuoteForSingleShell(const FString& Value)
{
	FString Escaped = Value;
	Escaped.ReplaceInline(TEXT("'"), TEXT("'\\''"));
	return FString::Printf(TEXT("'%s'"), *Escaped);
}

static bool ParseIsoTime(const FString& Value, FDateTime& OutTime)
{
	return !Value.IsEmpty() && FDateTime::ParseIso8601(*Value, OutTime);
}

static FString ExtractGeminiSessionIdFromListLine(const FString& Line)
{
	int32 CloseBracket = INDEX_NONE;
	if (!Line.FindLastChar(TEXT(']'), CloseBracket) || CloseBracket <= 0)
	{
		return FString();
	}

	int32 OpenBracket = INDEX_NONE;
	for (int32 Index = CloseBracket - 1; Index >= 0; --Index)
	{
		if (Line[Index] == TEXT('['))
		{
			OpenBracket = Index;
			break;
		}
	}

	if (OpenBracket == INDEX_NONE || OpenBracket + 1 >= CloseBracket)
	{
		return FString();
	}

	FString SessionId = Line.Mid(OpenBracket + 1, CloseBracket - OpenBracket - 1);
	SessionId.TrimStartAndEndInline();
	return SessionId;
}
}

TArray<FACPRemoteSessionEntry> FACPGeminiHistoryReader::ListSessions(const FString& WorkingDirectory)
{
	TArray<FACPRemoteSessionEntry> Sessions;

	FString GeminiExecutable;
	if (!FAgentInstaller::Get().ResolveExecutable(TEXT("gemini"), GeminiExecutable))
	{
		return Sessions;
	}

	FString StdOut, StdErr;
	int32 ReturnCode = -1;
	FString NormalizedWorkingDirectory = FPaths::ConvertRelativePathToFull(WorkingDirectory);
	const FString ShellCommand = FString::Printf(
		TEXT("cd %s && %s --list-sessions"),
		*QuoteForSingleShell(NormalizedWorkingDirectory),
		*QuoteForSingleShell(GeminiExecutable));
	FPlatformProcess::ExecProcess(
		TEXT("/bin/sh"),
		*FString::Printf(TEXT("-lc %s"), *QuoteForSingleShell(ShellCommand)),
		&ReturnCode,
		&StdOut,
		&StdErr);

	if (ReturnCode != 0 || StdOut.IsEmpty())
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("GeminiHistoryReader: list-sessions failed: %s"), *StdErr);
		return Sessions;
	}

	TSet<FString> SeenSessionIds;
	TArray<FString> Lines;
	StdOut.ParseIntoArrayLines(Lines, true);

	for (const FString& Line : Lines)
	{
		const FString SessionId = ExtractGeminiSessionIdFromListLine(Line);
		if (SessionId.IsEmpty() || SeenSessionIds.Contains(SessionId))
		{
			continue;
		}

		FACPRemoteSessionEntry Entry;
		if (ReadSessionFileMetadata(FindSessionFilePath(SessionId), Entry))
		{
			SeenSessionIds.Add(SessionId);
			Sessions.Add(MoveTemp(Entry));
		}
	}

	return Sessions;
}

bool FACPGeminiHistoryReader::ParseSession(const FString& SessionId, TArray<FACPChatMessage>& OutMessages, FACPRemoteSessionEntry* OutMetadata)
{
	OutMessages.Empty();

	const FString FilePath = FindSessionFilePath(SessionId);
	if (FilePath.IsEmpty())
	{
		return false;
	}

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return false;
	}

	if (OutMetadata)
	{
		ReadSessionFileMetadata(FilePath, *OutMetadata);
	}

	const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
	if (!Root->TryGetArrayField(TEXT("messages"), Messages) || !Messages)
	{
		return true;
	}

	for (const TSharedPtr<FJsonValue>& MessageValue : *Messages)
	{
		TSharedPtr<FJsonObject> MessageObj = MessageValue->AsObject();
		if (!MessageObj.IsValid())
		{
			continue;
		}

		FString Type;
		MessageObj->TryGetStringField(TEXT("type"), Type);

		FACPChatMessage Message;
		if (Type == TEXT("user"))
		{
			Message.Role = EACPMessageRole::User;
		}
		else if (Type == TEXT("gemini") || Type == TEXT("assistant"))
		{
			Message.Role = EACPMessageRole::Assistant;
		}
		else
		{
			continue;
		}

		FString TimestampString;
		if (MessageObj->TryGetStringField(TEXT("timestamp"), TimestampString))
		{
			ParseIsoTime(TimestampString, Message.Timestamp);
		}

		FString Content;
		MessageObj->TryGetStringField(TEXT("content"), Content);
		if (!Content.IsEmpty())
		{
			FACPContentBlock TextBlock;
			TextBlock.Type = EACPContentBlockType::Text;
			TextBlock.Text = Content;
			TextBlock.Timestamp = Message.Timestamp;
			Message.ContentBlocks.Add(TextBlock);
		}

		const TArray<TSharedPtr<FJsonValue>>* Thoughts = nullptr;
		if (Message.Role == EACPMessageRole::Assistant
			&& MessageObj->TryGetArrayField(TEXT("thoughts"), Thoughts)
			&& Thoughts)
		{
			for (const TSharedPtr<FJsonValue>& ThoughtValue : *Thoughts)
			{
				TSharedPtr<FJsonObject> ThoughtObj = ThoughtValue->AsObject();
				if (!ThoughtObj.IsValid())
				{
					continue;
				}

				FString Subject;
				FString Description;
				ThoughtObj->TryGetStringField(TEXT("subject"), Subject);
				ThoughtObj->TryGetStringField(TEXT("description"), Description);

				FString ThoughtText = Description;
				if (!Subject.IsEmpty())
				{
					ThoughtText = Subject + (Description.IsEmpty() ? FString() : TEXT(": ") + Description);
				}
				if (ThoughtText.IsEmpty())
				{
					continue;
				}

				FACPContentBlock ThoughtBlock;
				ThoughtBlock.Type = EACPContentBlockType::Thought;
				ThoughtBlock.Text = ThoughtText;
				ThoughtBlock.Timestamp = Message.Timestamp;
				Message.ContentBlocks.Add(ThoughtBlock);
			}
		}

		if (Message.ContentBlocks.Num() > 0)
		{
			OutMessages.Add(MoveTemp(Message));
		}
	}

	return true;
}

FString FACPGeminiHistoryReader::FindSessionFilePath(const FString& SessionId)
{
	const FString RootDir = FPaths::Combine(FPlatformProcess::UserHomeDir(), TEXT(".gemini"), TEXT("tmp"));
	if (!IFileManager::Get().DirectoryExists(*RootDir))
	{
		return FString();
	}

	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *RootDir, TEXT("session-*.json"), true, false);

	for (const FString& FilePath : Files)
	{
		FACPRemoteSessionEntry Entry;
		if (ReadSessionFileMetadata(FilePath, Entry) && Entry.SessionId == SessionId)
		{
			return FilePath;
		}
	}

	return FString();
}

bool FACPGeminiHistoryReader::ReadSessionFileMetadata(const FString& FilePath, FACPRemoteSessionEntry& OutEntry)
{
	if (FilePath.IsEmpty() || !IFileManager::Get().FileExists(*FilePath))
	{
		return false;
	}

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return false;
	}

	OutEntry = FACPRemoteSessionEntry();
	if (!Root->TryGetStringField(TEXT("sessionId"), OutEntry.SessionId) || OutEntry.SessionId.IsEmpty())
	{
		return false;
	}

	FString UpdatedAtString;
	if (Root->TryGetStringField(TEXT("lastUpdated"), UpdatedAtString))
	{
		ParseIsoTime(UpdatedAtString, OutEntry.UpdatedAt);
	}

	const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
	if (Root->TryGetArrayField(TEXT("messages"), Messages) && Messages)
	{
		TArray<FACPChatMessage> TitleMessages;
		for (const TSharedPtr<FJsonValue>& MessageValue : *Messages)
		{
			TSharedPtr<FJsonObject> MessageObj = MessageValue->AsObject();
			if (!MessageObj.IsValid())
			{
				continue;
			}

			FString Type;
			FString Content;
			MessageObj->TryGetStringField(TEXT("type"), Type);
			MessageObj->TryGetStringField(TEXT("content"), Content);
			if (Type != TEXT("user") || Content.IsEmpty())
			{
				continue;
			}

			FACPChatMessage Message;
			Message.Role = EACPMessageRole::User;

			FACPContentBlock Block;
			Block.Type = EACPContentBlockType::Text;
			Block.Text = Content;
			Message.ContentBlocks.Add(Block);
			TitleMessages.Add(MoveTemp(Message));
			break;
		}

		OutEntry.Title = BuildTitleFromMessages(TitleMessages);
	}

	return true;
}

FString FACPGeminiHistoryReader::BuildTitleFromMessages(const TArray<FACPChatMessage>& Messages)
{
	for (const FACPChatMessage& Message : Messages)
	{
		if (Message.Role != EACPMessageRole::User)
		{
			continue;
		}

		for (const FACPContentBlock& Block : Message.ContentBlocks)
		{
			if (Block.Type == EACPContentBlockType::Text && !Block.Text.IsEmpty())
			{
				FString Title = Block.Text.Left(80);
				Title.ReplaceInline(TEXT("\n"), TEXT(" "));
				Title.ReplaceInline(TEXT("\r"), TEXT(" "));
				Title.TrimStartAndEndInline();
				if (Title.Len() < Block.Text.Len())
				{
					Title += TEXT("...");
				}
				return Title;
			}
		}
	}

	return TEXT("Gemini Session");
}
