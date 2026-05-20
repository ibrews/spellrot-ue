// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "VT/RuntimeVirtualTexture.h"
#include "VT/RuntimeVirtualTextureEnum.h"
#include "PixelFormat.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static const char* MaterialTypeToString(ERuntimeVirtualTextureMaterialType Type)
{
	switch (Type)
	{
	case ERuntimeVirtualTextureMaterialType::BaseColor:                            return "base_color";
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	case ERuntimeVirtualTextureMaterialType::Mask4:                                return "mask4";
#endif
	case ERuntimeVirtualTextureMaterialType::BaseColor_Normal_Roughness:           return "base_color_normal_roughness";
	case ERuntimeVirtualTextureMaterialType::BaseColor_Normal_Specular:            return "base_color_normal_specular";
	case ERuntimeVirtualTextureMaterialType::BaseColor_Normal_Specular_YCoCg:      return "base_color_normal_specular_ycocg";
	case ERuntimeVirtualTextureMaterialType::BaseColor_Normal_Specular_Mask_YCoCg: return "base_color_normal_specular_mask_ycocg";
	case ERuntimeVirtualTextureMaterialType::WorldHeight:                          return "world_height";
	case ERuntimeVirtualTextureMaterialType::Displacement:                         return "displacement";
	default:                                                                       return "unknown";
	}
}

static ERuntimeVirtualTextureMaterialType StringToMaterialType(const std::string& S)
{
	if (S == "base_color")                            return ERuntimeVirtualTextureMaterialType::BaseColor;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	if (S == "mask4")                                 return ERuntimeVirtualTextureMaterialType::Mask4;
#endif
	if (S == "base_color_normal_roughness")           return ERuntimeVirtualTextureMaterialType::BaseColor_Normal_Roughness;
	if (S == "base_color_normal_specular")            return ERuntimeVirtualTextureMaterialType::BaseColor_Normal_Specular;
	if (S == "base_color_normal_specular_ycocg")      return ERuntimeVirtualTextureMaterialType::BaseColor_Normal_Specular_YCoCg;
	if (S == "base_color_normal_specular_mask_ycocg") return ERuntimeVirtualTextureMaterialType::BaseColor_Normal_Specular_Mask_YCoCg;
	if (S == "world_height")                          return ERuntimeVirtualTextureMaterialType::WorldHeight;
	if (S == "displacement")                          return ERuntimeVirtualTextureMaterialType::Displacement;
	return ERuntimeVirtualTextureMaterialType::BaseColor_Normal_Specular; // default
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
static const char* PriorityToString(EVTProducerPriority P)
{
	switch (P)
	{
	case EVTProducerPriority::Lowest:      return "lowest";
	case EVTProducerPriority::Lower:       return "lower";
	case EVTProducerPriority::Low:         return "low";
	case EVTProducerPriority::BelowNormal: return "below_normal";
	case EVTProducerPriority::Normal:      return "normal";
	case EVTProducerPriority::AboveNormal: return "above_normal";
	case EVTProducerPriority::High:        return "high";
	case EVTProducerPriority::Highest:     return "highest";
	default:                               return "normal";
	}
}

static EVTProducerPriority StringToPriority(const std::string& S)
{
	if (S == "lowest")       return EVTProducerPriority::Lowest;
	if (S == "lower")        return EVTProducerPriority::Lower;
	if (S == "low")          return EVTProducerPriority::Low;
	if (S == "below_normal") return EVTProducerPriority::BelowNormal;
	if (S == "normal")       return EVTProducerPriority::Normal;
	if (S == "above_normal") return EVTProducerPriority::AboveNormal;
	if (S == "high")         return EVTProducerPriority::High;
	if (S == "highest")      return EVTProducerPriority::Highest;
	return EVTProducerPriority::Normal;
}
#endif

static FString TextureGroupToString(TEnumAsByte<TextureGroup> Group)
{
	UEnum* Enum = StaticEnum<TextureGroup>();
	if (!Enum) return TEXT("World");
	FString Name = Enum->GetNameStringByValue((int64)Group.GetValue());
	// Strip "TEXTUREGROUP_" prefix for cleaner Lua interface
	Name.RemoveFromStart(TEXT("TEXTUREGROUP_"));
	return Name;
}

static TEnumAsByte<TextureGroup> StringToTextureGroup(const std::string& S)
{
	UEnum* Enum = StaticEnum<TextureGroup>();
	if (!Enum) return TEXTUREGROUP_World;
	FString Prefixed = FString::Printf(TEXT("TEXTUREGROUP_%s"), UTF8_TO_TCHAR(S.c_str()));
	int64 Val = Enum->GetValueByNameString(Prefixed);
	if (Val == INDEX_NONE) return TEXTUREGROUP_World;
	return static_cast<TextureGroup>(Val);
}

// Protected member access via property reflection — static caching per property
static int32* GetMutableTileCount(URuntimeVirtualTexture* RVT)
{
	static FProperty* Prop = URuntimeVirtualTexture::StaticClass()->FindPropertyByName(TEXT("TileCount"));
	return Prop ? Prop->ContainerPtrToValuePtr<int32>(RVT) : nullptr;
}

static int32* GetMutableTileSize(URuntimeVirtualTexture* RVT)
{
	static FProperty* Prop = URuntimeVirtualTexture::StaticClass()->FindPropertyByName(TEXT("TileSize"));
	return Prop ? Prop->ContainerPtrToValuePtr<int32>(RVT) : nullptr;
}

static int32* GetMutableTileBorderSize(URuntimeVirtualTexture* RVT)
{
	static FProperty* Prop = URuntimeVirtualTexture::StaticClass()->FindPropertyByName(TEXT("TileBorderSize"));
	return Prop ? Prop->ContainerPtrToValuePtr<int32>(RVT) : nullptr;
}

static ERuntimeVirtualTextureMaterialType* GetMutableMaterialType(URuntimeVirtualTexture* RVT)
{
	static FProperty* Prop = URuntimeVirtualTexture::StaticClass()->FindPropertyByName(TEXT("MaterialType"));
	return Prop ? Prop->ContainerPtrToValuePtr<ERuntimeVirtualTextureMaterialType>(RVT) : nullptr;
}

static int32* GetMutableRemoveLowMips(URuntimeVirtualTexture* RVT)
{
	static FProperty* Prop = URuntimeVirtualTexture::StaticClass()->FindPropertyByName(TEXT("RemoveLowMips"));
	return Prop ? Prop->ContainerPtrToValuePtr<int32>(RVT) : nullptr;
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
static FVector4f* GetMutableCustomMaterialData(URuntimeVirtualTexture* RVT)
{
	static FProperty* Prop = URuntimeVirtualTexture::StaticClass()->FindPropertyByName(TEXT("CustomMaterialData"));
	return Prop ? Prop->ContainerPtrToValuePtr<FVector4f>(RVT) : nullptr;
}
#endif

static TEnumAsByte<TextureGroup>* GetMutableLODGroup(URuntimeVirtualTexture* RVT)
{
	static FProperty* Prop = URuntimeVirtualTexture::StaticClass()->FindPropertyByName(TEXT("LODGroup"));
	return Prop ? Prop->ContainerPtrToValuePtr<TEnumAsByte<TextureGroup>>(RVT) : nullptr;
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
static EVTProducerPriority* GetMutableCustomPriority(URuntimeVirtualTexture* RVT)
{
	static FProperty* Prop = URuntimeVirtualTexture::StaticClass()->FindPropertyByName(TEXT("CustomPriority"));
	return Prop ? Prop->ContainerPtrToValuePtr<EVTProducerPriority>(RVT) : nullptr;
}
#endif

// Static-cached bool property lookup
static bool* GetMutableBool(URuntimeVirtualTexture* RVT, const TCHAR* PropName)
{
	static TMap<FName, FProperty*> Cache;
	FName Key(PropName);
	FProperty*& Prop = Cache.FindOrAdd(Key);
	if (!Prop)
	{
		Prop = URuntimeVirtualTexture::StaticClass()->FindPropertyByName(PropName);
	}
	return Prop ? Prop->ContainerPtrToValuePtr<bool>(RVT) : nullptr;
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> RuntimeVirtualTextureDocs = {};

static void BindRuntimeVirtualTexture(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_runtime_virtual_texture", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		URuntimeVirtualTexture* RVT = LoadObject<URuntimeVirtualTexture>(nullptr, *FPath);
		if (!RVT) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"RuntimeVirtualTexture enrichment methods:\n"
			"\n"
			"info() — structured summary:\n"
			"  tile_count, tile_size, tile_border_size, size (derived),\n"
			"  page_table_size, material_type, compress_textures,\n"
			"  clear_textures, adaptive, continuous_update,\n"
			"  single_physical_space, private_space, remove_low_mips,\n"
			"  low_quality_compression, layer_count, lod_group,\n"
			"  custom_material_data {x,y,z,w},\n"
			"  use_custom_priority, custom_priority, effective_priority,\n"
			"  layers [{index, format, srgb, ycocg}, ...]\n"
			"\n"
			"configure(params) — set properties (all protected, via reflection):\n"
			"  tile_count (int, raw log2 value, max depends on adaptive),\n"
			"  tile_size (int, raw value 0-4, actual = 2^(N+6)),\n"
			"  tile_border_size (int, raw value 0-4, actual = 2*N),\n"
			"  material_type (string: base_color/mask4/base_color_normal_roughness/\n"
			"    base_color_normal_specular/base_color_normal_specular_ycocg/\n"
			"    base_color_normal_specular_mask_ycocg/world_height/displacement),\n"
			"  compress_textures (bool), clear_textures (bool),\n"
			"  adaptive (bool), continuous_update (bool),\n"
			"  single_physical_space (bool), private_space (bool),\n"
			"  remove_low_mips (int 0-5),\n"
			"  low_quality_compression (bool),\n"
			"  lod_group (string, e.g. 'World', 'Character', 'Effects'),\n"
			"  custom_material_data ({x,y,z,w} table),\n"
			"  use_custom_priority (bool),\n"
			"  custom_priority (string: lowest/lower/low/below_normal/\n"
			"    normal/above_normal/high/highest)\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [RVT, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(RVT))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table R = Lua.create_table();

			R["tile_count"] = RVT->GetTileCount();
			R["tile_size"] = RVT->GetTileSize();
			R["tile_border_size"] = RVT->GetTileBorderSize();
			R["size"] = RVT->GetSize();
			R["page_table_size"] = RVT->GetPageTableSize();
			R["material_type"] = MaterialTypeToString(RVT->GetMaterialType());
			R["compress_textures"] = RVT->GetCompressTextures();
			R["clear_textures"] = RVT->GetClearTextures();
			R["adaptive"] = RVT->GetAdaptivePageTable();
			R["continuous_update"] = RVT->GetContinuousUpdate();
			R["single_physical_space"] = RVT->GetSinglePhysicalSpace();
			R["private_space"] = RVT->GetPrivateSpace();
			R["remove_low_mips"] = RVT->GetRemoveLowMips();
			R["low_quality_compression"] = RVT->GetLQCompression();
			R["layer_count"] = RVT->GetLayerCount();
			R["lod_group"] = std::string(TCHAR_TO_UTF8(*TextureGroupToString(RVT->GetLODGroup())));
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			R["use_custom_priority"] = RVT->GetUseCustomPriority();
			R["custom_priority"] = PriorityToString(RVT->GetPriority());
			R["effective_priority"] = PriorityToString(RVT->GetPriority());

			// Custom material data
			FVector4f CMD = RVT->GetCustomMaterialData();
			sol::table CmdT = Lua.create_table();
			CmdT["x"] = CMD.X;
			CmdT["y"] = CMD.Y;
			CmdT["z"] = CMD.Z;
			CmdT["w"] = CMD.W;
			R["custom_material_data"] = CmdT;
#endif

			// Per-layer info
			int32 LayerCount = RVT->GetLayerCount();
			sol::table Layers = Lua.create_table();
			for (int32 i = 0; i < LayerCount; ++i)
			{
				sol::table Layer = Lua.create_table();
				Layer["index"] = i;
				EPixelFormat Fmt = RVT->GetLayerFormat(i);
				Layer["format"] = std::string(TCHAR_TO_UTF8(GPixelFormats[Fmt].Name));
				Layer["srgb"] = RVT->IsLayerSRGB(i);
				Layer["ycocg"] = RVT->IsLayerYCoCg(i);
				Layers[i + 1] = Layer;
			}
			R["layers"] = Layers;

			Session.Log(FString::Printf(TEXT("[OK] info() -> RuntimeVirtualTexture, size=%d, material=%s, layers=%d"),
				RVT->GetSize(), UTF8_TO_TCHAR(MaterialTypeToString(RVT->GetMaterialType())),
				RVT->GetLayerCount()));
			return R;
		});

		// ================================================================
		// configure(params)
		// ================================================================
		AssetObj.set_function("configure", [RVT, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(RVT))
			{
				Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
				return sol::lua_nil;
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("RuntimeVirtualTexture: Configure")));
			RVT->Modify();
			bool bModified = false;
			FString Changes;

			// adaptive (process before tile_count since it affects clamping)
			sol::optional<bool> AdaptiveVal = Params.get<sol::optional<bool>>("adaptive");
			if (AdaptiveVal.has_value())
			{
				bool* Ptr = GetMutableBool(RVT, TEXT("bAdaptive"));
				if (Ptr)
				{
					*Ptr = AdaptiveVal.value();
					Changes += FString::Printf(TEXT(" adaptive=%s"), AdaptiveVal.value() ? TEXT("true") : TEXT("false"));
					bModified = true;
				}
			}

			// tile_count (raw log2 value, clamped to engine max based on current adaptive state)
			sol::optional<int> TC = Params.get<sol::optional<int>>("tile_count");
			if (TC.has_value())
			{
				int32* Ptr = GetMutableTileCount(RVT);
				if (Ptr)
				{
					int32 MaxLog2 = URuntimeVirtualTexture::GetMaxTileCountLog2(RVT->GetAdaptivePageTable());
					*Ptr = FMath::Clamp(TC.value(), 0, MaxLog2);
					Changes += FString::Printf(TEXT(" tile_count=%d(actual=%d)"), *Ptr, RVT->GetTileCount());
					bModified = true;
				}
			}

			// tile_size (raw value)
			sol::optional<int> TS = Params.get<sol::optional<int>>("tile_size");
			if (TS.has_value())
			{
				int32* Ptr = GetMutableTileSize(RVT);
				if (Ptr)
				{
					*Ptr = FMath::Clamp(TS.value(), 0, 4);
					Changes += FString::Printf(TEXT(" tile_size=%d(actual=%d)"), *Ptr, RVT->GetTileSize());
					bModified = true;
				}
			}

			// tile_border_size (raw value)
			sol::optional<int> TBS = Params.get<sol::optional<int>>("tile_border_size");
			if (TBS.has_value())
			{
				int32* Ptr = GetMutableTileBorderSize(RVT);
				if (Ptr)
				{
					*Ptr = FMath::Clamp(TBS.value(), 0, 4);
					Changes += FString::Printf(TEXT(" tile_border_size=%d(actual=%d)"), *Ptr, RVT->GetTileBorderSize());
					bModified = true;
				}
			}

			// material_type
			sol::optional<std::string> MT = Params.get<sol::optional<std::string>>("material_type");
			if (MT.has_value())
			{
				ERuntimeVirtualTextureMaterialType* Ptr = GetMutableMaterialType(RVT);
				if (Ptr)
				{
					*Ptr = StringToMaterialType(MT.value());
					Changes += FString::Printf(TEXT(" material_type=%s"), UTF8_TO_TCHAR(MT.value().c_str()));
					bModified = true;
				}
			}

			// Boolean properties (adaptive already handled above)
			auto SetBoolProp = [&](const char* LuaKey, const TCHAR* PropName)
			{
				sol::optional<bool> Val = Params.get<sol::optional<bool>>(LuaKey);
				if (Val.has_value())
				{
					bool* Ptr = GetMutableBool(RVT, PropName);
					if (Ptr)
					{
						*Ptr = Val.value();
						Changes += FString::Printf(TEXT(" %s=%s"), PropName, Val.value() ? TEXT("true") : TEXT("false"));
						bModified = true;
					}
				}
			};

			SetBoolProp("compress_textures", TEXT("bCompressTextures"));
			SetBoolProp("clear_textures", TEXT("bClearTextures"));
			SetBoolProp("continuous_update", TEXT("bContinuousUpdate"));
			SetBoolProp("single_physical_space", TEXT("bSinglePhysicalSpace"));
			SetBoolProp("private_space", TEXT("bPrivateSpace"));
			SetBoolProp("low_quality_compression", TEXT("bUseLowQualityCompression"));
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			SetBoolProp("use_custom_priority", TEXT("bUseCustomPriority"));
#endif

			// remove_low_mips
			sol::optional<int> RLM = Params.get<sol::optional<int>>("remove_low_mips");
			if (RLM.has_value())
			{
				int32* Ptr = GetMutableRemoveLowMips(RVT);
				if (Ptr)
				{
					*Ptr = FMath::Clamp(RLM.value(), 0, 5);
					Changes += FString::Printf(TEXT(" remove_low_mips=%d"), *Ptr);
					bModified = true;
				}
			}

			// lod_group
			sol::optional<std::string> LG = Params.get<sol::optional<std::string>>("lod_group");
			if (LG.has_value())
			{
				TEnumAsByte<TextureGroup>* Ptr = GetMutableLODGroup(RVT);
				if (Ptr)
				{
					*Ptr = StringToTextureGroup(LG.value());
					Changes += FString::Printf(TEXT(" lod_group=%s"), UTF8_TO_TCHAR(LG.value().c_str()));
					bModified = true;
				}
			}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			// custom_priority
			sol::optional<std::string> CP = Params.get<sol::optional<std::string>>("custom_priority");
			if (CP.has_value())
			{
				EVTProducerPriority* Ptr = GetMutableCustomPriority(RVT);
				if (Ptr)
				{
					*Ptr = StringToPriority(CP.value());
					Changes += FString::Printf(TEXT(" custom_priority=%s"), UTF8_TO_TCHAR(CP.value().c_str()));
					bModified = true;
				}
			}

			// custom_material_data
			sol::optional<sol::table> CMD = Params.get<sol::optional<sol::table>>("custom_material_data");
			if (CMD.has_value())
			{
				FVector4f* Ptr = GetMutableCustomMaterialData(RVT);
				if (Ptr)
				{
					sol::table T = CMD.value();
					Ptr->X = T.get_or("x", 0.0f);
					Ptr->Y = T.get_or("y", 0.0f);
					Ptr->Z = T.get_or("z", 0.0f);
					Ptr->W = T.get_or("w", 0.0f);
					Changes += FString::Printf(TEXT(" custom_material_data=(%.2f,%.2f,%.2f,%.2f)"),
						Ptr->X, Ptr->Y, Ptr->Z, Ptr->W);
					bModified = true;
				}
			}
#endif

			if (bModified)
			{
				RVT->PostEditChange();
				RVT->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(%s)"), *Changes.TrimStart()));
				return sol::make_object(Lua, true);
			}

			Session.Log(TEXT("[OK] configure() -> nothing changed. Use help() to see valid keys."));
			return sol::make_object(Lua, true);
		});
	});
}

REGISTER_LUA_BINDING(RuntimeVirtualTexture, RuntimeVirtualTextureDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindRuntimeVirtualTexture(Lua, Session);
});
