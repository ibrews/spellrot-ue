// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "Curves/CurveBase.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/RichCurve.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static const char* InterpModeToString(ERichCurveInterpMode Mode)
{
	switch (Mode)
	{
	case RCIM_Linear:   return "Linear";
	case RCIM_Constant: return "Constant";
	case RCIM_Cubic:    return "Cubic";
	default:            return "None";
	}
}

static ERichCurveInterpMode ParseInterpMode(const std::string& Str)
{
	FString S = UTF8_TO_TCHAR(Str.c_str());
	if (S.Equals(TEXT("Linear"), ESearchCase::IgnoreCase))   return RCIM_Linear;
	if (S.Equals(TEXT("Constant"), ESearchCase::IgnoreCase)) return RCIM_Constant;
	if (S.Equals(TEXT("Cubic"), ESearchCase::IgnoreCase))    return RCIM_Cubic;
	return RCIM_Linear; // default
}

static const char* TangentModeToString(ERichCurveTangentMode Mode)
{
	switch (Mode)
	{
	case RCTM_Auto:      return "Auto";
	case RCTM_User:      return "User";
	case RCTM_Break:     return "Break";
	case RCTM_SmartAuto: return "SmartAuto";
	default:             return "None";
	}
}

static ERichCurveTangentMode ParseTangentMode(const std::string& Str)
{
	FString S = UTF8_TO_TCHAR(Str.c_str());
	if (S.Equals(TEXT("Auto"), ESearchCase::IgnoreCase))      return RCTM_Auto;
	if (S.Equals(TEXT("User"), ESearchCase::IgnoreCase))      return RCTM_User;
	if (S.Equals(TEXT("Break"), ESearchCase::IgnoreCase))     return RCTM_Break;
	if (S.Equals(TEXT("SmartAuto"), ESearchCase::IgnoreCase)) return RCTM_SmartAuto;
	return RCTM_Auto;
}

static const char* TangentWeightModeToString(ERichCurveTangentWeightMode Mode)
{
	switch (Mode)
	{
	case RCTWM_WeightedNone:   return "None";
	case RCTWM_WeightedArrive: return "Arrive";
	case RCTWM_WeightedLeave:  return "Leave";
	case RCTWM_WeightedBoth:   return "Both";
	default:                   return "None";
	}
}

static ERichCurveTangentWeightMode ParseTangentWeightMode(const std::string& Str)
{
	FString S = UTF8_TO_TCHAR(Str.c_str());
	if (S.Equals(TEXT("None"), ESearchCase::IgnoreCase))   return RCTWM_WeightedNone;
	if (S.Equals(TEXT("Arrive"), ESearchCase::IgnoreCase)) return RCTWM_WeightedArrive;
	if (S.Equals(TEXT("Leave"), ESearchCase::IgnoreCase))  return RCTWM_WeightedLeave;
	if (S.Equals(TEXT("Both"), ESearchCase::IgnoreCase))   return RCTWM_WeightedBoth;
	return RCTWM_WeightedNone;
}

static const char* ExtrapolationToString(ERichCurveExtrapolation E)
{
	switch (E)
	{
	case RCCE_Cycle:           return "Cycle";
	case RCCE_CycleWithOffset: return "CycleWithOffset";
	case RCCE_Oscillate:       return "Oscillate";
	case RCCE_Linear:          return "Linear";
	case RCCE_Constant:        return "Constant";
	case RCCE_None:            return "None";
	default:                   return "Unknown";
	}
}

static ERichCurveExtrapolation ParseExtrapolation(const std::string& Str)
{
	FString S = UTF8_TO_TCHAR(Str.c_str());
	if (S.Equals(TEXT("Cycle"), ESearchCase::IgnoreCase))           return RCCE_Cycle;
	if (S.Equals(TEXT("CycleWithOffset"), ESearchCase::IgnoreCase)) return RCCE_CycleWithOffset;
	if (S.Equals(TEXT("Oscillate"), ESearchCase::IgnoreCase))       return RCCE_Oscillate;
	if (S.Equals(TEXT("Linear"), ESearchCase::IgnoreCase))          return RCCE_Linear;
	if (S.Equals(TEXT("Constant"), ESearchCase::IgnoreCase))        return RCCE_Constant;
	if (S.Equals(TEXT("None"), ESearchCase::IgnoreCase))            return RCCE_None;
	return RCCE_Constant;
}

// Curve type detection + channel access
struct FCurveInfo
{
	const char* TypeName;
	int32 NumChannels;
	const char* ChannelNames[4]; // max 4 channels
};

static FCurveInfo GetCurveInfo(UCurveBase* Curve)
{
	if (Curve->IsA<UCurveFloat>())
		return { "Float", 1, { "Value", nullptr, nullptr, nullptr } };
	if (Curve->IsA<UCurveVector>())
		return { "Vector", 3, { "X", "Y", "Z", nullptr } };
	if (Curve->IsA<UCurveLinearColor>())
		return { "Color", 4, { "R", "G", "B", "A" } };
	return { "Unknown", 0, { nullptr, nullptr, nullptr, nullptr } };
}

static FRichCurve* GetRichCurve(UCurveBase* Curve, int32 Channel)
{
	if (UCurveFloat* F = Cast<UCurveFloat>(Curve))
	{
		return &F->FloatCurve;
	}
	if (UCurveVector* V = Cast<UCurveVector>(Curve))
	{
		if (Channel >= 0 && Channel < 3)
			return &V->FloatCurves[Channel];
		return nullptr;
	}
	if (UCurveLinearColor* C = Cast<UCurveLinearColor>(Curve))
	{
		if (Channel >= 0 && Channel < 4)
			return &C->FloatCurves[Channel];
		return nullptr;
	}
	return nullptr;
}

// Get key handle by 0-based index using the public iterator API
static FKeyHandle GetKeyHandleAtIndex(FRichCurve* Curve, int32 Index)
{
	if (!Curve || Index < 0 || Index >= Curve->GetNumKeys())
		return FKeyHandle::Invalid();

	int32 Count = 0;
	for (auto It = Curve->GetKeyHandleIterator(); It; ++It)
	{
		if (Count == Index)
			return *It;
		++Count;
	}
	return FKeyHandle::Invalid();
}

// Apply key properties (interp, tangent, weights) from a params table to a key via handle.
// Uses proper engine APIs (SetKeyInterpMode, SetKeyTangentMode, SetKeyTangentWeightMode)
// instead of direct struct member assignment.
static void ApplyKeyProperties(FRichCurve* RC, FKeyHandle Handle, sol::table& P)
{
	// Interp mode
	std::string InterpStr = P.get_or<std::string>("interp_mode", "");
	if (!InterpStr.empty())
	{
		RC->SetKeyInterpMode(Handle, ParseInterpMode(InterpStr), false);
	}

	// Tangent mode
	std::string TangentStr = P.get_or<std::string>("tangent_mode", "");
	if (!TangentStr.empty())
	{
		RC->SetKeyTangentMode(Handle, ParseTangentMode(TangentStr), false);
	}

	// Tangent weight mode (use proper API instead of direct assignment)
	std::string WeightModeStr = P.get_or<std::string>("tangent_weight_mode", "");
	if (!WeightModeStr.empty())
	{
		RC->SetKeyTangentWeightMode(Handle, ParseTangentWeightMode(WeightModeStr), false);
	}

	// Tangent values
	sol::optional<double> ArriveTan = P.get<sol::optional<double>>("arrive_tangent");
	sol::optional<double> LeaveTan = P.get<sol::optional<double>>("leave_tangent");
	if (ArriveTan.has_value() || LeaveTan.has_value())
	{
		FRichCurveKey& Key = RC->GetKey(Handle);
		if (ArriveTan.has_value())
			Key.ArriveTangent = static_cast<float>(ArriveTan.value());
		if (LeaveTan.has_value())
			Key.LeaveTangent = static_cast<float>(LeaveTan.value());
	}

	// Tangent weight values
	sol::optional<double> ArriveWeight = P.get<sol::optional<double>>("arrive_tangent_weight");
	sol::optional<double> LeaveWeight = P.get<sol::optional<double>>("leave_tangent_weight");
	if (ArriveWeight.has_value() || LeaveWeight.has_value())
	{
		FRichCurveKey& Key = RC->GetKey(Handle);
		if (ArriveWeight.has_value()) Key.ArriveTangentWeight = static_cast<float>(ArriveWeight.value());
		if (LeaveWeight.has_value()) Key.LeaveTangentWeight = static_cast<float>(LeaveWeight.value());
	}

	// Auto-set tangents for Cubic+Auto/SmartAuto
	ERichCurveInterpMode FinalInterp = RC->GetKeyInterpMode(Handle);
	ERichCurveTangentMode FinalTangent = RC->GetKeyTangentMode(Handle);
	if (FinalInterp == RCIM_Cubic && (FinalTangent == RCTM_Auto || FinalTangent == RCTM_SmartAuto))
	{
		RC->AutoSetTangents();
	}
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> CurveDocs = {};

static void BindCurve(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_curve", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UCurveBase* CurveAsset = LoadObject<UCurveBase>(nullptr, *FPath);
		if (!CurveAsset) return;

		FCurveInfo CInfo = GetCurveInfo(CurveAsset);
		if (CInfo.NumChannels == 0) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"Element types for add/remove/list/configure:\n"
			"  key              — a keyframe on a curve channel\n"
			"\n"
			"add(type, params):\n"
			"  add(\"key\", {time=1.0, value=0.5, channel=0, interp_mode=\"Cubic\", tangent_mode=\"Auto\"})\n"
			"    channel: Float=0 only, Vector=0(X)/1(Y)/2(Z), Color=0(R)/1(G)/2(B)/3(A)\n"
			"    interp_mode: \"Linear\", \"Constant\", \"Cubic\" (default: \"Linear\")\n"
			"    tangent_mode: \"Auto\", \"User\", \"Break\", \"SmartAuto\" (default: \"Auto\")\n"
			"    arrive_tangent, leave_tangent: floats for User/Break tangent modes\n"
			"    tangent_weight_mode: \"None\", \"Arrive\", \"Leave\", \"Both\"\n"
			"    arrive_tangent_weight, leave_tangent_weight: float weights\n"
			"\n"
			"remove(type, params):\n"
			"  remove(\"key\", {index=1, channel=0})     — by 1-based index\n"
			"  remove(\"key\", {time=1.0, channel=0})    — by time (tolerance ~0.0001)\n"
			"  remove(\"all_keys\", {channel=0})          — clear all keys on channel\n"
			"  remove(\"redundant_keys\", {channel=0, tolerance=0.001}) — simplify curve\n"
			"\n"
			"list(type, params?):\n"
			"  list(\"keys\", {channel=0}) — all keyframes on the channel\n"
			"\n"
			"configure(params):\n"
			"  configure({channel=0, pre_infinity=\"Constant\", post_infinity=\"Cycle\", default_value=0.0})\n"
			"    pre_infinity/post_infinity: \"Cycle\", \"CycleWithOffset\", \"Oscillate\", \"Linear\", \"Constant\", \"None\"\n"
			"  configure({color_adjust={hue=0, saturation=1, brightness=1, brightness_curve=1, vibrance=0, min_alpha=0, max_alpha=1}})\n"
			"    color_adjust: only for Color curves (UCurveLinearColor)\n"
			"\n"
			"configure_key({index=1, channel=0, value=, time=, interp_mode=, tangent_mode=, ...}):\n"
			"  modify a single keyframe's properties\n"
			"\n"
			"Action methods:\n"
			"  info()                                — summary of curve type and per-channel data\n"
			"  evaluate({time=1.0, channel=0})       — evaluate curve at time\n"
			"  set_keys({channel=0, keys={{time=0, value=0}, {time=1, value=1}}}) — bulk replace keys\n"
			"  shift_keys({channel=0, delta_time=1.0})     — shift all keys in time\n"
			"  scale_keys({channel=0, origin=0, factor=2}) — scale keys around origin\n"
			"  bake({channel=0, sample_rate=30})           — bake curve to sampled keys\n"
			"  reset_all()                                 — clear all channels\n"
			"  import_csv(csv_string)                      — import from CSV\n"
			"  export_json()                               — export as JSON string\n"
			"  import_json(json_string)                    — import from JSON string\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [CurveAsset, CInfo, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();

			Result["type"] = CInfo.TypeName;
			Result["path"] = TCHAR_TO_UTF8(*CurveAsset->GetPathName());
			Result["channels"] = CInfo.NumChannels;

			sol::table Channels = Lua.create_table();
			for (int32 i = 0; i < CInfo.NumChannels; i++)
			{
				FRichCurve* RC = GetRichCurve(CurveAsset, i);
				if (!RC) continue;

				sol::table Ch = Lua.create_table();
				Ch["index"] = i;
				Ch["name"] = CInfo.ChannelNames[i];
				Ch["key_count"] = RC->GetNumKeys();
				Ch["is_empty"] = RC->IsEmpty();

				float MinTime = 0.f, MaxTime = 0.f;
				if (RC->GetNumKeys() > 0)
				{
					RC->GetTimeRange(MinTime, MaxTime);
				}
				sol::table TimeRange = Lua.create_table();
				TimeRange["min"] = MinTime;
				TimeRange["max"] = MaxTime;
				Ch["time_range"] = TimeRange;

				float MinVal = 0.f, MaxVal = 0.f;
				if (RC->GetNumKeys() > 0)
				{
					RC->GetValueRange(MinVal, MaxVal);
				}
				sol::table ValRange = Lua.create_table();
				ValRange["min"] = MinVal;
				ValRange["max"] = MaxVal;
				Ch["value_range"] = ValRange;

				Ch["pre_infinity"] = ExtrapolationToString(RC->PreInfinityExtrap);
				Ch["post_infinity"] = ExtrapolationToString(RC->PostInfinityExtrap);
				if (RC->DefaultValue < MAX_flt) Ch["default_value"] = static_cast<double>(RC->DefaultValue);

				Channels[i + 1] = Ch;
			}
			Result["channels_data"] = Channels;

			// Color curve adjustments
			if (UCurveLinearColor* ColorCurve = Cast<UCurveLinearColor>(CurveAsset))
			{
				sol::table Adj = Lua.create_table();
				Adj["hue"] = static_cast<double>(ColorCurve->AdjustHue);
				Adj["saturation"] = static_cast<double>(ColorCurve->AdjustSaturation);
				Adj["brightness"] = static_cast<double>(ColorCurve->AdjustBrightness);
				Adj["brightness_curve"] = static_cast<double>(ColorCurve->AdjustBrightnessCurve);
				Adj["vibrance"] = static_cast<double>(ColorCurve->AdjustVibrance);
				Adj["min_alpha"] = static_cast<double>(ColorCurve->AdjustMinAlpha);
				Adj["max_alpha"] = static_cast<double>(ColorCurve->AdjustMaxAlpha);
				Result["color_adjustments"] = Adj;
			}

			Session.Log(FString::Printf(TEXT("[OK] info() -> %s curve, %d channel(s)"), UTF8_TO_TCHAR(CInfo.TypeName), CInfo.NumChannels));
			return Result;
		});

		// ================================================================
		// evaluate({time=, channel=0})
		// ================================================================
		AssetObj.set_function("evaluate", [CurveAsset, CInfo, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			sol::optional<double> TimeOpt = Params.get<sol::optional<double>>("time");
			if (!TimeOpt.has_value())
			{
				Session.Log(TEXT("[FAIL] evaluate() -> 'time' required"));
				return sol::lua_nil;
			}

			int32 Channel = Params.get_or("channel", 0);
			FRichCurve* RC = GetRichCurve(CurveAsset, Channel);
			if (!RC)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] evaluate() -> invalid channel %d"), Channel));
				return sol::lua_nil;
			}

			float Time = static_cast<float>(TimeOpt.value());
			float Value = RC->Eval(Time);

			Session.Log(FString::Printf(TEXT("[OK] evaluate(time=%.4f, channel=%d) -> %.4f"), Time, Channel, Value));
			return sol::make_object(Lua, static_cast<double>(Value));
		});

		// ================================================================
		// list(type, params?)
		// ================================================================
		AssetObj.set_function("list", [CurveAsset, CInfo, &Session](sol::table Self,
			sol::optional<std::string> TypeOpt, sol::optional<sol::table> ParamsOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = Self["info"];
				if (InfoFn.valid()) return InfoFn(Self);
				return sol::lua_nil;
			}

			if (FType.Equals(TEXT("keys"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("key"), ESearchCase::IgnoreCase))
			{
				int32 Channel = 0;
				if (ParamsOpt.has_value())
				{
					Channel = ParamsOpt.value().get_or("channel", 0);
				}

				FRichCurve* RC = GetRichCurve(CurveAsset, Channel);
				if (!RC)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] list(\"keys\") -> invalid channel %d"), Channel));
					return sol::lua_nil;
				}

				const TArray<FRichCurveKey>& Keys = RC->GetConstRefOfKeys();
				sol::table Result = Lua.create_table();

				for (int32 i = 0; i < Keys.Num(); i++)
				{
					const FRichCurveKey& Key = Keys[i];
					sol::table K = Lua.create_table();
					K["index"] = i + 1; // 1-based Lua index
					K["time"] = static_cast<double>(Key.Time);
					K["value"] = static_cast<double>(Key.Value);
					K["interp_mode"] = InterpModeToString(Key.InterpMode);
					K["tangent_mode"] = TangentModeToString(Key.TangentMode);
					K["arrive_tangent"] = static_cast<double>(Key.ArriveTangent);
					K["leave_tangent"] = static_cast<double>(Key.LeaveTangent);
					K["tangent_weight_mode"] = TangentWeightModeToString(Key.TangentWeightMode);
					K["arrive_tangent_weight"] = static_cast<double>(Key.ArriveTangentWeight);
					K["leave_tangent_weight"] = static_cast<double>(Key.LeaveTangentWeight);
					Result[i + 1] = K;
				}

				const char* ChName = (Channel < CInfo.NumChannels) ? CInfo.ChannelNames[Channel] : "?";
				Session.Log(FString::Printf(TEXT("[OK] list(\"keys\", channel=%d/%s) -> %d keys"),
					Channel, UTF8_TO_TCHAR(ChName), Keys.Num()));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: keys"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// add(type, params)
		// ================================================================
		AssetObj.set_function("add", [CurveAsset, CInfo, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> ParamsOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("key"), ESearchCase::IgnoreCase))
			{
				if (!ParamsOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"key\") -> params required: {time=, value=, channel=0}"));
					return sol::lua_nil;
				}
				sol::table P = ParamsOpt.value();

				sol::optional<double> TimeOpt = P.get<sol::optional<double>>("time");
				sol::optional<double> ValueOpt = P.get<sol::optional<double>>("value");
				if (!TimeOpt.has_value() || !ValueOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"key\") -> 'time' and 'value' required"));
					return sol::lua_nil;
				}

				int32 Channel = P.get_or("channel", 0);
				FRichCurve* RC = GetRichCurve(CurveAsset, Channel);
				if (!RC)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"key\") -> invalid channel %d"), Channel));
					return sol::lua_nil;
				}

				float Time = static_cast<float>(TimeOpt.value());
				float Value = static_cast<float>(ValueOpt.value());

				// update_or_add mode: if a key exists at this time, update it instead
				bool bUpdateOrAdd = P.get_or("update_or_add", false);

				const FScopedTransaction Transaction(FText::FromString(TEXT("Curve: Add Key")));
				CurveAsset->Modify();

				FKeyHandle Handle;
				if (bUpdateOrAdd)
				{
					Handle = RC->UpdateOrAddKey(Time, Value);
				}
				else
				{
					Handle = RC->AddKey(Time, Value);
				}

				// Apply optional key properties
				ApplyKeyProperties(RC, Handle, P);

				CurveAsset->MarkPackageDirty();

				// Find the resulting index
				const TArray<FRichCurveKey>& Keys = RC->GetConstRefOfKeys();
				int32 ResultIndex = -1;
				for (int32 i = 0; i < Keys.Num(); i++)
				{
					if (FMath::IsNearlyEqual(Keys[i].Time, Time, UE_KINDA_SMALL_NUMBER))
					{
						ResultIndex = i;
						break;
					}
				}

				int32 LuaIdx = ResultIndex + 1;
				const char* ChName = (Channel < CInfo.NumChannels) ? CInfo.ChannelNames[Channel] : "?";
				Session.Log(FString::Printf(TEXT("[OK] add(\"key\", time=%.4f, value=%.4f, channel=%d/%s) -> index %d"),
					Time, Value, Channel, UTF8_TO_TCHAR(ChName), LuaIdx));
				return sol::make_object(Lua, LuaIdx);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: key"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// remove(type, params)
		// ================================================================
		AssetObj.set_function("remove", [CurveAsset, CInfo, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> ParamsOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("key"), ESearchCase::IgnoreCase))
			{
				if (!ParamsOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] remove(\"key\") -> params required: {index= or time=, channel=0}"));
					return sol::lua_nil;
				}
				sol::table P = ParamsOpt.value();

				int32 Channel = P.get_or("channel", 0);
				FRichCurve* RC = GetRichCurve(CurveAsset, Channel);
				if (!RC)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"key\") -> invalid channel %d"), Channel));
					return sol::lua_nil;
				}

				FKeyHandle Handle = FKeyHandle::Invalid();

				// By index (1-based)
				sol::optional<int> IndexOpt = P.get<sol::optional<int>>("index");
				if (IndexOpt.has_value())
				{
					int32 Idx = IndexOpt.value() - 1; // Convert to 0-based
					Handle = GetKeyHandleAtIndex(RC, Idx);
					if (!RC->IsKeyHandleValid(Handle))
					{
						Session.Log(FString::Printf(TEXT("[FAIL] remove(\"key\") -> index %d out of range (1-%d)"),
							IndexOpt.value(), RC->GetNumKeys()));
						return sol::lua_nil;
					}
				}
				else
				{
					// By time (tolerance = UE_KINDA_SMALL_NUMBER ~0.0001)
					sol::optional<double> TimeOpt = P.get<sol::optional<double>>("time");
					if (!TimeOpt.has_value())
					{
						Session.Log(TEXT("[FAIL] remove(\"key\") -> 'index' or 'time' required"));
						return sol::lua_nil;
					}

					float Time = static_cast<float>(TimeOpt.value());
					float Tolerance = static_cast<float>(P.get_or("tolerance", static_cast<double>(UE_KINDA_SMALL_NUMBER)));
					Handle = RC->FindKey(Time, Tolerance);
					if (!RC->IsKeyHandleValid(Handle))
					{
						Session.Log(FString::Printf(TEXT("[FAIL] remove(\"key\") -> no key found at time=%.4f (tolerance=%.6f)"), Time, Tolerance));
						return sol::lua_nil;
					}
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("Curve: Remove Key")));
				CurveAsset->Modify();
				RC->DeleteKey(Handle);
				CurveAsset->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"key\", channel=%d) -> %d keys remaining"),
					Channel, RC->GetNumKeys()));
				return sol::make_object(Lua, true);
			}

			if (FType.Equals(TEXT("all_keys"), ESearchCase::IgnoreCase))
			{
				int32 Channel = 0;
				if (ParamsOpt.has_value())
				{
					Channel = ParamsOpt.value().get_or("channel", 0);
				}

				FRichCurve* RC = GetRichCurve(CurveAsset, Channel);
				if (!RC)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"all_keys\") -> invalid channel %d"), Channel));
					return sol::lua_nil;
				}

				int32 PrevCount = RC->GetNumKeys();

				const FScopedTransaction Transaction(FText::FromString(TEXT("Curve: Clear Keys")));
				CurveAsset->Modify();
				RC->Reset();
				CurveAsset->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"all_keys\", channel=%d) -> removed %d keys"),
					Channel, PrevCount));
				return sol::make_object(Lua, PrevCount);
			}

			if (FType.Equals(TEXT("redundant_keys"), ESearchCase::IgnoreCase))
			{
				int32 Channel = 0;
				double Tolerance = 0.001;
				if (ParamsOpt.has_value())
				{
					Channel = ParamsOpt.value().get_or("channel", 0);
					Tolerance = ParamsOpt.value().get_or("tolerance", 0.001);
				}

				FRichCurve* RC = GetRichCurve(CurveAsset, Channel);
				if (!RC)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"redundant_keys\") -> invalid channel %d"), Channel));
					return sol::lua_nil;
				}

				int32 PrevCount = RC->GetNumKeys();

				const FScopedTransaction Transaction(FText::FromString(TEXT("Curve: Remove Redundant Keys")));
				CurveAsset->Modify();
				RC->RemoveRedundantKeys(static_cast<float>(Tolerance), FFrameRate(30, 1));
				CurveAsset->MarkPackageDirty();

				int32 Removed = PrevCount - RC->GetNumKeys();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"redundant_keys\", channel=%d, tolerance=%.4f) -> removed %d, %d remaining"),
					Channel, static_cast<float>(Tolerance), Removed, RC->GetNumKeys()));
				return sol::make_object(Lua, Removed);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: key, all_keys, redundant_keys"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// configure(params)
		// ================================================================
		AssetObj.set_function("configure", [CurveAsset, CInfo, &Session](sol::table /*self*/,
			sol::object Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!Params.is<sol::table>())
			{
				Session.Log(TEXT("[FAIL] configure() -> table params required: {channel=0, pre_infinity=, post_infinity=, default_value=}"));
				return sol::lua_nil;
			}
			sol::table P = Params.as<sol::table>();

			const FScopedTransaction Transaction(FText::FromString(TEXT("Curve: Configure")));
			CurveAsset->Modify();
			bool bModified = false;

			// Per-channel curve settings
			int32 Channel = P.get_or("channel", 0);
			FRichCurve* RC = GetRichCurve(CurveAsset, Channel);
			if (!RC)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure() -> invalid channel %d"), Channel));
				return sol::lua_nil;
			}

			std::string PreInf = P.get_or<std::string>("pre_infinity", "");
			if (!PreInf.empty())
			{
				RC->PreInfinityExtrap = ParseExtrapolation(PreInf);
				bModified = true;
			}

			std::string PostInf = P.get_or<std::string>("post_infinity", "");
			if (!PostInf.empty())
			{
				RC->PostInfinityExtrap = ParseExtrapolation(PostInf);
				bModified = true;
			}

			sol::optional<double> DefaultVal = P.get<sol::optional<double>>("default_value");
			if (DefaultVal.has_value())
			{
				RC->SetDefaultValue(static_cast<float>(DefaultVal.value()));
				bModified = true;
			}

			// Allow clearing the default value
			sol::optional<bool> ClearDefault = P.get<sol::optional<bool>>("clear_default_value");
			if (ClearDefault.has_value() && ClearDefault.value())
			{
				RC->ClearDefaultValue();
				bModified = true;
			}

			// Color curve adjustments (UCurveLinearColor only)
			sol::optional<sol::table> ColorAdj = P.get<sol::optional<sol::table>>("color_adjust");
			if (ColorAdj.has_value())
			{
				UCurveLinearColor* ColorCurve = Cast<UCurveLinearColor>(CurveAsset);
				if (!ColorCurve)
				{
					Session.Log(TEXT("[FAIL] configure() -> color_adjust only valid for Color curves"));
					return sol::lua_nil;
				}

				sol::table CA = ColorAdj.value();
				sol::optional<double> Hue = CA.get<sol::optional<double>>("hue");
				if (Hue.has_value()) { ColorCurve->AdjustHue = static_cast<float>(Hue.value()); bModified = true; }

				sol::optional<double> Sat = CA.get<sol::optional<double>>("saturation");
				if (Sat.has_value()) { ColorCurve->AdjustSaturation = static_cast<float>(Sat.value()); bModified = true; }

				sol::optional<double> Bright = CA.get<sol::optional<double>>("brightness");
				if (Bright.has_value()) { ColorCurve->AdjustBrightness = static_cast<float>(Bright.value()); bModified = true; }

				sol::optional<double> BrightCurve = CA.get<sol::optional<double>>("brightness_curve");
				if (BrightCurve.has_value()) { ColorCurve->AdjustBrightnessCurve = static_cast<float>(BrightCurve.value()); bModified = true; }

				sol::optional<double> Vib = CA.get<sol::optional<double>>("vibrance");
				if (Vib.has_value()) { ColorCurve->AdjustVibrance = static_cast<float>(Vib.value()); bModified = true; }

				sol::optional<double> MinA = CA.get<sol::optional<double>>("min_alpha");
				if (MinA.has_value()) { ColorCurve->AdjustMinAlpha = static_cast<float>(MinA.value()); bModified = true; }

				sol::optional<double> MaxA = CA.get<sol::optional<double>>("max_alpha");
				if (MaxA.has_value()) { ColorCurve->AdjustMaxAlpha = static_cast<float>(MaxA.value()); bModified = true; }

#if WITH_EDITOR
				if (bModified)
				{
					FPropertyChangedEvent Evt(nullptr);
					ColorCurve->PostEditChangeProperty(Evt);
				}
#endif
			}

			if (bModified)
			{
				CurveAsset->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(channel=%d) -> pre=%s post=%s"),
					Channel, UTF8_TO_TCHAR(ExtrapolationToString(RC->PreInfinityExtrap)),
					UTF8_TO_TCHAR(ExtrapolationToString(RC->PostInfinityExtrap))));
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[OK] configure(channel=%d) -> nothing changed"), Channel));
			}

			return sol::make_object(Lua, true);
		});

		// ================================================================
		// configure_key({index=, channel=0, value=, time=, interp_mode=, tangent_mode=, ...})
		// ================================================================
		AssetObj.set_function("configure_key", [CurveAsset, CInfo, &Session](sol::table /*self*/,
			sol::object Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!Params.is<sol::table>())
			{
				Session.Log(TEXT("[FAIL] configure_key() -> table params required: {index=, channel=0, ...}"));
				return sol::lua_nil;
			}
			sol::table P = Params.as<sol::table>();

			sol::optional<int> IndexOpt = P.get<sol::optional<int>>("index");
			if (!IndexOpt.has_value())
			{
				Session.Log(TEXT("[FAIL] configure_key() -> 'index' required (1-based)"));
				return sol::lua_nil;
			}

			int32 Index = IndexOpt.value() - 1; // Lua 1-based to 0-based
			int32 Channel = P.get_or("channel", 0);
			FRichCurve* RC = GetRichCurve(CurveAsset, Channel);
			if (!RC)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure_key() -> invalid channel %d"), Channel));
				return sol::lua_nil;
			}

			if (Index < 0 || Index >= RC->GetNumKeys())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure_key() -> index %d out of range (1-%d)"), Index + 1, RC->GetNumKeys()));
				return sol::lua_nil;
			}

			FKeyHandle Handle = GetKeyHandleAtIndex(RC, Index);
			if (!RC->IsKeyHandleValid(Handle))
			{
				Session.Log(TEXT("[FAIL] configure_key() -> invalid key handle"));
				return sol::lua_nil;
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("Curve: Configure Key")));
			CurveAsset->Modify();

			// Value — use SetKeyValue which auto-sets tangents
			sol::optional<double> ValOpt = P.get<sol::optional<double>>("value");
			if (ValOpt.has_value())
			{
				RC->SetKeyValue(Handle, static_cast<float>(ValOpt.value()), false);
			}

			// Time — SetKeyTime deletes+re-adds the key, but reuses the handle
			sol::optional<double> TimeOpt = P.get<sol::optional<double>>("time");
			if (TimeOpt.has_value())
			{
				RC->SetKeyTime(Handle, static_cast<float>(TimeOpt.value()));
			}

			// Apply interp/tangent/weight properties via shared helper
			ApplyKeyProperties(RC, Handle, P);

			CurveAsset->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] configure_key(index=%d, channel=%d)"), Index + 1, Channel));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// set_keys({channel=0, keys={{time=, value=, interp_mode=, ...}, ...}})
		// ================================================================
		AssetObj.set_function("set_keys", [CurveAsset, CInfo, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			int32 Channel = Params.get_or("channel", 0);
			FRichCurve* RC = GetRichCurve(CurveAsset, Channel);
			if (!RC)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_keys() -> invalid channel %d"), Channel));
				return sol::lua_nil;
			}

			sol::optional<sol::table> KeysOpt = Params.get<sol::optional<sol::table>>("keys");
			if (!KeysOpt.has_value())
			{
				Session.Log(TEXT("[FAIL] set_keys() -> 'keys' array required"));
				return sol::lua_nil;
			}
			sol::table KeysTable = KeysOpt.value();

			// Build sorted array of FRichCurveKey
			TArray<FRichCurveKey> NewKeys;
			int32 Count = static_cast<int32>(KeysTable.size());
			NewKeys.Reserve(Count);

			for (int32 i = 1; i <= Count; i++)
			{
				sol::optional<sol::table> EntryOpt = KeysTable.get<sol::optional<sol::table>>(i);
				if (!EntryOpt.has_value()) continue;
				sol::table E = EntryOpt.value();

				sol::optional<double> T = E.get<sol::optional<double>>("time");
				sol::optional<double> V = E.get<sol::optional<double>>("value");
				if (!T.has_value() || !V.has_value()) continue;

				FRichCurveKey Key(static_cast<float>(T.value()), static_cast<float>(V.value()));

				std::string InterpStr = E.get_or<std::string>("interp_mode", "");
				if (!InterpStr.empty()) Key.InterpMode = ParseInterpMode(InterpStr);

				std::string TangentStr = E.get_or<std::string>("tangent_mode", "");
				if (!TangentStr.empty()) Key.TangentMode = ParseTangentMode(TangentStr);

				std::string WMStr = E.get_or<std::string>("tangent_weight_mode", "");
				if (!WMStr.empty()) Key.TangentWeightMode = ParseTangentWeightMode(WMStr);

				sol::optional<double> AT = E.get<sol::optional<double>>("arrive_tangent");
				if (AT.has_value()) Key.ArriveTangent = static_cast<float>(AT.value());
				sol::optional<double> LT = E.get<sol::optional<double>>("leave_tangent");
				if (LT.has_value()) Key.LeaveTangent = static_cast<float>(LT.value());
				sol::optional<double> AW = E.get<sol::optional<double>>("arrive_tangent_weight");
				if (AW.has_value()) Key.ArriveTangentWeight = static_cast<float>(AW.value());
				sol::optional<double> LW = E.get<sol::optional<double>>("leave_tangent_weight");
				if (LW.has_value()) Key.LeaveTangentWeight = static_cast<float>(LW.value());

				NewKeys.Add(Key);
			}

			// Sort by time as required by SetKeys
			NewKeys.Sort([](const FRichCurveKey& A, const FRichCurveKey& B) { return A.Time < B.Time; });

			const FScopedTransaction Transaction(FText::FromString(TEXT("Curve: Set Keys")));
			CurveAsset->Modify();
			RC->SetKeys(NewKeys);
			RC->AutoSetTangents();
			CurveAsset->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] set_keys(channel=%d) -> %d keys set"), Channel, NewKeys.Num()));
			return sol::make_object(Lua, NewKeys.Num());
		});

		// ================================================================
		// shift_keys({channel=0, delta_time=1.0})
		// ================================================================
		AssetObj.set_function("shift_keys", [CurveAsset, CInfo, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			int32 Channel = Params.get_or("channel", 0);
			FRichCurve* RC = GetRichCurve(CurveAsset, Channel);
			if (!RC)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] shift_keys() -> invalid channel %d"), Channel));
				return sol::lua_nil;
			}

			sol::optional<double> DeltaOpt = Params.get<sol::optional<double>>("delta_time");
			if (!DeltaOpt.has_value())
			{
				Session.Log(TEXT("[FAIL] shift_keys() -> 'delta_time' required"));
				return sol::lua_nil;
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("Curve: Shift Keys")));
			CurveAsset->Modify();
			RC->ShiftCurve(static_cast<float>(DeltaOpt.value()));
			CurveAsset->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] shift_keys(channel=%d, delta=%.4f) -> %d keys shifted"),
				Channel, static_cast<float>(DeltaOpt.value()), RC->GetNumKeys()));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// scale_keys({channel=0, origin=0, factor=2})
		// ================================================================
		AssetObj.set_function("scale_keys", [CurveAsset, CInfo, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			int32 Channel = Params.get_or("channel", 0);
			FRichCurve* RC = GetRichCurve(CurveAsset, Channel);
			if (!RC)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] scale_keys() -> invalid channel %d"), Channel));
				return sol::lua_nil;
			}

			sol::optional<double> FactorOpt = Params.get<sol::optional<double>>("factor");
			if (!FactorOpt.has_value())
			{
				Session.Log(TEXT("[FAIL] scale_keys() -> 'factor' required"));
				return sol::lua_nil;
			}

			double Origin = Params.get_or("origin", 0.0);

			const FScopedTransaction Transaction(FText::FromString(TEXT("Curve: Scale Keys")));
			CurveAsset->Modify();
			RC->ScaleCurve(static_cast<float>(Origin), static_cast<float>(FactorOpt.value()));
			CurveAsset->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] scale_keys(channel=%d, origin=%.4f, factor=%.4f)"),
				Channel, static_cast<float>(Origin), static_cast<float>(FactorOpt.value())));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// bake({channel=0, sample_rate=30})
		// ================================================================
		AssetObj.set_function("bake", [CurveAsset, CInfo, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			int32 Channel = Params.get_or("channel", 0);
			FRichCurve* RC = GetRichCurve(CurveAsset, Channel);
			if (!RC)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] bake() -> invalid channel %d"), Channel));
				return sol::lua_nil;
			}

			if (RC->GetNumKeys() == 0)
			{
				Session.Log(FString::Printf(TEXT("[OK] bake(channel=%d) -> no keys to bake"), Channel));
				return sol::make_object(Lua, 0);
			}

			double SampleRate = Params.get_or("sample_rate", 30.0);
			if (SampleRate <= 0.0)
			{
				Session.Log(TEXT("[FAIL] bake() -> sample_rate must be > 0"));
				return sol::lua_nil;
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("Curve: Bake")));
			CurveAsset->Modify();
			RC->BakeCurve(static_cast<float>(SampleRate));
			CurveAsset->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] bake(channel=%d, sample_rate=%.1f) -> %d keys after bake"),
				Channel, static_cast<float>(SampleRate), RC->GetNumKeys()));
			return sol::make_object(Lua, RC->GetNumKeys());
		});

		// ================================================================
		// reset_all() — clear all channels at once
		// ================================================================
		AssetObj.set_function("reset_all", [CurveAsset, CInfo, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			const FScopedTransaction Transaction(FText::FromString(TEXT("Curve: Reset All")));
			CurveAsset->Modify();
			CurveAsset->ResetCurve();
			CurveAsset->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] reset_all() -> cleared all %d channel(s)"), CInfo.NumChannels));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// import_csv(csv_string)
		// ================================================================
		AssetObj.set_function("import_csv", [CurveAsset, CInfo, &Session](sol::table /*self*/,
			const std::string& CsvStr, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (CsvStr.empty())
			{
				Session.Log(TEXT("[FAIL] import_csv() -> CSV string required"));
				return sol::lua_nil;
			}

			FString FCSVStr = UTF8_TO_TCHAR(CsvStr.c_str());

			const FScopedTransaction Transaction(FText::FromString(TEXT("Curve: Import CSV")));
			CurveAsset->Modify();
			TArray<FString> Problems = CurveAsset->CreateCurveFromCSVString(FCSVStr);
			CurveAsset->MarkPackageDirty();

			sol::table Result = Lua.create_table();
			Result["success"] = (Problems.Num() == 0);

			if (Problems.Num() > 0)
			{
				sol::table ProbArr = Lua.create_table();
				for (int32 i = 0; i < Problems.Num(); i++)
				{
					ProbArr[i + 1] = TCHAR_TO_UTF8(*Problems[i]);
				}
				Result["problems"] = ProbArr;
				Session.Log(FString::Printf(TEXT("[OK] import_csv() -> imported with %d warnings"), Problems.Num()));
			}
			else
			{
				Session.Log(TEXT("[OK] import_csv() -> imported successfully"));
			}

			return Result;
		});

		// ================================================================
		// export_json() — export curve as JSON string
		// ================================================================
		AssetObj.set_function("export_json", [CurveAsset, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString JsonStr = CurveAsset->ExportAsJSONString();
			Session.Log(FString::Printf(TEXT("[OK] export_json() -> %d chars"), JsonStr.Len()));
			return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*JsonStr)));
		});

		// ================================================================
		// import_json(json_string)
		// ================================================================
		AssetObj.set_function("import_json", [CurveAsset, CInfo, &Session](sol::table /*self*/,
			const std::string& JsonStr, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (JsonStr.empty())
			{
				Session.Log(TEXT("[FAIL] import_json() -> JSON string required"));
				return sol::lua_nil;
			}

			FString FJsonStr = UTF8_TO_TCHAR(JsonStr.c_str());

			const FScopedTransaction Transaction(FText::FromString(TEXT("Curve: Import JSON")));
			CurveAsset->Modify();

			TArray<FString> Problems;
			CurveAsset->ImportFromJSONString(FJsonStr, Problems);
			CurveAsset->MarkPackageDirty();

			sol::table Result = Lua.create_table();
			Result["success"] = (Problems.Num() == 0);

			if (Problems.Num() > 0)
			{
				sol::table ProbArr = Lua.create_table();
				for (int32 i = 0; i < Problems.Num(); i++)
				{
					ProbArr[i + 1] = TCHAR_TO_UTF8(*Problems[i]);
				}
				Result["problems"] = ProbArr;
				Session.Log(FString::Printf(TEXT("[OK] import_json() -> imported with %d warnings"), Problems.Num()));
			}
			else
			{
				Session.Log(TEXT("[OK] import_json() -> imported successfully"));
			}

			return Result;
		});
	});
}

REGISTER_LUA_BINDING(Curve, CurveDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindCurve(Lua, Session);
});
