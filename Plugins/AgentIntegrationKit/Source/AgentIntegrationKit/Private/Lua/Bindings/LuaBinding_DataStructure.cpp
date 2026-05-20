#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
#include "StructUtils/UserDefinedStruct.h"
#else
#include "Engine/UserDefinedStruct.h"
#endif
#include "Engine/UserDefinedEnum.h"
#include "Engine/DataTable.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
#include "EdGraphSchema_K2.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

// Strip trailing _N numeric suffix from FriendlyName (e.g. "Health_1" -> "Health")
static FString StripFriendlyNameSuffix(const FString& FriendlyName)
{
	int32 LastUnderscore;
	if (FriendlyName.FindLastChar(TEXT('_'), LastUnderscore) && LastUnderscore > 0)
	{
		FString Suffix = FriendlyName.Mid(LastUnderscore + 1);
		bool bAllDigits = true;
		for (TCHAR Ch : Suffix)
		{
			if (!FChar::IsDigit(Ch)) { bAllDigits = false; break; }
		}
		if (bAllDigits && Suffix.Len() > 0)
		{
			return FriendlyName.Left(LastUnderscore);
		}
	}
	return FriendlyName;
}

// Find a VarDesc by user-facing display name (stripped FriendlyName)
static FStructVariableDescription* FindVarDescByDisplayName(UUserDefinedStruct* Struct, const FString& DisplayName)
{
	TArray<FStructVariableDescription>& VarDescs = FStructureEditorUtils::GetVarDesc(Struct);
	for (FStructVariableDescription& Desc : VarDescs)
	{
		FString Stripped = StripFriendlyNameSuffix(Desc.FriendlyName);
		if (Stripped.Equals(DisplayName, ESearchCase::IgnoreCase))
		{
			return &Desc;
		}
	}
	return nullptr;
}

// Try to find a UClass by short or full name, with common prefix fallbacks
static UClass* FindClassByName(const FString& Name)
{
	UClass* Found = UClass::TryFindTypeSlow<UClass>(Name);
	if (!Found) Found = UClass::TryFindTypeSlow<UClass>(TEXT("U") + Name);
	if (!Found) Found = UClass::TryFindTypeSlow<UClass>(TEXT("A") + Name);
	if (!Found) Found = UClass::TryFindTypeSlow<UClass>(FString::Printf(TEXT("/Script/Engine.%s"), *Name));
	return Found;
}

// Try to find a UEnum by short or full name
static UEnum* FindEnumByName(const FString& Name)
{
	UEnum* Found = UClass::TryFindTypeSlow<UEnum>(Name);
	if (!Found) Found = UClass::TryFindTypeSlow<UEnum>(TEXT("E") + Name);
	if (!Found) Found = UClass::TryFindTypeSlow<UEnum>(FString::Printf(TEXT("/Script/Engine.%s"), *Name));
	return Found;
}

// Resolve a base type name (no container) to an FEdGraphPinType
static FEdGraphPinType BaseTypeNameToPinType(const FString& TypeName, const FString& SubType = FString())
{
	FEdGraphPinType PinType;

	if (TypeName.Equals(TEXT("Boolean"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("bool"), ESearchCase::IgnoreCase))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	}
	else if (TypeName.Equals(TEXT("Integer"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("int"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("int32"), ESearchCase::IgnoreCase))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
	}
	else if (TypeName.Equals(TEXT("Int64"), ESearchCase::IgnoreCase))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
	}
	else if (TypeName.Equals(TEXT("Float"), ESearchCase::IgnoreCase))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
	}
	else if (TypeName.Equals(TEXT("Double"), ESearchCase::IgnoreCase))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
	}
	else if (TypeName.Equals(TEXT("String"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("FString"), ESearchCase::IgnoreCase))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_String;
	}
	else if (TypeName.Equals(TEXT("Name"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("FName"), ESearchCase::IgnoreCase))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
	}
	else if (TypeName.Equals(TEXT("Text"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("FText"), ESearchCase::IgnoreCase))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
	}
	else if (TypeName.Equals(TEXT("Vector"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("FVector"), ESearchCase::IgnoreCase))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
	}
	else if (TypeName.Equals(TEXT("Rotator"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("FRotator"), ESearchCase::IgnoreCase))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
	}
	else if (TypeName.Equals(TEXT("Transform"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("FTransform"), ESearchCase::IgnoreCase))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
	}
	else if (TypeName.Equals(TEXT("LinearColor"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("FLinearColor"), ESearchCase::IgnoreCase))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
	}
	else if (TypeName.Equals(TEXT("Color"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("FColor"), ESearchCase::IgnoreCase))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FColor>::Get();
	}
	else if (TypeName.Equals(TEXT("Object"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("UObject"), ESearchCase::IgnoreCase))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
		// Resolve sub_type to a UClass if provided, default to UObject
		UClass* BaseClass = UObject::StaticClass();
		if (!SubType.IsEmpty())
		{
			UClass* Found = FindClassByName(SubType);
			if (Found) BaseClass = Found;
		}
		PinType.PinSubCategoryObject = BaseClass;
	}
	else if (TypeName.Equals(TEXT("Class"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("UClass"), ESearchCase::IgnoreCase))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Class;
		UClass* BaseClass = UObject::StaticClass();
		if (!SubType.IsEmpty())
		{
			UClass* Found = FindClassByName(SubType);
			if (Found) BaseClass = Found;
		}
		PinType.PinSubCategoryObject = BaseClass;
	}
	else if (TypeName.Equals(TEXT("SoftObject"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("TSoftObjectPtr"), ESearchCase::IgnoreCase))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_SoftObject;
		UClass* BaseClass = UObject::StaticClass();
		if (!SubType.IsEmpty())
		{
			UClass* Found = FindClassByName(SubType);
			if (Found) BaseClass = Found;
		}
		PinType.PinSubCategoryObject = BaseClass;
	}
	else if (TypeName.Equals(TEXT("SoftClass"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("TSoftClassPtr"), ESearchCase::IgnoreCase))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_SoftClass;
		UClass* BaseClass = UObject::StaticClass();
		if (!SubType.IsEmpty())
		{
			UClass* Found = FindClassByName(SubType);
			if (Found) BaseClass = Found;
		}
		PinType.PinSubCategoryObject = BaseClass;
	}
	else if (TypeName.Equals(TEXT("Byte"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("uint8"), ESearchCase::IgnoreCase))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		// Support enum-backed byte fields via sub_type
		if (!SubType.IsEmpty())
		{
			UEnum* EnumType = FindEnumByName(SubType);
			if (EnumType)
			{
				PinType.PinSubCategoryObject = EnumType;
			}
		}
	}
	else if (TypeName.Equals(TEXT("Enum"), ESearchCase::IgnoreCase))
	{
		// Enum type — requires sub_type to specify which enum
		PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		if (!SubType.IsEmpty())
		{
			UEnum* EnumType = FindEnumByName(SubType);
			if (EnumType)
			{
				PinType.PinSubCategoryObject = EnumType;
			}
		}
	}
	else
	{
		// Default to string for unknown types
		PinType.PinCategory = UEdGraphSchema_K2::PC_String;
	}

	return PinType;
}

// Full type parser supporting container types: "Array<Float>", "Set<String>", "Map<String,Integer>"
// Also supports sub_type parameter for Object/Class/Byte/Enum base class / enum specification
static FEdGraphPinType TypeNameToPinType(const FString& TypeName, const FString& SubType = FString())
{
	FString Trimmed = TypeName.TrimStartAndEnd();

	// Check for Array<T>
	if (Trimmed.StartsWith(TEXT("Array<"), ESearchCase::IgnoreCase) && Trimmed.EndsWith(TEXT(">")))
	{
		FString Inner = Trimmed.Mid(6, Trimmed.Len() - 7).TrimStartAndEnd();
		FEdGraphPinType PinType = BaseTypeNameToPinType(Inner, SubType);
		PinType.ContainerType = EPinContainerType::Array;
		return PinType;
	}

	// Check for Set<T>
	if (Trimmed.StartsWith(TEXT("Set<"), ESearchCase::IgnoreCase) && Trimmed.EndsWith(TEXT(">")))
	{
		FString Inner = Trimmed.Mid(4, Trimmed.Len() - 5).TrimStartAndEnd();
		FEdGraphPinType PinType = BaseTypeNameToPinType(Inner, SubType);
		PinType.ContainerType = EPinContainerType::Set;
		return PinType;
	}

	// Check for Map<K,V>
	if (Trimmed.StartsWith(TEXT("Map<"), ESearchCase::IgnoreCase) && Trimmed.EndsWith(TEXT(">")))
	{
		FString Inner = Trimmed.Mid(4, Trimmed.Len() - 5).TrimStartAndEnd();
		int32 CommaIdx;
		if (Inner.FindChar(TEXT(','), CommaIdx))
		{
			FString KeyType = Inner.Left(CommaIdx).TrimStartAndEnd();
			FString ValueType = Inner.Mid(CommaIdx + 1).TrimStartAndEnd();
			FEdGraphPinType PinType = BaseTypeNameToPinType(KeyType, SubType);
			PinType.ContainerType = EPinContainerType::Map;
			// Set value terminal type
			FEdGraphPinType ValuePinType = BaseTypeNameToPinType(ValueType);
			PinType.PinValueType.TerminalCategory = ValuePinType.PinCategory;
			PinType.PinValueType.TerminalSubCategory = ValuePinType.PinSubCategory;
			PinType.PinValueType.TerminalSubCategoryObject = ValuePinType.PinSubCategoryObject;
			return PinType;
		}
	}

	return BaseTypeNameToPinType(Trimmed, SubType);
}

// Convert a PinCategory back to a base type name (no container prefix)
static FString BasePinTypeToTypeName(const FEdGraphPinType& PinType)
{
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean) return TEXT("Boolean");
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int) return TEXT("Integer");
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int64) return TEXT("Int64");
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
	{
		if (PinType.PinSubCategory == UEdGraphSchema_K2::PC_Float) return TEXT("Float");
		return TEXT("Double");
	}
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_String) return TEXT("String");
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Name) return TEXT("Name");
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Text) return TEXT("Text");
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Byte)
	{
		UEnum* EnumType = Cast<UEnum>(PinType.PinSubCategoryObject.Get());
		if (EnumType) return TEXT("Byte");  // enum-backed byte — sub_type reported separately
		return TEXT("Byte");
	}
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Object) return TEXT("Object");
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Class) return TEXT("Class");
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject) return TEXT("SoftObject");
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass) return TEXT("SoftClass");
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
	{
		UScriptStruct* StructType = Cast<UScriptStruct>(PinType.PinSubCategoryObject.Get());
		if (StructType) return StructType->GetName();
		return TEXT("Struct");
	}
	return PinType.PinCategory.ToString();
}

// Get the sub-type string for Object/Class/SoftObject/SoftClass/Byte pin types
static FString PinTypeToSubTypeName(const FEdGraphPinType& PinType)
{
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Object ||
		PinType.PinCategory == UEdGraphSchema_K2::PC_Class ||
		PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject ||
		PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass)
	{
		UClass* SubClass = Cast<UClass>(PinType.PinSubCategoryObject.Get());
		if (SubClass && SubClass != UObject::StaticClass())
		{
			return SubClass->GetName();
		}
	}
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Byte)
	{
		UEnum* EnumType = Cast<UEnum>(PinType.PinSubCategoryObject.Get());
		if (EnumType) return EnumType->GetName();
	}
	return FString();
}

// Convert terminal type back to a type name (for Map value types)
static FString TerminalTypeToTypeName(const FEdGraphTerminalType& TermType)
{
	FEdGraphPinType Tmp;
	Tmp.PinCategory = TermType.TerminalCategory;
	Tmp.PinSubCategory = TermType.TerminalSubCategory;
	Tmp.PinSubCategoryObject = TermType.TerminalSubCategoryObject;
	return BasePinTypeToTypeName(Tmp);
}

// Convert a PinCategory back to a human-readable type name, including container prefix
static FString PinTypeToTypeName(const FEdGraphPinType& PinType)
{
	FString Base = BasePinTypeToTypeName(PinType);

	if (PinType.ContainerType == EPinContainerType::Array)
	{
		return FString::Printf(TEXT("Array<%s>"), *Base);
	}
	if (PinType.ContainerType == EPinContainerType::Set)
	{
		return FString::Printf(TEXT("Set<%s>"), *Base);
	}
	if (PinType.ContainerType == EPinContainerType::Map)
	{
		FString ValueType = TerminalTypeToTypeName(PinType.PinValueType);
		return FString::Printf(TEXT("Map<%s,%s>"), *Base, *ValueType);
	}

	return Base;
}

// Get the column type name for a DataTable property
static FString GetPropertyTypeName(const FProperty* Prop)
{
	if (!Prop) return TEXT("Unknown");
	if (Prop->IsA<FBoolProperty>()) return TEXT("Boolean");
	if (Prop->IsA<FIntProperty>()) return TEXT("Integer");
	if (Prop->IsA<FInt64Property>()) return TEXT("Int64");
	if (Prop->IsA<FFloatProperty>()) return TEXT("Float");
	if (Prop->IsA<FDoubleProperty>()) return TEXT("Double");
	if (Prop->IsA<FStrProperty>()) return TEXT("String");
	if (Prop->IsA<FNameProperty>()) return TEXT("Name");
	if (Prop->IsA<FTextProperty>()) return TEXT("Text");
	if (Prop->IsA<FByteProperty>()) return TEXT("Byte");
	if (Prop->IsA<FObjectProperty>()) return TEXT("Object");
	if (Prop->IsA<FSoftObjectProperty>()) return TEXT("SoftObject");
	if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		if (StructProp->Struct) return StructProp->Struct->GetName();
	}
	return Prop->GetCPPType();
}

// ============================================================================
// DOCS + REGISTRATION
// ============================================================================

static TArray<FLuaFunctionDoc> DataStructureDocs = {};

static void BindDataStructure(sol::state& Lua, FLuaSessionData& Session)
{
	// ===========================================================================
	// UserDefinedStruct
	// ===========================================================================
	Lua.set_function("_enrich_user_defined_struct", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UUserDefinedStruct* Struct = LoadObject<UUserDefinedStruct>(nullptr, *FPath);
		if (!Struct)
		{
			// Try with .AssetName suffix (e.g. /Game/X/MyStruct -> /Game/X/MyStruct.MyStruct)
			FString AssetName = FPackageName::GetShortName(FPath);
			FString FullPath = FPath + TEXT(".") + AssetName;
			Struct = LoadObject<UUserDefinedStruct>(nullptr, *FullPath);
		}
		if (!Struct)
		{
			Session.Log(FString::Printf(TEXT("[WARN] _enrich_user_defined_struct -> could not load '%s' as UUserDefinedStruct"), *FPath));
			return;
		}

		// ---- help text ----
		AssetObj["_help_text"] =
			"Element types for add/remove/list/configure:\n"
			"  field — struct variable (Boolean, Integer, Float, Double, String, Name, Text, Vector, Rotator, Transform, LinearColor, Object, Class, Byte, Enum, etc.)\n"
			"\n"
			"add(\"field\", {name=.., type=\"Float\", default_value=.., description=.., sub_type=..}):\n"
			"  Adds a new field. Type defaults to String if omitted.\n"
			"  Container types: type=\"Array<Float>\", \"Set<String>\", \"Map<String,Integer>\"\n"
			"  Object/Class sub-type: type=\"Object\", sub_type=\"Actor\" (defaults to UObject)\n"
			"  Enum-backed byte: type=\"Byte\", sub_type=\"ECollisionChannel\" (or type=\"Enum\", sub_type=..)\n"
			"\n"
			"remove(\"field\", name):\n"
			"  Removes a field by display name.\n"
			"\n"
			"list(\"fields\"):\n"
			"  Returns array of {name, type, sub_type, default_value, description, editable_on_instance, save_game, multiline_text, 3d_widget}.\n"
			"\n"
			"configure(\"field\", {name=.., new_name=.., type=.., sub_type=.., default_value=.., description=.., move_before=.., move_after=..}):\n"
			"  Modifies an existing field (rename, retype, set default, set tooltip, reorder).\n"
			"  move_before/move_after: reorder field relative to another field by name.\n"
			"  editable_on_instance=true/false: control per-instance editability in Blueprints.\n"
			"  save_game=true/false: include field in save game serialization.\n"
			"  multiline_text=true/false: enable multi-line editing for text/string fields.\n"
			"  3d_widget=true/false: enable 3D widget for vector/transform fields.\n"
			"  metadata={ClampMin=\"0\", ClampMax=\"100\", UIMin=\"0\", UIMax=\"100\"}: set field metadata.\n"
			"\n"
			"configure(\"struct\", {description=\"Tooltip for the struct\"}):\n"
			"  Sets the struct-level tooltip.\n"
			"\n"
			"info():\n"
			"  Returns {num_fields, fields=[{name,type,sub_type}], is_valid, description}.\n";

		// ---- add("field", params) ----
		AssetObj.set_function("add", [Struct, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (!FType.Equals(TEXT("field"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: field"), *FType));
				return sol::lua_nil;
			}

			if (!Params.has_value())
			{
				Session.Log(TEXT("[FAIL] add(\"field\") -> params required: {name=.., type=\"Float\"}"));
				return sol::lua_nil;
			}

			sol::table P = Params.value();
			std::string Name = P.get_or<std::string>("name", "");
			if (Name.empty())
			{
				Session.Log(TEXT("[FAIL] add(\"field\") -> name required"));
				return sol::lua_nil;
			}

			FString FName_Display = UTF8_TO_TCHAR(Name.c_str());

			// Check duplicate by display name (FriendlyName stripped)
			if (FindVarDescByDisplayName(Struct, FName_Display))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add(\"field\") -> field '%s' already exists"), *FName_Display));
				return sol::lua_nil;
			}

			std::string TypeStr = P.get_or<std::string>("type", "String");
			std::string SubTypeStr = P.get_or<std::string>("sub_type", "");
			FEdGraphPinType PinType = TypeNameToPinType(UTF8_TO_TCHAR(TypeStr.c_str()), UTF8_TO_TCHAR(SubTypeStr.c_str()));

			FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Add Struct Field: %s"), *FName_Display)));

			if (!FStructureEditorUtils::AddVariable(Struct, PinType))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add(\"field\", \"%s\") -> AddVariable failed"), *FName_Display));
				return sol::lua_nil;
			}

			TArray<FStructVariableDescription>& VarDescs = FStructureEditorUtils::GetVarDesc(Struct);
			if (VarDescs.Num() == 0)
			{
				Session.Log(TEXT("[FAIL] add(\"field\") -> VarDesc empty after AddVariable"));
				return sol::lua_nil;
			}

			FStructVariableDescription& NewVar = VarDescs.Last();

			// Rename to desired name
			FStructureEditorUtils::RenameVariable(Struct, NewVar.VarGuid, FName_Display);

			// Set default value
			std::string DefaultVal = P.get_or<std::string>("default_value", "");
			if (!DefaultVal.empty())
			{
				FStructureEditorUtils::ChangeVariableDefaultValue(Struct, NewVar.VarGuid, UTF8_TO_TCHAR(DefaultVal.c_str()));
			}

			// Set tooltip via proper API (BUG FIX: MCP tool sets VarDesc.ToolTip directly)
			std::string Desc = P.get_or<std::string>("description", "");
			if (!Desc.empty())
			{
				FStructureEditorUtils::ChangeVariableTooltip(Struct, NewVar.VarGuid, FString(UTF8_TO_TCHAR(Desc.c_str())));
			}

			Struct->GetPackage()->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] add(\"field\", name=\"%s\", type=\"%s\")"), *FName_Display, UTF8_TO_TCHAR(TypeStr.c_str())));
			return sol::make_object(Lua, true);
		});

		// ---- remove("field", name) ----
		AssetObj.set_function("remove", [Struct, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (!FType.Equals(TEXT("field"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: field"), *FType));
				return sol::lua_nil;
			}

			if (!Id.is<std::string>())
			{
				Session.Log(TEXT("[FAIL] remove(\"field\") -> field name string required"));
				return sol::lua_nil;
			}

			FString FieldName = UTF8_TO_TCHAR(Id.as<std::string>().c_str());

			// BUG FIX: Match by FriendlyName (stripped), NOT VarName
			FStructVariableDescription* Desc = FindVarDescByDisplayName(Struct, FieldName);
			if (!Desc)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"field\", \"%s\") -> not found"), *FieldName));
				return sol::lua_nil;
			}

			FGuid VarGuid = Desc->VarGuid;
			FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Remove Struct Field: %s"), *FieldName)));

			bool bOk = FStructureEditorUtils::RemoveVariable(Struct, VarGuid);
			if (bOk) Struct->GetPackage()->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[%s] remove(\"field\", \"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *FieldName));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		// ---- list("fields") ----
		AssetObj.set_function("list", [Struct, &Session](sol::table self,
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

			if (!FType.Equals(TEXT("fields"), ESearchCase::IgnoreCase) && !FType.Equals(TEXT("field"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: fields"), *FType));
				return sol::lua_nil;
			}

			TArray<FStructVariableDescription>& VarDescs = FStructureEditorUtils::GetVarDesc(Struct);
			sol::table Result = Lua.create_table();

			for (int32 i = 0; i < VarDescs.Num(); i++)
			{
				const FStructVariableDescription& Desc = VarDescs[i];
				FEdGraphPinType PT = Desc.ToPinType();
				sol::table E = Lua.create_table();
				E["name"] = TCHAR_TO_UTF8(*StripFriendlyNameSuffix(Desc.FriendlyName));
				E["type"] = TCHAR_TO_UTF8(*PinTypeToTypeName(PT));
				FString SubTypeName = PinTypeToSubTypeName(PT);
				if (!SubTypeName.IsEmpty()) E["sub_type"] = TCHAR_TO_UTF8(*SubTypeName);
				E["default_value"] = TCHAR_TO_UTF8(*Desc.DefaultValue);
				E["description"] = TCHAR_TO_UTF8(*Desc.ToolTip);
				E["editable_on_instance"] = !Desc.bDontEditOnInstance;
				E["save_game"] = (bool)Desc.bEnableSaveGame;
				E["multiline_text"] = (bool)Desc.bEnableMultiLineText;
				E["3d_widget"] = (bool)Desc.bEnable3dWidget;
				Result[i + 1] = E;
			}

			Session.Log(FString::Printf(TEXT("[OK] list(\"fields\") -> %d"), VarDescs.Num()));
			return Result;
		});

		// ---- configure("field" or "struct", params) ----
		AssetObj.set_function("configure", [Struct, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// configure("struct", {description=..}) — struct-level tooltip
			if (FType.Equals(TEXT("struct"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"struct\") -> params required: {description=..}"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();
				std::string DescStr = P.get_or<std::string>("description", "");
				if (DescStr.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"struct\") -> description required"));
					return sol::lua_nil;
				}
				FScopedTransaction Transaction(FText::FromString(TEXT("Configure Struct Tooltip")));
				FStructureEditorUtils::ChangeTooltip(Struct, FString(UTF8_TO_TCHAR(DescStr.c_str())));
				Struct->GetPackage()->MarkPackageDirty();
				Session.Log(TEXT("[OK] configure(\"struct\") -> description updated"));
				return sol::make_object(Lua, true);
			}

			if (!FType.Equals(TEXT("field"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: field, struct"), *FType));
				return sol::lua_nil;
			}

			if (!Params.has_value())
			{
				Session.Log(TEXT("[FAIL] configure(\"field\") -> params required: {name=.., new_name=.., type=.., default_value=.., description=..}"));
				return sol::lua_nil;
			}

			sol::table P = Params.value();
			std::string Name = P.get_or<std::string>("name", "");
			if (Name.empty())
			{
				Session.Log(TEXT("[FAIL] configure(\"field\") -> name required to identify field"));
				return sol::lua_nil;
			}

			FString FName_Display = UTF8_TO_TCHAR(Name.c_str());

			// BUG FIX: Match by FriendlyName (stripped), NOT VarName
			FStructVariableDescription* Desc = FindVarDescByDisplayName(Struct, FName_Display);
			if (!Desc)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure(\"field\") -> field '%s' not found"), *FName_Display));
				return sol::lua_nil;
			}

			FGuid VarGuid = Desc->VarGuid;
			FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Configure Struct Field: %s"), *FName_Display)));
			TArray<FString> Changes;

			// Rename
			std::string NewName = P.get_or<std::string>("new_name", "");
			if (!NewName.empty())
			{
				FString FNewName = UTF8_TO_TCHAR(NewName.c_str());
				if (!FNewName.Equals(FName_Display))
				{
					FStructureEditorUtils::RenameVariable(Struct, VarGuid, FNewName);
					Changes.Add(FString::Printf(TEXT("renamed to '%s'"), *FNewName));
				}
			}

			// Change type (with optional sub_type)
			std::string TypeStr = P.get_or<std::string>("type", "");
			if (!TypeStr.empty())
			{
				std::string SubTypeStr = P.get_or<std::string>("sub_type", "");
				FEdGraphPinType NewPinType = TypeNameToPinType(UTF8_TO_TCHAR(TypeStr.c_str()), UTF8_TO_TCHAR(SubTypeStr.c_str()));
				if (FStructureEditorUtils::ChangeVariableType(Struct, VarGuid, NewPinType))
				{
					Changes.Add(FString::Printf(TEXT("type -> %s"), UTF8_TO_TCHAR(TypeStr.c_str())));
				}
			}

			// Change default value
			std::string DefaultVal = P.get_or<std::string>("default_value", "");
			if (!DefaultVal.empty())
			{
				FStructureEditorUtils::ChangeVariableDefaultValue(Struct, VarGuid, UTF8_TO_TCHAR(DefaultVal.c_str()));
				Changes.Add(FString::Printf(TEXT("default -> %s"), UTF8_TO_TCHAR(DefaultVal.c_str())));
			}

			// Change tooltip via proper API
			std::string DescStr = P.get_or<std::string>("description", "");
			if (!DescStr.empty())
			{
				FStructureEditorUtils::ChangeVariableTooltip(Struct, VarGuid, FString(UTF8_TO_TCHAR(DescStr.c_str())));
				Changes.Add(TEXT("description updated"));
			}

			// Editable on BP instance
			sol::optional<bool> EditableOpt = P.get<sol::optional<bool>>("editable_on_instance");
			if (EditableOpt.has_value())
			{
				FStructureEditorUtils::ChangeEditableOnBPInstance(Struct, VarGuid, EditableOpt.value());
				Changes.Add(FString::Printf(TEXT("editable_on_instance=%s"), EditableOpt.value() ? TEXT("true") : TEXT("false")));
			}

			// Save game serialization
			sol::optional<bool> SaveGameOpt = P.get<sol::optional<bool>>("save_game");
			if (SaveGameOpt.has_value())
			{
				FStructureEditorUtils::ChangeSaveGameEnabled(Struct, VarGuid, SaveGameOpt.value());
				Changes.Add(FString::Printf(TEXT("save_game=%s"), SaveGameOpt.value() ? TEXT("true") : TEXT("false")));
			}

			// Multi-line text
			sol::optional<bool> MultilineOpt = P.get<sol::optional<bool>>("multiline_text");
			if (MultilineOpt.has_value())
			{
				if (FStructureEditorUtils::CanEnableMultiLineText(Struct, VarGuid))
				{
					FStructureEditorUtils::ChangeMultiLineTextEnabled(Struct, VarGuid, MultilineOpt.value());
					Changes.Add(FString::Printf(TEXT("multiline_text=%s"), MultilineOpt.value() ? TEXT("true") : TEXT("false")));
				}
				else
				{
					Session.Log(TEXT("[WARN] multiline_text not supported for this field type"));
				}
			}

			// 3D widget
			sol::optional<bool> WidgetOpt = P.get<sol::optional<bool>>("3d_widget");
			if (WidgetOpt.has_value())
			{
				if (FStructureEditorUtils::CanEnable3dWidget(Struct, VarGuid))
				{
					FStructureEditorUtils::Change3dWidgetEnabled(Struct, VarGuid, WidgetOpt.value());
					Changes.Add(FString::Printf(TEXT("3d_widget=%s"), WidgetOpt.value() ? TEXT("true") : TEXT("false")));
				}
				else
				{
					Session.Log(TEXT("[WARN] 3d_widget not supported for this field type"));
				}
			}

			// Metadata (ClampMin, ClampMax, UIMin, UIMax, etc.)
			sol::optional<sol::table> MetadataOpt = P.get<sol::optional<sol::table>>("metadata");
			if (MetadataOpt.has_value())
			{
				sol::table MD = MetadataOpt.value();
				for (auto& KV : MD)
				{
					if (!KV.first.is<std::string>() || !KV.second.is<std::string>()) continue;
					FName Key = FName(UTF8_TO_TCHAR(KV.first.as<std::string>().c_str()));
					FString Value = UTF8_TO_TCHAR(KV.second.as<std::string>().c_str());
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
					FStructureEditorUtils::SetMetaData(Struct, VarGuid, Key, Value);
					Changes.Add(FString::Printf(TEXT("meta:%s=%s"), *Key.ToString(), *Value));
#else
					Session.Log(FString::Printf(TEXT("[WARN] configure(\"field\") -> metadata editing requires UE 5.5+")));
#endif
				}
			}

			// Move field (reorder): move_before or move_after
			std::string MoveBefore = P.get_or<std::string>("move_before", "");
			std::string MoveAfter = P.get_or<std::string>("move_after", "");
			if (!MoveBefore.empty())
			{
				FStructVariableDescription* RelDesc = FindVarDescByDisplayName(Struct, UTF8_TO_TCHAR(MoveBefore.c_str()));
				if (RelDesc)
				{
					if (FStructureEditorUtils::MoveVariable(Struct, VarGuid, RelDesc->VarGuid, FStructureEditorUtils::PositionAbove))
					{
						Changes.Add(FString::Printf(TEXT("moved before '%s'"), UTF8_TO_TCHAR(MoveBefore.c_str())));
					}
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[WARN] move_before target '%s' not found"), UTF8_TO_TCHAR(MoveBefore.c_str())));
				}
			}
			else if (!MoveAfter.empty())
			{
				FStructVariableDescription* RelDesc = FindVarDescByDisplayName(Struct, UTF8_TO_TCHAR(MoveAfter.c_str()));
				if (RelDesc)
				{
					if (FStructureEditorUtils::MoveVariable(Struct, VarGuid, RelDesc->VarGuid, FStructureEditorUtils::PositionBelow))
					{
						Changes.Add(FString::Printf(TEXT("moved after '%s'"), UTF8_TO_TCHAR(MoveAfter.c_str())));
					}
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[WARN] move_after target '%s' not found"), UTF8_TO_TCHAR(MoveAfter.c_str())));
				}
			}

			if (Changes.Num() > 0)
			{
				Struct->GetPackage()->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"field\", \"%s\"): %s"), *FName_Display, *FString::Join(Changes, TEXT(", "))));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[OK] configure(\"field\", \"%s\") -> no changes"), *FName_Display));
			return sol::make_object(Lua, true);
		});

		// ---- info() ----
		AssetObj.set_function("info", [Struct, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();

			TArray<FStructVariableDescription>& VarDescs = FStructureEditorUtils::GetVarDesc(Struct);
			Result["num_fields"] = VarDescs.Num();
			Result["is_valid"] = Struct->Status == EUserDefinedStructureStatus::UDSS_UpToDate;
			FString StructTooltip = FStructureEditorUtils::GetTooltip(Struct);
			if (!StructTooltip.IsEmpty()) Result["description"] = TCHAR_TO_UTF8(*StructTooltip);

			sol::table Fields = Lua.create_table();
			for (int32 i = 0; i < VarDescs.Num(); i++)
			{
				FEdGraphPinType PT = VarDescs[i].ToPinType();
				sol::table F = Lua.create_table();
				F["name"] = TCHAR_TO_UTF8(*StripFriendlyNameSuffix(VarDescs[i].FriendlyName));
				F["type"] = TCHAR_TO_UTF8(*PinTypeToTypeName(PT));
				FString SubTypeName = PinTypeToSubTypeName(PT);
				if (!SubTypeName.IsEmpty()) F["sub_type"] = TCHAR_TO_UTF8(*SubTypeName);
				Fields[i + 1] = F;
			}
			Result["fields"] = Fields;

			Session.Log(FString::Printf(TEXT("[OK] info() -> %d fields"), VarDescs.Num()));
			return Result;
		});
	});

	// ===========================================================================
	// UserDefinedEnum
	// ===========================================================================
	Lua.set_function("_enrich_user_defined_enum", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UUserDefinedEnum* Enum = LoadObject<UUserDefinedEnum>(nullptr, *FPath);
		if (!Enum) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"Element types for add/remove/list/configure:\n"
			"  value — enum enumerator\n"
			"\n"
			"add(\"value\", {display_name=\"MyValue\"}):\n"
			"  Appends a new enumerator with given display name.\n"
			"\n"
			"remove(\"value\", name_or_index):\n"
			"  Removes by display name (string) or 0-based index (number).\n"
			"\n"
			"list(\"values\"):\n"
			"  Returns array of {index, display_name}.\n"
			"\n"
			"configure(\"value\", {index=0, display_name=\"NewName\", move_to=2}):\n"
			"  Changes the display name of the value at given index.\n"
			"  Can also find by name: {name=\"OldName\", display_name=\"NewName\"}\n"
			"  move_to: reorder to a new 0-based index position.\n"
			"\n"
			"configure(\"bitflags\", {enabled=true}):\n"
			"  Enable/disable bitflags mode on the enum.\n"
			"\n"
			"info():\n"
			"  Returns {num_values, values=[display_name], is_bitflags}.\n";

		// ---- add("value", params) ----
		AssetObj.set_function("add", [Enum, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (!FType.Equals(TEXT("value"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: value"), *FType));
				return sol::lua_nil;
			}

			if (!Params.has_value())
			{
				Session.Log(TEXT("[FAIL] add(\"value\") -> params required: {display_name=\"...\"}"));
				return sol::lua_nil;
			}

			sol::table P = Params.value();
			std::string DisplayName = P.get_or<std::string>("display_name", "");
			if (DisplayName.empty()) DisplayName = P.get_or<std::string>("name", "");
			if (DisplayName.empty())
			{
				Session.Log(TEXT("[FAIL] add(\"value\") -> display_name required"));
				return sol::lua_nil;
			}

			FScopedTransaction Transaction(FText::FromString(TEXT("Add Enum Value")));

			int32 NumBefore = Enum->NumEnums();
			FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);

			if (Enum->NumEnums() <= NumBefore)
			{
				Session.Log(TEXT("[FAIL] add(\"value\") -> AddNewEnumerator failed"));
				return sol::lua_nil;
			}

			int32 NewIndex = Enum->NumEnums() - 2; // -1 for _MAX, -1 for zero-based
			if (NewIndex >= 0)
			{
				FString FDisplayName = UTF8_TO_TCHAR(DisplayName.c_str());
				FEnumEditorUtils::SetEnumeratorDisplayName(Enum, NewIndex, FText::FromString(FDisplayName));
				Enum->GetPackage()->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"value\", \"%s\") -> index %d"), *FDisplayName, NewIndex));
				return sol::make_object(Lua, NewIndex);
			}

			Session.Log(TEXT("[FAIL] add(\"value\") -> invalid index after add"));
			return sol::lua_nil;
		});

		// ---- remove("value", name_or_index) ----
		AssetObj.set_function("remove", [Enum, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (!FType.Equals(TEXT("value"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: value"), *FType));
				return sol::lua_nil;
			}

			int32 TargetIndex = -1;

			if (Id.is<std::string>())
			{
				// Find by display name
				FString SearchName = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
				for (int32 i = 0; i < Enum->NumEnums() - 1; i++)
				{
					FString DN = Enum->GetDisplayNameTextByIndex(i).ToString();
					if (DN.Equals(SearchName, ESearchCase::IgnoreCase))
					{
						TargetIndex = i;
						break;
					}
				}
				if (TargetIndex < 0)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"value\", \"%s\") -> not found"), *SearchName));
					return sol::lua_nil;
				}
			}
			else if (Id.is<int>())
			{
				TargetIndex = Id.as<int>();
			}
			else
			{
				Session.Log(TEXT("[FAIL] remove(\"value\") -> name (string) or index (number) required"));
				return sol::lua_nil;
			}

			if (TargetIndex < 0 || TargetIndex >= Enum->NumEnums() - 1)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"value\") -> index %d out of range"), TargetIndex));
				return sol::lua_nil;
			}

			FScopedTransaction Transaction(FText::FromString(TEXT("Remove Enum Value")));
			FString RemovedName = Enum->GetDisplayNameTextByIndex(TargetIndex).ToString();
			int32 NumBefore = Enum->NumEnums();
			FEnumEditorUtils::RemoveEnumeratorFromUserDefinedEnum(Enum, TargetIndex);

			bool bOk = Enum->NumEnums() < NumBefore;
			if (bOk) Enum->GetPackage()->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[%s] remove(\"value\", \"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *RemovedName));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		// ---- list("values") ----
		AssetObj.set_function("list", [Enum, &Session](sol::table self,
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

			if (!FType.Equals(TEXT("values"), ESearchCase::IgnoreCase) && !FType.Equals(TEXT("value"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: values"), *FType));
				return sol::lua_nil;
			}

			int32 Count = Enum->NumEnums() - 1; // exclude _MAX
			sol::table Result = Lua.create_table();
			for (int32 i = 0; i < Count; i++)
			{
				sol::table E = Lua.create_table();
				E["index"] = i;
				E["display_name"] = TCHAR_TO_UTF8(*Enum->GetDisplayNameTextByIndex(i).ToString());
				Result[i + 1] = E;
			}

			Session.Log(FString::Printf(TEXT("[OK] list(\"values\") -> %d"), Count));
			return Result;
		});

		// ---- configure("value" or "bitflags", params) ----
		AssetObj.set_function("configure", [Enum, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			// configure("bitflags", {enabled=true/false})
			if (FType.Equals(TEXT("bitflags"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"bitflags\") -> params required: {enabled=true/false}"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();
				sol::optional<bool> EnabledOpt = P.get<sol::optional<bool>>("enabled");
				if (!EnabledOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"bitflags\") -> enabled (bool) required"));
					return sol::lua_nil;
				}
				FScopedTransaction Transaction(FText::FromString(TEXT("Configure Enum Bitflags")));
				FEnumEditorUtils::SetEnumeratorBitflagsTypeState(Enum, EnabledOpt.value());
				Enum->GetPackage()->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"bitflags\") -> enabled=%s"), EnabledOpt.value() ? TEXT("true") : TEXT("false")));
				return sol::make_object(Lua, true);
			}

			if (!FType.Equals(TEXT("value"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: value, bitflags"), *FType));
				return sol::lua_nil;
			}

			if (!Params.has_value())
			{
				Session.Log(TEXT("[FAIL] configure(\"value\") -> params required: {index=.., display_name=.., move_to=..}"));
				return sol::lua_nil;
			}

			sol::table P = Params.value();
			sol::optional<int> IndexOpt = P.get<sol::optional<int>>("index");
			int32 TargetIndex = IndexOpt.has_value() ? IndexOpt.value() : -1;

			// If no index, try finding by name
			if (TargetIndex < 0)
			{
				std::string SearchName = P.get_or<std::string>("name", "");
				if (!SearchName.empty())
				{
					FString FSearchName = UTF8_TO_TCHAR(SearchName.c_str());
					for (int32 i = 0; i < Enum->NumEnums() - 1; i++)
					{
						if (Enum->GetDisplayNameTextByIndex(i).ToString().Equals(FSearchName, ESearchCase::IgnoreCase))
						{
							TargetIndex = i;
							break;
						}
					}
				}
			}

			if (TargetIndex < 0 || TargetIndex >= Enum->NumEnums() - 1)
			{
				Session.Log(TEXT("[FAIL] configure(\"value\") -> could not find value (provide index or name)"));
				return sol::lua_nil;
			}

			FScopedTransaction Transaction(FText::FromString(TEXT("Configure Enum Value")));
			TArray<FString> Changes;

			// Rename display name
			std::string NewDisplayName = P.get_or<std::string>("display_name", "");
			if (!NewDisplayName.empty())
			{
				FString FNewDN = UTF8_TO_TCHAR(NewDisplayName.c_str());
				FEnumEditorUtils::SetEnumeratorDisplayName(Enum, TargetIndex, FText::FromString(FNewDN));
				Changes.Add(FString::Printf(TEXT("display_name=\"%s\""), *FNewDN));
			}

			// Reorder: move_to (0-based target index)
			sol::optional<int> MoveToOpt = P.get<sol::optional<int>>("move_to");
			if (MoveToOpt.has_value())
			{
				int32 MoveTarget = MoveToOpt.value();
				int32 MaxIdx = Enum->NumEnums() - 2; // exclude _MAX
				if (MoveTarget >= 0 && MoveTarget <= MaxIdx && MoveTarget != TargetIndex)
				{
					FEnumEditorUtils::MoveEnumeratorInUserDefinedEnum(Enum, TargetIndex, MoveTarget);
					Changes.Add(FString::Printf(TEXT("moved to index %d"), MoveTarget));
				}
				else if (MoveTarget == TargetIndex)
				{
					// No-op, already at target
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[WARN] move_to %d out of range [0..%d]"), MoveTarget, MaxIdx));
				}
			}

			if (Changes.Num() == 0)
			{
				Session.Log(FString::Printf(TEXT("[OK] configure(\"value\", index=%d) -> no changes (provide display_name or move_to)"), TargetIndex));
				return sol::make_object(Lua, true);
			}

			Enum->GetPackage()->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] configure(\"value\", index=%d): %s"), TargetIndex, *FString::Join(Changes, TEXT(", "))));
			return sol::make_object(Lua, true);
		});

		// ---- info() ----
		AssetObj.set_function("info", [Enum, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();

			int32 Count = Enum->NumEnums() - 1; // exclude _MAX
			Result["num_values"] = Count;
			Result["is_bitflags"] = FEnumEditorUtils::IsEnumeratorBitflagsType(Enum);

			sol::table Values = Lua.create_table();
			for (int32 i = 0; i < Count; i++)
			{
				Values[i + 1] = TCHAR_TO_UTF8(*Enum->GetDisplayNameTextByIndex(i).ToString());
			}
			Result["values"] = Values;

			Session.Log(FString::Printf(TEXT("[OK] info() -> %d values"), Count));
			return Result;
		});
	});

	// ===========================================================================
	// DataTable
	// ===========================================================================
	Lua.set_function("_enrich_data_table", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UDataTable* DataTable = LoadObject<UDataTable>(nullptr, *FPath);
		if (!DataTable) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"Element types for add/remove/list/configure:\n"
			"  row — DataTable row\n"
			"\n"
			"add(\"row\", {row_name=.., values={Column1=\"val\", Column2=42}}):\n"
			"  Adds a new row. Column values can be strings, numbers, or booleans.\n"
			"\n"
			"remove(\"row\", row_name):\n"
			"  Removes a row by name.\n"
			"\n"
			"list(\"rows\"):\n"
			"  Returns {columns=[..], rows=[{row_name, values={Col=val}}]}.\n"
			"\n"
			"configure(\"row\", {row_name=.., values={Column=\"val\", Column2=42}}):\n"
			"  Modifies column values in an existing row. Values can be strings, numbers, or booleans.\n"
			"\n"
			"import_json(json_string):\n"
			"  Bulk import from JSON, REPLACING all existing rows.\n"
			"\n"
			"export_json():\n"
			"  Returns table as JSON string.\n"
			"\n"
			"info():\n"
			"  Returns {row_struct, num_rows, columns=[{name,type}]}.\n";

		// ---- add("row", params) ----
		AssetObj.set_function("add", [DataTable, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (!FType.Equals(TEXT("row"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: row"), *FType));
				return sol::lua_nil;
			}

			if (!Params.has_value())
			{
				Session.Log(TEXT("[FAIL] add(\"row\") -> params required: {row_name=.., values={..}}"));
				return sol::lua_nil;
			}

			UScriptStruct* RowStruct = DataTable->RowStruct;
			if (!RowStruct)
			{
				Session.Log(TEXT("[FAIL] add(\"row\") -> DataTable has no RowStruct"));
				return sol::lua_nil;
			}

			sol::table P = Params.value();
			std::string RowNameStr = P.get_or<std::string>("row_name", "");
			if (RowNameStr.empty())
			{
				Session.Log(TEXT("[FAIL] add(\"row\") -> row_name required"));
				return sol::lua_nil;
			}

			FName RowName = FName(UTF8_TO_TCHAR(RowNameStr.c_str()));

			if (DataTable->FindRowUnchecked(RowName))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add(\"row\") -> row '%s' already exists"), *RowName.ToString()));
				return sol::lua_nil;
			}

			FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Add DataTable Row: %s"), *RowName.ToString())));

			// Allocate + init row memory
			uint8* NewRowData = (uint8*)FMemory::Malloc(RowStruct->GetStructureSize());
			RowStruct->InitializeStruct(NewRowData);

			// Set values before adding (so AddRow deep-copies populated data)
			sol::optional<sol::table> ValuesOpt = P.get<sol::optional<sol::table>>("values");
			if (ValuesOpt.has_value())
			{
				sol::table Values = ValuesOpt.value();
				for (auto& KV : Values)
				{
					if (!KV.first.is<std::string>()) continue;
					FString ColName = UTF8_TO_TCHAR(KV.first.as<std::string>().c_str());
					// Accept both string and numeric values — convert to string for ImportText
					FString ColValue;
					if (KV.second.is<std::string>())
						ColValue = UTF8_TO_TCHAR(KV.second.as<std::string>().c_str());
					else if (KV.second.is<double>())
						ColValue = FString::SanitizeFloat(KV.second.as<double>());
					else if (KV.second.is<int>())
						ColValue = FString::FromInt(KV.second.as<int>());
					else if (KV.second.is<bool>())
						ColValue = KV.second.as<bool>() ? TEXT("true") : TEXT("false");
					else
						continue;

					FProperty* Prop = RowStruct->FindPropertyByName(FName(*ColName));
					if (Prop)
					{
						void* PropAddr = Prop->ContainerPtrToValuePtr<void>(NewRowData);
						Prop->ImportText_Direct(*ColValue, PropAddr, nullptr, PPF_None);
					}
				}
			}

			// Use typed overload for type safety
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			DataTable->AddRow(RowName, NewRowData, RowStruct);
#else
			DataTable->AddRow(RowName, *(FTableRowBase*)NewRowData);
#endif

			// DestroyStruct BEFORE Free — AddRow deep-copies so original memory is safe to release
			RowStruct->DestroyStruct(NewRowData);
			FMemory::Free(NewRowData);

			// AddRow internally uses DATATABLE_CHANGE_SCOPE which calls HandleDataTableChanged
			DataTable->GetPackage()->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] add(\"row\", \"%s\")"), *RowName.ToString()));
			return sol::make_object(Lua, true);
		});

		// ---- remove("row", name) ----
		AssetObj.set_function("remove", [DataTable, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (!FType.Equals(TEXT("row"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: row"), *FType));
				return sol::lua_nil;
			}

			if (!Id.is<std::string>())
			{
				Session.Log(TEXT("[FAIL] remove(\"row\") -> row_name string required"));
				return sol::lua_nil;
			}

			FName RowName = FName(UTF8_TO_TCHAR(Id.as<std::string>().c_str()));

			if (!DataTable->FindRowUnchecked(RowName))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"row\", \"%s\") -> not found"), *RowName.ToString()));
				return sol::lua_nil;
			}

			FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Remove DataTable Row: %s"), *RowName.ToString())));
			DataTable->RemoveRow(RowName);
			// RemoveRow internally uses DATATABLE_CHANGE_SCOPE which calls HandleDataTableChanged
			DataTable->GetPackage()->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] remove(\"row\", \"%s\")"), *RowName.ToString()));
			return sol::make_object(Lua, true);
		});

		// ---- list("rows") ----
		AssetObj.set_function("list", [DataTable, &Session](sol::table self,
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

			if (!FType.Equals(TEXT("rows"), ESearchCase::IgnoreCase) && !FType.Equals(TEXT("row"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: rows"), *FType));
				return sol::lua_nil;
			}

			UScriptStruct* RowStruct = DataTable->RowStruct;
			TArray<FName> RowNames = DataTable->GetRowNames();
			sol::table Result = Lua.create_table();

			// Include column names
			sol::table Columns = Lua.create_table();
			if (RowStruct)
			{
				int32 ColIdx = 1;
				for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
				{
					Columns[ColIdx++] = TCHAR_TO_UTF8(*It->GetName());
				}
			}
			Result["columns"] = Columns;

			sol::table Rows = Lua.create_table();
			for (int32 i = 0; i < RowNames.Num(); i++)
			{
				sol::table R = Lua.create_table();
				R["row_name"] = TCHAR_TO_UTF8(*RowNames[i].ToString());

				// Export row values
				uint8* RowData = DataTable->FindRowUnchecked(RowNames[i]);
				if (RowData && RowStruct)
				{
					sol::table ValuesTable = Lua.create_table();
					for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
					{
						FString ValueStr;
						const void* PropAddr = It->ContainerPtrToValuePtr<void>(RowData);
						It->ExportTextItem_Direct(ValueStr, PropAddr, nullptr, nullptr, PPF_None);
						ValuesTable[TCHAR_TO_UTF8(*It->GetName())] = TCHAR_TO_UTF8(*ValueStr);
					}
					R["values"] = ValuesTable;
				}

				Rows[i + 1] = R;
			}
			Result["rows"] = Rows;

			Session.Log(FString::Printf(TEXT("[OK] list(\"rows\") -> %d"), RowNames.Num()));
			return Result;
		});

		// ---- configure("row", params) ----
		AssetObj.set_function("configure", [DataTable, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (!FType.Equals(TEXT("row"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: row"), *FType));
				return sol::lua_nil;
			}

			if (!Params.has_value())
			{
				Session.Log(TEXT("[FAIL] configure(\"row\") -> params required: {row_name=.., values={..}}"));
				return sol::lua_nil;
			}

			UScriptStruct* RowStruct = DataTable->RowStruct;
			if (!RowStruct)
			{
				Session.Log(TEXT("[FAIL] configure(\"row\") -> DataTable has no RowStruct"));
				return sol::lua_nil;
			}

			sol::table P = Params.value();
			std::string RowNameStr = P.get_or<std::string>("row_name", "");
			if (RowNameStr.empty())
			{
				Session.Log(TEXT("[FAIL] configure(\"row\") -> row_name required"));
				return sol::lua_nil;
			}

			FName RowName = FName(UTF8_TO_TCHAR(RowNameStr.c_str()));
			uint8* RowData = DataTable->FindRowUnchecked(RowName);
			if (!RowData)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure(\"row\", \"%s\") -> not found"), *RowName.ToString()));
				return sol::lua_nil;
			}

			sol::optional<sol::table> ValuesOpt = P.get<sol::optional<sol::table>>("values");
			if (!ValuesOpt.has_value())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure(\"row\", \"%s\") -> values table required"), *RowName.ToString()));
				return sol::lua_nil;
			}

			FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Configure DataTable Row: %s"), *RowName.ToString())));
			DataTable->Modify();

			sol::table Values = ValuesOpt.value();
			TArray<FString> Modified;
			for (auto& KV : Values)
			{
				if (!KV.first.is<std::string>()) continue;
				FString ColName = UTF8_TO_TCHAR(KV.first.as<std::string>().c_str());
				// Accept both string and numeric values — convert to string for ImportText
				FString ColValue;
				if (KV.second.is<std::string>())
					ColValue = UTF8_TO_TCHAR(KV.second.as<std::string>().c_str());
				else if (KV.second.is<double>())
					ColValue = FString::SanitizeFloat(KV.second.as<double>());
				else if (KV.second.is<int>())
					ColValue = FString::FromInt(KV.second.as<int>());
				else if (KV.second.is<bool>())
					ColValue = KV.second.as<bool>() ? TEXT("true") : TEXT("false");
				else
					continue;

				FProperty* Prop = RowStruct->FindPropertyByName(FName(*ColName));
				if (Prop)
				{
					void* PropAddr = Prop->ContainerPtrToValuePtr<void>(RowData);
					Prop->ImportText_Direct(*ColValue, PropAddr, nullptr, PPF_None);
					Modified.Add(FString::Printf(TEXT("%s=%s"), *ColName, *ColValue));
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[WARN] column '%s' not found in RowStruct"), *ColName));
				}
			}

			DataTable->HandleDataTableChanged(RowName);
			DataTable->GetPackage()->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] configure(\"row\", \"%s\"): %s"), *RowName.ToString(), *FString::Join(Modified, TEXT(", "))));
			return sol::make_object(Lua, true);
		});

		// ---- import_json(json_string) ----
		AssetObj.set_function("import_json", [DataTable, &Session](sol::table /*self*/,
			const std::string& JsonStr, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FJson = UTF8_TO_TCHAR(JsonStr.c_str());

			if (FJson.IsEmpty())
			{
				Session.Log(TEXT("[FAIL] import_json() -> empty JSON string"));
				return sol::lua_nil;
			}

			FScopedTransaction Transaction(FText::FromString(TEXT("Import DataTable JSON")));
			TArray<FString> Problems = DataTable->CreateTableFromJSONString(FJson);

			int32 RowCount = DataTable->GetRowNames().Num();
			DataTable->GetPackage()->MarkPackageDirty();

			if (Problems.Num() > 0)
			{
				for (const FString& Prob : Problems)
				{
					Session.Log(FString::Printf(TEXT("[WARN] %s"), *Prob));
				}
			}

			Session.Log(FString::Printf(TEXT("[OK] import_json() -> %d rows imported (replaced all)"), RowCount));

			sol::table Result = Lua.create_table();
			Result["rows_imported"] = RowCount;
			Result["warnings"] = static_cast<int>(Problems.Num());
			return Result;
		});

		// ---- export_json() ----
		AssetObj.set_function("export_json", [DataTable, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString JSON = DataTable->GetTableAsJSON();
			Session.Log(FString::Printf(TEXT("[OK] export_json() -> %d chars"), JSON.Len()));
			return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*JSON)));
		});

		// ---- info() ----
		AssetObj.set_function("info", [DataTable, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();

			Result["row_struct"] = DataTable->RowStruct
				? TCHAR_TO_UTF8(*DataTable->RowStruct->GetName())
				: "none";
			Result["num_rows"] = static_cast<int>(DataTable->GetRowNames().Num());

			sol::table Columns = Lua.create_table();
			if (DataTable->RowStruct)
			{
				int32 ColIdx = 1;
				for (TFieldIterator<FProperty> It(DataTable->RowStruct); It; ++It)
				{
					sol::table Col = Lua.create_table();
					Col["name"] = TCHAR_TO_UTF8(*It->GetName());
					Col["type"] = TCHAR_TO_UTF8(*GetPropertyTypeName(*It));
					Columns[ColIdx++] = Col;
				}
			}
			Result["columns"] = Columns;

			Session.Log(FString::Printf(TEXT("[OK] info() -> %s, %d rows"),
				DataTable->RowStruct ? *DataTable->RowStruct->GetName() : TEXT("no struct"),
				DataTable->GetRowNames().Num()));
			return Result;
		});
	});

	// ===========================================================================
	// StringTable
	// ===========================================================================
	Lua.set_function("_enrich_string_table", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UStringTable* StringTableAsset = LoadObject<UStringTable>(nullptr, *FPath);
		if (!StringTableAsset) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"Element types for add/remove/list/configure:\n"
			"  entry — key/value string pair\n"
			"\n"
			"add(\"entry\", {key=\"KEY\", value=\"Localized text\"}):\n"
			"  Adds a new string entry.\n"
			"\n"
			"remove(\"entry\", key):\n"
			"  Removes an entry by key.\n"
			"\n"
			"list(\"entries\"):\n"
			"  Returns array of {key, value}.\n"
			"\n"
			"configure(\"entry\", {key=.., value=..}):\n"
			"  Updates the value for an existing key.\n"
			"\n"
			"configure(\"namespace\", {namespace=\"MyNamespace\"}):\n"
			"  Sets the string table namespace.\n"
			"\n"
			"info():\n"
			"  Returns {namespace, num_entries}.\n";

		// ---- add("entry", params) ----
		AssetObj.set_function("add", [StringTableAsset, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (!FType.Equals(TEXT("entry"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: entry"), *FType));
				return sol::lua_nil;
			}

			if (!Params.has_value())
			{
				Session.Log(TEXT("[FAIL] add(\"entry\") -> params required: {key=.., value=..}"));
				return sol::lua_nil;
			}

			sol::table P = Params.value();
			std::string Key = P.get_or<std::string>("key", "");
			std::string Value = P.get_or<std::string>("value", "");
			if (Key.empty())
			{
				Session.Log(TEXT("[FAIL] add(\"entry\") -> key required"));
				return sol::lua_nil;
			}

			FStringTableRef MutableTable = StringTableAsset->GetMutableStringTable();
			FString FKey = UTF8_TO_TCHAR(Key.c_str());
			FString FValue = UTF8_TO_TCHAR(Value.c_str());

			FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Add StringTable Entry: %s"), *FKey)));
			MutableTable->SetSourceString(FKey, FValue);
			StringTableAsset->GetPackage()->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] add(\"entry\", key=\"%s\")"), *FKey));
			return sol::make_object(Lua, true);
		});

		// ---- remove("entry", key) ----
		AssetObj.set_function("remove", [StringTableAsset, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (!FType.Equals(TEXT("entry"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: entry"), *FType));
				return sol::lua_nil;
			}

			if (!Id.is<std::string>())
			{
				Session.Log(TEXT("[FAIL] remove(\"entry\") -> key string required"));
				return sol::lua_nil;
			}

			FString FKey = UTF8_TO_TCHAR(Id.as<std::string>().c_str());
			FStringTableRef MutableTable = StringTableAsset->GetMutableStringTable();

			// Check key exists before removing
			FString ExistingValue;
			if (!MutableTable->GetSourceString(FKey, ExistingValue))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"entry\", \"%s\") -> key not found"), *FKey));
				return sol::lua_nil;
			}

			FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Remove StringTable Entry: %s"), *FKey)));
			MutableTable->RemoveSourceString(FKey);
			StringTableAsset->GetPackage()->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] remove(\"entry\", \"%s\")"), *FKey));
			return sol::make_object(Lua, true);
		});

		// ---- list("entries") ----
		AssetObj.set_function("list", [StringTableAsset, &Session](sol::table self,
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

			if (!FType.Equals(TEXT("entries"), ESearchCase::IgnoreCase) && !FType.Equals(TEXT("entry"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: entries"), *FType));
				return sol::lua_nil;
			}

			FStringTableConstRef Table = StringTableAsset->GetStringTable();
			sol::table Result = Lua.create_table();
			int32 Idx = 1;

			Table->EnumerateSourceStrings([&](const FString& InKey, const FString& InSourceString) -> bool
			{
				sol::table E = Lua.create_table();
				E["key"] = TCHAR_TO_UTF8(*InKey);
				E["value"] = TCHAR_TO_UTF8(*InSourceString);
				Result[Idx++] = E;
				return true; // continue
			});

			Session.Log(FString::Printf(TEXT("[OK] list(\"entries\") -> %d"), Idx - 1));
			return Result;
		});

		// ---- configure("entry" or "namespace", params) ----
		AssetObj.set_function("configure", [StringTableAsset, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("namespace"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"namespace\") -> params required: {namespace=..}"));
					return sol::lua_nil;
				}

				sol::table P = Params.value();
				std::string NS = P.get_or<std::string>("namespace", "");
				if (NS.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"namespace\") -> namespace value required"));
					return sol::lua_nil;
				}

				FString FNS = UTF8_TO_TCHAR(NS.c_str());
				FScopedTransaction Transaction(FText::FromString(TEXT("Set StringTable Namespace")));
				FStringTableRef MutableTable = StringTableAsset->GetMutableStringTable();
				MutableTable->SetNamespace(FNS);
				StringTableAsset->GetPackage()->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"namespace\") -> \"%s\""), *FNS));
				return sol::make_object(Lua, true);
			}

			if (FType.Equals(TEXT("entry"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"entry\") -> params required: {key=.., value=..}"));
					return sol::lua_nil;
				}

				sol::table P = Params.value();
				std::string Key = P.get_or<std::string>("key", "");
				std::string Value = P.get_or<std::string>("value", "");
				if (Key.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"entry\") -> key required"));
					return sol::lua_nil;
				}

				FString FKey = UTF8_TO_TCHAR(Key.c_str());
				FString FValue = UTF8_TO_TCHAR(Value.c_str());

				// Verify key exists — configure should only update, not create
				FStringTableRef MutableTable = StringTableAsset->GetMutableStringTable();
				FString ExistingValue;
				if (!MutableTable->GetSourceString(FKey, ExistingValue))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"entry\", key=\"%s\") -> key not found (use add to create)"), *FKey));
					return sol::lua_nil;
				}

				FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Configure StringTable Entry: %s"), *FKey)));
				MutableTable->SetSourceString(FKey, FValue);
				StringTableAsset->GetPackage()->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"entry\", key=\"%s\")"), *FKey));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: entry, namespace"), *FType));
			return sol::lua_nil;
		});

		// ---- info() ----
		AssetObj.set_function("info", [StringTableAsset, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();

			FStringTableConstRef Table = StringTableAsset->GetStringTable();
			FString NS = Table->GetNamespace();
			Result["namespace"] = TCHAR_TO_UTF8(NS.IsEmpty() ? TEXT("") : *NS);

			int32 Count = 0;
			Table->EnumerateSourceStrings([&](const FString& /*InKey*/, const FString& /*InSourceString*/) -> bool
			{
				Count++;
				return true;
			});
			Result["num_entries"] = Count;

			Session.Log(FString::Printf(TEXT("[OK] info() -> %d entries"), Count));
			return Result;
		});
	});
}

REGISTER_LUA_BINDING(DataStructure, DataStructureDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindDataStructure(Lua, Session);
});
