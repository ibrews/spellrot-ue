// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"

#include "LevelInstance/LevelInstanceActor.h"
#include "LevelInstance/LevelInstanceInterface.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "LevelInstance/LevelInstanceTypes.h"
#include "PackedLevelActor/PackedLevelActor.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorActorSubsystem.h"

#if WITH_EDITOR
#include "PackedLevelActor/PackedLevelActorBuilder.h"
#endif

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ─── Documentation ───

static TArray<FLuaFunctionDoc> LevelInstanceDocs = {
	{ TEXT("li_list()"), TEXT("List all level instances in the current editor world"), TEXT("table[]") },
	{ TEXT("li_get(name_or_label)"), TEXT("Get detailed info about a specific level instance"), TEXT("table or nil") },
	{ TEXT("li_list_actors(name_or_label)"), TEXT("List all actors inside a level instance"), TEXT("table[] or nil") },
	{ TEXT("li_list_children(name_or_label, recursive?)"), TEXT("List child level instances"), TEXT("table[] or nil") },
	{ TEXT("li_load(name_or_label)"), TEXT("Load a level instance by name or label"), TEXT("true or nil") },
	{ TEXT("li_unload(name_or_label)"), TEXT("Unload a level instance by name or label"), TEXT("true or nil") },
	{ TEXT("li_edit(name_or_label)"), TEXT("Enter edit mode for a level instance"), TEXT("true or nil") },
	{ TEXT("li_commit(name_or_label)"), TEXT("Commit changes to a level instance being edited"), TEXT("true or nil") },
	{ TEXT("li_discard(name_or_label)"), TEXT("Discard changes to a level instance being edited"), TEXT("true or nil") },
	{ TEXT("li_can_edit(name_or_label)"), TEXT("Check if a level instance can enter edit mode"), TEXT("{can_edit, reason}") },
	{ TEXT("li_can_commit(name_or_label, discard?)"), TEXT("Check if a level instance can commit/discard"), TEXT("{can_commit, reason}") },
	{ TEXT("li_is_editing(name_or_label)"), TEXT("Check if a level instance is currently being edited"), TEXT("bool") },
	{ TEXT("li_is_dirty(name_or_label)"), TEXT("Check if a level instance has unsaved changes"), TEXT("bool") },
	{ TEXT("li_has_dirty_children(name_or_label)"), TEXT("Check if any child level instances have unsaved changes"), TEXT("bool") },
	{ TEXT("li_has_parent_edit(name_or_label)"), TEXT("Check if any parent level instance is being edited"), TEXT("bool") },
	{ TEXT("li_current_edit()"), TEXT("Get the label of the level instance currently being edited"), TEXT("string or nil") },
	{ TEXT("li_set_current(name_or_label)"), TEXT("Set a level instance as the current editing context"), TEXT("true or nil") },
	{ TEXT("li_create(params)"), TEXT("Create a new level instance from actors — params: actors, package, type, pivot"), TEXT("{success, label} or nil") },
	{ TEXT("li_break(name_or_label, levels?)"), TEXT("Break a level instance, moving its actors into the current level"), TEXT("{success, moved_actor_count} or nil") },
	{ TEXT("li_move_actors(name_or_label, actor_labels)"), TEXT("Move actors into a level instance"), TEXT("true or nil") },
	{ TEXT("li_set_world_asset(name_or_label, world_path)"), TEXT("Set the world asset for a level instance"), TEXT("true or nil") },
	{ TEXT("li_set_runtime_behavior(name_or_label, behavior)"), TEXT("Set runtime behavior — Partitioned, LevelStreaming, or None"), TEXT("true or nil") },
	{ TEXT("li_set_hidden(name_or_label, hidden)"), TEXT("Temporarily hide or show a level instance in the editor"), TEXT("true or nil") },
	{ TEXT("pla_pack(name_or_label)"), TEXT("Pack a specific packed level actor using FPackedLevelActorBuilder"), TEXT("true or nil") },
	{ TEXT("pla_create_blueprint(name_or_label, blueprint_path)"), TEXT("Create or update a blueprint from a packed level actor"), TEXT("true or nil") },
};

// ─── Helpers ───
namespace LevelInstanceHelpers
{

UWorld* GetEditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

ULevelInstanceSubsystem* GetLISubsystem()
{
	UWorld* World = GetEditorWorld();
	return World ? World->GetSubsystem<ULevelInstanceSubsystem>() : nullptr;
}

ILevelInstanceInterface* FindLevelInstance(const FString& NameOrLabel)
{
	UWorld* World = GetEditorWorld();
	if (!World) return nullptr;
	for (TActorIterator<ALevelInstance> It(World); It; ++It)
	{
		if (It->GetActorLabel().Equals(NameOrLabel, ESearchCase::IgnoreCase) ||
			It->GetName().Equals(NameOrLabel, ESearchCase::IgnoreCase))
		{
			return Cast<ILevelInstanceInterface>(*It);
		}
	}
	return nullptr;
}

FString RuntimeBehaviorToString(ELevelInstanceRuntimeBehavior Behavior)
{
	switch (Behavior)
	{
	case ELevelInstanceRuntimeBehavior::Partitioned:    return TEXT("Partitioned");
	case ELevelInstanceRuntimeBehavior::LevelStreaming: return TEXT("LevelStreaming");
	case ELevelInstanceRuntimeBehavior::None:           return TEXT("None");
	default:                                            return TEXT("Unknown");
	}
}

AActor* FindActorByLabel(UWorld* World, const FString& Label)
{
	if (!World || !GEditor) return nullptr;
	UEditorActorSubsystem* ActorSub = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
	if (!ActorSub) return nullptr;
	TArray<AActor*> AllActors = ActorSub->GetAllLevelActors();
	for (AActor* Actor : AllActors)
	{
		if (Actor && Actor->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase))
		{
			return Actor;
		}
	}
	return nullptr;
}

} // namespace LevelInstanceHelpers

// ─── Binding ───

static void BindLevelInstance(sol::state& Lua, FLuaSessionData& Session)
{
	// ──────────────────────────────────────────────
	// 1. li_list()
	// ──────────────────────────────────────────────
	Lua.set_function("li_list", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = LevelInstanceHelpers::GetEditorWorld();
		if (!World)
		{
			Session.Log(TEXT("[FAIL] li_list -> no editor world"));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();
		int32 Idx = 1;

		for (TActorIterator<ALevelInstance> It(World); It; ++It)
		{
			ALevelInstance* Actor = *It;
			if (!Actor) continue;

			ILevelInstanceInterface* LI = Cast<ILevelInstanceInterface>(Actor);
			if (!LI) continue;

			sol::table Entry = Lua.create_table();
			Entry["label"] = std::string(TCHAR_TO_UTF8(*Actor->GetActorLabel()));
			Entry["name"] = std::string(TCHAR_TO_UTF8(*Actor->GetName()));
			Entry["world_asset"] = std::string(TCHAR_TO_UTF8(*LI->GetWorldAsset().GetUniqueID().GetLongPackageName()));
			Entry["is_loaded"] = LI->IsLoaded();
			Entry["is_packed"] = (Cast<APackedLevelActor>(Actor) != nullptr);

#if WITH_EDITOR
			Entry["is_editing"] = LI->IsEditing();
			Entry["is_dirty"] = LI->IsDirty();
			Entry["has_child_edit"] = LI->HasChildEdit();
			Entry["has_parent_edit"] = LI->HasParentEdit();
			Entry["has_dirty_children"] = LI->HasDirtyChildren();
			Entry["runtime_behavior"] = std::string(TCHAR_TO_UTF8(*LevelInstanceHelpers::RuntimeBehaviorToString(LI->GetDesiredRuntimeBehavior())));
#else
			Entry["is_editing"] = false;
			Entry["is_dirty"] = false;
			Entry["has_child_edit"] = false;
			Entry["has_parent_edit"] = false;
			Entry["has_dirty_children"] = false;
			Entry["runtime_behavior"] = std::string("Unknown");
#endif

			FVector Loc = Actor->GetActorLocation();
			sol::table LocTable = Lua.create_table();
			LocTable["x"] = Loc.X;
			LocTable["y"] = Loc.Y;
			LocTable["z"] = Loc.Z;
			Entry["location"] = LocTable;

			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] li_list -> %d level instance(s)"), Idx - 1));
		return sol::make_object(Lua, Result);
	});

	// ──────────────────────────────────────────────
	// 2. li_get(name_or_label)
	// ──────────────────────────────────────────────
	Lua.set_function("li_get", [&Session](std::string InName, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString NameOrLabel = UTF8_TO_TCHAR(InName.c_str());

		ILevelInstanceInterface* LI = LevelInstanceHelpers::FindLevelInstance(NameOrLabel);
		if (!LI)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_get -> level instance '%s' not found"), *NameOrLabel));
			return sol::lua_nil;
		}

		AActor* Actor = Cast<AActor>(LI);
		if (!Actor)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_get -> level instance '%s' is not an actor"), *NameOrLabel));
			return sol::lua_nil;
		}

		ULevelInstanceSubsystem* Subsystem = LevelInstanceHelpers::GetLISubsystem();

		sol::table Entry = Lua.create_table();
		Entry["label"] = std::string(TCHAR_TO_UTF8(*Actor->GetActorLabel()));
		Entry["name"] = std::string(TCHAR_TO_UTF8(*Actor->GetName()));
		Entry["world_asset"] = std::string(TCHAR_TO_UTF8(*LI->GetWorldAsset().GetUniqueID().GetLongPackageName()));
		Entry["is_loaded"] = LI->IsLoaded();
		Entry["is_packed"] = (Cast<APackedLevelActor>(Actor) != nullptr);

#if WITH_EDITOR
		Entry["is_editing"] = LI->IsEditing();
		Entry["is_dirty"] = LI->IsDirty();
		Entry["has_child_edit"] = LI->HasChildEdit();
		Entry["has_parent_edit"] = LI->HasParentEdit();
		Entry["has_dirty_children"] = LI->HasDirtyChildren();
		Entry["runtime_behavior"] = std::string(TCHAR_TO_UTF8(*LevelInstanceHelpers::RuntimeBehaviorToString(LI->GetDesiredRuntimeBehavior())));

		// Bounds
		if (Subsystem)
		{
			FBox OutBounds;
			if (Subsystem->GetLevelInstanceBounds(LI, OutBounds) && OutBounds.IsValid)
			{
				sol::table Bounds = Lua.create_table();
				sol::table MinT = Lua.create_table();
				MinT["x"] = OutBounds.Min.X;
				MinT["y"] = OutBounds.Min.Y;
				MinT["z"] = OutBounds.Min.Z;
				Bounds["min"] = MinT;
				sol::table MaxT = Lua.create_table();
				MaxT["x"] = OutBounds.Max.X;
				MaxT["y"] = OutBounds.Max.Y;
				MaxT["z"] = OutBounds.Max.Z;
				Bounds["max"] = MaxT;
				Entry["bounds"] = Bounds;
			}
		}

		// Actor count via ForEachActorInLevelInstance
		if (Subsystem)
		{
			int32 ActorCount = 0;
			Subsystem->ForEachActorInLevelInstance(LI, [&ActorCount](AActor* LevelActor) -> bool
			{
				ActorCount++;
				return true;
			});
			Entry["actor_count"] = ActorCount;
		}
#else
		Entry["is_editing"] = false;
		Entry["is_dirty"] = false;
		Entry["has_child_edit"] = false;
		Entry["has_parent_edit"] = false;
		Entry["has_dirty_children"] = false;
		Entry["runtime_behavior"] = std::string("Unknown");
		Entry["actor_count"] = 0;
#endif

		Session.Log(FString::Printf(TEXT("[OK] li_get -> '%s'"), *NameOrLabel));
		return sol::make_object(Lua, Entry);
	});

	// ──────────────────────────────────────────────
	// 3. li_list_actors(name_or_label)
	// ──────────────────────────────────────────────
	Lua.set_function("li_list_actors", [&Session](std::string InName, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString NameOrLabel = UTF8_TO_TCHAR(InName.c_str());

		ILevelInstanceInterface* LI = LevelInstanceHelpers::FindLevelInstance(NameOrLabel);
		if (!LI)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_list_actors -> level instance '%s' not found"), *NameOrLabel));
			return sol::lua_nil;
		}

		ULevelInstanceSubsystem* Subsystem = LevelInstanceHelpers::GetLISubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] li_list_actors -> no LevelInstance subsystem"));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();
		int32 Idx = 1;

		Subsystem->ForEachActorInLevelInstance(LI, [&](AActor* LevelActor) -> bool
		{
			if (!LevelActor) return true;
			sol::table Entry = Lua.create_table();
			Entry["name"] = std::string(TCHAR_TO_UTF8(*LevelActor->GetName()));
			Entry["label"] = std::string(TCHAR_TO_UTF8(*LevelActor->GetActorLabel()));
			Entry["class"] = std::string(TCHAR_TO_UTF8(*LevelActor->GetClass()->GetName()));
			Result[Idx++] = Entry;
			return true;
		});

		Session.Log(FString::Printf(TEXT("[OK] li_list_actors -> %d actor(s) in '%s'"), Idx - 1, *NameOrLabel));
		return sol::make_object(Lua, Result);
	});

#if WITH_EDITOR
	// ──────────────────────────────────────────────
	// 3b. li_list_children(name_or_label, recursive?)
	// ──────────────────────────────────────────────
	Lua.set_function("li_list_children", [&Session](std::string InName, sol::optional<bool> bRecursive, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString NameOrLabel = UTF8_TO_TCHAR(InName.c_str());

		ILevelInstanceInterface* LI = LevelInstanceHelpers::FindLevelInstance(NameOrLabel);
		if (!LI)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_list_children -> level instance '%s' not found"), *NameOrLabel));
			return sol::lua_nil;
		}

		ULevelInstanceSubsystem* Subsystem = LevelInstanceHelpers::GetLISubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] li_list_children -> no LevelInstance subsystem"));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();
		int32 Idx = 1;
		bool bRecurse = bRecursive.value_or(false);

		Subsystem->ForEachLevelInstanceChild(LI, bRecurse, [&](ILevelInstanceInterface* ChildLI) -> bool
		{
			AActor* ChildActor = Cast<AActor>(ChildLI);
			if (!ChildActor) return true;

			sol::table Entry = Lua.create_table();
			Entry["label"] = std::string(TCHAR_TO_UTF8(*ChildActor->GetActorLabel()));
			Entry["name"] = std::string(TCHAR_TO_UTF8(*ChildActor->GetName()));
			Entry["world_asset"] = std::string(TCHAR_TO_UTF8(*ChildLI->GetWorldAsset().GetUniqueID().GetLongPackageName()));
			Entry["is_loaded"] = ChildLI->IsLoaded();
			Entry["is_packed"] = (Cast<APackedLevelActor>(ChildActor) != nullptr);
			Entry["is_editing"] = ChildLI->IsEditing();
			Entry["is_dirty"] = ChildLI->IsDirty();
			Result[Idx++] = Entry;
			return true;
		});

		Session.Log(FString::Printf(TEXT("[OK] li_list_children -> %d child(ren) in '%s'"), Idx - 1, *NameOrLabel));
		return sol::make_object(Lua, Result);
	});
#endif

	// ──────────────────────────────────────────────
	// 4. li_load(name_or_label)
	// ──────────────────────────────────────────────
	Lua.set_function("li_load", [&Session](std::string InName, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString NameOrLabel = UTF8_TO_TCHAR(InName.c_str());

		ILevelInstanceInterface* LI = LevelInstanceHelpers::FindLevelInstance(NameOrLabel);
		if (!LI)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_load -> level instance '%s' not found"), *NameOrLabel));
			return sol::lua_nil;
		}

		LI->LoadLevelInstance();
		Session.Log(FString::Printf(TEXT("[OK] li_load -> '%s'"), *NameOrLabel));
		return sol::make_object(Lua, true);
	});

	// ──────────────────────────────────────────────
	// 5. li_unload(name_or_label)
	// ──────────────────────────────────────────────
	Lua.set_function("li_unload", [&Session](std::string InName, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString NameOrLabel = UTF8_TO_TCHAR(InName.c_str());

		ILevelInstanceInterface* LI = LevelInstanceHelpers::FindLevelInstance(NameOrLabel);
		if (!LI)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_unload -> level instance '%s' not found"), *NameOrLabel));
			return sol::lua_nil;
		}

		LI->UnloadLevelInstance();
		Session.Log(FString::Printf(TEXT("[OK] li_unload -> '%s'"), *NameOrLabel));
		return sol::make_object(Lua, true);
	});

#if WITH_EDITOR
	// ──────────────────────────────────────────────
	// 6. li_edit(name_or_label)
	// ──────────────────────────────────────────────
	Lua.set_function("li_edit", [&Session](std::string InName, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString NameOrLabel = UTF8_TO_TCHAR(InName.c_str());

		ILevelInstanceInterface* LI = LevelInstanceHelpers::FindLevelInstance(NameOrLabel);
		if (!LI)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_edit -> level instance '%s' not found"), *NameOrLabel));
			return sol::lua_nil;
		}

		// Check preconditions first to avoid check() crash inside EnterEdit
		FText Reason;
		if (!LI->CanEnterEdit(&Reason))
		{
			FString ReasonStr = Reason.ToString();
			Session.Log(FString::Printf(TEXT("[FAIL] li_edit -> cannot enter edit mode for '%s': %s"), *NameOrLabel, *ReasonStr));
			return sol::lua_nil;
		}

		LI->EnterEdit();
		Session.Log(FString::Printf(TEXT("[OK] li_edit -> entered edit mode for '%s'"), *NameOrLabel));
		return sol::make_object(Lua, true);
	});

	// ──────────────────────────────────────────────
	// 7. li_commit(name_or_label)
	// ──────────────────────────────────────────────
	Lua.set_function("li_commit", [&Session](std::string InName, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString NameOrLabel = UTF8_TO_TCHAR(InName.c_str());

		ILevelInstanceInterface* LI = LevelInstanceHelpers::FindLevelInstance(NameOrLabel);
		if (!LI)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_commit -> level instance '%s' not found"), *NameOrLabel));
			return sol::lua_nil;
		}

		FText Reason;
		if (!LI->CanExitEdit(false, &Reason))
		{
			FString ReasonStr = Reason.ToString();
			Session.Log(FString::Printf(TEXT("[FAIL] li_commit -> cannot commit '%s': %s"), *NameOrLabel, *ReasonStr));
			return sol::lua_nil;
		}

		bool bSuccess = LI->ExitEdit(false);
		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] li_commit -> committed changes for '%s'"), *NameOrLabel));
			return sol::make_object(Lua, true);
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_commit -> could not commit changes for '%s'"), *NameOrLabel));
			return sol::lua_nil;
		}
	});

	// ──────────────────────────────────────────────
	// 8. li_discard(name_or_label)
	// ──────────────────────────────────────────────
	Lua.set_function("li_discard", [&Session](std::string InName, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString NameOrLabel = UTF8_TO_TCHAR(InName.c_str());

		ILevelInstanceInterface* LI = LevelInstanceHelpers::FindLevelInstance(NameOrLabel);
		if (!LI)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_discard -> level instance '%s' not found"), *NameOrLabel));
			return sol::lua_nil;
		}

		FText Reason;
		if (!LI->CanExitEdit(true, &Reason))
		{
			FString ReasonStr = Reason.ToString();
			Session.Log(FString::Printf(TEXT("[FAIL] li_discard -> cannot discard '%s': %s"), *NameOrLabel, *ReasonStr));
			return sol::lua_nil;
		}

		bool bSuccess = LI->ExitEdit(true);
		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] li_discard -> discarded changes for '%s'"), *NameOrLabel));
			return sol::make_object(Lua, true);
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_discard -> could not discard changes for '%s'"), *NameOrLabel));
			return sol::lua_nil;
		}
	});

	// ──────────────────────────────────────────────
	// 8b. li_can_edit(name_or_label)
	// ──────────────────────────────────────────────
	Lua.set_function("li_can_edit", [&Session](std::string InName, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString NameOrLabel = UTF8_TO_TCHAR(InName.c_str());

		ILevelInstanceInterface* LI = LevelInstanceHelpers::FindLevelInstance(NameOrLabel);
		if (!LI)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_can_edit -> level instance '%s' not found"), *NameOrLabel));
			return sol::lua_nil;
		}

		FText Reason;
		bool bCanEdit = LI->CanEnterEdit(&Reason);

		sol::table Result = Lua.create_table();
		Result["can_edit"] = bCanEdit;
		Result["reason"] = std::string(TCHAR_TO_UTF8(*Reason.ToString()));

		Session.Log(FString::Printf(TEXT("[OK] li_can_edit -> '%s' = %s"), *NameOrLabel, bCanEdit ? TEXT("true") : TEXT("false")));
		return sol::make_object(Lua, Result);
	});

	// ──────────────────────────────────────────────
	// 8c. li_can_commit(name_or_label, discard?)
	// ──────────────────────────────────────────────
	Lua.set_function("li_can_commit", [&Session](std::string InName, sol::optional<bool> bDiscard, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString NameOrLabel = UTF8_TO_TCHAR(InName.c_str());

		ILevelInstanceInterface* LI = LevelInstanceHelpers::FindLevelInstance(NameOrLabel);
		if (!LI)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_can_commit -> level instance '%s' not found"), *NameOrLabel));
			return sol::lua_nil;
		}

		FText Reason;
		bool bDiscardEdits = bDiscard.value_or(false);
		bool bCanCommit = LI->CanExitEdit(bDiscardEdits, &Reason);

		sol::table Result = Lua.create_table();
		Result["can_commit"] = bCanCommit;
		Result["reason"] = std::string(TCHAR_TO_UTF8(*Reason.ToString()));

		Session.Log(FString::Printf(TEXT("[OK] li_can_commit -> '%s' = %s"), *NameOrLabel, bCanCommit ? TEXT("true") : TEXT("false")));
		return sol::make_object(Lua, Result);
	});

	// ──────────────────────────────────────────────
	// 9. li_is_editing(name_or_label)
	// ──────────────────────────────────────────────
	Lua.set_function("li_is_editing", [&Session](std::string InName, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString NameOrLabel = UTF8_TO_TCHAR(InName.c_str());

		ILevelInstanceInterface* LI = LevelInstanceHelpers::FindLevelInstance(NameOrLabel);
		if (!LI)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_is_editing -> level instance '%s' not found"), *NameOrLabel));
			return sol::lua_nil;
		}

		bool bEditing = LI->IsEditing();
		Session.Log(FString::Printf(TEXT("[OK] li_is_editing -> '%s' = %s"), *NameOrLabel, bEditing ? TEXT("true") : TEXT("false")));
		return sol::make_object(Lua, bEditing);
	});

	// ──────────────────────────────────────────────
	// 10. li_is_dirty(name_or_label)
	// ──────────────────────────────────────────────
	Lua.set_function("li_is_dirty", [&Session](std::string InName, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString NameOrLabel = UTF8_TO_TCHAR(InName.c_str());

		ILevelInstanceInterface* LI = LevelInstanceHelpers::FindLevelInstance(NameOrLabel);
		if (!LI)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_is_dirty -> level instance '%s' not found"), *NameOrLabel));
			return sol::lua_nil;
		}

		bool bDirty = LI->IsDirty();
		Session.Log(FString::Printf(TEXT("[OK] li_is_dirty -> '%s' = %s"), *NameOrLabel, bDirty ? TEXT("true") : TEXT("false")));
		return sol::make_object(Lua, bDirty);
	});

	// ──────────────────────────────────────────────
	// 10b. li_has_dirty_children(name_or_label)
	// ──────────────────────────────────────────────
	Lua.set_function("li_has_dirty_children", [&Session](std::string InName, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString NameOrLabel = UTF8_TO_TCHAR(InName.c_str());

		ILevelInstanceInterface* LI = LevelInstanceHelpers::FindLevelInstance(NameOrLabel);
		if (!LI)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_has_dirty_children -> level instance '%s' not found"), *NameOrLabel));
			return sol::lua_nil;
		}

		bool bDirtyChildren = LI->HasDirtyChildren();
		Session.Log(FString::Printf(TEXT("[OK] li_has_dirty_children -> '%s' = %s"), *NameOrLabel, bDirtyChildren ? TEXT("true") : TEXT("false")));
		return sol::make_object(Lua, bDirtyChildren);
	});

	// ──────────────────────────────────────────────
	// 10c. li_has_parent_edit(name_or_label)
	// ──────────────────────────────────────────────
	Lua.set_function("li_has_parent_edit", [&Session](std::string InName, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString NameOrLabel = UTF8_TO_TCHAR(InName.c_str());

		ILevelInstanceInterface* LI = LevelInstanceHelpers::FindLevelInstance(NameOrLabel);
		if (!LI)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_has_parent_edit -> level instance '%s' not found"), *NameOrLabel));
			return sol::lua_nil;
		}

		bool bHasParentEdit = LI->HasParentEdit();
		Session.Log(FString::Printf(TEXT("[OK] li_has_parent_edit -> '%s' = %s"), *NameOrLabel, bHasParentEdit ? TEXT("true") : TEXT("false")));
		return sol::make_object(Lua, bHasParentEdit);
	});

	// ──────────────────────────────────────────────
	// 11. li_current_edit()
	// ──────────────────────────────────────────────
	Lua.set_function("li_current_edit", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		ULevelInstanceSubsystem* Subsystem = LevelInstanceHelpers::GetLISubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] li_current_edit -> no LevelInstance subsystem"));
			return sol::lua_nil;
		}

		ILevelInstanceInterface* Editing = Subsystem->GetEditingLevelInstance();
		if (!Editing)
		{
			Session.Log(TEXT("[OK] li_current_edit -> no level instance is currently being edited"));
			return sol::lua_nil;
		}

		AActor* EditActor = Cast<AActor>(Editing);
		FString Label = EditActor ? EditActor->GetActorLabel() : TEXT("Unknown");
		Session.Log(FString::Printf(TEXT("[OK] li_current_edit -> '%s'"), *Label));
		return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*Label)));
	});

	// ──────────────────────────────────────────────
	// 11b. li_set_current(name_or_label)
	// ──────────────────────────────────────────────
	Lua.set_function("li_set_current", [&Session](std::string InName, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString NameOrLabel = UTF8_TO_TCHAR(InName.c_str());

		ILevelInstanceInterface* LI = LevelInstanceHelpers::FindLevelInstance(NameOrLabel);
		if (!LI)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_set_current -> level instance '%s' not found"), *NameOrLabel));
			return sol::lua_nil;
		}

		bool bSuccess = LI->SetCurrent();
		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] li_set_current -> '%s' set as current"), *NameOrLabel));
			return sol::make_object(Lua, true);
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_set_current -> could not set '%s' as current"), *NameOrLabel));
			return sol::lua_nil;
		}
	});

	// ──────────────────────────────────────────────
	// 12. li_create(params)
	// ──────────────────────────────────────────────
	Lua.set_function("li_create", [&Session](sol::table Params, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		ULevelInstanceSubsystem* Subsystem = LevelInstanceHelpers::GetLISubsystem();
		UWorld* World = LevelInstanceHelpers::GetEditorWorld();
		if (!Subsystem || !World)
		{
			Session.Log(TEXT("[FAIL] li_create -> no editor world or subsystem"));
			return sol::lua_nil;
		}

		// Gather actors by label
		sol::optional<sol::table> ActorLabels = Params["actors"];
		if (!ActorLabels.has_value())
		{
			Session.Log(TEXT("[FAIL] li_create -> 'actors' table is required"));
			return sol::lua_nil;
		}

		TArray<AActor*> ActorsToMove;
		sol::table Labels = ActorLabels.value();
		for (auto& Pair : Labels)
		{
			std::string LabelStr = Pair.second.as<std::string>();
			FString Label = UTF8_TO_TCHAR(LabelStr.c_str());
			AActor* Found = LevelInstanceHelpers::FindActorByLabel(World, Label);
			if (Found)
			{
				ActorsToMove.Add(Found);
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[FAIL] li_create -> actor '%s' not found"), *Label));
				return sol::lua_nil;
			}
		}

		if (ActorsToMove.Num() == 0)
		{
			Session.Log(TEXT("[FAIL] li_create -> no actors to move"));
			return sol::lua_nil;
		}

		// Build params
		FNewLevelInstanceParams CreationParams;

		sol::optional<std::string> PackageName = Params["package"];
		if (PackageName.has_value())
		{
			CreationParams.LevelPackageName = UTF8_TO_TCHAR(PackageName.value().c_str());
		}

		sol::optional<std::string> TypeStr = Params["type"];
		if (TypeStr.has_value())
		{
			FString TypeFStr = UTF8_TO_TCHAR(TypeStr.value().c_str());
			if (TypeFStr.Equals(TEXT("PackedLevelActor"), ESearchCase::IgnoreCase))
			{
				CreationParams.Type = ELevelInstanceCreationType::PackedLevelActor;
			}
			else
			{
				CreationParams.Type = ELevelInstanceCreationType::LevelInstance;
			}
		}

		sol::optional<std::string> PivotStr = Params["pivot"];
		if (PivotStr.has_value())
		{
			FString PivotFStr = UTF8_TO_TCHAR(PivotStr.value().c_str());
			if (PivotFStr.Equals(TEXT("Center"), ESearchCase::IgnoreCase))
			{
				CreationParams.PivotType = ELevelInstancePivotType::Center;
			}
			else if (PivotFStr.Equals(TEXT("WorldOrigin"), ESearchCase::IgnoreCase))
			{
				CreationParams.PivotType = ELevelInstancePivotType::WorldOrigin;
			}
			else
			{
				CreationParams.PivotType = ELevelInstancePivotType::CenterMinZ;
			}
		}

		CreationParams.bAlwaysShowDialog = false;
		CreationParams.bPromptForSave = false;

		FScopedTransaction Transaction(FText::FromString(TEXT("Create Level Instance")));
		ILevelInstanceInterface* NewLI = Subsystem->CreateLevelInstanceFrom(ActorsToMove, CreationParams);
		if (!NewLI)
		{
			Session.Log(TEXT("[FAIL] li_create -> CreateLevelInstanceFrom failed"));
			return sol::lua_nil;
		}

		AActor* NewActor = Cast<AActor>(NewLI);
		FString NewLabel = NewActor ? NewActor->GetActorLabel() : TEXT("Unknown");

		sol::table Result = Lua.create_table();
		Result["success"] = true;
		Result["label"] = std::string(TCHAR_TO_UTF8(*NewLabel));

		Session.Log(FString::Printf(TEXT("[OK] li_create -> created '%s' from %d actor(s)"), *NewLabel, ActorsToMove.Num()));
		return sol::make_object(Lua, Result);
	});

	// ──────────────────────────────────────────────
	// 13. li_break(name_or_label, levels?)
	// ──────────────────────────────────────────────
	Lua.set_function("li_break", [&Session](std::string InName, sol::optional<int> Levels, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString NameOrLabel = UTF8_TO_TCHAR(InName.c_str());

		ULevelInstanceSubsystem* Subsystem = LevelInstanceHelpers::GetLISubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] li_break -> no LevelInstance subsystem"));
			return sol::lua_nil;
		}

		ILevelInstanceInterface* LI = LevelInstanceHelpers::FindLevelInstance(NameOrLabel);
		if (!LI)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_break -> level instance '%s' not found"), *NameOrLabel));
			return sol::lua_nil;
		}

		uint32 NumLevels = Levels.value_or(1);
		TArray<AActor*> MovedActors;

		FScopedTransaction Transaction(FText::FromString(TEXT("Break Level Instance")));
		bool bSuccess = Subsystem->BreakLevelInstance(LI, NumLevels, &MovedActors);

		sol::table Result = Lua.create_table();
		Result["success"] = bSuccess;
		Result["moved_actor_count"] = MovedActors.Num();

		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] li_break -> broke '%s', moved %d actor(s)"), *NameOrLabel, MovedActors.Num()));
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_break -> could not break '%s'"), *NameOrLabel));
		}

		return sol::make_object(Lua, Result);
	});

	// ──────────────────────────────────────────────
	// 14. li_move_actors(name_or_label, actor_labels)
	// ──────────────────────────────────────────────
	Lua.set_function("li_move_actors", [&Session](std::string InName, sol::table InLabels, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString NameOrLabel = UTF8_TO_TCHAR(InName.c_str());

		ULevelInstanceSubsystem* Subsystem = LevelInstanceHelpers::GetLISubsystem();
		UWorld* World = LevelInstanceHelpers::GetEditorWorld();
		if (!Subsystem || !World)
		{
			Session.Log(TEXT("[FAIL] li_move_actors -> no editor world or subsystem"));
			return sol::lua_nil;
		}

		ILevelInstanceInterface* LI = LevelInstanceHelpers::FindLevelInstance(NameOrLabel);
		if (!LI)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_move_actors -> level instance '%s' not found"), *NameOrLabel));
			return sol::lua_nil;
		}

		TArray<AActor*> ActorsToMove;
		for (auto& Pair : InLabels)
		{
			std::string LabelStr = Pair.second.as<std::string>();
			FString Label = UTF8_TO_TCHAR(LabelStr.c_str());
			AActor* Found = LevelInstanceHelpers::FindActorByLabel(World, Label);
			if (Found)
			{
				ActorsToMove.Add(Found);
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[FAIL] li_move_actors -> actor '%s' not found"), *Label));
				return sol::lua_nil;
			}
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Move Actors to Level Instance")));
		bool bSuccess = Subsystem->MoveActorsTo(LI, ActorsToMove);
		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] li_move_actors -> moved %d actor(s) to '%s'"), ActorsToMove.Num(), *NameOrLabel));
			return sol::make_object(Lua, true);
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_move_actors -> could not move actors to '%s'"), *NameOrLabel));
			return sol::lua_nil;
		}
	});

	// ──────────────────────────────────────────────
	// 15. li_set_world_asset(name_or_label, world_path)
	// ──────────────────────────────────────────────
	Lua.set_function("li_set_world_asset", [&Session](std::string InName, std::string InPath, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString NameOrLabel = UTF8_TO_TCHAR(InName.c_str());
		FString WorldPath = UTF8_TO_TCHAR(InPath.c_str());

		ILevelInstanceInterface* LI = LevelInstanceHelpers::FindLevelInstance(NameOrLabel);
		if (!LI)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_set_world_asset -> level instance '%s' not found"), *NameOrLabel));
			return sol::lua_nil;
		}

		AActor* Actor = Cast<AActor>(LI);
		if (!Actor)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_set_world_asset -> level instance '%s' is not an actor"), *NameOrLabel));
			return sol::lua_nil;
		}

		FSoftObjectPath WorldSoftPath(WorldPath);
		TSoftObjectPtr<UWorld> NewWorldAsset(WorldSoftPath);

		FScopedTransaction Transaction(FText::FromString(TEXT("Set Level Instance World Asset")));
		Actor->Modify();

		bool bSuccess = LI->SetWorldAsset(NewWorldAsset);
		if (bSuccess)
		{
			// Trigger the actual level reload — SetWorldAsset only sets the member variable.
			// The engine's PostEditChangeProperty flow calls UpdateLevelInstanceFromWorldAsset;
			// since we're setting programmatically, we call it directly.
			LI->UpdateLevelInstanceFromWorldAsset();

			Session.Log(FString::Printf(TEXT("[OK] li_set_world_asset -> '%s' set to '%s'"), *NameOrLabel, *WorldPath));
			return sol::make_object(Lua, true);
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_set_world_asset -> could not set world asset for '%s'"), *NameOrLabel));
			return sol::lua_nil;
		}
	});

	// ──────────────────────────────────────────────
	// 16. li_set_runtime_behavior(name_or_label, behavior)
	// ──────────────────────────────────────────────
	Lua.set_function("li_set_runtime_behavior", [&Session](std::string InName, std::string InBehavior, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString NameOrLabel = UTF8_TO_TCHAR(InName.c_str());
		FString BehaviorStr = UTF8_TO_TCHAR(InBehavior.c_str());

		ILevelInstanceInterface* LI = LevelInstanceHelpers::FindLevelInstance(NameOrLabel);
		if (!LI)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_set_runtime_behavior -> level instance '%s' not found"), *NameOrLabel));
			return sol::lua_nil;
		}

		AActor* Actor = Cast<AActor>(LI);
		if (!Actor)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_set_runtime_behavior -> level instance '%s' is not an actor"), *NameOrLabel));
			return sol::lua_nil;
		}

		ELevelInstanceRuntimeBehavior NewBehavior;
		if (BehaviorStr.Equals(TEXT("Partitioned"), ESearchCase::IgnoreCase))
		{
			NewBehavior = ELevelInstanceRuntimeBehavior::Partitioned;
		}
		else if (BehaviorStr.Equals(TEXT("LevelStreaming"), ESearchCase::IgnoreCase))
		{
			NewBehavior = ELevelInstanceRuntimeBehavior::LevelStreaming;
		}
		else if (BehaviorStr.Equals(TEXT("None"), ESearchCase::IgnoreCase))
		{
			NewBehavior = ELevelInstanceRuntimeBehavior::None;
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_set_runtime_behavior -> unknown behavior '%s' (use Partitioned, LevelStreaming, or None)"), *BehaviorStr));
			return sol::lua_nil;
		}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		FScopedTransaction Transaction(FText::FromString(TEXT("Set Level Instance Runtime Behavior")));
		Actor->Modify();
		LI->SetDesiredRuntimeBehavior(NewBehavior);

		FProperty* BehaviorProp = ALevelInstance::StaticClass()->FindPropertyByName(TEXT("DesiredRuntimeBehavior"));
		if (BehaviorProp)
		{
			FPropertyChangedEvent Event(BehaviorProp);
			Actor->PostEditChangeProperty(Event);
		}

		Session.Log(FString::Printf(TEXT("[OK] li_set_runtime_behavior -> '%s' set to '%s'"), *NameOrLabel, *BehaviorStr));
		return sol::make_object(Lua, true);
#else
		Session.Log(TEXT("[FAIL] li_set_runtime_behavior requires UE 5.7+"));
		return sol::lua_nil;
#endif
	});
#endif // WITH_EDITOR

	// ──────────────────────────────────────────────
	// 17. li_set_hidden(name_or_label, hidden)
	// ──────────────────────────────────────────────
	Lua.set_function("li_set_hidden", [&Session](std::string InName, bool bHidden, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString NameOrLabel = UTF8_TO_TCHAR(InName.c_str());

#if WITH_EDITOR
		ILevelInstanceInterface* LI = LevelInstanceHelpers::FindLevelInstance(NameOrLabel);
		if (!LI)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] li_set_hidden -> level instance '%s' not found"), *NameOrLabel));
			return sol::lua_nil;
		}

		ULevelInstanceSubsystem* Subsystem = LevelInstanceHelpers::GetLISubsystem();
		if (!Subsystem)
		{
			Session.Log(TEXT("[FAIL] li_set_hidden -> no LevelInstance subsystem"));
			return sol::lua_nil;
		}

		Subsystem->SetIsTemporarilyHiddenInEditor(LI, bHidden);
		Session.Log(FString::Printf(TEXT("[OK] li_set_hidden -> '%s' %s"), *NameOrLabel, bHidden ? TEXT("hidden") : TEXT("shown")));
		return sol::make_object(Lua, true);
#else
		Session.Log(TEXT("[FAIL] li_set_hidden -> editor only"));
		return sol::lua_nil;
#endif
	});

#if WITH_EDITOR
	// ──────────────────────────────────────────────
	// 18. pla_pack(name_or_label)
	// ──────────────────────────────────────────────
	Lua.set_function("pla_pack", [&Session](std::string InName, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString NameOrLabel = UTF8_TO_TCHAR(InName.c_str());

		ILevelInstanceInterface* LI = LevelInstanceHelpers::FindLevelInstance(NameOrLabel);
		if (!LI)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] pla_pack -> level instance '%s' not found"), *NameOrLabel));
			return sol::lua_nil;
		}

		APackedLevelActor* PLA = Cast<APackedLevelActor>(Cast<AActor>(LI));
		if (!PLA)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] pla_pack -> '%s' is not a PackedLevelActor"), *NameOrLabel));
			return sol::lua_nil;
		}

		TSharedPtr<FPackedLevelActorBuilder> Builder = FPackedLevelActorBuilder::CreateDefaultBuilder();
		if (!Builder.IsValid())
		{
			Session.Log(TEXT("[FAIL] pla_pack -> could not create PackedLevelActorBuilder"));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Pack Level Actor")));
		bool bSuccess = Builder->PackActor(PLA);
		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] pla_pack -> packed '%s'"), *NameOrLabel));
			return sol::make_object(Lua, true);
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] pla_pack -> could not pack '%s'"), *NameOrLabel));
			return sol::lua_nil;
		}
	});

	// ──────────────────────────────────────────────
	// 19. pla_create_blueprint(name_or_label, blueprint_path)
	// ──────────────────────────────────────────────
	Lua.set_function("pla_create_blueprint", [&Session](std::string InName, std::string InBlueprintPath, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString NameOrLabel = UTF8_TO_TCHAR(InName.c_str());
		FString BlueprintPath = UTF8_TO_TCHAR(InBlueprintPath.c_str());

		ILevelInstanceInterface* LI = LevelInstanceHelpers::FindLevelInstance(NameOrLabel);
		if (!LI)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] pla_create_blueprint -> level instance '%s' not found"), *NameOrLabel));
			return sol::lua_nil;
		}

		TSharedPtr<FPackedLevelActorBuilder> Builder = FPackedLevelActorBuilder::CreateDefaultBuilder();
		if (!Builder.IsValid())
		{
			Session.Log(TEXT("[FAIL] pla_create_blueprint -> could not create PackedLevelActorBuilder"));
			return sol::lua_nil;
		}

		TSoftObjectPtr<UBlueprint> BPAsset{FSoftObjectPath(BlueprintPath)};

		FScopedTransaction Transaction(FText::FromString(TEXT("Create Packed Level Actor Blueprint")));
		bool bSuccess = Builder->CreateOrUpdateBlueprint(LI, BPAsset, true, false);
		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] pla_create_blueprint -> created/updated blueprint for '%s' at '%s'"), *NameOrLabel, *BlueprintPath));
			return sol::make_object(Lua, true);
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] pla_create_blueprint -> could not create blueprint for '%s'"), *NameOrLabel));
			return sol::lua_nil;
		}
	});
#endif // WITH_EDITOR
}

REGISTER_LUA_BINDING(LevelInstance, LevelInstanceDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindLevelInstance(Lua, Session);
});
