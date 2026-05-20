// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "ACPClient.h"
#include "AgentIntegrationKitModule.h"
#include "ACPSettings.h"
#include "AIKAnalytics.h"
#include "ACPSessionManager.h"
#include "MCPServer.h"
#include "ACPAttachmentManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Async/Async.h"
#include "Misc/Guid.h"

static void AddPathSegmentIfValid(const FString& Segment, TArray<FString>& InOutSegments)
{
	if (Segment.IsEmpty())
	{
		return;
	}

	FString Normalized = Segment;
	FPaths::NormalizeDirectoryName(Normalized);
	if (Normalized.IsEmpty())
	{
		return;
	}

	for (const FString& Existing : InOutSegments)
	{
#if PLATFORM_WINDOWS
		if (Existing.Equals(Normalized, ESearchCase::IgnoreCase))
#else
		if (Existing.Equals(Normalized, ESearchCase::CaseSensitive))
#endif
		{
			return;
		}
	}

	InOutSegments.Add(Normalized);
}

static FString BuildAugmentedPathForChildProcess(const FString& BasePath, const FString& ExecutablePath)
{
	const FString PathSeparator =
#if PLATFORM_WINDOWS
		TEXT(";");
#else
		TEXT(":");
#endif

	TArray<FString> Segments;
	BasePath.ParseIntoArray(Segments, *PathSeparator, true);

	// Ensure the resolved executable directory is in PATH. This is critical for npm-installed
	// wrappers that execute `env node` (Copilot/Gemini) when Unreal launches with a limited PATH.
	if (!ExecutablePath.IsEmpty() && FPaths::IsRelative(ExecutablePath) == false)
	{
		AddPathSegmentIfValid(FPaths::GetPath(ExecutablePath), Segments);
	}

#if !PLATFORM_WINDOWS
	AddPathSegmentIfValid(TEXT("/opt/homebrew/bin"), Segments);
	AddPathSegmentIfValid(TEXT("/usr/local/bin"), Segments);

	const FString HomeDir = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
	if (!HomeDir.IsEmpty())
	{
		AddPathSegmentIfValid(FPaths::Combine(HomeDir, TEXT("bin")), Segments);
		AddPathSegmentIfValid(FPaths::Combine(HomeDir, TEXT(".bun/bin")), Segments);
		AddPathSegmentIfValid(FPaths::Combine(HomeDir, TEXT(".local/bin")), Segments);
		AddPathSegmentIfValid(FPaths::Combine(HomeDir, TEXT(".opencode/bin")), Segments);
	}
#endif

	FString Result;
	for (const FString& Segment : Segments)
	{
		if (Segment.IsEmpty())
		{
			continue;
		}

		if (!Result.IsEmpty())
		{
			Result += PathSeparator;
		}
		Result += Segment;
	}

	return Result;
}

struct FEnvVarRestore
{
	FString Name;
	FString PreviousValue;
};

static void ApplyTemporaryEnvVar(
	const FString& Name,
	const FString& Value,
	TArray<FEnvVarRestore>& OutRestoreList)
{
	FEnvVarRestore Backup;
	Backup.Name = Name;
	Backup.PreviousValue = FPlatformMisc::GetEnvironmentVariable(*Name);
	OutRestoreList.Add(Backup);
	FPlatformMisc::SetEnvironmentVar(*Name, *Value);
}

static void RestoreTemporaryEnvVars(const TArray<FEnvVarRestore>& RestoreList)
{
	for (int32 Index = RestoreList.Num() - 1; Index >= 0; --Index)
	{
		const FEnvVarRestore& Backup = RestoreList[Index];
		FPlatformMisc::SetEnvironmentVar(*Backup.Name, *Backup.PreviousValue);
	}
}

static FString BuildCopilotAdditionalMcpConfigFile(const int32 MCPPort)
{
	const FString TempDir = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("aik-copilot-mcp"));
	IFileManager::Get().MakeDirectory(*TempDir, true);

	const FString FilePath = FPaths::Combine(
		TempDir,
		FString::Printf(TEXT("mcp-%s.json"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

	TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> McpServers = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> UnrealEntry = MakeShared<FJsonObject>();

	UnrealEntry->SetStringField(TEXT("type"), TEXT("http"));
	UnrealEntry->SetStringField(TEXT("url"), FString::Printf(TEXT("http://127.0.0.1:%d/mcp"), MCPPort));
	TArray<TSharedPtr<FJsonValue>> AllTools;
	AllTools.Add(MakeShared<FJsonValueString>(TEXT("*")));
	UnrealEntry->SetArrayField(TEXT("tools"), AllTools);

	McpServers->SetObjectField(TEXT("unreal-editor"), UnrealEntry);
	RootObj->SetObjectField(TEXT("mcpServers"), McpServers);

	FString OutputString;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
	FJsonSerializer::Serialize(RootObj.ToSharedRef(), Writer);

	if (FFileHelper::SaveStringToFile(OutputString, *FilePath))
	{
		return FilePath;
	}

	return FString();
}

static FString BuildGeminiSystemSettingsFile(const int32 MCPPort)
{
	const FString TempDir = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("aik-gemini-mcp"));
	IFileManager::Get().MakeDirectory(*TempDir, true);

	const FString FilePath = FPaths::Combine(
		TempDir,
		FString::Printf(TEXT("settings-%s.json"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

	TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> McpServers = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> UnrealEntry = MakeShared<FJsonObject>();

	// Gemini supports httpUrl for streamable HTTP MCP servers.
	UnrealEntry->SetStringField(TEXT("httpUrl"), FString::Printf(TEXT("http://127.0.0.1:%d/mcp"), MCPPort));
	McpServers->SetObjectField(TEXT("unreal-editor"), UnrealEntry);
	RootObj->SetObjectField(TEXT("mcpServers"), McpServers);

	FString OutputString;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
	FJsonSerializer::Serialize(RootObj.ToSharedRef(), Writer);

	if (FFileHelper::SaveStringToFile(OutputString, *FilePath))
	{
		return FilePath;
	}

	return FString();
}

static FCriticalSection GAgentProcessLaunchEnvLock;

// Helper to strip ANSI escape sequences from text (terminal color codes, etc.)
static FString StripAnsiCodes(const FString& Input)
{
	FString Result;
	Result.Reserve(Input.Len());

	bool bInEscape = false;
	for (int32 i = 0; i < Input.Len(); ++i)
	{
		TCHAR C = Input[i];

		if (C == 0x1B) // ESC character
		{
			bInEscape = true;
			continue;
		}

		if (bInEscape)
		{
			// ANSI escape sequences end with a letter (a-zA-Z)
			if ((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z'))
			{
				bInEscape = false;
			}
			continue;
		}

		Result.AppendChar(C);
	}

	return Result;
}

FACPClient::FACPClient()
	: bStopRequested(false)
{
	// Set default client capabilities (ACP spec v1)
	ClientCapabilities.bSupportsFileSystem = true;
	ClientCapabilities.bSupportsTerminal = false;
	ClientCapabilities.bSupportsAuthTerminal = true;
}

FACPClient::~FACPClient()
{
	Disconnect();
}

bool FACPClient::Connect(const FACPAgentConfig& Config)
{
	if (IsConnected())
	{
		Disconnect();
	}

	// Clean up any stale per-process MCP config files from a previous run.
	if (!CopilotAdditionalMcpConfigPath.IsEmpty())
	{
		IFileManager::Get().Delete(*CopilotAdditionalMcpConfigPath, false, true);
		CopilotAdditionalMcpConfigPath.Empty();
	}
	if (!GeminiSystemSettingsPath.IsEmpty())
	{
		IFileManager::Get().Delete(*GeminiSystemSettingsPath, false, true);
		GeminiSystemSettingsPath.Empty();
	}

	CurrentConfig = Config;
	AvailableReasoningEfforts.Empty();
	CurrentReasoningEffort.Empty();
	ReasoningConfigOptionId.Empty();
	SetState(EACPClientState::Connecting, TEXT("Connecting to agent..."));

	// Build command line — like Zed, wrap through the system shell so that
	// .cmd/.bat files work on Windows and the user's PATH/env is inherited.
	FString ExecutablePath;
	FString Arguments;

#if PLATFORM_WINDOWS
	// Windows: cmd.exe /S /C ""executable" arg1 arg2"
	// /S strips the outermost quotes, leaving: "executable" arg1 arg2
	// The inner quotes protect paths with spaces (e.g., C:\Program Files\nodejs\npx)
	{
		FString InnerCmd = FString::Printf(TEXT("\"%s\""), *Config.ExecutablePath);
		for (const FString& Arg : Config.Arguments)
		{
			InnerCmd += TEXT(" ") + Arg;
		}
		ExecutablePath = TEXT("cmd.exe");
		Arguments = FString::Printf(TEXT("/S /C \"%s\""), *InnerCmd);
	}
#else
	// Mac/Linux: launch directly (POSIX exec handles scripts and binaries)
	ExecutablePath = Config.ExecutablePath;
	for (const FString& Arg : Config.Arguments)
	{
		Arguments += TEXT(" ") + Arg;
	}
#endif

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Launching %s: %s %s"),
		*Config.AgentName, *ExecutablePath, *Arguments);

	// Set working directory — MUST be absolute so codex-acp resolves config.cwd correctly.
	// A relative UE path (../../..) can resolve differently inside the child process,
	// causing list_threads to miss sessions created with the absolute path.
	FString WorkingDir = FPaths::ConvertRelativePathToFull(
		Config.WorkingDirectory.IsEmpty() ? FPaths::ProjectDir() : Config.WorkingDirectory);
	// Strip trailing slash for consistency
	while (WorkingDir.Len() > 1 && (WorkingDir.EndsWith(TEXT("/")) || WorkingDir.EndsWith(TEXT("\\"))))
	{
		WorkingDir.LeftChopInline(1);
	}

	// Create pipes for stdin/stdout
	// bWritePipeLocal parameter: true = ReadPipe is inheritable, false = WritePipe is inheritable
	void* StdinReadPipe = nullptr;
	void* StdoutWritePipe = nullptr;

	// Ensure executable has +x permission (Fab/zip/git can strip it on macOS/Linux)
#if PLATFORM_MAC || PLATFORM_LINUX
	{
		FString PermCheckPath = ExecutablePath;
		FPaths::NormalizeFilename(PermCheckPath);
		if (FPaths::FileExists(PermCheckPath))
		{
			FPlatformProcess::ExecProcess(TEXT("/bin/chmod"), *FString::Printf(TEXT("+x \"%s\""), *PermCheckPath), nullptr, nullptr, nullptr);
		}
	}
#endif

	// stdin: child reads (ReadPipe inheritable), parent writes (WritePipe local)
	if (!FPlatformProcess::CreatePipe(StdinReadPipe, StdinWritePipe, true))
	{
		SetState(EACPClientState::Error, TEXT("Failed to create stdin pipe"));
		return false;
	}

	// stdout: child writes (WritePipe inheritable), parent reads (ReadPipe local)
	if (!FPlatformProcess::CreatePipe(StdoutReadPipe, StdoutWritePipe, false))
	{
		FPlatformProcess::ClosePipe(StdinReadPipe, StdinWritePipe);
		SetState(EACPClientState::Error, TEXT("Failed to create stdout pipe"));
		return false;
	}

	// stderr: separate pipe so we can capture agent errors and npx download progress
	void* StderrWritePipe = nullptr;
	if (!FPlatformProcess::CreatePipe(StderrReadPipe, StderrWritePipe, false))
	{
		// Non-fatal: stderr just merges into void
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPClient: Failed to create stderr pipe, agent errors won't be visible"));
		StderrReadPipe = nullptr;
	}

	// Spawn the process.
	// Environment variables in UE are process-global, so serialize launch-time overrides.
	{
		FScopeLock LaunchLock(&GAgentProcessLaunchEnvLock);
		TArray<FEnvVarRestore> RestoreList;

		const FString OriginalPathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
		const FString AugmentedPathEnv = BuildAugmentedPathForChildProcess(OriginalPathEnv, ExecutablePath);
		const bool bUpdatedPathForChild = !AugmentedPathEnv.IsEmpty() && AugmentedPathEnv != OriginalPathEnv;
		if (bUpdatedPathForChild)
		{
			ApplyTemporaryEnvVar(TEXT("PATH"), AugmentedPathEnv, RestoreList);
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Temporarily augmented PATH for child process launch"));
		}

		if (!GeminiSystemSettingsPath.IsEmpty())
		{
			ApplyTemporaryEnvVar(TEXT("GEMINI_CLI_SYSTEM_SETTINGS_PATH"), GeminiSystemSettingsPath, RestoreList);
		}

		for (const TPair<FString, FString>& Pair : Config.EnvironmentVariables)
		{
			if (!Pair.Key.IsEmpty())
			{
				ApplyTemporaryEnvVar(Pair.Key, Pair.Value, RestoreList);
			}
		}

		// UE 5.7+ has FProcessStartInfo with separate StdErr pipe support.
		// Older UE versions use the legacy CreateProc overload (stderr merges into stdout).
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 7
		{
			UE::HAL::FProcessStartInfo StartInfo;
			StartInfo.Uri = *ExecutablePath;
			StartInfo.Arguments = *Arguments;
			StartInfo.WorkingDirectory = *WorkingDir;
			StartInfo.bHidden = true;
			StartInfo.bInheritHandles = false;  // false = UE sets up PROC_THREAD_ATTRIBUTE_HANDLE_LIST for specific handles only
			StartInfo.StdIn = UE::HAL::FReadHandle{ StdinReadPipe };
			StartInfo.StdOut = UE::HAL::FWriteHandle{ StdoutWritePipe };
			StartInfo.StdErr = StderrWritePipe
				? UE::HAL::FWriteHandle{ StderrWritePipe }
				: UE::HAL::FWriteHandle{ StdoutWritePipe };

			auto Result = FPlatformProcess::CreateProc(StartInfo);
			ProcessHandle = Result.Get<FProcHandle>();
		}
#else
		// Legacy: no separate stderr pipe
		if (StderrWritePipe)
		{
			FPlatformProcess::ClosePipe(nullptr, StderrWritePipe);
			StderrWritePipe = nullptr;
			FPlatformProcess::ClosePipe(StderrReadPipe, nullptr);
			StderrReadPipe = nullptr;
		}
		ProcessHandle = FPlatformProcess::CreateProc(
			*ExecutablePath,
			*Arguments,
			false,  // bLaunchDetached
			true,   // bLaunchHidden
			true,   // bLaunchReallyHidden
			nullptr, // OutProcessID
			0,      // PriorityModifier
			*WorkingDir,
			StdoutWritePipe,
			StdinReadPipe
		);
#endif

		RestoreTemporaryEnvVars(RestoreList);
	}

	// Close the child-side pipe handles (we keep the parent-side)
	FPlatformProcess::ClosePipe(StdinReadPipe, nullptr);
	FPlatformProcess::ClosePipe(nullptr, StdoutWritePipe);
	if (StderrWritePipe)
	{
		FPlatformProcess::ClosePipe(nullptr, StderrWritePipe);
	}

	if (!ProcessHandle.IsValid())
	{
		FPlatformProcess::ClosePipe(nullptr, StdinWritePipe);
		FPlatformProcess::ClosePipe(StdoutReadPipe, nullptr);
		StdinWritePipe = nullptr;
		StdoutReadPipe = nullptr;
		SetState(EACPClientState::Error, FString::Printf(TEXT("Failed to start agent: %s"), *ExecutablePath));
		return false;
	}

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Process started successfully: %s %s"), *ExecutablePath, *Arguments);
	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Working directory: %s"), *WorkingDir);

	// Start the reading threads
	bStopRequested = false;
	ReadThread = FRunnableThread::Create(this, TEXT("ACPClientReader"));

	if (!ReadThread)
	{
		Disconnect();
		SetState(EACPClientState::Error, TEXT("Failed to create reader thread"));
		return false;
	}

	// Start stderr reader (captures npx download progress, agent errors — like Zed's _stderr_task)
	if (StderrReadPipe)
	{
		TWeakPtr<FACPClient> WeakSelfForStderr = AsShared();
		Async(EAsyncExecution::Thread, [WeakSelfForStderr]()
		{
			if (TSharedPtr<FACPClient> Self = WeakSelfForStderr.Pin())
			{
				Self->StderrReaderLoop();
			}
		});
	}

	// Give the process a moment to initialize (especially important on Windows with .cmd wrappers)
	FPlatformProcess::Sleep(0.1f);

	// Verify process is still running
	if (!FPlatformProcess::IsProcRunning(ProcessHandle))
	{
		UE_LOG(LogAgentIntegrationKit, Error, TEXT("ACPClient: Process exited immediately after start"));
		Disconnect();
		SetState(EACPClientState::Error, TEXT("Agent process exited immediately"));
		return false;
	}

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Process verified running, sending initialize..."));

	// Connection established, now initialize
	SetState(EACPClientState::Initializing, TEXT("Initializing ACP session..."));
	Initialize();

	return true;
}

void FACPClient::Disconnect()
{
	// Guard against re-entry: if already disconnected (e.g., destructor calling Disconnect
	// while we're already inside a Disconnect → SetState → OnStateChanged callback chain),
	// skip to avoid infinite recursion / stack overflow.
	if (GetState() == EACPClientState::Disconnected)
	{
		return;
	}

	bStopRequested = true;

	// Wait for read thread to finish
	if (ReadThread)
	{
		ReadThread->WaitForCompletion();
		delete ReadThread;
		ReadThread = nullptr;
	}

	// Close pipes
	{
		FScopeLock Lock(&WriteLock);
		if (StdinWritePipe)
		{
			FPlatformProcess::ClosePipe(nullptr, StdinWritePipe);
			StdinWritePipe = nullptr;
		}
	}

	if (StdoutReadPipe)
	{
		FPlatformProcess::ClosePipe(StdoutReadPipe, nullptr);
		StdoutReadPipe = nullptr;
	}

	if (StderrReadPipe)
	{
		FPlatformProcess::ClosePipe(StderrReadPipe, nullptr);
		StderrReadPipe = nullptr;
	}

	// Terminate process
	if (ProcessHandle.IsValid())
	{
		FPlatformProcess::TerminateProc(ProcessHandle);
		FPlatformProcess::CloseProc(ProcessHandle);
		ProcessHandle.Reset();
	}

	if (!CopilotAdditionalMcpConfigPath.IsEmpty())
	{
		IFileManager::Get().Delete(*CopilotAdditionalMcpConfigPath, false, true);
		CopilotAdditionalMcpConfigPath.Empty();
	}
	if (!GeminiSystemSettingsPath.IsEmpty())
	{
		IFileManager::Get().Delete(*GeminiSystemSettingsPath, false, true);
		GeminiSystemSettingsPath.Empty();
	}

	// Clear state
	{
		FScopeLock Lock(&StateLock);
		PendingRequests.Empty();
		CurrentSessionId.Empty();
		ReadBuffer.Empty();
	}

	SetState(EACPClientState::Disconnected, TEXT("Disconnected"));
}

bool FACPClient::Init()
{
	return true;
}

void FACPClient::StderrReaderLoop()
{
	FString Buffer;
	while (!bStopRequested && StderrReadPipe)
	{
		FString Output = FPlatformProcess::ReadPipe(StderrReadPipe);
		if (Output.IsEmpty())
		{
			FPlatformProcess::Sleep(0.1f);
			continue;
		}

		Buffer += Output;

		// Process complete lines
		FString Left, Right;
		while (Buffer.Split(TEXT("\n"), &Left, &Right))
		{
			Left.TrimStartAndEndInline();
			Buffer = Right;

			if (Left.IsEmpty())
			{
				continue;
			}

			UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient stderr [%s]: %s"), *CurrentConfig.AgentName, *Left);

			// Forward to UI as a status update while still connecting
			if (GetState() == EACPClientState::Connecting)
			{
				TWeakPtr<FACPClient> WeakSelf = AsShared();
				FString StatusLine = Left;
				AsyncTask(ENamedThreads::GameThread, [WeakSelf, StatusLine]()
				{
					if (TSharedPtr<FACPClient> Self = WeakSelf.Pin())
					{
						if (Self->GetState() == EACPClientState::Connecting)
						{
							Self->OnStateChanged.Broadcast(EACPClientState::Connecting, StatusLine);
						}
					}
				});
			}
		}
	}
}

uint32 FACPClient::Run()
{
	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Reader thread started"));

	// Capture a weak reference for AsyncTask lambdas — prevents use-after-free
	// if the client is destroyed before the game thread processes the task.
	TWeakPtr<FACPClient> WeakSelf = AsShared();

	int32 EmptyReadCount = 0;
	double LastLogTime = FPlatformTime::Seconds();

	while (!bStopRequested)
	{
		if (!StdoutReadPipe)
		{
			UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPClient: StdoutReadPipe is null, exiting reader thread"));
			break;
		}

		// Read from stdout
		FString Output = FPlatformProcess::ReadPipe(StdoutReadPipe);
		if (!Output.IsEmpty())
		{
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Read %d chars from pipe: [%s]"), Output.Len(), *Output.Left(200));
			ReadBuffer += Output;
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Buffer now has %d chars"), ReadBuffer.Len());

			// Process complete lines (newline-delimited JSON)
			// Handle both Unix (\n) and Windows (\r\n) line endings
			int32 NewlineIndex;
			while (ReadBuffer.FindChar(TEXT('\n'), NewlineIndex))
			{
				FString Line = ReadBuffer.Left(NewlineIndex);
				ReadBuffer = ReadBuffer.Mid(NewlineIndex + 1);

				// Remove any trailing \r (from CRLF on Windows)
				Line.TrimStartAndEndInline();
				if (!Line.IsEmpty())
				{
					UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Found complete line (%d chars)"), Line.Len());
					// Process on game thread — use weak ptr to guard against client destruction
					AsyncTask(ENamedThreads::GameThread, [WeakSelf, Line]()
					{
						if (TSharedPtr<FACPClient> Self = WeakSelf.Pin())
						{
							Self->ProcessLine(Line);
						}
					});
				}
			}
		}
		else
		{
			EmptyReadCount++;

			// Check if process is still running — like Zed's _wait_task
			if (!FPlatformProcess::IsProcRunning(ProcessHandle))
			{
				int32 ExitCode = -1;
				FPlatformProcess::GetProcReturnCode(ProcessHandle, &ExitCode);

				FString ErrorMsg;
				if (ExitCode == 0)
				{
					ErrorMsg = TEXT("Agent process exited normally");
				}
				else
				{
					ErrorMsg = FString::Printf(TEXT("Agent process exited with code %d"), ExitCode);
				}

				UE_LOG(LogAgentIntegrationKit, Error, TEXT("ACPClient [%s]: %s"), *CurrentConfig.AgentName, *ErrorMsg);

				AsyncTask(ENamedThreads::GameThread, [WeakSelf, ErrorMsg, ExitCode]()
				{
					if (TSharedPtr<FACPClient> Self = WeakSelf.Pin())
					{
						Self->SetState(EACPClientState::Error, ErrorMsg);
						Self->OnError.Broadcast(ExitCode, ErrorMsg);
					}
				});
				break;
			}

			// Log periodically (every 5 seconds) to show reader thread is alive
			double CurrentTime = FPlatformTime::Seconds();
			if (CurrentTime - LastLogTime >= 5.0)
			{
				UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Reader thread alive, waiting for data... (empty reads: %d, buffer: %d chars)"),
					EmptyReadCount, ReadBuffer.Len());
				LastLogTime = CurrentTime;
				EmptyReadCount = 0;
			}

			// Small sleep to avoid busy waiting
			FPlatformProcess::Sleep(0.01f);
		}
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Reader thread exiting"));
	return 0;
}

void FACPClient::Stop()
{
	bStopRequested = true;
}

void FACPClient::Exit()
{
}

int32 FACPClient::SendRequest(const FString& Method, TSharedPtr<FJsonObject> Params)
{
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Preparing request for method: %s"), *Method);

	int32 RequestId;
	{
		FScopeLock Lock(&StateLock);
		RequestId = NextRequestId++;
		PendingRequests.Add(RequestId, Method);
	}

	TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Request->SetNumberField(TEXT("id"), RequestId);
	Request->SetStringField(TEXT("method"), Method);

	if (Params.IsValid())
	{
		Request->SetObjectField(TEXT("params"), Params);
	}

	FString JsonString;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonString);
	FJsonSerializer::Serialize(Request, Writer);
	Writer->Close();

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Sending request ID %d for method: %s"), RequestId, *Method);
	SendRawMessage(JsonString);

	return RequestId;
}

void FACPClient::SendNotification(const FString& Method, TSharedPtr<FJsonObject> Params)
{
	TSharedRef<FJsonObject> Notification = MakeShared<FJsonObject>();
	Notification->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Notification->SetStringField(TEXT("method"), Method);

	if (Params.IsValid())
	{
		Notification->SetObjectField(TEXT("params"), Params);
	}

	FString JsonString;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonString);
	FJsonSerializer::Serialize(Notification, Writer);
	Writer->Close();

	SendRawMessage(JsonString);
}

void FACPClient::SendRawMessage(const FString& JsonMessage)
{
	FScopeLock Lock(&WriteLock);

	if (!StdinWritePipe)
	{
		UE_LOG(LogAgentIntegrationKit, Error, TEXT("ACPClient: Cannot send message, pipe not open"));
		return;
	}

	// Log outgoing message for debugging
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Sending message (%d chars): %s"), JsonMessage.Len(), *JsonMessage);

	// Append newline (NDJSON format)
	FString Message = JsonMessage + TEXT("\n");

	// Write to pipe - return value may be unreliable on some platforms
	bool bWriteSuccess = FPlatformProcess::WritePipe(StdinWritePipe, Message);
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: WritePipe returned: %s"), bWriteSuccess ? TEXT("true") : TEXT("false"));
}

void FACPClient::ProcessLine(const FString& Line)
{
	// Bail if client was already disconnected (stale AsyncTask from reader thread)
	if (GetState() == EACPClientState::Disconnected)
	{
		return;
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: ProcessLine called with %d chars"), Line.Len());

	// Skip lines that don't look like JSON (e.g., stderr log messages that leaked through)
	if (!Line.StartsWith(TEXT("{")))
	{
		// Log as info so we can see what non-JSON output is coming
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Skipping non-JSON line: %s"), *Line);
		return;
	}

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPClient: Failed to parse JSON: %s"), *Line);
		return;
	}

	// Log all incoming JSON for debugging
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Received JSON: %s"), *Line);

	bool bHasId = JsonObject->HasField(TEXT("id"));
	bool bHasMethod = JsonObject->HasField(TEXT("method"));

	// Check if it's a server request (has both id and method)
	if (bHasId && bHasMethod)
	{
		// Server request - requires a response
		int32 Id = static_cast<int32>(JsonObject->GetNumberField(TEXT("id")));
		FString Method = JsonObject->GetStringField(TEXT("method"));
		TSharedPtr<FJsonObject> Params = JsonObject->HasField(TEXT("params"))
			? JsonObject->GetObjectField(TEXT("params"))
			: MakeShared<FJsonObject>();
		HandleServerRequest(Id, Method, Params);
	}
	else if (bHasId)
	{
		// Response to our request
		int32 Id = static_cast<int32>(JsonObject->GetNumberField(TEXT("id")));

		if (JsonObject->HasField(TEXT("error")))
		{
			TSharedPtr<FJsonObject> Error = JsonObject->GetObjectField(TEXT("error"));
			int32 Code = static_cast<int32>(Error->GetNumberField(TEXT("code")));
			FString Message = Error->GetStringField(TEXT("message"));

			// Check for more detailed message in error.data (agents use different field names)
			if (Error->HasField(TEXT("data")))
			{
				TSharedPtr<FJsonObject> ErrorData = Error->GetObjectField(TEXT("data"));
				if (ErrorData.IsValid())
				{
					FString DetailedMessage;
					// Try data.message first (Codex CLI)
					if (ErrorData->TryGetStringField(TEXT("message"), DetailedMessage) && !DetailedMessage.IsEmpty())
					{
						Message = DetailedMessage;
					}
					// Try data.details (Gemini CLI)
					else if (ErrorData->TryGetStringField(TEXT("details"), DetailedMessage) && !DetailedMessage.IsEmpty())
					{
						Message = DetailedMessage;
					}
				}
			}

			HandleError(Id, Code, Message);
		}
		else if (JsonObject->HasField(TEXT("result")))
		{
			TSharedPtr<FJsonObject> Result = JsonObject->GetObjectField(TEXT("result"));
			HandleResponse(Id, Result);
		}
	}
	else if (bHasMethod)
	{
		// Notification (no id)
		FString Method = JsonObject->GetStringField(TEXT("method"));
		TSharedPtr<FJsonObject> Params = JsonObject->HasField(TEXT("params"))
			? JsonObject->GetObjectField(TEXT("params"))
			: MakeShared<FJsonObject>();
		HandleNotification(Method, Params);
	}
}

void FACPClient::HandleResponse(int32 Id, TSharedPtr<FJsonObject> Result)
{
	FString Method;
	{
		FScopeLock Lock(&StateLock);
		if (FString* MethodPtr = PendingRequests.Find(Id))
		{
			Method = *MethodPtr;
			PendingRequests.Remove(Id);
		}
	}

	if (Method == TEXT("initialize"))
	{
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Received initialize response"));
		// Parse agent capabilities — support both current ACP format (agentCapabilities)
		// and legacy format (capabilities.sessions) for backward compatibility
		if (Result.IsValid())
		{
			if (Result->HasField(TEXT("agentCapabilities")))
			{
				TSharedPtr<FJsonObject> AgentCaps = Result->GetObjectField(TEXT("agentCapabilities"));
				if (AgentCaps.IsValid())
				{
					// loadSession (top-level boolean)
					if (AgentCaps->HasField(TEXT("loadSession")))
					{
						AgentCapabilities.bSupportsLoadSession = AgentCaps->GetBoolField(TEXT("loadSession"));
					}

					// sessionCapabilities.resume, sessionCapabilities.list, etc.
					if (AgentCaps->HasField(TEXT("sessionCapabilities")))
					{
						TSharedPtr<FJsonObject> SessionCaps = AgentCaps->GetObjectField(TEXT("sessionCapabilities"));
						if (SessionCaps.IsValid())
						{
							AgentCapabilities.bSupportsResumeSession = SessionCaps->HasField(TEXT("resume"));
							AgentCapabilities.bSupportsListSessions = SessionCaps->HasField(TEXT("list"));
							AgentCapabilities.bSupportsCloseSession = SessionCaps->HasField(TEXT("close"));
							AgentCapabilities.bSupportsDeleteSession = SessionCaps->HasField(TEXT("delete"));
						}
					}

					// promptCapabilities.image, promptCapabilities.audio
					if (AgentCaps->HasField(TEXT("promptCapabilities")))
					{
						TSharedPtr<FJsonObject> PromptCaps = AgentCaps->GetObjectField(TEXT("promptCapabilities"));
						if (PromptCaps.IsValid())
						{
							AgentCapabilities.bSupportsImage = PromptCaps->HasField(TEXT("image")) && PromptCaps->GetBoolField(TEXT("image"));
							AgentCapabilities.bSupportsAudio = PromptCaps->HasField(TEXT("audio")) && PromptCaps->GetBoolField(TEXT("audio"));
						}
					}

					UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Capabilities - Resume=%d, Load=%d, List=%d, Close=%d, Delete=%d, Image=%d, Audio=%d"),
						AgentCapabilities.bSupportsResumeSession, AgentCapabilities.bSupportsLoadSession,
						AgentCapabilities.bSupportsListSessions, AgentCapabilities.bSupportsCloseSession,
						AgentCapabilities.bSupportsDeleteSession,
						AgentCapabilities.bSupportsImage, AgentCapabilities.bSupportsAudio);
				}
			}
			// Note: Legacy "capabilities.sessions/prompts" format removed — all spec-compliant
			// agents use "agentCapabilities" per ACP v1.
		}

		// Parse authMethods (top-level field in initialize response)
		if (Result.IsValid() && Result->HasField(TEXT("authMethods")))
		{
			const TArray<TSharedPtr<FJsonValue>>* MethodsArray = nullptr;
			if (Result->TryGetArrayField(TEXT("authMethods"), MethodsArray) && MethodsArray)
			{
				for (const auto& Val : *MethodsArray)
				{
					TSharedPtr<FJsonObject> Obj = Val->AsObject();
					if (!Obj.IsValid()) continue;

					FACPAuthMethod AuthMethod;
					Obj->TryGetStringField(TEXT("id"), AuthMethod.Id);
					Obj->TryGetStringField(TEXT("name"), AuthMethod.Name);
					Obj->TryGetStringField(TEXT("description"), AuthMethod.Description);

					// ACP spec: prefer standard "type" field for auth method type detection
					Obj->TryGetStringField(TEXT("type"), AuthMethod.Type);
					if (AuthMethod.Type == TEXT("terminal"))
					{
						AuthMethod.bIsTerminalAuth = true;
					}

					// Fallback: check _meta.terminal-auth for older adapters that don't set "type"
					if (!AuthMethod.bIsTerminalAuth && Obj->HasField(TEXT("_meta")))
					{
						TSharedPtr<FJsonObject> Meta = Obj->GetObjectField(TEXT("_meta"));
						if (Meta.IsValid() && Meta->HasField(TEXT("terminal-auth")))
						{
							TSharedPtr<FJsonObject> TA = Meta->GetObjectField(TEXT("terminal-auth"));
							if (TA.IsValid())
							{
								AuthMethod.bIsTerminalAuth = true;
								if (AuthMethod.Type.IsEmpty())
								{
									AuthMethod.Type = TEXT("terminal");
								}
								TA->TryGetStringField(TEXT("command"), AuthMethod.TerminalAuthCommand);
								TA->TryGetStringField(TEXT("label"), AuthMethod.TerminalAuthLabel);

								const TArray<TSharedPtr<FJsonValue>>* ArgsArr = nullptr;
								if (TA->TryGetArrayField(TEXT("args"), ArgsArr) && ArgsArr)
								{
									for (const auto& Arg : *ArgsArr)
									{
										AuthMethod.TerminalAuthArgs.Add(Arg->AsString());
									}
								}
							}
						}
					}

					AgentCapabilities.AuthMethods.Add(AuthMethod);
					UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Auth method: %s (%s) type=%s terminal=%d"),
						*AuthMethod.Id, *AuthMethod.Name, *AuthMethod.Type, AuthMethod.bIsTerminalAuth);
				}
			}
		}

		if (!CurrentConfig.LaunchResumeSessionId.IsEmpty()
			&& (CurrentConfig.AgentName == TEXT("Gemini CLI") || CurrentConfig.AgentName == TEXT("Copilot CLI")))
		{
			CurrentSessionId = CurrentConfig.LaunchResumeSessionId;
			SetState(EACPClientState::InSession, TEXT("Session resumed"));
		}
		else
		{
			SetState(EACPClientState::Ready, TEXT("Connected to agent"));
		}
	}
	else if (Method == TEXT("session/new"))
	{
		if (Result.IsValid())
		{
			// ACP spec: session ID returned as "sessionId"
			if (Result->HasField(TEXT("sessionId")))
			{
				CurrentSessionId = Result->GetStringField(TEXT("sessionId"));
			}

			// Parse configOptions FIRST so reasoning options are available before model broadcasts
				if (Result->HasField(TEXT("configOptions")))
				{
					const TArray<TSharedPtr<FJsonValue>>* ConfigArray = nullptr;
					if (Result->TryGetArrayField(TEXT("configOptions"), ConfigArray))
					{
						bool bFoundReasoningConfig = false;
						bool bFoundModels = false;
						bool bFoundModes = false;
						AvailableReasoningEfforts.Empty();
						CurrentReasoningEffort.Empty();
						ReasoningConfigOptionId.Empty();

						for (const TSharedPtr<FJsonValue>& OptionValue : *ConfigArray)
						{
							TSharedPtr<FJsonObject> OptionObj = OptionValue->AsObject();
							if (!OptionObj.IsValid()) continue;

							FString OptionId, Category, CurrentValue;
							OptionObj->TryGetStringField(TEXT("id"), OptionId);
							OptionObj->TryGetStringField(TEXT("category"), Category);
							OptionObj->TryGetStringField(TEXT("currentValue"), CurrentValue);

						const TArray<TSharedPtr<FJsonValue>>* OptionsArray = nullptr;
						OptionObj->TryGetArrayField(TEXT("options"), OptionsArray);

						if (Category == TEXT("model") && OptionsArray)
						{
							// Agent provides models via configOptions — use unified config path
							bUsesConfigOptions = true;
							bFoundModels = true;
							SessionModelState.AvailableModels.Empty();
							SessionModelState.CurrentModelId = CurrentValue;
							for (const TSharedPtr<FJsonValue>& OptValue : *OptionsArray)
							{
								TSharedPtr<FJsonObject> OptObj = OptValue->AsObject();
								if (!OptObj.IsValid()) continue;
								FACPModelInfo ModelInfo;
								OptObj->TryGetStringField(TEXT("value"), ModelInfo.ModelId);
								OptObj->TryGetStringField(TEXT("name"), ModelInfo.Name);
								OptObj->TryGetStringField(TEXT("description"), ModelInfo.Description);
								SessionModelState.AvailableModels.Add(ModelInfo);
							}
						}
						else if (Category == TEXT("mode") && OptionsArray)
						{
							bFoundModes = true;
							SessionModeState.AvailableModes.Empty();
							SessionModeState.CurrentModeId = CurrentValue;
							for (const TSharedPtr<FJsonValue>& OptValue : *OptionsArray)
							{
								TSharedPtr<FJsonObject> OptObj = OptValue->AsObject();
								if (!OptObj.IsValid()) continue;
								FACPSessionMode ModeInfo;
								OptObj->TryGetStringField(TEXT("value"), ModeInfo.ModeId);
								OptObj->TryGetStringField(TEXT("name"), ModeInfo.Name);
								OptObj->TryGetStringField(TEXT("description"), ModeInfo.Description);
								SessionModeState.AvailableModes.Add(ModeInfo);
							}
						}
							else if (Category == TEXT("thought_level") && OptionsArray)
							{
								bFoundReasoningConfig = true;
								ReasoningConfigOptionId = OptionId.IsEmpty() ? TEXT("thinking") : OptionId;
								AvailableReasoningEfforts.Empty();
								CurrentReasoningEffort = CurrentValue;
								for (const TSharedPtr<FJsonValue>& OptValue : *OptionsArray)
								{
									TSharedPtr<FJsonObject> OptObj = OptValue->AsObject();
									if (!OptObj.IsValid()) continue;
									FString Value;
									OptObj->TryGetStringField(TEXT("value"), Value);
									if (!Value.IsEmpty())
									{
										AvailableReasoningEfforts.Add(Value);
									}
								}

								UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: session/create - %d reasoning options, current: %s"),
									AvailableReasoningEfforts.Num(), *CurrentReasoningEffort);

								// Reapply persisted reasoning level if available and supported.
									if (UACPSettings* Settings = UACPSettings::Get())
									{
										FString SavedReasoning = Settings->GetSavedReasoningForAgent(CurrentConfig.AgentName);
										if (!SavedReasoning.IsEmpty())
										{
											FString SavedThinkingValue = SavedReasoning == TEXT("none") ? TEXT("off") : SavedReasoning;
											if (AvailableReasoningEfforts.Contains(SavedThinkingValue)
												&& SavedThinkingValue != CurrentReasoningEffort
												&& !CurrentSessionId.IsEmpty())
											{
												SetReasoningEffort(SavedThinkingValue);
											}
										}
									}
								}
							}

						if (!bFoundReasoningConfig)
						{
							AvailableReasoningEfforts.Empty();
							CurrentReasoningEffort.Empty();
							ReasoningConfigOptionId.Empty();
						}

						// Broadcast AFTER all configOptions are parsed so reasoning
						// state is set before the models push checks SupportsReasoningEffortControl()
						if (bFoundModels)
						{
							OnModelsAvailable.Broadcast(SessionModelState);
						}
						if (bFoundModes)
						{
							OnModesAvailable.Broadcast(SessionModeState);
						}
						}
					}

			// Parse old-style models object (fallback for agents that don't use configOptions)
			if (!bUsesConfigOptions && Result->HasField(TEXT("models")))
			{
				TSharedPtr<FJsonObject> ModelsObj = Result->GetObjectField(TEXT("models"));
				if (ModelsObj.IsValid())
				{
					SessionModelState.AvailableModels.Empty();

					if (ModelsObj->HasField(TEXT("currentModelId")))
					{
						SessionModelState.CurrentModelId = ModelsObj->GetStringField(TEXT("currentModelId"));
					}

					const TArray<TSharedPtr<FJsonValue>>* AvailableModelsArray;
					if (ModelsObj->TryGetArrayField(TEXT("availableModels"), AvailableModelsArray))
					{
						for (const TSharedPtr<FJsonValue>& ModelValue : *AvailableModelsArray)
						{
							TSharedPtr<FJsonObject> ModelObj = ModelValue->AsObject();
							if (ModelObj.IsValid())
							{
								FACPModelInfo ModelInfo;
								ModelObj->TryGetStringField(TEXT("modelId"), ModelInfo.ModelId);
								ModelObj->TryGetStringField(TEXT("name"), ModelInfo.Name);
								ModelObj->TryGetStringField(TEXT("description"), ModelInfo.Description);
								SessionModelState.AvailableModels.Add(ModelInfo);
							}
						}
					}

					OnModelsAvailable.Broadcast(SessionModelState);
				}
			}

			// Parse old-style modes object (fallback for agents that don't use configOptions)
			if (!bUsesConfigOptions && Result->HasField(TEXT("modes")))
			{
				TSharedPtr<FJsonObject> ModesObj = Result->GetObjectField(TEXT("modes"));
				if (ModesObj.IsValid())
				{
					SessionModeState.AvailableModes.Empty();

					if (ModesObj->HasField(TEXT("currentModeId")))
					{
						SessionModeState.CurrentModeId = ModesObj->GetStringField(TEXT("currentModeId"));
					}

					const TArray<TSharedPtr<FJsonValue>>* AvailableModesArray;
					if (ModesObj->TryGetArrayField(TEXT("availableModes"), AvailableModesArray))
					{
						for (const TSharedPtr<FJsonValue>& ModeValue : *AvailableModesArray)
						{
							TSharedPtr<FJsonObject> ModeObj = ModeValue->AsObject();
							if (ModeObj.IsValid())
							{
								FACPSessionMode ModeInfo;
								ModeObj->TryGetStringField(TEXT("id"), ModeInfo.ModeId);
								ModeObj->TryGetStringField(TEXT("name"), ModeInfo.Name);
								ModeObj->TryGetStringField(TEXT("description"), ModeInfo.Description);
								SessionModeState.AvailableModes.Add(ModeInfo);
							}
						}
					}

					OnModesAvailable.Broadcast(SessionModeState);
				}
			}

			if (!CurrentSessionId.IsEmpty())
			{
				UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Session created with ID: %s"), *CurrentSessionId);
				// Don't clobber Prompting state — a pending prompt may have already been
				// sent via another session/new response, transitioning us to Prompting.
				if (GetState() != EACPClientState::Prompting)
				{
					SetState(EACPClientState::InSession, TEXT("Session started"));
				}
			}
		}
	}
	else if (Method == TEXT("session/resume") || Method == TEXT("session/load"))
	{
		// CurrentSessionId was already set before sending the request (in ResumeSession/LoadSession)
		// The response may optionally include sessionId (some agents do), but per ACP spec it's not required
		if (Result.IsValid())
		{
			// Update sessionId if the response provides one (some agents may return it)
			if (Result->HasField(TEXT("sessionId")))
			{
				CurrentSessionId = Result->GetStringField(TEXT("sessionId"));
			}

			// Parse unified configOptions first when provided so they remain the source of truth.
			if (Result->HasField(TEXT("configOptions")))
			{
				const TArray<TSharedPtr<FJsonValue>>* ConfigArray = nullptr;
				if (Result->TryGetArrayField(TEXT("configOptions"), ConfigArray) && ConfigArray)
				{
					TSharedRef<FJsonObject> SyntheticUpdate = MakeShared<FJsonObject>();
					SyntheticUpdate->SetStringField(TEXT("updateType"), TEXT("config_option_update"));
					SyntheticUpdate->SetArrayField(TEXT("configOptions"), *ConfigArray);
					ProcessSessionUpdate(SyntheticUpdate);
				}
			}

			// Parse models if provided (same as session/new)
			if (!bUsesConfigOptions && Result->HasField(TEXT("models")))
			{
				TSharedPtr<FJsonObject> ModelsObj = Result->GetObjectField(TEXT("models"));
				if (ModelsObj.IsValid())
				{
					SessionModelState.AvailableModels.Empty();

					if (ModelsObj->HasField(TEXT("currentModelId")))
					{
						SessionModelState.CurrentModelId = ModelsObj->GetStringField(TEXT("currentModelId"));
					}

					const TArray<TSharedPtr<FJsonValue>>* AvailableModelsArray;
					if (ModelsObj->TryGetArrayField(TEXT("availableModels"), AvailableModelsArray))
					{
						for (const TSharedPtr<FJsonValue>& ModelValue : *AvailableModelsArray)
						{
							TSharedPtr<FJsonObject> ModelObj = ModelValue->AsObject();
							if (ModelObj.IsValid())
							{
								FACPModelInfo ModelInfo;
								ModelObj->TryGetStringField(TEXT("modelId"), ModelInfo.ModelId);
								ModelObj->TryGetStringField(TEXT("name"), ModelInfo.Name);
								ModelObj->TryGetStringField(TEXT("description"), ModelInfo.Description);
								SessionModelState.AvailableModels.Add(ModelInfo);
							}
						}
					}

					OnModelsAvailable.Broadcast(SessionModelState);
				}
			}

			// Parse modes if provided (same as session/new)
			if (!bUsesConfigOptions && Result->HasField(TEXT("modes")))
			{
				TSharedPtr<FJsonObject> ModesObj = Result->GetObjectField(TEXT("modes"));
				if (ModesObj.IsValid())
				{
					SessionModeState.AvailableModes.Empty();

					if (ModesObj->HasField(TEXT("currentModeId")))
					{
						SessionModeState.CurrentModeId = ModesObj->GetStringField(TEXT("currentModeId"));
					}

					const TArray<TSharedPtr<FJsonValue>>* AvailableModesArray;
					if (ModesObj->TryGetArrayField(TEXT("availableModes"), AvailableModesArray))
					{
						for (const TSharedPtr<FJsonValue>& ModeValue : *AvailableModesArray)
						{
							TSharedPtr<FJsonObject> ModeObj = ModeValue->AsObject();
							if (ModeObj.IsValid())
							{
								FACPSessionMode ModeInfo;
								ModeObj->TryGetStringField(TEXT("id"), ModeInfo.ModeId);
								ModeObj->TryGetStringField(TEXT("name"), ModeInfo.Name);
								ModeObj->TryGetStringField(TEXT("description"), ModeInfo.Description);
								SessionModeState.AvailableModes.Add(ModeInfo);
							}
						}
					}

					OnModesAvailable.Broadcast(SessionModeState);
				}
			}
		}

		if (!CurrentSessionId.IsEmpty())
		{
			UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Session %s with ID: %s"),
				Method == TEXT("session/resume") ? TEXT("resumed") : TEXT("loaded"), *CurrentSessionId);
			// Don't clobber Prompting state — a pending prompt may have already been
			// sent via another session response, transitioning us to Prompting.
			if (GetState() != EACPClientState::Prompting)
			{
				SetState(EACPClientState::InSession,
					Method == TEXT("session/resume") ? TEXT("Session resumed") : TEXT("Session loaded"));
			}
		}
		else
		{
			UE_LOG(LogAgentIntegrationKit, Error, TEXT("ACPClient: %s response but no session ID available"), *Method);
			SetState(EACPClientState::Error, TEXT("Session restore failed - no session ID"));
		}
	}
	else if (Method == TEXT("session/prompt"))
	{
		// Parse usage from prompt response if available
		if (Result.IsValid() && Result->HasField(TEXT("usage")))
		{
			TSharedPtr<FJsonObject> UsageObj = Result->GetObjectField(TEXT("usage"));
			if (UsageObj.IsValid())
			{
				int32 TotalTokens = 0, InputTokens = 0, OutputTokens = 0;
				int32 ThoughtTokens = 0, CachedRead = 0, CachedWrite = 0;

				UsageObj->TryGetNumberField(TEXT("total_tokens"), TotalTokens);
				UsageObj->TryGetNumberField(TEXT("input_tokens"), InputTokens);
				UsageObj->TryGetNumberField(TEXT("output_tokens"), OutputTokens);
				UsageObj->TryGetNumberField(TEXT("thought_tokens"), ThoughtTokens);

				// Support both ACP field names and Claude API field names
				// ACP: cached_read_tokens, cached_write_tokens
				// Claude: cache_read_input_tokens, cache_creation_input_tokens
				if (!UsageObj->TryGetNumberField(TEXT("cached_read_tokens"), CachedRead))
				{
					UsageObj->TryGetNumberField(TEXT("cache_read_input_tokens"), CachedRead);
				}
				if (!UsageObj->TryGetNumberField(TEXT("cached_write_tokens"), CachedWrite))
				{
					UsageObj->TryGetNumberField(TEXT("cache_creation_input_tokens"), CachedWrite);
				}

				// Update session usage
				SessionUsage.TotalTokens = TotalTokens;
				SessionUsage.InputTokens = InputTokens;
				SessionUsage.OutputTokens = OutputTokens;
				SessionUsage.ReasoningTokens = ThoughtTokens;
				SessionUsage.CachedTokens = CachedRead + CachedWrite;

				UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Prompt usage - Total: %d, Input: %d, Output: %d, Thought: %d"),
					TotalTokens, InputTokens, OutputTokens, ThoughtTokens);

				// Broadcast usage update
				FACPSessionUpdate UsageUpdate;
				UsageUpdate.UpdateType = EACPUpdateType::UsageUpdate;
				UsageUpdate.Usage = SessionUsage;
				OnSessionUpdate.Broadcast(UsageUpdate);
			}
		}

		// ACP spec: read stopReason from prompt response
		FString StopReason;
		if (Result.IsValid())
		{
			Result->TryGetStringField(TEXT("stopReason"), StopReason);
		}

		// Analytics: record prompt completed with response duration
		if (AnalyticsPromptStartTime > 0.0)
		{
			double DurationSec = FPlatformTime::Seconds() - AnalyticsPromptStartTime;
			TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
			Props->SetStringField(TEXT("agent"), CurrentConfig.AgentName);
			Props->SetNumberField(TEXT("duration_sec"), DurationSec);
			Props->SetStringField(TEXT("stop_reason"), StopReason);
			FAIKAnalytics::Get().RecordEvent(TEXT("prompt_completed"), Props);
			AnalyticsPromptStartTime = 0.0;
		}

		// Broadcast prompt completion with stop reason
		OnPromptComplete.Broadcast(StopReason, SessionUsage);

		// Prompt completed
		SetState(EACPClientState::InSession, TEXT("Ready"));
	}
	else if (Method == TEXT("session/close"))
	{
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Session closed successfully"));
		CurrentSessionId.Empty();
		SetState(EACPClientState::Ready, TEXT("Session closed"));
	}
	else if (Method == TEXT("session/delete"))
	{
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Session deleted successfully via ACP"));
	}
	else if (Method == TEXT("session/set_mode"))
	{
		// Mode change successful
		if (Result.IsValid() && Result->HasField(TEXT("success")))
		{
			bool bSuccess = Result->GetBoolField(TEXT("success"));
			if (bSuccess)
			{
				UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Session mode changed successfully"));
				// The current mode will be updated via session/update notification
			}
		}
	}
	else if (Method == TEXT("session/set_config_option"))
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Config option set successfully"));

		// Some adapters include updated configOptions in the response but do not
		// always emit a separate config_option_update notification. Parse here too.
		if (Result.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* ConfigArray = nullptr;
			if (Result->TryGetArrayField(TEXT("configOptions"), ConfigArray) && ConfigArray)
			{
				TSharedRef<FJsonObject> SyntheticUpdate = MakeShared<FJsonObject>();
				SyntheticUpdate->SetStringField(TEXT("updateType"), TEXT("config_option_update"));
				SyntheticUpdate->SetArrayField(TEXT("configOptions"), *ConfigArray);
				ProcessSessionUpdate(SyntheticUpdate);
			}
		}
	}
	else if (Method == TEXT("authenticate"))
	{
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Authentication succeeded"));
		OnAuthComplete.Broadcast(true, TEXT(""));
	}
	else if (Method == TEXT("session/list"))
	{
		TArray<FACPRemoteSessionEntry> PageSessions;
		if (Result.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* SessionsArray = nullptr;
			if (Result->TryGetArrayField(TEXT("sessions"), SessionsArray) && SessionsArray)
			{
				for (const auto& Val : *SessionsArray)
				{
					const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
					if (!Val.IsValid() || !Val->TryGetObject(ObjPtr) || !ObjPtr || !ObjPtr->IsValid()) continue;
					const TSharedPtr<FJsonObject>& Obj = *ObjPtr;

					FACPRemoteSessionEntry Entry;
					Obj->TryGetStringField(TEXT("sessionId"), Entry.SessionId);
					Obj->TryGetStringField(TEXT("title"), Entry.Title);
					Obj->TryGetStringField(TEXT("cwd"), Entry.Cwd);

					FString UpdatedStr;
					if (Obj->TryGetStringField(TEXT("updatedAt"), UpdatedStr))
					{
						FDateTime::ParseIso8601(*UpdatedStr, Entry.UpdatedAt);
					}

					if (!Entry.SessionId.IsEmpty())
					{
						PageSessions.Add(MoveTemp(Entry));
					}
				}
			}
		}

		// Accumulate into paginated buffer
		PaginatedSessionAccumulator.Append(PageSessions);

		// ACP spec: cursor-based pagination — auto-fetch remaining pages
		FString NextCursor;
		if (Result.IsValid() && Result->TryGetStringField(TEXT("nextCursor"), NextCursor) && !NextCursor.IsEmpty())
		{
			// Infinite-cursor protection (Zed pattern): stop if cursor repeats
			if (NextCursor == LastPaginationCursor)
			{
				UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPClient: Session list pagination returned same cursor '%s'; stopping to avoid loop"), *NextCursor);
			}
			else
			{
				UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Session list has more pages (nextCursor=%s), fetching next"), *NextCursor);
				LastPaginationCursor = NextCursor;
				ListSessions(PaginatedSessionCwd, NextCursor);
				// Don't broadcast yet — wait for the final page
				OnResponse.Broadcast(Result);
				return;
			}
		}

		// All pages received — filter by cwd on our side (Zed pattern) and broadcast
		TArray<FACPRemoteSessionEntry> AllSessions = MoveTemp(PaginatedSessionAccumulator);
		PaginatedSessionAccumulator.Empty();
		LastPaginationCursor.Empty();

		const int32 TotalBeforeFilter = AllSessions.Num();

		// Client-side cwd filtering: normalize paths and compare
		// We don't send cwd to codex-acp's session/list (matches Zed behavior)
		// because codex-acp's internal list_threads has issues with cwd-scoped results.
		// Instead we get ALL sessions and filter here.
		if (!PaginatedSessionCwd.IsEmpty())
		{
			FString FilterCwd = FPaths::ConvertRelativePathToFull(PaginatedSessionCwd);
			// Strip trailing slash
			while (FilterCwd.Len() > 1 && (FilterCwd.EndsWith(TEXT("/")) || FilterCwd.EndsWith(TEXT("\\"))))
			{
				FilterCwd.LeftChopInline(1);
			}
			FilterCwd = FilterCwd.Replace(TEXT("\\"), TEXT("/"));

			UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Filtering %d sessions by cwd='%s'"), TotalBeforeFilter, *FilterCwd);

			int32 FilteredCount = 0;
			AllSessions.RemoveAll([&FilterCwd, &FilteredCount](const FACPRemoteSessionEntry& Entry)
			{
				if (Entry.Cwd.IsEmpty())
				{
					FilteredCount++;
					return true;
				}
				FString SessionCwd = Entry.Cwd.Replace(TEXT("\\"), TEXT("/"));
				// Strip trailing slash
				while (SessionCwd.Len() > 1 && (SessionCwd.EndsWith(TEXT("/")) || SessionCwd.EndsWith(TEXT("\\"))))
				{
					SessionCwd.LeftChopInline(1);
				}
				// Case-insensitive on Windows
#if PLATFORM_WINDOWS
				bool bMatch = FilterCwd.Equals(SessionCwd, ESearchCase::IgnoreCase);
#else
				bool bMatch = (FilterCwd == SessionCwd);
#endif
				if (!bMatch)
				{
					FilteredCount++;
					UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Session '%s' cwd mismatch: filter='%s' session='%s'"),
						*Entry.SessionId, *FilterCwd, *SessionCwd);
				}
				return !bMatch;
			});

			if (FilteredCount > 0)
			{
				UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Filtered out %d sessions (cwd mismatch), %d remaining"), FilteredCount, AllSessions.Num());
			}
		}
		PaginatedSessionCwd.Empty();

		UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Session list complete: %d total from agent, %d after cwd filter"), TotalBeforeFilter, AllSessions.Num());
		OnSessionListReceived.Broadcast(AllSessions);
	}

	OnResponse.Broadcast(Result);
}

void FACPClient::HandleError(int32 Id, int32 Code, const FString& Message)
{
	FString Method;
	{
		FScopeLock Lock(&StateLock);
		if (FString* MethodPtr = PendingRequests.Find(Id))
		{
			Method = *MethodPtr;
			PendingRequests.Remove(Id);
		}
	}

	UE_LOG(LogAgentIntegrationKit, Error, TEXT("ACPClient: Error in %s (code %d): %s"), *Method, Code, *Message);

	if (Method == TEXT("initialize"))
	{
		SetState(EACPClientState::Error, FString::Printf(TEXT("Initialization failed: %s"), *Message));
	}
	else if (Method == TEXT("session/resume"))
	{
		// session/resume failed — fall back to session/load if supported, then session/new
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPClient: session/resume failed, attempting fallback"));
		if (AgentCapabilities.bSupportsLoadSession && !CurrentSessionId.IsEmpty())
		{
			// CurrentSessionId was pre-set in ResumeSession(), reuse it for load
			FString SessionIdToLoad = CurrentSessionId;
			CurrentSessionId.Empty(); // Clear so LoadSession can set it fresh
			LoadSession(SessionIdToLoad, FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		}
		else
		{
			CurrentSessionId.Empty(); // Clear stale session ID
			NewSession(FPaths::ProjectDir());
		}
	}
	else if (Method == TEXT("session/load"))
	{
		// session/load failed — fall back to session/new
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPClient: session/load failed, falling back to new session"));
		CurrentSessionId.Empty(); // Clear stale session ID
		NewSession(FPaths::ProjectDir());
	}
	else if (Method == TEXT("session/new"))
	{
		SetState(EACPClientState::Error, FString::Printf(TEXT("Failed to create session: %s"), *Message));
	}
	else if (Method == TEXT("authenticate"))
	{
		UE_LOG(LogAgentIntegrationKit, Error, TEXT("ACPClient: Authentication failed: %s"), *Message);
		OnAuthComplete.Broadcast(false, Message);
	}
	else if (Method == TEXT("session/list"))
	{
		// Don't permanently disable — the error may be transient (e.g., auth not yet complete).
		// The session list will be re-requested on next state change to InSession.
		// Reset pagination state so the next request starts fresh.
		PaginatedSessionAccumulator.Empty();
		PaginatedSessionCwd.Empty();
		LastPaginationCursor.Empty();
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPClient: session/list failed (error %d: %s) — will retry after next session"), Code, *Message);
	}
	else if (Method == TEXT("session/delete"))
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPClient: session/delete failed (error %d: %s)"), Code, *Message);
	}
	else if (Method == TEXT("session/prompt"))
	{
		// Prompt request failed — transition out of Prompting so UI streaming can finalize.
		SetState(EACPClientState::InSession, TEXT("Ready"));
	}
	else
	{
		// Don't clobber Prompting state for non-critical errors (e.g., set_mode error
		// arriving while a prompt is in progress)
		if (GetState() != EACPClientState::Prompting)
		{
			SetState(EACPClientState::InSession, TEXT("Ready"));
		}
	}

	OnError.Broadcast(Code, Message);
}

void FACPClient::HandleNotification(const FString& Method, TSharedPtr<FJsonObject> Params)
{
	if (Method == TEXT("session/update"))
	{
		ProcessSessionUpdate(Params);
	}
	// Handle other notifications as needed
}

void FACPClient::ProcessSessionUpdate(TSharedPtr<FJsonObject> Params)
{
	if (!Params.IsValid())
	{
		return;
	}

	FACPSessionUpdate Update;

	// Track source session for manager-side routing.
	if (!Params->TryGetStringField(TEXT("sessionId"), Update.SessionId))
	{
		Params->TryGetStringField(TEXT("id"), Update.SessionId);
	}

	// ACP spec: params contains sessionId and update object
	// The update object has sessionUpdate (type) and content
	// OpenRouter uses flat format with type/text at top level
	TSharedPtr<FJsonObject> UpdateObj;
	if (Params->HasField(TEXT("update")))
	{
		UpdateObj = Params->GetObjectField(TEXT("update"));
	}
	else
	{
		// Fallback: flat format (OpenRouter agent uses type/text at top level)
		UpdateObj = Params;
	}

	// Get the update type - ACP uses "sessionUpdate", legacy uses "type"
	FString UpdateType;
	if (!UpdateObj->TryGetStringField(TEXT("sessionUpdate"), UpdateType))
	{
		UpdateObj->TryGetStringField(TEXT("type"), UpdateType);
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Processing session update type: '%s'"), *UpdateType);

	if (UpdateType == TEXT("agent_message_chunk"))
	{
		// Check for system status messages (e.g., compaction events) via _meta.systemStatus
		bool bIsSystemStatus = false;
		if (UpdateObj->HasField(TEXT("_meta")))
		{
			TSharedPtr<FJsonObject> Meta = UpdateObj->GetObjectField(TEXT("_meta"));
			if (Meta.IsValid() && Meta->HasField(TEXT("systemStatus")))
			{
				bIsSystemStatus = true;
				Update.SystemStatus = Meta->GetStringField(TEXT("systemStatus"));
			}
		}

		if (bIsSystemStatus)
		{
			// System status message — render as inline status indicator (not regular text)
			Update.UpdateType = EACPUpdateType::AgentMessageChunk;
			Update.bIsSystemStatus = true;
			if (UpdateObj->HasField(TEXT("content")))
			{
				TSharedPtr<FJsonObject> Content = UpdateObj->GetObjectField(TEXT("content"));
				if (Content.IsValid())
				{
					Update.TextChunk = Content->GetStringField(TEXT("text"));
				}
			}
			UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: System status: '%s' — %s"), *Update.SystemStatus, *Update.TextChunk);
		}
		else
		{
			Update.UpdateType = EACPUpdateType::AgentMessageChunk;
			// ACP format: content.text, legacy: text
			if (UpdateObj->HasField(TEXT("content")))
			{
				TSharedPtr<FJsonObject> Content = UpdateObj->GetObjectField(TEXT("content"));
				if (Content.IsValid())
				{
					Update.TextChunk = StripAnsiCodes(Content->GetStringField(TEXT("text")));
				}
			}
			else
			{
				Update.TextChunk = StripAnsiCodes(UpdateObj->GetStringField(TEXT("text")));
			}
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Parsed message chunk: '%s'"), *Update.TextChunk);
		}
	}
	else if (UpdateType == TEXT("agent_thought_chunk"))
	{
		Update.UpdateType = EACPUpdateType::AgentThoughtChunk;
		if (UpdateObj->HasField(TEXT("content")))
		{
			TSharedPtr<FJsonObject> Content = UpdateObj->GetObjectField(TEXT("content"));
			if (Content.IsValid())
			{
				Update.TextChunk = StripAnsiCodes(Content->GetStringField(TEXT("text")));
			}
		}
		else
		{
			Update.TextChunk = StripAnsiCodes(UpdateObj->GetStringField(TEXT("text")));
		}
	}
	else if (UpdateType == TEXT("user_message_chunk"))
	{
		// User messages arrive during session/load history replay
		Update.UpdateType = EACPUpdateType::UserMessageChunk;
		if (UpdateObj->HasField(TEXT("content")))
		{
			TSharedPtr<FJsonObject> Content = UpdateObj->GetObjectField(TEXT("content"));
			if (Content.IsValid())
			{
				Update.TextChunk = Content->GetStringField(TEXT("text"));
			}
		}
		else
		{
			Update.TextChunk = UpdateObj->GetStringField(TEXT("text"));
		}
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: User message chunk during replay: '%s'"), *Update.TextChunk.Left(100));
	}
	else if (UpdateType == TEXT("tool_call"))
	{
		Update.UpdateType = EACPUpdateType::ToolCall;

		// Claude Code uses "toolCallId", legacy uses "id"
		if (!UpdateObj->TryGetStringField(TEXT("toolCallId"), Update.ToolCallId))
		{
			UpdateObj->TryGetStringField(TEXT("id"), Update.ToolCallId);
		}

		// Get tool name - try multiple locations:
		// 1. _meta.claudeCode.toolName (Claude Code format)
		// 2. title (display name)
		// 3. name (legacy)
		if (UpdateObj->HasField(TEXT("_meta")))
		{
			TSharedPtr<FJsonObject> Meta = UpdateObj->GetObjectField(TEXT("_meta"));
			if (Meta.IsValid() && Meta->HasField(TEXT("claudeCode")))
			{
				TSharedPtr<FJsonObject> ClaudeCode = Meta->GetObjectField(TEXT("claudeCode"));
				if (ClaudeCode.IsValid())
				{
					ClaudeCode->TryGetStringField(TEXT("toolName"), Update.ToolName);
					ClaudeCode->TryGetStringField(TEXT("parentToolCallId"), Update.ParentToolCallId);
				}
			}
		}

		// Fall back to title or name if toolName not found
		if (Update.ToolName.IsEmpty())
		{
			if (!UpdateObj->TryGetStringField(TEXT("title"), Update.ToolName))
			{
				UpdateObj->TryGetStringField(TEXT("name"), Update.ToolName);
			}
		}

		// If ToolName is empty or just "{}" (Gemini CLI quirk), extract from toolCallId
		if (Update.ToolName.IsEmpty() || Update.ToolName == TEXT("{}"))
		{
			// toolCallId format: "toolname-timestamp" (e.g., "read_asset-1768495861936")
			FString ExtractedName = Update.ToolCallId;
			int32 DashIndex;
			if (ExtractedName.FindLastChar(TEXT('-'), DashIndex))
			{
				ExtractedName = ExtractedName.Left(DashIndex);
			}
			if (!ExtractedName.IsEmpty())
			{
				Update.ToolName = ExtractedName;
			}
		}

		// Arguments - try rawInput (Claude Code) or arguments (legacy)
		if (UpdateObj->HasField(TEXT("rawInput")))
		{
			TSharedPtr<FJsonObject> Args = UpdateObj->GetObjectField(TEXT("rawInput"));
			if (Args.IsValid() && Args->Values.Num() > 0)
			{
				FString ArgsString;
				TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ArgsString);
				FJsonSerializer::Serialize(Args.ToSharedRef(), Writer);
				Update.ToolArguments = ArgsString;
			}
		}
		else if (UpdateObj->HasField(TEXT("arguments")))
		{
			TSharedPtr<FJsonObject> Args = UpdateObj->GetObjectField(TEXT("arguments"));
			if (Args.IsValid())
			{
				FString ArgsString;
				TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ArgsString);
				FJsonSerializer::Serialize(Args.ToSharedRef(), Writer);
				Update.ToolArguments = ArgsString;
			}
		}

		// ACP spec: tool call kind and status
		UpdateObj->TryGetStringField(TEXT("kind"), Update.ToolCallKind);
		UpdateObj->TryGetStringField(TEXT("status"), Update.ToolCallStatus);

		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Tool call - ID: %s, Name: %s, Kind: %s"), *Update.ToolCallId, *Update.ToolName, *Update.ToolCallKind);
	}
	else if (UpdateType == TEXT("tool_call_update"))
	{
		Update.UpdateType = EACPUpdateType::ToolCallUpdate;

		// Claude Code uses "toolCallId", legacy uses "id"
		if (!UpdateObj->TryGetStringField(TEXT("toolCallId"), Update.ToolCallId))
		{
			UpdateObj->TryGetStringField(TEXT("id"), Update.ToolCallId);
		}

		// Extract parentToolCallId from _meta.claudeCode
		if (UpdateObj->HasField(TEXT("_meta")))
		{
			TSharedPtr<FJsonObject> Meta = UpdateObj->GetObjectField(TEXT("_meta"));
			if (Meta.IsValid() && Meta->HasField(TEXT("claudeCode")))
			{
				TSharedPtr<FJsonObject> ClaudeCode = Meta->GetObjectField(TEXT("claudeCode"));
				if (ClaudeCode.IsValid())
				{
					ClaudeCode->TryGetStringField(TEXT("parentToolCallId"), Update.ParentToolCallId);
				}
			}
		}

		// ACP spec: tool call kind and status
		UpdateObj->TryGetStringField(TEXT("kind"), Update.ToolCallKind);
		UpdateObj->TryGetStringField(TEXT("status"), Update.ToolCallStatus);

		// Determine success from status field
		FString Status = Update.ToolCallStatus;
		if (!Status.IsEmpty())
		{
			Update.bToolSuccess = (Status == TEXT("completed"));
		}
		else
		{
			Update.bToolSuccess = !UpdateObj->HasField(TEXT("error"));
		}

		// Get result from content array (Claude Code) or result field (legacy)
		if (UpdateObj->HasField(TEXT("content")))
		{
			const TArray<TSharedPtr<FJsonValue>>* ContentArray;
			if (UpdateObj->TryGetArrayField(TEXT("content"), ContentArray) && ContentArray->Num() > 0)
			{
				// Loop through content blocks to extract text and images
				for (const TSharedPtr<FJsonValue>& ContentValue : *ContentArray)
				{
					TSharedPtr<FJsonObject> ContentBlock = ContentValue->AsObject();
					if (!ContentBlock.IsValid())
					{
						continue;
					}

					FString ContentType;
					ContentBlock->TryGetStringField(TEXT("type"), ContentType);

					if (ContentType == TEXT("text"))
					{
						// Extract text content
						FString Text;
						if (ContentBlock->TryGetStringField(TEXT("text"), Text))
						{
							if (!Update.ToolResult.IsEmpty())
							{
								Update.ToolResult += TEXT("\n");
							}
							Update.ToolResult += Text;
						}
						// Also check nested content structure (Claude Code format)
						else if (ContentBlock->HasField(TEXT("content")))
						{
							TSharedPtr<FJsonObject> InnerContent = ContentBlock->GetObjectField(TEXT("content"));
							if (InnerContent.IsValid())
							{
								InnerContent->TryGetStringField(TEXT("text"), Text);
								if (!Update.ToolResult.IsEmpty())
								{
									Update.ToolResult += TEXT("\n");
								}
								Update.ToolResult += Text;
							}
						}
					}
					else if (ContentType == TEXT("image"))
					{
						FACPToolResultImage Image;
						Image.Width = 0;
						Image.Height = 0;

						// Try direct format first: { type: "image", data: "...", mimeType: "..." }
						ContentBlock->TryGetStringField(TEXT("data"), Image.Base64Data);
						ContentBlock->TryGetStringField(TEXT("mimeType"), Image.MimeType);

						// Fall back to Anthropic API format: { type: "image", source: { data: "...", media_type: "...", type: "base64" } }
						if (Image.Base64Data.IsEmpty())
						{
							TSharedPtr<FJsonObject> SourceObj = ContentBlock->GetObjectField(TEXT("source"));
							if (SourceObj.IsValid())
							{
								SourceObj->TryGetStringField(TEXT("data"), Image.Base64Data);
								SourceObj->TryGetStringField(TEXT("media_type"), Image.MimeType);
							}
						}

						if (!Image.Base64Data.IsEmpty())
						{
							Update.ToolResultImages.Add(Image);
							UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Extracted image from tool result (%s)"), *Image.MimeType);
						}
					}
					else if (ContentType == TEXT("content"))
					{
						// ACP wraps content blocks: { type: "content", content: { type: "image"|"text", ... } }
						TSharedPtr<FJsonObject> InnerContent = ContentBlock->GetObjectField(TEXT("content"));
						if (InnerContent.IsValid())
						{
							FString InnerType;
							InnerContent->TryGetStringField(TEXT("type"), InnerType);

							if (InnerType == TEXT("image"))
							{
								FACPToolResultImage Image;
								Image.Width = 0;
								Image.Height = 0;

								// Try ACP direct format first: { type: "image", data: "...", mimeType: "..." }
								InnerContent->TryGetStringField(TEXT("data"), Image.Base64Data);
								InnerContent->TryGetStringField(TEXT("mimeType"), Image.MimeType);

								// Fall back to Anthropic API format: { type: "image", source: { data, media_type } }
								if (Image.Base64Data.IsEmpty())
								{
									TSharedPtr<FJsonObject> SourceObj = InnerContent->GetObjectField(TEXT("source"));
									if (SourceObj.IsValid())
									{
										SourceObj->TryGetStringField(TEXT("data"), Image.Base64Data);
										SourceObj->TryGetStringField(TEXT("media_type"), Image.MimeType);
									}
								}

								if (!Image.Base64Data.IsEmpty())
								{
									Update.ToolResultImages.Add(Image);
									UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Extracted nested image from tool result (%s)"), *Image.MimeType);
								}
							}
							else if (InnerType == TEXT("text"))
							{
								// Also handle nested text in content type
								FString Text;
								if (InnerContent->TryGetStringField(TEXT("text"), Text))
								{
									if (!Update.ToolResult.IsEmpty())
									{
										Update.ToolResult += TEXT("\n");
									}
									Update.ToolResult += Text;
								}
							}
						}
					}
					else if (ContentType == TEXT("diff"))
					{
						// ACP spec: diff content — { type: "diff", path, oldText?, newText }
						Update.bHasDiff = true;
						ContentBlock->TryGetStringField(TEXT("path"), Update.DiffPath);
						ContentBlock->TryGetStringField(TEXT("oldText"), Update.DiffOldText);
						ContentBlock->TryGetStringField(TEXT("newText"), Update.DiffNewText);

						// Also produce a readable text summary for the tool result
						FString DiffSummary = FString::Printf(TEXT("File: %s\n"), *Update.DiffPath);
						if (!Update.DiffOldText.IsEmpty())
						{
							DiffSummary += FString::Printf(TEXT("--- old\n%s\n"), *Update.DiffOldText);
						}
						DiffSummary += FString::Printf(TEXT("+++ new\n%s"), *Update.DiffNewText);
						if (!Update.ToolResult.IsEmpty()) Update.ToolResult += TEXT("\n");
						Update.ToolResult += DiffSummary;

						UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Diff content for %s"), *Update.DiffPath);
					}
					else if (ContentType == TEXT("terminal"))
					{
						// ACP spec: terminal content — { type: "terminal", terminalId }
						FString TerminalId;
						ContentBlock->TryGetStringField(TEXT("terminalId"), TerminalId);
						if (!Update.ToolResult.IsEmpty()) Update.ToolResult += TEXT("\n");
						Update.ToolResult += FString::Printf(TEXT("[Terminal: %s]"), *TerminalId);

						UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Terminal content ref: %s"), *TerminalId);
					}
				}
			}
		}
		else
		{
			UpdateObj->TryGetStringField(TEXT("result"), Update.ToolResult);
		}

		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Tool update - ID: %s, Success: %d, Images: %d"), *Update.ToolCallId, Update.bToolSuccess, Update.ToolResultImages.Num());
	}
	else if (UpdateType == TEXT("plan"))
	{
		Update.UpdateType = EACPUpdateType::Plan;

		// Parse plan entries
		const TArray<TSharedPtr<FJsonValue>>* EntriesArray = nullptr;

		// Try content.entries (ACP format) or entries directly
		if (UpdateObj->HasField(TEXT("content")))
		{
			TSharedPtr<FJsonObject> Content = UpdateObj->GetObjectField(TEXT("content"));
			if (Content.IsValid())
			{
				Content->TryGetArrayField(TEXT("entries"), EntriesArray);
			}
		}
		if (!EntriesArray)
		{
			UpdateObj->TryGetArrayField(TEXT("entries"), EntriesArray);
		}

		if (EntriesArray)
		{
			for (const TSharedPtr<FJsonValue>& EntryValue : *EntriesArray)
			{
				TSharedPtr<FJsonObject> EntryObj = EntryValue->AsObject();
				if (!EntryObj.IsValid())
				{
					continue;
				}

				FACPPlanEntry Entry;
				EntryObj->TryGetStringField(TEXT("content"), Entry.Content);
				EntryObj->TryGetStringField(TEXT("activeForm"), Entry.ActiveForm);

				// Parse priority
				FString PriorityStr;
				if (EntryObj->TryGetStringField(TEXT("priority"), PriorityStr))
				{
					if (PriorityStr == TEXT("high"))
					{
						Entry.Priority = EACPPlanEntryPriority::High;
					}
					else if (PriorityStr == TEXT("low"))
					{
						Entry.Priority = EACPPlanEntryPriority::Low;
					}
					else
					{
						Entry.Priority = EACPPlanEntryPriority::Medium;
					}
				}

				// Parse status
				FString StatusStr;
				if (EntryObj->TryGetStringField(TEXT("status"), StatusStr))
				{
					if (StatusStr == TEXT("completed"))
					{
						Entry.Status = EACPPlanEntryStatus::Completed;
					}
					else if (StatusStr == TEXT("in_progress"))
					{
						Entry.Status = EACPPlanEntryStatus::InProgress;
					}
					else
					{
						Entry.Status = EACPPlanEntryStatus::Pending;
					}
				}

				Update.Plan.Entries.Add(Entry);
			}

			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Plan update - %d entries, %d completed"),
				Update.Plan.Entries.Num(), Update.Plan.GetCompletedCount());
		}
	}
	else if (UpdateType == TEXT("error"))
	{
		Update.UpdateType = EACPUpdateType::Error;
		Update.ErrorCode = static_cast<int32>(UpdateObj->GetNumberField(TEXT("code")));
		Update.ErrorMessage = UpdateObj->GetStringField(TEXT("message"));

		// Check for more detailed message in data (agents use different field names)
		if (UpdateObj->HasField(TEXT("data")))
		{
			TSharedPtr<FJsonObject> ErrorData = UpdateObj->GetObjectField(TEXT("data"));
			if (ErrorData.IsValid())
			{
				FString DetailedMessage;
				// Try data.message first (Codex CLI)
				if (ErrorData->TryGetStringField(TEXT("message"), DetailedMessage) && !DetailedMessage.IsEmpty())
				{
					Update.ErrorMessage = DetailedMessage;
				}
				// Try data.details (Gemini CLI)
				else if (ErrorData->TryGetStringField(TEXT("details"), DetailedMessage) && !DetailedMessage.IsEmpty())
				{
					Update.ErrorMessage = DetailedMessage;
				}
			}
		}
	}
	else if (UpdateType == TEXT("available_commands_update"))
	{
		// Parse available slash commands
		AvailableCommands.Empty();

		const TArray<TSharedPtr<FJsonValue>>* CommandsArray = nullptr;

		// Try content.availableCommands (ACP format) or availableCommands directly
		if (UpdateObj->HasField(TEXT("content")))
		{
			TSharedPtr<FJsonObject> Content = UpdateObj->GetObjectField(TEXT("content"));
			if (Content.IsValid())
			{
				Content->TryGetArrayField(TEXT("availableCommands"), CommandsArray);
			}
		}
		if (!CommandsArray)
		{
			UpdateObj->TryGetArrayField(TEXT("availableCommands"), CommandsArray);
		}

		if (CommandsArray)
		{
			for (const TSharedPtr<FJsonValue>& CmdValue : *CommandsArray)
			{
				TSharedPtr<FJsonObject> CmdObj = CmdValue->AsObject();
				if (!CmdObj.IsValid())
				{
					continue;
				}

				FACPSlashCommand Command;
				CmdObj->TryGetStringField(TEXT("name"), Command.Name);
				CmdObj->TryGetStringField(TEXT("description"), Command.Description);

				// Parse input hint if present (input can be null or an object)
				const TSharedPtr<FJsonObject>* InputObjPtr = nullptr;
				if (CmdObj->TryGetObjectField(TEXT("input"), InputObjPtr) && InputObjPtr && InputObjPtr->IsValid())
				{
					(*InputObjPtr)->TryGetStringField(TEXT("hint"), Command.InputHint);
				}

				if (!Command.Name.IsEmpty())
				{
					AvailableCommands.Add(Command);
				}
			}

			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Received %d available commands"), AvailableCommands.Num());
			OnCommandsAvailable.Broadcast(AvailableCommands);
		}

		return; // Don't broadcast as regular session update
	}
	else if (UpdateType == TEXT("current_mode_update"))
	{
		// Mode changed - update our state
		FString NewModeId;
		if (UpdateObj->HasField(TEXT("content")))
		{
			TSharedPtr<FJsonObject> Content = UpdateObj->GetObjectField(TEXT("content"));
			if (Content.IsValid())
			{
				Content->TryGetStringField(TEXT("modeId"), NewModeId);
			}
		}
		else
		{
			UpdateObj->TryGetStringField(TEXT("modeId"), NewModeId);
		}

		if (!NewModeId.IsEmpty())
		{
			SessionModeState.CurrentModeId = NewModeId;
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Session mode changed to: %s"), *NewModeId);
			OnModeChanged.Broadcast(NewModeId);
		}
		return; // Don't broadcast as session update
	}
	else if (UpdateType == TEXT("usage_update"))
	{
		Update.UpdateType = EACPUpdateType::UsageUpdate;

		// Parse context window info (required per ACP spec)
		int32 ContextUsed = 0, ContextSize = 0;
		if (UpdateObj->TryGetNumberField(TEXT("used"), ContextUsed))
		{
			Update.Usage.ContextUsed = ContextUsed;
		}
		if (UpdateObj->TryGetNumberField(TEXT("size"), ContextSize))
		{
			Update.Usage.ContextSize = ContextSize;
		}

		// Parse cost info (optional per ACP spec)
		if (UpdateObj->HasField(TEXT("cost")))
		{
			TSharedPtr<FJsonObject> CostObj = UpdateObj->GetObjectField(TEXT("cost"));
			if (CostObj.IsValid())
			{
				double CostAmount = 0.0;
				FString CostCurrency = TEXT("USD");
				CostObj->TryGetNumberField(TEXT("amount"), CostAmount);
				CostObj->TryGetStringField(TEXT("currency"), CostCurrency);

				Update.Usage.CostAmount = CostAmount;
				Update.Usage.CostCurrency = CostCurrency;
			}
		}

		// Parse detailed token breakdown from _meta (sent by our adapter)
		if (UpdateObj->HasField(TEXT("_meta")))
		{
			TSharedPtr<FJsonObject> Meta = UpdateObj->GetObjectField(TEXT("_meta"));
			if (Meta.IsValid())
			{
				int32 InTok = 0, OutTok = 0, CacheRead = 0, CacheCreate = 0;
				Meta->TryGetNumberField(TEXT("inputTokens"), InTok);
				Meta->TryGetNumberField(TEXT("outputTokens"), OutTok);
				Meta->TryGetNumberField(TEXT("cacheReadTokens"), CacheRead);
				Meta->TryGetNumberField(TEXT("cacheCreationTokens"), CacheCreate);

				Update.Usage.InputTokens = InTok;
				Update.Usage.OutputTokens = OutTok;
				Update.Usage.CacheReadTokens = CacheRead;
				Update.Usage.CacheCreationTokens = CacheCreate;
				Update.Usage.CachedTokens = CacheRead + CacheCreate;
				Update.Usage.TotalTokens = InTok + OutTok;

				// Result-only fields
				double TotalCost = 0.0, TurnCost = 0.0;
				int32 NumTurns = 0, DurationMs = 0;
				Meta->TryGetNumberField(TEXT("totalCostUSD"), TotalCost);
				Meta->TryGetNumberField(TEXT("turnCostUSD"), TurnCost);
				Meta->TryGetNumberField(TEXT("numTurns"), NumTurns);
				Meta->TryGetNumberField(TEXT("durationMs"), DurationMs);
				Update.Usage.TurnCostUSD = TurnCost;
				Update.Usage.NumTurns = NumTurns;
				Update.Usage.DurationMs = DurationMs;

				// Per-model breakdown
				if (Meta->HasField(TEXT("modelUsage")))
				{
					TSharedPtr<FJsonObject> ModelObj = Meta->GetObjectField(TEXT("modelUsage"));
					if (ModelObj.IsValid())
					{
						Update.Usage.ModelUsage.Empty();
						for (const auto& Pair : ModelObj->Values)
						{
							TSharedPtr<FJsonObject> MU = Pair.Value->AsObject();
							if (!MU.IsValid()) continue;

							FModelUsageEntry Entry;
							Entry.ModelName = Pair.Key;
							MU->TryGetNumberField(TEXT("inputTokens"), Entry.InputTokens);
							MU->TryGetNumberField(TEXT("outputTokens"), Entry.OutputTokens);
							MU->TryGetNumberField(TEXT("cacheReadTokens"), Entry.CacheReadTokens);
							MU->TryGetNumberField(TEXT("cacheCreationTokens"), Entry.CacheCreationTokens);
							MU->TryGetNumberField(TEXT("costUSD"), Entry.CostUSD);
							MU->TryGetNumberField(TEXT("contextWindow"), Entry.ContextWindow);
							MU->TryGetNumberField(TEXT("maxOutputTokens"), Entry.MaxOutputTokens);
							MU->TryGetNumberField(TEXT("webSearchRequests"), Entry.WebSearchRequests);
							Update.Usage.ModelUsage.Add(MoveTemp(Entry));
						}
					}
				}
			}
		}

		// Update cumulative session usage
		SessionUsage.ContextUsed = ContextUsed;
		SessionUsage.ContextSize = ContextSize;
		SessionUsage.InputTokens = Update.Usage.InputTokens;
		SessionUsage.OutputTokens = Update.Usage.OutputTokens;
		SessionUsage.CacheReadTokens = Update.Usage.CacheReadTokens;
		SessionUsage.CacheCreationTokens = Update.Usage.CacheCreationTokens;
		SessionUsage.CachedTokens = Update.Usage.CachedTokens;
		SessionUsage.TotalTokens = Update.Usage.TotalTokens;
		SessionUsage.TurnCostUSD = Update.Usage.TurnCostUSD;
		SessionUsage.NumTurns = Update.Usage.NumTurns;
		SessionUsage.DurationMs = Update.Usage.DurationMs;
		SessionUsage.ModelUsage = Update.Usage.ModelUsage;
		if (Update.Usage.CostAmount > 0.0)
		{
			SessionUsage.CostAmount = Update.Usage.CostAmount;
			SessionUsage.CostCurrency = Update.Usage.CostCurrency;
		}

		UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Usage update - Context: %d/%d, In: %d, Out: %d, Cache: %d+%d, Cost: %s"),
			ContextUsed, ContextSize, Update.Usage.InputTokens, Update.Usage.OutputTokens,
			Update.Usage.CacheReadTokens, Update.Usage.CacheCreationTokens,
			*Update.Usage.GetFormattedCost());
	}
	else if (UpdateType == TEXT("config_option_update"))
	{
		// Unified config options — parse and feed into existing model/mode/thinking infrastructure
		const TArray<TSharedPtr<FJsonValue>>* ConfigArray = nullptr;
		UpdateObj->TryGetArrayField(TEXT("configOptions"), ConfigArray);
		if (!ConfigArray)
		{
			return;
		}

		bool bFoundReasoningConfig = false;
		bool bFoundModels = false;
		bool bFoundModes = false;
		for (const TSharedPtr<FJsonValue>& OptionValue : *ConfigArray)
		{
			TSharedPtr<FJsonObject> OptionObj = OptionValue->AsObject();
			if (!OptionObj.IsValid())
			{
				continue;
			}

			FString OptionId, Category, CurrentValue;
			OptionObj->TryGetStringField(TEXT("id"), OptionId);
			OptionObj->TryGetStringField(TEXT("category"), Category);
			OptionObj->TryGetStringField(TEXT("currentValue"), CurrentValue);

			const TArray<TSharedPtr<FJsonValue>>* OptionsArray = nullptr;
			OptionObj->TryGetArrayField(TEXT("options"), OptionsArray);

			if (Category == TEXT("model") && OptionsArray)
			{
				// Agent provides models via configOptions — use unified config path
				bUsesConfigOptions = true;
				bFoundModels = true;
				SessionModelState.AvailableModels.Empty();
				SessionModelState.CurrentModelId = CurrentValue;

				for (const TSharedPtr<FJsonValue>& OptValue : *OptionsArray)
				{
					TSharedPtr<FJsonObject> OptObj = OptValue->AsObject();
					if (!OptObj.IsValid()) continue;

					FACPModelInfo ModelInfo;
					OptObj->TryGetStringField(TEXT("value"), ModelInfo.ModelId);
					OptObj->TryGetStringField(TEXT("name"), ModelInfo.Name);
					OptObj->TryGetStringField(TEXT("description"), ModelInfo.Description);
					SessionModelState.AvailableModels.Add(ModelInfo);
				}

				UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: config_option_update - %d models, current: %s"),
					SessionModelState.AvailableModels.Num(), *CurrentValue);
			}
			else if (Category == TEXT("mode") && OptionsArray)
			{
				// Convert to SessionModeState for existing mode selector UI
				bFoundModes = true;
				SessionModeState.AvailableModes.Empty();
				SessionModeState.CurrentModeId = CurrentValue;

				for (const TSharedPtr<FJsonValue>& OptValue : *OptionsArray)
				{
					TSharedPtr<FJsonObject> OptObj = OptValue->AsObject();
					if (!OptObj.IsValid()) continue;

					FACPSessionMode ModeInfo;
					OptObj->TryGetStringField(TEXT("value"), ModeInfo.ModeId);
					OptObj->TryGetStringField(TEXT("name"), ModeInfo.Name);
					OptObj->TryGetStringField(TEXT("description"), ModeInfo.Description);
					SessionModeState.AvailableModes.Add(ModeInfo);
				}

				UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: config_option_update - %d modes, current: %s"),
					SessionModeState.AvailableModes.Num(), *CurrentValue);
			}
			// ACP spec: boolean config options (UNSTABLE)
			FString OptionType;
			OptionObj->TryGetStringField(TEXT("type"), OptionType);
			if (OptionType == TEXT("boolean"))
			{
				bool bCurrentBool = false;
				OptionObj->TryGetBoolField(TEXT("currentValue"), bCurrentBool);
				BooleanConfigOptions.Add(OptionId, bCurrentBool);
				UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Boolean config option '%s' = %s"),
					*OptionId, bCurrentBool ? TEXT("true") : TEXT("false"));
				continue; // Boolean options don't have select options array
			}

			if (Category == TEXT("thought_level") && OptionsArray)
			{
				bFoundReasoningConfig = true;
				// Reasoning effort options for agents like Codex
				ReasoningConfigOptionId = OptionId.IsEmpty() ? TEXT("thinking") : OptionId;
				AvailableReasoningEfforts.Empty();
				CurrentReasoningEffort = CurrentValue;

				for (const TSharedPtr<FJsonValue>& OptValue : *OptionsArray)
				{
					TSharedPtr<FJsonObject> OptObj = OptValue->AsObject();
					if (!OptObj.IsValid()) continue;

					FString Value;
					OptObj->TryGetStringField(TEXT("value"), Value);
					if (!Value.IsEmpty())
					{
						AvailableReasoningEfforts.Add(Value);
					}
				}

				UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: config_option_update - %d reasoning options, current: %s"),
					AvailableReasoningEfforts.Num(), *CurrentValue);

				// Reapply persisted reasoning level if available and supported.
					if (UACPSettings* Settings = UACPSettings::Get())
					{
						FString SavedReasoning = Settings->GetSavedReasoningForAgent(CurrentConfig.AgentName);
						if (!SavedReasoning.IsEmpty())
						{
							FString SavedThinkingValue = SavedReasoning == TEXT("none") ? TEXT("off") : SavedReasoning;
							if (AvailableReasoningEfforts.Contains(SavedThinkingValue)
								&& SavedThinkingValue != CurrentReasoningEffort
								&& !CurrentSessionId.IsEmpty())
							{
								SetReasoningEffort(SavedThinkingValue);
							}
						}
				}
			}
		}

		if (!bFoundReasoningConfig)
		{
			AvailableReasoningEfforts.Empty();
			CurrentReasoningEffort.Empty();
			ReasoningConfigOptionId.Empty();
		}

		// Broadcast AFTER all configOptions are parsed so reasoning
		// state is set before the models push checks SupportsReasoningEffortControl()
		if (bFoundModels)
		{
			OnModelsAvailable.Broadcast(SessionModelState);
		}
		if (bFoundModes)
		{
			OnModesAvailable.Broadcast(SessionModeState);
		}

		return; // Don't broadcast as regular session update
	}
	else if (UpdateType == TEXT("session_info_update"))
	{
		// ACP spec: session metadata update (title, updatedAt)
		FString Title;
		if (UpdateObj->HasField(TEXT("content")))
		{
			TSharedPtr<FJsonObject> Content = UpdateObj->GetObjectField(TEXT("content"));
			if (Content.IsValid())
			{
				Content->TryGetStringField(TEXT("title"), Title);
			}
		}
		else
		{
			UpdateObj->TryGetStringField(TEXT("title"), Title);
		}

		if (!Title.IsEmpty() && !UnrealSessionId.IsEmpty())
		{
			FACPSessionManager::Get().UpdateSessionTitle(UnrealSessionId, Title);
			OnSessionInfoUpdated.Broadcast(UnrealSessionId, Title);
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Session title updated via agent: %s"), *Title);
		}
		return; // Don't broadcast as regular session update
	}
	else if (!UpdateType.IsEmpty())
	{
		// Log unknown update types so we can add support for them
		FString UpdateJson;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&UpdateJson);
		FJsonSerializer::Serialize(UpdateObj.ToSharedRef(), Writer);
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPClient: Unhandled session update type '%s': %s"), *UpdateType, *UpdateJson);
	}

	OnSessionUpdate.Broadcast(Update);
}

void FACPClient::SetState(EACPClientState NewState, const FString& Message)
{
	{
		FScopeLock Lock(&StateLock);
		State = NewState;
	}

	OnStateChanged.Broadcast(NewState, Message);
}

// ============================================================================
// ACP Protocol Methods
// ============================================================================

void FACPClient::Initialize()
{
	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Sending initialize request..."));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	// Protocol version (integer, currently version 1 per ACP spec)
	Params->SetNumberField(TEXT("protocolVersion"), 1);

	// Client info
	TSharedPtr<FJsonObject> ClientInfo = MakeShared<FJsonObject>();
	ClientInfo->SetStringField(TEXT("name"), TEXT("AgentIntegrationKit"));
	ClientInfo->SetStringField(TEXT("version"), TEXT("1.0.0"));
	Params->SetObjectField(TEXT("clientInfo"), ClientInfo);

	// Client capabilities (ACP spec v1)
	TSharedPtr<FJsonObject> Capabilities = MakeShared<FJsonObject>();

	// ACP spec: capabilities.fs (not "fileSystem")
	TSharedPtr<FJsonObject> Fs = MakeShared<FJsonObject>();
	Fs->SetBoolField(TEXT("readTextFile"), ClientCapabilities.bSupportsFileSystem);
	Fs->SetBoolField(TEXT("writeTextFile"), ClientCapabilities.bSupportsFileSystem);
	Capabilities->SetObjectField(TEXT("fs"), Fs);

	// ACP spec: capabilities.terminal is a boolean (not an object)
	Capabilities->SetBoolField(TEXT("terminal"), ClientCapabilities.bSupportsTerminal);

	// ACP spec: capabilities.auth.terminal — advertise terminal auth support
	if (ClientCapabilities.bSupportsAuthTerminal)
	{
		TSharedPtr<FJsonObject> Auth = MakeShared<FJsonObject>();
		Auth->SetBoolField(TEXT("terminal"), true);
		Capabilities->SetObjectField(TEXT("auth"), Auth);
	}

	Params->SetObjectField(TEXT("capabilities"), Capabilities);

	SendRequest(TEXT("initialize"), Params);
}

void FACPClient::NewSession(const FString& WorkingDirectory)
{
	// Reset usage tracking for new session
	SessionUsage = FACPUsageData();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	// Use absolute path for cwd, strip trailing slash for consistent matching
	FString AbsolutePath = FPaths::ConvertRelativePathToFull(WorkingDirectory);
	while (AbsolutePath.Len() > 1 && (AbsolutePath.EndsWith(TEXT("/")) || AbsolutePath.EndsWith(TEXT("\\"))))
	{
		AbsolutePath.LeftChopInline(1);
	}
	Params->SetStringField(TEXT("cwd"), AbsolutePath);

	// Build mcpServers array
	TArray<TSharedPtr<FJsonValue>> McpServers;

	// Add Unreal MCP server if running
	if (FMCPServer::Get().IsRunning())
	{
		TSharedPtr<FJsonObject> UnrealMcp = MakeShared<FJsonObject>();
		UnrealMcp->SetStringField(TEXT("name"), TEXT("unreal-editor"));
		UnrealMcp->SetStringField(TEXT("type"), TEXT("http"));
		// Use 127.0.0.1 instead of localhost — Node.js fetch() resolves localhost via DNS,
		// which may try IPv6 (::1) first on some systems. The Unreal HTTP server only binds IPv4.
		UnrealMcp->SetStringField(TEXT("url"),
			FString::Printf(TEXT("http://127.0.0.1:%d/mcp"), FMCPServer::Get().GetPort()));

		TArray<TSharedPtr<FJsonValue>> EmptyHeaders;
		UnrealMcp->SetArrayField(TEXT("headers"), EmptyHeaders);

		McpServers.Add(MakeShared<FJsonValueObject>(UnrealMcp));

		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Adding Unreal MCP server to session: %s"),
			*UnrealMcp->GetStringField(TEXT("url")));
	}

	Params->SetArrayField(TEXT("mcpServers"), McpServers);

	// Build _meta object
	TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
	bool bHasMeta = false;

	// Reset first-prompt tracking for new session
	bFirstPromptSent = false;

	// Add custom system prompt via _meta only if delivery mode is SessionMeta
	UACPSettings* Settings = UACPSettings::Get();
	if (Settings)
	{
		ESystemPromptDelivery Delivery = Settings->GetSystemPromptDeliveryForAgent(CurrentConfig.AgentName);
		if (Delivery == ESystemPromptDelivery::SessionMeta)
		{
			FString EffectivePrompt = Settings->GetProfileSystemPromptAppend();
			if (!EffectivePrompt.IsEmpty())
			{
				TSharedPtr<FJsonObject> SystemPrompt = MakeShared<FJsonObject>();
				SystemPrompt->SetStringField(TEXT("append"), EffectivePrompt);
				Meta->SetObjectField(TEXT("systemPrompt"), SystemPrompt);
				bHasMeta = true;

				UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Adding custom system prompt via _meta (SessionMeta mode)"));
			}
		}
		else
		{
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: System prompt will be delivered via user message (mode=%d) for agent '%s'"),
				(int32)Delivery, *CurrentConfig.AgentName);
		}
	}

	// Add Claude Code options (thinking tokens, etc.)
	if (MaxThinkingTokens > 0)
	{
		TSharedPtr<FJsonObject> ClaudeCodeObj = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> OptionsObj = MakeShared<FJsonObject>();
		OptionsObj->SetNumberField(TEXT("maxThinkingTokens"), MaxThinkingTokens);
		ClaudeCodeObj->SetObjectField(TEXT("options"), OptionsObj);
		Meta->SetObjectField(TEXT("claudeCode"), ClaudeCodeObj);
		bHasMeta = true;

		UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Setting maxThinkingTokens=%d for session"), MaxThinkingTokens);
	}

	if (bHasMeta)
	{
		Params->SetObjectField(TEXT("_meta"), Meta);
	}

	SendRequest(TEXT("session/new"), Params);
}

void FACPClient::LoadSession(const FString& SessionId, const FString& WorkingDirectory)
{
	// Set CurrentSessionId before sending — session/load response does not return sessionId
	CurrentSessionId = SessionId;
	bFirstPromptSent = false;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("sessionId"), SessionId);

	FString AbsolutePath = FPaths::ConvertRelativePathToFull(WorkingDirectory);
	// Strip trailing slash — session cwds don't have one
	while (AbsolutePath.Len() > 1 && (AbsolutePath.EndsWith(TEXT("/")) || AbsolutePath.EndsWith(TEXT("\\"))))
	{
		AbsolutePath.LeftChopInline(1);
	}
	Params->SetStringField(TEXT("cwd"), AbsolutePath);

	if (FMCPServer::Get().IsRunning())
	{
		TArray<TSharedPtr<FJsonValue>> McpServers;
		TSharedPtr<FJsonObject> UnrealMcp = MakeShared<FJsonObject>();
		UnrealMcp->SetStringField(TEXT("name"), TEXT("unreal-editor"));
		UnrealMcp->SetStringField(TEXT("type"), TEXT("http"));
		UnrealMcp->SetStringField(TEXT("url"),
			FString::Printf(TEXT("http://127.0.0.1:%d/mcp"), FMCPServer::Get().GetPort()));
		TArray<TSharedPtr<FJsonValue>> EmptyHeaders;
		UnrealMcp->SetArrayField(TEXT("headers"), EmptyHeaders);
		McpServers.Add(MakeShared<FJsonValueObject>(UnrealMcp));
		Params->SetArrayField(TEXT("mcpServers"), McpServers);
	}

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Loading session %s"), *SessionId);
	SendRequest(TEXT("session/load"), Params);
}

void FACPClient::ResumeSession(const FString& SessionId, const FString& WorkingDirectory)
{
	// Set CurrentSessionId before sending — the session/resume response does NOT return
	// a sessionId per ACP spec (the client already has it)
	CurrentSessionId = SessionId;
	bFirstPromptSent = false;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("sessionId"), SessionId);

	FString AbsolutePath = FPaths::ConvertRelativePathToFull(WorkingDirectory);
	// Strip trailing slash — session cwds don't have one
	while (AbsolutePath.Len() > 1 && (AbsolutePath.EndsWith(TEXT("/")) || AbsolutePath.EndsWith(TEXT("\\"))))
	{
		AbsolutePath.LeftChopInline(1);
	}
	Params->SetStringField(TEXT("cwd"), AbsolutePath);

	if (FMCPServer::Get().IsRunning())
	{
		TArray<TSharedPtr<FJsonValue>> McpServers;
		TSharedPtr<FJsonObject> UnrealMcp = MakeShared<FJsonObject>();
		UnrealMcp->SetStringField(TEXT("name"), TEXT("unreal-editor"));
		UnrealMcp->SetStringField(TEXT("type"), TEXT("http"));
		UnrealMcp->SetStringField(TEXT("url"),
			FString::Printf(TEXT("http://127.0.0.1:%d/mcp"), FMCPServer::Get().GetPort()));
		TArray<TSharedPtr<FJsonValue>> EmptyHeaders;
		UnrealMcp->SetArrayField(TEXT("headers"), EmptyHeaders);
		McpServers.Add(MakeShared<FJsonValueObject>(UnrealMcp));
		Params->SetArrayField(TEXT("mcpServers"), McpServers);
	}

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Resuming session %s"), *SessionId);
	SendRequest(TEXT("session/resume"), Params);
}

void FACPClient::SendPrompt(const FString& PromptText)
{
	SetState(EACPClientState::Prompting, TEXT("Processing..."));

	// Analytics: record prompt sent (no content, just agent name and timestamp)
	AnalyticsPromptStartTime = FPlatformTime::Seconds();
	{
		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		Props->SetStringField(TEXT("agent"), CurrentConfig.AgentName);
		FAIKAnalytics::Get().RecordEvent(TEXT("prompt_sent"), Props);
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	// Session ID is required
	Params->SetStringField(TEXT("sessionId"), CurrentSessionId);

	// Build prompt content blocks
	TArray<TSharedPtr<FJsonValue>> Prompt;

	// Prepend system prompt as user message content if delivery mode requires it
	UACPSettings* Settings = UACPSettings::Get();
	if (Settings)
	{
		ESystemPromptDelivery Delivery = Settings->GetSystemPromptDeliveryForAgent(CurrentConfig.AgentName);
		bool bShouldPrepend = (Delivery == ESystemPromptDelivery::EveryUserMessage)
			|| (Delivery == ESystemPromptDelivery::FirstUserMessage && !bFirstPromptSent);

		if (bShouldPrepend)
		{
			FString EffectivePrompt = Settings->GetProfileSystemPromptAppend();
			if (!EffectivePrompt.IsEmpty())
			{
				TSharedPtr<FJsonObject> SysBlock = MakeShared<FJsonObject>();
				SysBlock->SetStringField(TEXT("type"), TEXT("text"));
				SysBlock->SetStringField(TEXT("text"),
					TEXT("<system-instructions>\n") + EffectivePrompt + TEXT("\n</system-instructions>"));

				// Mark as assistant-only audience so it doesn't render as user text in the agent's UI
				TSharedPtr<FJsonObject> Annotations = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> Audience;
				Audience.Add(MakeShared<FJsonValueString>(TEXT("assistant")));
				Annotations->SetArrayField(TEXT("audience"), Audience);
				SysBlock->SetObjectField(TEXT("annotations"), Annotations);

				Prompt.Add(MakeShared<FJsonValueObject>(SysBlock));

				UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Prepended system prompt to user message (mode=%d, firstPrompt=%d)"),
					(int32)Delivery, !bFirstPromptSent);
			}
		}
	}
	bFirstPromptSent = true;

	// Add attachment context blocks FIRST (before user text)
	TArray<TSharedPtr<FJsonValue>> AttachmentBlocks = FACPAttachmentManager::Get().SerializeForPrompt();
	if (AttachmentBlocks.Num() > 0)
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Adding %d attachment blocks to prompt"), AttachmentBlocks.Num());
		for (const TSharedPtr<FJsonValue>& Block : AttachmentBlocks)
		{
			Prompt.Add(Block);
		}
	}

	// Add user text block
	TSharedPtr<FJsonObject> TextBlock = MakeShared<FJsonObject>();
	TextBlock->SetStringField(TEXT("type"), TEXT("text"));
	TextBlock->SetStringField(TEXT("text"), PromptText);
	Prompt.Add(MakeShared<FJsonValueObject>(TextBlock));

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Sending prompt with %d content blocks"), Prompt.Num());

	Params->SetArrayField(TEXT("prompt"), Prompt);

	SendRequest(TEXT("session/prompt"), Params);

	// Clear attachments after sending (one-shot context)
	FACPAttachmentManager::Get().ClearAllAttachments();
}

void FACPClient::CancelPrompt()
{
	if (CurrentSessionId.IsEmpty())
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPClient: Cannot cancel - no active session"));
		return;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("sessionId"), CurrentSessionId);

	// Cancel is a notification (no response expected)
	SendNotification(TEXT("session/cancel"), Params);
	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Sent cancel request for session %s"), *CurrentSessionId);
}

void FACPClient::CloseSession(const FString& SessionId)
{
	if (SessionId.IsEmpty())
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPClient: Cannot close session - no session ID"));
		return;
	}

	if (!AgentCapabilities.bSupportsCloseSession)
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Agent does not support session/close, skipping"));
		return;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("sessionId"), SessionId);

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Closing session %s"), *SessionId);
	SendRequest(TEXT("session/close"), Params);
}

void FACPClient::DeleteSession(const FString& SessionId)
{
	if (SessionId.IsEmpty())
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPClient: Cannot delete session - no session ID"));
		return;
	}

	if (!AgentCapabilities.bSupportsDeleteSession)
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Agent does not support session/delete, skipping"));
		return;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("sessionId"), SessionId);

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Deleting session %s"), *SessionId);
	SendRequest(TEXT("session/delete"), Params);
}

void FACPClient::SetMode(const FString& ModeId)
{
	if (CurrentSessionId.IsEmpty())
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPClient: Cannot set mode - no active session"));
		return;
	}

	// Use unified config option for agents that support it (Codex), old method for others (Claude Code)
	if (bUsesConfigOptions)
	{
		SetConfigOption(TEXT("mode"), ModeId);
		return;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("sessionId"), CurrentSessionId);
	Params->SetStringField(TEXT("modeId"), ModeId);

	SendRequest(TEXT("session/set_mode"), Params);
	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Setting session mode to %s"), *ModeId);
}

void FACPClient::SetModel(const FString& ModelId)
{
	if (CurrentSessionId.IsEmpty())
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPClient: Cannot set model - no active session"));
		return;
	}

	// Gemini CLI ACP currently does not implement session/set_model or
	// session/set_config_option for model changes. Model is selected at launch.
	if (CurrentConfig.AgentName == TEXT("Gemini CLI") && !bUsesConfigOptions)
	{
		SessionModelState.CurrentModelId = ModelId;
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Gemini model set to '%s' for next new connection/session"), *ModelId);
		return;
	}

	// Use unified config option for agents that support it (Codex), old method for others (Claude Code)
	if (bUsesConfigOptions)
	{
		SetConfigOption(TEXT("model"), ModelId);
		SessionModelState.CurrentModelId = ModelId;
		return;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("sessionId"), CurrentSessionId);
	Params->SetStringField(TEXT("modelId"), ModelId);

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Setting model to: %s"), *ModelId);
	SendRequest(TEXT("session/set_model"), Params);

	// Update local state
	SessionModelState.CurrentModelId = ModelId;
}

void FACPClient::SetMaxThinkingTokens(int32 Tokens)
{
	MaxThinkingTokens = Tokens;

	// Send to adapter if we have an active session
	if (!CurrentSessionId.IsEmpty())
	{
		FString Value;
		if (Tokens <= 0)       Value = TEXT("off");
		else if (Tokens <= 2000) Value = TEXT("low");
		else if (Tokens <= 4000) Value = TEXT("medium");
		else                     Value = TEXT("high");

		SetReasoningEffort(Value);
		UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Sent thinking level '%s' (tokens=%d) to adapter"), *Value, Tokens);
	}
}

void FACPClient::SetReasoningEffort(const FString& Value)
{
	if (CurrentSessionId.IsEmpty())
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPClient: Cannot set reasoning effort - no active session"));
		return;
	}

	if (!SupportsReasoningEffortControl())
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Ignoring reasoning effort '%s' - no reasoning config option available"), *Value);
		return;
	}

	if (!AvailableReasoningEfforts.Contains(Value))
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Ignoring unsupported reasoning effort '%s' for option '%s'"), *Value, *ReasoningConfigOptionId);
		return;
	}

	SetConfigOption(ReasoningConfigOptionId, Value);
}

void FACPClient::Authenticate(const FString& MethodId)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("methodId"), MethodId);

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Sending authenticate with method %s"), *MethodId);
	SendRequest(TEXT("authenticate"), Params);
}

void FACPClient::ListSessions(const FString& WorkingDirectory, const FString& Cursor)
{
	// First page of a new request: reset pagination accumulator
	if (Cursor.IsEmpty())
	{
		PaginatedSessionAccumulator.Empty();
		PaginatedSessionCwd = WorkingDirectory;
		LastPaginationCursor.Empty();
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	// Don't send cwd in session/list — matches Zed's behavior.
	// codex-acp's list_threads has issues with cwd filtering (sessions created
	// via ACP can be missed). Instead, fetch ALL sessions and filter on our side.

	// ACP spec: cursor-based pagination
	if (!Cursor.IsEmpty())
	{
		Params->SetStringField(TEXT("cursor"), Cursor);
	}

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Listing sessions (no server-side cwd filter, client filter='%s') cursor=%s"),
		*WorkingDirectory, *Cursor);
	SendRequest(TEXT("session/list"), Params);
}

void FACPClient::SetConfigOption(const FString& ConfigId, const FString& Value)
{
	if (CurrentSessionId.IsEmpty())
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPClient: Cannot set config option - no active session"));
		return;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("sessionId"), CurrentSessionId);
	Params->SetStringField(TEXT("configId"), ConfigId);
	Params->SetStringField(TEXT("value"), Value);

	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Setting config option %s = %s"), *ConfigId, *Value);
	SendRequest(TEXT("session/set_config_option"), Params);
}

void FACPClient::HandleServerRequest(int32 Id, const FString& Method, TSharedPtr<FJsonObject> Params)
{
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Server request - Method: %s, Id: %d"), *Method, Id);

	if (Method == TEXT("session/request_permission"))
	{
		// Parse permission request
		FACPPermissionRequest PermRequest;
		PermRequest.RequestId = Id;

		if (Params->HasField(TEXT("sessionId")))
		{
			PermRequest.SessionId = Params->GetStringField(TEXT("sessionId"));
		}

		// Parse options
		if (Params->HasField(TEXT("options")))
		{
			const TArray<TSharedPtr<FJsonValue>>& OptionsArray = Params->GetArrayField(TEXT("options"));
			for (const TSharedPtr<FJsonValue>& OptionVal : OptionsArray)
			{
				TSharedPtr<FJsonObject> OptionObj = OptionVal->AsObject();
				if (OptionObj.IsValid())
				{
					FACPPermissionOption Option;
					OptionObj->TryGetStringField(TEXT("optionId"), Option.OptionId);
					OptionObj->TryGetStringField(TEXT("name"), Option.Name);
					OptionObj->TryGetStringField(TEXT("kind"), Option.Kind);
					PermRequest.Options.Add(Option);
				}
			}
		}

		// Parse tool call info
		if (Params->HasField(TEXT("toolCall")))
		{
			TSharedPtr<FJsonObject> ToolCallObj = Params->GetObjectField(TEXT("toolCall"));
			if (ToolCallObj.IsValid())
			{
				ToolCallObj->TryGetStringField(TEXT("toolCallId"), PermRequest.ToolCall.ToolCallId);
				ToolCallObj->TryGetStringField(TEXT("title"), PermRequest.ToolCall.Title);

				// Serialize rawInput to string
				if (ToolCallObj->HasField(TEXT("rawInput")))
				{
					TSharedPtr<FJsonObject> RawInput = ToolCallObj->GetObjectField(TEXT("rawInput"));
					if (RawInput.IsValid())
					{
						FString RawInputStr;
						TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RawInputStr);
						FJsonSerializer::Serialize(RawInput.ToSharedRef(), Writer);
						PermRequest.ToolCall.RawInput = RawInputStr;
					}
				}
			}
		}

		// Parse _meta for AskUserQuestion support
		if (Params->HasField(TEXT("_meta")))
		{
			TSharedPtr<FJsonObject> Meta = Params->GetObjectField(TEXT("_meta"));
			if (Meta.IsValid() && Meta->HasField(TEXT("askUserQuestion")))
			{
				PermRequest.bIsAskUserQuestion = true;
				TSharedPtr<FJsonObject> AskObj = Meta->GetObjectField(TEXT("askUserQuestion"));
				if (AskObj.IsValid() && AskObj->HasField(TEXT("questions")))
				{
					const TArray<TSharedPtr<FJsonValue>>& QuestionsArray = AskObj->GetArrayField(TEXT("questions"));
					for (const TSharedPtr<FJsonValue>& QVal : QuestionsArray)
					{
						TSharedPtr<FJsonObject> QObj = QVal->AsObject();
						if (!QObj.IsValid()) continue;

						FACPQuestion Question;
						QObj->TryGetStringField(TEXT("question"), Question.Question);
						QObj->TryGetStringField(TEXT("header"), Question.Header);
						QObj->TryGetBoolField(TEXT("multiSelect"), Question.bMultiSelect);

						if (QObj->HasField(TEXT("options")))
						{
							const TArray<TSharedPtr<FJsonValue>>& OptsArray = QObj->GetArrayField(TEXT("options"));
							for (const TSharedPtr<FJsonValue>& OptVal : OptsArray)
							{
								TSharedPtr<FJsonObject> OptObj = OptVal->AsObject();
								if (!OptObj.IsValid()) continue;

								FACPQuestionOption Opt;
								OptObj->TryGetStringField(TEXT("label"), Opt.Label);
								OptObj->TryGetStringField(TEXT("description"), Opt.Description);
								Question.Options.Add(Opt);
							}
						}

						PermRequest.Questions.Add(Question);
					}
				}
			}
		}

		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Permission request for tool: %s (AskUser=%d)"), *PermRequest.ToolCall.Title, PermRequest.bIsAskUserQuestion ? 1 : 0);

		// Broadcast to UI (already on game thread from ProcessLine dispatch)
		OnPermissionRequest.Broadcast(PermRequest);
	}
	else if (Method == TEXT("session/elicitation"))
	{
		// ACP spec (UNSTABLE): agent requests structured input from client
		FACPElicitationRequest Elicitation;
		Elicitation.RequestId = Id;
		Params->TryGetStringField(TEXT("sessionId"), Elicitation.SessionId);
		Params->TryGetStringField(TEXT("mode"), Elicitation.Mode);
		Params->TryGetStringField(TEXT("message"), Elicitation.Message);

		if (Elicitation.Mode == TEXT("form") && Params->HasField(TEXT("requestedSchema")))
		{
			TSharedPtr<FJsonObject> Schema = Params->GetObjectField(TEXT("requestedSchema"));
			if (Schema.IsValid())
			{
				FString SchemaStr;
				TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&SchemaStr);
				FJsonSerializer::Serialize(Schema.ToSharedRef(), Writer);
				Elicitation.RequestedSchema = SchemaStr;
			}
		}
		else if (Elicitation.Mode == TEXT("url"))
		{
			Params->TryGetStringField(TEXT("url"), Elicitation.Url);
			Params->TryGetStringField(TEXT("elicitationId"), Elicitation.ElicitationId);
		}

		UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Elicitation request (mode=%s): %s"), *Elicitation.Mode, *Elicitation.Message);
		OnElicitationRequest.Broadcast(Elicitation);
	}
	else
	{
		UE_LOG(LogAgentIntegrationKit, Warning, TEXT("ACPClient: Unknown server request method: %s"), *Method);
	}
}

void FACPClient::RespondToElicitation(int32 RequestId, const FString& Action, TSharedPtr<FJsonObject> Content)
{
	UE_LOG(LogAgentIntegrationKit, Log, TEXT("ACPClient: Responding to elicitation %d with action: %s"), RequestId, *Action);

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Response->SetNumberField(TEXT("id"), RequestId);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("action"), Action); // "accept" | "decline" | "cancel"
	if (Content.IsValid())
	{
		Result->SetObjectField(TEXT("content"), Content);
	}

	Response->SetObjectField(TEXT("result"), Result);

	FString ResponseStr;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ResponseStr);
	FJsonSerializer::Serialize(Response.ToSharedRef(), Writer);
	SendRawMessage(ResponseStr);
}

void FACPClient::RespondToPermissionRequest(int32 RequestId, const FString& OptionId, TSharedPtr<FJsonObject> OutcomeMeta)
{
	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Responding to permission request %d with option: %s"), RequestId, *OptionId);

	// Build JSON-RPC response
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Response->SetNumberField(TEXT("id"), RequestId);

	// Build result with correct ACP format: { outcome: { outcome: "selected", optionId: "<id>" } }
	// See claude-code-acp/src/tests/acp-agent.test.ts for reference
	TSharedPtr<FJsonObject> OutcomeInner = MakeShared<FJsonObject>();
	OutcomeInner->SetStringField(TEXT("outcome"), TEXT("selected"));
	OutcomeInner->SetStringField(TEXT("optionId"), OptionId);

	// Attach _meta if provided (used for AskUserQuestion answers)
	if (OutcomeMeta.IsValid())
	{
		OutcomeInner->SetObjectField(TEXT("_meta"), OutcomeMeta);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("outcome"), OutcomeInner);

	Response->SetObjectField(TEXT("result"), Result);

	// Serialize and send
	FString ResponseStr;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ResponseStr);
	FJsonSerializer::Serialize(Response.ToSharedRef(), Writer);

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("ACPClient: Sending permission response: %s"), *ResponseStr);
	SendRawMessage(ResponseStr);
}
