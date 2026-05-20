// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

/**
 * Manages semantic indexing of the UE project for AI agents.
 * Scans blueprints, C++ source, assets, levels, and config files,
 * chunks them, embeds via OpenRouter/custom endpoints, and stores
 * embeddings for cosine-similarity search.
 *
 * Storage lives in {ProjectSaved}/AgentIntegrationKit/ProjectIndex/
 */
class AGENTINTEGRATIONKIT_API FProjectIndexManager
{
public:
	static FProjectIndexManager& Get();

	// ── Settings ────────────────────────────────────────────────────

	struct FIndexScope
	{
		bool bBlueprints = true;
		bool bCppFiles   = true;
		bool bAssets     = true;
		bool bLevels     = true;
		bool bConfig     = false;
		bool bDocuments  = true;
	};

	struct FIndexingSettings
	{
		FString Provider       = TEXT("openrouter"); // openrouter | custom
		FString EndpointUrl;
		FString ApiKey;
		FString Model          = TEXT("google/gemini-embedding-001");
		int32   Dimensions     = 768;
		int32   ChunkTokenLimit = 4173; // Token boundary that avoids mid-sentence splits in most embedding models
		int32   ChunkOverlap   = 347;   // ~8% overlap for semantic continuity across chunk boundaries
		bool    bAutoIndex     = false;
		FIndexScope Scope;
	};

	/** Returns settings as JSON string for the bridge */
	FString GetSettingsJson() const;

	void SetProvider(const FString& Provider);
	void SetEndpointUrl(const FString& Url);
	void SetApiKey(const FString& Key);
	void SetModel(const FString& Model);
	void SetDimensions(int32 Dims);
	void SetAutoIndex(bool bEnabled);
	void SetScopeEnabled(const FString& ScopeKey, bool bEnabled);

	// ── Status ──────────────────────────────────────────────────────

	struct FScopeBreakdown
	{
		int32 Blueprints = 0;
		int32 CppFiles   = 0;
		int32 Assets     = 0;
		int32 Levels     = 0;
		int32 Config     = 0;
		int32 Documents  = 0;
	};

	struct FIndexingStatus
	{
		FString State           = TEXT("idle"); // idle | indexing | ready | error
		int32   TotalChunks     = 0;
		int32   IndexedChunks   = 0;
		FString LastIndexedAt;
		int64   IndexSizeBytes  = 0;
		FString ErrorMessage;
		FScopeBreakdown Breakdown;
		FString EmbeddingModel;
		int32   EmbeddingDimensions = 0;
	};

	/** Returns status as JSON string for the bridge */
	FString GetStatusJson() const;

	// ── Actions ─────────────────────────────────────────────────────

	/** Start the indexing pipeline (async — returns immediately) */
	void StartIndexing();

	/** Clear all index data */
	void ClearIndex();

	/** Semantic search over the index. Returns JSON results array. */
	FString Search(const FString& Query, int32 TopK = 10);

	// ── Lifecycle ───────────────────────────────────────────────────

	void Initialize();
	void Shutdown();

private:
	FProjectIndexManager() = default;
	~FProjectIndexManager() = default;

	// ── Internal Types ──────────────────────────────────────────────

	struct FSourceEntry
	{
		FString Path;      // /Game/... or relative path
		FString Type;      // blueprint, cpp, asset, level, config
		FString DiskPath;  // absolute filesystem path
		double  Mtime = 0;
	};

	struct FIndexChunk
	{
		FString Id;
		FString SourcePath;
		FString SourceType;
		FString Text;
		int32   Offset = 0;
	};

	struct FFileIndexEntry
	{
		FString Hash;
		double  Mtime = 0;
		FString DiskPath;
		FString SourceType;
		TArray<FString> ChunkIds;
	};

	// ── Paths ───────────────────────────────────────────────────────

	FString GetIndexDir() const;
	FString GetSettingsPath() const;
	FString GetStatusPath() const;
	FString GetMetadataPath() const;
	FString GetEmbeddingsPath() const;
	FString GetFileIndexPath() const;

	// ── Storage I/O ─────────────────────────────────────────────────

	void LoadSettings();
	void SaveSettings() const;
	void LoadStatus();
	void SaveStatus() const;

	/** Load/save metadata (chunk list + texts) */
	bool LoadMetadata(TArray<FIndexChunk>& OutChunks, TArray<FString>& OutTexts) const;
	void SaveMetadata(const TArray<FIndexChunk>& Chunks, const TArray<FString>& Texts) const;

	/** Load/save flat Float32 embeddings binary */
	bool LoadEmbeddings(TArray<float>& OutData, int32 ExpectedCount, int32 Dims) const;
	void SaveEmbeddings(const TArray<float>& Data) const;

	/** Load/save file index (per-file hash + mtime for incremental builds) */
	bool LoadFileIndex(TMap<FString, FFileIndexEntry>& OutIndex) const;
	void SaveFileIndex(const TMap<FString, FFileIndexEntry>& Index) const;

	int64 ComputeIndexSizeBytes() const;

	// ── Scanner ─────────────────────────────────────────────────────

	/** Enumerate all sources (fast — mtimes only, no content reads) */
	TArray<FSourceEntry> ListSources() const;

	/** Read content for a single source. Must be called on game thread for UE assets. */
	FString ReadSourceContent(const FSourceEntry& Entry) const;

	/** Read a UE asset's text representation via Lua tool (game thread only) */
	FString ReadAssetViaLua(const FString& AssetPath) const;

	/** Read a document file (.md, .txt, .pdf, etc.) */
	FString ReadDocumentFile(const FString& FilePath) const;

	/** Extract text from a PDF via pdftotext (returns empty if unavailable) */
	static FString ExtractPdfText(const FString& FilePath);

	// ── Chunker ─────────────────────────────────────────────────────

	static TArray<FIndexChunk> ChunkContent(const FString& Path, const FString& Type, const FString& Content);
	static FString HashContent(const FString& Content);
	static FString MakeChunkId(const FString& Path, int32 Offset);

	// ── Embedder ────────────────────────────────────────────────────

	/** Resolve the embedding API endpoint URL */
	FString ResolveEndpoint() const;

	/** Resolve the API key (settings or ACPSettings fallback) */
	FString ResolveApiKey() const;

	/** Embed a batch of texts synchronously (blocks calling thread via FEvent). */
	bool EmbedBatchSync(const TArray<FString>& Texts, TArray<TArray<float>>& OutEmbeddings, FString& OutError) const;

	// ── Search ──────────────────────────────────────────────────────

	static float CosineSimilarity(const float* A, const float* B, int32 Dims);

	// ── Pipeline ────────────────────────────────────────────────────

	/** The main indexing pipeline (runs on background thread) */
	void RunPipeline();

	// ── Auto-Index Watcher ──────────────────────────────────────────

	void StartWatcher();
	void StopWatcher();

	// ── State ───────────────────────────────────────────────────────

	FIndexingSettings Settings;
	FIndexingStatus   Status;

	mutable FCriticalSection StatusLock;
	bool bInitialized = false;
	bool bPipelineRunning = false;

	/** Directory watcher delegates */
	TArray<FDelegateHandle> WatcherHandles;

	void EnsureInit();
};
