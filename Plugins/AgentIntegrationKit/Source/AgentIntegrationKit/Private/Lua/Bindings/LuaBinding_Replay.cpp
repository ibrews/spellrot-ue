// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include <sol/sol.hpp>
#include "Tools/NeoStackToolUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Engine/DemoNetDriver.h"
#include "GameFramework/WorldSettings.h"
#include "ReplaySubsystem.h"

// ─── Documentation ───

static TArray<FLuaFunctionDoc> ReplayDocs = {
	{ TEXT("replay_start_recording(name?, friendly_name?)"), TEXT("Start recording a replay in the current PIE session. Recording starts asynchronously — check replay_status() to confirm."), TEXT("true or nil") },
	{ TEXT("replay_stop_recording()"), TEXT("Stop recording the current replay"), TEXT("true or nil") },
	{ TEXT("replay_play(name)"), TEXT("Play back a previously recorded replay by name"), TEXT("true or nil") },
	{ TEXT("replay_stop()"), TEXT("Stop the current replay recording or playback via the replay subsystem"), TEXT("true or nil") },
	{ TEXT("replay_pause()"), TEXT("Pause replay playback"), TEXT("true or nil") },
	{ TEXT("replay_resume()"), TEXT("Resume replay playback after pausing"), TEXT("true or nil") },
	{ TEXT("replay_goto_time(seconds)"), TEXT("Scrub to a specific time in the replay"), TEXT("true or nil") },
	{ TEXT("replay_set_speed(speed)"), TEXT("Set replay playback speed (1.0 = normal, 2.0 = double, 0.5 = half)"), TEXT("true or nil") },
	{ TEXT("replay_request_checkpoint()"), TEXT("Request a checkpoint save during recording (improves scrubbing accuracy)"), TEXT("true or nil") },
	{ TEXT("replay_status()"), TEXT("Get current replay system status (recording, playing, paused, times, name, speed)"), TEXT("table") },
};

// ─── Helpers ───

// Get the PIE world — replay only works in PIE, not the editor world
static UWorld* GetPIEWorld()
{
	if (!GEditor) return nullptr;

	// Try to get the PIE world context
	FWorldContext* PIEContext = GEditor->GetPIEWorldContext(0);
	if (PIEContext && PIEContext->World())
	{
		return PIEContext->World();
	}

	// Fallback: check PlayWorld directly
	if (GEditor->PlayWorld)
	{
		return GEditor->PlayWorld;
	}

	return nullptr;
}

static UGameInstance* GetPIEGameInstance()
{
	UWorld* World = GetPIEWorld();
	return World ? World->GetGameInstance() : nullptr;
}

static UDemoNetDriver* GetDemoDriver()
{
	UWorld* World = GetPIEWorld();
	return World ? World->GetDemoNetDriver() : nullptr;
}

static UReplaySubsystem* GetReplaySubsystem()
{
	UGameInstance* GI = GetPIEGameInstance();
	return GI ? GI->GetSubsystem<UReplaySubsystem>() : nullptr;
}

// ─── Binding ───

static void BindReplay(sol::state& Lua, FLuaSessionData& Session)
{
	// ── replay_start_recording(name?, friendly_name?) ──
	Lua.set_function("replay_start_recording", [&Session](
		sol::optional<std::string> NameOpt,
		sol::optional<std::string> FriendlyNameOpt,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UGameInstance* GI = GetPIEGameInstance();
		if (!GI)
		{
			Session.Log(TEXT("[FAIL] replay_start_recording -> no active PIE session (start Play In Editor first)"));
			return sol::lua_nil;
		}

		// Check if already recording
		UDemoNetDriver* Driver = GetDemoDriver();
		if (Driver && Driver->IsRecording())
		{
			Session.Log(TEXT("[FAIL] replay_start_recording -> already recording a replay"));
			return sol::lua_nil;
		}

		FString Name = NameOpt.has_value() ? UTF8_TO_TCHAR(NameOpt.value().c_str()) : TEXT("");
		FString FriendlyName = FriendlyNameOpt.has_value() ? UTF8_TO_TCHAR(FriendlyNameOpt.value().c_str()) : TEXT("");

		GI->StartRecordingReplay(Name, FriendlyName);

		// StartRecordingReplay is async — verify the driver was created
		UDemoNetDriver* NewDriver = GetDemoDriver();
		if (!NewDriver)
		{
			Session.Log(FString::Printf(TEXT("[WARN] replay_start_recording(\"%s\", \"%s\") -> initiated but demo net driver not yet available (recording may still start asynchronously)"), *Name, *FriendlyName));
			return sol::make_object(LuaView, true);
		}

		Session.Log(FString::Printf(TEXT("[OK] replay_start_recording(\"%s\", \"%s\")"), *Name, *FriendlyName));
		return sol::make_object(LuaView, true);
	});

	// ── replay_stop_recording() ──
	Lua.set_function("replay_stop_recording", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UGameInstance* GI = GetPIEGameInstance();
		if (!GI)
		{
			Session.Log(TEXT("[FAIL] replay_stop_recording -> no active PIE session"));
			return sol::lua_nil;
		}

		UDemoNetDriver* Driver = GetDemoDriver();
		if (!Driver || !Driver->IsRecording())
		{
			Session.Log(TEXT("[WARN] replay_stop_recording -> no replay is currently recording"));
			return sol::lua_nil;
		}

		GI->StopRecordingReplay();
		Session.Log(TEXT("[OK] replay_stop_recording"));
		return sol::make_object(LuaView, true);
	});

	// ── replay_play(name) ──
	Lua.set_function("replay_play", [&Session](const std::string& Name, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UGameInstance* GI = GetPIEGameInstance();
		if (!GI)
		{
			Session.Log(TEXT("[FAIL] replay_play -> no active PIE session (start Play In Editor first)"));
			return sol::lua_nil;
		}

		FString ReplayName = UTF8_TO_TCHAR(Name.c_str());
		if (ReplayName.IsEmpty())
		{
			Session.Log(TEXT("[FAIL] replay_play -> replay name is required"));
			return sol::lua_nil;
		}

		bool bSuccess = GI->PlayReplay(ReplayName);
		if (!bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] replay_play(\"%s\") -> failed to start playback (replay may not exist)"), *ReplayName));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] replay_play(\"%s\")"), *ReplayName));
		return sol::make_object(LuaView, true);
	});

	// ── replay_stop() ──
	Lua.set_function("replay_stop", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UGameInstance* GI = GetPIEGameInstance();
		if (!GI)
		{
			Session.Log(TEXT("[FAIL] replay_stop -> no active PIE session"));
			return sol::lua_nil;
		}

		UDemoNetDriver* Driver = GetDemoDriver();
		if (!Driver || (!Driver->IsPlaying() && !Driver->IsRecording()))
		{
			Session.Log(TEXT("[WARN] replay_stop -> no replay is currently active"));
			return sol::make_object(LuaView, true);
		}

		// Use StopRecordingReplay which goes through UReplaySubsystem::StopReplay()
		// This properly cleans up the ReplayConnection and handles bLoadDefaultMapOnStop
		GI->StopRecordingReplay();
		Session.Log(TEXT("[OK] replay_stop"));
		return sol::make_object(LuaView, true);
	});

	// ── replay_pause() ──
	Lua.set_function("replay_pause", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UDemoNetDriver* Driver = GetDemoDriver();
		if (!Driver)
		{
			Session.Log(TEXT("[FAIL] replay_pause -> no active demo net driver (no PIE or no replay in progress)"));
			return sol::lua_nil;
		}

		if (!Driver->IsPlaying())
		{
			Session.Log(TEXT("[FAIL] replay_pause -> no replay is currently playing"));
			return sol::lua_nil;
		}

		if (Driver->GetChannelsArePaused())
		{
			Session.Log(TEXT("[WARN] replay_pause -> already paused"));
			return sol::make_object(LuaView, true);
		}

		Driver->PauseChannels(true);
		Session.Log(TEXT("[OK] replay_pause"));
		return sol::make_object(LuaView, true);
	});

	// ── replay_resume() ──
	Lua.set_function("replay_resume", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UDemoNetDriver* Driver = GetDemoDriver();
		if (!Driver)
		{
			Session.Log(TEXT("[FAIL] replay_resume -> no active demo net driver"));
			return sol::lua_nil;
		}

		if (!Driver->IsPlaying())
		{
			Session.Log(TEXT("[FAIL] replay_resume -> no replay is currently playing"));
			return sol::lua_nil;
		}

		if (!Driver->GetChannelsArePaused())
		{
			Session.Log(TEXT("[WARN] replay_resume -> not paused"));
			return sol::make_object(LuaView, true);
		}

		Driver->PauseChannels(false);
		Session.Log(TEXT("[OK] replay_resume"));
		return sol::make_object(LuaView, true);
	});

	// ── replay_goto_time(seconds) ──
	Lua.set_function("replay_goto_time", [&Session](double Seconds, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UDemoNetDriver* Driver = GetDemoDriver();
		if (!Driver)
		{
			Session.Log(TEXT("[FAIL] replay_goto_time -> no active demo net driver"));
			return sol::lua_nil;
		}

		if (!Driver->IsPlaying())
		{
			Session.Log(TEXT("[FAIL] replay_goto_time -> no replay is currently playing"));
			return sol::lua_nil;
		}

		float TotalTime = Driver->GetDemoTotalTime();
		float TargetTime = static_cast<float>(Seconds);

		if (TargetTime < 0.0f)
		{
			TargetTime = 0.0f;
		}
		else if (TargetTime > TotalTime)
		{
			Session.Log(FString::Printf(TEXT("[WARN] replay_goto_time -> clamping %.2f to total time %.2f"), TargetTime, TotalTime));
			TargetTime = TotalTime;
		}

		Driver->GotoTimeInSeconds(TargetTime);
		Session.Log(FString::Printf(TEXT("[OK] replay_goto_time(%.2f) — total: %.2f"), TargetTime, TotalTime));
		return sol::make_object(LuaView, true);
	});

	// ── replay_set_speed(speed) ──
	Lua.set_function("replay_set_speed", [&Session](double Speed, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UWorld* World = GetPIEWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] replay_set_speed -> no active PIE session"));
			return sol::lua_nil;
		}

		UDemoNetDriver* Driver = World->GetDemoNetDriver();
		if (!Driver || !Driver->IsPlaying())
		{
			Session.Log(TEXT("[FAIL] replay_set_speed -> no replay is currently playing"));
			return sol::lua_nil;
		}

		float SpeedFloat = static_cast<float>(Speed);
		if (SpeedFloat <= 0.0f)
		{
			Session.Log(TEXT("[FAIL] replay_set_speed -> speed must be greater than 0"));
			return sol::lua_nil;
		}

		AWorldSettings* WorldSettings = World->GetWorldSettings();
		if (!WorldSettings)
		{
			Session.Log(TEXT("[FAIL] replay_set_speed -> no world settings available"));
			return sol::lua_nil;
		}

		WorldSettings->DemoPlayTimeDilation = SpeedFloat;
		Session.Log(FString::Printf(TEXT("[OK] replay_set_speed(%.2f)"), SpeedFloat));
		return sol::make_object(LuaView, true);
	});

	// ── replay_request_checkpoint() ──
	Lua.set_function("replay_request_checkpoint", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		UReplaySubsystem* ReplaySub = GetReplaySubsystem();
		if (!ReplaySub)
		{
			Session.Log(TEXT("[FAIL] replay_request_checkpoint -> no replay subsystem (no active PIE session)"));
			return sol::lua_nil;
		}

		if (!ReplaySub->IsRecording())
		{
			Session.Log(TEXT("[FAIL] replay_request_checkpoint -> no replay is currently recording"));
			return sol::lua_nil;
		}

		ReplaySub->RequestCheckpoint();
		Session.Log(TEXT("[OK] replay_request_checkpoint"));
		return sol::make_object(LuaView, true);
	});

	// ── replay_status() ──
	Lua.set_function("replay_status", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		sol::table Result = LuaView.create_table();

		UWorld* PIEWorld = GetPIEWorld();
		Result["has_pie"] = (PIEWorld != nullptr);

		UDemoNetDriver* Driver = PIEWorld ? PIEWorld->GetDemoNetDriver() : nullptr;

		if (!Driver)
		{
			Result["is_recording"] = false;
			Result["is_playing"] = false;
			Result["is_paused"] = false;
			Result["is_recording_paused"] = false;
			Result["current_time"] = 0.0;
			Result["total_time"] = 0.0;
			Result["name"] = std::string();
			Result["speed"] = 1.0;
			Result["is_saving_checkpoint"] = false;

			Session.Log(TEXT("[OK] replay_status -> no active demo net driver"));
			return sol::make_object(LuaView, Result);
		}

		bool bRecording = Driver->IsRecording();
		bool bPlaying = Driver->IsPlaying();
		bool bPaused = Driver->GetChannelsArePaused();
		bool bRecordingPaused = Driver->IsRecordingPaused();
		float CurrentTime = Driver->GetDemoCurrentTime();
		float TotalTime = Driver->GetDemoTotalTime();

		Result["is_recording"] = bRecording;
		Result["is_playing"] = bPlaying;
		Result["is_paused"] = bPaused;
		Result["is_recording_paused"] = bRecordingPaused;
		Result["current_time"] = static_cast<double>(CurrentTime);
		Result["total_time"] = static_cast<double>(TotalTime);

		// Active replay name
		const FString& ActiveName = Driver->GetActiveReplayName();
		Result["name"] = std::string(TCHAR_TO_UTF8(*ActiveName));

		// Playback speed from DemoPlayTimeDilation
		AWorldSettings* WorldSettings = PIEWorld->GetWorldSettings();
		Result["speed"] = WorldSettings ? static_cast<double>(WorldSettings->DemoPlayTimeDilation) : 1.0;

		// Checkpoint status
		UReplaySubsystem* ReplaySub = GetReplaySubsystem();
		Result["is_saving_checkpoint"] = ReplaySub ? ReplaySub->IsSavingCheckpoint() : false;

		Session.Log(FString::Printf(TEXT("[OK] replay_status -> recording=%s playing=%s paused=%s time=%.2f/%.2f name=\"%s\""),
			bRecording ? TEXT("true") : TEXT("false"),
			bPlaying ? TEXT("true") : TEXT("false"),
			bPaused ? TEXT("true") : TEXT("false"),
			CurrentTime, TotalTime, *ActiveName));
		return sol::make_object(LuaView, Result);
	});
}

REGISTER_LUA_BINDING(Replay, ReplayDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindReplay(Lua, Session);
});
