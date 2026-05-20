// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"

#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "ScopedTransaction.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// Helpers
// ============================================================================

namespace
{

const char* InputTypeToString(EFunctionInputType Type)
{
	switch (Type)
	{
	case FunctionInput_Scalar:              return "Scalar";
	case FunctionInput_Vector2:             return "Vector2";
	case FunctionInput_Vector3:             return "Vector3";
	case FunctionInput_Vector4:             return "Vector4";
	case FunctionInput_Texture2D:           return "Texture2D";
	case FunctionInput_TextureCube:         return "TextureCube";
	case FunctionInput_Texture2DArray:      return "Texture2DArray";
	case FunctionInput_VolumeTexture:       return "VolumeTexture";
	case FunctionInput_StaticBool:          return "StaticBool";
	case FunctionInput_MaterialAttributes: return "MaterialAttributes";
	case FunctionInput_TextureExternal:     return "TextureExternal";
	case FunctionInput_Bool:                return "Bool";
	case FunctionInput_Substrate:           return "Substrate";
	default:                                return "Unknown";
	}
}

EFunctionInputType StringToInputType(const FString& Str)
{
	if (Str.Equals(TEXT("Scalar"), ESearchCase::IgnoreCase))             return FunctionInput_Scalar;
	if (Str.Equals(TEXT("Vector2"), ESearchCase::IgnoreCase))            return FunctionInput_Vector2;
	if (Str.Equals(TEXT("Vector3"), ESearchCase::IgnoreCase))            return FunctionInput_Vector3;
	if (Str.Equals(TEXT("Vector4"), ESearchCase::IgnoreCase))            return FunctionInput_Vector4;
	if (Str.Equals(TEXT("Texture2D"), ESearchCase::IgnoreCase))          return FunctionInput_Texture2D;
	if (Str.Equals(TEXT("TextureCube"), ESearchCase::IgnoreCase))        return FunctionInput_TextureCube;
	if (Str.Equals(TEXT("Texture2DArray"), ESearchCase::IgnoreCase))     return FunctionInput_Texture2DArray;
	if (Str.Equals(TEXT("VolumeTexture"), ESearchCase::IgnoreCase))      return FunctionInput_VolumeTexture;
	if (Str.Equals(TEXT("StaticBool"), ESearchCase::IgnoreCase))         return FunctionInput_StaticBool;
	if (Str.Equals(TEXT("MaterialAttributes"), ESearchCase::IgnoreCase)) return FunctionInput_MaterialAttributes;
	if (Str.Equals(TEXT("TextureExternal"), ESearchCase::IgnoreCase))    return FunctionInput_TextureExternal;
	if (Str.Equals(TEXT("Bool"), ESearchCase::IgnoreCase))               return FunctionInput_Bool;
	if (Str.Equals(TEXT("Substrate"), ESearchCase::IgnoreCase))          return FunctionInput_Substrate;
	return FunctionInput_Scalar;
}

const char* UsageToString(EMaterialFunctionUsage Usage)
{
	switch (Usage)
	{
	case EMaterialFunctionUsage::Default:            return "Default";
	case EMaterialFunctionUsage::MaterialLayer:      return "MaterialLayer";
	case EMaterialFunctionUsage::MaterialLayerBlend: return "MaterialLayerBlend";
	default:                                         return "Unknown";
	}
}

EMaterialFunctionUsage StringToUsage(const FString& Str)
{
	if (Str.Equals(TEXT("MaterialLayer"), ESearchCase::IgnoreCase))      return EMaterialFunctionUsage::MaterialLayer;
	if (Str.Equals(TEXT("MaterialLayerBlend"), ESearchCase::IgnoreCase)) return EMaterialFunctionUsage::MaterialLayerBlend;
	return EMaterialFunctionUsage::Default;
}

/** Break any links from other expressions to the target expression. */
void BreakLinksToExpression(TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions, UMaterialExpression* Target)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	for (UMaterialExpression* TestExpr : Expressions)
	{
		if (!TestExpr || TestExpr == Target) continue;
		for (FExpressionInputIterator It{ TestExpr }; It; ++It)
		{
			if (It->Expression == Target)
			{
				It->Expression = nullptr;
			}
		}
	}
#else
	for (UMaterialExpression* TestExpr : Expressions)
	{
		if (!TestExpr || TestExpr == Target) continue;
		const TArray<FExpressionInput*> Inputs = TestExpr->GetInputs();
		for (FExpressionInput* Input : Inputs)
		{
			if (Input && Input->Expression == Target)
			{
				Input->Expression = nullptr;
			}
		}
	}
#endif
}

} // anonymous namespace

// ============================================================================
// Material Function Enrichment
// ============================================================================

static TArray<FLuaFunctionDoc> MaterialFunctionDocs = {};

static void BindMaterialFunction(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_material_function", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UMaterialFunction* MatFunc = LoadObject<UMaterialFunction>(nullptr, *FPath);
		if (!MatFunc) return;

		AssetObj["_help_text"] =
			"MaterialFunction enrichment:\n"
			"\n"
			"info() -> structured summary (description, user_exposed_caption, is_exposed_to_library,\n"
			"  usage, library_categories, expression_count, input_count, output_count,\n"
			"  enable_new_hlsl_generator)\n"
			"\n"
			"list(type):\n"
			"  list(\"inputs\")       -> {name, description, input_type, sort_priority, use_preview_as_default}\n"
			"  list(\"outputs\")      -> {name, description, sort_priority}\n"
			"  list(\"expressions\")  -> {index, class, name, desc, position}\n"
			"  list(\"categories\")   -> array of library category strings\n"
			"  list(\"dependencies\") -> array of dependent function paths\n"
			"\n"
			"add(type, params):\n"
			"  add(\"input\", {name=\"MyInput\", input_type=\"Scalar\", description=\"...\",\n"
			"    sort_priority=0, use_preview_as_default=false, preview_value={x,y,z,w},\n"
			"    position={x,y}})                   -> add function input expression\n"
			"  add(\"output\", {name=\"MyOutput\", description=\"...\", sort_priority=0,\n"
			"    position={x,y}})                    -> add function output expression\n"
			"\n"
			"remove(type, params):\n"
			"  remove(\"expression\", {index=N})      -> remove expression by index (1-based)\n"
			"  remove(\"input\", {name=\"MyInput\"})    -> remove function input by name\n"
			"  remove(\"output\", {name=\"MyOutput\"})  -> remove function output by name\n"
			"\n"
			"configure(params):\n"
			"  configure({description=\"My func\"})                 -> set description\n"
			"  configure({user_exposed_caption=\"CustomName\"})     -> set display name\n"
			"  configure({expose_to_library=true})                 -> toggle library exposure\n"
			"  configure({library_categories={\"Math\",\"Utils\"}})   -> set categories\n"
			"  configure({enable_new_hlsl_generator=true})         -> toggle HLSL generator\n"
			"  configure({usage=\"Default|MaterialLayer|MaterialLayerBlend\"}) -> set usage\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [MatFunc, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(MatFunc))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();

			Result["description"] = TCHAR_TO_UTF8(*MatFunc->Description);
			Result["user_exposed_caption"] = TCHAR_TO_UTF8(*MatFunc->UserExposedCaption);
			Result["is_exposed_to_library"] = (bool)MatFunc->bExposeToLibrary;
			Result["usage"] = UsageToString(MatFunc->GetMaterialFunctionUsage());
			Result["enable_new_hlsl_generator"] = (bool)MatFunc->bEnableNewHLSLGenerator;

#if WITH_EDITOR
			// Library categories
			sol::table CatsArr = Lua.create_table();
			int32 CatIdx = 1;
			for (const FText& Cat : MatFunc->LibraryCategoriesText)
			{
				CatsArr[CatIdx++] = TCHAR_TO_UTF8(*Cat.ToString());
			}
			Result["library_categories"] = CatsArr;

			// Expression count
			TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = MatFunc->GetExpressions();
			Result["expression_count"] = Expressions.Num();

			// Input/output counts via GetInputsAndOutputs
			TArray<FFunctionExpressionInput> Inputs;
			TArray<FFunctionExpressionOutput> Outputs;
			MatFunc->GetInputsAndOutputs(Inputs, Outputs);
			Result["input_count"] = Inputs.Num();
			Result["output_count"] = Outputs.Num();

			// Comments count
			TConstArrayView<TObjectPtr<UMaterialExpressionComment>> Comments = MatFunc->GetEditorComments();
			Result["comment_count"] = Comments.Num();
#endif

			Session.Log(TEXT("[OK] info() -> MaterialFunction"));
			return Result;
		});

		// ================================================================
		// list(type)
		// ================================================================
		AssetObj.set_function("list", [MatFunc, &Session](sol::table /*self*/,
			sol::optional<std::string> TypeOpt, sol::optional<sol::table> /*OptsOpt*/,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(MatFunc))
			{
				Session.Log(TEXT("[FAIL] list -> asset no longer valid"));
				return sol::lua_nil;
			}
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

#if WITH_EDITOR
			// ---- inputs ----
			if (FType.Equals(TEXT("inputs"), ESearchCase::IgnoreCase))
			{
				TArray<FFunctionExpressionInput> Inputs;
				TArray<FFunctionExpressionOutput> Outputs;
				MatFunc->GetInputsAndOutputs(Inputs, Outputs);

				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (const FFunctionExpressionInput& FI : Inputs)
				{
					UMaterialExpressionFunctionInput* Expr = FI.ExpressionInput;
					if (!Expr) continue;

					sol::table Entry = Lua.create_table();
					Entry["index"] = Idx;
					Entry["name"] = TCHAR_TO_UTF8(*Expr->InputName.ToString());
					Entry["description"] = TCHAR_TO_UTF8(*Expr->Description);
					Entry["input_type"] = InputTypeToString(Expr->InputType);
					Entry["sort_priority"] = Expr->SortPriority;
					Entry["use_preview_as_default"] = (bool)Expr->bUsePreviewValueAsDefault;

					sol::table PV = Lua.create_table();
					PV["x"] = Expr->PreviewValue.X;
					PV["y"] = Expr->PreviewValue.Y;
					PV["z"] = Expr->PreviewValue.Z;
					PV["w"] = Expr->PreviewValue.W;
					Entry["preview_value"] = PV;

					Result[Idx++] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"inputs\") -> %d inputs"), Idx - 1));
				return Result;
			}

			// ---- outputs ----
			if (FType.Equals(TEXT("outputs"), ESearchCase::IgnoreCase))
			{
				TArray<FFunctionExpressionInput> Inputs;
				TArray<FFunctionExpressionOutput> Outputs;
				MatFunc->GetInputsAndOutputs(Inputs, Outputs);

				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (const FFunctionExpressionOutput& FO : Outputs)
				{
					UMaterialExpressionFunctionOutput* Expr = FO.ExpressionOutput;
					if (!Expr) continue;

					sol::table Entry = Lua.create_table();
					Entry["index"] = Idx;
					Entry["name"] = TCHAR_TO_UTF8(*Expr->OutputName.ToString());
					Entry["description"] = TCHAR_TO_UTF8(*Expr->Description);
					Entry["sort_priority"] = Expr->SortPriority;

					Result[Idx++] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"outputs\") -> %d outputs"), Idx - 1));
				return Result;
			}

			// ---- expressions ----
			if (FType.Equals(TEXT("expressions"), ESearchCase::IgnoreCase))
			{
				TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = MatFunc->GetExpressions();

				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (const TObjectPtr<UMaterialExpression>& ExprPtr : Expressions)
				{
					UMaterialExpression* Expr = ExprPtr.Get();
					if (!Expr) continue;

					sol::table Entry = Lua.create_table();
					Entry["index"] = Idx;
					Entry["class"] = TCHAR_TO_UTF8(*Expr->GetClass()->GetName());

					// Description from the expression
					Entry["desc"] = TCHAR_TO_UTF8(*Expr->Desc);

					// Caption (node title)
					TArray<FString> Captions;
					Expr->GetCaption(Captions);
					if (Captions.Num() > 0)
					{
						Entry["name"] = TCHAR_TO_UTF8(*Captions[0]);
					}

					// Position
					sol::table Pos = Lua.create_table();
					Pos["x"] = Expr->MaterialExpressionEditorX;
					Pos["y"] = Expr->MaterialExpressionEditorY;
					Entry["position"] = Pos;

					// If it's a function call, show which function
					if (UMaterialExpressionMaterialFunctionCall* FuncCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expr))
					{
						if (FuncCall->MaterialFunction)
						{
							Entry["called_function"] = TCHAR_TO_UTF8(*FuncCall->MaterialFunction->GetPathName());
						}
					}

					Result[Idx++] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"expressions\") -> %d expressions"), Idx - 1));
				return Result;
			}

			// ---- categories ----
			if (FType.Equals(TEXT("categories"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (const FText& Cat : MatFunc->LibraryCategoriesText)
				{
					Result[Idx++] = TCHAR_TO_UTF8(*Cat.ToString());
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"categories\") -> %d categories"), Idx - 1));
				return Result;
			}

			// ---- dependencies ----
			if (FType.Equals(TEXT("dependencies"), ESearchCase::IgnoreCase))
			{
				TArray<UMaterialFunctionInterface*> DepFunctions;
				MatFunc->GetDependentFunctions(DepFunctions);

				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (UMaterialFunctionInterface* Dep : DepFunctions)
				{
					if (!Dep) continue;
					Result[Idx++] = TCHAR_TO_UTF8(*Dep->GetPathName());
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"dependencies\") -> %d dependencies"), Idx - 1));
				return Result;
			}
#endif // WITH_EDITOR

			// ---- all (summary) ----
			sol::table Result = Lua.create_table();
			Result["available_types"] = "inputs, outputs, expressions, categories, dependencies";
			Session.Log(TEXT("[OK] list() -> available types"));
			return Result;
		});

		// ================================================================
		// add(type, params)
		// ================================================================
		AssetObj.set_function("add", [MatFunc, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> ParamsOpt,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(MatFunc))
			{
				Session.Log(TEXT("[FAIL] add -> asset no longer valid"));
				return sol::lua_nil;
			}
			FString FType = UTF8_TO_TCHAR(Type.c_str());

#if WITH_EDITOR
			// ---- add input ----
			if (FType.Equals(TEXT("input"), ESearchCase::IgnoreCase))
			{
				if (!ParamsOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"input\") -> {name=...} required"));
					return sol::lua_nil;
				}
				sol::table P = ParamsOpt.value();
				std::string Name = P.get_or<std::string>("name", "");
				if (Name.empty())
				{
					Session.Log(TEXT("[FAIL] add(\"input\") -> name is required"));
					return sol::lua_nil;
				}

				FScopedTransaction Tx(FText::FromString(TEXT("Add Material Function Input")));
				MatFunc->Modify();

				UMaterialExpressionFunctionInput* NewInput = NewObject<UMaterialExpressionFunctionInput>(
					MatFunc, NAME_None, RF_Transactional);

				NewInput->InputName = FName(UTF8_TO_TCHAR(Name.c_str()));

				std::string Desc = P.get_or<std::string>("description", "");
				if (!Desc.empty())
				{
					NewInput->Description = UTF8_TO_TCHAR(Desc.c_str());
				}

				std::string InputTypeStr = P.get_or<std::string>("input_type", "");
				if (!InputTypeStr.empty())
				{
					NewInput->InputType = StringToInputType(UTF8_TO_TCHAR(InputTypeStr.c_str()));
				}

				sol::optional<int> SortOpt = P.get<sol::optional<int>>("sort_priority");
				if (SortOpt.has_value())
				{
					NewInput->SortPriority = SortOpt.value();
				}

				sol::optional<bool> PreviewDefaultOpt = P.get<sol::optional<bool>>("use_preview_as_default");
				if (PreviewDefaultOpt.has_value())
				{
					NewInput->bUsePreviewValueAsDefault = PreviewDefaultOpt.value();
				}

				sol::optional<sol::table> PVOpt = P.get<sol::optional<sol::table>>("preview_value");
				if (PVOpt.has_value())
				{
					sol::table PV = PVOpt.value();
					NewInput->PreviewValue.X = PV.get_or("x", 0.0f);
					NewInput->PreviewValue.Y = PV.get_or("y", 0.0f);
					NewInput->PreviewValue.Z = PV.get_or("z", 0.0f);
					NewInput->PreviewValue.W = PV.get_or("w", 0.0f);
				}

				sol::optional<sol::table> PosOpt = P.get<sol::optional<sol::table>>("position");
				if (PosOpt.has_value())
				{
					sol::table Pos = PosOpt.value();
					NewInput->MaterialExpressionEditorX = Pos.get_or("x", 0);
					NewInput->MaterialExpressionEditorY = Pos.get_or("y", 0);
				}

				// Generate unique Id for this input
				NewInput->ConditionallyGenerateId(true);
				NewInput->UpdateMaterialExpressionGuid(true, true);

				// Validate name to prevent duplicates
				NewInput->ValidateName();

				// Add to expression collection
				MatFunc->GetExpressionCollection().AddExpression(NewInput);

				MatFunc->MarkPackageDirty();

				FString InputNameStr = UTF8_TO_TCHAR(Name.c_str());
				Session.Log(FString::Printf(TEXT("[OK] add(\"input\", name=\"%s\")"), *InputNameStr));

				sol::table Result = Lua.create_table();
				Result["name"] = Name;
				Result["input_type"] = InputTypeToString(NewInput->InputType);
				return Result;
			}

			// ---- add output ----
			if (FType.Equals(TEXT("output"), ESearchCase::IgnoreCase))
			{
				if (!ParamsOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"output\") -> {name=...} required"));
					return sol::lua_nil;
				}
				sol::table P = ParamsOpt.value();
				std::string Name = P.get_or<std::string>("name", "");
				if (Name.empty())
				{
					Session.Log(TEXT("[FAIL] add(\"output\") -> name is required"));
					return sol::lua_nil;
				}

				FScopedTransaction Tx(FText::FromString(TEXT("Add Material Function Output")));
				MatFunc->Modify();

				UMaterialExpressionFunctionOutput* NewOutput = NewObject<UMaterialExpressionFunctionOutput>(
					MatFunc, NAME_None, RF_Transactional);

				NewOutput->OutputName = FName(UTF8_TO_TCHAR(Name.c_str()));

				std::string Desc = P.get_or<std::string>("description", "");
				if (!Desc.empty())
				{
					NewOutput->Description = UTF8_TO_TCHAR(Desc.c_str());
				}

				sol::optional<int> SortOpt = P.get<sol::optional<int>>("sort_priority");
				if (SortOpt.has_value())
				{
					NewOutput->SortPriority = SortOpt.value();
				}

				sol::optional<sol::table> PosOpt = P.get<sol::optional<sol::table>>("position");
				if (PosOpt.has_value())
				{
					sol::table Pos = PosOpt.value();
					NewOutput->MaterialExpressionEditorX = Pos.get_or("x", 0);
					NewOutput->MaterialExpressionEditorY = Pos.get_or("y", 0);
				}

				// Generate unique Id for this output
				NewOutput->ConditionallyGenerateId(true);
				NewOutput->UpdateMaterialExpressionGuid(true, true);

				// Validate name to prevent duplicates
				NewOutput->ValidateName();

				// Add to expression collection
				MatFunc->GetExpressionCollection().AddExpression(NewOutput);

				MatFunc->MarkPackageDirty();

				FString OutputNameStr = UTF8_TO_TCHAR(Name.c_str());
				Session.Log(FString::Printf(TEXT("[OK] add(\"output\", name=\"%s\")"), *OutputNameStr));

				sol::table Result = Lua.create_table();
				Result["name"] = Name;
				return Result;
			}
#endif // WITH_EDITOR

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type (expected: input, output)"),
				*FType));
			return sol::lua_nil;
		});

		// ================================================================
		// remove(type, params)
		// ================================================================
		AssetObj.set_function("remove", [MatFunc, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> ParamsOpt,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(MatFunc))
			{
				Session.Log(TEXT("[FAIL] remove -> asset no longer valid"));
				return sol::lua_nil;
			}
			FString FType = UTF8_TO_TCHAR(Type.c_str());

#if WITH_EDITOR
			// ---- remove expression by index ----
			if (FType.Equals(TEXT("expression"), ESearchCase::IgnoreCase))
			{
				if (!ParamsOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] remove(\"expression\") -> {index=N} required"));
					return sol::lua_nil;
				}
				sol::table P = ParamsOpt.value();
				sol::optional<int> IdxOpt = P.get<sol::optional<int>>("index");
				if (!IdxOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] remove(\"expression\") -> index is required"));
					return sol::lua_nil;
				}

				int32 LuaIdx = IdxOpt.value(); // 1-based
				TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = MatFunc->GetExpressions();
				int32 EngineIdx = LuaIdx - 1; // 0-based

				if (EngineIdx < 0 || EngineIdx >= Expressions.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"expression\") -> index %d out of range (1-%d)"),
						LuaIdx, Expressions.Num()));
					return sol::lua_nil;
				}

				UMaterialExpression* Expr = Expressions[EngineIdx];
				if (!Expr)
				{
					Session.Log(TEXT("[FAIL] remove(\"expression\") -> expression is null"));
					return sol::lua_nil;
				}

				FScopedTransaction Tx(FText::FromString(TEXT("Remove Material Function Expression")));
				MatFunc->Modify();

				// Break links from other expressions to this one
				BreakLinksToExpression(MatFunc->GetExpressions(), Expr);

				MatFunc->GetExpressionCollection().RemoveExpression(Expr);
				Expr->MarkAsGarbage();
				MatFunc->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"expression\", index=%d)"), LuaIdx));
				return sol::make_object(Lua, true);
			}

			// ---- remove input by name ----
			if (FType.Equals(TEXT("input"), ESearchCase::IgnoreCase))
			{
				if (!ParamsOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] remove(\"input\") -> {name=...} required"));
					return sol::lua_nil;
				}
				sol::table P = ParamsOpt.value();
				std::string Name = P.get_or<std::string>("name", "");
				if (Name.empty())
				{
					Session.Log(TEXT("[FAIL] remove(\"input\") -> name is required"));
					return sol::lua_nil;
				}

				FName TargetName(UTF8_TO_TCHAR(Name.c_str()));
				UMaterialExpressionFunctionInput* Found = nullptr;

				TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = MatFunc->GetExpressions();
				for (const TObjectPtr<UMaterialExpression>& ExprPtr : Expressions)
				{
					if (UMaterialExpressionFunctionInput* Input = Cast<UMaterialExpressionFunctionInput>(ExprPtr.Get()))
					{
						if (Input->InputName == TargetName)
						{
							Found = Input;
							break;
						}
					}
				}

				if (!Found)
				{
					FString NameStr = UTF8_TO_TCHAR(Name.c_str());
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"input\") -> input '%s' not found"), *NameStr));
					return sol::lua_nil;
				}

				FScopedTransaction Tx(FText::FromString(TEXT("Remove Material Function Input")));
				MatFunc->Modify();

				BreakLinksToExpression(MatFunc->GetExpressions(), Found);
				MatFunc->GetExpressionCollection().RemoveExpression(Found);
				Found->MarkAsGarbage();
				MatFunc->MarkPackageDirty();

				FString NameStr = UTF8_TO_TCHAR(Name.c_str());
				Session.Log(FString::Printf(TEXT("[OK] remove(\"input\", name=\"%s\")"), *NameStr));
				return sol::make_object(Lua, true);
			}

			// ---- remove output by name ----
			if (FType.Equals(TEXT("output"), ESearchCase::IgnoreCase))
			{
				if (!ParamsOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] remove(\"output\") -> {name=...} required"));
					return sol::lua_nil;
				}
				sol::table P = ParamsOpt.value();
				std::string Name = P.get_or<std::string>("name", "");
				if (Name.empty())
				{
					Session.Log(TEXT("[FAIL] remove(\"output\") -> name is required"));
					return sol::lua_nil;
				}

				FName TargetName(UTF8_TO_TCHAR(Name.c_str()));
				UMaterialExpressionFunctionOutput* Found = nullptr;

				TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = MatFunc->GetExpressions();
				for (const TObjectPtr<UMaterialExpression>& ExprPtr : Expressions)
				{
					if (UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(ExprPtr.Get()))
					{
						if (Output->OutputName == TargetName)
						{
							Found = Output;
							break;
						}
					}
				}

				if (!Found)
				{
					FString NameStr = UTF8_TO_TCHAR(Name.c_str());
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"output\") -> output '%s' not found"), *NameStr));
					return sol::lua_nil;
				}

				FScopedTransaction Tx(FText::FromString(TEXT("Remove Material Function Output")));
				MatFunc->Modify();

				BreakLinksToExpression(MatFunc->GetExpressions(), Found);
				MatFunc->GetExpressionCollection().RemoveExpression(Found);
				Found->MarkAsGarbage();
				MatFunc->MarkPackageDirty();

				FString NameStr = UTF8_TO_TCHAR(Name.c_str());
				Session.Log(FString::Printf(TEXT("[OK] remove(\"output\", name=\"%s\")"), *NameStr));
				return sol::make_object(Lua, true);
			}
#endif // WITH_EDITOR

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type (expected: expression, input, output)"),
				*FType));
			return sol::lua_nil;
		});

		// ================================================================
		// configure(params)
		// ================================================================
		AssetObj.set_function("configure", [MatFunc, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(MatFunc))
			{
				Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
				return sol::lua_nil;
			}

			FScopedTransaction Tx(FText::FromString(TEXT("Configure Material Function")));
			MatFunc->Modify();

			int32 ChangeCount = 0;
			FProperty* LastChangedProp = nullptr;

			// description
			sol::optional<std::string> DescOpt = Params.get<sol::optional<std::string>>("description");
			if (DescOpt.has_value())
			{
				MatFunc->Description = UTF8_TO_TCHAR(DescOpt.value().c_str());
				LastChangedProp = MatFunc->GetClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UMaterialFunction, Description));
				ChangeCount++;
			}

			// user_exposed_caption
			sol::optional<std::string> CaptionOpt = Params.get<sol::optional<std::string>>("user_exposed_caption");
			if (CaptionOpt.has_value())
			{
				MatFunc->UserExposedCaption = UTF8_TO_TCHAR(CaptionOpt.value().c_str());
				LastChangedProp = MatFunc->GetClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UMaterialFunction, UserExposedCaption));
				ChangeCount++;
			}

			// expose_to_library
			sol::optional<bool> ExposeOpt = Params.get<sol::optional<bool>>("expose_to_library");
			if (ExposeOpt.has_value())
			{
				MatFunc->bExposeToLibrary = ExposeOpt.value();
				LastChangedProp = MatFunc->GetClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UMaterialFunction, bExposeToLibrary));
				ChangeCount++;
			}

			// enable_new_hlsl_generator
			sol::optional<bool> HlslOpt = Params.get<sol::optional<bool>>("enable_new_hlsl_generator");
			if (HlslOpt.has_value())
			{
				MatFunc->bEnableNewHLSLGenerator = HlslOpt.value();
				LastChangedProp = MatFunc->GetClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UMaterialFunction, bEnableNewHLSLGenerator));
				ChangeCount++;
			}

			// usage
			sol::optional<std::string> UsageOpt = Params.get<sol::optional<std::string>>("usage");
			if (UsageOpt.has_value())
			{
				EMaterialFunctionUsage NewUsage = StringToUsage(UTF8_TO_TCHAR(UsageOpt.value().c_str()));
				MatFunc->SetMaterialFunctionUsage(NewUsage);
				ChangeCount++;
			}

#if WITH_EDITORONLY_DATA
			// library_categories
			sol::optional<sol::table> CatsOpt = Params.get<sol::optional<sol::table>>("library_categories");
			if (CatsOpt.has_value())
			{
				MatFunc->LibraryCategoriesText.Empty();
				sol::table CatsTable = CatsOpt.value();
				for (auto& kv : CatsTable)
				{
					if (kv.second.is<std::string>())
					{
						FString CatStr = UTF8_TO_TCHAR(kv.second.as<std::string>().c_str());
						MatFunc->LibraryCategoriesText.Add(FText::FromString(CatStr));
					}
				}
				LastChangedProp = MatFunc->GetClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UMaterialFunction, LibraryCategoriesText));
				ChangeCount++;
			}
#endif

			// Post-edit with the last changed property for proper engine-side updates
#if WITH_EDITOR
			FPropertyChangedEvent PCE(LastChangedProp);
			MatFunc->PostEditChangeProperty(PCE);
#endif
			MatFunc->MarkPackageDirty();

			sol::table Result = Lua.create_table();
			Result["changes"] = ChangeCount;
			Session.Log(FString::Printf(TEXT("[OK] configure -> %d changes"), ChangeCount));
			return Result;
		});
	});
}

REGISTER_LUA_BINDING(MaterialFunction, MaterialFunctionDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindMaterialFunction(Lua, Session);
});
