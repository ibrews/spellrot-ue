// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"

#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCube.h"
#include "Engine/TextureDefines.h"
#include "Engine/VolumeTexture.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static const char* CompressionSettingsToString(TextureCompressionSettings CS)
{
	switch (CS)
	{
	case TC_Default:              return "Default";
	case TC_Normalmap:            return "Normalmap";
	case TC_Masks:                return "Masks";
	case TC_Grayscale:            return "Grayscale";
	case TC_Displacementmap:      return "Displacementmap";
	case TC_VectorDisplacementmap:return "VectorDisplacementmap";
	case TC_HDR:                  return "HDR";
	case TC_EditorIcon:           return "EditorIcon";
	case TC_Alpha:                return "Alpha";
	case TC_DistanceFieldFont:    return "DistanceFieldFont";
	case TC_HDR_Compressed:       return "HDR_Compressed";
	case TC_BC7:                  return "BC7";
	case TC_HalfFloat:            return "HalfFloat";
	case TC_LQ:                   return "LQ";
	case TC_SingleFloat:          return "SingleFloat";
	case TC_HDR_F32:              return "HDR_F32";
	default:                      return "Unknown";
	}
}

static TextureCompressionSettings ParseCompressionSettings(const FString& Str)
{
	if (Str.Equals(TEXT("Default"), ESearchCase::IgnoreCase))              return TC_Default;
	if (Str.Equals(TEXT("Normalmap"), ESearchCase::IgnoreCase))            return TC_Normalmap;
	if (Str.Equals(TEXT("Masks"), ESearchCase::IgnoreCase))                return TC_Masks;
	if (Str.Equals(TEXT("Grayscale"), ESearchCase::IgnoreCase))            return TC_Grayscale;
	if (Str.Equals(TEXT("Displacementmap"), ESearchCase::IgnoreCase))      return TC_Displacementmap;
	if (Str.Equals(TEXT("VectorDisplacementmap"), ESearchCase::IgnoreCase))return TC_VectorDisplacementmap;
	if (Str.Equals(TEXT("HDR"), ESearchCase::IgnoreCase))                  return TC_HDR;
	if (Str.Equals(TEXT("EditorIcon"), ESearchCase::IgnoreCase))           return TC_EditorIcon;
	if (Str.Equals(TEXT("Alpha"), ESearchCase::IgnoreCase))                return TC_Alpha;
	if (Str.Equals(TEXT("DistanceFieldFont"), ESearchCase::IgnoreCase))    return TC_DistanceFieldFont;
	if (Str.Equals(TEXT("HDR_Compressed"), ESearchCase::IgnoreCase))       return TC_HDR_Compressed;
	if (Str.Equals(TEXT("BC7"), ESearchCase::IgnoreCase))                  return TC_BC7;
	if (Str.Equals(TEXT("HalfFloat"), ESearchCase::IgnoreCase))            return TC_HalfFloat;
	if (Str.Equals(TEXT("LQ"), ESearchCase::IgnoreCase))                   return TC_LQ;
	if (Str.Equals(TEXT("SingleFloat"), ESearchCase::IgnoreCase))          return TC_SingleFloat;
	if (Str.Equals(TEXT("HDR_F32"), ESearchCase::IgnoreCase))              return TC_HDR_F32;
	return TC_Default;
}

static const char* FilterToString(TextureFilter F)
{
	switch (F)
	{
	case TF_Nearest:  return "Nearest";
	case TF_Bilinear: return "Bilinear";
	case TF_Trilinear:return "Trilinear";
	case TF_Default:  return "Default";
	default:          return "Unknown";
	}
}

static TextureFilter ParseFilter(const FString& Str)
{
	if (Str.Equals(TEXT("Nearest"), ESearchCase::IgnoreCase))   return TF_Nearest;
	if (Str.Equals(TEXT("Bilinear"), ESearchCase::IgnoreCase))  return TF_Bilinear;
	if (Str.Equals(TEXT("Trilinear"), ESearchCase::IgnoreCase)) return TF_Trilinear;
	return TF_Default;
}

static const char* AddressToString(TextureAddress A)
{
	switch (A)
	{
	case TA_Wrap:   return "Wrap";
	case TA_Clamp:  return "Clamp";
	case TA_Mirror: return "Mirror";
	default:        return "Unknown";
	}
}

static TextureAddress ParseAddress(const FString& Str)
{
	if (Str.Equals(TEXT("Wrap"), ESearchCase::IgnoreCase))   return TA_Wrap;
	if (Str.Equals(TEXT("Clamp"), ESearchCase::IgnoreCase))  return TA_Clamp;
	if (Str.Equals(TEXT("Mirror"), ESearchCase::IgnoreCase)) return TA_Mirror;
	return TA_Wrap;
}

static const char* MipGenToString(TextureMipGenSettings M)
{
	switch (M)
	{
	case TMGS_FromTextureGroup:  return "FromTextureGroup";
	case TMGS_SimpleAverage:     return "SimpleAverage";
	case TMGS_Sharpen0:          return "Sharpen0";
	case TMGS_Sharpen1:          return "Sharpen1";
	case TMGS_Sharpen2:          return "Sharpen2";
	case TMGS_Sharpen3:          return "Sharpen3";
	case TMGS_Sharpen4:          return "Sharpen4";
	case TMGS_Sharpen5:          return "Sharpen5";
	case TMGS_Sharpen6:          return "Sharpen6";
	case TMGS_Sharpen7:          return "Sharpen7";
	case TMGS_Sharpen8:          return "Sharpen8";
	case TMGS_Sharpen9:          return "Sharpen9";
	case TMGS_Sharpen10:         return "Sharpen10";
	case TMGS_NoMipmaps:         return "NoMipmaps";
	case TMGS_LeaveExistingMips: return "LeaveExistingMips";
	case TMGS_Blur1:             return "Blur1";
	case TMGS_Blur2:             return "Blur2";
	case TMGS_Blur3:             return "Blur3";
	case TMGS_Blur4:             return "Blur4";
	case TMGS_Blur5:             return "Blur5";
	case TMGS_Unfiltered:        return "Unfiltered";
	case TMGS_Angular:           return "Angular";
	default:                     return "Unknown";
	}
}

static TextureMipGenSettings ParseMipGen(const FString& Str)
{
	if (Str.Equals(TEXT("FromTextureGroup"), ESearchCase::IgnoreCase))  return TMGS_FromTextureGroup;
	if (Str.Equals(TEXT("SimpleAverage"), ESearchCase::IgnoreCase))     return TMGS_SimpleAverage;
	if (Str.Equals(TEXT("NoMipmaps"), ESearchCase::IgnoreCase))         return TMGS_NoMipmaps;
	if (Str.Equals(TEXT("LeaveExistingMips"), ESearchCase::IgnoreCase)) return TMGS_LeaveExistingMips;
	if (Str.Equals(TEXT("Unfiltered"), ESearchCase::IgnoreCase))        return TMGS_Unfiltered;
	if (Str.Equals(TEXT("Angular"), ESearchCase::IgnoreCase))           return TMGS_Angular;
	if (Str.Contains(TEXT("Sharpen")))
	{
		int32 Level = FCString::Atoi(*Str.Replace(TEXT("Sharpen"), TEXT("")));
		Level = FMath::Clamp(Level, 0, 10);
		return static_cast<TextureMipGenSettings>(TMGS_Sharpen0 + Level);
	}
	if (Str.Contains(TEXT("Blur")))
	{
		int32 Level = FCString::Atoi(*Str.Replace(TEXT("Blur"), TEXT("")));
		Level = FMath::Clamp(Level, 1, 5);
		return static_cast<TextureMipGenSettings>(TMGS_Blur1 + Level - 1);
	}
	return TMGS_FromTextureGroup;
}

static const char* LossyCompressionToString(ETextureLossyCompressionAmount A)
{
	switch (A)
	{
	case TLCA_Default: return "Default";
	case TLCA_None:    return "None";
	case TLCA_Lowest:  return "Lowest";
	case TLCA_Low:     return "Low";
	case TLCA_Medium:  return "Medium";
	case TLCA_High:    return "High";
	case TLCA_Highest: return "Highest";
	default:           return "Unknown";
	}
}

static const char* PowerOfTwoToString(ETexturePowerOfTwoSetting::Type P)
{
	switch (P)
	{
	case ETexturePowerOfTwoSetting::None:                       return "None";
	case ETexturePowerOfTwoSetting::PadToPowerOfTwo:            return "PadToPowerOfTwo";
	case ETexturePowerOfTwoSetting::PadToSquarePowerOfTwo:      return "PadToSquarePowerOfTwo";
	case ETexturePowerOfTwoSetting::StretchToPowerOfTwo:        return "StretchToPowerOfTwo";
	case ETexturePowerOfTwoSetting::StretchToSquarePowerOfTwo:  return "StretchToSquarePowerOfTwo";
	case ETexturePowerOfTwoSetting::ResizeToSpecificResolution: return "ResizeToSpecificResolution";
	default:                                                    return "Unknown";
	}
}

static const char* CompositeTextureToString(ECompositeTextureMode M)
{
	switch (M)
	{
	case CTM_Disabled:                 return "Disabled";
	case CTM_NormalRoughnessToRed:     return "NormalRoughnessToRed";
	case CTM_NormalRoughnessToGreen:   return "NormalRoughnessToGreen";
	case CTM_NormalRoughnessToBlue:    return "NormalRoughnessToBlue";
	case CTM_NormalRoughnessToAlpha:   return "NormalRoughnessToAlpha";
	default:                           return "Unknown";
	}
}

static const char* CompressionQualityToString(ETextureCompressionQuality Q)
{
	switch (Q)
	{
	case TCQ_Default: return "Default";
	case TCQ_Lowest:  return "Lowest";
	case TCQ_Low:     return "Low";
	case TCQ_Medium:  return "Medium";
	case TCQ_High:    return "High";
	case TCQ_Highest: return "Highest";
	default:          return "Unknown";
	}
}

static const char* AvailabilityToString(ETextureAvailability A)
{
	switch (A)
	{
	case ETextureAvailability::GPU: return "GPU";
	case ETextureAvailability::CPU: return "CPU";
	default:                        return "Unknown";
	}
}

static const char* MipLoadOptionsToString(ETextureMipLoadOptions O)
{
	switch (O)
	{
	case ETextureMipLoadOptions::Default:      return "Default";
	case ETextureMipLoadOptions::AllMips:       return "AllMips";
	case ETextureMipLoadOptions::OnlyFirstMip:  return "OnlyFirstMip";
	default:                                    return "Unknown";
	}
}

static const char* CookTilingToString(TextureCookPlatformTilingSettings T)
{
	switch (T)
	{
	case TCPTS_FromTextureGroup: return "FromTextureGroup";
	case TCPTS_Tile:             return "Tile";
	case TCPTS_DoNotTile:        return "DoNotTile";
	default:                     return "Unknown";
	}
}

static const char* DownscaleOptionsToString(ETextureDownscaleOptions D)
{
	switch (D)
	{
	case ETextureDownscaleOptions::Default:       return "Default";
	case ETextureDownscaleOptions::Unfiltered:    return "Unfiltered";
	case ETextureDownscaleOptions::SimpleAverage: return "SimpleAverage";
	case ETextureDownscaleOptions::Sharpen0:      return "Sharpen0";
	case ETextureDownscaleOptions::Sharpen1:      return "Sharpen1";
	case ETextureDownscaleOptions::Sharpen2:      return "Sharpen2";
	case ETextureDownscaleOptions::Sharpen3:      return "Sharpen3";
	case ETextureDownscaleOptions::Sharpen4:      return "Sharpen4";
	case ETextureDownscaleOptions::Sharpen5:      return "Sharpen5";
	case ETextureDownscaleOptions::Sharpen6:      return "Sharpen6";
	case ETextureDownscaleOptions::Sharpen7:      return "Sharpen7";
	case ETextureDownscaleOptions::Sharpen8:      return "Sharpen8";
	case ETextureDownscaleOptions::Sharpen9:      return "Sharpen9";
	case ETextureDownscaleOptions::Sharpen10:     return "Sharpen10";
	default:                                      return "Unknown";
	}
}

// Match LOD group by exact name after stripping TEXTUREGROUP_ prefix
static bool TryParseLODGroup(const FString& Str, TextureGroup& OutGroup)
{
	for (int32 i = 0; i < TEXTUREGROUP_MAX; i++)
	{
		FString GroupName = UTexture::GetTextureGroupString(static_cast<TextureGroup>(i));
		// Strip TEXTUREGROUP_ prefix for exact match
		FString ShortName = GroupName;
		ShortName.RemoveFromStart(TEXT("TEXTUREGROUP_"));
		if (ShortName.Equals(Str, ESearchCase::IgnoreCase) || GroupName.Equals(Str, ESearchCase::IgnoreCase))
		{
			OutGroup = static_cast<TextureGroup>(i);
			return true;
		}
	}
	return false;
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> TextureDocs = {};

static void BindTexture(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_texture", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UTexture* Texture = LoadObject<UTexture>(nullptr, *FPath);
		if (!Texture) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"Texture enrichment.\n"
			"\n"
			"info() — texture summary (size, format, compression, LOD group, mips, source info, color settings, etc.)\n"
			"\n"
			"list(type):\n"
			"  list(\"settings\")    — all current texture settings\n"
			"  list(\"adjustments\") — brightness, vibrance, saturation, hue adjustments\n"
			"  list(\"advanced\")    — advanced compression, padding, availability, downscale settings\n"
			"\n"
			"configure(params):\n"
			"  configure({compression?, srgb?, filter?, lod_group?, max_texture_size?,\n"
			"    address_x?, address_y?, address_mode?,\n"
			"    mip_gen?, virtual_texture_streaming?,\n"
			"    lod_bias?, adjust_brightness?, adjust_vibrance?, adjust_saturation?,\n"
			"    adjust_hue?, adjust_rgb_curve?, adjust_min_alpha?, adjust_max_alpha?,\n"
			"    chroma_key?, chroma_key_threshold?, chroma_key_color?,\n"
			"    flip_green_channel?, normalize_normals?, preserve_border?,\n"
			"    lossy_compression_amount?, compression_no_alpha?, compression_force_alpha?,\n"
			"    compression_none?, compression_quality?,\n"
			"    color_space?, encoding_override?,\n"
			"    power_of_two_mode?, padding_color?, pad_with_border_color?,\n"
			"    resize_x?, resize_y?,\n"
			"    composite_texture_mode?, composite_power?,\n"
			"    availability?, mip_load_options?, cook_tiling?,\n"
			"    use_new_mip_filter?, use_legacy_gamma?,\n"
			"    alpha_coverage?, alpha_coverage_thresholds?,\n"
			"    oodle_preserve_extremes?, downscale_options?})\n"
			"\n"
			"  compression: Default, Normalmap, Masks, Grayscale, HDR, Alpha, BC7, HalfFloat, etc.\n"
			"  filter: Default, Nearest, Bilinear, Trilinear\n"
			"  address: Wrap, Clamp, Mirror\n"
			"  mip_gen: FromTextureGroup, SimpleAverage, NoMipmaps, Sharpen0-10, Blur1-5, Unfiltered, Angular\n"
			"  lossy_compression_amount: Default, None, Lowest, Low, Medium, High, Highest\n"
			"  compression_quality: Default, Lowest, Low, Medium, High, Highest\n"
			"  power_of_two_mode: None, PadToPowerOfTwo, PadToSquarePowerOfTwo, StretchToPowerOfTwo, StretchToSquarePowerOfTwo, ResizeToSpecificResolution\n"
			"  composite_texture_mode: Disabled, NormalRoughnessToRed/Green/Blue/Alpha\n"
			"  availability: GPU, CPU\n"
			"  mip_load_options: Default, AllMips, OnlyFirstMip\n"
			"  cook_tiling: FromTextureGroup, Tile, DoNotTile\n"
			"  downscale_options: Default, Unfiltered, SimpleAverage, Sharpen0-10\n"
			"  color_space: TCS_None, TCS_sRGB, TCS_Rec2020, TCS_ACESAP0, TCS_ACESAP1, TCS_P3DCI, TCS_P3D65,\n"
			"    TCS_REDWideGamut, TCS_SonySGamut3, TCS_SonySGamut3Cine, TCS_AlexaWideGamut,\n"
			"    TCS_CanonCinemaGamut, TCS_GoProProtuneNative, TCS_PanasonicVGamut, TCS_Custom\n"
			"  encoding_override: TSE_None, TSE_Linear, TSE_sRGB, TSE_ST2084, TSE_Gamma22, TSE_BT1886,\n"
			"    TSE_Gamma26, TSE_Cineon, TSE_REDLog, TSE_REDLog3G10, TSE_SLog1, TSE_SLog2, TSE_SLog3,\n"
			"    TSE_AlexaV3LogC, TSE_CanonLog, TSE_ProTune, TSE_VLog\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [Texture, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Texture)) { Session.Log(TEXT("[FAIL] info -> asset no longer valid")); return sol::lua_nil; }

			sol::table Result = Lua.create_table();
			Result["name"] = TCHAR_TO_UTF8(*Texture->GetName());
			Result["path"] = TCHAR_TO_UTF8(*Texture->GetPathName());
			Result["class"] = TCHAR_TO_UTF8(*Texture->GetClass()->GetName());

			// Type-specific size info
			UTexture2D* Tex2D = Cast<UTexture2D>(Texture);
			UTextureCube* TexCube = Cast<UTextureCube>(Texture);
			UVolumeTexture* TexVol = Cast<UVolumeTexture>(Texture);
			if (Tex2D)
			{
				Result["width"] = Tex2D->GetSizeX();
				Result["height"] = Tex2D->GetSizeY();
				Result["num_mips"] = Tex2D->GetNumMips();
				Result["address_x"] = AddressToString(static_cast<TextureAddress>(Tex2D->AddressX.GetValue()));
				Result["address_y"] = AddressToString(static_cast<TextureAddress>(Tex2D->AddressY.GetValue()));
			}
			else if (TexCube)
			{
				Result["width"] = TexCube->GetSizeX();
				Result["height"] = TexCube->GetSizeY();
				Result["num_mips"] = TexCube->GetNumMips();
				Result["num_faces"] = 6;
			}
			else if (TexVol)
			{
				Result["width"] = TexVol->GetSizeX();
				Result["height"] = TexVol->GetSizeY();
				Result["depth"] = TexVol->GetSizeZ();
				Result["num_mips"] = TexVol->GetNumMips();
				Result["address_mode"] = AddressToString(static_cast<TextureAddress>(TexVol->AddressMode.GetValue()));
			}

			// Common texture properties (runtime — always available)
			Result["compression"] = CompressionSettingsToString(static_cast<TextureCompressionSettings>(Texture->CompressionSettings.GetValue()));
			Result["srgb"] = static_cast<bool>(Texture->SRGB);
			Result["filter"] = FilterToString(static_cast<TextureFilter>(Texture->Filter.GetValue()));
			Result["lod_group"] = TCHAR_TO_UTF8(UTexture::GetTextureGroupString(static_cast<TextureGroup>(Texture->LODGroup.GetValue())));
			Result["lod_bias"] = Texture->LODBias;
			Result["virtual_texture_streaming"] = static_cast<bool>(Texture->VirtualTextureStreaming);
			Result["availability"] = AvailabilityToString(Texture->Availability);
			Result["mip_load_options"] = MipLoadOptionsToString(Texture->MipLoadOptions);
			Result["cook_tiling"] = CookTilingToString(static_cast<TextureCookPlatformTilingSettings>(Texture->CookPlatformTilingSettings.GetValue()));
			Result["oodle_preserve_extremes"] = Texture->bOodlePreserveExtremes;
			Result["downscale_options"] = DownscaleOptionsToString(Texture->DownscaleOptions);

#if WITH_EDITORONLY_DATA
			Result["max_texture_size"] = Texture->MaxTextureSize;
			Result["mip_gen"] = MipGenToString(static_cast<TextureMipGenSettings>(Texture->MipGenSettings.GetValue()));
			Result["lossy_compression_amount"] = LossyCompressionToString(static_cast<ETextureLossyCompressionAmount>(Texture->LossyCompressionAmount.GetValue()));
			Result["compression_quality"] = CompressionQualityToString(static_cast<ETextureCompressionQuality>(Texture->CompressionQuality.GetValue()));
			Result["compression_no_alpha"] = static_cast<bool>(Texture->CompressionNoAlpha);
			Result["compression_force_alpha"] = static_cast<bool>(Texture->CompressionForceAlpha);
			Result["compression_none"] = static_cast<bool>(Texture->CompressionNone);
			Result["power_of_two_mode"] = PowerOfTwoToString(static_cast<ETexturePowerOfTwoSetting::Type>(Texture->PowerOfTwoMode.GetValue()));

			// Adjustments
			Result["adjust_brightness"] = Texture->AdjustBrightness;
			Result["adjust_brightness_curve"] = Texture->AdjustBrightnessCurve;
			Result["adjust_vibrance"] = Texture->AdjustVibrance;
			Result["adjust_saturation"] = Texture->AdjustSaturation;
			Result["adjust_rgb_curve"] = Texture->AdjustRGBCurve;
			Result["adjust_hue"] = Texture->AdjustHue;
			Result["adjust_min_alpha"] = Texture->AdjustMinAlpha;
			Result["adjust_max_alpha"] = Texture->AdjustMaxAlpha;

			// Flags
			Result["flip_green_channel"] = static_cast<bool>(Texture->bFlipGreenChannel);
			Result["normalize_normals"] = static_cast<bool>(Texture->bNormalizeNormals);
			Result["preserve_border"] = static_cast<bool>(Texture->bPreserveBorder);
			Result["use_new_mip_filter"] = Texture->bUseNewMipFilter;
			Result["use_legacy_gamma"] = static_cast<bool>(Texture->bUseLegacyGamma);
			Result["alpha_coverage"] = Texture->bDoScaleMipsForAlphaCoverage;

			// Chroma key
			Result["chroma_key"] = Texture->bChromaKeyTexture;
			Result["chroma_key_threshold"] = Texture->ChromaKeyThreshold;
			{
				sol::table CK = Lua.create_table();
				CK["r"] = Texture->ChromaKeyColor.R;
				CK["g"] = Texture->ChromaKeyColor.G;
				CK["b"] = Texture->ChromaKeyColor.B;
				CK["a"] = Texture->ChromaKeyColor.A;
				Result["chroma_key_color"] = CK;
			}

			// Composite texture
			Result["composite_texture_mode"] = CompositeTextureToString(static_cast<ECompositeTextureMode>(Texture->CompositeTextureMode.GetValue()));
			Result["composite_power"] = Texture->CompositePower;
			UTexture* CompTex = Texture->GetCompositeTexture();
			if (CompTex)
			{
				Result["composite_texture"] = TCHAR_TO_UTF8(*CompTex->GetPathName());
			}

			// Source texture info
			if (Texture->Source.IsValid())
			{
				Result["source_width"] = Texture->Source.GetSizeX();
				Result["source_height"] = Texture->Source.GetSizeY();
				Result["source_num_mips"] = Texture->Source.GetNumMips();
				Result["source_num_slices"] = Texture->Source.GetNumSlices();
				Result["source_num_layers"] = Texture->Source.GetNumLayers();
				ETextureSourceFormat Fmt = Texture->Source.GetFormat();
				Result["source_format"] = TCHAR_TO_UTF8(*StaticEnum<ETextureSourceFormat>()->GetNameStringByValue(static_cast<int64>(Fmt)));
			}

			// Color management settings
			{
				sol::table ColorInfo = Lua.create_table();
				ColorInfo["color_space"] = TCHAR_TO_UTF8(*StaticEnum<ETextureColorSpace>()->GetNameStringByValue(static_cast<int64>(Texture->SourceColorSettings.ColorSpace)));
				ColorInfo["encoding_override"] = TCHAR_TO_UTF8(*StaticEnum<ETextureSourceEncoding>()->GetNameStringByValue(static_cast<int64>(Texture->SourceColorSettings.EncodingOverride)));
				ColorInfo["chromatic_adaptation"] = TCHAR_TO_UTF8(*StaticEnum<ETextureChromaticAdaptationMethod>()->GetNameStringByValue(static_cast<int64>(Texture->SourceColorSettings.ChromaticAdaptationMethod)));
				Result["color_settings"] = ColorInfo;
			}
#endif

			Session.Log(FString::Printf(TEXT("[OK] info() -> %s (%s)"),
				*Texture->GetName(), *Texture->GetClass()->GetName()));
			return Result;
		});

		// ================================================================
		// list(type)
		// ================================================================
		AssetObj.set_function("list", [Texture, &Session](sol::table Self,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Texture)) { Session.Log(TEXT("[FAIL] list -> asset no longer valid")); return sol::lua_nil; }
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = Self["info"];
				if (InfoFn.valid()) return InfoFn(Self);
				return sol::lua_nil;
			}

			if (FType.Equals(TEXT("settings"), ESearchCase::IgnoreCase))
			{
				sol::table R = Lua.create_table();
				R["compression"] = CompressionSettingsToString(static_cast<TextureCompressionSettings>(Texture->CompressionSettings.GetValue()));
				R["srgb"] = static_cast<bool>(Texture->SRGB);
				R["filter"] = FilterToString(static_cast<TextureFilter>(Texture->Filter.GetValue()));
				R["lod_group"] = TCHAR_TO_UTF8(UTexture::GetTextureGroupString(static_cast<TextureGroup>(Texture->LODGroup.GetValue())));
				R["lod_bias"] = Texture->LODBias;
				R["virtual_texture_streaming"] = static_cast<bool>(Texture->VirtualTextureStreaming);
				R["availability"] = AvailabilityToString(Texture->Availability);
				R["mip_load_options"] = MipLoadOptionsToString(Texture->MipLoadOptions);
				R["cook_tiling"] = CookTilingToString(static_cast<TextureCookPlatformTilingSettings>(Texture->CookPlatformTilingSettings.GetValue()));

				UTexture2D* Tex2D = Cast<UTexture2D>(Texture);
				if (Tex2D)
				{
					R["address_x"] = AddressToString(static_cast<TextureAddress>(Tex2D->AddressX.GetValue()));
					R["address_y"] = AddressToString(static_cast<TextureAddress>(Tex2D->AddressY.GetValue()));
				}
				UVolumeTexture* TexVol = Cast<UVolumeTexture>(Texture);
				if (TexVol)
				{
					R["address_mode"] = AddressToString(static_cast<TextureAddress>(TexVol->AddressMode.GetValue()));
				}

#if WITH_EDITORONLY_DATA
				R["max_texture_size"] = Texture->MaxTextureSize;
				R["mip_gen"] = MipGenToString(static_cast<TextureMipGenSettings>(Texture->MipGenSettings.GetValue()));
#endif

				Session.Log(TEXT("[OK] list(\"settings\")"));
				return R;
			}

			if (FType.Equals(TEXT("adjustments"), ESearchCase::IgnoreCase))
			{
				sol::table R = Lua.create_table();
#if WITH_EDITORONLY_DATA
				R["brightness"] = Texture->AdjustBrightness;
				R["brightness_curve"] = Texture->AdjustBrightnessCurve;
				R["vibrance"] = Texture->AdjustVibrance;
				R["saturation"] = Texture->AdjustSaturation;
				R["rgb_curve"] = Texture->AdjustRGBCurve;
				R["hue"] = Texture->AdjustHue;
				R["min_alpha"] = Texture->AdjustMinAlpha;
				R["max_alpha"] = Texture->AdjustMaxAlpha;
#endif
				Session.Log(TEXT("[OK] list(\"adjustments\")"));
				return R;
			}

			if (FType.Equals(TEXT("advanced"), ESearchCase::IgnoreCase))
			{
				sol::table R = Lua.create_table();
				R["oodle_preserve_extremes"] = Texture->bOodlePreserveExtremes;
				R["downscale_options"] = DownscaleOptionsToString(Texture->DownscaleOptions);
#if WITH_EDITORONLY_DATA
				R["lossy_compression_amount"] = LossyCompressionToString(static_cast<ETextureLossyCompressionAmount>(Texture->LossyCompressionAmount.GetValue()));
				R["compression_quality"] = CompressionQualityToString(static_cast<ETextureCompressionQuality>(Texture->CompressionQuality.GetValue()));
				R["compression_no_alpha"] = static_cast<bool>(Texture->CompressionNoAlpha);
				R["compression_force_alpha"] = static_cast<bool>(Texture->CompressionForceAlpha);
				R["compression_none"] = static_cast<bool>(Texture->CompressionNone);
				R["power_of_two_mode"] = PowerOfTwoToString(static_cast<ETexturePowerOfTwoSetting::Type>(Texture->PowerOfTwoMode.GetValue()));
				R["use_new_mip_filter"] = Texture->bUseNewMipFilter;
				R["use_legacy_gamma"] = static_cast<bool>(Texture->bUseLegacyGamma);
				R["alpha_coverage"] = Texture->bDoScaleMipsForAlphaCoverage;
				R["composite_texture_mode"] = CompositeTextureToString(static_cast<ECompositeTextureMode>(Texture->CompositeTextureMode.GetValue()));
				R["composite_power"] = Texture->CompositePower;
#endif
				Session.Log(TEXT("[OK] list(\"advanced\")"));
				return R;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: settings, adjustments, advanced"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// configure(params)
		// ================================================================
		AssetObj.set_function("configure", [Texture, &Session](sol::table /*Self*/,
			sol::table P, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Texture)) { Session.Log(TEXT("[FAIL] configure -> asset no longer valid")); return sol::lua_nil; }

			Texture->Modify();

			int32 ChangedCount = 0;

			// ---- Runtime properties (always available) ----

			// Compression
			sol::optional<std::string> CompOpt = P.get<sol::optional<std::string>>("compression");
			if (CompOpt.has_value())
			{
				Texture->CompressionSettings = ParseCompressionSettings(UTF8_TO_TCHAR(CompOpt.value().c_str()));
				ChangedCount++;
			}

			// SRGB
			if (P.get<sol::optional<bool>>("srgb").has_value())
			{
				Texture->SRGB = P.get<bool>("srgb");
				ChangedCount++;
			}

			// Filter
			sol::optional<std::string> FilterOpt = P.get<sol::optional<std::string>>("filter");
			if (FilterOpt.has_value())
			{
				Texture->Filter = ParseFilter(UTF8_TO_TCHAR(FilterOpt.value().c_str()));
				ChangedCount++;
			}

			// LOD Group
			sol::optional<std::string> LODGroupOpt = P.get<sol::optional<std::string>>("lod_group");
			if (LODGroupOpt.has_value())
			{
				FString GroupStr = UTF8_TO_TCHAR(LODGroupOpt.value().c_str());
				TextureGroup MatchedGroup;
				if (TryParseLODGroup(GroupStr, MatchedGroup))
				{
					Texture->LODGroup = static_cast<TEnumAsByte<TextureGroup>>(MatchedGroup);
					ChangedCount++;
				}
			}

			// LOD Bias
			if (P.get<sol::optional<int>>("lod_bias").has_value())
			{
				Texture->LODBias = P.get<int>("lod_bias");
				ChangedCount++;
			}

			// Virtual Texture Streaming
			if (P.get<sol::optional<bool>>("virtual_texture_streaming").has_value())
			{
				Texture->VirtualTextureStreaming = P.get<bool>("virtual_texture_streaming");
				ChangedCount++;
			}

			// Availability
			sol::optional<std::string> AvailOpt = P.get<sol::optional<std::string>>("availability");
			if (AvailOpt.has_value())
			{
				FString AvailStr = UTF8_TO_TCHAR(AvailOpt.value().c_str());
				if (AvailStr.Equals(TEXT("GPU"), ESearchCase::IgnoreCase)) { Texture->Availability = ETextureAvailability::GPU; ChangedCount++; }
				else if (AvailStr.Equals(TEXT("CPU"), ESearchCase::IgnoreCase)) { Texture->Availability = ETextureAvailability::CPU; ChangedCount++; }
			}

			// Mip Load Options
			sol::optional<std::string> MipLoadOpt = P.get<sol::optional<std::string>>("mip_load_options");
			if (MipLoadOpt.has_value())
			{
				FString MLStr = UTF8_TO_TCHAR(MipLoadOpt.value().c_str());
				if (MLStr.Equals(TEXT("Default"), ESearchCase::IgnoreCase))     { Texture->MipLoadOptions = ETextureMipLoadOptions::Default; ChangedCount++; }
				else if (MLStr.Equals(TEXT("AllMips"), ESearchCase::IgnoreCase))    { Texture->MipLoadOptions = ETextureMipLoadOptions::AllMips; ChangedCount++; }
				else if (MLStr.Equals(TEXT("OnlyFirstMip"), ESearchCase::IgnoreCase)) { Texture->MipLoadOptions = ETextureMipLoadOptions::OnlyFirstMip; ChangedCount++; }
			}

			// Cook Tiling
			sol::optional<std::string> CookOpt = P.get<sol::optional<std::string>>("cook_tiling");
			if (CookOpt.has_value())
			{
				FString CTStr = UTF8_TO_TCHAR(CookOpt.value().c_str());
				if (CTStr.Equals(TEXT("FromTextureGroup"), ESearchCase::IgnoreCase)) { Texture->CookPlatformTilingSettings = TCPTS_FromTextureGroup; ChangedCount++; }
				else if (CTStr.Equals(TEXT("Tile"), ESearchCase::IgnoreCase))         { Texture->CookPlatformTilingSettings = TCPTS_Tile; ChangedCount++; }
				else if (CTStr.Equals(TEXT("DoNotTile"), ESearchCase::IgnoreCase))    { Texture->CookPlatformTilingSettings = TCPTS_DoNotTile; ChangedCount++; }
			}

			// Address modes (Texture2D only)
			UTexture2D* Tex2D = Cast<UTexture2D>(Texture);
			if (Tex2D)
			{
				sol::optional<std::string> AddrXOpt = P.get<sol::optional<std::string>>("address_x");
				if (AddrXOpt.has_value()) { Tex2D->AddressX = ParseAddress(UTF8_TO_TCHAR(AddrXOpt.value().c_str())); ChangedCount++; }

				sol::optional<std::string> AddrYOpt = P.get<sol::optional<std::string>>("address_y");
				if (AddrYOpt.has_value()) { Tex2D->AddressY = ParseAddress(UTF8_TO_TCHAR(AddrYOpt.value().c_str())); ChangedCount++; }
			}

			// Address mode (VolumeTexture only)
			UVolumeTexture* TexVol = Cast<UVolumeTexture>(Texture);
			if (TexVol)
			{
				sol::optional<std::string> AddrModeOpt = P.get<sol::optional<std::string>>("address_mode");
				if (AddrModeOpt.has_value()) { TexVol->AddressMode = ParseAddress(UTF8_TO_TCHAR(AddrModeOpt.value().c_str())); ChangedCount++; }
			}

			// Oodle preserve extremes
			if (P.get<sol::optional<bool>>("oodle_preserve_extremes").has_value())
			{
				Texture->bOodlePreserveExtremes = P.get<bool>("oodle_preserve_extremes");
				ChangedCount++;
			}

			// Downscale options (runtime property)
			sol::optional<std::string> DSOpt = P.get<sol::optional<std::string>>("downscale_options");
			if (DSOpt.has_value())
			{
				FString DSStr = UTF8_TO_TCHAR(DSOpt.value().c_str());
				int64 EnumVal = StaticEnum<ETextureDownscaleOptions>()->GetValueByNameString(DSStr);
				if (EnumVal == INDEX_NONE)
				{
					EnumVal = StaticEnum<ETextureDownscaleOptions>()->GetValueByNameString(TEXT("ETextureDownscaleOptions::") + DSStr);
				}
				if (EnumVal != INDEX_NONE)
				{
					Texture->DownscaleOptions = static_cast<ETextureDownscaleOptions>(EnumVal);
					ChangedCount++;
				}
			}

			// ---- Editor-only properties ----
#if WITH_EDITORONLY_DATA

			// Max Texture Size
			if (P.get<sol::optional<int>>("max_texture_size").has_value())
			{
				Texture->MaxTextureSize = P.get<int>("max_texture_size");
				ChangedCount++;
			}

			// MipGen
			sol::optional<std::string> MipGenOpt = P.get<sol::optional<std::string>>("mip_gen");
			if (MipGenOpt.has_value())
			{
				Texture->MipGenSettings = ParseMipGen(UTF8_TO_TCHAR(MipGenOpt.value().c_str()));
				ChangedCount++;
			}

			// Adjustments
			if (P.get<sol::optional<double>>("adjust_brightness").has_value())    { Texture->AdjustBrightness = static_cast<float>(P.get<double>("adjust_brightness")); ChangedCount++; }
			if (P.get<sol::optional<double>>("adjust_brightness_curve").has_value()) { Texture->AdjustBrightnessCurve = static_cast<float>(P.get<double>("adjust_brightness_curve")); ChangedCount++; }
			if (P.get<sol::optional<double>>("adjust_vibrance").has_value())      { Texture->AdjustVibrance = static_cast<float>(P.get<double>("adjust_vibrance")); ChangedCount++; }
			if (P.get<sol::optional<double>>("adjust_saturation").has_value())    { Texture->AdjustSaturation = static_cast<float>(P.get<double>("adjust_saturation")); ChangedCount++; }
			if (P.get<sol::optional<double>>("adjust_rgb_curve").has_value())     { Texture->AdjustRGBCurve = static_cast<float>(P.get<double>("adjust_rgb_curve")); ChangedCount++; }
			if (P.get<sol::optional<double>>("adjust_hue").has_value())           { Texture->AdjustHue = static_cast<float>(P.get<double>("adjust_hue")); ChangedCount++; }
			if (P.get<sol::optional<double>>("adjust_min_alpha").has_value())     { Texture->AdjustMinAlpha = static_cast<float>(P.get<double>("adjust_min_alpha")); ChangedCount++; }
			if (P.get<sol::optional<double>>("adjust_max_alpha").has_value())     { Texture->AdjustMaxAlpha = static_cast<float>(P.get<double>("adjust_max_alpha")); ChangedCount++; }

			// Flags
			if (P.get<sol::optional<bool>>("flip_green_channel").has_value())  { Texture->bFlipGreenChannel = P.get<bool>("flip_green_channel"); ChangedCount++; }
			if (P.get<sol::optional<bool>>("normalize_normals").has_value())   { Texture->bNormalizeNormals = P.get<bool>("normalize_normals"); ChangedCount++; }
			if (P.get<sol::optional<bool>>("preserve_border").has_value())     { Texture->bPreserveBorder = P.get<bool>("preserve_border"); ChangedCount++; }
			if (P.get<sol::optional<bool>>("use_new_mip_filter").has_value())  { Texture->bUseNewMipFilter = P.get<bool>("use_new_mip_filter"); ChangedCount++; }
			if (P.get<sol::optional<bool>>("use_legacy_gamma").has_value())    { Texture->bUseLegacyGamma = P.get<bool>("use_legacy_gamma"); ChangedCount++; }

			// Alpha coverage
			if (P.get<sol::optional<bool>>("alpha_coverage").has_value())
			{
				Texture->bDoScaleMipsForAlphaCoverage = P.get<bool>("alpha_coverage");
				ChangedCount++;
			}
			sol::optional<sol::table> ACThreshOpt = P.get<sol::optional<sol::table>>("alpha_coverage_thresholds");
			if (ACThreshOpt.has_value())
			{
				sol::table T = ACThreshOpt.value();
				Texture->AlphaCoverageThresholds = FVector4(
					T.get_or("r", 0.0),
					T.get_or("g", 0.0),
					T.get_or("b", 0.0),
					T.get_or("a", 0.0)
				);
				ChangedCount++;
			}

			// Chroma Key
			if (P.get<sol::optional<bool>>("chroma_key").has_value())          { Texture->bChromaKeyTexture = P.get<bool>("chroma_key"); ChangedCount++; }
			if (P.get<sol::optional<double>>("chroma_key_threshold").has_value()) { Texture->ChromaKeyThreshold = static_cast<float>(P.get<double>("chroma_key_threshold")); ChangedCount++; }
			sol::optional<sol::table> CKColorOpt = P.get<sol::optional<sol::table>>("chroma_key_color");
			if (CKColorOpt.has_value())
			{
				sol::table C = CKColorOpt.value();
				Texture->ChromaKeyColor = FColor(
					static_cast<uint8>(C.get_or("r", 0)),
					static_cast<uint8>(C.get_or("g", 0)),
					static_cast<uint8>(C.get_or("b", 0)),
					static_cast<uint8>(C.get_or("a", 255))
				);
				ChangedCount++;
			}

			// Lossy Compression
			sol::optional<std::string> LossyOpt = P.get<sol::optional<std::string>>("lossy_compression_amount");
			if (LossyOpt.has_value())
			{
				FString LossyStr = UTF8_TO_TCHAR(LossyOpt.value().c_str());
				bool bMatched = true;
				if (LossyStr.Equals(TEXT("Default"), ESearchCase::IgnoreCase))      Texture->LossyCompressionAmount = TLCA_Default;
				else if (LossyStr.Equals(TEXT("None"), ESearchCase::IgnoreCase))     Texture->LossyCompressionAmount = TLCA_None;
				else if (LossyStr.Equals(TEXT("Lowest"), ESearchCase::IgnoreCase))   Texture->LossyCompressionAmount = TLCA_Lowest;
				else if (LossyStr.Equals(TEXT("Low"), ESearchCase::IgnoreCase))      Texture->LossyCompressionAmount = TLCA_Low;
				else if (LossyStr.Equals(TEXT("Medium"), ESearchCase::IgnoreCase))   Texture->LossyCompressionAmount = TLCA_Medium;
				else if (LossyStr.Equals(TEXT("High"), ESearchCase::IgnoreCase))     Texture->LossyCompressionAmount = TLCA_High;
				else if (LossyStr.Equals(TEXT("Highest"), ESearchCase::IgnoreCase))  Texture->LossyCompressionAmount = TLCA_Highest;
				else bMatched = false;
				if (bMatched) ChangedCount++;
			}

			// Compression alpha flags
			if (P.get<sol::optional<bool>>("compression_no_alpha").has_value())
			{
				Texture->CompressionNoAlpha = P.get<bool>("compression_no_alpha");
				ChangedCount++;
			}
			if (P.get<sol::optional<bool>>("compression_force_alpha").has_value())
			{
				Texture->CompressionForceAlpha = P.get<bool>("compression_force_alpha");
				ChangedCount++;
			}
			if (P.get<sol::optional<bool>>("compression_none").has_value())
			{
				Texture->CompressionNone = P.get<bool>("compression_none");
				ChangedCount++;
			}

			// Compression Quality (ASTC)
			sol::optional<std::string> CQOpt = P.get<sol::optional<std::string>>("compression_quality");
			if (CQOpt.has_value())
			{
				FString CQStr = UTF8_TO_TCHAR(CQOpt.value().c_str());
				bool bMatched = true;
				if (CQStr.Equals(TEXT("Default"), ESearchCase::IgnoreCase))      Texture->CompressionQuality = TCQ_Default;
				else if (CQStr.Equals(TEXT("Lowest"), ESearchCase::IgnoreCase))  Texture->CompressionQuality = TCQ_Lowest;
				else if (CQStr.Equals(TEXT("Low"), ESearchCase::IgnoreCase))     Texture->CompressionQuality = TCQ_Low;
				else if (CQStr.Equals(TEXT("Medium"), ESearchCase::IgnoreCase))  Texture->CompressionQuality = TCQ_Medium;
				else if (CQStr.Equals(TEXT("High"), ESearchCase::IgnoreCase))    Texture->CompressionQuality = TCQ_High;
				else if (CQStr.Equals(TEXT("Highest"), ESearchCase::IgnoreCase)) Texture->CompressionQuality = TCQ_Highest;
				else bMatched = false;
				if (bMatched) ChangedCount++;
			}

			// Power of Two Mode
			sol::optional<std::string> P2Opt = P.get<sol::optional<std::string>>("power_of_two_mode");
			if (P2Opt.has_value())
			{
				FString P2Str = UTF8_TO_TCHAR(P2Opt.value().c_str());
				bool bMatched = true;
				if (P2Str.Equals(TEXT("None"), ESearchCase::IgnoreCase))                         Texture->PowerOfTwoMode = ETexturePowerOfTwoSetting::None;
				else if (P2Str.Equals(TEXT("PadToPowerOfTwo"), ESearchCase::IgnoreCase))          Texture->PowerOfTwoMode = ETexturePowerOfTwoSetting::PadToPowerOfTwo;
				else if (P2Str.Equals(TEXT("PadToSquarePowerOfTwo"), ESearchCase::IgnoreCase))    Texture->PowerOfTwoMode = ETexturePowerOfTwoSetting::PadToSquarePowerOfTwo;
				else if (P2Str.Equals(TEXT("StretchToPowerOfTwo"), ESearchCase::IgnoreCase))      Texture->PowerOfTwoMode = ETexturePowerOfTwoSetting::StretchToPowerOfTwo;
				else if (P2Str.Equals(TEXT("StretchToSquarePowerOfTwo"), ESearchCase::IgnoreCase))Texture->PowerOfTwoMode = ETexturePowerOfTwoSetting::StretchToSquarePowerOfTwo;
				else if (P2Str.Equals(TEXT("ResizeToSpecificResolution"), ESearchCase::IgnoreCase))Texture->PowerOfTwoMode = ETexturePowerOfTwoSetting::ResizeToSpecificResolution;
				else bMatched = false;
				if (bMatched) ChangedCount++;
			}

			// Padding
			sol::optional<sol::table> PadColorOpt = P.get<sol::optional<sol::table>>("padding_color");
			if (PadColorOpt.has_value())
			{
				sol::table C = PadColorOpt.value();
				Texture->PaddingColor = FColor(
					static_cast<uint8>(C.get_or("r", 0)),
					static_cast<uint8>(C.get_or("g", 0)),
					static_cast<uint8>(C.get_or("b", 0)),
					static_cast<uint8>(C.get_or("a", 255))
				);
				ChangedCount++;
			}
			if (P.get<sol::optional<bool>>("pad_with_border_color").has_value())
			{
				Texture->bPadWithBorderColor = P.get<bool>("pad_with_border_color");
				ChangedCount++;
			}

			// Resize during build
			if (P.get<sol::optional<int>>("resize_x").has_value())
			{
				Texture->ResizeDuringBuildX = P.get<int>("resize_x");
				ChangedCount++;
			}
			if (P.get<sol::optional<int>>("resize_y").has_value())
			{
				Texture->ResizeDuringBuildY = P.get<int>("resize_y");
				ChangedCount++;
			}

			// Composite texture settings
			sol::optional<std::string> CTMOpt = P.get<sol::optional<std::string>>("composite_texture_mode");
			if (CTMOpt.has_value())
			{
				FString CTMStr = UTF8_TO_TCHAR(CTMOpt.value().c_str());
				bool bMatched = true;
				if (CTMStr.Equals(TEXT("Disabled"), ESearchCase::IgnoreCase))                 Texture->CompositeTextureMode = CTM_Disabled;
				else if (CTMStr.Equals(TEXT("NormalRoughnessToRed"), ESearchCase::IgnoreCase))   Texture->CompositeTextureMode = CTM_NormalRoughnessToRed;
				else if (CTMStr.Equals(TEXT("NormalRoughnessToGreen"), ESearchCase::IgnoreCase)) Texture->CompositeTextureMode = CTM_NormalRoughnessToGreen;
				else if (CTMStr.Equals(TEXT("NormalRoughnessToBlue"), ESearchCase::IgnoreCase))  Texture->CompositeTextureMode = CTM_NormalRoughnessToBlue;
				else if (CTMStr.Equals(TEXT("NormalRoughnessToAlpha"), ESearchCase::IgnoreCase)) Texture->CompositeTextureMode = CTM_NormalRoughnessToAlpha;
				else bMatched = false;
				if (bMatched) ChangedCount++;
			}
			if (P.get<sol::optional<double>>("composite_power").has_value())
			{
				Texture->CompositePower = static_cast<float>(P.get<double>("composite_power"));
				ChangedCount++;
			}

			// Color management settings
			sol::optional<std::string> ColorSpaceOpt = P.get<sol::optional<std::string>>("color_space");
			if (ColorSpaceOpt.has_value())
			{
				FString CSStr = UTF8_TO_TCHAR(ColorSpaceOpt.value().c_str());
				int64 EnumVal = StaticEnum<ETextureColorSpace>()->GetValueByNameString(CSStr);
				if (EnumVal == INDEX_NONE)
				{
					EnumVal = StaticEnum<ETextureColorSpace>()->GetValueByNameString(TEXT("ETextureColorSpace::") + CSStr);
				}
				if (EnumVal != INDEX_NONE)
				{
					Texture->SourceColorSettings.ColorSpace = static_cast<ETextureColorSpace>(EnumVal);
					ChangedCount++;
				}
			}

			sol::optional<std::string> EncodingOpt = P.get<sol::optional<std::string>>("encoding_override");
			if (EncodingOpt.has_value())
			{
				FString EncStr = UTF8_TO_TCHAR(EncodingOpt.value().c_str());
				int64 EnumVal = StaticEnum<ETextureSourceEncoding>()->GetValueByNameString(EncStr);
				if (EnumVal == INDEX_NONE)
				{
					EnumVal = StaticEnum<ETextureSourceEncoding>()->GetValueByNameString(TEXT("ETextureSourceEncoding::") + EncStr);
				}
				if (EnumVal != INDEX_NONE)
				{
					Texture->SourceColorSettings.EncodingOverride = static_cast<ETextureSourceEncoding>(EnumVal);
					ChangedCount++;
				}
			}

#endif // WITH_EDITORONLY_DATA

			// Trigger rebuild via PostEditChange
#if WITH_EDITOR
			Texture->PostEditChange();
#endif
			Texture->UpdateResource();
			Texture->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] configure() -> %d properties updated on %s"), ChangedCount, *Texture->GetName()));
			sol::table R = Lua.create_table();
			R["updated"] = ChangedCount;
			return R;
		});
	});
}

REGISTER_LUA_BINDING(Texture, TextureDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindTexture(Lua, Session);
});
