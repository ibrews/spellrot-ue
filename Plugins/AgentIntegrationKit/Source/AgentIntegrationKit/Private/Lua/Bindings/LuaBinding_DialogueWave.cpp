// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"

#include "Sound/DialogueWave.h"
#include "Sound/DialogueVoice.h"
#include "Sound/DialogueTypes.h"
#include "Sound/SoundWave.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static const char* GenderToStr(EGrammaticalGender::Type Gender)
{
	switch (Gender)
	{
	case EGrammaticalGender::Neuter:    return "neuter";
	case EGrammaticalGender::Masculine: return "masculine";
	case EGrammaticalGender::Feminine:  return "feminine";
	case EGrammaticalGender::Mixed:     return "mixed";
	default:                            return "neuter";
	}
}

static const char* PluralityToStr(EGrammaticalNumber::Type Plurality)
{
	switch (Plurality)
	{
	case EGrammaticalNumber::Singular: return "singular";
	case EGrammaticalNumber::Plural:   return "plural";
	default:                           return "singular";
	}
}

static sol::table VoiceToTable(sol::state_view& Lua, UDialogueVoice* Voice)
{
	sol::table T = Lua.create_table();
	if (Voice)
	{
		T["name"] = TCHAR_TO_UTF8(*Voice->GetName());
		T["path"] = TCHAR_TO_UTF8(*Voice->GetPathName());
		T["gender"] = GenderToStr(Voice->Gender);
		T["plurality"] = PluralityToStr(Voice->Plurality);
	}
	return T;
}

static sol::table ContextMappingToTable(sol::state_view& Lua, const FDialogueContextMapping& Mapping, int32 Index)
{
	sol::table T = Lua.create_table();
	T["index"] = Index;

	// Speaker
	if (Mapping.Context.Speaker)
	{
		T["speaker"] = VoiceToTable(Lua, Mapping.Context.Speaker);
	}

	// Targets
	sol::table Targets = Lua.create_table();
	int32 TIdx = 1;
	for (const TObjectPtr<UDialogueVoice>& Target : Mapping.Context.Targets)
	{
		if (Target)
		{
			Targets[TIdx++] = VoiceToTable(Lua, Target.Get());
		}
	}
	T["targets"] = Targets;

	// SoundWave
	if (Mapping.SoundWave)
	{
		T["sound_wave"] = TCHAR_TO_UTF8(*Mapping.SoundWave->GetPathName());
		T["sound_wave_name"] = TCHAR_TO_UTF8(*Mapping.SoundWave->GetName());
	}

	// LocalizationKeyFormat
	T["localization_key_format"] = TCHAR_TO_UTF8(*Mapping.LocalizationKeyFormat);

	return T;
}

// Helper to load targets from a Lua table, returns false if any target fails to load
static bool LoadTargetsFromTable(sol::table& Params, TArray<UDialogueVoice*>& OutTargets, FLuaSessionData& Session, const FString& Verb)
{
	sol::optional<sol::table> TargetsOpt = Params.get<sol::optional<sol::table>>("targets");
	if (!TargetsOpt.has_value()) return true;

	sol::table TargetsTable = TargetsOpt.value();
	for (auto& Pair : TargetsTable)
	{
		if (Pair.second.is<std::string>())
		{
			std::string TargetPath = Pair.second.as<std::string>();
			UDialogueVoice* Target = LoadObject<UDialogueVoice>(nullptr, UTF8_TO_TCHAR(TargetPath.c_str()));
			if (Target)
			{
				OutTargets.Add(Target);
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[FAIL] %s -> target not found: %hs"), *Verb, TargetPath.c_str()));
				return false;
			}
		}
	}
	return true;
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> DialogueWaveDocs = {};

static void BindDialogueWave(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_dialogue_wave", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UDialogueWave* Wave = LoadObject<UDialogueWave>(nullptr, *FPath);
		if (!Wave) return;

		AssetObj["_help_text"] =
			"DialogueWave enrichment:\n"
			"\n"
			"info() -> {spoken_text, subtitle_override, is_mature, voice_actor_direction,\n"
			"           localization_guid, localized_spoken_text, localized_subtitle,\n"
			"           context_count, contexts: [{index, speaker, targets, sound_wave,\n"
			"           localization_key_format}]}\n"
			"\n"
			"list(\"contexts\") -> array of context mappings\n"
			"\n"
			"add(\"context\", {speaker=\"/path/Voice\", targets={\"/path/V2\"},\n"
			"     sound_wave=\"/path/SW\", localization_key_format=\"{ContextHash}\"})\n"
			"  -> adds a new context mapping (returns 1-based index)\n"
			"\n"
			"remove(\"context\", index) -> removes context mapping at 1-based index\n"
			"\n"
			"configure(params):\n"
			"  spoken_text               = string (use false to clear)\n"
			"  subtitle_override         = string (also sets bOverride_SubtitleOverride; use false to clear)\n"
			"  is_mature                 = bool\n"
			"  voice_actor_direction     = string (use false to clear)\n"
			"\n"
			"configure(\"context\", index, params):\n"
			"  speaker                   = string (path to DialogueVoice)\n"
			"  targets                   = {string...} (paths to DialogueVoices)\n"
			"  sound_wave                = string (path to SoundWave)\n"
			"  localization_key_format   = string\n";

		// ---- info() ----
		AssetObj.set_function("info", [Wave, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Wave))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();
			Result["spoken_text"] = TCHAR_TO_UTF8(*Wave->SpokenText);
			Result["subtitle_override"] = TCHAR_TO_UTF8(*Wave->SubtitleOverride);
			Result["is_mature"] = (bool)Wave->bMature;
			Result["override_subtitle"] = (bool)Wave->bOverride_SubtitleOverride;
#if WITH_EDITORONLY_DATA
			Result["voice_actor_direction"] = TCHAR_TO_UTF8(*Wave->VoiceActorDirection);
#endif
			Result["localization_guid"] = TCHAR_TO_UTF8(*Wave->LocalizationGUID.ToString());
			Result["localized_spoken_text"] = TCHAR_TO_UTF8(*Wave->GetLocalizedSpokenText().ToString());
			Result["localized_subtitle"] = TCHAR_TO_UTF8(*Wave->GetLocalizedSubtitle().ToString());
			Result["context_count"] = Wave->ContextMappings.Num();

			// Contexts summary
			sol::table Contexts = Lua.create_table();
			for (int32 i = 0; i < Wave->ContextMappings.Num(); ++i)
			{
				Contexts[i + 1] = ContextMappingToTable(Lua, Wave->ContextMappings[i], i + 1);
			}
			Result["contexts"] = Contexts;

			Session.Log(FString::Printf(TEXT("[OK] info() -> DialogueWave, %d contexts"), Wave->ContextMappings.Num()));
			return Result;
		});

		// ---- list("contexts") ----
		AssetObj.set_function("list", [Wave, &Session](sol::table /*self*/,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Wave))
			{
				Session.Log(TEXT("[FAIL] list -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString Type = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("contexts");

			if (!Type.Equals(TEXT("contexts"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Use: contexts"), *Type));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();
			for (int32 i = 0; i < Wave->ContextMappings.Num(); ++i)
			{
				Result[i + 1] = ContextMappingToTable(Lua, Wave->ContextMappings[i], i + 1);
			}

			Session.Log(FString::Printf(TEXT("[OK] list(\"contexts\") -> %d items"), Wave->ContextMappings.Num()));
			return Result;
		});

		// ---- add("context", params) ----
		AssetObj.set_function("add", [Wave, &Session](sol::table /*self*/,
			const std::string& TypeStr, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Wave))
			{
				Session.Log(TEXT("[FAIL] add -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString Type = UTF8_TO_TCHAR(TypeStr.c_str());
			if (!Type.Equals(TEXT("context"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Use: context"), *Type));
				return sol::lua_nil;
			}

			// Load speaker
			UDialogueVoice* Speaker = nullptr;
			auto SpeakerPath = Params.get_or<std::string>("speaker", "");
			if (!SpeakerPath.empty())
			{
				Speaker = LoadObject<UDialogueVoice>(nullptr, UTF8_TO_TCHAR(SpeakerPath.c_str()));
				if (!Speaker)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"context\") -> speaker not found: %hs"), SpeakerPath.c_str()));
					return sol::lua_nil;
				}
			}

			// Load targets
			TArray<UDialogueVoice*> Targets;
			if (!LoadTargetsFromTable(Params, Targets, Session, TEXT("add(\"context\")")))
			{
				return sol::lua_nil;
			}

			// Load sound wave
			USoundWave* SoundWaveAsset = nullptr;
			auto SWPath = Params.get_or<std::string>("sound_wave", "");
			if (!SWPath.empty())
			{
				SoundWaveAsset = LoadObject<USoundWave>(nullptr, UTF8_TO_TCHAR(SWPath.c_str()));
				if (!SoundWaveAsset)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"context\") -> sound_wave not found: %hs"), SWPath.c_str()));
					return sol::lua_nil;
				}
			}

			Wave->Modify();

			FDialogueContextMapping NewMapping;

			// Set optional localization key format
			auto LocKeyFormat = Params.get_or<std::string>("localization_key_format", "");
			if (!LocKeyFormat.empty())
			{
				NewMapping.LocalizationKeyFormat = UTF8_TO_TCHAR(LocKeyFormat.c_str());
			}

			Wave->ContextMappings.Add(NewMapping);

			// Use UpdateContext to properly set fields AND create the proxy
			Wave->UpdateContext(Wave->ContextMappings.Last(), SoundWaveAsset, Speaker, Targets);

			Wave->MarkPackageDirty();

			int32 NewIndex = Wave->ContextMappings.Num();
			Session.Log(FString::Printf(TEXT("[OK] add(\"context\") -> index %d, speaker=%hs"),
				NewIndex, SpeakerPath.empty() ? "(none)" : SpeakerPath.c_str()));
			return sol::make_object(Lua, NewIndex);
		});

		// ---- remove("context", index) ----
		AssetObj.set_function("remove", [Wave, &Session](sol::table /*self*/,
			const std::string& TypeStr, int Index, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Wave))
			{
				Session.Log(TEXT("[FAIL] remove -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString Type = UTF8_TO_TCHAR(TypeStr.c_str());
			if (!Type.Equals(TEXT("context"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Use: context"), *Type));
				return sol::lua_nil;
			}

			int32 ZeroIndex = Index - 1;
			if (ZeroIndex < 0 || ZeroIndex >= Wave->ContextMappings.Num())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"context\", %d) -> index out of range (count=%d)"),
					Index, Wave->ContextMappings.Num()));
				return sol::lua_nil;
			}

			Wave->Modify();
			Wave->ContextMappings.RemoveAt(ZeroIndex);
			Wave->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] remove(\"context\", %d) -> %d remaining"),
				Index, Wave->ContextMappings.Num()));
			return sol::make_object(Lua, true);
		});

		// ---- configure(params) OR configure("context", index, params) ----
		AssetObj.set_function("configure", [Wave, &Session](sol::table /*self*/,
			sol::variadic_args va, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Wave))
			{
				Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
				return sol::lua_nil;
			}

			// Determine if this is configure(params) or configure("context", index, params)
			if (va.size() < 1)
			{
				Session.Log(TEXT("[FAIL] configure -> requires at least 1 argument"));
				return sol::lua_nil;
			}

			// ---- configure("context", index, params) — per-context modification ----
			if (va[0].is<std::string>())
			{
				if (va.size() < 3)
				{
					Session.Log(TEXT("[FAIL] configure(\"context\", index, params) -> requires 3 arguments"));
					return sol::lua_nil;
				}

				FString Type = UTF8_TO_TCHAR(va[0].as<std::string>().c_str());
				if (!Type.Equals(TEXT("context"), ESearchCase::IgnoreCase))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Use: context"), *Type));
					return sol::lua_nil;
				}

				int32 Index = va[1].as<int>();
				int32 ZeroIndex = Index - 1;
				if (ZeroIndex < 0 || ZeroIndex >= Wave->ContextMappings.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"context\", %d) -> index out of range (count=%d)"),
						Index, Wave->ContextMappings.Num()));
					return sol::lua_nil;
				}

				sol::table Params = va[2].as<sol::table>();
				FDialogueContextMapping& Mapping = Wave->ContextMappings[ZeroIndex];

				// Resolve new values, falling back to existing
				UDialogueVoice* NewSpeaker = Mapping.Context.Speaker;
				auto SpeakerPath = Params.get_or<std::string>("speaker", "");
				if (!SpeakerPath.empty())
				{
					NewSpeaker = LoadObject<UDialogueVoice>(nullptr, UTF8_TO_TCHAR(SpeakerPath.c_str()));
					if (!NewSpeaker)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"context\", %d) -> speaker not found: %hs"), Index, SpeakerPath.c_str()));
						return sol::lua_nil;
					}
				}

				TArray<UDialogueVoice*> NewTargets;
				sol::optional<sol::table> TargetsOpt = Params.get<sol::optional<sol::table>>("targets");
				if (TargetsOpt.has_value())
				{
					sol::table TargetsTable = TargetsOpt.value();
					if (!LoadTargetsFromTable(Params, NewTargets, Session, FString::Printf(TEXT("configure(\"context\", %d)"), Index)))
					{
						return sol::lua_nil;
					}
				}
				else
				{
					// Keep existing targets
					for (const TObjectPtr<UDialogueVoice>& T : Mapping.Context.Targets)
					{
						NewTargets.Add(T.Get());
					}
				}

				USoundWave* NewSoundWave = Mapping.SoundWave;
				auto SWPath = Params.get_or<std::string>("sound_wave", "");
				if (!SWPath.empty())
				{
					NewSoundWave = LoadObject<USoundWave>(nullptr, UTF8_TO_TCHAR(SWPath.c_str()));
					if (!NewSoundWave)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"context\", %d) -> sound_wave not found: %hs"), Index, SWPath.c_str()));
						return sol::lua_nil;
					}
				}

				Wave->Modify();

				// Set localization key format if provided
				auto LocKeyFormat = Params.get_or<std::string>("localization_key_format", "");
				if (!LocKeyFormat.empty())
				{
					Mapping.LocalizationKeyFormat = UTF8_TO_TCHAR(LocKeyFormat.c_str());
				}

				// Use UpdateContext to set fields + rebuild proxy
				Wave->UpdateContext(Mapping, NewSoundWave, NewSpeaker, NewTargets);

				Wave->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"context\", %d) -> updated"), Index));
				return sol::make_object(Lua, true);
			}

			// ---- configure(params) — wave-level properties ----
			if (!va[0].is<sol::table>())
			{
				Session.Log(TEXT("[FAIL] configure -> first argument must be a table or \"context\""));
				return sol::lua_nil;
			}

			sol::table Params = va[0].as<sol::table>();
			int32 Changed = 0;

			// spoken_text: string to set, false to clear
			sol::object SpokenTextObj = Params["spoken_text"];
			if (SpokenTextObj.is<std::string>())
			{
				if (Changed == 0) Wave->Modify();
				Wave->SpokenText = UTF8_TO_TCHAR(SpokenTextObj.as<std::string>().c_str());
				Changed++;
			}
			else if (SpokenTextObj.is<bool>() && !SpokenTextObj.as<bool>())
			{
				if (Changed == 0) Wave->Modify();
				Wave->SpokenText.Empty();
				Changed++;
			}

			// subtitle_override: string to set (enables override), false to clear + disable override
			sol::object SubtitleObj = Params["subtitle_override"];
			if (SubtitleObj.is<std::string>())
			{
				if (Changed == 0) Wave->Modify();
				Wave->SubtitleOverride = UTF8_TO_TCHAR(SubtitleObj.as<std::string>().c_str());
				Wave->bOverride_SubtitleOverride = true;
				Changed++;
			}
			else if (SubtitleObj.is<bool>() && !SubtitleObj.as<bool>())
			{
				if (Changed == 0) Wave->Modify();
				Wave->SubtitleOverride.Empty();
				Wave->bOverride_SubtitleOverride = false;
				Changed++;
			}

			// is_mature: bool
			sol::optional<bool> IsMature = Params.get<sol::optional<bool>>("is_mature");
			if (IsMature.has_value())
			{
				if (Changed == 0) Wave->Modify();
				Wave->bMature = IsMature.value() ? 1 : 0;
				Changed++;
			}

#if WITH_EDITORONLY_DATA
			// voice_actor_direction: string to set, false to clear
			sol::object VADObj = Params["voice_actor_direction"];
			if (VADObj.is<std::string>())
			{
				if (Changed == 0) Wave->Modify();
				Wave->VoiceActorDirection = UTF8_TO_TCHAR(VADObj.as<std::string>().c_str());
				Changed++;
			}
			else if (VADObj.is<bool>() && !VADObj.as<bool>())
			{
				if (Changed == 0) Wave->Modify();
				Wave->VoiceActorDirection.Empty();
				Changed++;
			}
#endif

			if (Changed > 0)
			{
				// Rebuild proxies if spoken_text changed (affects subtitles in proxies)
				if (SpokenTextObj.valid() && !SpokenTextObj.is<sol::lua_nil_t>())
				{
					for (FDialogueContextMapping& Mapping : Wave->ContextMappings)
					{
						TArray<UDialogueVoice*> RawTargets;
						for (const TObjectPtr<UDialogueVoice>& T : Mapping.Context.Targets)
						{
							RawTargets.Add(T.Get());
						}
						Wave->UpdateContext(Mapping, Mapping.SoundWave, Mapping.Context.Speaker, RawTargets);
					}
				}

				Wave->MarkPackageDirty();
			}

			Session.Log(FString::Printf(TEXT("[OK] configure() -> %d properties changed"), Changed));
			return sol::make_object(Lua, true);
		});
	});
}

REGISTER_LUA_BINDING(DialogueWave, DialogueWaveDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindDialogueWave(Lua, Session);
});
