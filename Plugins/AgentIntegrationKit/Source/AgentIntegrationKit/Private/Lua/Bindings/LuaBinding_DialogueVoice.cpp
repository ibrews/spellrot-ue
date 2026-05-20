// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"

#include "Sound/DialogueVoice.h"
#include "Sound/DialogueTypes.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static const char* GenderToString(EGrammaticalGender::Type Gender)
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

static EGrammaticalGender::Type StringToGender(const FString& Str)
{
	if (Str.Equals(TEXT("masculine"), ESearchCase::IgnoreCase)) return EGrammaticalGender::Masculine;
	if (Str.Equals(TEXT("feminine"), ESearchCase::IgnoreCase))  return EGrammaticalGender::Feminine;
	if (Str.Equals(TEXT("mixed"), ESearchCase::IgnoreCase))     return EGrammaticalGender::Mixed;
	return EGrammaticalGender::Neuter;
}

static const char* PluralityToString(EGrammaticalNumber::Type Plurality)
{
	switch (Plurality)
	{
	case EGrammaticalNumber::Singular: return "singular";
	case EGrammaticalNumber::Plural:   return "plural";
	default:                           return "singular";
	}
}

static EGrammaticalNumber::Type StringToPlurality(const FString& Str)
{
	if (Str.Equals(TEXT("plural"), ESearchCase::IgnoreCase)) return EGrammaticalNumber::Plural;
	return EGrammaticalNumber::Singular;
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> DialogueVoiceDocs = {};

static void BindDialogueVoice(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_dialogue_voice", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UDialogueVoice* Voice = LoadObject<UDialogueVoice>(nullptr, *FPath);
		if (!Voice) return;

		AssetObj["_help_text"] =
			"DialogueVoice enrichment:\n"
			"\n"
			"info() -> {name, gender, plurality, description, localization_guid}\n"
			"\n"
			"configure(params):\n"
			"  gender    = \"neuter\"|\"masculine\"|\"feminine\"|\"mixed\"\n"
			"  plurality = \"singular\"|\"plural\"\n";

		// ---- info() ----
		AssetObj.set_function("info", [Voice, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Voice))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();
			Result["name"] = TCHAR_TO_UTF8(*Voice->GetName());
			Result["gender"] = GenderToString(Voice->Gender);
			Result["plurality"] = PluralityToString(Voice->Plurality);
			Result["description"] = TCHAR_TO_UTF8(*Voice->GetDesc());
			Result["localization_guid"] = TCHAR_TO_UTF8(*Voice->LocalizationGUID.ToString());

			Session.Log(TEXT("[OK] info() -> DialogueVoice summary"));
			return Result;
		});

		// ---- configure(params) ----
		AssetObj.set_function("configure", [Voice, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Voice))
			{
				Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
				return sol::lua_nil;
			}

			Voice->Modify();

			bool bChanged = false;

			auto Gender = Params.get_or<std::string>("gender", "");
			if (!Gender.empty())
			{
				Voice->Gender = StringToGender(UTF8_TO_TCHAR(Gender.c_str()));
				bChanged = true;
			}

			auto Plurality = Params.get_or<std::string>("plurality", "");
			if (!Plurality.empty())
			{
				Voice->Plurality = StringToPlurality(UTF8_TO_TCHAR(Plurality.c_str()));
				bChanged = true;
			}

			if (!bChanged)
			{
				Session.Log(TEXT("[OK] configure() -> nothing changed"));
				return sol::make_object(Lua, true);
			}

			FProperty* Prop = Voice->GetClass()->FindPropertyByName(
				!Gender.empty() ? GET_MEMBER_NAME_CHECKED(UDialogueVoice, Gender)
				                : GET_MEMBER_NAME_CHECKED(UDialogueVoice, Plurality));
			FPropertyChangedEvent Evt(Prop, EPropertyChangeType::ValueSet);
			Voice->PostEditChangeProperty(Evt);
			Voice->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] configure() -> gender=%hs, plurality=%hs"),
				GenderToString(Voice->Gender), PluralityToString(Voice->Plurality)));
			return sol::make_object(Lua, true);
		});
	});
}

REGISTER_LUA_BINDING(DialogueVoice, DialogueVoiceDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindDialogueVoice(Lua, Session);
});
