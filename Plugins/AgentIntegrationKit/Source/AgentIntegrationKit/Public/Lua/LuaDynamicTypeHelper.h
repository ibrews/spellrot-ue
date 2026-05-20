// Copyright 2026 Betide Studio. All Rights Reserved.
//
// Dynamic type discovery utilities for Lua bindings.
// Replaces hardcoded if/else type dispatch chains with cached GetDerivedClasses() lookups.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

namespace LuaDynamicType
{

// ── Cache ──────────────────────────────────────────────────────────────

struct FDerivedClassCache
{
	// Keyed by (BaseClass, CacheKey) -> (LowercaseFriendlyName -> UClass*)
	TMap<FString, TMap<FString, UClass*>> ClassMaps;
	// Keyed by (BaseClass, CacheKey) -> ordered friendly names for help text
	TMap<FString, TArray<FString>> NameLists;
};

inline FDerivedClassCache& GetCache()
{
	static FDerivedClassCache Cache;
	return Cache;
}

inline FString MakeCacheKey(const UClass* Base, const TArray<FString>& Prefixes, const TArray<FString>& Suffixes)
{
	return FString::Printf(TEXT("%s|%s|%s"),
		*Base->GetPathName(),
		*FString::Join(Prefixes, TEXT(",")),
		*FString::Join(Suffixes, TEXT(",")));
}

inline FString MakeFriendlyName(const FString& ClassName, const TArray<FString>& Prefixes, const TArray<FString>& Suffixes)
{
	FString Name = ClassName;
	for (const FString& Prefix : Prefixes)
	{
		if (Name.StartsWith(Prefix))
		{
			Name.RemoveFromStart(Prefix);
			break; // Only strip one prefix
		}
	}
	for (const FString& Suffix : Suffixes)
	{
		if (Name.EndsWith(Suffix))
		{
			Name.RemoveFromEnd(Suffix);
			break; // Only strip one suffix
		}
	}
	return Name;
}

inline void EnsurePopulated(const UClass* Base, const TArray<FString>& Prefixes, const TArray<FString>& Suffixes)
{
	FDerivedClassCache& Cache = GetCache();
	FString Key = MakeCacheKey(Base, Prefixes, Suffixes);
	if (Cache.ClassMaps.Contains(Key)) return;

	TArray<UClass*> Derived;
	GetDerivedClasses(Base, Derived, true);

	TMap<FString, UClass*>& Map = Cache.ClassMaps.Add(Key);
	TArray<FString>& Names = Cache.NameLists.Add(Key);

	for (UClass* Cls : Derived)
	{
		if (!Cls || Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			continue;

		FString Friendly = MakeFriendlyName(Cls->GetName(), Prefixes, Suffixes);
		if (Friendly.IsEmpty()) Friendly = Cls->GetName();

		Map.Add(Friendly.ToLower(), Cls);
		// Also add full class name as fallback
		Map.Add(Cls->GetName().ToLower(), Cls);
		Names.Add(Friendly);
	}

	Names.Sort();
}

// ── Public API ─────────────────────────────────────────────────────────

/**
 * Find a derived class by friendly type name.
 * E.g., FindDerivedClass(UInputTrigger, "Hold", {"InputTrigger"}, {}) finds UInputTriggerHold.
 * Returns nullptr if not found.
 */
inline UClass* FindDerivedClass(
	const UClass* Base,
	const FString& TypeName,
	const TArray<FString>& PrefixesToStrip = {},
	const TArray<FString>& SuffixesToStrip = {})
{
	if (TypeName.IsEmpty() || !Base) return nullptr;

	EnsurePopulated(Base, PrefixesToStrip, SuffixesToStrip);

	const FString Key = MakeCacheKey(Base, PrefixesToStrip, SuffixesToStrip);
	const TMap<FString, UClass*>* Map = GetCache().ClassMaps.Find(Key);
	if (!Map) return nullptr;

	// Exact match (case-insensitive via lowered key)
	if (UClass* const* Found = Map->Find(TypeName.ToLower()))
		return *Found;

	// Partial match: check if TypeName is contained in any friendly name
	for (const auto& Pair : *Map)
	{
		if (Pair.Key.Contains(TypeName.ToLower()))
			return Pair.Value;
	}

	return nullptr;
}

/**
 * Get the friendly type name for an object, given the same prefix/suffix rules.
 * E.g., GetFriendlyTypeName(SpriteRenderer, {"Niagara"}, {"RendererProperties"}) -> "Sprite"
 */
inline FString GetFriendlyTypeName(
	const UObject* Obj,
	const TArray<FString>& PrefixesToStrip = {},
	const TArray<FString>& SuffixesToStrip = {})
{
	if (!Obj) return TEXT("Unknown");
	FString Name = MakeFriendlyName(Obj->GetClass()->GetName(), PrefixesToStrip, SuffixesToStrip);
	return Name.IsEmpty() ? Obj->GetClass()->GetName() : Name;
}

/**
 * List all available friendly type names for a base class.
 * Useful for help text and error messages.
 */
inline const TArray<FString>& ListDerivedTypeNames(
	const UClass* Base,
	const TArray<FString>& PrefixesToStrip = {},
	const TArray<FString>& SuffixesToStrip = {})
{
	EnsurePopulated(Base, PrefixesToStrip, SuffixesToStrip);

	const FString Key = MakeCacheKey(Base, PrefixesToStrip, SuffixesToStrip);
	const TArray<FString>* Names = GetCache().NameLists.Find(Key);
	static const TArray<FString> Empty;
	return Names ? *Names : Empty;
}

/**
 * Format available type names as a comma-separated string (for error messages).
 */
inline FString FormatAvailableTypes(
	const UClass* Base,
	const TArray<FString>& PrefixesToStrip = {},
	const TArray<FString>& SuffixesToStrip = {})
{
	return FString::Join(ListDerivedTypeNames(Base, PrefixesToStrip, SuffixesToStrip), TEXT(", "));
}

/**
 * Invalidate all cached class hierarchies. Call if hot-reload changes the class tree.
 */
inline void InvalidateCache()
{
	GetCache().ClassMaps.Empty();
	GetCache().NameLists.Empty();
}

// ── UScriptStruct Discovery ────────────────────────────────────────────

struct FDerivedStructCache
{
	TMap<FString, TMap<FString, UScriptStruct*>> StructMaps;
	TMap<FString, TArray<FString>> NameLists;
};

inline FDerivedStructCache& GetStructCache()
{
	static FDerivedStructCache Cache;
	return Cache;
}

inline FString MakeStructCacheKey(const UScriptStruct* Base, const TArray<FString>& Prefixes, const TArray<FString>& Suffixes)
{
	return FString::Printf(TEXT("S|%s|%s|%s"),
		*Base->GetPathName(),
		*FString::Join(Prefixes, TEXT(",")),
		*FString::Join(Suffixes, TEXT(",")));
}

inline void EnsureStructPopulated(const UScriptStruct* Base, const TArray<FString>& Prefixes, const TArray<FString>& Suffixes)
{
	FDerivedStructCache& Cache = GetStructCache();
	FString Key = MakeStructCacheKey(Base, Prefixes, Suffixes);
	if (Cache.StructMaps.Contains(Key)) return;

	TMap<FString, UScriptStruct*>& Map = Cache.StructMaps.Add(Key);
	TArray<FString>& Names = Cache.NameLists.Add(Key);

	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		UScriptStruct* S = *It;
		if (!S || !S->IsChildOf(Base) || S == Base) continue;
		// Skip hidden/internal structs
		if (S->HasMetaData(TEXT("Hidden"))) continue;

		FString Friendly = MakeFriendlyName(S->GetName(), Prefixes, Suffixes);
		if (Friendly.IsEmpty()) Friendly = S->GetName();

		Map.Add(Friendly.ToLower(), S);
		Map.Add(S->GetName().ToLower(), S);
		Names.Add(Friendly);
	}

	Names.Sort();
}

/**
 * Find a derived UScriptStruct by friendly type name.
 * E.g., FindDerivedStruct(FIKRigSolverBase::StaticStruct(), "FBIK", {"FIKRig","F"}, {"Solver"})
 */
inline UScriptStruct* FindDerivedStruct(
	const UScriptStruct* Base,
	const FString& TypeName,
	const TArray<FString>& PrefixesToStrip = {},
	const TArray<FString>& SuffixesToStrip = {})
{
	if (TypeName.IsEmpty() || !Base) return nullptr;

	EnsureStructPopulated(Base, PrefixesToStrip, SuffixesToStrip);

	const FString Key = MakeStructCacheKey(Base, PrefixesToStrip, SuffixesToStrip);
	const TMap<FString, UScriptStruct*>* Map = GetStructCache().StructMaps.Find(Key);
	if (!Map) return nullptr;

	if (UScriptStruct* const* Found = Map->Find(TypeName.ToLower()))
		return *Found;

	// Partial match
	for (const auto& Pair : *Map)
	{
		if (Pair.Key.Contains(TypeName.ToLower()))
			return Pair.Value;
	}

	return nullptr;
}

/**
 * List all available friendly struct type names.
 */
inline const TArray<FString>& ListDerivedStructNames(
	const UScriptStruct* Base,
	const TArray<FString>& PrefixesToStrip = {},
	const TArray<FString>& SuffixesToStrip = {})
{
	EnsureStructPopulated(Base, PrefixesToStrip, SuffixesToStrip);

	const FString Key = MakeStructCacheKey(Base, PrefixesToStrip, SuffixesToStrip);
	const TArray<FString>* Names = GetStructCache().NameLists.Find(Key);
	static const TArray<FString> Empty;
	return Names ? *Names : Empty;
}

inline FString FormatAvailableStructTypes(
	const UScriptStruct* Base,
	const TArray<FString>& PrefixesToStrip = {},
	const TArray<FString>& SuffixesToStrip = {})
{
	return FString::Join(ListDerivedStructNames(Base, PrefixesToStrip, SuffixesToStrip), TEXT(", "));
}

} // namespace LuaDynamicType
