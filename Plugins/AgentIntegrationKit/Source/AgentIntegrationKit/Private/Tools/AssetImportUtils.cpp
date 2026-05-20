// Copyright 2025 Betide Studio. All Rights Reserved.

#include "Tools/AssetImportUtils.h"
#include "AgentIntegrationKitModule.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"

// Asset import
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"

// Texture import
#include "Engine/Texture2D.h"
#include "Factories/TextureFactory.h"
#include "EditorFramework/AssetImportData.h"

// Static mesh import
#include "Engine/StaticMesh.h"

// Audio import
#include "Sound/SoundWave.h"

// HTTP for downloads
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

namespace AssetImportUtils
{
	FString GetGeneratedContentTempDir()
	{
		FString TempDir = FPaths::ProjectSavedDir() / TEXT("GeneratedContent");

		// Create directory if it doesn't exist
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (!PlatformFile.DirectoryExists(*TempDir))
		{
			PlatformFile.CreateDirectoryTree(*TempDir);
		}

		return TempDir;
	}

	FString SanitizeAssetName(const FString& Input)
	{
		FString Sanitized = Input;

		// Remove invalid characters for asset names
		FString InvalidChars = TEXT(" .,:;'\"\\/?!@#$%^&*()[]{}|<>~`");
		for (int32 i = 0; i < InvalidChars.Len(); ++i)
		{
			Sanitized = Sanitized.Replace(&InvalidChars[i], TEXT("_"));
		}

		// Remove consecutive underscores
		while (Sanitized.Contains(TEXT("__")))
		{
			Sanitized = Sanitized.Replace(TEXT("__"), TEXT("_"));
		}

		// Trim underscores from start and end
		Sanitized.TrimStartAndEndInline();
		while (Sanitized.StartsWith(TEXT("_")))
		{
			Sanitized = Sanitized.RightChop(1);
		}
		while (Sanitized.EndsWith(TEXT("_")))
		{
			Sanitized = Sanitized.LeftChop(1);
		}

		// Limit length
		if (Sanitized.Len() > 64)
		{
			Sanitized = Sanitized.Left(64);
		}

		// Ensure it starts with a letter or underscore
		if (Sanitized.Len() > 0 && FChar::IsDigit(Sanitized[0]))
		{
			Sanitized = TEXT("Asset_") + Sanitized;
		}

		// Default name if empty
		if (Sanitized.IsEmpty())
		{
			Sanitized = TEXT("GeneratedAsset");
		}

		return Sanitized;
	}

	FString SaveBase64ToTempFile(const FString& Base64Data, const FString& Extension, FString& OutError)
	{
		FString DataToDecode = Base64Data;

		// Strip data URL prefix if present (e.g., "data:image/png;base64,")
		int32 CommaIndex;
		if (DataToDecode.FindChar(TEXT(','), CommaIndex))
		{
			if (DataToDecode.Left(CommaIndex).Contains(TEXT("base64")))
			{
				DataToDecode = DataToDecode.RightChop(CommaIndex + 1);
			}
		}

		// Decode base64
		TArray<uint8> DecodedData;
		if (!FBase64::Decode(DataToDecode, DecodedData))
		{
			OutError = TEXT("Failed to decode base64 data");
			return FString();
		}

		if (DecodedData.Num() == 0)
		{
			OutError = TEXT("Decoded data is empty");
			return FString();
		}

		// Generate unique filename
		FString TempDir = GetGeneratedContentTempDir();
		FString FileName = FString::Printf(TEXT("Generated_%s.%s"), *FGuid::NewGuid().ToString(), *Extension);
		FString FilePath = TempDir / FileName;

		// Write to file
		if (!FFileHelper::SaveArrayToFile(DecodedData, *FilePath))
		{
			OutError = FString::Printf(TEXT("Failed to write temp file: %s"), *FilePath);
			return FString();
		}

		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] Saved base64 data to temp file: %s (%d bytes)"), *FilePath, DecodedData.Num());
		return FilePath;
	}

	FString GenerateUniqueAssetName(const FString& BaseName, const FString& Path)
	{
		FString SanitizedName = SanitizeAssetName(BaseName);
		FString AssetPath = Path;

		// Ensure path starts with /Game
		if (!AssetPath.StartsWith(TEXT("/Game")))
		{
			AssetPath = FString::Printf(TEXT("/Game/%s"), *AssetPath);
		}

		// Check if asset exists
		FString PackageName = AssetPath / SanitizedName;
		if (!FPackageName::DoesPackageExist(PackageName))
		{
			return SanitizedName;
		}

		// Try with numeric suffix
		for (int32 i = 1; i < 1000; ++i)
		{
			FString NewName = FString::Printf(TEXT("%s_%d"), *SanitizedName, i);
			PackageName = AssetPath / NewName;
			if (!FPackageName::DoesPackageExist(PackageName))
			{
				return NewName;
			}
		}

		// Fallback with GUID
		return FString::Printf(TEXT("%s_%s"), *SanitizedName, *FGuid::NewGuid().ToString().Left(8));
	}

	UTexture2D* ImportTexture(const FString& SourceFile, const FString& DestinationPath, const FString& AssetName, FString& OutError)
	{
		// Validate source file exists
		if (!FPaths::FileExists(SourceFile))
		{
			OutError = FString::Printf(TEXT("Source file not found: %s"), *SourceFile);
			return nullptr;
		}

		// Build asset path
		FString AssetPath = DestinationPath;
		if (!AssetPath.StartsWith(TEXT("/Game")))
		{
			AssetPath = FString::Printf(TEXT("/Game/%s"), *AssetPath);
		}

		// Generate unique name
		FString FinalAssetName = GenerateUniqueAssetName(AssetName, AssetPath);

		// Add T_ prefix for textures if not present
		if (!FinalAssetName.StartsWith(TEXT("T_")))
		{
			FinalAssetName = TEXT("T_") + FinalAssetName;
		}

		FString PackageName = AssetPath / FinalAssetName;

		// Use IAssetTools for import
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

		// Create texture factory
		UTextureFactory* TextureFactory = NewObject<UTextureFactory>();
		TextureFactory->AddToRoot(); // Prevent GC during import
		TextureFactory->SuppressImportOverwriteDialog();

		// Import the texture
		TArray<FString> FilesToImport;
		FilesToImport.Add(SourceFile);

		TArray<UObject*> ImportedAssets = AssetTools.ImportAssets(FilesToImport, AssetPath, TextureFactory, false);

		TextureFactory->RemoveFromRoot();

		if (ImportedAssets.Num() == 0)
		{
			OutError = TEXT("Import failed - no assets created");
			return nullptr;
		}

		UTexture2D* ImportedTexture = Cast<UTexture2D>(ImportedAssets[0]);
		if (!ImportedTexture)
		{
			OutError = TEXT("Import succeeded but result is not a Texture2D");
			return nullptr;
		}

		// Rename if necessary (import may use filename)
		if (ImportedTexture->GetName() != FinalAssetName)
		{
			// The asset was imported with a different name, that's fine for now
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] Texture imported as: %s"), *ImportedTexture->GetPathName());
		}

		// Mark dirty
		ImportedTexture->GetPackage()->MarkPackageDirty();

		// Notify asset registry
		FAssetRegistryModule::AssetCreated(ImportedTexture);

		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] Successfully imported texture: %s"), *ImportedTexture->GetPathName());
		return ImportedTexture;
	}

	UStaticMesh* ImportStaticMesh(const FString& SourceFile, const FString& DestinationPath, const FString& AssetName, FString& OutError)
	{
		// Validate source file exists
		if (!FPaths::FileExists(SourceFile))
		{
			OutError = FString::Printf(TEXT("Source file not found: %s"), *SourceFile);
			return nullptr;
		}

		// Build asset path
		FString AssetPath = DestinationPath;
		if (!AssetPath.StartsWith(TEXT("/Game")))
		{
			AssetPath = FString::Printf(TEXT("/Game/%s"), *AssetPath);
		}

		// Generate unique name
		FString FinalAssetName = GenerateUniqueAssetName(AssetName, AssetPath);

		// Add SM_ prefix for static meshes if not present
		if (!FinalAssetName.StartsWith(TEXT("SM_")))
		{
			FinalAssetName = TEXT("SM_") + FinalAssetName;
		}

		// Use IAssetTools for import
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

		// Import the mesh - UE will auto-detect GLB/FBX/OBJ format
		TArray<FString> FilesToImport;
		FilesToImport.Add(SourceFile);

		// Import with default settings (will use Interchange for GLB if available)
		TArray<UObject*> ImportedAssets = AssetTools.ImportAssets(FilesToImport, AssetPath, nullptr, false);

		if (ImportedAssets.Num() == 0)
		{
			OutError = TEXT("Import failed - no assets created. Ensure the file format is supported.");
			return nullptr;
		}

		// Find the static mesh in imported assets (may import multiple assets like materials)
		UStaticMesh* ImportedMesh = nullptr;
		for (UObject* Asset : ImportedAssets)
		{
			ImportedMesh = Cast<UStaticMesh>(Asset);
			if (ImportedMesh)
			{
				break;
			}
		}

		if (!ImportedMesh)
		{
			OutError = TEXT("Import succeeded but no StaticMesh found in imported assets");
			return nullptr;
		}

		// Mark dirty
		ImportedMesh->GetPackage()->MarkPackageDirty();

		// Notify asset registry
		FAssetRegistryModule::AssetCreated(ImportedMesh);

		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] Successfully imported static mesh: %s"), *ImportedMesh->GetPathName());
		return ImportedMesh;
	}

	USoundWave* ImportAudio(const FString& SourceFile, const FString& DestinationPath, const FString& AssetName, FString& OutError)
	{
		if (!FPaths::FileExists(SourceFile))
		{
			OutError = FString::Printf(TEXT("Source file not found: %s"), *SourceFile);
			return nullptr;
		}

		FString AssetPath = DestinationPath;
		if (!AssetPath.StartsWith(TEXT("/Game")))
		{
			AssetPath = FString::Printf(TEXT("/Game/%s"), *AssetPath);
		}

		FString FinalAssetName = GenerateUniqueAssetName(AssetName, AssetPath);

		// Add S_ prefix for sound waves if not present
		if (!FinalAssetName.StartsWith(TEXT("S_")))
		{
			FinalAssetName = TEXT("S_") + FinalAssetName;
		}

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

		TArray<FString> FilesToImport;
		FilesToImport.Add(SourceFile);

		// Import with auto-detection (UE handles WAV/OGG via USoundFactory)
		TArray<UObject*> ImportedAssets = AssetTools.ImportAssets(FilesToImport, AssetPath, nullptr, false);

		if (ImportedAssets.Num() == 0)
		{
			OutError = TEXT("Import failed - no assets created. Ensure the file format is supported (WAV, OGG).");
			return nullptr;
		}

		USoundWave* ImportedSound = nullptr;
		for (UObject* Asset : ImportedAssets)
		{
			ImportedSound = Cast<USoundWave>(Asset);
			if (ImportedSound) break;
		}

		if (!ImportedSound)
		{
			OutError = TEXT("Import succeeded but no SoundWave found in imported assets");
			return nullptr;
		}

		ImportedSound->GetPackage()->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(ImportedSound);

		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] Successfully imported audio: %s"), *ImportedSound->GetPathName());
		return ImportedSound;
	}

	bool DownloadFile(const FString& Url, const FString& OutputPath, FString& OutError)
	{
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Url);
		Request->SetVerb(TEXT("GET"));
		Request->SetTimeout(300.0f); // 5 minute timeout for large files

		// Synchronous HTTP — safe from game thread (uses CompleteOnHttpThread internally)
		Request->ProcessRequestUntilComplete();

		FHttpResponsePtr Response = Request->GetResponse();
		if (Request->GetStatus() != EHttpRequestStatus::Succeeded || !Response.IsValid())
		{
			OutError = TEXT("Connection failed");
			return false;
		}

		int32 ResponseCode = Response->GetResponseCode();
		if (ResponseCode < 200 || ResponseCode >= 300)
		{
			OutError = FString::Printf(TEXT("HTTP %d: %s"), ResponseCode, *Response->GetContentAsString().Left(200));
			return false;
		}

		const TArray<uint8>& ResponseData = Response->GetContent();
		if (ResponseData.Num() == 0)
		{
			OutError = TEXT("Downloaded file is empty");
			return false;
		}

		// Ensure directory exists
		FString Directory = FPaths::GetPath(OutputPath);
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (!PlatformFile.DirectoryExists(*Directory))
		{
			PlatformFile.CreateDirectoryTree(*Directory);
		}

		// Write to file
		if (!FFileHelper::SaveArrayToFile(ResponseData, *OutputPath))
		{
			OutError = FString::Printf(TEXT("Failed to write file: %s"), *OutputPath);
			return false;
		}

		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[AIK] Downloaded file: %s (%d bytes)"), *OutputPath, ResponseData.Num());
		return true;
	}
}
