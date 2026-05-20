// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
#include "EnvironmentQuery/EnvQueryContext.h"
#include "AIGraph.h"
#include "DataProviders/AIDataProvider.h"
#include "UObject/UnrealType.h"
#include "Modules/ModuleManager.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

namespace
{

// --- Enum to string helpers ---

const char* TestPurposeToString(EEnvTestPurpose::Type P)
{
	switch (P)
	{
	case EEnvTestPurpose::Filter: return "Filter";
	case EEnvTestPurpose::Score: return "Score";
	case EEnvTestPurpose::FilterAndScore: return "FilterAndScore";
	default: return "Unknown";
	}
}

EEnvTestPurpose::Type StringToTestPurpose(const FString& S)
{
	if (S.Equals(TEXT("Filter"), ESearchCase::IgnoreCase)) return EEnvTestPurpose::Filter;
	if (S.Equals(TEXT("Score"), ESearchCase::IgnoreCase)) return EEnvTestPurpose::Score;
	if (S.Equals(TEXT("FilterAndScore"), ESearchCase::IgnoreCase)) return EEnvTestPurpose::FilterAndScore;
	return EEnvTestPurpose::FilterAndScore;
}

const char* FilterTypeToString(EEnvTestFilterType::Type F)
{
	switch (F)
	{
	case EEnvTestFilterType::Minimum: return "Minimum";
	case EEnvTestFilterType::Maximum: return "Maximum";
	case EEnvTestFilterType::Range: return "Range";
	case EEnvTestFilterType::Match: return "Match";
	default: return "Unknown";
	}
}

EEnvTestFilterType::Type StringToFilterType(const FString& S)
{
	if (S.Equals(TEXT("Minimum"), ESearchCase::IgnoreCase)) return EEnvTestFilterType::Minimum;
	if (S.Equals(TEXT("Maximum"), ESearchCase::IgnoreCase)) return EEnvTestFilterType::Maximum;
	if (S.Equals(TEXT("Range"), ESearchCase::IgnoreCase)) return EEnvTestFilterType::Range;
	if (S.Equals(TEXT("Match"), ESearchCase::IgnoreCase)) return EEnvTestFilterType::Match;
	return EEnvTestFilterType::Minimum;
}

const char* ScoringEquationToString(EEnvTestScoreEquation::Type E)
{
	switch (E)
	{
	case EEnvTestScoreEquation::Linear: return "Linear";
	case EEnvTestScoreEquation::Square: return "Square";
	case EEnvTestScoreEquation::InverseLinear: return "InverseLinear";
	case EEnvTestScoreEquation::SquareRoot: return "SquareRoot";
	case EEnvTestScoreEquation::Constant: return "Constant";
	default: return "Unknown";
	}
}

EEnvTestScoreEquation::Type StringToScoringEquation(const FString& S)
{
	if (S.Equals(TEXT("Linear"), ESearchCase::IgnoreCase)) return EEnvTestScoreEquation::Linear;
	if (S.Equals(TEXT("Square"), ESearchCase::IgnoreCase)) return EEnvTestScoreEquation::Square;
	if (S.Equals(TEXT("InverseLinear"), ESearchCase::IgnoreCase)) return EEnvTestScoreEquation::InverseLinear;
	if (S.Equals(TEXT("SquareRoot"), ESearchCase::IgnoreCase)) return EEnvTestScoreEquation::SquareRoot;
	if (S.Equals(TEXT("Constant"), ESearchCase::IgnoreCase)) return EEnvTestScoreEquation::Constant;
	return EEnvTestScoreEquation::Linear;
}

const char* ClampTypeToString(EEnvQueryTestClamping::Type C)
{
	switch (C)
	{
	case EEnvQueryTestClamping::None: return "None";
	case EEnvQueryTestClamping::SpecifiedValue: return "SpecifiedValue";
	case EEnvQueryTestClamping::FilterThreshold: return "FilterThreshold";
	default: return "Unknown";
	}
}

// --- Resolve a class by name from derived classes ---

UClass* ResolveClassByName(UClass* BaseClass, const FString& Name)
{
	if (!BaseClass) return nullptr;

	TArray<UClass*> DerivedClasses;
	GetDerivedClasses(BaseClass, DerivedClasses);
	for (UClass* Cls : DerivedClasses)
	{
		if (Cls->HasAnyClassFlags(CLASS_Abstract)) continue;
		if (Cls->GetName().Equals(Name, ESearchCase::IgnoreCase) ||
			Cls->GetName().Replace(TEXT("EnvQueryGenerator_"), TEXT("")).Equals(Name, ESearchCase::IgnoreCase) ||
			Cls->GetName().Replace(TEXT("EnvQueryTest_"), TEXT("")).Equals(Name, ESearchCase::IgnoreCase))
		{
			return Cls;
		}
	}
	return nullptr;
}

// --- Get editable properties on a subclass (skipping base class props) ---

sol::table GetEditableProperties(UObject* Obj, UClass* BaseClass, sol::state_view& Lua)
{
	sol::table Props = Lua.create_table();
	if (!Obj) return Props;

	int32 Idx = 1;
	for (TFieldIterator<FProperty> PropIt(Obj->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Property = *PropIt;
		if (!Property->HasAnyPropertyFlags(CPF_Edit)) continue;
		if (Property->HasAnyPropertyFlags(CPF_Deprecated)) continue;
		if (BaseClass && Property->GetOwnerClass() == BaseClass) continue;
		if (Property->IsA<FDelegateProperty>() || Property->IsA<FMulticastDelegateProperty>()) continue;

		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Obj);
		if (!ValuePtr) continue;

		sol::table PropEntry = Lua.create_table();
		PropEntry["name"] = TCHAR_TO_UTF8(*Property->GetName());

		// FAIDataProvider*Value — extract DefaultValue
		if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
		{
			if (StructProp->Struct)
			{
				FString StructName = StructProp->Struct->GetName();
				if (StructName == TEXT("AIDataProviderFloatValue") ||
					StructName == TEXT("AIDataProviderIntValue") ||
					StructName == TEXT("AIDataProviderBoolValue"))
				{
					FProperty* DefaultProp = StructProp->Struct->FindPropertyByName(TEXT("DefaultValue"));
					if (DefaultProp)
					{
						void* DefaultPtr = DefaultProp->ContainerPtrToValuePtr<void>(const_cast<void*>(ValuePtr));
						if (DefaultPtr)
						{
							FString DefaultStr;
							DefaultProp->ExportTextItem_Direct(DefaultStr, DefaultPtr, nullptr, nullptr, PPF_None);
							PropEntry["type"] = TCHAR_TO_UTF8(*StructName);
							PropEntry["value"] = TCHAR_TO_UTF8(*DefaultStr);
							Props[Idx++] = PropEntry;
							continue;
						}
					}
				}
			}
		}

		// Enum
		if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
		{
			if (UEnum* Enum = EnumProp->GetEnum())
			{
				FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
				int64 EnumValue = UnderlyingProp->GetSignedIntPropertyValue(
					EnumProp->ContainerPtrToValuePtr<void>(Obj));
				FString ValueName = Enum->GetNameStringByValue(EnumValue);
				PropEntry["type"] = "Enum";
				PropEntry["value"] = TCHAR_TO_UTF8(*ValueName);
				Props[Idx++] = PropEntry;
				continue;
			}
		}

		// TEnumAsByte
		if (FByteProperty* ByteProp = CastField<FByteProperty>(Property))
		{
			if (UEnum* Enum = ByteProp->GetIntPropertyEnum())
			{
				uint8 ByteValue = ByteProp->GetPropertyValue_InContainer(Obj);
				FString ValueName = Enum->GetNameStringByValue(ByteValue);
				PropEntry["type"] = "Enum";
				PropEntry["value"] = TCHAR_TO_UTF8(*ValueName);
				Props[Idx++] = PropEntry;
				continue;
			}
		}

		// Generic export
		FString ValueStr;
		Property->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, nullptr, PPF_None);
		if (ValueStr.Len() > 120) ValueStr = ValueStr.Left(117) + TEXT("...");

		PropEntry["type"] = TCHAR_TO_UTF8(*Property->GetCPPType());
		PropEntry["value"] = TCHAR_TO_UTF8(*ValueStr);
		Props[Idx++] = PropEntry;
	}
	return Props;
}

// --- Set a property by name via reflection ---

bool SetPropertyByReflection(UObject* Obj, const FString& PropName, const FString& Value)
{
	if (!Obj) return false;

	FProperty* Prop = nullptr;
	for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
	{
		if (It->GetName().Equals(PropName, ESearchCase::IgnoreCase))
		{
			Prop = *It;
			break;
		}
	}
	if (!Prop) return false;
	if (Prop->HasAnyPropertyFlags(CPF_EditConst)) return false;

	// Handle FAIDataProvider*Value structs — set the DefaultValue inside
	if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		if (StructProp->Struct)
		{
			FString StructName = StructProp->Struct->GetName();
			if (StructName == TEXT("AIDataProviderFloatValue") ||
				StructName == TEXT("AIDataProviderIntValue") ||
				StructName == TEXT("AIDataProviderBoolValue"))
			{
				void* StructPtr = Prop->ContainerPtrToValuePtr<void>(Obj);
				FProperty* DefaultProp = StructProp->Struct->FindPropertyByName(TEXT("DefaultValue"));
				if (DefaultProp && StructPtr)
				{
					void* DefaultPtr = DefaultProp->ContainerPtrToValuePtr<void>(StructPtr);
					if (DefaultPtr)
					{
						DefaultProp->ImportText_Direct(*Value, DefaultPtr, Obj, PPF_None);
						return true;
					}
				}
			}
		}
	}

	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Obj);
	if (!ValuePtr) return false;
	Prop->ImportText_Direct(*Value, ValuePtr, Obj, PPF_None);
	return true;
}

FString GetClassTooltipDescription(UClass* Cls)
{
#if WITH_EDITOR
	return Cls ? Cls->GetMetaData(TEXT("ToolTip")) : FString();
#else
	(void)Cls;
	return FString();
#endif
}

void NotifyPostEditChange(UObject* Obj, FProperty* ChangedProperty = nullptr)
{
#if WITH_EDITOR
	FPropertyChangedEvent PCE(ChangedProperty, EPropertyChangeType::ValueSet);
	Obj->PostEditChangeProperty(PCE);
#else
	(void)Obj;
	(void)ChangedProperty;
#endif
}

// Sync the editor graph if the EQS editor is open
// Uses UAIGraph::Initialize() (virtual, AIGRAPH_API exported) which dispatches
// to UEnvironmentQueryGraph::Initialize() -> SpawnMissingNodes + CalculateAllWeights
void SyncEditorGraph(UEnvQuery* Query)
{
#if WITH_EDITORONLY_DATA
	if (Query && Query->EdGraph)
	{
		if (UAIGraph* AIGraph = Cast<UAIGraph>(Query->EdGraph))
		{
			AIGraph->Initialize();
		}
	}
#else
	(void)Query;
#endif
}

} // anonymous namespace

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> EQSDocs = {};

static void BindEQS(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_eqs", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UEnvQuery* Query = LoadObject<UEnvQuery>(nullptr, *FPath);
		if (!Query) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"Element types for add/remove/list/configure:\n"
			"  option        — EQS query option (generator + tests)\n"
			"  test          — test within an option\n"
			"\n"
			"add(type, params):\n"
			"  add(\"option\", {generator=\"SimpleGrid\"})  — add option with generator\n"
			"  add(\"test\", {option=0, type=\"Distance\"})  — add test to option (0-based)\n"
			"\n"
			"remove(type, id):\n"
			"  remove(\"option\", 0)                      — remove option by 0-based index\n"
			"  remove(\"test\", {option=0, index=1})       — remove test by 0-based indices\n"
			"\n"
			"list(type, params?):\n"
			"  list(\"options\")                            — all options with generators\n"
			"  list(\"tests\", {option=0})                  — tests for an option\n"
			"  list(\"generators\")                         — available generator classes\n"
			"  list(\"test_types\")                         — available test classes\n"
			"  list(\"contexts\")                           — available query context classes\n"
			"  list(\"params\")                             — named query parameters\n"
			"\n"
			"configure(type, params):\n"
			"  configure(\"test\", {option=0, index=0, purpose=\"Score\", scoring_equation=\"Linear\",\n"
			"            normalization_type=\"Absolute\", reference_value=100.0, define_reference_value=true,\n"
			"            multi_context_filter_op=\"AllPass\", multi_context_score_op=\"AverageScore\"})\n"
			"  configure(\"generator\", {option=0, property_name=\"value\"})\n"
			"  configure(\"option\", {index=0, generator=\"ActorsOfClass\"}) — replace generator\n"
			"\n"
			"Action methods:\n"
			"  info()                                    — summary of query options and tests\n"
			"  reorder(\"option\", {from=0, to=2})         — move option to new position\n"
			"  reorder(\"test\", {option=0, from=1, to=0}) — move test within option\n"
			"  duplicate(\"option\", 0)                    — duplicate option with all tests\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [Query, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();

			Result["name"] = TCHAR_TO_UTF8(*Query->GetQueryName().ToString());
			Result["path"] = TCHAR_TO_UTF8(*Query->GetPathName());

			const TArray<UEnvQueryOption*>& Options = Query->GetOptions();
			Result["option_count"] = static_cast<int>(Options.Num());

			int32 TotalTests = 0;
			sol::table OptionsTable = Lua.create_table();

			for (int32 i = 0; i < Options.Num(); i++)
			{
				const UEnvQueryOption* Option = Options[i];
				if (!Option) continue;

				sol::table OptEntry = Lua.create_table();
				OptEntry["index"] = i;

				if (Option->Generator)
				{
					OptEntry["generator_class"] = TCHAR_TO_UTF8(*Option->Generator->GetClass()->GetName());
					OptEntry["generator_title"] = TCHAR_TO_UTF8(*Option->Generator->GetDescriptionTitle().ToString());
					OptEntry["item_type"] = Option->Generator->ItemType
						? TCHAR_TO_UTF8(*Option->Generator->ItemType->GetName())
						: "None";
				}
				else
				{
					OptEntry["generator_class"] = "None";
					OptEntry["generator_title"] = "None";
					OptEntry["item_type"] = "None";
				}

				OptEntry["test_count"] = static_cast<int>(Option->Tests.Num());
				TotalTests += Option->Tests.Num();
				OptionsTable[i + 1] = OptEntry;
			}

			Result["total_tests"] = TotalTests;
			Result["options"] = OptionsTable;

			Session.Log(FString::Printf(TEXT("[OK] info() -> %s, %d options, %d tests"),
				*Query->GetQueryName().ToString(), Options.Num(), TotalTests));
			return Result;
		});

		// ================================================================
		// list(type, params?)
		// ================================================================
		AssetObj.set_function("list", [Query, &Session](sol::table Self,
			sol::optional<std::string> TypeOpt, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = Self["info"];
				if (InfoFn.valid()) return InfoFn(Self);
				return sol::lua_nil;
			}

			// ---- list("options") ----
			if (FType.Equals(TEXT("options"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("option"), ESearchCase::IgnoreCase))
			{
				const TArray<UEnvQueryOption*>& Options = Query->GetOptions();
				sol::table Result = Lua.create_table();

				for (int32 i = 0; i < Options.Num(); i++)
				{
					const UEnvQueryOption* Option = Options[i];
					if (!Option) continue;

					sol::table OptEntry = Lua.create_table();
					OptEntry["index"] = i;

					if (Option->Generator)
					{
						OptEntry["generator_class"] = TCHAR_TO_UTF8(*Option->Generator->GetClass()->GetName());
						OptEntry["generator_title"] = TCHAR_TO_UTF8(*Option->Generator->GetDescriptionTitle().ToString());
						OptEntry["generator_details"] = TCHAR_TO_UTF8(*Option->Generator->GetDescriptionDetails().ToString());
						OptEntry["item_type"] = Option->Generator->ItemType
							? TCHAR_TO_UTF8(*Option->Generator->ItemType->GetName())
							: "None";
						OptEntry["auto_sort_tests"] = static_cast<bool>(Option->Generator->bAutoSortTests);
					}
					else
					{
						OptEntry["generator_class"] = "None";
						OptEntry["generator_title"] = "None";
						OptEntry["generator_details"] = "";
						OptEntry["item_type"] = "None";
						OptEntry["auto_sort_tests"] = false;
					}

					OptEntry["test_count"] = static_cast<int>(Option->Tests.Num());
					Result[i + 1] = OptEntry;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"options\") -> %d"), Options.Num()));
				return Result;
			}

			// ---- list("tests", {option=N}) ----
			if (FType.Equals(TEXT("tests"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("test"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] list(\"tests\") -> {option=N} required (0-based)"));
					return sol::lua_nil;
				}

				sol::optional<int> OptIdx = Params.value().get<sol::optional<int>>("option");
				if (!OptIdx.has_value())
				{
					Session.Log(TEXT("[FAIL] list(\"tests\") -> 'option' index required"));
					return sol::lua_nil;
				}

				const TArray<UEnvQueryOption*>& Options = Query->GetOptions();
				int32 OptionIndex = OptIdx.value();
				if (OptionIndex < 0 || OptionIndex >= Options.Num())
				{
					if (Options.Num() == 0)
						Session.Log(FString::Printf(TEXT("[FAIL] list(\"tests\") -> option %d out of range (no options exist)"), OptionIndex));
					else
						Session.Log(FString::Printf(TEXT("[FAIL] list(\"tests\") -> option %d out of range (0-%d)"), OptionIndex, Options.Num() - 1));
					return sol::lua_nil;
				}

				const UEnvQueryOption* Option = Options[OptionIndex];
				if (!Option)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] list(\"tests\") -> option %d is null"), OptionIndex));
					return sol::lua_nil;
				}

				sol::table Result = Lua.create_table();
				for (int32 t = 0; t < Option->Tests.Num(); t++)
				{
					UEnvQueryTest* Test = Option->Tests[t];
					if (!Test) continue;

					sol::table TestEntry = Lua.create_table();
					TestEntry["index"] = t;
					TestEntry["class"] = TCHAR_TO_UTF8(*Test->GetClass()->GetName());
					TestEntry["title"] = TCHAR_TO_UTF8(*Test->GetDescriptionTitle().ToString());
					TestEntry["details"] = TCHAR_TO_UTF8(*Test->GetDescriptionDetails().ToString());
					TestEntry["purpose"] = TestPurposeToString(static_cast<EEnvTestPurpose::Type>(Test->TestPurpose.GetValue()));
					TestEntry["filter_type"] = FilterTypeToString(static_cast<EEnvTestFilterType::Type>(Test->FilterType.GetValue()));
					TestEntry["scoring_equation"] = ScoringEquationToString(static_cast<EEnvTestScoreEquation::Type>(Test->ScoringEquation.GetValue()));
					TestEntry["scoring_factor"] = Test->ScoringFactor.DefaultValue;
					TestEntry["comment"] = TCHAR_TO_UTF8(*Test->TestComment);
					TestEntry["normalization_type"] = (Test->NormalizationType == EEQSNormalizationType::Absolute) ? "Absolute" : "RelativeToScores";
					TestEntry["clamp_min_type"] = ClampTypeToString(static_cast<EEnvQueryTestClamping::Type>(Test->ClampMinType.GetValue()));
					TestEntry["clamp_max_type"] = ClampTypeToString(static_cast<EEnvQueryTestClamping::Type>(Test->ClampMaxType.GetValue()));
					TestEntry["score_clamp_min"] = Test->ScoreClampMin.DefaultValue;
					TestEntry["score_clamp_max"] = Test->ScoreClampMax.DefaultValue;
					TestEntry["float_value_min"] = Test->FloatValueMin.DefaultValue;
					TestEntry["float_value_max"] = Test->FloatValueMax.DefaultValue;
					TestEntry["bool_value"] = Test->BoolValue.DefaultValue;
					TestEntry["define_reference_value"] = Test->bDefineReferenceValue;
					if (Test->bDefineReferenceValue)
					{
						TestEntry["reference_value"] = Test->ReferenceValue.DefaultValue;
					}

					// Editable subclass properties
					TestEntry["properties"] = GetEditableProperties(Test, UEnvQueryTest::StaticClass(), Lua);

					Result[t + 1] = TestEntry;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"tests\", option=%d) -> %d"), OptionIndex, Option->Tests.Num()));
				return Result;
			}

			// ---- list("generators") ----
			if (FType.Equals(TEXT("generators"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("generator"), ESearchCase::IgnoreCase))
			{
				TArray<UClass*> DerivedClasses;
				GetDerivedClasses(UEnvQueryGenerator::StaticClass(), DerivedClasses);

				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (UClass* Cls : DerivedClasses)
				{
					if (Cls->HasAnyClassFlags(CLASS_Abstract)) continue;

					sol::table Entry = Lua.create_table();
					Entry["class_name"] = TCHAR_TO_UTF8(*Cls->GetName());
					FString DisplayName = Cls->GetName();
					DisplayName.RemoveFromStart(TEXT("EnvQueryGenerator_"));
					Entry["display_name"] = TCHAR_TO_UTF8(*DisplayName);
					Result[Idx++] = Entry;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"generators\") -> %d"), Idx - 1));
				return Result;
			}

			// ---- list("test_types") ----
			if (FType.Equals(TEXT("test_types"), ESearchCase::IgnoreCase))
			{
				TArray<UClass*> DerivedClasses;
				GetDerivedClasses(UEnvQueryTest::StaticClass(), DerivedClasses);

				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (UClass* Cls : DerivedClasses)
				{
					if (Cls->HasAnyClassFlags(CLASS_Abstract)) continue;

					sol::table Entry = Lua.create_table();
					Entry["class_name"] = TCHAR_TO_UTF8(*Cls->GetName());
					FString DisplayName = Cls->GetName();
					DisplayName.RemoveFromStart(TEXT("EnvQueryTest_"));
					Entry["display_name"] = TCHAR_TO_UTF8(*DisplayName);
					Result[Idx++] = Entry;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"test_types\") -> %d"), Idx - 1));
				return Result;
			}

			// ---- list("contexts") ----
			if (FType.Equals(TEXT("contexts"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("context"), ESearchCase::IgnoreCase))
			{
				TArray<UClass*> DerivedClasses;
				GetDerivedClasses(UEnvQueryContext::StaticClass(), DerivedClasses);

				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (UClass* Cls : DerivedClasses)
				{
					if (Cls->HasAnyClassFlags(CLASS_Abstract)) continue;

					sol::table Entry = Lua.create_table();
					Entry["class_name"] = TCHAR_TO_UTF8(*Cls->GetName());
					FString DisplayName = Cls->GetName();
					DisplayName.RemoveFromStart(TEXT("EnvQueryContext_"));
					Entry["display_name"] = TCHAR_TO_UTF8(*DisplayName);

					// Include tooltip/description from class metadata if available
					FString Tooltip = GetClassTooltipDescription(Cls);
					if (!Tooltip.IsEmpty())
					{
						Entry["description"] = TCHAR_TO_UTF8(*Tooltip);
					}
					Result[Idx++] = Entry;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"contexts\") -> %d"), Idx - 1));
				return Result;
			}

			// ---- list("params") ----
			if (FType.Equals(TEXT("params"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("parameters"), ESearchCase::IgnoreCase))
			{
				TArray<FAIDynamicParam> NamedValues;
				// CollectQueryParams requires a UObject owner — use the query itself as a dummy owner
				Query->CollectQueryParams(*Query, NamedValues);

				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < NamedValues.Num(); i++)
				{
					const FAIDynamicParam& Param = NamedValues[i];
					sol::table Entry = Lua.create_table();
					Entry["name"] = TCHAR_TO_UTF8(*Param.ParamName.ToString());

					switch (Param.ParamType)
					{
					case EAIParamType::Float: Entry["type"] = "Float"; break;
					case EAIParamType::Int:   Entry["type"] = "Int"; break;
					case EAIParamType::Bool:  Entry["type"] = "Bool"; break;
					default:                  Entry["type"] = "Unknown"; break;
					}

					Entry["default_value"] = Param.Value;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
					Entry["allow_bb_key"] = static_cast<bool>(Param.bAllowBBKey);
#endif
					Result[i + 1] = Entry;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"params\") -> %d"), NamedValues.Num()));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: options, tests, generators, test_types, contexts, params"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// add(type, params)
		// ================================================================
		AssetObj.set_function("add", [Query, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- add("option", {generator="SimpleGrid"}) ----
			if (FType.Equals(TEXT("option"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"option\") -> {generator=..} required"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();

				std::string GenName = P.get_or<std::string>("generator", "");
				if (GenName.empty())
				{
					Session.Log(TEXT("[FAIL] add(\"option\") -> 'generator' class name required"));
					return sol::lua_nil;
				}

				FString FGenName = UTF8_TO_TCHAR(GenName.c_str());
				UClass* GenClass = ResolveClassByName(UEnvQueryGenerator::StaticClass(), FGenName);
				if (!GenClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"option\") -> generator class '%s' not found. Use list(\"generators\") to see available."), *FGenName));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("EQS: Add Option")));
				Query->Modify();

				UEnvQueryOption* NewOption = NewObject<UEnvQueryOption>(Query);
				UEnvQueryGenerator* NewGen = NewObject<UEnvQueryGenerator>(NewOption, GenClass);
				NewOption->Generator = NewGen;

				TArray<TObjectPtr<UEnvQueryOption>>& Options = Query->GetOptionsMutable();
				Options.Add(NewOption);

				NotifyPostEditChange(Query);
				SyncEditorGraph(Query);
				Query->MarkPackageDirty();

				int32 NewIndex = Options.Num() - 1;
				Session.Log(FString::Printf(TEXT("[OK] add(\"option\", generator=\"%s\") -> index %d"), *GenClass->GetName(), NewIndex));
				return sol::make_object(Lua, NewIndex);
			}

			// ---- add("test", {option=N, type="Distance"}) ----
			if (FType.Equals(TEXT("test"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"test\") -> {option=N, type=..} required"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();

				sol::optional<int> OptIdx = P.get<sol::optional<int>>("option");
				if (!OptIdx.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"test\") -> 'option' index required (0-based)"));
					return sol::lua_nil;
				}

				std::string TestTypeName = P.get_or<std::string>("type", "");
				if (TestTypeName.empty())
				{
					Session.Log(TEXT("[FAIL] add(\"test\") -> 'type' class name required"));
					return sol::lua_nil;
				}

				TArray<TObjectPtr<UEnvQueryOption>>& Options = Query->GetOptionsMutable();
				int32 OptionIndex = OptIdx.value();
				if (OptionIndex < 0 || OptionIndex >= Options.Num())
				{
					if (Options.Num() == 0)
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"test\") -> option %d out of range (no options exist)"), OptionIndex));
					else
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"test\") -> option %d out of range (0-%d)"), OptionIndex, Options.Num() - 1));
					return sol::lua_nil;
				}

				UEnvQueryOption* Option = Options[OptionIndex];
				if (!Option)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"test\") -> option %d is null"), OptionIndex));
					return sol::lua_nil;
				}

				FString FTestName = UTF8_TO_TCHAR(TestTypeName.c_str());
				UClass* TestClass = ResolveClassByName(UEnvQueryTest::StaticClass(), FTestName);
				if (!TestClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"test\") -> test class '%s' not found. Use list(\"test_types\") to see available."), *FTestName));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("EQS: Add Test")));
				Query->Modify();

				UEnvQueryTest* NewTest = NewObject<UEnvQueryTest>(Option, TestClass);
				Option->Tests.Add(NewTest);

				NotifyPostEditChange(Query);
				SyncEditorGraph(Query);
				Query->MarkPackageDirty();

				int32 NewTestIndex = Option->Tests.Num() - 1;
				Session.Log(FString::Printf(TEXT("[OK] add(\"test\", option=%d, type=\"%s\") -> test index %d"), OptionIndex, *TestClass->GetName(), NewTestIndex));
				return sol::make_object(Lua, NewTestIndex);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: option, test"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// remove(type, id)
		// ================================================================
		AssetObj.set_function("remove", [Query, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- remove("option", N) ----
			if (FType.Equals(TEXT("option"), ESearchCase::IgnoreCase))
			{
				int32 OptionIndex = -1;
				if (Id.is<int>()) OptionIndex = Id.as<int>();
				else if (Id.is<double>()) OptionIndex = static_cast<int32>(Id.as<double>());
				else
				{
					Session.Log(TEXT("[FAIL] remove(\"option\") -> 0-based index required"));
					return sol::lua_nil;
				}

				TArray<TObjectPtr<UEnvQueryOption>>& Options = Query->GetOptionsMutable();
				if (OptionIndex < 0 || OptionIndex >= Options.Num())
				{
					if (Options.Num() == 0)
						Session.Log(FString::Printf(TEXT("[FAIL] remove(\"option\") -> index %d out of range (no options exist)"), OptionIndex));
					else
						Session.Log(FString::Printf(TEXT("[FAIL] remove(\"option\") -> index %d out of range (0-%d)"), OptionIndex, Options.Num() - 1));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("EQS: Remove Option")));
				Query->Modify();

				UEnvQueryOption* Option = Options[OptionIndex];
				Options.RemoveAt(OptionIndex);

				// Clean up orphaned UObjects by renaming to transient package
				if (Option)
				{
					// Clean up tests first
					for (UEnvQueryTest* Test : Option->Tests)
					{
						if (Test)
						{
							Test->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
						}
					}
					// Clean up generator
					if (Option->Generator)
					{
						Option->Generator->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
					}
					Option->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
				}

				NotifyPostEditChange(Query);
				SyncEditorGraph(Query);
				Query->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"option\", %d)"), OptionIndex));
				return sol::make_object(Lua, true);
			}

			// ---- remove("test", {option=N, index=M}) ----
			if (FType.Equals(TEXT("test"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] remove(\"test\") -> {option=N, index=M} required (0-based)"));
					return sol::lua_nil;
				}
				sol::table P = Id.as<sol::table>();

				sol::optional<int> OptIdx = P.get<sol::optional<int>>("option");
				sol::optional<int> TestIdx = P.get<sol::optional<int>>("index");
				if (!OptIdx.has_value() || !TestIdx.has_value())
				{
					Session.Log(TEXT("[FAIL] remove(\"test\") -> both 'option' and 'index' required"));
					return sol::lua_nil;
				}

				TArray<TObjectPtr<UEnvQueryOption>>& Options = Query->GetOptionsMutable();
				int32 OptionIndex = OptIdx.value();
				if (OptionIndex < 0 || OptionIndex >= Options.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"test\") -> option %d out of range"), OptionIndex));
					return sol::lua_nil;
				}

				UEnvQueryOption* Option = Options[OptionIndex];
				if (!Option)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"test\") -> option %d is null"), OptionIndex));
					return sol::lua_nil;
				}

				int32 TestIndex = TestIdx.value();
				if (TestIndex < 0 || TestIndex >= Option->Tests.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"test\") -> test index %d out of range (0-%d)"), TestIndex, Option->Tests.Num() - 1));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("EQS: Remove Test")));
				Query->Modify();

				UEnvQueryTest* RemovedTest = Option->Tests[TestIndex];
				Option->Tests.RemoveAt(TestIndex);

				// Clean up orphaned test UObject
				if (RemovedTest)
				{
					RemovedTest->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
				}

				NotifyPostEditChange(Query);
				SyncEditorGraph(Query);
				Query->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"test\", option=%d, index=%d)"), OptionIndex, TestIndex));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: option, test"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// configure(type, params)
		// ================================================================
		AssetObj.set_function("configure", [Query, &Session](sol::table /*self*/,
			const std::string& Type, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- configure("test", {option=N, index=M, purpose=.., ...}) ----
			if (FType.Equals(TEXT("test"), ESearchCase::IgnoreCase))
			{
				sol::optional<int> OptIdx = Params.get<sol::optional<int>>("option");
				sol::optional<int> TestIdx = Params.get<sol::optional<int>>("index");
				if (!OptIdx.has_value() || !TestIdx.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"test\") -> 'option' and 'index' required (0-based)"));
					return sol::lua_nil;
				}

				TArray<TObjectPtr<UEnvQueryOption>>& Options = Query->GetOptionsMutable();
				int32 OptionIndex = OptIdx.value();
				if (OptionIndex < 0 || OptionIndex >= Options.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"test\") -> option %d out of range"), OptionIndex));
					return sol::lua_nil;
				}

				UEnvQueryOption* Option = Options[OptionIndex];
				if (!Option)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"test\") -> option %d is null"), OptionIndex));
					return sol::lua_nil;
				}

				int32 TestIndex = TestIdx.value();
				if (TestIndex < 0 || TestIndex >= Option->Tests.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"test\") -> test %d out of range (0-%d)"), TestIndex, Option->Tests.Num() - 1));
					return sol::lua_nil;
				}

				UEnvQueryTest* Test = Option->Tests[TestIndex];
				if (!Test)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"test\") -> test %d is null"), TestIndex));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("EQS: Configure Test")));
				Query->Modify();
				Test->Modify();

				// Base test properties
				std::string PurposeStr = Params.get_or<std::string>("purpose", "");
				if (!PurposeStr.empty())
				{
					Test->TestPurpose = StringToTestPurpose(UTF8_TO_TCHAR(PurposeStr.c_str()));
				}

				std::string FilterTypeStr = Params.get_or<std::string>("filter_type", "");
				if (!FilterTypeStr.empty())
				{
					Test->FilterType = StringToFilterType(UTF8_TO_TCHAR(FilterTypeStr.c_str()));
				}

				std::string ScoringEqStr = Params.get_or<std::string>("scoring_equation", "");
				if (!ScoringEqStr.empty())
				{
					Test->ScoringEquation = StringToScoringEquation(UTF8_TO_TCHAR(ScoringEqStr.c_str()));
				}

				sol::optional<double> ScoringFactorVal = Params.get<sol::optional<double>>("scoring_factor");
				if (ScoringFactorVal.has_value())
				{
					Test->ScoringFactor.DefaultValue = static_cast<float>(ScoringFactorVal.value());
				}

				std::string CommentStr = Params.get_or<std::string>("comment", "");
				if (!CommentStr.empty())
				{
					Test->TestComment = UTF8_TO_TCHAR(CommentStr.c_str());
				}

				sol::optional<double> FloatMinVal = Params.get<sol::optional<double>>("float_value_min");
				if (FloatMinVal.has_value())
				{
					Test->FloatValueMin.DefaultValue = static_cast<float>(FloatMinVal.value());
				}

				sol::optional<double> FloatMaxVal = Params.get<sol::optional<double>>("float_value_max");
				if (FloatMaxVal.has_value())
				{
					Test->FloatValueMax.DefaultValue = static_cast<float>(FloatMaxVal.value());
				}

				sol::optional<bool> BoolVal = Params.get<sol::optional<bool>>("bool_value");
				if (BoolVal.has_value())
				{
					Test->BoolValue.DefaultValue = BoolVal.value();
				}

				std::string ClampMinStr = Params.get_or<std::string>("clamp_min_type", "");
				if (!ClampMinStr.empty())
				{
					if (ClampMinStr == "None" || ClampMinStr == "none") Test->ClampMinType = EEnvQueryTestClamping::None;
					else if (ClampMinStr == "SpecifiedValue") Test->ClampMinType = EEnvQueryTestClamping::SpecifiedValue;
					else if (ClampMinStr == "FilterThreshold") Test->ClampMinType = EEnvQueryTestClamping::FilterThreshold;
				}

				std::string ClampMaxStr = Params.get_or<std::string>("clamp_max_type", "");
				if (!ClampMaxStr.empty())
				{
					if (ClampMaxStr == "None" || ClampMaxStr == "none") Test->ClampMaxType = EEnvQueryTestClamping::None;
					else if (ClampMaxStr == "SpecifiedValue") Test->ClampMaxType = EEnvQueryTestClamping::SpecifiedValue;
					else if (ClampMaxStr == "FilterThreshold") Test->ClampMaxType = EEnvQueryTestClamping::FilterThreshold;
				}

				sol::optional<double> ScoreClampMinVal = Params.get<sol::optional<double>>("score_clamp_min");
				if (ScoreClampMinVal.has_value())
				{
					Test->ScoreClampMin.DefaultValue = static_cast<float>(ScoreClampMinVal.value());
				}

				sol::optional<double> ScoreClampMaxVal = Params.get<sol::optional<double>>("score_clamp_max");
				if (ScoreClampMaxVal.has_value())
				{
					Test->ScoreClampMax.DefaultValue = static_cast<float>(ScoreClampMaxVal.value());
				}

				// Normalization type
				std::string NormTypeStr = Params.get_or<std::string>("normalization_type", "");
				if (!NormTypeStr.empty())
				{
					if (NormTypeStr == "Absolute" || NormTypeStr == "absolute")
						Test->NormalizationType = EEQSNormalizationType::Absolute;
					else if (NormTypeStr == "RelativeToScores" || NormTypeStr == "relative")
						Test->NormalizationType = EEQSNormalizationType::RelativeToScores;
				}

				// Reference value
				sol::optional<bool> DefRefVal = Params.get<sol::optional<bool>>("define_reference_value");
				if (DefRefVal.has_value())
				{
					Test->bDefineReferenceValue = DefRefVal.value();
				}

				sol::optional<double> RefVal = Params.get<sol::optional<double>>("reference_value");
				if (RefVal.has_value())
				{
					Test->ReferenceValue.DefaultValue = static_cast<float>(RefVal.value());
					Test->bDefineReferenceValue = true;
				}

				// Multi-context operators
				std::string MultiFilterOpStr = Params.get_or<std::string>("multi_context_filter_op", "");
				if (!MultiFilterOpStr.empty())
				{
					if (MultiFilterOpStr == "AllPass" || MultiFilterOpStr == "allpass")
						Test->MultipleContextFilterOp = EEnvTestFilterOperator::AllPass;
					else if (MultiFilterOpStr == "AnyPass" || MultiFilterOpStr == "anypass")
						Test->MultipleContextFilterOp = EEnvTestFilterOperator::AnyPass;
				}

				std::string MultiScoreOpStr = Params.get_or<std::string>("multi_context_score_op", "");
				if (!MultiScoreOpStr.empty())
				{
					if (MultiScoreOpStr == "AverageScore" || MultiScoreOpStr == "average")
						Test->MultipleContextScoreOp = EEnvTestScoreOperator::AverageScore;
					else if (MultiScoreOpStr == "MinScore" || MultiScoreOpStr == "min")
						Test->MultipleContextScoreOp = EEnvTestScoreOperator::MinScore;
					else if (MultiScoreOpStr == "MaxScore" || MultiScoreOpStr == "max")
						Test->MultipleContextScoreOp = EEnvTestScoreOperator::MaxScore;
					else if (MultiScoreOpStr == "Multiply" || MultiScoreOpStr == "multiply")
						Test->MultipleContextScoreOp = EEnvTestScoreOperator::Multiply;
				}

				// Subclass-specific properties via reflection
				// Iterate Params table for any keys not in the known set
				static const TSet<FString> KnownKeys = {
					TEXT("option"), TEXT("index"), TEXT("purpose"), TEXT("filter_type"),
					TEXT("scoring_equation"), TEXT("scoring_factor"), TEXT("comment"),
					TEXT("float_value_min"), TEXT("float_value_max"), TEXT("bool_value"),
					TEXT("clamp_min_type"), TEXT("clamp_max_type"),
					TEXT("score_clamp_min"), TEXT("score_clamp_max"),
					TEXT("normalization_type"), TEXT("define_reference_value"), TEXT("reference_value"),
					TEXT("multi_context_filter_op"), TEXT("multi_context_score_op")
				};

				for (auto& kv : Params)
				{
					if (!kv.first.is<std::string>()) continue;
					FString Key = UTF8_TO_TCHAR(kv.first.as<std::string>().c_str());
					if (KnownKeys.Contains(Key)) continue;

					FString ValueStr;
					if (kv.second.is<std::string>())
						ValueStr = UTF8_TO_TCHAR(kv.second.as<std::string>().c_str());
					else if (kv.second.is<double>())
						ValueStr = FString::SanitizeFloat(kv.second.as<double>());
					else if (kv.second.is<int>())
						ValueStr = FString::FromInt(kv.second.as<int>());
					else if (kv.second.is<bool>())
						ValueStr = kv.second.as<bool>() ? TEXT("true") : TEXT("false");
					else
						continue;

					if (!SetPropertyByReflection(Test, Key, ValueStr))
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure(\"test\") -> property '%s' not found or could not be set"), *Key));
					}
				}

				// Call UpdatePreviewData to refresh scoring preview (PostEditChangeProperty triggers this for specific properties)
				Test->UpdatePreviewData();
				NotifyPostEditChange(Test);
				Query->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"test\", option=%d, index=%d)"), OptionIndex, TestIndex));
				return sol::make_object(Lua, true);
			}

			// ---- configure("generator", {option=N, ...}) ----
			if (FType.Equals(TEXT("generator"), ESearchCase::IgnoreCase))
			{
				sol::optional<int> OptIdx = Params.get<sol::optional<int>>("option");
				if (!OptIdx.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"generator\") -> 'option' index required (0-based)"));
					return sol::lua_nil;
				}

				TArray<TObjectPtr<UEnvQueryOption>>& Options = Query->GetOptionsMutable();
				int32 OptionIndex = OptIdx.value();
				if (OptionIndex < 0 || OptionIndex >= Options.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"generator\") -> option %d out of range"), OptionIndex));
					return sol::lua_nil;
				}

				UEnvQueryOption* Option = Options[OptionIndex];
				if (!Option || !Option->Generator)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"generator\") -> option %d has no generator"), OptionIndex));
					return sol::lua_nil;
				}

				UEnvQueryGenerator* Gen = Option->Generator;

				const FScopedTransaction Transaction(FText::FromString(TEXT("EQS: Configure Generator")));
				Query->Modify();
				Gen->Modify();

				// Set properties via reflection
				static const TSet<FString> GenKnownKeys = { TEXT("option") };

				for (auto& kv : Params)
				{
					if (!kv.first.is<std::string>()) continue;
					FString Key = UTF8_TO_TCHAR(kv.first.as<std::string>().c_str());
					if (GenKnownKeys.Contains(Key)) continue;

					FString ValueStr;
					if (kv.second.is<std::string>())
						ValueStr = UTF8_TO_TCHAR(kv.second.as<std::string>().c_str());
					else if (kv.second.is<double>())
						ValueStr = FString::SanitizeFloat(kv.second.as<double>());
					else if (kv.second.is<int>())
						ValueStr = FString::FromInt(kv.second.as<int>());
					else if (kv.second.is<bool>())
						ValueStr = kv.second.as<bool>() ? TEXT("true") : TEXT("false");
					else
						continue;

					if (!SetPropertyByReflection(Gen, Key, ValueStr))
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure(\"generator\") -> property '%s' not found or could not be set"), *Key));
					}
				}

				NotifyPostEditChange(Gen);
				Query->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"generator\", option=%d)"), OptionIndex));
				return sol::make_object(Lua, true);
			}

			// ---- configure("option", {index=N, generator="ActorsOfClass"}) ----
			if (FType.Equals(TEXT("option"), ESearchCase::IgnoreCase))
			{
				sol::optional<int> OptIdx = Params.get<sol::optional<int>>("index");
				if (!OptIdx.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"option\") -> 'index' required (0-based)"));
					return sol::lua_nil;
				}

				TArray<TObjectPtr<UEnvQueryOption>>& Options = Query->GetOptionsMutable();
				int32 OptionIndex = OptIdx.value();
				if (OptionIndex < 0 || OptionIndex >= Options.Num())
				{
					if (Options.Num() == 0)
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"option\") -> index %d out of range (no options exist)"), OptionIndex));
					else
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"option\") -> index %d out of range (0-%d)"), OptionIndex, Options.Num() - 1));
					return sol::lua_nil;
				}

				UEnvQueryOption* Option = Options[OptionIndex];
				if (!Option)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"option\") -> option %d is null"), OptionIndex));
					return sol::lua_nil;
				}

				// Replace generator if specified
				std::string NewGenName = Params.get_or<std::string>("generator", "");
				if (!NewGenName.empty())
				{
					FString FGenName = UTF8_TO_TCHAR(NewGenName.c_str());
					UClass* GenClass = ResolveClassByName(UEnvQueryGenerator::StaticClass(), FGenName);
					if (!GenClass)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"option\") -> generator class '%s' not found"), *FGenName));
						return sol::lua_nil;
					}

					const FScopedTransaction Transaction(FText::FromString(TEXT("EQS: Replace Generator")));
					Query->Modify();

					// Clean up old generator
					if (Option->Generator)
					{
						Option->Generator->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
					}

					UEnvQueryGenerator* NewGen = NewObject<UEnvQueryGenerator>(Option, GenClass);
					Option->Generator = NewGen;

					NotifyPostEditChange(Query);
					SyncEditorGraph(Query);
					Query->MarkPackageDirty();

					Session.Log(FString::Printf(TEXT("[OK] configure(\"option\", index=%d, generator=\"%s\")"), OptionIndex, *GenClass->GetName()));
					return sol::make_object(Lua, true);
				}

				Session.Log(TEXT("[FAIL] configure(\"option\") -> 'generator' required"));
				return sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: test, generator, option"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// reorder(type, params)
		// ================================================================
		AssetObj.set_function("reorder", [Query, &Session](sol::table /*self*/,
			const std::string& Type, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- reorder("option", {from=N, to=M}) ----
			if (FType.Equals(TEXT("option"), ESearchCase::IgnoreCase))
			{
				sol::optional<int> FromIdx = Params.get<sol::optional<int>>("from");
				sol::optional<int> ToIdx = Params.get<sol::optional<int>>("to");
				if (!FromIdx.has_value() || !ToIdx.has_value())
				{
					Session.Log(TEXT("[FAIL] reorder(\"option\") -> 'from' and 'to' required (0-based)"));
					return sol::lua_nil;
				}

				TArray<TObjectPtr<UEnvQueryOption>>& Options = Query->GetOptionsMutable();
				int32 From = FromIdx.value();
				int32 To = ToIdx.value();

				if (From < 0 || From >= Options.Num() || To < 0 || To >= Options.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] reorder(\"option\") -> indices out of range (0-%d)"), Options.Num() - 1));
					return sol::lua_nil;
				}

				if (From == To)
				{
					Session.Log(FString::Printf(TEXT("[OK] reorder(\"option\", from=%d, to=%d) -> no change"), From, To));
					return sol::make_object(Lua, true);
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("EQS: Reorder Option")));
				Query->Modify();

				TObjectPtr<UEnvQueryOption> MovedOption = Options[From];
				Options.RemoveAt(From);
				Options.Insert(MovedOption, To);

				NotifyPostEditChange(Query);
				SyncEditorGraph(Query);
				Query->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] reorder(\"option\", from=%d, to=%d)"), From, To));
				return sol::make_object(Lua, true);
			}

			// ---- reorder("test", {option=N, from=A, to=B}) ----
			if (FType.Equals(TEXT("test"), ESearchCase::IgnoreCase))
			{
				sol::optional<int> OptIdx = Params.get<sol::optional<int>>("option");
				sol::optional<int> FromIdx = Params.get<sol::optional<int>>("from");
				sol::optional<int> ToIdx = Params.get<sol::optional<int>>("to");
				if (!OptIdx.has_value() || !FromIdx.has_value() || !ToIdx.has_value())
				{
					Session.Log(TEXT("[FAIL] reorder(\"test\") -> 'option', 'from', and 'to' required (0-based)"));
					return sol::lua_nil;
				}

				TArray<TObjectPtr<UEnvQueryOption>>& Options = Query->GetOptionsMutable();
				int32 OptionIndex = OptIdx.value();
				if (OptionIndex < 0 || OptionIndex >= Options.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] reorder(\"test\") -> option %d out of range"), OptionIndex));
					return sol::lua_nil;
				}

				UEnvQueryOption* Option = Options[OptionIndex];
				if (!Option)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] reorder(\"test\") -> option %d is null"), OptionIndex));
					return sol::lua_nil;
				}

				int32 From = FromIdx.value();
				int32 To = ToIdx.value();

				if (From < 0 || From >= Option->Tests.Num() || To < 0 || To >= Option->Tests.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] reorder(\"test\") -> indices out of range (0-%d)"), Option->Tests.Num() - 1));
					return sol::lua_nil;
				}

				if (From == To)
				{
					Session.Log(FString::Printf(TEXT("[OK] reorder(\"test\", option=%d, from=%d, to=%d) -> no change"), OptionIndex, From, To));
					return sol::make_object(Lua, true);
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("EQS: Reorder Test")));
				Query->Modify();

				TObjectPtr<UEnvQueryTest> MovedTest = Option->Tests[From];
				Option->Tests.RemoveAt(From);
				Option->Tests.Insert(MovedTest, To);

				NotifyPostEditChange(Query);
				SyncEditorGraph(Query);
				Query->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] reorder(\"test\", option=%d, from=%d, to=%d)"), OptionIndex, From, To));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] reorder(\"%s\") -> unknown type. Valid: option, test"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// duplicate(type, index)
		// ================================================================
		AssetObj.set_function("duplicate", [Query, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// ---- duplicate("option", N) ----
			if (FType.Equals(TEXT("option"), ESearchCase::IgnoreCase))
			{
				int32 OptionIndex = -1;
				if (Id.is<int>()) OptionIndex = Id.as<int>();
				else if (Id.is<double>()) OptionIndex = static_cast<int32>(Id.as<double>());
				else
				{
					Session.Log(TEXT("[FAIL] duplicate(\"option\") -> 0-based index required"));
					return sol::lua_nil;
				}

				TArray<TObjectPtr<UEnvQueryOption>>& Options = Query->GetOptionsMutable();
				if (OptionIndex < 0 || OptionIndex >= Options.Num())
				{
					if (Options.Num() == 0)
						Session.Log(FString::Printf(TEXT("[FAIL] duplicate(\"option\") -> index %d out of range (no options exist)"), OptionIndex));
					else
						Session.Log(FString::Printf(TEXT("[FAIL] duplicate(\"option\") -> index %d out of range (0-%d)"), OptionIndex, Options.Num() - 1));
					return sol::lua_nil;
				}

				UEnvQueryOption* SourceOption = Options[OptionIndex];
				if (!SourceOption)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] duplicate(\"option\") -> option %d is null"), OptionIndex));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("EQS: Duplicate Option")));
				Query->Modify();

				UEnvQueryOption* NewOption = NewObject<UEnvQueryOption>(Query);

				// Duplicate generator
				if (SourceOption->Generator)
				{
					UEnvQueryGenerator* NewGen = DuplicateObject<UEnvQueryGenerator>(SourceOption->Generator, NewOption);
					NewOption->Generator = NewGen;
				}

				// Duplicate tests
				for (UEnvQueryTest* SourceTest : SourceOption->Tests)
				{
					if (!SourceTest) continue;
					UEnvQueryTest* NewTest = DuplicateObject<UEnvQueryTest>(SourceTest, NewOption);
					NewOption->Tests.Add(NewTest);
				}

				Options.Insert(NewOption, OptionIndex + 1);

				NotifyPostEditChange(Query);
				SyncEditorGraph(Query);
				Query->MarkPackageDirty();

				int32 NewIndex = OptionIndex + 1;
				Session.Log(FString::Printf(TEXT("[OK] duplicate(\"option\", %d) -> new index %d"), OptionIndex, NewIndex));
				return sol::make_object(Lua, NewIndex);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] duplicate(\"%s\") -> unknown type. Valid: option"), *FType));
			return sol::lua_nil;
		});
	});
}

REGISTER_LUA_BINDING(EQS, EQSDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("EnvironmentQueryEditor")))
	{
		Session.Log(TEXT("[WARN] EnvironmentQueryEditor plugin is not loaded. Enable it in Edit > Plugins to use this feature."));
		return;
	}
	BindEQS(Lua, Session);
});

