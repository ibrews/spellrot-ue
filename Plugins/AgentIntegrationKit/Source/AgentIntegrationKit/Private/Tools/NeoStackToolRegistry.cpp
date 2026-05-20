// Copyright 2025 Betide Studio. All Rights Reserved.

#include "Tools/NeoStackToolRegistry.h"
#include "AgentIntegrationKitModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Async/Async.h"

// Analytics & Crash Reporting
#include "AIKAnalytics.h"
#include "AIKCrashReporter.h"

// Safety net: transaction rollback, blueprint validation, crash protection
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"

// Tool headers — only the Lua executor remains as an MCP tool
#include "Tools/ExecuteNeoStackLuaTool.h"

FNeoStackToolRegistry& FNeoStackToolRegistry::Get()
{
	static FNeoStackToolRegistry Instance;
	return Instance;
}

FNeoStackToolRegistry::FNeoStackToolRegistry()
{
	RegisterBuiltInTools();
}

void FNeoStackToolRegistry::RegisterBuiltInTools()
{
	RegisterNeoStackLuaTool();
	// execute_python, screenshot, generate_image, generate_3d_model are now Lua bindings
	// (LuaBinding_ExecutePython, LuaBinding_Screenshot, LuaBinding_GenerateImage, LuaBinding_Generate3DModel)

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] Tool registry initialized with %d tools"), Tools.Num());
}

void FNeoStackToolRegistry::RegisterNeoStackLuaTool()
{
	Register(MakeShared<FExecuteNeoStackLuaTool>());
}

void FNeoStackToolRegistry::Register(TSharedPtr<FNeoStackToolBase> Tool)
{
	if (!Tool.IsValid())
	{
		return;
	}

	FString Name = Tool->GetName();
	if (Tools.Contains(Name))
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("[AIK] Tool '%s' already registered, overwriting"), *Name);
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] Registered tool: %s"), *Name);
	Tools.Add(Name, Tool);
}

FToolResult FNeoStackToolRegistry::Execute(const FString& ToolName, const FString& ArgsJson)
{
	// Parse JSON args
	TSharedPtr<FJsonObject> Args;

	if (!ArgsJson.IsEmpty())
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgsJson);
		if (!FJsonSerializer::Deserialize(Reader, Args) || !Args.IsValid())
		{
			return FToolResult::Fail(FString::Printf(TEXT("Failed to parse arguments for tool '%s'"), *ToolName));
		}
	}
	else
	{
		Args = MakeShared<FJsonObject>();
	}

	return Execute(ToolName, Args);
}

// Extract the target asset identifier from common argument patterns for audit logging
static FString ExtractAssetTarget(const TSharedPtr<FJsonObject>& Args)
{
	if (!Args.IsValid()) return TEXT("");

	// Try common field names tools use to identify their target asset
	FString Target;
	if (Args->TryGetStringField(TEXT("name"), Target)) return Target;
	if (Args->TryGetStringField(TEXT("asset"), Target)) return Target;
	if (Args->TryGetStringField(TEXT("path"), Target)) return Target;
	if (Args->TryGetStringField(TEXT("asset_path"), Target)) return Target;
	if (Args->TryGetStringField(TEXT("operation"), Target)) return Target;
	return TEXT("");
}

// Write a line to the tool audit log (Saved/Logs/AIK_ToolAudit.log)
static void WriteAuditLog(const FString& ToolName, const FString& Target, bool bSuccess, const FString& Summary)
{
	static FString AuditLogPath = FPaths::ProjectSavedDir() / TEXT("Logs") / TEXT("AIK_ToolAudit.log");

	FString Timestamp = FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S"));
	FString Status = bSuccess ? TEXT("OK") : TEXT("FAIL");

	// Truncate summary to keep log readable (173 chars fits a single log viewer line at 1920px)
	FString ShortSummary = Summary.Left(173);
	ShortSummary.ReplaceInline(TEXT("\n"), TEXT(" "), ESearchCase::CaseSensitive);
	ShortSummary.ReplaceInline(TEXT("\r"), TEXT(""), ESearchCase::CaseSensitive);

	FString Line = FString::Printf(TEXT("[%s] %s | %s | %s | %s\n"),
		*Timestamp, *Status, *ToolName, *Target, *ShortSummary);

	FFileHelper::SaveStringToFile(Line, *AuditLogPath, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
}

// ---------------------------------------------------------------------------
// Safety net: SEH crash protection (Windows only)
// ---------------------------------------------------------------------------
// MSVC C2712: __try cannot be used in functions that have objects requiring
// unwinding (destructors). We split into two functions:
//   1. ExecuteToolCall() — does the actual work (C++ objects OK here)
//   2. TryExecuteWithSEH() — only has POD types, wraps call in __try/__except
#if PLATFORM_WINDOWS

// EXCEPTION_EXECUTE_HANDLER is defined in <excpt.h> / <Windows.h>.
// Define directly to avoid pulling Windows headers into UE plugin code.
#ifndef EXCEPTION_EXECUTE_HANDLER
#define EXCEPTION_EXECUTE_HANDLER 1
#endif

struct FSEHToolCallContext
{
	FNeoStackToolBase* Tool;
	const TSharedPtr<FJsonObject>* ArgsPtr;
	FToolResult* ResultPtr;
};

// Separate function: C++ objects with destructors are fine here (no __try)
static void ExecuteToolCall(FSEHToolCallContext* Ctx)
{
	*Ctx->ResultPtr = Ctx->Tool->Execute(*Ctx->ArgsPtr);
}

// SEH wrapper: only POD types in scope — no C++ destructors
static bool TryExecuteWithSEH(FSEHToolCallContext* Ctx)
{
	__try
	{
		ExecuteToolCall(Ctx);
		return true;
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

#endif

// ---------------------------------------------------------------------------
// Safety net: Blueprint health validation after tool execution
// ---------------------------------------------------------------------------
// Detects corruption (broken outer chains, null GeneratedClass, invalid nodes)
// BEFORE the tool returns "success" so the transaction can be rolled back.
static bool ValidateBlueprintHealth(UBlueprint* BP, FString& OutErrors, bool bCheckGeneratedClass = true)
{
	if (!BP) { OutErrors = TEXT("null blueprint pointer"); return false; }

	// 1. GeneratedClass must exist — if null, compilation state is corrupted.
	//    Skip this check for blueprints that already had null GeneratedClass before
	//    execution (e.g. Mover/Locomotor plugin movement mode blueprints where this
	//    is the normal state, not corruption).
	if (bCheckGeneratedClass && !BP->GeneratedClass)
	{
		OutErrors = FString::Printf(TEXT("'%s' GeneratedClass is null (compilation state corrupted)"), *BP->GetName());
		return false;
	}

	// 2. All graphs must trace outer chain back to this blueprint
	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph || !IsValid(Graph)) continue;

		UBlueprint* OwnerBP = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
		if (!OwnerBP)
		{
			OutErrors = FString::Printf(TEXT("Graph '%s' in '%s' has broken outer chain (orphaned)"),
				*Graph->GetName(), *BP->GetName());
			return false;
		}
	}

	// 3. No invalid/null nodes in any graph
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph || !IsValid(Graph)) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || !IsValid(Node))
			{
				OutErrors = FString::Printf(TEXT("Graph '%s' in '%s' contains invalid/null node"),
					*Graph->GetName(), *BP->GetName());
				return false;
			}
		}
	}

	return true;
}

FToolResult FNeoStackToolRegistry::Execute(const FString& ToolName, const TSharedPtr<FJsonObject>& Args)
{
	FString Target = ExtractAssetTarget(Args);
	UE_LOG(LogAgentIntegrationKit, Log, TEXT("[AIK] Tool: %s | Target: %s"), *ToolName, *Target);

	// Set crash context so crash dumps show what AIK was doing
	FAIKCrashReporter::SetCrashContext(TEXT("CurrentTool"), ToolName);
	FAIKCrashReporter::SetCrashContext(TEXT("Status"), TEXT("ExecutingTool"));
	if (!Target.IsEmpty())
	{
		FAIKCrashReporter::SetCrashContext(TEXT("Target"), Target);
	}

	FNeoStackToolBase* Tool = GetTool(ToolName);
	if (!Tool)
	{
		FString Err = FString::Printf(TEXT("Unknown tool: %s"), *ToolName);
		WriteAuditLog(ToolName, Target, false, Err);
		FAIKCrashReporter::ClearCrashContext(TEXT("CurrentTool"));
		FAIKCrashReporter::ClearCrashContext(TEXT("Status"));
		FAIKCrashReporter::ClearCrashContext(TEXT("Target"));
		return FToolResult::Fail(Err);
	}

	// All tool operations (graph editing, asset loading, editor subsystem access) require the
	// game thread. MCP server calls this from an HTTP background thread, so dispatch there.
	// Tools that already have their own IsInGameThread() check will harmlessly pass through.
	FToolResult Result;
	bool bToolCrashed = false;

	auto DoExecute = [&Tool, &Args, &Result, &ToolName, &bToolCrashed]()
	{
		// --- 0. Snapshot blueprints that already have null GeneratedClass ---
		// Some blueprints (e.g. Mover plugin movement modes) legitimately have
		// null GeneratedClass. We must not flag these as "corrupted" after execution.
		TSet<UBlueprint*> PreExistingNullGeneratedClass;
		for (TObjectIterator<UBlueprint> It; It; ++It)
		{
			if (IsValid(*It) && !(*It)->GeneratedClass)
			{
				PreExistingNullGeneratedClass.Add(*It);
			}
		}

		// --- 1. Track packages modified during this tool execution ---
		TSet<UPackage*> ModifiedPackages;
		FDelegateHandle DirtyHandle = UPackage::PackageMarkedDirtyEvent.AddLambda(
			[&ModifiedPackages](UPackage* Pkg, bool /*bWasDirty*/)
			{
				if (Pkg) ModifiedPackages.Add(Pkg);
			});

		// --- 2. Begin transaction for rollback capability ---
		int32 TransIndex = INDEX_NONE;
		if (GEditor)
		{
			TransIndex = GEditor->BeginTransaction(
				FText::FromString(FString::Printf(TEXT("AIK: %s"), *ToolName)));
		}

		// --- 3. Execute with crash protection ---
		bool bCrashed = false;
#if PLATFORM_WINDOWS
		FSEHToolCallContext Ctx = { Tool, &Args, &Result };
		bCrashed = !TryExecuteWithSEH(&Ctx);
		if (bCrashed)
		{
			bToolCrashed = true;
			// Result may be partially corrupted (FString mid-assignment when the crash
			// occurred). Using placement-new to reconstruct it avoids calling the
			// destructor on the corrupted FString, which would itself crash.
			new (&Result) FToolResult();
			Result.bSuccess = false;
			Result.Output = FString::Printf(
				TEXT("Tool '%s' crashed (access violation caught). Changes rolled back. "
				     "This usually indicates blueprint compilation during PIE or stale engine state."),
				*ToolName);
		}
#else
		Result = Tool->Execute(Args);
#endif

		// --- 4. Unsubscribe from package dirty events ---
		UPackage::PackageMarkedDirtyEvent.Remove(DirtyHandle);

		// --- 5. Validate blueprints that were modified ---
		bool bNeedsRollback = bCrashed;
		if (!bNeedsRollback && Result.bSuccess && ModifiedPackages.Num() > 0)
		{
			for (TObjectIterator<UBlueprint> It; It; ++It)
			{
				UBlueprint* BP = *It;
				if (BP && IsValid(BP) && ModifiedPackages.Contains(BP->GetPackage()))
				{
					// Skip GeneratedClass check for blueprints that already had null
					// GeneratedClass before execution (e.g. Mover plugin movement modes).
					// Only validate graph/node health for those.
					bool bHadGeneratedClass = PreExistingNullGeneratedClass.Find(BP) == nullptr;
					FString ValidationErrors;
					if (!ValidateBlueprintHealth(BP, ValidationErrors, bHadGeneratedClass))
					{
						bNeedsRollback = true;
						Result = FToolResult::Fail(FString::Printf(
							TEXT("Blueprint corruption detected after '%s': %s. Changes rolled back."),
							*ToolName, *ValidationErrors));
						break;
					}
				}
			}
		}

		// --- 6. Rollback or commit ---
		if (GEditor && TransIndex != INDEX_NONE)
		{
			if (bNeedsRollback)
			{
				GEditor->CancelTransaction(TransIndex);
				// CancelTransaction resets ActiveCount — do NOT call EndTransaction
				UE_LOG(LogAgentIntegrationKit, Error,
					TEXT("[AIK] Transaction rolled back for tool '%s'"), *ToolName);
			}
			else
			{
				GEditor->EndTransaction();
			}
		}
	};

	if (!IsInGameThread())
	{
		FEvent* DoneEvent = FPlatformProcess::GetSynchEventFromPool();
		AsyncTask(ENamedThreads::GameThread, [&]()
		{
			DoExecute();
			DoneEvent->Trigger();
		});
		DoneEvent->Wait();
		FPlatformProcess::ReturnSynchEventToPool(DoneEvent);
	}
	else
	{
		DoExecute();
	}

	WriteAuditLog(ToolName, Target, Result.bSuccess,
		bToolCrashed ? FString::Printf(TEXT("CRASH in %s"), *ToolName) : Result.Output);

	// Analytics: track tool execution
	// Even "successful" scripts may have [FAIL] lines from individual binding calls.
	// Count them and extract all failure messages for error tracking.
	TArray<FString> FailMessages;
	int32 FailCount = 0;
	{
		int32 SearchFrom = 0;
		while (true)
		{
			int32 Pos = Result.Output.Find(TEXT("[FAIL]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
			if (Pos == INDEX_NONE) break;
			FailCount++;
			// Extract the line: from [FAIL] to the next newline
			int32 LineEnd = Result.Output.Find(TEXT("\n"), ESearchCase::IgnoreCase, ESearchDir::FromStart, Pos);
			FString FailLine = LineEnd != INDEX_NONE
				? Result.Output.Mid(Pos, LineEnd - Pos)
				: Result.Output.Mid(Pos);
			FailMessages.Add(FailLine);
			SearchFrom = Pos + 6;
		}
	}

	bool bHasErrors = !Result.bSuccess || FailCount > 0;
	FString ErrorForAnalytics = !Result.bSuccess ? Result.Output : (FailMessages.Num() > 0 ? FString::Join(FailMessages, TEXT("\n")) : TEXT(""));

	{
		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		Props->SetStringField(TEXT("tool"), ToolName);
		Props->SetBoolField(TEXT("success"), Result.bSuccess);
		Props->SetNumberField(TEXT("duration_ms"), 0.0);
		if (FailCount > 0)
		{
			Props->SetNumberField(TEXT("fail_count"), FailCount);
		}
		if (!ErrorForAnalytics.IsEmpty())
		{
			Props->SetStringField(TEXT("error"), FAIKAnalytics::SanitizeErrorForAnalytics(ErrorForAnalytics));
		}
		FAIKAnalytics::Get().RecordEvent(TEXT("tool_executed"), Props);
	}

	if (!Result.bSuccess)
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("[AIK] Tool '%s' failed: %s"), *ToolName, *Result.Output);
	}

	// Clear crash context
	FAIKCrashReporter::ClearCrashContext(TEXT("CurrentTool"));
	FAIKCrashReporter::ClearCrashContext(TEXT("Status"));
	FAIKCrashReporter::ClearCrashContext(TEXT("Target"));

	return Result;
}

bool FNeoStackToolRegistry::HasTool(const FString& ToolName) const
{
	return Tools.Contains(ToolName);
}

FNeoStackToolBase* FNeoStackToolRegistry::GetTool(const FString& ToolName) const
{
	const TSharedPtr<FNeoStackToolBase>* Found = Tools.Find(ToolName);
	return Found ? Found->Get() : nullptr;
}

TArray<FString> FNeoStackToolRegistry::GetToolNames() const
{
	TArray<FString> Names;
	Tools.GetKeys(Names);
	return Names;
}
