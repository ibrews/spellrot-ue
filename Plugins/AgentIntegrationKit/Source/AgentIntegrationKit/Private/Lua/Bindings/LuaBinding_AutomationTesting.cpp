// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Misc/AutomationTest.h"
#include "IAutomationControllerModule.h"
#include "IAutomationControllerManager.h"
#include "IAutomationReport.h"
#include "AutomationControllerSettings.h"
#include "AutomationTestExcludelist.h"
#include "Framework/Application/SlateApplication.h"
#include "RenderingThread.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ─── Documentation ───

static TArray<FLuaFunctionDoc> AutomationTestingDocs = {
	{ TEXT("test_list(filter?, tags?, flags?)"), TEXT("List registered automation tests with optional prefix/tag/flag filters"), TEXT("table[]") },
	{ TEXT("test_find(pattern)"), TEXT("Search tests by name pattern (substring, case-insensitive)"), TEXT("table[]") },
	{ TEXT("test_run(test_names_or_filter, opts?)"), TEXT("Run automation tests — opts: passes, wait, flags, timeout"), TEXT("{started, test_count}") },
	{ TEXT("test_stop()"), TEXT("Stop all currently running automation tests"), TEXT("bool") },
	{ TEXT("test_is_running()"), TEXT("Check if automation tests are currently running"), TEXT("bool") },
	{ TEXT("test_get_results(filter_state?)"), TEXT("Get results from last run — filter: All, Pass, Fail, InProcess, NotRun, Skipped"), TEXT("table[]") },
	{ TEXT("test_get_summary()"), TEXT("Get summary of the last test run (total, passed, failed, skipped, duration, has_errors, has_warnings)"), TEXT("table") },
	{ TEXT("test_export_report()"), TEXT("Export the automation report to the default log directory"), TEXT("bool") },
	{ TEXT("test_exclude(test_name, opts?)"), TEXT("Add a test to the exclude list — opts: reason, rhis, warn"), TEXT("bool") },
	{ TEXT("test_unexclude(test_name)"), TEXT("Remove a test from the exclude list"), TEXT("bool") },
	{ TEXT("test_is_excluded(test_name)"), TEXT("Check if a test is in the exclude list"), TEXT("{excluded, reason, warn}") },
	{ TEXT("test_clear_reports()"), TEXT("Clear all accumulated automation test reports"), TEXT("bool") },
	{ TEXT("test_list_excludes()"), TEXT("List all entries in the automation test exclude list"), TEXT("table[]") },
	{ TEXT("test_get_settings()"), TEXT("Read automation controller settings"), TEXT("table") },
	{ TEXT("test_set_settings(params)"), TEXT("Update automation controller settings"), TEXT("bool") },
	{ TEXT("test_get_enabled()"), TEXT("Get list of currently enabled test names and count"), TEXT("{names, count}") },
	{ TEXT("test_set_flags(flags)"), TEXT("Set which test flags to request from workers"), TEXT("bool") },
	{ TEXT("test_set_dev_dir(include)"), TEXT("Include or exclude developer content directories from tests"), TEXT("bool") },
	{ TEXT("test_get_report_path()"), TEXT("Get the automation report output directory path"), TEXT("string") },
};

// ─── Helpers ───

static IAutomationControllerManagerPtr GetAutomationController()
{
	IAutomationControllerModule* Module = FModuleManager::LoadModulePtr<IAutomationControllerModule>("AutomationController");
	if (!Module)
	{
		return nullptr;
	}
	return Module->GetAutomationController();
}

static FString AutomationStateToStr(EAutomationState State)
{
	switch (State)
	{
	case EAutomationState::NotRun:    return TEXT("NotRun");
	case EAutomationState::InProcess: return TEXT("InProcess");
	case EAutomationState::Fail:      return TEXT("Fail");
	case EAutomationState::Success:   return TEXT("Success");
	case EAutomationState::Skipped:   return TEXT("Skipped");
	default:                          return TEXT("Unknown");
	}
}

static FString ControllerStateToStr(EAutomationControllerModuleState::Type State)
{
	switch (State)
	{
	case EAutomationControllerModuleState::Ready:    return TEXT("Ready");
	case EAutomationControllerModuleState::Running:  return TEXT("Running");
	case EAutomationControllerModuleState::Disabled: return TEXT("Disabled");
	default:                                         return TEXT("Unknown");
	}
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
static EAutomationTestFlags ParseFlagString(const FString& FlagStr)
{
	static const TMap<FString, EAutomationTestFlags> ShortNames = {
		// Filter flags
		{ TEXT("smoke"),        EAutomationTestFlags::SmokeFilter },
		{ TEXT("engine"),       EAutomationTestFlags::EngineFilter },
		{ TEXT("product"),      EAutomationTestFlags::ProductFilter },
		{ TEXT("perf"),         EAutomationTestFlags::PerfFilter },
		{ TEXT("stress"),       EAutomationTestFlags::StressFilter },
		{ TEXT("negative"),     EAutomationTestFlags::NegativeFilter },
		// Context flags
		{ TEXT("editor"),       EAutomationTestFlags::EditorContext },
		{ TEXT("client"),       EAutomationTestFlags::ClientContext },
		{ TEXT("server"),       EAutomationTestFlags::ServerContext },
		{ TEXT("commandlet"),   EAutomationTestFlags::CommandletContext },
		{ TEXT("program"),      EAutomationTestFlags::ProgramContext },
		// Priority flags
		{ TEXT("critical"),     EAutomationTestFlags::CriticalPriority },
		{ TEXT("high"),         EAutomationTestFlags::HighPriority },
		{ TEXT("medium"),       EAutomationTestFlags::MediumPriority },
		{ TEXT("low"),          EAutomationTestFlags::LowPriority },
		// Feature flags
		{ TEXT("nonnullrhi"),   EAutomationTestFlags::NonNullRHI },
		{ TEXT("requiresuser"), EAutomationTestFlags::RequiresUser },
		// One-off flags
		{ TEXT("disabled"),     EAutomationTestFlags::Disabled },
	};

	FString Lower = FlagStr.ToLower();
	if (const EAutomationTestFlags* Found = ShortNames.Find(Lower))
	{
		return *Found;
	}
	return EAutomationTestFlags::None;
}

static EAutomationTestFlags ParseFlagsTable(const sol::table& FlagsTable)
{
	EAutomationTestFlags Combined = EAutomationTestFlags::None;
	for (auto& Pair : FlagsTable)
	{
		if (Pair.second.is<std::string>())
		{
			Combined |= ParseFlagString(UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str()));
		}
	}
	return Combined;
}
#else
// UE 5.4: EAutomationTestFlags is an old-style enum (EAutomationTestFlags::Type), not an enum class
static uint32 ParseFlagString(const FString& FlagStr)
{
	static const TMap<FString, uint32> ShortNames = {
		// Filter flags
		{ TEXT("smoke"),        (uint32)EAutomationTestFlags::SmokeFilter },
		{ TEXT("engine"),       (uint32)EAutomationTestFlags::EngineFilter },
		{ TEXT("product"),      (uint32)EAutomationTestFlags::ProductFilter },
		{ TEXT("perf"),         (uint32)EAutomationTestFlags::PerfFilter },
		{ TEXT("stress"),       (uint32)EAutomationTestFlags::StressFilter },
		{ TEXT("negative"),     (uint32)EAutomationTestFlags::NegativeFilter },
		// Context flags
		{ TEXT("editor"),       (uint32)EAutomationTestFlags::EditorContext },
		{ TEXT("client"),       (uint32)EAutomationTestFlags::ClientContext },
		{ TEXT("server"),       (uint32)EAutomationTestFlags::ServerContext },
		{ TEXT("commandlet"),   (uint32)EAutomationTestFlags::CommandletContext },
		// Priority flags
		{ TEXT("critical"),     (uint32)EAutomationTestFlags::CriticalPriority },
		{ TEXT("high"),         (uint32)EAutomationTestFlags::HighPriority },
		{ TEXT("medium"),       (uint32)EAutomationTestFlags::MediumPriority },
		{ TEXT("low"),          (uint32)EAutomationTestFlags::LowPriority },
		// Feature flags
		{ TEXT("nonnullrhi"),   (uint32)EAutomationTestFlags::NonNullRHI },
		{ TEXT("requiresuser"), (uint32)EAutomationTestFlags::RequiresUser },
		// One-off flags
		{ TEXT("disabled"),     (uint32)EAutomationTestFlags::Disabled },
	};

	FString Lower = FlagStr.ToLower();
	if (const uint32* Found = ShortNames.Find(Lower))
	{
		return *Found;
	}
	return 0;
}

static uint32 ParseFlagsTable(const sol::table& FlagsTable)
{
	uint32 Combined = 0;
	for (auto& Pair : FlagsTable)
	{
		if (Pair.second.is<std::string>())
		{
			Combined |= ParseFlagString(UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str()));
		}
	}
	return Combined;
}
#endif

static void PumpGameThread()
{
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().Tick();
	}
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	FlushRenderingCommands();
}

// ─── Helpers for version-conditional code (cannot use #if inside macro arguments) ───

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
using FTestFlagsType = EAutomationTestFlags;
static const FTestFlagsType GTestFlagsNone = EAutomationTestFlags::None;
#else
using FTestFlagsType = uint32;
static const FTestFlagsType GTestFlagsNone = 0;
#endif

static FTestFlagsType AutomationTesting_ParseFlagsTableCompat(const sol::table& FlagsTable)
{
	return ParseFlagsTable(FlagsTable);
}

static bool AutomationTesting_HasAllFlags(const FAutomationTestInfo& Info, FTestFlagsType RequiredFlags)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	return EnumHasAllFlags(Info.GetTestFlags(), RequiredFlags);
#else
	return ((uint32)Info.GetTestFlags() & RequiredFlags) == RequiredFlags;
#endif
}

static bool AutomationTesting_HasAnyFlagSmoke(const FAutomationTestInfo& Info)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	return EnumHasAnyFlags(Info.GetTestFlags(), EAutomationTestFlags::SmokeFilter);
#else
	return ((uint32)Info.GetTestFlags() & (uint32)EAutomationTestFlags::SmokeFilter) != 0;
#endif
}

static bool AutomationTesting_PassesTagFilter(const FAutomationTestInfo& Info, const FString& TagsFilter)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	if (!TagsFilter.IsEmpty())
	{
		FString TestTags = Info.GetTestTags();
		if (!TestTags.Contains(TagsFilter, ESearchCase::IgnoreCase))
			return false;
	}
#endif
	return true;
}

static void AutomationTesting_PopulateTags(sol::table& Entry, const FAutomationTestInfo& Info)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	Entry["tags"] = std::string(TCHAR_TO_UTF8(*Info.GetTestTags()));
#endif
}

static void AutomationTesting_ReadSettings55(sol::table& Result, const UAutomationControllerSettings* Settings)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	Result["sort_by_failure"] = Settings->bSortTestsByFailure;
	Result["prune_logs_on_success"] = Settings->bPruneLogsOnSuccess;
#endif
}

static void AutomationTesting_WriteSettings55(UAutomationControllerSettings* Settings, const sol::table& Params)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	if (Params["sort_by_failure"].valid())
		Settings->bSortTestsByFailure = Params["sort_by_failure"].get<bool>();
	if (Params["prune_logs_on_success"].valid())
		Settings->bPruneLogsOnSuccess = Params["prune_logs_on_success"].get<bool>();
#endif
}

static void AutomationTesting_SetRequestedFlags(IAutomationControllerManagerPtr Controller, const sol::table& FlagsTable, FString& OutLogMsg)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	EAutomationTestFlags Flags = ParseFlagsTable(FlagsTable);
	Controller->SetRequestedTestFlags(Flags);
	OutLogMsg = FString::Printf(TEXT("[OK] test_set_flags -> set flags 0x%08X"), static_cast<uint32>(Flags));
#else
	uint32 Flags = ParseFlagsTable(FlagsTable);
	Controller->SetRequestedTestFlags(Flags);
	OutLogMsg = FString::Printf(TEXT("[OK] test_set_flags -> set flags 0x%08X"), Flags);
#endif
}

// ─── Binding ───

REGISTER_LUA_BINDING(AutomationTesting, AutomationTestingDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	// ──────────────────────────────────────────────
	// 1. test_list(filter?, tags?, flags?)
	// ──────────────────────────────────────────────
	Lua.set_function("test_list", [&Session](sol::optional<std::string> FilterOpt, sol::optional<std::string> TagsOpt, sol::optional<sol::table> FlagsOpt, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);

		TArray<FAutomationTestInfo> TestInfos;
		FAutomationTestFramework::Get().GetValidTestNames(TestInfos);

		FString Filter;
		if (FilterOpt.has_value())
			Filter = UTF8_TO_TCHAR(FilterOpt.value().c_str());

		FString TagsFilter;
		if (TagsOpt.has_value())
			TagsFilter = UTF8_TO_TCHAR(TagsOpt.value().c_str());

		FTestFlagsType RequiredFlags = GTestFlagsNone;
		if (FlagsOpt.has_value())
			RequiredFlags = AutomationTesting_ParseFlagsTableCompat(FlagsOpt.value());

		sol::table Result = Lua.create_table();
		int32 Idx = 1;

		for (const FAutomationTestInfo& Info : TestInfos)
		{
			const FString& FullPath = Info.GetFullTestPath();

			// Prefix filter
			if (!Filter.IsEmpty() && !FullPath.StartsWith(Filter, ESearchCase::IgnoreCase))
				continue;

			// Tag filter (substring match, 5.5+ only)
			if (!AutomationTesting_PassesTagFilter(Info, TagsFilter))
				continue;

			// Flag filter (test must have ALL required flags)
			if (RequiredFlags != GTestFlagsNone)
			{
				if (!AutomationTesting_HasAllFlags(Info, RequiredFlags))
					continue;
			}

			sol::table Entry = Lua.create_table();
			Entry["name"] = std::string(TCHAR_TO_UTF8(*Info.GetDisplayName()));
			Entry["full_path"] = std::string(TCHAR_TO_UTF8(*FullPath));
			Entry["flags"] = static_cast<int64>(static_cast<uint32>(Info.GetTestFlags()));
			Entry["is_smoke"] = AutomationTesting_HasAnyFlagSmoke(Info);
			Entry["source_file"] = std::string(TCHAR_TO_UTF8(*Info.GetSourceFile()));
			Entry["source_line"] = Info.GetSourceFileLine();
			AutomationTesting_PopulateTags(Entry, Info);

			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] test_list -> %d tests"), Idx - 1));
		return sol::make_object(Lua, Result);
	});

	// ──────────────────────────────────────────────
	// 2. test_find(pattern)
	// ──────────────────────────────────────────────
	Lua.set_function("test_find", [&Session](const std::string& Pattern, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FPattern = UTF8_TO_TCHAR(Pattern.c_str());

		TArray<FAutomationTestInfo> TestInfos;
		FAutomationTestFramework::Get().GetValidTestNames(TestInfos);

		sol::table Result = Lua.create_table();
		int32 Idx = 1;

		for (const FAutomationTestInfo& Info : TestInfos)
		{
			const FString& FullPath = Info.GetFullTestPath();
			const FString& DisplayName = Info.GetDisplayName();

			if (FullPath.Contains(FPattern, ESearchCase::IgnoreCase) ||
				DisplayName.Contains(FPattern, ESearchCase::IgnoreCase))
			{
				sol::table Entry = Lua.create_table();
				Entry["name"] = std::string(TCHAR_TO_UTF8(*DisplayName));
				Entry["full_path"] = std::string(TCHAR_TO_UTF8(*FullPath));
				AutomationTesting_PopulateTags(Entry, Info);
				Entry["flags"] = static_cast<int64>(static_cast<uint32>(Info.GetTestFlags()));
				Entry["source_file"] = std::string(TCHAR_TO_UTF8(*Info.GetSourceFile()));
				Entry["source_line"] = Info.GetSourceFileLine();
				Result[Idx++] = Entry;
			}
		}

		Session.Log(FString::Printf(TEXT("[OK] test_find(\"%s\") -> %d matches"), *FPattern, Idx - 1));
		return sol::make_object(Lua, Result);
	});

	// ──────────────────────────────────────────────
	// 3. test_run(test_names_or_filter, opts?)
	// ──────────────────────────────────────────────
	Lua.set_function("test_run", [&Session](sol::object TestNamesOrFilter, sol::optional<sol::table> OptsOpt, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		IAutomationControllerManagerPtr Controller = GetAutomationController();
		if (!Controller.IsValid())
		{
			Session.Log(TEXT("[FAIL] test_run -> AutomationController module not available"));
			sol::table ResultTbl = Lua.create_table();
			ResultTbl["started"] = false;
			ResultTbl["test_count"] = 0;
			return sol::make_object(Lua, ResultTbl);
		}

		int32 NumPasses = 1;
		bool bWait = false;
		double TimeoutSeconds = 600.0;
		FTestFlagsType RequiredFlags = GTestFlagsNone;
		if (OptsOpt.has_value())
		{
			sol::table Opts = OptsOpt.value();
			if (Opts["passes"].valid())
				NumPasses = Opts["passes"].get<int32>();
			if (Opts["wait"].valid())
				bWait = Opts["wait"].get<bool>();
			if (Opts["timeout"].valid())
				TimeoutSeconds = Opts["timeout"].get<double>();
			if (Opts["flags"].valid() && Opts["flags"].get_type() == sol::type::table)
				RequiredFlags = ParseFlagsTable(Opts["flags"].get<sol::table>());
		}

		Controller->SetNumPasses(NumPasses);

		// Request available workers and tests
		Controller->RequestAvailableWorkers(FGuid());
		Controller->RequestTests();

		// Determine which tests to enable
		TArray<FString> TestNames;

		if (TestNamesOrFilter.is<std::string>())
		{
			// String: use as prefix filter — find matching test paths
			FString Prefix = UTF8_TO_TCHAR(TestNamesOrFilter.as<std::string>().c_str());

			TArray<FAutomationTestInfo> AllTests;
			FAutomationTestFramework::Get().GetValidTestNames(AllTests);

			for (const FAutomationTestInfo& Info : AllTests)
			{
				if (Info.GetFullTestPath().StartsWith(Prefix, ESearchCase::IgnoreCase))
				{
					// Apply flag filter if specified
					if (RequiredFlags != GTestFlagsNone && !AutomationTesting_HasAllFlags(Info, RequiredFlags))
						continue;
					TestNames.Add(Info.GetFullTestPath());
				}
			}
		}
		else if (TestNamesOrFilter.is<sol::table>())
		{
			// Table of exact test paths — if flags specified, validate each
			sol::table Tbl = TestNamesOrFilter.as<sol::table>();

			TArray<FAutomationTestInfo> AllTests;
			if (RequiredFlags != GTestFlagsNone)
				FAutomationTestFramework::Get().GetValidTestNames(AllTests);

			for (auto& Pair : Tbl)
			{
				if (Pair.second.is<std::string>())
				{
					FString TestPath = UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str());

					if (RequiredFlags != GTestFlagsNone)
					{
						// Find matching test to check flags
						bool bPassesFilter = false;
						for (const FAutomationTestInfo& Info : AllTests)
						{
							if (Info.GetFullTestPath() == TestPath)
							{
								bPassesFilter = AutomationTesting_HasAllFlags(Info, RequiredFlags);
								break;
							}
						}
						if (!bPassesFilter)
							continue;
					}

					TestNames.Add(TestPath);
				}
			}
		}

		if (TestNames.Num() == 0)
		{
			Session.Log(TEXT("[FAIL] test_run -> no tests matched the filter"));
			sol::table ResultTbl = Lua.create_table();
			ResultTbl["started"] = false;
			ResultTbl["test_count"] = 0;
			return sol::make_object(Lua, ResultTbl);
		}

		Controller->SetEnabledTests(TestNames);
		Controller->RunTests(/*bIsLocalSession=*/true);

		Session.Log(FString::Printf(TEXT("[OK] test_run -> started %d tests (%d passes)"), TestNames.Num(), NumPasses));

		// Optionally wait for completion — pump game thread instead of blocking
		if (bWait)
		{
			const double StartTime = FPlatformTime::Seconds();
			while (Controller->GetTestState() == EAutomationControllerModuleState::Running)
			{
				Controller->Tick();
				PumpGameThread();

				if (FPlatformTime::Seconds() - StartTime > TimeoutSeconds)
				{
					Session.Log(FString::Printf(TEXT("[WARN] test_run -> timed out waiting for completion (%.0fs)"), TimeoutSeconds));
					break;
				}
			}
			Session.Log(TEXT("[OK] test_run -> tests completed"));
		}

		sol::table ResultTbl = Lua.create_table();
		ResultTbl["started"] = true;
		ResultTbl["test_count"] = TestNames.Num();
		return sol::make_object(Lua, ResultTbl);
	});

	// ──────────────────────────────────────────────
	// 4. test_stop()
	// ──────────────────────────────────────────────
	Lua.set_function("test_stop", [&Session]() -> bool
	{
		IAutomationControllerManagerPtr Controller = GetAutomationController();
		if (!Controller.IsValid())
		{
			Session.Log(TEXT("[FAIL] test_stop -> AutomationController module not available"));
			return false;
		}
		Controller->StopTests();
		Session.Log(TEXT("[OK] test_stop -> tests stopped"));
		return true;
	});

	// ──────────────────────────────────────────────
	// 5. test_is_running()
	// ──────────────────────────────────────────────
	Lua.set_function("test_is_running", [&Session]() -> bool
	{
		IAutomationControllerManagerPtr Controller = GetAutomationController();
		if (!Controller.IsValid())
		{
			Session.Log(TEXT("[FAIL] test_is_running -> AutomationController module not available"));
			return false;
		}
		bool bRunning = Controller->GetTestState() == EAutomationControllerModuleState::Running;
		Session.Log(FString::Printf(TEXT("[OK] test_is_running -> %s"), bRunning ? TEXT("true") : TEXT("false")));
		return bRunning;
	});

	// ──────────────────────────────────────────────
	// 6. test_get_results(filter_state?)
	// ──────────────────────────────────────────────
	Lua.set_function("test_get_results", [&Session](sol::optional<std::string> FilterStateOpt, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		IAutomationControllerManagerPtr Controller = GetAutomationController();
		if (!Controller.IsValid())
		{
			Session.Log(TEXT("[FAIL] test_get_results -> AutomationController module not available"));
			return sol::lua_nil;
		}

		FString FilterState = TEXT("All");
		if (FilterStateOpt.has_value())
			FilterState = UTF8_TO_TCHAR(FilterStateOpt.value().c_str());

		TArray<TSharedPtr<IAutomationReport>>& Reports = Controller->GetFilteredReports();

		sol::table Result = Lua.create_table();
		int32 Idx = 1;

		for (const TSharedPtr<IAutomationReport>& Report : Reports)
		{
			if (!Report.IsValid() || Report->IsParent())
				continue;

			EAutomationState State = Report->GetState(0, 0);

			// Apply state filter
			if (!FilterState.Equals(TEXT("All"), ESearchCase::IgnoreCase))
			{
				if (FilterState.Equals(TEXT("Pass"), ESearchCase::IgnoreCase) && State != EAutomationState::Success)
					continue;
				if (FilterState.Equals(TEXT("Fail"), ESearchCase::IgnoreCase) && State != EAutomationState::Fail)
					continue;
				if (FilterState.Equals(TEXT("InProcess"), ESearchCase::IgnoreCase) && State != EAutomationState::InProcess)
					continue;
				if (FilterState.Equals(TEXT("NotRun"), ESearchCase::IgnoreCase) && State != EAutomationState::NotRun)
					continue;
				if (FilterState.Equals(TEXT("Skipped"), ESearchCase::IgnoreCase) && State != EAutomationState::Skipped)
					continue;
			}

			const FAutomationTestResults& Results = Report->GetResults(0, 0);

			sol::table Entry = Lua.create_table();
			Entry["name"] = std::string(TCHAR_TO_UTF8(*Report->GetDisplayName()));
			Entry["full_path"] = std::string(TCHAR_TO_UTF8(*Report->GetFullTestPath()));
			Entry["state"] = std::string(TCHAR_TO_UTF8(*AutomationStateToStr(State)));
			Entry["duration"] = static_cast<double>(Results.Duration);
			Entry["error_count"] = Results.GetErrorTotal();
			Entry["warning_count"] = Results.GetWarningTotal();
			Entry["log_count"] = Results.GetLogTotal();

			// Collect errors and warnings from entries
			sol::table Errors = Lua.create_table();
			sol::table Warnings = Lua.create_table();
			sol::table Logs = Lua.create_table();
			int32 ErrIdx = 1;
			int32 WarnIdx = 1;
			int32 LogIdx = 1;

			for (const FAutomationExecutionEntry& ExecEntry : Results.GetEntries())
			{
				sol::table EventTbl = Lua.create_table();
				EventTbl["message"] = std::string(TCHAR_TO_UTF8(*ExecEntry.Event.Message));
				EventTbl["context"] = std::string(TCHAR_TO_UTF8(*ExecEntry.Event.Context));
				EventTbl["filename"] = std::string(TCHAR_TO_UTF8(*ExecEntry.Filename));
				EventTbl["line"] = ExecEntry.LineNumber;

				if (ExecEntry.Event.Type == EAutomationEventType::Error)
				{
					Errors[ErrIdx++] = EventTbl;
				}
				else if (ExecEntry.Event.Type == EAutomationEventType::Warning)
				{
					Warnings[WarnIdx++] = EventTbl;
				}
				else
				{
					Logs[LogIdx++] = EventTbl;
				}
			}

			Entry["errors"] = Errors;
			Entry["warnings"] = Warnings;
			Entry["logs"] = Logs;

			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] test_get_results(\"%s\") -> %d results"), *FilterState, Idx - 1));
		return sol::make_object(Lua, Result);
	});

	// ──────────────────────────────────────────────
	// 7. test_get_summary()
	// ──────────────────────────────────────────────
	Lua.set_function("test_get_summary", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		IAutomationControllerManagerPtr Controller = GetAutomationController();
		if (!Controller.IsValid())
		{
			Session.Log(TEXT("[FAIL] test_get_summary -> AutomationController module not available"));
			return sol::lua_nil;
		}

		TArray<TSharedPtr<IAutomationReport>>& Reports = Controller->GetFilteredReports();

		int32 Total = 0;
		int32 Passed = 0;
		int32 Failed = 0;
		int32 Skipped = 0;
		int32 InProcess = 0;
		int32 NotRun = 0;
		int32 WarningCount = 0;
		int32 ErrorCount = 0;
		double TotalDuration = 0.0;

		for (const TSharedPtr<IAutomationReport>& Report : Reports)
		{
			if (!Report.IsValid() || Report->IsParent())
				continue;

			Total++;
			EAutomationState State = Report->GetState(0, 0);
			const FAutomationTestResults& Results = Report->GetResults(0, 0);
			TotalDuration += static_cast<double>(Results.Duration);
			WarningCount += Results.GetWarningTotal();
			ErrorCount += Results.GetErrorTotal();

			switch (State)
			{
			case EAutomationState::Success:   Passed++;    break;
			case EAutomationState::Fail:      Failed++;    break;
			case EAutomationState::Skipped:   Skipped++;   break;
			case EAutomationState::InProcess: InProcess++;  break;
			case EAutomationState::NotRun:    NotRun++;     break;
			default: break;
			}
		}

		EAutomationControllerModuleState::Type ControllerState = Controller->GetTestState();

		sol::table Summary = Lua.create_table();
		Summary["total"] = Total;
		Summary["passed"] = Passed;
		Summary["failed"] = Failed;
		Summary["skipped"] = Skipped;
		Summary["in_process"] = InProcess;
		Summary["not_run"] = NotRun;
		Summary["warnings"] = WarningCount;
		Summary["errors"] = ErrorCount;
		Summary["duration"] = TotalDuration;
		Summary["state"] = std::string(TCHAR_TO_UTF8(*ControllerStateToStr(ControllerState)));
		Summary["results_available"] = Controller->CheckTestResultsAvailable();
		Summary["has_errors"] = Controller->ReportsHaveErrors();
		Summary["has_warnings"] = Controller->ReportsHaveWarnings();
		Summary["has_logs"] = Controller->ReportsHaveLogs();
		Summary["enabled_count"] = Controller->GetEnabledTestsNum();

		Session.Log(FString::Printf(TEXT("[OK] test_get_summary -> total=%d passed=%d failed=%d skipped=%d"), Total, Passed, Failed, Skipped));
		return sol::make_object(Lua, Summary);
	});

	// ──────────────────────────────────────────────
	// 8. test_export_report()
	// ──────────────────────────────────────────────
	Lua.set_function("test_export_report", [&Session]() -> bool
	{
		IAutomationControllerManagerPtr Controller = GetAutomationController();
		if (!Controller.IsValid())
		{
			Session.Log(TEXT("[FAIL] test_export_report -> AutomationController module not available"));
			return false;
		}

		uint32 ExportMask = EFileExportType::FET_All;
		bool bSuccess = Controller->ExportReport(ExportMask);

		if (bSuccess)
		{
			FString ReportPath = Controller->GetReportOutputPath();
			if (ReportPath.IsEmpty())
			{
				ReportPath = FPaths::ConvertRelativePathToFull(FPaths::AutomationDir());
			}
			Session.Log(FString::Printf(TEXT("[OK] test_export_report -> exported to %s"), *ReportPath));
		}
		else
		{
			Session.Log(TEXT("[FAIL] test_export_report -> export failed"));
		}
		return bSuccess;
	});

	// ──────────────────────────────────────────────
	// 9. test_exclude(test_name, opts?)
	// ──────────────────────────────────────────────
	Lua.set_function("test_exclude", [&Session](const std::string& TestName, sol::optional<sol::table> OptsOpt) -> bool
	{
		FString FTestName = UTF8_TO_TCHAR(TestName.c_str());

		UAutomationTestExcludelist* Excludelist = UAutomationTestExcludelist::Get();
		if (!Excludelist)
		{
			Session.Log(TEXT("[FAIL] test_exclude -> could not access exclude list"));
			return false;
		}

		FAutomationTestExcludelistEntry Entry;
		Entry.Test = FName(*FTestName);
		Entry.FullTestName = FTestName.ToLower();

		if (OptsOpt.has_value())
		{
			sol::table Opts = OptsOpt.value();

			if (Opts["reason"].valid() && Opts["reason"].get_type() == sol::type::string)
			{
				Entry.Reason = FName(UTF8_TO_TCHAR(Opts["reason"].get<std::string>().c_str()));
			}

			if (Opts["warn"].valid())
			{
				Entry.Warn = Opts["warn"].get<bool>();
			}

			// RHIs persist via UPROPERTY(Config) — use this for platform-level filtering
			if (Opts["rhis"].valid() && Opts["rhis"].get_type() == sol::type::table)
			{
				sol::table RhisTable = Opts["rhis"].get<sol::table>();
				for (auto& Pair : RhisTable)
				{
					if (Pair.second.is<std::string>())
					{
						Entry.RHIs.Add(FName(UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str())));
					}
				}
			}
		}

		Excludelist->AddToExcludeTest(FTestName, Entry);
		Excludelist->SaveToConfigs();

		Session.Log(FString::Printf(TEXT("[OK] test_exclude(\"%s\") -> added to exclude list"), *FTestName));
		return true;
	});

	// ──────────────────────────────────────────────
	// 10. test_unexclude(test_name)
	// ──────────────────────────────────────────────
	Lua.set_function("test_unexclude", [&Session](const std::string& TestName) -> bool
	{
		FString FTestName = UTF8_TO_TCHAR(TestName.c_str());

		UAutomationTestExcludelist* Excludelist = UAutomationTestExcludelist::Get();
		if (!Excludelist)
		{
			Session.Log(TEXT("[FAIL] test_unexclude -> could not access exclude list"));
			return false;
		}

		Excludelist->RemoveFromExcludeTest(FTestName);
		Excludelist->SaveToConfigs();

		Session.Log(FString::Printf(TEXT("[OK] test_unexclude(\"%s\") -> removed from exclude list"), *FTestName));
		return true;
	});

	// ──────────────────────────────────────────────
	// 11. test_is_excluded(test_name)
	// ──────────────────────────────────────────────
	Lua.set_function("test_is_excluded", [&Session](const std::string& TestName, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FTestName = UTF8_TO_TCHAR(TestName.c_str());

		UAutomationTestExcludelist* Excludelist = UAutomationTestExcludelist::Get();
		if (!Excludelist)
		{
			Session.Log(TEXT("[FAIL] test_is_excluded -> could not access exclude list"));
			return sol::lua_nil;
		}

		FName OutReason;
		bool OutWarn = false;
		TSet<FName> EmptyRHI;
		bool bExcluded = Excludelist->IsTestExcluded(FTestName, EmptyRHI, &OutReason, &OutWarn);

		sol::table Result = Lua.create_table();
		Result["excluded"] = bExcluded;
		if (bExcluded)
		{
			Result["reason"] = std::string(TCHAR_TO_UTF8(*OutReason.ToString()));
			Result["warn"] = OutWarn;

			const FAutomationTestExcludelistEntry* EntryPtr = Excludelist->GetExcludeTestEntry(FTestName);
			if (EntryPtr)
			{
				sol::table RhisTbl = Lua.create_table();
				int32 RhiIdx = 1;
				for (const FName& Rhi : EntryPtr->RHIs)
				{
					RhisTbl[RhiIdx++] = std::string(TCHAR_TO_UTF8(*Rhi.ToString()));
				}
				Result["rhis"] = RhisTbl;
			}
		}

		Session.Log(FString::Printf(TEXT("[OK] test_is_excluded(\"%s\") -> %s"), *FTestName, bExcluded ? TEXT("true") : TEXT("false")));
		return sol::make_object(Lua, Result);
	});

	// ──────────────────────────────────────────────
	// 12. test_clear_reports()
	// ──────────────────────────────────────────────
	Lua.set_function("test_clear_reports", [&Session]() -> bool
	{
		IAutomationControllerManagerPtr Controller = GetAutomationController();
		if (!Controller.IsValid())
		{
			Session.Log(TEXT("[FAIL] test_clear_reports -> AutomationController module not available"));
			return false;
		}
		Controller->ClearAutomationReports();
		Session.Log(TEXT("[OK] test_clear_reports -> reports cleared"));
		return true;
	});

	// ──────────────────────────────────────────────
	// 13. test_list_excludes()
	// ──────────────────────────────────────────────
	Lua.set_function("test_list_excludes", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);

		UAutomationTestExcludelistConfig* Config = GetMutableDefault<UAutomationTestExcludelistConfig>();
		if (!Config)
		{
			Session.Log(TEXT("[FAIL] test_list_excludes -> could not access exclude list config"));
			return sol::lua_nil;
		}

		// GetEntries() is not exported (MinimalAPI class) — access the UPROPERTY via reflection
		const FProperty* EntriesProp = UAutomationTestExcludelistConfig::StaticClass()->FindPropertyByName(TEXT("ExcludeTest"));
		if (!EntriesProp)
		{
			Session.Log(TEXT("[FAIL] test_list_excludes -> could not find ExcludeTest property via reflection"));
			return sol::lua_nil;
		}
		const TArray<FAutomationTestExcludelistEntry>& Entries = *EntriesProp->ContainerPtrToValuePtr<TArray<FAutomationTestExcludelistEntry>>(Config);

		sol::table Result = Lua.create_table();
		int32 Idx = 1;

		for (const FAutomationTestExcludelistEntry& Entry : Entries)
		{
			sol::table EntryTbl = Lua.create_table();
			EntryTbl["test"] = std::string(TCHAR_TO_UTF8(*Entry.Test.ToString()));
			EntryTbl["full_test_name"] = std::string(TCHAR_TO_UTF8(*Entry.FullTestName));
			EntryTbl["reason"] = std::string(TCHAR_TO_UTF8(*Entry.Reason.ToString()));
			EntryTbl["warn"] = Entry.Warn;

			// RHIs are UPROPERTY(Config) and persist
			sol::table RhisTbl = Lua.create_table();
			int32 RhiIdx = 1;
			for (const FName& Rhi : Entry.RHIs)
			{
				RhisTbl[RhiIdx++] = std::string(TCHAR_TO_UTF8(*Rhi.ToString()));
			}
			EntryTbl["rhis"] = RhisTbl;

			// Map field (for functional test exclusions)
			if (!Entry.Map.IsNone())
			{
				EntryTbl["map"] = std::string(TCHAR_TO_UTF8(*Entry.Map.ToString()));
			}

			Result[Idx++] = EntryTbl;
		}

		Session.Log(FString::Printf(TEXT("[OK] test_list_excludes -> %d entries"), Idx - 1));
		return sol::make_object(Lua, Result);
	});

	// ──────────────────────────────────────────────
	// 14. test_get_settings()
	// ──────────────────────────────────────────────
	Lua.set_function("test_get_settings", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);

		UAutomationControllerSettings* Settings = GetMutableDefault<UAutomationControllerSettings>();
		if (!Settings)
		{
			Session.Log(TEXT("[FAIL] test_get_settings -> could not access automation controller settings"));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();
		Result["suppress_log_errors"] = Settings->bSuppressLogErrors;
		Result["suppress_log_warnings"] = Settings->bSuppressLogWarnings;
		Result["elevate_warnings"] = Settings->bElevateLogWarningsToErrors;
		Result["keep_pie_open"] = Settings->bKeepPIEOpen;
		Result["telemetry_dir"] = std::string(TCHAR_TO_UTF8(*Settings->TelemetryDirectory));
		Result["auto_expand_single_items"] = Settings->bAutoExpandSingleItemSubgroups;
		AutomationTesting_ReadSettings55(Result, Settings);
		Result["check_interval_seconds"] = Settings->CheckTestIntervalSeconds;
		Result["lost_timer_seconds"] = Settings->GameInstanceLostTimerSeconds;

		// Suppressed log categories
		sol::table SuppressedCats = Lua.create_table();
		for (int32 i = 0; i < Settings->SuppressedLogCategories.Num(); ++i)
		{
			SuppressedCats[i + 1] = std::string(TCHAR_TO_UTF8(*Settings->SuppressedLogCategories[i]));
		}
		Result["suppressed_log_categories"] = SuppressedCats;

		// Groups
		sol::table Groups = Lua.create_table();
		for (int32 i = 0; i < Settings->Groups.Num(); ++i)
		{
			sol::table Group = Lua.create_table();
			Group["name"] = std::string(TCHAR_TO_UTF8(*Settings->Groups[i].Name));

			sol::table Filters = Lua.create_table();
			for (int32 j = 0; j < Settings->Groups[i].Filters.Num(); ++j)
			{
				sol::table FilterEntry = Lua.create_table();
				FilterEntry["contains"] = std::string(TCHAR_TO_UTF8(*Settings->Groups[i].Filters[j].Contains));
				FilterEntry["match_from_start"] = Settings->Groups[i].Filters[j].MatchFromStart;
				FilterEntry["match_from_end"] = Settings->Groups[i].Filters[j].MatchFromEnd;
				Filters[j + 1] = FilterEntry;
			}
			Group["filters"] = Filters;
			Groups[i + 1] = Group;
		}
		Result["groups"] = Groups;

		Session.Log(TEXT("[OK] test_get_settings -> retrieved"));
		return sol::make_object(Lua, Result);
	});

	// ──────────────────────────────────────────────
	// 15. test_set_settings(params)
	// ──────────────────────────────────────────────
	Lua.set_function("test_set_settings", [&Session](sol::table Params) -> bool
	{
		UAutomationControllerSettings* Settings = GetMutableDefault<UAutomationControllerSettings>();
		if (!Settings)
		{
			Session.Log(TEXT("[FAIL] test_set_settings -> could not access automation controller settings"));
			return false;
		}

		if (Params["suppress_log_errors"].valid())
			Settings->bSuppressLogErrors = Params["suppress_log_errors"].get<bool>();
		if (Params["suppress_log_warnings"].valid())
			Settings->bSuppressLogWarnings = Params["suppress_log_warnings"].get<bool>();
		if (Params["elevate_warnings"].valid())
			Settings->bElevateLogWarningsToErrors = Params["elevate_warnings"].get<bool>();
		if (Params["keep_pie_open"].valid())
			Settings->bKeepPIEOpen = Params["keep_pie_open"].get<bool>();
		if (Params["auto_expand_single_items"].valid())
			Settings->bAutoExpandSingleItemSubgroups = Params["auto_expand_single_items"].get<bool>();
		AutomationTesting_WriteSettings55(Settings, Params);
		if (Params["check_interval_seconds"].valid())
			Settings->CheckTestIntervalSeconds = Params["check_interval_seconds"].get<float>();
		if (Params["lost_timer_seconds"].valid())
			Settings->GameInstanceLostTimerSeconds = Params["lost_timer_seconds"].get<float>();
		if (Params["telemetry_dir"].valid() && Params["telemetry_dir"].get_type() == sol::type::string)
			Settings->TelemetryDirectory = UTF8_TO_TCHAR(Params["telemetry_dir"].get<std::string>().c_str());

		Settings->SaveConfig();

		Session.Log(TEXT("[OK] test_set_settings -> updated and saved"));
		return true;
	});

	// ──────────────────────────────────────────────
	// 16. test_get_enabled()
	// ──────────────────────────────────────────────
	Lua.set_function("test_get_enabled", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		IAutomationControllerManagerPtr Controller = GetAutomationController();
		if (!Controller.IsValid())
		{
			Session.Log(TEXT("[FAIL] test_get_enabled -> AutomationController module not available"));
			return sol::lua_nil;
		}

		TArray<FString> EnabledNames;
		Controller->GetEnabledTestNames(EnabledNames);

		sol::table Result = Lua.create_table();
		sol::table Names = Lua.create_table();
		for (int32 i = 0; i < EnabledNames.Num(); ++i)
		{
			Names[i + 1] = std::string(TCHAR_TO_UTF8(*EnabledNames[i]));
		}
		Result["names"] = Names;
		Result["count"] = Controller->GetEnabledTestsNum();

		Session.Log(FString::Printf(TEXT("[OK] test_get_enabled -> %d tests"), EnabledNames.Num()));
		return sol::make_object(Lua, Result);
	});

	// ──────────────────────────────────────────────
	// 17. test_set_flags(flags)
	// ──────────────────────────────────────────────
	Lua.set_function("test_set_flags", [&Session](sol::table FlagsTable) -> bool
	{
		IAutomationControllerManagerPtr Controller = GetAutomationController();
		if (!Controller.IsValid())
		{
			Session.Log(TEXT("[FAIL] test_set_flags -> AutomationController module not available"));
			return false;
		}

		FString LogMsg;
		AutomationTesting_SetRequestedFlags(Controller, FlagsTable, LogMsg);
		Session.Log(LogMsg);
		return true;
	});

	// ──────────────────────────────────────────────
	// 18. test_set_dev_dir(include)
	// ──────────────────────────────────────────────
	Lua.set_function("test_set_dev_dir", [&Session](bool bInclude) -> bool
	{
		IAutomationControllerManagerPtr Controller = GetAutomationController();
		if (!Controller.IsValid())
		{
			Session.Log(TEXT("[FAIL] test_set_dev_dir -> AutomationController module not available"));
			return false;
		}

		Controller->SetDeveloperDirectoryIncluded(bInclude);

		Session.Log(FString::Printf(TEXT("[OK] test_set_dev_dir -> developer directory %s"), bInclude ? TEXT("included") : TEXT("excluded")));
		return true;
	});

	// ──────────────────────────────────────────────
	// 19. test_get_report_path()
	// ──────────────────────────────────────────────
	Lua.set_function("test_get_report_path", [&Session]() -> std::string
	{
		IAutomationControllerManagerPtr Controller = GetAutomationController();
		if (!Controller.IsValid())
		{
			Session.Log(TEXT("[FAIL] test_get_report_path -> AutomationController module not available"));
			return std::string();
		}

		FString Path = Controller->GetReportOutputPath();
		if (Path.IsEmpty())
		{
			Path = FPaths::ConvertRelativePathToFull(FPaths::AutomationDir());
		}

		Session.Log(FString::Printf(TEXT("[OK] test_get_report_path -> %s"), *Path));
		return std::string(TCHAR_TO_UTF8(*Path));
	});
});
