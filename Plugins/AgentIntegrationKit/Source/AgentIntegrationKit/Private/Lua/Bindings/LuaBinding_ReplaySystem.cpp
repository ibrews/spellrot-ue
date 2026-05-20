// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "NetworkReplayStreaming.h"
#include "LocalFileNetworkReplayStreaming.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ─── Documentation ───

static TArray<FLuaFunctionDoc> ReplaySystemDocs = {
	{ TEXT("replay_list(path?)"), TEXT("List all replay files with metadata. Uses default Saved/Demos if path omitted"), TEXT("table[]") },
	{ TEXT("replay_info(name, path?)"), TEXT("Detailed info for a single replay including events and checkpoints"), TEXT("table") },
	{ TEXT("replay_get_events(name, group?, path?)"), TEXT("Get events from a replay, optionally filtered by group"), TEXT("table[]") },
	{ TEXT("replay_delete(name, path?)"), TEXT("Delete a replay file from disk"), TEXT("bool") },
	{ TEXT("replay_rename(name, new_name, path?)"), TEXT("Rename a replay file on disk"), TEXT("bool") },
	{ TEXT("replay_set_friendly_name(name, friendly_name, path?)"), TEXT("Update the friendly name stored in replay file header metadata"), TEXT("table") },
	{ TEXT("replay_keep(name, keep, path?)"), TEXT("Mark a replay as kept (permanent) via file prefix convention"), TEXT("table") },
	{ TEXT("replay_get_default_path()"), TEXT("Return the default replay save directory (Saved/Demos)"), TEXT("string") },
	{ TEXT("replay_get_extension()"), TEXT("Return the replay file extension (.replay)"), TEXT("string") },
	{ TEXT("replay_exists(name, path?)"), TEXT("Check if a replay file exists on disk"), TEXT("bool") },
	{ TEXT("replay_get_duration(name, path?)"), TEXT("Quick query for replay length in milliseconds"), TEXT("int|nil") },
	{ TEXT("replay_search(opts)"), TEXT("Search replays by criteria: min_length_ms, max_length_ms, after_date, before_date, has_events_in_group, name_contains"), TEXT("table[]") },
	{ TEXT("replay_get_size(name, path?)"), TEXT("Get the file size of a replay in bytes without parsing metadata"), TEXT("int|nil") },
	{ TEXT("replay_copy(name, dest_name, path?)"), TEXT("Copy a replay file within the same directory"), TEXT("bool") },
	{ TEXT("replay_validate(name, path?)"), TEXT("Validate replay file integrity: checks magic number, header, and structure"), TEXT("table") },
	{ TEXT("replay_get_checkpoints(name, path?)"), TEXT("Get detailed checkpoint info including sizes and offsets"), TEXT("table[]") },
};

// ─── Derived streamer to expose protected ReadReplayInfo / WriteReplayInfo ───

class FLuaLocalFileReplayStreamer : public FLocalFileNetworkReplayStreamer
{
public:
	FLuaLocalFileReplayStreamer() : FLocalFileNetworkReplayStreamer() {}
	FLuaLocalFileReplayStreamer(const FString& InDemoSavePath) : FLocalFileNetworkReplayStreamer(InDemoSavePath) {}

	bool LuaReadReplayInfo(const FString& StreamName, FLocalFileReplayInfo& OutReplayInfo) const
	{
		return ReadReplayInfo(StreamName, OutReplayInfo);
	}

	FString LuaGetDemoFullFilename(const FString& FileName) const
	{
		return GetDemoFullFilename(FileName);
	}

	bool LuaSetFriendlyName(const FString& StreamName, const FString& NewFriendlyName, FString& OutError)
	{
		const FString FullReplayName = GetDemoFullFilename(StreamName);
		if (!FPaths::FileExists(FullReplayName))
		{
			OutError = TEXT("replay file not found");
			return false;
		}

		FLocalFileReplayInfo TempReplayInfo;

		if (!ReadReplayInfo(StreamName, TempReplayInfo, EReadReplayInfoFlags::None))
		{
			OutError = TEXT("failed to read replay info");
			return false;
		}

		TempReplayInfo.FriendlyName = NewFriendlyName;

		if (!WriteReplayInfo(StreamName, TempReplayInfo))
		{
			OutError = TEXT("failed to write replay info");
			return false;
		}

		return true;
	}

	bool LuaValidateReplay(const FString& StreamName, FLocalFileReplayInfo& OutReplayInfo, FString& OutError)
	{
		const FString FullReplayName = GetDemoFullFilename(StreamName);
		if (!FPaths::FileExists(FullReplayName))
		{
			OutError = TEXT("file not found");
			return false;
		}

		TSharedPtr<FArchive> ReadAr = CreateLocalFileReader(FullReplayName);
		if (!ReadAr.IsValid())
		{
			OutError = TEXT("could not open file for reading");
			return false;
		}

		if (ReadAr->TotalSize() <= 0)
		{
			OutError = TEXT("file is empty");
			return false;
		}

		// Read magic number
		uint32 MagicNumber = 0;
		*ReadAr << MagicNumber;
		if (MagicNumber != FLocalFileNetworkReplayStreamer::FileMagic)
		{
			OutError = TEXT("invalid magic number — not a valid replay file");
			return false;
		}

		// Reset and do full parse
		ReadAr->Seek(0);
		if (!ReadReplayInfo(*ReadAr.Get(), OutReplayInfo, EReadReplayInfoFlags::None))
		{
			OutError = TEXT("header parse failed — file may be corrupted");
			return false;
		}

		return true;
	}
};

// ─── Helpers ───

static TSharedPtr<FLuaLocalFileReplayStreamer> CreateStreamer(const FString& CustomPath = TEXT(""))
{
	if (!CustomPath.IsEmpty())
	{
		return MakeShared<FLuaLocalFileReplayStreamer>(CustomPath);
	}
	return MakeShared<FLuaLocalFileReplayStreamer>();
}

static FString GetDefaultReplayPath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Demos"));
}

static FString ResolvePath(sol::optional<std::string>& PathArg)
{
	if (PathArg.has_value() && !PathArg.value().empty())
	{
		return UTF8_TO_TCHAR(PathArg.value().c_str());
	}
	return GetDefaultReplayPath();
}

static FString BuildFullReplayPath(const FString& Directory, const FString& Name)
{
	return FPaths::Combine(Directory, Name + TEXT(".replay"));
}

static bool IsValidReplayName(const FString& Name)
{
	return !Name.Contains(TEXT("..")) && !Name.Contains(TEXT("/")) && !Name.Contains(TEXT("\\"));
}

// Populate a Lua table for an event entry (shared across functions)
static void PopulateEventEntry(sol::table& Entry, const FLocalFileEventInfo& Ev)
{
	Entry["id"] = std::string(TCHAR_TO_UTF8(*Ev.Id));
	Entry["group"] = std::string(TCHAR_TO_UTF8(*Ev.Group));
	Entry["metadata"] = std::string(TCHAR_TO_UTF8(*Ev.Metadata));
	Entry["time1_ms"] = static_cast<int64>(Ev.Time1);
	Entry["time2_ms"] = static_cast<int64>(Ev.Time2);
}

// Populate a Lua table for a replay summary (shared across list/search)
static void PopulateReplaySummary(sol::table& Entry, const FString& ReplayName, const FLocalFileReplayInfo& Info)
{
	Entry["name"] = std::string(TCHAR_TO_UTF8(*ReplayName));
	Entry["friendly_name"] = std::string(TCHAR_TO_UTF8(*Info.FriendlyName));
	Entry["timestamp"] = std::string(TCHAR_TO_UTF8(*Info.Timestamp.ToString()));
	Entry["length_ms"] = Info.LengthInMS;
	Entry["size_bytes"] = Info.TotalDataSizeInBytes;
	Entry["is_live"] = Info.bIsLive;
	Entry["is_valid"] = Info.bIsValid;
	Entry["compressed"] = Info.bCompressed;
	Entry["encrypted"] = Info.bEncrypted;
	Entry["num_events"] = Info.Events.Num();
	Entry["num_checkpoints"] = Info.Checkpoints.Num();
}

// ─── Implementation ───

REGISTER_LUA_BINDING(ReplaySystem, ReplaySystemDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	// ════════════════════════════════════════════════════════════════════
	// replay_list(path?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("replay_list", [&Session](sol::optional<std::string> PathArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		FString DemoPath = PathArg.has_value() && !PathArg.value().empty()
			? FString(UTF8_TO_TCHAR(PathArg.value().c_str()))
			: GetDefaultReplayPath();

		TSharedPtr<FLuaLocalFileReplayStreamer> Streamer = CreateStreamer(DemoPath);

		TArray<FString> ReplayFiles;
		IFileManager::Get().FindFiles(ReplayFiles, *FPaths::Combine(DemoPath, TEXT("*.replay")), true, false);

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;

		for (const FString& FileName : ReplayFiles)
		{
			FString ReplayName = FPaths::GetBaseFilename(FileName);

			FLocalFileReplayInfo Info;
			bool bSuccess = Streamer->LuaReadReplayInfo(ReplayName, Info);

			sol::table Entry = LuaView.create_table();

			if (bSuccess)
			{
				PopulateReplaySummary(Entry, ReplayName, Info);
			}
			else
			{
				Entry["name"] = std::string(TCHAR_TO_UTF8(*ReplayName));
				Entry["is_valid"] = false;
				Entry["error"] = "failed to read replay info";
			}

			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] replay_list('%s') -> %d replays found"), *DemoPath, Idx - 1));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// replay_info(name, path?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("replay_info", [&Session](const std::string& NameStr, sol::optional<std::string> PathArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (NameStr.empty())
		{
			Session.Log(TEXT("[FAIL] replay_info: name argument is required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FString ReplayName = UTF8_TO_TCHAR(NameStr.c_str());

		if (!IsValidReplayName(ReplayName))
		{
			Session.Log(TEXT("[FAIL] replay_info: name must not contain path separators or '..'"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FString DemoPath = ResolvePath(PathArg);
		TSharedPtr<FLuaLocalFileReplayStreamer> Streamer = CreateStreamer(DemoPath);

		FLocalFileReplayInfo Info;
		bool bSuccess = Streamer->LuaReadReplayInfo(ReplayName, Info);

		if (!bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] replay_info: could not read replay '%s'"), *ReplayName));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		sol::table Result = LuaView.create_table();
		PopulateReplaySummary(Result, ReplayName, Info);
		Result["network_version"] = static_cast<int64>(Info.NetworkVersion);
		Result["changelist"] = static_cast<int64>(Info.Changelist);
		Result["num_data_chunks"] = Info.DataChunks.Num();

		// Events array
		sol::table EventsTable = LuaView.create_table();
		int32 EvIdx = 1;
		for (const FLocalFileEventInfo& Ev : Info.Events)
		{
			sol::table EvEntry = LuaView.create_table();
			PopulateEventEntry(EvEntry, Ev);
			EventsTable[EvIdx++] = EvEntry;
		}
		Result["events"] = EventsTable;

		// Checkpoints array
		sol::table CheckpointsTable = LuaView.create_table();
		int32 CpIdx = 1;
		for (const FLocalFileEventInfo& Cp : Info.Checkpoints)
		{
			sol::table CpEntry = LuaView.create_table();
			CpEntry["id"] = std::string(TCHAR_TO_UTF8(*Cp.Id));
			CpEntry["group"] = std::string(TCHAR_TO_UTF8(*Cp.Group));
			CpEntry["time1_ms"] = static_cast<int64>(Cp.Time1);
			CpEntry["time2_ms"] = static_cast<int64>(Cp.Time2);
			CheckpointsTable[CpIdx++] = CpEntry;
		}
		Result["checkpoints"] = CheckpointsTable;

		Session.Log(FString::Printf(TEXT("[OK] replay_info('%s') -> length=%dms, events=%d, checkpoints=%d"),
			*ReplayName, Info.LengthInMS, Info.Events.Num(), Info.Checkpoints.Num()));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// replay_get_events(name, group?, path?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("replay_get_events", [&Session](const std::string& NameStr,
		sol::optional<std::string> GroupArg, sol::optional<std::string> PathArg,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (NameStr.empty())
		{
			Session.Log(TEXT("[FAIL] replay_get_events: name argument is required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FString ReplayName = UTF8_TO_TCHAR(NameStr.c_str());

		if (!IsValidReplayName(ReplayName))
		{
			Session.Log(TEXT("[FAIL] replay_get_events: name must not contain path separators or '..'"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FString DemoPath = ResolvePath(PathArg);
		TSharedPtr<FLuaLocalFileReplayStreamer> Streamer = CreateStreamer(DemoPath);

		FLocalFileReplayInfo Info;
		bool bSuccess = Streamer->LuaReadReplayInfo(ReplayName, Info);

		if (!bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] replay_get_events: could not read replay '%s'"), *ReplayName));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		bool bFilterByGroup = GroupArg.has_value() && !GroupArg.value().empty();
		FString FilterGroup;
		if (bFilterByGroup)
		{
			FilterGroup = UTF8_TO_TCHAR(GroupArg.value().c_str());
		}

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;

		for (const FLocalFileEventInfo& Ev : Info.Events)
		{
			if (bFilterByGroup && !Ev.Group.Equals(FilterGroup, ESearchCase::IgnoreCase))
			{
				continue;
			}

			sol::table EvEntry = LuaView.create_table();
			PopulateEventEntry(EvEntry, Ev);
			Result[Idx++] = EvEntry;
		}

		Session.Log(FString::Printf(TEXT("[OK] replay_get_events('%s'%s) -> %d events"),
			*ReplayName,
			bFilterByGroup ? *FString::Printf(TEXT(", group='%s'"), *FilterGroup) : TEXT(""),
			Idx - 1));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// replay_delete(name, path?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("replay_delete", [&Session](const std::string& NameStr,
		sol::optional<std::string> PathArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (NameStr.empty())
		{
			Session.Log(TEXT("[FAIL] replay_delete: name argument is required"));
			return sol::make_object(LuaView, false);
		}

		FString ReplayName = UTF8_TO_TCHAR(NameStr.c_str());

		if (!IsValidReplayName(ReplayName))
		{
			Session.Log(TEXT("[FAIL] replay_delete: name must not contain path separators or '..'"));
			return sol::make_object(LuaView, false);
		}

		FString DemoPath = ResolvePath(PathArg);
		FString FullPath = BuildFullReplayPath(DemoPath, ReplayName);

		if (!IFileManager::Get().FileExists(*FullPath))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] replay_delete: file not found '%s'"), *FullPath));
			return sol::make_object(LuaView, false);
		}

		bool bDeleted = IFileManager::Get().Delete(*FullPath, false, true);

		if (bDeleted)
		{
			Session.Log(FString::Printf(TEXT("[OK] replay_delete('%s') -> deleted"), *ReplayName));
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] replay_delete('%s') -> could not delete file"), *ReplayName));
		}

		return sol::make_object(LuaView, bDeleted);
	});

	// ════════════════════════════════════════════════════════════════════
	// replay_rename(name, new_name, path?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("replay_rename", [&Session](const std::string& NameStr, const std::string& NewNameStr,
		sol::optional<std::string> PathArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (NameStr.empty() || NewNameStr.empty())
		{
			Session.Log(TEXT("[FAIL] replay_rename: both name and new_name arguments are required"));
			return sol::make_object(LuaView, false);
		}

		FString OldName = UTF8_TO_TCHAR(NameStr.c_str());
		FString NewName = UTF8_TO_TCHAR(NewNameStr.c_str());

		if (!IsValidReplayName(OldName) || !IsValidReplayName(NewName))
		{
			Session.Log(TEXT("[FAIL] replay_rename: names must not contain path separators or '..'"));
			return sol::make_object(LuaView, false);
		}

		FString DemoPath = ResolvePath(PathArg);
		FString OldPath = BuildFullReplayPath(DemoPath, OldName);
		FString NewPath = BuildFullReplayPath(DemoPath, NewName);

		if (!IFileManager::Get().FileExists(*OldPath))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] replay_rename: source file not found '%s'"), *OldPath));
			return sol::make_object(LuaView, false);
		}

		if (IFileManager::Get().FileExists(*NewPath))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] replay_rename: destination file already exists '%s'"), *NewPath));
			return sol::make_object(LuaView, false);
		}

		bool bMoved = IFileManager::Get().Move(*NewPath, *OldPath, true, true);

		if (bMoved)
		{
			Session.Log(FString::Printf(TEXT("[OK] replay_rename('%s' -> '%s')"), *OldName, *NewName));
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] replay_rename('%s' -> '%s') -> move failed"), *OldName, *NewName));
		}

		return sol::make_object(LuaView, bMoved);
	});

	// ════════════════════════════════════════════════════════════════════
	// replay_set_friendly_name(name, friendly_name, path?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("replay_set_friendly_name", [&Session](const std::string& NameStr,
		const std::string& FriendlyNameStr, sol::optional<std::string> PathArg,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (NameStr.empty())
		{
			Session.Log(TEXT("[FAIL] replay_set_friendly_name: name argument is required"));
			sol::table Err = LuaView.create_table();
			Err["success"] = false;
			Err["error"] = "name argument is required";
			return sol::make_object(LuaView, Err);
		}

		FString ReplayName = UTF8_TO_TCHAR(NameStr.c_str());

		if (!IsValidReplayName(ReplayName))
		{
			Session.Log(TEXT("[FAIL] replay_set_friendly_name: name must not contain path separators or '..'"));
			sol::table Err = LuaView.create_table();
			Err["success"] = false;
			Err["error"] = "name must not contain path separators or '..'";
			return sol::make_object(LuaView, Err);
		}

		FString DemoPath = ResolvePath(PathArg);
		TSharedPtr<FLuaLocalFileReplayStreamer> Streamer = CreateStreamer(DemoPath);

		FString NewFriendlyName = UTF8_TO_TCHAR(FriendlyNameStr.c_str());
		FString OutError;
		bool bSuccess = Streamer->LuaSetFriendlyName(ReplayName, NewFriendlyName, OutError);

		sol::table Result = LuaView.create_table();
		Result["name"] = NameStr;
		Result["success"] = bSuccess;

		if (bSuccess)
		{
			Result["friendly_name"] = FriendlyNameStr;
			Session.Log(FString::Printf(TEXT("[OK] replay_set_friendly_name('%s') -> '%s'"),
				*ReplayName, *NewFriendlyName));
		}
		else
		{
			Result["error"] = std::string(TCHAR_TO_UTF8(*OutError));
			Session.Log(FString::Printf(TEXT("[FAIL] replay_set_friendly_name('%s'): %s"),
				*ReplayName, *OutError));
		}

		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// replay_keep(name, keep, path?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("replay_keep", [&Session](const std::string& NameStr, bool bKeep,
		sol::optional<std::string> PathArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (NameStr.empty())
		{
			Session.Log(TEXT("[FAIL] replay_keep: name argument is required"));
			sol::table Err = LuaView.create_table();
			Err["success"] = false;
			Err["error"] = "name argument is required";
			return sol::make_object(LuaView, Err);
		}

		FString ReplayName = UTF8_TO_TCHAR(NameStr.c_str());

		if (!IsValidReplayName(ReplayName))
		{
			Session.Log(TEXT("[FAIL] replay_keep: name must not contain path separators or '..'"));
			sol::table Err = LuaView.create_table();
			Err["success"] = false;
			Err["error"] = "name must not contain path separators or '..'";
			return sol::make_object(LuaView, Err);
		}

		FString DemoPath = ResolvePath(PathArg);

		// Convention: prefix the replay file with "KEEP_" to mark it as permanent.
		// The engine's KeepReplay_Internal is a no-op for local files (just checks existence),
		// so this file-level convention provides actual "keep" semantics.
		bool bHasKeepPrefix = ReplayName.StartsWith(TEXT("KEEP_"));
		FString OriginalPath = BuildFullReplayPath(DemoPath, ReplayName);

		sol::table Result = LuaView.create_table();
		Result["name"] = NameStr;
		Result["keep"] = bKeep;

		if (bKeep && !bHasKeepPrefix)
		{
			FString NewName = TEXT("KEEP_") + ReplayName;
			FString NewPath = BuildFullReplayPath(DemoPath, NewName);

			if (!IFileManager::Get().FileExists(*OriginalPath))
			{
				Result["success"] = false;
				Result["error"] = "replay file not found";
				Session.Log(FString::Printf(TEXT("[FAIL] replay_keep('%s'): file not found"), *ReplayName));
				return sol::make_object(LuaView, Result);
			}

			bool bMoved = IFileManager::Get().Move(*NewPath, *OriginalPath, true, true);
			Result["success"] = bMoved;
			Result["new_name"] = std::string(TCHAR_TO_UTF8(*NewName));

			if (bMoved)
			{
				Session.Log(FString::Printf(TEXT("[OK] replay_keep('%s') -> marked as KEEP ('%s')"), *ReplayName, *NewName));
			}
			else
			{
				Result["error"] = "failed to rename file";
				Session.Log(FString::Printf(TEXT("[FAIL] replay_keep('%s') -> rename failed"), *ReplayName));
			}
		}
		else if (!bKeep && bHasKeepPrefix)
		{
			FString BaseName = ReplayName.Mid(5); // Remove "KEEP_"
			FString NewPath = BuildFullReplayPath(DemoPath, BaseName);

			if (!IFileManager::Get().FileExists(*OriginalPath))
			{
				Result["success"] = false;
				Result["error"] = "replay file not found";
				Session.Log(FString::Printf(TEXT("[FAIL] replay_keep('%s'): file not found"), *ReplayName));
				return sol::make_object(LuaView, Result);
			}

			bool bMoved = IFileManager::Get().Move(*NewPath, *OriginalPath, true, true);
			Result["success"] = bMoved;
			Result["new_name"] = std::string(TCHAR_TO_UTF8(*BaseName));

			if (bMoved)
			{
				Session.Log(FString::Printf(TEXT("[OK] replay_keep('%s') -> unmarked KEEP ('%s')"), *ReplayName, *BaseName));
			}
			else
			{
				Result["error"] = "failed to rename file";
				Session.Log(FString::Printf(TEXT("[FAIL] replay_keep('%s') -> rename failed"), *ReplayName));
			}
		}
		else
		{
			// Already in the desired state
			Result["success"] = true;
			Result["new_name"] = NameStr;
			Session.Log(FString::Printf(TEXT("[OK] replay_keep('%s') -> already in desired state (keep=%s)"),
				*ReplayName, bKeep ? TEXT("true") : TEXT("false")));
		}

		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// replay_get_default_path()
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("replay_get_default_path", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		FString DefaultPath = GetDefaultReplayPath();
		Session.Log(FString::Printf(TEXT("[OK] replay_get_default_path -> '%s'"), *DefaultPath));
		return sol::make_object(LuaView, std::string(TCHAR_TO_UTF8(*DefaultPath)));
	});

	// ════════════════════════════════════════════════════════════════════
	// replay_get_extension()
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("replay_get_extension", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		Session.Log(TEXT("[OK] replay_get_extension -> '.replay'"));
		return sol::make_object(LuaView, std::string(".replay"));
	});

	// ════════════════════════════════════════════════════════════════════
	// replay_exists(name, path?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("replay_exists", [&Session](const std::string& NameStr,
		sol::optional<std::string> PathArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (NameStr.empty())
		{
			Session.Log(TEXT("[FAIL] replay_exists: name argument is required"));
			return sol::make_object(LuaView, false);
		}

		FString ReplayName = UTF8_TO_TCHAR(NameStr.c_str());

		if (!IsValidReplayName(ReplayName))
		{
			Session.Log(TEXT("[FAIL] replay_exists: name must not contain path separators or '..'"));
			return sol::make_object(LuaView, false);
		}

		FString DemoPath = ResolvePath(PathArg);
		FString FullPath = BuildFullReplayPath(DemoPath, ReplayName);

		bool bExists = IFileManager::Get().FileExists(*FullPath);

		Session.Log(FString::Printf(TEXT("[OK] replay_exists('%s') -> %s"), *ReplayName, bExists ? TEXT("true") : TEXT("false")));
		return sol::make_object(LuaView, bExists);
	});

	// ════════════════════════════════════════════════════════════════════
	// replay_get_duration(name, path?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("replay_get_duration", [&Session](const std::string& NameStr,
		sol::optional<std::string> PathArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (NameStr.empty())
		{
			Session.Log(TEXT("[FAIL] replay_get_duration: name argument is required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FString ReplayName = UTF8_TO_TCHAR(NameStr.c_str());

		if (!IsValidReplayName(ReplayName))
		{
			Session.Log(TEXT("[FAIL] replay_get_duration: name must not contain path separators or '..'"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FString DemoPath = ResolvePath(PathArg);
		TSharedPtr<FLuaLocalFileReplayStreamer> Streamer = CreateStreamer(DemoPath);

		FLocalFileReplayInfo Info;
		bool bSuccess = Streamer->LuaReadReplayInfo(ReplayName, Info);

		if (!bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] replay_get_duration: could not read replay '%s'"), *ReplayName));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		Session.Log(FString::Printf(TEXT("[OK] replay_get_duration('%s') -> %dms"), *ReplayName, Info.LengthInMS));
		return sol::make_object(LuaView, Info.LengthInMS);
	});

	// ════════════════════════════════════════════════════════════════════
	// replay_search(opts)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("replay_search", [&Session](sol::table Opts, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		// Parse search criteria
		sol::optional<int> MinLengthMs = Opts.get<sol::optional<int>>("min_length_ms");
		sol::optional<int> MaxLengthMs = Opts.get<sol::optional<int>>("max_length_ms");
		sol::optional<std::string> AfterDateStr = Opts.get<sol::optional<std::string>>("after_date");
		sol::optional<std::string> BeforeDateStr = Opts.get<sol::optional<std::string>>("before_date");
		sol::optional<std::string> EventGroupFilter = Opts.get<sol::optional<std::string>>("has_events_in_group");
		sol::optional<std::string> NameContains = Opts.get<sol::optional<std::string>>("name_contains");
		sol::optional<std::string> PathArg = Opts.get<sol::optional<std::string>>("path");

		FString DemoPath = PathArg.has_value() && !PathArg.value().empty()
			? FString(UTF8_TO_TCHAR(PathArg.value().c_str()))
			: GetDefaultReplayPath();

		TSharedPtr<FLuaLocalFileReplayStreamer> Streamer = CreateStreamer(DemoPath);

		// Parse date filters if provided
		FDateTime AfterDate = FDateTime::MinValue();
		FDateTime BeforeDate = FDateTime::MaxValue();

		if (AfterDateStr.has_value() && !AfterDateStr.value().empty())
		{
			FDateTime Parsed;
			if (FDateTime::Parse(UTF8_TO_TCHAR(AfterDateStr.value().c_str()), Parsed))
			{
				AfterDate = Parsed;
			}
		}

		if (BeforeDateStr.has_value() && !BeforeDateStr.value().empty())
		{
			FDateTime Parsed;
			if (FDateTime::Parse(UTF8_TO_TCHAR(BeforeDateStr.value().c_str()), Parsed))
			{
				BeforeDate = Parsed;
			}
		}

		FString NameFilter;
		if (NameContains.has_value() && !NameContains.value().empty())
		{
			NameFilter = UTF8_TO_TCHAR(NameContains.value().c_str());
		}

		FString GroupFilter;
		if (EventGroupFilter.has_value() && !EventGroupFilter.value().empty())
		{
			GroupFilter = UTF8_TO_TCHAR(EventGroupFilter.value().c_str());
		}

		TArray<FString> ReplayFiles;
		IFileManager::Get().FindFiles(ReplayFiles, *FPaths::Combine(DemoPath, TEXT("*.replay")), true, false);

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;

		for (const FString& FileName : ReplayFiles)
		{
			FString ReplayName = FPaths::GetBaseFilename(FileName);

			// Name contains filter (before reading info for efficiency)
			if (!NameFilter.IsEmpty() && !ReplayName.Contains(NameFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}

			FLocalFileReplayInfo Info;
			bool bSuccess = Streamer->LuaReadReplayInfo(ReplayName, Info);

			if (!bSuccess || !Info.bIsValid)
			{
				continue;
			}

			// Length filters
			if (MinLengthMs.has_value() && Info.LengthInMS < MinLengthMs.value())
			{
				continue;
			}
			if (MaxLengthMs.has_value() && Info.LengthInMS > MaxLengthMs.value())
			{
				continue;
			}

			// Date filters
			if (Info.Timestamp < AfterDate || Info.Timestamp > BeforeDate)
			{
				continue;
			}

			// Event group filter
			if (!GroupFilter.IsEmpty())
			{
				bool bHasGroup = false;
				for (const FLocalFileEventInfo& Ev : Info.Events)
				{
					if (Ev.Group.Equals(GroupFilter, ESearchCase::IgnoreCase))
					{
						bHasGroup = true;
						break;
					}
				}
				if (!bHasGroup)
				{
					continue;
				}
			}

			// Passed all filters
			sol::table Entry = LuaView.create_table();
			PopulateReplaySummary(Entry, ReplayName, Info);
			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] replay_search -> %d matching replays"), Idx - 1));
		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// replay_get_size(name, path?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("replay_get_size", [&Session](const std::string& NameStr,
		sol::optional<std::string> PathArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (NameStr.empty())
		{
			Session.Log(TEXT("[FAIL] replay_get_size: name argument is required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FString ReplayName = UTF8_TO_TCHAR(NameStr.c_str());

		if (!IsValidReplayName(ReplayName))
		{
			Session.Log(TEXT("[FAIL] replay_get_size: name must not contain path separators or '..'"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FString DemoPath = ResolvePath(PathArg);
		FString FullPath = BuildFullReplayPath(DemoPath, ReplayName);

		int64 FileSize = IFileManager::Get().FileSize(*FullPath);
		if (FileSize < 0)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] replay_get_size('%s'): file not found"), *ReplayName));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		Session.Log(FString::Printf(TEXT("[OK] replay_get_size('%s') -> %lld bytes"), *ReplayName, FileSize));
		return sol::make_object(LuaView, FileSize);
	});

	// ════════════════════════════════════════════════════════════════════
	// replay_copy(name, dest_name, path?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("replay_copy", [&Session](const std::string& NameStr, const std::string& DestNameStr,
		sol::optional<std::string> PathArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (NameStr.empty() || DestNameStr.empty())
		{
			Session.Log(TEXT("[FAIL] replay_copy: both name and dest_name arguments are required"));
			return sol::make_object(LuaView, false);
		}

		FString SrcName = UTF8_TO_TCHAR(NameStr.c_str());
		FString DestName = UTF8_TO_TCHAR(DestNameStr.c_str());

		if (!IsValidReplayName(SrcName) || !IsValidReplayName(DestName))
		{
			Session.Log(TEXT("[FAIL] replay_copy: names must not contain path separators or '..'"));
			return sol::make_object(LuaView, false);
		}

		FString DemoPath = ResolvePath(PathArg);
		FString SrcPath = BuildFullReplayPath(DemoPath, SrcName);
		FString DestPath = BuildFullReplayPath(DemoPath, DestName);

		if (!IFileManager::Get().FileExists(*SrcPath))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] replay_copy: source file not found '%s'"), *SrcPath));
			return sol::make_object(LuaView, false);
		}

		if (IFileManager::Get().FileExists(*DestPath))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] replay_copy: destination file already exists '%s'"), *DestPath));
			return sol::make_object(LuaView, false);
		}

		uint32 CopyResult = IFileManager::Get().Copy(*DestPath, *SrcPath);
		bool bSuccess = (CopyResult == COPY_OK);

		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] replay_copy('%s' -> '%s')"), *SrcName, *DestName));
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] replay_copy('%s' -> '%s') -> copy failed"), *SrcName, *DestName));
		}

		return sol::make_object(LuaView, bSuccess);
	});

	// ════════════════════════════════════════════════════════════════════
	// replay_validate(name, path?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("replay_validate", [&Session](const std::string& NameStr,
		sol::optional<std::string> PathArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (NameStr.empty())
		{
			Session.Log(TEXT("[FAIL] replay_validate: name argument is required"));
			sol::table Err = LuaView.create_table();
			Err["valid"] = false;
			Err["error"] = "name argument is required";
			return sol::make_object(LuaView, Err);
		}

		FString ReplayName = UTF8_TO_TCHAR(NameStr.c_str());

		if (!IsValidReplayName(ReplayName))
		{
			Session.Log(TEXT("[FAIL] replay_validate: name must not contain path separators or '..'"));
			sol::table Err = LuaView.create_table();
			Err["valid"] = false;
			Err["error"] = "name must not contain path separators or '..'";
			return sol::make_object(LuaView, Err);
		}

		FString DemoPath = ResolvePath(PathArg);
		TSharedPtr<FLuaLocalFileReplayStreamer> Streamer = CreateStreamer(DemoPath);

		FLocalFileReplayInfo Info;
		FString OutError;
		bool bValid = Streamer->LuaValidateReplay(ReplayName, Info, OutError);

		sol::table Result = LuaView.create_table();
		Result["name"] = NameStr;
		Result["valid"] = bValid;

		if (bValid)
		{
			Result["length_ms"] = Info.LengthInMS;
			Result["network_version"] = static_cast<int64>(Info.NetworkVersion);
			Result["changelist"] = static_cast<int64>(Info.Changelist);
			Result["is_valid"] = Info.bIsValid;
			Result["compressed"] = Info.bCompressed;
			Result["encrypted"] = Info.bEncrypted;
			Result["num_chunks"] = Info.Chunks.Num();
			Result["num_events"] = Info.Events.Num();
			Result["num_checkpoints"] = Info.Checkpoints.Num();
			Result["num_data_chunks"] = Info.DataChunks.Num();

			Session.Log(FString::Printf(TEXT("[OK] replay_validate('%s') -> valid, %d chunks, length=%dms"),
				*ReplayName, Info.Chunks.Num(), Info.LengthInMS));
		}
		else
		{
			Result["error"] = std::string(TCHAR_TO_UTF8(*OutError));
			Session.Log(FString::Printf(TEXT("[FAIL] replay_validate('%s'): %s"), *ReplayName, *OutError));
		}

		return sol::make_object(LuaView, Result);
	});

	// ════════════════════════════════════════════════════════════════════
	// replay_get_checkpoints(name, path?)
	// ════════════════════════════════════════════════════════════════════
	Lua.set_function("replay_get_checkpoints", [&Session](const std::string& NameStr,
		sol::optional<std::string> PathArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (NameStr.empty())
		{
			Session.Log(TEXT("[FAIL] replay_get_checkpoints: name argument is required"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FString ReplayName = UTF8_TO_TCHAR(NameStr.c_str());

		if (!IsValidReplayName(ReplayName))
		{
			Session.Log(TEXT("[FAIL] replay_get_checkpoints: name must not contain path separators or '..'"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FString DemoPath = ResolvePath(PathArg);
		TSharedPtr<FLuaLocalFileReplayStreamer> Streamer = CreateStreamer(DemoPath);

		FLocalFileReplayInfo Info;
		bool bSuccess = Streamer->LuaReadReplayInfo(ReplayName, Info);

		if (!bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] replay_get_checkpoints: could not read replay '%s'"), *ReplayName));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		sol::table Result = LuaView.create_table();
		int32 Idx = 1;

		for (const FLocalFileEventInfo& Cp : Info.Checkpoints)
		{
			sol::table CpEntry = LuaView.create_table();
			CpEntry["id"] = std::string(TCHAR_TO_UTF8(*Cp.Id));
			CpEntry["group"] = std::string(TCHAR_TO_UTF8(*Cp.Group));
			CpEntry["metadata"] = std::string(TCHAR_TO_UTF8(*Cp.Metadata));
			CpEntry["time1_ms"] = static_cast<int64>(Cp.Time1);
			CpEntry["time2_ms"] = static_cast<int64>(Cp.Time2);
			CpEntry["size_bytes"] = Cp.SizeInBytes;
			CpEntry["data_offset"] = Cp.EventDataOffset;
			CpEntry["chunk_index"] = Cp.ChunkIndex;
			Result[Idx++] = CpEntry;
		}

		Session.Log(FString::Printf(TEXT("[OK] replay_get_checkpoints('%s') -> %d checkpoints"), *ReplayName, Idx - 1));
		return sol::make_object(LuaView, Result);
	});
});
