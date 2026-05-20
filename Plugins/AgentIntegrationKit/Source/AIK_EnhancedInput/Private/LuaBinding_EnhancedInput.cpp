// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

// Enhanced Input
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputTriggers.h"
#include "InputModifiers.h"
#include "EnhancedActionKeyMapping.h"
#include "EnhancedInputModule.h"
#include "EnhancedInputLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Lua/LuaDynamicTypeHelper.h"
#include "Modules/ModuleManager.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// Helpers
// ============================================================================

static void FirePostEditProperty(UObject* Obj, FName PropertyName)
{
	if (!Obj) return;
	FProperty* Prop = Obj->GetClass()->FindPropertyByName(PropertyName);
	if (Prop)
	{
		FPropertyChangedEvent Evt(Prop);
		Obj->PostEditChangeProperty(Evt);
	}
	else
	{
		Obj->PostEditChange();
	}
}

static UInputAction* ResolveInputActionFromRef(const FString& ActionRef)
{
	if (ActionRef.IsEmpty()) return nullptr;

	// Try as full asset path
	if (ActionRef.Contains(TEXT("/")))
	{
		FString FullPath = ActionRef;
		if (!FullPath.Contains(TEXT(".")))
		{
			FString AssetName = FPaths::GetBaseFilename(FullPath);
			FullPath = FullPath + TEXT(".") + AssetName;
		}
		UInputAction* Action = LoadObject<UInputAction>(nullptr, *FullPath);
		if (Action) return Action;
	}

	// Search asset registry by name
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();
	TArray<FAssetData> Assets;
	AR.GetAssetsByClass(UInputAction::StaticClass()->GetClassPathName(), Assets);
	for (const FAssetData& AD : Assets)
	{
		if (AD.AssetName.ToString().Equals(ActionRef, ESearchCase::IgnoreCase))
		{
			return Cast<UInputAction>(AD.GetAsset());
		}
	}

	// Try common prefixes
	static const TCHAR* Prefixes[] = { TEXT("/Game/Input/"), TEXT("/Game/Input/Actions/"), TEXT("/Game/") };
	for (const TCHAR* Prefix : Prefixes)
	{
		FString TestPath = FString(Prefix) + ActionRef + TEXT(".") + ActionRef;
		UInputAction* Action = LoadObject<UInputAction>(nullptr, *TestPath);
		if (Action) return Action;
	}

	return nullptr;
}

// Backward-compatible aliases: friendly Lua param name -> UPROPERTY name
static const TMap<FString, FString> TriggerPropAliases = {
	{ TEXT("hold_time"), TEXT("HoldTimeThreshold") },
	{ TEXT("is_one_shot"), TEXT("bIsOneShot") },
	{ TEXT("tap_time"), TEXT("TapReleaseTimeThreshold") },
	{ TEXT("trigger_on_start"), TEXT("bTriggerOnStart") },
	{ TEXT("trigger_limit"), TEXT("TriggerLimit") },
	{ TEXT("repeat_delay"), TEXT("RepeatDelay") },
	{ TEXT("number_of_taps"), TEXT("NumberOfTapsWhichTriggerRepeat") },
	{ TEXT("actuation_threshold"), TEXT("ActuationThreshold") },
	{ TEXT("affected_by_time_dilation"), TEXT("bAffectedByTimeDilation") },
	{ TEXT("interval"), TEXT("Interval") },
};

static const TArray<FString> TriggerPrefixes = { TEXT("InputTrigger") };

// Common key name aliases — maps user-friendly names to UE's actual FKey names
static const TMap<FString, FString> KeyAliases = {
	{ TEXT("Gamepad_LeftStick2D"), TEXT("Gamepad_Left2D") },
	{ TEXT("Gamepad_RightStick2D"), TEXT("Gamepad_Right2D") },
	{ TEXT("Gamepad_LeftStick"), TEXT("Gamepad_Left2D") },
	{ TEXT("Gamepad_RightStick"), TEXT("Gamepad_Right2D") },
	{ TEXT("Gamepad_LeftThumbstick2D"), TEXT("Gamepad_Left2D") },
	{ TEXT("Gamepad_RightThumbstick2D"), TEXT("Gamepad_Right2D") },
};

// Resolve an FKey from a string, applying aliases if the direct name is invalid
static FKey ResolveKey(const FString& KeyName)
{
	FKey Key{FName(*KeyName)};
	if (Key.IsValid()) return Key;

	// Try alias lookup (case-insensitive)
	for (const auto& Pair : KeyAliases)
	{
		if (Pair.Key.Equals(KeyName, ESearchCase::IgnoreCase))
		{
			return FKey{FName(*Pair.Value)};
		}
	}
	return Key; // Return the invalid key so caller can report the error
}

// Set a property on a UObject from a sol value, with alias support
static bool SetPropertyFromSolValue(UObject* Obj, const FString& Key, const sol::object& Value,
	const TMap<FString, FString>& Aliases, FString& OutError)
{
	// Resolve alias to UPROPERTY name
	FString PropName = Key;
	if (const FString* Alias = Aliases.Find(Key))
		PropName = *Alias;

	FProperty* Prop = Obj->GetClass()->FindPropertyByName(FName(*PropName));
	if (!Prop) return false; // Not a recognized property — skip silently

	void* Container = Prop->ContainerPtrToValuePtr<void>(Obj);

	if (Value.is<double>())
	{
		double NumVal = Value.as<double>();
		if (FFloatProperty* FP = CastField<FFloatProperty>(Prop)) { FP->SetPropertyValue(Container, static_cast<float>(NumVal)); return true; }
		if (FDoubleProperty* DP = CastField<FDoubleProperty>(Prop)) { DP->SetPropertyValue(Container, NumVal); return true; }
		if (FIntProperty* IP = CastField<FIntProperty>(Prop)) { IP->SetPropertyValue(Container, static_cast<int32>(NumVal)); return true; }
		if (FByteProperty* BP = CastField<FByteProperty>(Prop)) { BP->SetPropertyValue(Container, static_cast<uint8>(NumVal)); return true; }
		if (FBoolProperty* BoolP = CastField<FBoolProperty>(Prop)) { BoolP->SetPropertyValue(Container, NumVal != 0.0); return true; }
		if (FEnumProperty* EP = CastField<FEnumProperty>(Prop))
		{
			FNumericProperty* Under = EP->GetUnderlyingProperty();
			if (Under) Under->SetIntPropertyValue(Container, static_cast<int64>(NumVal));
			return true;
		}
	}
	else if (Value.is<bool>())
	{
		bool BoolVal = Value.as<bool>();
		if (FBoolProperty* BoolP = CastField<FBoolProperty>(Prop)) { BoolP->SetPropertyValue(Container, BoolVal); return true; }
	}
	else if (Value.is<std::string>())
	{
		FString StrVal = UTF8_TO_TCHAR(Value.as<std::string>().c_str());
		if (FStrProperty* SP = CastField<FStrProperty>(Prop)) { SP->SetPropertyValue(Container, StrVal); return true; }
		if (FNameProperty* NP = CastField<FNameProperty>(Prop)) { NP->SetPropertyValue(Container, FName(*StrVal)); return true; }
		// Fallback: ImportText handles enums, structs, objects
		const TCHAR* Result = Prop->ImportText_Direct(*StrVal, Container, Obj, PPF_None);
		return Result != nullptr;
	}

	return false;
}

static UInputTrigger* CreateTriggerFromTable(UObject* Outer, const sol::table& P, FString& OutError)
{
	std::string TypeStr = P.get_or<std::string>("type", "");
	if (TypeStr.empty()) { OutError = TEXT("Missing 'type' in trigger params"); return nullptr; }
	FString Type = UTF8_TO_TCHAR(TypeStr.c_str());

	// Handle "DoubleTap" alias
	if (Type.Equals(TEXT("DoubleTap"), ESearchCase::IgnoreCase)) Type = TEXT("RepeatedTap");

	// Dynamic class discovery
	UClass* TriggerClass = LuaDynamicType::FindDerivedClass(UInputTrigger::StaticClass(), Type, TriggerPrefixes);
	if (!TriggerClass)
	{
		OutError = FString::Printf(TEXT("Unknown trigger type '%s'. Valid: %s"),
			*Type, *LuaDynamicType::FormatAvailableTypes(UInputTrigger::StaticClass(), TriggerPrefixes));
		return nullptr;
	}

	UInputTrigger* Trigger = NewObject<UInputTrigger>(Outer, TriggerClass);

	// Special handling: ChordAction needs asset resolution (can't be set via reflection)
	std::string ChordRef = P.get_or<std::string>("chord_action", "");
	if (!ChordRef.empty())
	{
		if (FObjectProperty* ChordProp = CastField<FObjectProperty>(TriggerClass->FindPropertyByName(TEXT("ChordAction"))))
		{
			UInputAction* ChordAction = ResolveInputActionFromRef(UTF8_TO_TCHAR(ChordRef.c_str()));
			if (ChordAction)
			{
				ChordProp->SetObjectPropertyValue(ChordProp->ContainerPtrToValuePtr<void>(Trigger), ChordAction);
			}
			else
			{
				OutError = FString::Printf(TEXT("Could not resolve chord_action '%s'"), UTF8_TO_TCHAR(ChordRef.c_str()));
				return nullptr;
			}
		}
	}

	// Special handling: Combo needs array of asset references
	sol::optional<sol::table> ComboActions = P.get<sol::optional<sol::table>>("combo_actions");
	if (ComboActions.has_value())
	{
		FProperty* CAProp = TriggerClass->FindPropertyByName(TEXT("ComboActions"));
		if (CAProp)
		{
			FArrayProperty* ArrProp = CastField<FArrayProperty>(CAProp);
			if (ArrProp)
			{
				FScriptArrayHelper Arr(ArrProp, ArrProp->ContainerPtrToValuePtr<void>(Trigger));
				sol::table CA = ComboActions.value();
				for (auto& Pair : CA)
				{
					if (Pair.second.is<std::string>())
					{
						FString Ref = UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str());
						UInputAction* A = ResolveInputActionFromRef(Ref);
						if (A)
						{
							int32 Idx = Arr.AddValue();
							// Set the ComboStepAction field via ImportText
							FString ImportStr = FString::Printf(TEXT("(ComboStepAction=%s)"), *A->GetPathName());
							ArrProp->Inner->ImportText_Direct(*ImportStr, Arr.GetRawPtr(Idx), Trigger, PPF_None);
						}
					}
				}
			}
		}
	}

	// Generic property setting: iterate all table keys, set via reflection
	static const TSet<FString> SkipKeys = { TEXT("type"), TEXT("chord_action"), TEXT("combo_actions") };
	for (auto& Pair : P)
	{
		if (!Pair.first.is<std::string>()) continue;
		FString Key = UTF8_TO_TCHAR(Pair.first.as<std::string>().c_str());
		if (SkipKeys.Contains(Key)) continue;

		FString PropError;
		SetPropertyFromSolValue(Trigger, Key, Pair.second, TriggerPropAliases, PropError);
	}

	return Trigger;
}

// Backward-compatible aliases for modifier properties
static const TMap<FString, FString> ModifierPropAliases = {
	{ TEXT("lower_threshold"), TEXT("LowerThreshold") },
	{ TEXT("upper_threshold"), TEXT("UpperThreshold") },
	{ TEXT("dead_zone_type"), TEXT("Type") },
	{ TEXT("negate_x"), TEXT("bX") },
	{ TEXT("negate_y"), TEXT("bY") },
	{ TEXT("negate_z"), TEXT("bZ") },
	{ TEXT("scalar_x"), TEXT("Scalar.X") },
	{ TEXT("scalar_y"), TEXT("Scalar.Y") },
	{ TEXT("scalar_z"), TEXT("Scalar.Z") },
	{ TEXT("swizzle_order"), TEXT("Order") },
	{ TEXT("fov_scale"), TEXT("FOVScale") },
	{ TEXT("exponent_x"), TEXT("CurveExponent.X") },
	{ TEXT("exponent_y"), TEXT("CurveExponent.Y") },
	{ TEXT("exponent_z"), TEXT("CurveExponent.Z") },
	{ TEXT("easing_exponent"), TEXT("EasingExponent") },
	{ TEXT("speed"), TEXT("Speed") },
	{ TEXT("smoothing_method"), TEXT("SmoothingMethod") },
};

static const TArray<FString> ModifierPrefixes = { TEXT("InputModifier") };

static UInputModifier* CreateModifierFromTable(UObject* Outer, const sol::table& P, FString& OutError)
{
	std::string TypeStr = P.get_or<std::string>("type", "");
	if (TypeStr.empty()) { OutError = TEXT("Missing 'type' in modifier params"); return nullptr; }
	FString Type = UTF8_TO_TCHAR(TypeStr.c_str());

	// Handle alias: "Swizzle" -> "SwizzleAxis"
	if (Type.Equals(TEXT("Swizzle"), ESearchCase::IgnoreCase)) Type = TEXT("SwizzleAxis");

	// Dynamic class discovery
	UClass* ModifierClass = LuaDynamicType::FindDerivedClass(UInputModifier::StaticClass(), Type, ModifierPrefixes);
	if (!ModifierClass)
	{
		OutError = FString::Printf(TEXT("Unknown modifier type '%s'. Valid: %s"),
			*Type, *LuaDynamicType::FormatAvailableTypes(UInputModifier::StaticClass(), ModifierPrefixes));
		return nullptr;
	}

	UInputModifier* Modifier = NewObject<UInputModifier>(Outer, ModifierClass);

	// Negate fix: if any negate_ param is specified, default all axes to false first
	// (CDO defaults bX/bY/bZ to true, which is unintuitive when specifying individual axes)
	if (P.get<sol::optional<bool>>("negate_x").has_value() ||
		P.get<sol::optional<bool>>("negate_y").has_value() ||
		P.get<sol::optional<bool>>("negate_z").has_value())
	{
		if (FBoolProperty* BX = CastField<FBoolProperty>(ModifierClass->FindPropertyByName(TEXT("bX"))))
			BX->SetPropertyValue_InContainer(Modifier, false);
		if (FBoolProperty* BY = CastField<FBoolProperty>(ModifierClass->FindPropertyByName(TEXT("bY"))))
			BY->SetPropertyValue_InContainer(Modifier, false);
		if (FBoolProperty* BZ = CastField<FBoolProperty>(ModifierClass->FindPropertyByName(TEXT("bZ"))))
			BZ->SetPropertyValue_InContainer(Modifier, false);
	}

	// Generic property setting via reflection
	static const TSet<FString> SkipKeys = { TEXT("type") };
	for (auto& Pair : P)
	{
		if (!Pair.first.is<std::string>()) continue;
		FString Key = UTF8_TO_TCHAR(Pair.first.as<std::string>().c_str());
		if (SkipKeys.Contains(Key)) continue;

		// Resolve alias
		FString PropName = Key;
		if (const FString* Alias = ModifierPropAliases.Find(Key))
			PropName = *Alias;

		// Handle nested struct paths (e.g., "Scalar.X")
		if (PropName.Contains(TEXT(".")))
		{
			// Split into struct name + field name
			FString StructName, FieldName;
			PropName.Split(TEXT("."), &StructName, &FieldName);

			FProperty* StructProp = Modifier->GetClass()->FindPropertyByName(FName(*StructName));
			if (StructProp)
			{
				void* StructPtr = StructProp->ContainerPtrToValuePtr<void>(Modifier);
				if (FStructProperty* SP = CastField<FStructProperty>(StructProp))
				{
					FProperty* InnerProp = SP->Struct->FindPropertyByName(FName(*FieldName));
					if (InnerProp && Pair.second.is<double>())
					{
						void* InnerPtr = InnerProp->ContainerPtrToValuePtr<void>(StructPtr);
						if (FDoubleProperty* DP = CastField<FDoubleProperty>(InnerProp))
							DP->SetPropertyValue(InnerPtr, Pair.second.as<double>());
						else if (FFloatProperty* FP = CastField<FFloatProperty>(InnerProp))
							FP->SetPropertyValue(InnerPtr, static_cast<float>(Pair.second.as<double>()));
					}
				}
			}
			continue;
		}

		FString PropError;
		SetPropertyFromSolValue(Modifier, Key, Pair.second, ModifierPropAliases, PropError);
	}

	return Modifier;
}

static FString GetTriggerTypeName(UInputTrigger* T)
{
	if (!T) return TEXT("null");
	FString Name = T->GetClass()->GetName();
	Name.RemoveFromStart(TEXT("InputTrigger"));
	return Name;
}

static FString GetModifierTypeName(UInputModifier* M)
{
	if (!M) return TEXT("null");
	FString Name = M->GetClass()->GetName();
	Name.RemoveFromStart(TEXT("InputModifier"));
	return Name;
}

static sol::table TriggerToTable(sol::state_view& Lua, UInputTrigger* T, int32 Index)
{
	sol::table E = Lua.create_table();
	E["index"] = Index;
	E["type"] = TCHAR_TO_UTF8(*GetTriggerTypeName(T));
	if (!T) return E;
	E["actuation_threshold"] = T->ActuationThreshold;

	if (UInputTriggerHold* Hold = Cast<UInputTriggerHold>(T))
	{
		E["hold_time"] = Hold->HoldTimeThreshold;
		E["is_one_shot"] = Hold->bIsOneShot;
	}
	else if (UInputTriggerHoldAndRelease* HR = Cast<UInputTriggerHoldAndRelease>(T))
	{
		E["hold_time"] = HR->HoldTimeThreshold;
	}
	else if (UInputTriggerTap* Tap = Cast<UInputTriggerTap>(T))
	{
		E["tap_time"] = Tap->TapReleaseTimeThreshold;
	}
	else if (UInputTriggerPulse* Pulse = Cast<UInputTriggerPulse>(T))
	{
		E["interval"] = Pulse->Interval;
		E["trigger_on_start"] = Pulse->bTriggerOnStart;
		E["trigger_limit"] = Pulse->TriggerLimit;
	}
	else if (UInputTriggerChordAction* Chord = Cast<UInputTriggerChordAction>(T))
	{
		E["chord_action"] = Chord->ChordAction ? TCHAR_TO_UTF8(*Chord->ChordAction->GetName()) : "none";
	}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	else if (UInputTriggerRepeatedTap* RepTap = Cast<UInputTriggerRepeatedTap>(T))
	{
		// Protected members — access via reflection (all are UPROPERTY)
		if (FProperty* P = RepTap->GetClass()->FindPropertyByName(TEXT("RepeatDelay")))
			E["repeat_delay"] = *P->ContainerPtrToValuePtr<double>(RepTap);
		if (FProperty* P = RepTap->GetClass()->FindPropertyByName(TEXT("NumberOfTapsWhichTriggerRepeat")))
			E["number_of_taps"] = *P->ContainerPtrToValuePtr<int32>(RepTap);
		if (FProperty* P = RepTap->GetClass()->FindPropertyByName(TEXT("TapReleaseTimeThreshold")))
			E["tap_time"] = *P->ContainerPtrToValuePtr<float>(RepTap);
	}
#endif
	else if (UInputTriggerCombo* Combo = Cast<UInputTriggerCombo>(T))
	{
		E["combo_steps"] = static_cast<int>(Combo->ComboActions.Num());
	}
	return E;
}

static sol::table ModifierToTable(sol::state_view& Lua, UInputModifier* M, int32 Index)
{
	sol::table E = Lua.create_table();
	E["index"] = Index;
	E["type"] = TCHAR_TO_UTF8(*GetModifierTypeName(M));
	if (!M) return E;

	if (UInputModifierDeadZone* DZ = Cast<UInputModifierDeadZone>(M))
	{
		E["lower_threshold"] = DZ->LowerThreshold;
		E["upper_threshold"] = DZ->UpperThreshold;
		E["dead_zone_type"] = (DZ->Type == EDeadZoneType::Axial) ? "Axial"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			: (DZ->Type == EDeadZoneType::UnscaledRadial) ? "UnscaledRadial"
#endif
			: "Radial";
	}
	else if (UInputModifierNegate* Neg = Cast<UInputModifierNegate>(M))
	{
		E["negate_x"] = Neg->bX;
		E["negate_y"] = Neg->bY;
		E["negate_z"] = Neg->bZ;
	}
	else if (UInputModifierScalar* Sc = Cast<UInputModifierScalar>(M))
	{
		E["scalar_x"] = Sc->Scalar.X;
		E["scalar_y"] = Sc->Scalar.Y;
		E["scalar_z"] = Sc->Scalar.Z;
	}
	else if (UInputModifierSwizzleAxis* Sw = Cast<UInputModifierSwizzleAxis>(M))
	{
		switch (Sw->Order)
		{
		case EInputAxisSwizzle::YXZ: E["swizzle_order"] = "YXZ"; break;
		case EInputAxisSwizzle::ZYX: E["swizzle_order"] = "ZYX"; break;
		case EInputAxisSwizzle::XZY: E["swizzle_order"] = "XZY"; break;
		case EInputAxisSwizzle::YZX: E["swizzle_order"] = "YZX"; break;
		case EInputAxisSwizzle::ZXY: E["swizzle_order"] = "ZXY"; break;
		default: E["swizzle_order"] = "YXZ"; break;
		}
	}
	else if (UInputModifierFOVScaling* FOV = Cast<UInputModifierFOVScaling>(M))
	{
		E["fov_scale"] = FOV->FOVScale;
	}
	else if (UInputModifierResponseCurveExponential* Exp = Cast<UInputModifierResponseCurveExponential>(M))
	{
		E["exponent_x"] = Exp->CurveExponent.X;
		E["exponent_y"] = Exp->CurveExponent.Y;
		E["exponent_z"] = Exp->CurveExponent.Z;
	}
	else if (UInputModifierSmoothDelta* SD = Cast<UInputModifierSmoothDelta>(M))
	{
		E["speed"] = SD->Speed;
		E["easing_exponent"] = SD->EasingExponent;
		switch (SD->SmoothingMethod)
		{
		case ENormalizeInputSmoothingType::Lerp: E["smoothing_method"] = "Lerp"; break;
		case ENormalizeInputSmoothingType::Interp_To: E["smoothing_method"] = "InterpTo"; break;
		case ENormalizeInputSmoothingType::Interp_Constant_To: E["smoothing_method"] = "InterpConstantTo"; break;
		case ENormalizeInputSmoothingType::Interp_Ease_In: E["smoothing_method"] = "EaseIn"; break;
		case ENormalizeInputSmoothingType::Interp_Ease_Out: E["smoothing_method"] = "EaseOut"; break;
		case ENormalizeInputSmoothingType::Interp_Ease_In_Out: E["smoothing_method"] = "EaseInOut"; break;
		default: E["smoothing_method"] = "Other"; break;
		}
	}
	return E;
}

// ============================================================================
// Reflection access to protected members & rebuild helper
// ============================================================================

// Get mutable access to the mappings array via reflection (protected member)
static TArray<FEnhancedActionKeyMapping>* GetMutableMappings(UInputMappingContext* Context)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	FProperty* Prop = Context->GetClass()->FindPropertyByName(TEXT("DefaultKeyMappings"));
	if (!Prop) return nullptr;
	FStructProperty* SP = CastField<FStructProperty>(Prop);
	if (!SP) return nullptr;
	FInputMappingContextMappingData* Data = SP->ContainerPtrToValuePtr<FInputMappingContextMappingData>(Context);
	if (!Data) return nullptr;
	return &Data->Mappings;
#else
	FProperty* Prop = Context->GetClass()->FindPropertyByName(TEXT("Mappings"));
	if (!Prop) return nullptr;
	FArrayProperty* AP = CastField<FArrayProperty>(Prop);
	if (!AP) return nullptr;
	return AP->ContainerPtrToValuePtr<TArray<FEnhancedActionKeyMapping>>(Context);
#endif
}

// Notify the Enhanced Input subsystem that mappings changed (mirrors what MapKey/UnmapKey do internally)
static void RequestRebuildMappings(const UInputMappingContext* Context)
{
	if (IEnhancedInputModule::IsAvailable())
	{
		IEnhancedInputModule::Get().GetLibrary()->RequestRebuildControlMappingsUsingContext(Context);
	}
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
static TMap<FString, FInputMappingContextMappingData>* GetMappingProfileOverrides(UInputMappingContext* Context)
{
	FProperty* Prop = Context->GetClass()->FindPropertyByName(TEXT("MappingProfileOverrides"));
	if (!Prop) return nullptr;
	FMapProperty* MapProp = CastField<FMapProperty>(Prop);
	if (!MapProp) return nullptr;
	return MapProp->ContainerPtrToValuePtr<TMap<FString, FInputMappingContextMappingData>>(Context);
}
#endif

static FString ValueTypeToString(EInputActionValueType VT)
{
	switch (VT)
	{
	case EInputActionValueType::Boolean: return TEXT("Boolean");
	case EInputActionValueType::Axis1D:  return TEXT("Axis1D");
	case EInputActionValueType::Axis2D:  return TEXT("Axis2D");
	case EInputActionValueType::Axis3D:  return TEXT("Axis3D");
	default: return TEXT("Unknown");
	}
}

// ============================================================================
// Registration
// ============================================================================

static TArray<FLuaFunctionDoc> EnhancedInputDocs = {};

static void BindEnhancedInput(sol::state& Lua, FLuaSessionData& Session)
{
	// ==================================================================
	// InputAction enrichment
	// ==================================================================
	Lua.set_function("_enrich_input_action", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UInputAction* Action = LoadObject<UInputAction>(nullptr, *FPath);
		if (!Action) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"Element types for add/remove/list:\n"
			"  trigger  — input trigger (dynamically discovered — use list(\"trigger_types\") to see available)\n"
			"  modifier — input modifier (dynamically discovered — use list(\"modifier_types\") to see available)\n"
			"\n"
			"add(type, params):\n"
			"  add(\"trigger\", {type=\"Hold\", hold_time=1.0, is_one_shot=true})\n"
			"  add(\"modifier\", {type=\"DeadZone\", lower_threshold=0.2, upper_threshold=1.0})\n"
			"\n"
			"remove(type, index):\n"
			"  remove(\"trigger\", 1)   — 1-based Lua index\n"
			"  remove(\"modifier\", 1)\n"
			"\n"
			"list(type):\n"
			"  list(\"triggers\"), list(\"modifiers\")\n"
			"\n"
			"configure(params):\n"
			"  configure({value_type=\"Axis2D\", consume_input=true, trigger_when_paused=false, accumulation=\"Cumulative\",\n"
			"    value_type aliases: Boolean/Bool/Digital, Axis1D/Float/Scalar, Axis2D/Vector2D, Axis3D/Vector\n"
			"    description=\"Jump action\", consume_legacy=false, reserve_all_mappings=false})\n"
			"\n"
			"info() — value_type, consume_input, trigger_when_paused, description, num_triggers, num_modifiers\n";

		// ---- add(type, params) ----
		AssetObj.set_function("add", [Action, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("trigger"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"trigger\") -> params required: {type=\"Hold\", ...}")); return sol::lua_nil; }

				FString Error;
				UInputTrigger* Trigger = CreateTriggerFromTable(Action, Params.value(), Error);
				if (!Trigger) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"trigger\") -> %s"), *Error)); return sol::lua_nil; }

				const FScopedTransaction Txn(NSLOCTEXT("AgentIntegrationKit", "LuaAddTrigger", "Lua: Add Trigger"));
				Action->Modify();
				Action->Triggers.Add(Trigger);
				FirePostEditProperty(Action, GET_MEMBER_NAME_CHECKED(UInputAction, Triggers));
				Action->GetPackage()->MarkPackageDirty();

				FString TName = GetTriggerTypeName(Trigger);
				Session.Log(FString::Printf(TEXT("[OK] add(\"trigger\", type=\"%s\") -> index %d"), *TName, Action->Triggers.Num()));
				return sol::make_object(Lua, Action->Triggers.Num()); // 1-based
			}
			else if (FType.Equals(TEXT("modifier"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"modifier\") -> params required: {type=\"DeadZone\", ...}")); return sol::lua_nil; }

				FString Error;
				UInputModifier* Modifier = CreateModifierFromTable(Action, Params.value(), Error);
				if (!Modifier) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"modifier\") -> %s"), *Error)); return sol::lua_nil; }

				const FScopedTransaction Txn(NSLOCTEXT("AgentIntegrationKit", "LuaAddModifier", "Lua: Add Modifier"));
				Action->Modify();
				Action->Modifiers.Add(Modifier);
				FirePostEditProperty(Action, GET_MEMBER_NAME_CHECKED(UInputAction, Modifiers));
				Action->GetPackage()->MarkPackageDirty();

				FString MName = GetModifierTypeName(Modifier);
				Session.Log(FString::Printf(TEXT("[OK] add(\"modifier\", type=\"%s\") -> index %d"), *MName, Action->Modifiers.Num()));
				return sol::make_object(Lua, Action->Modifiers.Num()); // 1-based
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: trigger, modifier"), *FType));
			return sol::lua_nil;
		});

		// ---- remove(type, index) ----
		AssetObj.set_function("remove", [Action, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("trigger"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<int>()) { Session.Log(TEXT("[FAIL] remove(\"trigger\") -> 1-based index required")); return sol::lua_nil; }
				int32 LuaIdx = Id.as<int>();
				int32 Idx = LuaIdx - 1; // Convert to 0-based
				if (Idx < 0 || Idx >= Action->Triggers.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"trigger\", %d) -> out of range (1-%d)"), LuaIdx, Action->Triggers.Num()));
					return sol::lua_nil;
				}

				const FScopedTransaction Txn(NSLOCTEXT("AgentIntegrationKit", "LuaRemoveTrigger", "Lua: Remove Trigger"));
				Action->Modify();
				FString TName = GetTriggerTypeName(Action->Triggers[Idx]);
				Action->Triggers.RemoveAt(Idx);
				FirePostEditProperty(Action, GET_MEMBER_NAME_CHECKED(UInputAction, Triggers));
				Action->GetPackage()->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"trigger\", %d) -> removed %s"), LuaIdx, *TName));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("modifier"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<int>()) { Session.Log(TEXT("[FAIL] remove(\"modifier\") -> 1-based index required")); return sol::lua_nil; }
				int32 LuaIdx = Id.as<int>();
				int32 Idx = LuaIdx - 1;
				if (Idx < 0 || Idx >= Action->Modifiers.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"modifier\", %d) -> out of range (1-%d)"), LuaIdx, Action->Modifiers.Num()));
					return sol::lua_nil;
				}

				const FScopedTransaction Txn(NSLOCTEXT("AgentIntegrationKit", "LuaRemoveModifier", "Lua: Remove Modifier"));
				Action->Modify();
				FString MName = GetModifierTypeName(Action->Modifiers[Idx]);
				Action->Modifiers.RemoveAt(Idx);
				FirePostEditProperty(Action, GET_MEMBER_NAME_CHECKED(UInputAction, Modifiers));
				Action->GetPackage()->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"modifier\", %d) -> removed %s"), LuaIdx, *MName));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: trigger, modifier"), *FType));
			return sol::lua_nil;
		});

		// ---- list(type?) ----
		AssetObj.set_function("list", [Action, &Session](sol::table self,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = self["info"];
				if (InfoFn.valid()) return InfoFn(self);
				return sol::lua_nil;
			}

			if (FType.Equals(TEXT("triggers"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("trigger"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Action->Triggers.Num(); i++)
				{
					Result[i + 1] = TriggerToTable(Lua, Action->Triggers[i], i + 1);
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"triggers\") -> %d"), Action->Triggers.Num()));
				return Result;
			}

			if (FType.Equals(TEXT("modifiers"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("modifier"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Action->Modifiers.Num(); i++)
				{
					Result[i + 1] = ModifierToTable(Lua, Action->Modifiers[i], i + 1);
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"modifiers\") -> %d"), Action->Modifiers.Num()));
				return Result;
			}

			if (FType.Equals(TEXT("trigger_types"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				const TArray<FString>& Names = LuaDynamicType::ListDerivedTypeNames(UInputTrigger::StaticClass(), TriggerPrefixes);
				for (int32 i = 0; i < Names.Num(); i++) Result[i + 1] = TCHAR_TO_UTF8(*Names[i]);
				Session.Log(FString::Printf(TEXT("[OK] list(\"trigger_types\") -> %d"), Names.Num()));
				return Result;
			}

			if (FType.Equals(TEXT("modifier_types"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				const TArray<FString>& Names = LuaDynamicType::ListDerivedTypeNames(UInputModifier::StaticClass(), ModifierPrefixes);
				for (int32 i = 0; i < Names.Num(); i++) Result[i + 1] = TCHAR_TO_UTF8(*Names[i]);
				Session.Log(FString::Printf(TEXT("[OK] list(\"modifier_types\") -> %d"), Names.Num()));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: triggers, modifiers, trigger_types, modifier_types"), *FType));
			return sol::lua_nil;
		});

		// ---- configure(params) ----
		AssetObj.set_function("configure", [Action, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			int32 Changes = 0;

			const FScopedTransaction Txn(NSLOCTEXT("AgentIntegrationKit", "LuaConfigureAction", "Lua: Configure InputAction"));
			Action->Modify();

			// value_type
			std::string VTStr = Params.get_or<std::string>("value_type", "");
			if (!VTStr.empty())
			{
				FString VT = UTF8_TO_TCHAR(VTStr.c_str());
				EInputActionValueType NewType = Action->ValueType;
				if (VT.Equals(TEXT("Boolean"), ESearchCase::IgnoreCase) ||
					VT.Equals(TEXT("Bool"), ESearchCase::IgnoreCase) ||
					VT.Equals(TEXT("Digital"), ESearchCase::IgnoreCase))
					NewType = EInputActionValueType::Boolean;
				else if (VT.Equals(TEXT("Axis1D"), ESearchCase::IgnoreCase) ||
					VT.Equals(TEXT("Float"), ESearchCase::IgnoreCase) ||
					VT.Equals(TEXT("Scalar"), ESearchCase::IgnoreCase))
					NewType = EInputActionValueType::Axis1D;
				else if (VT.Equals(TEXT("Axis2D"), ESearchCase::IgnoreCase) ||
					VT.Equals(TEXT("Vector2D"), ESearchCase::IgnoreCase))
					NewType = EInputActionValueType::Axis2D;
				else if (VT.Equals(TEXT("Axis3D"), ESearchCase::IgnoreCase) ||
					VT.Equals(TEXT("Vector"), ESearchCase::IgnoreCase))
					NewType = EInputActionValueType::Axis3D;
				else { Session.Log(FString::Printf(TEXT("[FAIL] configure -> unknown value_type '%s'. Valid: Boolean/Bool/Digital, Axis1D/Float, Axis2D/Vector2D, Axis3D/Vector"), *VT)); }

				if (NewType != Action->ValueType)
				{
					Action->ValueType = NewType;
					FirePostEditProperty(Action, GET_MEMBER_NAME_CHECKED(UInputAction, ValueType));
					Changes++;
				}
			}

			// consume_input
			sol::optional<bool> Consume = Params.get<sol::optional<bool>>("consume_input");
			if (Consume.has_value()) { Action->bConsumeInput = Consume.value(); Changes++; }

			// trigger_when_paused
			sol::optional<bool> Paused = Params.get<sol::optional<bool>>("trigger_when_paused");
			if (Paused.has_value()) { Action->bTriggerWhenPaused = Paused.value(); Changes++; }

			// accumulation
			std::string AccStr = Params.get_or<std::string>("accumulation", "");
			if (!AccStr.empty())
			{
				FString Acc = UTF8_TO_TCHAR(AccStr.c_str());
				if (Acc.Equals(TEXT("TakeHighestAbsoluteValue"), ESearchCase::IgnoreCase))
				{
					Action->AccumulationBehavior = EInputActionAccumulationBehavior::TakeHighestAbsoluteValue;
					Changes++;
				}
				else if (Acc.Equals(TEXT("Cumulative"), ESearchCase::IgnoreCase))
				{
					Action->AccumulationBehavior = EInputActionAccumulationBehavior::Cumulative;
					Changes++;
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure -> unknown accumulation '%s'. Valid: TakeHighestAbsoluteValue, Cumulative"), *Acc));
				}
			}

			// description
			std::string DescStr = Params.get_or<std::string>("description", "");
			if (!DescStr.empty())
			{
				Action->ActionDescription = FText::FromString(UTF8_TO_TCHAR(DescStr.c_str()));
				Changes++;
			}

			// consume_legacy (bConsumesActionAndAxisMappings)
			sol::optional<bool> ConsumeLegacy = Params.get<sol::optional<bool>>("consume_legacy");
			if (ConsumeLegacy.has_value()) { Action->bConsumesActionAndAxisMappings = ConsumeLegacy.value(); Changes++; }

			// reserve_all_mappings
			sol::optional<bool> Reserve = Params.get<sol::optional<bool>>("reserve_all_mappings");
			if (Reserve.has_value()) { Action->bReserveAllMappings = Reserve.value(); Changes++; }

			if (Changes > 0)
			{
				Action->GetPackage()->MarkPackageDirty();
			}

			Session.Log(FString::Printf(TEXT("[OK] configure() -> %d change(s) on %s"), Changes, *Action->GetName()));
			return sol::make_object(Lua, Changes);
		});

		// ---- info() ----
		AssetObj.set_function("info", [Action, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();
			Result["name"] = TCHAR_TO_UTF8(*Action->GetName());
			Result["value_type"] = TCHAR_TO_UTF8(*ValueTypeToString(Action->ValueType));
			Result["consume_input"] = Action->bConsumeInput;
			Result["trigger_when_paused"] = Action->bTriggerWhenPaused;
			Result["num_triggers"] = static_cast<int>(Action->Triggers.Num());
			Result["num_modifiers"] = static_cast<int>(Action->Modifiers.Num());
			Result["accumulation"] = (Action->AccumulationBehavior == EInputActionAccumulationBehavior::Cumulative)
				? "Cumulative" : "TakeHighestAbsoluteValue";
			Result["description"] = TCHAR_TO_UTF8(*Action->ActionDescription.ToString());
			Result["consume_legacy"] = Action->bConsumesActionAndAxisMappings;
			Result["reserve_all_mappings"] = Action->bReserveAllMappings;

			Session.Log(FString::Printf(TEXT("[OK] info() -> %s, %s, %d triggers, %d modifiers"),
				*Action->GetName(), *ValueTypeToString(Action->ValueType),
				Action->Triggers.Num(), Action->Modifiers.Num()));
			return Result;
		});
	});

	// ==================================================================
	// InputMappingContext enrichment
	// ==================================================================
	Lua.set_function("_enrich_mapping_context", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UInputMappingContext* Context = LoadObject<UInputMappingContext>(nullptr, *FPath);
		if (!Context) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"Element types for add/remove/list/configure:\n"
			"  mapping          — action+key binding with per-mapping triggers and modifiers\n"
			"  all_mappings     — remove all mappings at once\n"
			"  all_for_action   — remove all mappings for a specific action\n"
			"  profile_override — mapping profile override (alternative mappings for remappable key profiles)\n"
			"\n"
			"add(type, params):\n"
			"  add(\"mapping\", {action=\"IA_Jump\", key=\"SpaceBar\",\n"
			"    triggers={{type=\"Pressed\"}}, modifiers={{type=\"Negate\", negate_y=true}}})\n"
			"  add(\"profile_override\", {profile=\"Lefty\", action=\"IA_Move\", key=\"IJKL\",\n"
			"    triggers={{type=\"Down\"}}, modifiers={{type=\"Negate\"}}})\n"
			"\n"
			"remove(type, id):\n"
			"  remove(\"mapping\", 1)  — by 1-based index (stable order, exact index)\n"
			"  remove(\"mapping\", {action=\"IA_Jump\", key=\"SpaceBar\"})  — by action+key\n"
			"  remove(\"all_mappings\")  — clear all mappings\n"
			"  remove(\"all_for_action\", \"IA_Jump\")  — remove all mappings for an action\n"
			"  remove(\"profile_override\", {profile=\"Lefty\"})  — remove entire profile\n"
			"  remove(\"profile_override\", {profile=\"Lefty\", index=1})  — remove single mapping from profile\n"
			"\n"
			"configure(\"mapping\", index, params):\n"
			"  configure(\"mapping\", 1, {key=\"W\", add_triggers={{type=\"Hold\"}}, remove_triggers={1}})\n"
			"\n"
			"list(type):\n"
			"  list(\"mappings\"), list(\"mapping_details\", \"1\"),\n"
			"  list(\"profiles\"), list(\"profile_override\", \"Lefty\"),\n"
			"  list(\"trigger_types\"), list(\"modifier_types\")\n"
			"\n"
			"info() — description, num_mappings, profiles, action->key summary\n";

		// ---- add(type, params) ----
		AssetObj.set_function("add", [Context, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("mapping"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"mapping\") -> params required: {action=.., key=..}")); return sol::lua_nil; }
				sol::table P = Params.value();

				// Resolve action
				std::string ActionRef = P.get_or<std::string>("action", "");
				if (ActionRef.empty()) { Session.Log(TEXT("[FAIL] add(\"mapping\") -> 'action' required")); return sol::lua_nil; }

				UInputAction* Action = ResolveInputActionFromRef(UTF8_TO_TCHAR(ActionRef.c_str()));
				if (!Action)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"mapping\") -> could not resolve action '%s'"), UTF8_TO_TCHAR(ActionRef.c_str())));
					return sol::lua_nil;
				}

				// Resolve key
				std::string KeyStr = P.get_or<std::string>("key", "");
				if (KeyStr.empty()) { Session.Log(TEXT("[FAIL] add(\"mapping\") -> 'key' required")); return sol::lua_nil; }

				FKey Key = ResolveKey(UTF8_TO_TCHAR(KeyStr.c_str()));
				if (!Key.IsValid())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"mapping\") -> invalid key '%s'"), UTF8_TO_TCHAR(KeyStr.c_str())));
					return sol::lua_nil;
				}

				const FScopedTransaction Txn(NSLOCTEXT("AgentIntegrationKit", "LuaAddMapping", "Lua: Add Mapping"));
				Context->Modify();

				// MapKey adds the mapping and triggers internal rebuild
				FEnhancedActionKeyMapping& NewMapping = Context->MapKey(Action, Key);

				// Per-mapping triggers
				sol::optional<sol::table> TriggersOpt = P.get<sol::optional<sol::table>>("triggers");
				if (TriggersOpt.has_value())
				{
					sol::table TrigTable = TriggersOpt.value();
					for (auto& Pair : TrigTable)
					{
						if (Pair.second.is<sol::table>())
						{
							FString Error;
							UInputTrigger* Trig = CreateTriggerFromTable(Context, Pair.second.as<sol::table>(), Error);
							if (Trig) NewMapping.Triggers.Add(Trig);
							else Session.Log(FString::Printf(TEXT("[WARN] add(\"mapping\") trigger: %s"), *Error));
						}
					}
				}

				// Per-mapping modifiers
				sol::optional<sol::table> ModifiersOpt = P.get<sol::optional<sol::table>>("modifiers");
				if (ModifiersOpt.has_value())
				{
					sol::table ModTable = ModifiersOpt.value();
					for (auto& Pair : ModTable)
					{
						if (Pair.second.is<sol::table>())
						{
							FString Error;
							UInputModifier* Mod = CreateModifierFromTable(Context, Pair.second.as<sol::table>(), Error);
							if (Mod) NewMapping.Modifiers.Add(Mod);
							else Session.Log(FString::Printf(TEXT("[WARN] add(\"mapping\") modifier: %s"), *Error));
						}
					}
				}

				// MapKey already called RequestRebuild, but that was before triggers/modifiers were added.
				// Re-request rebuild so the subsystem picks up the per-mapping triggers/modifiers.
				if (NewMapping.Triggers.Num() > 0 || NewMapping.Modifiers.Num() > 0)
				{
					RequestRebuildMappings(Context);
				}
				Context->GetPackage()->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add(\"mapping\", action=\"%s\", key=\"%s\") -> triggers=%d, modifiers=%d"),
					*Action->GetName(), UTF8_TO_TCHAR(KeyStr.c_str()),
					NewMapping.Triggers.Num(), NewMapping.Modifiers.Num()));
				return sol::make_object(Lua, static_cast<int>(Context->GetMappings().Num())); // 1-based count = index of new
			}
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		else if (FType.Equals(TEXT("profile_override"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"profile_override\") -> params required: {profile=.., action=.., key=..}")); return sol::lua_nil; }
				sol::table P = Params.value();

				std::string ProfileId = P.get_or<std::string>("profile", "");
				if (ProfileId.empty()) { Session.Log(TEXT("[FAIL] add(\"profile_override\") -> 'profile' required")); return sol::lua_nil; }

				std::string ActionRef = P.get_or<std::string>("action", "");
				if (ActionRef.empty()) { Session.Log(TEXT("[FAIL] add(\"profile_override\") -> 'action' required")); return sol::lua_nil; }

				UInputAction* Action = ResolveInputActionFromRef(UTF8_TO_TCHAR(ActionRef.c_str()));
				if (!Action)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"profile_override\") -> could not resolve action '%s'"), UTF8_TO_TCHAR(ActionRef.c_str())));
					return sol::lua_nil;
				}

				std::string KeyStr = P.get_or<std::string>("key", "");
				if (KeyStr.empty()) { Session.Log(TEXT("[FAIL] add(\"profile_override\") -> 'key' required")); return sol::lua_nil; }

				FKey Key = ResolveKey(UTF8_TO_TCHAR(KeyStr.c_str()));
				if (!Key.IsValid())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"profile_override\") -> invalid key '%s'"), UTF8_TO_TCHAR(KeyStr.c_str())));
					return sol::lua_nil;
				}

				TMap<FString, FInputMappingContextMappingData>* Overrides = GetMappingProfileOverrides(Context);
				if (!Overrides)
				{
					Session.Log(TEXT("[FAIL] add(\"profile_override\") -> could not access MappingProfileOverrides"));
					return sol::lua_nil;
				}

				const FScopedTransaction Txn(NSLOCTEXT("AgentIntegrationKit", "LuaAddProfileOverride", "Lua: Add Profile Override Mapping"));
				Context->Modify();

				FString FProfileId = UTF8_TO_TCHAR(ProfileId.c_str());
				FInputMappingContextMappingData& ProfileData = Overrides->FindOrAdd(FProfileId);

				FEnhancedActionKeyMapping NewMapping;
				NewMapping.Action = Action;
				NewMapping.Key = Key;

				// Per-mapping triggers
				sol::optional<sol::table> TriggersOpt = P.get<sol::optional<sol::table>>("triggers");
				if (TriggersOpt.has_value())
				{
					for (auto& Pair : TriggersOpt.value())
					{
						if (Pair.second.is<sol::table>())
						{
							FString Error;
							UInputTrigger* Trig = CreateTriggerFromTable(Context, Pair.second.as<sol::table>(), Error);
							if (Trig) NewMapping.Triggers.Add(Trig);
							else Session.Log(FString::Printf(TEXT("[WARN] add(\"profile_override\") trigger: %s"), *Error));
						}
					}
				}

				// Per-mapping modifiers
				sol::optional<sol::table> ModifiersOpt = P.get<sol::optional<sol::table>>("modifiers");
				if (ModifiersOpt.has_value())
				{
					for (auto& Pair : ModifiersOpt.value())
					{
						if (Pair.second.is<sol::table>())
						{
							FString Error;
							UInputModifier* Mod = CreateModifierFromTable(Context, Pair.second.as<sol::table>(), Error);
							if (Mod) NewMapping.Modifiers.Add(Mod);
							else Session.Log(FString::Printf(TEXT("[WARN] add(\"profile_override\") modifier: %s"), *Error));
						}
					}
				}

				ProfileData.Mappings.Add(NewMapping);
				Context->PostEditChange();
				Context->GetPackage()->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add(\"profile_override\", profile=\"%s\", action=\"%s\", key=\"%s\") -> index %d"),
					*FProfileId, *Action->GetName(), UTF8_TO_TCHAR(KeyStr.c_str()), ProfileData.Mappings.Num()));
				return sol::make_object(Lua, static_cast<int>(ProfileData.Mappings.Num()));
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: mapping, profile_override"), *FType));
#else
			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: mapping"), *FType));
#endif
			return sol::lua_nil;
		});

		// ---- remove(type, id?) ----
		AssetObj.set_function("remove", [Context, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("mapping"), ESearchCase::IgnoreCase))
			{
				const TArray<FEnhancedActionKeyMapping>& Mappings = Context->GetMappings();

				// By 1-based index — use direct array removal (stable order, exact index)
				// UnmapKey finds by Action+Key and uses RemoveAtSwap, which can remove the wrong
				// mapping when duplicates exist and reorders the array unpredictably.
				if (Id.is<int>())
				{
					int32 LuaIdx = Id.as<int>();
					int32 Idx = LuaIdx - 1;
					if (Idx < 0 || Idx >= Mappings.Num())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] remove(\"mapping\", %d) -> out of range (1-%d)"), LuaIdx, Mappings.Num()));
						return sol::lua_nil;
					}

					FString ActionName = Mappings[Idx].Action ? Mappings[Idx].Action->GetName() : TEXT("(none)");
					FString KeyName = Mappings[Idx].Key.GetFName().ToString();

					const FScopedTransaction Txn(NSLOCTEXT("AgentIntegrationKit", "LuaRemoveMapping", "Lua: Remove Mapping"));
					Context->Modify();

					// Access DefaultKeyMappings.Mappings directly for stable RemoveAt at exact index
					TArray<FEnhancedActionKeyMapping>* MutableMappings = GetMutableMappings(Context);
					if (MutableMappings)
					{
						MutableMappings->RemoveAt(Idx);
						RequestRebuildMappings(Context);
					}
					else
					{
						// Fallback to UnmapKey if reflection fails
						Context->UnmapKey(Mappings[Idx].Action.Get(), Mappings[Idx].Key);
					}
					Context->GetPackage()->MarkPackageDirty();

					Session.Log(FString::Printf(TEXT("[OK] remove(\"mapping\", %d) -> removed %s+%s"), LuaIdx, *ActionName, *KeyName));
					return sol::make_object(Lua, true);
				}

				// By {action, key} table
				if (Id.is<sol::table>())
				{
					sol::table IdTable = Id.as<sol::table>();
					std::string ActionRef = IdTable.get_or<std::string>("action", "");
					std::string KeyStr = IdTable.get_or<std::string>("key", "");

					if (ActionRef.empty() || KeyStr.empty())
					{
						Session.Log(TEXT("[FAIL] remove(\"mapping\", {}) -> 'action' and 'key' required"));
						return sol::lua_nil;
					}

					UInputAction* Action = ResolveInputActionFromRef(UTF8_TO_TCHAR(ActionRef.c_str()));
					if (!Action)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] remove(\"mapping\") -> could not resolve action '%s'"), UTF8_TO_TCHAR(ActionRef.c_str())));
						return sol::lua_nil;
					}

					FKey Key = ResolveKey(UTF8_TO_TCHAR(KeyStr.c_str()));

					// Verify mapping exists
					bool bFound = false;
					for (int32 i = 0; i < Mappings.Num(); i++)
					{
						if (Mappings[i].Action == Action && Mappings[i].Key == Key)
						{
							bFound = true;
							break;
						}
					}

					if (!bFound)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] remove(\"mapping\") -> no mapping for %s+%s"),
							*Action->GetName(), UTF8_TO_TCHAR(KeyStr.c_str())));
						return sol::lua_nil;
					}

					const FScopedTransaction Txn(NSLOCTEXT("AgentIntegrationKit", "LuaRemoveMappingByKey", "Lua: Remove Mapping by Key"));
					Context->Modify();
					Context->UnmapKey(Action, Key);
					Context->GetPackage()->MarkPackageDirty();

					Session.Log(FString::Printf(TEXT("[OK] remove(\"mapping\", action=\"%s\", key=\"%s\")"),
						*Action->GetName(), UTF8_TO_TCHAR(KeyStr.c_str())));
					return sol::make_object(Lua, true);
				}

				Session.Log(TEXT("[FAIL] remove(\"mapping\") -> provide 1-based index or {action=.., key=..}"));
				return sol::lua_nil;
			}
			else if (FType.Equals(TEXT("all_mappings"), ESearchCase::IgnoreCase))
			{
				int32 Count = Context->GetMappings().Num();
				if (Count == 0)
				{
					Session.Log(TEXT("[OK] remove(\"all_mappings\") -> already empty"));
					return sol::make_object(Lua, 0);
				}

				const FScopedTransaction Txn(NSLOCTEXT("AgentIntegrationKit", "LuaUnmapAll", "Lua: Unmap All"));
				Context->Modify();
				Context->UnmapAll();
				Context->GetPackage()->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"all_mappings\") -> removed %d mapping(s)"), Count));
				return sol::make_object(Lua, Count);
			}
			else if (FType.Equals(TEXT("all_for_action"), ESearchCase::IgnoreCase))
			{
				// Id should be an action reference string
				if (!Id.is<std::string>())
				{
					Session.Log(TEXT("[FAIL] remove(\"all_for_action\") -> action name/path required"));
					return sol::lua_nil;
				}

				FString ActionRef = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				UInputAction* Action = ResolveInputActionFromRef(ActionRef);
				if (!Action)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"all_for_action\") -> could not resolve action '%s'"), *ActionRef));
					return sol::lua_nil;
				}

				// Count how many will be removed
				int32 Count = 0;
				for (const FEnhancedActionKeyMapping& M : Context->GetMappings())
				{
					if (M.Action == Action) Count++;
				}

				if (Count == 0)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"all_for_action\") -> no mappings for %s"), *Action->GetName()));
					return sol::lua_nil;
				}

				const FScopedTransaction Txn(NSLOCTEXT("AgentIntegrationKit", "LuaUnmapAllForAction", "Lua: Unmap All Keys From Action"));
				Context->Modify();
				Context->UnmapAllKeysFromAction(Action);
				Context->GetPackage()->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"all_for_action\", \"%s\") -> removed %d mapping(s)"), *Action->GetName(), Count));
				return sol::make_object(Lua, Count);
			}
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		else if (FType.Equals(TEXT("profile_override"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] remove(\"profile_override\") -> table required: {profile=.., index=N (optional)}"));
					return sol::lua_nil;
				}

				sol::table IdTable = Id.as<sol::table>();
				std::string ProfileId = IdTable.get_or<std::string>("profile", "");
				if (ProfileId.empty())
				{
					Session.Log(TEXT("[FAIL] remove(\"profile_override\") -> 'profile' required"));
					return sol::lua_nil;
				}

				TMap<FString, FInputMappingContextMappingData>* Overrides = GetMappingProfileOverrides(Context);
				if (!Overrides)
				{
					Session.Log(TEXT("[FAIL] remove(\"profile_override\") -> could not access MappingProfileOverrides"));
					return sol::lua_nil;
				}

				FString FProfileId = UTF8_TO_TCHAR(ProfileId.c_str());
				FInputMappingContextMappingData* ProfileData = Overrides->Find(FProfileId);
				if (!ProfileData)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"profile_override\") -> profile '%s' not found"), *FProfileId));
					return sol::lua_nil;
				}

				const FScopedTransaction Txn(NSLOCTEXT("AgentIntegrationKit", "LuaRemoveProfileOverride", "Lua: Remove Profile Override"));
				Context->Modify();

				// If index is provided, remove single mapping from the profile
				sol::optional<int> IndexOpt = IdTable.get<sol::optional<int>>("index");
				if (IndexOpt.has_value())
				{
					int32 LuaIdx = IndexOpt.value();
					int32 Idx = LuaIdx - 1;
					if (Idx < 0 || Idx >= ProfileData->Mappings.Num())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] remove(\"profile_override\", index=%d) -> out of range (1-%d)"),
							LuaIdx, ProfileData->Mappings.Num()));
						return sol::lua_nil;
					}

					FString ActionName = ProfileData->Mappings[Idx].Action ? ProfileData->Mappings[Idx].Action->GetName() : TEXT("(none)");
					FString KeyName = ProfileData->Mappings[Idx].Key.GetFName().ToString();
					ProfileData->Mappings.RemoveAt(Idx);

					Context->PostEditChange();
					Context->GetPackage()->MarkPackageDirty();

					Session.Log(FString::Printf(TEXT("[OK] remove(\"profile_override\", profile=\"%s\", index=%d) -> removed %s+%s"),
						*FProfileId, LuaIdx, *ActionName, *KeyName));
					return sol::make_object(Lua, true);
				}

				// No index — remove entire profile
				int32 RemovedCount = ProfileData->Mappings.Num();
				Overrides->Remove(FProfileId);

				Context->PostEditChange();
				Context->GetPackage()->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"profile_override\", profile=\"%s\") -> removed entire profile (%d mappings)"),
					*FProfileId, RemovedCount));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: mapping, all_mappings, all_for_action, profile_override"), *FType));
#else
			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: mapping, all_mappings, all_for_action"), *FType));
#endif
			return sol::lua_nil;
		});

		// ---- configure("mapping", index, params) ----
		AssetObj.set_function("configure", [Context, &Session](sol::table /*self*/,
			const std::string& Type, sol::object IndexObj, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (!FType.Equals(TEXT("mapping"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: mapping"), *FType));
				return sol::lua_nil;
			}

			if (!IndexObj.is<int>()) { Session.Log(TEXT("[FAIL] configure(\"mapping\") -> 1-based index required as second arg")); return sol::lua_nil; }
			if (!Params.has_value()) { Session.Log(TEXT("[FAIL] configure(\"mapping\") -> params table required as third arg")); return sol::lua_nil; }

			int32 LuaIdx = IndexObj.as<int>();
			int32 Idx = LuaIdx - 1;
			if (Idx < 0 || Idx >= Context->GetMappings().Num())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure(\"mapping\", %d) -> out of range (1-%d)"), LuaIdx, Context->GetMappings().Num()));
				return sol::lua_nil;
			}

			const FScopedTransaction Txn(NSLOCTEXT("AgentIntegrationKit", "LuaConfigureMapping", "Lua: Configure Mapping"));
			Context->Modify();

			// Use GetMapping for mutable access (not const_cast)
			FEnhancedActionKeyMapping& Mapping = Context->GetMapping(Idx);
			sol::table P = Params.value();
			int32 Changes = 0;

			// Change key
			std::string NewKeyStr = P.get_or<std::string>("key", "");
			if (!NewKeyStr.empty())
			{
				FKey NewKey = ResolveKey(UTF8_TO_TCHAR(NewKeyStr.c_str()));
				if (NewKey.IsValid())
				{
					Mapping.Key = NewKey;
					Changes++;
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[WARN] configure(\"mapping\") -> invalid key '%s'"), UTF8_TO_TCHAR(NewKeyStr.c_str())));
				}
			}

			// Remove triggers first (descending order) so indices remain valid
			sol::optional<sol::table> RemTrigs = P.get<sol::optional<sol::table>>("remove_triggers");
			if (RemTrigs.has_value())
			{
				TArray<int32> Indices;
				for (auto& Pair : RemTrigs.value())
				{
					if (Pair.second.is<int>())
					{
						int32 TIdx = Pair.second.as<int>() - 1; // 1-based to 0-based
						if (TIdx >= 0 && TIdx < Mapping.Triggers.Num()) Indices.AddUnique(TIdx);
					}
				}
				Indices.Sort([](int32 A, int32 B) { return A > B; });
				for (int32 TIdx : Indices) { Mapping.Triggers.RemoveAt(TIdx); Changes++; }
			}

			// Add triggers
			sol::optional<sol::table> AddTrigs = P.get<sol::optional<sol::table>>("add_triggers");
			if (AddTrigs.has_value())
			{
				for (auto& Pair : AddTrigs.value())
				{
					if (Pair.second.is<sol::table>())
					{
						FString Error;
						UInputTrigger* Trig = CreateTriggerFromTable(Context, Pair.second.as<sol::table>(), Error);
						if (Trig) { Mapping.Triggers.Add(Trig); Changes++; }
						else Session.Log(FString::Printf(TEXT("[WARN] configure trigger: %s"), *Error));
					}
				}
			}

			// Remove modifiers (descending)
			sol::optional<sol::table> RemMods = P.get<sol::optional<sol::table>>("remove_modifiers");
			if (RemMods.has_value())
			{
				TArray<int32> Indices;
				for (auto& Pair : RemMods.value())
				{
					if (Pair.second.is<int>())
					{
						int32 MIdx = Pair.second.as<int>() - 1;
						if (MIdx >= 0 && MIdx < Mapping.Modifiers.Num()) Indices.AddUnique(MIdx);
					}
				}
				Indices.Sort([](int32 A, int32 B) { return A > B; });
				for (int32 MIdx : Indices) { Mapping.Modifiers.RemoveAt(MIdx); Changes++; }
			}

			// Add modifiers
			sol::optional<sol::table> AddMods = P.get<sol::optional<sol::table>>("add_modifiers");
			if (AddMods.has_value())
			{
				for (auto& Pair : AddMods.value())
				{
					if (Pair.second.is<sol::table>())
					{
						FString Error;
						UInputModifier* Mod = CreateModifierFromTable(Context, Pair.second.as<sol::table>(), Error);
						if (Mod) { Mapping.Modifiers.Add(Mod); Changes++; }
						else Session.Log(FString::Printf(TEXT("[WARN] configure modifier: %s"), *Error));
					}
				}
			}

			// After direct mapping modification, request rebuild so runtime picks up changes
			// (MapKey/UnmapKey do this internally, but GetMapping() modifications need it explicitly)
			if (Changes > 0)
			{
				RequestRebuildMappings(Context);
				Context->PostEditChange();
				Context->GetPackage()->MarkPackageDirty();
			}

			FString ActionName = Mapping.Action ? Mapping.Action->GetName() : TEXT("(none)");
			Session.Log(FString::Printf(TEXT("[OK] configure(\"mapping\", %d) -> %d change(s) on %s+%s"),
				LuaIdx, Changes, *ActionName, *Mapping.Key.GetFName().ToString()));
			return sol::make_object(Lua, Changes);
		});

		// ---- list(type?, arg?) ----
		AssetObj.set_function("list", [Context, &Session](sol::table self,
			sol::optional<std::string> TypeOpt, sol::optional<std::string> ArgOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = self["info"];
				if (InfoFn.valid()) return InfoFn(self);
				return sol::lua_nil;
			}

			if (FType.Equals(TEXT("mappings"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("mapping"), ESearchCase::IgnoreCase))
			{
				const TArray<FEnhancedActionKeyMapping>& Mappings = Context->GetMappings();
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Mappings.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["index"] = i + 1; // 1-based
					E["action"] = Mappings[i].Action ? TCHAR_TO_UTF8(*Mappings[i].Action->GetName()) : "none";
					E["action_path"] = Mappings[i].Action ? TCHAR_TO_UTF8(*Mappings[i].Action->GetPathName()) : "none";
					E["key"] = TCHAR_TO_UTF8(*Mappings[i].Key.GetFName().ToString());
					E["num_triggers"] = static_cast<int>(Mappings[i].Triggers.Num());
					E["num_modifiers"] = static_cast<int>(Mappings[i].Modifiers.Num());
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"mappings\") -> %d"), Mappings.Num()));
				return Result;
			}

			// Per-mapping trigger/modifier details: list("mapping_details", "2") where arg is 1-based index
			if (FType.Equals(TEXT("mapping_details"), ESearchCase::IgnoreCase))
			{
				if (!ArgOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] list(\"mapping_details\") -> 1-based mapping index required as second arg"));
					return sol::lua_nil;
				}

				int32 LuaIdx = FCString::Atoi(UTF8_TO_TCHAR(ArgOpt.value().c_str()));
				int32 Idx = LuaIdx - 1;
				const TArray<FEnhancedActionKeyMapping>& Mappings = Context->GetMappings();
				if (Idx < 0 || Idx >= Mappings.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] list(\"mapping_details\", %d) -> out of range (1-%d)"), LuaIdx, Mappings.Num()));
					return sol::lua_nil;
				}

				const FEnhancedActionKeyMapping& M = Mappings[Idx];
				sol::table Result = Lua.create_table();
				Result["index"] = LuaIdx;
				Result["action"] = M.Action ? TCHAR_TO_UTF8(*M.Action->GetName()) : "none";
				Result["action_path"] = M.Action ? TCHAR_TO_UTF8(*M.Action->GetPathName()) : "none";
				Result["key"] = TCHAR_TO_UTF8(*M.Key.GetFName().ToString());

				sol::table TrigsTable = Lua.create_table();
				for (int32 t = 0; t < M.Triggers.Num(); t++)
				{
					TrigsTable[t + 1] = TriggerToTable(Lua, M.Triggers[t], t + 1);
				}
				Result["triggers"] = TrigsTable;

				sol::table ModsTable = Lua.create_table();
				for (int32 m = 0; m < M.Modifiers.Num(); m++)
				{
					ModsTable[m + 1] = ModifierToTable(Lua, M.Modifiers[m], m + 1);
				}
				Result["modifiers"] = ModsTable;

				Session.Log(FString::Printf(TEXT("[OK] list(\"mapping_details\", %d) -> %d triggers, %d modifiers"),
					LuaIdx, M.Triggers.Num(), M.Modifiers.Num()));
				return Result;
			}

	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		if (FType.Equals(TEXT("profiles"), ESearchCase::IgnoreCase))
			{
				TMap<FString, FInputMappingContextMappingData>* Overrides = GetMappingProfileOverrides(Context);
				sol::table Result = Lua.create_table();
				if (Overrides)
				{
					int32 Idx = 1;
					FString ProfileNames;
					for (auto& Pair : *Overrides)
					{
						sol::table E = Lua.create_table();
						E["profile"] = TCHAR_TO_UTF8(*Pair.Key);
						E["num_mappings"] = static_cast<int>(Pair.Value.Mappings.Num());
						Result[Idx++] = E;
						if (!ProfileNames.IsEmpty()) ProfileNames += TEXT(", ");
						ProfileNames += FString::Printf(TEXT("\"%s\" (%d mappings)"), *Pair.Key, Pair.Value.Mappings.Num());
					}
					Session.Log(FString::Printf(TEXT("[OK] list(\"profiles\") -> %d profile(s): %s"), Overrides->Num(), *ProfileNames));
				}
				else
				{
					Session.Log(TEXT("[OK] list(\"profiles\") -> 0 (could not access MappingProfileOverrides)"));
				}
				return Result;
			}

			if (FType.Equals(TEXT("profile_override"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("profile_overrides"), ESearchCase::IgnoreCase))
			{
				if (!ArgOpt.has_value())
				{
					// No profile specified — list all profiles (same as list("profiles"))
					TMap<FString, FInputMappingContextMappingData>* Overrides = GetMappingProfileOverrides(Context);
					sol::table Result = Lua.create_table();
					if (Overrides)
					{
						int32 Idx = 1;
						for (auto& Pair : *Overrides)
						{
							sol::table E = Lua.create_table();
							E["profile"] = TCHAR_TO_UTF8(*Pair.Key);
							E["num_mappings"] = static_cast<int>(Pair.Value.Mappings.Num());
							Result[Idx++] = E;
						}
						Session.Log(FString::Printf(TEXT("[OK] list(\"profile_override\") -> %d profile(s)"), Overrides->Num()));
					}
					return Result;
				}

				// List mappings for a specific profile
				FString FProfileId = UTF8_TO_TCHAR(ArgOpt.value().c_str());
				TMap<FString, FInputMappingContextMappingData>* Overrides = GetMappingProfileOverrides(Context);
				if (!Overrides)
				{
					Session.Log(TEXT("[FAIL] list(\"profile_override\") -> could not access MappingProfileOverrides"));
					return sol::lua_nil;
				}

				FInputMappingContextMappingData* ProfileData = Overrides->Find(FProfileId);
				if (!ProfileData)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] list(\"profile_override\", \"%s\") -> profile not found"), *FProfileId));
					return sol::lua_nil;
				}

				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < ProfileData->Mappings.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["index"] = i + 1;
					E["action"] = ProfileData->Mappings[i].Action ? TCHAR_TO_UTF8(*ProfileData->Mappings[i].Action->GetName()) : "none";
					E["action_path"] = ProfileData->Mappings[i].Action ? TCHAR_TO_UTF8(*ProfileData->Mappings[i].Action->GetPathName()) : "none";
					E["key"] = TCHAR_TO_UTF8(*ProfileData->Mappings[i].Key.GetFName().ToString());
					E["num_triggers"] = static_cast<int>(ProfileData->Mappings[i].Triggers.Num());
					E["num_modifiers"] = static_cast<int>(ProfileData->Mappings[i].Modifiers.Num());
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"profile_override\", \"%s\") -> %d mapping(s)"),
					*FProfileId, ProfileData->Mappings.Num()));
				return Result;
			}
#else
			if (FType.Equals(TEXT("profiles"), ESearchCase::IgnoreCase) ||
				FType.Equals(TEXT("profile_override"), ESearchCase::IgnoreCase) ||
				FType.Equals(TEXT("profile_overrides"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				Result["error"] = "error: profile overrides require UE 5.7+";
				Session.Log(TEXT("[FAIL] list(\"profiles\"/\"profile_override\") -> profile overrides require UE 5.7+"));
				return Result;
			}
#endif

			if (FType.Equals(TEXT("trigger_types"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				const TArray<FString>& Names = LuaDynamicType::ListDerivedTypeNames(UInputTrigger::StaticClass(), TriggerPrefixes);
				for (int32 i = 0; i < Names.Num(); i++) Result[i + 1] = TCHAR_TO_UTF8(*Names[i]);
				Session.Log(FString::Printf(TEXT("[OK] list(\"trigger_types\") -> %d"), Names.Num()));
				return Result;
			}

			if (FType.Equals(TEXT("modifier_types"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				const TArray<FString>& Names = LuaDynamicType::ListDerivedTypeNames(UInputModifier::StaticClass(), ModifierPrefixes);
				for (int32 i = 0; i < Names.Num(); i++) Result[i + 1] = TCHAR_TO_UTF8(*Names[i]);
				Session.Log(FString::Printf(TEXT("[OK] list(\"modifier_types\") -> %d"), Names.Num()));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: mappings, mapping_details, profiles, profile_override, trigger_types, modifier_types"), *FType));
			return sol::lua_nil;
		});

		// ---- info() ----
		AssetObj.set_function("info", [Context, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			const TArray<FEnhancedActionKeyMapping>& Mappings = Context->GetMappings();

			sol::table Result = Lua.create_table();
			Result["name"] = TCHAR_TO_UTF8(*Context->GetName());
			Result["description"] = TCHAR_TO_UTF8(*Context->ContextDescription.ToString());
			Result["num_mappings"] = static_cast<int>(Mappings.Num());

			sol::table MappingsSummary = Lua.create_table();
			for (int32 i = 0; i < Mappings.Num(); i++)
			{
				sol::table E = Lua.create_table();
				E["index"] = i + 1;
				E["action"] = Mappings[i].Action ? TCHAR_TO_UTF8(*Mappings[i].Action->GetName()) : "none";
				E["key"] = TCHAR_TO_UTF8(*Mappings[i].Key.GetFName().ToString());
				MappingsSummary[i + 1] = E;
			}
			Result["mappings"] = MappingsSummary;

			// Include profile overrides in info
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			TMap<FString, FInputMappingContextMappingData>* Overrides = GetMappingProfileOverrides(Context);
			if (Overrides && Overrides->Num() > 0)
			{
				sol::table ProfilesTable = Lua.create_table();
				int32 Idx = 1;
				for (auto& Pair : *Overrides)
				{
					sol::table PE = Lua.create_table();
					PE["profile"] = TCHAR_TO_UTF8(*Pair.Key);
					PE["num_mappings"] = static_cast<int>(Pair.Value.Mappings.Num());
					ProfilesTable[Idx++] = PE;
				}
				Result["profile_overrides"] = ProfilesTable;
				Result["num_profiles"] = static_cast<int>(Overrides->Num());
			}
			else
			{
				Result["num_profiles"] = 0;
			}

			Session.Log(FString::Printf(TEXT("[OK] info() -> %s, %d mappings, %d profile(s)"),
				*Context->GetName(), Mappings.Num(), Overrides ? Overrides->Num() : 0));
#else
			Result["num_profiles"] = 0;
			Session.Log(FString::Printf(TEXT("[OK] info() -> %s, %d mappings"),
				*Context->GetName(), Mappings.Num()));
#endif
			return Result;
		});
	});
}

REGISTER_LUA_BINDING(EnhancedInput, EnhancedInputDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("EnhancedInput")))
	{
		Session.Log(TEXT("[WARN] EnhancedInput plugin is not loaded. Enable it in Edit > Plugins to use this feature."));
		return;
	}
	BindEnhancedInput(Lua, Session);
});
