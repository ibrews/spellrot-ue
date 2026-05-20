// Copyright 2026 Betide Studio. All Rights Reserved.

#include "ProjectIndexManager.h"
#include "ACPSettings.h"
#include "Tools/NeoStackToolRegistry.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Misc/DateTime.h"
#include "Async/Async.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "DirectoryWatcherModule.h"
#include "IDirectoryWatcher.h"

// ═══════════════════════════════════════════════════════════════════════
// JSON Helpers
// ═══════════════════════════════════════════════════════════════════════

static FString JsonObjToString(const TSharedRef<FJsonObject>& Obj)
{
	FString Out;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Obj, Writer);
	return Out;
}

static bool ParseJsonString(const FString& In, TSharedPtr<FJsonObject>& Out)
{
	auto Reader = TJsonReaderFactory<>::Create(In);
	return FJsonSerializer::Deserialize(Reader, Out);
}

// ═══════════════════════════════════════════════════════════════════════
// Singleton
// ═══════════════════════════════════════════════════════════════════════

FProjectIndexManager& FProjectIndexManager::Get()
{
	static FProjectIndexManager Instance;
	return Instance;
}

void FProjectIndexManager::Initialize()
{
	EnsureInit();
}

void FProjectIndexManager::Shutdown()
{
	StopWatcher();
}

void FProjectIndexManager::EnsureInit()
{
	if (bInitialized) return;
	bInitialized = true;

	IFileManager::Get().MakeDirectory(*GetIndexDir(), true);
	LoadSettings();
	LoadStatus();
}

// ═══════════════════════════════════════════════════════════════════════
// Paths
// ═══════════════════════════════════════════════════════════════════════

FString FProjectIndexManager::GetIndexDir() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AgentIntegrationKit"), TEXT("ProjectIndex"));
}

FString FProjectIndexManager::GetSettingsPath() const   { return FPaths::Combine(GetIndexDir(), TEXT("settings.json")); }
FString FProjectIndexManager::GetStatusPath() const     { return FPaths::Combine(GetIndexDir(), TEXT("status.json")); }
FString FProjectIndexManager::GetMetadataPath() const   { return FPaths::Combine(GetIndexDir(), TEXT("index.json")); }
FString FProjectIndexManager::GetEmbeddingsPath() const { return FPaths::Combine(GetIndexDir(), TEXT("embeddings.bin")); }
FString FProjectIndexManager::GetFileIndexPath() const  { return FPaths::Combine(GetIndexDir(), TEXT("fileIndex.json")); }

// ═══════════════════════════════════════════════════════════════════════
// Settings I/O
// ═══════════════════════════════════════════════════════════════════════

void FProjectIndexManager::LoadSettings()
{
	FString Raw;
	if (FFileHelper::LoadFileToString(Raw, *GetSettingsPath()))
	{
		TSharedPtr<FJsonObject> Obj;
		if (ParseJsonString(Raw, Obj) && Obj.IsValid())
		{
			Obj->TryGetStringField(TEXT("provider"), Settings.Provider);
			Obj->TryGetStringField(TEXT("endpointUrl"), Settings.EndpointUrl);
			Obj->TryGetStringField(TEXT("apiKey"), Settings.ApiKey);
			Obj->TryGetStringField(TEXT("model"), Settings.Model);
			Obj->TryGetNumberField(TEXT("dimensions"), Settings.Dimensions);
			Obj->TryGetBoolField(TEXT("autoIndex"), Settings.bAutoIndex);

			const TSharedPtr<FJsonObject>* ScopeObj = nullptr;
			if (Obj->TryGetObjectField(TEXT("scope"), ScopeObj) && ScopeObj)
			{
				(*ScopeObj)->TryGetBoolField(TEXT("blueprints"), Settings.Scope.bBlueprints);
				(*ScopeObj)->TryGetBoolField(TEXT("cppFiles"), Settings.Scope.bCppFiles);
				(*ScopeObj)->TryGetBoolField(TEXT("assets"), Settings.Scope.bAssets);
				(*ScopeObj)->TryGetBoolField(TEXT("levels"), Settings.Scope.bLevels);
				(*ScopeObj)->TryGetBoolField(TEXT("config"), Settings.Scope.bConfig);
				(*ScopeObj)->TryGetBoolField(TEXT("documents"), Settings.Scope.bDocuments);
			}
		}
	}
}

void FProjectIndexManager::SaveSettings() const
{
	auto Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("provider"), Settings.Provider);
	Obj->SetStringField(TEXT("endpointUrl"), Settings.EndpointUrl);
	Obj->SetStringField(TEXT("apiKey"), Settings.ApiKey);
	Obj->SetStringField(TEXT("model"), Settings.Model);
	Obj->SetNumberField(TEXT("dimensions"), Settings.Dimensions);
	Obj->SetBoolField(TEXT("autoIndex"), Settings.bAutoIndex);

	auto ScopeObj = MakeShared<FJsonObject>();
	ScopeObj->SetBoolField(TEXT("blueprints"), Settings.Scope.bBlueprints);
	ScopeObj->SetBoolField(TEXT("cppFiles"), Settings.Scope.bCppFiles);
	ScopeObj->SetBoolField(TEXT("assets"), Settings.Scope.bAssets);
	ScopeObj->SetBoolField(TEXT("levels"), Settings.Scope.bLevels);
	ScopeObj->SetBoolField(TEXT("config"), Settings.Scope.bConfig);
	ScopeObj->SetBoolField(TEXT("documents"), Settings.Scope.bDocuments);
	Obj->SetObjectField(TEXT("scope"), ScopeObj);

	IFileManager::Get().MakeDirectory(*GetIndexDir(), true);
	FFileHelper::SaveStringToFile(JsonObjToString(Obj), *GetSettingsPath());
}

FString FProjectIndexManager::GetSettingsJson() const
{
	const_cast<FProjectIndexManager*>(this)->EnsureInit();

	bool bHasOpenRouterKey = false;
	if (const UACPSettings* S = UACPSettings::Get())
	{
		bHasOpenRouterKey = !S->OpenRouterApiKey.IsEmpty();
	}

	auto Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("provider"), Settings.Provider);
	Obj->SetStringField(TEXT("endpointUrl"), Settings.EndpointUrl);
	// Mask API key for display
	Obj->SetStringField(TEXT("apiKey"), Settings.ApiKey.IsEmpty() ? TEXT("") : TEXT("••••••••"));
	Obj->SetStringField(TEXT("model"), Settings.Model);
	Obj->SetNumberField(TEXT("dimensions"), Settings.Dimensions);
	Obj->SetBoolField(TEXT("autoIndex"), Settings.bAutoIndex);
	Obj->SetBoolField(TEXT("hasOpenRouterKey"), bHasOpenRouterKey);

	auto ScopeObj = MakeShared<FJsonObject>();
	ScopeObj->SetBoolField(TEXT("blueprints"), Settings.Scope.bBlueprints);
	ScopeObj->SetBoolField(TEXT("cppFiles"), Settings.Scope.bCppFiles);
	ScopeObj->SetBoolField(TEXT("assets"), Settings.Scope.bAssets);
	ScopeObj->SetBoolField(TEXT("levels"), Settings.Scope.bLevels);
	ScopeObj->SetBoolField(TEXT("config"), Settings.Scope.bConfig);
	ScopeObj->SetBoolField(TEXT("documents"), Settings.Scope.bDocuments);
	Obj->SetObjectField(TEXT("scope"), ScopeObj);

	return JsonObjToString(Obj);
}

// ── Settings Setters ─────────────────────────────────────────────────

void FProjectIndexManager::SetProvider(const FString& Provider)
{
	EnsureInit();
	Settings.Provider = Provider;
	SaveSettings();
}

void FProjectIndexManager::SetEndpointUrl(const FString& Url)
{
	EnsureInit();
	Settings.EndpointUrl = Url;
	SaveSettings();
}

void FProjectIndexManager::SetApiKey(const FString& Key)
{
	EnsureInit();
	if (!Key.Contains(TEXT("••")))
	{
		Settings.ApiKey = Key;
		SaveSettings();
	}
}

void FProjectIndexManager::SetModel(const FString& Model)
{
	EnsureInit();
	Settings.Model = Model;
	SaveSettings();
}

void FProjectIndexManager::SetDimensions(int32 Dims)
{
	EnsureInit();
	if (Dims > 0)
	{
		Settings.Dimensions = Dims;
		SaveSettings();
	}
}

void FProjectIndexManager::SetAutoIndex(bool bEnabled)
{
	EnsureInit();
	Settings.bAutoIndex = bEnabled;
	SaveSettings();

	if (bEnabled && Status.State == TEXT("ready"))
	{
		StartWatcher();
	}
	else
	{
		StopWatcher();
	}
}

void FProjectIndexManager::SetScopeEnabled(const FString& ScopeKey, bool bEnabled)
{
	EnsureInit();
	if      (ScopeKey == TEXT("blueprints")) Settings.Scope.bBlueprints = bEnabled;
	else if (ScopeKey == TEXT("cppFiles"))   Settings.Scope.bCppFiles = bEnabled;
	else if (ScopeKey == TEXT("assets"))     Settings.Scope.bAssets = bEnabled;
	else if (ScopeKey == TEXT("levels"))     Settings.Scope.bLevels = bEnabled;
	else if (ScopeKey == TEXT("config"))     Settings.Scope.bConfig = bEnabled;
	else if (ScopeKey == TEXT("documents"))  Settings.Scope.bDocuments = bEnabled;
	SaveSettings();
}

// ═══════════════════════════════════════════════════════════════════════
// Status I/O
// ═══════════════════════════════════════════════════════════════════════

void FProjectIndexManager::LoadStatus()
{
	FString Raw;
	if (FFileHelper::LoadFileToString(Raw, *GetStatusPath()))
	{
		TSharedPtr<FJsonObject> Obj;
		if (ParseJsonString(Raw, Obj) && Obj.IsValid())
		{
			FScopeLock Lock(&StatusLock);
			Obj->TryGetStringField(TEXT("state"), Status.State);
			Obj->TryGetNumberField(TEXT("totalChunks"), Status.TotalChunks);
			Obj->TryGetNumberField(TEXT("indexedChunks"), Status.IndexedChunks);
			Obj->TryGetStringField(TEXT("lastIndexedAt"), Status.LastIndexedAt);

			double SizeBytes = 0;
			if (Obj->TryGetNumberField(TEXT("indexSizeBytes"), SizeBytes))
			{
				Status.IndexSizeBytes = static_cast<int64>(SizeBytes);
			}

			Obj->TryGetStringField(TEXT("errorMessage"), Status.ErrorMessage);
			Obj->TryGetStringField(TEXT("embeddingModel"), Status.EmbeddingModel);
			Obj->TryGetNumberField(TEXT("embeddingDimensions"), Status.EmbeddingDimensions);

			const TSharedPtr<FJsonObject>* BdObj = nullptr;
			if (Obj->TryGetObjectField(TEXT("breakdown"), BdObj) && BdObj)
			{
				(*BdObj)->TryGetNumberField(TEXT("blueprints"), Status.Breakdown.Blueprints);
				(*BdObj)->TryGetNumberField(TEXT("cppFiles"), Status.Breakdown.CppFiles);
				(*BdObj)->TryGetNumberField(TEXT("assets"), Status.Breakdown.Assets);
				(*BdObj)->TryGetNumberField(TEXT("levels"), Status.Breakdown.Levels);
				(*BdObj)->TryGetNumberField(TEXT("config"), Status.Breakdown.Config);
				(*BdObj)->TryGetNumberField(TEXT("documents"), Status.Breakdown.Documents);
			}
		}
	}
}

void FProjectIndexManager::SaveStatus() const
{
	FScopeLock Lock(&const_cast<FProjectIndexManager*>(this)->StatusLock);

	auto Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("state"), Status.State);
	Obj->SetNumberField(TEXT("totalChunks"), Status.TotalChunks);
	Obj->SetNumberField(TEXT("indexedChunks"), Status.IndexedChunks);
	Obj->SetStringField(TEXT("lastIndexedAt"), Status.LastIndexedAt);
	Obj->SetNumberField(TEXT("indexSizeBytes"), static_cast<double>(Status.IndexSizeBytes));
	Obj->SetStringField(TEXT("errorMessage"), Status.ErrorMessage);
	Obj->SetStringField(TEXT("embeddingModel"), Status.EmbeddingModel);
	Obj->SetNumberField(TEXT("embeddingDimensions"), Status.EmbeddingDimensions);

	auto BdObj = MakeShared<FJsonObject>();
	BdObj->SetNumberField(TEXT("blueprints"), Status.Breakdown.Blueprints);
	BdObj->SetNumberField(TEXT("cppFiles"), Status.Breakdown.CppFiles);
	BdObj->SetNumberField(TEXT("assets"), Status.Breakdown.Assets);
	BdObj->SetNumberField(TEXT("levels"), Status.Breakdown.Levels);
	BdObj->SetNumberField(TEXT("config"), Status.Breakdown.Config);
	BdObj->SetNumberField(TEXT("documents"), Status.Breakdown.Documents);
	Obj->SetObjectField(TEXT("breakdown"), BdObj);

	IFileManager::Get().MakeDirectory(*GetIndexDir(), true);
	FFileHelper::SaveStringToFile(JsonObjToString(Obj), *GetStatusPath());
}

FString FProjectIndexManager::GetStatusJson() const
{
	const_cast<FProjectIndexManager*>(this)->EnsureInit();

	FScopeLock Lock(&const_cast<FProjectIndexManager*>(this)->StatusLock);

	auto Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("state"), Status.State);
	Obj->SetNumberField(TEXT("totalChunks"), Status.TotalChunks);
	Obj->SetNumberField(TEXT("indexedChunks"), Status.IndexedChunks);
	Obj->SetStringField(TEXT("lastIndexedAt"), Status.LastIndexedAt);
	Obj->SetNumberField(TEXT("indexSizeBytes"), static_cast<double>(Status.IndexSizeBytes));
	Obj->SetStringField(TEXT("errorMessage"), Status.ErrorMessage);
	Obj->SetStringField(TEXT("embeddingModel"), Status.EmbeddingModel);
	Obj->SetNumberField(TEXT("embeddingDimensions"), Status.EmbeddingDimensions);

	auto BdObj = MakeShared<FJsonObject>();
	BdObj->SetNumberField(TEXT("blueprints"), Status.Breakdown.Blueprints);
	BdObj->SetNumberField(TEXT("cppFiles"), Status.Breakdown.CppFiles);
	BdObj->SetNumberField(TEXT("assets"), Status.Breakdown.Assets);
	BdObj->SetNumberField(TEXT("levels"), Status.Breakdown.Levels);
	BdObj->SetNumberField(TEXT("config"), Status.Breakdown.Config);
	BdObj->SetNumberField(TEXT("documents"), Status.Breakdown.Documents);
	Obj->SetObjectField(TEXT("breakdown"), BdObj);

	return JsonObjToString(Obj);
}

// ═══════════════════════════════════════════════════════════════════════
// Metadata I/O
// ═══════════════════════════════════════════════════════════════════════

bool FProjectIndexManager::LoadMetadata(TArray<FIndexChunk>& OutChunks, TArray<FString>& OutTexts) const
{
	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *GetMetadataPath())) return false;

	TSharedPtr<FJsonObject> Obj;
	if (!ParseJsonString(Raw, Obj) || !Obj.IsValid()) return false;

	const TArray<TSharedPtr<FJsonValue>>* ChunksArr = nullptr;
	if (!Obj->TryGetArrayField(TEXT("chunks"), ChunksArr)) return false;

	const TArray<TSharedPtr<FJsonValue>>* TextsArr = nullptr;
	if (!Obj->TryGetArrayField(TEXT("texts"), TextsArr)) return false;

	OutChunks.Reset();
	OutTexts.Reset();

	for (const auto& Val : *ChunksArr)
	{
		const TSharedPtr<FJsonObject>* ChunkObj = nullptr;
		if (!Val->TryGetObject(ChunkObj) || !ChunkObj) continue;

		FIndexChunk Chunk;
		(*ChunkObj)->TryGetStringField(TEXT("id"), Chunk.Id);
		(*ChunkObj)->TryGetStringField(TEXT("sourcePath"), Chunk.SourcePath);
		(*ChunkObj)->TryGetStringField(TEXT("sourceType"), Chunk.SourceType);
		(*ChunkObj)->TryGetNumberField(TEXT("offset"), Chunk.Offset);
		OutChunks.Add(MoveTemp(Chunk));
	}

	for (const auto& Val : *TextsArr)
	{
		OutTexts.Add(Val->AsString());
	}

	return OutChunks.Num() > 0 && OutChunks.Num() == OutTexts.Num();
}

void FProjectIndexManager::SaveMetadata(const TArray<FIndexChunk>& Chunks, const TArray<FString>& Texts) const
{
	TArray<TSharedPtr<FJsonValue>> ChunksArr;
	for (const auto& C : Chunks)
	{
		auto CObj = MakeShared<FJsonObject>();
		CObj->SetStringField(TEXT("id"), C.Id);
		CObj->SetStringField(TEXT("sourcePath"), C.SourcePath);
		CObj->SetStringField(TEXT("sourceType"), C.SourceType);
		CObj->SetNumberField(TEXT("offset"), C.Offset);
		ChunksArr.Add(MakeShared<FJsonValueObject>(CObj));
	}

	TArray<TSharedPtr<FJsonValue>> TextsArr;
	for (const auto& T : Texts)
	{
		TextsArr.Add(MakeShared<FJsonValueString>(T));
	}

	auto Obj = MakeShared<FJsonObject>();
	Obj->SetArrayField(TEXT("chunks"), ChunksArr);
	Obj->SetArrayField(TEXT("texts"), TextsArr);
	Obj->SetNumberField(TEXT("version"), 1);

	IFileManager::Get().MakeDirectory(*GetIndexDir(), true);
	FFileHelper::SaveStringToFile(JsonObjToString(Obj), *GetMetadataPath());
}

// ═══════════════════════════════════════════════════════════════════════
// Embeddings I/O (binary Float32)
// ═══════════════════════════════════════════════════════════════════════

bool FProjectIndexManager::LoadEmbeddings(TArray<float>& OutData, int32 ExpectedCount, int32 Dims) const
{
	TArray<uint8> FileData;
	if (!FFileHelper::LoadFileToArray(FileData, *GetEmbeddingsPath())) return false;

	const int32 ExpectedBytes = ExpectedCount * Dims * sizeof(float);
	if (FileData.Num() != ExpectedBytes) return false;

	OutData.SetNumUninitialized(ExpectedCount * Dims);
	FMemory::Memcpy(OutData.GetData(), FileData.GetData(), ExpectedBytes);
	return true;
}

void FProjectIndexManager::SaveEmbeddings(const TArray<float>& Data) const
{
	IFileManager::Get().MakeDirectory(*GetIndexDir(), true);

	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(Data.Num() * sizeof(float));
	FMemory::Memcpy(Bytes.GetData(), Data.GetData(), Bytes.Num());
	FFileHelper::SaveArrayToFile(Bytes, *GetEmbeddingsPath());
}

// ═══════════════════════════════════════════════════════════════════════
// File Index I/O
// ═══════════════════════════════════════════════════════════════════════

bool FProjectIndexManager::LoadFileIndex(TMap<FString, FFileIndexEntry>& OutIndex) const
{
	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *GetFileIndexPath())) return false;

	TSharedPtr<FJsonObject> Obj;
	if (!ParseJsonString(Raw, Obj) || !Obj.IsValid()) return false;

	const TSharedPtr<FJsonObject>* FilesObj = nullptr;
	if (!Obj->TryGetObjectField(TEXT("files"), FilesObj) || !FilesObj) return false;

	OutIndex.Reset();
	for (const auto& Pair : (*FilesObj)->Values)
	{
		const TSharedPtr<FJsonObject>* EntryObj = nullptr;
		if (!Pair.Value->TryGetObject(EntryObj) || !EntryObj) continue;

		FFileIndexEntry Entry;
		(*EntryObj)->TryGetStringField(TEXT("hash"), Entry.Hash);

		double MtimeVal = 0;
		if ((*EntryObj)->TryGetNumberField(TEXT("mtime"), MtimeVal))
		{
			Entry.Mtime = MtimeVal;
		}

		(*EntryObj)->TryGetStringField(TEXT("diskPath"), Entry.DiskPath);
		(*EntryObj)->TryGetStringField(TEXT("sourceType"), Entry.SourceType);

		const TArray<TSharedPtr<FJsonValue>>* IdsArr = nullptr;
		if ((*EntryObj)->TryGetArrayField(TEXT("chunkIds"), IdsArr))
		{
			for (const auto& V : *IdsArr)
			{
				Entry.ChunkIds.Add(V->AsString());
			}
		}

		OutIndex.Add(Pair.Key, MoveTemp(Entry));
	}

	return true;
}

void FProjectIndexManager::SaveFileIndex(const TMap<FString, FFileIndexEntry>& Index) const
{
	auto FilesObj = MakeShared<FJsonObject>();
	for (const auto& Pair : Index)
	{
		auto EntryObj = MakeShared<FJsonObject>();
		EntryObj->SetStringField(TEXT("hash"), Pair.Value.Hash);
		EntryObj->SetNumberField(TEXT("mtime"), Pair.Value.Mtime);
		EntryObj->SetStringField(TEXT("diskPath"), Pair.Value.DiskPath);
		EntryObj->SetStringField(TEXT("sourceType"), Pair.Value.SourceType);

		TArray<TSharedPtr<FJsonValue>> Ids;
		for (const auto& Id : Pair.Value.ChunkIds)
		{
			Ids.Add(MakeShared<FJsonValueString>(Id));
		}
		EntryObj->SetArrayField(TEXT("chunkIds"), Ids);

		FilesObj->SetObjectField(Pair.Key, EntryObj);
	}

	auto Obj = MakeShared<FJsonObject>();
	Obj->SetObjectField(TEXT("files"), FilesObj);
	Obj->SetNumberField(TEXT("version"), 1);

	IFileManager::Get().MakeDirectory(*GetIndexDir(), true);
	FFileHelper::SaveStringToFile(JsonObjToString(Obj), *GetFileIndexPath());
}

int64 FProjectIndexManager::ComputeIndexSizeBytes() const
{
	int64 Total = 0;
	for (const FString& P : { GetMetadataPath(), GetEmbeddingsPath() })
	{
		const int64 Size = IFileManager::Get().FileSize(*P);
		if (Size > 0) Total += Size;
	}
	return Total;
}

// ═══════════════════════════════════════════════════════════════════════
// Scanner
// ═══════════════════════════════════════════════════════════════════════

static double GetFileMtime(const FString& AbsPath)
{
	FFileStatData StatData = IFileManager::Get().GetStatData(*AbsPath);
	if (StatData.bIsValid)
	{
		return static_cast<double>(StatData.ModificationTime.ToUnixTimestamp());
	}
	return 0.0;
}

static void CollectFilesRecursive(const FString& Dir, const TArray<FString>& Extensions, TArray<FString>& OutFiles)
{
	TArray<FString> Found;
	IFileManager::Get().FindFilesRecursive(Found, *Dir, TEXT("*"), true, false);
	for (const FString& F : Found)
	{
		const FString Ext = FPaths::GetExtension(F).ToLower();
		if (Extensions.Contains(Ext))
		{
			OutFiles.Add(F);
		}
	}
}

TArray<FProjectIndexManager::FSourceEntry> FProjectIndexManager::ListSources() const
{
	TArray<FSourceEntry> Results;

	const FString ProjectDir = FPaths::ProjectDir();
	const FString ContentDir = FPaths::Combine(ProjectDir, TEXT("Content"));
	const FString SourceDir  = FPaths::Combine(ProjectDir, TEXT("Source"));
	const FString ConfigDir  = FPaths::Combine(ProjectDir, TEXT("Config"));

	// ── UE Assets (via Asset Registry) ──
	if (Settings.Scope.bBlueprints || Settings.Scope.bAssets || Settings.Scope.bLevels)
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		TArray<FAssetData> AllAssets;
		FARFilter Filter;
		Filter.PackagePaths.Add(FName(TEXT("/Game")));
		Filter.bRecursivePaths = true;
		Registry.GetAssets(Filter, AllAssets);

		for (const FAssetData& Asset : AllAssets)
		{
			const FString ClassName = Asset.AssetClassPath.GetAssetName().ToString();
			const FString AssetPath = Asset.GetObjectPathString();

			// Determine type
			bool bIsBlueprint = ClassName.Contains(TEXT("Blueprint"));
			bool bIsLevel = ClassName == TEXT("World");
			bool bIsAsset = !bIsBlueprint && !bIsLevel;

			FString Type;
			if (bIsBlueprint && Settings.Scope.bBlueprints) Type = TEXT("blueprint");
			else if (bIsLevel && Settings.Scope.bLevels)    Type = TEXT("level");
			else if (bIsAsset && Settings.Scope.bAssets)     Type = TEXT("asset");
			else continue;

			// Resolve disk path
			FString PackagePath = Asset.PackageName.ToString();
			FString DiskPath;
			if (FPackageName::TryConvertLongPackageNameToFilename(PackagePath, DiskPath, FPackageName::GetAssetPackageExtension()))
			{
				DiskPath = FPaths::ConvertRelativePathToFull(DiskPath);
			}
			else
			{
				continue;
			}

			FSourceEntry Entry;
			Entry.Path = AssetPath;
			Entry.Type = Type;
			Entry.DiskPath = DiskPath;
			Entry.Mtime = GetFileMtime(DiskPath);
			Results.Add(MoveTemp(Entry));
		}
	}

	// ── C++ Source Files ──
	if (Settings.Scope.bCppFiles && IFileManager::Get().DirectoryExists(*SourceDir))
	{
		TArray<FString> CppFiles;
		CollectFilesRecursive(SourceDir, { TEXT("cpp"), TEXT("h"), TEXT("c"), TEXT("hpp") }, CppFiles);
		for (const FString& FilePath : CppFiles)
		{
			FSourceEntry Entry;
			Entry.Path = FilePath;
			Entry.Type = TEXT("cpp");
			Entry.DiskPath = FilePath;
			Entry.Mtime = GetFileMtime(FilePath);
			Results.Add(MoveTemp(Entry));
		}
	}

	// ── Config Files ──
	if (Settings.Scope.bConfig && IFileManager::Get().DirectoryExists(*ConfigDir))
	{
		TArray<FString> ConfigFiles;
		CollectFilesRecursive(ConfigDir, { TEXT("ini"), TEXT("cfg") }, ConfigFiles);
		for (const FString& FilePath : ConfigFiles)
		{
			FSourceEntry Entry;
			Entry.Path = FilePath;
			Entry.Type = TEXT("config");
			Entry.DiskPath = FilePath;
			Entry.Mtime = GetFileMtime(FilePath);
			Results.Add(MoveTemp(Entry));
		}
	}

	// ── Documents (game docs, design docs, PDFs, etc.) ──
	if (Settings.Scope.bDocuments)
	{
		static const TArray<FString> DocExtensions = {
			TEXT("md"), TEXT("txt"), TEXT("rst"), TEXT("html"), TEXT("htm"),
			TEXT("csv"), TEXT("json"), TEXT("yaml"), TEXT("yml"), TEXT("xml"),
			TEXT("pdf"), TEXT("rtf"), TEXT("log")
		};

		// Scan common doc locations: project root + well-known subdirs
		TArray<FString> DocSearchDirs;
		DocSearchDirs.Add(ProjectDir); // root-level docs (README.md, CLAUDE.md, etc.)

		// Known doc folder names
		static const TArray<FString> DocFolderNames = {
			TEXT("Docs"), TEXT("Documentation"), TEXT("Design"), TEXT("GDD"),
			TEXT("Notes"), TEXT("Wiki"), TEXT("Reference"), TEXT("Guides")
		};
		for (const FString& FolderName : DocFolderNames)
		{
			const FString Dir = FPaths::Combine(ProjectDir, FolderName);
			if (IFileManager::Get().DirectoryExists(*Dir))
			{
				DocSearchDirs.Add(Dir);
			}
		}

		// Also check inside Plugins/ for plugin docs
		const FString PluginsDir = FPaths::Combine(ProjectDir, TEXT("Plugins"));
		if (IFileManager::Get().DirectoryExists(*PluginsDir))
		{
			DocSearchDirs.Add(PluginsDir);
		}

		for (const FString& SearchDir : DocSearchDirs)
		{
			TArray<FString> DocFiles;

			if (SearchDir == ProjectDir)
			{
				// For project root, only grab top-level files (not recursive — avoid Content/, Source/, etc.)
				TArray<FString> RootFiles;
				IFileManager::Get().FindFiles(RootFiles, *FPaths::Combine(ProjectDir, TEXT("*")), true, false);
				for (const FString& FileName : RootFiles)
				{
					const FString Ext = FPaths::GetExtension(FileName).ToLower();
					if (DocExtensions.Contains(Ext))
					{
						DocFiles.Add(FPaths::Combine(ProjectDir, FileName));
					}
				}
			}
			else
			{
				CollectFilesRecursive(SearchDir, DocExtensions, DocFiles);
			}

			for (const FString& FilePath : DocFiles)
			{
				// Skip node_modules, build artifacts, binary dirs
				if (FilePath.Contains(TEXT("node_modules")) || FilePath.Contains(TEXT("/build/")) ||
					FilePath.Contains(TEXT("/Binaries/")) || FilePath.Contains(TEXT("/Intermediate/")))
				{
					continue;
				}

				FSourceEntry Entry;
				Entry.Path = FilePath;
				Entry.Type = TEXT("document");
				Entry.DiskPath = FilePath;
				Entry.Mtime = GetFileMtime(FilePath);
				Results.Add(MoveTemp(Entry));
			}
		}
	}

	return Results;
}

// ── Content Reading ──────────────────────────────────────────────────

FString FProjectIndexManager::ReadSourceContent(const FSourceEntry& Entry) const
{
	// C++ and config: read file directly
	if (Entry.Type == TEXT("cpp") || Entry.Type == TEXT("config"))
	{
		FString Content;
		if (FFileHelper::LoadFileToString(Content, *Entry.DiskPath))
		{
			return Content;
		}
		return FString();
	}

	// Documents: read text-based files directly, PDFs via extraction
	if (Entry.Type == TEXT("document"))
	{
		return ReadDocumentFile(Entry.DiskPath);
	}

	// UE assets: use Lua tool for rich text representation
	return ReadAssetViaLua(Entry.Path);
}

FString FProjectIndexManager::ReadDocumentFile(const FString& FilePath) const
{
	const FString Ext = FPaths::GetExtension(FilePath).ToLower();

	// PDF: extract via pdftotext
	if (Ext == TEXT("pdf"))
	{
		return ExtractPdfText(FilePath);
	}

	// Text-based formats: read directly
	FString Content;
	if (FFileHelper::LoadFileToString(Content, *FilePath))
	{
		// Sanity check: skip files that look binary (too many null/control chars)
		int32 ControlCount = 0;
		const int32 CheckLen = FMath::Min(Content.Len(), 1024);
		for (int32 i = 0; i < CheckLen; i++)
		{
			const TCHAR Ch = Content[i];
			if (Ch != TEXT('\n') && Ch != TEXT('\r') && Ch != TEXT('\t') && Ch < 32)
			{
				ControlCount++;
			}
		}
		if (CheckLen > 0 && ControlCount > CheckLen / 4)
		{
			return FString(); // Likely binary — skip
		}

		return Content;
	}

	return FString();
}

FString FProjectIndexManager::ExtractPdfText(const FString& FilePath)
{
	// Try pdftotext (from poppler-utils — available via brew install poppler)
	FString Output;
	int32 ReturnCode = -1;

	const FString Command = FString::Printf(TEXT("pdftotext \"%s\" -"), *FilePath);

#if PLATFORM_MAC || PLATFORM_LINUX
	// Try common install paths
	static const TArray<FString> PdfToTextPaths = {
		TEXT("/opt/homebrew/bin/pdftotext"),
		TEXT("/usr/local/bin/pdftotext"),
		TEXT("/usr/bin/pdftotext")
	};

	FString PdfToTextBin;
	for (const FString& Path : PdfToTextPaths)
	{
		if (IFileManager::Get().FileExists(*Path))
		{
			PdfToTextBin = Path;
			break;
		}
	}

	if (PdfToTextBin.IsEmpty())
	{
		UE_LOG(LogTemp, Verbose, TEXT("[ProjectIndex] pdftotext not found — skipping PDF: %s"), *FilePath);
		return FString();
	}

	const FString FullCmd = FString::Printf(TEXT("\"%s\" \"%s\" -"), *PdfToTextBin, *FilePath);
	FPlatformProcess::ExecProcess(*PdfToTextBin, *FString::Printf(TEXT("\"%s\" -"), *FilePath), &ReturnCode, &Output, nullptr);
#else
	// Windows: try pdftotext in PATH
	FPlatformProcess::ExecProcess(TEXT("pdftotext"), *FString::Printf(TEXT("\"%s\" -"), *FilePath), &ReturnCode, &Output, nullptr);
#endif

	if (ReturnCode == 0 && !Output.IsEmpty())
	{
		return Output;
	}

	return FString();
}

FString FProjectIndexManager::ReadAssetViaLua(const FString& AssetPath) const
{
	// Extract the package path for open_asset (strip the .ClassName suffix if present)
	FString PackagePath = AssetPath;
	int32 DotIdx = INDEX_NONE;
	if (PackagePath.FindLastChar(TEXT('.'), DotIdx))
	{
		PackagePath = PackagePath.Left(DotIdx);
	}

	const FString Script = FString::Printf(
		TEXT("local a = open_asset(\"%s\")\nif a and a.info then return a:info() else return \"\" end"),
		*PackagePath
	);

	auto ArgsObj = MakeShared<FJsonObject>();
	ArgsObj->SetStringField(TEXT("script"), Script);

	FToolResult Result = FNeoStackToolRegistry::Get().Execute(TEXT("execute_lua"), ArgsObj);
	if (Result.bSuccess && !Result.Output.IsEmpty())
	{
		return Result.Output;
	}

	// Fallback: basic metadata
	return FString::Printf(TEXT("[%s] %s"), *PackagePath, *FPaths::GetBaseFilename(PackagePath));
}

// ═══════════════════════════════════════════════════════════════════════
// Chunker (ported from TypeScript)
// ═══════════════════════════════════════════════════════════════════════

static const int32 TARGET_CHUNK_SIZE = 2000;
static const int32 CHUNK_OVERLAP = 200;

FString FProjectIndexManager::HashContent(const FString& Content)
{
	return FMD5::HashAnsiString(*Content);
}

FString FProjectIndexManager::MakeChunkId(const FString& Path, int32 Offset)
{
	const FString Combined = FString::Printf(TEXT("%s:%d"), *Path, Offset);
	const FString Hash = FMD5::HashAnsiString(*Combined);
	return Hash.Left(16);
}

static TArray<FString> SplitOnBoundaries(const FString& Text)
{
	TArray<FString> Sections;
	FString Current;

	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, false);

	int32 EmptyCount = 0;
	for (const FString& Line : Lines)
	{
		if (Line.TrimStartAndEnd().IsEmpty())
		{
			EmptyCount++;
			if (EmptyCount >= 2 && !Current.IsEmpty())
			{
				Sections.Add(Current);
				Current.Empty();
				EmptyCount = 0;
			}
			else
			{
				Current += Line + TEXT("\n");
			}
		}
		else
		{
			EmptyCount = 0;
			Current += Line + TEXT("\n");
		}
	}

	if (!Current.TrimStartAndEnd().IsEmpty())
	{
		Sections.Add(Current);
	}
	return Sections;
}

static TArray<FString> SplitCodeBlock(const FString& Text)
{
	TArray<FString> Blocks;
	FString Current;

	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, false);

	for (const FString& Line : Lines)
	{
		Current += Line + TEXT("\n");

		if (Line.TrimStartAndEnd().IsEmpty() && !Current.TrimStartAndEnd().IsEmpty())
		{
			Blocks.Add(Current);
			Current.Empty();
		}
	}

	if (!Current.TrimStartAndEnd().IsEmpty())
	{
		Blocks.Add(Current);
	}
	return Blocks;
}

static TArray<FString> MergeSmallSections(const TArray<FString>& Sections, int32 TargetSize)
{
	TArray<FString> Merged;
	FString Current;

	for (const FString& Section : Sections)
	{
		if (Current.Len() + Section.Len() <= TargetSize)
		{
			if (!Current.IsEmpty()) Current += TEXT("\n\n");
			Current += Section;
		}
		else
		{
			if (!Current.IsEmpty()) Merged.Add(Current);
			Current = Section;
		}
	}
	if (!Current.IsEmpty()) Merged.Add(Current);
	return Merged;
}

static TArray<FString> SplitLargeChunk(const FString& Text, int32 TargetSize)
{
	if (Text.Len() <= TargetSize) return { Text };

	TArray<FString> Chunks;
	int32 Start = 0;

	while (Start < Text.Len())
	{
		int32 End = FMath::Min(Start + TargetSize, Text.Len());

		// Try to break on a newline
		if (End < Text.Len())
		{
			int32 LastNewline = Text.Find(TEXT("\n"), ESearchCase::IgnoreCase, ESearchDir::FromEnd, End);
			if (LastNewline > Start + TargetSize / 2)
			{
				End = LastNewline + 1;
			}
		}

		Chunks.Add(Text.Mid(Start, End - Start));
		Start = FMath::Max(Start + 1, End - CHUNK_OVERLAP);
	}

	return Chunks;
}

TArray<FProjectIndexManager::FIndexChunk> FProjectIndexManager::ChunkContent(
	const FString& Path, const FString& Type, const FString& Content)
{
	TArray<FIndexChunk> Result;
	if (Content.TrimStartAndEnd().IsEmpty()) return Result;

	// Choose splitting strategy
	const bool bIsCode = (Type == TEXT("cpp") || Type == TEXT("config"));
	TArray<FString> Sections = bIsCode ? SplitCodeBlock(Content) : SplitOnBoundaries(Content);

	// Merge small → split large
	TArray<FString> Merged = MergeSmallSections(Sections, TARGET_CHUNK_SIZE);
	TArray<FString> Final;
	for (const FString& S : Merged)
	{
		Final.Append(SplitLargeChunk(S, TARGET_CHUNK_SIZE));
	}

	// Build chunks with source prefix
	const FString Prefix = FString::Printf(TEXT("[%s] %s\n---\n"), *Type, *Path);
	int32 CharOffset = 0;

	for (const FString& Text : Final)
	{
		FIndexChunk Chunk;
		Chunk.Id = MakeChunkId(Path, CharOffset);
		Chunk.SourcePath = Path;
		Chunk.SourceType = Type;
		Chunk.Text = Prefix + Text;
		Chunk.Offset = CharOffset;
		CharOffset += Text.Len();
		Result.Add(MoveTemp(Chunk));
	}

	return Result;
}

// ═══════════════════════════════════════════════════════════════════════
// Embedder
// ═══════════════════════════════════════════════════════════════════════

FString FProjectIndexManager::ResolveEndpoint() const
{
	if (Settings.Provider == TEXT("openrouter"))
	{
		return TEXT("https://openrouter.ai/api/v1/embeddings");
	}

	// Custom endpoint: ensure /v1/embeddings suffix
	FString Url = Settings.EndpointUrl.TrimStartAndEnd();
	if (Url.IsEmpty())
	{
		return FString();
	}
	while (Url.EndsWith(TEXT("/"))) Url.LeftChopInline(1);

	if (!Url.EndsWith(TEXT("/embeddings")))
	{
		if (!Url.EndsWith(TEXT("/v1")))
		{
			Url += TEXT("/v1");
		}
		Url += TEXT("/embeddings");
	}
	return Url;
}

FString FProjectIndexManager::ResolveApiKey() const
{
	if (Settings.Provider == TEXT("openrouter"))
	{
		// Fall back to ACPSettings OpenRouter key
		if (const UACPSettings* S = UACPSettings::Get())
		{
			if (!S->OpenRouterApiKey.IsEmpty())
			{
				return S->OpenRouterApiKey;
			}
		}
	}
	return Settings.ApiKey;
}

static const int32 EMBED_BATCH_SIZE = 50;

bool FProjectIndexManager::EmbedBatchSync(
	const TArray<FString>& Texts,
	TArray<TArray<float>>& OutEmbeddings,
	FString& OutError) const
{
	OutEmbeddings.Reset();

	const FString Endpoint = ResolveEndpoint();
	const FString ApiKey = ResolveApiKey();

	if (Endpoint.IsEmpty())
	{
		OutError = TEXT("No embedding endpoint configured");
		return false;
	}

	for (int32 BatchStart = 0; BatchStart < Texts.Num(); BatchStart += EMBED_BATCH_SIZE)
	{
		const int32 BatchEnd = FMath::Min(BatchStart + EMBED_BATCH_SIZE, Texts.Num());

		// Build request body
		TArray<TSharedPtr<FJsonValue>> InputArr;
		for (int32 i = BatchStart; i < BatchEnd; i++)
		{
			InputArr.Add(MakeShared<FJsonValueString>(Texts[i]));
		}

		auto BodyObj = MakeShared<FJsonObject>();
		BodyObj->SetStringField(TEXT("model"), Settings.Model);
		BodyObj->SetArrayField(TEXT("input"), InputArr);
		if (Settings.Dimensions > 0)
		{
			BodyObj->SetNumberField(TEXT("dimensions"), Settings.Dimensions);
		}

		const FString BodyStr = JsonObjToString(BodyObj);

		// Synchronous HTTP via FEvent
		FEvent* DoneEvent = FPlatformProcess::GetSynchEventFromPool();
		bool bRequestSuccess = false;
		FString ResponseBody;
		int32 ResponseCode = 0;

		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
		Req->SetURL(Endpoint);
		Req->SetVerb(TEXT("POST"));
		Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		if (!ApiKey.IsEmpty())
		{
			Req->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
		}
		Req->SetContentAsString(BodyStr);
		Req->SetTimeout(60.0f);

		Req->OnProcessRequestComplete().BindLambda(
			[&](FHttpRequestPtr, FHttpResponsePtr Response, bool bSuccess)
			{
				bRequestSuccess = bSuccess && Response.IsValid();
				if (bRequestSuccess)
				{
					ResponseCode = Response->GetResponseCode();
					ResponseBody = Response->GetContentAsString();
				}
				DoneEvent->Trigger();
			}
		);

		if (!Req->ProcessRequest())
		{
			FPlatformProcess::ReturnSynchEventToPool(DoneEvent);
			OutError = TEXT("Failed to start HTTP request");
			return false;
		}

		DoneEvent->Wait();
		FPlatformProcess::ReturnSynchEventToPool(DoneEvent);

		if (!bRequestSuccess)
		{
			OutError = TEXT("Embedding API request failed (network error)");
			return false;
		}

		if (ResponseCode != 200)
		{
			OutError = FString::Printf(TEXT("Embedding API error %d: %s"),
				ResponseCode, *ResponseBody.Left(200));
			return false;
		}

		// Parse response
		TSharedPtr<FJsonObject> RespObj;
		if (!ParseJsonString(ResponseBody, RespObj) || !RespObj.IsValid())
		{
			OutError = TEXT("Invalid embedding API response JSON");
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* DataArr = nullptr;
		if (!RespObj->TryGetArrayField(TEXT("data"), DataArr) || !DataArr)
		{
			OutError = TEXT("Embedding API response missing 'data' array");
			return false;
		}

		// Sort by index, extract embeddings
		struct FEmbeddingEntry { int32 Index; TArray<float> Values; };
		TArray<FEmbeddingEntry> Entries;

		for (const auto& Val : *DataArr)
		{
			const TSharedPtr<FJsonObject>* ItemObj = nullptr;
			if (!Val->TryGetObject(ItemObj) || !ItemObj) continue;

			FEmbeddingEntry E;
			(*ItemObj)->TryGetNumberField(TEXT("index"), E.Index);

			const TArray<TSharedPtr<FJsonValue>>* EmbArr = nullptr;
			if ((*ItemObj)->TryGetArrayField(TEXT("embedding"), EmbArr) && EmbArr)
			{
				for (const auto& V : *EmbArr)
				{
					E.Values.Add(static_cast<float>(V->AsNumber()));
				}
			}
			Entries.Add(MoveTemp(E));
		}

		Entries.Sort([](const FEmbeddingEntry& A, const FEmbeddingEntry& B) { return A.Index < B.Index; });

		for (auto& E : Entries)
		{
			OutEmbeddings.Add(MoveTemp(E.Values));
		}

		// Rate limit between batches
		if (BatchStart + EMBED_BATCH_SIZE < Texts.Num())
		{
			FPlatformProcess::Sleep(0.1f);
		}
	}

	return true;
}

// ═══════════════════════════════════════════════════════════════════════
// Search
// ═══════════════════════════════════════════════════════════════════════

float FProjectIndexManager::CosineSimilarity(const float* A, const float* B, int32 Dims)
{
	float Dot = 0, NormA = 0, NormB = 0;
	for (int32 i = 0; i < Dims; i++)
	{
		Dot   += A[i] * B[i];
		NormA += A[i] * A[i];
		NormB += B[i] * B[i];
	}
	const float Denom = FMath::Sqrt(NormA) * FMath::Sqrt(NormB);
	// Epsilon floor avoids degenerate near-zero vectors amplifying noise
	return Denom > 1.0e-7f ? Dot / Denom : 0.0f;
}

FString FProjectIndexManager::Search(const FString& Query, int32 TopK)
{
	EnsureInit();

	// Load stored data
	TArray<FIndexChunk> Chunks;
	TArray<FString> Texts;
	if (!LoadMetadata(Chunks, Texts))
	{
		return TEXT("{\"ok\":false,\"error\":\"No index found\"}");
	}

	const int32 Dims = Status.EmbeddingDimensions > 0 ? Status.EmbeddingDimensions : Settings.Dimensions;
	TArray<float> Embeddings;
	if (!LoadEmbeddings(Embeddings, Chunks.Num(), Dims))
	{
		return TEXT("{\"ok\":false,\"error\":\"Failed to load embeddings\"}");
	}

	// Embed the query
	TArray<TArray<float>> QueryEmbeddings;
	FString EmbedError;
	if (!EmbedBatchSync({ Query }, QueryEmbeddings, EmbedError) || QueryEmbeddings.Num() == 0)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"Failed to embed query: %s\"}"), *EmbedError);
	}

	const TArray<float>& QueryVec = QueryEmbeddings[0];
	if (QueryVec.Num() != Dims)
	{
		return TEXT("{\"ok\":false,\"error\":\"Query embedding dimension mismatch\"}");
	}

	// Compute similarities
	struct FScored { int32 Index; float Score; };
	TArray<FScored> Scores;
	Scores.Reserve(Chunks.Num());

	for (int32 i = 0; i < Chunks.Num(); i++)
	{
		const float Score = CosineSimilarity(&Embeddings[i * Dims], QueryVec.GetData(), Dims);
		Scores.Add({ i, Score });
	}

	Scores.Sort([](const FScored& A, const FScored& B) { return A.Score > B.Score; });

	// Build results JSON
	TArray<TSharedPtr<FJsonValue>> ResultsArr;
	const int32 Count = FMath::Min(TopK, Scores.Num());
	for (int32 i = 0; i < Count; i++)
	{
		const auto& S = Scores[i];
		const auto& C = Chunks[S.Index];

		auto RObj = MakeShared<FJsonObject>();
		RObj->SetStringField(TEXT("sourcePath"), C.SourcePath);
		RObj->SetStringField(TEXT("sourceType"), C.SourceType);
		RObj->SetStringField(TEXT("text"), Texts[S.Index]);
		RObj->SetNumberField(TEXT("score"), FMath::RoundToFloat(S.Score * 1000.0f) / 1000.0f);
		RObj->SetNumberField(TEXT("offset"), C.Offset);
		ResultsArr.Add(MakeShared<FJsonValueObject>(RObj));
	}

	auto OutObj = MakeShared<FJsonObject>();
	OutObj->SetBoolField(TEXT("ok"), true);
	OutObj->SetArrayField(TEXT("results"), ResultsArr);

	return JsonObjToString(OutObj);
}

// ═══════════════════════════════════════════════════════════════════════
// Pipeline
// ═══════════════════════════════════════════════════════════════════════

void FProjectIndexManager::StartIndexing()
{
	EnsureInit();

	if (bPipelineRunning) return;
	bPipelineRunning = true;

	{
		FScopeLock Lock(&StatusLock);
		Status.State = TEXT("indexing");
		Status.ErrorMessage.Empty();
		Status.TotalChunks = 0;
		Status.IndexedChunks = 0;
	}
	SaveStatus();

	// Run pipeline on background thread
	Async(EAsyncExecution::Thread, [this]() { RunPipeline(); });
}

void FProjectIndexManager::RunPipeline()
{
	auto SetError = [this](const FString& Msg)
	{
		FScopeLock Lock(&StatusLock);
		Status.State = TEXT("error");
		Status.ErrorMessage = Msg;
		SaveStatus();
		bPipelineRunning = false;
	};

	// ── Phase 1: Enumerate sources (game thread for Asset Registry) ──
	TArray<FSourceEntry> Sources;
	{
		FEvent* Done = FPlatformProcess::GetSynchEventFromPool();
		AsyncTask(ENamedThreads::GameThread, [this, &Sources, Done]()
		{
			Sources = ListSources();
			Done->Trigger();
		});
		Done->Wait();
		FPlatformProcess::ReturnSynchEventToPool(Done);
	}

	// ── Phase 2: Load previous index for incremental build ──
	TMap<FString, FFileIndexEntry> PrevFileIndex;
	LoadFileIndex(PrevFileIndex);

	const bool bModelChanged = !Status.EmbeddingModel.IsEmpty() &&
		(Status.EmbeddingModel != Settings.Model || Status.EmbeddingDimensions != Settings.Dimensions);

	if (bModelChanged)
	{
		PrevFileIndex.Reset(); // Force full rebuild
	}

	// ── Phase 3: Diff sources ──
	TSet<FString> CurrentPaths;
	for (const auto& S : Sources) CurrentPaths.Add(S.Path);

	struct FDiffItem
	{
		FSourceEntry Source;
		enum { New, Changed, Unchanged } Action;
	};
	TArray<FDiffItem> Diff;

	for (const auto& Source : Sources)
	{
		const FFileIndexEntry* Prev = PrevFileIndex.Find(Source.Path);
		if (!Prev)
		{
			Diff.Add({ Source, FDiffItem::New });
		}
		else if (Prev->Mtime == Source.Mtime)
		{
			Diff.Add({ Source, FDiffItem::Unchanged });
		}
		else
		{
			Diff.Add({ Source, FDiffItem::Changed });
		}
	}

	// ── Phase 4: Read + hash changed files, chunk ──
	struct FToEmbed
	{
		FSourceEntry Source;
		TArray<FIndexChunk> Chunks;
		FString Hash;
	};
	TArray<FToEmbed> ToEmbed;
	TArray<TPair<FString, FFileIndexEntry>> UnchangedEntries;

	for (const auto& Item : Diff)
	{
		if (Item.Action == FDiffItem::Unchanged)
		{
			if (const FFileIndexEntry* Prev = PrevFileIndex.Find(Item.Source.Path))
			{
				UnchangedEntries.Add({ Item.Source.Path, *Prev });
			}
			continue;
		}

		// Read content (game thread dispatch for UE assets)
		FString Content;
		{
			FEvent* Done = FPlatformProcess::GetSynchEventFromPool();
			AsyncTask(ENamedThreads::GameThread, [this, &Item, &Content, Done]()
			{
				Content = ReadSourceContent(Item.Source);
				Done->Trigger();
			});
			Done->Wait();
			FPlatformProcess::ReturnSynchEventToPool(Done);
		}

		if (Content.IsEmpty()) continue;

		const FString Hash = HashContent(Content);

		// Check if content actually changed
		const FFileIndexEntry* Prev = PrevFileIndex.Find(Item.Source.Path);
		if (Prev && Prev->Hash == Hash)
		{
			FFileIndexEntry Updated = *Prev;
			Updated.Mtime = Item.Source.Mtime;
			UnchangedEntries.Add({ Item.Source.Path, Updated });
			continue;
		}

		auto Chunks = ChunkContent(Item.Source.Path, Item.Source.Type, Content);
		if (Chunks.Num() > 0)
		{
			ToEmbed.Add({ Item.Source, MoveTemp(Chunks), Hash });
		}
	}

	// ── Phase 5: Reconstruct kept chunks + embeddings ──
	TSet<FString> KeepChunkIds;
	for (const auto& UE : UnchangedEntries)
	{
		for (const auto& Id : UE.Value.ChunkIds)
		{
			KeepChunkIds.Add(Id);
		}
	}

	const int32 PrevDims = Status.EmbeddingDimensions > 0 ? Status.EmbeddingDimensions : Settings.Dimensions;

	TArray<FIndexChunk> KeptChunks;
	TArray<FString> KeptTexts;
	TArray<TArray<float>> KeptEmbRows;

	TArray<FIndexChunk> PrevChunks;
	TArray<FString> PrevTexts;
	if (KeepChunkIds.Num() > 0 && LoadMetadata(PrevChunks, PrevTexts))
	{
		TArray<float> PrevEmb;
		if (LoadEmbeddings(PrevEmb, PrevChunks.Num(), PrevDims))
		{
			for (int32 i = 0; i < PrevChunks.Num(); i++)
			{
				if (KeepChunkIds.Contains(PrevChunks[i].Id))
				{
					KeptChunks.Add(PrevChunks[i]);
					KeptTexts.Add(PrevTexts[i]);

					TArray<float> Row;
					Row.SetNumUninitialized(PrevDims);
					FMemory::Memcpy(Row.GetData(), &PrevEmb[i * PrevDims], PrevDims * sizeof(float));
					KeptEmbRows.Add(MoveTemp(Row));
				}
			}
		}
	}

	// ── Phase 6: Embed new/changed chunks ──
	TArray<FIndexChunk> NewChunksFlat;
	for (const auto& T : ToEmbed)
	{
		NewChunksFlat.Append(T.Chunks);
	}

	const int32 TotalToEmbed = NewChunksFlat.Num();

	if (TotalToEmbed > 0)
	{
		{
			FScopeLock Lock(&StatusLock);
			Status.TotalChunks = TotalToEmbed;
			Status.IndexedChunks = 0;
		}
		SaveStatus();

		TArray<FString> TextsToEmbed;
		for (const auto& C : NewChunksFlat)
		{
			TextsToEmbed.Add(C.Text);
		}

		TArray<TArray<float>> NewEmbeddings;
		FString EmbedError;
		if (!EmbedBatchSync(TextsToEmbed, NewEmbeddings, EmbedError))
		{
			SetError(FString::Printf(TEXT("Embedding failed: %s"), *EmbedError));
			return;
		}

		if (NewEmbeddings.Num() != TotalToEmbed)
		{
			SetError(TEXT("Embedding count mismatch"));
			return;
		}

		// Merge with kept
		for (int32 i = 0; i < NewChunksFlat.Num(); i++)
		{
			KeptChunks.Add(NewChunksFlat[i]);
			KeptTexts.Add(NewChunksFlat[i].Text);
			KeptEmbRows.Add(MoveTemp(NewEmbeddings[i]));
		}
	}
	else if (ToEmbed.Num() == 0 && UnchangedEntries.Num() > 0)
	{
		// Nothing changed
		FScopeLock Lock(&StatusLock);
		Status.State = TEXT("ready");
		Status.LastIndexedAt = FDateTime::UtcNow().ToIso8601();
		SaveStatus();

		if (Settings.bAutoIndex)
		{
			AsyncTask(ENamedThreads::GameThread, [this]() { StartWatcher(); });
		}
		bPipelineRunning = false;
		return;
	}

	// ── Phase 7: Save everything ──
	const int32 Dims = KeptEmbRows.Num() > 0 ? KeptEmbRows[0].Num() : Settings.Dimensions;
	const int32 FinalCount = KeptChunks.Num();

	// Save metadata
	SaveMetadata(KeptChunks, KeptTexts);

	// Save embeddings as flat Float32
	TArray<float> FlatEmb;
	FlatEmb.SetNumUninitialized(FinalCount * Dims);
	for (int32 i = 0; i < KeptEmbRows.Num(); i++)
	{
		if (KeptEmbRows[i].Num() == Dims)
		{
			FMemory::Memcpy(&FlatEmb[i * Dims], KeptEmbRows[i].GetData(), Dims * sizeof(float));
		}
	}
	SaveEmbeddings(FlatEmb);

	// Build new file index
	TMap<FString, FFileIndexEntry> NewFileIndex;
	for (const auto& UE : UnchangedEntries)
	{
		NewFileIndex.Add(UE.Key, UE.Value);
	}
	for (const auto& Item : ToEmbed)
	{
		FFileIndexEntry Entry;
		Entry.Hash = Item.Hash;
		Entry.Mtime = Item.Source.Mtime;
		Entry.DiskPath = Item.Source.DiskPath;
		Entry.SourceType = Item.Source.Type;
		for (const auto& C : Item.Chunks)
		{
			Entry.ChunkIds.Add(C.Id);
		}
		NewFileIndex.Add(Item.Source.Path, MoveTemp(Entry));
	}
	SaveFileIndex(NewFileIndex);

	// Compute breakdown
	FScopeBreakdown Breakdown;
	for (const auto& C : KeptChunks)
	{
		if      (C.SourceType == TEXT("blueprint")) Breakdown.Blueprints++;
		else if (C.SourceType == TEXT("cpp"))        Breakdown.CppFiles++;
		else if (C.SourceType == TEXT("asset"))      Breakdown.Assets++;
		else if (C.SourceType == TEXT("level"))       Breakdown.Levels++;
		else if (C.SourceType == TEXT("config"))      Breakdown.Config++;
		else if (C.SourceType == TEXT("document"))    Breakdown.Documents++;
	}

	{
		FScopeLock Lock(&StatusLock);
		Status.State = TEXT("ready");
		Status.TotalChunks = FinalCount;
		Status.IndexedChunks = FinalCount;
		Status.LastIndexedAt = FDateTime::UtcNow().ToIso8601();
		Status.IndexSizeBytes = ComputeIndexSizeBytes();
		Status.Breakdown = Breakdown;
		Status.EmbeddingModel = Settings.Model;
		Status.EmbeddingDimensions = Dims;
		Status.ErrorMessage.Empty();
	}
	SaveStatus();

	// Start watcher if auto-index
	if (Settings.bAutoIndex)
	{
		AsyncTask(ENamedThreads::GameThread, [this]() { StartWatcher(); });
	}

	bPipelineRunning = false;

	UE_LOG(LogTemp, Log, TEXT("[ProjectIndex] Indexing complete: %d chunks, %d dimensions"), FinalCount, Dims);
}

// ═══════════════════════════════════════════════════════════════════════
// Clear
// ═══════════════════════════════════════════════════════════════════════

void FProjectIndexManager::ClearIndex()
{
	EnsureInit();
	StopWatcher();

	// Delete index files
	for (const FString& P : { GetMetadataPath(), GetEmbeddingsPath(), GetStatusPath(), GetFileIndexPath() })
	{
		IFileManager::Get().Delete(*P);
	}

	{
		FScopeLock Lock(&StatusLock);
		Status = FIndexingStatus();
	}

	bPipelineRunning = false;
}

// ═══════════════════════════════════════════════════════════════════════
// Auto-Index Watcher
// ═══════════════════════════════════════════════════════════════════════

void FProjectIndexManager::StartWatcher()
{
	StopWatcher();

	if (!Settings.bAutoIndex) return;

	FDirectoryWatcherModule& DWModule = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
	IDirectoryWatcher* Watcher = DWModule.Get();
	if (!Watcher) return;

	const FString ProjectDir = FPaths::ProjectDir();
	TArray<FString> WatchDirs;

	if (Settings.Scope.bBlueprints || Settings.Scope.bAssets || Settings.Scope.bLevels)
	{
		const FString ContentDir = FPaths::Combine(ProjectDir, TEXT("Content"));
		if (IFileManager::Get().DirectoryExists(*ContentDir))
		{
			WatchDirs.Add(ContentDir);
		}
	}
	if (Settings.Scope.bCppFiles)
	{
		const FString SourceDir = FPaths::Combine(ProjectDir, TEXT("Source"));
		if (IFileManager::Get().DirectoryExists(*SourceDir))
		{
			WatchDirs.Add(SourceDir);
		}
	}
	if (Settings.Scope.bConfig)
	{
		const FString ConfigDir = FPaths::Combine(ProjectDir, TEXT("Config"));
		if (IFileManager::Get().DirectoryExists(*ConfigDir))
		{
			WatchDirs.Add(ConfigDir);
		}
	}
	if (Settings.Scope.bDocuments)
	{
		static const TArray<FString> DocFolderNames = {
			TEXT("Docs"), TEXT("Documentation"), TEXT("Design"), TEXT("GDD"),
			TEXT("Notes"), TEXT("Wiki"), TEXT("Reference"), TEXT("Guides")
		};
		for (const FString& FolderName : DocFolderNames)
		{
			const FString Dir = FPaths::Combine(ProjectDir, FolderName);
			if (IFileManager::Get().DirectoryExists(*Dir))
			{
				WatchDirs.Add(Dir);
			}
		}
	}

	for (const FString& Dir : WatchDirs)
	{
		FDelegateHandle Handle;
		Watcher->RegisterDirectoryChangedCallback_Handle(
			Dir,
			IDirectoryWatcher::FDirectoryChanged::CreateLambda(
				[this](const TArray<FFileChangeData>& Changes)
				{
					// Debounce: only trigger if not already indexing
					if (bPipelineRunning) return;

					// Check for relevant file types
					bool bRelevant = false;
					for (const auto& Change : Changes)
					{
						const FString Ext = FPaths::GetExtension(Change.Filename).ToLower();
						if (Ext == TEXT("uasset") || Ext == TEXT("umap") ||
							Ext == TEXT("cpp") || Ext == TEXT("h") || Ext == TEXT("c") || Ext == TEXT("hpp") ||
							Ext == TEXT("ini") || Ext == TEXT("cfg") ||
							Ext == TEXT("md") || Ext == TEXT("txt") || Ext == TEXT("pdf") ||
							Ext == TEXT("json") || Ext == TEXT("yaml") || Ext == TEXT("yml"))
						{
							bRelevant = true;
							break;
						}
					}

					if (bRelevant)
					{
						UE_LOG(LogTemp, Log, TEXT("[ProjectIndex] Auto-index: changes detected, starting re-index"));
						StartIndexing();
					}
				}
			),
			Handle,
			IDirectoryWatcher::WatchOptions::IncludeDirectoryChanges
		);

		if (Handle.IsValid())
		{
			WatcherHandles.Add(Handle);
		}
	}

	if (WatcherHandles.Num() > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("[ProjectIndex] Auto-index watcher started on %d directories"), WatchDirs.Num());
	}
}

void FProjectIndexManager::StopWatcher()
{
	if (WatcherHandles.Num() == 0) return;

	if (FModuleManager::Get().IsModuleLoaded(TEXT("DirectoryWatcher")))
	{
		FDirectoryWatcherModule& DWModule = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
		IDirectoryWatcher* Watcher = DWModule.Get();
		if (Watcher)
		{
			// DirectoryWatcher doesn't have a direct unregister by handle
			// Handles will be cleaned up on module shutdown
		}
	}

	WatcherHandles.Reset();
}
