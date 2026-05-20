// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/OutputDeviceRedirector.h"
#include "HAL/FileManager.h"
#include "Algo/Reverse.h"
#include "Logging/MessageLog.h"
#include "Logging/TokenizedMessage.h"
#include "MessageLogModule.h"
#include "IMessageLogListing.h"
#include "Modules/ModuleManager.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Materials/Material.h"
#include "MaterialEditingLibrary.h"
#if PLATFORM_WINDOWS
#include "ILiveCodingModule.h"
#endif

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ─── Documentation ───

static const TCHAR* ReadLog_HelpText = TEXT(
	"read_log(type, opts?) — Read editor logs\n\n"
	"Types:\n"
	"  output    — Main editor output log (Saved/Logs/)\n"
	"  message   — Structured message logs (BlueprintLog, MapCheck, PIE, etc.)\n"
	"  compile   — Compile a blueprint and return errors/warnings\n"
	"  livecoding — C++ Live Coding status and compile (Windows only)\n\n"
	"Examples:\n"
	"  read_log('output', {tail=50})\n"
	"  read_log('output', {search={'Error','Blueprint'}, severity='error'})\n"
	"  read_log('output', {category='LogShaders'})\n"
	"  read_log('output', {list_categories=true})\n"
	"  read_log('output', {offset=100, limit=50})\n"
	"  read_log('message', {list=true})\n"
	"  read_log('message', {log='BlueprintLog', severity='error', limit=20})\n"
	"  read_log('message', {log='BlueprintLog', clear=true})\n"
	"  read_log('message', {log='MapCheck', open=true})\n"
	"  read_log('compile', {asset='/Game/Blueprints/MyBP'})\n"
	"  read_log('compile', {asset='MyBP', path='/Game/Blueprints'})\n"
	"  read_log('livecoding')\n"
	"  read_log('livecoding', {compile=true})\n\n"
	"Use read_log(type, {help=true}) for type-specific help.\n"
);

static const TCHAR* OutputHelpText = TEXT(
	"read_log('output', opts?) — Read the main editor output log\n\n"
	"Options:\n"
	"  tail      (int)    Last N lines (max 5000). Overrides offset.\n"
	"  offset    (int)    Start line (0-based, default 0)\n"
	"  limit     (int)    Max lines (default 100, max 2000)\n"
	"  search    (table)  Array of terms — ALL must match (case-insensitive)\n"
	"  severity  (string) 'all'|'error'|'warning'|'display'|'log'\n"
	"  category  (string) UE log category, e.g. 'LogShaders', 'LogBlueprintUserMessages'\n"
	"  list_categories (bool) Scan log and return all unique categories found\n\n"
	"Returns: {lines, total_lines, matched_lines, start_line, end_line, path}\n"
);

static const TCHAR* MessageHelpText = TEXT(
	"read_log('message', opts?) — Read structured message logs\n\n"
	"Options:\n"
	"  list      (bool)   List all registered message logs with counts\n"
	"  log       (string) Log name to read (required unless list/clear/open)\n"
	"  severity  (string) 'all'|'error'|'warning'|'info'\n"
	"  limit     (int)    Max messages (default 100, max 500)\n"
	"  clear     (bool)   Clear all messages from the specified log\n"
	"  open      (bool)   Open the message log UI panel to the specified log\n\n"
	"Returns (list): array of {name, message_count, has_errors}\n"
	"Returns (read): {log_name, total_messages, messages} where each message is {severity, text}\n"
	"Returns (clear): {log_name, cleared}\n"
	"Returns (open): {log_name, opened}\n"
);

static const TCHAR* CompileHelpText = TEXT(
	"read_log('compile', opts?) — Compile a blueprint or material and return results\n\n"
	"Options:\n"
	"  asset     (string) Asset path, e.g. '/Game/Blueprints/MyBP' or '/Game/Materials/M_Test'\n"
	"  path      (string) Asset folder (default '/Game'), used when asset is just a name\n"
	"  force     (bool)   Force compile even with live instances (Windows Live Coding, BP only)\n\n"
	"Returns (Blueprint): {name, status_before, status_after, errors, warnings, messages}\n"
	"  messages is array of {severity, text}\n"
	"Returns (Material): {name, type, status}\n"
);

static const TCHAR* LiveCodingHelpText = TEXT(
	"read_log('livecoding', opts?) — C++ Live Coding status/compile (Windows only)\n\n"
	"Options:\n"
	"  compile   (bool) Trigger a Live Coding compile (default: just show status)\n\n"
	"Returns (status): {enabled_by_default, enabled_for_session, has_started, is_compiling, can_enable}\n"
	"Returns (compile): {result, compiler_output}\n"
);

static TArray<FLuaFunctionDoc> ReadLogDocs = {
	{ TEXT("read_log(type, opts?)"), TEXT("Read editor logs — types: output, message, compile, livecoding"), TEXT("table or string") },
};

// ─── Helpers ───

static FString NormalizeSeverity(const FString& SeverityStr)
{
	FString Lower = SeverityStr.ToLower();
	if (Lower == TEXT("err") || Lower == TEXT("errors"))
		return TEXT("error");
	if (Lower == TEXT("warn") || Lower == TEXT("warnings"))
		return TEXT("warning");
	return Lower;
}

static bool MatchesSearch(const FString& Line, const TArray<FString>& SearchTerms)
{
	if (SearchTerms.Num() == 0)
		return true;

	FString LowerLine = Line.ToLower();
	for (const FString& Term : SearchTerms)
	{
		if (!LowerLine.Contains(Term))
			return false;
	}
	return true;
}

static bool MatchesSeverity(const FString& Line, const FString& Severity)
{
	if (Severity.IsEmpty() || Severity == TEXT("all"))
		return true;

	FString Norm = NormalizeSeverity(Severity);

	if (Norm == TEXT("error"))
	{
		return Line.Contains(TEXT("Error:")) || Line.Contains(TEXT("Error]")) ||
			   Line.Contains(TEXT(": Error")) || Line.Contains(TEXT("LogError"));
	}
	else if (Norm == TEXT("warning"))
	{
		return Line.Contains(TEXT("Warning:")) || Line.Contains(TEXT("Warning]")) ||
			   Line.Contains(TEXT(": Warning")) || Line.Contains(TEXT("LogWarning"));
	}
	else if (Norm == TEXT("display"))
	{
		return Line.Contains(TEXT("Display:")) || Line.Contains(TEXT("Display]"));
	}
	else if (Norm == TEXT("log"))
	{
		return !Line.Contains(TEXT("Error:")) && !Line.Contains(TEXT("Warning:"));
	}

	return true;
}

// Extract the log category from a UE log line.
// Typical formats:
//   [2025.01.01-00.00.00:000][  0]LogShaders: Display: ...
//   LogShaders: Error: ...
//   LogShaders: ...
static FString ExtractCategory(const FString& Line)
{
	// Skip leading timestamp brackets: [2025.01.01-...][  0]
	int32 Start = 0;
	while (Start < Line.Len() && Line[Start] == TEXT('['))
	{
		int32 Close = Line.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
		if (Close == INDEX_NONE)
			break;
		Start = Close + 1;
	}

	// Find the first colon after Start — the category is the word before it
	int32 ColonIdx = Line.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
	if (ColonIdx == INDEX_NONE || ColonIdx <= Start)
		return FString();

	FString Candidate = Line.Mid(Start, ColonIdx - Start).TrimStartAndEnd();

	// Validate: UE log categories are single tokens like "LogShaders", "LogBlueprintUserMessages"
	// They should not contain spaces (that would indicate this isn't a category line)
	if (Candidate.IsEmpty() || Candidate.Contains(TEXT(" ")))
		return FString();

	return Candidate;
}

static bool MatchesCategory(const FString& Line, const FString& Category)
{
	if (Category.IsEmpty())
		return true;

	FString Cat = ExtractCategory(Line);
	return Cat.Equals(Category, ESearchCase::IgnoreCase);
}

static FString GetOutputLogPath()
{
	FString LogDir = FPaths::ProjectLogDir();
	TArray<FString> LogFiles;
	IFileManager::Get().FindFiles(LogFiles, *LogDir, TEXT("*.log"));

	if (LogFiles.Num() == 0)
		return FString();

	FString MostRecentLog;
	FDateTime MostRecentTime = FDateTime::MinValue();

	for (const FString& LogFile : LogFiles)
	{
		FString FullPath = LogDir / LogFile;
		FDateTime ModTime = IFileManager::Get().GetTimeStamp(*FullPath);
		if (ModTime > MostRecentTime)
		{
			MostRecentTime = ModTime;
			MostRecentLog = FullPath;
		}
	}

	return MostRecentLog;
}

static FString SeverityToString(EMessageSeverity::Type Severity)
{
	switch (Severity)
	{
	case EMessageSeverity::Error:              return TEXT("ERROR");
	case EMessageSeverity::PerformanceWarning: return TEXT("PERF_WARN");
	case EMessageSeverity::Warning:            return TEXT("WARNING");
	case EMessageSeverity::Info:
	default:                                   return TEXT("INFO");
	}
}

static FString BlueprintStatusToString(EBlueprintStatus Status)
{
	switch (Status)
	{
	case BS_Unknown:                return TEXT("Unknown");
	case BS_Dirty:                  return TEXT("Dirty");
	case BS_Error:                  return TEXT("Error");
	case BS_UpToDate:               return TEXT("UpToDate");
	case BS_BeingCreated:           return TEXT("BeingCreated");
	case BS_UpToDateWithWarnings:   return TEXT("UpToDateWithWarnings");
	default:                        return TEXT("Unknown");
	}
}

static FString BuildAssetPath(const FString& Asset, const FString& Path)
{
	FString FullAssetPath;
	if (Asset.StartsWith(TEXT("/Game/")) || Asset.StartsWith(TEXT("/Engine/")))
	{
		FullAssetPath = Asset;
		if (!FullAssetPath.Contains(TEXT(".")))
		{
			FString BaseName = FPaths::GetBaseFilename(FullAssetPath);
			FullAssetPath = FullAssetPath + TEXT(".") + BaseName;
		}
	}
	else
	{
		FString Folder = Path;
		if (Folder.IsEmpty())
			Folder = TEXT("/Game");
		if (!Folder.StartsWith(TEXT("/Game")) && !Folder.StartsWith(TEXT("/Engine")))
			Folder = FString::Printf(TEXT("/Game/%s"), *Folder);
		FullAssetPath = Folder / Asset + TEXT(".") + Asset;
	}
	return FullAssetPath;
}

static bool IsBlueprintCompileBlockedByLiveCoding(UBlueprint* Blueprint, bool bForce, FString& OutError)
{
#if PLATFORM_WINDOWS
	if (!Blueprint || !Blueprint->GeneratedClass || !Blueprint->GeneratedClass->IsChildOf(AActor::StaticClass()))
	{
		return false;
	}

	bool bLiveCodingActive = false;
	if (ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME))
	{
		bLiveCodingActive = LiveCoding->HasStarted();
	}

	if (!bLiveCodingActive || bForce)
	{
		return false;
	}

	bool bHasInstances = false;
	if (GEngine)
	{
		TSubclassOf<AActor> ActorClass = static_cast<UClass*>(Blueprint->GeneratedClass);
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (!World) continue;
			for (TActorIterator<AActor> It(World, ActorClass); It; ++It)
			{
				if (IsValid(*It))
				{
					bHasInstances = true;
					break;
				}
			}
			if (bHasInstances) break;
		}
	}

	if (bHasInstances)
	{
		OutError = FString::Printf(
			TEXT("Cannot compile '%s': Live Coding patches active and instances exist. Delete instances or restart editor. Use force=true to override."),
			*Blueprint->GetName());
		return true;
	}
#else
	(void)Blueprint;
	(void)bForce;
	(void)OutError;
#endif
	return false;
}

static sol::object HandleLiveCodingRequest(sol::state_view LuaView, sol::optional<sol::table> Opts, FLuaSessionData& Session)
{
#if PLATFORM_WINDOWS
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (!LiveCoding)
	{
		sol::table Err = LuaView.create_table();
		Err["error"] = "Live Coding module not available. Enable it in Editor Preferences.";
		return sol::make_object(LuaView, Err);
	}

	bool bCompile = false;
	if (Opts.has_value())
	{
		bCompile = Opts.value().get_or("compile", false);
	}

	if (!bCompile)
	{
		sol::table Result = LuaView.create_table();
		Result["enabled_by_default"] = LiveCoding->IsEnabledByDefault();
		Result["enabled_for_session"] = LiveCoding->IsEnabledForSession();
		Result["has_started"] = LiveCoding->HasStarted();
		Result["is_compiling"] = LiveCoding->IsCompiling();
		Result["can_enable"] = LiveCoding->CanEnableForSession();

		Session.Log(TEXT("read_log('livecoding') → status"));
		return sol::make_object(LuaView, Result);
	}

	if (!LiveCoding->IsEnabledForSession())
	{
		if (!LiveCoding->CanEnableForSession())
		{
			sol::table Err = LuaView.create_table();
			Err["error"] = "Cannot enable Live Coding — modules may have been hot-reloaded. Restart the editor.";
			return sol::make_object(LuaView, Err);
		}
		LiveCoding->EnableForSession(true);
	}

	if (LiveCoding->IsCompiling())
	{
		sol::table Err = LuaView.create_table();
		Err["error"] = "Live Coding is already compiling. Wait for the current compilation to finish.";
		return sol::make_object(LuaView, Err);
	}

	ELiveCodingCompileResult CompileResult = ELiveCodingCompileResult::Failure;
	LiveCoding->Compile(ELiveCodingCompileFlags::WaitForCompletion, &CompileResult);

	FString ResultStr;
	switch (CompileResult)
	{
	case ELiveCodingCompileResult::Success:            ResultStr = TEXT("SUCCESS"); break;
	case ELiveCodingCompileResult::NoChanges:          ResultStr = TEXT("NO_CHANGES"); break;
	case ELiveCodingCompileResult::Failure:            ResultStr = TEXT("FAILURE"); break;
	case ELiveCodingCompileResult::Cancelled:          ResultStr = TEXT("CANCELLED"); break;
	case ELiveCodingCompileResult::InProgress:         ResultStr = TEXT("IN_PROGRESS"); break;
	case ELiveCodingCompileResult::CompileStillActive: ResultStr = TEXT("COMPILE_STILL_ACTIVE"); break;
	case ELiveCodingCompileResult::NotStarted:         ResultStr = TEXT("NOT_STARTED"); break;
	default:                                           ResultStr = TEXT("UNKNOWN"); break;
	}

	sol::table Result = LuaView.create_table();
	Result["result"] = std::string(TCHAR_TO_UTF8(*ResultStr));

	if (GLog)
	{
		GLog->Flush();
	}

	FString LogFilePath = GetOutputLogPath();
	if (!LogFilePath.IsEmpty())
	{
		// Guard against OOM on massive log files (100MB limit)
		const int64 FileSize = IFileManager::Get().FileSize(*LogFilePath);
		if (FileSize > 100 * 1024 * 1024)
		{
			Session.Log(TEXT("[FAIL] read_log -> log file exceeds 100MB, too large to read"));
			return sol::lua_nil;
		}
		TArray<FString> AllLines;
		FFileHelper::LoadFileToStringArray(AllLines, *LogFilePath);

		TArray<FString> CompilerLines;
		int32 Count = 0;
		for (int32 i = AllLines.Num() - 1; i >= 0 && Count < 100; --i)
		{
			if (AllLines[i].Contains(TEXT("LiveCoding"), ESearchCase::IgnoreCase) ||
				AllLines[i].Contains(TEXT("LogLiveCoding"), ESearchCase::IgnoreCase))
			{
				CompilerLines.Add(AllLines[i]);
				Count++;
			}
		}
		Algo::Reverse(CompilerLines);

		if (CompilerLines.Num() > 0)
		{
			FString CompilerOutput;
			for (const FString& Line : CompilerLines)
			{
				if (!CompilerOutput.IsEmpty())
				{
					CompilerOutput.Append(TEXT("\n"));
				}
				CompilerOutput.Append(Line);
			}
			Result["compiler_output"] = std::string(TCHAR_TO_UTF8(*CompilerOutput));
		}
	}

	Session.Log(FString::Printf(TEXT("read_log('livecoding', {compile=true}) → %s"), *ResultStr));
	return sol::make_object(LuaView, Result);
#else
	(void)Opts;
	sol::table Err = LuaView.create_table();
	Err["error"] = "Live Coding is only available on Windows.";
	return sol::make_object(LuaView, Err);
#endif
}

// ─── Known message log names ───

static const TArray<FString>& GetKnownLogNames()
{
	static TArray<FString> Names = {
		TEXT("BlueprintLog"),
		TEXT("MapCheck"),
		TEXT("PIE"),
		TEXT("LoadErrors"),
		TEXT("EditorErrors"),
		TEXT("SlateStyleLog"),
		TEXT("AssetCheck"),
		TEXT("LightingResults"),
		TEXT("BuildAndSubmitErrors"),
		TEXT("PackagingResults"),
		TEXT("LocalizationService"),
		TEXT("HLODResults"),
		TEXT("AutomationTestingLog"),
		TEXT("AssetTools"),
		TEXT("SourceControl"),
		TEXT("TranslationEditor"),
		TEXT("UDNParser"),
		TEXT("AnimBlueprintLog"),
		TEXT("WidgetBlueprintLog"),
		TEXT("NiagaraLog"),
		TEXT("SequencerLog"),
		TEXT("MaterialLog"),
	};
	return Names;
}

// ─── Implementation ───

REGISTER_LUA_BINDING(ReadLog, ReadLogDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("read_log", [&Session](sol::optional<std::string> TypeArg,
		sol::optional<sol::table> Opts, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		// ── Validate type argument ──
		if (!TypeArg.has_value() || TypeArg.value().empty())
		{
			Session.Log(TEXT("read_log: type argument is required"));
			return sol::make_object(LuaView, std::string(TCHAR_TO_UTF8(ReadLog_HelpText)));
		}

		FString Type = UTF8_TO_TCHAR(TypeArg.value().c_str());
		Type = Type.ToLower().TrimStartAndEnd();

		// ── help ──
		if (Type == TEXT("help"))
		{
			return sol::make_object(LuaView, std::string(TCHAR_TO_UTF8(ReadLog_HelpText)));
		}

		// Check for per-type help
		bool bHelp = false;
		if (Opts.has_value())
		{
			bHelp = Opts.value().get_or("help", false);
		}

		// ════════════════════════════════════════════════════════════════════
		// TYPE: output
		// ════════════════════════════════════════════════════════════════════
		if (Type == TEXT("output"))
		{
			if (bHelp)
				return sol::make_object(LuaView, std::string(TCHAR_TO_UTF8(OutputHelpText)));

			// Parse options
			int32 Offset = 0;
			int32 Limit = 100;
			int32 Tail = 0;
			TArray<FString> SearchTerms;
			FString Severity;
			FString Category;
			bool bListCategories = false;

			if (Opts.has_value())
			{
				sol::table O = Opts.value();
				if (auto V = O.get<sol::optional<int>>("offset"))   Offset = FMath::Max(0, V.value());
				if (auto V = O.get<sol::optional<int>>("limit"))    Limit  = FMath::Clamp(V.value(), 1, 2000);
				if (auto V = O.get<sol::optional<int>>("tail"))     Tail   = FMath::Clamp(V.value(), 1, 5000);
				if (auto V = O.get<sol::optional<std::string>>("severity"))
					Severity = UTF8_TO_TCHAR(V.value().c_str());
				if (auto V = O.get<sol::optional<std::string>>("category"))
					Category = UTF8_TO_TCHAR(V.value().c_str());

				bListCategories = O.get_or("list_categories", false);

				// Parse search array
				sol::optional<sol::table> SearchTable = O.get<sol::optional<sol::table>>("search");
				if (SearchTable.has_value())
				{
					for (auto& Pair : SearchTable.value())
					{
						if (Pair.second.is<std::string>())
						{
							FString Term = UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str());
							if (!Term.IsEmpty())
								SearchTerms.Add(Term.ToLower());
						}
					}
				}
			}

			// Flush log
			if (GLog)
				GLog->Flush();

			FString LogFilePath = GetOutputLogPath();
			if (LogFilePath.IsEmpty())
			{
				sol::table Err = LuaView.create_table();
				Err["error"] = "Could not find output log file in Saved/Logs/";
				return Err;
			}

			// Read all lines (guard against massive logs)
			const int64 LogFileSize = IFileManager::Get().FileSize(*LogFilePath);
			if (LogFileSize > 100 * 1024 * 1024)
			{
				sol::table Err = LuaView.create_table();
				Err["error"] = "Output log exceeds 100MB, too large to read";
				return Err;
			}
			TArray<FString> AllLines;
			FFileHelper::LoadFileToStringArray(AllLines, *LogFilePath);

			if (AllLines.Num() == 0)
			{
				sol::table Err = LuaView.create_table();
				Err["error"] = "Log file is empty or could not be read";
				Err["path"] = std::string(TCHAR_TO_UTF8(*LogFilePath));
				return Err;
			}

			int32 TotalLines = AllLines.Num();

			// ── list_categories mode ──
			if (bListCategories)
			{
				TSet<FString> Categories;
				for (const FString& Line : AllLines)
				{
					FString Cat = ExtractCategory(Line);
					if (!Cat.IsEmpty())
						Categories.Add(Cat);
				}

				TArray<FString> Sorted = Categories.Array();
				Sorted.Sort();

				sol::table Result = LuaView.create_table();
				Result["total_lines"] = TotalLines;
				Result["path"] = std::string(TCHAR_TO_UTF8(*LogFilePath));

				sol::table CatTable = LuaView.create_table();
				int32 Idx = 1;
				for (const FString& Cat : Sorted)
				{
					CatTable[Idx++] = std::string(TCHAR_TO_UTF8(*Cat));
				}
				Result["categories"] = CatTable;
				Result["category_count"] = Sorted.Num();

				Session.Log(FString::Printf(TEXT("read_log('output', {list_categories=true}) → %d categories"), Sorted.Num()));
				return Result;
			}

			// ── Filter lines ──
			TArray<TPair<int32, FString>> FilteredLines;
			for (int32 i = 0; i < AllLines.Num(); ++i)
			{
				const FString& Line = AllLines[i];
				if (MatchesSeverity(Line, Severity) &&
					MatchesSearch(Line, SearchTerms) &&
					MatchesCategory(Line, Category))
				{
					FilteredLines.Add(TPair<int32, FString>(i + 1, Line));
				}
			}

			int32 MatchedLines = FilteredLines.Num();

			// ── Pagination ──
			int32 StartIndex, EndIndex;
			if (Tail > 0)
			{
				StartIndex = FMath::Max(0, FilteredLines.Num() - Tail);
				EndIndex = FilteredLines.Num();
			}
			else
			{
				StartIndex = FMath::Min(Offset, FilteredLines.Num());
				EndIndex = FMath::Min(StartIndex + Limit, FilteredLines.Num());
			}

			// Build result
			sol::table Result = LuaView.create_table();
			Result["total_lines"] = TotalLines;
			Result["matched_lines"] = MatchedLines;
			Result["path"] = std::string(TCHAR_TO_UTF8(*LogFilePath));

			sol::table LinesTable = LuaView.create_table();
			int32 Idx = 1;

			if (FilteredLines.Num() > 0 && StartIndex < FilteredLines.Num())
			{
				Result["start_line"] = FilteredLines[StartIndex].Key;
				if (EndIndex > 0 && EndIndex <= FilteredLines.Num())
					Result["end_line"] = FilteredLines[EndIndex - 1].Key;

				for (int32 i = StartIndex; i < EndIndex; ++i)
				{
					FString FormattedLine = FString::Printf(TEXT("%5d: %s"), FilteredLines[i].Key, *FilteredLines[i].Value);
					LinesTable[Idx++] = std::string(TCHAR_TO_UTF8(*FormattedLine));
				}
			}
			else
			{
				Result["start_line"] = 0;
				Result["end_line"] = 0;
			}

			Result["lines"] = LinesTable;

			Session.Log(FString::Printf(TEXT("read_log('output') → %d/%d lines (showing %d)"),
				MatchedLines, TotalLines, Idx - 1));
			return Result;
		}

		// ════════════════════════════════════════════════════════════════════
		// TYPE: message
		// ════════════════════════════════════════════════════════════════════
		if (Type == TEXT("message"))
		{
			if (bHelp)
				return sol::make_object(LuaView, std::string(TCHAR_TO_UTF8(MessageHelpText)));

			FMessageLogModule* MessageLogModule = FModuleManager::GetModulePtr<FMessageLogModule>("MessageLog");
			if (!MessageLogModule)
			{
				sol::table Err = LuaView.create_table();
				Err["error"] = "MessageLog module not available";
				return Err;
			}

			// Check for list mode
			bool bList = false;
			bool bClear = false;
			bool bOpen = false;
			FString LogName;
			FString Severity;
			int32 Limit = 100;

			if (Opts.has_value())
			{
				sol::table O = Opts.value();
				bList = O.get_or("list", false);
				bClear = O.get_or("clear", false);
				bOpen = O.get_or("open", false);
				if (auto V = O.get<sol::optional<std::string>>("log"))
					LogName = UTF8_TO_TCHAR(V.value().c_str());
				if (auto V = O.get<sol::optional<std::string>>("severity"))
					Severity = UTF8_TO_TCHAR(V.value().c_str());
				if (auto V = O.get<sol::optional<int>>("limit"))
					Limit = FMath::Clamp(V.value(), 1, 500);
			}

			// ── list mode ──
			if (bList)
			{
				sol::table Result = LuaView.create_table();
				int32 Idx = 1;

				for (const FString& Name : GetKnownLogNames())
				{
					if (!MessageLogModule->IsRegisteredLogListing(FName(*Name)))
						continue;

					TSharedRef<IMessageLogListing> Listing = MessageLogModule->GetLogListing(FName(*Name));
					const TArray<TSharedRef<FTokenizedMessage>>& Messages = Listing->GetFilteredMessages();

					int32 MsgCount = Messages.Num();
					bool bHasErrors = false;
					for (const auto& Msg : Messages)
					{
						if (Msg->GetSeverity() == EMessageSeverity::Error)
						{
							bHasErrors = true;
							break;
						}
					}

					sol::table Entry = LuaView.create_table();
					Entry["name"] = std::string(TCHAR_TO_UTF8(*Name));
					Entry["message_count"] = MsgCount;
					Entry["has_errors"] = bHasErrors;
					Result[Idx++] = Entry;
				}

				Session.Log(FString::Printf(TEXT("read_log('message', {list=true}) → %d registered logs"), Idx - 1));
				return Result;
			}

			// ── clear mode ──
			if (bClear)
			{
				if (LogName.IsEmpty())
				{
					sol::table Err = LuaView.create_table();
					Err["error"] = "Missing 'log' option for clear. Example: {log='BlueprintLog', clear=true}";
					return Err;
				}

				if (MessageLogModule->IsRegisteredLogListing(FName(*LogName)))
				{
					TSharedRef<IMessageLogListing> Listing = MessageLogModule->GetLogListing(FName(*LogName));
					Listing->ClearMessages();

					sol::table Result = LuaView.create_table();
					Result["log_name"] = std::string(TCHAR_TO_UTF8(*LogName));
					Result["cleared"] = true;

					Session.Log(FString::Printf(TEXT("read_log('message', {log='%s', clear=true}) → cleared"), *LogName));
					return Result;
				}
				else
				{
					sol::table Err = LuaView.create_table();
					Err["error"] = std::string(TCHAR_TO_UTF8(*FString::Printf(
						TEXT("Message log '%s' not found. Use {list=true} to see available logs."), *LogName)));
					return Err;
				}
			}

			// ── open mode ──
			if (bOpen)
			{
				if (LogName.IsEmpty())
				{
					sol::table Err = LuaView.create_table();
					Err["error"] = "Missing 'log' option for open. Example: {log='BlueprintLog', open=true}";
					return Err;
				}

				MessageLogModule->OpenMessageLog(FName(*LogName));

				sol::table Result = LuaView.create_table();
				Result["log_name"] = std::string(TCHAR_TO_UTF8(*LogName));
				Result["opened"] = true;

				Session.Log(FString::Printf(TEXT("read_log('message', {log='%s', open=true}) → opened"), *LogName));
				return Result;
			}

			// ── read a specific log ──
			if (LogName.IsEmpty())
			{
				sol::table Err = LuaView.create_table();
				Err["error"] = "Missing 'log' option. Use {list=true} to see available logs, or {log='BlueprintLog'}.";
				return Err;
			}

			// Try to register on-demand if not registered
			if (!MessageLogModule->IsRegisteredLogListing(FName(*LogName)))
			{
				FMessageLog(FName(*LogName));

				if (!MessageLogModule->IsRegisteredLogListing(FName(*LogName)))
				{
					sol::table Err = LuaView.create_table();
					Err["error"] = std::string(TCHAR_TO_UTF8(*FString::Printf(
						TEXT("Message log '%s' not found. Use {list=true} to see available logs."), *LogName)));
					return Err;
				}
			}

			TSharedRef<IMessageLogListing> Listing = MessageLogModule->GetLogListing(FName(*LogName));
			const TArray<TSharedRef<FTokenizedMessage>>& Messages = Listing->GetFilteredMessages();

			FString NormSeverity = NormalizeSeverity(Severity);
			int32 ShownCount = 0;

			sol::table Result = LuaView.create_table();
			Result["log_name"] = std::string(TCHAR_TO_UTF8(*LogName));
			Result["total_messages"] = static_cast<int>(Messages.Num());

			sol::table MsgTable = LuaView.create_table();
			int32 Idx = 1;

			// Most recent first
			for (int32 i = Messages.Num() - 1; i >= 0 && ShownCount < Limit; --i)
			{
				const TSharedRef<FTokenizedMessage>& Message = Messages[i];
				EMessageSeverity::Type MsgSeverity = Message->GetSeverity();

				// Apply severity filter
				if (!NormSeverity.IsEmpty() && NormSeverity != TEXT("all"))
				{
					if (NormSeverity == TEXT("error") && MsgSeverity != EMessageSeverity::Error)
						continue;
					if (NormSeverity == TEXT("warning") && MsgSeverity != EMessageSeverity::Warning &&
						MsgSeverity != EMessageSeverity::PerformanceWarning)
						continue;
					if (NormSeverity == TEXT("info") && MsgSeverity != EMessageSeverity::Info)
						continue;
				}

				sol::table MsgEntry = LuaView.create_table();
				MsgEntry["severity"] = std::string(TCHAR_TO_UTF8(*SeverityToString(MsgSeverity)));
				MsgEntry["text"] = std::string(TCHAR_TO_UTF8(*Message->ToText().ToString()));
				MsgTable[Idx++] = MsgEntry;
				ShownCount++;
			}

			Result["messages"] = MsgTable;

			Session.Log(FString::Printf(TEXT("read_log('message', {log='%s'}) → %d/%d messages"),
				*LogName, ShownCount, Messages.Num()));
			return Result;
		}

		// ════════════════════════════════════════════════════════════════════
		// TYPE: compile
		// ════════════════════════════════════════════════════════════════════
		if (Type == TEXT("compile"))
		{
			if (bHelp)
				return sol::make_object(LuaView, std::string(TCHAR_TO_UTF8(CompileHelpText)));

			FString Asset;
			FString Path;
			bool bForce = false;

			if (Opts.has_value())
			{
				sol::table O = Opts.value();
				if (auto V = O.get<sol::optional<std::string>>("asset"))
					Asset = UTF8_TO_TCHAR(V.value().c_str());
				if (auto V = O.get<sol::optional<std::string>>("path"))
					Path = UTF8_TO_TCHAR(V.value().c_str());
				bForce = O.get_or("force", false);
			}

			if (Asset.IsEmpty())
			{
				sol::table Err = LuaView.create_table();
				Err["error"] = "Missing required 'asset' option. Usage: read_log('compile', {asset='/Game/Blueprints/MyBP'}) or {asset='MyBP', path='/Game/Blueprints'}";
				return Err;
			}

			FString FullAssetPath = BuildAssetPath(Asset, Path);

			// Load and validate
			UObject* AssetObj = LoadObject<UObject>(nullptr, *FullAssetPath);
			if (!AssetObj)
			{
				sol::table Err = LuaView.create_table();
				Err["error"] = std::string(TCHAR_TO_UTF8(*FString::Printf(TEXT("Asset not found: %s"), *FullAssetPath)));
				return Err;
			}

			// Material compile: use UMaterialEditingLibrary::RecompileMaterial
			if (UMaterial* Mat = Cast<UMaterial>(AssetObj))
			{
				UMaterialEditingLibrary::RecompileMaterial(Mat);

				sol::table Result = LuaView.create_table();
				Result["name"] = TCHAR_TO_UTF8(*Mat->GetName());
				Result["type"] = "Material";
				Result["status"] = "compiled";

				Session.Log(FString::Printf(TEXT("read_log('compile', {asset='%s'}) -> Material compiled"),
					*Mat->GetName()));
				return Result;
			}

			UBlueprint* Blueprint = Cast<UBlueprint>(AssetObj);
			if (!Blueprint)
			{
				sol::table Err = LuaView.create_table();
				Err["error"] = std::string(TCHAR_TO_UTF8(*FString::Printf(
					TEXT("Asset is not a Blueprint or Material: %s (%s)"), *FullAssetPath, *AssetObj->GetClass()->GetName())));
				return Err;
			}

			if (!Blueprint->GeneratedClass)
			{
				sol::table Err = LuaView.create_table();
				Err["error"] = std::string(TCHAR_TO_UTF8(*FString::Printf(
					TEXT("Blueprint '%s' has no generated class — may be corrupted. Cannot compile."), *Blueprint->GetName())));
				return Err;
			}

			if (Blueprint->bBeingCompiled)
			{
				sol::table Err = LuaView.create_table();
				Err["error"] = std::string(TCHAR_TO_UTF8(*FString::Printf(
					TEXT("Blueprint '%s' is already being compiled."), *Blueprint->GetName())));
				return Err;
			}

			// Block during PIE
			if (GEditor && GEditor->IsPlayingSessionInEditor())
			{
				sol::table Err = LuaView.create_table();
				Err["error"] = std::string(TCHAR_TO_UTF8(*FString::Printf(
					TEXT("Cannot compile '%s' during PIE. Stop PIE first."), *Blueprint->GetName())));
				return Err;
			}

			// Live Coding safety check (Windows only)
			FString LiveCodingError;
			if (IsBlueprintCompileBlockedByLiveCoding(Blueprint, bForce, LiveCodingError))
			{
				sol::table Err = LuaView.create_table();
				Err["error"] = std::string(TCHAR_TO_UTF8(*LiveCodingError));
				return Err;
			}

			// Get status before
			FString StatusBefore = BlueprintStatusToString(Blueprint->Status);

			// Compile
			FCompilerResultsLog CompileLog;
			CompileLog.bSilentMode = false;
			CompileLog.bAnnotateMentionedNodes = true;

			FKismetEditorUtilities::CompileBlueprint(Blueprint,
				EBlueprintCompileOptions::None,
				&CompileLog);

			FString StatusAfter = BlueprintStatusToString(Blueprint->Status);

			// Build result
			sol::table Result = LuaView.create_table();
			Result["name"] = std::string(TCHAR_TO_UTF8(*Blueprint->GetName()));
			Result["status_before"] = std::string(TCHAR_TO_UTF8(*StatusBefore));
			Result["status_after"] = std::string(TCHAR_TO_UTF8(*StatusAfter));
			Result["errors"] = CompileLog.NumErrors;
			Result["warnings"] = CompileLog.NumWarnings;

			sol::table MsgTable = LuaView.create_table();
			int32 Idx = 1;

			for (const TSharedRef<FTokenizedMessage>& Message : CompileLog.Messages)
			{
				FString MsgText = Message->ToText().ToString();
				MsgText.ReplaceInline(TEXT("\r\n"), TEXT(" "), ESearchCase::CaseSensitive);
				MsgText.ReplaceInline(TEXT("\n"), TEXT(" "), ESearchCase::CaseSensitive);

				sol::table MsgEntry = LuaView.create_table();
				MsgEntry["severity"] = std::string(TCHAR_TO_UTF8(*SeverityToString(Message->GetSeverity())));
				MsgEntry["text"] = std::string(TCHAR_TO_UTF8(*MsgText));
				MsgTable[Idx++] = MsgEntry;
			}

			Result["messages"] = MsgTable;

			// Check BlueprintLog for additional messages related to this BP
			FMessageLogModule* MessageLogModule = FModuleManager::GetModulePtr<FMessageLogModule>("MessageLog");
			if (MessageLogModule && MessageLogModule->IsRegisteredLogListing(FName("BlueprintLog")))
			{
				TSharedRef<IMessageLogListing> BPLog = MessageLogModule->GetLogListing(FName("BlueprintLog"));
				const TArray<TSharedRef<FTokenizedMessage>>& LogMessages = BPLog->GetFilteredMessages();

				sol::table BPLogTable = LuaView.create_table();
				int32 BPIdx = 1;

				for (int32 i = LogMessages.Num() - 1; i >= 0 && BPIdx <= 10; --i)
				{
					FString MsgText = LogMessages[i]->ToText().ToString();
					if (MsgText.Contains(Blueprint->GetName()))
					{
						sol::table Entry = LuaView.create_table();
						Entry["severity"] = std::string(TCHAR_TO_UTF8(*SeverityToString(LogMessages[i]->GetSeverity())));
						Entry["text"] = std::string(TCHAR_TO_UTF8(*MsgText));
						BPLogTable[BPIdx++] = Entry;
					}
				}

				if (BPIdx > 1)
					Result["blueprint_log_messages"] = BPLogTable;
			}

			Session.Log(FString::Printf(TEXT("read_log('compile', {asset='%s'}) → %s, %d errors, %d warnings"),
				*Blueprint->GetName(), *StatusAfter, CompileLog.NumErrors, CompileLog.NumWarnings));
			return Result;
		}

		// ════════════════════════════════════════════════════════════════════
		// TYPE: livecoding
		// ════════════════════════════════════════════════════════════════════
		if (Type == TEXT("livecoding"))
		{
			if (bHelp)
				return sol::make_object(LuaView, std::string(TCHAR_TO_UTF8(LiveCodingHelpText)));

			return HandleLiveCodingRequest(LuaView, Opts, Session);
		}

		// ── Unknown type ──
		Session.Log(FString::Printf(TEXT("read_log: unknown type '%s'"), *Type));
		sol::table Err = LuaView.create_table();
		Err["error"] = std::string(TCHAR_TO_UTF8(*FString::Printf(
			TEXT("Unknown type '%s'. Valid types: output, message, compile, livecoding. Use read_log('help') for usage."), *Type)));
		return Err;
	});
});
