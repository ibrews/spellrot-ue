// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlOperation.h"
#include "SourceControlOperations.h"
#include "ISourceControlState.h"
#include "ISourceControlRevision.h"
#include "ISourceControlChangelist.h"
#include "ISourceControlChangelistState.h"
#include "SourceControlHelpers.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ─── Documentation ───

static TArray<FLuaFunctionDoc> SourceControlDocs = {
	// Status
	{ TEXT("sc_is_enabled()"),                          TEXT("Check if source control is enabled"),                         TEXT("bool") },
	{ TEXT("sc_is_available()"),                         TEXT("Check if source control provider is available"),              TEXT("bool") },
	{ TEXT("sc_provider()"),                             TEXT("Get the name of the current source control provider"),        TEXT("string") },
	{ TEXT("sc_status()"),                               TEXT("Get provider status as a table of key-value pairs"),          TEXT("{key=value,...}") },
	{ TEXT("sc_last_error()"),                           TEXT("Get the last source control error message"),                  TEXT("string") },
	// File Operations
	{ TEXT("sc_checkout(files)"),                        TEXT("Check out file(s) — accepts string or array"),               TEXT("{success=bool}") },
	{ TEXT("sc_add(files)"),                             TEXT("Mark file(s) for add — accepts string or array"),            TEXT("{success=bool}") },
	{ TEXT("sc_checkout_or_add(files)"),                 TEXT("Check out or add file(s) — accepts string or array"),        TEXT("{success=bool}") },
	{ TEXT("sc_delete(files)"),                          TEXT("Mark file(s) for delete — accepts string or array"),         TEXT("{success=bool}") },
	{ TEXT("sc_revert(files)"),                          TEXT("Revert file(s) — accepts string or array"),                  TEXT("{success=bool}") },
	{ TEXT("sc_revert_unchanged(files)"),                TEXT("Revert unchanged file(s) — accepts string or array"),        TEXT("{success=bool}") },
	{ TEXT("sc_sync(files)"),                            TEXT("Sync file(s) to head — accepts string or array"),            TEXT("{success=bool}") },
	{ TEXT("sc_checkin(files, description, opts?)"),      TEXT("Check in file(s) with description — opts: {keep_checked_out=bool}"), TEXT("{success=bool}") },
	{ TEXT("sc_copy(src, dst)"),                         TEXT("Copy a file under source control"),                           TEXT("bool") },
	// File State
	{ TEXT("sc_query_state(file)"),                      TEXT("Query source control state of a single file"),                TEXT("{filename, is_valid, is_checked_out, checked_out_by, ...}") },
	{ TEXT("sc_query_states(files)"),                    TEXT("Batch query source control state of files (array)"),          TEXT("array of state tables") },
	// History
	{ TEXT("sc_query_history(file, max_revisions?)"),    TEXT("Query revision history for a file"),                          TEXT("array of {revision, description, user, date, action, file_size}") },
	// Changelist Management
	{ TEXT("sc_list_changelists()"),                     TEXT("List all pending changelists"),                               TEXT("array of {id, description, file_count}") },
	{ TEXT("sc_new_changelist(description)"),            TEXT("Create a new changelist"),                                    TEXT("string (changelist id) or nil") },
	{ TEXT("sc_edit_changelist(id, description)"),       TEXT("Edit a changelist description"),                              TEXT("bool") },
	{ TEXT("sc_delete_changelist(id)"),                  TEXT("Delete an empty changelist"),                                 TEXT("bool") },
	{ TEXT("sc_move_to_changelist(files, changelist_id)"), TEXT("Move files to a changelist"),                              TEXT("bool") },
	{ TEXT("sc_changelist_files(id)"),                   TEXT("List files in a changelist with their states"),              TEXT("array of state tables") },
	// Provider Info
	{ TEXT("sc_capabilities()"),                         TEXT("Query provider capabilities (changelists, checkout, etc)"),  TEXT("{uses_changelists, uses_checkout, ...}") },
	// Utility
	{ TEXT("sc_package_filename(package_path)"),         TEXT("Convert a package path to a filename"),                       TEXT("string") },
	{ TEXT("sc_get_files_in_depot(path)"),               TEXT("Get list of files in depot at path"),                         TEXT("array of strings") },
	// Advanced Operations
	{ TEXT("sc_shelve(changelist_id, files?, description?)"), TEXT("Shelve files in a changelist — if files omitted, shelves all files in the CL"), TEXT("{success=bool}") },
	{ TEXT("sc_unshelve(changelist_id)"),                TEXT("Unshelve files from a changelist"),                           TEXT("{success=bool}") },
	{ TEXT("sc_delete_shelved(changelist_id)"),          TEXT("Delete shelved files from a changelist"),                     TEXT("{success=bool}") },
	{ TEXT("sc_sync_revision(files, revision?)"),        TEXT("Sync file(s) to a specific revision — omit revision for head"), TEXT("{success=bool}") },
	{ TEXT("sc_resolve(files)"),                         TEXT("Resolve conflicted file(s) — marks conflicts as resolved"),  TEXT("{success=bool}") },
	{ TEXT("sc_get_history(file, opts?)"),               TEXT("Get submitted changelist history — opts: {max_count=N}"),            TEXT("array of {changelist, description, user, date, files}") },
	// Revision Queries
	{ TEXT("sc_current_revision(file)"),                 TEXT("Get the currently synced revision for a file"),                   TEXT("{revision, revision_number, user, date, ...} or nil") },
	{ TEXT("sc_find_revision(file, revision)"),          TEXT("Find a specific revision by number or string"),                  TEXT("{revision, description, user, date, ...} or nil") },
	{ TEXT("sc_resolve_info(file)"),                     TEXT("Get merge resolve info for a conflicted file"),                  TEXT("{remote_file, base_file, remote_revision, base_revision} or nil") },
	// Provider Queries
	{ TEXT("sc_is_at_latest()"),                         TEXT("Check if workspace is at latest revision (provider-dependent)"), TEXT("bool or nil") },
	{ TEXT("sc_local_changes()"),                        TEXT("Get count of local workspace changes (provider-dependent)"),     TEXT("int or nil") },
	// Changelist Extras
	{ TEXT("sc_shelved_files(changelist_id)"),           TEXT("List shelved files in a changelist"),                             TEXT("array of state tables") },
	// Annotation
	{ TEXT("sc_annotate(file, label_or_cl)"),            TEXT("Get file annotation/blame for a label (string) or changelist number (int)"), TEXT("array of {change_number, user, line}") },
};

// ─── Helpers ───

static ISourceControlProvider& GetSCProvider()
{
	return ISourceControlModule::Get().GetProvider();
}

static TArray<FString> ToFileArray(const sol::object& Input)
{
	TArray<FString> Files;
	if (Input.is<std::string>())
	{
		Files.Add(UTF8_TO_TCHAR(Input.as<std::string>().c_str()));
	}
	else if (Input.is<sol::table>())
	{
		sol::table T = Input.as<sol::table>();
		for (auto& Pair : T)
		{
			if (Pair.second.is<std::string>())
				Files.Add(UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str()));
		}
	}
	return Files;
}

static sol::table StateToTable(sol::state_view& LuaView, const FSourceControlState& S)
{
	sol::table T = LuaView.create_table();
	T["filename"]              = TCHAR_TO_UTF8(*S.Filename);
	T["is_valid"]              = S.bIsValid;
	T["is_unknown"]            = S.bIsUnknown;
	T["is_checked_out"]        = S.bIsCheckedOut;
	T["is_checked_out_other"]  = S.bIsCheckedOutOther;
	T["is_current"]            = S.bIsCurrent;
	T["is_source_controlled"]  = S.bIsSourceControlled;
	T["is_added"]              = S.bIsAdded;
	T["is_deleted"]            = S.bIsDeleted;
	T["is_ignored"]            = S.bIsIgnored;
	T["can_edit"]              = S.bCanEdit;
	T["can_delete"]            = S.bCanDelete;
	T["is_modified"]           = S.bIsModified;
	T["can_add"]               = S.bCanAdd;
	T["is_conflicted"]         = S.bIsConflicted;
	T["can_revert"]            = S.bCanRevert;
	T["can_checkout"]          = S.bCanCheckOut;
	T["can_checkin"]           = S.bCanCheckIn;
	T["checked_out_by"]        = TCHAR_TO_UTF8(*S.CheckedOutOther);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	T["is_checked_out_in_other_branch"] = S.bIsCheckedOutInOtherBranch;
	T["is_modified_in_other_branch"]    = S.bIsModifiedInOtherBranch;
#endif
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	T["previous_user"]         = TCHAR_TO_UTF8(*S.PreviousUser);
#endif
	return T;
}

static sol::object SC_QueryFileStates(FLuaSessionData& Session, const sol::object& FilesArg, sol::this_state S)
{
	sol::state_view LuaView(S);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	TArray<FString> Files = ToFileArray(FilesArg);
	if (Files.IsEmpty())
	{
		Session.Log(TEXT("[FAIL] sc_query_states: no files specified"));
		return sol::lua_nil;
	}
	TArray<FSourceControlState> States = USourceControlHelpers::QueryFileStates(Files);
	sol::table Result = LuaView.create_table();
	int32 Index = 1;
	for (const FSourceControlState& State : States)
	{
		Result[Index++] = StateToTable(LuaView, State);
	}
	Session.Log(FString::Printf(TEXT("[OK] sc_query_states: %d file(s)"), Files.Num()));
	return Result;
#else
	Session.Log(TEXT("[FAIL] sc_query_states requires UE 5.6+"));
	return sol::lua_nil;
#endif
}

static FSourceControlChangelistPtr FindChangelistById(const FString& Id)
{
	ISourceControlProvider& Provider = GetSCProvider();
	TArray<FSourceControlChangelistRef> Changelists = Provider.GetChangelists(EStateCacheUsage::Use);
	for (const FSourceControlChangelistRef& CL : Changelists)
	{
		if (CL->GetIdentifier() == Id)
		{
			return CL;
		}
	}
	return nullptr;
}

static sol::table RevisionToTable(sol::state_view& LuaView, const TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe>& Rev)
{
	sol::table Entry = LuaView.create_table();
	Entry["revision"]        = TCHAR_TO_UTF8(*Rev->GetRevision());
	Entry["revision_number"] = Rev->GetRevisionNumber();
	Entry["description"]     = TCHAR_TO_UTF8(*Rev->GetDescription());
	Entry["user"]            = TCHAR_TO_UTF8(*Rev->GetUserName());
	Entry["client_spec"]     = TCHAR_TO_UTF8(*Rev->GetClientSpec());
	Entry["date"]            = TCHAR_TO_UTF8(*Rev->GetDate().ToString());
	Entry["action"]          = TCHAR_TO_UTF8(*Rev->GetAction());
	Entry["file_size"]       = Rev->GetFileSize();
	Entry["changelist"]      = Rev->GetCheckInIdentifier();
	return Entry;
}

// ─── Binding ───

static void BindSourceControl(sol::state& Lua, FLuaSessionData& Session)
{
	// ────────────────────────────────────────────
	// 1. sc_is_enabled
	// ────────────────────────────────────────────
	Lua.set_function("sc_is_enabled", [&Session]() -> bool
	{
		bool bEnabled = ISourceControlModule::Get().IsEnabled();
		Session.Log(FString::Printf(TEXT("[OK] sc_is_enabled → %s"), bEnabled ? TEXT("true") : TEXT("false")));
		return bEnabled;
	});

	// ────────────────────────────────────────────
	// 2. sc_is_available
	// ────────────────────────────────────────────
	Lua.set_function("sc_is_available", [&Session]() -> bool
	{
		bool bAvail = GetSCProvider().IsAvailable();
		Session.Log(FString::Printf(TEXT("[OK] sc_is_available → %s"), bAvail ? TEXT("true") : TEXT("false")));
		return bAvail;
	});

	// ────────────────────────────────────────────
	// 3. sc_provider
	// ────────────────────────────────────────────
	Lua.set_function("sc_provider", [&Session]() -> std::string
	{
		FString Name = GetSCProvider().GetName().ToString();
		Session.Log(FString::Printf(TEXT("[OK] sc_provider → %s"), *Name));
		return TCHAR_TO_UTF8(*Name);
	});

	// ────────────────────────────────────────────
	// 4. sc_status
	// ────────────────────────────────────────────
	Lua.set_function("sc_status", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		TMap<ISourceControlProvider::EStatus, FString> StatusMap = GetSCProvider().GetStatus();

		sol::table Result = LuaView.create_table();

		auto SetIfPresent = [&](ISourceControlProvider::EStatus Key, const char* LuaKey)
		{
			if (const FString* Val = StatusMap.Find(Key))
			{
				Result[LuaKey] = TCHAR_TO_UTF8(**Val);
			}
		};

		SetIfPresent(ISourceControlProvider::EStatus::Enabled,       "enabled");
		SetIfPresent(ISourceControlProvider::EStatus::Connected,     "connected");
		SetIfPresent(ISourceControlProvider::EStatus::Port,          "port");
		SetIfPresent(ISourceControlProvider::EStatus::User,          "user");
		SetIfPresent(ISourceControlProvider::EStatus::Client,        "client");
		SetIfPresent(ISourceControlProvider::EStatus::Repository,    "repository");
		SetIfPresent(ISourceControlProvider::EStatus::Remote,        "remote");
		SetIfPresent(ISourceControlProvider::EStatus::Branch,        "branch");
		SetIfPresent(ISourceControlProvider::EStatus::Email,         "email");
		SetIfPresent(ISourceControlProvider::EStatus::ScmVersion,    "scm_version");
		SetIfPresent(ISourceControlProvider::EStatus::PluginVersion, "plugin_version");
		SetIfPresent(ISourceControlProvider::EStatus::Workspace,     "workspace");
		SetIfPresent(ISourceControlProvider::EStatus::WorkspacePath, "workspace_path");
		SetIfPresent(ISourceControlProvider::EStatus::Changeset,     "changeset");

		Session.Log(TEXT("[OK] sc_status"));
		return Result;
	});

	// ────────────────────────────────────────────
	// 5. sc_last_error
	// ────────────────────────────────────────────
	Lua.set_function("sc_last_error", [&Session]() -> std::string
	{
		FString Msg = USourceControlHelpers::LastErrorMsg().ToString();
		Session.Log(FString::Printf(TEXT("[OK] sc_last_error → %s"), *Msg));
		return TCHAR_TO_UTF8(*Msg);
	});

	// ────────────────────────────────────────────
	// 6. sc_checkout
	// ────────────────────────────────────────────
	Lua.set_function("sc_checkout", [&Session](const sol::object& FilesArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		TArray<FString> Files = ToFileArray(FilesArg);
		if (Files.IsEmpty())
		{
			Session.Log(TEXT("[FAIL] sc_checkout: no files specified"));
			return sol::lua_nil;
		}

		bool bSuccess = USourceControlHelpers::CheckOutFiles(Files);
		Session.Log(FString::Printf(TEXT("[%s] sc_checkout: %d file(s)"), bSuccess ? TEXT("OK") : TEXT("FAIL"), Files.Num()));

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		return Result;
	});

	// ────────────────────────────────────────────
	// 7. sc_add
	// ────────────────────────────────────────────
	Lua.set_function("sc_add", [&Session](const sol::object& FilesArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		TArray<FString> Files = ToFileArray(FilesArg);
		if (Files.IsEmpty())
		{
			Session.Log(TEXT("[FAIL] sc_add: no files specified"));
			return sol::lua_nil;
		}

		bool bSuccess = USourceControlHelpers::MarkFilesForAdd(Files);
		Session.Log(FString::Printf(TEXT("[%s] sc_add: %d file(s)"), bSuccess ? TEXT("OK") : TEXT("FAIL"), Files.Num()));

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		return Result;
	});

	// ────────────────────────────────────────────
	// 8. sc_checkout_or_add
	// ────────────────────────────────────────────
	Lua.set_function("sc_checkout_or_add", [&Session](const sol::object& FilesArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		TArray<FString> Files = ToFileArray(FilesArg);
		if (Files.IsEmpty())
		{
			Session.Log(TEXT("[FAIL] sc_checkout_or_add: no files specified"));
			return sol::lua_nil;
		}

		bool bSuccess = USourceControlHelpers::CheckOutOrAddFiles(Files);
		Session.Log(FString::Printf(TEXT("[%s] sc_checkout_or_add: %d file(s)"), bSuccess ? TEXT("OK") : TEXT("FAIL"), Files.Num()));

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		return Result;
	});

	// ────────────────────────────────────────────
	// 9. sc_delete
	// ────────────────────────────────────────────
	Lua.set_function("sc_delete", [&Session](const sol::object& FilesArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		TArray<FString> Files = ToFileArray(FilesArg);
		if (Files.IsEmpty())
		{
			Session.Log(TEXT("[FAIL] sc_delete: no files specified"));
			return sol::lua_nil;
		}

		bool bSuccess = USourceControlHelpers::MarkFilesForDelete(Files);
		Session.Log(FString::Printf(TEXT("[%s] sc_delete: %d file(s)"), bSuccess ? TEXT("OK") : TEXT("FAIL"), Files.Num()));

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		return Result;
	});

	// ────────────────────────────────────────────
	// 10. sc_revert
	// ────────────────────────────────────────────
	Lua.set_function("sc_revert", [&Session](const sol::object& FilesArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		TArray<FString> Files = ToFileArray(FilesArg);
		if (Files.IsEmpty())
		{
			Session.Log(TEXT("[FAIL] sc_revert: no files specified"));
			return sol::lua_nil;
		}

		bool bSuccess = USourceControlHelpers::RevertFiles(Files);
		Session.Log(FString::Printf(TEXT("[%s] sc_revert: %d file(s)"), bSuccess ? TEXT("OK") : TEXT("FAIL"), Files.Num()));

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		return Result;
	});

	// ────────────────────────────────────────────
	// 11. sc_revert_unchanged
	// ────────────────────────────────────────────
	Lua.set_function("sc_revert_unchanged", [&Session](const sol::object& FilesArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		TArray<FString> Files = ToFileArray(FilesArg);
		if (Files.IsEmpty())
		{
			Session.Log(TEXT("[FAIL] sc_revert_unchanged: no files specified"));
			return sol::lua_nil;
		}

		bool bSuccess = USourceControlHelpers::RevertUnchangedFiles(Files);
		Session.Log(FString::Printf(TEXT("[%s] sc_revert_unchanged: %d file(s)"), bSuccess ? TEXT("OK") : TEXT("FAIL"), Files.Num()));

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		return Result;
	});

	// ────────────────────────────────────────────
	// 12. sc_sync
	// ────────────────────────────────────────────
	Lua.set_function("sc_sync", [&Session](const sol::object& FilesArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		TArray<FString> Files = ToFileArray(FilesArg);
		if (Files.IsEmpty())
		{
			Session.Log(TEXT("[FAIL] sc_sync: no files specified"));
			return sol::lua_nil;
		}

		bool bSuccess = USourceControlHelpers::SyncFiles(Files);
		Session.Log(FString::Printf(TEXT("[%s] sc_sync: %d file(s)"), bSuccess ? TEXT("OK") : TEXT("FAIL"), Files.Num()));

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		return Result;
	});

	// ────────────────────────────────────────────
	// 13. sc_checkin
	// ────────────────────────────────────────────
	Lua.set_function("sc_checkin", [&Session](const sol::object& FilesArg, const std::string& Description, sol::optional<sol::table> Opts, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		TArray<FString> Files = ToFileArray(FilesArg);
		if (Files.IsEmpty())
		{
			Session.Log(TEXT("[FAIL] sc_checkin: no files specified"));
			return sol::lua_nil;
		}

		bool bKeepCheckedOut = false;
		if (Opts.has_value())
		{
			sol::table O = Opts.value();
			bKeepCheckedOut = O.get_or("keep_checked_out", false);
		}

		FString Desc = UTF8_TO_TCHAR(Description.c_str());
		bool bSuccess = USourceControlHelpers::CheckInFiles(Files, Desc, /*bSilent=*/false, bKeepCheckedOut);
		Session.Log(FString::Printf(TEXT("[%s] sc_checkin: %d file(s), desc='%s', keep_checked_out=%s"),
			bSuccess ? TEXT("OK") : TEXT("FAIL"), Files.Num(), *Desc, bKeepCheckedOut ? TEXT("true") : TEXT("false")));

		sol::table Result = LuaView.create_table();
		Result["success"] = bSuccess;
		return Result;
	});

	// ────────────────────────────────────────────
	// 14. sc_copy
	// ────────────────────────────────────────────
	Lua.set_function("sc_copy", [&Session](const std::string& Src, const std::string& Dst) -> bool
	{
		FString SrcFile = UTF8_TO_TCHAR(Src.c_str());
		FString DstFile = UTF8_TO_TCHAR(Dst.c_str());

		bool bSuccess = USourceControlHelpers::CopyFile(SrcFile, DstFile);
		Session.Log(FString::Printf(TEXT("[%s] sc_copy: '%s' → '%s'"), bSuccess ? TEXT("OK") : TEXT("FAIL"), *SrcFile, *DstFile));
		return bSuccess;
	});

	// ────────────────────────────────────────────
	// 15. sc_query_state
	// ────────────────────────────────────────────
	Lua.set_function("sc_query_state", [&Session](const std::string& File, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString FilePath = UTF8_TO_TCHAR(File.c_str());

		FSourceControlState State = USourceControlHelpers::QueryFileState(FilePath);

		Session.Log(FString::Printf(TEXT("[%s] sc_query_state: '%s'"), State.bIsValid ? TEXT("OK") : TEXT("WARN"), *FilePath));
		return StateToTable(LuaView, State);
	});

	// ────────────────────────────────────────────
	// 16. sc_query_states
	// ────────────────────────────────────────────
	Lua.set_function("sc_query_states", [&Session](const sol::object& FilesArg, sol::this_state S) -> sol::object
	{
		return SC_QueryFileStates(Session, FilesArg, S);
	});

	// ────────────────────────────────────────────
	// 17. sc_query_history
	// ────────────────────────────────────────────
	Lua.set_function("sc_query_history", [&Session](const std::string& File, sol::optional<int> MaxRevisions, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString FilePath = UTF8_TO_TCHAR(File.c_str());
		int32 MaxItems = MaxRevisions.value_or(32);

		ISourceControlProvider& Provider = GetSCProvider();

		// Execute FUpdateStatus with history enabled
		TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> UpdateOp = ISourceControlOperation::Create<FUpdateStatus>();
		UpdateOp->SetUpdateHistory(true);

		TArray<FString> FileArray;
		FileArray.Add(FilePath);

		ECommandResult::Type Result = Provider.Execute(UpdateOp, FileArray, EConcurrency::Synchronous);
		if (Result != ECommandResult::Succeeded)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] sc_query_history: update status failed for '%s'"), *FilePath));
			return sol::lua_nil;
		}

		// Get the state with history
		FSourceControlStatePtr FileState = Provider.GetState(FilePath, EStateCacheUsage::Use);
		if (!FileState.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] sc_query_history: no state for '%s'"), *FilePath));
			return sol::lua_nil;
		}

		int32 HistorySize = FileState->GetHistorySize();
		int32 Count = FMath::Min(HistorySize, MaxItems);

		sol::table HistoryTable = LuaView.create_table();
		for (int32 i = 0; i < Count; i++)
		{
			TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> Rev = FileState->GetHistoryItem(i);
			if (!Rev.IsValid())
				continue;

			HistoryTable[i + 1] = RevisionToTable(LuaView, Rev);
		}

		Session.Log(FString::Printf(TEXT("[OK] sc_query_history: '%s' → %d revision(s)"), *FilePath, Count));
		return HistoryTable;
	});

	// ────────────────────────────────────────────
	// 18. sc_list_changelists
	// ────────────────────────────────────────────
	Lua.set_function("sc_list_changelists", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		ISourceControlProvider& Provider = GetSCProvider();

		// Fetch pending changelists
		TSharedRef<FGetPendingChangelists, ESPMode::ThreadSafe> GetCLOp = ISourceControlOperation::Create<FGetPendingChangelists>();
		ECommandResult::Type Result = Provider.Execute(GetCLOp, EConcurrency::Synchronous);
		if (Result != ECommandResult::Succeeded)
		{
			Session.Log(TEXT("[FAIL] sc_list_changelists: FGetPendingChangelists failed"));
			return sol::lua_nil;
		}

		// Update changelist status to get descriptions and file counts
		TSharedRef<FUpdatePendingChangelistsStatus, ESPMode::ThreadSafe> UpdateOp = ISourceControlOperation::Create<FUpdatePendingChangelistsStatus>();
		UpdateOp->SetUpdateFilesStates(true);
		UpdateOp->SetUpdateAllChangelists(true);
		Provider.Execute(UpdateOp, EConcurrency::Synchronous);

		// Read changelists from provider
		TArray<FSourceControlChangelistRef> Changelists = Provider.GetChangelists(EStateCacheUsage::Use);

		sol::table ResultTable = LuaView.create_table();
		int32 Index = 1;
		for (const FSourceControlChangelistRef& CL : Changelists)
		{
			FSourceControlChangelistStatePtr CLState = Provider.GetState(CL, EStateCacheUsage::Use);

			sol::table Entry = LuaView.create_table();
			Entry["id"] = TCHAR_TO_UTF8(*CL->GetIdentifier());

			if (CLState.IsValid())
			{
				Entry["description"] = TCHAR_TO_UTF8(*CLState->GetDescriptionText().ToString());
				Entry["file_count"]  = CLState->GetFilesStatesNum();
			}
			else
			{
				Entry["description"] = "";
				Entry["file_count"]  = 0;
			}

			ResultTable[Index++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] sc_list_changelists: %d changelist(s)"), Changelists.Num()));
		return ResultTable;
	});

	// ────────────────────────────────────────────
	// 19. sc_new_changelist
	// ────────────────────────────────────────────
	Lua.set_function("sc_new_changelist", [&Session](const std::string& Description, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		ISourceControlProvider& Provider = GetSCProvider();

		FString Desc = UTF8_TO_TCHAR(Description.c_str());

		TSharedRef<FNewChangelist, ESPMode::ThreadSafe> NewCLOp = ISourceControlOperation::Create<FNewChangelist>();
		NewCLOp->SetDescription(FText::FromString(Desc));

		ECommandResult::Type Result = Provider.Execute(NewCLOp, EConcurrency::Synchronous);
		if (Result != ECommandResult::Succeeded)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] sc_new_changelist: failed to create changelist '%s'"), *Desc));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FSourceControlChangelistPtr NewCL = NewCLOp->GetNewChangelist();
		if (!NewCL.IsValid())
		{
			Session.Log(TEXT("[FAIL] sc_new_changelist: operation succeeded but no changelist returned"));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FString Id = NewCL->GetIdentifier();
		Session.Log(FString::Printf(TEXT("[OK] sc_new_changelist: created CL %s"), *Id));
		return sol::make_object(LuaView, TCHAR_TO_UTF8(*Id));
	});

	// ────────────────────────────────────────────
	// 20. sc_edit_changelist
	// ────────────────────────────────────────────
	Lua.set_function("sc_edit_changelist", [&Session](const std::string& IdStr, const std::string& Description) -> bool
	{
		ISourceControlProvider& Provider = GetSCProvider();
		FString Id = UTF8_TO_TCHAR(IdStr.c_str());
		FString Desc = UTF8_TO_TCHAR(Description.c_str());

		FSourceControlChangelistPtr CL = FindChangelistById(Id);
		if (!CL.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] sc_edit_changelist: changelist '%s' not found"), *Id));
			return false;
		}

		TSharedRef<FEditChangelist, ESPMode::ThreadSafe> EditOp = ISourceControlOperation::Create<FEditChangelist>();
		EditOp->SetDescription(FText::FromString(Desc));

		ECommandResult::Type Result = Provider.Execute(EditOp, CL, EConcurrency::Synchronous);
		bool bSuccess = (Result == ECommandResult::Succeeded);
		Session.Log(FString::Printf(TEXT("[%s] sc_edit_changelist: CL %s"), bSuccess ? TEXT("OK") : TEXT("FAIL"), *Id));
		return bSuccess;
	});

	// ────────────────────────────────────────────
	// 21. sc_delete_changelist
	// ────────────────────────────────────────────
	Lua.set_function("sc_delete_changelist", [&Session](const std::string& IdStr) -> bool
	{
		ISourceControlProvider& Provider = GetSCProvider();
		FString Id = UTF8_TO_TCHAR(IdStr.c_str());

		FSourceControlChangelistPtr CL = FindChangelistById(Id);
		if (!CL.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] sc_delete_changelist: changelist '%s' not found"), *Id));
			return false;
		}

		TSharedRef<FDeleteChangelist, ESPMode::ThreadSafe> DeleteOp = ISourceControlOperation::Create<FDeleteChangelist>();

		ECommandResult::Type Result = Provider.Execute(DeleteOp, CL, EConcurrency::Synchronous);
		bool bSuccess = (Result == ECommandResult::Succeeded);
		Session.Log(FString::Printf(TEXT("[%s] sc_delete_changelist: CL %s"), bSuccess ? TEXT("OK") : TEXT("FAIL"), *Id));
		return bSuccess;
	});

	// ────────────────────────────────────────────
	// 22. sc_move_to_changelist
	// ────────────────────────────────────────────
	Lua.set_function("sc_move_to_changelist", [&Session](const sol::object& FilesArg, const std::string& ChangelistId) -> bool
	{
		ISourceControlProvider& Provider = GetSCProvider();
		TArray<FString> Files = ToFileArray(FilesArg);
		FString Id = UTF8_TO_TCHAR(ChangelistId.c_str());

		if (Files.IsEmpty())
		{
			Session.Log(TEXT("[FAIL] sc_move_to_changelist: no files specified"));
			return false;
		}

		FSourceControlChangelistPtr CL = FindChangelistById(Id);
		if (!CL.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] sc_move_to_changelist: changelist '%s' not found"), *Id));
			return false;
		}

		TSharedRef<FMoveToChangelist, ESPMode::ThreadSafe> MoveOp = ISourceControlOperation::Create<FMoveToChangelist>();

		ECommandResult::Type Result = Provider.Execute(MoveOp, CL, Files, EConcurrency::Synchronous);
		bool bSuccess = (Result == ECommandResult::Succeeded);
		Session.Log(FString::Printf(TEXT("[%s] sc_move_to_changelist: %d file(s) → CL %s"), bSuccess ? TEXT("OK") : TEXT("FAIL"), Files.Num(), *Id));
		return bSuccess;
	});

	// ────────────────────────────────────────────
	// 23. sc_changelist_files
	// ────────────────────────────────────────────
	Lua.set_function("sc_changelist_files", [&Session](const std::string& IdStr, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		ISourceControlProvider& Provider = GetSCProvider();
		FString Id = UTF8_TO_TCHAR(IdStr.c_str());

		FSourceControlChangelistPtr CL = FindChangelistById(Id);
		if (!CL.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] sc_changelist_files: changelist '%s' not found"), *Id));
			return sol::lua_nil;
		}

		// Ensure file states are up to date
		TSharedRef<FUpdatePendingChangelistsStatus, ESPMode::ThreadSafe> UpdateOp = ISourceControlOperation::Create<FUpdatePendingChangelistsStatus>();
		UpdateOp->SetUpdateFilesStates(true);
		TArray<FSourceControlChangelistRef> CLArray;
		CLArray.Add(CL.ToSharedRef());
		UpdateOp->SetChangelistsToUpdate(CLArray);
		Provider.Execute(UpdateOp, EConcurrency::Synchronous);

		FSourceControlChangelistStatePtr CLState = Provider.GetState(CL.ToSharedRef(), EStateCacheUsage::Use);
		if (!CLState.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] sc_changelist_files: could not get state for CL '%s'"), *Id));
			return sol::lua_nil;
		}

		const TArray<FSourceControlStateRef> FileStates = CLState->GetFilesStates();

		sol::table ResultTable = LuaView.create_table();
		int32 Index = 1;
		for (const FSourceControlStateRef& FileState : FileStates)
		{
			sol::table Entry = LuaView.create_table();
			Entry["filename"]              = TCHAR_TO_UTF8(*FileState->GetFilename());
			Entry["is_checked_out"]        = FileState->IsCheckedOut();
			Entry["is_added"]              = FileState->IsAdded();
			Entry["is_deleted"]            = FileState->IsDeleted();
			Entry["is_modified"]           = FileState->IsModified();
			Entry["can_checkin"]           = FileState->CanCheckIn();
			Entry["can_revert"]            = FileState->CanRevert();
			Entry["display_name"]          = TCHAR_TO_UTF8(*FileState->GetDisplayName().ToString());
			ResultTable[Index++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] sc_changelist_files: CL %s → %d file(s)"), *Id, FileStates.Num()));
		return ResultTable;
	});

	// ────────────────────────────────────────────
	// 24. sc_capabilities
	// ────────────────────────────────────────────
	Lua.set_function("sc_capabilities", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		ISourceControlProvider& Provider = GetSCProvider();

		sol::table Result = LuaView.create_table();
		Result["uses_changelists"]            = Provider.UsesChangelists();
		Result["uses_uncontrolled_changelists"] = Provider.UsesUncontrolledChangelists();
		Result["uses_checkout"]               = Provider.UsesCheckout();
		Result["uses_file_revisions"]         = Provider.UsesFileRevisions();
		Result["uses_snapshots"]              = Provider.UsesSnapshots();
		Result["uses_local_read_only_state"]  = Provider.UsesLocalReadOnlyState();
		Result["allows_diff_against_depot"]   = Provider.AllowsDiffAgainstDepot();

		Session.Log(TEXT("[OK] sc_capabilities"));
		return Result;
	});

	// ────────────────────────────────────────────
	// 25. sc_package_filename
	// ────────────────────────────────────────────
	Lua.set_function("sc_package_filename", [&Session](const std::string& PackagePath) -> std::string
	{
		FString PkgPath = UTF8_TO_TCHAR(PackagePath.c_str());
		FString Filename = USourceControlHelpers::PackageFilename(PkgPath);
		Session.Log(FString::Printf(TEXT("[OK] sc_package_filename: '%s' → '%s'"), *PkgPath, *Filename));
		return TCHAR_TO_UTF8(*Filename);
	});

	// ────────────────────────────────────────────
	// 26. sc_get_files_in_depot
	// ────────────────────────────────────────────
	Lua.set_function("sc_get_files_in_depot", [&Session](const std::string& Path, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString DepotPath = UTF8_TO_TCHAR(Path.c_str());

		TArray<FString> FilesList;
		bool bSuccess = USourceControlHelpers::GetFilesInDepotAtPath(DepotPath, FilesList);

		if (!bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] sc_get_files_in_depot: query failed for '%s'"), *DepotPath));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		for (int32 i = 0; i < FilesList.Num(); i++)
		{
			Result[i + 1] = TCHAR_TO_UTF8(*FilesList[i]);
		}

		Session.Log(FString::Printf(TEXT("[OK] sc_get_files_in_depot: '%s' → %d file(s)"), *DepotPath, FilesList.Num()));
		return Result;
	});

	// ────────────────────────────────────────────
	// 27. sc_shelve
	// ────────────────────────────────────────────
	Lua.set_function("sc_shelve", [&Session](const std::string& ChangelistId, sol::optional<sol::object> FilesOrDesc, sol::optional<std::string> DescOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		ISourceControlProvider& Provider = GetSCProvider();
		FString Id = UTF8_TO_TCHAR(ChangelistId.c_str());

		// Check if provider supports the operation
		TSharedRef<FShelve, ESPMode::ThreadSafe> ShelveOp = ISourceControlOperation::Create<FShelve>();
		if (!Provider.CanExecuteOperation(ShelveOp))
		{
			Session.Log(TEXT("[FAIL] sc_shelve: operation not supported by current provider"));
			sol::table R = LuaView.create_table();
			R["success"] = false;
			return R;
		}

		FSourceControlChangelistPtr CL = FindChangelistById(Id);
		if (!CL.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] sc_shelve: changelist '%s' not found"), *Id));
			sol::table R = LuaView.create_table();
			R["success"] = false;
			return R;
		}

		// Parse optional files and description
		TArray<FString> Files;
		FString Description;

		if (FilesOrDesc.has_value())
		{
			sol::object Val = FilesOrDesc.value();
			if (Val.is<std::string>())
			{
				// Could be a single file path or a description — treat as description if no DescOpt
				if (DescOpt.has_value())
				{
					Files.Add(UTF8_TO_TCHAR(Val.as<std::string>().c_str()));
					Description = UTF8_TO_TCHAR(DescOpt.value().c_str());
				}
				else
				{
					Description = UTF8_TO_TCHAR(Val.as<std::string>().c_str());
				}
			}
			else if (Val.is<sol::table>())
			{
				Files = ToFileArray(Val);
				if (DescOpt.has_value())
				{
					Description = UTF8_TO_TCHAR(DescOpt.value().c_str());
				}
			}
		}

		if (!Description.IsEmpty())
		{
			ShelveOp->SetDescription(FText::FromString(Description));
		}

		ECommandResult::Type Result;
		if (Files.IsEmpty())
		{
			Result = Provider.Execute(ShelveOp, CL, EConcurrency::Synchronous);
		}
		else
		{
			Result = Provider.Execute(ShelveOp, CL, Files, EConcurrency::Synchronous);
		}

		bool bSuccess = (Result == ECommandResult::Succeeded);
		Session.Log(FString::Printf(TEXT("[%s] sc_shelve: CL %s, %d file(s)"), bSuccess ? TEXT("OK") : TEXT("FAIL"), *Id, Files.Num()));

		sol::table R = LuaView.create_table();
		R["success"] = bSuccess;
		return R;
	});

	// ────────────────────────────────────────────
	// 28. sc_unshelve
	// ────────────────────────────────────────────
	Lua.set_function("sc_unshelve", [&Session](const std::string& ChangelistId, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		ISourceControlProvider& Provider = GetSCProvider();
		FString Id = UTF8_TO_TCHAR(ChangelistId.c_str());

		TSharedRef<FUnshelve, ESPMode::ThreadSafe> UnshelveOp = ISourceControlOperation::Create<FUnshelve>();
		if (!Provider.CanExecuteOperation(UnshelveOp))
		{
			Session.Log(TEXT("[FAIL] sc_unshelve: operation not supported by current provider"));
			sol::table R = LuaView.create_table();
			R["success"] = false;
			return R;
		}

		FSourceControlChangelistPtr CL = FindChangelistById(Id);
		if (!CL.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] sc_unshelve: changelist '%s' not found"), *Id));
			sol::table R = LuaView.create_table();
			R["success"] = false;
			return R;
		}

		ECommandResult::Type Result = Provider.Execute(UnshelveOp, CL, EConcurrency::Synchronous);
		bool bSuccess = (Result == ECommandResult::Succeeded);
		Session.Log(FString::Printf(TEXT("[%s] sc_unshelve: CL %s"), bSuccess ? TEXT("OK") : TEXT("FAIL"), *Id));

		sol::table R = LuaView.create_table();
		R["success"] = bSuccess;
		return R;
	});

	// ────────────────────────────────────────────
	// 29. sc_delete_shelved
	// ────────────────────────────────────────────
	Lua.set_function("sc_delete_shelved", [&Session](const std::string& ChangelistId, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		ISourceControlProvider& Provider = GetSCProvider();
		FString Id = UTF8_TO_TCHAR(ChangelistId.c_str());

		TSharedRef<FDeleteShelved, ESPMode::ThreadSafe> DeleteShelvedOp = ISourceControlOperation::Create<FDeleteShelved>();
		if (!Provider.CanExecuteOperation(DeleteShelvedOp))
		{
			Session.Log(TEXT("[FAIL] sc_delete_shelved: operation not supported by current provider"));
			sol::table R = LuaView.create_table();
			R["success"] = false;
			return R;
		}

		FSourceControlChangelistPtr CL = FindChangelistById(Id);
		if (!CL.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] sc_delete_shelved: changelist '%s' not found"), *Id));
			sol::table R = LuaView.create_table();
			R["success"] = false;
			return R;
		}

		ECommandResult::Type Result = Provider.Execute(DeleteShelvedOp, CL, EConcurrency::Synchronous);
		bool bSuccess = (Result == ECommandResult::Succeeded);
		Session.Log(FString::Printf(TEXT("[%s] sc_delete_shelved: CL %s"), bSuccess ? TEXT("OK") : TEXT("FAIL"), *Id));

		sol::table R = LuaView.create_table();
		R["success"] = bSuccess;
		return R;
	});

	// ────────────────────────────────────────────
	// 30. sc_sync_revision
	// ────────────────────────────────────────────
	Lua.set_function("sc_sync_revision", [&Session](const sol::object& FilesArg, sol::optional<std::string> RevisionOpt, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		ISourceControlProvider& Provider = GetSCProvider();
		TArray<FString> Files = ToFileArray(FilesArg);

		if (Files.IsEmpty())
		{
			Session.Log(TEXT("[FAIL] sc_sync_revision: no files specified"));
			sol::table R = LuaView.create_table();
			R["success"] = false;
			return R;
		}

		TSharedRef<FSync, ESPMode::ThreadSafe> SyncOp = ISourceControlOperation::Create<FSync>();

		if (RevisionOpt.has_value())
		{
			FString Revision = UTF8_TO_TCHAR(RevisionOpt.value().c_str());
			SyncOp->SetRevision(Revision);
		}
		else
		{
			SyncOp->SetHeadRevisionFlag(true);
		}

		ECommandResult::Type Result = Provider.Execute(SyncOp, Files, EConcurrency::Synchronous);
		bool bSuccess = (Result == ECommandResult::Succeeded);

		FString RevStr = RevisionOpt.has_value() ? UTF8_TO_TCHAR(RevisionOpt.value().c_str()) : TEXT("head");
		Session.Log(FString::Printf(TEXT("[%s] sc_sync_revision: %d file(s) → revision %s"), bSuccess ? TEXT("OK") : TEXT("FAIL"), Files.Num(), *RevStr));

		sol::table R = LuaView.create_table();
		R["success"] = bSuccess;
		return R;
	});

	// ────────────────────────────────────────────
	// 31. sc_resolve
	// ────────────────────────────────────────────
	Lua.set_function("sc_resolve", [&Session](const sol::object& FilesArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		ISourceControlProvider& Provider = GetSCProvider();
		TArray<FString> Files = ToFileArray(FilesArg);

		if (Files.IsEmpty())
		{
			Session.Log(TEXT("[FAIL] sc_resolve: no files specified"));
			sol::table R = LuaView.create_table();
			R["success"] = false;
			return R;
		}

		TSharedRef<FResolve, ESPMode::ThreadSafe> ResolveOp = ISourceControlOperation::Create<FResolve>();
		if (!Provider.CanExecuteOperation(ResolveOp))
		{
			Session.Log(TEXT("[FAIL] sc_resolve: operation not supported by current provider"));
			sol::table R = LuaView.create_table();
			R["success"] = false;
			return R;
		}

		ECommandResult::Type Result = Provider.Execute(ResolveOp, Files, EConcurrency::Synchronous);
		bool bSuccess = (Result == ECommandResult::Succeeded);
		Session.Log(FString::Printf(TEXT("[%s] sc_resolve: %d file(s)"), bSuccess ? TEXT("OK") : TEXT("FAIL"), Files.Num()));

		sol::table R = LuaView.create_table();
		R["success"] = bSuccess;
		return R;
	});

	// ────────────────────────────────────────────
	// 32. sc_get_history
	// ────────────────────────────────────────────
	Lua.set_function("sc_get_history", [&Session](const std::string& File, sol::optional<sol::table> Opts, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		ISourceControlProvider& Provider = GetSCProvider();
		FString FilePath = UTF8_TO_TCHAR(File.c_str());

		int32 MaxCount = 32;
		if (Opts.has_value())
		{
			sol::table O = Opts.value();
			MaxCount = O.get_or("max_count", 32);
		}

		// Execute FUpdateStatus with history enabled
		TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> UpdateOp = ISourceControlOperation::Create<FUpdateStatus>();
		UpdateOp->SetUpdateHistory(true);

		TArray<FString> FileArray;
		FileArray.Add(FilePath);

		ECommandResult::Type Result = Provider.Execute(UpdateOp, FileArray, EConcurrency::Synchronous);
		if (Result != ECommandResult::Succeeded)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] sc_get_history: update status failed for '%s'"), *FilePath));
			return sol::lua_nil;
		}

		// Get the state with history
		FSourceControlStatePtr FileState = Provider.GetState(FilePath, EStateCacheUsage::Use);
		if (!FileState.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] sc_get_history: no state for '%s'"), *FilePath));
			return sol::lua_nil;
		}

		int32 HistorySize = FileState->GetHistorySize();
		int32 Count = FMath::Min(HistorySize, MaxCount);

		sol::table HistoryTable = LuaView.create_table();
		for (int32 i = 0; i < Count; i++)
		{
			TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> Rev = FileState->GetHistoryItem(i);
			if (!Rev.IsValid())
				continue;

			HistoryTable[i + 1] = RevisionToTable(LuaView, Rev);
		}

		Session.Log(FString::Printf(TEXT("[OK] sc_get_history: '%s' → %d revision(s)"), *FilePath, Count));
		return HistoryTable;
	});

	// ────────────────────────────────────────────
	// 33. sc_current_revision
	// ────────────────────────────────────────────
	Lua.set_function("sc_current_revision", [&Session](const std::string& File, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		ISourceControlProvider& Provider = GetSCProvider();
		FString FilePath = UTF8_TO_TCHAR(File.c_str());

		// Ensure state is up to date
		TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> UpdateOp = ISourceControlOperation::Create<FUpdateStatus>();
		UpdateOp->SetUpdateHistory(true);

		TArray<FString> FileArray;
		FileArray.Add(FilePath);
		Provider.Execute(UpdateOp, FileArray, EConcurrency::Synchronous);

		FSourceControlStatePtr FileState = Provider.GetState(FilePath, EStateCacheUsage::Use);
		if (!FileState.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] sc_current_revision: no state for '%s'"), *FilePath));
			return sol::lua_nil;
		}

		TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> Rev = FileState->GetCurrentRevision();
		if (!Rev.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] sc_current_revision: no current revision for '%s'"), *FilePath));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] sc_current_revision: '%s' → r%s"), *FilePath, *Rev->GetRevision()));
		return RevisionToTable(LuaView, Rev);
	});

	// ────────────────────────────────────────────
	// 34. sc_find_revision
	// ────────────────────────────────────────────
	Lua.set_function("sc_find_revision", [&Session](const std::string& File, const sol::object& RevisionArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		ISourceControlProvider& Provider = GetSCProvider();
		FString FilePath = UTF8_TO_TCHAR(File.c_str());

		// Ensure history is fetched
		TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> UpdateOp = ISourceControlOperation::Create<FUpdateStatus>();
		UpdateOp->SetUpdateHistory(true);

		TArray<FString> FileArray;
		FileArray.Add(FilePath);
		Provider.Execute(UpdateOp, FileArray, EConcurrency::Synchronous);

		FSourceControlStatePtr FileState = Provider.GetState(FilePath, EStateCacheUsage::Use);
		if (!FileState.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] sc_find_revision: no state for '%s'"), *FilePath));
			return sol::lua_nil;
		}

		TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> Rev;
		if (RevisionArg.is<int>())
		{
			Rev = FileState->FindHistoryRevision(RevisionArg.as<int>());
		}
		else if (RevisionArg.is<std::string>())
		{
			FString RevStr = UTF8_TO_TCHAR(RevisionArg.as<std::string>().c_str());
			Rev = FileState->FindHistoryRevision(RevStr);
		}
		else
		{
			Session.Log(TEXT("[FAIL] sc_find_revision: revision must be a number or string"));
			return sol::lua_nil;
		}

		if (!Rev.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] sc_find_revision: revision not found for '%s'"), *FilePath));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] sc_find_revision: '%s' → r%s"), *FilePath, *Rev->GetRevision()));
		return RevisionToTable(LuaView, Rev);
	});

	// ────────────────────────────────────────────
	// 35. sc_resolve_info
	// ────────────────────────────────────────────
	Lua.set_function("sc_resolve_info", [&Session](const std::string& File, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		ISourceControlProvider& Provider = GetSCProvider();
		FString FilePath = UTF8_TO_TCHAR(File.c_str());

		FSourceControlStatePtr FileState = Provider.GetState(FilePath, EStateCacheUsage::ForceUpdate);
		if (!FileState.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] sc_resolve_info: no state for '%s'"), *FilePath));
			return sol::lua_nil;
		}

		ISourceControlState::FResolveInfo Info = FileState->GetResolveInfo();
		if (!Info.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[OK] sc_resolve_info: '%s' — not in a resolve state"), *FilePath));
			return sol::lua_nil;
		}

		sol::table Result = LuaView.create_table();
		Result["remote_file"]     = TCHAR_TO_UTF8(*Info.RemoteFile);
		Result["base_file"]       = TCHAR_TO_UTF8(*Info.BaseFile);
		Result["remote_revision"] = TCHAR_TO_UTF8(*Info.RemoteRevision);
		Result["base_revision"]   = TCHAR_TO_UTF8(*Info.BaseRevision);

		Session.Log(FString::Printf(TEXT("[OK] sc_resolve_info: '%s' — remote=%s, base=%s"), *FilePath, *Info.RemoteRevision, *Info.BaseRevision));
		return Result;
	});

	// ────────────────────────────────────────────
	// 36. sc_is_at_latest
	// ────────────────────────────────────────────
	Lua.set_function("sc_is_at_latest", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		TOptional<bool> Result = GetSCProvider().IsAtLatestRevision();
		if (!Result.IsSet())
		{
			Session.Log(TEXT("[OK] sc_is_at_latest: not supported by current provider"));
			return sol::lua_nil;
		}

		bool bAtLatest = Result.GetValue();
		Session.Log(FString::Printf(TEXT("[OK] sc_is_at_latest → %s"), bAtLatest ? TEXT("true") : TEXT("false")));
		return sol::make_object(LuaView, bAtLatest);
	});

	// ────────────────────────────────────────────
	// 37. sc_local_changes
	// ────────────────────────────────────────────
	Lua.set_function("sc_local_changes", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		TOptional<int> Result = GetSCProvider().GetNumLocalChanges();
		if (!Result.IsSet())
		{
			Session.Log(TEXT("[OK] sc_local_changes: not supported by current provider"));
			return sol::lua_nil;
		}

		int Count = Result.GetValue();
		Session.Log(FString::Printf(TEXT("[OK] sc_local_changes → %d"), Count));
		return sol::make_object(LuaView, Count);
	});

	// ────────────────────────────────────────────
	// 38. sc_shelved_files
	// ────────────────────────────────────────────
	Lua.set_function("sc_shelved_files", [&Session](const std::string& IdStr, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		ISourceControlProvider& Provider = GetSCProvider();
		FString Id = UTF8_TO_TCHAR(IdStr.c_str());

		FSourceControlChangelistPtr CL = FindChangelistById(Id);
		if (!CL.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] sc_shelved_files: changelist '%s' not found"), *Id));
			return sol::lua_nil;
		}

		// Update shelved file states
		TSharedRef<FUpdatePendingChangelistsStatus, ESPMode::ThreadSafe> UpdateOp = ISourceControlOperation::Create<FUpdatePendingChangelistsStatus>();
		UpdateOp->SetUpdateShelvedFilesStates(true);
		TArray<FSourceControlChangelistRef> CLArray;
		CLArray.Add(CL.ToSharedRef());
		UpdateOp->SetChangelistsToUpdate(CLArray);
		Provider.Execute(UpdateOp, EConcurrency::Synchronous);

		FSourceControlChangelistStatePtr CLState = Provider.GetState(CL.ToSharedRef(), EStateCacheUsage::Use);
		if (!CLState.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] sc_shelved_files: could not get state for CL '%s'"), *Id));
			return sol::lua_nil;
		}

		const TArray<FSourceControlStateRef> ShelvedStates = CLState->GetShelvedFilesStates();

		sol::table ResultTable = LuaView.create_table();
		int32 Index = 1;
		for (const FSourceControlStateRef& FileState : ShelvedStates)
		{
			sol::table Entry = LuaView.create_table();
			Entry["filename"]       = TCHAR_TO_UTF8(*FileState->GetFilename());
			Entry["is_checked_out"] = FileState->IsCheckedOut();
			Entry["is_added"]       = FileState->IsAdded();
			Entry["is_deleted"]     = FileState->IsDeleted();
			Entry["is_modified"]    = FileState->IsModified();
			Entry["display_name"]   = TCHAR_TO_UTF8(*FileState->GetDisplayName().ToString());
			ResultTable[Index++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] sc_shelved_files: CL %s → %d shelved file(s)"), *Id, ShelvedStates.Num()));
		return ResultTable;
	});

	// ────────────────────────────────────────────
	// 39. sc_annotate
	// ────────────────────────────────────────────
	Lua.set_function("sc_annotate", [&Session](const std::string& File, const sol::object& LabelOrCL, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		ISourceControlProvider& Provider = GetSCProvider();
		FString FilePath = UTF8_TO_TCHAR(File.c_str());

		TArray<FAnnotationLine> Lines;
		bool bSuccess = false;

		if (LabelOrCL.is<int>())
		{
			bSuccess = USourceControlHelpers::AnnotateFile(Provider, LabelOrCL.as<int>(), FilePath, Lines);
		}
		else if (LabelOrCL.is<std::string>())
		{
			FString Label = UTF8_TO_TCHAR(LabelOrCL.as<std::string>().c_str());
			bSuccess = USourceControlHelpers::AnnotateFile(Provider, Label, FilePath, Lines);
		}
		else
		{
			Session.Log(TEXT("[FAIL] sc_annotate: second argument must be a label (string) or changelist number (int)"));
			return sol::lua_nil;
		}

		if (!bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] sc_annotate: annotation failed for '%s'"), *FilePath));
			return sol::lua_nil;
		}

		sol::table ResultTable = LuaView.create_table();
		for (int32 i = 0; i < Lines.Num(); i++)
		{
			sol::table Entry = LuaView.create_table();
			Entry["change_number"] = Lines[i].ChangeNumber;
			Entry["user"]          = TCHAR_TO_UTF8(*Lines[i].UserName);
			Entry["line"]          = TCHAR_TO_UTF8(*Lines[i].Line);
			ResultTable[i + 1] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] sc_annotate: '%s' → %d line(s)"), *FilePath, Lines.Num()));
		return ResultTable;
	});
}

REGISTER_LUA_BINDING(SourceControl, SourceControlDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindSourceControl(Lua, Session);
});
