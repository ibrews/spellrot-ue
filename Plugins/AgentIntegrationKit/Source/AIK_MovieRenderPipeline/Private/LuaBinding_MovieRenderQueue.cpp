// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include <sol/sol.hpp>
#include "Tools/NeoStackToolUtils.h"

#include "MoviePipelineQueue.h"
#include "MoviePipelineQueueEngineSubsystem.h"
#include "MoviePipelineExecutor.h"
#include "MoviePipelineLinearExecutor.h"
#include "MoviePipelineInProcessExecutor.h"
#include "MoviePipeline.h"
#include "MoviePipelinePrimaryConfig.h"
#include "MoviePipelineSetting.h"
#include "MoviePipelineBlueprintLibrary.h"
#include "LevelSequence.h"
#include "Engine/Engine.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"

// ─── Documentation ───

static TArray<FLuaFunctionDoc> MovieRenderQueueDocs = {
	{ TEXT("mrq_add_job(sequence_path)"), TEXT("Add a render job to the queue for a Level Sequence (does NOT clear existing jobs) — returns job handle"), TEXT("job handle or nil") },
	{ TEXT("mrq_allocate_job(sequence_path)"), TEXT("Allocate a render job (clears queue first) — returns handle with find_or_add_setting, set_map, etc."), TEXT("job handle or nil") },
	{ TEXT("mrq_duplicate_job(index)"), TEXT("Duplicate a job by 1-based index — returns new job handle"), TEXT("job handle or nil") },
	{ TEXT("mrq_set_job_index(index, new_index)"), TEXT("Move a job from one 1-based index to another"), TEXT("true or nil") },
	{ TEXT("mrq_delete_job(index)"), TEXT("Delete a job from the render queue by 1-based index"), TEXT("true or nil") },
	{ TEXT("mrq_delete_all_jobs()"), TEXT("Remove all jobs from the render queue"), TEXT("true or nil") },
	{ TEXT("mrq_list_settings(filter?)"), TEXT("List available MRQ setting classes with optional name filter"), TEXT("table[]") },
	{ TEXT("mrq_render_job(job)"), TEXT("Render a single job allocated via mrq_allocate_job"), TEXT("true or nil") },
	{ TEXT("mrq_render_queue()"), TEXT("Render the full queue using the in-process executor"), TEXT("true or nil") },
	{ TEXT("mrq_is_rendering()"), TEXT("Check if a render is currently in progress"), TEXT("bool") },
	{ TEXT("mrq_render_progress()"), TEXT("Get detailed render progress — executor, per-job, and per-shot status"), TEXT("table") },
	{ TEXT("mrq_cancel_render(cancel_all?)"), TEXT("Cancel the current render — cancel_all=true cancels all remaining jobs"), TEXT("true or nil") },
	{ TEXT("mrq_get_queue()"), TEXT("Get info about the current render queue and its jobs"), TEXT("table") },
	{ TEXT("mrq_save_queue(asset_path)"), TEXT("Save the current render queue as a reusable asset"), TEXT("true or nil") },
	{ TEXT("mrq_load_queue(asset_path)"), TEXT("Load a saved queue asset and replace the current queue"), TEXT("true or nil") },
};

// ─── Helpers ───

static UMoviePipelineQueueEngineSubsystem* GetMRQSubsystem()
{
	if (GEngine)
	{
		return GEngine->GetEngineSubsystem<UMoviePipelineQueueEngineSubsystem>();
	}
	return nullptr;
}

// Resolve a setting class name (exact, prefix-stripped, or partial match)
static UClass* ResolveMRQSettingClass(const FString& InClassName)
{
	FString FClassName = InClassName;
	if (!FClassName.StartsWith(TEXT("U")))
		FClassName = TEXT("U") + FClassName;

	// Exact match (with or without U prefix)
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->IsChildOf(UMoviePipelineSetting::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract))
		{
			if (It->GetName() == FClassName || It->GetName() == FClassName.Mid(1))
			{
				return *It;
			}
		}
	}

	// Partial match
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->IsChildOf(UMoviePipelineSetting::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract))
		{
			if (It->GetName().Contains(FClassName.Mid(1)))
			{
				return *It;
			}
		}
	}

	return nullptr;
}

// Build a Lua setting handle with get/set/list_properties for the given setting
static sol::table BuildSettingHandle(sol::state_view& Lua, TWeakObjectPtr<UMoviePipelineSetting> WeakSetting, FLuaSessionData& Session)
{
	sol::table SettingHandle = Lua.create_table();

	UMoviePipelineSetting* Setting = WeakSetting.Get();
	if (Setting)
	{
		SettingHandle["_name"] = std::string(TCHAR_TO_UTF8(*Setting->GetClass()->GetName()));
	}

	SettingHandle.set_function("get", [WeakSetting, &Session](sol::table, const std::string& PropName, sol::this_state S2) -> sol::object
	{
		sol::state_view L(S2);
		UMoviePipelineSetting* S = WeakSetting.Get();
		if (!S) { Session.Log(TEXT("[FAIL] get -> setting no longer valid")); return sol::lua_nil; }
		FString FPropName = UTF8_TO_TCHAR(PropName.c_str());
		FProperty* Prop = S->GetClass()->FindPropertyByName(*FPropName);
		if (!Prop) return sol::lua_nil;
		FString Value;
		Prop->ExportTextItem_InContainer(Value, S, nullptr, nullptr, PPF_None);
		return sol::make_object(L, std::string(TCHAR_TO_UTF8(*Value)));
	});

	SettingHandle.set_function("set", [WeakSetting, &Session](sol::table, const std::string& PropName, const std::string& Value)
	{
		UMoviePipelineSetting* S = WeakSetting.Get();
		if (!S) { Session.Log(TEXT("[FAIL] set -> setting no longer valid")); return; }
		FString FPropName = UTF8_TO_TCHAR(PropName.c_str());
		FString FValue = UTF8_TO_TCHAR(Value.c_str());
		FProperty* Prop = S->GetClass()->FindPropertyByName(*FPropName);
		if (!Prop)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] set -> property '%s' not found"), *FPropName));
			return;
		}
		S->Modify();
		S->PreEditChange(Prop);
		Prop->ImportText_InContainer(*FValue, S, nullptr, PPF_None);
		FPropertyChangedEvent PropEvent(Prop, EPropertyChangeType::ValueSet);
		S->PostEditChangeProperty(PropEvent);
		Session.Log(FString::Printf(TEXT("[OK] set %s = %s"), *FPropName, *FValue));
	});

	SettingHandle.set_function("list_properties", [WeakSetting, &Session](sol::table, sol::this_state S2) -> sol::object
	{
		sol::state_view L(S2);
		UMoviePipelineSetting* S = WeakSetting.Get();
		if (!S) { Session.Log(TEXT("[FAIL] list_properties -> setting no longer valid")); return sol::lua_nil; }
		sol::table Result = L.create_table();
		int32 Idx = 1;
		for (TFieldIterator<FProperty> It(S->GetClass()); It; ++It)
		{
			if (!It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				FString Value;
				It->ExportTextItem_InContainer(Value, S, nullptr, nullptr, PPF_None);
				sol::table Entry = L.create_table();
				Entry["name"] = std::string(TCHAR_TO_UTF8(*It->GetName()));
				Entry["type"] = std::string(TCHAR_TO_UTF8(*It->GetCPPType()));
				Entry["value"] = std::string(TCHAR_TO_UTF8(*Value));
				Result[Idx++] = Entry;
			}
		}
		return sol::make_object(L, Result);
	});

	return SettingHandle;
}

// Build a Lua job handle for the given job with all sub-methods
static sol::table BuildJobHandle(sol::state_view& Lua, UMoviePipelineExecutorJob* Job, FLuaSessionData& Session)
{
	TWeakObjectPtr<UMoviePipelineExecutorJob> WeakJob(Job);

	sol::table JobHandle = Lua.create_table();
	JobHandle["sequence"] = std::string(TCHAR_TO_UTF8(*Job->Sequence.GetAssetPathString()));

	// job:find_or_add_setting(class_name)
	JobHandle.set_function("find_or_add_setting", [WeakJob, &Session](sol::table /*self*/, const std::string& ClassName, sol::this_state S) -> sol::object
	{
		sol::state_view InnerLua(S);
		UMoviePipelineExecutorJob* J = WeakJob.Get();
		if (!J) { Session.Log(TEXT("[FAIL] find_or_add_setting -> job no longer valid")); return sol::lua_nil; }

		FString FClassName = UTF8_TO_TCHAR(ClassName.c_str());
		UClass* SettingClass = ResolveMRQSettingClass(FClassName);
		if (!SettingClass)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] find_or_add_setting -> class not found: %s"), *FClassName));
			return sol::lua_nil;
		}

		UMoviePipelineConfigBase* Config = J->GetConfiguration();
		if (!Config)
		{
			Session.Log(TEXT("[FAIL] find_or_add_setting -> job has no configuration"));
			return sol::lua_nil;
		}

		UMoviePipelineSetting* Setting = Config->FindOrAddSettingByClass(SettingClass);
		if (!Setting)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] find_or_add_setting -> could not create setting: %s"), *SettingClass->GetName()));
			return sol::lua_nil;
		}

		sol::table SettingHandle = BuildSettingHandle(InnerLua, TWeakObjectPtr<UMoviePipelineSetting>(Setting), Session);
		Session.Log(FString::Printf(TEXT("[OK] find_or_add_setting -> %s"), *SettingClass->GetName()));
		return sol::make_object(InnerLua, SettingHandle);
	});

	// job:remove_setting(class_name)
	JobHandle.set_function("remove_setting", [WeakJob, &Session](sol::table, const std::string& ClassName, sol::this_state S) -> sol::object
	{
		sol::state_view InnerLua(S);
		UMoviePipelineExecutorJob* J = WeakJob.Get();
		if (!J) { Session.Log(TEXT("[FAIL] remove_setting -> job no longer valid")); return sol::lua_nil; }

		FString FClassName = UTF8_TO_TCHAR(ClassName.c_str());
		UClass* SettingClass = ResolveMRQSettingClass(FClassName);
		if (!SettingClass)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] remove_setting -> class not found: %s"), *FClassName));
			return sol::lua_nil;
		}

		UMoviePipelineConfigBase* Config = J->GetConfiguration();
		if (!Config)
		{
			Session.Log(TEXT("[FAIL] remove_setting -> job has no configuration"));
			return sol::lua_nil;
		}

		UMoviePipelineSetting* Setting = Config->FindSettingByClass(SettingClass);
		if (!Setting)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] remove_setting -> setting not found: %s"), *SettingClass->GetName()));
			return sol::lua_nil;
		}

		Config->RemoveSetting(Setting);
		Session.Log(FString::Printf(TEXT("[OK] remove_setting -> removed %s"), *SettingClass->GetName()));
		return sol::make_object(InnerLua, true);
	});

	// job:list_settings()
	JobHandle.set_function("list_settings", [WeakJob, &Session](sol::table, sol::this_state S) -> sol::object
	{
		sol::state_view InnerLua(S);
		UMoviePipelineExecutorJob* J = WeakJob.Get();
		if (!J) { Session.Log(TEXT("[FAIL] list_settings -> job no longer valid")); return sol::lua_nil; }

		UMoviePipelineConfigBase* Config = J->GetConfiguration();
		if (!Config) { Session.Log(TEXT("[FAIL] list_settings -> job has no configuration")); return sol::lua_nil; }

		TArray<UMoviePipelineSetting*> Settings = Config->GetUserSettings();
		sol::table Result = InnerLua.create_table();
		int32 Idx = 1;
		for (UMoviePipelineSetting* Setting : Settings)
		{
			if (!Setting) continue;
			sol::table Entry = InnerLua.create_table();
			Entry["name"] = std::string(TCHAR_TO_UTF8(*Setting->GetClass()->GetName()));
			Entry["display_name"] = std::string(TCHAR_TO_UTF8(*Setting->GetClass()->GetDisplayNameText().ToString()));
			Entry["is_enabled"] = Setting->IsEnabled();
			Result[Idx++] = Entry;
		}
		return sol::make_object(InnerLua, Result);
	});

	// job:set_map(map_path)
	JobHandle.set_function("set_map", [WeakJob, &Session](sol::table, const std::string& MapPath)
	{
		UMoviePipelineExecutorJob* J = WeakJob.Get();
		if (!J) { Session.Log(TEXT("[FAIL] set_map -> job no longer valid")); return; }
		FString FMapPath = UTF8_TO_TCHAR(MapPath.c_str());
		J->Map = FSoftObjectPath(FMapPath);
		Session.Log(FString::Printf(TEXT("[OK] set_map -> %s"), *FMapPath));
	});

	// job:set_author(name)
	JobHandle.set_function("set_author", [WeakJob, &Session](sol::table, const std::string& Author)
	{
		UMoviePipelineExecutorJob* J = WeakJob.Get();
		if (!J) { Session.Log(TEXT("[FAIL] set_author -> job no longer valid")); return; }
		J->Author = UTF8_TO_TCHAR(Author.c_str());
		Session.Log(FString::Printf(TEXT("[OK] set_author -> %s"), *J->Author));
	});

	// job:set_job_name(name)
	JobHandle.set_function("set_job_name", [WeakJob, &Session](sol::table, const std::string& Name)
	{
		UMoviePipelineExecutorJob* J = WeakJob.Get();
		if (!J) { Session.Log(TEXT("[FAIL] set_job_name -> job no longer valid")); return; }
		J->JobName = UTF8_TO_TCHAR(Name.c_str());
		Session.Log(FString::Printf(TEXT("[OK] set_job_name -> %s"), *J->JobName));
	});

	// job:set_comment(text)
	JobHandle.set_function("set_comment", [WeakJob, &Session](sol::table, const std::string& Text)
	{
		UMoviePipelineExecutorJob* J = WeakJob.Get();
		if (!J) { Session.Log(TEXT("[FAIL] set_comment -> job no longer valid")); return; }
		J->Comment = UTF8_TO_TCHAR(Text.c_str());
		Session.Log(FString::Printf(TEXT("[OK] set_comment -> %s"), *J->Comment));
	});

	// job:set_enabled(bool)
	JobHandle.set_function("set_enabled", [WeakJob, &Session](sol::table, bool bEnabled)
	{
		UMoviePipelineExecutorJob* J = WeakJob.Get();
		if (!J) { Session.Log(TEXT("[FAIL] set_enabled -> job no longer valid")); return; }
		J->SetIsEnabled(bEnabled);
		Session.Log(FString::Printf(TEXT("[OK] set_enabled -> %s"), bEnabled ? TEXT("true") : TEXT("false")));
	});

	// job:set_consumed(bool)
	JobHandle.set_function("set_consumed", [WeakJob, &Session](sol::table, bool bConsumed)
	{
		UMoviePipelineExecutorJob* J = WeakJob.Get();
		if (!J) { Session.Log(TEXT("[FAIL] set_consumed -> job no longer valid")); return; }
		J->SetConsumed(bConsumed);
		Session.Log(FString::Printf(TEXT("[OK] set_consumed -> %s"), bConsumed ? TEXT("true") : TEXT("false")));
	});

	// job:is_consumed()
	JobHandle.set_function("is_consumed", [WeakJob](sol::table, sol::this_state S) -> sol::object
	{
		sol::state_view L(S);
		UMoviePipelineExecutorJob* J = WeakJob.Get();
		if (!J) return sol::lua_nil;
		return sol::make_object(L, J->IsConsumed());
	});

	// job:set_sequence(sequence_path)
	JobHandle.set_function("set_sequence", [WeakJob, &Session](sol::table, const std::string& SequencePath)
	{
		UMoviePipelineExecutorJob* J = WeakJob.Get();
		if (!J) { Session.Log(TEXT("[FAIL] set_sequence -> job no longer valid")); return; }
		FString SeqPath = UTF8_TO_TCHAR(SequencePath.c_str());
		if (!SeqPath.StartsWith(TEXT("/")))
			SeqPath = TEXT("/Game/") + SeqPath;
		J->SetSequence(FSoftObjectPath(SeqPath));
		Session.Log(FString::Printf(TEXT("[OK] set_sequence -> %s"), *SeqPath));
	});

	// job:info()
	JobHandle.set_function("info", [WeakJob, &Session](sol::table, sol::this_state S) -> sol::object
	{
		sol::state_view L(S);
		UMoviePipelineExecutorJob* J = WeakJob.Get();
		if (!J) { Session.Log(TEXT("[FAIL] info -> job no longer valid")); return sol::lua_nil; }

		sol::table Info = L.create_table();
		Info["sequence"] = std::string(TCHAR_TO_UTF8(*J->Sequence.GetAssetPathString()));
		Info["map"] = std::string(TCHAR_TO_UTF8(*J->Map.GetAssetPathString()));
		Info["author"] = std::string(TCHAR_TO_UTF8(*J->Author));
		Info["job_name"] = std::string(TCHAR_TO_UTF8(*J->JobName));
		Info["comment"] = std::string(TCHAR_TO_UTF8(*J->Comment));
		Info["is_enabled"] = J->IsEnabled();
		Info["is_consumed"] = J->IsConsumed();
		Info["user_data"] = std::string(TCHAR_TO_UTF8(*J->UserData));
		Info["is_graph_config"] = J->IsUsingGraphConfiguration();
		return sol::make_object(L, Info);
	});

	return JobHandle;
}

// ─── Binding ───

static void BindMovieRenderQueue(sol::state& Lua, FLuaSessionData& Session)
{
	// ---- mrq_add_job(sequence_path) — add to queue without clearing ----
	Lua.set_function("mrq_add_job", [&Session](const std::string& SequencePath, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);

		UMoviePipelineQueueEngineSubsystem* Subsystem = GetMRQSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] mrq_add_job -> Movie Pipeline subsystem not available"));
			return sol::lua_nil;
		}

		if (Subsystem->IsRendering())
		{
			Session.Log(TEXT("[FAIL] mrq_add_job -> a render is already in progress"));
			return sol::lua_nil;
		}

		FString SeqPath = UTF8_TO_TCHAR(SequencePath.c_str());
		if (!SeqPath.StartsWith(TEXT("/")))
			SeqPath = TEXT("/Game/") + SeqPath;

		ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SeqPath);
		if (!Sequence)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] mrq_add_job -> sequence not found: %s"), *SeqPath));
			return sol::lua_nil;
		}

		UMoviePipelineQueue* Queue = Subsystem->GetQueue();
		if (!Queue)
		{
			Session.Log(TEXT("[FAIL] mrq_add_job -> no queue available"));
			return sol::lua_nil;
		}

		UMoviePipelineExecutorJob* Job = Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
		if (!Job)
		{
			Session.Log(TEXT("[FAIL] mrq_add_job -> failed to allocate job"));
			return sol::lua_nil;
		}

		Job->SetSequence(FSoftObjectPath(Sequence));
		Job->Map = FSoftObjectPath(GEngine->GetWorldContexts()[0].World());
		Job->JobName = Job->Sequence.GetAssetName();

		sol::table JobHandle = BuildJobHandle(Lua, Job, Session);
		Session.Log(FString::Printf(TEXT("[OK] mrq_add_job -> job added for %s (queue now has %d jobs)"), *SeqPath, Queue->GetJobs().Num()));
		return sol::make_object(Lua, JobHandle);
	});

	// ---- mrq_allocate_job(sequence_path) — clears queue, adds one job ----
	Lua.set_function("mrq_allocate_job", [&Session](const std::string& SequencePath, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);

		UMoviePipelineQueueEngineSubsystem* Subsystem = GetMRQSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] mrq_allocate_job -> Movie Pipeline subsystem not available"));
			return sol::lua_nil;
		}

		if (Subsystem->IsRendering())
		{
			Session.Log(TEXT("[FAIL] mrq_allocate_job -> a render is already in progress"));
			return sol::lua_nil;
		}

		FString SeqPath = UTF8_TO_TCHAR(SequencePath.c_str());
		if (!SeqPath.StartsWith(TEXT("/")))
			SeqPath = TEXT("/Game/") + SeqPath;

		ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SeqPath);
		if (!Sequence)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] mrq_allocate_job -> sequence not found: %s"), *SeqPath));
			return sol::lua_nil;
		}

		// Note: AllocateJob clears the queue first, then creates a single job
		UMoviePipelineExecutorJob* Job = Subsystem->AllocateJob(Sequence);
		if (!Job)
		{
			Session.Log(TEXT("[FAIL] mrq_allocate_job -> failed to allocate job"));
			return sol::lua_nil;
		}

		sol::table JobHandle = BuildJobHandle(Lua, Job, Session);
		Session.Log(FString::Printf(TEXT("[OK] mrq_allocate_job -> job created for %s (queue cleared, 1 job)"), *SeqPath));
		return sol::make_object(Lua, JobHandle);
	});

	// ---- mrq_duplicate_job(index) ----
	Lua.set_function("mrq_duplicate_job", [&Session](int Index, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UMoviePipelineQueueEngineSubsystem* Subsystem = GetMRQSubsystem();
		if (!Subsystem) { Session.Log(TEXT("[FAIL] mrq_duplicate_job -> subsystem not available")); return sol::lua_nil; }

		UMoviePipelineQueue* Queue = Subsystem->GetQueue();
		if (!Queue) { Session.Log(TEXT("[FAIL] mrq_duplicate_job -> no queue")); return sol::lua_nil; }

		TArray<UMoviePipelineExecutorJob*> Jobs = Queue->GetJobs();
		int32 Idx = Index - 1;
		if (Idx < 0 || Idx >= Jobs.Num())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] mrq_duplicate_job -> index %d out of range (1-%d)"), Index, Jobs.Num()));
			return sol::lua_nil;
		}

		UMoviePipelineExecutorJob* NewJob = Queue->DuplicateJob(Jobs[Idx]);
		if (!NewJob)
		{
			Session.Log(TEXT("[FAIL] mrq_duplicate_job -> duplication failed"));
			return sol::lua_nil;
		}

		sol::table JobHandle = BuildJobHandle(Lua, NewJob, Session);
		Session.Log(FString::Printf(TEXT("[OK] mrq_duplicate_job(%d) -> duplicated, queue now has %d jobs"), Index, Queue->GetJobs().Num()));
		return sol::make_object(Lua, JobHandle);
	});

	// ---- mrq_set_job_index(index, new_index) ----
	Lua.set_function("mrq_set_job_index", [&Session](int Index, int NewIndex, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UMoviePipelineQueueEngineSubsystem* Subsystem = GetMRQSubsystem();
		if (!Subsystem) { Session.Log(TEXT("[FAIL] mrq_set_job_index -> subsystem not available")); return sol::lua_nil; }

		UMoviePipelineQueue* Queue = Subsystem->GetQueue();
		if (!Queue) { Session.Log(TEXT("[FAIL] mrq_set_job_index -> no queue")); return sol::lua_nil; }

		TArray<UMoviePipelineExecutorJob*> Jobs = Queue->GetJobs();
		int32 Idx = Index - 1;
		int32 NewIdx = NewIndex - 1;
		if (Idx < 0 || Idx >= Jobs.Num())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] mrq_set_job_index -> source index %d out of range (1-%d)"), Index, Jobs.Num()));
			return sol::lua_nil;
		}
		if (NewIdx < 0 || NewIdx >= Jobs.Num())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] mrq_set_job_index -> target index %d out of range (1-%d)"), NewIndex, Jobs.Num()));
			return sol::lua_nil;
		}

		Queue->SetJobIndex(Jobs[Idx], NewIdx);
		Session.Log(FString::Printf(TEXT("[OK] mrq_set_job_index -> moved job %d to %d"), Index, NewIndex));
		return sol::make_object(Lua, true);
	});

	// ---- mrq_render_job(job_handle) ----
	Lua.set_function("mrq_render_job", [&Session](sol::table JobHandle, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);

		UMoviePipelineQueueEngineSubsystem* Subsystem = GetMRQSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] mrq_render_job -> Movie Pipeline subsystem not available"));
			return sol::lua_nil;
		}

		if (Subsystem->IsRendering())
		{
			Session.Log(TEXT("[FAIL] mrq_render_job -> a render is already in progress"));
			return sol::lua_nil;
		}

		// The job handle no longer stores _ptr; we need to find the job in the queue.
		// mrq_allocate_job only creates one job, so we render the first one.
		// For multi-job queues, use mrq_render_queue instead.
		UMoviePipelineQueue* Queue = Subsystem->GetQueue();
		if (!Queue || Queue->GetJobs().Num() == 0)
		{
			Session.Log(TEXT("[FAIL] mrq_render_job -> queue is empty, nothing to render"));
			return sol::lua_nil;
		}

		// Verify job exists in queue
		TArray<UMoviePipelineExecutorJob*> Jobs = Queue->GetJobs();
		if (Jobs.Num() != 1)
		{
			Session.Log(FString::Printf(TEXT("[WARN] mrq_render_job -> queue has %d jobs, RenderJob will clear queue after render"), Jobs.Num()));
		}

		UMoviePipelineExecutorJob* Job = Jobs[0];
		if (!Job)
		{
			Session.Log(TEXT("[FAIL] mrq_render_job -> first job in queue is null"));
			return sol::lua_nil;
		}

		Subsystem->RenderJob(Job);
		Session.Log(TEXT("[OK] mrq_render_job -> render started"));
		return sol::make_object(Lua, true);
	});

	// ---- mrq_render_queue() ----
	Lua.set_function("mrq_render_queue", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);

		UMoviePipelineQueueEngineSubsystem* Subsystem = GetMRQSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] mrq_render_queue -> Movie Pipeline subsystem not available"));
			return sol::lua_nil;
		}

		if (Subsystem->IsRendering())
		{
			Session.Log(TEXT("[FAIL] mrq_render_queue -> a render is already in progress"));
			return sol::lua_nil;
		}

		UMoviePipelineQueue* Queue = Subsystem->GetQueue();
		if (!Queue || Queue->GetJobs().Num() == 0)
		{
			Session.Log(TEXT("[FAIL] mrq_render_queue -> queue is empty, nothing to render"));
			return sol::lua_nil;
		}

		// Use UMoviePipelineInProcessExecutor — same as what RenderJob() uses internally
		Subsystem->RenderQueueWithExecutor(UMoviePipelineInProcessExecutor::StaticClass());
		Session.Log(FString::Printf(TEXT("[OK] mrq_render_queue -> started with InProcessExecutor (%d jobs)"), Queue->GetJobs().Num()));
		return sol::make_object(Lua, true);
	});

	// ---- mrq_is_rendering() ----
	Lua.set_function("mrq_is_rendering", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UMoviePipelineQueueEngineSubsystem* Subsystem = GetMRQSubsystem();
		if (!Subsystem) return sol::make_object(Lua, false);
		return sol::make_object(Lua, Subsystem->IsRendering());
	});

	// ---- mrq_render_progress() ----
	Lua.set_function("mrq_render_progress", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);

		UMoviePipelineQueueEngineSubsystem* Subsystem = GetMRQSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] mrq_render_progress -> Movie Pipeline subsystem not available"));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();
		bool bIsRendering = Subsystem->IsRendering();
		Result["is_rendering"] = bIsRendering;

		UMoviePipelineExecutorBase* Executor = Subsystem->GetActiveExecutor();
		if (Executor)
		{
			Result["progress"] = Executor->GetStatusProgress();
			Result["status_message"] = std::string(TCHAR_TO_UTF8(*Executor->GetStatusMessage()));
		}
		else
		{
			Result["progress"] = 0.0;
			Result["status_message"] = std::string("");
		}

		// Per-job progress from the queue
		UMoviePipelineQueue* Queue = Subsystem->GetQueue();
		sol::table JobsTable = Lua.create_table();
		if (Queue)
		{
			TArray<UMoviePipelineExecutorJob*> Jobs = Queue->GetJobs();
			for (int32 i = 0; i < Jobs.Num(); ++i)
			{
				UMoviePipelineExecutorJob* Job = Jobs[i];
				if (!Job) continue;

				sol::table JobInfo = Lua.create_table();
				JobInfo["index"] = i + 1;
				JobInfo["progress"] = Job->GetStatusProgress();
				JobInfo["status_message"] = std::string(TCHAR_TO_UTF8(*Job->GetStatusMessage()));
				JobInfo["sequence"] = std::string(TCHAR_TO_UTF8(*Job->Sequence.GetAssetPathString()));

				// Per-shot progress from the executor's active pipeline (if linear executor)
				sol::table ShotsTable = Lua.create_table();
				if (Executor)
				{
					UMoviePipelineLinearExecutorBase* LinearExecutor = Cast<UMoviePipelineLinearExecutorBase>(Executor);
					if (LinearExecutor)
					{
						// ActiveMoviePipeline is protected (TObjectPtr<UMoviePipelineBase>) — access via reflection
						FProperty* PipelineProp = LinearExecutor->GetClass()->FindPropertyByName(TEXT("ActiveMoviePipeline"));
						UMoviePipelineBase* PipelineBase = nullptr;
						if (PipelineProp)
						{
							FObjectProperty* ObjProp = CastField<FObjectProperty>(PipelineProp);
							if (ObjProp)
							{
								PipelineBase = Cast<UMoviePipelineBase>(ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(LinearExecutor)));
							}
						}

						// Legacy pipeline (UMoviePipeline) — has per-shot data
						UMoviePipeline* Pipeline = Cast<UMoviePipeline>(PipelineBase);
						if (Pipeline && Pipeline->GetCurrentJob() == Job)
						{
							JobInfo["pipeline_type"] = std::string("legacy");
							const TArray<UMoviePipelineExecutorShot*>& Shots = Pipeline->GetActiveShotList();
							int32 CurrentShotIdx = Pipeline->GetCurrentShotIndex();
							JobInfo["current_shot_index"] = CurrentShotIdx + 1;
							JobInfo["total_shots"] = Shots.Num();
							JobInfo["pipeline_progress"] = UMoviePipelineBlueprintLibrary::GetCompletionPercentage(Pipeline);

							for (int32 s = 0; s < Shots.Num(); ++s)
							{
								UMoviePipelineExecutorShot* Shot = Shots[s];
								if (!Shot) continue;

								sol::table ShotInfo = Lua.create_table();
								FString ShotName = Shot->OuterName.IsEmpty() ? Shot->InnerName : (Shot->OuterName + TEXT(".") + Shot->InnerName);
								ShotInfo["name"] = std::string(TCHAR_TO_UTF8(*ShotName));
								ShotInfo["progress"] = Shot->GetStatusProgress();
								ShotInfo["status_message"] = std::string(TCHAR_TO_UTF8(*Shot->GetStatusMessage()));
								ShotInfo["is_current"] = (s == CurrentShotIdx);
								ShotsTable[s + 1] = ShotInfo;
							}
						}
						else if (PipelineBase && !Pipeline)
						{
							// Graph-based pipeline — no per-shot data available through legacy API
							JobInfo["pipeline_type"] = std::string("graph");
						}
					}
				}
				JobInfo["shots"] = ShotsTable;
				JobsTable[i + 1] = JobInfo;
			}
		}
		Result["jobs"] = JobsTable;

		Session.Log(FString::Printf(TEXT("[OK] mrq_render_progress -> is_rendering=%s progress=%.1f%%"),
			bIsRendering ? TEXT("true") : TEXT("false"),
			Executor ? Executor->GetStatusProgress() * 100.0f : 0.0f));
		return sol::make_object(Lua, Result);
	});

	// ---- mrq_cancel_render(cancel_all?) ----
	Lua.set_function("mrq_cancel_render", [&Session](sol::optional<bool> CancelAllOpt, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);

		UMoviePipelineQueueEngineSubsystem* Subsystem = GetMRQSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] mrq_cancel_render -> Movie Pipeline subsystem not available"));
			return sol::lua_nil;
		}

		if (!Subsystem->IsRendering())
		{
			Session.Log(TEXT("[FAIL] mrq_cancel_render -> no render is in progress"));
			return sol::lua_nil;
		}

		UMoviePipelineExecutorBase* Executor = Subsystem->GetActiveExecutor();
		if (!Executor)
		{
			Session.Log(TEXT("[FAIL] mrq_cancel_render -> no active executor"));
			return sol::lua_nil;
		}

		bool bCancelAll = CancelAllOpt.value_or(true);
		if (bCancelAll)
		{
			Executor->CancelAllJobs();
			Session.Log(TEXT("[OK] mrq_cancel_render -> cancelled all jobs"));
		}
		else
		{
			Executor->CancelCurrentJob();
			Session.Log(TEXT("[OK] mrq_cancel_render -> cancelled current job"));
		}

		return sol::make_object(Lua, true);
	});

	// ---- mrq_delete_job(index) ----
	Lua.set_function("mrq_delete_job", [&Session](int Index, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UMoviePipelineQueueEngineSubsystem* Subsystem = GetMRQSubsystem();
		if (!Subsystem) { Session.Log(TEXT("[FAIL] mrq_delete_job -> subsystem not available")); return sol::lua_nil; }
		UMoviePipelineQueue* Queue = Subsystem->GetQueue();
		if (!Queue) { Session.Log(TEXT("[FAIL] mrq_delete_job -> no queue")); return sol::lua_nil; }
		TArray<UMoviePipelineExecutorJob*> Jobs = Queue->GetJobs();
		int32 Idx = Index - 1;
		if (Idx < 0 || Idx >= Jobs.Num())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] mrq_delete_job -> index %d out of range (1-%d)"), Index, Jobs.Num()));
			return sol::lua_nil;
		}
		Queue->DeleteJob(Jobs[Idx]);
		Session.Log(FString::Printf(TEXT("[OK] mrq_delete_job(%d)"), Index));
		return sol::make_object(Lua, true);
	});

	// ---- mrq_delete_all_jobs() ----
	Lua.set_function("mrq_delete_all_jobs", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UMoviePipelineQueueEngineSubsystem* Subsystem = GetMRQSubsystem();
		if (!Subsystem) { Session.Log(TEXT("[FAIL] mrq_delete_all_jobs -> subsystem not available")); return sol::lua_nil; }
		UMoviePipelineQueue* Queue = Subsystem->GetQueue();
		if (!Queue) { Session.Log(TEXT("[FAIL] mrq_delete_all_jobs -> no queue")); return sol::lua_nil; }
		int32 Count = Queue->GetJobs().Num();
		Queue->DeleteAllJobs();
		Session.Log(FString::Printf(TEXT("[OK] mrq_delete_all_jobs -> removed %d jobs"), Count));
		return sol::make_object(Lua, true);
	});

	// ---- mrq_list_settings(filter?) ----
	Lua.set_function("mrq_list_settings", [&Session](sol::optional<std::string> FilterOpt, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString Filter = FilterOpt.has_value() ? UTF8_TO_TCHAR(FilterOpt.value().c_str()) : TEXT("");
		sol::table Result = Lua.create_table();
		int32 Idx = 1;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (!It->IsChildOf(UMoviePipelineSetting::StaticClass())) continue;
			if (It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated)) continue;
			FString Name = It->GetName();
			if (!Filter.IsEmpty() && !Name.Contains(Filter, ESearchCase::IgnoreCase)) continue;
			sol::table Entry = Lua.create_table();
			Entry["name"] = std::string(TCHAR_TO_UTF8(*Name));
			Entry["display_name"] = std::string(TCHAR_TO_UTF8(*It->GetDisplayNameText().ToString()));
			Result[Idx++] = Entry;
		}
		Session.Log(FString::Printf(TEXT("[OK] mrq_list_settings -> %d classes"), Idx - 1));
		return sol::make_object(Lua, Result);
	});

	// ---- mrq_get_queue() ----
	Lua.set_function("mrq_get_queue", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UMoviePipelineQueueEngineSubsystem* Subsystem = GetMRQSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] mrq_get_queue -> Movie Pipeline subsystem not available"));
			return sol::lua_nil;
		}

		UMoviePipelineQueue* Queue = Subsystem->GetQueue();
		if (!Queue)
		{
			Session.Log(TEXT("[FAIL] mrq_get_queue -> no queue available"));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();
		sol::table Jobs = Lua.create_table();

		TArray<UMoviePipelineExecutorJob*> JobList = Queue->GetJobs();
		for (int32 i = 0; i < JobList.Num(); ++i)
		{
			UMoviePipelineExecutorJob* Job = JobList[i];
			if (!Job) continue;

			sol::table JobInfo = Lua.create_table();
			JobInfo["index"] = i + 1;
			JobInfo["sequence"] = std::string(TCHAR_TO_UTF8(*Job->Sequence.GetAssetPathString()));
			JobInfo["map"] = std::string(TCHAR_TO_UTF8(*Job->Map.GetAssetPathString()));
			JobInfo["author"] = std::string(TCHAR_TO_UTF8(*Job->Author));
			JobInfo["job_name"] = std::string(TCHAR_TO_UTF8(*Job->JobName));
			JobInfo["comment"] = std::string(TCHAR_TO_UTF8(*Job->Comment));
			JobInfo["is_enabled"] = Job->IsEnabled();
			JobInfo["is_consumed"] = Job->IsConsumed();
			JobInfo["user_data"] = std::string(TCHAR_TO_UTF8(*Job->UserData));
			JobInfo["is_graph_config"] = Job->IsUsingGraphConfiguration();

			// List settings on this job's config
			UMoviePipelineConfigBase* Config = Job->GetConfiguration();
			if (Config)
			{
				sol::table SettingsList = Lua.create_table();
				TArray<UMoviePipelineSetting*> Settings = Config->GetUserSettings();
				int32 SIdx = 1;
				for (UMoviePipelineSetting* Setting : Settings)
				{
					if (!Setting) continue;
					sol::table SE = Lua.create_table();
					SE["name"] = std::string(TCHAR_TO_UTF8(*Setting->GetClass()->GetName()));
					SE["is_enabled"] = Setting->IsEnabled();
					SettingsList[SIdx++] = SE;
				}
				JobInfo["settings"] = SettingsList;
			}

			Jobs[i + 1] = JobInfo;
		}

		Result["jobs"] = Jobs;
		Result["count"] = JobList.Num();
		Result["is_rendering"] = Subsystem->IsRendering();

		Session.Log(FString::Printf(TEXT("[OK] mrq_get_queue -> %d jobs"), JobList.Num()));
		return sol::make_object(Lua, Result);
	});

	// ---- mrq_save_queue(asset_path) ----
	Lua.set_function("mrq_save_queue", [&Session](const std::string& AssetPath, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);

		UMoviePipelineQueueEngineSubsystem* Subsystem = GetMRQSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] mrq_save_queue -> Movie Pipeline subsystem not available"));
			return sol::lua_nil;
		}

		UMoviePipelineQueue* CurrentQueue = Subsystem->GetQueue();
		if (!CurrentQueue)
		{
			Session.Log(TEXT("[FAIL] mrq_save_queue -> no queue available"));
			return sol::lua_nil;
		}

		if (CurrentQueue->GetJobs().Num() == 0)
		{
			Session.Log(TEXT("[FAIL] mrq_save_queue -> queue is empty, nothing to save"));
			return sol::lua_nil;
		}

		FString FullPath = UTF8_TO_TCHAR(AssetPath.c_str());
		if (!FullPath.StartsWith(TEXT("/")))
			FullPath = TEXT("/Game/") + FullPath;

		FString PackageName = FullPath;
		FString AssetName = FPackageName::GetLongPackageAssetName(FullPath);

		// Check for existing asset and warn
		FString ObjectPath = PackageName + TEXT(".") + AssetName;
		UObject* Existing = LoadObject<UObject>(nullptr, *ObjectPath);
		if (Existing)
		{
			Session.Log(FString::Printf(TEXT("[WARN] mrq_save_queue -> overwriting existing asset at '%s'"), *FullPath));
		}

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] mrq_save_queue -> cannot create package '%s'"), *PackageName));
			return sol::lua_nil;
		}
		Package->MarkAsFullyLoaded();

		UMoviePipelineQueue* SavedQueue = DuplicateObject<UMoviePipelineQueue>(CurrentQueue, Package, *AssetName);
		if (!SavedQueue)
		{
			Session.Log(TEXT("[FAIL] mrq_save_queue -> failed to duplicate queue"));
			return sol::lua_nil;
		}

		SavedQueue->SetQueueOrigin(nullptr);
		SavedQueue->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
		SavedQueue->MarkPackageDirty();

		FAssetRegistryModule::AssetCreated(SavedQueue);

		FString PackageFilename = FPackageName::LongPackageNameToFilename(PackageName,
			FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Standalone;
		FSavePackageResultStruct SaveResult = UPackage::Save(Package, SavedQueue, *PackageFilename, SaveArgs);

		if (SaveResult.Result != ESavePackageResult::Success)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] mrq_save_queue -> save failed for '%s'"), *FullPath));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] mrq_save_queue -> saved %d jobs to '%s'"),
			CurrentQueue->GetJobs().Num(), *FullPath));
		return sol::make_object(Lua, true);
	});

	// ---- mrq_load_queue(asset_path) ----
	Lua.set_function("mrq_load_queue", [&Session](const std::string& AssetPath, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);

		UMoviePipelineQueueEngineSubsystem* Subsystem = GetMRQSubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] mrq_load_queue -> Movie Pipeline subsystem not available"));
			return sol::lua_nil;
		}

		if (Subsystem->IsRendering())
		{
			Session.Log(TEXT("[FAIL] mrq_load_queue -> a render is already in progress"));
			return sol::lua_nil;
		}

		FString FullPath = UTF8_TO_TCHAR(AssetPath.c_str());
		if (!FullPath.StartsWith(TEXT("/")))
			FullPath = TEXT("/Game/") + FullPath;

		UMoviePipelineQueue* LoadedQueue = LoadObject<UMoviePipelineQueue>(nullptr, *FullPath);
		if (!LoadedQueue)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] mrq_load_queue -> queue asset not found: %s"), *FullPath));
			return sol::lua_nil;
		}

		UMoviePipelineQueue* CurrentQueue = Subsystem->GetQueue();
		if (!CurrentQueue)
		{
			Session.Log(TEXT("[FAIL] mrq_load_queue -> no current queue available"));
			return sol::lua_nil;
		}

	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		UMoviePipelineQueue* Result = CurrentQueue->CopyFrom(LoadedQueue);
		if (!Result)
		{
			Session.Log(TEXT("[FAIL] mrq_load_queue -> CopyFrom failed"));
			return sol::lua_nil;
		}
#else
		CurrentQueue->CopyFrom(LoadedQueue);
#endif

		Session.Log(FString::Printf(TEXT("[OK] mrq_load_queue -> loaded %d jobs from '%s'"),
			CurrentQueue->GetJobs().Num(), *FullPath));
		return sol::make_object(Lua, true);
	});
}

REGISTER_LUA_BINDING(MovieRenderQueue, MovieRenderQueueDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindMovieRenderQueue(Lua, Session);
});
