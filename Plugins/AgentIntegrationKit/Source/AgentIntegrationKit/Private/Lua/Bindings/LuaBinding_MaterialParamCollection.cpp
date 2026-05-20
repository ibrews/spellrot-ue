// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "Materials/MaterialParameterCollection.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// Helpers
// ============================================================================

static sol::table ScalarParamToTable(sol::state_view& Lua, const FCollectionScalarParameter& Param)
{
	sol::table E = Lua.create_table();
	E["name"] = TCHAR_TO_UTF8(*Param.ParameterName.ToString());
	E["default_value"] = Param.DefaultValue;
	E["id"] = TCHAR_TO_UTF8(*Param.Id.ToString());
	return E;
}

static sol::table VectorParamToTable(sol::state_view& Lua, const FCollectionVectorParameter& Param)
{
	sol::table E = Lua.create_table();
	E["name"] = TCHAR_TO_UTF8(*Param.ParameterName.ToString());
	sol::table Color = Lua.create_table();
	Color["r"] = Param.DefaultValue.R;
	Color["g"] = Param.DefaultValue.G;
	Color["b"] = Param.DefaultValue.B;
	Color["a"] = Param.DefaultValue.A;
	E["default_value"] = Color;
	E["id"] = TCHAR_TO_UTF8(*Param.Id.ToString());
	return E;
}

// Wraps PreEditChange + Modify + mutation + PostEditChangeProperty + MarkPackageDirty
static void MPCPreEdit(UMaterialParameterCollection* MPC)
{
	MPC->PreEditChange(nullptr);
	MPC->Modify();
}

static void MPCPostEdit(UMaterialParameterCollection* MPC, EPropertyChangeType::Type ChangeType)
{
	FPropertyChangedEvent Event(nullptr, ChangeType);
	MPC->PostEditChangeProperty(Event);
	MPC->MarkPackageDirty();
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> MaterialParamCollectionDocs = {};

static void BindMaterialParamCollection(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_material_param_collection", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UMaterialParameterCollection* MPC = LoadObject<UMaterialParameterCollection>(nullptr, *FPath);
		if (!MPC) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"MaterialParameterCollection enrichment methods:\n"
			"\n"
			"info() — structured summary:\n"
			"  scalar_param_count, vector_param_count, state_id, base_collection\n"
			"\n"
			"list(type):\n"
			"  list() or list(\"all\") — {scalars=[...], vectors=[...]}\n"
			"  list(\"scalars\") — array of {name, default_value, id}\n"
			"  list(\"vectors\") — array of {name, default_value={r,g,b,a}, id}\n"
			"\n"
			"add(type, params):\n"
			"  add(\"scalar\", {name=\"MyParam\", default_value=0.5})\n"
			"  add(\"vector\", {name=\"MyColor\", default_value={r=1,g=0,b=0,a=1}})\n"
			"\n"
			"remove(type, name):\n"
			"  remove(\"scalar\", \"ParamName\")\n"
			"  remove(\"vector\", \"ParamName\")\n"
			"\n"
			"configure(type, name, params):\n"
			"  configure(\"scalar\", \"ParamName\", {default_value=0.8, new_name=\"Renamed\"})\n"
			"  configure(\"vector\", \"ParamName\", {default_value={r=1,g=1,b=1,a=1}, new_name=\"Renamed\"})\n"
			"  Note: if new_name conflicts with an existing param, the engine may auto-suffix it.\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [MPC, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(MPC))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();
			Result["scalar_param_count"] = MPC->ScalarParameters.Num();
			Result["vector_param_count"] = MPC->VectorParameters.Num();
			Result["state_id"] = TCHAR_TO_UTF8(*MPC->StateId.ToString());

	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			UMaterialParameterCollection* Base = MPC->GetBaseParameterCollection();
			if (Base)
			{
				Result["base_collection"] = TCHAR_TO_UTF8(*Base->GetPathName());
			}
#endif

			Session.Log(FString::Printf(TEXT("[OK] info() -> MPC, %d scalars, %d vectors"),
				MPC->ScalarParameters.Num(), MPC->VectorParameters.Num()));
			return Result;
		});

		// ================================================================
		// list(type?)
		// ================================================================
		AssetObj.set_function("list", [MPC, &Session](sol::table /*self*/,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			if (!IsValid(MPC))
			{
				Session.Log(TEXT("[FAIL] list -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();

				sol::table Scalars = Lua.create_table();
				for (int32 i = 0; i < MPC->ScalarParameters.Num(); i++)
				{
					Scalars[i + 1] = ScalarParamToTable(Lua, MPC->ScalarParameters[i]);
				}
				Result["scalars"] = Scalars;

				sol::table Vectors = Lua.create_table();
				for (int32 i = 0; i < MPC->VectorParameters.Num(); i++)
				{
					Vectors[i + 1] = VectorParamToTable(Lua, MPC->VectorParameters[i]);
				}
				Result["vectors"] = Vectors;

				Session.Log(FString::Printf(TEXT("[OK] list(\"all\") -> %d scalars, %d vectors"),
					MPC->ScalarParameters.Num(), MPC->VectorParameters.Num()));
				return Result;
			}

			if (FType.Equals(TEXT("scalars"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < MPC->ScalarParameters.Num(); i++)
				{
					Result[i + 1] = ScalarParamToTable(Lua, MPC->ScalarParameters[i]);
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"scalars\") -> %d"), MPC->ScalarParameters.Num()));
				return Result;
			}

			if (FType.Equals(TEXT("vectors"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < MPC->VectorParameters.Num(); i++)
				{
					Result[i + 1] = VectorParamToTable(Lua, MPC->VectorParameters[i]);
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"vectors\") -> %d"), MPC->VectorParameters.Num()));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: all, scalars, vectors"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// add(type, params)
		// ================================================================
		AssetObj.set_function("add", [MPC, &Session](sol::table /*self*/,
			std::string TypeStr, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(MPC))
			{
				Session.Log(TEXT("[FAIL] add -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FType = UTF8_TO_TCHAR(TypeStr.c_str());
			std::string NameStr = Params.get_or<std::string>("name", "");
			if (NameStr.empty())
			{
				Session.Log(TEXT("[FAIL] add -> 'name' is required"));
				return sol::lua_nil;
			}
			FName ParamName = FName(UTF8_TO_TCHAR(NameStr.c_str()));

			if (FType.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
			{
				if (MPC->GetScalarParameterByName(ParamName) != nullptr)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"scalar\") -> '%s' already exists"), *ParamName.ToString()));
					return sol::lua_nil;
				}

				FCollectionScalarParameter NewParam;
				NewParam.ParameterName = ParamName;
				NewParam.DefaultValue = static_cast<float>(Params.get_or("default_value", 0.0));

				const FScopedTransaction Transaction(FText::FromString(TEXT("MPC: Add Scalar Parameter")));
				MPCPreEdit(MPC);
				MPC->ScalarParameters.Add(NewParam);
				MPCPostEdit(MPC, EPropertyChangeType::ArrayAdd);

				Session.Log(FString::Printf(TEXT("[OK] add(\"scalar\", \"%s\") -> default=%.4f"),
					*ParamName.ToString(), NewParam.DefaultValue + 0.0));
				return sol::make_object(Lua, true);
			}

			if (FType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
			{
				if (MPC->GetVectorParameterByName(ParamName) != nullptr)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"vector\") -> '%s' already exists"), *ParamName.ToString()));
					return sol::lua_nil;
				}

				FCollectionVectorParameter NewParam;
				NewParam.ParameterName = ParamName;

				sol::optional<sol::table> ColorOpt = Params.get<sol::optional<sol::table>>("default_value");
				if (ColorOpt.has_value())
				{
					sol::table Color = ColorOpt.value();
					NewParam.DefaultValue.R = static_cast<float>(Color.get_or("r", 0.0));
					NewParam.DefaultValue.G = static_cast<float>(Color.get_or("g", 0.0));
					NewParam.DefaultValue.B = static_cast<float>(Color.get_or("b", 0.0));
					NewParam.DefaultValue.A = static_cast<float>(Color.get_or("a", 1.0));
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("MPC: Add Vector Parameter")));
				MPCPreEdit(MPC);
				MPC->VectorParameters.Add(NewParam);
				MPCPostEdit(MPC, EPropertyChangeType::ArrayAdd);

				Session.Log(FString::Printf(TEXT("[OK] add(\"vector\", \"%s\") -> default=(%.2f, %.2f, %.2f, %.2f)"),
					*ParamName.ToString(),
					NewParam.DefaultValue.R + 0.0, NewParam.DefaultValue.G + 0.0,
					NewParam.DefaultValue.B + 0.0, NewParam.DefaultValue.A + 0.0));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: scalar, vector"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// remove(type, name)
		// ================================================================
		AssetObj.set_function("remove", [MPC, &Session](sol::table /*self*/,
			std::string TypeStr, std::string NameStr, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(MPC))
			{
				Session.Log(TEXT("[FAIL] remove -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FType = UTF8_TO_TCHAR(TypeStr.c_str());
			FName ParamName = FName(UTF8_TO_TCHAR(NameStr.c_str()));

			if (FType.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
			{
				int32 Idx = MPC->ScalarParameters.IndexOfByPredicate([&ParamName](const FCollectionScalarParameter& P) { return P.ParameterName == ParamName; });
				if (Idx == INDEX_NONE)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"scalar\", \"%s\") -> not found"), *ParamName.ToString()));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("MPC: Remove Scalar Parameter")));
				MPCPreEdit(MPC);
				MPC->ScalarParameters.RemoveAt(Idx);
				MPCPostEdit(MPC, EPropertyChangeType::ArrayRemove);

				Session.Log(FString::Printf(TEXT("[OK] remove(\"scalar\", \"%s\")"), *ParamName.ToString()));
				return sol::make_object(Lua, true);
			}

			if (FType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
			{
				int32 Idx = MPC->VectorParameters.IndexOfByPredicate([&ParamName](const FCollectionVectorParameter& P) { return P.ParameterName == ParamName; });
				if (Idx == INDEX_NONE)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"vector\", \"%s\") -> not found"), *ParamName.ToString()));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("MPC: Remove Vector Parameter")));
				MPCPreEdit(MPC);
				MPC->VectorParameters.RemoveAt(Idx);
				MPCPostEdit(MPC, EPropertyChangeType::ArrayRemove);

				Session.Log(FString::Printf(TEXT("[OK] remove(\"vector\", \"%s\")"), *ParamName.ToString()));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: scalar, vector"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// configure(type, name, params)
		// ================================================================
		AssetObj.set_function("configure", [MPC, &Session](sol::table /*self*/,
			std::string TypeStr, std::string NameStr, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(MPC))
			{
				Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FType = UTF8_TO_TCHAR(TypeStr.c_str());
			FName ParamName = FName(UTF8_TO_TCHAR(NameStr.c_str()));

			if (FType.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
			{
				int32 Idx = MPC->ScalarParameters.IndexOfByPredicate([&ParamName](const FCollectionScalarParameter& P) { return P.ParameterName == ParamName; });
				if (Idx == INDEX_NONE)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"scalar\", \"%s\") -> not found"), *ParamName.ToString()));
					return sol::lua_nil;
				}

				FString Changes;
				bool bChanged = false;

				sol::optional<double> DefaultVal = Params.get<sol::optional<double>>("default_value");
				if (DefaultVal.has_value())
				{
					bChanged = true;
				}

				std::string NewNameStr = Params.get_or<std::string>("new_name", "");
				FName NewName;
				if (!NewNameStr.empty())
				{
					NewName = FName(UTF8_TO_TCHAR(NewNameStr.c_str()));
					bChanged = true;
				}

				if (!bChanged)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"scalar\", \"%s\") -> no changes specified"), *ParamName.ToString()));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("MPC: Configure Scalar Parameter")));
				MPCPreEdit(MPC);

				FCollectionScalarParameter& Param = MPC->ScalarParameters[Idx];

				if (DefaultVal.has_value())
				{
					Param.DefaultValue = static_cast<float>(DefaultVal.value());
					Changes += FString::Printf(TEXT(" default_value=%.4f"), Param.DefaultValue + 0.0);
				}

				if (!NewNameStr.empty())
				{
					if (MPC->GetScalarParameterByName(NewName) != nullptr)
					{
						Changes += FString::Printf(TEXT(" name=%s (warning: name exists, engine may auto-suffix)"), *NewName.ToString());
					}
					else
					{
						Changes += FString::Printf(TEXT(" name=%s"), *NewName.ToString());
					}
					Param.ParameterName = NewName;
				}

				MPCPostEdit(MPC, EPropertyChangeType::ValueSet);

				Session.Log(FString::Printf(TEXT("[OK] configure(\"scalar\", \"%s\",%s)"),
					*ParamName.ToString(), *Changes));
				return sol::make_object(Lua, true);
			}

			if (FType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
			{
				int32 Idx = MPC->VectorParameters.IndexOfByPredicate([&ParamName](const FCollectionVectorParameter& P) { return P.ParameterName == ParamName; });
				if (Idx == INDEX_NONE)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"vector\", \"%s\") -> not found"), *ParamName.ToString()));
					return sol::lua_nil;
				}

				FString Changes;
				bool bChanged = false;

				sol::optional<sol::table> ColorOpt = Params.get<sol::optional<sol::table>>("default_value");
				if (ColorOpt.has_value())
				{
					bChanged = true;
				}

				std::string NewNameStr = Params.get_or<std::string>("new_name", "");
				FName NewName;
				if (!NewNameStr.empty())
				{
					NewName = FName(UTF8_TO_TCHAR(NewNameStr.c_str()));
					bChanged = true;
				}

				if (!bChanged)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"vector\", \"%s\") -> no changes specified"), *ParamName.ToString()));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("MPC: Configure Vector Parameter")));
				MPCPreEdit(MPC);

				FCollectionVectorParameter& Param = MPC->VectorParameters[Idx];

				if (ColorOpt.has_value())
				{
					sol::table Color = ColorOpt.value();
					Param.DefaultValue.R = static_cast<float>(Color.get_or("r", static_cast<double>(Param.DefaultValue.R)));
					Param.DefaultValue.G = static_cast<float>(Color.get_or("g", static_cast<double>(Param.DefaultValue.G)));
					Param.DefaultValue.B = static_cast<float>(Color.get_or("b", static_cast<double>(Param.DefaultValue.B)));
					Param.DefaultValue.A = static_cast<float>(Color.get_or("a", static_cast<double>(Param.DefaultValue.A)));
					Changes += FString::Printf(TEXT(" default_value=(%.2f,%.2f,%.2f,%.2f)"),
						Param.DefaultValue.R + 0.0, Param.DefaultValue.G + 0.0,
						Param.DefaultValue.B + 0.0, Param.DefaultValue.A + 0.0);
				}

				if (!NewNameStr.empty())
				{
					if (MPC->GetVectorParameterByName(NewName) != nullptr)
					{
						Changes += FString::Printf(TEXT(" name=%s (warning: name exists, engine may auto-suffix)"), *NewName.ToString());
					}
					else
					{
						Changes += FString::Printf(TEXT(" name=%s"), *NewName.ToString());
					}
					Param.ParameterName = NewName;
				}

				MPCPostEdit(MPC, EPropertyChangeType::ValueSet);

				Session.Log(FString::Printf(TEXT("[OK] configure(\"vector\", \"%s\",%s)"),
					*ParamName.ToString(), *Changes));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: scalar, vector"), *FType));
			return sol::lua_nil;
		});
	});
}

REGISTER_LUA_BINDING(MaterialParamCollection, MaterialParamCollectionDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindMaterialParamCollection(Lua, Session);
});
