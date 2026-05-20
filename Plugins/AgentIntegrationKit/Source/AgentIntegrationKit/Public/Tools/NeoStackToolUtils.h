// Copyright 2025 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/UnrealType.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

/**
 * Shared utilities for NeoStack tools
 */
namespace NeoStackToolUtils
{
	//--------------------------------------------------------------------
	// Path Utilities
	//--------------------------------------------------------------------

	/** Check if path indicates a UE asset (vs text file) */
	bool IsAssetPath(const FString& Name, const FString& Path);

	/** Build full file path from name and relative path */
	FString BuildFilePath(const FString& Name, const FString& Path);

	/** Build asset path in /Game/ format */
	FString BuildAssetPath(const FString& Name, const FString& Path);

	/**
	 * Build package name for asset creation.
	 * Handles cases where Name might contain a full path (e.g., "/Game/Blueprints/BP_Cube")
	 * and properly extracts just the asset name to avoid double paths.
	 * @param Name - Asset name (may be full path like "/Game/Folder/AssetName" or just "AssetName")
	 * @param Path - Folder path (e.g., "/Game/Blueprints" or empty)
	 * @param OutAssetName - The sanitized asset name (just the name, no path)
	 * @return Package name in format "/Game/Folder/AssetName"
	 */
	FString BuildPackageName(const FString& Name, const FString& Path, FString& OutAssetName);

	/** Ensure directory exists, create if needed */
	bool EnsureDirectoryExists(const FString& FilePath, FString& OutError);

	//--------------------------------------------------------------------
	// Blueprint Utilities
	//--------------------------------------------------------------------

	/** Load a Blueprint from name and path */
	UBlueprint* LoadBlueprint(const FString& Name, const FString& Path, FString& OutError);

	/** Find parent class by name (handles A/U prefixes) */
	UClass* FindParentClass(const FString& ClassName, FString& OutError);

	//--------------------------------------------------------------------
	// Graph Utilities
	//--------------------------------------------------------------------

	/** Find graph by name in a Blueprint */
	UEdGraph* FindGraphByName(UBlueprint* Blueprint, const FString& GraphName);

	/** Get graph type string (ubergraph, function, macro, collapsed) */
	FString GetGraphType(UEdGraph* Graph, UBlueprint* Blueprint);

	//--------------------------------------------------------------------
	// Node Utilities
	//--------------------------------------------------------------------

	/** Find node by GUID string */
	UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& GuidString);

	/** Get node GUID as string */
	FString GetNodeGuid(UEdGraphNode* Node);

	/** Get comma-separated list of visible pin names */
	FString GetNodePinNames(UEdGraphNode* Node);

	//--------------------------------------------------------------------
	// Pin Utilities
	//--------------------------------------------------------------------

	/** Find pin by name on a node */
	UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction = EGPD_MAX);

	//--------------------------------------------------------------------
	// Asset Loading Utilities
	//--------------------------------------------------------------------

	/**
	 * Load an asset, falling back to appending ".AssetName" suffix if the first attempt fails.
	 * Many agents omit the required ObjectName suffix in UE asset paths.
	 */
	template <typename T>
	T* LoadAssetWithFallback(const FString& Path)
	{
		T* Asset = LoadObject<T>(nullptr, *Path);
		if (!Asset && !Path.Contains(TEXT(".")))
		{
			FString PathWithSuffix = Path + TEXT(".") + FPaths::GetBaseFilename(Path);
			Asset = LoadObject<T>(nullptr, *PathWithSuffix);
		}
		return Asset;
	}

	/**
	 * Create a UPackage for a new asset, combining BuildPackageName + CreatePackage.
	 * @param Name      Asset name (may be full path)
	 * @param Path      Folder path
	 * @param OutAssetName  The sanitized asset name (just the name, no path)
	 * @return Created package, or nullptr on failure
	 */
	UPackage* CreateAssetPackage(const FString& Name, const FString& Path, FString& OutAssetName);

	//--------------------------------------------------------------------
	// Property Utilities
	//--------------------------------------------------------------------

	/**
	 * Get a property's value as a human-readable string.
	 * Handles Bool, Enum, ByteEnum explicitly; falls back to ExportText.
	 * @param OwnerObject  Optional owner for ExportText context (pass the UObject that owns the property)
	 * @param MaxLength    Truncate result to this length (0 = no truncation)
	 */
	FString GetPropertyValueAsString(const void* ContainerPtr, FProperty* Property, UObject* OwnerObject = nullptr, int32 MaxLength = 0);

	/** Get a human-readable type name for a property (Bool, Int, Enum(Name), Struct(Name), etc.) */
	FString GetPropertyTypeName(FProperty* Property);
}
