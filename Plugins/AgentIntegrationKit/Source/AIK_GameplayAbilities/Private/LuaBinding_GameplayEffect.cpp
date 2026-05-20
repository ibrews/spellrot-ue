// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Engine/Blueprint.h"
#include "GameplayEffect.h"
#include "GameplayEffectComponent.h"
#include "Lua/LuaDynamicTypeHelper.h"
#include "GameplayEffectTypes.h"
#include "ScalableFloat.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "AttributeSet.h"
#include "GameplayEffectExecutionCalculation.h"
#include "GameplayModMagnitudeCalculation.h"
#include "UObject/UnrealType.h"
#include "Modules/ModuleManager.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// Helpers
// ============================================================================

namespace
{

// Map short component names to full class names
static const TMap<FString, FString> ComponentClassMap = {
	{ TEXT("TargetTagRequirements"),  TEXT("TargetTagRequirementsGameplayEffectComponent") },
	{ TEXT("AssetTags"),              TEXT("AssetTagsGameplayEffectComponent") },
	{ TEXT("BlockAbilityTags"),       TEXT("BlockAbilityTagsGameplayEffectComponent") },
	{ TEXT("TargetTags"),             TEXT("TargetTagsGameplayEffectComponent") },
	{ TEXT("AdditionalEffects"),      TEXT("AdditionalEffectsGameplayEffectComponent") },
	{ TEXT("Abilities"),              TEXT("AbilitiesGameplayEffectComponent") },
	{ TEXT("CancelAbilityTags"),      TEXT("CancelAbilityTagsGameplayEffectComponent") },
	{ TEXT("RemoveOther"),            TEXT("RemoveOtherGameplayEffectComponent") },
	{ TEXT("Immunity"),               TEXT("ImmunityGameplayEffectComponent") },
	{ TEXT("ChanceToApply"),          TEXT("ChanceToApplyGameplayEffectComponent") },
	{ TEXT("CustomCanApply"),         TEXT("CustomCanApplyGameplayEffectComponent") },
};

static const TArray<FString> GEComponentSuffixes = { TEXT("GameplayEffectComponent") };

// Resolve a component type short name or full class name to a UClass
UClass* ResolveComponentClass(const FString& TypeName)
{
	// Try short name from backward-compatible map first
	if (const FString* FullName = ComponentClassMap.Find(TypeName))
	{
		UClass* Cls = FindFirstObject<UClass>(**FullName, EFindFirstObjectOptions::NativeFirst);
		if (Cls) return Cls;
	}
	// Try case-insensitive short name from map
	for (const auto& Pair : ComponentClassMap)
	{
		if (Pair.Key.Equals(TypeName, ESearchCase::IgnoreCase))
		{
			UClass* Cls = FindFirstObject<UClass>(*Pair.Value, EFindFirstObjectOptions::NativeFirst);
			if (Cls) return Cls;
		}
	}
	// Dynamic discovery fallback — finds any GE component subclass by name
	UClass* DynCls = LuaDynamicType::FindDerivedClass(UGameplayEffectComponent::StaticClass(), TypeName, {}, GEComponentSuffixes);
	if (DynCls) return DynCls;

	// Last resort: try as full class name
	UClass* Cls = FindFirstObject<UClass>(*TypeName, EFindFirstObjectOptions::NativeFirst);
	if (Cls && Cls->IsChildOf(UGameplayEffectComponent::StaticClass()))
	{
		return Cls;
	}
	return nullptr;
}

// Get short name from a component class
FString GetComponentShortName(const UGameplayEffectComponent* Comp)
{
	if (!Comp) return TEXT("(null)");
	FString FullName = Comp->GetClass()->GetName();
	FString ShortName = FullName;
	ShortName.RemoveFromEnd(TEXT("GameplayEffectComponent"));
	if (ShortName.IsEmpty()) ShortName = FullName;
	return ShortName;
}

// Access the protected GEComponents array via reflection
struct FGEComponentsAccessor
{
	FArrayProperty* ArrayProp = nullptr;
	FObjectPropertyBase* InnerProp = nullptr;
	mutable TOptional<FScriptArrayHelper> ArrayHelper;
	UGameplayEffect* GE = nullptr;

	FGEComponentsAccessor(UGameplayEffect* InGE)
		: GE(InGE)
	{
		if (!GE) return;
		ArrayProp = CastField<FArrayProperty>(GE->GetClass()->FindPropertyByName(TEXT("GEComponents")));
		if (!ArrayProp) return;
		InnerProp = CastField<FObjectPropertyBase>(ArrayProp->Inner);
		if (!InnerProp) return;
		ArrayHelper.Emplace(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(GE));
	}

	bool IsValid() const { return ArrayHelper.IsSet() && InnerProp != nullptr; }
	int32 Num() const { return ArrayHelper.IsSet() ? ArrayHelper->Num() : 0; }

	UGameplayEffectComponent* Get(int32 Index) const
	{
		if (!IsValid() || Index < 0 || Index >= ArrayHelper->Num()) return nullptr;
		return Cast<UGameplayEffectComponent>(
			InnerProp->GetObjectPropertyValue(ArrayHelper->GetElementPtr(Index)));
	}

	// Find component by class
	UGameplayEffectComponent* FindByClass(UClass* CompClass) const
	{
		if (!IsValid() || !CompClass) return nullptr;
		for (int32 i = 0; i < ArrayHelper->Num(); i++)
		{
			UGameplayEffectComponent* Comp = Get(i);
			if (Comp && Comp->IsA(CompClass))
			{
				return Comp;
			}
		}
		return nullptr;
	}

	int32 FindIndexByClass(UClass* CompClass) const
	{
		if (!IsValid() || !CompClass) return INDEX_NONE;
		for (int32 i = 0; i < ArrayHelper->Num(); i++)
		{
			UGameplayEffectComponent* Comp = Get(i);
			if (Comp && Comp->IsA(CompClass))
			{
				return i;
			}
		}
		return INDEX_NONE;
	}

	void AddComponent(UGameplayEffectComponent* NewComp)
	{
		if (!IsValid() || !NewComp) return;
		int32 NewIdx = ArrayHelper->AddValue();
		InnerProp->SetObjectPropertyValue(ArrayHelper->GetElementPtr(NewIdx), NewComp);
	}

	void RemoveAt(int32 Index)
	{
		if (!IsValid() || Index < 0 || Index >= ArrayHelper->Num()) return;
		ArrayHelper->RemoveValues(Index, 1);
	}
};

// Parse modifier operation string to enum
bool ParseModifierOp(const FString& OpStr, EGameplayModOp::Type& OutOp)
{
	if (OpStr.Equals(TEXT("Add"), ESearchCase::IgnoreCase) || OpStr.Equals(TEXT("Additive"), ESearchCase::IgnoreCase))
		{ OutOp = EGameplayModOp::Additive; return true; }
	if (OpStr.Equals(TEXT("Multiply"), ESearchCase::IgnoreCase) || OpStr.Equals(TEXT("Multiplicitive"), ESearchCase::IgnoreCase))
		{ OutOp = EGameplayModOp::Multiplicitive; return true; }
	if (OpStr.Equals(TEXT("Divide"), ESearchCase::IgnoreCase) || OpStr.Equals(TEXT("Division"), ESearchCase::IgnoreCase))
		{ OutOp = EGameplayModOp::Division; return true; }
	if (OpStr.Equals(TEXT("Override"), ESearchCase::IgnoreCase))
		{ OutOp = EGameplayModOp::Override; return true; }
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	if (OpStr.Equals(TEXT("MultiplyCompound"), ESearchCase::IgnoreCase))
		{ OutOp = EGameplayModOp::MultiplyCompound; return true; }
	if (OpStr.Equals(TEXT("AddFinal"), ESearchCase::IgnoreCase))
		{ OutOp = EGameplayModOp::AddFinal; return true; }
#endif
	return false;
}

FString FormatModOp(TEnumAsByte<EGameplayModOp::Type> Op)
{
	switch (Op.GetValue())
	{
	case EGameplayModOp::Additive: return TEXT("Add");
	case EGameplayModOp::Multiplicitive: return TEXT("Multiply");
	case EGameplayModOp::Division: return TEXT("Divide");
	case EGameplayModOp::Override: return TEXT("Override");
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	case EGameplayModOp::MultiplyCompound: return TEXT("MultiplyCompound");
	case EGameplayModOp::AddFinal: return TEXT("AddFinal");
#endif
	default: return FString::Printf(TEXT("Op(%d)"), (int32)Op.GetValue());
	}
}

FString FormatMagnitude(const FGameplayEffectModifierMagnitude& Magnitude)
{
	switch (Magnitude.GetMagnitudeCalculationType())
	{
	case EGameplayEffectMagnitudeCalculation::ScalableFloat:
	{
		float Value = 0.f;
		Magnitude.GetStaticMagnitudeIfPossible(1.f, Value);
		return FString::Printf(TEXT("ScalableFloat(%.2f)"), Value);
	}
	case EGameplayEffectMagnitudeCalculation::AttributeBased:
		return TEXT("AttributeBased");
	case EGameplayEffectMagnitudeCalculation::CustomCalculationClass:
		return TEXT("CustomCalculation");
	case EGameplayEffectMagnitudeCalculation::SetByCaller:
		return TEXT("SetByCaller");
	default:
		return TEXT("Unknown");
	}
}

// Construct FGameplayAttribute from "ClassName.PropertyName" string
bool ConstructGameplayAttribute(const FString& AttrStr, FGameplayAttribute& OutAttr, FString& OutError)
{
	FString ClassName, PropName;
	if (!AttrStr.Split(TEXT("."), &ClassName, &PropName))
	{
		OutError = FString::Printf(TEXT("attribute must be 'ClassName.PropertyName', got '%s'"), *AttrStr);
		return false;
	}

	// Strip leading U if present
	if (ClassName.StartsWith(TEXT("U")))
	{
		ClassName.RemoveFromStart(TEXT("U"));
	}

	UClass* AttrSetClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	if (!AttrSetClass)
	{
		OutError = FString::Printf(TEXT("attribute set class '%s' not found"), *ClassName);
		return false;
	}

	if (!AttrSetClass->IsChildOf(UAttributeSet::StaticClass()))
	{
		OutError = FString::Printf(TEXT("'%s' is not an AttributeSet subclass"), *ClassName);
		return false;
	}

	FProperty* Prop = AttrSetClass->FindPropertyByName(FName(*PropName));
	if (!Prop)
	{
		OutError = FString::Printf(TEXT("property '%s' not found on '%s'"), *PropName, *ClassName);
		return false;
	}

	OutAttr = FGameplayAttribute(Prop);
	if (!OutAttr.IsValid())
	{
		OutError = FString::Printf(TEXT("'%s.%s' is not a valid gameplay attribute"), *ClassName, *PropName);
		return false;
	}

	return true;
}

// Validate and request a gameplay tag, returning whether it's valid
FGameplayTag ValidateTag(const FString& TagStr)
{
	return FGameplayTag::RequestGameplayTag(FName(*TagStr), /*ErrorIfNotFound=*/false);
}

// Parse a Lua table of tag strings into a tag container, validating each
bool ParseTagArray(const sol::table& TagTable, FGameplayTagContainer& OutContainer, FString& OutError)
{
	for (auto& Pair : TagTable)
	{
		if (!Pair.second.is<std::string>()) continue;
		FString TagStr = UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str());
		FGameplayTag Tag = ValidateTag(TagStr);
		if (!Tag.IsValid())
		{
			OutError = FString::Printf(TEXT("tag '%s' not found in project tag registry"), *TagStr);
			return false;
		}
		OutContainer.AddTag(Tag);
	}
	return true;
}

// Parse a Lua table {require={"Tag1","Tag2"}, ignore={"Tag3"}} into FGameplayTagRequirements
bool ParseTagRequirements(const sol::table& Table, FGameplayTagRequirements& OutReqs, FString& OutError)
{
	sol::optional<sol::table> ReqTags = Table.get<sol::optional<sol::table>>("require");
	if (ReqTags.has_value())
	{
		if (!ParseTagArray(ReqTags.value(), OutReqs.RequireTags, OutError))
			return false;
	}
	sol::optional<sol::table> IgnTags = Table.get<sol::optional<sol::table>>("ignore");
	if (IgnTags.has_value())
	{
		if (!ParseTagArray(IgnTags.value(), OutReqs.IgnoreTags, OutError))
			return false;
	}
	return true;
}

// Pre/Post edit helpers for CDO modification
void PreEditGE(UGameplayEffect* GE, FProperty* Prop)
{
	GE->SetFlags(RF_Transactional);
	GE->Modify();
	if (Prop)
	{
		GE->PreEditChange(Prop);
	}
}

void PostEditGE(UGameplayEffect* GE, FProperty* Prop)
{
	FPropertyChangedEvent Evt(Prop, EPropertyChangeType::ValueSet);
	GE->PostEditChangeProperty(Evt);
	GE->OnGameplayEffectChanged();
	GE->MarkPackageDirty();
}

} // anonymous namespace

// ============================================================================
// Registration
// ============================================================================

static TArray<FLuaFunctionDoc> GameplayEffectDocs = {};

static void BindGameplayEffect(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_gameplay_effect", [&Session](sol::table BPObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = BPObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());

		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *FPath);
		if (!Blueprint || !Blueprint->GeneratedClass) return;

		UGameplayEffect* GE = Cast<UGameplayEffect>(Blueprint->GeneratedClass->GetDefaultObject());
		if (!GE) return;

		// ---- help text ----
		BPObj["_help_text"] =
			"GameplayEffect verbs (add/remove/list/configure/info/help):\n"
			"\n"
			"add(type, params):\n"
			"  add(\"component\", {type=\"TargetTagRequirements\"})\n"
			"  add(\"modifier\", {attribute=\"MyAttrSet.Health\", op=\"Add\", value=50})\n"
			"  add(\"modifier\", {attribute=\"...\", op=\"Multiply\", magnitude={type=\"SetByCaller\", data_tag=\"SetByCaller.Damage\"}})\n"
			"  add(\"modifier\", {attribute=\"...\", op=\"Add\", magnitude={type=\"AttributeBased\", backing_attribute=\"MyAttrSet.Strength\",\n"
			"    coefficient=1.5, pre_multiply_additive=10, post_multiply_additive=5,\n"
			"    attribute_calculation_type=\"AttributeBaseValue\", capture_source=\"Target\", snapshot=true,\n"
			"    source_tag_filter={\"Buff.Active\"}, target_tag_filter={\"Status.Alive\"}}})\n"
			"  add(\"modifier\", {attribute=\"...\", op=\"Add\", magnitude={type=\"CustomCalculation\",\n"
			"    calculation_class=\"/Script/MyGame.MyMagnitudeCalc\", coefficient=2.0,\n"
			"    pre_multiply_additive=0, post_multiply_additive=10}})\n"
			"  add(\"modifier\", {..., source_tags={require={\"Buff.Active\"}, ignore={\"Debuff.Immune\"}},\n"
			"    target_tags={require={\"Status.Alive\"}}})\n"
			"  add(\"tag\", {component=\"TargetTags\", tag=\"Buff.Shield\"})\n"
			"  add(\"cue\", {tag=\"GameplayCue.MyTag\", magnitude_attribute=\"MyAttrSet.Health\", min_level=0, max_level=1})\n"
			"  add(\"execution\", {class=\"/Script/MyGame.MyExecCalc\", passed_in_tags={\"Tag1\",\"Tag2\"}})\n"
			"\n"
			"remove(type, id):\n"
			"  remove(\"component\", {type=\"TargetTags\"}) or remove(\"component\", 1)\n"
			"  remove(\"modifier\", 1)  -- 1-based index\n"
			"  remove(\"tag\", {component=\"TargetTags\", tag=\"Buff.Shield\"})\n"
			"  remove(\"cue\", 1), remove(\"execution\", 1)\n"
			"\n"
			"list(type):\n"
			"  list(\"components\"), list(\"modifiers\"), list(\"cues\"), list(\"executions\")\n"
			"\n"
			"configure(type, params):\n"
			"  configure(\"component\", {type=\"TargetTagRequirements\", application_require={\"Buff.Active\"}, application_ignore={\"Debuff.Immune\"}})\n"
			"  configure(\"component\", {type=\"ChanceToApply\", chance=0.5})\n"
			"  configure(\"modifier\", {index=1, op=\"Multiply\", value=2.0})\n"
			"  configure(\"modifier\", {index=1, source_tags={require={\"Tag1\"}, ignore={\"Tag2\"}}, target_tags={require={\"Tag3\"}}})\n"
			"  configure(\"duration\", {policy=\"HasDuration\", magnitude=5.0, max_magnitude=10.0, period=1.0,\n"
			"    execute_on_application=true, periodic_inhibition_policy=\"ResetPeriod\"})\n"
			"  configure(\"stacking\", {type=\"AggregateByTarget\", limit=5, deny_overflow_application=true,\n"
			"    clear_stack_on_overflow=false, factor_in_stack_count=true,\n"
			"    duration_refresh=\"ExtendDuration\", period_reset=\"NeverReset\",\n"
			"    expiration=\"ClearEntireStack\", overflow_effects={\"/Script/MyGame.MyOverflowGE\"}})\n"
			"  configure(\"cues\", {require_modifier_success=true, suppress_stacking_cues=false})\n"
			"\n"
			"Component types: TargetTagRequirements, AssetTags, BlockAbilityTags, TargetTags,\n"
			"  AdditionalEffects, Abilities, CancelAbilityTags, RemoveOther, Immunity,\n"
			"  ChanceToApply, CustomCanApply\n"
			"\n"
			"Modifier ops: Add, Multiply, Divide, Override, MultiplyCompound, AddFinal\n"
			"Magnitude types: ScalableFloat (default), AttributeBased, SetByCaller, CustomCalculation\n"
			"AttributeBased: capture_source=Source|Target, snapshot=true|false\n"
			"AttributeBased calc types: AttributeMagnitude, AttributeBaseValue, AttributeBonusMagnitude, AttributeMagnitudeEvaluatedUpToChannel\n"
			"Duration policies: Instant, Infinite, HasDuration\n"
			"Stacking duration_refresh: RefreshOnSuccessfulApplication, NeverRefresh, ExtendDuration\n"
			"Stacking period_reset: ResetOnSuccessfulApplication, NeverReset\n"
			"Stacking expiration: ClearEntireStack, RemoveSingleStackAndRefreshDuration, RefreshDuration\n"
			"Periodic inhibition policies: NeverReset, ResetPeriod, ExecuteAndResetPeriod\n";

		// ==================================================================
		// add(type, params)
		// ==================================================================
		BPObj.set_function("add", [GE, Blueprint, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- add("component", {type="..."}) ----
			if (FType.Equals(TEXT("component"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"component\") -> {type=\"...\"} required"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();
				std::string CompType = P.get_or<std::string>("type", "");
				if (CompType.empty())
				{
					Session.Log(TEXT("[FAIL] add(\"component\") -> 'type' required"));
					return sol::lua_nil;
				}

				FString FCompType = UTF8_TO_TCHAR(CompType.c_str());
				UClass* CompClass = ResolveComponentClass(FCompType);
				if (!CompClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"component\") -> unknown type '%s'"), *FCompType));
					return sol::lua_nil;
				}

				// Check if component of this type already exists
				FGEComponentsAccessor Accessor(GE);
				if (!Accessor.IsValid())
				{
					Session.Log(TEXT("[FAIL] add(\"component\") -> cannot access GEComponents array"));
					return sol::lua_nil;
				}

				if (Accessor.FindByClass(CompClass))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"component\") -> '%s' already exists"), *FCompType));
					return sol::lua_nil;
				}

				FProperty* CompProp = GE->GetClass()->FindPropertyByName(TEXT("GEComponents"));
				const FScopedTransaction Tx(FText::FromString(TEXT("Add GE Component")));
				PreEditGE(GE, CompProp);

				UGameplayEffectComponent* NewComp = NewObject<UGameplayEffectComponent>(
					GE, CompClass, NAME_None,
					GE->GetMaskedFlags(RF_PropagateToSubObjects) | RF_Transactional);

				Accessor.AddComponent(NewComp);

				PostEditGE(GE, CompProp);

				Session.Log(FString::Printf(TEXT("[OK] add(\"component\", type=\"%s\")"), *FCompType));
				return sol::make_object(Lua, true);
			}

			// ---- add("modifier", {attribute="...", op="...", value=...}) ----
			if (FType.Equals(TEXT("modifier"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"modifier\") -> params required"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();

				// Parse attribute
				std::string AttrStr = P.get_or<std::string>("attribute", "");
				FGameplayAttribute Attribute;
				if (!AttrStr.empty())
				{
					FString Error;
					if (!ConstructGameplayAttribute(UTF8_TO_TCHAR(AttrStr.c_str()), Attribute, Error))
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"modifier\") -> %s"), *Error));
						return sol::lua_nil;
					}
				}

				// Parse operation
				EGameplayModOp::Type ModOp = EGameplayModOp::Additive;
				std::string OpStr = P.get_or<std::string>("op", "Add");
				if (!ParseModifierOp(UTF8_TO_TCHAR(OpStr.c_str()), ModOp))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"modifier\") -> unknown op '%s'. Valid: Add, Multiply, Divide, Override, MultiplyCompound, AddFinal"),
						UTF8_TO_TCHAR(OpStr.c_str())));
					return sol::lua_nil;
				}

				// Parse magnitude
				EGameplayEffectMagnitudeCalculation MagType = EGameplayEffectMagnitudeCalculation::ScalableFloat;
				float ScalableValue = 0.f;
				FString SetByCallerTag;
				FString BackingAttrStr;
				float Coefficient = 1.f;

				sol::optional<sol::table> MagTable = P.get<sol::optional<sol::table>>("magnitude");
				if (MagTable.has_value())
				{
					sol::table M = MagTable.value();
					std::string MagTypeStr = M.get_or<std::string>("type", "ScalableFloat");
					FString FMagType = UTF8_TO_TCHAR(MagTypeStr.c_str());

					if (FMagType.Equals(TEXT("ScalableFloat"), ESearchCase::IgnoreCase))
					{
						MagType = EGameplayEffectMagnitudeCalculation::ScalableFloat;
						ScalableValue = M.get<sol::optional<float>>("value").value_or(0.f);
					}
					else if (FMagType.Equals(TEXT("AttributeBased"), ESearchCase::IgnoreCase))
					{
						MagType = EGameplayEffectMagnitudeCalculation::AttributeBased;
						BackingAttrStr = UTF8_TO_TCHAR(M.get_or<std::string>("backing_attribute", "").c_str());
						Coefficient = M.get<sol::optional<float>>("coefficient").value_or(1.f);
					}
					else if (FMagType.Equals(TEXT("SetByCaller"), ESearchCase::IgnoreCase))
					{
						MagType = EGameplayEffectMagnitudeCalculation::SetByCaller;
						SetByCallerTag = UTF8_TO_TCHAR(M.get_or<std::string>("data_tag", "").c_str());
					}
					else if (FMagType.Equals(TEXT("CustomCalculation"), ESearchCase::IgnoreCase))
					{
						MagType = EGameplayEffectMagnitudeCalculation::CustomCalculationClass;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"modifier\") -> unknown magnitude type '%s'"), *FMagType));
						return sol::lua_nil;
					}
				}
				else
				{
					// Simple value= param → ScalableFloat
					ScalableValue = P.get<sol::optional<float>>("value").value_or(0.f);
				}

				// Apply to CDO
				FProperty* ModProp = GE->GetClass()->FindPropertyByName(TEXT("Modifiers"));
				const FScopedTransaction Tx(FText::FromString(TEXT("Add GE Modifier")));
				PreEditGE(GE, ModProp);

				int32 NewIdx = GE->Modifiers.AddDefaulted();
				FGameplayModifierInfo& NewMod = GE->Modifiers[NewIdx];
				NewMod.Attribute = Attribute;
				NewMod.ModifierOp = ModOp;

				// Set magnitude using public constructors (fields are protected)
				if (MagType == EGameplayEffectMagnitudeCalculation::ScalableFloat)
				{
					FScalableFloat SF;
					SF.SetValue(ScalableValue);
					NewMod.ModifierMagnitude = FGameplayEffectModifierMagnitude(SF);
				}
				else if (MagType == EGameplayEffectMagnitudeCalculation::SetByCaller)
				{
					FSetByCallerFloat SBC;
					if (!SetByCallerTag.IsEmpty())
					{
						FGameplayTag Tag = ValidateTag(SetByCallerTag);
						if (Tag.IsValid())
						{
							SBC.DataTag = Tag;
						}
						else
						{
							SBC.DataName = FName(*SetByCallerTag);
						}
					}
					NewMod.ModifierMagnitude = FGameplayEffectModifierMagnitude(SBC);
				}
				else if (MagType == EGameplayEffectMagnitudeCalculation::AttributeBased)
				{
					FAttributeBasedFloat ABF;
					ABF.Coefficient.SetValue(Coefficient);
					if (!BackingAttrStr.IsEmpty())
					{
						FGameplayAttribute BackingAttr;
						FString Error;
						if (ConstructGameplayAttribute(BackingAttrStr, BackingAttr, Error))
						{
							ABF.BackingAttribute.AttributeToCapture = BackingAttr;
						}
						else
						{
							Session.Log(FString::Printf(TEXT("[WARN] add(\"modifier\") -> backing_attribute: %s"), *Error));
						}
					}

					if (MagTable.has_value())
					{
						sol::table M = MagTable.value();

						// capture_source (Source or Target), snapshot
						std::string CaptureSourceStr = M.get_or<std::string>("capture_source", "");
						if (!CaptureSourceStr.empty())
						{
							FString FCS = UTF8_TO_TCHAR(CaptureSourceStr.c_str());
							if (FCS.Equals(TEXT("Source"), ESearchCase::IgnoreCase))
								ABF.BackingAttribute.AttributeSource = EGameplayEffectAttributeCaptureSource::Source;
							else if (FCS.Equals(TEXT("Target"), ESearchCase::IgnoreCase))
								ABF.BackingAttribute.AttributeSource = EGameplayEffectAttributeCaptureSource::Target;
							else
								Session.Log(FString::Printf(TEXT("[WARN] add(\"modifier\") -> unknown capture_source '%s'. Valid: Source, Target"), *FCS));
						}
						sol::optional<bool> SnapshotOpt = M.get<sol::optional<bool>>("snapshot");
						if (SnapshotOpt.has_value())
						{
							ABF.BackingAttribute.bSnapshot = SnapshotOpt.value();
						}

						// pre_multiply_additive, post_multiply_additive
						sol::optional<double> PreMultOpt = M.get<sol::optional<double>>("pre_multiply_additive");
						if (PreMultOpt.has_value())
						{
							ABF.PreMultiplyAdditiveValue.SetValue((float)PreMultOpt.value());
						}
						sol::optional<double> PostMultOpt = M.get<sol::optional<double>>("post_multiply_additive");
						if (PostMultOpt.has_value())
						{
							ABF.PostMultiplyAdditiveValue.SetValue((float)PostMultOpt.value());
						}

						// attribute_calculation_type
						std::string CalcTypeStr = M.get_or<std::string>("attribute_calculation_type", "");
						if (!CalcTypeStr.empty())
						{
							FString FCalcType = UTF8_TO_TCHAR(CalcTypeStr.c_str());
							if (FCalcType.Equals(TEXT("AttributeMagnitude"), ESearchCase::IgnoreCase))
								ABF.AttributeCalculationType = EAttributeBasedFloatCalculationType::AttributeMagnitude;
							else if (FCalcType.Equals(TEXT("AttributeBaseValue"), ESearchCase::IgnoreCase))
								ABF.AttributeCalculationType = EAttributeBasedFloatCalculationType::AttributeBaseValue;
							else if (FCalcType.Equals(TEXT("AttributeBonusMagnitude"), ESearchCase::IgnoreCase))
								ABF.AttributeCalculationType = EAttributeBasedFloatCalculationType::AttributeBonusMagnitude;
							else if (FCalcType.Equals(TEXT("AttributeMagnitudeEvaluatedUpToChannel"), ESearchCase::IgnoreCase))
								ABF.AttributeCalculationType = EAttributeBasedFloatCalculationType::AttributeMagnitudeEvaluatedUpToChannel;
							else
								Session.Log(FString::Printf(TEXT("[WARN] add(\"modifier\") -> unknown attribute_calculation_type '%s'"), *FCalcType));
						}

						// source_tag_filter, target_tag_filter
						sol::optional<sol::table> SrcFilterOpt = M.get<sol::optional<sol::table>>("source_tag_filter");
						if (SrcFilterOpt.has_value())
						{
							FString Error;
							if (!ParseTagArray(SrcFilterOpt.value(), ABF.SourceTagFilter, Error))
							{
								Session.Log(FString::Printf(TEXT("[WARN] add(\"modifier\") -> source_tag_filter: %s"), *Error));
							}
						}
						sol::optional<sol::table> TgtFilterOpt = M.get<sol::optional<sol::table>>("target_tag_filter");
						if (TgtFilterOpt.has_value())
						{
							FString Error;
							if (!ParseTagArray(TgtFilterOpt.value(), ABF.TargetTagFilter, Error))
							{
								Session.Log(FString::Printf(TEXT("[WARN] add(\"modifier\") -> target_tag_filter: %s"), *Error));
							}
						}
					}

					NewMod.ModifierMagnitude = FGameplayEffectModifierMagnitude(ABF);
				}
				else if (MagType == EGameplayEffectMagnitudeCalculation::CustomCalculationClass)
				{
					FCustomCalculationBasedFloat Custom;

					if (MagTable.has_value())
					{
						sol::table M = MagTable.value();

						// calculation_class (string class path)
						std::string CalcClassStr = M.get_or<std::string>("calculation_class", "");
						if (!CalcClassStr.empty())
						{
							FString FCalcClassStr = UTF8_TO_TCHAR(CalcClassStr.c_str());
							UClass* CalcCls = LoadClass<UGameplayModMagnitudeCalculation>(nullptr, *FCalcClassStr);
							if (!CalcCls)
							{
								CalcCls = FindFirstObject<UClass>(*FCalcClassStr, EFindFirstObjectOptions::NativeFirst);
							}
							if (CalcCls && CalcCls->IsChildOf(UGameplayModMagnitudeCalculation::StaticClass()))
							{
								Custom.CalculationClassMagnitude = CalcCls;
							}
							else
							{
								Session.Log(FString::Printf(TEXT("[WARN] add(\"modifier\") -> calculation_class '%s' not found or not a UGameplayModMagnitudeCalculation subclass"), *FCalcClassStr));
							}
						}

						// coefficient, pre_multiply_additive, post_multiply_additive
						sol::optional<double> CoeffOpt = M.get<sol::optional<double>>("coefficient");
						if (CoeffOpt.has_value())
						{
							Custom.Coefficient.SetValue((float)CoeffOpt.value());
						}
						sol::optional<double> PreMultOpt = M.get<sol::optional<double>>("pre_multiply_additive");
						if (PreMultOpt.has_value())
						{
							Custom.PreMultiplyAdditiveValue.SetValue((float)PreMultOpt.value());
						}
						sol::optional<double> PostMultOpt = M.get<sol::optional<double>>("post_multiply_additive");
						if (PostMultOpt.has_value())
						{
							Custom.PostMultiplyAdditiveValue.SetValue((float)PostMultOpt.value());
						}
					}

					NewMod.ModifierMagnitude = FGameplayEffectModifierMagnitude(Custom);
				}

				// Modifier source_tags / target_tags
				if (Params.has_value())
				{
					sol::table P2 = Params.value();
					sol::optional<sol::table> SrcTagsOpt = P2.get<sol::optional<sol::table>>("source_tags");
					if (SrcTagsOpt.has_value())
					{
						FString Error;
						if (!ParseTagRequirements(SrcTagsOpt.value(), NewMod.SourceTags, Error))
						{
							Session.Log(FString::Printf(TEXT("[WARN] add(\"modifier\") -> source_tags: %s"), *Error));
						}
					}
					sol::optional<sol::table> TgtTagsOpt = P2.get<sol::optional<sol::table>>("target_tags");
					if (TgtTagsOpt.has_value())
					{
						FString Error;
						if (!ParseTagRequirements(TgtTagsOpt.value(), NewMod.TargetTags, Error))
						{
							Session.Log(FString::Printf(TEXT("[WARN] add(\"modifier\") -> target_tags: %s"), *Error));
						}
					}
				}

				PostEditGE(GE, ModProp);

				FString AttrName = Attribute.IsValid() ? Attribute.GetName() : TEXT("(none)");
				Session.Log(FString::Printf(TEXT("[OK] add(\"modifier\", attribute=\"%s\", op=\"%s\") -> index %d"),
					*AttrName, *FormatModOp(ModOp), NewIdx + 1));
				return sol::make_object(Lua, NewIdx + 1);
			}

			// ---- add("tag", {component="...", tag="..."}) ----
			if (FType.Equals(TEXT("tag"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"tag\") -> {component=\"...\", tag=\"...\"} required"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();
				std::string CompName = P.get_or<std::string>("component", "");
				std::string TagStr = P.get_or<std::string>("tag", "");
				if (CompName.empty() || TagStr.empty())
				{
					Session.Log(TEXT("[FAIL] add(\"tag\") -> both 'component' and 'tag' required"));
					return sol::lua_nil;
				}

				FString FCompName = UTF8_TO_TCHAR(CompName.c_str());
				FString FTagStr = UTF8_TO_TCHAR(TagStr.c_str());

				// Validate tag
				FGameplayTag Tag = ValidateTag(FTagStr);
				if (!Tag.IsValid())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"tag\") -> tag '%s' not found in project registry"), *FTagStr));
					return sol::lua_nil;
				}

				// Find component
				UClass* CompClass = ResolveComponentClass(FCompName);
				if (!CompClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"tag\") -> unknown component type '%s'"), *FCompName));
					return sol::lua_nil;
				}

				FGEComponentsAccessor Accessor(GE);
				UGameplayEffectComponent* Comp = Accessor.FindByClass(CompClass);
				if (!Comp)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"tag\") -> component '%s' not found on this GE. Add it first."), *FCompName));
					return sol::lua_nil;
				}

				// Find the first FInheritedTagContainer or FGameplayTagContainer property and add the tag
				const FScopedTransaction Tx(FText::FromString(TEXT("Add GE Tag")));
				Comp->SetFlags(RF_Transactional);
				Comp->Modify();

				bool bAdded = false;
				for (TFieldIterator<FStructProperty> PropIt(Comp->GetClass()); PropIt; ++PropIt)
				{
					FStructProperty* StructProp = *PropIt;
					if (!StructProp) continue;

					if (StructProp->Struct->GetFName() == FName("InheritedTagContainer"))
					{
						FInheritedTagContainer* Container = StructProp->ContainerPtrToValuePtr<FInheritedTagContainer>(Comp);
						if (Container)
						{
							Container->AddTag(Tag);
							bAdded = true;
							break;
						}
					}
					else if (StructProp->Struct->GetFName() == FName("GameplayTagContainer"))
					{
						FGameplayTagContainer* Container = StructProp->ContainerPtrToValuePtr<FGameplayTagContainer>(Comp);
						if (Container)
						{
							Container->AddTag(Tag);
							bAdded = true;
							break;
						}
					}
				}

				if (!bAdded)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"tag\") -> no tag container found on component '%s'"), *FCompName));
					return sol::lua_nil;
				}

				FPropertyChangedEvent Evt(nullptr, EPropertyChangeType::ValueSet);
				Comp->PostEditChangeProperty(Evt);
				GE->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add(\"tag\", component=\"%s\", tag=\"%s\")"), *FCompName, *FTagStr));
				return sol::make_object(Lua, true);
			}

			// ---- add("cue", {tag="...", magnitude_attribute="..."}) ----
			if (FType.Equals(TEXT("cue"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"cue\") -> {tag=\"...\"} required"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();
				std::string TagStr = P.get_or<std::string>("tag", "");
				if (TagStr.empty())
				{
					Session.Log(TEXT("[FAIL] add(\"cue\") -> 'tag' required"));
					return sol::lua_nil;
				}

				FString FTagStr = UTF8_TO_TCHAR(TagStr.c_str());
				FGameplayTag Tag = ValidateTag(FTagStr);
				if (!Tag.IsValid())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"cue\") -> tag '%s' not found in project registry"), *FTagStr));
					return sol::lua_nil;
				}

				FProperty* CueProp = GE->GetClass()->FindPropertyByName(TEXT("GameplayCues"));
				const FScopedTransaction Tx(FText::FromString(TEXT("Add GE Cue")));
				PreEditGE(GE, CueProp);

				FGameplayEffectCue NewCue;
				NewCue.GameplayCueTags.AddTag(Tag);

				// Optional magnitude attribute
				std::string MagAttrStr = P.get_or<std::string>("magnitude_attribute", "");
				if (!MagAttrStr.empty())
				{
					FGameplayAttribute MagAttr;
					FString Error;
					if (ConstructGameplayAttribute(UTF8_TO_TCHAR(MagAttrStr.c_str()), MagAttr, Error))
					{
						NewCue.MagnitudeAttribute = MagAttr;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] add(\"cue\") -> magnitude_attribute: %s"), *Error));
					}
				}

				// Optional min/max level
				sol::optional<double> MinLevelOpt = P.get<sol::optional<double>>("min_level");
				if (MinLevelOpt.has_value())
				{
					NewCue.MinLevel = (float)MinLevelOpt.value();
				}
				sol::optional<double> MaxLevelOpt = P.get<sol::optional<double>>("max_level");
				if (MaxLevelOpt.has_value())
				{
					NewCue.MaxLevel = (float)MaxLevelOpt.value();
				}

				GE->GameplayCues.Add(NewCue);
				PostEditGE(GE, CueProp);

				Session.Log(FString::Printf(TEXT("[OK] add(\"cue\", tag=\"%s\") -> index %d"), *FTagStr, GE->GameplayCues.Num()));
				return sol::make_object(Lua, GE->GameplayCues.Num());
			}

			// ---- add("execution", {class="..."}) ----
			if (FType.Equals(TEXT("execution"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"execution\") -> {class=\"...\"} required"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();
				std::string ClassStr = P.get_or<std::string>("class", "");
				if (ClassStr.empty())
				{
					Session.Log(TEXT("[FAIL] add(\"execution\") -> 'class' required"));
					return sol::lua_nil;
				}

				FString FClassStr = UTF8_TO_TCHAR(ClassStr.c_str());
				UClass* CalcClass = LoadObject<UClass>(nullptr, *FClassStr);
				if (!CalcClass)
				{
					CalcClass = FindFirstObject<UClass>(*FClassStr, EFindFirstObjectOptions::NativeFirst);
				}
				if (!CalcClass || !CalcClass->IsChildOf(UGameplayEffectExecutionCalculation::StaticClass()))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"execution\") -> class '%s' not found or not a UGameplayEffectExecutionCalculation subclass"), *FClassStr));
					return sol::lua_nil;
				}

				FProperty* ExecProp = GE->GetClass()->FindPropertyByName(TEXT("Executions"));
				const FScopedTransaction Tx(FText::FromString(TEXT("Add GE Execution")));
				PreEditGE(GE, ExecProp);

				FGameplayEffectExecutionDefinition NewExec;
				NewExec.CalculationClass = CalcClass;

				// Optional passed-in tags
				sol::optional<sol::table> PassedTagsOpt = P.get<sol::optional<sol::table>>("passed_in_tags");
				if (PassedTagsOpt.has_value())
				{
					FString Error;
					if (!ParseTagArray(PassedTagsOpt.value(), NewExec.PassedInTags, Error))
					{
						Session.Log(FString::Printf(TEXT("[WARN] add(\"execution\") -> passed_in_tags: %s"), *Error));
					}
				}

				GE->Executions.Add(NewExec);

				PostEditGE(GE, ExecProp);

				Session.Log(FString::Printf(TEXT("[OK] add(\"execution\", class=\"%s\") -> index %d"), *FClassStr, GE->Executions.Num()));
				return sol::make_object(Lua, GE->Executions.Num());
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: component, modifier, tag, cue, execution"), *FType));
			return sol::lua_nil;
		});

		// ==================================================================
		// remove(type, id)
		// ==================================================================
		BPObj.set_function("remove", [GE, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- remove("component", {type="..."} or index) ----
			if (FType.Equals(TEXT("component"), ESearchCase::IgnoreCase))
			{
				FGEComponentsAccessor Accessor(GE);
				if (!Accessor.IsValid())
				{
					Session.Log(TEXT("[FAIL] remove(\"component\") -> cannot access GEComponents array"));
					return sol::lua_nil;
				}

				int32 RemoveIdx = INDEX_NONE;
				FString IdDesc;

				if (Id.is<sol::table>())
				{
					sol::table P = Id.as<sol::table>();
					std::string CompType = P.get_or<std::string>("type", "");
					if (CompType.empty())
					{
						Session.Log(TEXT("[FAIL] remove(\"component\") -> {type=\"...\"} or index required"));
						return sol::lua_nil;
					}
					FString FCompType = UTF8_TO_TCHAR(CompType.c_str());
					UClass* CompClass = ResolveComponentClass(FCompType);
					if (!CompClass)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] remove(\"component\") -> unknown type '%s'"), *FCompType));
						return sol::lua_nil;
					}
					RemoveIdx = Accessor.FindIndexByClass(CompClass);
					IdDesc = FCompType;
				}
				else if (Id.is<int>() || Id.is<double>())
				{
					int32 LuaIdx = Id.is<int>() ? Id.as<int>() : (int32)Id.as<double>();
					RemoveIdx = LuaIdx - 1; // 1-based to 0-based
					IdDesc = FString::Printf(TEXT("index %d"), LuaIdx);
				}
				else
				{
					Session.Log(TEXT("[FAIL] remove(\"component\") -> {type=\"...\"} or 1-based index required"));
					return sol::lua_nil;
				}

				if (RemoveIdx < 0 || RemoveIdx >= Accessor.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"component\", %s) -> not found"), *IdDesc));
					return sol::lua_nil;
				}

				FProperty* CompProp = GE->GetClass()->FindPropertyByName(TEXT("GEComponents"));
				const FScopedTransaction Tx(FText::FromString(TEXT("Remove GE Component")));
				PreEditGE(GE, CompProp);
				Accessor.RemoveAt(RemoveIdx);
				PostEditGE(GE, CompProp);

				Session.Log(FString::Printf(TEXT("[OK] remove(\"component\", %s)"), *IdDesc));
				return sol::make_object(Lua, true);
			}

			// ---- remove("modifier", index) ----
			if (FType.Equals(TEXT("modifier"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<int>() && !Id.is<double>())
				{
					Session.Log(TEXT("[FAIL] remove(\"modifier\") -> 1-based index required"));
					return sol::lua_nil;
				}
				int32 LuaIdx = Id.is<int>() ? Id.as<int>() : (int32)Id.as<double>();
				int32 Idx = LuaIdx - 1;

				if (Idx < 0 || Idx >= GE->Modifiers.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"modifier\", %d) -> out of range (count=%d)"), LuaIdx, GE->Modifiers.Num()));
					return sol::lua_nil;
				}

				FProperty* ModProp = GE->GetClass()->FindPropertyByName(TEXT("Modifiers"));
				const FScopedTransaction Tx(FText::FromString(TEXT("Remove GE Modifier")));
				PreEditGE(GE, ModProp);
				GE->Modifiers.RemoveAt(Idx);
				PostEditGE(GE, ModProp);

				Session.Log(FString::Printf(TEXT("[OK] remove(\"modifier\", %d)"), LuaIdx));
				return sol::make_object(Lua, true);
			}

			// ---- remove("tag", {component="...", tag="..."}) ----
			if (FType.Equals(TEXT("tag"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] remove(\"tag\") -> {component=\"...\", tag=\"...\"} required"));
					return sol::lua_nil;
				}
				sol::table P = Id.as<sol::table>();
				std::string CompName = P.get_or<std::string>("component", "");
				std::string TagStr = P.get_or<std::string>("tag", "");
				if (CompName.empty() || TagStr.empty())
				{
					Session.Log(TEXT("[FAIL] remove(\"tag\") -> both 'component' and 'tag' required"));
					return sol::lua_nil;
				}

				FString FCompName = UTF8_TO_TCHAR(CompName.c_str());
				FString FTagStr = UTF8_TO_TCHAR(TagStr.c_str());
				FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*FTagStr), false);

				UClass* CompClass = ResolveComponentClass(FCompName);
				if (!CompClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"tag\") -> unknown component type '%s'"), *FCompName));
					return sol::lua_nil;
				}

				FGEComponentsAccessor Accessor(GE);
				UGameplayEffectComponent* Comp = Accessor.FindByClass(CompClass);
				if (!Comp)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"tag\") -> component '%s' not found"), *FCompName));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(FText::FromString(TEXT("Remove GE Tag")));
				Comp->SetFlags(RF_Transactional);
				Comp->Modify();

				bool bRemoved = false;
				for (TFieldIterator<FStructProperty> PropIt(Comp->GetClass()); PropIt; ++PropIt)
				{
					FStructProperty* StructProp = *PropIt;
					if (!StructProp) continue;

					if (StructProp->Struct->GetFName() == FName("InheritedTagContainer"))
					{
						FInheritedTagContainer* Container = StructProp->ContainerPtrToValuePtr<FInheritedTagContainer>(Comp);
						if (Container)
						{
							Container->RemoveTag(Tag);
							bRemoved = true;
							break;
						}
					}
					else if (StructProp->Struct->GetFName() == FName("GameplayTagContainer"))
					{
						FGameplayTagContainer* Container = StructProp->ContainerPtrToValuePtr<FGameplayTagContainer>(Comp);
						if (Container)
						{
							Container->RemoveTag(Tag);
							bRemoved = true;
							break;
						}
					}
				}

				if (bRemoved)
				{
					FPropertyChangedEvent Evt(nullptr, EPropertyChangeType::ValueSet);
					Comp->PostEditChangeProperty(Evt);
					GE->MarkPackageDirty();
				}

				Session.Log(FString::Printf(TEXT("[OK] remove(\"tag\", component=\"%s\", tag=\"%s\")"), *FCompName, *FTagStr));
				return sol::make_object(Lua, true);
			}

			// ---- remove("cue", index) ----
			if (FType.Equals(TEXT("cue"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<int>() && !Id.is<double>())
				{
					Session.Log(TEXT("[FAIL] remove(\"cue\") -> 1-based index required"));
					return sol::lua_nil;
				}
				int32 LuaIdx = Id.is<int>() ? Id.as<int>() : (int32)Id.as<double>();
				int32 Idx = LuaIdx - 1;

				if (Idx < 0 || Idx >= GE->GameplayCues.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"cue\", %d) -> out of range (count=%d)"), LuaIdx, GE->GameplayCues.Num()));
					return sol::lua_nil;
				}

				FProperty* CueProp = GE->GetClass()->FindPropertyByName(TEXT("GameplayCues"));
				const FScopedTransaction Tx(FText::FromString(TEXT("Remove GE Cue")));
				PreEditGE(GE, CueProp);
				GE->GameplayCues.RemoveAt(Idx);
				PostEditGE(GE, CueProp);

				Session.Log(FString::Printf(TEXT("[OK] remove(\"cue\", %d)"), LuaIdx));
				return sol::make_object(Lua, true);
			}

			// ---- remove("execution", index) ----
			if (FType.Equals(TEXT("execution"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<int>() && !Id.is<double>())
				{
					Session.Log(TEXT("[FAIL] remove(\"execution\") -> 1-based index required"));
					return sol::lua_nil;
				}
				int32 LuaIdx = Id.is<int>() ? Id.as<int>() : (int32)Id.as<double>();
				int32 Idx = LuaIdx - 1;

				if (Idx < 0 || Idx >= GE->Executions.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"execution\", %d) -> out of range (count=%d)"), LuaIdx, GE->Executions.Num()));
					return sol::lua_nil;
				}

				FProperty* ExecProp = GE->GetClass()->FindPropertyByName(TEXT("Executions"));
				const FScopedTransaction Tx(FText::FromString(TEXT("Remove GE Execution")));
				PreEditGE(GE, ExecProp);
				GE->Executions.RemoveAt(Idx);
				PostEditGE(GE, ExecProp);

				Session.Log(FString::Printf(TEXT("[OK] remove(\"execution\", %d)"), LuaIdx));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: component, modifier, tag, cue, execution"), *FType));
			return sol::lua_nil;
		});

		// ==================================================================
		// list(type)
		// ==================================================================
		BPObj.set_function("list", [GE, &Session](sol::table self,
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

			// ---- list("components") ----
			if (FType.Contains(TEXT("component"), ESearchCase::IgnoreCase))
			{
				FGEComponentsAccessor Accessor(GE);
				sol::table Result = Lua.create_table();

				if (Accessor.IsValid())
				{
					for (int32 i = 0; i < Accessor.Num(); i++)
					{
						UGameplayEffectComponent* Comp = Accessor.Get(i);
						sol::table E = Lua.create_table();
						E["index"] = i + 1;
						E["type"] = TCHAR_TO_UTF8(*GetComponentShortName(Comp));
						E["class"] = Comp ? TCHAR_TO_UTF8(*Comp->GetClass()->GetName()) : "(null)";

						// Extract tag info from the component
						if (Comp)
						{
							sol::table Tags = Lua.create_table();
							for (TFieldIterator<FStructProperty> PropIt(Comp->GetClass()); PropIt; ++PropIt)
							{
								FStructProperty* StructProp = *PropIt;
								if (!StructProp) continue;

								FString PropName = StructProp->GetName();

								if (StructProp->Struct->GetFName() == FName("InheritedTagContainer"))
								{
									const FInheritedTagContainer* Container = StructProp->ContainerPtrToValuePtr<FInheritedTagContainer>(Comp);
									if (Container && Container->CombinedTags.Num() > 0)
									{
										Tags[TCHAR_TO_UTF8(*PropName)] = TCHAR_TO_UTF8(*Container->CombinedTags.ToStringSimple());
									}
									else if (Container && Container->Added.Num() > 0)
									{
										Tags[TCHAR_TO_UTF8(*PropName)] = TCHAR_TO_UTF8(*Container->Added.ToStringSimple());
									}
								}
								else if (StructProp->Struct->GetFName() == FName("GameplayTagContainer"))
								{
									const FGameplayTagContainer* Container = StructProp->ContainerPtrToValuePtr<FGameplayTagContainer>(Comp);
									if (Container && Container->Num() > 0)
									{
										Tags[TCHAR_TO_UTF8(*PropName)] = TCHAR_TO_UTF8(*Container->ToStringSimple());
									}
								}
								else if (StructProp->Struct->GetFName() == FName("GameplayTagRequirements"))
								{
									const FGameplayTagRequirements* Reqs = StructProp->ContainerPtrToValuePtr<FGameplayTagRequirements>(Comp);
									if (Reqs && !Reqs->IsEmpty())
									{
										sol::table ReqTable = Lua.create_table();
										if (Reqs->RequireTags.Num() > 0)
											ReqTable["require"] = TCHAR_TO_UTF8(*Reqs->RequireTags.ToStringSimple());
										if (Reqs->IgnoreTags.Num() > 0)
											ReqTable["ignore"] = TCHAR_TO_UTF8(*Reqs->IgnoreTags.ToStringSimple());
										Tags[TCHAR_TO_UTF8(*PropName)] = ReqTable;
									}
								}
							}
							E["tags"] = Tags;
						}

						Result[i + 1] = E;
					}
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"components\") -> %d"), Accessor.Num()));
				return Result;
			}

			// ---- list("modifiers") ----
			if (FType.Contains(TEXT("modifier"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < GE->Modifiers.Num(); i++)
				{
					const FGameplayModifierInfo& Mod = GE->Modifiers[i];
					sol::table E = Lua.create_table();
					E["index"] = i + 1;
					E["attribute"] = Mod.Attribute.IsValid()
						? TCHAR_TO_UTF8(*Mod.Attribute.GetName())
						: "(none)";
					if (Mod.Attribute.IsValid() && Mod.Attribute.GetAttributeSetClass())
					{
						E["attribute_set"] = TCHAR_TO_UTF8(*Mod.Attribute.GetAttributeSetClass()->GetName());
					}
					E["op"] = TCHAR_TO_UTF8(*FormatModOp(Mod.ModifierOp));
					E["magnitude"] = TCHAR_TO_UTF8(*FormatMagnitude(Mod.ModifierMagnitude));
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"modifiers\") -> %d"), GE->Modifiers.Num()));
				return Result;
			}

			// ---- list("cues") ----
			if (FType.Contains(TEXT("cue"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < GE->GameplayCues.Num(); i++)
				{
					const FGameplayEffectCue& Cue = GE->GameplayCues[i];
					sol::table E = Lua.create_table();
					E["index"] = i + 1;
					E["tags"] = TCHAR_TO_UTF8(*Cue.GameplayCueTags.ToStringSimple());
					if (Cue.MagnitudeAttribute.IsValid())
					{
						E["magnitude_attribute"] = TCHAR_TO_UTF8(*Cue.MagnitudeAttribute.GetName());
					}
					E["min_level"] = Cue.MinLevel;
					E["max_level"] = Cue.MaxLevel;
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"cues\") -> %d"), GE->GameplayCues.Num()));
				return Result;
			}

			// ---- list("executions") ----
			if (FType.Contains(TEXT("execution"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < GE->Executions.Num(); i++)
				{
					const FGameplayEffectExecutionDefinition& Exec = GE->Executions[i];
					sol::table E = Lua.create_table();
					E["index"] = i + 1;
					if (Exec.CalculationClass)
					{
						E["class_name"] = TCHAR_TO_UTF8(*Exec.CalculationClass->GetName());
						E["class_path"] = TCHAR_TO_UTF8(*Exec.CalculationClass->GetPathName());
					}
					else
					{
						E["class_name"] = "(none)";
						E["class_path"] = "(none)";
					}
					if (Exec.PassedInTags.Num() > 0)
					{
						E["passed_in_tags"] = TCHAR_TO_UTF8(*Exec.PassedInTags.ToStringSimple());
					}
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"executions\") -> %d"), GE->Executions.Num()));
				return Result;
			}

			// ---- list("tags", {component="..."}) ----
			if (FType.Contains(TEXT("tag"), ESearchCase::IgnoreCase))
			{
				Session.Log(TEXT("[FAIL] list(\"tags\") -> use list(\"components\") to see tags on each component"));
				return sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: components, modifiers, cues, executions"), *FType));
			return sol::lua_nil;
		});

		// ==================================================================
		// configure(type, params)
		// ==================================================================
		BPObj.set_function("configure", [GE, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Param, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- configure("component", {type="...", ...}) ----
			if (FType.Equals(TEXT("component"), ESearchCase::IgnoreCase))
			{
				if (!Param.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"component\") -> table required with 'type' and config fields"));
					return sol::lua_nil;
				}
				sol::table P = Param.as<sol::table>();
				std::string CompType = P.get_or<std::string>("type", "");
				if (CompType.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"component\") -> 'type' required"));
					return sol::lua_nil;
				}

				FString FCompType = UTF8_TO_TCHAR(CompType.c_str());
				UClass* CompClass = ResolveComponentClass(FCompType);
				if (!CompClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"component\") -> unknown type '%s'"), *FCompType));
					return sol::lua_nil;
				}

				FGEComponentsAccessor Accessor(GE);
				UGameplayEffectComponent* Comp = Accessor.FindByClass(CompClass);
				if (!Comp)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"component\") -> '%s' not found on this GE. Add it first."), *FCompType));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(FText::FromString(TEXT("Configure GE Component")));
				Comp->SetFlags(RF_Transactional);
				Comp->Modify();

				bool bChanged = false;

				// ---- ChanceToApply: chance= ----
				sol::optional<double> Chance = P.get<sol::optional<double>>("chance");
				if (Chance.has_value())
				{
					FProperty* ChanceProp = Comp->GetClass()->FindPropertyByName(TEXT("ChanceToApplyToTarget"));
					if (ChanceProp)
					{
						FScalableFloat* SF = ChanceProp->ContainerPtrToValuePtr<FScalableFloat>(Comp);
						if (SF)
						{
							SF->SetValue((float)Chance.value());
							bChanged = true;
						}
					}
				}

				// ---- Tag-based components: various tag arrays ----
				// Helper lambda to set tags on a FGameplayTagRequirements field
				auto SetTagRequirements = [&](const char* PropName, const char* RequireKey, const char* IgnoreKey) -> bool
				{
					FStructProperty* StructProp = CastField<FStructProperty>(Comp->GetClass()->FindPropertyByName(FName(PropName)));
					if (!StructProp || StructProp->Struct->GetFName() != FName("GameplayTagRequirements")) return false;

					FGameplayTagRequirements* Reqs = StructProp->ContainerPtrToValuePtr<FGameplayTagRequirements>(Comp);
					if (!Reqs) return false;

					bool bModified = false;

					sol::optional<sol::table> ReqTags = P.get<sol::optional<sol::table>>(RequireKey);
					if (ReqTags.has_value())
					{
						Reqs->RequireTags.Reset();
						FString Error;
						if (!ParseTagArray(ReqTags.value(), Reqs->RequireTags, Error))
						{
							Session.Log(FString::Printf(TEXT("[FAIL] configure(\"component\") -> %s: %s"), UTF8_TO_TCHAR(RequireKey), *Error));
							return false;
						}
						bModified = true;
					}

					sol::optional<sol::table> IgnTags = P.get<sol::optional<sol::table>>(IgnoreKey);
					if (IgnTags.has_value())
					{
						Reqs->IgnoreTags.Reset();
						FString Error;
						if (!ParseTagArray(IgnTags.value(), Reqs->IgnoreTags, Error))
						{
							Session.Log(FString::Printf(TEXT("[FAIL] configure(\"component\") -> %s: %s"), UTF8_TO_TCHAR(IgnoreKey), *Error));
							return false;
						}
						bModified = true;
					}

					return bModified;
				};

				// TargetTagRequirements component
				if (SetTagRequirements("ApplicationTagRequirements", "application_require", "application_ignore"))
					bChanged = true;
				if (SetTagRequirements("OngoingTagRequirements", "ongoing_require", "ongoing_ignore"))
					bChanged = true;
				if (SetTagRequirements("RemovalTagRequirements", "removal_require", "removal_ignore"))
					bChanged = true;

				// Helper lambda for FInheritedTagContainer fields
				auto SetInheritedTags = [&](const char* PropName, const char* TagsKey) -> bool
				{
					FStructProperty* StructProp = CastField<FStructProperty>(Comp->GetClass()->FindPropertyByName(FName(PropName)));
					if (!StructProp || StructProp->Struct->GetFName() != FName("InheritedTagContainer")) return false;

					sol::optional<sol::table> Tags = P.get<sol::optional<sol::table>>(TagsKey);
					if (!Tags.has_value()) return false;

					FInheritedTagContainer* Container = StructProp->ContainerPtrToValuePtr<FInheritedTagContainer>(Comp);
					if (!Container) return false;

					Container->Added.Reset();
					FString Error;
					if (!ParseTagArray(Tags.value(), Container->Added, Error))
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"component\") -> %s: %s"), UTF8_TO_TCHAR(TagsKey), *Error));
						return false;
					}
					return true;
				};

				// AssetTags, TargetTags, BlockAbilityTags, CancelAbilityTags
				if (SetInheritedTags("InheritableAssetTags", "tags"))
					bChanged = true;
				if (SetInheritedTags("InheritableGrantedTagsContainer", "grant_tags"))
					bChanged = true;
				if (SetInheritedTags("InheritableBlockedAbilityTagsContainer", "block_tags"))
					bChanged = true;
				if (SetInheritedTags("InheritableCancelAbilitiesWithTagsContainer", "cancel_with_tags"))
					bChanged = true;
				if (SetInheritedTags("InheritableCancelAbilitiesWithoutTagsContainer", "cancel_without_tags"))
					bChanged = true;

				if (bChanged)
				{
					FPropertyChangedEvent Evt(nullptr, EPropertyChangeType::ValueSet);
					Comp->PostEditChangeProperty(Evt);
					GE->MarkPackageDirty();
				}

				Session.Log(FString::Printf(TEXT("[OK] configure(\"component\", type=\"%s\")"), *FCompType));
				return sol::make_object(Lua, true);
			}

			// ---- configure("modifier", {index=1, ...}) ----
			if (FType.Equals(TEXT("modifier"), ESearchCase::IgnoreCase))
			{
				if (!Param.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"modifier\") -> table required with 'index'"));
					return sol::lua_nil;
				}
				sol::table P = Param.as<sol::table>();
				int32 LuaIdx = P.get<sol::optional<int>>("index").value_or(0);
				if (LuaIdx <= 0)
				{
					Session.Log(TEXT("[FAIL] configure(\"modifier\") -> 'index' (1-based) required"));
					return sol::lua_nil;
				}
				int32 Idx = LuaIdx - 1;
				if (Idx >= GE->Modifiers.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"modifier\", index=%d) -> out of range (count=%d)"), LuaIdx, GE->Modifiers.Num()));
					return sol::lua_nil;
				}

				FProperty* ModProp = GE->GetClass()->FindPropertyByName(TEXT("Modifiers"));
				const FScopedTransaction Tx(FText::FromString(TEXT("Configure GE Modifier")));
				PreEditGE(GE, ModProp);

				FGameplayModifierInfo& Mod = GE->Modifiers[Idx];

				// Update attribute
				sol::optional<std::string> AttrOpt = P.get<sol::optional<std::string>>("attribute");
				if (AttrOpt.has_value())
				{
					FString Error;
					FGameplayAttribute NewAttr;
					if (ConstructGameplayAttribute(UTF8_TO_TCHAR(AttrOpt.value().c_str()), NewAttr, Error))
					{
						Mod.Attribute = NewAttr;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"modifier\") -> %s"), *Error));
						PostEditGE(GE, ModProp);
						return sol::lua_nil;
					}
				}

				// Update op
				sol::optional<std::string> OpOpt = P.get<sol::optional<std::string>>("op");
				if (OpOpt.has_value())
				{
					EGameplayModOp::Type NewOp;
					if (ParseModifierOp(UTF8_TO_TCHAR(OpOpt.value().c_str()), NewOp))
					{
						Mod.ModifierOp = NewOp;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"modifier\") -> unknown op '%s'"),
							UTF8_TO_TCHAR(OpOpt.value().c_str())));
						PostEditGE(GE, ModProp);
						return sol::lua_nil;
					}
				}

				// Update ScalableFloat value (replaces entire magnitude with a new ScalableFloat)
				sol::optional<double> ValueOpt = P.get<sol::optional<double>>("value");
				if (ValueOpt.has_value())
				{
					FScalableFloat SF;
					SF.SetValue((float)ValueOpt.value());
					Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(SF);
				}

				// Update source_tags / target_tags
				sol::optional<sol::table> SrcTagsOpt = P.get<sol::optional<sol::table>>("source_tags");
				if (SrcTagsOpt.has_value())
				{
					Mod.SourceTags = FGameplayTagRequirements();
					FString Error;
					if (!ParseTagRequirements(SrcTagsOpt.value(), Mod.SourceTags, Error))
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure(\"modifier\") -> source_tags: %s"), *Error));
					}
				}
				sol::optional<sol::table> TgtTagsOpt = P.get<sol::optional<sol::table>>("target_tags");
				if (TgtTagsOpt.has_value())
				{
					Mod.TargetTags = FGameplayTagRequirements();
					FString Error;
					if (!ParseTagRequirements(TgtTagsOpt.value(), Mod.TargetTags, Error))
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure(\"modifier\") -> target_tags: %s"), *Error));
					}
				}

				PostEditGE(GE, ModProp);

				Session.Log(FString::Printf(TEXT("[OK] configure(\"modifier\", index=%d)"), LuaIdx));
				return sol::make_object(Lua, true);
			}

			// ---- configure("duration", {policy="...", magnitude=..., period=..., execute_on_application=...}) ----
			if (FType.Equals(TEXT("duration"), ESearchCase::IgnoreCase))
			{
				if (!Param.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"duration\") -> table required"));
					return sol::lua_nil;
				}
				sol::table P = Param.as<sol::table>();

				const FScopedTransaction Tx(FText::FromString(TEXT("Configure GE Duration")));
				FProperty* DurProp = GE->GetClass()->FindPropertyByName(TEXT("DurationPolicy"));
				PreEditGE(GE, DurProp);

				// Duration policy
				sol::optional<std::string> PolicyOpt = P.get<sol::optional<std::string>>("policy");
				if (PolicyOpt.has_value())
				{
					FString PolicyStr = UTF8_TO_TCHAR(PolicyOpt.value().c_str());
					if (PolicyStr.Equals(TEXT("Instant"), ESearchCase::IgnoreCase))
						GE->DurationPolicy = EGameplayEffectDurationType::Instant;
					else if (PolicyStr.Equals(TEXT("Infinite"), ESearchCase::IgnoreCase))
						GE->DurationPolicy = EGameplayEffectDurationType::Infinite;
					else if (PolicyStr.Equals(TEXT("HasDuration"), ESearchCase::IgnoreCase))
						GE->DurationPolicy = EGameplayEffectDurationType::HasDuration;
					else
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"duration\") -> unknown policy '%s'. Valid: Instant, Infinite, HasDuration"), *PolicyStr));
						PostEditGE(GE, DurProp);
						return sol::lua_nil;
					}
				}

				// Duration magnitude
				sol::optional<double> MagOpt = P.get<sol::optional<double>>("magnitude");
				if (MagOpt.has_value())
				{
					FScalableFloat SF;
					SF.SetValue((float)MagOpt.value());
					GE->DurationMagnitude = FGameplayEffectModifierMagnitude(SF);
				}

				// Max duration magnitude
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				sol::optional<double> MaxMagOpt = P.get<sol::optional<double>>("max_magnitude");
				if (MaxMagOpt.has_value())
				{
					FScalableFloat SF;
					SF.SetValue((float)MaxMagOpt.value());
					GE->MaxDurationMagnitude = FGameplayEffectModifierMagnitude(SF);
				}
#endif

				// Period
				sol::optional<double> PeriodOpt = P.get<sol::optional<double>>("period");
				if (PeriodOpt.has_value())
				{
					GE->Period.SetValue((float)PeriodOpt.value());
				}

				// Execute on application
				sol::optional<bool> ExecOnAppOpt = P.get<sol::optional<bool>>("execute_on_application");
				if (ExecOnAppOpt.has_value())
				{
					GE->bExecutePeriodicEffectOnApplication = ExecOnAppOpt.value();
				}

				// Periodic inhibition removed policy
				sol::optional<std::string> InhibPolicyOpt = P.get<sol::optional<std::string>>("periodic_inhibition_policy");
				if (InhibPolicyOpt.has_value())
				{
					FString IPStr = UTF8_TO_TCHAR(InhibPolicyOpt.value().c_str());
					if (IPStr.Equals(TEXT("NeverReset"), ESearchCase::IgnoreCase))
						GE->PeriodicInhibitionPolicy = EGameplayEffectPeriodInhibitionRemovedPolicy::NeverReset;
					else if (IPStr.Equals(TEXT("ResetPeriod"), ESearchCase::IgnoreCase))
						GE->PeriodicInhibitionPolicy = EGameplayEffectPeriodInhibitionRemovedPolicy::ResetPeriod;
					else if (IPStr.Equals(TEXT("ExecuteAndResetPeriod"), ESearchCase::IgnoreCase))
						GE->PeriodicInhibitionPolicy = EGameplayEffectPeriodInhibitionRemovedPolicy::ExecuteAndResetPeriod;
					else
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"duration\") -> unknown periodic_inhibition_policy '%s'. Valid: NeverReset, ResetPeriod, ExecuteAndResetPeriod"), *IPStr));
						PostEditGE(GE, DurProp);
						return sol::lua_nil;
					}
				}

				PostEditGE(GE, DurProp);

				Session.Log(TEXT("[OK] configure(\"duration\")"));
				return sol::make_object(Lua, true);
			}

			// ---- configure("stacking", {type="...", limit=..., ...}) ----
			if (FType.Equals(TEXT("stacking"), ESearchCase::IgnoreCase))
			{
				if (!Param.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"stacking\") -> table required"));
					return sol::lua_nil;
				}
				sol::table P = Param.as<sol::table>();

				const FScopedTransaction Tx(FText::FromString(TEXT("Configure GE Stacking")));
				FProperty* StackProp = GE->GetClass()->FindPropertyByName(TEXT("StackingType"));
				PreEditGE(GE, StackProp);

				// Stacking type – set via reflection to avoid linker issues with
				// UGameplayEffect::SetStackingType() which is WITH_EDITOR-only and not exported.
				sol::optional<std::string> TypeOpt = P.get<sol::optional<std::string>>("type");
				if (TypeOpt.has_value())
				{
					FString TypeStr = UTF8_TO_TCHAR(TypeOpt.value().c_str());
					EGameplayEffectStackingType NewStackType;
					if (TypeStr.Equals(TEXT("None"), ESearchCase::IgnoreCase))
					{
						NewStackType = EGameplayEffectStackingType::None;
					}
					else if (TypeStr.Equals(TEXT("AggregateBySource"), ESearchCase::IgnoreCase))
					{
						NewStackType = EGameplayEffectStackingType::AggregateBySource;
					}
					else if (TypeStr.Equals(TEXT("AggregateByTarget"), ESearchCase::IgnoreCase))
					{
						NewStackType = EGameplayEffectStackingType::AggregateByTarget;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"stacking\") -> unknown type '%s'. Valid: None, AggregateBySource, AggregateByTarget"), *TypeStr));
						PostEditGE(GE, StackProp);
						return sol::lua_nil;
					}

					if (StackProp)
					{
						EGameplayEffectStackingType* ValuePtr = StackProp->ContainerPtrToValuePtr<EGameplayEffectStackingType>(GE);
						if (ValuePtr)
						{
							*ValuePtr = NewStackType;
						}
					}
				}

				// Stack limit
				sol::optional<int> LimitOpt = P.get<sol::optional<int>>("limit");
				if (LimitOpt.has_value())
				{
					GE->StackLimitCount = LimitOpt.value();
				}

				// Duration refresh policy
				sol::optional<std::string> DurRefreshOpt = P.get<sol::optional<std::string>>("duration_refresh");
				if (DurRefreshOpt.has_value())
				{
					FString DRStr = UTF8_TO_TCHAR(DurRefreshOpt.value().c_str());
					if (DRStr.Equals(TEXT("RefreshOnSuccessfulApplication"), ESearchCase::IgnoreCase))
						GE->StackDurationRefreshPolicy = EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication;
					else if (DRStr.Equals(TEXT("NeverRefresh"), ESearchCase::IgnoreCase))
						GE->StackDurationRefreshPolicy = EGameplayEffectStackingDurationPolicy::NeverRefresh;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
					else if (DRStr.Equals(TEXT("ExtendDuration"), ESearchCase::IgnoreCase))
						GE->StackDurationRefreshPolicy = EGameplayEffectStackingDurationPolicy::ExtendDuration;
#endif
					else
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"stacking\") -> unknown duration_refresh '%s'. Valid: RefreshOnSuccessfulApplication, NeverRefresh, ExtendDuration"), *DRStr));
						PostEditGE(GE, StackProp);
						return sol::lua_nil;
					}
				}

				// Period reset policy
				sol::optional<std::string> PeriodResetOpt = P.get<sol::optional<std::string>>("period_reset");
				if (PeriodResetOpt.has_value())
				{
					FString PRStr = UTF8_TO_TCHAR(PeriodResetOpt.value().c_str());
					if (PRStr.Equals(TEXT("ResetOnSuccessfulApplication"), ESearchCase::IgnoreCase))
						GE->StackPeriodResetPolicy = EGameplayEffectStackingPeriodPolicy::ResetOnSuccessfulApplication;
					else if (PRStr.Equals(TEXT("NeverReset"), ESearchCase::IgnoreCase))
						GE->StackPeriodResetPolicy = EGameplayEffectStackingPeriodPolicy::NeverReset;
					else
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"stacking\") -> unknown period_reset '%s'. Valid: ResetOnSuccessfulApplication, NeverReset"), *PRStr));
						PostEditGE(GE, StackProp);
						return sol::lua_nil;
					}
				}

				// Expiration policy
				sol::optional<std::string> ExpOpt = P.get<sol::optional<std::string>>("expiration");
				if (ExpOpt.has_value())
				{
					FString ExpStr = UTF8_TO_TCHAR(ExpOpt.value().c_str());
					if (ExpStr.Equals(TEXT("ClearEntireStack"), ESearchCase::IgnoreCase))
						GE->StackExpirationPolicy = EGameplayEffectStackingExpirationPolicy::ClearEntireStack;
					else if (ExpStr.Equals(TEXT("RemoveSingleStackAndRefreshDuration"), ESearchCase::IgnoreCase))
						GE->StackExpirationPolicy = EGameplayEffectStackingExpirationPolicy::RemoveSingleStackAndRefreshDuration;
					else if (ExpStr.Equals(TEXT("RefreshDuration"), ESearchCase::IgnoreCase))
						GE->StackExpirationPolicy = EGameplayEffectStackingExpirationPolicy::RefreshDuration;
					else
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"stacking\") -> unknown expiration '%s'. Valid: ClearEntireStack, RemoveSingleStackAndRefreshDuration, RefreshDuration"), *ExpStr));
						PostEditGE(GE, StackProp);
						return sol::lua_nil;
					}
				}

				// Overflow settings
				sol::optional<bool> DenyOverflowOpt = P.get<sol::optional<bool>>("deny_overflow_application");
				if (DenyOverflowOpt.has_value())
				{
					GE->bDenyOverflowApplication = DenyOverflowOpt.value();
				}

				sol::optional<bool> ClearOnOverflowOpt = P.get<sol::optional<bool>>("clear_stack_on_overflow");
				if (ClearOnOverflowOpt.has_value())
				{
					GE->bClearStackOnOverflow = ClearOnOverflowOpt.value();
				}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
				sol::optional<bool> FactorStackOpt = P.get<sol::optional<bool>>("factor_in_stack_count");
				if (FactorStackOpt.has_value())
				{
					GE->bFactorInStackCount = FactorStackOpt.value();
				}
#endif

				// overflow_effects (array of class path strings)
				sol::optional<sol::table> OverflowFxOpt = P.get<sol::optional<sol::table>>("overflow_effects");
				if (OverflowFxOpt.has_value())
				{
					GE->OverflowEffects.Empty();
					for (auto& Pair : OverflowFxOpt.value())
					{
						if (!Pair.second.is<std::string>()) continue;
						FString ClassPath = UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str());
						UClass* FxClass = LoadClass<UGameplayEffect>(nullptr, *ClassPath);
						if (!FxClass)
						{
							FxClass = FindFirstObject<UClass>(*ClassPath, EFindFirstObjectOptions::NativeFirst);
						}
						if (FxClass && FxClass->IsChildOf(UGameplayEffect::StaticClass()))
						{
							GE->OverflowEffects.Add(FxClass);
						}
						else
						{
							Session.Log(FString::Printf(TEXT("[WARN] configure(\"stacking\") -> overflow_effects: '%s' not found or not a UGameplayEffect subclass"), *ClassPath));
						}
					}
				}

				PostEditGE(GE, StackProp);

				Session.Log(TEXT("[OK] configure(\"stacking\")"));
				return sol::make_object(Lua, true);
			}

			// ---- configure("cues", {require_modifier_success=true, suppress_stacking_cues=false}) ----
			if (FType.Equals(TEXT("cues"), ESearchCase::IgnoreCase))
			{
				if (!Param.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"cues\") -> table required"));
					return sol::lua_nil;
				}
				sol::table P = Param.as<sol::table>();

				const FScopedTransaction Tx(FText::FromString(TEXT("Configure GE Cues")));
				FProperty* CueProp = GE->GetClass()->FindPropertyByName(TEXT("bRequireModifierSuccessToTriggerCues"));
				PreEditGE(GE, CueProp);

				sol::optional<bool> ReqModSuccessOpt = P.get<sol::optional<bool>>("require_modifier_success");
				if (ReqModSuccessOpt.has_value())
				{
					GE->bRequireModifierSuccessToTriggerCues = ReqModSuccessOpt.value();
				}

				sol::optional<bool> SuppressStackOpt = P.get<sol::optional<bool>>("suppress_stacking_cues");
				if (SuppressStackOpt.has_value())
				{
					GE->bSuppressStackingCues = SuppressStackOpt.value();
				}

				PostEditGE(GE, CueProp);

				Session.Log(TEXT("[OK] configure(\"cues\")"));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: component, modifier, duration, stacking, cues"), *FType));
			return sol::lua_nil;
		});

		// ==================================================================
		// info()
		// ==================================================================
		BPObj.set_function("info", [GE, Blueprint, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Info = Lua.create_table();

			Info["name"] = TCHAR_TO_UTF8(*Blueprint->GetName());
			Info["class"] = TCHAR_TO_UTF8(*GE->GetClass()->GetName());

			// Duration
			FString DurationStr;
			switch (GE->DurationPolicy)
			{
			case EGameplayEffectDurationType::Instant: DurationStr = TEXT("Instant"); break;
			case EGameplayEffectDurationType::Infinite: DurationStr = TEXT("Infinite"); break;
			case EGameplayEffectDurationType::HasDuration: DurationStr = TEXT("HasDuration"); break;
			default: DurationStr = TEXT("Unknown"); break;
			}
			Info["duration_policy"] = TCHAR_TO_UTF8(*DurationStr);

			if (GE->DurationPolicy == EGameplayEffectDurationType::HasDuration)
			{
				Info["duration_magnitude"] = TCHAR_TO_UTF8(*FormatMagnitude(GE->DurationMagnitude));

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				float MaxDurVal = 0.f;
				if (GE->MaxDurationMagnitude.GetStaticMagnitudeIfPossible(1.f, MaxDurVal) && MaxDurVal > 0.f)
				{
					Info["max_duration_magnitude"] = TCHAR_TO_UTF8(*FormatMagnitude(GE->MaxDurationMagnitude));
				}
#endif
			}

			float PeriodValue = GE->Period.GetValueAtLevel(1.f);
			if (PeriodValue > 0.f)
			{
				Info["period"] = PeriodValue;
				Info["execute_on_application"] = GE->bExecutePeriodicEffectOnApplication;

				FString InhibStr;
				switch (GE->PeriodicInhibitionPolicy)
				{
				case EGameplayEffectPeriodInhibitionRemovedPolicy::NeverReset: InhibStr = TEXT("NeverReset"); break;
				case EGameplayEffectPeriodInhibitionRemovedPolicy::ResetPeriod: InhibStr = TEXT("ResetPeriod"); break;
				case EGameplayEffectPeriodInhibitionRemovedPolicy::ExecuteAndResetPeriod: InhibStr = TEXT("ExecuteAndResetPeriod"); break;
				default: InhibStr = TEXT("Unknown"); break;
				}
				Info["periodic_inhibition_policy"] = TCHAR_TO_UTF8(*InhibStr);
			}

			// Modifiers
			Info["modifier_count"] = GE->Modifiers.Num();
			if (GE->Modifiers.Num() > 0)
			{
				sol::table Mods = Lua.create_table();
				for (int32 i = 0; i < GE->Modifiers.Num(); i++)
				{
					const FGameplayModifierInfo& Mod = GE->Modifiers[i];
					sol::table M = Lua.create_table();
					M["attribute"] = Mod.Attribute.IsValid() ? TCHAR_TO_UTF8(*Mod.Attribute.GetName()) : "(none)";
					M["op"] = TCHAR_TO_UTF8(*FormatModOp(Mod.ModifierOp));
					M["magnitude"] = TCHAR_TO_UTF8(*FormatMagnitude(Mod.ModifierMagnitude));
					Mods[i + 1] = M;
				}
				Info["modifiers"] = Mods;
			}

			// Components
			FGEComponentsAccessor Accessor(GE);
			Info["component_count"] = Accessor.Num();
			if (Accessor.Num() > 0)
			{
				sol::table Comps = Lua.create_table();
				for (int32 i = 0; i < Accessor.Num(); i++)
				{
					UGameplayEffectComponent* Comp = Accessor.Get(i);
					Comps[i + 1] = TCHAR_TO_UTF8(*GetComponentShortName(Comp));
				}
				Info["components"] = Comps;
			}

			// Executions
			Info["execution_count"] = GE->Executions.Num();

			// GameplayCues
			Info["cue_count"] = GE->GameplayCues.Num();
			Info["require_modifier_success_to_trigger_cues"] = GE->bRequireModifierSuccessToTriggerCues;
			Info["suppress_stacking_cues"] = GE->bSuppressStackingCues;

			// Stacking (read via reflection since StackingType is deprecated and GetStackingType() is not exported)
			{
				FProperty* StackTypeProp = GE->GetClass()->FindPropertyByName(TEXT("StackingType"));
				EGameplayEffectStackingType StackType = EGameplayEffectStackingType::None;
				if (StackTypeProp)
				{
					const EGameplayEffectStackingType* ValuePtr = StackTypeProp->ContainerPtrToValuePtr<EGameplayEffectStackingType>(GE);
					if (ValuePtr) StackType = *ValuePtr;
				}

				FString StackTypeStr;
				switch (StackType)
				{
				case EGameplayEffectStackingType::None: StackTypeStr = TEXT("None"); break;
				case EGameplayEffectStackingType::AggregateBySource: StackTypeStr = TEXT("AggregateBySource"); break;
				case EGameplayEffectStackingType::AggregateByTarget: StackTypeStr = TEXT("AggregateByTarget"); break;
				default: StackTypeStr = TEXT("Unknown"); break;
				}
				Info["stacking_type"] = TCHAR_TO_UTF8(*StackTypeStr);

				if (StackType != EGameplayEffectStackingType::None)
				{
					sol::table Stack = Lua.create_table();
					Stack["type"] = TCHAR_TO_UTF8(*StackTypeStr);
					Stack["limit"] = GE->StackLimitCount;

					FString DurRefreshStr;
					switch (GE->StackDurationRefreshPolicy)
					{
					case EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication: DurRefreshStr = TEXT("RefreshOnSuccessfulApplication"); break;
					case EGameplayEffectStackingDurationPolicy::NeverRefresh: DurRefreshStr = TEXT("NeverRefresh"); break;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
					case EGameplayEffectStackingDurationPolicy::ExtendDuration: DurRefreshStr = TEXT("ExtendDuration"); break;
#endif
					default: DurRefreshStr = TEXT("Unknown"); break;
					}
					Stack["duration_refresh"] = TCHAR_TO_UTF8(*DurRefreshStr);

					FString PeriodResetStr;
					switch (GE->StackPeriodResetPolicy)
					{
					case EGameplayEffectStackingPeriodPolicy::ResetOnSuccessfulApplication: PeriodResetStr = TEXT("ResetOnSuccessfulApplication"); break;
					case EGameplayEffectStackingPeriodPolicy::NeverReset: PeriodResetStr = TEXT("NeverReset"); break;
					default: PeriodResetStr = TEXT("Unknown"); break;
					}
					Stack["period_reset"] = TCHAR_TO_UTF8(*PeriodResetStr);

					FString ExpStr;
					switch (GE->StackExpirationPolicy)
					{
					case EGameplayEffectStackingExpirationPolicy::ClearEntireStack: ExpStr = TEXT("ClearEntireStack"); break;
					case EGameplayEffectStackingExpirationPolicy::RemoveSingleStackAndRefreshDuration: ExpStr = TEXT("RemoveSingleStackAndRefreshDuration"); break;
					case EGameplayEffectStackingExpirationPolicy::RefreshDuration: ExpStr = TEXT("RefreshDuration"); break;
					default: ExpStr = TEXT("Unknown"); break;
					}
					Stack["expiration"] = TCHAR_TO_UTF8(*ExpStr);

					Stack["deny_overflow_application"] = GE->bDenyOverflowApplication;
					Stack["clear_stack_on_overflow"] = GE->bClearStackOnOverflow;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
					Stack["factor_in_stack_count"] = GE->bFactorInStackCount;
#endif

					Info["stacking"] = Stack;
				}
			}

			Session.Log(FString::Printf(TEXT("[OK] info() -> %s: %s, %d modifiers, %d components"),
				*Blueprint->GetName(), *DurationStr, GE->Modifiers.Num(), Accessor.Num()));
			return Info;
		});

		// help() is handled by Blueprint's help() which reads _help_text
	});
}

REGISTER_LUA_BINDING(GameplayEffect, GameplayEffectDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("GameplayAbilities")))
	{
		Session.Log(TEXT("[WARN] GameplayAbilities plugin is not loaded. Enable it in Edit > Plugins to use this feature."));
		return;
	}
	BindGameplayEffect(Lua, Session);
});
