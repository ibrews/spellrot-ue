// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "Sound/SoundMix.h"
#include "Sound/SoundClass.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> SoundMixDocs = {};

static void BindSoundMix(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_sound_mix", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		USoundMix* Mix = LoadObject<USoundMix>(nullptr, *FPath);
		if (!Mix) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"SoundMix enrichment methods:\n"
			"\n"
			"info() — structured summary:\n"
			"  initial_delay, fade_in_time, duration, fade_out_time,\n"
			"  apply_eq, eq_priority, eq_settings (bands 0-3),\n"
			"  effect_count, effects (array), has_dependency_loop\n"
			"\n"
			"list('effect') — list all sound class adjusters:\n"
			"  returns array of {sound_class, volume, pitch, low_pass_filter_frequency,\n"
			"    apply_to_children, voice_center_channel_volume}\n"
			"\n"
			"add('effect', {sound_class='/path/to/SoundClass', volume=1.0, pitch=1.0,\n"
			"  low_pass_filter_frequency=20000, apply_to_children=false,\n"
			"  voice_center_channel_volume=1.0})\n"
			"\n"
			"remove('effect', {sound_class='/path/to/SoundClass'}) — remove by sound class\n"
			"remove('effect', {index=1}) — remove by 1-based index\n"
			"\n"
			"configure(params) — set mix-level settings:\n"
			"  initial_delay (float), fade_in_time (float), duration (float),\n"
			"  fade_out_time (float), apply_eq (bool), eq_priority (float),\n"
			"  eq (table with frequency_center_0..3, gain_0..3, bandwidth_0..3)\n"
			"\n"
			"configure('effect', {index=1, volume=0.5, pitch=1.0,\n"
			"  low_pass_filter_frequency=20000, apply_to_children=false,\n"
			"  voice_center_channel_volume=1.0, sound_class='/path'}) — modify existing adjuster\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [Mix, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Mix))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table R = Lua.create_table();

			R["initial_delay"] = Mix->InitialDelay;
			R["fade_in_time"] = Mix->FadeInTime;
			R["duration"] = Mix->Duration;
			R["fade_out_time"] = Mix->FadeOutTime;
			R["apply_eq"] = static_cast<bool>(Mix->bApplyEQ);
			R["eq_priority"] = Mix->EQPriority;

			// EQ settings summary
			sol::table EQ = Lua.create_table();
			const FAudioEQEffect& E = Mix->EQSettings;
			EQ["frequency_center_0"] = E.FrequencyCenter0; EQ["gain_0"] = E.Gain0; EQ["bandwidth_0"] = E.Bandwidth0;
			EQ["frequency_center_1"] = E.FrequencyCenter1; EQ["gain_1"] = E.Gain1; EQ["bandwidth_1"] = E.Bandwidth1;
			EQ["frequency_center_2"] = E.FrequencyCenter2; EQ["gain_2"] = E.Gain2; EQ["bandwidth_2"] = E.Bandwidth2;
			EQ["frequency_center_3"] = E.FrequencyCenter3; EQ["gain_3"] = E.Gain3; EQ["bandwidth_3"] = E.Bandwidth3;
			R["eq_settings"] = EQ;

			R["effect_count"] = Mix->SoundClassEffects.Num();

			// Brief effects list
			sol::table Effects = Lua.create_table();
			for (int32 i = 0; i < Mix->SoundClassEffects.Num(); i++)
			{
				const FSoundClassAdjuster& Adj = Mix->SoundClassEffects[i];
				sol::table Entry = Lua.create_table();
				Entry["index"] = i + 1;
				Entry["sound_class"] = Adj.SoundClassObject
					? std::string(TCHAR_TO_UTF8(*Adj.SoundClassObject->GetPathName()))
					: std::string("(none)");
				Entry["volume"] = Adj.VolumeAdjuster;
				Entry["pitch"] = Adj.PitchAdjuster;
				Entry["low_pass_filter_frequency"] = Adj.LowPassFilterFrequency;
				Entry["apply_to_children"] = static_cast<bool>(Adj.bApplyToChildren);
				Entry["voice_center_channel_volume"] = Adj.VoiceCenterChannelVolumeAdjuster;
				Effects[i + 1] = Entry;
			}
			R["effects"] = Effects;

			Session.Log(FString::Printf(TEXT("[OK] info() -> SoundMix, effects=%d, fade_in=%.2f, fade_out=%.2f"),
				Mix->SoundClassEffects.Num(), Mix->FadeInTime, Mix->FadeOutTime));
			return R;
		});

		// ================================================================
		// list("effect")
		// ================================================================
		AssetObj.set_function("list", [Mix, &Session](sol::table /*self*/,
			const std::string& What, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Mix))
			{
				Session.Log(TEXT("[FAIL] list -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (What != "effect" && What != "effects")
			{
				Session.Log(FString::Printf(TEXT("[FAIL] list('%s') -> unknown. Valid: 'effect'"),
					UTF8_TO_TCHAR(What.c_str())));
				return sol::lua_nil;
			}

			sol::table R = Lua.create_table();
			for (int32 i = 0; i < Mix->SoundClassEffects.Num(); i++)
			{
				const FSoundClassAdjuster& Adj = Mix->SoundClassEffects[i];
				sol::table Entry = Lua.create_table();
				Entry["index"] = i + 1;
				Entry["sound_class"] = Adj.SoundClassObject
					? std::string(TCHAR_TO_UTF8(*Adj.SoundClassObject->GetPathName()))
					: std::string("(none)");
				Entry["volume"] = Adj.VolumeAdjuster;
				Entry["pitch"] = Adj.PitchAdjuster;
				Entry["low_pass_filter_frequency"] = Adj.LowPassFilterFrequency;
				Entry["apply_to_children"] = static_cast<bool>(Adj.bApplyToChildren);
				Entry["voice_center_channel_volume"] = Adj.VoiceCenterChannelVolumeAdjuster;
				R[i + 1] = Entry;
			}

			Session.Log(FString::Printf(TEXT("[OK] list('effect') -> %d effects"), Mix->SoundClassEffects.Num()));
			return R;
		});

		// ================================================================
		// add("effect", opts)
		// ================================================================
		AssetObj.set_function("add", [Mix, &Session](sol::table /*self*/,
			const std::string& What, sol::table Opts, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Mix))
			{
				Session.Log(TEXT("[FAIL] add -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (What != "effect")
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add('%s') -> unknown. Valid: 'effect'"),
					UTF8_TO_TCHAR(What.c_str())));
				return sol::lua_nil;
			}

			// sound_class is required
			std::string ClassPath = Opts.get_or<std::string>("sound_class", "");
			if (ClassPath.empty())
			{
				Session.Log(TEXT("[FAIL] add('effect') -> 'sound_class' path is required"));
				return sol::lua_nil;
			}

			USoundClass* SC = LoadObject<USoundClass>(nullptr, UTF8_TO_TCHAR(ClassPath.c_str()));
			if (!SC)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add('effect') -> could not load SoundClass '%s'"),
					UTF8_TO_TCHAR(ClassPath.c_str())));
				return sol::lua_nil;
			}

			// Warn about duplicates
			for (int32 i = 0; i < Mix->SoundClassEffects.Num(); i++)
			{
				if (Mix->SoundClassEffects[i].SoundClassObject == SC)
				{
					Session.Log(FString::Printf(TEXT("[WARN] add('effect') -> SoundClass '%s' already exists at index %d, adding duplicate"),
						*SC->GetName(), i + 1));
					break;
				}
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("SoundMix: Add Effect")));
			Mix->Modify();

			FSoundClassAdjuster NewAdj;
			NewAdj.SoundClassObject = SC;
			NewAdj.VolumeAdjuster = static_cast<float>(Opts.get_or("volume", 1.0));
			NewAdj.PitchAdjuster = static_cast<float>(Opts.get_or("pitch", 1.0));
			NewAdj.LowPassFilterFrequency = static_cast<float>(Opts.get_or("low_pass_filter_frequency", 20000.0));
			NewAdj.bApplyToChildren = Opts.get_or("apply_to_children", false);
			NewAdj.VoiceCenterChannelVolumeAdjuster = static_cast<float>(Opts.get_or("voice_center_channel_volume", 1.0));

			Mix->SoundClassEffects.Add(NewAdj);

			Mix->PostEditChange();
			Mix->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] add('effect') -> added SoundClass '%s' at index %d"),
				*SC->GetName(), Mix->SoundClassEffects.Num()));

			sol::table R = Lua.create_table();
			R["index"] = Mix->SoundClassEffects.Num();
			return R;
		});

		// ================================================================
		// remove("effect", opts)
		// ================================================================
		AssetObj.set_function("remove", [Mix, &Session](sol::table /*self*/,
			const std::string& What, sol::table Opts, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Mix))
			{
				Session.Log(TEXT("[FAIL] remove -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (What != "effect")
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove('%s') -> unknown. Valid: 'effect'"),
					UTF8_TO_TCHAR(What.c_str())));
				return sol::lua_nil;
			}

			int32 RemoveIdx = INDEX_NONE;

			// By index (1-based)
			sol::optional<int> Idx = Opts.get<sol::optional<int>>("index");
			if (Idx.has_value())
			{
				RemoveIdx = Idx.value() - 1; // convert to 0-based
			}
			else
			{
				// By sound_class path
				std::string ClassPath = Opts.get_or<std::string>("sound_class", "");
				if (!ClassPath.empty())
				{
					FString FClassPath = UTF8_TO_TCHAR(ClassPath.c_str());
					for (int32 i = 0; i < Mix->SoundClassEffects.Num(); i++)
					{
						if (Mix->SoundClassEffects[i].SoundClassObject &&
							Mix->SoundClassEffects[i].SoundClassObject->GetPathName() == FClassPath)
						{
							RemoveIdx = i;
							break;
						}
					}
				}
			}

			if (RemoveIdx < 0 || RemoveIdx >= Mix->SoundClassEffects.Num())
			{
				Session.Log(TEXT("[FAIL] remove('effect') -> effect not found or index out of range"));
				return sol::lua_nil;
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("SoundMix: Remove Effect")));
			Mix->Modify();

			FString RemovedName = Mix->SoundClassEffects[RemoveIdx].SoundClassObject
				? Mix->SoundClassEffects[RemoveIdx].SoundClassObject->GetName()
				: TEXT("(none)");

			Mix->SoundClassEffects.RemoveAt(RemoveIdx);

			Mix->PostEditChange();
			Mix->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] remove('effect') -> removed '%s', %d effects remain"),
				*RemovedName, Mix->SoundClassEffects.Num()));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// configure(params) — mix-level settings
		// configure('effect', {index=N, ...}) — modify existing adjuster
		// ================================================================
		AssetObj.set_function("configure", [Mix, &Session](sol::table /*self*/,
			sol::object FirstArg, sol::optional<sol::table> SecondArg, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Mix))
			{
				Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
				return sol::lua_nil;
			}

			// ---- configure('effect', {index=N, ...}) ----
			if (FirstArg.is<std::string>())
			{
				std::string What = FirstArg.as<std::string>();
				if (What != "effect")
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure('%s') -> unknown. Valid: 'effect'"),
						UTF8_TO_TCHAR(What.c_str())));
					return sol::lua_nil;
				}

				if (!SecondArg.has_value())
				{
					Session.Log(TEXT("[FAIL] configure('effect') -> options table with 'index' is required"));
					return sol::lua_nil;
				}

				sol::table Opts = SecondArg.value();
				sol::optional<int> Idx = Opts.get<sol::optional<int>>("index");
				if (!Idx.has_value())
				{
					Session.Log(TEXT("[FAIL] configure('effect') -> 'index' (1-based) is required"));
					return sol::lua_nil;
				}

				int32 AdjIdx = Idx.value() - 1;
				if (AdjIdx < 0 || AdjIdx >= Mix->SoundClassEffects.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure('effect') -> index %d out of range (1..%d)"),
						Idx.value(), Mix->SoundClassEffects.Num()));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("SoundMix: Configure Effect")));
				Mix->Modify();

				FSoundClassAdjuster& Adj = Mix->SoundClassEffects[AdjIdx];
				bool bModified = false;
				FString Changes;

				// sound_class (reassign)
				sol::optional<std::string> NewClassPath = Opts.get<sol::optional<std::string>>("sound_class");
				if (NewClassPath.has_value() && !NewClassPath.value().empty())
				{
					USoundClass* NewSC = LoadObject<USoundClass>(nullptr, UTF8_TO_TCHAR(NewClassPath.value().c_str()));
					if (NewSC)
					{
						Adj.SoundClassObject = NewSC;
						Changes += FString::Printf(TEXT(" sound_class=%s"), *NewSC->GetName());
						bModified = true;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure('effect') -> could not load SoundClass '%s', skipping"),
							UTF8_TO_TCHAR(NewClassPath.value().c_str())));
					}
				}

				sol::optional<double> Vol = Opts.get<sol::optional<double>>("volume");
				if (Vol.has_value())
				{
					Adj.VolumeAdjuster = FMath::Max(0.0f, static_cast<float>(Vol.value()));
					Changes += FString::Printf(TEXT(" volume=%.2f"), Adj.VolumeAdjuster);
					bModified = true;
				}

				sol::optional<double> Pitch = Opts.get<sol::optional<double>>("pitch");
				if (Pitch.has_value())
				{
					Adj.PitchAdjuster = FMath::Clamp(static_cast<float>(Pitch.value()), 0.0f, 8.0f);
					Changes += FString::Printf(TEXT(" pitch=%.2f"), Adj.PitchAdjuster);
					bModified = true;
				}

				sol::optional<double> LPF = Opts.get<sol::optional<double>>("low_pass_filter_frequency");
				if (LPF.has_value())
				{
					Adj.LowPassFilterFrequency = FMath::Clamp(static_cast<float>(LPF.value()), 0.0f, 20000.0f);
					Changes += FString::Printf(TEXT(" lpf=%.0f"), Adj.LowPassFilterFrequency);
					bModified = true;
				}

				sol::optional<bool> ApplyChildren = Opts.get<sol::optional<bool>>("apply_to_children");
				if (ApplyChildren.has_value())
				{
					Adj.bApplyToChildren = ApplyChildren.value();
					Changes += FString::Printf(TEXT(" apply_to_children=%s"), ApplyChildren.value() ? TEXT("true") : TEXT("false"));
					bModified = true;
				}

				sol::optional<double> VoiceCenter = Opts.get<sol::optional<double>>("voice_center_channel_volume");
				if (VoiceCenter.has_value())
				{
					Adj.VoiceCenterChannelVolumeAdjuster = FMath::Max(0.0f, static_cast<float>(VoiceCenter.value()));
					Changes += FString::Printf(TEXT(" voice_center=%.2f"), Adj.VoiceCenterChannelVolumeAdjuster);
					bModified = true;
				}

				if (bModified)
				{
					Mix->PostEditChange();
					Mix->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] configure('effect', index=%d) ->%s"), Idx.value(), *Changes));
					return sol::make_object(Lua, true);
				}

				Session.Log(FString::Printf(TEXT("[OK] configure('effect', index=%d) -> nothing changed"), Idx.value()));
				return sol::make_object(Lua, true);
			}

			// ---- configure({...}) — mix-level settings ----
			if (!FirstArg.is<sol::table>())
			{
				Session.Log(TEXT("[FAIL] configure() -> expected a params table or configure('effect', {index=N, ...})"));
				return sol::lua_nil;
			}

			sol::table Params = FirstArg.as<sol::table>();

			const FScopedTransaction Transaction(FText::FromString(TEXT("SoundMix: Configure")));
			Mix->Modify();
			bool bModified = false;
			FString Changes;

			// initial_delay
			sol::optional<double> Delay = Params.get<sol::optional<double>>("initial_delay");
			if (Delay.has_value())
			{
				Mix->InitialDelay = static_cast<float>(Delay.value());
				Changes += FString::Printf(TEXT(" initial_delay=%.2f"), (double)Mix->InitialDelay);
				bModified = true;
			}

			// fade_in_time
			sol::optional<double> FadeIn = Params.get<sol::optional<double>>("fade_in_time");
			if (FadeIn.has_value())
			{
				Mix->FadeInTime = static_cast<float>(FadeIn.value());
				Changes += FString::Printf(TEXT(" fade_in_time=%.2f"), (double)Mix->FadeInTime);
				bModified = true;
			}

			// duration
			sol::optional<double> Dur = Params.get<sol::optional<double>>("duration");
			if (Dur.has_value())
			{
				Mix->Duration = static_cast<float>(Dur.value());
				Changes += FString::Printf(TEXT(" duration=%.2f"), (double)Mix->Duration);
				bModified = true;
			}

			// fade_out_time
			sol::optional<double> FadeOut = Params.get<sol::optional<double>>("fade_out_time");
			if (FadeOut.has_value())
			{
				Mix->FadeOutTime = static_cast<float>(FadeOut.value());
				Changes += FString::Printf(TEXT(" fade_out_time=%.2f"), (double)Mix->FadeOutTime);
				bModified = true;
			}

			// apply_eq
			sol::optional<bool> ApplyEQ = Params.get<sol::optional<bool>>("apply_eq");
			if (ApplyEQ.has_value())
			{
				Mix->bApplyEQ = ApplyEQ.value();
				Changes += FString::Printf(TEXT(" apply_eq=%s"), ApplyEQ.value() ? TEXT("true") : TEXT("false"));
				bModified = true;
			}

			// eq_priority
			sol::optional<double> EQPri = Params.get<sol::optional<double>>("eq_priority");
			if (EQPri.has_value())
			{
				Mix->EQPriority = static_cast<float>(EQPri.value());
				Changes += FString::Printf(TEXT(" eq_priority=%.1f"), (double)Mix->EQPriority);
				bModified = true;
			}

			// eq (sub-table for EQ band settings)
			sol::optional<sol::table> EQOpt = Params.get<sol::optional<sol::table>>("eq");
			if (EQOpt.has_value())
			{
				sol::table EQ = EQOpt.value();
				FAudioEQEffect& E = Mix->EQSettings;
				bool bEQModified = false;

				sol::optional<double> FC0 = EQ.get<sol::optional<double>>("frequency_center_0");
				if (FC0.has_value()) { E.FrequencyCenter0 = static_cast<float>(FC0.value()); bEQModified = true; }
				sol::optional<double> G0 = EQ.get<sol::optional<double>>("gain_0");
				if (G0.has_value()) { E.Gain0 = static_cast<float>(G0.value()); bEQModified = true; }
				sol::optional<double> BW0 = EQ.get<sol::optional<double>>("bandwidth_0");
				if (BW0.has_value()) { E.Bandwidth0 = static_cast<float>(BW0.value()); bEQModified = true; }

				sol::optional<double> FC1 = EQ.get<sol::optional<double>>("frequency_center_1");
				if (FC1.has_value()) { E.FrequencyCenter1 = static_cast<float>(FC1.value()); bEQModified = true; }
				sol::optional<double> G1 = EQ.get<sol::optional<double>>("gain_1");
				if (G1.has_value()) { E.Gain1 = static_cast<float>(G1.value()); bEQModified = true; }
				sol::optional<double> BW1 = EQ.get<sol::optional<double>>("bandwidth_1");
				if (BW1.has_value()) { E.Bandwidth1 = static_cast<float>(BW1.value()); bEQModified = true; }

				sol::optional<double> FC2 = EQ.get<sol::optional<double>>("frequency_center_2");
				if (FC2.has_value()) { E.FrequencyCenter2 = static_cast<float>(FC2.value()); bEQModified = true; }
				sol::optional<double> G2 = EQ.get<sol::optional<double>>("gain_2");
				if (G2.has_value()) { E.Gain2 = static_cast<float>(G2.value()); bEQModified = true; }
				sol::optional<double> BW2 = EQ.get<sol::optional<double>>("bandwidth_2");
				if (BW2.has_value()) { E.Bandwidth2 = static_cast<float>(BW2.value()); bEQModified = true; }

				sol::optional<double> FC3 = EQ.get<sol::optional<double>>("frequency_center_3");
				if (FC3.has_value()) { E.FrequencyCenter3 = static_cast<float>(FC3.value()); bEQModified = true; }
				sol::optional<double> G3 = EQ.get<sol::optional<double>>("gain_3");
				if (G3.has_value()) { E.Gain3 = static_cast<float>(G3.value()); bEQModified = true; }
				sol::optional<double> BW3 = EQ.get<sol::optional<double>>("bandwidth_3");
				if (BW3.has_value()) { E.Bandwidth3 = static_cast<float>(BW3.value()); bEQModified = true; }

				if (bEQModified)
				{
					E.FrequencyCenter0 = FMath::Clamp(E.FrequencyCenter0, 20.0f, 20000.0f);
					E.FrequencyCenter1 = FMath::Clamp(E.FrequencyCenter1, 20.0f, 20000.0f);
					E.FrequencyCenter2 = FMath::Clamp(E.FrequencyCenter2, 20.0f, 20000.0f);
					E.FrequencyCenter3 = FMath::Clamp(E.FrequencyCenter3, 20.0f, 20000.0f);
					E.Gain0 = FMath::Clamp(E.Gain0, 0.0f, 7.94f);
					E.Gain1 = FMath::Clamp(E.Gain1, 0.0f, 7.94f);
					E.Gain2 = FMath::Clamp(E.Gain2, 0.0f, 7.94f);
					E.Gain3 = FMath::Clamp(E.Gain3, 0.0f, 7.94f);
					E.Bandwidth0 = FMath::Clamp(E.Bandwidth0, 0.1f, 2.0f);
					E.Bandwidth1 = FMath::Clamp(E.Bandwidth1, 0.1f, 2.0f);
					E.Bandwidth2 = FMath::Clamp(E.Bandwidth2, 0.1f, 2.0f);
					E.Bandwidth3 = FMath::Clamp(E.Bandwidth3, 0.1f, 2.0f);
					Changes += TEXT(" eq=<updated>");
					bModified = true;
				}
			}

			if (bModified)
			{
				Mix->PostEditChange();
				Mix->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(%s)"), *Changes.TrimStart()));
				return sol::make_object(Lua, true);
			}

			Session.Log(TEXT("[OK] configure() -> nothing changed. Use help() to see valid keys."));
			return sol::make_object(Lua, true);
		});
	});
}

REGISTER_LUA_BINDING(SoundMix, SoundMixDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindSoundMix(Lua, Session);
});
