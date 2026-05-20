// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Blueprint/BlueprintUtils.h"
#include "Tools/NeoStackToolUtils.h"

#include "Modules/ModuleManager.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyAccessUtil.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "WidgetBlueprint.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/PoseAsset.h"
#include "Animation/Skeleton.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Editor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "IMaterialEditor.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTNode.h"
#include "LevelSequence.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
#include "StructUtils/UserDefinedStruct.h"
#else
#include "Engine/UserDefinedStruct.h"
#endif
#include "Engine/UserDefinedEnum.h"
#include "Engine/DataTable.h"
#include "GameplayTagContainer.h"
#include "Internationalization/StringTable.h"
#include "Engine/CurveTable.h"
#include "Curves/CurveBase.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundWave.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundMix.h"
#include "VT/RuntimeVirtualTexture.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialParameterCollection.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Interfaces/Interface_AssetUserData.h"
#include "Engine/AssetUserData.h"
#include "FoliageType.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "FoliageType_Actor.h"
#include "LandscapeGrassType.h"
#include "Sound/DialogueVoice.h"
#include "Sound/DialogueWave.h"
// PoseSearch headers removed — binding lives in AIK_PoseSearch extension module.
// Type detection uses class name strings (no link dependency needed).

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// Helpers — property find, get, set, with all the ConfigureAssetTool smarts
// ============================================================================

namespace LuaAssetHelper
{

static FProperty* FindProperty(UObject* Asset, const FString& PropertyName)
{
	if (!Asset) return nullptr;

	// Exact match first
	FProperty* Prop = PropertyAccessUtil::FindPropertyByName(FName(*PropertyName), Asset->GetClass());
	if (Prop) return Prop;

	// Case-insensitive fallback
	for (TFieldIterator<FProperty> PropIt(Asset->GetClass()); PropIt; ++PropIt)
	{
		if (PropIt->GetName().Equals(PropertyName, ESearchCase::IgnoreCase))
		{
			return *PropIt;
		}
	}
	return nullptr;
}

// Resolve dot-notation paths like "Axis[0].Min" or "MyMap{key}.SubField" into (Property, ContainerPtr)
struct FResolvedProperty
{
	FProperty* Property = nullptr;
	void* ContainerPtr = nullptr;
};

static FResolvedProperty ResolvePropertyPath(UObject* Object, const FString& PropertyPath)
{
	FResolvedProperty Result;
	if (!Object || PropertyPath.IsEmpty()) return Result;

	// No dots and no brackets — flat lookup
	if (!PropertyPath.Contains(TEXT(".")) && !PropertyPath.Contains(TEXT("[")))
	{
		Result.Property = FindProperty(Object, PropertyPath);
		Result.ContainerPtr = Object;
		return Result;
	}

	TArray<FString> Segments;
	PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
	if (Segments.Num() == 0 || Segments.Num() > 10) return Result;

	UStruct* CurrentStruct = Object->GetClass();
	void* CurrentContainer = Object;

	for (int32 i = 0; i < Segments.Num(); i++)
	{
		FString Segment = Segments[i];
		bool bIsLast = (i == Segments.Num() - 1);

		// Parse optional array index: "PropertyName[N]"
		int32 ArrayIndex = INDEX_NONE;
		int32 BracketPos = INDEX_NONE;
		// Parse optional map key: "PropertyName{key}"
		FString MapKeyStr;
		int32 BracePos = INDEX_NONE;

		if (Segment.FindChar(TEXT('['), BracketPos))
		{
			FString IndexStr = Segment.Mid(BracketPos + 1);
			IndexStr.RemoveFromEnd(TEXT("]"));
			if (IndexStr.IsNumeric())
			{
				ArrayIndex = FCString::Atoi(*IndexStr);
			}
			else
			{
				return FResolvedProperty();
			}
			Segment = Segment.Left(BracketPos);
		}
		else if (Segment.FindChar(TEXT('{'), BracePos))
		{
			MapKeyStr = Segment.Mid(BracePos + 1);
			MapKeyStr.RemoveFromEnd(TEXT("}"));
			Segment = Segment.Left(BracePos);
		}

		// Find property in current struct
		FProperty* Prop = PropertyAccessUtil::FindPropertyByName(FName(*Segment), CurrentStruct);
		if (!Prop)
		{
			for (TFieldIterator<FProperty> PropIt(CurrentStruct); PropIt; ++PropIt)
			{
				if (PropIt->GetName().Equals(Segment, ESearchCase::IgnoreCase))
				{
					Prop = *PropIt;
					break;
				}
			}
		}
		if (!Prop) return FResolvedProperty();

		// Handle array indexing
		if (ArrayIndex != INDEX_NONE)
		{
			void* PropContainer = Prop->ContainerPtrToValuePtr<void>(CurrentContainer);
			FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
			if (!ArrayProp) return FResolvedProperty();

			FScriptArrayHelper ArrayHelper(ArrayProp, PropContainer);
			if (ArrayIndex < 0 || ArrayIndex >= ArrayHelper.Num()) return FResolvedProperty();

			void* ElementPtr = ArrayHelper.GetRawPtr(ArrayIndex);
			FProperty* InnerProp = ArrayProp->Inner;

			if (bIsLast)
			{
				Result.Property = InnerProp;
				Result.ContainerPtr = ElementPtr;
				return Result;
			}

			FStructProperty* InnerStruct = CastField<FStructProperty>(InnerProp);
			if (!InnerStruct) return FResolvedProperty();
			CurrentContainer = ElementPtr;
			CurrentStruct = InnerStruct->Struct;
			continue;
		}

		// Handle map key lookup: "MapProp{key}.SubField"
		if (!MapKeyStr.IsEmpty())
		{
			void* PropContainer = Prop->ContainerPtrToValuePtr<void>(CurrentContainer);
			FMapProperty* MapProp = CastField<FMapProperty>(Prop);
			if (!MapProp) return FResolvedProperty();

			FScriptMapHelper MapHelper(MapProp, PropContainer);
			FProperty* KeyProp = MapHelper.GetKeyProperty();
			FProperty* ValProp = MapHelper.GetValueProperty();

			// Allocate temp key and parse the string into it
			void* TempKey = FMemory::Malloc(KeyProp->GetSize(), KeyProp->GetMinAlignment());
			KeyProp->InitializeValue(TempKey);

			const TCHAR* ParseResult = KeyProp->ImportText_Direct(*MapKeyStr, TempKey, nullptr, PPF_None);
			if (!ParseResult)
			{
				KeyProp->DestroyValue(TempKey);
				FMemory::Free(TempKey);
				return FResolvedProperty();
			}

			uint8* ValuePtr = MapHelper.FindValueFromHash(TempKey);
			KeyProp->DestroyValue(TempKey);
			FMemory::Free(TempKey);

			if (!ValuePtr) return FResolvedProperty();

			if (bIsLast)
			{
				Result.Property = ValProp;
				Result.ContainerPtr = ValuePtr;
				return Result;
			}

			// Walk into the value if it's a struct
			FStructProperty* ValStruct = CastField<FStructProperty>(ValProp);
			if (!ValStruct) return FResolvedProperty();
			CurrentContainer = ValuePtr;
			CurrentStruct = ValStruct->Struct;
			continue;
		}

		if (bIsLast)
		{
			Result.Property = Prop;
			Result.ContainerPtr = CurrentContainer;
			return Result;
		}

		// Walk into struct
		FStructProperty* StructProp = CastField<FStructProperty>(Prop);
		if (!StructProp) return FResolvedProperty();
		CurrentContainer = StructProp->ContainerPtrToValuePtr<void>(CurrentContainer);
		CurrentStruct = StructProp->Struct;
	}

	return Result;
}

// ---- Map/Set/Metadata helpers ----

static FMapProperty* FindMapProperty(UObject* Asset, const FString& PropertyName)
{
	FProperty* Prop = FindProperty(Asset, PropertyName);
	if (!Prop) return nullptr;
	return CastField<FMapProperty>(Prop);
}

static FSetProperty* FindSetProperty(UObject* Asset, const FString& PropertyName)
{
	FProperty* Prop = FindProperty(Asset, PropertyName);
	if (!Prop) return nullptr;
	return CastField<FSetProperty>(Prop);
}

// Export a single property value from raw memory to string
static FString ExportPropertyValue(FProperty* Prop, const void* ValuePtr, UObject* OwnerObject)
{
	FString Out;
	Prop->ExportText_Direct(Out, ValuePtr, nullptr, OwnerObject, PPF_None);
	return Out;
}

// Check if a property holds a GameplayTag or GameplayTagContainer struct.
// These need PPF_SerializedAsImportText so that tags not yet in the registry are preserved.
static bool IsGameplayTagProperty(FProperty* Prop)
{
	FStructProperty* StructProp = CastField<FStructProperty>(Prop);
	if (!StructProp) return false;
	const FName StructName = StructProp->Struct->GetFName();
	return StructName == TEXT("GameplayTagContainer") || StructName == TEXT("GameplayTag");
}

// Import a string into a property value in raw memory. Returns true on success.
static bool ImportPropertyValue(FProperty* Prop, void* ValuePtr, const FString& ValueStr, UObject* OwnerObject)
{
	const int32 Flags = IsGameplayTagProperty(Prop) ? PPF_SerializedAsImportText : PPF_None;
	const TCHAR* Result = Prop->ImportText_Direct(*ValueStr, ValuePtr, OwnerObject, Flags);
	if (Result) return true;

	// Boolean fixup
	FString Transformed = ValueStr;
	if (ValueStr.Equals(TEXT("true"), ESearchCase::IgnoreCase)) Transformed = TEXT("True");
	else if (ValueStr.Equals(TEXT("false"), ESearchCase::IgnoreCase)) Transformed = TEXT("False");
	Result = Prop->ImportText_Direct(*Transformed, ValuePtr, OwnerObject, Flags);
	return Result != nullptr;
}

static FString GetPropertyValue(UObject* Asset, FProperty* Property, void* Container)
{
	return NeoStackToolUtils::GetPropertyValueAsString(Container, Property, Asset);
}

static bool SetPropertyValue(UObject* Asset, FProperty* Property, void* Container, const FString& Value, FString& OutError)
{
	if (!Asset || !Property)
	{
		OutError = TEXT("Invalid asset or property");
		return false;
	}

	const int32 Flags = IsGameplayTagProperty(Property) ? PPF_SerializedAsImportText : PPF_None;
	const TCHAR* Result = Property->ImportText_InContainer(*Value, Container, Asset, Flags);
	if (!Result)
	{
		// Boolean case fixup
		FString Transformed = Value;
		if (Value.Equals(TEXT("true"), ESearchCase::IgnoreCase)) Transformed = TEXT("True");
		else if (Value.Equals(TEXT("false"), ESearchCase::IgnoreCase)) Transformed = TEXT("False");

		Result = Property->ImportText_InContainer(*Transformed, Container, Asset, Flags);
		if (!Result)
		{
			OutError = FString::Printf(TEXT("Failed to set value '%s'. Use list_properties() to see valid format."), *Value);
			return false;
		}
	}
	return true;
}

// Asset-specific post-edit hooks (Material recompile, BlendSpace resample, etc.)
// Called AFTER PreEditChange/PostEditChangeProperty have already been done.
static void PostEdit(UObject* Asset, FProperty* /*ChangedProperty*/)
{
	if (!Asset) return;

	// Material: recompile shaders
	if (UMaterial* Mat = Cast<UMaterial>(Asset))
	{
		Mat->ForceRecompileForRendering();
		if (GEditor)
		{
			UAssetEditorSubsystem* Sub = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (Sub)
			{
				IAssetEditorInstance* Ed = Sub->FindEditorForAsset(Mat, false);
				if (Ed && Ed->GetEditorName() == TEXT("MaterialEditor"))
				{
					static_cast<IMaterialEditor*>(Ed)->MarkMaterialDirty();
				}
			}
		}
	}
	// Blueprint: recompile
	else if (UBlueprint* BP = Cast<UBlueprint>(Asset))
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	}
	// AnimMontage: rebuild cache
	else if (UAnimMontage* Montage = Cast<UAnimMontage>(Asset))
	{
		Montage->RefreshCacheData();
	}
	// BlendSpace: revalidate samples
	else if (UBlendSpace* BS = Cast<UBlendSpace>(Asset))
	{
		BS->ValidateSampleData();
		BS->ResampleData();
	}
	// BehaviorTree node: re-init from asset
	else if (UBTNode* BTNode = Cast<UBTNode>(Asset))
	{
		UBehaviorTree* OwnerTree = BTNode->GetTypedOuter<UBehaviorTree>();
		if (OwnerTree)
		{
			BTNode->InitializeFromAsset(*OwnerTree);
			OwnerTree->MarkPackageDirty();
		}
	}
}

static const char* GetPoseSearchEnrichmentFunction(UObject* Asset)
{
	const FString ClassName = Asset->GetClass()->GetName();
	if (ClassName == TEXT("PoseSearchSchema"))
	{
		return "_enrich_pose_search_schema";
	}
	else if (ClassName == TEXT("PoseSearchDatabase"))
	{
		return "_enrich_pose_search_database";
	}
	else if (ClassName == TEXT("PoseSearchNormalizationSet"))
	{
		return "_enrich_pose_search_normalization_set";
	}
	return nullptr;
}

// Check if asset is a PoseSearch type by class name (for when module is not loaded)
static bool IsPoseSearchAsset(UObject* Asset)
{
	FString ClassName = Asset->GetClass()->GetName();
	return ClassName == TEXT("PoseSearchSchema") || ClassName == TEXT("PoseSearchDatabase") || ClassName == TEXT("PoseSearchNormalizationSet");
}

// ---- AssetUserData helpers ----

static IInterface_AssetUserData* GetAssetUserDataInterface(UObject* Asset)
{
	return Cast<IInterface_AssetUserData>(Asset);
}

static FString GetUserDataClassName(UAssetUserData* UserData)
{
	if (!UserData) return TEXT("Unknown");
	FString ClassName = UserData->GetClass()->GetName();
	return ClassName;
}

static UClass* FindUserDataClass(const FString& ClassName)
{
	// Try exact match first (e.g. "UAnimationModifiersAssetUserData" or "AnimationModifiersAssetUserData")
	FString SearchName = ClassName;
	if (!SearchName.StartsWith(TEXT("U")))
	{
		SearchName = TEXT("U") + SearchName;
	}

	// Search loaded classes
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (Class->IsChildOf(UAssetUserData::StaticClass()) && !Class->HasAnyClassFlags(CLASS_Abstract))
		{
			if (Class->GetName().Equals(SearchName, ESearchCase::IgnoreCase) ||
				Class->GetName().Equals(ClassName, ESearchCase::IgnoreCase))
			{
				return Class;
			}
		}
	}

	// Try partial match (e.g. "AnimationModifiers" matches "UAnimationModifiersAssetUserData")
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (Class->IsChildOf(UAssetUserData::StaticClass()) && !Class->HasAnyClassFlags(CLASS_Abstract))
		{
			if (Class->GetName().Contains(ClassName, ESearchCase::IgnoreCase))
			{
				return Class;
			}
		}
	}

	return nullptr;
}

} // namespace LuaAssetHelper

// ============================================================================
// Lua Binding: open_asset
// ============================================================================

static TArray<FLuaFunctionDoc> OpenAssetDocs = {
	{ TEXT("open_asset(path)"), TEXT("Open any Unreal asset — returns handle with reflection + asset-specific methods; call help() to discover"), TEXT("asset table or nil") },
};

static void BindOpenAsset(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("open_asset", [&Session](const std::string& Path, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString FPath = UTF8_TO_TCHAR(Path.c_str());

		// Normalize path
		if (!FPath.StartsWith(TEXT("/")))
		{
			FPath = TEXT("/Game/") + FPath;
		}

		// Load the asset
		UObject* Asset = LoadObject<UObject>(nullptr, *FPath);

		// Fallback: some asset types (PCG, etc.) don't resolve from short package paths.
		// Use Asset Registry to find the full object path and retry.
		if (!Asset)
		{
			FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
			TArray<FAssetData> Assets;
			ARM.Get().GetAssetsByPackageName(FName(*FPath), Assets, true);
			if (Assets.Num() > 0)
			{
				Asset = Assets[0].GetAsset();
			}
		}

		if (!Asset)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] open_asset(\"%s\") -> asset not found"), *FPath));
			return sol::lua_nil;
		}

		// ControlRig blueprints use their own enrichment path — MUST skip the generic
		// _open_blueprint path because the Kismet compiler does CastChecked<UEdGraphSchema_K2>
		// on all BP graphs, which crashes on ControlRig's URigVMEdGraphSchema.
		{
			FString BPClassName = Asset->GetClass()->GetName();
			if (BPClassName == TEXT("ControlRigBlueprint") || BPClassName == TEXT("ControlRigBlueprintLegacy"))
			{
				sol::protected_function EnrichCR = LuaView["_enrich_control_rig"];
				if (EnrichCR.valid())
				{
					sol::table CRTable = LuaView.create_table();
					CRTable["path"] = TCHAR_TO_UTF8(*FPath);
					CRTable["type"] = TCHAR_TO_UTF8(*BPClassName);
					CRTable["name"] = TCHAR_TO_UTF8(*Asset->GetName());
					EnrichCR(CRTable);
					Session.Log(FString::Printf(TEXT("[OK] open_asset(\"%s\") -> ControlRig enrichment"), *FPath));
					return CRTable;
				}
			}
		}

		// For Blueprints, delegate to internal _open_blueprint for the richer API
		if (Asset->IsA<UBlueprint>())
		{
			sol::protected_function OpenBP = LuaView["_open_blueprint"];
			if (OpenBP.valid())
			{
				auto BPResult = OpenBP(Path);
				if (BPResult.valid())
				{
					// Enrich GE Blueprints with GameplayEffect-specific verbs
					if (UBlueprint* BP = Cast<UBlueprint>(Asset))
					{
						if (BP->GeneratedClass)
						{
							FString GenClassName = BP->GeneratedClass->GetName();
							// Check if this is a GameplayEffect BP by walking the class hierarchy
							UClass* WalkClass = BP->GeneratedClass;
							bool bIsGameplayEffect = false;
							while (WalkClass)
							{
								if (WalkClass->GetName() == TEXT("GameplayEffect"))
								{
									bIsGameplayEffect = true;
									break;
								}
								WalkClass = WalkClass->GetSuperClass();
							}
							if (bIsGameplayEffect)
							{
								sol::protected_function EnrichGE = LuaView["_enrich_gameplay_effect"];
								if (EnrichGE.valid()) EnrichGE(BPResult);
							}
						}
					}

					// Enrich Widget Blueprints with widget tree methods
					if (Asset->IsA<UWidgetBlueprint>())
					{
						sol::protected_function EnrichWB = LuaView["_enrich_widget_blueprint"];
						if (EnrichWB.valid()) EnrichWB(BPResult);
					}

					Session.Log(FString::Printf(TEXT("[OK] open_asset(\"%s\") -> delegated to open_blueprint (%s)"),
						*FPath, *Asset->GetClass()->GetName()));
					return BPResult;
				}
			}
		}

		FString AssetClassName = Asset->GetClass()->GetName();
		Session.Log(FString::Printf(TEXT("[OK] open_asset(\"%s\") -> %s"), *FPath, *AssetClassName));

		// Build the asset handle table
		sol::table AssetObj = LuaView.create_table();
		AssetObj["path"] = TCHAR_TO_UTF8(*FPath);
		AssetObj["type"] = TCHAR_TO_UTF8(*AssetClassName);
		AssetObj["class"] = TCHAR_TO_UTF8(*Asset->GetClass()->GetPathName());

		// GC-safe weak reference — all lambdas capture this instead of raw Asset*
		TWeakObjectPtr<UObject> WeakAsset(Asset);

		// ----------------------------------------------------------------
		// asset:get(property) -> string value
		// ----------------------------------------------------------------
		AssetObj.set_function("get", [WeakAsset, FPath, &Session](sol::table /*self*/,
			const std::string& PropertyName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = UTF8_TO_TCHAR(PropertyName.c_str());

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] get -> asset no longer valid"));
				return sol::lua_nil;
			}

			// Try dot-notation resolution
			LuaAssetHelper::FResolvedProperty Resolved = LuaAssetHelper::ResolvePropertyPath(Asset, FProp);
			if (!Resolved.Property)
			{
				FString Error = NeoBlueprint::FuzzyMatchProperty(Asset, FProp);
				Session.Log(FString::Printf(TEXT("[FAIL] get(\"%s\") -> %s"), *FProp, *Error));
				return sol::lua_nil;
			}

			FString Value = LuaAssetHelper::GetPropertyValue(Asset, Resolved.Property, Resolved.ContainerPtr);
			return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*Value)));
		});

		// ----------------------------------------------------------------
		// asset:set(property, value) -> true/nil
		// ----------------------------------------------------------------
		AssetObj.set_function("set", [WeakAsset, FPath, &Session](sol::table /*self*/,
			const std::string& PropertyName, const std::string& Value, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = UTF8_TO_TCHAR(PropertyName.c_str());
			FString FValue = UTF8_TO_TCHAR(Value.c_str());

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] set -> asset no longer valid"));
				return sol::lua_nil;
			}

			LuaAssetHelper::FResolvedProperty Resolved = LuaAssetHelper::ResolvePropertyPath(Asset, FProp);
			if (!Resolved.Property)
			{
				FString Error = NeoBlueprint::FuzzyMatchProperty(Asset, FProp);
				Session.Log(FString::Printf(TEXT("[FAIL] set(\"%s\") -> %s"), *FProp, *Error));
				return sol::lua_nil;
			}

			// Check writable
			EPropertyAccessResultFlags AccessResult = PropertyAccessUtil::CanSetPropertyValue(
				Resolved.Property, PropertyAccessUtil::EditorReadOnlyFlags,
				PropertyAccessUtil::IsObjectTemplate(Asset));
			if (EnumHasAnyFlags(AccessResult, EPropertyAccessResultFlags::PermissionDenied))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set(\"%s\") -> property is read-only"), *FProp));
				return sol::lua_nil;
			}

			FString OldValue = LuaAssetHelper::GetPropertyValue(Asset, Resolved.Property, Resolved.ContainerPtr);

			// Bracket the change with PreEditChange/PostEditChangeProperty
			Asset->Modify();
			Asset->PreEditChange(Resolved.Property);

			FString Error;
			if (!LuaAssetHelper::SetPropertyValue(Asset, Resolved.Property, Resolved.ContainerPtr, FValue, Error))
			{
				// Still need to close the bracket even on failure
				FPropertyChangedEvent FailEvent(Resolved.Property, EPropertyChangeType::ValueSet);
				Asset->PostEditChangeProperty(FailEvent);
				Session.Log(FString::Printf(TEXT("[FAIL] set(\"%s\", \"%s\") -> %s"), *FProp, *FValue, *Error));
				return sol::lua_nil;
			}

			Asset->MarkPackageDirty();
			FPropertyChangedEvent SuccessEvent(Resolved.Property, EPropertyChangeType::ValueSet);
			Asset->PostEditChangeProperty(SuccessEvent);

			// Asset-specific post-edit hooks (Material recompile, BlendSpace resample, etc.)
			LuaAssetHelper::PostEdit(Asset, nullptr);

			FString NewValue = LuaAssetHelper::GetPropertyValue(Asset, Resolved.Property, Resolved.ContainerPtr);
			Session.Log(FString::Printf(TEXT("[OK] set(\"%s\") = \"%s\" (was \"%s\")"),
				*FProp, *NewValue, *OldValue));
			return sol::make_object(Lua, true);
		});

		// ----------------------------------------------------------------
		// asset:list_properties(filter?, all?) -> table
		// ----------------------------------------------------------------
		AssetObj.set_function("list_properties", [WeakAsset, &Session](sol::table /*self*/,
			sol::optional<std::string> Filter, sol::optional<bool> IncludeAll,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] list_properties -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FFilter = Filter.has_value() ? UTF8_TO_TCHAR(Filter.value().c_str()) : TEXT("");
			bool bAll = IncludeAll.value_or(false);

			sol::table Result = Lua.create_table();
			int32 Index = 1;

			for (TFieldIterator<FProperty> PropIt(Asset->GetClass()); PropIt; ++PropIt)
			{
				FProperty* Property = *PropIt;
				if (Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient)) continue;
				if (!bAll && !Property->HasAnyPropertyFlags(CPF_Edit)) continue;

				FString Name = Property->GetName();
				if (!FFilter.IsEmpty() && !Name.Contains(FFilter, ESearchCase::IgnoreCase)) continue;

				FString Type = NeoStackToolUtils::GetPropertyTypeName(Property);
				FString Value = NeoStackToolUtils::GetPropertyValueAsString(Asset, Property, Asset);
				FString Category = Property->GetMetaData(TEXT("Category"));
				if (Category.IsEmpty()) Category = TEXT("Default");

				// Truncate long values for readability
				if (Value.Len() > 120) Value = Value.Left(117) + TEXT("...");

				sol::table Entry = Lua.create_table();
				Entry["name"] = TCHAR_TO_UTF8(*Name);
				Entry["type"] = TCHAR_TO_UTF8(*Type);
				Entry["value"] = TCHAR_TO_UTF8(*Value);
				Entry["category"] = TCHAR_TO_UTF8(*Category);
				Result[Index++] = Entry;
			}

			Session.Log(FString::Printf(TEXT("[OK] list_properties(%s) -> %d properties"),
				FFilter.IsEmpty() ? TEXT("*") : *FFilter, Index - 1));
			return Result;
		});

		// ----------------------------------------------------------------
		// asset:array_count(property) -> integer
		// ----------------------------------------------------------------
		AssetObj.set_function("array_count", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = UTF8_TO_TCHAR(PropertyName.c_str());

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] array_count -> asset no longer valid"));
				return sol::lua_nil;
			}

			FProperty* Prop = LuaAssetHelper::FindProperty(Asset, FProp);
			if (!Prop)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] array_count(\"%s\") -> property not found"), *FProp));
				return sol::lua_nil;
			}

			FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
			if (!ArrayProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] array_count(\"%s\") -> not an array property"), *FProp));
				return sol::lua_nil;
			}

			void* ArrayContainer = ArrayProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptArrayHelper ArrayHelper(ArrayProp, ArrayContainer);

			return sol::make_object(Lua, ArrayHelper.Num());
		});

		// ----------------------------------------------------------------
		// asset:array_add(property, value?) -> new count
		// ----------------------------------------------------------------
		AssetObj.set_function("array_add", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, sol::optional<std::string> Value,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = UTF8_TO_TCHAR(PropertyName.c_str());

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] array_add -> asset no longer valid"));
				return sol::lua_nil;
			}

			FProperty* Prop = LuaAssetHelper::FindProperty(Asset, FProp);
			if (!Prop)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] array_add(\"%s\") -> property not found"), *FProp));
				return sol::lua_nil;
			}

			FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
			if (!ArrayProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] array_add(\"%s\") -> not an array property"), *FProp));
				return sol::lua_nil;
			}

			Asset->Modify();
			Asset->PreEditChange(Prop);

			void* ArrayContainer = ArrayProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptArrayHelper ArrayHelper(ArrayProp, ArrayContainer);
			int32 NewIndex = ArrayHelper.AddValue();

			// If value provided, set it via ImportText on the inner property
			if (Value.has_value())
			{
				FString FValue = UTF8_TO_TCHAR(Value.value().c_str());
				void* ElementPtr = ArrayHelper.GetRawPtr(NewIndex);
				FProperty* InnerProp = ArrayProp->Inner;

				const TCHAR* ImportResult = InnerProp->ImportText_Direct(*FValue, ElementPtr, Asset, PPF_None);
				if (!ImportResult)
				{
					// Try boolean fixup
					FString Transformed = FValue;
					if (FValue.Equals(TEXT("true"), ESearchCase::IgnoreCase)) Transformed = TEXT("True");
					else if (FValue.Equals(TEXT("false"), ESearchCase::IgnoreCase)) Transformed = TEXT("False");

					ImportResult = InnerProp->ImportText_Direct(*Transformed, ElementPtr, Asset, PPF_None);
					if (!ImportResult)
					{
						// Remove the element we just added — it has garbage data
						ArrayHelper.RemoveValues(NewIndex, 1);
						FPropertyChangedEvent Event(Prop, EPropertyChangeType::ValueSet);
						Asset->PostEditChangeProperty(Event);

						Session.Log(FString::Printf(TEXT("[FAIL] array_add(\"%s\") -> failed to parse value '%s'"),
							*FProp, *FValue));
						return sol::lua_nil;
					}
				}
			}

			Asset->MarkPackageDirty();
			FPropertyChangedEvent Event(Prop, EPropertyChangeType::ArrayAdd);
			Asset->PostEditChangeProperty(Event);

			// Run asset-specific post-edit
			LuaAssetHelper::PostEdit(Asset, nullptr);

			int32 NewCount = ArrayHelper.Num();
			Session.Log(FString::Printf(TEXT("[OK] array_add(\"%s\") -> count = %d"), *FProp, NewCount));
			return sol::make_object(Lua, NewCount);
		});

		// ----------------------------------------------------------------
		// asset:array_remove(property, index) -> new count
		// 1-based index to match Lua convention
		// ----------------------------------------------------------------
		AssetObj.set_function("array_remove", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, int Index,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = UTF8_TO_TCHAR(PropertyName.c_str());

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] array_remove -> asset no longer valid"));
				return sol::lua_nil;
			}

			FProperty* Prop = LuaAssetHelper::FindProperty(Asset, FProp);
			if (!Prop)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] array_remove(\"%s\") -> property not found"), *FProp));
				return sol::lua_nil;
			}

			FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
			if (!ArrayProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] array_remove(\"%s\") -> not an array property"), *FProp));
				return sol::lua_nil;
			}

			// Convert 1-based Lua index to 0-based
			int32 ZeroIndex = Index - 1;

			Asset->Modify();
			Asset->PreEditChange(Prop);

			void* ArrayContainer = ArrayProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptArrayHelper ArrayHelper(ArrayProp, ArrayContainer);

			if (ZeroIndex < 0 || ZeroIndex >= ArrayHelper.Num())
			{
				FPropertyChangedEvent Event(Prop, EPropertyChangeType::ValueSet);
				Asset->PostEditChangeProperty(Event);
				Session.Log(FString::Printf(TEXT("[FAIL] array_remove(\"%s\", %d) -> index out of range (count=%d)"),
					*FProp, Index, ArrayHelper.Num()));
				return sol::lua_nil;
			}

			ArrayHelper.RemoveValues(ZeroIndex, 1);

			Asset->MarkPackageDirty();
			FPropertyChangedEvent Event(Prop, EPropertyChangeType::ArrayRemove);
			Asset->PostEditChangeProperty(Event);

			LuaAssetHelper::PostEdit(Asset, nullptr);

			int32 NewCount = ArrayHelper.Num();
			Session.Log(FString::Printf(TEXT("[OK] array_remove(\"%s\", %d) -> count = %d"), *FProp, Index, NewCount));
			return sol::make_object(Lua, NewCount);
		});

		// ================================================================
		// Map property operations
		// ================================================================

		// ----------------------------------------------------------------
		// asset:map_count(property) -> integer
		// ----------------------------------------------------------------
		AssetObj.set_function("map_count", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = UTF8_TO_TCHAR(PropertyName.c_str());

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] map_count -> asset no longer valid"));
				return sol::lua_nil;
			}

			FMapProperty* MapProp = LuaAssetHelper::FindMapProperty(Asset, FProp);
			if (!MapProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] map_count(\"%s\") -> not a map property"), *FProp));
				return sol::lua_nil;
			}

			void* MapContainer = MapProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptMapHelper MapHelper(MapProp, MapContainer);

			return sol::make_object(Lua, MapHelper.Num());
		});

		// ----------------------------------------------------------------
		// asset:map_keys(property) -> table of key strings
		// ----------------------------------------------------------------
		AssetObj.set_function("map_keys", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = UTF8_TO_TCHAR(PropertyName.c_str());

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] map_keys -> asset no longer valid"));
				return sol::lua_nil;
			}

			FMapProperty* MapProp = LuaAssetHelper::FindMapProperty(Asset, FProp);
			if (!MapProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] map_keys(\"%s\") -> not a map property"), *FProp));
				return sol::lua_nil;
			}

			void* MapContainer = MapProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptMapHelper MapHelper(MapProp, MapContainer);
			FProperty* KeyProp = MapHelper.GetKeyProperty();

			sol::table Result = Lua.create_table();
			int32 Index = 1;
			for (FScriptMapHelper::FIterator It = MapHelper.CreateIterator(); It; ++It)
			{
				const uint8* KeyPtr = MapHelper.GetKeyPtr(It);
				FString KeyStr = LuaAssetHelper::ExportPropertyValue(KeyProp, KeyPtr, Asset);
				Result[Index++] = TCHAR_TO_UTF8(*KeyStr);
			}

			Session.Log(FString::Printf(TEXT("[OK] map_keys(\"%s\") -> %d keys"), *FProp, Index - 1));
			return Result;
		});

		// ----------------------------------------------------------------
		// asset:map_get(property, key) -> value string
		// ----------------------------------------------------------------
		AssetObj.set_function("map_get", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, const std::string& Key,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = UTF8_TO_TCHAR(PropertyName.c_str());
			FString FKey = UTF8_TO_TCHAR(Key.c_str());

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] map_get -> asset no longer valid"));
				return sol::lua_nil;
			}

			FMapProperty* MapProp = LuaAssetHelper::FindMapProperty(Asset, FProp);
			if (!MapProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] map_get(\"%s\") -> not a map property"), *FProp));
				return sol::lua_nil;
			}

			void* MapContainer = MapProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptMapHelper MapHelper(MapProp, MapContainer);
			FProperty* KeyProp = MapHelper.GetKeyProperty();
			FProperty* ValProp = MapHelper.GetValueProperty();

			// Parse the key string
			void* TempKey = FMemory::Malloc(KeyProp->GetSize(), KeyProp->GetMinAlignment());
			KeyProp->InitializeValue(TempKey);

			if (!LuaAssetHelper::ImportPropertyValue(KeyProp, TempKey, FKey, Asset))
			{
				KeyProp->DestroyValue(TempKey);
				FMemory::Free(TempKey);
				Session.Log(FString::Printf(TEXT("[FAIL] map_get(\"%s\", \"%s\") -> failed to parse key"), *FProp, *FKey));
				return sol::lua_nil;
			}

			uint8* ValuePtr = MapHelper.FindValueFromHash(TempKey);
			KeyProp->DestroyValue(TempKey);
			FMemory::Free(TempKey);

			if (!ValuePtr)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] map_get(\"%s\", \"%s\") -> key not found"), *FProp, *FKey));
				return sol::lua_nil;
			}

			FString ValueStr = LuaAssetHelper::ExportPropertyValue(ValProp, ValuePtr, Asset);
			return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*ValueStr)));
		});

		// ----------------------------------------------------------------
		// asset:map_set(property, key, value) -> true/nil
		// ----------------------------------------------------------------
		AssetObj.set_function("map_set", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, const std::string& Key, const std::string& Value,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = UTF8_TO_TCHAR(PropertyName.c_str());
			FString FKey = UTF8_TO_TCHAR(Key.c_str());
			FString FValue = UTF8_TO_TCHAR(Value.c_str());

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] map_set -> asset no longer valid"));
				return sol::lua_nil;
			}

			FMapProperty* MapProp = LuaAssetHelper::FindMapProperty(Asset, FProp);
			if (!MapProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] map_set(\"%s\") -> not a map property"), *FProp));
				return sol::lua_nil;
			}

			FProperty* RawProp = LuaAssetHelper::FindProperty(Asset, FProp);

			void* MapContainer = MapProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptMapHelper MapHelper(MapProp, MapContainer);
			FProperty* KeyProp = MapHelper.GetKeyProperty();
			FProperty* ValProp = MapHelper.GetValueProperty();

			// Parse key
			void* TempKey = FMemory::Malloc(KeyProp->GetSize(), KeyProp->GetMinAlignment());
			KeyProp->InitializeValue(TempKey);

			if (!LuaAssetHelper::ImportPropertyValue(KeyProp, TempKey, FKey, Asset))
			{
				KeyProp->DestroyValue(TempKey);
				FMemory::Free(TempKey);
				Session.Log(FString::Printf(TEXT("[FAIL] map_set(\"%s\", \"%s\") -> failed to parse key"), *FProp, *FKey));
				return sol::lua_nil;
			}

			// Parse value
			void* TempVal = FMemory::Malloc(ValProp->GetSize(), ValProp->GetMinAlignment());
			ValProp->InitializeValue(TempVal);

			if (!LuaAssetHelper::ImportPropertyValue(ValProp, TempVal, FValue, Asset))
			{
				KeyProp->DestroyValue(TempKey);
				FMemory::Free(TempKey);
				ValProp->DestroyValue(TempVal);
				FMemory::Free(TempVal);
				Session.Log(FString::Printf(TEXT("[FAIL] map_set(\"%s\", \"%s\", \"%s\") -> failed to parse value"), *FProp, *FKey, *FValue));
				return sol::lua_nil;
			}

			Asset->Modify();
			Asset->PreEditChange(RawProp);

			MapHelper.AddPair(TempKey, TempVal);

			KeyProp->DestroyValue(TempKey);
			FMemory::Free(TempKey);
			ValProp->DestroyValue(TempVal);
			FMemory::Free(TempVal);

			Asset->MarkPackageDirty();
			FPropertyChangedEvent Event(RawProp, EPropertyChangeType::ValueSet);
			Asset->PostEditChangeProperty(Event);
			LuaAssetHelper::PostEdit(Asset, nullptr);

			Session.Log(FString::Printf(TEXT("[OK] map_set(\"%s\", \"%s\", \"%s\")"), *FProp, *FKey, *FValue));
			return sol::make_object(Lua, true);
		});

		// ----------------------------------------------------------------
		// asset:map_remove(property, key) -> true/nil
		// ----------------------------------------------------------------
		AssetObj.set_function("map_remove", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, const std::string& Key,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = UTF8_TO_TCHAR(PropertyName.c_str());
			FString FKey = UTF8_TO_TCHAR(Key.c_str());

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] map_remove -> asset no longer valid"));
				return sol::lua_nil;
			}

			FMapProperty* MapProp = LuaAssetHelper::FindMapProperty(Asset, FProp);
			if (!MapProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] map_remove(\"%s\") -> not a map property"), *FProp));
				return sol::lua_nil;
			}

			FProperty* RawProp = LuaAssetHelper::FindProperty(Asset, FProp);

			void* MapContainer = MapProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptMapHelper MapHelper(MapProp, MapContainer);
			FProperty* KeyProp = MapHelper.GetKeyProperty();

			// Parse key
			void* TempKey = FMemory::Malloc(KeyProp->GetSize(), KeyProp->GetMinAlignment());
			KeyProp->InitializeValue(TempKey);

			if (!LuaAssetHelper::ImportPropertyValue(KeyProp, TempKey, FKey, Asset))
			{
				KeyProp->DestroyValue(TempKey);
				FMemory::Free(TempKey);
				Session.Log(FString::Printf(TEXT("[FAIL] map_remove(\"%s\", \"%s\") -> failed to parse key"), *FProp, *FKey));
				return sol::lua_nil;
			}

			int32 FoundIndex = MapHelper.FindMapPairIndexFromHash(TempKey);
			KeyProp->DestroyValue(TempKey);
			FMemory::Free(TempKey);

			if (FoundIndex == INDEX_NONE)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] map_remove(\"%s\", \"%s\") -> key not found"), *FProp, *FKey));
				return sol::lua_nil;
			}

			Asset->Modify();
			Asset->PreEditChange(RawProp);

			MapHelper.RemoveAt(FoundIndex);
			MapHelper.Rehash();

			Asset->MarkPackageDirty();
			FPropertyChangedEvent Event(RawProp, EPropertyChangeType::ValueSet);
			Asset->PostEditChangeProperty(Event);
			LuaAssetHelper::PostEdit(Asset, nullptr);

			Session.Log(FString::Printf(TEXT("[OK] map_remove(\"%s\", \"%s\") -> count = %d"), *FProp, *FKey, MapHelper.Num()));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// Set property operations
		// ================================================================

		// ----------------------------------------------------------------
		// asset:set_count(property) -> integer
		// ----------------------------------------------------------------
		AssetObj.set_function("set_count", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = UTF8_TO_TCHAR(PropertyName.c_str());

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] set_count -> asset no longer valid"));
				return sol::lua_nil;
			}

			FSetProperty* SetProp = LuaAssetHelper::FindSetProperty(Asset, FProp);
			if (!SetProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_count(\"%s\") -> not a set property"), *FProp));
				return sol::lua_nil;
			}

			void* SetContainer = SetProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptSetHelper SetHelper(SetProp, SetContainer);

			return sol::make_object(Lua, SetHelper.Num());
		});

		// ----------------------------------------------------------------
		// asset:set_values(property) -> table of value strings
		// ----------------------------------------------------------------
		AssetObj.set_function("set_values", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = UTF8_TO_TCHAR(PropertyName.c_str());

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] set_values -> asset no longer valid"));
				return sol::lua_nil;
			}

			FSetProperty* SetProp = LuaAssetHelper::FindSetProperty(Asset, FProp);
			if (!SetProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_values(\"%s\") -> not a set property"), *FProp));
				return sol::lua_nil;
			}

			void* SetContainer = SetProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptSetHelper SetHelper(SetProp, SetContainer);
			FProperty* ElemProp = SetHelper.GetElementProperty();

			sol::table Result = Lua.create_table();
			int32 Index = 1;
			for (FScriptSetHelper::FIterator It = SetHelper.CreateIterator(); It; ++It)
			{
				const uint8* ElemPtr = SetHelper.GetElementPtr(It);
				FString ElemStr = LuaAssetHelper::ExportPropertyValue(ElemProp, ElemPtr, Asset);
				Result[Index++] = TCHAR_TO_UTF8(*ElemStr);
			}

			Session.Log(FString::Printf(TEXT("[OK] set_values(\"%s\") -> %d elements"), *FProp, Index - 1));
			return Result;
		});

		// ----------------------------------------------------------------
		// asset:set_add(property, value) -> true/nil
		// ----------------------------------------------------------------
		AssetObj.set_function("set_add", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, const std::string& Value,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = UTF8_TO_TCHAR(PropertyName.c_str());
			FString FValue = UTF8_TO_TCHAR(Value.c_str());

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] set_add -> asset no longer valid"));
				return sol::lua_nil;
			}

			FSetProperty* SetProp = LuaAssetHelper::FindSetProperty(Asset, FProp);
			if (!SetProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_add(\"%s\") -> not a set property"), *FProp));
				return sol::lua_nil;
			}

			FProperty* RawProp = LuaAssetHelper::FindProperty(Asset, FProp);

			void* SetContainer = SetProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptSetHelper SetHelper(SetProp, SetContainer);
			FProperty* ElemProp = SetHelper.GetElementProperty();

			// Parse value
			void* TempElem = FMemory::Malloc(ElemProp->GetSize(), ElemProp->GetMinAlignment());
			ElemProp->InitializeValue(TempElem);

			if (!LuaAssetHelper::ImportPropertyValue(ElemProp, TempElem, FValue, Asset))
			{
				ElemProp->DestroyValue(TempElem);
				FMemory::Free(TempElem);
				Session.Log(FString::Printf(TEXT("[FAIL] set_add(\"%s\", \"%s\") -> failed to parse value"), *FProp, *FValue));
				return sol::lua_nil;
			}

			Asset->Modify();
			Asset->PreEditChange(RawProp);

			SetHelper.AddElement(TempElem);

			ElemProp->DestroyValue(TempElem);
			FMemory::Free(TempElem);

			Asset->MarkPackageDirty();
			FPropertyChangedEvent Event(RawProp, EPropertyChangeType::ValueSet);
			Asset->PostEditChangeProperty(Event);
			LuaAssetHelper::PostEdit(Asset, nullptr);

			Session.Log(FString::Printf(TEXT("[OK] set_add(\"%s\", \"%s\") -> count = %d"), *FProp, *FValue, SetHelper.Num()));
			return sol::make_object(Lua, true);
		});

		// ----------------------------------------------------------------
		// asset:set_remove(property, value) -> true/nil
		// ----------------------------------------------------------------
		AssetObj.set_function("set_remove", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, const std::string& Value,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = UTF8_TO_TCHAR(PropertyName.c_str());
			FString FValue = UTF8_TO_TCHAR(Value.c_str());

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] set_remove -> asset no longer valid"));
				return sol::lua_nil;
			}

			FSetProperty* SetProp = LuaAssetHelper::FindSetProperty(Asset, FProp);
			if (!SetProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_remove(\"%s\") -> not a set property"), *FProp));
				return sol::lua_nil;
			}

			FProperty* RawProp = LuaAssetHelper::FindProperty(Asset, FProp);

			void* SetContainer = SetProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptSetHelper SetHelper(SetProp, SetContainer);
			FProperty* ElemProp = SetHelper.GetElementProperty();

			// Parse value
			void* TempElem = FMemory::Malloc(ElemProp->GetSize(), ElemProp->GetMinAlignment());
			ElemProp->InitializeValue(TempElem);

			if (!LuaAssetHelper::ImportPropertyValue(ElemProp, TempElem, FValue, Asset))
			{
				ElemProp->DestroyValue(TempElem);
				FMemory::Free(TempElem);
				Session.Log(FString::Printf(TEXT("[FAIL] set_remove(\"%s\", \"%s\") -> failed to parse value"), *FProp, *FValue));
				return sol::lua_nil;
			}

			Asset->Modify();
			Asset->PreEditChange(RawProp);

			bool bRemoved = SetHelper.RemoveElement(TempElem);

			ElemProp->DestroyValue(TempElem);
			FMemory::Free(TempElem);

			if (!bRemoved)
			{
				FPropertyChangedEvent FailEvent(RawProp, EPropertyChangeType::ValueSet);
				Asset->PostEditChangeProperty(FailEvent);
				Session.Log(FString::Printf(TEXT("[FAIL] set_remove(\"%s\", \"%s\") -> element not found"), *FProp, *FValue));
				return sol::lua_nil;
			}

			Asset->MarkPackageDirty();
			FPropertyChangedEvent Event(RawProp, EPropertyChangeType::ValueSet);
			Asset->PostEditChangeProperty(Event);
			LuaAssetHelper::PostEdit(Asset, nullptr);

			Session.Log(FString::Printf(TEXT("[OK] set_remove(\"%s\", \"%s\") -> count = %d"), *FProp, *FValue, SetHelper.Num()));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// Property metadata
		// ================================================================

		// ----------------------------------------------------------------
		// asset:property_meta(property) -> table with metadata keys
		// ----------------------------------------------------------------
		AssetObj.set_function("property_meta", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = UTF8_TO_TCHAR(PropertyName.c_str());

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] property_meta -> asset no longer valid"));
				return sol::lua_nil;
			}

			// Use ResolvePropertyPath for dot notation and array index support
			// (e.g. "Modifiers[0]", "Modifiers[0].ModifierOp")
			FProperty* Prop = nullptr;
			if (FProp.Contains(TEXT(".")) || FProp.Contains(TEXT("[")))
			{
				LuaAssetHelper::FResolvedProperty Resolved = LuaAssetHelper::ResolvePropertyPath(Asset, FProp);
				Prop = Resolved.Property;
			}
			else
			{
				Prop = LuaAssetHelper::FindProperty(Asset, FProp);
			}
			if (!Prop)
			{
				FString Error = NeoBlueprint::FuzzyMatchProperty(Asset, FProp);
				Session.Log(FString::Printf(TEXT("[FAIL] property_meta(\"%s\") -> %s"), *FProp, *Error));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();

			// Property type info
			Result["type"] = TCHAR_TO_UTF8(*NeoStackToolUtils::GetPropertyTypeName(Prop));
			Result["cpp_type"] = TCHAR_TO_UTF8(*Prop->GetCPPType());

			// Flags
			Result["editable"] = Prop->HasAnyPropertyFlags(CPF_Edit);
			Result["blueprint_visible"] = Prop->HasAnyPropertyFlags(CPF_BlueprintVisible);
			Result["blueprint_read_only"] = Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly);
			Result["config"] = Prop->HasAnyPropertyFlags(CPF_Config);
			Result["transient"] = Prop->HasAnyPropertyFlags(CPF_Transient);
			Result["deprecated"] = Prop->HasAnyPropertyFlags(CPF_Deprecated);

#if WITH_METADATA
			// All UE metadata (ClampMin, ClampMax, UIMin, UIMax, ToolTip, AllowedClasses, etc.)
			const TMap<FName, FString>* MetaMap = Prop->GetMetaDataMap();
			if (MetaMap)
			{
				sol::table Meta = Lua.create_table();
				for (const auto& Pair : *MetaMap)
				{
					Meta[TCHAR_TO_UTF8(*Pair.Key.ToString())] = TCHAR_TO_UTF8(*Pair.Value);
				}
				Result["metadata"] = Meta;
			}
#endif

			// Container info for array/map/set
			if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
			{
				Result["container"] = "array";
				Result["inner_type"] = TCHAR_TO_UTF8(*NeoStackToolUtils::GetPropertyTypeName(ArrayProp->Inner));
			}
			else if (FMapProperty* MapPropTyped = CastField<FMapProperty>(Prop))
			{
				Result["container"] = "map";
				Result["key_type"] = TCHAR_TO_UTF8(*MapPropTyped->GetKeyProperty()->GetCPPType());
				Result["value_type"] = TCHAR_TO_UTF8(*MapPropTyped->GetValueProperty()->GetCPPType());
			}
			else if (FSetProperty* SetPropTyped = CastField<FSetProperty>(Prop))
			{
				Result["container"] = "set";
				Result["element_type"] = TCHAR_TO_UTF8(*SetPropTyped->GetElementProperty()->GetCPPType());
			}
			else if (FStructProperty* StructPropTyped = CastField<FStructProperty>(Prop))
			{
				Result["container"] = "struct";
				Result["struct_name"] = TCHAR_TO_UTF8(*StructPropTyped->Struct->GetName());

				// List struct fields
				sol::table Fields = Lua.create_table();
				int32 FieldIdx = 1;
				for (TFieldIterator<FProperty> FieldIt(StructPropTyped->Struct); FieldIt; ++FieldIt)
				{
					sol::table FieldEntry = Lua.create_table();
					FieldEntry["name"] = TCHAR_TO_UTF8(*FieldIt->GetName());
					FieldEntry["type"] = TCHAR_TO_UTF8(*NeoStackToolUtils::GetPropertyTypeName(*FieldIt));
					Fields[FieldIdx++] = FieldEntry;
				}
				Result["fields"] = Fields;
			}

			Session.Log(FString::Printf(TEXT("[OK] property_meta(\"%s\") -> %s"), *FProp, *Prop->GetCPPType()));
			return Result;
		});

		// ----------------------------------------------------------------
		// asset:list_user_data() -> table of attached AssetUserData
		// ----------------------------------------------------------------
		AssetObj.set_function("list_user_data", [WeakAsset, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] list_user_data -> asset no longer valid"));
				return sol::lua_nil;
			}

			IInterface_AssetUserData* UDInterface = LuaAssetHelper::GetAssetUserDataInterface(Asset);
			if (!UDInterface)
			{
				Session.Log(TEXT("[OK] list_user_data() -> asset does not support AssetUserData"));
				return Lua.create_table();
			}

			const TArray<UAssetUserData*>* UserDataArray = UDInterface->GetAssetUserDataArray();
			if (!UserDataArray || UserDataArray->Num() == 0)
			{
				Session.Log(TEXT("[OK] list_user_data() -> 0 items"));
				return Lua.create_table();
			}

			sol::table Result = Lua.create_table();
			int32 Index = 1;

			for (UAssetUserData* UD : *UserDataArray)
			{
				if (!UD) continue;

				sol::table Entry = Lua.create_table();
				Entry["index"] = Index;
				Entry["class_name"] = TCHAR_TO_UTF8(*LuaAssetHelper::GetUserDataClassName(UD));
				Entry["class_path"] = TCHAR_TO_UTF8(*UD->GetClass()->GetPathName());

				// Count properties
				int32 PropCount = 0;
				for (TFieldIterator<FProperty> PropIt(UD->GetClass()); PropIt; ++PropIt)
				{
					if (!(*PropIt)->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient))
						PropCount++;
				}
				Entry["properties"] = PropCount;

				Result[Index++] = Entry;
			}

			Session.Log(FString::Printf(TEXT("[OK] list_user_data() -> %d items"), Index - 1));
			return Result;
		});

		// ----------------------------------------------------------------
		// asset:get_user_data(class_name_or_index) -> sub-object table with get/set/list_properties
		// ----------------------------------------------------------------
		AssetObj.set_function("get_user_data", [WeakAsset, &Session](sol::table /*self*/,
			sol::object Identifier, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] get_user_data -> asset no longer valid"));
				return sol::lua_nil;
			}

			IInterface_AssetUserData* UDInterface = LuaAssetHelper::GetAssetUserDataInterface(Asset);
			if (!UDInterface)
			{
				Session.Log(TEXT("[FAIL] get_user_data -> asset does not support AssetUserData"));
				return sol::lua_nil;
			}

			UAssetUserData* FoundUD = nullptr;

			if (Identifier.is<int>())
			{
				// By 1-based index
				int32 ZeroIndex = Identifier.as<int>() - 1;
				const TArray<UAssetUserData*>* Arr = UDInterface->GetAssetUserDataArray();
				if (Arr && ZeroIndex >= 0 && ZeroIndex < Arr->Num())
				{
					FoundUD = (*Arr)[ZeroIndex];
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] get_user_data(%d) -> index out of range"), ZeroIndex + 1));
					return sol::lua_nil;
				}
			}
			else if (Identifier.is<std::string>())
			{
				// By class name
				FString ClassName = UTF8_TO_TCHAR(Identifier.as<std::string>().c_str());
				const TArray<UAssetUserData*>* Arr = UDInterface->GetAssetUserDataArray();
				if (Arr)
				{
					for (UAssetUserData* UD : *Arr)
					{
						if (UD && (UD->GetClass()->GetName().Contains(ClassName, ESearchCase::IgnoreCase)))
						{
							FoundUD = UD;
							break;
						}
					}
				}
				if (!FoundUD)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] get_user_data(\"%s\") -> not found"), *ClassName));
					return sol::lua_nil;
				}
			}
			else
			{
				Session.Log(TEXT("[FAIL] get_user_data -> pass class name (string) or index (number)"));
				return sol::lua_nil;
			}

			if (!FoundUD)
			{
				Session.Log(TEXT("[FAIL] get_user_data -> user data is null"));
				return sol::lua_nil;
			}

			// Build a sub-object table with get/set/list_properties — reuse the same reflection pattern
			sol::table UDObj = Lua.create_table();
			UDObj["class_name"] = TCHAR_TO_UTF8(*LuaAssetHelper::GetUserDataClassName(FoundUD));
			UDObj["class_path"] = TCHAR_TO_UTF8(*FoundUD->GetClass()->GetPathName());

			// GC-safe weak references for nested lambdas
			TWeakObjectPtr<UAssetUserData> WeakFoundUD(FoundUD);

			// get(property)
			UDObj.set_function("get", [WeakFoundUD, &Session](sol::table /*self*/,
				const std::string& PropertyName, sol::this_state S) -> sol::object
			{
				sol::state_view Lua(S);
				FString FProp = UTF8_TO_TCHAR(PropertyName.c_str());

				UAssetUserData* FoundUD = WeakFoundUD.Get();
				if (!FoundUD)
				{
					Session.Log(TEXT("[FAIL] user_data:get -> no longer valid"));
					return sol::lua_nil;
				}

				LuaAssetHelper::FResolvedProperty Resolved = LuaAssetHelper::ResolvePropertyPath(FoundUD, FProp);
				if (!Resolved.Property)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] user_data:get(\"%s\") -> property not found"), *FProp));
					return sol::lua_nil;
				}

				FString Value = LuaAssetHelper::GetPropertyValue(FoundUD, Resolved.Property, Resolved.ContainerPtr);
				return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*Value)));
			});

			// set(property, value)
			UDObj.set_function("set", [WeakFoundUD, WeakAsset, &Session](sol::table /*self*/,
				const std::string& PropertyName, const std::string& Value, sol::this_state S) -> sol::object
			{
				sol::state_view Lua(S);
				FString FProp = UTF8_TO_TCHAR(PropertyName.c_str());
				FString FValue = UTF8_TO_TCHAR(Value.c_str());

				UAssetUserData* FoundUD = WeakFoundUD.Get();
				UObject* Asset = WeakAsset.Get();
				if (!FoundUD || !Asset)
				{
					Session.Log(TEXT("[FAIL] user_data:set -> asset or user data no longer valid"));
					return sol::lua_nil;
				}

				LuaAssetHelper::FResolvedProperty Resolved = LuaAssetHelper::ResolvePropertyPath(FoundUD, FProp);
				if (!Resolved.Property)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] user_data:set(\"%s\") -> property not found"), *FProp));
					return sol::lua_nil;
				}

				Asset->Modify();
				FoundUD->PreEditChange(Resolved.Property);

				FString Error;
				if (!LuaAssetHelper::SetPropertyValue(FoundUD, Resolved.Property, Resolved.ContainerPtr, FValue, Error))
				{
					FPropertyChangedEvent FailEvent(Resolved.Property, EPropertyChangeType::ValueSet);
					FoundUD->PostEditChangeProperty(FailEvent);
					Session.Log(FString::Printf(TEXT("[FAIL] user_data:set(\"%s\", \"%s\") -> %s"), *FProp, *FValue, *Error));
					return sol::lua_nil;
				}

				Asset->MarkPackageDirty();
				FPropertyChangedEvent SuccessEvent(Resolved.Property, EPropertyChangeType::ValueSet);
				FoundUD->PostEditChangeProperty(SuccessEvent);

				Session.Log(FString::Printf(TEXT("[OK] user_data:set(\"%s\") = \"%s\""), *FProp, *FValue));
				return sol::make_object(Lua, true);
			});

			// list_properties(filter?, all?)
			UDObj.set_function("list_properties", [WeakFoundUD, &Session](sol::table /*self*/,
				sol::optional<std::string> Filter, sol::optional<bool> IncludeAll,
				sol::this_state S) -> sol::object
			{
				sol::state_view Lua(S);
				UAssetUserData* FoundUD = WeakFoundUD.Get();
				if (!FoundUD)
				{
					Session.Log(TEXT("[FAIL] user_data:list_properties -> no longer valid"));
					return sol::lua_nil;
				}

				FString FFilter = Filter.has_value() ? UTF8_TO_TCHAR(Filter.value().c_str()) : TEXT("");
				bool bAll = IncludeAll.value_or(true); // Default to all for user data (less noise than main asset)

				sol::table Result = Lua.create_table();
				int32 Index = 1;

				for (TFieldIterator<FProperty> PropIt(FoundUD->GetClass()); PropIt; ++PropIt)
				{
					FProperty* Property = *PropIt;
					if (Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient)) continue;
					if (!bAll && !Property->HasAnyPropertyFlags(CPF_Edit)) continue;

					FString Name = Property->GetName();
					if (!FFilter.IsEmpty() && !Name.Contains(FFilter, ESearchCase::IgnoreCase)) continue;

					FString Type = NeoStackToolUtils::GetPropertyTypeName(Property);
					FString Value = NeoStackToolUtils::GetPropertyValueAsString(FoundUD, Property, FoundUD);
					FString Category = Property->GetMetaData(TEXT("Category"));
					if (Category.IsEmpty()) Category = TEXT("Default");

					if (Value.Len() > 120) Value = Value.Left(117) + TEXT("...");

					sol::table Entry = Lua.create_table();
					Entry["name"] = TCHAR_TO_UTF8(*Name);
					Entry["type"] = TCHAR_TO_UTF8(*Type);
					Entry["value"] = TCHAR_TO_UTF8(*Value);
					Entry["category"] = TCHAR_TO_UTF8(*Category);
					Result[Index++] = Entry;
				}

				Session.Log(FString::Printf(TEXT("[OK] user_data:list_properties(%s) -> %d properties"),
					FFilter.IsEmpty() ? TEXT("*") : *FFilter, Index - 1));
				return Result;
			});

			Session.Log(FString::Printf(TEXT("[OK] get_user_data(\"%s\") -> sub-object with get/set/list_properties"),
				*LuaAssetHelper::GetUserDataClassName(FoundUD)));
			return UDObj;
		});

		// ----------------------------------------------------------------
		// asset:add_user_data(class_name) -> true/nil
		// ----------------------------------------------------------------
		AssetObj.set_function("add_user_data", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& ClassName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FClassName = UTF8_TO_TCHAR(ClassName.c_str());

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] add_user_data -> asset no longer valid"));
				return sol::lua_nil;
			}

			IInterface_AssetUserData* UDInterface = LuaAssetHelper::GetAssetUserDataInterface(Asset);
			if (!UDInterface)
			{
				Session.Log(TEXT("[FAIL] add_user_data -> asset does not support AssetUserData"));
				return sol::lua_nil;
			}

			UClass* UDClass = LuaAssetHelper::FindUserDataClass(FClassName);
			if (!UDClass)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_user_data(\"%s\") -> class not found. Must be a UAssetUserData subclass."), *FClassName));
				return sol::lua_nil;
			}

			bool bWasAlreadyAttached = (UDInterface->GetAssetUserDataOfClass(UDClass) != nullptr);

			Asset->Modify();
			UAssetUserData* NewUD = NewObject<UAssetUserData>(Asset, UDClass);
			UDInterface->AddAssetUserData(NewUD);
			Asset->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] add_user_data(\"%s\") -> %s %s"),
				*UDClass->GetName(), bWasAlreadyAttached ? TEXT("replaced on") : TEXT("attached to"),
				*Asset->GetName()));
			return sol::make_object(Lua, true);
		});

		// ----------------------------------------------------------------
		// asset:remove_user_data(class_name_or_index) -> true/nil
		// ----------------------------------------------------------------
		AssetObj.set_function("remove_user_data", [WeakAsset, &Session](sol::table /*self*/,
			sol::object Identifier, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] remove_user_data -> asset no longer valid"));
				return sol::lua_nil;
			}

			IInterface_AssetUserData* UDInterface = LuaAssetHelper::GetAssetUserDataInterface(Asset);
			if (!UDInterface)
			{
				Session.Log(TEXT("[FAIL] remove_user_data -> asset does not support AssetUserData"));
				return sol::lua_nil;
			}

			UClass* ClassToRemove = nullptr;

			if (Identifier.is<int>())
			{
				int32 ZeroIndex = Identifier.as<int>() - 1;
				const TArray<UAssetUserData*>* Arr = UDInterface->GetAssetUserDataArray();
				if (Arr && ZeroIndex >= 0 && ZeroIndex < Arr->Num() && (*Arr)[ZeroIndex])
				{
					ClassToRemove = (*Arr)[ZeroIndex]->GetClass();
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove_user_data(%d) -> index out of range"), ZeroIndex + 1));
					return sol::lua_nil;
				}
			}
			else if (Identifier.is<std::string>())
			{
				FString ClassName = UTF8_TO_TCHAR(Identifier.as<std::string>().c_str());
				ClassToRemove = LuaAssetHelper::FindUserDataClass(ClassName);
				if (!ClassToRemove)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove_user_data(\"%s\") -> class not found"), *ClassName));
					return sol::lua_nil;
				}
			}
			else
			{
				Session.Log(TEXT("[FAIL] remove_user_data -> pass class name (string) or index (number)"));
				return sol::lua_nil;
			}

			Asset->Modify();
			UDInterface->RemoveUserDataOfClass(ClassToRemove);
			Asset->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] remove_user_data(\"%s\") -> removed from %s"),
				*ClassToRemove->GetName(), *Asset->GetName()));
			return sol::make_object(Lua, true);
		});

		// ----------------------------------------------------------------
		// asset:help() -> string
		// ----------------------------------------------------------------
		AssetObj.set_function("help", [&Session](sol::table self, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			std::string Out = "=== Generic methods (all assets) ===\n"
				"  get(property) — read property (dot-notation: \"Struct.Field[0].Sub\", map: \"Map{key}.Sub\")\n"
				"  set(property, value) — set property via ImportText\n"
				"  list_properties(filter?, all?) — list editable properties\n"
				"  property_meta(property) — metadata (ClampMin/Max, UIMin/Max, ToolTip, container info, struct fields)\n"
				"  array_add(property, value?) / array_remove(property, index) / array_count(property)\n"
				"  map_get(prop,key) / map_set(prop,key,val) / map_remove(prop,key) / map_count(prop) / map_keys(prop)\n"
				"  set_add(prop,value) / set_remove(prop,value) / set_count(prop) / set_values(prop)\n"
				"  save() — save to disk\n"
				"  info() — structured read of asset contents\n"
				"\n=== AssetUserData methods (StaticMesh, SkeletalMesh, Texture, AnimSequence, etc.) ===\n"
				"  list_user_data() — list all attached user data with class names\n"
				"  get_user_data(class_or_index) — get sub-object with get/set/list_properties\n"
				"  add_user_data(class_name) — attach new user data instance\n"
				"  remove_user_data(class_or_index) — remove attached user data\n";

			sol::optional<std::string> AssetHelp = self.get<sol::optional<std::string>>("_help_text");
			if (AssetHelp.has_value())
			{
				Out += "\n=== Asset-specific methods ===\n" + AssetHelp.value();
			}
			else
			{
				Out += "\nNo asset-specific methods. Use get/set/list_properties for any property.\n";
			}

			Session.Log(FString::Printf(TEXT("[OK] help()\n%s"), UTF8_TO_TCHAR(Out.c_str())));
			return sol::make_object(Lua, Out);
		});

		// ----------------------------------------------------------------
		// asset:info() -> table (default: type + property summary, enrichments override)
		// ----------------------------------------------------------------
		AssetObj.set_function("info", [WeakAsset, FPath, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();
			Result["type"] = TCHAR_TO_UTF8(*Asset->GetClass()->GetName());
			Result["path"] = TCHAR_TO_UTF8(*FPath);

			// Count editable properties
			int32 EditableCount = 0, TotalCount = 0;
			for (TFieldIterator<FProperty> PropIt(Asset->GetClass()); PropIt; ++PropIt)
			{
				if ((*PropIt)->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient)) continue;
				TotalCount++;
				if ((*PropIt)->HasAnyPropertyFlags(CPF_Edit)) EditableCount++;
			}
			Result["editable_properties"] = EditableCount;
			Result["total_properties"] = TotalCount;

			Session.Log(FString::Printf(TEXT("[OK] info() -> %s (%d editable, %d total properties)"),
				*Asset->GetClass()->GetName(), EditableCount, TotalCount));
			return Result;
		});

		// ----------------------------------------------------------------
		// asset:save() -> true
		// ----------------------------------------------------------------
		AssetObj.set_function("save", [WeakAsset, FPath, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] save -> asset no longer valid"));
				return sol::lua_nil;
			}

			UPackage* Package = Asset->GetOutermost();
			if (!Package)
			{
				Session.Log(TEXT("[FAIL] save -> no package"));
				return sol::lua_nil;
			}

			FString PackageFilename;
			if (!FPackageName::DoesPackageExist(Package->GetName(), &PackageFilename))
			{
				PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(),
					FPackageName::GetAssetPackageExtension());
			}

			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Standalone;
			// Use a non-fatal error device — the default GError calls appError() which
			// terminates the editor on illegal references (e.g. cross-package refs)
			SaveArgs.Error = GWarn;
			FSavePackageResultStruct SaveResult = UPackage::Save(Package, Asset, *PackageFilename, SaveArgs);

			bool bSuccess = (SaveResult.Result == ESavePackageResult::Success);
			if (bSuccess)
			{
				Session.Log(FString::Printf(TEXT("[OK] save(\"%s\")"), *FPath));
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[FAIL] save(\"%s\") -> save failed (check log for details)"), *FPath));
			}

			return sol::make_object(Lua, bSuccess);
		});

		// ----------------------------------------------------------------
		// Enrich with domain-specific methods based on asset type
		// Order matters: check UAnimMontage before UAnimSequenceBase (inheritance)
		// ----------------------------------------------------------------
		auto TryEnrich = [&](const char* FuncName)
		{
			sol::protected_function Fn = LuaView[FuncName];
			if (Fn.valid()) Fn(AssetObj);
		};

		if (Asset->IsA<UAnimMontage>())
			TryEnrich("_enrich_montage");
		else if (Asset->IsA<UAnimComposite>())
			TryEnrich("_enrich_anim_composite");
		else if (Asset->IsA<UAnimSequence>())
			TryEnrich("_enrich_anim_sequence");

		// Get class name once for runtime type checks (plugins no longer linked by core)
		const FString ClassName = Asset->GetClass()->GetName();

		if (Asset->IsA<USkeleton>())
			TryEnrich("_enrich_skeleton");
		else if (Asset->IsA<UPhysicsAsset>())
			TryEnrich("_enrich_physics_asset");
		else if (ClassName == TEXT("IKRigDefinition"))
			TryEnrich("_enrich_ikrig");
		else if (Asset->IsA<UBehaviorTree>() || Asset->IsA<UBlackboardData>())
			TryEnrich("_enrich_behavior_tree");
		else if (ClassName == TEXT("StateTree"))
			TryEnrich("_enrich_state_tree");
		else if (Asset->IsA<ULevelSequence>())
			TryEnrich("_enrich_sequencer");
		else if (Asset->IsA<UPoseAsset>())
			TryEnrich("_enrich_pose_asset");
		else if (Asset->IsA<UBlendSpace>())
			TryEnrich("_enrich_blend_space");
		else if (ClassName == TEXT("NiagaraSystem"))
			TryEnrich("_enrich_niagara");
		else if (ClassName == TEXT("InputAction"))
			TryEnrich("_enrich_input_action");
		else if (ClassName == TEXT("InputMappingContext"))
			TryEnrich("_enrich_mapping_context");
		else if (ClassName == TEXT("IKRetargeter"))
			TryEnrich("_enrich_ik_retargeter");
		else if (Asset->IsA<UUserDefinedStruct>())
			TryEnrich("_enrich_user_defined_struct");
		else if (Asset->IsA<UUserDefinedEnum>())
			TryEnrich("_enrich_user_defined_enum");
		else if (Asset->IsA<UDataTable>())
			TryEnrich("_enrich_data_table");
		else if (Asset->IsA<UStringTable>())
			TryEnrich("_enrich_string_table");
		else if (ClassName == TEXT("ChooserTable"))
			TryEnrich("_enrich_chooser_table");
		else if (Asset->IsA<UCurveTable>())
			TryEnrich("_enrich_curve_table");
		else if (Asset->IsA<UCurveBase>())
			TryEnrich("_enrich_curve");
		else if (Asset->IsA<UMaterialFunction>())
			TryEnrich("_enrich_material_function");
		else if (Asset->IsA<UMaterialInstance>())
			TryEnrich("_enrich_material_instance");
		else if (Asset->IsA<UStaticMesh>())
			TryEnrich("_enrich_static_mesh");
		else if (Asset->IsA<USkeletalMesh>())
			TryEnrich("_enrich_skeletal_mesh");
		else if (Asset->IsA<UTexture>())
			TryEnrich("_enrich_texture");
		else if (ClassName == TEXT("EnvQuery"))
			TryEnrich("_enrich_eqs");
		else if (Asset->IsA<UPhysicalMaterial>())
			TryEnrich("_enrich_physical_material");
		else if (Asset->IsA<UMaterialParameterCollection>())
			TryEnrich("_enrich_material_param_collection");
		else if (Asset->IsA<USoundWave>())
			TryEnrich("_enrich_sound_wave");
		else if (Asset->IsA<USoundClass>())
			TryEnrich("_enrich_sound_class");
		else if (Asset->IsA<USoundAttenuation>())
			TryEnrich("_enrich_sound_attenuation");
		else if (Asset->IsA<USoundCue>())
			TryEnrich("_enrich_sound_cue");
		else if (Asset->IsA<USoundConcurrency>())
			TryEnrich("_enrich_sound_concurrency");
		else if (Asset->IsA<USoundMix>())
			TryEnrich("_enrich_sound_mix");
		else if (Asset->IsA<URuntimeVirtualTexture>())
			TryEnrich("_enrich_runtime_virtual_texture");
		else if (ClassName == TEXT("SmartObjectDefinition"))
			TryEnrich("_enrich_smart_object");
		else if (ClassName == TEXT("PaperSprite"))
			TryEnrich("_enrich_paper_sprite");
		else if (ClassName == TEXT("PaperFlipbook"))
			TryEnrich("_enrich_paper_flipbook");
		else if (ClassName == TEXT("PaperTileSet"))
			TryEnrich("_enrich_paper_tile_set");
		else if (ClassName == TEXT("PaperTileMap"))
			TryEnrich("_enrich_paper_tile_map");
		else if (ClassName == TEXT("PaperSpriteAtlas"))
			TryEnrich("_enrich_paper_sprite_atlas");
		else if (ClassName == TEXT("PaperTerrainMaterial"))
			TryEnrich("_enrich_paper_terrain_material");
		else if (Asset->IsA<UFoliageType>())
			TryEnrich("_enrich_foliage_type");
		else if (Asset->IsA<ULandscapeGrassType>())
			TryEnrich("_enrich_landscape_grass_type");
		else if (Asset->IsA<UDialogueVoice>())
			TryEnrich("_enrich_dialogue_voice");
		else if (Asset->IsA<UDialogueWave>())
			TryEnrich("_enrich_dialogue_wave");
		else if (ClassName == TEXT("PCGGraph") || ClassName == TEXT("PCGGraphInstance"))
			TryEnrich("_enrich_pcg");
		else
		{
			if (const char* PoseSearchEnrichment = LuaAssetHelper::GetPoseSearchEnrichmentFunction(Asset))
			{
				TryEnrich(PoseSearchEnrichment);
			}
		}

		return AssetObj;
	});
}

REGISTER_LUA_BINDING(OpenAsset, OpenAssetDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindOpenAsset(Lua, Session);
});
