// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"

#include "Chooser.h"
#include "IObjectChooser.h"
#include "IChooserColumn.h"
#include "Modules/ModuleManager.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
#include "ObjectChooser_Asset.h"
#include "OutputObjectColumn.h"
#include "OutputFloatColumn.h"
#include "OutputStructColumn.h"
#include "OutputBoolColumn.h"
#include "OutputEnumColumn.h"
#if ENGINE_MINOR_VERSION >= 7
#include "OutputGameplayTagQueryColumn.h"
#endif
#include "BoolColumn.h"
#include "EnumColumn.h"
#include "GameplayTagColumn.h"
#if ENGINE_MINOR_VERSION >= 7
#include "GameplayTagQueryColumn.h"
#endif
#include "FloatRangeColumn.h"
#include "FloatDistanceColumn.h"
#include "RandomizeColumn.h"
#include "MultiEnumColumn.h"
#include "ObjectColumn.h"
#include "ObjectClassColumn.h"
#endif // ENGINE_MINOR_VERSION >= 5

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
// ============================================================================
// HELPERS
// ============================================================================

static int32 GetChooserRowCount(const UChooserTable* CT)
{
#if WITH_EDITORONLY_DATA
	if (CT->ResultsStructs.Num() > 0)
	{
		return CT->ResultsStructs.Num();
	}
#endif
	// Fallback: use the max row count across output columns
	int32 MaxRows = 0;
	for (const FInstancedStruct& ColStruct : CT->ColumnsStructs)
	{
		if (!ColStruct.IsValid()) continue;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		if (const FOutputObjectColumn* ObjCol = ColStruct.GetPtr<FOutputObjectColumn>())
		{
			MaxRows = FMath::Max(MaxRows, ObjCol->RowValues.Num());
		}
		else
#endif // ENGINE_MINOR_VERSION >= 5
		if (const FOutputFloatColumn* FloatCol = ColStruct.GetPtr<FOutputFloatColumn>())
		{
			MaxRows = FMath::Max(MaxRows, FloatCol->RowValues.Num());
		}
		else if (const FOutputStructColumn* StructCol = ColStruct.GetPtr<FOutputStructColumn>())
		{
			MaxRows = FMath::Max(MaxRows, StructCol->RowValues.Num());
		}
		else if (const FOutputBoolColumn* BoolOutCol = ColStruct.GetPtr<FOutputBoolColumn>())
		{
			MaxRows = FMath::Max(MaxRows, BoolOutCol->RowValues.Num());
		}
		else if (const FOutputEnumColumn* EnumOutCol = ColStruct.GetPtr<FOutputEnumColumn>())
		{
			MaxRows = FMath::Max(MaxRows, EnumOutCol->RowValues.Num());
		}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		else if (const FOutputGameplayTagQueryColumn* TagQOutCol = ColStruct.GetPtr<FOutputGameplayTagQueryColumn>())
		{
			MaxRows = FMath::Max(MaxRows, TagQOutCol->RowValues.Num());
		}
#endif
	}
	return MaxRows;
}

static const char* GetResultTypeName(EObjectChooserResultType Type)
{
	switch (Type)
	{
	case EObjectChooserResultType::ObjectResult: return "Object";
	case EObjectChooserResultType::ClassResult: return "Class";
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	case EObjectChooserResultType::NoPrimaryResult: return "NoPrimaryResult";
#endif
	default: return "Unknown";
	}
}

static void AddChooserEditorSummary(sol::table& Result, const UChooserTable* CT)
{
#if WITH_EDITORONLY_DATA
	int32 DisabledCount = 0;
	for (int32 i = 0; i < CT->DisabledRows.Num(); ++i)
	{
		if (CT->DisabledRows[i])
		{
			DisabledCount++;
		}
	}
	Result["disabled_rows"] = DisabledCount;
	Result["nested_choosers"] = static_cast<int>(CT->NestedChoosers.Num());
#else
	(void)CT;
	Result["disabled_rows"] = 0;
	Result["nested_choosers"] = 0;
#endif
}

static void AddChooserColumnEditorFields(sol::table& Entry, const FChooserColumnBase* ColumnBase)
{
#if WITH_EDITORONLY_DATA
	Entry["is_disabled"] = ColumnBase->bDisabled;
#else
	(void)Entry;
	(void)ColumnBase;
#endif
}

static void AddChooserEnumValueName(sol::table& Entry, const FChooserEnumRowData& EnumRow)
{
#if WITH_EDITORONLY_DATA
	if (!EnumRow.ValueName.IsNone())
	{
		Entry["value_name"] = TCHAR_TO_UTF8(*EnumRow.ValueName.ToString());
	}
#else
	(void)Entry;
	(void)EnumRow;
#endif
}

static void AddChooserRowResultInfo(sol::table& RowTable, const UChooserTable* CT, int32 RowIdx)
{
#if WITH_EDITORONLY_DATA
	if (RowIdx < CT->ResultsStructs.Num())
	{
		const FInstancedStruct& ResultStruct = CT->ResultsStructs[RowIdx];
		if (ResultStruct.IsValid())
		{
			FString ResultType = ResultStruct.GetScriptStruct()
				? ResultStruct.GetScriptStruct()->GetName()
				: TEXT("Unknown");
			RowTable["result_type"] = TCHAR_TO_UTF8(*ResultType);

			if (const FAssetChooser* AssetResult = ResultStruct.GetPtr<FAssetChooser>())
			{
				if (UObject* Asset = AssetResult->Asset.Get())
				{
					RowTable["result_asset"] = TCHAR_TO_UTF8(*Asset->GetName());
					RowTable["result_asset_path"] = TCHAR_TO_UTF8(*Asset->GetPathName());
				}
			}
			else if (const FSoftAssetChooser* SoftResult = ResultStruct.GetPtr<FSoftAssetChooser>())
			{
				FSoftObjectPath SoftPath = SoftResult->Asset.ToSoftObjectPath();
				if (SoftPath.IsValid())
				{
					RowTable["result_asset"] = TCHAR_TO_UTF8(*FPackageName::GetShortName(SoftPath.ToString()));
					RowTable["result_asset_path"] = TCHAR_TO_UTF8(*SoftPath.ToString());
					RowTable["result_is_soft"] = true;
				}
			}
			else if (const FNestedChooser* Nested = ResultStruct.GetPtr<FNestedChooser>())
			{
				if (Nested->Chooser)
				{
					RowTable["result_nested_chooser"] = TCHAR_TO_UTF8(*Nested->Chooser->GetName());
				}
			}
			else if (const FEvaluateChooser* EvalChooser = ResultStruct.GetPtr<FEvaluateChooser>())
			{
				if (EvalChooser->Chooser)
				{
					RowTable["result_evaluate_chooser"] = TCHAR_TO_UTF8(*EvalChooser->Chooser->GetName());
				}
			}
		}
	}
#else
	(void)RowTable;
	(void)CT;
	(void)RowIdx;
#endif
}

// Map a column type name (case-insensitive) to a UScriptStruct for column creation
static const UScriptStruct* ResolveChooserColumnStruct(const FString& TypeName)
{
	// Filter columns
	if (TypeName.Equals(TEXT("Bool"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("BoolColumn"), ESearchCase::IgnoreCase))
		return FBoolColumn::StaticStruct();
	if (TypeName.Equals(TEXT("Enum"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("EnumColumn"), ESearchCase::IgnoreCase))
		return FEnumColumn::StaticStruct();
	if (TypeName.Equals(TEXT("GameplayTag"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("GameplayTagColumn"), ESearchCase::IgnoreCase))
		return FGameplayTagColumn::StaticStruct();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	if (TypeName.Equals(TEXT("GameplayTagQuery"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("GameplayTagQueryColumn"), ESearchCase::IgnoreCase))
		return FGameplayTagQueryColumn::StaticStruct();
#endif
	if (TypeName.Equals(TEXT("FloatRange"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("FloatRangeColumn"), ESearchCase::IgnoreCase))
		return FFloatRangeColumn::StaticStruct();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	if (TypeName.Equals(TEXT("FloatDistance"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("FloatDistanceColumn"), ESearchCase::IgnoreCase))
		return FFloatDistanceColumn::StaticStruct();
#endif
	if (TypeName.Equals(TEXT("Randomize"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("RandomizeColumn"), ESearchCase::IgnoreCase))
		return FRandomizeColumn::StaticStruct();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	if (TypeName.Equals(TEXT("MultiEnum"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("MultiEnumColumn"), ESearchCase::IgnoreCase))
		return FMultiEnumColumn::StaticStruct();
#endif
	if (TypeName.Equals(TEXT("Object"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("ObjectColumn"), ESearchCase::IgnoreCase))
		return FObjectColumn::StaticStruct();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	if (TypeName.Equals(TEXT("ObjectClass"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("ObjectClassColumn"), ESearchCase::IgnoreCase))
		return FObjectClassColumn::StaticStruct();
#endif

	// Output columns
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	if (TypeName.Equals(TEXT("OutputObject"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("OutputObjectColumn"), ESearchCase::IgnoreCase))
		return FOutputObjectColumn::StaticStruct();
#endif // ENGINE_MINOR_VERSION >= 5
	if (TypeName.Equals(TEXT("OutputFloat"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("OutputFloatColumn"), ESearchCase::IgnoreCase))
		return FOutputFloatColumn::StaticStruct();
	if (TypeName.Equals(TEXT("OutputStruct"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("OutputStructColumn"), ESearchCase::IgnoreCase))
		return FOutputStructColumn::StaticStruct();
	if (TypeName.Equals(TEXT("OutputBool"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("OutputBoolColumn"), ESearchCase::IgnoreCase))
		return FOutputBoolColumn::StaticStruct();
	if (TypeName.Equals(TEXT("OutputEnum"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("OutputEnumColumn"), ESearchCase::IgnoreCase))
		return FOutputEnumColumn::StaticStruct();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	if (TypeName.Equals(TEXT("OutputGameplayTagQuery"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("OutputGameplayTagQueryColumn"), ESearchCase::IgnoreCase))
		return FOutputGameplayTagQueryColumn::StaticStruct();
#endif

	return nullptr;
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> ChooserTableDocs = {};

static void BindChooserTable(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_chooser_table", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UChooserTable* CT = LoadObject<UChooserTable>(nullptr, *FPath);
		if (!CT) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"ChooserTable enrichment (read + write).\n"
			"\n"
			"info() — summary: column/row counts, result type, fallback, disabled rows, nested choosers\n"
			"\n"
			"list(type):\n"
			"  list(\"columns\")   — all columns with index, struct type, is_output, is_disabled\n"
			"  list(\"rows\")      — all rows with index, disabled flag, filter values (Bool/Enum/Tag/FloatRange/Randomize/FloatDistance/MultiEnum/GameplayTagQuery), outputs (Object/Float/Struct/Bool/Enum/GameplayTagQuery)\n"
			"  list(\"references\")— flat list of all asset references across all output columns\n"
			"  list(\"context\")   — context parameter definitions\n"
			"\n"
			"Column management (editor-only):\n"
			"  add_column(type, opts?) — add column. Types: Bool, Enum, GameplayTag, GameplayTagQuery, FloatRange, FloatDistance, Randomize, MultiEnum, Object, ObjectClass, OutputObject, OutputFloat, OutputStruct, OutputBool, OutputEnum, OutputGameplayTagQuery. opts.index = 1-based insertion position\n"
			"  remove_column(index)    — remove column by 1-based index\n"
			"\n"
			"Cell value setting (editor-only, 1-based row/column indices):\n"
			"  set_cell(row, column, opts) — set filter cell value:\n"
			"    Bool:             opts.value = \"True\"/\"False\"/\"Any\"\n"
			"    Enum:             opts.value = int, opts.comparison = \"Equal\"/\"NotEqual\"/\"Any\"\n"
			"    GameplayTag:      opts.tags = \"Tag.One,Tag.Two\"\n"
			"    FloatRange:       opts.min, opts.max, opts.no_min, opts.no_max\n"
			"    Randomize:        opts.weight = float\n"
			"    FloatDistance:    opts.value = float\n"
			"    MultiEnum:        opts.value = int (bitmask)\n"
			"    GameplayTagQuery: opts.match_type = \"any\"/\"all\"/\"none\", opts.tags = \"Tag.One,Tag.Two\"\n"
			"    Object:           opts.asset_path = \"/Game/...\", opts.comparison = \"Equal\"/\"NotEqual\"/\"Any\"\n"
			"    ObjectClass:      opts.class_path = \"/Script/...\", opts.comparison = \"Equal\"/\"NotEqual\"/\"SubClassOf\"/\"NotSubClassOf\"/\"Any\"\n"
			"  set_output_cell(row, column, opts) — set output cell value:\n"
			"    OutputObject:     opts.asset_path = \"/Game/...\"\n"
			"    OutputFloat:      opts.value = float\n"
			"    OutputBool:       opts.value = bool\n"
			"    OutputEnum:       opts.value = int\n"
			"    OutputGameplayTagQuery: opts.match_type = \"any\"/\"all\"/\"none\", opts.tags = \"Tag.One,Tag.Two\"\n"
			"\n"
			"Row operations (editor-only):\n"
			"  add_rows(count?, index?)   — insert rows (default 1 at end)\n"
			"  delete_rows(indices)       — delete rows by 1-based index array, e.g. {1, 3, 5}\n"
			"  move_row(from, to)         — move row from 1-based index to another\n"
			"  set_row_disabled(index, disabled) — enable/disable a row\n"
			"  set_row_result(index, asset_path) — set the result asset for a row\n"
			"  set_fallback_result(asset_path)   — set the fallback result asset\n"
			"  compile()                  — recompile the chooser after edits\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [CT, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();

			int32 ColumnCount = CT->ColumnsStructs.Num();
			int32 OutputColumnCount = 0;
			int32 FilterColumnCount = 0;

			for (const FInstancedStruct& ColStruct : CT->ColumnsStructs)
			{
				if (!ColStruct.IsValid()) continue;
				if (const FChooserColumnBase* ColBase = ColStruct.GetPtr<FChooserColumnBase>())
				{
					if (ColBase->HasOutputs())
						OutputColumnCount++;
					else
						FilterColumnCount++;
				}
			}

			int32 RowCount = GetChooserRowCount(CT);

			Result["column_count"] = ColumnCount;
			Result["filter_columns"] = FilterColumnCount;
			Result["output_columns"] = OutputColumnCount;
			Result["row_count"] = RowCount;
			Result["has_fallback"] = CT->FallbackResult.IsValid();
			Result["result_type"] = GetResultTypeName(CT->ResultType);

			if (CT->OutputObjectType)
			{
				Result["output_class"] = TCHAR_TO_UTF8(*CT->OutputObjectType->GetName());
			}

			AddChooserEditorSummary(Result, CT);

			// Context data
			TConstArrayView<FInstancedStruct> CtxData = CT->GetContextData();
			Result["context_count"] = static_cast<int>(CtxData.Num());

			Session.Log(FString::Printf(TEXT("[OK] info() -> %d columns (%d filter, %d output), %d rows"),
				ColumnCount, FilterColumnCount, OutputColumnCount, RowCount));
			return Result;
		});

		// ================================================================
		// list(type)
		// ================================================================
		AssetObj.set_function("list", [CT, &Session](sol::table Self,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			// ---- list() / list("all") -> info() ----
			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = Self["info"];
				if (InfoFn.valid()) return InfoFn(Self);
				return sol::lua_nil;
			}

			// ---- list("columns") ----
			if (FType.Equals(TEXT("columns"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("column"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				int32 ColIdx = 0;

				for (const FInstancedStruct& ColStruct : CT->ColumnsStructs)
				{
					sol::table E = Lua.create_table();
					E["index"] = ColIdx + 1; // 1-based

					if (!ColStruct.IsValid())
					{
						E["type"] = "(invalid)";
						E["is_output"] = false;
						Result[ColIdx + 1] = E;
						ColIdx++;
						continue;
					}

					FString TypeName = ColStruct.GetScriptStruct()
						? ColStruct.GetScriptStruct()->GetName()
						: TEXT("Unknown");
					E["type"] = TCHAR_TO_UTF8(*TypeName);

					if (const FChooserColumnBase* ColBase = ColStruct.GetPtr<FChooserColumnBase>())
					{
						E["is_output"] = ColBase->HasOutputs();
						E["has_filters"] = ColBase->HasFilters();
						E["has_costs"] = ColBase->HasCosts();
						AddChooserColumnEditorFields(E, ColBase);

						// Row value counts for output columns
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
						if (const FOutputObjectColumn* ObjCol = ColStruct.GetPtr<FOutputObjectColumn>())
						{
							E["row_values_count"] = static_cast<int>(ObjCol->RowValues.Num());
						}
						else
#endif // ENGINE_MINOR_VERSION >= 5
						if (const FOutputFloatColumn* FloatCol = ColStruct.GetPtr<FOutputFloatColumn>())
						{
							E["row_values_count"] = static_cast<int>(FloatCol->RowValues.Num());
						}
						else if (const FOutputStructColumn* StructCol = ColStruct.GetPtr<FOutputStructColumn>())
						{
							E["row_values_count"] = static_cast<int>(StructCol->RowValues.Num());
						}
						else if (const FOutputBoolColumn* BoolOutCol = ColStruct.GetPtr<FOutputBoolColumn>())
						{
							E["row_values_count"] = static_cast<int>(BoolOutCol->RowValues.Num());
						}
						else if (const FOutputEnumColumn* EnumOutCol = ColStruct.GetPtr<FOutputEnumColumn>())
						{
							E["row_values_count"] = static_cast<int>(EnumOutCol->RowValues.Num());
						}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
						else if (const FOutputGameplayTagQueryColumn* TagQOutCol = ColStruct.GetPtr<FOutputGameplayTagQueryColumn>())
						{
							E["row_values_count"] = static_cast<int>(TagQOutCol->RowValues.Num());
						}
#endif
						// Row value counts for filter/scoring columns
						else if (const FRandomizeColumn* RandCol = ColStruct.GetPtr<FRandomizeColumn>())
						{
							E["row_values_count"] = static_cast<int>(RandCol->RowValues.Num());
							E["is_randomize"] = true;
						}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
						else if (const FFloatDistanceColumn* FDistCol = ColStruct.GetPtr<FFloatDistanceColumn>())
						{
							E["row_values_count"] = static_cast<int>(FDistCol->RowValues.Num());
						}
#endif
					}

					Result[ColIdx + 1] = E;
					ColIdx++;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"columns\") -> %d"), ColIdx));
				return Result;
			}

			// ---- list("rows") ----
			if (FType.Equals(TEXT("rows"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("row"), ESearchCase::IgnoreCase))
			{
				int32 RowCount = GetChooserRowCount(CT);
				sol::table Result = Lua.create_table();

				for (int32 RowIdx = 0; RowIdx < RowCount; ++RowIdx)
				{
					sol::table RowT = Lua.create_table();
					RowT["index"] = RowIdx + 1; // 1-based
					RowT["disabled"] = CT->IsRowDisabled(RowIdx);

					// Collect filter values from filter columns for this row
					sol::table Filters = Lua.create_table();
					int32 FiltIdx = 1;

					for (int32 ColI = 0; ColI < CT->ColumnsStructs.Num(); ++ColI)
					{
						const FInstancedStruct& ColStruct = CT->ColumnsStructs[ColI];
						if (!ColStruct.IsValid()) continue;

						// Bool filter column
						if (const FBoolColumn* BoolCol = ColStruct.GetPtr<FBoolColumn>())
						{
							if (RowIdx < BoolCol->RowValuesWithAny.Num())
							{
								sol::table FE = Lua.create_table();
								FE["column_index"] = ColI + 1;
								FE["column_type"] = "Bool";
								EBoolColumnCellValue CellVal = BoolCol->RowValuesWithAny[RowIdx];
								switch (CellVal)
								{
								case EBoolColumnCellValue::MatchFalse: FE["value"] = "False"; break;
								case EBoolColumnCellValue::MatchTrue:  FE["value"] = "True"; break;
								case EBoolColumnCellValue::MatchAny:   FE["value"] = "Any"; break;
								}
								Filters[FiltIdx++] = FE;
							}
						}
						// Enum filter column
						else if (const FEnumColumn* EnumCol = ColStruct.GetPtr<FEnumColumn>())
						{
							if (RowIdx < EnumCol->RowValues.Num())
							{
								const FChooserEnumRowData& EnumRow = EnumCol->RowValues[RowIdx];
								sol::table FE = Lua.create_table();
								FE["column_index"] = ColI + 1;
								FE["column_type"] = "Enum";
								FE["value"] = static_cast<int>(EnumRow.Value);
								switch (EnumRow.Comparison)
								{
								case EEnumColumnCellValueComparison::MatchEqual:    FE["comparison"] = "Equal"; break;
								case EEnumColumnCellValueComparison::MatchNotEqual: FE["comparison"] = "NotEqual"; break;
								case EEnumColumnCellValueComparison::MatchAny:      FE["comparison"] = "Any"; break;
								default: FE["comparison"] = "Unknown"; break;
								}
								AddChooserEnumValueName(FE, EnumRow);
								Filters[FiltIdx++] = FE;
							}
						}
						// GameplayTag filter column
						else if (const FGameplayTagColumn* TagCol = ColStruct.GetPtr<FGameplayTagColumn>())
						{
							if (RowIdx < TagCol->RowValues.Num())
							{
								const FGameplayTagContainer& Tags = TagCol->RowValues[RowIdx];
								sol::table FE = Lua.create_table();
								FE["column_index"] = ColI + 1;
								FE["column_type"] = "GameplayTag";
								FE["tags"] = TCHAR_TO_UTF8(*Tags.ToStringSimple());
								Filters[FiltIdx++] = FE;
							}
						}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
						// GameplayTagQuery filter column
						else if (const FGameplayTagQueryColumn* TagQCol = ColStruct.GetPtr<FGameplayTagQueryColumn>())
						{
							if (RowIdx < TagQCol->RowValues.Num())
							{
								const FGameplayTagQuery& Query = TagQCol->RowValues[RowIdx];
								sol::table FE = Lua.create_table();
								FE["column_index"] = ColI + 1;
								FE["column_type"] = "GameplayTagQuery";
								FE["description"] = TCHAR_TO_UTF8(*Query.GetDescription());
								// List the tags referenced by the query
								TArray<FGameplayTag> QueryTags;
								Query.GetGameplayTagArray(QueryTags);
								FString TagStr;
								for (int32 Ti = 0; Ti < QueryTags.Num(); ++Ti)
								{
									if (Ti > 0) TagStr += TEXT(",");
									TagStr += QueryTags[Ti].ToString();
								}
								FE["tags"] = TCHAR_TO_UTF8(*TagStr);
								Filters[FiltIdx++] = FE;
							}
						}
#endif
						// Float range filter column
						else if (const FFloatRangeColumn* FloatRangeCol = ColStruct.GetPtr<FFloatRangeColumn>())
						{
							if (RowIdx < FloatRangeCol->RowValues.Num())
							{
								const FChooserFloatRangeRowData& RangeRow = FloatRangeCol->RowValues[RowIdx];
								sol::table FE = Lua.create_table();
								FE["column_index"] = ColI + 1;
								FE["column_type"] = "FloatRange";
								if (!RangeRow.bNoMin) FE["min"] = RangeRow.Min;
								if (!RangeRow.bNoMax) FE["max"] = RangeRow.Max;
								FE["no_min"] = RangeRow.bNoMin;
								FE["no_max"] = RangeRow.bNoMax;
								Filters[FiltIdx++] = FE;
							}
						}
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
						// FloatDistance scoring column
						else if (const FFloatDistanceColumn* FDistCol = ColStruct.GetPtr<FFloatDistanceColumn>())
						{
							if (RowIdx < FDistCol->RowValues.Num())
							{
								const FChooserFloatDistanceRowData& DistRow = FDistCol->RowValues[RowIdx];
								sol::table FE = Lua.create_table();
								FE["column_index"] = ColI + 1;
								FE["column_type"] = "FloatDistance";
								FE["value"] = DistRow.Value;
								Filters[FiltIdx++] = FE;
							}
						}
#endif
						// Randomize column
						else if (const FRandomizeColumn* RandCol = ColStruct.GetPtr<FRandomizeColumn>())
						{
							if (RowIdx < RandCol->RowValues.Num())
							{
								sol::table FE = Lua.create_table();
								FE["column_index"] = ColI + 1;
								FE["column_type"] = "Randomize";
								FE["weight"] = RandCol->RowValues[RowIdx];
								Filters[FiltIdx++] = FE;
							}
						}
						// MultiEnum filter column (5.5+)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
						else if (const FMultiEnumColumn* MEnumCol = ColStruct.GetPtr<FMultiEnumColumn>())
						{
							if (RowIdx < MEnumCol->RowValues.Num())
							{
								const FChooserMultiEnumRowData& MEnumRow = MEnumCol->RowValues[RowIdx];
								sol::table FE = Lua.create_table();
								FE["column_index"] = ColI + 1;
								FE["column_type"] = "MultiEnum";
								FE["value"] = static_cast<int>(MEnumRow.Value);
								Filters[FiltIdx++] = FE;
							}
						}
#endif
						// Object filter column
						else if (const FObjectColumn* ObjFilterCol = ColStruct.GetPtr<FObjectColumn>())
						{
							if (RowIdx < ObjFilterCol->RowValues.Num())
							{
								const FChooserObjectRowData& ObjRow = ObjFilterCol->RowValues[RowIdx];
								sol::table FE = Lua.create_table();
								FE["column_index"] = ColI + 1;
								FE["column_type"] = "Object";
								switch (ObjRow.Comparison)
								{
								case EObjectColumnCellValueComparison::MatchEqual:    FE["comparison"] = "Equal"; break;
								case EObjectColumnCellValueComparison::MatchNotEqual: FE["comparison"] = "NotEqual"; break;
								case EObjectColumnCellValueComparison::MatchAny:      FE["comparison"] = "Any"; break;
								default: FE["comparison"] = "Unknown"; break;
								}
								if (ObjRow.Value.IsValid())
								{
									FE["value"] = TCHAR_TO_UTF8(*ObjRow.Value.ToSoftObjectPath().ToString());
								}
								Filters[FiltIdx++] = FE;
							}
						}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
						// Object class filter column
						else if (const FObjectClassColumn* ObjClassCol = ColStruct.GetPtr<FObjectClassColumn>())
						{
							if (RowIdx < ObjClassCol->RowValues.Num())
							{
								const FChooserObjectClassRowData& ClassRow = ObjClassCol->RowValues[RowIdx];
								sol::table FE = Lua.create_table();
								FE["column_index"] = ColI + 1;
								FE["column_type"] = "ObjectClass";
								switch (ClassRow.Comparison)
								{
								case EObjectClassColumnCellValueComparison::Equal:         FE["comparison"] = "Equal"; break;
								case EObjectClassColumnCellValueComparison::NotEqual:      FE["comparison"] = "NotEqual"; break;
								case EObjectClassColumnCellValueComparison::SubClassOf:    FE["comparison"] = "SubClassOf"; break;
								case EObjectClassColumnCellValueComparison::NotSubClassOf: FE["comparison"] = "NotSubClassOf"; break;
								case EObjectClassColumnCellValueComparison::Any:           FE["comparison"] = "Any"; break;
								}
								if (ClassRow.Value)
								{
									FE["value"] = TCHAR_TO_UTF8(*ClassRow.Value->GetPathName());
								}
								Filters[FiltIdx++] = FE;
							}
						}
#endif
					}

					RowT["filters"] = Filters;

					// Collect outputs from all output columns for this row
					sol::table Outputs = Lua.create_table();
					int32 OutIdx = 1;

					for (int32 ColI = 0; ColI < CT->ColumnsStructs.Num(); ++ColI)
					{
						const FInstancedStruct& ColStruct = CT->ColumnsStructs[ColI];
						if (!ColStruct.IsValid()) continue;

						// Output Object column — extract asset references
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
						if (const FOutputObjectColumn* ObjCol = ColStruct.GetPtr<FOutputObjectColumn>())
						{
							if (RowIdx < ObjCol->RowValues.Num())
							{
								const FChooserOutputObjectRowData& RowData = ObjCol->RowValues[RowIdx];
								if (RowData.Value.IsValid())
								{
									sol::table OutEntry = Lua.create_table();
									OutEntry["column_index"] = ColI + 1;
									FString ValType = RowData.Value.GetScriptStruct()
										? RowData.Value.GetScriptStruct()->GetName()
										: TEXT("Unknown");
									OutEntry["chooser_type"] = TCHAR_TO_UTF8(*ValType);

									// Try FAssetChooser (hard reference)
									if (const FAssetChooser* AssetChooser = RowData.Value.GetPtr<FAssetChooser>())
									{
										if (UObject* Asset = AssetChooser->Asset.Get())
										{
											OutEntry["asset_name"] = TCHAR_TO_UTF8(*Asset->GetName());
											OutEntry["asset_path"] = TCHAR_TO_UTF8(*Asset->GetPathName());
											OutEntry["asset_class"] = TCHAR_TO_UTF8(*Asset->GetClass()->GetName());
										}
									}
									// Try FSoftAssetChooser (soft reference)
									else if (const FSoftAssetChooser* SoftChooser = RowData.Value.GetPtr<FSoftAssetChooser>())
									{
										FSoftObjectPath SoftPath = SoftChooser->Asset.ToSoftObjectPath();
										if (SoftPath.IsValid())
										{
											OutEntry["asset_name"] = TCHAR_TO_UTF8(*FPackageName::GetShortName(SoftPath.ToString()));
											OutEntry["asset_path"] = TCHAR_TO_UTF8(*SoftPath.ToString());
											OutEntry["is_soft"] = true;
										}
									}
									// Try FNestedChooser
									else if (const FNestedChooser* Nested = RowData.Value.GetPtr<FNestedChooser>())
									{
										if (Nested->Chooser)
										{
											OutEntry["nested_chooser"] = TCHAR_TO_UTF8(*Nested->Chooser->GetName());
											OutEntry["nested_chooser_path"] = TCHAR_TO_UTF8(*Nested->Chooser->GetPathName());
										}
									}
									// Try FEvaluateChooser
									else if (const FEvaluateChooser* EvalChooser = RowData.Value.GetPtr<FEvaluateChooser>())
									{
										if (EvalChooser->Chooser)
										{
											OutEntry["evaluate_chooser"] = TCHAR_TO_UTF8(*EvalChooser->Chooser->GetName());
											OutEntry["evaluate_chooser_path"] = TCHAR_TO_UTF8(*EvalChooser->Chooser->GetPathName());
										}
									}

									Outputs[OutIdx++] = OutEntry;
								}
							}
						}
						// Output Float column
						else
#endif // ENGINE_MINOR_VERSION >= 5
						if (const FOutputFloatColumn* FloatCol = ColStruct.GetPtr<FOutputFloatColumn>())
						{
							if (RowIdx < FloatCol->RowValues.Num())
							{
								sol::table OutEntry = Lua.create_table();
								OutEntry["column_index"] = ColI + 1;
								OutEntry["type"] = "float";
								OutEntry["value"] = FloatCol->RowValues[RowIdx];
								Outputs[OutIdx++] = OutEntry;
							}
						}
						// Output Bool column
						else if (const FOutputBoolColumn* BoolOutCol = ColStruct.GetPtr<FOutputBoolColumn>())
						{
							if (RowIdx < BoolOutCol->RowValues.Num())
							{
								sol::table OutEntry = Lua.create_table();
								OutEntry["column_index"] = ColI + 1;
								OutEntry["type"] = "bool";
								OutEntry["value"] = BoolOutCol->RowValues[RowIdx];
								Outputs[OutIdx++] = OutEntry;
							}
						}
						// Output Enum column
						else if (const FOutputEnumColumn* EnumOutCol = ColStruct.GetPtr<FOutputEnumColumn>())
						{
							if (RowIdx < EnumOutCol->RowValues.Num())
							{
								const FChooserOutputEnumRowData& EnumRow = EnumOutCol->RowValues[RowIdx];
								sol::table OutEntry = Lua.create_table();
								OutEntry["column_index"] = ColI + 1;
								OutEntry["type"] = "enum";
								OutEntry["value"] = static_cast<int>(EnumRow.Value);
#if WITH_EDITORONLY_DATA
								if (!EnumRow.ValueName.IsNone())
								{
									OutEntry["value_name"] = TCHAR_TO_UTF8(*EnumRow.ValueName.ToString());
								}
#endif
								Outputs[OutIdx++] = OutEntry;
							}
						}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
						// Output GameplayTagQuery column
						else if (const FOutputGameplayTagQueryColumn* TagQOutCol = ColStruct.GetPtr<FOutputGameplayTagQueryColumn>())
						{
							if (RowIdx < TagQOutCol->RowValues.Num())
							{
								const FGameplayTagQuery& Query = TagQOutCol->RowValues[RowIdx];
								sol::table OutEntry = Lua.create_table();
								OutEntry["column_index"] = ColI + 1;
								OutEntry["type"] = "gameplay_tag_query";
								OutEntry["description"] = TCHAR_TO_UTF8(*Query.GetDescription());
								TArray<FGameplayTag> QueryTags;
								Query.GetGameplayTagArray(QueryTags);
								FString TagStr;
								for (int32 Ti = 0; Ti < QueryTags.Num(); ++Ti)
								{
									if (Ti > 0) TagStr += TEXT(",");
									TagStr += QueryTags[Ti].ToString();
								}
								OutEntry["tags"] = TCHAR_TO_UTF8(*TagStr);
								Outputs[OutIdx++] = OutEntry;
							}
						}
#endif
						// Output Struct column
						else if (const FOutputStructColumn* StructCol = ColStruct.GetPtr<FOutputStructColumn>())
						{
							if (RowIdx < StructCol->RowValues.Num())
							{
								const FInstancedStruct& StructVal = StructCol->RowValues[RowIdx];
								sol::table OutEntry = Lua.create_table();
								OutEntry["column_index"] = ColI + 1;
								OutEntry["type"] = "struct";
								if (StructVal.IsValid() && StructVal.GetScriptStruct())
								{
									OutEntry["struct_type"] = TCHAR_TO_UTF8(*StructVal.GetScriptStruct()->GetName());
								}
								Outputs[OutIdx++] = OutEntry;
							}
						}
					}

					RowT["outputs"] = Outputs;

					// Also include the result struct info if available
					AddChooserRowResultInfo(RowT, CT, RowIdx);

					Result[RowIdx + 1] = RowT;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"rows\") -> %d"), RowCount));
				return Result;
			}

			// ---- list("references") — flat list of all asset references ----
			if (FType.Equals(TEXT("references"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("reference"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				int32 RefIdx = 1;

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				for (const FInstancedStruct& ColStruct : CT->ColumnsStructs)
				{
					if (!ColStruct.IsValid()) continue;
					const FOutputObjectColumn* ObjCol = ColStruct.GetPtr<FOutputObjectColumn>();
					if (!ObjCol) continue;

					for (int32 RowIdx = 0; RowIdx < ObjCol->RowValues.Num(); ++RowIdx)
					{
						const FChooserOutputObjectRowData& RowData = ObjCol->RowValues[RowIdx];
						if (!RowData.Value.IsValid()) continue;

						if (const FAssetChooser* AssetChooser = RowData.Value.GetPtr<FAssetChooser>())
						{
							if (UObject* Asset = AssetChooser->Asset.Get())
							{
								sol::table Ref = Lua.create_table();
								Ref["row"] = RowIdx + 1;
								Ref["asset_name"] = TCHAR_TO_UTF8(*Asset->GetName());
								Ref["asset_path"] = TCHAR_TO_UTF8(*Asset->GetPathName());
								Ref["asset_class"] = TCHAR_TO_UTF8(*Asset->GetClass()->GetName());
								Ref["is_soft"] = false;
								Result[RefIdx++] = Ref;
							}
						}
						else if (const FSoftAssetChooser* SoftChooser = RowData.Value.GetPtr<FSoftAssetChooser>())
						{
							FSoftObjectPath SoftPath = SoftChooser->Asset.ToSoftObjectPath();
							if (SoftPath.IsValid())
							{
								sol::table Ref = Lua.create_table();
								Ref["row"] = RowIdx + 1;
								Ref["asset_name"] = TCHAR_TO_UTF8(*FPackageName::GetShortName(SoftPath.ToString()));
								Ref["asset_path"] = TCHAR_TO_UTF8(*SoftPath.ToString());
								Ref["is_soft"] = true;
								Result[RefIdx++] = Ref;
							}
						}
					}

					// Also include fallback reference
					if (ObjCol->FallbackValue.Value.IsValid())
					{
						if (const FAssetChooser* AssetChooser = ObjCol->FallbackValue.Value.GetPtr<FAssetChooser>())
						{
							if (UObject* Asset = AssetChooser->Asset.Get())
							{
								sol::table Ref = Lua.create_table();
								Ref["row"] = "fallback";
								Ref["asset_name"] = TCHAR_TO_UTF8(*Asset->GetName());
								Ref["asset_path"] = TCHAR_TO_UTF8(*Asset->GetPathName());
								Ref["asset_class"] = TCHAR_TO_UTF8(*Asset->GetClass()->GetName());
								Ref["is_fallback"] = true;
								Result[RefIdx++] = Ref;
							}
						}
					}
				}
#endif // ENGINE_MINOR_VERSION >= 5

				Session.Log(FString::Printf(TEXT("[OK] list(\"references\") -> %d"), RefIdx - 1));
				return Result;
			}

			// ---- list("context") — context parameter definitions ----
			if (FType.Equals(TEXT("context"), ESearchCase::IgnoreCase))
			{
				TConstArrayView<FInstancedStruct> CtxData = CT->GetContextData();
				sol::table Result = Lua.create_table();

				for (int32 i = 0; i < CtxData.Num(); ++i)
				{
					sol::table E = Lua.create_table();
					E["index"] = i + 1;

					if (CtxData[i].IsValid() && CtxData[i].GetScriptStruct())
					{
						E["type"] = TCHAR_TO_UTF8(*CtxData[i].GetScriptStruct()->GetName());
					}
					else
					{
						E["type"] = "(invalid)";
					}

					Result[i + 1] = E;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"context\") -> %d"), CtxData.Num()));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: columns, rows, references, context"), *FType));
			return sol::lua_nil;
		});

#if WITH_EDITOR
		// ================================================================
		// add_column(type, opts?) — add a column by type name
		// ================================================================
		AssetObj.set_function("add_column", [CT, &Session](sol::table /*self*/,
			const std::string& TypeStr, sol::optional<sol::table> OptsOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FTypeName = UTF8_TO_TCHAR(TypeStr.c_str());

			const UScriptStruct* ColStruct = ResolveChooserColumnStruct(FTypeName);
			if (!ColStruct)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_column(\"%s\") -> unknown column type. Valid: Bool, Enum, GameplayTag, GameplayTagQuery, FloatRange, FloatDistance, Randomize, MultiEnum, Object, ObjectClass, OutputObject, OutputFloat, OutputStruct, OutputBool, OutputEnum, OutputGameplayTagQuery"), *FTypeName));
				return sol::lua_nil;
			}

			int32 CurrentRows = GetChooserRowCount(CT);
			int32 InsertAt = CT->ColumnsStructs.Num(); // default: append

			if (OptsOpt.has_value())
			{
				sol::table Opts = OptsOpt.value();
				int OptIdx = Opts.get_or("index", 0);
				if (OptIdx > 0)
				{
					InsertAt = FMath::Clamp(OptIdx - 1, 0, CT->ColumnsStructs.Num());
				}
			}

			CT->Modify();

			// Create the new column via FInstancedStruct
			FInstancedStruct NewCol;
			NewCol.InitializeAs(ColStruct);

			// Initialize the column for this chooser and set row count to match existing rows
			if (FChooserColumnBase* ColBase = NewCol.GetMutablePtr<FChooserColumnBase>())
			{
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				ColBase->Initialize(CT);
#endif
				if (CurrentRows > 0)
				{
					ColBase->SetNumRows(CurrentRows);
				}
			}

			CT->ColumnsStructs.Insert(MoveTemp(NewCol), InsertAt);

			CT->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] add_column(\"%s\") at index %d -> now %d columns"),
				*FTypeName, InsertAt + 1, CT->ColumnsStructs.Num()));
			return sol::make_object(Lua, InsertAt + 1); // return 1-based index
		});

		// ================================================================
		// remove_column(index) — remove a column by 1-based index
		// ================================================================
		AssetObj.set_function("remove_column", [CT, &Session](sol::table /*self*/,
			int Index, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			int32 Idx = Index - 1;

			if (Idx < 0 || Idx >= CT->ColumnsStructs.Num())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_column(%d) -> index out of range (columns: %d)"),
					Index, CT->ColumnsStructs.Num()));
				return sol::lua_nil;
			}

			CT->Modify();
			CT->ColumnsStructs.RemoveAt(Idx);
			CT->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] remove_column(%d) -> now %d columns"),
				Index, CT->ColumnsStructs.Num()));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// set_cell(row, column, opts) — set a filter cell value
		// ================================================================
		AssetObj.set_function("set_cell", [CT, &Session](sol::table /*self*/,
			int RowIndex, int ColIndex, sol::table Opts, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			int32 RowIdx = RowIndex - 1;
			int32 ColIdx = ColIndex - 1;

			if (RowIdx < 0)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_cell -> row index %d must be >= 1"), RowIndex));
				return sol::lua_nil;
			}

			if (ColIdx < 0 || ColIdx >= CT->ColumnsStructs.Num())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_cell -> column %d out of range (columns: %d)"),
					ColIndex, CT->ColumnsStructs.Num()));
				return sol::lua_nil;
			}

			FInstancedStruct& ColStruct = CT->ColumnsStructs[ColIdx];
			if (!ColStruct.IsValid())
			{
				Session.Log(TEXT("[FAIL] set_cell -> column is invalid"));
				return sol::lua_nil;
			}

			CT->Modify();

			// ---- Bool column ----
			if (FBoolColumn* BoolCol = ColStruct.GetMutablePtr<FBoolColumn>())
			{
				// Expand if needed
				while (BoolCol->RowValuesWithAny.Num() <= RowIdx)
				{
					BoolCol->RowValuesWithAny.Add(EBoolColumnCellValue::MatchAny);
				}

				std::string ValStr = Opts.get_or<std::string>("value", "Any");
				FString FVal = UTF8_TO_TCHAR(ValStr.c_str());
				if (FVal.Equals(TEXT("True"), ESearchCase::IgnoreCase))
					BoolCol->RowValuesWithAny[RowIdx] = EBoolColumnCellValue::MatchTrue;
				else if (FVal.Equals(TEXT("False"), ESearchCase::IgnoreCase))
					BoolCol->RowValuesWithAny[RowIdx] = EBoolColumnCellValue::MatchFalse;
				else
					BoolCol->RowValuesWithAny[RowIdx] = EBoolColumnCellValue::MatchAny;

				CT->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] set_cell(%d, %d) -> Bool = %s"), RowIndex, ColIndex, *FVal));
				return sol::make_object(Lua, true);
			}

			// ---- Enum column ----
			if (FEnumColumn* EnumCol = ColStruct.GetMutablePtr<FEnumColumn>())
			{
				while (EnumCol->RowValues.Num() <= RowIdx)
				{
					EnumCol->RowValues.Add(FChooserEnumRowData());
				}

				FChooserEnumRowData& RowData = EnumCol->RowValues[RowIdx];
				RowData.Value = static_cast<uint8>(Opts.get_or("value", 0));

				std::string CompStr = Opts.get_or<std::string>("comparison", "Equal");
				FString FComp = UTF8_TO_TCHAR(CompStr.c_str());
				if (FComp.Equals(TEXT("NotEqual"), ESearchCase::IgnoreCase))
					RowData.Comparison = EEnumColumnCellValueComparison::MatchNotEqual;
				else if (FComp.Equals(TEXT("Any"), ESearchCase::IgnoreCase))
					RowData.Comparison = EEnumColumnCellValueComparison::MatchAny;
				else
					RowData.Comparison = EEnumColumnCellValueComparison::MatchEqual;

				CT->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] set_cell(%d, %d) -> Enum = %d (%s)"),
					RowIndex, ColIndex, RowData.Value, *FComp));
				return sol::make_object(Lua, true);
			}

			// ---- GameplayTag column ----
			if (FGameplayTagColumn* TagCol = ColStruct.GetMutablePtr<FGameplayTagColumn>())
			{
				while (TagCol->RowValues.Num() <= RowIdx)
				{
					TagCol->RowValues.Add(FGameplayTagContainer());
				}

				std::string TagStr = Opts.get_or<std::string>("tags", "");
				FString FTags = UTF8_TO_TCHAR(TagStr.c_str());
				FGameplayTagContainer NewTags;
				TArray<FString> TagArray;
				FTags.ParseIntoArray(TagArray, TEXT(","));
				for (const FString& TagName : TagArray)
				{
					FString Trimmed = TagName.TrimStartAndEnd();
					FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*Trimmed), false);
					if (Tag.IsValid())
					{
						NewTags.AddTag(Tag);
					}
				}
				TagCol->RowValues[RowIdx] = NewTags;

				CT->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] set_cell(%d, %d) -> GameplayTag = %s"),
					RowIndex, ColIndex, *NewTags.ToStringSimple()));
				return sol::make_object(Lua, true);
			}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			// ---- GameplayTagQuery column ----
			if (FGameplayTagQueryColumn* TagQCol = ColStruct.GetMutablePtr<FGameplayTagQueryColumn>())
			{
				while (TagQCol->RowValues.Num() <= RowIdx)
				{
					TagQCol->RowValues.Add(FGameplayTagQuery());
				}

				// Parse tags
				std::string TagStr = Opts.get_or<std::string>("tags", "");
				FString FTags = UTF8_TO_TCHAR(TagStr.c_str());
				FGameplayTagContainer TagContainer;
				TArray<FString> TagArray;
				FTags.ParseIntoArray(TagArray, TEXT(","));
				for (const FString& TagName : TagArray)
				{
					FString Trimmed = TagName.TrimStartAndEnd();
					FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*Trimmed), false);
					if (Tag.IsValid())
					{
						TagContainer.AddTag(Tag);
					}
				}

				// Build query based on match_type
				std::string MatchType = Opts.get_or<std::string>("match_type", "any");
				FString FMatchType = UTF8_TO_TCHAR(MatchType.c_str());

				FGameplayTagQuery NewQuery;
				if (FMatchType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
					NewQuery = FGameplayTagQuery::MakeQuery_MatchAllTags(TagContainer);
				else if (FMatchType.Equals(TEXT("none"), ESearchCase::IgnoreCase))
					NewQuery = FGameplayTagQuery::MakeQuery_MatchNoTags(TagContainer);
				else
					NewQuery = FGameplayTagQuery::MakeQuery_MatchAnyTags(TagContainer);

				TagQCol->RowValues[RowIdx] = MoveTemp(NewQuery);

				CT->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] set_cell(%d, %d) -> GameplayTagQuery (%s) = %s"),
					RowIndex, ColIndex, *FMatchType, *FTags));
				return sol::make_object(Lua, true);
			}
#endif

			// ---- FloatRange column ----
			if (FFloatRangeColumn* FRangeCol = ColStruct.GetMutablePtr<FFloatRangeColumn>())
			{
				while (FRangeCol->RowValues.Num() <= RowIdx)
				{
					FRangeCol->RowValues.Add(FChooserFloatRangeRowData());
				}

				FChooserFloatRangeRowData& RangeRow = FRangeCol->RowValues[RowIdx];

				sol::optional<double> MinVal = Opts.get<sol::optional<double>>("min");
				sol::optional<double> MaxVal = Opts.get<sol::optional<double>>("max");

				if (MinVal.has_value())
					RangeRow.Min = static_cast<float>(MinVal.value());
				if (MaxVal.has_value())
					RangeRow.Max = static_cast<float>(MaxVal.value());

				RangeRow.bNoMin = Opts.get_or("no_min", false);
				RangeRow.bNoMax = Opts.get_or("no_max", false);

				CT->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] set_cell(%d, %d) -> FloatRange [%s%g, %g%s]"),
					RowIndex, ColIndex,
					RangeRow.bNoMin ? TEXT("-inf,") : TEXT(""),
					RangeRow.bNoMin ? 0.0f : RangeRow.Min,
					RangeRow.bNoMax ? 0.0f : RangeRow.Max,
					RangeRow.bNoMax ? TEXT(",+inf") : TEXT("")));
				return sol::make_object(Lua, true);
			}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			// ---- FloatDistance column ----
			if (FFloatDistanceColumn* FDistCol = ColStruct.GetMutablePtr<FFloatDistanceColumn>())
			{
				while (FDistCol->RowValues.Num() <= RowIdx)
				{
					FDistCol->RowValues.Add(FChooserFloatDistanceRowData());
				}

				FDistCol->RowValues[RowIdx].Value = static_cast<float>(Opts.get_or("value", 0.0));

				CT->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] set_cell(%d, %d) -> FloatDistance = %g"),
					RowIndex, ColIndex, FDistCol->RowValues[RowIdx].Value));
				return sol::make_object(Lua, true);
			}
#endif

			// ---- Randomize column ----
			if (FRandomizeColumn* RandCol = ColStruct.GetMutablePtr<FRandomizeColumn>())
			{
				while (RandCol->RowValues.Num() <= RowIdx)
				{
					RandCol->RowValues.Add(1.0f);
				}

				RandCol->RowValues[RowIdx] = static_cast<float>(Opts.get_or("weight", 1.0));

				CT->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] set_cell(%d, %d) -> Randomize weight = %g"),
					RowIndex, ColIndex, RandCol->RowValues[RowIdx]));
				return sol::make_object(Lua, true);
			}

			// ---- MultiEnum column ---- (5.5+)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			if (FMultiEnumColumn* MEnumCol = ColStruct.GetMutablePtr<FMultiEnumColumn>())
			{
				while (MEnumCol->RowValues.Num() <= RowIdx)
				{
					MEnumCol->RowValues.Add(FChooserMultiEnumRowData());
				}

				MEnumCol->RowValues[RowIdx].Value = static_cast<uint32>(Opts.get_or("value", 0));

				CT->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] set_cell(%d, %d) -> MultiEnum bitmask = %u"),
					RowIndex, ColIndex, MEnumCol->RowValues[RowIdx].Value));
				return sol::make_object(Lua, true);
			}
#endif

			// ---- Object filter column ----
			if (FObjectColumn* ObjCol = ColStruct.GetMutablePtr<FObjectColumn>())
			{
				while (ObjCol->RowValues.Num() <= RowIdx)
				{
					ObjCol->RowValues.Add(FChooserObjectRowData());
				}

				FChooserObjectRowData& RowData = ObjCol->RowValues[RowIdx];

				std::string CompStr = Opts.get_or<std::string>("comparison", "Equal");
				FString FComp = UTF8_TO_TCHAR(CompStr.c_str());
				if (FComp.Equals(TEXT("NotEqual"), ESearchCase::IgnoreCase))
					RowData.Comparison = EObjectColumnCellValueComparison::MatchNotEqual;
				else if (FComp.Equals(TEXT("Any"), ESearchCase::IgnoreCase))
					RowData.Comparison = EObjectColumnCellValueComparison::MatchAny;
				else
					RowData.Comparison = EObjectColumnCellValueComparison::MatchEqual;

				std::string AssetPath = Opts.get_or<std::string>("asset_path", "");
				if (!AssetPath.empty())
				{
					FString FAssetPath = UTF8_TO_TCHAR(AssetPath.c_str());
					if (!FAssetPath.StartsWith(TEXT("/")))
						FAssetPath = TEXT("/Game/") + FAssetPath;
					RowData.Value = TSoftObjectPtr<UObject>(FSoftObjectPath(FAssetPath));
				}

				CT->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] set_cell(%d, %d) -> Object (%s)"),
					RowIndex, ColIndex, *FComp));
				return sol::make_object(Lua, true);
			}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			// ---- ObjectClass filter column ----
			if (FObjectClassColumn* ObjClassCol = ColStruct.GetMutablePtr<FObjectClassColumn>())
			{
				while (ObjClassCol->RowValues.Num() <= RowIdx)
				{
					ObjClassCol->RowValues.Add(FChooserObjectClassRowData());
				}

				FChooserObjectClassRowData& RowData = ObjClassCol->RowValues[RowIdx];

				std::string CompStr = Opts.get_or<std::string>("comparison", "SubClassOf");
				FString FComp = UTF8_TO_TCHAR(CompStr.c_str());
				if (FComp.Equals(TEXT("Equal"), ESearchCase::IgnoreCase))
					RowData.Comparison = EObjectClassColumnCellValueComparison::Equal;
				else if (FComp.Equals(TEXT("NotEqual"), ESearchCase::IgnoreCase))
					RowData.Comparison = EObjectClassColumnCellValueComparison::NotEqual;
				else if (FComp.Equals(TEXT("NotSubClassOf"), ESearchCase::IgnoreCase))
					RowData.Comparison = EObjectClassColumnCellValueComparison::NotSubClassOf;
				else if (FComp.Equals(TEXT("Any"), ESearchCase::IgnoreCase))
					RowData.Comparison = EObjectClassColumnCellValueComparison::Any;
				else
					RowData.Comparison = EObjectClassColumnCellValueComparison::SubClassOf;

				std::string ClassPath = Opts.get_or<std::string>("class_path", "");
				if (!ClassPath.empty())
				{
					FString FClassPath = UTF8_TO_TCHAR(ClassPath.c_str());
					UClass* FoundClass = FindObject<UClass>(nullptr, *FClassPath);
					if (!FoundClass)
					{
						FoundClass = LoadObject<UClass>(nullptr, *FClassPath);
					}
					RowData.Value = FoundClass; // nullptr is valid (clears the class)
				}

				CT->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] set_cell(%d, %d) -> ObjectClass (%s)"),
					RowIndex, ColIndex, *FComp));
				return sol::make_object(Lua, true);
			}
#endif

			// Not a recognized filter column type
			FString ColTypeName = ColStruct.GetScriptStruct()
				? ColStruct.GetScriptStruct()->GetName()
				: TEXT("Unknown");
			Session.Log(FString::Printf(TEXT("[FAIL] set_cell(%d, %d) -> column type %s is not a settable filter column"),
				RowIndex, ColIndex, *ColTypeName));
			return sol::lua_nil;
		});

		// ================================================================
		// set_output_cell(row, column, opts) — set an output cell value
		// ================================================================
		AssetObj.set_function("set_output_cell", [CT, &Session](sol::table /*self*/,
			int RowIndex, int ColIndex, sol::table Opts, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			int32 RowIdx = RowIndex - 1;
			int32 ColIdx = ColIndex - 1;

			if (RowIdx < 0)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_output_cell -> row index %d must be >= 1"), RowIndex));
				return sol::lua_nil;
			}

			if (ColIdx < 0 || ColIdx >= CT->ColumnsStructs.Num())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_output_cell -> column %d out of range (columns: %d)"),
					ColIndex, CT->ColumnsStructs.Num()));
				return sol::lua_nil;
			}

			FInstancedStruct& ColStruct = CT->ColumnsStructs[ColIdx];
			if (!ColStruct.IsValid())
			{
				Session.Log(TEXT("[FAIL] set_output_cell -> column is invalid"));
				return sol::lua_nil;
			}

			CT->Modify();

			// ---- Output Object column ----
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			if (FOutputObjectColumn* ObjCol = ColStruct.GetMutablePtr<FOutputObjectColumn>())
			{
				while (ObjCol->RowValues.Num() <= RowIdx)
				{
					ObjCol->RowValues.Add(FChooserOutputObjectRowData());
				}

				std::string AssetPath = Opts.get_or<std::string>("asset_path", "");
				FString FAssetPath = UTF8_TO_TCHAR(AssetPath.c_str());
				if (!FAssetPath.StartsWith(TEXT("/")))
					FAssetPath = TEXT("/Game/") + FAssetPath;

				UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *FAssetPath);
				if (!Asset)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set_output_cell -> asset not found: %s"), *FAssetPath));
					return sol::lua_nil;
				}

				FAssetChooser NewResult;
				NewResult.Asset = Asset;
				ObjCol->RowValues[RowIdx].Value = FInstancedStruct::Make(NewResult);

				CT->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] set_output_cell(%d, %d) -> OutputObject = %s"),
					RowIndex, ColIndex, *Asset->GetName()));
				return sol::make_object(Lua, true);
			}
#endif // ENGINE_MINOR_VERSION >= 5

			// ---- Output Float column ----
			if (FOutputFloatColumn* FloatCol = ColStruct.GetMutablePtr<FOutputFloatColumn>())
			{
				while (FloatCol->RowValues.Num() <= RowIdx)
				{
					FloatCol->RowValues.Add(0.0);
				}

				FloatCol->RowValues[RowIdx] = Opts.get_or("value", 0.0);

				CT->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] set_output_cell(%d, %d) -> OutputFloat = %g"),
					RowIndex, ColIndex, FloatCol->RowValues[RowIdx]));
				return sol::make_object(Lua, true);
			}

			// ---- Output Bool column ----
			if (FOutputBoolColumn* BoolCol = ColStruct.GetMutablePtr<FOutputBoolColumn>())
			{
				while (BoolCol->RowValues.Num() <= RowIdx)
				{
					BoolCol->RowValues.Add(false);
				}

				BoolCol->RowValues[RowIdx] = Opts.get_or("value", false);

				CT->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] set_output_cell(%d, %d) -> OutputBool = %s"),
					RowIndex, ColIndex, BoolCol->RowValues[RowIdx] ? TEXT("true") : TEXT("false")));
				return sol::make_object(Lua, true);
			}

			// ---- Output Enum column ----
			if (FOutputEnumColumn* EnumCol = ColStruct.GetMutablePtr<FOutputEnumColumn>())
			{
				while (EnumCol->RowValues.Num() <= RowIdx)
				{
					EnumCol->RowValues.Add(FChooserOutputEnumRowData());
				}

				EnumCol->RowValues[RowIdx].Value = static_cast<uint8>(Opts.get_or("value", 0));

				CT->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] set_output_cell(%d, %d) -> OutputEnum = %d"),
					RowIndex, ColIndex, EnumCol->RowValues[RowIdx].Value));
				return sol::make_object(Lua, true);
			}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			// ---- Output GameplayTagQuery column ----
			if (FOutputGameplayTagQueryColumn* TagQCol = ColStruct.GetMutablePtr<FOutputGameplayTagQueryColumn>())
			{
				while (TagQCol->RowValues.Num() <= RowIdx)
				{
					TagQCol->RowValues.Add(FGameplayTagQuery());
				}

				// Parse tags
				std::string TagStr = Opts.get_or<std::string>("tags", "");
				FString FTags = UTF8_TO_TCHAR(TagStr.c_str());
				FGameplayTagContainer TagContainer;
				TArray<FString> TagArray;
				FTags.ParseIntoArray(TagArray, TEXT(","));
				for (const FString& TagName : TagArray)
				{
					FString Trimmed = TagName.TrimStartAndEnd();
					FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*Trimmed), false);
					if (Tag.IsValid())
					{
						TagContainer.AddTag(Tag);
					}
				}

				// Build query based on match_type
				std::string MatchType = Opts.get_or<std::string>("match_type", "any");
				FString FMatchType = UTF8_TO_TCHAR(MatchType.c_str());

				FGameplayTagQuery NewQuery;
				if (FMatchType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
					NewQuery = FGameplayTagQuery::MakeQuery_MatchAllTags(TagContainer);
				else if (FMatchType.Equals(TEXT("none"), ESearchCase::IgnoreCase))
					NewQuery = FGameplayTagQuery::MakeQuery_MatchNoTags(TagContainer);
				else
					NewQuery = FGameplayTagQuery::MakeQuery_MatchAnyTags(TagContainer);

				TagQCol->RowValues[RowIdx] = MoveTemp(NewQuery);

				CT->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] set_output_cell(%d, %d) -> OutputGameplayTagQuery (%s) = %s"),
					RowIndex, ColIndex, *FMatchType, *FTags));
				return sol::make_object(Lua, true);
			}
#endif

			// Not a recognized output column type
			FString ColTypeName = ColStruct.GetScriptStruct()
				? ColStruct.GetScriptStruct()->GetName()
				: TEXT("Unknown");
			Session.Log(FString::Printf(TEXT("[FAIL] set_output_cell(%d, %d) -> column type %s is not a settable output column"),
				RowIndex, ColIndex, *ColTypeName));
			return sol::lua_nil;
		});

		// ================================================================
		// add_rows(count?, index?) — insert rows
		// ================================================================
		AssetObj.set_function("add_rows", [CT, &Session](sol::table /*self*/,
			sol::optional<int> CountOpt, sol::optional<int> IndexOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			int32 Count = CountOpt.value_or(1);
			int32 CurrentRows = GetChooserRowCount(CT);
			int32 InsertAt = IndexOpt.has_value() ? (IndexOpt.value() - 1) : CurrentRows; // 1-based to 0-based
			InsertAt = FMath::Clamp(InsertAt, 0, CurrentRows);

			CT->Modify();

			// Insert into all columns
			for (FInstancedStruct& ColStruct : CT->ColumnsStructs)
			{
				if (!ColStruct.IsValid()) continue;
				if (FChooserColumnBase* ColBase = ColStruct.GetMutablePtr<FChooserColumnBase>())
				{
					ColBase->InsertRows(InsertAt, Count);
				}
			}

			// Insert into ResultsStructs
			for (int32 i = 0; i < Count; ++i)
			{
				if (InsertAt <= CT->ResultsStructs.Num())
				{
					CT->ResultsStructs.Insert(FInstancedStruct(), InsertAt);
				}
				else
				{
					CT->ResultsStructs.Add(FInstancedStruct());
				}
			}

			// Insert into DisabledRows
			for (int32 i = 0; i < Count; ++i)
			{
				if (InsertAt <= CT->DisabledRows.Num())
				{
					CT->DisabledRows.Insert(false, InsertAt);
				}
				else
				{
					CT->DisabledRows.Add(false);
				}
			}

			CT->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] add_rows(%d) at index %d -> now %d rows"),
				Count, InsertAt + 1, GetChooserRowCount(CT)));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// delete_rows(indices) — delete rows by 1-based index array
		// ================================================================
		AssetObj.set_function("delete_rows", [CT, &Session](sol::table /*self*/,
			sol::table Indices, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			// Collect and sort indices (convert 1-based to 0-based)
			TArray<uint32> RowIndices;
			for (auto& Pair : Indices)
			{
				if (Pair.second.is<int>())
				{
					RowIndices.Add(static_cast<uint32>(Pair.second.as<int>() - 1));
				}
			}

			if (RowIndices.Num() == 0)
			{
				Session.Log(TEXT("[FAIL] delete_rows -> no valid indices provided"));
				return sol::lua_nil;
			}

			// Sort descending and deduplicate — engine's DeleteRows calls RemoveAt
			// sequentially, so indices must be in reverse order to stay valid
			RowIndices.Sort([](uint32 A, uint32 B) { return A > B; });
			for (int32 i = RowIndices.Num() - 1; i > 0; --i)
			{
				if (RowIndices[i] == RowIndices[i - 1])
				{
					RowIndices.RemoveAt(i);
				}
			}

			CT->Modify();

			// Delete from columns (indices already reverse-sorted)
			for (FInstancedStruct& ColStruct : CT->ColumnsStructs)
			{
				if (!ColStruct.IsValid()) continue;
				if (FChooserColumnBase* ColBase = ColStruct.GetMutablePtr<FChooserColumnBase>())
				{
					ColBase->DeleteRows(RowIndices);
				}
			}

			// Delete from ResultsStructs and DisabledRows (same reverse order)
			for (uint32 Idx : RowIndices)
			{
				if ((int32)Idx < CT->ResultsStructs.Num())
				{
					CT->ResultsStructs.RemoveAt(Idx);
				}
				if ((int32)Idx < CT->DisabledRows.Num())
				{
					CT->DisabledRows.RemoveAt(Idx);
				}
			}

			CT->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] delete_rows -> deleted %d rows, now %d rows"),
				RowIndices.Num(), GetChooserRowCount(CT)));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// move_row(from, to) — 1-based indices
		// ================================================================
		AssetObj.set_function("move_row", [CT, &Session](sol::table /*self*/,
			int From, int To, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			int32 SrcIdx = From - 1; // 1-based to 0-based
			int32 DstIdx = To - 1;
			int32 RowCount = GetChooserRowCount(CT);

			if (SrcIdx < 0 || SrcIdx >= RowCount || DstIdx < 0 || DstIdx >= RowCount)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] move_row -> index out of range (rows: %d)"), RowCount));
				return sol::lua_nil;
			}

			CT->Modify();

			for (FInstancedStruct& ColStruct : CT->ColumnsStructs)
			{
				if (!ColStruct.IsValid()) continue;
				if (FChooserColumnBase* ColBase = ColStruct.GetMutablePtr<FChooserColumnBase>())
				{
					ColBase->MoveRow(SrcIdx, DstIdx);
				}
			}

			// Move in ResultsStructs — must match engine MoveRow logic:
			// RemoveAt shifts elements, so adjust target when moving forward
			if (SrcIdx < CT->ResultsStructs.Num() && DstIdx < CT->ResultsStructs.Num())
			{
				FInstancedStruct Temp = MoveTemp(CT->ResultsStructs[SrcIdx]);
				CT->ResultsStructs.RemoveAt(SrcIdx);
				int32 AdjustedDst = (SrcIdx < DstIdx) ? DstIdx - 1 : DstIdx;
				CT->ResultsStructs.Insert(MoveTemp(Temp), AdjustedDst);
			}

			// Move in DisabledRows — same adjustment
			if (SrcIdx < CT->DisabledRows.Num() && DstIdx < CT->DisabledRows.Num())
			{
				bool Temp = CT->DisabledRows[SrcIdx];
				CT->DisabledRows.RemoveAt(SrcIdx);
				int32 AdjustedDst = (SrcIdx < DstIdx) ? DstIdx - 1 : DstIdx;
				CT->DisabledRows.Insert(Temp, AdjustedDst);
			}

			CT->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] move_row -> moved row %d to %d"), From, To));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// set_row_disabled(index, disabled)
		// ================================================================
		AssetObj.set_function("set_row_disabled", [CT, &Session](sol::table /*self*/,
			int Index, bool bDisabled, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			int32 Idx = Index - 1;

			if (Idx < 0)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_row_disabled -> index %d must be >= 1"), Index));
				return sol::lua_nil;
			}

			// Expand if needed
			while (CT->DisabledRows.Num() <= Idx)
			{
				CT->DisabledRows.Add(false);
			}

			CT->Modify();
			CT->DisabledRows[Idx] = bDisabled;
			CT->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] set_row_disabled(%d, %s)"),
				Index, bDisabled ? TEXT("true") : TEXT("false")));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// set_row_result(index, asset_path) — set result asset for a row
		// ================================================================
		AssetObj.set_function("set_row_result", [CT, &Session](sol::table /*self*/,
			int Index, const std::string& AssetPath, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			int32 Idx = Index - 1;

			if (Idx < 0 || Idx >= CT->ResultsStructs.Num())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_row_result -> index %d out of range (rows: %d)"),
					Index, CT->ResultsStructs.Num()));
				return sol::lua_nil;
			}

			FString FAssetPath = UTF8_TO_TCHAR(AssetPath.c_str());
			if (!FAssetPath.StartsWith(TEXT("/")))
				FAssetPath = TEXT("/Game/") + FAssetPath;

			UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *FAssetPath);
			if (!Asset)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_row_result -> asset not found: %s"), *FAssetPath));
				return sol::lua_nil;
			}

			CT->Modify();

			FAssetChooser NewResult;
			NewResult.Asset = Asset;
			CT->ResultsStructs[Idx] = FInstancedStruct::Make(NewResult);

			CT->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] set_row_result(%d) -> %s"), Index, *Asset->GetName()));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// set_fallback_result(asset_path)
		// ================================================================
		AssetObj.set_function("set_fallback_result", [CT, &Session](sol::table /*self*/,
			const std::string& AssetPath, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			FString FAssetPath = UTF8_TO_TCHAR(AssetPath.c_str());
			if (!FAssetPath.StartsWith(TEXT("/")))
				FAssetPath = TEXT("/Game/") + FAssetPath;

			UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *FAssetPath);
			if (!Asset)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_fallback_result -> asset not found: %s"), *FAssetPath));
				return sol::lua_nil;
			}

			CT->Modify();

			FAssetChooser NewResult;
			NewResult.Asset = Asset;
			CT->FallbackResult = FInstancedStruct::Make(NewResult);

			CT->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] set_fallback_result -> %s"), *Asset->GetName()));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// compile() — recompile the chooser after edits
		// ================================================================
		AssetObj.set_function("compile", [CT, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			CT->Compile(true);
			Session.Log(TEXT("[OK] compile -> chooser recompiled"));
			return sol::make_object(Lua, true);
		});
#endif // WITH_EDITOR
	});
}

#else
// Chooser binding not supported on UE 5.4 — API changed significantly in 5.5
static TArray<FLuaFunctionDoc> ChooserTableDocs = {};
#endif // ENGINE_MINOR_VERSION >= 5

static void ChooserTable_TryBind(sol::state& Lua, FLuaSessionData& Session)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("Chooser")))
	{
		Session.Log(TEXT("[WARN] Chooser plugin is not loaded. Enable it in Edit > Plugins to use this feature."));
		return;
	}
	BindChooserTable(Lua, Session);
#else
	Session.Log(TEXT("[WARN] Chooser editing requires UE 5.5+. Binding not available in this engine version."));
#endif
}

REGISTER_LUA_BINDING(ChooserTable, ChooserTableDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	ChooserTable_TryBind(Lua, Session);
});
