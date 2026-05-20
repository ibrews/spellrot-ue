// Copyright 2026 Betide Studio. All Rights Reserved.

#include "AIKCrashReporter.h"
#include "AIKAnalytics.h"
#include "ACPSettings.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformCrashContext.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IPluginManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"

DEFINE_LOG_CATEGORY_STATIC(LogAIKCrash, Log, All);

// Crash report upload endpoint
static const FString CrashReportEndpoint = TEXT("https://betide.studio/api/aik-crash-report");

// Static members
TMap<FString, FString> FAIKCrashReporter::CrashContextData;
FCriticalSection FAIKCrashReporter::ContextLock;

// Max crash history entries to keep
static constexpr int32 MaxCrashHistoryEntries = 30;

FAIKCrashReporter& FAIKCrashReporter::Get()
{
	static FAIKCrashReporter Instance;
	return Instance;
}

// ============================================
// Lifecycle
// ============================================

void FAIKCrashReporter::Initialize()
{
	if (bInitialized) return;
	bInitialized = true;

	// Set persistent crash context so crash dumps include our plugin info
	FGenericCrashContext::SetEngineData(TEXT("AIK.Loaded"), TEXT("true"));

	// Get plugin version
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AgentIntegrationKit"));
	if (Plugin.IsValid())
	{
		FGenericCrashContext::SetEngineData(TEXT("AIK.Version"), Plugin->GetDescriptor().VersionName);
	}

	// Register crash delegates
	CrashDelegateHandle = FCoreDelegates::OnHandleSystemError.AddRaw(this, &FAIKCrashReporter::OnCrash);
	EnsureDelegateHandle = FCoreDelegates::OnHandleSystemEnsure.AddRaw(this, &FAIKCrashReporter::OnEnsure);
	ShutdownErrorHandle = FCoreDelegates::OnShutdownAfterError.AddRaw(this, &FAIKCrashReporter::OnShutdownAfterError);

	// Check for crashes from previous session (deferred to allow editor to finish loading)
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([this](float) -> bool
		{
			DetectPreviousCrashes();
			return false; // One-shot
		}),
		3.0f); // 3 second delay after startup

	UE_LOG(LogAIKCrash, Log, TEXT("AIK Crash Reporter initialized"));
}

void FAIKCrashReporter::Shutdown()
{
	if (!bInitialized) return;

	FCoreDelegates::OnHandleSystemError.Remove(CrashDelegateHandle);
	FCoreDelegates::OnHandleSystemEnsure.Remove(EnsureDelegateHandle);
	FCoreDelegates::OnShutdownAfterError.Remove(ShutdownErrorHandle);

	FGenericCrashContext::SetEngineData(TEXT("AIK.Loaded"), TEXT(""));
	FGenericCrashContext::SetEngineData(TEXT("AIK.Version"), TEXT(""));
	FGenericCrashContext::SetEngineData(TEXT("AIK.Status"), TEXT(""));
	FGenericCrashContext::SetEngineData(TEXT("AIK.CurrentTool"), TEXT(""));

	bInitialized = false;
}

// ============================================
// Crash context (called from tool execution code)
// ============================================

void FAIKCrashReporter::SetCrashContext(const FString& Key, const FString& Value)
{
	{
		FScopeLock Lock(&ContextLock);
		CrashContextData.Add(Key, Value);
	}
	FGenericCrashContext::SetEngineData(*FString::Printf(TEXT("AIK.%s"), *Key), Value);
}

void FAIKCrashReporter::ClearCrashContext(const FString& Key)
{
	{
		FScopeLock Lock(&ContextLock);
		CrashContextData.Remove(Key);
	}
	FGenericCrashContext::SetEngineData(*FString::Printf(TEXT("AIK.%s"), *Key), TEXT(""));
}

// ============================================
// Crash delegate callbacks
// ============================================

void FAIKCrashReporter::OnCrash()
{
	// CRASH HANDLER — minimal work, no allocations if possible
	// Write a breadcrumb file so next launch knows AIK was active
	WriteBreadcrumb();
}

void FAIKCrashReporter::OnEnsure()
{
	// Ensures are non-fatal — safe to do normal operations
	// Record as analytics event
	FAIKAnalytics::Get().RecordEvent(TEXT("ensure_fired"));
}

void FAIKCrashReporter::OnShutdownAfterError()
{
	// Last chance before process dies — breadcrumb should already be written
}

// ============================================
// Breadcrumb (written during crash)
// ============================================

FString FAIKCrashReporter::GetBreadcrumbPath() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AIK"), TEXT("crash_breadcrumb.json"));
}

void FAIKCrashReporter::WriteBreadcrumb()
{
	// CRASH HANDLER — keep this minimal and safe
	// Build a simple JSON string manually (no TSharedPtr allocation)
	FString Json = TEXT("{\n");
	Json += FString::Printf(TEXT("  \"timestamp\": \"%s\",\n"), *FDateTime::UtcNow().ToIso8601());

	{
		FScopeLock Lock(&ContextLock);
		for (const auto& Pair : CrashContextData)
		{
			// Escape quotes in values
			FString SafeValue = Pair.Value.Replace(TEXT("\""), TEXT("\\\""));
			Json += FString::Printf(TEXT("  \"%s\": \"%s\",\n"), *Pair.Key, *SafeValue);
		}
	}

	Json += TEXT("  \"aik_active\": true\n");
	Json += TEXT("}\n");

	// Direct file write — FFileHelper should be safe in crash handlers
	FString Path = GetBreadcrumbPath();
	FString Dir = FPaths::GetPath(Path);
	IFileManager::Get().MakeDirectory(*Dir, true);
	FFileHelper::SaveStringToFile(Json, *Path);
}

// ============================================
// Crash history persistence
// ============================================

FString FAIKCrashReporter::GetCrashHistoryPath() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AIK"), TEXT("crash_history.json"));
}

TArray<FAIKCrashReporter::FCrashRecord> FAIKCrashReporter::LoadCrashHistory() const
{
	TArray<FCrashRecord> Records;
	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *GetCrashHistoryPath()))
	{
		return Records;
	}

	TSharedPtr<FJsonValue> Parsed;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
	{
		return Records;
	}

	const TArray<TSharedPtr<FJsonValue>>* ArrayPtr;
	if (!Parsed->TryGetArray(ArrayPtr)) return Records;

	for (const TSharedPtr<FJsonValue>& Item : *ArrayPtr)
	{
		const TSharedPtr<FJsonObject>* ObjPtr;
		if (!Item->TryGetObject(ObjPtr)) continue;
		const TSharedPtr<FJsonObject>& Obj = *ObjPtr;

		FCrashRecord R;
		R.CrashId = Obj->GetStringField(TEXT("crashId"));
		R.Timestamp = Obj->GetStringField(TEXT("timestamp"));
		R.ErrorMessage = Obj->GetStringField(TEXT("errorMessage"));
		R.CrashType = Obj->GetStringField(TEXT("crashType"));
		R.CallstackSummary = Obj->GetStringField(TEXT("callstackSummary"));
		R.bBasicReported = Obj->GetBoolField(TEXT("basicReported"));
		R.bFullLogSent = Obj->GetBoolField(TEXT("fullLogSent"));
		R.bFullLogDeclined = Obj->GetBoolField(TEXT("fullLogDeclined"));
		R.bManuallyReported = Obj->GetBoolField(TEXT("manuallyReported"));
		Records.Add(MoveTemp(R));
	}

	return Records;
}

void FAIKCrashReporter::SaveCrashHistory(const TArray<FCrashRecord>& Records) const
{
	TArray<TSharedPtr<FJsonValue>> Array;
	for (const FCrashRecord& R : Records)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("crashId"), R.CrashId);
		Obj->SetStringField(TEXT("timestamp"), R.Timestamp);
		Obj->SetStringField(TEXT("errorMessage"), R.ErrorMessage);
		Obj->SetStringField(TEXT("crashType"), R.CrashType);
		Obj->SetStringField(TEXT("callstackSummary"), R.CallstackSummary);
		Obj->SetBoolField(TEXT("basicReported"), R.bBasicReported);
		Obj->SetBoolField(TEXT("fullLogSent"), R.bFullLogSent);
		Obj->SetBoolField(TEXT("fullLogDeclined"), R.bFullLogDeclined);
		Obj->SetBoolField(TEXT("manuallyReported"), R.bManuallyReported);
		Array.Add(MakeShared<FJsonValueObject>(Obj));
	}

	FString JsonString;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonString);

	FJsonSerializer::Serialize(Array, Writer);

	FString Path = GetCrashHistoryPath();
	FString Dir = FPaths::GetPath(Path);
	IFileManager::Get().MakeDirectory(*Dir, true);
	FFileHelper::SaveStringToFile(JsonString, *Path);
}

FString FAIKCrashReporter::GetCrashHistoryJson() const
{
	FString JsonString;
	if (FFileHelper::LoadFileToString(JsonString, *GetCrashHistoryPath()))
	{
		return JsonString;
	}
	return TEXT("[]");
}

// ============================================
// Crash detection on startup
// ============================================

void FAIKCrashReporter::DetectPreviousCrashes()
{
	// Check for breadcrumb first
	FString BreadcrumbPath = GetBreadcrumbPath();
	bool bBreadcrumbExists = FPaths::FileExists(BreadcrumbPath);

	// Scan crash directories — location differs by platform
	// Windows: <Project>/Saved/Crashes/
	// Mac: ~/Library/Application Support/Epic/UnrealEngine/<Version>/Saved/Crashes/
	TArray<FString> CrashDirs;
	CrashDirs.Add(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Crashes")));

#if PLATFORM_MAC
	// Mac stores crashes in ~/Library/Application Support/Epic/UnrealEngine/<Version>/Saved/Crashes/
	CrashDirs.Add(FPaths::Combine(FPaths::EngineUserDir(), TEXT("Saved"), TEXT("Crashes")));
#endif

	bool bAnyCrashDirExists = false;
	for (const FString& Dir : CrashDirs)
	{
		if (FPaths::DirectoryExists(Dir)) bAnyCrashDirExists = true;
	}
	if (!bAnyCrashDirExists && !bBreadcrumbExists)
	{
		return;
	}

	TArray<FCrashRecord> History = LoadCrashHistory();

	// Build set of known crash IDs
	TSet<FString> KnownCrashIds;
	for (const FCrashRecord& R : History)
	{
		KnownCrashIds.Add(R.CrashId);
	}

	// Find crash folders from all directories
	TArray<TPair<FString, FString>> CrashFoldersWithPaths; // FolderName -> FullPath
	for (const FString& CrashDir : CrashDirs)
	{
		if (!FPaths::DirectoryExists(CrashDir)) continue;
		TArray<FString> Folders;
		IFileManager::Get().FindFiles(Folders, *(CrashDir / TEXT("*")), false, true);
		for (const FString& F : Folders)
		{
			CrashFoldersWithPaths.Add(TPair<FString, FString>(F, CrashDir / F));
		}
	}

	// Collect new crashes that need prompting
	TArray<FPendingCrash> PendingPrompts;

	const UACPSettings* Settings = UACPSettings::Get();

	for (const TPair<FString, FString>& Entry : CrashFoldersWithPaths)
	{
		const FString& FolderName = Entry.Key;
		const FString& CrashFolderPath = Entry.Value;
		if (KnownCrashIds.Contains(FolderName)) continue;

		// Skip ensure reports — only prompt for actual crashes
		if (FolderName.StartsWith(TEXT("EnsureReport"))) continue;

		FString XmlPath = CrashFolderPath / TEXT("CrashContext.runtime-xml");
		if (!FPaths::FileExists(XmlPath)) continue;

		FString ErrorMessage, CrashType, Callstack;
		bool bAIKInCallstack = false;

		if (!ParseCrashContextXml(XmlPath, ErrorMessage, CrashType, Callstack, bAIKInCallstack))
		{
			continue;
		}

		// Only track crashes where AIK is in the callstack OR breadcrumb was written
		if (!bAIKInCallstack && !bBreadcrumbExists)
		{
			continue;
		}

		FCrashRecord Record;
		Record.CrashId = FolderName;
		Record.Timestamp = FDateTime::UtcNow().ToIso8601();
		Record.ErrorMessage = ErrorMessage.Left(500);
		Record.CrashType = CrashType;
		Record.CallstackSummary = Callstack.Left(1000);

		// Auto-send basic report if enabled
		if (Settings && Settings->bEnableAnalytics && Settings->bEnableCrashReporting)
		{
			SendBasicCrashReport(Record);
			Record.bBasicReported = true;
		}

		// Full log handling
		if (Settings && Settings->bAlwaysSendCrashLogs)
		{
			FString LogFile = FindLogFileForCrash(CrashFolderPath);
			if (!LogFile.IsEmpty())
			{
				SendFullCrashReport(Record.CrashId, LogFile);
				Record.bFullLogSent = true;
			}
			History.Add(MoveTemp(Record));
		}
		else if (Settings && Settings->bEnableCrashReporting)
		{
			// Queue for batched prompt
			FPendingCrash Pending;
			Pending.Record = Record;
			Pending.FolderPath = CrashFolderPath;
			PendingPrompts.Add(MoveTemp(Pending));
			History.Add(Record);
		}
		else
		{
			History.Add(MoveTemp(Record));
		}
	}

	// Show ONE batched prompt for all pending crashes
	if (PendingPrompts.Num() > 0)
	{
		SaveCrashHistory(History);

		// Capture data for the lambda
		TArray<FPendingCrash> CapturedPending = MoveTemp(PendingPrompts);
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([this, CapturedPending](float) -> bool
			{
				ShowBatchCrashPrompt(CapturedPending);
				return false;
			}),
			1.0f);
	}

	// Trim history to max entries
	while (History.Num() > MaxCrashHistoryEntries)
	{
		History.RemoveAt(0);
	}

	SaveCrashHistory(History);

	// Clean up breadcrumb
	if (bBreadcrumbExists)
	{
		IFileManager::Get().Delete(*BreadcrumbPath);
	}
}

// ============================================
// XML parsing
// ============================================

bool FAIKCrashReporter::ParseCrashContextXml(const FString& XmlPath, FString& OutErrorMessage,
	FString& OutCrashType, FString& OutCallstack, bool& bOutAIKInCallstack)
{
	FString XmlContent;
	if (!FFileHelper::LoadFileToString(XmlContent, *XmlPath))
	{
		return false;
	}

	// Simple tag extraction (crash context XML is well-formed, no need for full parser)
	auto ExtractTag = [&XmlContent](const FString& TagName) -> FString
	{
		FString OpenTag = FString::Printf(TEXT("<%s>"), *TagName);
		FString CloseTag = FString::Printf(TEXT("</%s>"), *TagName);
		int32 Start = XmlContent.Find(OpenTag);
		if (Start == INDEX_NONE) return FString();
		Start += OpenTag.Len();
		int32 End = XmlContent.Find(CloseTag, ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
		if (End == INDEX_NONE) return FString();
		return XmlContent.Mid(Start, End - Start).TrimStartAndEnd();
	};

	OutErrorMessage = ExtractTag(TEXT("ErrorMessage"));
	OutCrashType = ExtractTag(TEXT("CrashType"));
	OutCallstack = ExtractTag(TEXT("PCallStack"));

	// Check if our module appears in the portable callstack
	bOutAIKInCallstack = OutCallstack.Contains(TEXT("AgentIntegrationKit"));

	// Also check EngineData for our breadcrumb context
	if (!bOutAIKInCallstack)
	{
		FString EngineData = ExtractTag(TEXT("EngineData"));
		if (EngineData.Contains(TEXT("AIK.Loaded")))
		{
			bOutAIKInCallstack = true;
		}
	}

	return !OutErrorMessage.IsEmpty() || !OutCrashType.IsEmpty();
}

// ============================================
// Log file discovery
// ============================================

FString FAIKCrashReporter::FindLogFileForCrash(const FString& CrashFolderPath) const
{
	// Crash folder may contain a copy of the log
	FString CrashLog = CrashFolderPath / TEXT("Saved.log");
	if (FPaths::FileExists(CrashLog))
	{
		return CrashLog;
	}

	// Fall back to the main project log
	FString ProjectLog = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"),
		FString(FApp::GetProjectName()) + TEXT(".log"));
	if (FPaths::FileExists(ProjectLog))
	{
		return ProjectLog;
	}

	return FString();
}

// ============================================
// Reporting
// ============================================

void FAIKCrashReporter::SendBasicCrashReport(const FCrashRecord& Record)
{
	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("crash_type"), Record.CrashType);
	Props->SetStringField(TEXT("error_message"), FAIKAnalytics::SanitizeErrorForAnalytics(Record.ErrorMessage));
	Props->SetStringField(TEXT("callstack_summary"), FAIKAnalytics::SanitizeErrorForAnalytics(Record.CallstackSummary));
	Props->SetStringField(TEXT("crash_id"), Record.CrashId);

	FAIKAnalytics::Get().RecordEvent(TEXT("engine_crash"), Props);

	UE_LOG(LogAIKCrash, Log, TEXT("Basic crash report sent for %s"), *Record.CrashId);
}

void FAIKCrashReporter::SendFullCrashReport(const FString& CrashId, const FString& LogFilePath)
{
	// Read log file as raw bytes
	TArray<uint8> LogBytes;
	if (!FFileHelper::LoadFileToArray(LogBytes, *LogFilePath))
	{
		UE_LOG(LogAIKCrash, Warning, TEXT("Failed to read log file for crash report: %s"), *LogFilePath);
		return;
	}

	UE_LOG(LogAIKCrash, Log, TEXT("Uploading crash log for %s (%d bytes)"), *CrashId, LogBytes.Num());

	// Step 1: Request an upload URL from our API
	FString UploadUrlEndpoint = CrashReportEndpoint + TEXT("/upload-url?crashId=") + CrashId;

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> UrlRequest = FHttpModule::Get().CreateRequest();
	UrlRequest->SetURL(UploadUrlEndpoint);
	UrlRequest->SetVerb(TEXT("GET"));

	// Capture log bytes for the upload step
	TSharedPtr<TArray<uint8>> LogBytesPtr = MakeShared<TArray<uint8>>(MoveTemp(LogBytes));

	UrlRequest->OnProcessRequestComplete().BindLambda(
		[this, CrashId, LogBytesPtr](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
		{
			if (!bConnectedSuccessfully || !Response.IsValid() || Response->GetResponseCode() != 200)
			{
				UE_LOG(LogAIKCrash, Warning, TEXT("Failed to get upload token for %s (code: %d)"),
					*CrashId, Response.IsValid() ? Response->GetResponseCode() : 0);
				return;
			}

			// Parse client token and upload URL from response
			TSharedPtr<FJsonObject> JsonResponse;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
			if (!FJsonSerializer::Deserialize(Reader, JsonResponse) || !JsonResponse.IsValid())
			{
				UE_LOG(LogAIKCrash, Warning, TEXT("Failed to parse upload token response for %s"), *CrashId);
				return;
			}

			FString ClientToken = JsonResponse->GetStringField(TEXT("clientToken"));
			FString UploadUrl = JsonResponse->GetStringField(TEXT("uploadUrl"));
			if (ClientToken.IsEmpty() || UploadUrl.IsEmpty())
			{
				UE_LOG(LogAIKCrash, Warning, TEXT("Empty upload token/URL for %s"), *CrashId);
				return;
			}

			// Step 2: Upload log file directly to Vercel Blob (bypasses serverless function)
			TSharedRef<IHttpRequest, ESPMode::ThreadSafe> UploadRequest = FHttpModule::Get().CreateRequest();
			UploadRequest->SetURL(UploadUrl);
			UploadRequest->SetVerb(TEXT("PUT"));
			UploadRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ClientToken));
			UploadRequest->SetHeader(TEXT("Content-Type"), TEXT("text/plain"));
			UploadRequest->SetHeader(TEXT("x-vercel-blob-access"), TEXT("private"));
			UploadRequest->SetHeader(TEXT("x-api-version"), TEXT("12"));
			UploadRequest->SetHeader(TEXT("x-add-random-suffix"), TEXT("1"));
			UploadRequest->SetHeader(TEXT("x-content-type"), TEXT("text/plain"));
			UploadRequest->SetContent(*LogBytesPtr);

			UE_LOG(LogAIKCrash, Log, TEXT("Uploading %d bytes directly to Vercel Blob for %s"), LogBytesPtr->Num(), *CrashId);

			UploadRequest->OnProcessRequestComplete().BindLambda(
				[this, CrashId](FHttpRequestPtr UpReq, FHttpResponsePtr UpResp, bool bUpConnected)
				{
					if (!bUpConnected || !UpResp.IsValid() || UpResp->GetResponseCode() != 200)
					{
						UE_LOG(LogAIKCrash, Warning, TEXT("Failed to upload log to Blob for %s (code: %d, body: %s)"),
							*CrashId,
							UpResp.IsValid() ? UpResp->GetResponseCode() : 0,
							UpResp.IsValid() ? *UpResp->GetContentAsString().Left(200) : TEXT("no response"));
						return;
					}

					// Parse blob URL from Vercel Blob response
					TSharedPtr<FJsonObject> BlobResponse;
					TSharedRef<TJsonReader<>> BlobReader = TJsonReaderFactory<>::Create(UpResp->GetContentAsString());
					if (!FJsonSerializer::Deserialize(BlobReader, BlobResponse) || !BlobResponse.IsValid())
					{
						UE_LOG(LogAIKCrash, Warning, TEXT("Failed to parse blob response for %s"), *CrashId);
						return;
					}

					FString BlobUrl = BlobResponse->GetStringField(TEXT("url"));

					// Step 3: Send metadata + blob URL to our API (tiny JSON, no log content)
					TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
					Payload->SetStringField(TEXT("installId"), FAIKAnalytics::Get().GetInstallId());
					Payload->SetStringField(TEXT("crashId"), CrashId);
					Payload->SetStringField(TEXT("logBlobUrl"), BlobUrl);

					FString PluginVersion = TEXT("unknown");
					TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AgentIntegrationKit"));
					if (Plugin.IsValid())
					{
						PluginVersion = Plugin->GetDescriptor().VersionName;
					}
					Payload->SetStringField(TEXT("pluginVersion"), PluginVersion);
					Payload->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString(EVersionComponent::Minor));

				#if PLATFORM_WINDOWS
					Payload->SetStringField(TEXT("platform"), TEXT("Win64"));
				#elif PLATFORM_MAC
					Payload->SetStringField(TEXT("platform"), TEXT("Mac"));
				#elif PLATFORM_LINUX
					Payload->SetStringField(TEXT("platform"), TEXT("Linux"));
				#else
					Payload->SetStringField(TEXT("platform"), TEXT("Other"));
				#endif

					FString PayloadString;
					TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&PayloadString);
					FJsonSerializer::Serialize(Payload.ToSharedRef(), Writer);

					TSharedRef<IHttpRequest, ESPMode::ThreadSafe> MetaRequest = FHttpModule::Get().CreateRequest();
					MetaRequest->SetURL(CrashReportEndpoint);
					MetaRequest->SetVerb(TEXT("POST"));
					MetaRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
					MetaRequest->SetContentAsString(PayloadString);

					MetaRequest->OnProcessRequestComplete().BindLambda(
						[CrashId](FHttpRequestPtr, FHttpResponsePtr MetaResp, bool bMetaConnected)
						{
							if (bMetaConnected && MetaResp.IsValid() && MetaResp->GetResponseCode() == 200)
							{
								UE_LOG(LogAIKCrash, Log, TEXT("Full crash report sent successfully for %s"), *CrashId);
							}
							else
							{
								UE_LOG(LogAIKCrash, Warning, TEXT("Failed to send crash metadata for %s (code: %d)"),
									*CrashId, MetaResp.IsValid() ? MetaResp->GetResponseCode() : 0);
							}
						});

					MetaRequest->ProcessRequest();
				});

			UploadRequest->ProcessRequest();
		});

	UrlRequest->ProcessRequest();
}

// ============================================
// Manual report from Web UI
// ============================================

bool FAIKCrashReporter::ManuallyReportCrash(const FString& CrashId)
{
	TArray<FCrashRecord> History = LoadCrashHistory();
	FCrashRecord* Found = History.FindByPredicate([&](const FCrashRecord& R) { return R.CrashId == CrashId; });
	if (!Found)
	{
		return false;
	}

	// Find the crash folder
	FString CrashFolderPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Crashes"), CrashId);
	FString LogFile = FindLogFileForCrash(CrashFolderPath);

	if (LogFile.IsEmpty())
	{
		// No log file available — send basic report if not already sent
		if (!Found->bBasicReported)
		{
			SendBasicCrashReport(*Found);
			Found->bBasicReported = true;
		}
	}
	else
	{
		SendFullCrashReport(CrashId, LogFile);
		Found->bFullLogSent = true;
	}

	Found->bManuallyReported = true;
	Found->bFullLogDeclined = false;
	SaveCrashHistory(History);
	return true;
}

// ============================================
// Batched crash prompt dialog
// ============================================

void FAIKCrashReporter::ShowBatchCrashPrompt(const TArray<FPendingCrash>& PendingCrashes)
{
	if (!FSlateApplication::IsInitialized() || PendingCrashes.Num() == 0) return;

	// Collect crash IDs and find log files
	TArray<TPair<FString, FString>> CrashIdsAndLogs; // CrashId -> LogFile
	for (const FPendingCrash& P : PendingCrashes)
	{
		FString LogFile = FindLogFileForCrash(P.FolderPath);
		if (!LogFile.IsEmpty())
		{
			CrashIdsAndLogs.Add(TPair<FString, FString>(P.Record.CrashId, LogFile));
		}
	}

	if (CrashIdsAndLogs.Num() == 0) return;

	// Build summary text
	FString TitleText = CrashIdsAndLogs.Num() == 1
		? TEXT("Agent Integration Kit detected a crash from your last session.")
		: FString::Printf(TEXT("Agent Integration Kit detected %d crashes from previous sessions."), CrashIdsAndLogs.Num());

	// Show the most recent error as preview
	FString ErrorPreview = PendingCrashes.Last().Record.ErrorMessage.Left(200);

	TSharedRef<SWindow> PromptWindow = SNew(SWindow)
		.Title(FText::FromString(TEXT("AIK Crash Report")))
		.ClientSize(FVector2D(520, 280))
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		.IsTopmostWindow(true)
		.SizingRule(ESizingRule::FixedSize);

	TWeakPtr<SWindow> WeakWindow = PromptWindow;

	PromptWindow->SetContent(
		SNew(SBox)
		.Padding(FMargin(20))
		[
			SNew(SVerticalBox)

			// Title
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 12)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TitleText))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
				.AutoWrapText(true)
			]

			// Error preview
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 16)
			[
				SNew(SBox)
				.Padding(FMargin(8))
				[
					SNew(STextBlock)
					.Text(FText::FromString(ErrorPreview))
					.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.4f, 0.4f)))
				]
			]

			// Description
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 20)
			[
				SNew(STextBlock)
				.Text(FText::FromString(
					CrashIdsAndLogs.Num() == 1
						? TEXT("Sending the full editor log helps us diagnose and fix this issue faster.")
						: FString::Printf(TEXT("Send full editor logs for all %d crashes? This helps us diagnose and fix issues faster."), CrashIdsAndLogs.Num())))
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor(FLinearColor(0.7f, 0.7f, 0.7f)))
			]

			// Buttons
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Right)
			[
				SNew(SHorizontalBox)

				// Don't Send
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0, 0, 8, 0)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Don't Send")))
					.OnClicked_Lambda([this, CrashIdsAndLogs, WeakWindow]() -> FReply
					{
						TArray<FCrashRecord> H = LoadCrashHistory();
						for (FCrashRecord& R : H)
						{
							for (const auto& Pair : CrashIdsAndLogs)
							{
								if (R.CrashId == Pair.Key)
								{
									R.bFullLogDeclined = true;
								}
							}
						}
						SaveCrashHistory(H);

						if (TSharedPtr<SWindow> Win = WeakWindow.Pin())
						{
							Win->RequestDestroyWindow();
						}
						return FReply::Handled();
					})
				]

				// Always Send
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0, 0, 8, 0)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Always Send")))
					.OnClicked_Lambda([this, CrashIdsAndLogs, WeakWindow]() -> FReply
					{
						UACPSettings* S = GetMutableDefault<UACPSettings>();
						if (S)
						{
							S->bAlwaysSendCrashLogs = true;
							S->SaveConfig();
						}

						TArray<FCrashRecord> H = LoadCrashHistory();
						for (const auto& Pair : CrashIdsAndLogs)
						{
							SendFullCrashReport(Pair.Key, Pair.Value);
							for (FCrashRecord& R : H)
							{
								if (R.CrashId == Pair.Key)
								{
									R.bFullLogSent = true;
								}
							}
						}
						SaveCrashHistory(H);

						if (TSharedPtr<SWindow> Win = WeakWindow.Pin())
						{
							Win->RequestDestroyWindow();
						}
						return FReply::Handled();
					})
				]

				// Send
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(FText::FromString(
						CrashIdsAndLogs.Num() == 1
							? TEXT("Send Log")
							: FString::Printf(TEXT("Send All (%d)"), CrashIdsAndLogs.Num())))
					.ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("PrimaryButton"))
					.OnClicked_Lambda([this, CrashIdsAndLogs, WeakWindow]() -> FReply
					{
						TArray<FCrashRecord> H = LoadCrashHistory();
						for (const auto& Pair : CrashIdsAndLogs)
						{
							SendFullCrashReport(Pair.Key, Pair.Value);
							for (FCrashRecord& R : H)
							{
								if (R.CrashId == Pair.Key)
								{
									R.bFullLogSent = true;
								}
							}
						}
						SaveCrashHistory(H);

						if (TSharedPtr<SWindow> Win = WeakWindow.Pin())
						{
							Win->RequestDestroyWindow();
						}
						return FReply::Handled();
					})
				]
			]
		]
	);

	FSlateApplication::Get().AddWindow(PromptWindow);
}
