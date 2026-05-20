// Copyright 2025 Betide Studio. All Rights Reserved.

#include "Tools/NeoStackToolUtils.h"
#include "AgentIntegrationKitModule.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"

namespace NeoStackToolUtils
{
	//--------------------------------------------------------------------
	// Path Utilities
	//--------------------------------------------------------------------

	bool IsAssetPath(const FString& Name, const FString& Path)
	{
		// If path starts with /Game, it's an asset
		if (Path.StartsWith(TEXT("/Game")))
		{
			return true;
		}

		// If name has no extension or has .uasset, it's an asset
		if (!Name.Contains(TEXT(".")) || Name.EndsWith(TEXT(".uasset")))
		{
			return true;
		}

		return false;
	}

	FString BuildFilePath(const FString& Name, const FString& Path)
	{
		FString ProjectDir = FPaths::ProjectDir();
		FString FullPath;
		const bool bNameIsAbsolute = FPaths::IsRelative(Name) == false;

		// If Name is already absolute, treat it as canonical. This avoids
		// malformed paths like "<project>/<path>/<absolute-name>".
		if (bNameIsAbsolute)
		{
			FullPath = Name;
		}
		else if (Path.IsEmpty())
		{
			FullPath = ProjectDir / Name;
		}
		else if (FPaths::IsRelative(Path))
		{
			FullPath = ProjectDir / Path / Name;
		}
		else
		{
			FullPath = Name.IsEmpty() ? Path : (Path / Name);
		}

		FPaths::NormalizeFilename(FullPath);
		return FullPath;
	}

	FString BuildAssetPath(const FString& Name, const FString& Path)
	{
		FString WorkingName = Name;
		FString WorkingPath = Path;

		// Check if Name is an absolute filesystem path (contains drive letter on Windows or starts with /)
		if (Name.Contains(TEXT(":")) || (Name.StartsWith(TEXT("/")) && !Name.StartsWith(TEXT("/Game"))))
		{
			// Convert absolute path to UE content path
			// e.g., C:/Users/.../Content/Blueprints/BP_Player.uasset -> /Game/Blueprints/BP_Player
			FString NormalizedPath = Name.Replace(TEXT("\\"), TEXT("/"));

			// Find the Content folder
			int32 ContentIndex = NormalizedPath.Find(TEXT("/Content/"), ESearchCase::IgnoreCase);
			if (ContentIndex != INDEX_NONE)
			{
				// Extract the part after /Content/
				FString RelativePath = NormalizedPath.Mid(ContentIndex + 9); // +9 for "/Content/"

				// Remove .uasset extension if present
				if (RelativePath.EndsWith(TEXT(".uasset")))
				{
					RelativePath = RelativePath.LeftChop(7);
				}

				// Return UE content path format: /Game/Path/AssetName.AssetName
				FString AssetName = FPaths::GetBaseFilename(RelativePath);
				FString AssetDir = FPaths::GetPath(RelativePath);

				if (AssetDir.IsEmpty())
				{
					return FString::Printf(TEXT("/Game/%s.%s"), *AssetName, *AssetName);
				}
				return FString::Printf(TEXT("/Game/%s/%s.%s"), *AssetDir, *AssetName, *AssetName);
			}

			UE_LOG(LogAgentIntegrationKit, Warning, TEXT("[AIK] Could not find Content folder in path: %s"), *Name);
		}

		// Check if Name starts with /Content/ (relative to project)
		if (WorkingName.StartsWith(TEXT("/Content/")) || WorkingName.StartsWith(TEXT("Content/")))
		{
			// Remove /Content/ or Content/ prefix and treat as /Game/ path
			FString RelativePath = WorkingName;
			if (RelativePath.StartsWith(TEXT("/Content/")))
			{
				RelativePath = RelativePath.Mid(9); // Remove "/Content/"
			}
			else if (RelativePath.StartsWith(TEXT("Content/")))
			{
				RelativePath = RelativePath.Mid(8); // Remove "Content/"
			}

			// Remove .uasset extension if present
			if (RelativePath.EndsWith(TEXT(".uasset")))
			{
				RelativePath = RelativePath.LeftChop(7);
			}

			FString AssetName = FPaths::GetBaseFilename(RelativePath);
			FString AssetDir = FPaths::GetPath(RelativePath);

			if (AssetDir.IsEmpty())
			{
				return FString::Printf(TEXT("/Game/%s.%s"), *AssetName, *AssetName);
			}
			return FString::Printf(TEXT("/Game/%s/%s.%s"), *AssetDir, *AssetName, *AssetName);
		}

		// Handle case where Name is already a full /Game/ path (e.g., "/Game/Blueprints/BP_Player")
		if (WorkingName.StartsWith(TEXT("/Game/")))
		{
			// Remove .uasset extension if present
			FString CleanPath = WorkingName;
			if (CleanPath.EndsWith(TEXT(".uasset")))
			{
				CleanPath = CleanPath.LeftChop(7);
			}

			// Remove any trailing asset name after dot (e.g., "/Game/Path/Asset.Asset" -> "/Game/Path/Asset")
			int32 DotIndex;
			if (CleanPath.FindLastChar('.', DotIndex))
			{
				// Check if this is the "Asset.Asset" format
				FString BeforeDot = CleanPath.Left(DotIndex);
				FString AfterDot = CleanPath.Mid(DotIndex + 1);
				if (BeforeDot.EndsWith(AfterDot))
				{
					CleanPath = BeforeDot;
				}
			}

			// Extract the asset name (last component of the path)
			FString AssetName = FPaths::GetBaseFilename(CleanPath);
			return FString::Printf(TEXT("%s.%s"), *CleanPath, *AssetName);
		}

		// Handle engine content paths (don't prefix with /Game)
		if (WorkingName.StartsWith(TEXT("/Engine/")) || WorkingPath.StartsWith(TEXT("/Engine/")))
		{
			FString CleanPath = WorkingPath.IsEmpty() ? WorkingName : (WorkingPath / WorkingName);
			// Remove .uasset extension if present
			if (CleanPath.EndsWith(TEXT(".uasset")))
			{
				CleanPath = CleanPath.LeftChop(7);
			}
			FString AssetName = FPaths::GetBaseFilename(CleanPath);
			return FString::Printf(TEXT("%s.%s"), *CleanPath, *AssetName);
		}

		// Handle other engine paths like /Script/, /Temp/, etc.
		if (WorkingName.StartsWith(TEXT("/")) || WorkingPath.StartsWith(TEXT("/")))
		{
			// If it's already an absolute UE path (but not /Game), use it as-is
			if (!WorkingPath.IsEmpty() && !WorkingPath.StartsWith(TEXT("/Game")))
			{
				FString CleanPath = WorkingPath / WorkingName;
				if (CleanPath.EndsWith(TEXT(".uasset")))
				{
					CleanPath = CleanPath.LeftChop(7);
				}
				// Normalize double slashes
				CleanPath = CleanPath.Replace(TEXT("//"), TEXT("/"));
				FString AssetName = FPaths::GetBaseFilename(CleanPath);
				return FString::Printf(TEXT("%s.%s"), *CleanPath, *AssetName);
			}
		}

		// Original logic for simple names or paths
		FString AssetPath = WorkingPath.IsEmpty() ? TEXT("/Game") : WorkingPath;
		if (!AssetPath.StartsWith(TEXT("/Game")))
		{
			AssetPath = FString::Printf(TEXT("/Game/%s"), *AssetPath);
		}

		while (AssetPath.EndsWith(TEXT("/")))
		{
			AssetPath = AssetPath.LeftChop(1);
		}

		FString AssetName = WorkingName.EndsWith(TEXT(".uasset")) ? WorkingName.LeftChop(7) : WorkingName;
		FString Result = FString::Printf(TEXT("%s/%s.%s"), *AssetPath, *AssetName, *AssetName);

		// Safety: normalize any double slashes
		Result = Result.Replace(TEXT("//"), TEXT("/"));
		return Result;
	}

	FString BuildPackageName(const FString& Name, const FString& Path, FString& OutAssetName)
	{
		FString WorkingName = Name;
		FString WorkingPath = Path;

		// If Name contains a full /Game/ path, extract the asset name and optionally use its path
		if (WorkingName.StartsWith(TEXT("/Game/")))
		{
			// Extract just the asset name (last component after final /)
			int32 LastSlashIndex;
			if (WorkingName.FindLastChar('/', LastSlashIndex))
			{
				FString ExtractedPath = WorkingName.Left(LastSlashIndex);
				FString ExtractedName = WorkingName.Mid(LastSlashIndex + 1);

				// Remove .uasset extension if present
				if (ExtractedName.EndsWith(TEXT(".uasset")))
				{
					ExtractedName = ExtractedName.LeftChop(7);
				}

				// If no path was provided, use the path from the name
				if (WorkingPath.IsEmpty() && !ExtractedPath.IsEmpty())
				{
					WorkingPath = ExtractedPath;
				}

				WorkingName = ExtractedName;
			}
		}
		// Handle paths with slashes that aren't /Game/ paths (relative paths like "Folder/Asset")
		else if (WorkingName.Contains(TEXT("/")))
		{
			int32 LastSlashIndex;
			if (WorkingName.FindLastChar('/', LastSlashIndex))
			{
				FString ExtractedPath = WorkingName.Left(LastSlashIndex);
				FString ExtractedName = WorkingName.Mid(LastSlashIndex + 1);

				// Remove .uasset extension if present
				if (ExtractedName.EndsWith(TEXT(".uasset")))
				{
					ExtractedName = ExtractedName.LeftChop(7);
				}

				// If no path was provided, use the extracted path
				if (WorkingPath.IsEmpty() && !ExtractedPath.IsEmpty())
				{
					WorkingPath = ExtractedPath;
				}

				WorkingName = ExtractedName;
			}
		}

		// Remove .uasset extension from name if still present
		if (WorkingName.EndsWith(TEXT(".uasset")))
		{
			WorkingName = WorkingName.LeftChop(7);
		}

		// Set the output asset name
		OutAssetName = WorkingName;

		// Build the package path
		FString PackagePath = WorkingPath.IsEmpty() ? TEXT("/Game") : WorkingPath;
		if (!PackagePath.StartsWith(TEXT("/")))
		{
			PackagePath = TEXT("/Game/") + PackagePath;
		}

		// Remove trailing slashes
		while (PackagePath.EndsWith(TEXT("/")))
		{
			PackagePath = PackagePath.LeftChop(1);
		}

		// Build final package name
		FString PackageName = PackagePath / OutAssetName;

		// Safety: normalize any double slashes
		PackageName = PackageName.Replace(TEXT("//"), TEXT("/"));

		return PackageName;
	}

	bool EnsureDirectoryExists(const FString& FilePath, FString& OutError)
	{
		FString Directory = FPaths::GetPath(FilePath);
		if (!FPaths::DirectoryExists(Directory))
		{
			if (!IFileManager::Get().MakeDirectory(*Directory, true))
			{
				OutError = FString::Printf(TEXT("Failed to create directory: %s"), *Directory);
				return false;
			}
		}
		return true;
	}

	//--------------------------------------------------------------------
	// Blueprint Utilities
	//--------------------------------------------------------------------

	UBlueprint* LoadBlueprint(const FString& Name, const FString& Path, FString& OutError)
	{
		FString FullAssetPath = BuildAssetPath(Name, Path);

		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *FullAssetPath);
		if (!Blueprint)
		{
			OutError = FString::Printf(TEXT("Blueprint not found: %s"), *FullAssetPath);
			return nullptr;
		}

		return Blueprint;
	}

	UClass* FindParentClass(const FString& ClassName, FString& OutError)
	{
		UClass* ParentClass = nullptr;

		// Try common class name patterns
		TArray<FString> Variants = {
			ClassName,
			FString::Printf(TEXT("A%s"), *ClassName),  // Actor classes
			FString::Printf(TEXT("U%s"), *ClassName),  // UObject classes
		};

		for (const FString& Variant : Variants)
		{
			ParentClass = FindFirstObject<UClass>(*Variant, EFindFirstObjectOptions::None);
			if (ParentClass)
			{
				return ParentClass;
			}
		}

		// Try loading by path
		ParentClass = LoadClass<UObject>(nullptr, *ClassName);
		if (ParentClass)
		{
			return ParentClass;
		}

		OutError = FString::Printf(TEXT("Parent class not found: %s"), *ClassName);
		return nullptr;
	}

	//--------------------------------------------------------------------
	// Graph Utilities
	//--------------------------------------------------------------------

	UEdGraph* FindGraphByName(UBlueprint* Blueprint, const FString& GraphName)
	{
		if (!Blueprint) return nullptr;

		// Search in ubergraph pages
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
			{
				return Graph;
			}
		}

		// Search in function graphs
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
			{
				return Graph;
			}
		}

		// Search in macro graphs
		for (UEdGraph* Graph : Blueprint->MacroGraphs)
		{
			if (Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
			{
				return Graph;
			}
		}

		return nullptr;
	}

	FString GetGraphType(UEdGraph* Graph, UBlueprint* Blueprint)
	{
		if (!Graph || !Blueprint) return TEXT("unknown");

		if (Blueprint->UbergraphPages.Contains(Graph))
		{
			return TEXT("ubergraph");
		}
		if (Blueprint->FunctionGraphs.Contains(Graph))
		{
			return TEXT("function");
		}
		if (Blueprint->MacroGraphs.Contains(Graph))
		{
			return TEXT("macro");
		}

		return TEXT("unknown");
	}

	//--------------------------------------------------------------------
	// Node Utilities
	//--------------------------------------------------------------------

	UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& GuidString)
	{
		if (!Graph) return nullptr;

		FGuid TargetGuid;
		if (!FGuid::Parse(GuidString, TargetGuid))
		{
			return nullptr;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == TargetGuid)
			{
				return Node;
			}
		}

		return nullptr;
	}

	FString GetNodeGuid(UEdGraphNode* Node)
	{
		if (!Node) return TEXT("");
		return Node->NodeGuid.ToString();
	}

	FString GetNodePinNames(UEdGraphNode* Node)
	{
		if (!Node) return TEXT("");

		TArray<FString> PinNames;

		for (UEdGraphPin* Pin : Node->Pins)
		{
			// Skip hidden pins
			if (Pin->bHidden) continue;
			PinNames.Add(Pin->PinName.ToString());
		}

		return FString::Join(PinNames, TEXT(","));
	}

	//--------------------------------------------------------------------
	// Pin Utilities
	//--------------------------------------------------------------------

	UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction)
	{
		if (!Node) return nullptr;

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
			{
				// If direction specified, check it matches
				if (Direction != EGPD_MAX && Pin->Direction != Direction)
				{
					continue;
				}
				return Pin;
			}
		}

		return nullptr;
	}

	//--------------------------------------------------------------------
	// Asset Loading Utilities
	//--------------------------------------------------------------------

	UPackage* CreateAssetPackage(const FString& Name, const FString& Path, FString& OutAssetName)
	{
		FString PackageName = BuildPackageName(Name, Path, OutAssetName);
		UPackage* Package = CreatePackage(*PackageName);
		return Package;
	}

	//--------------------------------------------------------------------
	// Property Utilities
	//--------------------------------------------------------------------

	FString GetPropertyValueAsString(const void* ContainerPtr, FProperty* Property, UObject* OwnerObject, int32 MaxLength)
	{
		if (!ContainerPtr || !Property) return TEXT("");

		// Handle bool properties explicitly
		if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
		{
			bool bValue = BoolProp->GetPropertyValue_InContainer(ContainerPtr);
			return bValue ? TEXT("True") : TEXT("False");
		}

		// Handle enum properties explicitly
		if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
		{
			if (UEnum* Enum = EnumProp->GetEnum())
			{
				FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
				int64 EnumValue = UnderlyingProp->GetSignedIntPropertyValue(
					EnumProp->ContainerPtrToValuePtr<void>(ContainerPtr));
				return Enum->GetNameStringByValue(EnumValue);
			}
		}

		// Handle byte enums
		if (FByteProperty* ByteProp = CastField<FByteProperty>(Property))
		{
			if (UEnum* Enum = ByteProp->GetIntPropertyEnum())
			{
				uint8 ByteValue = ByteProp->GetPropertyValue_InContainer(ContainerPtr);
				return Enum->GetNameStringByValue(ByteValue);
			}
		}

		// Standard export for other types
		FString Value;
		Property->ExportText_InContainer(0, Value, ContainerPtr, nullptr, OwnerObject, PPF_None);

		if (MaxLength > 0 && Value.Len() > MaxLength)
		{
			Value = Value.Left(MaxLength) + TEXT("...");
		}

		return Value;
	}

	FString GetPropertyTypeName(FProperty* Property)
	{
		if (!Property) return TEXT("Unknown");

		// Handle enum properties specially
		if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
		{
			if (UEnum* Enum = EnumProp->GetEnum())
			{
				return FString::Printf(TEXT("Enum(%s)"), *Enum->GetName());
			}
		}
		else if (FByteProperty* ByteProp = CastField<FByteProperty>(Property))
		{
			if (UEnum* Enum = ByteProp->GetIntPropertyEnum())
			{
				return FString::Printf(TEXT("Enum(%s)"), *Enum->GetName());
			}
		}

		// Standard type names
		if (CastField<FBoolProperty>(Property)) return TEXT("Bool");
		if (CastField<FIntProperty>(Property)) return TEXT("Int");
		if (CastField<FFloatProperty>(Property)) return TEXT("Float");
		if (CastField<FDoubleProperty>(Property)) return TEXT("Double");
		if (CastField<FStrProperty>(Property)) return TEXT("String");
		if (CastField<FNameProperty>(Property)) return TEXT("Name");
		if (CastField<FTextProperty>(Property)) return TEXT("Text");

		if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
		{
			return FString::Printf(TEXT("Struct(%s)"), *StructProp->Struct->GetName());
		}

		if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Property))
		{
			return FString::Printf(TEXT("Object(%s)"), *ObjProp->PropertyClass->GetName());
		}

		if (CastField<FArrayProperty>(Property))
		{
			return TEXT("Array");
		}

		return Property->GetCPPType();
	}
}
