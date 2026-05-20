// Copyright 2026 Betide Studio. All Rights Reserved.

#include "ACPCopilotHistoryReader.h"
#include "AgentIntegrationKitModule.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
static bool ParseIsoTimeCopilot(const FString& Value, FDateTime& OutTime)
{
	return !Value.IsEmpty() && FDateTime::ParseIso8601(*Value, OutTime);
}

static FString ParseYamlScalar(const FString& Yaml, const FString& Key)
{
	const FString Prefix = Key + TEXT(":");
	TArray<FString> Lines;
	Yaml.ParseIntoArrayLines(Lines, true);
	for (const FString& Line : Lines)
	{
		if (!Line.StartsWith(Prefix))
		{
			continue;
		}

		FString Value = Line.Mid(Prefix.Len());
		Value.TrimStartAndEndInline();
		if ((Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\"")))
			|| (Value.StartsWith(TEXT("'")) && Value.EndsWith(TEXT("'"))))
		{
			Value = Value.Mid(1, Value.Len() - 2);
		}
		return Value;
	}

	return FString();
}

static bool IsPathWithinOrEqual(const FString& Candidate, const FString& Root)
{
	FString NormalizedCandidate = Candidate;
	FString NormalizedRoot = Root;
	FPaths::NormalizeDirectoryName(NormalizedCandidate);
	FPaths::NormalizeDirectoryName(NormalizedRoot);

	return NormalizedCandidate.Equals(NormalizedRoot, ESearchCase::IgnoreCase)
		|| NormalizedCandidate.StartsWith(NormalizedRoot + TEXT("/"), ESearchCase::IgnoreCase)
		|| NormalizedCandidate.StartsWith(NormalizedRoot + TEXT("\\"), ESearchCase::IgnoreCase);
}
}

TArray<FACPRemoteSessionEntry> FACPCopilotHistoryReader::ListSessions(const FString& WorkingDirectory)
{
	TArray<FACPRemoteSessionEntry> Sessions;

	const FString SessionRoot = FPaths::Combine(FPlatformProcess::UserHomeDir(), TEXT(".copilot"), TEXT("session-state"));
	if (!IFileManager::Get().DirectoryExists(*SessionRoot))
	{
		return Sessions;
	}

	FString NormalizedWorkingDirectory = FPaths::ConvertRelativePathToFull(WorkingDirectory);
	FPaths::NormalizeDirectoryName(NormalizedWorkingDirectory);

	TArray<FString> SubDirs;
	IFileManager::Get().FindFiles(SubDirs, *SessionRoot, false, true);
	UE_LOG(LogAgentIntegrationKit, Log, TEXT("CopilotHistory: SessionRoot=%s, found %d subdirs, NormalizedWorkDir=%s"),
		*SessionRoot, SubDirs.Num(), *NormalizedWorkingDirectory);
	for (const FString& DirName : SubDirs)
	{
		const FString SessionDir = FPaths::Combine(SessionRoot, DirName);
		const FString WorkspaceYamlPath = FPaths::Combine(SessionDir, TEXT("workspace.yaml"));

		FACPRemoteSessionEntry Entry;
		FString GitRoot;
		FString Cwd;
		if (!ParseWorkspaceMetadata(WorkspaceYamlPath, Entry, GitRoot, Cwd))
		{
			continue;
		}

		if (GitRoot.IsEmpty() && Cwd.IsEmpty())
		{
			continue;
		}

		const bool bMatchesProject =
			(!GitRoot.IsEmpty() && IsPathWithinOrEqual(NormalizedWorkingDirectory, GitRoot))
			|| (!Cwd.IsEmpty() && IsPathWithinOrEqual(Cwd, NormalizedWorkingDirectory))
			|| (!Cwd.IsEmpty() && IsPathWithinOrEqual(NormalizedWorkingDirectory, Cwd));

		if (bMatchesProject)
		{
			Sessions.Add(MoveTemp(Entry));
		}
	}

	Sessions.Sort([](const FACPRemoteSessionEntry& A, const FACPRemoteSessionEntry& B)
	{
		return A.UpdatedAt > B.UpdatedAt;
	});

	return Sessions;
}

bool FACPCopilotHistoryReader::ParseSession(const FString& SessionId, TArray<FACPChatMessage>& OutMessages, FACPRemoteSessionEntry* OutMetadata)
{
	OutMessages.Empty();

	const FString SessionDir = GetSessionDirectory(SessionId);
	if (SessionDir.IsEmpty())
	{
		return false;
	}

	if (OutMetadata)
	{
		FString GitRoot;
		FString Cwd;
		ParseWorkspaceMetadata(FPaths::Combine(SessionDir, TEXT("workspace.yaml")), *OutMetadata, GitRoot, Cwd);
	}

	FString EventsText;
	if (!FFileHelper::LoadFileToString(EventsText, *FPaths::Combine(SessionDir, TEXT("events.jsonl"))))
	{
		return true;
	}

	TArray<FString> Lines;
	EventsText.ParseIntoArrayLines(Lines, true);

	for (const FString& Line : Lines)
	{
		if (Line.TrimStartAndEnd().IsEmpty())
		{
			continue;
		}

		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			continue;
		}

		FString EventType;
		Root->TryGetStringField(TEXT("type"), EventType);

		FString TimestampString;
		Root->TryGetStringField(TEXT("timestamp"), TimestampString);
		FDateTime Timestamp = FDateTime::Now();
		ParseIsoTimeCopilot(TimestampString, Timestamp);

		const TSharedPtr<FJsonObject>* DataPtr = nullptr;
		Root->TryGetObjectField(TEXT("data"), DataPtr);
		TSharedPtr<FJsonObject> Data = DataPtr ? *DataPtr : nullptr;

		if (EventType == TEXT("user.message") && Data.IsValid())
		{
			FString Content;
			Data->TryGetStringField(TEXT("content"), Content);
			if (Content.IsEmpty())
			{
				continue;
			}

			FACPChatMessage Message;
			Message.Role = EACPMessageRole::User;
			Message.Timestamp = Timestamp;

			FACPContentBlock Block;
			Block.Type = EACPContentBlockType::Text;
			Block.Text = Content;
			Block.Timestamp = Timestamp;
			Message.ContentBlocks.Add(Block);
			OutMessages.Add(MoveTemp(Message));
		}
		else if (EventType == TEXT("assistant.message") && Data.IsValid())
		{
			FACPChatMessage Message;
			Message.Role = EACPMessageRole::Assistant;
			Message.Timestamp = Timestamp;

			FString ReasoningText;
			Data->TryGetStringField(TEXT("reasoningText"), ReasoningText);
			if (!ReasoningText.IsEmpty())
			{
				FACPContentBlock Thought;
				Thought.Type = EACPContentBlockType::Thought;
				Thought.Text = ReasoningText;
				Thought.Timestamp = Timestamp;
				Message.ContentBlocks.Add(Thought);
			}

			FString Content;
			Data->TryGetStringField(TEXT("content"), Content);
			if (!Content.IsEmpty())
			{
				FACPContentBlock Text;
				Text.Type = EACPContentBlockType::Text;
				Text.Text = Content;
				Text.Timestamp = Timestamp;
				Message.ContentBlocks.Add(Text);
			}

			if (Message.ContentBlocks.Num() > 0)
			{
				OutMessages.Add(MoveTemp(Message));
			}
		}
	}

	return true;
}

FString FACPCopilotHistoryReader::GetSessionDirectory(const FString& SessionId)
{
	const FString SessionDir = FPaths::Combine(FPlatformProcess::UserHomeDir(), TEXT(".copilot"), TEXT("session-state"), SessionId);
	return IFileManager::Get().DirectoryExists(*SessionDir) ? SessionDir : FString();
}

bool FACPCopilotHistoryReader::ParseWorkspaceMetadata(const FString& WorkspaceYamlPath, FACPRemoteSessionEntry& OutEntry, FString& OutGitRoot, FString& OutCwd)
{
	if (WorkspaceYamlPath.IsEmpty() || !IFileManager::Get().FileExists(*WorkspaceYamlPath))
	{
		return false;
	}

	FString Yaml;
	if (!FFileHelper::LoadFileToString(Yaml, *WorkspaceYamlPath))
	{
		return false;
	}

	OutEntry = FACPRemoteSessionEntry();
	OutEntry.SessionId = ParseYamlScalar(Yaml, TEXT("id"));
	OutEntry.Title = ParseYamlScalar(Yaml, TEXT("summary"));
	OutGitRoot = ParseYamlScalar(Yaml, TEXT("git_root"));
	OutCwd = ParseYamlScalar(Yaml, TEXT("cwd"));

	const FString UpdatedAt = ParseYamlScalar(Yaml, TEXT("updated_at"));
	ParseIsoTimeCopilot(UpdatedAt, OutEntry.UpdatedAt);

	if (OutEntry.SessionId.IsEmpty())
	{
		return false;
	}

	if (OutEntry.Title.IsEmpty())
	{
		TArray<FACPChatMessage> ParsedMessages;
		if (ParseSession(OutEntry.SessionId, ParsedMessages, nullptr))
		{
			OutEntry.Title = BuildTitleFromMessages(ParsedMessages);
		}
	}

	return true;
}

FString FACPCopilotHistoryReader::BuildTitleFromMessages(const TArray<FACPChatMessage>& Messages)
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

	return TEXT("Copilot Session");
}
