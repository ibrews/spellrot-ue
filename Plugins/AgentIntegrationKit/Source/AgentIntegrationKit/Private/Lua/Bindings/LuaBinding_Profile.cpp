// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Engine/Engine.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformMemory.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/App.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#include "Misc/StringOutputDevice.h"
#endif
#include "Misc/StringBuilder.h"
#include "Framework/Application/SlateApplication.h"
#include "ProfilingDebugging/TraceAuxiliary.h"
#include "RHIStats.h"
#include "DynamicRHI.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ─── Session State ───

namespace
{
	struct FProfileSessionState
	{
		bool bActive = false;
		FString Mode;
		FString SessionName;
		FString Channels;
		FString TraceFilePath;
		FString CsvCaptureName;
		FDateTime StartedAt;
		TArray<FString> StartCommands;
	};

	FCriticalSection GProfileStateLock;
	FProfileSessionState GProfileState;
	FString GLastTraceArtifactPath;
	FString GLastCsvCaptureName;
	FString GLastCsvArtifactPath;
	FString GLastCaptureMode;

	// ─── Helper Structs ───

	struct FTraceSummaryArtifacts
	{
		FString ScopesCsv;
		FString CountersCsv;
		FString BookmarksCsv;
		FString TelemetryCsv;
	};

	struct FTopScopeRow
	{
		FString Name;
		uint64 Count = 0;
		double TotalDurationSeconds = 0.0;
		double MeanDurationSeconds = 0.0;
		double MaxDurationSeconds = 0.0;
	};

	struct FNumericSeries
	{
		int32 Count = 0;
		double Sum = 0.0;
		double Min = TNumericLimits<double>::Max();
		double Max = -TNumericLimits<double>::Max();
		TArray<double> Values;

		void Add(double Value)
		{
			if (!FMath::IsFinite(Value)) return;
			++Count;
			Sum += Value;
			Min = FMath::Min(Min, Value);
			Max = FMath::Max(Max, Value);
			Values.Add(Value);
		}

		double Mean() const { return Count > 0 ? (Sum / double(Count)) : 0.0; }

		double Percentile(double Pct) const
		{
			if (Values.Num() == 0) return 0.0;
			TArray<double> Sorted = Values;
			Sorted.Sort();
			const double ClampedPct = FMath::Clamp(Pct, 0.0, 100.0);
			const double Rank = (ClampedPct / 100.0) * double(Sorted.Num() - 1);
			const int32 Index = FMath::Clamp(FMath::RoundToInt(Rank), 0, Sorted.Num() - 1);
			return Sorted[Index];
		}
	};

	struct FFrameBreakdown
	{
		int32 FrameIndex = 0;
		double FrameMs = 0.0;
		double GameMs = 0.0;
		double RenderMs = 0.0;
		double GpuMs = 0.0;
	};

	// ─── Helper Functions ───

	static bool ExecuteConsoleCommand(const FString& Command, FString& OutResult)
	{
		if (!GEngine)
		{
			OutResult = TEXT("Engine is not available.");
			return false;
		}
		FStringOutputDevice Ar;
		const bool bOk = GEngine->Exec(nullptr, *Command, Ar);
		OutResult = Ar;
		return bOk;
	}

	static FString BuildDefaultSessionName()
	{
		return FString::Printf(TEXT("aik_profile_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	}

	static FString SanitizeSessionName(const FString& InSessionName)
	{
		FString Out = InSessionName;
		Out.TrimStartAndEndInline();
		if (Out.IsEmpty()) return BuildDefaultSessionName();

		for (int32 Index = 0; Index < Out.Len(); ++Index)
		{
			const TCHAR C = Out[Index];
			const bool bIsAllowed =
				(C >= 'a' && C <= 'z') ||
				(C >= 'A' && C <= 'Z') ||
				(C >= '0' && C <= '9') ||
				C == '_' || C == '-';
			if (!bIsAllowed) Out[Index] = '_';
		}
		return Out;
	}

	static FString BuildDefaultTracePath(const FString& SessionName)
	{
		const FString Directory = FPaths::ProjectSavedDir() / TEXT("Profiling") / TEXT("AIK");
		IFileManager::Get().MakeDirectory(*Directory, true);
		return FPaths::ConvertRelativePathToFull(Directory / FString::Printf(TEXT("%s.utrace"), *SessionName));
	}

	static FString NormalizePathForRead(const FString& InPath)
	{
		FString Path = InPath;
		Path.TrimStartAndEndInline();
		if (Path.IsEmpty()) return Path;
		if (FPaths::IsRelative(Path))
		{
			Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / Path);
		}
		FPaths::NormalizeFilename(Path);
		return Path;
	}

	static FString FindNewestTraceArtifact(const FDateTime& NotBefore)
	{
		TArray<FString> FoundFiles;
		const FString SearchRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
		IFileManager::Get().FindFilesRecursive(FoundFiles, *SearchRoot, TEXT("*.utrace"), true, false, false);

		FDateTime BestTime = FDateTime::MinValue();
		FString BestFile;
		for (const FString& File : FoundFiles)
		{
			const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*File);
			if (Stamp >= NotBefore && Stamp > BestTime)
			{
				BestTime = Stamp;
				BestFile = File;
			}
		}
		return BestFile;
	}

	static FString GetCsvProfilingDirectory()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProfilingDir() / TEXT("CSV"));
	}

	static TArray<FString> FindCsvArtifactsByPrefix(const FString& Prefix)
	{
		TArray<FString> Results;
		const FString CsvDir = GetCsvProfilingDirectory();
		if (!IFileManager::Get().DirectoryExists(*CsvDir)) return Results;

		TArray<FString> FileNames;
		IFileManager::Get().FindFiles(FileNames, *(CsvDir / TEXT("*")), true, false);
		for (const FString& FileName : FileNames)
		{
			if (FileName.StartsWith(Prefix, ESearchCase::IgnoreCase))
			{
				FString FullPath = CsvDir / FileName;
				FPaths::NormalizeFilename(FullPath);
				Results.Add(FullPath);
			}
		}
		return Results;
	}

	static FString FindBestCsvArtifactForCapture(const FString& CaptureName)
	{
		const TArray<FString> AllCandidates = FindCsvArtifactsByPrefix(CaptureName);
		if (AllCandidates.Num() == 0) return TEXT("");

		// Filter to uncompressed .csv files only — compressed .csv.gz cannot be parsed
		TArray<FString> Candidates;
		for (const FString& C : AllCandidates)
		{
			if (C.EndsWith(TEXT(".csv"), ESearchCase::IgnoreCase) && !C.EndsWith(TEXT(".csv.gz"), ESearchCase::IgnoreCase))
				Candidates.Add(C);
		}
		if (Candidates.Num() == 0) return TEXT("");

		// Prefer exact filename match
		for (const FString& Candidate : Candidates)
		{
			if (FPaths::GetCleanFilename(Candidate).Equals(CaptureName, ESearchCase::IgnoreCase))
				return Candidate;
		}

		// Fall back to newest by timestamp
		FDateTime BestTime = FDateTime::MinValue();
		FString BestPath;
		for (const FString& Candidate : Candidates)
		{
			const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Candidate);
			if (Stamp >= BestTime)
			{
				BestTime = Stamp;
				BestPath = Candidate;
			}
		}
		return BestPath;
	}

	static FString FindNewestCsvArtifact(const FDateTime& NotBefore)
	{
		const FString CsvDir = GetCsvProfilingDirectory();
		if (!IFileManager::Get().DirectoryExists(*CsvDir)) return TEXT("");

		TArray<FString> FileNames;
		IFileManager::Get().FindFiles(FileNames, *(CsvDir / TEXT("*")), true, false);
		FDateTime BestTime = FDateTime::MinValue();
		FString BestPath;
		for (const FString& FileName : FileNames)
		{
			FString FullPath = CsvDir / FileName;
			FPaths::NormalizeFilename(FullPath);
			const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*FullPath);
			if (Stamp >= NotBefore && Stamp > BestTime)
			{
				BestTime = Stamp;
				BestPath = FullPath;
			}
		}
		return BestPath;
	}

	static FString NormalizeMode(const FString& InMode)
	{
		FString Mode = InMode;
		Mode.TrimStartAndEndInline();
		Mode = Mode.ToLower();
		if (Mode.IsEmpty()) Mode = TEXT("trace");
		return Mode;
	}

	static bool IsSupportedMode(const FString& Mode)
	{
		return Mode == TEXT("trace") || Mode == TEXT("csv") || Mode == TEXT("statfile");
	}

	static bool IsTraceConnectionActive(FString* OutDestination = nullptr)
	{
		const bool bConnected = FTraceAuxiliary::IsConnected();
		const FString Destination = FTraceAuxiliary::GetTraceDestinationString();
		const bool bHasDestination = !Destination.IsEmpty();
		if (OutDestination) *OutDestination = Destination;
		return bConnected || bHasDestination;
	}

	static bool WaitForTraceConnection(double TimeoutSeconds, FString& OutDestination)
	{
		// Check immediately first — avoids any blocking in the common case
		if (IsTraceConnectionActive(&OutDestination)) return true;

		// Brief polling with Slate ticks to keep editor responsive
		const double EndTime = FPlatformTime::Seconds() + TimeoutSeconds;
		while (FPlatformTime::Seconds() <= EndTime)
		{
			// Tick Slate so the editor stays responsive instead of hard-blocking
			if (FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().PumpMessages();
				FSlateApplication::Get().Tick();
			}
			else
			{
				FPlatformProcess::SleepNoStats(0.02f);
			}

			if (IsTraceConnectionActive(&OutDestination)) return true;
		}
		return false;
	}

	static FString BuildEditorCmdExecutablePath()
	{
		TArray<FString> Candidates;
		const FString ExecutableDir = FPaths::GetPath(FPlatformProcess::ExecutablePath());

#if PLATFORM_WINDOWS
		const FString EditorCmdName = TEXT("UnrealEditor-Cmd.exe");
		const FString EditorName = TEXT("UnrealEditor.exe");
		const FString PlatformDir = TEXT("Win64");
#elif PLATFORM_MAC
		const FString EditorCmdName = TEXT("UnrealEditor-Cmd");
		const FString EditorName = TEXT("UnrealEditor");
		const FString PlatformDir = TEXT("Mac");
#else
		const FString EditorCmdName = TEXT("UnrealEditor-Cmd");
		const FString EditorName = TEXT("UnrealEditor");
		const FString PlatformDir = TEXT("Linux");
#endif

		Candidates.Add(ExecutableDir / EditorCmdName);
		Candidates.Add(ExecutableDir / EditorName);
		Candidates.Add(FPaths::ConvertRelativePathToFull(ExecutableDir / TEXT("../../../") / EditorCmdName));
		Candidates.Add(FPaths::ConvertRelativePathToFull(ExecutableDir / TEXT("../../../") / EditorName));
		Candidates.Add(FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries") / PlatformDir / EditorCmdName));
		Candidates.Add(FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries") / PlatformDir / EditorName));
		Candidates.Add(FPaths::ConvertRelativePathToFull(FPaths::RootDir() / TEXT("Engine") / TEXT("Binaries") / PlatformDir / EditorCmdName));
		Candidates.Add(FPaths::ConvertRelativePathToFull(FPaths::RootDir() / TEXT("Engine") / TEXT("Binaries") / PlatformDir / EditorName));

		for (FString Candidate : Candidates)
		{
			FPaths::NormalizeFilename(Candidate);
			if (FPaths::FileExists(Candidate)) return Candidate;
		}
		return TEXT("");
	}

	static FTraceSummaryArtifacts BuildSummaryArtifacts(const FString& TraceFilePath)
	{
		const FString TraceDir = FPaths::GetPath(TraceFilePath);
		const FString TraceBase = FPaths::GetBaseFilename(TraceFilePath);

		FTraceSummaryArtifacts Artifacts;
		Artifacts.ScopesCsv = TraceDir / FString::Printf(TEXT("%sScopes.csv"), *TraceBase);
		Artifacts.CountersCsv = TraceDir / FString::Printf(TEXT("%sCounters.csv"), *TraceBase);
		Artifacts.BookmarksCsv = TraceDir / FString::Printf(TEXT("%sBookmarks.csv"), *TraceBase);
		Artifacts.TelemetryCsv = TraceDir / FString::Printf(TEXT("%sTelemetry.csv"), *TraceBase);
		return Artifacts;
	}

	static bool ParseUInt64Field(const FString& Text, uint64& OutValue)
	{
		const TCHAR* Start = *Text;
		TCHAR* End = nullptr;
		OutValue = FCString::Strtoui64(Start, &End, 10);
		return End != Start;
	}

	static bool ParseDoubleField(const FString& Text, double& OutValue)
	{
		OutValue = FCString::Atod(*Text);
		return true;
	}

	static int32 FindColumnIndex(const TArray<FString>& HeaderCells, const TArray<FString>& CandidateNames)
	{
		for (int32 i = 0; i < HeaderCells.Num(); ++i)
		{
			for (const FString& Candidate : CandidateNames)
			{
				if (HeaderCells[i].Equals(Candidate, ESearchCase::IgnoreCase))
					return i;
			}
		}
		return INDEX_NONE;
	}

	static bool ReadTopScopesFromCsv(const FString& ScopesCsvPath, int32 TopCount, TArray<FTopScopeRow>& OutRows, FString& OutError)
	{
		OutRows.Reset();

		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *ScopesCsvPath))
		{
			OutError = FString::Printf(TEXT("Failed to read scopes csv: %s"), *ScopesCsvPath);
			return false;
		}

		TArray<FString> Lines;
		Content.ParseIntoArrayLines(Lines);
		if (Lines.Num() <= 1)
		{
			OutError = FString::Printf(TEXT("Scopes csv has no rows: %s"), *ScopesCsvPath);
			return false;
		}

		// Header-based column lookup instead of hardcoded indices
		TArray<FString> HeaderCells;
		Lines[0].ParseIntoArray(HeaderCells, TEXT(","), false);

		const int32 NameIdx = FindColumnIndex(HeaderCells, { TEXT("Name"), TEXT("Scope") });
		const int32 CountIdx = FindColumnIndex(HeaderCells, { TEXT("Count") });
		const int32 TotalIdx = FindColumnIndex(HeaderCells, { TEXT("TotalDurationSeconds"), TEXT("Total") });
		const int32 MaxIdx = FindColumnIndex(HeaderCells, { TEXT("MaxDurationSeconds"), TEXT("Max") });
		const int32 MeanIdx = FindColumnIndex(HeaderCells, { TEXT("MeanDurationSeconds"), TEXT("Mean"), TEXT("Average") });

		if (NameIdx == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("Scopes csv missing Name column: %s"), *ScopesCsvPath);
			return false;
		}

		TArray<FTopScopeRow> ParsedRows;
		for (int32 LineIndex = 1; LineIndex < Lines.Num(); ++LineIndex)
		{
			const FString& Line = Lines[LineIndex];
			if (Line.IsEmpty()) continue;

			TArray<FString> Cells;
			Line.ParseIntoArray(Cells, TEXT(","), false);
			if (NameIdx >= Cells.Num()) continue;

			FTopScopeRow Row;
			Row.Name = Cells[NameIdx];
			if (Row.Name.IsEmpty()) continue;

			if (CountIdx != INDEX_NONE && CountIdx < Cells.Num())
				ParseUInt64Field(Cells[CountIdx], Row.Count);
			if (TotalIdx != INDEX_NONE && TotalIdx < Cells.Num())
				ParseDoubleField(Cells[TotalIdx], Row.TotalDurationSeconds);
			if (MaxIdx != INDEX_NONE && MaxIdx < Cells.Num())
				ParseDoubleField(Cells[MaxIdx], Row.MaxDurationSeconds);
			if (MeanIdx != INDEX_NONE && MeanIdx < Cells.Num())
				ParseDoubleField(Cells[MeanIdx], Row.MeanDurationSeconds);
			if (!FMath::IsFinite(Row.TotalDurationSeconds) || Row.TotalDurationSeconds < 0.0) continue;

			ParsedRows.Add(Row);
		}

		if (ParsedRows.Num() == 0)
		{
			OutError = FString::Printf(TEXT("Scopes csv had no parseable rows: %s"), *ScopesCsvPath);
			return false;
		}

		ParsedRows.Sort([](const FTopScopeRow& A, const FTopScopeRow& B)
		{
			return A.TotalDurationSeconds > B.TotalDurationSeconds;
		});

		const int32 ClampedTop = FMath::Clamp(TopCount, 1, 50);
		OutRows.Append(ParsedRows.GetData(), FMath::Min(ClampedTop, ParsedRows.Num()));
		return true;
	}

	static bool GetFiniteCellValue(const TArray<FString>& Cells, int32 Index, double& OutValue)
	{
		if (Index < 0 || Index >= Cells.Num()) return false;
		OutValue = FCString::Atod(*Cells[Index]);
		return FMath::IsFinite(OutValue);
	}

	static bool AnalyzeCsvArtifact(const FString& CsvPath, int32 TopCount,
		sol::state_view& LuaView, sol::table& OutResult, FString& OutError)
	{
		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *CsvPath))
		{
			OutError = FString::Printf(TEXT("Failed to read CSV artifact: %s"), *CsvPath);
			return false;
		}

		TArray<FString> Lines;
		Content.ParseIntoArrayLines(Lines);
		if (Lines.Num() < 2)
		{
			OutError = FString::Printf(TEXT("CSV artifact has insufficient rows: %s"), *CsvPath);
			return false;
		}

		TArray<FString> HeaderCells;
		Lines[0].ParseIntoArray(HeaderCells, TEXT(","), false);
		if (HeaderCells.Num() < 2)
		{
			OutError = FString::Printf(TEXT("CSV artifact header could not be parsed: %s"), *CsvPath);
			return false;
		}

		const int32 FrameIdx = FindColumnIndex(HeaderCells, { TEXT("FrameTime") });
		const int32 GameIdx = FindColumnIndex(HeaderCells, { TEXT("GameThreadTime"), TEXT("GameThreadTime_CriticalPath") });
		const int32 RenderIdx = FindColumnIndex(HeaderCells, { TEXT("RenderThreadTime"), TEXT("RenderThreadTime_CriticalPath") });
		const int32 GpuIdx = FindColumnIndex(HeaderCells, { TEXT("GPUTime") });
		const int32 RhiIdx = FindColumnIndex(HeaderCells, { TEXT("RHIThreadTime") });

		if (FrameIdx == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("CSV artifact does not contain FrameTime column: %s"), *CsvPath);
			return false;
		}

		FNumericSeries FrameSeries, GameSeries, RenderSeries, GpuSeries, RhiSeries;
		int32 HitchOver33 = 0, HitchOver50 = 0;
		int32 BoundByGame = 0, BoundByRender = 0, BoundByGpu = 0;

		// Bounded worst-frames tracking — only keep the top N heaviest frames
		const int32 MaxWorst = FMath::Clamp(TopCount, 1, 50);
		TArray<FFrameBreakdown> WorstFrames;
		WorstFrames.Reserve(MaxWorst + 1);

		for (int32 LineIndex = 1; LineIndex < Lines.Num(); ++LineIndex)
		{
			const FString& Line = Lines[LineIndex];
			if (Line.IsEmpty()) continue;

			TArray<FString> Cells;
			Line.ParseIntoArray(Cells, TEXT(","), false);
			if (Cells.Num() <= FrameIdx) continue;

			double FrameMs = 0.0;
			if (!GetFiniteCellValue(Cells, FrameIdx, FrameMs)) continue;

			FrameSeries.Add(FrameMs);
			if (FrameMs > 33.333) ++HitchOver33;
			if (FrameMs > 50.0) ++HitchOver50;

			double GameMs = 0.0, RenderMs = 0.0, GpuMs = 0.0, RhiMs = 0.0;
			const bool bHasGame = GetFiniteCellValue(Cells, GameIdx, GameMs);
			const bool bHasRender = GetFiniteCellValue(Cells, RenderIdx, RenderMs);
			const bool bHasGpu = GetFiniteCellValue(Cells, GpuIdx, GpuMs);
			const bool bHasRhi = GetFiniteCellValue(Cells, RhiIdx, RhiMs);

			if (bHasGame) GameSeries.Add(GameMs);
			if (bHasRender) RenderSeries.Add(RenderMs);
			if (bHasGpu) GpuSeries.Add(GpuMs);
			if (bHasRhi) RhiSeries.Add(RhiMs);

			double DominantValue = -TNumericLimits<double>::Max();
			FString Dominant;
			if (bHasGame && GameMs > DominantValue) { DominantValue = GameMs; Dominant = TEXT("game"); }
			if (bHasRender && RenderMs > DominantValue) { DominantValue = RenderMs; Dominant = TEXT("render"); }
			if (bHasGpu && GpuMs > DominantValue) { DominantValue = GpuMs; Dominant = TEXT("gpu"); }
			if (Dominant == TEXT("game")) ++BoundByGame;
			else if (Dominant == TEXT("render")) ++BoundByRender;
			else if (Dominant == TEXT("gpu")) ++BoundByGpu;

			// Bounded insertion: only keep the worst N frames
			if (WorstFrames.Num() < MaxWorst || FrameMs > WorstFrames.Last().FrameMs)
			{
				FFrameBreakdown Breakdown;
				Breakdown.FrameIndex = LineIndex;
				Breakdown.FrameMs = FrameMs;
				Breakdown.GameMs = bHasGame ? GameMs : 0.0;
				Breakdown.RenderMs = bHasRender ? RenderMs : 0.0;
				Breakdown.GpuMs = bHasGpu ? GpuMs : 0.0;

				// Insert sorted (descending by FrameMs)
				int32 InsertIdx = 0;
				while (InsertIdx < WorstFrames.Num() && WorstFrames[InsertIdx].FrameMs >= FrameMs)
					++InsertIdx;
				WorstFrames.Insert(Breakdown, InsertIdx);

				if (WorstFrames.Num() > MaxWorst)
					WorstFrames.SetNum(MaxWorst);
			}
		}

		if (FrameSeries.Count == 0)
		{
			OutError = FString::Printf(TEXT("CSV artifact did not contain any parseable frame rows: %s"), *CsvPath);
			return false;
		}

		OutResult["analysis_mode"] = "csv_direct";
		OutResult["artifact_csv"] = TCHAR_TO_UTF8(*CsvPath);
		OutResult["frames"] = FrameSeries.Count;
		OutResult["frame_time_avg_ms"] = FrameSeries.Mean();
		OutResult["frame_time_p95_ms"] = FrameSeries.Percentile(95.0);
		OutResult["frame_time_p99_ms"] = FrameSeries.Percentile(99.0);
		OutResult["frame_time_max_ms"] = FrameSeries.Max;
		OutResult["hitches_over_33ms"] = HitchOver33;
		OutResult["hitches_over_50ms"] = HitchOver50;

		if (GameSeries.Count > 0)
		{
			OutResult["game_thread_avg_ms"] = GameSeries.Mean();
			OutResult["game_thread_p95_ms"] = GameSeries.Percentile(95.0);
		}
		if (RenderSeries.Count > 0)
		{
			OutResult["render_thread_avg_ms"] = RenderSeries.Mean();
			OutResult["render_thread_p95_ms"] = RenderSeries.Percentile(95.0);
		}
		if (GpuSeries.Count > 0)
		{
			OutResult["gpu_avg_ms"] = GpuSeries.Mean();
			OutResult["gpu_p95_ms"] = GpuSeries.Percentile(95.0);
		}
		if (RhiSeries.Count > 0)
		{
			OutResult["rhi_thread_avg_ms"] = RhiSeries.Mean();
		}

		const int32 BoundSamples = BoundByGame + BoundByRender + BoundByGpu;
		if (BoundSamples > 0)
		{
			const double Inv = 100.0 / double(BoundSamples);
			OutResult["bound_by_game_pct"] = double(BoundByGame) * Inv;
			OutResult["bound_by_render_pct"] = double(BoundByRender) * Inv;
			OutResult["bound_by_gpu_pct"] = double(BoundByGpu) * Inv;
		}

		sol::table WorstTable = LuaView.create_table();
		for (int32 i = 0; i < WorstFrames.Num(); ++i)
		{
			const FFrameBreakdown& Row = WorstFrames[i];
			sol::table FrameRow = LuaView.create_table();
			FrameRow["row"] = Row.FrameIndex;
			FrameRow["frame_ms"] = Row.FrameMs;
			FrameRow["game_ms"] = Row.GameMs;
			FrameRow["render_ms"] = Row.RenderMs;
			FrameRow["gpu_ms"] = Row.GpuMs;
			WorstTable[i + 1] = FrameRow;
		}
		OutResult["worst_frames"] = WorstTable;

		return true;
	}

	static void PopulateFrameStats(sol::state_view& LuaView, sol::table& Result)
	{
		Result["delta_time_ms"] = FApp::GetDeltaTime() * 1000.0;
		Result["idle_time_ms"] = FApp::GetIdleTime() * 1000.0;
		Result["frame_number"] = (int64)GFrameNumber;
		Result["elapsed_time_s"] = FApp::GetCurrentTime();

		// RHI stats
		Result["draw_calls"] = GNumDrawCallsRHI[0];
		Result["primitives_drawn"] = GNumPrimitivesDrawnRHI[0];

		// GPU frame time
		uint32 GPUCycles = RHIGetGPUFrameCycles();
		Result["gpu_frame_time_ms"] = FPlatformTime::ToMilliseconds(GPUCycles);

		// Target FPS context
		if (GEngine)
		{
			const float MaxFPS = GEngine->GetMaxFPS();
			if (MaxFPS > 0.0f)
				Result["target_fps"] = MaxFPS;
		}
	}

	static void PopulateMemoryStats(sol::state_view& LuaView, sol::table& Result)
	{
		FPlatformMemoryStats MemStats = FPlatformMemory::GetStats();

		Result["used_physical_mb"] = (double)MemStats.UsedPhysical / (1024.0 * 1024.0);
		Result["peak_used_physical_mb"] = (double)MemStats.PeakUsedPhysical / (1024.0 * 1024.0);
		Result["available_physical_mb"] = (double)MemStats.AvailablePhysical / (1024.0 * 1024.0);
		Result["used_virtual_mb"] = (double)MemStats.UsedVirtual / (1024.0 * 1024.0);
		Result["peak_used_virtual_mb"] = (double)MemStats.PeakUsedVirtual / (1024.0 * 1024.0);

		// Total system memory (from FPlatformMemoryConstants, inherited by FPlatformMemoryStats)
		Result["total_physical_mb"] = (double)MemStats.TotalPhysical / (1024.0 * 1024.0);
		Result["total_virtual_mb"] = (double)MemStats.TotalVirtual / (1024.0 * 1024.0);
		Result["total_physical_gb"] = (int32)MemStats.TotalPhysicalGB;

		auto Pressure = MemStats.GetMemoryPressureStatus();
		if (Pressure == FGenericPlatformMemoryStats::EMemoryPressureStatus::Nominal)
			Result["memory_pressure"] = "nominal";
		else if (Pressure == FGenericPlatformMemoryStats::EMemoryPressureStatus::Warning)
			Result["memory_pressure"] = "warning";
		else if (Pressure == FGenericPlatformMemoryStats::EMemoryPressureStatus::Critical)
			Result["memory_pressure"] = "critical";
		else
			Result["memory_pressure"] = "unknown";
	}
}

// ─── Documentation ───

static const TCHAR* Profile_HelpText = TEXT(
	"profile(verb, opts?) -- Runtime profiling and performance metrics\n"
	"\n"
	"Session management:\n"
	"  profile('start', {mode?, session?, channels?, trace_file?})\n"
	"    mode: 'trace' (default), 'csv', 'statfile'\n"
	"    channels: trace channels (default 'cpu,gpu,frame,bookmark,log')\n"
	"    session: name for capture (auto-generated if omitted)\n"
	"    trace_file: output path for .utrace (trace mode only)\n"
	"  profile('stop')        -- stop active session\n"
	"  profile('status')      -- session state + trace system status + active channels\n"
	"  profile('analyze', {trace_file?, top?, preview?})\n"
	"    Converts .utrace->CSV via SummarizeTrace commandlet, or reads CSV directly\n"
	"    top: number of top scopes (1-50, default 10)\n"
	"    preview: include parsed top-scope preview (default true)\n"
	"\n"
	"Live snapshots (no session needed):\n"
	"  profile('frame_stats') -- {delta_time_ms, idle_time_ms, frame_number, draw_calls, primitives_drawn, gpu_frame_time_ms, target_fps?}\n"
	"  profile('memory')      -- {used/peak/available/total physical+virtual MB, total_physical_gb, memory_pressure}\n"
	"  profile('snapshot')    -- combined frame + memory + timestamp\n"
	"\n"
	"Discovery:\n"
	"  profile('list_channels') -- available trace channels\n"
	"  profile('list_presets')  -- channel presets from engine config\n"
	"\n"
	"Recommended workflow:\n"
	"  1. profile('start')\n"
	"  2. Ask user to run PIE/playtest\n"
	"  3. profile('stop')\n"
	"  4. profile('analyze')\n"
	"\n"
	"For detailed LLM memory breakdown, use execute_python to run:\n"
	"  unreal.SystemLibrary.execute_console_command(None, 'memreport -full')\n"
);

static TArray<FLuaFunctionDoc> ProfileDocs = {
	{ TEXT("profile(verb, opts?)"), TEXT("Runtime profiling -- verbs: start, stop, status, analyze, frame_stats, memory, snapshot, list_channels, list_presets"), TEXT("table or string") },
};

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
static const char* Profile_TraceSystemStatusToString(FTraceAuxiliary::ETraceSystemStatus Status)
{
	switch (Status)
	{
	case FTraceAuxiliary::ETraceSystemStatus::NotAvailable:    return "not_available";
	case FTraceAuxiliary::ETraceSystemStatus::Available:       return "available";
	case FTraceAuxiliary::ETraceSystemStatus::TracingToServer: return "tracing_to_server";
	case FTraceAuxiliary::ETraceSystemStatus::TracingToFile:   return "tracing_to_file";
#if ENGINE_MINOR_VERSION >= 7
	case FTraceAuxiliary::ETraceSystemStatus::TracingToCustomRelay: return "tracing_to_relay";
#endif
	default: return "unknown";
	}
}
#endif

// ─── Helpers for version-conditional code (cannot use #if inside macro arguments) ───

static void Profile_PopulatePresets(sol::table& Result, int32& Index, sol::state_view& LuaView)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	// Fixed presets (defined in code)
	FTraceAuxiliary::EnumerateFixedChannelPresets(
		[&](const FTraceAuxiliary::FChannelPreset& Preset) -> FTraceAuxiliary::EEnumerateResult
	{
		sol::table Entry = LuaView.create_table();
		Entry["name"] = TCHAR_TO_UTF8(Preset.Name);
		Entry["channels"] = TCHAR_TO_UTF8(Preset.ChannelList);
		Entry["source"] = "fixed";
		Entry["read_only"] = Preset.bIsReadOnly;
		Result[Index++] = Entry;
		return FTraceAuxiliary::EEnumerateResult::Continue;
	});

	// Settings presets (from BaseEngine.ini)
	FTraceAuxiliary::EnumerateChannelPresetsFromSettings(
		[&](const FTraceAuxiliary::FChannelPreset& Preset) -> FTraceAuxiliary::EEnumerateResult
	{
		sol::table Entry = LuaView.create_table();
		Entry["name"] = TCHAR_TO_UTF8(Preset.Name);
		Entry["channels"] = TCHAR_TO_UTF8(Preset.ChannelList);
		Entry["source"] = "settings";
		Entry["read_only"] = Preset.bIsReadOnly;
		Result[Index++] = Entry;
		return FTraceAuxiliary::EEnumerateResult::Continue;
	});
#endif
}

static void Profile_PopulateTraceSystemStatus(sol::table& Result)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	Result["trace_system"] = Profile_TraceSystemStatusToString(FTraceAuxiliary::GetTraceSystemStatus());
#endif
}

// ─── Implementation ───

REGISTER_LUA_BINDING(Profile, ProfileDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("profile", [&Session](sol::optional<std::string> VerbArg,
		sol::optional<sol::table> Opts, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		if (!VerbArg.has_value() || VerbArg.value().empty())
		{
			Session.Log(TEXT("profile: verb argument is required"));
			return sol::make_object(LuaView, "error: verb argument is required. Use profile('help') for usage.");
		}

		FString Verb = UTF8_TO_TCHAR(VerbArg.value().c_str());
		Verb = Verb.ToLower().TrimStartAndEnd();

		// ── help ──
		if (Verb == TEXT("help"))
		{
			Session.Log(TEXT("profile('help')"));
			return sol::make_object(LuaView, TCHAR_TO_UTF8(Profile_HelpText));
		}

		// ── frame_stats ──
		if (Verb == TEXT("frame_stats"))
		{
			sol::table Result = LuaView.create_table();
			PopulateFrameStats(LuaView, Result);
			Session.Log(TEXT("profile('frame_stats')"));
			return Result;
		}

		// ── memory ──
		if (Verb == TEXT("memory"))
		{
			sol::table Result = LuaView.create_table();
			PopulateMemoryStats(LuaView, Result);
			Session.Log(TEXT("profile('memory')"));
			return Result;
		}

		// ── snapshot ──
		if (Verb == TEXT("snapshot"))
		{
			sol::table Result = LuaView.create_table();
			PopulateFrameStats(LuaView, Result);
			PopulateMemoryStats(LuaView, Result);
			Result["timestamp"] = TCHAR_TO_UTF8(*FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")));
			Session.Log(TEXT("profile('snapshot')"));
			return Result;
		}

		// ── list_channels ──
		if (Verb == TEXT("list_channels"))
		{
			sol::table Result = LuaView.create_table();
			int32 Index = 1;

			const TCHAR* BuiltinChannels[] = {
				TEXT("cpu"), TEXT("gpu"), TEXT("frame"), TEXT("bookmark"), TEXT("log"),
				TEXT("memory"), TEXT("loadtime"), TEXT("io"), TEXT("context_switches"),
				TEXT("callstacks"), TEXT("object"), TEXT("counters"), TEXT("rendering")
			};
			for (const TCHAR* Ch : BuiltinChannels)
			{
				sol::table Entry = LuaView.create_table();
				Entry["name"] = TCHAR_TO_UTF8(Ch);
				Entry["source"] = "builtin";
				Result[Index++] = Entry;
			}

			Session.Log(TEXT("profile('list_channels')"));
			return Result;
		}

		// ── list_presets ──
		if (Verb == TEXT("list_presets"))
		{
			sol::table Result = LuaView.create_table();
			int32 Index = 1;

			Profile_PopulatePresets(Result, Index, LuaView);

			Session.Log(FString::Printf(TEXT("profile('list_presets') -> %d presets"), Index - 1));
			return Result;
		}

		// ── status ──
		if (Verb == TEXT("status"))
		{
			sol::table Result = LuaView.create_table();
			FScopeLock Lock(&GProfileStateLock);
			Result["active"] = GProfileState.bActive;
			if (GProfileState.bActive)
			{
				Result["mode"] = TCHAR_TO_UTF8(*GProfileState.Mode);
				Result["session"] = TCHAR_TO_UTF8(*GProfileState.SessionName);
				Result["started_at"] = TCHAR_TO_UTF8(*GProfileState.StartedAt.ToString(TEXT("%Y-%m-%d %H:%M:%S")));
				if (!GProfileState.Channels.IsEmpty())
					Result["channels"] = TCHAR_TO_UTF8(*GProfileState.Channels);
				if (!GProfileState.TraceFilePath.IsEmpty())
					Result["trace_file"] = TCHAR_TO_UTF8(*GProfileState.TraceFilePath);
				if (!GProfileState.CsvCaptureName.IsEmpty())
					Result["csv_capture"] = TCHAR_TO_UTF8(*GProfileState.CsvCaptureName);
			}

			// Trace system status (always available, independent of our session state)
			Result["trace_connected"] = FTraceAuxiliary::IsConnected();
			const FString TraceDest = FTraceAuxiliary::GetTraceDestinationString();
			if (!TraceDest.IsEmpty())
				Result["trace_destination"] = TCHAR_TO_UTF8(*TraceDest);

			Profile_PopulateTraceSystemStatus(Result);

			// Active channels
			TStringBuilder<1024> ActiveChannels;
			FTraceAuxiliary::GetActiveChannelsString(ActiveChannels);
			const FString ChannelsStr(ActiveChannels.ToView());
			if (!ChannelsStr.IsEmpty())
				Result["active_channels"] = TCHAR_TO_UTF8(*ChannelsStr);

			Session.Log(TEXT("profile('status')"));
			return Result;
		}

		// ── start ──
		if (Verb == TEXT("start"))
		{
			// Parse options
			FString Mode = TEXT("trace");
			FString SessionName;
			FString TraceChannels = TEXT("cpu,gpu,frame,bookmark,log");
			FString TraceFilePath;

			if (Opts.has_value())
			{
				sol::table O = Opts.value();
				if (auto V = O.get<sol::optional<std::string>>("mode"))
					Mode = NormalizeMode(UTF8_TO_TCHAR(V.value().c_str()));
				if (auto V = O.get<sol::optional<std::string>>("session"))
					SessionName = UTF8_TO_TCHAR(V.value().c_str());
				if (auto V = O.get<sol::optional<std::string>>("channels"))
				{
					FString Ch = UTF8_TO_TCHAR(V.value().c_str());
					Ch.TrimStartAndEndInline();
					if (!Ch.IsEmpty()) TraceChannels = Ch;
				}
				if (auto V = O.get<sol::optional<std::string>>("trace_file"))
					TraceFilePath = UTF8_TO_TCHAR(V.value().c_str());
			}

			SessionName = SanitizeSessionName(SessionName);

			if (!IsSupportedMode(Mode))
			{
				Session.Log(FString::Printf(TEXT("profile('start'): unsupported mode '%s'"), *Mode));
				return sol::make_object(LuaView, std::string("error: unsupported mode '") + TCHAR_TO_UTF8(*Mode) + "'. Valid: trace, csv, statfile");
			}

			{
				FScopeLock Lock(&GProfileStateLock);
				if (GProfileState.bActive)
				{
					Session.Log(TEXT("profile('start'): session already active"));
					return sol::make_object(LuaView, std::string("error: a profiling session is already active: ") + TCHAR_TO_UTF8(*GProfileState.SessionName));
				}
			}

			TArray<FString> ExecutedCommands;
			sol::table Result = LuaView.create_table();

			if (Mode == TEXT("trace"))
			{
				TraceFilePath.TrimStartAndEndInline();
				if (TraceFilePath.IsEmpty())
				{
					TraceFilePath = BuildDefaultTracePath(SessionName);
				}
				else if (FPaths::IsRelative(TraceFilePath))
				{
					TraceFilePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TraceFilePath);
				}

				IFileManager::Get().MakeDirectory(*FPaths::GetPath(TraceFilePath), true);

				const FString Command = FString::Printf(TEXT("Trace.File \"%s\" %s"), *TraceFilePath, *TraceChannels);
				FString Output;
				const bool bExecOk = ExecuteConsoleCommand(Command, Output);
				ExecutedCommands.Add(Command);

				if (!bExecOk)
				{
					Session.Log(FString::Printf(TEXT("profile('start'): failed to execute: %s"), *Command));
					return sol::make_object(LuaView, std::string("error: failed to execute command: ") + TCHAR_TO_UTF8(*Command));
				}

				FString RuntimeTraceDest;
				bool bConnected = WaitForTraceConnection(1.5, RuntimeTraceDest);
				if (!bConnected)
				{
					const FString FallbackCommand = FString::Printf(TEXT("Trace.Start %s"), *TraceChannels);
					FString FallbackOutput;
					const bool bFallbackOk = ExecuteConsoleCommand(FallbackCommand, FallbackOutput);
					ExecutedCommands.Add(FallbackCommand);

					if (bFallbackOk)
						bConnected = WaitForTraceConnection(1.5, RuntimeTraceDest);

					if (!bConnected)
					{
						Session.Log(TEXT("profile('start'): trace did not connect"));
						return sol::make_object(LuaView, "error: trace start did not connect after retries");
					}
				}

				if (!RuntimeTraceDest.IsEmpty())
					TraceFilePath = RuntimeTraceDest;

				FScopeLock Lock(&GProfileStateLock);
				GProfileState = FProfileSessionState();
				GProfileState.bActive = true;
				GProfileState.Mode = Mode;
				GProfileState.SessionName = SessionName;
				GProfileState.Channels = TraceChannels;
				GProfileState.TraceFilePath = TraceFilePath;
				GProfileState.StartedAt = FDateTime::Now();
				GProfileState.StartCommands = ExecutedCommands;

				Result["trace_file"] = TCHAR_TO_UTF8(*TraceFilePath);
				Result["channels"] = TCHAR_TO_UTF8(*TraceChannels);
			}
			else if (Mode == TEXT("csv"))
			{
				const FString CsvCaptureName = SessionName;
				const FString CmdSetName = FString::Printf(TEXT("CsvProfile StartFile=%s"), *CsvCaptureName);
				const FString CmdStart = TEXT("CsvProfile Start");

				FString OutputSetName;
				if (!ExecuteConsoleCommand(CmdSetName, OutputSetName))
				{
					Session.Log(FString::Printf(TEXT("profile('start'): failed: %s"), *CmdSetName));
					return sol::make_object(LuaView, std::string("error: failed to execute: ") + TCHAR_TO_UTF8(*CmdSetName));
				}
				ExecutedCommands.Add(CmdSetName);

				FString OutputStart;
				if (!ExecuteConsoleCommand(CmdStart, OutputStart))
				{
					Session.Log(FString::Printf(TEXT("profile('start'): failed: %s"), *CmdStart));
					return sol::make_object(LuaView, std::string("error: failed to execute: ") + TCHAR_TO_UTF8(*CmdStart));
				}
				ExecutedCommands.Add(CmdStart);

				FScopeLock Lock(&GProfileStateLock);
				GProfileState = FProfileSessionState();
				GProfileState.bActive = true;
				GProfileState.Mode = Mode;
				GProfileState.SessionName = SessionName;
				GProfileState.CsvCaptureName = CsvCaptureName;
				GProfileState.StartedAt = FDateTime::Now();
				GProfileState.StartCommands = ExecutedCommands;

				Result["csv_capture"] = TCHAR_TO_UTF8(*CsvCaptureName);
			}
			else if (Mode == TEXT("statfile"))
			{
				const FString CmdStart = TEXT("stat startfile");
				FString OutputStart;
				if (!ExecuteConsoleCommand(CmdStart, OutputStart))
				{
					Session.Log(FString::Printf(TEXT("profile('start'): failed: %s"), *CmdStart));
					return sol::make_object(LuaView, std::string("error: failed to execute: ") + TCHAR_TO_UTF8(*CmdStart));
				}
				ExecutedCommands.Add(CmdStart);

				FScopeLock Lock(&GProfileStateLock);
				GProfileState = FProfileSessionState();
				GProfileState.bActive = true;
				GProfileState.Mode = Mode;
				GProfileState.SessionName = SessionName;
				GProfileState.StartedAt = FDateTime::Now();
				GProfileState.StartCommands = ExecutedCommands;
			}

			Result["session"] = TCHAR_TO_UTF8(*SessionName);
			Result["mode"] = TCHAR_TO_UTF8(*Mode);

			sol::table CmdTable = LuaView.create_table();
			for (int32 i = 0; i < ExecutedCommands.Num(); ++i)
				CmdTable[i + 1] = TCHAR_TO_UTF8(*ExecutedCommands[i]);
			Result["commands"] = CmdTable;

			Session.Log(FString::Printf(TEXT("profile('start', mode=%s, session=%s)"), *Mode, *SessionName));
			return Result;
		}

		// ── stop ──
		if (Verb == TEXT("stop"))
		{
			FProfileSessionState SessionCopy;
			{
				FScopeLock Lock(&GProfileStateLock);
				if (!GProfileState.bActive)
				{
					Session.Log(TEXT("profile('stop'): no active session"));
					return sol::make_object(LuaView, "error: no active profiling session");
				}
				SessionCopy = GProfileState;
			}

			TArray<FString> StopCommands;

			if (SessionCopy.Mode == TEXT("trace"))
			{
				const FString CmdStop = TEXT("Trace.Stop");
				FString OutputStop;
				const bool bOk = ExecuteConsoleCommand(CmdStop, OutputStop);
				StopCommands.Add(CmdStop);
				if (!bOk)
				{
					Session.Log(FString::Printf(TEXT("profile('stop'): failed: %s"), *CmdStop));
					return sol::make_object(LuaView, std::string("error: failed to execute: ") + TCHAR_TO_UTF8(*CmdStop));
				}
			}
			else if (SessionCopy.Mode == TEXT("csv"))
			{
				const FString CmdStop = TEXT("CsvProfile Stop");
				FString OutputStop;
				const bool bOk = ExecuteConsoleCommand(CmdStop, OutputStop);
				StopCommands.Add(CmdStop);
				if (!bOk)
				{
					Session.Log(FString::Printf(TEXT("profile('stop'): failed: %s"), *CmdStop));
					return sol::make_object(LuaView, std::string("error: failed to execute: ") + TCHAR_TO_UTF8(*CmdStop));
				}
			}
			else if (SessionCopy.Mode == TEXT("statfile"))
			{
				const FString CmdStop = TEXT("stat stopfile");
				FString OutputStop;
				const bool bOk = ExecuteConsoleCommand(CmdStop, OutputStop);
				StopCommands.Add(CmdStop);
				if (!bOk)
				{
					Session.Log(FString::Printf(TEXT("profile('stop'): failed: %s"), *CmdStop));
					return sol::make_object(LuaView, std::string("error: failed to execute: ") + TCHAR_TO_UTF8(*CmdStop));
				}
			}

			// Clear active state
			{
				FScopeLock Lock(&GProfileStateLock);
				GProfileState = FProfileSessionState();
			}

			sol::table Result = LuaView.create_table();
			Result["session"] = TCHAR_TO_UTF8(*SessionCopy.SessionName);
			Result["mode"] = TCHAR_TO_UTF8(*SessionCopy.Mode);

			// Resolve artifact path
			if (SessionCopy.Mode == TEXT("trace") && !SessionCopy.TraceFilePath.IsEmpty())
			{
				bool bTraceExists = FPaths::FileExists(SessionCopy.TraceFilePath);
				if (!bTraceExists)
				{
					const FString FallbackTrace = FindNewestTraceArtifact(SessionCopy.StartedAt - FTimespan::FromSeconds(5));
					if (!FallbackTrace.IsEmpty())
					{
						SessionCopy.TraceFilePath = FallbackTrace;
						bTraceExists = true;
					}
				}

				Result["artifact_path"] = TCHAR_TO_UTF8(*SessionCopy.TraceFilePath);
				Result["artifact_exists"] = bTraceExists;

				FScopeLock Lock(&GProfileStateLock);
				GLastTraceArtifactPath = bTraceExists ? SessionCopy.TraceFilePath : TEXT("");
				GLastCaptureMode = TEXT("trace");
			}
			else if (SessionCopy.Mode == TEXT("csv") && !SessionCopy.CsvCaptureName.IsEmpty())
			{
				const FString CsvPath = FindBestCsvArtifactForCapture(SessionCopy.CsvCaptureName);
				if (!CsvPath.IsEmpty())
				{
					Result["artifact_path"] = TCHAR_TO_UTF8(*CsvPath);
					Result["artifact_exists"] = true;
				}
				else
				{
					const FString CsvDir = FPaths::ProfilingDir() / TEXT("CSV");
					Result["artifact_path"] = TCHAR_TO_UTF8(*FString::Printf(TEXT("%s/%s.csv"), *CsvDir, *SessionCopy.CsvCaptureName));
					Result["artifact_exists"] = false;
				}

				FScopeLock Lock(&GProfileStateLock);
				GLastCsvCaptureName = SessionCopy.CsvCaptureName;
				GLastCsvArtifactPath = CsvPath;
				GLastCaptureMode = TEXT("csv");
			}

			sol::table CmdTable = LuaView.create_table();
			for (int32 i = 0; i < StopCommands.Num(); ++i)
				CmdTable[i + 1] = TCHAR_TO_UTF8(*StopCommands[i]);
			Result["commands"] = CmdTable;

			Session.Log(FString::Printf(TEXT("profile('stop', session=%s, mode=%s)"), *SessionCopy.SessionName, *SessionCopy.Mode));
			return Result;
		}

		// ── analyze ──
		if (Verb == TEXT("analyze"))
		{
			int32 TopCount = 10;
			bool bPreview = true;
			FString ArtifactPath;

			if (Opts.has_value())
			{
				sol::table O = Opts.value();
				if (auto V = O.get<sol::optional<int>>("top"))
					TopCount = FMath::Clamp(V.value(), 1, 50);
				if (auto V = O.get<sol::optional<bool>>("preview"))
					bPreview = V.value();
				if (auto V = O.get<sol::optional<std::string>>("trace_file"))
					ArtifactPath = NormalizePathForRead(UTF8_TO_TCHAR(V.value().c_str()));
			}

			{
				FScopeLock Lock(&GProfileStateLock);
				if (GProfileState.bActive)
				{
					Session.Log(TEXT("profile('analyze'): session still active"));
					return sol::make_object(LuaView, "error: a profiling session is still active. Call profile('stop') first.");
				}

				if (ArtifactPath.IsEmpty())
				{
					if (GLastCaptureMode == TEXT("trace") && !GLastTraceArtifactPath.IsEmpty() && FPaths::FileExists(GLastTraceArtifactPath))
					{
						ArtifactPath = GLastTraceArtifactPath;
					}
					else if (GLastCaptureMode == TEXT("csv"))
					{
						if (!GLastCsvArtifactPath.IsEmpty() && FPaths::FileExists(GLastCsvArtifactPath))
							ArtifactPath = GLastCsvArtifactPath;
						else if (!GLastCsvCaptureName.IsEmpty())
							ArtifactPath = FindBestCsvArtifactForCapture(GLastCsvCaptureName);
					}
				}
			}

			if (ArtifactPath.IsEmpty())
			{
				const FString NewestTrace = FindNewestTraceArtifact(FDateTime::MinValue());
				const FString NewestCsv = FindNewestCsvArtifact(FDateTime::MinValue());
				if (!NewestTrace.IsEmpty() && !NewestCsv.IsEmpty())
				{
					const FDateTime TraceStamp = IFileManager::Get().GetTimeStamp(*NewestTrace);
					const FDateTime CsvStamp = IFileManager::Get().GetTimeStamp(*NewestCsv);
					ArtifactPath = (TraceStamp >= CsvStamp) ? NewestTrace : NewestCsv;
				}
				else
				{
					ArtifactPath = !NewestTrace.IsEmpty() ? NewestTrace : NewestCsv;
				}
			}

			if (ArtifactPath.IsEmpty())
			{
				Session.Log(TEXT("profile('analyze'): no artifact found"));
				return sol::make_object(LuaView, "error: no profiling artifact found. Provide trace_file or run start/stop first.");
			}
			if (!FPaths::FileExists(ArtifactPath))
			{
				Session.Log(FString::Printf(TEXT("profile('analyze'): artifact not found: %s"), *ArtifactPath));
				return sol::make_object(LuaView, std::string("error: artifact not found: ") + TCHAR_TO_UTF8(*ArtifactPath));
			}

			const bool bIsTrace = ArtifactPath.EndsWith(TEXT(".utrace"), ESearchCase::IgnoreCase);

			// Direct CSV analysis
			if (!bIsTrace)
			{
				if (ArtifactPath.EndsWith(TEXT(".csv.gz"), ESearchCase::IgnoreCase))
				{
					Session.Log(FString::Printf(TEXT("profile('analyze'): compressed csv not supported: %s"), *ArtifactPath));
					return sol::make_object(LuaView, "error: compressed CSV (.csv.gz) not supported. Disable csv.CompressionMode=0 and recapture.");
				}

				sol::table Result = LuaView.create_table();
				FString CsvError;
				if (!AnalyzeCsvArtifact(ArtifactPath, TopCount, LuaView, Result, CsvError))
				{
					Session.Log(FString::Printf(TEXT("profile('analyze'): csv error: %s"), *CsvError));
					return sol::make_object(LuaView, std::string("error: ") + TCHAR_TO_UTF8(*CsvError));
				}

				{
					FScopeLock Lock(&GProfileStateLock);
					GLastCsvArtifactPath = ArtifactPath;
					GLastCaptureMode = TEXT("csv");
				}

				Session.Log(FString::Printf(TEXT("profile('analyze', csv=%s)"), *ArtifactPath));
				return Result;
			}

			// .utrace -> SummarizeTrace commandlet
			{
				FScopeLock Lock(&GProfileStateLock);
				GLastTraceArtifactPath = ArtifactPath;
				GLastCaptureMode = TEXT("trace");
			}

			const FString EditorCmdPath = BuildEditorCmdExecutablePath();
			if (EditorCmdPath.IsEmpty())
			{
				Session.Log(TEXT("profile('analyze'): UnrealEditor-Cmd not found"));
				return sol::make_object(LuaView, "error: failed to locate UnrealEditor-Cmd executable for SummarizeTrace");
			}

			FString ProjectFilePath = FPaths::GetProjectFilePath();
			ProjectFilePath = NormalizePathForRead(ProjectFilePath);
			if (ProjectFilePath.IsEmpty() || !FPaths::FileExists(ProjectFilePath))
			{
				Session.Log(TEXT("profile('analyze'): project file not found"));
				return sol::make_object(LuaView, "error: project file not found");
			}

			const FString CommandLine = FString::Printf(
				TEXT("\"%s\" -run=SummarizeTrace -inputfile=\"%s\" -alltelemetry -skipbaseline -nop4 -nosplash -unattended"),
				*ProjectFilePath,
				*ArtifactPath);

			// Launch SummarizeTrace as async child process, tick Slate to keep editor responsive
			void* ReadPipe = nullptr;
			void* WritePipe = nullptr;
			if (!FPlatformProcess::CreatePipe(ReadPipe, WritePipe))
			{
				Session.Log(TEXT("profile('analyze'): failed to create pipe"));
				return sol::make_object(LuaView, "error: failed to create output pipe for SummarizeTrace");
			}

			FProcHandle ProcHandle = FPlatformProcess::CreateProc(
				*EditorCmdPath, *CommandLine,
				/*bLaunchDetached=*/false, /*bLaunchHidden=*/true, /*bLaunchReallyHidden=*/true,
				/*OutProcessID=*/nullptr, /*PriorityModifier=*/0,
				/*OptionalWorkingDirectory=*/nullptr,
				WritePipe);

			if (!ProcHandle.IsValid())
			{
				FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
				Session.Log(TEXT("profile('analyze'): SummarizeTrace exec failed"));
				return sol::make_object(LuaView, std::string("error: failed to execute SummarizeTrace: ") + TCHAR_TO_UTF8(*EditorCmdPath));
			}

			// Poll for completion while keeping the editor responsive
			FString StdOut;
			while (FPlatformProcess::IsProcRunning(ProcHandle))
			{
				StdOut += FPlatformProcess::ReadPipe(ReadPipe);

				if (FSlateApplication::IsInitialized())
				{
					FSlateApplication::Get().PumpMessages();
					FSlateApplication::Get().Tick();
				}
				else
				{
					FPlatformProcess::SleepNoStats(0.05f);
				}
			}
			// Read any remaining output
			StdOut += FPlatformProcess::ReadPipe(ReadPipe);

			int32 ReturnCode = -1;
			FPlatformProcess::GetProcReturnCode(ProcHandle, &ReturnCode);
			FPlatformProcess::CloseProc(ProcHandle);
			FPlatformProcess::ClosePipe(ReadPipe, WritePipe);

			if (ReturnCode != 0)
			{
				FString Failure = FString::Printf(TEXT("SummarizeTrace failed (exit_code=%d)"), ReturnCode);
				if (!StdOut.IsEmpty()) Failure += TEXT("\n") + StdOut.Left(2000);
				Session.Log(FString::Printf(TEXT("profile('analyze'): %s"), *Failure));
				return sol::make_object(LuaView, std::string("error: ") + TCHAR_TO_UTF8(*Failure));
			}

			const FTraceSummaryArtifacts Artifacts = BuildSummaryArtifacts(ArtifactPath);
			const bool bScopesExists = FPaths::FileExists(Artifacts.ScopesCsv);
			const bool bCountersExists = FPaths::FileExists(Artifacts.CountersCsv);
			const bool bBookmarksExists = FPaths::FileExists(Artifacts.BookmarksCsv);
			const bool bTelemetryExists = FPaths::FileExists(Artifacts.TelemetryCsv);

			if (!bScopesExists && !bCountersExists && !bBookmarksExists)
			{
				Session.Log(TEXT("profile('analyze'): no CSV artifacts generated"));
				return sol::make_object(LuaView, "error: SummarizeTrace completed but no CSV artifacts found");
			}

			sol::table Result = LuaView.create_table();
			Result["analysis_mode"] = "summarize_trace";
			Result["trace_file"] = TCHAR_TO_UTF8(*ArtifactPath);

			sol::table ArtifactsTable = LuaView.create_table();
			if (bScopesExists)
				ArtifactsTable["scopes_csv"] = TCHAR_TO_UTF8(*Artifacts.ScopesCsv);
			if (bCountersExists)
				ArtifactsTable["counters_csv"] = TCHAR_TO_UTF8(*Artifacts.CountersCsv);
			if (bBookmarksExists)
				ArtifactsTable["bookmarks_csv"] = TCHAR_TO_UTF8(*Artifacts.BookmarksCsv);
			if (bTelemetryExists)
				ArtifactsTable["telemetry_csv"] = TCHAR_TO_UTF8(*Artifacts.TelemetryCsv);
			Result["artifacts"] = ArtifactsTable;

			Result["command"] = TCHAR_TO_UTF8(*FString::Printf(TEXT("%s %s"), *EditorCmdPath, *CommandLine));

			if (bPreview && bScopesExists)
			{
				TArray<FTopScopeRow> TopScopes;
				FString ParseError;
				if (ReadTopScopesFromCsv(Artifacts.ScopesCsv, TopCount, TopScopes, ParseError))
				{
					sol::table ScopesTable = LuaView.create_table();
					for (int32 i = 0; i < TopScopes.Num(); ++i)
					{
						const FTopScopeRow& Row = TopScopes[i];
						sol::table ScopeRow = LuaView.create_table();
						ScopeRow["name"] = TCHAR_TO_UTF8(*Row.Name);
						ScopeRow["count"] = (int64)Row.Count;
						ScopeRow["total_s"] = Row.TotalDurationSeconds;
						ScopeRow["mean_s"] = Row.MeanDurationSeconds;
						ScopeRow["max_s"] = Row.MaxDurationSeconds;
						ScopesTable[i + 1] = ScopeRow;
					}
					Result["top_scopes"] = ScopesTable;
				}
				else
				{
					Result["warning"] = TCHAR_TO_UTF8(*FString::Printf(TEXT("Failed to parse scopes preview: %s"), *ParseError));
				}
			}

			Session.Log(FString::Printf(TEXT("profile('analyze', trace=%s)"), *ArtifactPath));
			return Result;
		}

		// ── unknown verb ──
		Session.Log(FString::Printf(TEXT("profile: unknown verb '%s'"), *Verb));
		return sol::make_object(LuaView,
			"error: unknown verb '" + std::string(TCHAR_TO_UTF8(*Verb)) +
			"'. Valid: start, stop, status, analyze, frame_stats, memory, snapshot, list_channels, list_presets, help");
	});

	// ── help accessor ──
	Lua.set_function("_profile_help", []() -> std::string
	{
		return TCHAR_TO_UTF8(Profile_HelpText);
	});
});
