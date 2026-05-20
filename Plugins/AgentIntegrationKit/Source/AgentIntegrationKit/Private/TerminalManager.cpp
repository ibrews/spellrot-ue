// Copyright 2026 Betide Studio. All Rights Reserved.

#include "TerminalManager.h"
#include "Async/Async.h"
#include "Misc/Guid.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#if PLATFORM_MAC
#include <util.h>
#else
#include <pty.h>
#endif
#elif PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

// ConPTY API — dynamically loaded since UE's Windows headers may not expose them
// (requires NTDDI_WIN10_RS5 / Windows 10 1809+)

// HPCON may not be typedef'd in UE's restricted Windows headers
#ifndef PSEUDOCONSOLE_INHERIT_CURSOR
typedef void* HPCON;
#endif

// PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE = ProcThreadAttributeValue(22, FALSE, TRUE, FALSE)
// Expanded: (22 & 0x0000FFFF) | (0x00020000) = 0x00020016
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

typedef HRESULT (WINAPI *PFN_CreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
typedef HRESULT (WINAPI *PFN_ResizePseudoConsole)(HPCON, COORD);
typedef void    (WINAPI *PFN_ClosePseudoConsole)(HPCON);

static PFN_CreatePseudoConsole GCreatePseudoConsole = nullptr;
static PFN_ResizePseudoConsole GResizePseudoConsole = nullptr;
static PFN_ClosePseudoConsole  GClosePseudoConsole = nullptr;
static bool GConPTYLoaded = false;

static bool LoadConPTY()
{
	if (GConPTYLoaded) return GCreatePseudoConsole != nullptr;
	GConPTYLoaded = true;

	HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
	if (!Kernel32) return false;

	GCreatePseudoConsole = (PFN_CreatePseudoConsole)GetProcAddress(Kernel32, "CreatePseudoConsole");
	GResizePseudoConsole = (PFN_ResizePseudoConsole)GetProcAddress(Kernel32, "ResizePseudoConsole");
	GClosePseudoConsole  = (PFN_ClosePseudoConsole)GetProcAddress(Kernel32, "ClosePseudoConsole");

	return GCreatePseudoConsole && GResizePseudoConsole && GClosePseudoConsole;
}
#endif

DEFINE_LOG_CATEGORY_STATIC(LogTerminal, Log, All);

// ── FTerminalReaderRunnable ────────────────────────────────────────

FTerminalReaderRunnable::FTerminalReaderRunnable(
	FTerminalSession* InSession,
	FOnTerminalOutput* InOutputDelegate,
	FOnTerminalExit* InExitDelegate)
	: Session(InSession)
	, OutputDelegate(InOutputDelegate)
	, ExitDelegate(InExitDelegate)
{
}

void FTerminalReaderRunnable::Stop()
{
	if (Session)
	{
		Session->bStopRequested.Store(true);
	}
}

uint32 FTerminalReaderRunnable::Run()
{
#if PLATFORM_MAC || PLATFORM_LINUX
	constexpr int32 ReadBufSize = 8192;
	uint8 ReadBuf[ReadBufSize];

	// Accumulation buffer for batching output (~16ms worth)
	TArray<uint8> AccumBuffer;
	AccumBuffer.Reserve(32768);
	double LastFlushTime = FPlatformTime::Seconds();
	constexpr double FlushIntervalSec = 0.016; // ~60fps

	while (!Session->bStopRequested.Load())
	{
		// Use select() with a short timeout so we can check bStopRequested
		fd_set ReadSet;
		FD_ZERO(&ReadSet);
		FD_SET(Session->MasterFd, &ReadSet);

		struct timeval Timeout;
		Timeout.tv_sec = 0;
		Timeout.tv_usec = 16000; // 16ms

		int SelectResult = select(Session->MasterFd + 1, &ReadSet, nullptr, nullptr, &Timeout);

		if (SelectResult > 0 && FD_ISSET(Session->MasterFd, &ReadSet))
		{
			ssize_t BytesRead = read(Session->MasterFd, ReadBuf, ReadBufSize);
			if (BytesRead > 0)
			{
				AccumBuffer.Append(ReadBuf, BytesRead);
			}
			else if (BytesRead == 0 || (BytesRead < 0 && errno != EAGAIN && errno != EINTR))
			{
				// PTY closed or error — flush remaining and exit
				break;
			}
		}

		// Flush accumulated buffer periodically or if it's getting large
		double Now = FPlatformTime::Seconds();
		if (AccumBuffer.Num() > 0 && (Now - LastFlushTime >= FlushIntervalSec || AccumBuffer.Num() > 32768))
		{
			FString Base64 = FBase64::Encode(AccumBuffer.GetData(), AccumBuffer.Num());
			FString TermId = Session->TerminalId;

			AsyncTask(ENamedThreads::GameThread, [this, TermId, Base64]()
			{
				if (OutputDelegate)
				{
					OutputDelegate->Broadcast(TermId, Base64);
				}
			});

			AccumBuffer.Reset();
			LastFlushTime = Now;
		}
	}

	// Flush any remaining data
	if (AccumBuffer.Num() > 0)
	{
		FString Base64 = FBase64::Encode(AccumBuffer.GetData(), AccumBuffer.Num());
		FString TermId = Session->TerminalId;

		AsyncTask(ENamedThreads::GameThread, [this, TermId, Base64]()
		{
			if (OutputDelegate)
			{
				OutputDelegate->Broadcast(TermId, Base64);
			}
		});
	}

	// Wait for child process and get exit code
	int32 ExitCode = -1;
	if (Session->ChildPid > 0)
	{
		int Status = 0;
		waitpid(Session->ChildPid, &Status, WNOHANG);
		if (WIFEXITED(Status))
		{
			ExitCode = WEXITSTATUS(Status);
		}
	}

	FString TermId = Session->TerminalId;
	AsyncTask(ENamedThreads::GameThread, [this, TermId, ExitCode]()
	{
		if (ExitDelegate)
		{
			ExitDelegate->Broadcast(TermId, ExitCode);
		}
	});

#elif PLATFORM_WINDOWS
	constexpr DWORD ReadBufSize = 8192;
	uint8 ReadBuf[ReadBufSize];

	TArray<uint8> AccumBuffer;
	AccumBuffer.Reserve(32768);
	double LastFlushTime = FPlatformTime::Seconds();
	constexpr double FlushIntervalSec = 0.016;

	while (!Session->bStopRequested.Load())
	{
		DWORD BytesAvailable = 0;
		if (PeekNamedPipe(Session->ReadPipe, nullptr, 0, nullptr, &BytesAvailable, nullptr) && BytesAvailable > 0)
		{
			DWORD BytesRead = 0;
			DWORD ToRead = FMath::Min(BytesAvailable, ReadBufSize);
			if (ReadFile(Session->ReadPipe, ReadBuf, ToRead, &BytesRead, nullptr) && BytesRead > 0)
			{
				AccumBuffer.Append(ReadBuf, BytesRead);
			}
		}
		else
		{
			// Check if process is still alive
			DWORD WaitResult = WaitForSingleObject(Session->ProcessHandle, 0);
			if (WaitResult == WAIT_OBJECT_0)
			{
				// Process exited — drain remaining output
				DWORD BytesRead = 0;
				while (PeekNamedPipe(Session->ReadPipe, nullptr, 0, nullptr, &BytesAvailable, nullptr) && BytesAvailable > 0)
				{
					DWORD ToRead = FMath::Min(BytesAvailable, ReadBufSize);
					if (ReadFile(Session->ReadPipe, ReadBuf, ToRead, &BytesRead, nullptr) && BytesRead > 0)
					{
						AccumBuffer.Append(ReadBuf, BytesRead);
					}
				}
				break;
			}
			FPlatformProcess::Sleep(0.016f);
		}

		double Now = FPlatformTime::Seconds();
		if (AccumBuffer.Num() > 0 && (Now - LastFlushTime >= FlushIntervalSec || AccumBuffer.Num() > 32768))
		{
			FString Base64 = FBase64::Encode(AccumBuffer.GetData(), AccumBuffer.Num());
			FString TermId = Session->TerminalId;

			AsyncTask(ENamedThreads::GameThread, [this, TermId, Base64]()
			{
				if (OutputDelegate)
				{
					OutputDelegate->Broadcast(TermId, Base64);
				}
			});

			AccumBuffer.Reset();
			LastFlushTime = Now;
		}
	}

	// Flush remaining
	if (AccumBuffer.Num() > 0)
	{
		FString Base64 = FBase64::Encode(AccumBuffer.GetData(), AccumBuffer.Num());
		FString TermId = Session->TerminalId;

		AsyncTask(ENamedThreads::GameThread, [this, TermId, Base64]()
		{
			if (OutputDelegate)
			{
				OutputDelegate->Broadcast(TermId, Base64);
			}
		});
	}

	int32 ExitCode = -1;
	if (Session->ProcessHandle)
	{
		DWORD WinExitCode = 0;
		GetExitCodeProcess(Session->ProcessHandle, &WinExitCode);
		ExitCode = static_cast<int32>(WinExitCode);
	}

	FString TermId = Session->TerminalId;
	AsyncTask(ENamedThreads::GameThread, [this, TermId, ExitCode]()
	{
		if (ExitDelegate)
		{
			ExitDelegate->Broadcast(TermId, ExitCode);
		}
	});
#endif

	return 0;
}

// ── FTerminalManager ───────────────────────────────────────────────

FTerminalManager& FTerminalManager::Get()
{
	static FTerminalManager Instance;
	return Instance;
}

FString FTerminalManager::StartTerminal(const FString& WorkingDir, const FString& Shell)
{
	if (bShuttingDown.Load())
	{
		UE_LOG(LogTerminal, Warning, TEXT("Cannot start terminal — shutdown in progress"));
		return FString();
	}

	{
		FScopeLock Lock(&SessionsLock);
		if (Sessions.Num() >= MaxTerminals)
		{
			UE_LOG(LogTerminal, Warning, TEXT("Cannot start terminal — max sessions reached (%d)"), MaxTerminals);
			return FString();
		}
	}

	auto Session = MakeUnique<FTerminalSession>();
	Session->TerminalId = FGuid::NewGuid().ToString();
	Session->WorkingDirectory = WorkingDir;
	Session->Shell = Shell;

	if (!CreatePTY(*Session))
	{
		UE_LOG(LogTerminal, Error, TEXT("Failed to create PTY for terminal session"));
		return FString();
	}

	FString TerminalId = Session->TerminalId;

	// Start reader thread — session owns both runnable and thread
	FTerminalReaderRunnable* Runnable = new FTerminalReaderRunnable(Session.Get(), &OnTerminalOutput, &OnTerminalExit);
	Session->ReaderRunnable = Runnable;
	Session->ReaderThread = FRunnableThread::Create(Runnable, *FString::Printf(TEXT("TerminalReader_%s"), *TerminalId.Left(8)));

	{
		FScopeLock Lock(&SessionsLock);
		Sessions.Add(TerminalId, MoveTemp(Session));
	}

	UE_LOG(LogTerminal, Log, TEXT("Terminal started: %s (shell: %s, cwd: %s)"), *TerminalId, *Shell, *WorkingDir);
	return TerminalId;
}

bool FTerminalManager::WriteTerminal(const FString& TerminalId, const FString& Data)
{
	FScopeLock Lock(&SessionsLock);
	TUniquePtr<FTerminalSession>* Found = Sessions.Find(TerminalId);
	if (!Found || !Found->IsValid())
	{
		return false;
	}

	FTerminalSession& Session = **Found;

#if PLATFORM_MAC || PLATFORM_LINUX
	if (Session.MasterFd < 0) return false;

	FTCHARToUTF8 Utf8(*Data);
	const char* Buf = Utf8.Get();
	int32 Len = Utf8.Length();
	int32 Written = 0;

	while (Written < Len)
	{
		ssize_t Result = write(Session.MasterFd, Buf + Written, Len - Written);
		if (Result < 0)
		{
			if (errno == EINTR || errno == EAGAIN) continue;
			return false;
		}
		Written += Result;
	}
	return true;

#elif PLATFORM_WINDOWS
	if (!Session.WritePipe) return false;

	FTCHARToUTF8 Utf8(*Data);
	DWORD BytesWritten = 0;
	return WriteFile(Session.WritePipe, Utf8.Get(), Utf8.Length(), &BytesWritten, nullptr) != 0;
#else
	return false;
#endif
}

bool FTerminalManager::ResizeTerminal(const FString& TerminalId, int32 Cols, int32 Rows)
{
	FScopeLock Lock(&SessionsLock);
	TUniquePtr<FTerminalSession>* Found = Sessions.Find(TerminalId);
	if (!Found || !Found->IsValid())
	{
		return false;
	}

	FTerminalSession& Session = **Found;

#if PLATFORM_MAC || PLATFORM_LINUX
	if (Session.MasterFd < 0) return false;

	struct winsize WS;
	WS.ws_col = static_cast<unsigned short>(Cols);
	WS.ws_row = static_cast<unsigned short>(Rows);
	WS.ws_xpixel = 0;
	WS.ws_ypixel = 0;
	return ioctl(Session.MasterFd, TIOCSWINSZ, &WS) == 0;

#elif PLATFORM_WINDOWS
	if (!Session.PseudoConsole) return false;

	COORD Size;
	Size.X = static_cast<SHORT>(Cols);
	Size.Y = static_cast<SHORT>(Rows);
	if (!GResizePseudoConsole) return false;
	HRESULT Hr = GResizePseudoConsole(static_cast<HPCON>(Session.PseudoConsole), Size);
	return SUCCEEDED(Hr);
#else
	return false;
#endif
}

void FTerminalManager::CloseTerminal(const FString& TerminalId)
{
	TUniquePtr<FTerminalSession> Session;

	{
		FScopeLock Lock(&SessionsLock);
		Sessions.RemoveAndCopyValue(TerminalId, Session);
	}

	if (Session.IsValid())
	{
		CleanupSession(*Session);
		UE_LOG(LogTerminal, Log, TEXT("Terminal closed: %s"), *TerminalId);
	}
}

void FTerminalManager::CloseAll()
{
	bShuttingDown.Store(true);

	TMap<FString, TUniquePtr<FTerminalSession>> Snapshot;

	{
		FScopeLock Lock(&SessionsLock);
		Snapshot = MoveTemp(Sessions);
		Sessions.Empty();
	}

	for (auto& Pair : Snapshot)
	{
		CleanupSession(*Pair.Value);
	}

	UE_LOG(LogTerminal, Log, TEXT("All terminals closed (%d sessions)"), Snapshot.Num());
}

bool FTerminalManager::CreatePTY(FTerminalSession& Session)
{
#if PLATFORM_MAC || PLATFORM_LINUX
	struct winsize WS;
	WS.ws_col = 80;
	WS.ws_row = 24;
	WS.ws_xpixel = 0;
	WS.ws_ypixel = 0;

	int MasterFd = -1;
	pid_t Pid = forkpty(&MasterFd, nullptr, nullptr, &WS);

	if (Pid < 0)
	{
		UE_LOG(LogTerminal, Error, TEXT("forkpty() failed: %s"), UTF8_TO_TCHAR(strerror(errno)));
		return false;
	}

	if (Pid == 0)
	{
		// ── Child process ──
		// Set working directory
		FTCHARToUTF8 Utf8Cwd(*Session.WorkingDirectory);
		if (Utf8Cwd.Length() > 0)
		{
			chdir(Utf8Cwd.Get());
		}

		// Set TERM for color/escape support
		setenv("TERM", "xterm-256color", 1);

		// Determine shell
		FString ShellToUse = Session.Shell;
		if (ShellToUse.IsEmpty())
		{
			const char* EnvShell = getenv("SHELL");
			if (EnvShell && EnvShell[0])
			{
				ShellToUse = UTF8_TO_TCHAR(EnvShell);
			}
			else
			{
				ShellToUse = TEXT("/bin/zsh");
			}
		}

		FTCHARToUTF8 Utf8Shell(*ShellToUse);
		// Exec the shell as a login shell (prefix argv[0] with -)
		FString LoginArg = FString::Printf(TEXT("-%s"), *FPaths::GetCleanFilename(ShellToUse));
		FTCHARToUTF8 Utf8LoginArg(*LoginArg);

		const char* Args[] = { Utf8LoginArg.Get(), nullptr };
		execvp(Utf8Shell.Get(), const_cast<char* const*>(Args));

		// If exec fails, exit child
		_exit(127);
	}

	// ── Parent process ──
	Session.MasterFd = MasterFd;
	Session.ChildPid = Pid;

	// Set master fd to non-blocking for select() usage
	int Flags = fcntl(MasterFd, F_GETFL);
	if (Flags >= 0)
	{
		fcntl(MasterFd, F_SETFL, Flags | O_NONBLOCK);
	}

	return true;

#elif PLATFORM_WINDOWS
	// Dynamically load ConPTY functions (Windows 10 1809+)
	if (!LoadConPTY())
	{
		UE_LOG(LogTerminal, Error, TEXT("ConPTY API not available — requires Windows 10 1809+"));
		return false;
	}

	// Create pipes for ConPTY
	HANDLE InPipeRead = nullptr, InPipeWrite = nullptr;
	HANDLE OutPipeRead = nullptr, OutPipeWrite = nullptr;

	if (!CreatePipe(&InPipeRead, &InPipeWrite, nullptr, 0) ||
		!CreatePipe(&OutPipeRead, &OutPipeWrite, nullptr, 0))
	{
		UE_LOG(LogTerminal, Error, TEXT("CreatePipe() failed for ConPTY"));
		return false;
	}

	// Create pseudo console
	COORD Size = { 80, 24 };
	HPCON PseudoConsole = nullptr;
	HRESULT Hr = GCreatePseudoConsole(Size, InPipeRead, OutPipeWrite, 0, &PseudoConsole);
	if (FAILED(Hr))
	{
		UE_LOG(LogTerminal, Error, TEXT("CreatePseudoConsole() failed: 0x%08X"), Hr);
		CloseHandle(InPipeRead);
		CloseHandle(InPipeWrite);
		CloseHandle(OutPipeRead);
		CloseHandle(OutPipeWrite);
		return false;
	}

	// Set up startup info with pseudo console
	SIZE_T AttrListSize = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &AttrListSize);
	TArray<uint8> AttrListBuf;
	AttrListBuf.SetNumZeroed(AttrListSize);
	LPPROC_THREAD_ATTRIBUTE_LIST AttrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(AttrListBuf.GetData());
	InitializeProcThreadAttributeList(AttrList, 1, 0, &AttrListSize);
	UpdateProcThreadAttribute(AttrList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, PseudoConsole, sizeof(HPCON), nullptr, nullptr);

	STARTUPINFOEXW StartupInfo = {};
	StartupInfo.StartupInfo.cb = sizeof(STARTUPINFOEXW);
	StartupInfo.lpAttributeList = AttrList;

	// Determine shell
	FString ShellToUse = Session.Shell;
	if (ShellToUse.IsEmpty())
	{
		ShellToUse = FPlatformMisc::GetEnvironmentVariable(TEXT("COMSPEC"));
		if (ShellToUse.IsEmpty())
		{
			ShellToUse = TEXT("cmd.exe");
		}
	}

	// Create process
	PROCESS_INFORMATION ProcInfo = {};
	BOOL Created = CreateProcessW(
		nullptr,
		const_cast<LPWSTR>(*ShellToUse),
		nullptr, nullptr,
		0, // bInheritHandles = FALSE
		EXTENDED_STARTUPINFO_PRESENT,
		nullptr,
		Session.WorkingDirectory.IsEmpty() ? nullptr : *Session.WorkingDirectory,
		&StartupInfo.StartupInfo,
		&ProcInfo
	);

	DeleteProcThreadAttributeList(AttrList);

	if (!Created)
	{
		UE_LOG(LogTerminal, Error, TEXT("CreateProcess() failed for shell: %s"), *ShellToUse);
		GClosePseudoConsole(PseudoConsole);
		CloseHandle(InPipeRead);
		CloseHandle(InPipeWrite);
		CloseHandle(OutPipeRead);
		CloseHandle(OutPipeWrite);
		return false;
	}

	// Close handles we don't need in parent
	CloseHandle(ProcInfo.hThread);
	CloseHandle(InPipeRead);
	CloseHandle(OutPipeWrite);

	Session.PseudoConsole = PseudoConsole;
	Session.ProcessHandle = ProcInfo.hProcess;
	Session.ReadPipe = OutPipeRead;
	Session.WritePipe = InPipeWrite;

	return true;
#else
	UE_LOG(LogTerminal, Error, TEXT("Terminal not supported on this platform"));
	return false;
#endif
}

void FTerminalManager::CleanupSession(FTerminalSession& Session)
{
	// Step 1: Signal the reader thread to stop and wait for it BEFORE closing handles.
	// This prevents the reader from accessing closed fds/handles.
	Session.bStopRequested.Store(true);

	if (Session.ReaderThread)
	{
		Session.ReaderThread->WaitForCompletion();
		delete Session.ReaderThread;
		Session.ReaderThread = nullptr;
	}

	if (Session.ReaderRunnable)
	{
		delete Session.ReaderRunnable;
		Session.ReaderRunnable = nullptr;
	}

	// Step 2: Kill child process and close platform handles.
#if PLATFORM_MAC || PLATFORM_LINUX
	// Kill entire process group (shell + children like npm, webpack, etc.)
	if (Session.ChildPid > 0)
	{
		kill(-Session.ChildPid, SIGTERM); // Negative PID = process group
		int Status = 0;
		for (int i = 0; i < 10; ++i)
		{
			if (waitpid(Session.ChildPid, &Status, WNOHANG) != 0) break;
			FPlatformProcess::Sleep(0.05f);
		}
		if (waitpid(Session.ChildPid, &Status, WNOHANG) == 0)
		{
			kill(-Session.ChildPid, SIGKILL);
			waitpid(Session.ChildPid, &Status, 0);
		}
	}

	if (Session.MasterFd >= 0)
	{
		close(Session.MasterFd);
		Session.MasterFd = -1;
	}

#elif PLATFORM_WINDOWS
	if (Session.PseudoConsole)
	{
		if (GClosePseudoConsole) GClosePseudoConsole(static_cast<HPCON>(Session.PseudoConsole));
		Session.PseudoConsole = nullptr;
	}

	if (Session.ProcessHandle)
	{
		TerminateProcess(Session.ProcessHandle, 0);
		WaitForSingleObject(Session.ProcessHandle, 5000);
		CloseHandle(Session.ProcessHandle);
		Session.ProcessHandle = nullptr;
	}

	if (Session.ReadPipe)
	{
		CloseHandle(Session.ReadPipe);
		Session.ReadPipe = nullptr;
	}

	if (Session.WritePipe)
	{
		CloseHandle(Session.WritePipe);
		Session.WritePipe = nullptr;
	}
#endif
}
