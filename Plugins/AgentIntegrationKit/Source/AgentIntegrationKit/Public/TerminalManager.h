// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/Base64.h"

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnTerminalOutput, const FString& /*TerminalId*/, const FString& /*Base64Data*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnTerminalExit, const FString& /*TerminalId*/, int32 /*ExitCode*/);

// Forward declaration — defined below FTerminalSession
class FTerminalReaderRunnable;

/**
 * A single terminal session backed by a platform PTY.
 */
struct FTerminalSession
{
	FString TerminalId;
	FString WorkingDirectory;
	FString Shell;

#if PLATFORM_MAC || PLATFORM_LINUX
	int32 MasterFd = -1;
	pid_t ChildPid = -1;
#elif PLATFORM_WINDOWS
	void* PseudoConsole = nullptr;   // HPCON
	void* ProcessHandle = nullptr;   // HANDLE
	void* ReadPipe = nullptr;        // HANDLE
	void* WritePipe = nullptr;       // HANDLE
#endif

	FRunnableThread* ReaderThread = nullptr;
	FTerminalReaderRunnable* ReaderRunnable = nullptr; // Owned — deleted in CleanupSession
	TAtomic<bool> bStopRequested{false};
};

/**
 * Runnable that reads PTY output on a background thread and dispatches
 * via a delegate (which the bridge marshals to the game thread).
 */
class FTerminalReaderRunnable : public FRunnable
{
public:
	FTerminalReaderRunnable(FTerminalSession* InSession, FOnTerminalOutput* InOutputDelegate, FOnTerminalExit* InExitDelegate);

	virtual uint32 Run() override;
	virtual void Stop() override;

private:
	FTerminalSession* Session;
	FOnTerminalOutput* OutputDelegate;
	FOnTerminalExit* ExitDelegate;
};

/**
 * Singleton manager for terminal sessions.
 * Manages PTY lifecycle, I/O routing, and cleanup.
 */
class AGENTINTEGRATIONKIT_API FTerminalManager
{
public:
	static FTerminalManager& Get();

	/** Start a new terminal session. Returns the terminal ID, or empty on failure. */
	FString StartTerminal(const FString& WorkingDir, const FString& Shell);

	/** Write input data to a terminal (raw UTF-8 string from xterm.js onData). */
	bool WriteTerminal(const FString& TerminalId, const FString& Data);

	/** Resize terminal PTY. */
	bool ResizeTerminal(const FString& TerminalId, int32 Cols, int32 Rows);

	/** Close and clean up a terminal session. */
	void CloseTerminal(const FString& TerminalId);

	/** Close all open terminals (called on module shutdown). */
	void CloseAll();

	/** Delegates for output/exit events. */
	FOnTerminalOutput OnTerminalOutput;
	FOnTerminalExit OnTerminalExit;

	/** Maximum concurrent terminal sessions. */
	static constexpr int32 MaxTerminals = 8;

private:
	FTerminalManager() = default;

	/** Active terminal sessions. */
	TMap<FString, TUniquePtr<FTerminalSession>> Sessions;

	/** Mutex for session map access. */
	FCriticalSection SessionsLock;

	/** Prevents new sessions during shutdown. */
	TAtomic<bool> bShuttingDown{false};

	/** Platform-specific PTY creation. Returns true on success. */
	bool CreatePTY(FTerminalSession& Session);

	/** Clean up a single session (must hold lock or be sole accessor). */
	void CleanupSession(FTerminalSession& Session);
};
