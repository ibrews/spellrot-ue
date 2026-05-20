// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Crash detection and reporting for AgentIntegrationKit.
 *
 * Detects crashes caused by the plugin and reports them:
 * - Basic crash data (error, callstack summary) sent automatically if analytics are on
 * - Full editor log sent only with explicit user consent (prompt or Always Send setting)
 * - Crash history stored locally in crash_history.json for Web UI viewing
 *
 * Flow:
 * 1. OnHandleSystemError: write breadcrumb file with AIK state
 * 2. On next launch: scan Saved/Crashes/, detect AIK in callstack
 * 3. Auto-send basic report if enabled
 * 4. Show Slate prompt for full log (unless Always Send is on)
 * 5. User can also send reports manually from Web UI
 */
class AGENTINTEGRATIONKIT_API FAIKCrashReporter
{
public:
	static FAIKCrashReporter& Get();

	/** Call from StartupModule after analytics init. Registers crash delegates and checks for previous crashes. */
	void Initialize();

	/** Call from ShutdownModule. Unregisters delegates. */
	void Shutdown();

	/** Get crash history as JSON string (for Web UI) */
	FString GetCrashHistoryJson() const;

	/** Manually send a crash report for a previously declined crash (from Web UI) */
	bool ManuallyReportCrash(const FString& CrashId);

	/** Update AIK crash context (call before risky operations like Lua execution) */
	static void SetCrashContext(const FString& Key, const FString& Value);

	/** Clear AIK crash context key */
	static void ClearCrashContext(const FString& Key);

private:
	FAIKCrashReporter() = default;

	// --- Crash delegate callbacks ---
	void OnCrash();
	void OnEnsure();
	void OnShutdownAfterError();

	// --- Crash detection on startup ---
	void DetectPreviousCrashes();
	bool ParseCrashContextXml(const FString& XmlPath, FString& OutErrorMessage, FString& OutCrashType,
		FString& OutCallstack, bool& bOutAIKInCallstack);

	// --- Breadcrumb file ---
	void WriteBreadcrumb();
	FString GetBreadcrumbPath() const;

	// --- Crash history ---
	struct FCrashRecord
	{
		FString CrashId;           // UECC-GUID folder name
		FString Timestamp;         // ISO 8601
		FString ErrorMessage;      // Sanitized error
		FString CrashType;         // Crash, Assert, Ensure, etc.
		FString CallstackSummary;  // First few AIK frames
		bool bBasicReported = false;
		bool bFullLogSent = false;
		bool bFullLogDeclined = false;
		bool bManuallyReported = false;
	};

	FString GetCrashHistoryPath() const;
	TArray<FCrashRecord> LoadCrashHistory() const;
	void SaveCrashHistory(const TArray<FCrashRecord>& Records) const;

	// --- Reporting ---
	void SendBasicCrashReport(const FCrashRecord& Record);
	void SendFullCrashReport(const FString& CrashId, const FString& LogFilePath);
	FString FindLogFileForCrash(const FString& CrashFolderPath) const;

	// --- Prompt ---
	struct FPendingCrash
	{
		FCrashRecord Record;
		FString FolderPath;
	};
	void ShowBatchCrashPrompt(const TArray<FPendingCrash>& PendingCrashes);

	// --- State ---
	bool bInitialized = false;
	FDelegateHandle CrashDelegateHandle;
	FDelegateHandle EnsureDelegateHandle;
	FDelegateHandle ShutdownErrorHandle;

	/** Current AIK operation context — written into breadcrumb on crash */
	static TMap<FString, FString> CrashContextData;
	static FCriticalSection ContextLock;
};
