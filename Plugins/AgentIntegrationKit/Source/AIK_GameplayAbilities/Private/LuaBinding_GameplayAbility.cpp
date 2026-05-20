// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Engine/Blueprint.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "Modules/ModuleManager.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// Helpers
// ============================================================================

namespace
{

// ---- Reflection helpers to access protected UPROPERTY members ----

template<typename T>
T* GetPropertyPtr(UGameplayAbility* GA, const TCHAR* PropName)
{
	FProperty* Prop = FindFProperty<FProperty>(GA->GetClass(), PropName);
	if (Prop)
	{
		return Prop->ContainerPtrToValuePtr<T>(GA);
	}
	return nullptr;
}

static FGameplayTagContainer* GetTagContainer(UGameplayAbility* GA, const TCHAR* PropName)
{
	return GetPropertyPtr<FGameplayTagContainer>(GA, PropName);
}

static TArray<FAbilityTriggerData>* GetAbilityTriggers(UGameplayAbility* GA)
{
	return GetPropertyPtr<TArray<FAbilityTriggerData>>(GA, TEXT("AbilityTriggers"));
}

static TSubclassOf<UGameplayEffect>* GetCostEffectClass(UGameplayAbility* GA)
{
	return GetPropertyPtr<TSubclassOf<UGameplayEffect>>(GA, TEXT("CostGameplayEffectClass"));
}

static TSubclassOf<UGameplayEffect>* GetCooldownEffectClass(UGameplayAbility* GA)
{
	return GetPropertyPtr<TSubclassOf<UGameplayEffect>>(GA, TEXT("CooldownGameplayEffectClass"));
}

// ---- end reflection helpers ----

static void SetTagContainerFromLuaArray(FGameplayTagContainer& Container, const sol::table& Tags)
{
	Container.Reset();
	for (auto& Pair : Tags)
	{
		if (Pair.second.is<std::string>())
		{
			FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str())), false);
			if (Tag.IsValid())
				Container.AddTag(Tag);
		}
	}
}

static FString FormatInstancingPolicy(TEnumAsByte<EGameplayAbilityInstancingPolicy::Type> Policy)
{
	switch (Policy.GetValue())
	{
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	case EGameplayAbilityInstancingPolicy::NonInstanced: return TEXT("NonInstanced");
PRAGMA_ENABLE_DEPRECATION_WARNINGS
	case EGameplayAbilityInstancingPolicy::InstancedPerActor: return TEXT("InstancedPerActor");
	case EGameplayAbilityInstancingPolicy::InstancedPerExecution: return TEXT("InstancedPerExecution");
	default: return TEXT("Unknown");
	}
}

static FString FormatNetExecutionPolicy(TEnumAsByte<EGameplayAbilityNetExecutionPolicy::Type> Policy)
{
	switch (Policy.GetValue())
	{
	case EGameplayAbilityNetExecutionPolicy::LocalPredicted: return TEXT("LocalPredicted");
	case EGameplayAbilityNetExecutionPolicy::LocalOnly: return TEXT("LocalOnly");
	case EGameplayAbilityNetExecutionPolicy::ServerInitiated: return TEXT("ServerInitiated");
	case EGameplayAbilityNetExecutionPolicy::ServerOnly: return TEXT("ServerOnly");
	default: return TEXT("Unknown");
	}
}

static FString FormatReplicationPolicy(TEnumAsByte<EGameplayAbilityReplicationPolicy::Type> Policy)
{
	switch (Policy.GetValue())
	{
	case EGameplayAbilityReplicationPolicy::ReplicateNo: return TEXT("ReplicateNo");
	case EGameplayAbilityReplicationPolicy::ReplicateYes: return TEXT("ReplicateYes");
	default: return TEXT("Unknown");
	}
}

static FString FormatNetSecurityPolicy(TEnumAsByte<EGameplayAbilityNetSecurityPolicy::Type> Policy)
{
	switch (Policy.GetValue())
	{
	case EGameplayAbilityNetSecurityPolicy::ClientOrServer: return TEXT("ClientOrServer");
	case EGameplayAbilityNetSecurityPolicy::ServerOnlyExecution: return TEXT("ServerOnlyExecution");
	case EGameplayAbilityNetSecurityPolicy::ServerOnlyTermination: return TEXT("ServerOnlyTermination");
	case EGameplayAbilityNetSecurityPolicy::ServerOnly: return TEXT("ServerOnly");
	default: return TEXT("Unknown");
	}
}

static FString FormatTriggerSource(TEnumAsByte<EGameplayAbilityTriggerSource::Type> Source)
{
	switch (Source.GetValue())
	{
	case EGameplayAbilityTriggerSource::GameplayEvent: return TEXT("GameplayEvent");
	case EGameplayAbilityTriggerSource::OwnedTagAdded: return TEXT("OwnedTagAdded");
	case EGameplayAbilityTriggerSource::OwnedTagPresent: return TEXT("OwnedTagPresent");
	default: return TEXT("Unknown");
	}
}

static sol::table TagContainerToLuaArray(sol::state_view& Lua, const FGameplayTagContainer& Container)
{
	sol::table Result = Lua.create_table();
	int32 Idx = 1;
	for (const FGameplayTag& Tag : Container)
	{
		Result[Idx++] = TCHAR_TO_UTF8(*Tag.ToString());
	}
	return Result;
}

// Pre/Post edit helpers for CDO modification
static void PreEditGA(UGameplayAbility* GA, FProperty* Prop)
{
	GA->SetFlags(RF_Transactional);
	GA->Modify();
	if (Prop)
	{
		GA->PreEditChange(Prop);
	}
}

static void PostEditGA(UGameplayAbility* GA, FProperty* Prop)
{
	FPropertyChangedEvent Evt(Prop, EPropertyChangeType::ValueSet);
	GA->PostEditChangeProperty(Evt);
	GA->MarkPackageDirty();
}

} // anonymous namespace

// ============================================================================
// Registration
// ============================================================================

static TArray<FLuaFunctionDoc> GameplayAbilityDocs = {};

static void BindGameplayAbility(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_gameplay_ability", [&Session](sol::table BPObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = BPObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());

		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *FPath);
		if (!Blueprint || !Blueprint->GeneratedClass) return;

		UGameplayAbility* GA = Cast<UGameplayAbility>(Blueprint->GeneratedClass->GetDefaultObject());
		if (!GA) return;

		// ---- help text ----
		BPObj["_help_text"] =
			"GameplayAbility verbs (add/remove/list/configure/info/help):\n"
			"\n"
			"configure(type, params):\n"
			"  configure(\"policy\", {instancing=\"InstancedPerActor\", net_execution=\"LocalPredicted\",\n"
			"    replication=\"ReplicateYes\", net_security=\"ServerOnly\",\n"
			"    retrigger_instanced_ability=true, replicate_input_directly=false,\n"
			"    server_respects_remote_ability_cancellation=true})\n"
			"  configure(\"cost\", {effect=\"/Game/Path/To/GE_Cost.GE_Cost\"})\n"
			"  configure(\"cost\", {clear=true})  -- remove cost effect\n"
			"  configure(\"cooldown\", {effect=\"/Game/Path/To/GE_Cooldown.GE_Cooldown\"})\n"
			"  configure(\"cooldown\", {clear=true})  -- remove cooldown effect\n"
			"  configure(\"tags\", {ability_tags={\"Ability.Melee\"}, activation_required={\"Tag.One\"},\n"
			"    activation_blocked={\"Tag.Two\"}, cancel_abilities_with={...},\n"
			"    block_abilities_with={...}, activation_owned={...},\n"
			"    source_required={...}, source_blocked={...},\n"
			"    target_required={...}, target_blocked={...}})\n"
			"\n"
			"add(type, params):\n"
			"  add(\"trigger\", {tag=\"Event.Attack\", type=\"GameplayEvent\"})\n"
			"    type: GameplayEvent, OwnedTagAdded, OwnedTagPresent\n"
			"\n"
			"remove(type, id):\n"
			"  remove(\"trigger\", 1)  -- 1-based index\n"
			"\n"
			"list(type):\n"
			"  list(\"triggers\"), list(\"tags\")\n";

		// ==================================================================
		// info()
		// ==================================================================
		BPObj.set_function("info", [GA, Blueprint, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Info = Lua.create_table();

			Info["name"] = TCHAR_TO_UTF8(*Blueprint->GetName());
			Info["class_name"] = TCHAR_TO_UTF8(*GA->GetClass()->GetName());

			// Policies (these have public getters)
			Info["instancing_policy"] = TCHAR_TO_UTF8(*FormatInstancingPolicy(GA->GetInstancingPolicy()));
			Info["net_execution_policy"] = TCHAR_TO_UTF8(*FormatNetExecutionPolicy(GA->GetNetExecutionPolicy()));
			Info["replication_policy"] = TCHAR_TO_UTF8(*FormatReplicationPolicy(GA->GetReplicationPolicy()));

			// Net security policy (public getter)
			Info["net_security_policy"] = TCHAR_TO_UTF8(*FormatNetSecurityPolicy(GA->GetNetSecurityPolicy()));

			// Ability tags (public getter)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			const FGameplayTagContainer& AssetTags = GA->GetAssetTags();
			if (AssetTags.Num() > 0)
			{
				Info["ability_tags"] = TagContainerToLuaArray(Lua, AssetTags);
			}
#endif

			// Bool flags (via reflection)
			if (auto* V = GetPropertyPtr<bool>(GA, TEXT("bRetriggerInstancedAbility")))
				Info["retrigger_instanced_ability"] = *V;
			if (auto* V = GetPropertyPtr<bool>(GA, TEXT("bReplicateInputDirectly")))
				Info["replicate_input_directly"] = *V;
			if (auto* V = GetPropertyPtr<bool>(GA, TEXT("bServerRespectsRemoteAbilityCancellation")))
				Info["server_respects_remote_ability_cancellation"] = *V;

			// Cost effect (via reflection)
			TSubclassOf<UGameplayEffect>* CostClass = GetCostEffectClass(GA);
			if (CostClass && *CostClass)
			{
				Info["cost_effect"] = TCHAR_TO_UTF8(*(*CostClass)->GetName());
			}

			// Cooldown effect (via reflection)
			TSubclassOf<UGameplayEffect>* CooldownClass = GetCooldownEffectClass(GA);
			if (CooldownClass && *CooldownClass)
			{
				Info["cooldown_effect"] = TCHAR_TO_UTF8(*(*CooldownClass)->GetName());
			}

			// Triggers (via reflection)
			TArray<FAbilityTriggerData>* Triggers = GetAbilityTriggers(GA);
			Info["trigger_count"] = Triggers ? Triggers->Num() : 0;

			// Tag containers (via reflection)
			struct FTagMapping { const char* Key; const TCHAR* PropName; };
			static const FTagMapping TagMappings[] = {
				{"activation_required_tags", TEXT("ActivationRequiredTags")},
				{"activation_blocked_tags", TEXT("ActivationBlockedTags")},
				{"cancel_abilities_with_tag", TEXT("CancelAbilitiesWithTag")},
				{"block_abilities_with_tag", TEXT("BlockAbilitiesWithTag")},
				{"activation_owned_tags", TEXT("ActivationOwnedTags")},
				{"source_required_tags", TEXT("SourceRequiredTags")},
				{"source_blocked_tags", TEXT("SourceBlockedTags")},
				{"target_required_tags", TEXT("TargetRequiredTags")},
				{"target_blocked_tags", TEXT("TargetBlockedTags")},
			};

			for (const auto& Mapping : TagMappings)
			{
				FGameplayTagContainer* Tags = GetTagContainer(GA, Mapping.PropName);
				if (Tags && Tags->Num() > 0)
				{
					Info[Mapping.Key] = TagContainerToLuaArray(Lua, *Tags);
				}
			}

			int32 TriggerCount = Triggers ? Triggers->Num() : 0;
			Session.Log(FString::Printf(TEXT("[OK] info() -> %s: instancing=%s, net=%s, %d triggers"),
				*Blueprint->GetName(),
				*FormatInstancingPolicy(GA->GetInstancingPolicy()),
				*FormatNetExecutionPolicy(GA->GetNetExecutionPolicy()),
				TriggerCount));
			return Info;
		});

		// ==================================================================
		// configure(type, params)
		// ==================================================================
		BPObj.set_function("configure", [GA, Blueprint, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Param, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- configure("policy", {instancing="...", net_execution="...", ...}) ----
			if (FType.Equals(TEXT("policy"), ESearchCase::IgnoreCase))
			{
				if (!Param.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"policy\") -> table required"));
					return sol::lua_nil;
				}
				sol::table P = Param.as<sol::table>();

				const FScopedTransaction Tx(NSLOCTEXT("AgentIntegrationKit", "EditGAPolicy", "Edit Gameplay Ability Policy"));
				PreEditGA(GA, nullptr);

				// Instancing policy (via reflection)
				sol::optional<std::string> InstOpt = P.get<sol::optional<std::string>>("instancing");
				if (InstOpt.has_value())
				{
					FString InstStr = UTF8_TO_TCHAR(InstOpt.value().c_str());
					auto* InstPolicy = GetPropertyPtr<TEnumAsByte<EGameplayAbilityInstancingPolicy::Type>>(GA, TEXT("InstancingPolicy"));
					if (InstPolicy)
					{
						if (InstStr.Equals(TEXT("NonInstanced"), ESearchCase::IgnoreCase))
						{
PRAGMA_DISABLE_DEPRECATION_WARNINGS
							*InstPolicy = EGameplayAbilityInstancingPolicy::NonInstanced;
PRAGMA_ENABLE_DEPRECATION_WARNINGS
						}
						else if (InstStr.Equals(TEXT("InstancedPerActor"), ESearchCase::IgnoreCase))
							*InstPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
						else if (InstStr.Equals(TEXT("InstancedPerExecution"), ESearchCase::IgnoreCase))
							*InstPolicy = EGameplayAbilityInstancingPolicy::InstancedPerExecution;
						else
						{
							Session.Log(FString::Printf(TEXT("[FAIL] configure(\"policy\") -> unknown instancing '%s'. Valid: NonInstanced, InstancedPerActor, InstancedPerExecution"), *InstStr));
							PostEditGA(GA, nullptr);
							return sol::lua_nil;
						}
					}
				}

				// Net execution policy (via reflection)
				sol::optional<std::string> NetExecOpt = P.get<sol::optional<std::string>>("net_execution");
				if (NetExecOpt.has_value())
				{
					FString NEStr = UTF8_TO_TCHAR(NetExecOpt.value().c_str());
					auto* NetExecPolicy = GetPropertyPtr<TEnumAsByte<EGameplayAbilityNetExecutionPolicy::Type>>(GA, TEXT("NetExecutionPolicy"));
					if (NetExecPolicy)
					{
						if (NEStr.Equals(TEXT("LocalPredicted"), ESearchCase::IgnoreCase))
							*NetExecPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
						else if (NEStr.Equals(TEXT("LocalOnly"), ESearchCase::IgnoreCase))
							*NetExecPolicy = EGameplayAbilityNetExecutionPolicy::LocalOnly;
						else if (NEStr.Equals(TEXT("ServerInitiated"), ESearchCase::IgnoreCase))
							*NetExecPolicy = EGameplayAbilityNetExecutionPolicy::ServerInitiated;
						else if (NEStr.Equals(TEXT("ServerOnly"), ESearchCase::IgnoreCase))
							*NetExecPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
						else
						{
							Session.Log(FString::Printf(TEXT("[FAIL] configure(\"policy\") -> unknown net_execution '%s'. Valid: LocalPredicted, LocalOnly, ServerInitiated, ServerOnly"), *NEStr));
							PostEditGA(GA, nullptr);
							return sol::lua_nil;
						}
					}
				}

				// Replication policy (via reflection)
				sol::optional<std::string> RepOpt = P.get<sol::optional<std::string>>("replication");
				if (RepOpt.has_value())
				{
					FString RepStr = UTF8_TO_TCHAR(RepOpt.value().c_str());
					auto* RepPolicy = GetPropertyPtr<TEnumAsByte<EGameplayAbilityReplicationPolicy::Type>>(GA, TEXT("ReplicationPolicy"));
					if (RepPolicy)
					{
						if (RepStr.Equals(TEXT("ReplicateNo"), ESearchCase::IgnoreCase))
							*RepPolicy = EGameplayAbilityReplicationPolicy::ReplicateNo;
						else if (RepStr.Equals(TEXT("ReplicateYes"), ESearchCase::IgnoreCase))
							*RepPolicy = EGameplayAbilityReplicationPolicy::ReplicateYes;
						else
						{
							Session.Log(FString::Printf(TEXT("[FAIL] configure(\"policy\") -> unknown replication '%s'. Valid: ReplicateNo, ReplicateYes"), *RepStr));
							PostEditGA(GA, nullptr);
							return sol::lua_nil;
						}
					}
				}

				// Net security policy (via reflection)
				sol::optional<std::string> SecOpt = P.get<sol::optional<std::string>>("net_security");
				if (SecOpt.has_value())
				{
					FString SecStr = UTF8_TO_TCHAR(SecOpt.value().c_str());
					auto* SecPolicy = GetPropertyPtr<TEnumAsByte<EGameplayAbilityNetSecurityPolicy::Type>>(GA, TEXT("NetSecurityPolicy"));
					if (SecPolicy)
					{
						if (SecStr.Equals(TEXT("ClientOrServer"), ESearchCase::IgnoreCase))
							*SecPolicy = EGameplayAbilityNetSecurityPolicy::ClientOrServer;
						else if (SecStr.Equals(TEXT("ServerOnlyExecution"), ESearchCase::IgnoreCase))
							*SecPolicy = EGameplayAbilityNetSecurityPolicy::ServerOnlyExecution;
						else if (SecStr.Equals(TEXT("ServerOnlyTermination"), ESearchCase::IgnoreCase))
							*SecPolicy = EGameplayAbilityNetSecurityPolicy::ServerOnlyTermination;
						else if (SecStr.Equals(TEXT("ServerOnly"), ESearchCase::IgnoreCase))
							*SecPolicy = EGameplayAbilityNetSecurityPolicy::ServerOnly;
						else
						{
							Session.Log(FString::Printf(TEXT("[FAIL] configure(\"policy\") -> unknown net_security '%s'. Valid: ClientOrServer, ServerOnlyExecution, ServerOnlyTermination, ServerOnly"), *SecStr));
							PostEditGA(GA, nullptr);
							return sol::lua_nil;
						}
					}
				}

				// Bool flags
				if (auto BVal = P.get<sol::optional<bool>>("retrigger_instanced_ability"))
				{
					if (auto* Prop = GetPropertyPtr<bool>(GA, TEXT("bRetriggerInstancedAbility")))
						*Prop = BVal.value();
				}
				if (auto BVal = P.get<sol::optional<bool>>("replicate_input_directly"))
				{
					if (auto* Prop = GetPropertyPtr<bool>(GA, TEXT("bReplicateInputDirectly")))
						*Prop = BVal.value();
				}
				if (auto BVal = P.get<sol::optional<bool>>("server_respects_remote_ability_cancellation"))
				{
					if (auto* Prop = GetPropertyPtr<bool>(GA, TEXT("bServerRespectsRemoteAbilityCancellation")))
						*Prop = BVal.value();
				}

				PostEditGA(GA, nullptr);
				Session.Log(TEXT("[OK] configure(\"policy\")"));
				return sol::make_object(Lua, true);
			}

			// ---- configure("cost", {effect="..."}) or configure("cost", {clear=true}) ----
			if (FType.Equals(TEXT("cost"), ESearchCase::IgnoreCase))
			{
				if (!Param.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"cost\") -> table with 'effect' or 'clear' required"));
					return sol::lua_nil;
				}
				sol::table P = Param.as<sol::table>();

				// Clear cost effect
				if (P.get_or("clear", false))
				{
					const FScopedTransaction Tx(NSLOCTEXT("AgentIntegrationKit", "ClearGACost", "Clear Gameplay Ability Cost"));
					FProperty* CostProp = GA->GetClass()->FindPropertyByName(TEXT("CostGameplayEffectClass"));
					PreEditGA(GA, CostProp);

					TSubclassOf<UGameplayEffect>* CostClass = GetCostEffectClass(GA);
					if (CostClass)
					{
						*CostClass = nullptr;
					}

					PostEditGA(GA, CostProp);
					Session.Log(TEXT("[OK] configure(\"cost\", clear=true)"));
					return sol::make_object(Lua, true);
				}

				std::string EffectPath = P.get_or<std::string>("effect", "");
				if (EffectPath.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"cost\") -> 'effect' (asset path) or 'clear' required"));
					return sol::lua_nil;
				}

				FString FEffectPath = UTF8_TO_TCHAR(EffectPath.c_str());
				UBlueprint* EffectBP = LoadObject<UBlueprint>(nullptr, *FEffectPath);
				if (!EffectBP || !EffectBP->GeneratedClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"cost\") -> could not load blueprint '%s'"), *FEffectPath));
					return sol::lua_nil;
				}

				if (!EffectBP->GeneratedClass->IsChildOf(UGameplayEffect::StaticClass()))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"cost\") -> '%s' is not a GameplayEffect blueprint"), *FEffectPath));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AgentIntegrationKit", "EditGACost", "Edit Gameplay Ability Cost"));
				FProperty* CostProp = GA->GetClass()->FindPropertyByName(TEXT("CostGameplayEffectClass"));
				PreEditGA(GA, CostProp);

				TSubclassOf<UGameplayEffect>* CostClass = GetCostEffectClass(GA);
				if (CostClass)
				{
					*CostClass = EffectBP->GeneratedClass;
				}

				PostEditGA(GA, CostProp);

				Session.Log(FString::Printf(TEXT("[OK] configure(\"cost\", effect=\"%s\")"), *FEffectPath));
				return sol::make_object(Lua, true);
			}

			// ---- configure("cooldown", {effect="..."}) or configure("cooldown", {clear=true}) ----
			if (FType.Equals(TEXT("cooldown"), ESearchCase::IgnoreCase))
			{
				if (!Param.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"cooldown\") -> table with 'effect' or 'clear' required"));
					return sol::lua_nil;
				}
				sol::table P = Param.as<sol::table>();

				// Clear cooldown effect
				if (P.get_or("clear", false))
				{
					const FScopedTransaction Tx(NSLOCTEXT("AgentIntegrationKit", "ClearGACooldown", "Clear Gameplay Ability Cooldown"));
					FProperty* CooldownProp = GA->GetClass()->FindPropertyByName(TEXT("CooldownGameplayEffectClass"));
					PreEditGA(GA, CooldownProp);

					TSubclassOf<UGameplayEffect>* CooldownClass = GetCooldownEffectClass(GA);
					if (CooldownClass)
					{
						*CooldownClass = nullptr;
					}

					PostEditGA(GA, CooldownProp);
					Session.Log(TEXT("[OK] configure(\"cooldown\", clear=true)"));
					return sol::make_object(Lua, true);
				}

				std::string EffectPath = P.get_or<std::string>("effect", "");
				if (EffectPath.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"cooldown\") -> 'effect' (asset path) or 'clear' required"));
					return sol::lua_nil;
				}

				FString FEffectPath = UTF8_TO_TCHAR(EffectPath.c_str());
				UBlueprint* EffectBP = LoadObject<UBlueprint>(nullptr, *FEffectPath);
				if (!EffectBP || !EffectBP->GeneratedClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"cooldown\") -> could not load blueprint '%s'"), *FEffectPath));
					return sol::lua_nil;
				}

				if (!EffectBP->GeneratedClass->IsChildOf(UGameplayEffect::StaticClass()))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"cooldown\") -> '%s' is not a GameplayEffect blueprint"), *FEffectPath));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AgentIntegrationKit", "EditGACooldown", "Edit Gameplay Ability Cooldown"));
				FProperty* CooldownProp = GA->GetClass()->FindPropertyByName(TEXT("CooldownGameplayEffectClass"));
				PreEditGA(GA, CooldownProp);

				TSubclassOf<UGameplayEffect>* CooldownClass = GetCooldownEffectClass(GA);
				if (CooldownClass)
				{
					*CooldownClass = EffectBP->GeneratedClass;
				}

				PostEditGA(GA, CooldownProp);

				Session.Log(FString::Printf(TEXT("[OK] configure(\"cooldown\", effect=\"%s\")"), *FEffectPath));
				return sol::make_object(Lua, true);
			}

			// ---- configure("tags", {activation_required={...}, ...}) ----
			if (FType.Equals(TEXT("tags"), ESearchCase::IgnoreCase))
			{
				if (!Param.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"tags\") -> table required"));
					return sol::lua_nil;
				}
				sol::table P = Param.as<sol::table>();

				const FScopedTransaction Tx(NSLOCTEXT("AgentIntegrationKit", "EditGATags", "Edit Gameplay Ability Tags"));
				PreEditGA(GA, nullptr);

				struct FTagSetMapping { const char* Key; const TCHAR* PropName; };
				static const FTagSetMapping TagSetMappings[] = {
					{"ability_tags", TEXT("AbilityTags")},
					{"activation_required", TEXT("ActivationRequiredTags")},
					{"activation_blocked", TEXT("ActivationBlockedTags")},
					{"cancel_abilities_with", TEXT("CancelAbilitiesWithTag")},
					{"block_abilities_with", TEXT("BlockAbilitiesWithTag")},
					{"activation_owned", TEXT("ActivationOwnedTags")},
					{"source_required", TEXT("SourceRequiredTags")},
					{"source_blocked", TEXT("SourceBlockedTags")},
					{"target_required", TEXT("TargetRequiredTags")},
					{"target_blocked", TEXT("TargetBlockedTags")},
				};

				for (const auto& Mapping : TagSetMappings)
				{
					sol::optional<sol::table> TagsOpt = P.get<sol::optional<sol::table>>(Mapping.Key);
					if (TagsOpt.has_value())
					{
						FGameplayTagContainer* Container = GetTagContainer(GA, Mapping.PropName);
						if (Container)
						{
							SetTagContainerFromLuaArray(*Container, TagsOpt.value());
						}
					}
				}

				PostEditGA(GA, nullptr);

				Session.Log(TEXT("[OK] configure(\"tags\")"));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: policy, cost, cooldown, tags"), *FType));
			return sol::lua_nil;
		});

		// ==================================================================
		// add(type, params)
		// ==================================================================
		BPObj.set_function("add", [GA, Blueprint, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- add("trigger", {tag="...", type="..."}) ----
			if (FType.Equals(TEXT("trigger"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"trigger\") -> {tag=\"...\", type=\"...\"} required"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();

				std::string TagStr = P.get_or<std::string>("tag", "");
				if (TagStr.empty())
				{
					Session.Log(TEXT("[FAIL] add(\"trigger\") -> 'tag' required"));
					return sol::lua_nil;
				}

				FString FTagStr = UTF8_TO_TCHAR(TagStr.c_str());
				FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*FTagStr), false);
				if (!Tag.IsValid())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"trigger\") -> tag '%s' not found in project registry"), *FTagStr));
					return sol::lua_nil;
				}

				// Parse trigger source type
				EGameplayAbilityTriggerSource::Type TriggerSource = EGameplayAbilityTriggerSource::GameplayEvent;
				std::string TypeStr = P.get_or<std::string>("type", "GameplayEvent");
				FString FTypeStr = UTF8_TO_TCHAR(TypeStr.c_str());

				if (FTypeStr.Equals(TEXT("GameplayEvent"), ESearchCase::IgnoreCase))
					TriggerSource = EGameplayAbilityTriggerSource::GameplayEvent;
				else if (FTypeStr.Equals(TEXT("OwnedTagAdded"), ESearchCase::IgnoreCase))
					TriggerSource = EGameplayAbilityTriggerSource::OwnedTagAdded;
				else if (FTypeStr.Equals(TEXT("OwnedTagPresent"), ESearchCase::IgnoreCase))
					TriggerSource = EGameplayAbilityTriggerSource::OwnedTagPresent;
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"trigger\") -> unknown type '%s'. Valid: GameplayEvent, OwnedTagAdded, OwnedTagPresent"), *FTypeStr));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AgentIntegrationKit", "AddGATrigger", "Add Gameplay Ability Trigger"));
				FProperty* TriggerProp = GA->GetClass()->FindPropertyByName(TEXT("AbilityTriggers"));
				PreEditGA(GA, TriggerProp);

				TArray<FAbilityTriggerData>* Triggers = GetAbilityTriggers(GA);
				if (!Triggers)
				{
					PostEditGA(GA, TriggerProp);
					Session.Log(TEXT("[FAIL] add(\"trigger\") -> could not access AbilityTriggers via reflection"));
					return sol::lua_nil;
				}

				FAbilityTriggerData NewTrigger;
				NewTrigger.TriggerTag = Tag;
				NewTrigger.TriggerSource = TriggerSource;
				Triggers->Add(NewTrigger);

				PostEditGA(GA, TriggerProp);

				Session.Log(FString::Printf(TEXT("[OK] add(\"trigger\", tag=\"%s\", type=\"%s\") -> index %d"),
					*FTagStr, *FTypeStr, Triggers->Num()));
				return sol::make_object(Lua, Triggers->Num());
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: trigger"), *FType));
			return sol::lua_nil;
		});

		// ==================================================================
		// remove(type, id)
		// ==================================================================
		BPObj.set_function("remove", [GA, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- remove("trigger", index) ----
			if (FType.Equals(TEXT("trigger"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<int>() && !Id.is<double>())
				{
					Session.Log(TEXT("[FAIL] remove(\"trigger\") -> 1-based index required"));
					return sol::lua_nil;
				}
				int32 LuaIdx = Id.is<int>() ? Id.as<int>() : (int32)Id.as<double>();
				int32 Idx = LuaIdx - 1;

				TArray<FAbilityTriggerData>* Triggers = GetAbilityTriggers(GA);
				if (!Triggers)
				{
					Session.Log(TEXT("[FAIL] remove(\"trigger\") -> could not access AbilityTriggers via reflection"));
					return sol::lua_nil;
				}

				if (Idx < 0 || Idx >= Triggers->Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"trigger\", %d) -> out of range (count=%d)"), LuaIdx, Triggers->Num()));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AgentIntegrationKit", "RemoveGATrigger", "Remove Gameplay Ability Trigger"));
				FProperty* TriggerProp = GA->GetClass()->FindPropertyByName(TEXT("AbilityTriggers"));
				PreEditGA(GA, TriggerProp);
				Triggers->RemoveAt(Idx);
				PostEditGA(GA, TriggerProp);

				Session.Log(FString::Printf(TEXT("[OK] remove(\"trigger\", %d)"), LuaIdx));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: trigger"), *FType));
			return sol::lua_nil;
		});

		// ==================================================================
		// list(type)
		// ==================================================================
		BPObj.set_function("list", [GA, &Session](sol::table self,
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

			// ---- list("triggers") ----
			if (FType.Contains(TEXT("trigger"), ESearchCase::IgnoreCase))
			{
				TArray<FAbilityTriggerData>* Triggers = GetAbilityTriggers(GA);
				sol::table Result = Lua.create_table();
				if (Triggers)
				{
					for (int32 i = 0; i < Triggers->Num(); i++)
					{
						const FAbilityTriggerData& Trigger = (*Triggers)[i];
						sol::table E = Lua.create_table();
						E["index"] = i + 1;
						E["tag"] = TCHAR_TO_UTF8(*Trigger.TriggerTag.ToString());
						E["type"] = TCHAR_TO_UTF8(*FormatTriggerSource(Trigger.TriggerSource));
						Result[i + 1] = E;
					}
					Session.Log(FString::Printf(TEXT("[OK] list(\"triggers\") -> %d"), Triggers->Num()));
				}
				else
				{
					Session.Log(TEXT("[OK] list(\"triggers\") -> 0 (reflection failed)"));
				}
				return Result;
			}

			// ---- list("tags") ----
			if (FType.Contains(TEXT("tag"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();

				struct FTagListMapping { const char* Key; const TCHAR* PropName; };
				static const FTagListMapping TagListMappings[] = {
					{"ability_tags", TEXT("AbilityTags")},
					{"activation_required", TEXT("ActivationRequiredTags")},
					{"activation_blocked", TEXT("ActivationBlockedTags")},
					{"cancel_abilities_with", TEXT("CancelAbilitiesWithTag")},
					{"block_abilities_with", TEXT("BlockAbilitiesWithTag")},
					{"activation_owned", TEXT("ActivationOwnedTags")},
					{"source_required", TEXT("SourceRequiredTags")},
					{"source_blocked", TEXT("SourceBlockedTags")},
					{"target_required", TEXT("TargetRequiredTags")},
					{"target_blocked", TEXT("TargetBlockedTags")},
				};

				for (const auto& Mapping : TagListMappings)
				{
					FGameplayTagContainer* Container = GetTagContainer(GA, Mapping.PropName);
					sol::table TagList = Lua.create_table();
					if (Container)
					{
						int32 Idx = 1;
						for (const FGameplayTag& Tag : *Container)
						{
							TagList[Idx++] = TCHAR_TO_UTF8(*Tag.ToString());
						}
					}
					Result[Mapping.Key] = TagList;
				}

				Session.Log(TEXT("[OK] list(\"tags\")"));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: triggers, tags"), *FType));
			return sol::lua_nil;
		});

		// help() is handled by Blueprint's help() which reads _help_text
	});
}

REGISTER_LUA_BINDING(GameplayAbility, GameplayAbilityDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("GameplayAbilities")))
	{
		Session.Log(TEXT("[WARN] GameplayAbilities plugin is not loaded. Enable it in Edit > Plugins to use this feature."));
		return;
	}
	BindGameplayAbility(Lua, Session);
});
