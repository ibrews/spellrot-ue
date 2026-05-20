// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "SmartObjectDefinition.h"
#include "SmartObjectTypes.h"
#include "Logging/TokenizedMessage.h"
#include "Modules/ModuleManager.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static const char* TagMergingPolicyToString(ESmartObjectTagMergingPolicy Policy)
{
	switch (Policy)
	{
	case ESmartObjectTagMergingPolicy::Combine:  return "Combine";
	case ESmartObjectTagMergingPolicy::Override:  return "Override";
	default:                                      return "Combine";
	}
}

static bool ParseTagMergingPolicy(const FString& Str, ESmartObjectTagMergingPolicy& OutPolicy)
{
	if (Str.Equals(TEXT("Combine"), ESearchCase::IgnoreCase))  { OutPolicy = ESmartObjectTagMergingPolicy::Combine;  return true; }
	if (Str.Equals(TEXT("Override"), ESearchCase::IgnoreCase)) { OutPolicy = ESmartObjectTagMergingPolicy::Override; return true; }
	return false;
}

static const char* TagFilteringPolicyToString(ESmartObjectTagFilteringPolicy Policy)
{
	switch (Policy)
	{
	case ESmartObjectTagFilteringPolicy::NoFilter: return "NoFilter";
	case ESmartObjectTagFilteringPolicy::Combine:  return "Combine";
	case ESmartObjectTagFilteringPolicy::Override:  return "Override";
	default:                                        return "NoFilter";
	}
}

static bool ParseTagFilteringPolicy(const FString& Str, ESmartObjectTagFilteringPolicy& OutPolicy)
{
	if (Str.Equals(TEXT("NoFilter"), ESearchCase::IgnoreCase)) { OutPolicy = ESmartObjectTagFilteringPolicy::NoFilter; return true; }
	if (Str.Equals(TEXT("Combine"), ESearchCase::IgnoreCase))  { OutPolicy = ESmartObjectTagFilteringPolicy::Combine;  return true; }
	if (Str.Equals(TEXT("Override"), ESearchCase::IgnoreCase)) { OutPolicy = ESmartObjectTagFilteringPolicy::Override; return true; }
	return false;
}

static FGameplayTagContainer ParseTagContainer(sol::object TagsObj)
{
	FGameplayTagContainer Container;
	if (TagsObj.is<std::string>())
	{
		FString TagStr = UTF8_TO_TCHAR(TagsObj.as<std::string>().c_str());
		TArray<FString> TagStrings;
		TagStr.ParseIntoArray(TagStrings, TEXT(","));
		for (const FString& S : TagStrings)
		{
			FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*S.TrimStartAndEnd()), false);
			if (Tag.IsValid())
			{
				Container.AddTag(Tag);
			}
		}
	}
	else if (TagsObj.is<sol::table>())
	{
		sol::table TagArr = TagsObj.as<sol::table>();
		for (auto& KV : TagArr)
		{
			if (KV.second.is<std::string>())
			{
				FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(UTF8_TO_TCHAR(KV.second.as<std::string>().c_str())), false);
				if (Tag.IsValid())
				{
					Container.AddTag(Tag);
				}
			}
		}
	}
	return Container;
}

static sol::table TagContainerToLua(sol::state_view& Lua, const FGameplayTagContainer& Container)
{
	sol::table Arr = Lua.create_table();
	int Idx = 1;
	for (const FGameplayTag& Tag : Container)
	{
		Arr[Idx++] = TCHAR_TO_UTF8(*Tag.ToString());
	}
	return Arr;
}

static const char* SlotShapeToString(ESmartObjectSlotShape Shape)
{
	switch (Shape)
	{
	case ESmartObjectSlotShape::Circle:    return "Circle";
	case ESmartObjectSlotShape::Rectangle: return "Rectangle";
	default:                               return "Circle";
	}
}

static bool ParseSlotShape(const FString& Str, ESmartObjectSlotShape& OutShape)
{
	if (Str.Equals(TEXT("Circle"), ESearchCase::IgnoreCase))    { OutShape = ESmartObjectSlotShape::Circle;    return true; }
	if (Str.Equals(TEXT("Rectangle"), ESearchCase::IgnoreCase)) { OutShape = ESmartObjectSlotShape::Rectangle; return true; }
	return false;
}

static sol::table SlotToLua(sol::state_view& Lua, const USmartObjectDefinition* Def, const FSmartObjectSlotDefinition& Slot, int32 Index)
{
	sol::table T = Lua.create_table();
	T["index"] = Index + 1; // 1-based for Lua

#if WITH_EDITORONLY_DATA
	T["name"] = TCHAR_TO_UTF8(*Slot.Name.ToString());
	T["color"] = Lua.create_table_with(
		"r", Slot.DEBUG_DrawColor.R,
		"g", Slot.DEBUG_DrawColor.G,
		"b", Slot.DEBUG_DrawColor.B,
		"a", Slot.DEBUG_DrawColor.A);
	T["draw_size"] = Slot.DEBUG_DrawSize;
	T["draw_shape"] = SlotShapeToString(Slot.DEBUG_DrawShape);
#endif

	T["offset"] = Lua.create_table_with(
		"x", static_cast<double>(Slot.Offset.X),
		"y", static_cast<double>(Slot.Offset.Y),
		"z", static_cast<double>(Slot.Offset.Z));

	T["rotation"] = Lua.create_table_with(
		"pitch", static_cast<double>(Slot.Rotation.Pitch),
		"yaw", static_cast<double>(Slot.Rotation.Yaw),
		"roll", static_cast<double>(Slot.Rotation.Roll));

	T["enabled"] = Slot.bEnabled;
	T["has_user_tag_filter"] = !Slot.UserTagFilter.IsEmpty();
	T["has_preconditions"] = Slot.SelectionPreconditions.IsValid();
	T["activity_tags"] = TagContainerToLua(Lua, Slot.ActivityTags);

	// Effective tags after merging policy is applied
	if (Def)
	{
		FGameplayTagContainer MergedTags;
		Def->GetSlotActivityTags(Slot, MergedTags);
		T["effective_activity_tags"] = TagContainerToLua(Lua, MergedTags);
	}

	T["runtime_tags"] = TagContainerToLua(Lua, Slot.RuntimeTags);
	T["behavior_definition_count"] = Slot.BehaviorDefinitions.Num();

	// List behavior definition class names
	sol::table BehaviorDefs = Lua.create_table();
	int BIdx = 1;
	for (const TObjectPtr<USmartObjectBehaviorDefinition>& BehaviorDef : Slot.BehaviorDefinitions)
	{
		if (BehaviorDef)
		{
			BehaviorDefs[BIdx++] = TCHAR_TO_UTF8(*BehaviorDef->GetClass()->GetName());
		}
	}
	T["behavior_definitions"] = BehaviorDefs;

	return T;
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> SmartObjectDocs = {};

static void BindSmartObject(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_smart_object", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		USmartObjectDefinition* Def = LoadObject<USmartObjectDefinition>(nullptr, *FPath);
		if (!Def) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"SmartObjectDefinition enrichment methods:\n"
			"\n"
			"info() — structured summary:\n"
			"  slot_count, activity_tags, user_tag_filter_empty, has_preconditions,\n"
			"  activity_tags_merging_policy, user_tags_filtering_policy,\n"
			"  default_behavior_definition_count, default_behavior_definitions,\n"
			"  world_condition_schema, is_valid\n"
			"\n"
			"info(\"validate\") — runs full validation, returns {valid, errors}\n"
			"\n"
			"list(\"slots\") — array of slot details:\n"
			"  index (1-based), name, offset {x,y,z}, rotation {pitch,yaw,roll},\n"
			"  enabled, activity_tags, effective_activity_tags, runtime_tags,\n"
			"  behavior_definitions, has_user_tag_filter, has_preconditions,\n"
			"  color, draw_size, draw_shape\n"
			"\n"
			"add(\"slot\", {offset={x,y,z}, rotation={pitch,yaw,roll}, enabled=true,\n"
			"             name=\"SlotName\", activity_tags=\"Tag1,Tag2\", runtime_tags=\"Tag3\"})\n"
			"  Adds a new slot. All fields optional (defaults: origin, no rotation, enabled).\n"
			"\n"
			"remove(\"slot\", index) — removes slot at 1-based index.\n"
			"\n"
			"configure(\"slot\", index, {offset, rotation, enabled, name, activity_tags,\n"
			"          runtime_tags, color, draw_size, draw_shape})\n"
			"  Modifies slot at 1-based index. Only provided fields are changed.\n"
			"  draw_shape: Circle, Rectangle\n"
			"\n"
			"configure(\"definition\", {activity_tags, activity_tags_merging_policy,\n"
			"          user_tags_filtering_policy})\n"
			"  Modifies definition-level settings.\n"
			"  Merging policies: Combine, Override\n"
			"  Filtering policies: NoFilter, Combine, Override\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [Def, &Session](sol::table /*self*/, sol::optional<std::string> WhatOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Def))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			// ---- info("validate") ----
			if (WhatOpt.has_value())
			{
				FString WhatStr = UTF8_TO_TCHAR(WhatOpt.value().c_str());
				if (WhatStr.Equals(TEXT("validate"), ESearchCase::IgnoreCase))
				{
					sol::table Result = Lua.create_table();
					sol::table ErrorArr = Lua.create_table();
					int Idx = 1;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
					TArray<TPair<EMessageSeverity::Type, FText>> Errors;
					bool bIsValid = Def->Validate(&Errors);
					for (const auto& Err : Errors)
					{
						sol::table E = Lua.create_table();
						switch (Err.Key)
						{
						case EMessageSeverity::Error:   E["severity"] = "error"; break;
						case EMessageSeverity::Warning: E["severity"] = "warning"; break;
						case EMessageSeverity::Info:    E["severity"] = "info"; break;
						default:                        E["severity"] = "info"; break;
						}
						E["message"] = TCHAR_TO_UTF8(*Err.Value.ToString());
						ErrorArr[Idx++] = E;
					}
#else
					TArray<FText> Errors;
					bool bIsValid = Def->Validate(&Errors);
					for (const auto& Err : Errors)
					{
						sol::table E = Lua.create_table();
						E["severity"] = "error";
						E["message"] = TCHAR_TO_UTF8(*Err.ToString());
						ErrorArr[Idx++] = E;
					}
#endif
					Result["valid"] = bIsValid;
					Result["errors"] = ErrorArr;

					Session.Log(FString::Printf(TEXT("[OK] info(\"validate\") -> valid=%s, %d messages"),
						bIsValid ? TEXT("true") : TEXT("false"), Errors.Num()));
					return Result;
				}

				Session.Log(FString::Printf(TEXT("[FAIL] info(\"%s\") -> unknown. Valid: validate (or no argument for summary)"), *WhatStr));
				return sol::lua_nil;
			}

			// ---- info() — general summary ----
			sol::table Result = Lua.create_table();

			Result["slot_count"] = Def->GetSlots().Num();
			Result["activity_tags"] = TagContainerToLua(Lua, Def->GetActivityTags());
			Result["user_tag_filter_empty"] = Def->GetUserTagFilter().IsEmpty();
			Result["has_preconditions"] = Def->GetPreconditions().IsValid();
			Result["activity_tags_merging_policy"] = TagMergingPolicyToString(Def->GetActivityTagsMergingPolicy());
			Result["user_tags_filtering_policy"] = TagFilteringPolicyToString(Def->GetUserTagsFilteringPolicy());

			// Validation status
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			if (Def->HasBeenValidated())
			{
				Result["is_valid"] = Def->IsDefinitionValid();
			}
			else
			{
				Result["is_valid"] = sol::lua_nil;
			}
#endif

			// Default behavior definitions count (private, use reflection)
			static FProperty* DefBehaviorProp = USmartObjectDefinition::StaticClass()->FindPropertyByName(TEXT("DefaultBehaviorDefinitions"));
			if (DefBehaviorProp)
			{
				const TArray<TObjectPtr<USmartObjectBehaviorDefinition>>* Defs = DefBehaviorProp->ContainerPtrToValuePtr<TArray<TObjectPtr<USmartObjectBehaviorDefinition>>>(Def);
				if (Defs)
				{
					Result["default_behavior_definition_count"] = Defs->Num();

					sol::table DefNames = Lua.create_table();
					int Idx = 1;
					for (const auto& BDef : *Defs)
					{
						if (BDef)
						{
							DefNames[Idx++] = TCHAR_TO_UTF8(*BDef->GetClass()->GetName());
						}
					}
					Result["default_behavior_definitions"] = DefNames;
				}
			}

			// World condition schema
			const USmartObjectWorldConditionSchema* Schema = Def->GetWorldConditionSchema();
			if (Schema)
			{
				Result["world_condition_schema"] = TCHAR_TO_UTF8(*Schema->GetClass()->GetName());
			}
			else
			{
				Result["world_condition_schema"] = sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[OK] info() -> SmartObjectDefinition, %d slots"), Def->GetSlots().Num()));
			return Result;
		});

		// ================================================================
		// list("slots")
		// ================================================================
		AssetObj.set_function("list", [Def, &Session](sol::table /*self*/, std::string What, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Def))
			{
				Session.Log(TEXT("[FAIL] list -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString WhatStr = UTF8_TO_TCHAR(What.c_str());

			if (WhatStr.Equals(TEXT("slots"), ESearchCase::IgnoreCase))
			{
				TConstArrayView<FSmartObjectSlotDefinition> Slots = Def->GetSlots();
				sol::table Result = Lua.create_table();

				for (int32 i = 0; i < Slots.Num(); ++i)
				{
					Result[i + 1] = SlotToLua(Lua, Def, Slots[i], i);
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"slots\") -> %d slots"), Slots.Num()));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown category. Valid: slots"), *WhatStr));
			return sol::lua_nil;
		});

		// ================================================================
		// add("slot", opts?)
		// ================================================================
		AssetObj.set_function("add", [Def, &Session](sol::table /*self*/, std::string What,
			sol::optional<sol::table> OptsOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Def))
			{
				Session.Log(TEXT("[FAIL] add -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString WhatStr = UTF8_TO_TCHAR(What.c_str());

			if (!WhatStr.Equals(TEXT("slot"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: slot"), *WhatStr));
				return sol::lua_nil;
			}

#if WITH_EDITOR
			const FScopedTransaction Transaction(FText::FromString(TEXT("SmartObject: Add Slot")));
			Def->Modify();

			// Access private Slots TArray via reflection (GetMutableSlots() returns TArrayView which can't AddDefaulted)
			static FProperty* SlotsProp = USmartObjectDefinition::StaticClass()->FindPropertyByName(TEXT("Slots"));
			if (!SlotsProp)
			{
				Session.Log(TEXT("[FAIL] add(\"slot\") -> could not find Slots property"));
				return sol::lua_nil;
			}
			TArray<FSmartObjectSlotDefinition>* SlotsArr = SlotsProp->ContainerPtrToValuePtr<TArray<FSmartObjectSlotDefinition>>(Def);
			if (!SlotsArr)
			{
				Session.Log(TEXT("[FAIL] add(\"slot\") -> could not access Slots array"));
				return sol::lua_nil;
			}

			FSmartObjectSlotDefinition& NewSlot = SlotsArr->AddDefaulted_GetRef();

#if WITH_EDITORONLY_DATA
			NewSlot.ID = FGuid::NewGuid();
#endif

			// Set schema class on preconditions (matches engine PostEditChangeChainProperty logic)
			{
				static FProperty* SchemaProp = USmartObjectDefinition::StaticClass()->FindPropertyByName(TEXT("WorldConditionSchemaClass"));
				if (SchemaProp)
				{
					const TSubclassOf<USmartObjectWorldConditionSchema>* SchemaClass =
						SchemaProp->ContainerPtrToValuePtr<TSubclassOf<USmartObjectWorldConditionSchema>>(Def);
					if (SchemaClass && *SchemaClass)
					{
						NewSlot.SelectionPreconditions.SetSchemaClass(*SchemaClass);
					}
				}
			}

			// Apply optional parameters
			if (OptsOpt.has_value())
			{
				sol::table Opts = OptsOpt.value();

				sol::optional<sol::table> OffsetOpt = Opts.get<sol::optional<sol::table>>("offset");
				if (OffsetOpt.has_value())
				{
					sol::table O = OffsetOpt.value();
					NewSlot.Offset.X = static_cast<float>(O.get_or("x", 0.0));
					NewSlot.Offset.Y = static_cast<float>(O.get_or("y", 0.0));
					NewSlot.Offset.Z = static_cast<float>(O.get_or("z", 0.0));
				}

				sol::optional<sol::table> RotOpt = Opts.get<sol::optional<sol::table>>("rotation");
				if (RotOpt.has_value())
				{
					sol::table R = RotOpt.value();
					NewSlot.Rotation.Pitch = static_cast<float>(R.get_or("pitch", 0.0));
					NewSlot.Rotation.Yaw = static_cast<float>(R.get_or("yaw", 0.0));
					NewSlot.Rotation.Roll = static_cast<float>(R.get_or("roll", 0.0));
				}

				sol::optional<bool> EnabledOpt = Opts.get<sol::optional<bool>>("enabled");
				if (EnabledOpt.has_value())
				{
					NewSlot.bEnabled = EnabledOpt.value();
				}

#if WITH_EDITORONLY_DATA
				sol::optional<std::string> NameOpt = Opts.get<sol::optional<std::string>>("name");
				if (NameOpt.has_value())
				{
					NewSlot.Name = FName(UTF8_TO_TCHAR(NameOpt.value().c_str()));
				}
#endif

				sol::object ActivityTagsObj = Opts.get<sol::object>("activity_tags");
				if (ActivityTagsObj.valid() && ActivityTagsObj.get_type() != sol::type::lua_nil)
				{
					NewSlot.ActivityTags = ParseTagContainer(ActivityTagsObj);
				}

				sol::object RuntimeTagsObj = Opts.get<sol::object>("runtime_tags");
				if (RuntimeTagsObj.valid() && RuntimeTagsObj.get_type() != sol::type::lua_nil)
				{
					NewSlot.RuntimeTags = ParseTagContainer(RuntimeTagsObj);
				}
			}

			// Fire delegate (matches engine PostEditChangeChainProperty logic)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			UE::SmartObject::Delegates::OnSlotDefinitionCreated.ExecuteIfBound(*Def, NewSlot);
#endif

			int32 NewIndex = SlotsArr->Num(); // 1-based
			FPropertyChangedEvent Event(SlotsProp, EPropertyChangeType::ArrayAdd);
			Def->PostEditChangeProperty(Event);
			Def->MarkPackageDirty();
			Def->Validate();

			Session.Log(FString::Printf(TEXT("[OK] add(\"slot\") -> slot %d added"), NewIndex));

			sol::table Result = Lua.create_table();
			Result["index"] = NewIndex;
			return Result;
#else
			Session.Log(TEXT("[FAIL] add(\"slot\") -> editor-only operation"));
			return sol::lua_nil;
#endif
		});

		// ================================================================
		// remove("slot", index)
		// ================================================================
		AssetObj.set_function("remove", [Def, &Session](sol::table /*self*/, std::string What,
			int Index, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Def))
			{
				Session.Log(TEXT("[FAIL] remove -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString WhatStr = UTF8_TO_TCHAR(What.c_str());
			if (!WhatStr.Equals(TEXT("slot"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: slot"), *WhatStr));
				return sol::lua_nil;
			}

#if WITH_EDITOR
			int32 SlotIdx = Index - 1; // Convert from 1-based Lua to 0-based

			if (!Def->IsValidSlotIndex(SlotIdx))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"slot\", %d) -> invalid index (valid: 1..%d)"),
					Index, Def->GetSlots().Num()));
				return sol::lua_nil;
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("SmartObject: Remove Slot")));
			Def->Modify();

			static FProperty* SlotsProp = USmartObjectDefinition::StaticClass()->FindPropertyByName(TEXT("Slots"));
			if (!SlotsProp)
			{
				Session.Log(TEXT("[FAIL] remove(\"slot\") -> could not find Slots property"));
				return sol::lua_nil;
			}
			TArray<FSmartObjectSlotDefinition>* SlotsArr = SlotsProp->ContainerPtrToValuePtr<TArray<FSmartObjectSlotDefinition>>(Def);
			if (!SlotsArr)
			{
				Session.Log(TEXT("[FAIL] remove(\"slot\") -> could not access Slots array"));
				return sol::lua_nil;
			}

			SlotsArr->RemoveAt(SlotIdx);

			FPropertyChangedEvent Event(SlotsProp, EPropertyChangeType::ArrayRemove);
			Def->PostEditChangeProperty(Event);
			Def->MarkPackageDirty();
			Def->Validate();

			Session.Log(FString::Printf(TEXT("[OK] remove(\"slot\", %d) -> removed, %d slots remain"),
				Index, SlotsArr->Num()));
			return sol::make_object(Lua, true);
#else
			Session.Log(TEXT("[FAIL] remove(\"slot\") -> editor-only operation"));
			return sol::lua_nil;
#endif
		});

		// ================================================================
		// configure("slot", index, params) or configure("definition", params)
		// ================================================================
		AssetObj.set_function("configure", [Def, &Session](sol::table /*self*/, std::string What,
			sol::variadic_args Va, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Def))
			{
				Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString WhatStr = UTF8_TO_TCHAR(What.c_str());

			// ---- configure("definition", params) ----
			if (WhatStr.Equals(TEXT("definition"), ESearchCase::IgnoreCase))
			{
				if (Va.size() < 1 || !Va[0].is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"definition\", params) -> params table required"));
					return sol::lua_nil;
				}
				sol::table Params = Va[0].as<sol::table>();

				const FScopedTransaction Transaction(FText::FromString(TEXT("SmartObject: Configure Definition")));
				Def->Modify();
				bool bModified = false;
				FString Changes;

				// Activity tags
				sol::object ActivityTagsObj = Params.get<sol::object>("activity_tags");
				if (ActivityTagsObj.valid() && ActivityTagsObj.get_type() != sol::type::lua_nil)
				{
					Def->SetActivityTags(ParseTagContainer(ActivityTagsObj));
					Changes += TEXT(" activity_tags=set");
					bModified = true;
				}

				// Activity tags merging policy
				sol::optional<std::string> MergingOpt = Params.get<sol::optional<std::string>>("activity_tags_merging_policy");
				if (MergingOpt.has_value())
				{
					FString MStr = UTF8_TO_TCHAR(MergingOpt.value().c_str());
					ESmartObjectTagMergingPolicy Policy;
					if (ParseTagMergingPolicy(MStr, Policy))
					{
						Def->SetActivityTagsMergingPolicy(Policy);
						Changes += FString::Printf(TEXT(" activity_tags_merging_policy=%s"), *MStr);
						bModified = true;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure -> invalid activity_tags_merging_policy '%s'. Valid: Combine, Override"), *MStr));
					}
				}

				// User tags filtering policy
				sol::optional<std::string> FilteringOpt = Params.get<sol::optional<std::string>>("user_tags_filtering_policy");
				if (FilteringOpt.has_value())
				{
					FString FStr = UTF8_TO_TCHAR(FilteringOpt.value().c_str());
					ESmartObjectTagFilteringPolicy Policy;
					if (ParseTagFilteringPolicy(FStr, Policy))
					{
						Def->SetUserTagsFilteringPolicy(Policy);
						Changes += FString::Printf(TEXT(" user_tags_filtering_policy=%s"), *FStr);
						bModified = true;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure -> invalid user_tags_filtering_policy '%s'. Valid: NoFilter, Combine, Override"), *FStr));
					}
				}

				if (bModified)
				{
					FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
					Def->PostEditChangeProperty(Event);
					Def->MarkPackageDirty();
					Def->Validate();
					Session.Log(FString::Printf(TEXT("[OK] configure(\"definition\",%s)"), *Changes.TrimStart()));
					return sol::make_object(Lua, true);
				}

				Session.Log(TEXT("[OK] configure(\"definition\") -> nothing changed. Valid keys: activity_tags, activity_tags_merging_policy, user_tags_filtering_policy"));
				return sol::make_object(Lua, true);
			}

			// ---- configure("slot", index, params) ----
			if (WhatStr.Equals(TEXT("slot"), ESearchCase::IgnoreCase))
			{
				if (Va.size() < 2 || !Va[0].is<int>() || !Va[1].is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"slot\", index, params) -> index (number) and params (table) required"));
					return sol::lua_nil;
				}

				int Index = Va[0].as<int>();
				sol::table Params = Va[1].as<sol::table>();
				int32 SlotIdx = Index - 1; // 1-based to 0-based

				if (!Def->IsValidSlotIndex(SlotIdx))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"slot\", %d) -> invalid index (valid: 1..%d)"),
						Index, Def->GetSlots().Num()));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("SmartObject: Configure Slot")));
				Def->Modify();

				FSmartObjectSlotDefinition& Slot = Def->GetMutableSlot(SlotIdx);
				bool bModified = false;
				FString Changes;

				// Offset
				sol::optional<sol::table> OffsetOpt = Params.get<sol::optional<sol::table>>("offset");
				if (OffsetOpt.has_value())
				{
					sol::table O = OffsetOpt.value();
					Slot.Offset.X = static_cast<float>(O.get_or("x", static_cast<double>(Slot.Offset.X)));
					Slot.Offset.Y = static_cast<float>(O.get_or("y", static_cast<double>(Slot.Offset.Y)));
					Slot.Offset.Z = static_cast<float>(O.get_or("z", static_cast<double>(Slot.Offset.Z)));
					Changes += FString::Printf(TEXT(" offset=(%.1f,%.1f,%.1f)"),
						static_cast<double>(Slot.Offset.X), static_cast<double>(Slot.Offset.Y), static_cast<double>(Slot.Offset.Z));
					bModified = true;
				}

				// Rotation
				sol::optional<sol::table> RotOpt = Params.get<sol::optional<sol::table>>("rotation");
				if (RotOpt.has_value())
				{
					sol::table R = RotOpt.value();
					Slot.Rotation.Pitch = static_cast<float>(R.get_or("pitch", static_cast<double>(Slot.Rotation.Pitch)));
					Slot.Rotation.Yaw = static_cast<float>(R.get_or("yaw", static_cast<double>(Slot.Rotation.Yaw)));
					Slot.Rotation.Roll = static_cast<float>(R.get_or("roll", static_cast<double>(Slot.Rotation.Roll)));
					Changes += FString::Printf(TEXT(" rotation=(%.1f,%.1f,%.1f)"),
						static_cast<double>(Slot.Rotation.Pitch), static_cast<double>(Slot.Rotation.Yaw), static_cast<double>(Slot.Rotation.Roll));
					bModified = true;
				}

				// Enabled
				sol::optional<bool> EnabledOpt = Params.get<sol::optional<bool>>("enabled");
				if (EnabledOpt.has_value())
				{
					Slot.bEnabled = EnabledOpt.value();
					Changes += FString::Printf(TEXT(" enabled=%s"), Slot.bEnabled ? TEXT("true") : TEXT("false"));
					bModified = true;
				}

#if WITH_EDITORONLY_DATA
				// Name
				sol::optional<std::string> NameOpt = Params.get<sol::optional<std::string>>("name");
				if (NameOpt.has_value())
				{
					Slot.Name = FName(UTF8_TO_TCHAR(NameOpt.value().c_str()));
					Changes += FString::Printf(TEXT(" name=%s"), *Slot.Name.ToString());
					bModified = true;
				}

				// Color
				sol::optional<sol::table> ColorOpt = Params.get<sol::optional<sol::table>>("color");
				if (ColorOpt.has_value())
				{
					sol::table C = ColorOpt.value();
					Slot.DEBUG_DrawColor.R = static_cast<uint8>(C.get_or("r", static_cast<int>(Slot.DEBUG_DrawColor.R)));
					Slot.DEBUG_DrawColor.G = static_cast<uint8>(C.get_or("g", static_cast<int>(Slot.DEBUG_DrawColor.G)));
					Slot.DEBUG_DrawColor.B = static_cast<uint8>(C.get_or("b", static_cast<int>(Slot.DEBUG_DrawColor.B)));
					Slot.DEBUG_DrawColor.A = static_cast<uint8>(C.get_or("a", static_cast<int>(Slot.DEBUG_DrawColor.A)));
					Changes += TEXT(" color=set");
					bModified = true;
				}

				// Draw size
				sol::optional<double> DrawSizeOpt = Params.get<sol::optional<double>>("draw_size");
				if (DrawSizeOpt.has_value())
				{
					Slot.DEBUG_DrawSize = static_cast<float>(DrawSizeOpt.value());
					Changes += FString::Printf(TEXT(" draw_size=%.1f"), static_cast<double>(Slot.DEBUG_DrawSize));
					bModified = true;
				}

				// Draw shape
				sol::optional<std::string> DrawShapeOpt = Params.get<sol::optional<std::string>>("draw_shape");
				if (DrawShapeOpt.has_value())
				{
					FString ShapeStr = UTF8_TO_TCHAR(DrawShapeOpt.value().c_str());
					ESmartObjectSlotShape Shape;
					if (ParseSlotShape(ShapeStr, Shape))
					{
						Slot.DEBUG_DrawShape = Shape;
						Changes += FString::Printf(TEXT(" draw_shape=%s"), *ShapeStr);
						bModified = true;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure -> invalid draw_shape '%s'. Valid: Circle, Rectangle"), *ShapeStr));
					}
				}
#endif

				// Activity tags
				sol::object ActivityTagsObj = Params.get<sol::object>("activity_tags");
				if (ActivityTagsObj.valid() && ActivityTagsObj.get_type() != sol::type::lua_nil)
				{
					Slot.ActivityTags = ParseTagContainer(ActivityTagsObj);
					Changes += TEXT(" activity_tags=set");
					bModified = true;
				}

				// Runtime tags
				sol::object RuntimeTagsObj = Params.get<sol::object>("runtime_tags");
				if (RuntimeTagsObj.valid() && RuntimeTagsObj.get_type() != sol::type::lua_nil)
				{
					Slot.RuntimeTags = ParseTagContainer(RuntimeTagsObj);
					Changes += TEXT(" runtime_tags=set");
					bModified = true;
				}

				if (bModified)
				{
					FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
					Def->PostEditChangeProperty(Event);
					Def->MarkPackageDirty();
					Def->Validate();
					Session.Log(FString::Printf(TEXT("[OK] configure(\"slot\", %d,%s)"), Index, *Changes.TrimStart()));
					return sol::make_object(Lua, true);
				}

				Session.Log(FString::Printf(TEXT("[OK] configure(\"slot\", %d) -> nothing changed. Valid keys: offset, rotation, enabled, name, color, draw_size, draw_shape, activity_tags, runtime_tags"), Index));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown target. Valid: slot, definition"), *WhatStr));
			return sol::lua_nil;
		});
	});
}

REGISTER_LUA_BINDING(SmartObject, SmartObjectDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("SmartObjectsModule")))
	{
		Session.Log(TEXT("[WARN] SmartObjects plugin is not loaded. Enable it in Edit > Plugins to use this feature."));
		return;
	}
	BindSmartObject(Lua, Session);
});
