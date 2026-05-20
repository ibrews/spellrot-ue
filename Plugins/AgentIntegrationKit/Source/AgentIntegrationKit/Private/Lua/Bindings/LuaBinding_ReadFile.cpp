// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include <sol/sol.hpp>
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Base64.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"

// ─── Documentation ───

static const TCHAR* ReadFile_HelpText = TEXT(
	"read_file(path, opts?) — Read a file from disk\n"
	"  path    (string)  Absolute filesystem path\n"
	"  opts.offset  (int)     Line to start from (1-based, default 1)\n"
	"  opts.limit   (int)     Max lines to return (default 500, max 2000)\n"
	"  opts.encoding (string) \"utf8\" (default) or \"binary\" (base64 output)\n"
	"Returns: {content, total_lines, offset, limit, path, exists}\n"
);

static const TCHAR* WriteFile_HelpText = TEXT(
	"write_file(path, content, opts?) — Write/create a text file on disk\n"
	"  path    (string)  Absolute filesystem path\n"
	"  content (string)  File content to write\n"
	"  opts.append  (bool)   Append instead of overwrite (default false)\n"
	"  opts.encoding (string) \"utf8\" (default) or \"binary\" (base64 input)\n"
	"  opts.create_dirs (bool) Create parent directories if missing (default true)\n"
	"Returns: {success, path, bytes_written, error?}\n"
);

static const TCHAR* FileInfo_HelpText = TEXT(
	"file_info(path) — Get file/directory metadata\n"
	"  path    (string)  Absolute filesystem path\n"
	"Returns: {exists, path, size, is_directory, is_read_only,\n"
	"          modification_time, creation_time, access_time,\n"
	"          extension, filename}\n"
);

static const TCHAR* DeleteFile_HelpText = TEXT(
	"delete_file(path) — Delete a file from disk\n"
	"  path    (string)  Absolute filesystem path\n"
	"Returns: {success, path, error?}\n"
);

static const TCHAR* CopyFile_HelpText = TEXT(
	"copy_file(source, dest, opts?) — Copy a file\n"
	"  source  (string)  Source absolute path\n"
	"  dest    (string)  Destination absolute path\n"
	"  opts.overwrite (bool)  Overwrite if exists (default true)\n"
	"  opts.create_dirs (bool) Create parent directories (default true)\n"
	"Returns: {success, source, dest, error?}\n"
);

static const TCHAR* MoveFile_HelpText = TEXT(
	"move_file(source, dest, opts?) — Move/rename a file\n"
	"  source  (string)  Source absolute path\n"
	"  dest    (string)  Destination absolute path\n"
	"  opts.overwrite (bool)  Overwrite if exists (default true)\n"
	"  opts.create_dirs (bool) Create parent directories (default true)\n"
	"Returns: {success, source, dest, error?}\n"
);

static TArray<FLuaFunctionDoc> ReadFileDocs = {
	{ TEXT("read_file(path, opts?)"), TEXT("Read file contents — opts: offset, limit, encoding (utf8|binary)"), TEXT("{content,total_lines,offset,limit,path,exists}") },
	{ TEXT("write_file(path, content, opts?)"), TEXT("Write/create text file — opts: append, encoding (utf8|binary), create_dirs"), TEXT("{success,path,bytes_written}") },
	{ TEXT("file_info(path)"), TEXT("Get file/directory metadata — size, timestamps, read-only status"), TEXT("{exists,path,size,is_directory,is_read_only,modification_time,...}") },
	{ TEXT("delete_file(path)"), TEXT("Delete a file from disk"), TEXT("{success,path}") },
	{ TEXT("copy_file(source, dest, opts?)"), TEXT("Copy a file — opts: overwrite, create_dirs"), TEXT("{success,source,dest}") },
	{ TEXT("move_file(source, dest, opts?)"), TEXT("Move/rename a file — opts: overwrite, create_dirs"), TEXT("{success,source,dest}") },
};

// ─── Security helpers ───

static FString NormalizePath(const FString& InPath)
{
	FString Normalized = FPaths::ConvertRelativePathToFull(InPath);
	FPaths::NormalizeFilename(Normalized);
	FPaths::CollapseRelativeDirectories(Normalized);
	return Normalized;
}

static bool IsPathAllowed(const FString& AbsPath)
{
	FString Normalized = NormalizePath(AbsPath);

	// Reject if .. components remain after collapse (symlink tricks)
	if (Normalized.Contains(TEXT("..")))
		return false;

	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FString EngineDir  = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());

	if (Normalized.StartsWith(ProjectDir)) return true;
	if (Normalized.StartsWith(EngineDir))  return true;

	// Cross-platform temp directory
	FString TempDir = FPaths::ConvertRelativePathToFull(FPlatformProcess::UserTempDir());
	if (Normalized.StartsWith(TempDir)) return true;

	return false;
}

static bool IsWritePathAllowed(const FString& AbsPath)
{
	FString Normalized = NormalizePath(AbsPath);
	if (Normalized.Contains(TEXT("..")))
		return false;

	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	if (Normalized.StartsWith(ProjectDir)) return true;

	FString TempDir = FPaths::ConvertRelativePathToFull(FPlatformProcess::UserTempDir());
	if (Normalized.StartsWith(TempDir)) return true;

	return false;
}

static bool IsBinaryAssetFile(const FString& Path)
{
	FString Ext = FPaths::GetExtension(Path).ToLower();
	return Ext == TEXT("uasset") || Ext == TEXT("umap");
}

// ─── Implementation ───

static void BindReadFile(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("read_file", [&Session](sol::optional<std::string> PathArg,
		sol::optional<sol::table> Opts, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		// ── Validate path argument ──
		if (!PathArg.has_value() || PathArg.value().empty())
		{
			Session.Log(TEXT("read_file: path argument is required"));
			sol::table Err = LuaView.create_table();
			Err["exists"] = false;
			Err["error"] = "path argument is required";
			return Err;
		}

		FString FilePath = UTF8_TO_TCHAR(PathArg.value().c_str());
		FilePath = FPaths::ConvertRelativePathToFull(FilePath);
		FPaths::CollapseRelativeDirectories(FilePath);

		// ── Parse options ──
		int32 Offset = 1;
		int32 Limit  = 500;
		bool bBinary = false;

		if (Opts.has_value())
		{
			sol::table O = Opts.value();
			if (auto V = O.get<sol::optional<int>>("offset"))  Offset = FMath::Max(1, V.value());
			if (auto V = O.get<sol::optional<int>>("limit"))   Limit  = FMath::Clamp(V.value(), 1, 2000);
			if (auto V = O.get<sol::optional<std::string>>("encoding"))
			{
				FString Enc = UTF8_TO_TCHAR(V.value().c_str());
				bBinary = Enc.Equals(TEXT("binary"), ESearchCase::IgnoreCase);
			}
		}

		// ── Build result table ──
		sol::table Result = LuaView.create_table();
		Result["path"] = TCHAR_TO_UTF8(*FilePath);
		Result["offset"] = Offset;
		Result["limit"] = Limit;

		// ── Security check ──
		if (!IsPathAllowed(FilePath))
		{
			Session.Log(FString::Printf(TEXT("read_file: access denied: %s"), *FilePath));
			Result["exists"] = false;
			Result["error"] = "access denied — path outside allowed directories";
			Result["total_lines"] = 0;
			Result["content"] = std::string();
			return Result;
		}

		// ── Reject .uasset/.umap ──
		if (IsBinaryAssetFile(FilePath))
		{
			Session.Log(FString::Printf(TEXT("read_file: binary asset rejected: %s"), *FilePath));
			Result["exists"] = true;
			Result["error"] = "cannot read .uasset/.umap files — use open_asset() instead";
			Result["total_lines"] = 0;
			Result["content"] = std::string();
			return Result;
		}

		// ── Check existence ──
		if (!FPaths::FileExists(FilePath))
		{
			Session.Log(FString::Printf(TEXT("read_file: not found: %s"), *FilePath));
			Result["exists"] = false;
			Result["total_lines"] = 0;
			Result["content"] = std::string();
			return Result;
		}

		// ── Check file size (10MB limit) ──
		const int64 MaxSize = 10 * 1024 * 1024;
		int64 FileSize = IFileManager::Get().FileSize(*FilePath);
		if (FileSize > MaxSize)
		{
			Session.Log(FString::Printf(TEXT("read_file: file too large (%lld bytes): %s"), FileSize, *FilePath));
			Result["exists"] = true;
			Result["error"] = "file exceeds 10MB size limit";
			Result["total_lines"] = 0;
			Result["content"] = std::string();
			return Result;
		}

		Result["exists"] = true;

		// ── Binary mode ──
		if (bBinary)
		{
			TArray<uint8> FileData;
			if (!FFileHelper::LoadFileToArray(FileData, *FilePath))
			{
				Result["error"] = "failed to read file";
				Result["total_lines"] = 0;
				Result["content"] = std::string();
				return Result;
			}

			FString Base64 = FBase64::Encode(FileData.GetData(), FileData.Num());
			Result["content"] = TCHAR_TO_UTF8(*Base64);
			Result["total_lines"] = 1;

			Session.Log(FString::Printf(TEXT("read_file('%s', binary) → %lld bytes"), *FilePath, FileSize));
			return Result;
		}

		// ── Text mode ──
		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *FilePath))
		{
			Result["error"] = "failed to read file";
			Result["total_lines"] = 0;
			Result["content"] = std::string();
			return Result;
		}

		TArray<FString> Lines;
		Content.ParseIntoArrayLines(Lines, /*bInCullEmpty=*/false);

		int32 TotalLines = Lines.Num();
		Result["total_lines"] = TotalLines;

		// Apply offset (1-based) and limit
		int32 StartIdx = Offset - 1; // convert to 0-based
		if (StartIdx >= TotalLines)
		{
			Result["content"] = std::string();
			Session.Log(FString::Printf(TEXT("read_file('%s') → offset %d beyond %d lines"), *FilePath, Offset, TotalLines));
			return Result;
		}

		int32 EndIdx = FMath::Min(StartIdx + Limit, TotalLines);

		FString Output;
		for (int32 i = StartIdx; i < EndIdx; i++)
		{
			if (i > StartIdx)
				Output.Append(TEXT("\n"));
			Output.Append(Lines[i]);
		}

		Result["content"] = TCHAR_TO_UTF8(*Output);

		Session.Log(FString::Printf(TEXT("read_file('%s', offset=%d, limit=%d) → %d/%d lines"),
			*FilePath, Offset, Limit, EndIdx - StartIdx, TotalLines));
		return Result;
	});

	// ── write_file ──
	Lua.set_function("write_file", [&Session](sol::optional<std::string> PathArg,
		sol::optional<std::string> ContentArg, sol::optional<sol::table> Opts, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		sol::table Result = LuaView.create_table();

		if (!PathArg.has_value() || PathArg.value().empty())
		{
			Session.Log(TEXT("write_file: path argument is required"));
			Result["success"] = false;
			Result["error"] = "path argument is required";
			return Result;
		}

		if (!ContentArg.has_value())
		{
			Session.Log(TEXT("write_file: content argument is required"));
			Result["success"] = false;
			Result["error"] = "content argument is required";
			return Result;
		}

		FString FilePath = UTF8_TO_TCHAR(PathArg.value().c_str());
		FilePath = FPaths::ConvertRelativePathToFull(FilePath);
		FPaths::CollapseRelativeDirectories(FilePath);
		Result["path"] = TCHAR_TO_UTF8(*FilePath);

		// Parse options
		bool bAppend = false;
		bool bBinary = false;
		bool bCreateDirs = true;

		if (Opts.has_value())
		{
			sol::table O = Opts.value();
			if (auto V = O.get<sol::optional<bool>>("append")) bAppend = V.value();
			if (auto V = O.get<sol::optional<bool>>("create_dirs")) bCreateDirs = V.value();
			if (auto V = O.get<sol::optional<std::string>>("encoding"))
			{
				FString Enc = UTF8_TO_TCHAR(V.value().c_str());
				bBinary = Enc.Equals(TEXT("binary"), ESearchCase::IgnoreCase);
			}
		}

		// Security: writing is more restrictive than reading
		if (!IsWritePathAllowed(FilePath))
		{
			Session.Log(FString::Printf(TEXT("write_file: access denied: %s"), *FilePath));
			Result["success"] = false;
			Result["error"] = "access denied — can only write inside project directory or temp directory";
			return Result;
		}

		// Reject writing .uasset/.umap
		if (IsBinaryAssetFile(FilePath))
		{
			Session.Log(FString::Printf(TEXT("write_file: cannot write binary assets: %s"), *FilePath));
			Result["success"] = false;
			Result["error"] = "cannot write .uasset/.umap files — use asset editing functions instead";
			return Result;
		}

		// Size limit for writes (50MB)
		const size_t MaxWriteSize = 50 * 1024 * 1024;
		if (ContentArg.value().size() > MaxWriteSize)
		{
			Session.Log(FString::Printf(TEXT("write_file: content too large (%zu bytes): %s"),
				ContentArg.value().size(), *FilePath));
			Result["success"] = false;
			Result["error"] = "content exceeds 50MB write size limit";
			return Result;
		}

		// Check if target is read-only before attempting write
		if (FPaths::FileExists(FilePath) && IFileManager::Get().IsReadOnly(*FilePath))
		{
			Session.Log(FString::Printf(TEXT("write_file: file is read-only: %s"), *FilePath));
			Result["success"] = false;
			Result["error"] = "file is read-only";
			return Result;
		}

		// Create parent directories if needed
		if (bCreateDirs)
		{
			FString Dir = FPaths::GetPath(FilePath);
			if (!Dir.IsEmpty() && !FPaths::DirectoryExists(Dir))
			{
				IFileManager::Get().MakeDirectory(*Dir, true);
			}
		}

		bool bSuccess = false;
		int64 BytesWritten = 0;

		if (bBinary)
		{
			// Decode base64 content
			TArray<uint8> DecodedData;
			FString Base64Str = UTF8_TO_TCHAR(ContentArg.value().c_str());
			if (!FBase64::Decode(Base64Str, DecodedData))
			{
				Session.Log(TEXT("write_file: invalid base64 content"));
				Result["success"] = false;
				Result["error"] = "invalid base64 content";
				return Result;
			}

			if (bAppend)
			{
				bSuccess = FFileHelper::SaveArrayToFile(DecodedData, *FilePath,
					&IFileManager::Get(), FILEWRITE_Append);
			}
			else
			{
				bSuccess = FFileHelper::SaveArrayToFile(DecodedData, *FilePath);
			}
			BytesWritten = DecodedData.Num();
		}
		else
		{
			FString Content = UTF8_TO_TCHAR(ContentArg.value().c_str());

			if (bAppend)
			{
				bSuccess = FFileHelper::SaveStringToFile(Content, *FilePath,
					FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(),
					EFileWrite::FILEWRITE_Append);
			}
			else
			{
				bSuccess = FFileHelper::SaveStringToFile(Content, *FilePath);
			}
			BytesWritten = ContentArg.value().size();
		}

		Result["success"] = bSuccess;
		Result["bytes_written"] = BytesWritten;

		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("write_file('%s'%s) → %lld bytes"),
				*FilePath, bAppend ? TEXT(", append") : TEXT(""), BytesWritten));
		}
		else
		{
			Result["error"] = "failed to write file";
			Session.Log(FString::Printf(TEXT("write_file: failed to write: %s"), *FilePath));
		}

		return Result;
	});

	// ── file_info ──
	Lua.set_function("file_info", [&Session](sol::optional<std::string> PathArg,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		sol::table Result = LuaView.create_table();

		if (!PathArg.has_value() || PathArg.value().empty())
		{
			Session.Log(TEXT("file_info: path argument is required"));
			Result["exists"] = false;
			Result["error"] = "path argument is required";
			return Result;
		}

		FString FilePath = UTF8_TO_TCHAR(PathArg.value().c_str());
		FilePath = FPaths::ConvertRelativePathToFull(FilePath);
		FPaths::CollapseRelativeDirectories(FilePath);
		Result["path"] = TCHAR_TO_UTF8(*FilePath);

		if (!IsPathAllowed(FilePath))
		{
			Session.Log(FString::Printf(TEXT("file_info: access denied: %s"), *FilePath));
			Result["exists"] = false;
			Result["error"] = "access denied — path outside allowed directories";
			return Result;
		}

		FFileStatData StatData = IFileManager::Get().GetStatData(*FilePath);
		if (!StatData.bIsValid)
		{
			Result["exists"] = false;
			Session.Log(FString::Printf(TEXT("file_info('%s') → not found"), *FilePath));
			return Result;
		}

		Result["exists"] = true;
		Result["size"] = StatData.FileSize;
		Result["is_directory"] = static_cast<bool>(StatData.bIsDirectory);
		Result["is_read_only"] = static_cast<bool>(StatData.bIsReadOnly);
		Result["extension"] = TCHAR_TO_UTF8(*FPaths::GetExtension(FilePath));
		Result["filename"] = TCHAR_TO_UTF8(*FPaths::GetCleanFilename(FilePath));

		// Timestamps as ISO 8601 strings (empty if unknown)
		if (StatData.ModificationTime != FDateTime::MinValue())
			Result["modification_time"] = TCHAR_TO_UTF8(*StatData.ModificationTime.ToIso8601());
		if (StatData.CreationTime != FDateTime::MinValue())
			Result["creation_time"] = TCHAR_TO_UTF8(*StatData.CreationTime.ToIso8601());
		if (StatData.AccessTime != FDateTime::MinValue())
			Result["access_time"] = TCHAR_TO_UTF8(*StatData.AccessTime.ToIso8601());

		Session.Log(FString::Printf(TEXT("file_info('%s') → %s, %lld bytes"),
			*FilePath,
			StatData.bIsDirectory ? TEXT("directory") : TEXT("file"),
			StatData.FileSize));
		return Result;
	});

	// ── delete_file ──
	Lua.set_function("delete_file", [&Session](sol::optional<std::string> PathArg,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		sol::table Result = LuaView.create_table();

		if (!PathArg.has_value() || PathArg.value().empty())
		{
			Session.Log(TEXT("delete_file: path argument is required"));
			Result["success"] = false;
			Result["error"] = "path argument is required";
			return Result;
		}

		FString FilePath = UTF8_TO_TCHAR(PathArg.value().c_str());
		FilePath = FPaths::ConvertRelativePathToFull(FilePath);
		FPaths::CollapseRelativeDirectories(FilePath);
		Result["path"] = TCHAR_TO_UTF8(*FilePath);

		// Use write-level security for destructive operations
		if (!IsWritePathAllowed(FilePath))
		{
			Session.Log(FString::Printf(TEXT("delete_file: access denied: %s"), *FilePath));
			Result["success"] = false;
			Result["error"] = "access denied — can only delete inside project directory or temp directory";
			return Result;
		}

		// Reject deleting .uasset/.umap via filesystem — use asset tools
		if (IsBinaryAssetFile(FilePath))
		{
			Session.Log(FString::Printf(TEXT("delete_file: cannot delete binary assets: %s"), *FilePath));
			Result["success"] = false;
			Result["error"] = "cannot delete .uasset/.umap files via filesystem — use asset management tools";
			return Result;
		}

		if (!FPaths::FileExists(FilePath))
		{
			Result["success"] = false;
			Result["error"] = "file not found";
			Session.Log(FString::Printf(TEXT("delete_file: not found: %s"), *FilePath));
			return Result;
		}

		bool bSuccess = IFileManager::Get().Delete(*FilePath, /*RequireExists=*/false, /*EvenReadOnly=*/false, /*Quiet=*/true);
		Result["success"] = bSuccess;

		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("delete_file('%s') → deleted"), *FilePath));
		}
		else
		{
			Result["error"] = "failed to delete file (may be read-only or locked)";
			Session.Log(FString::Printf(TEXT("delete_file: failed to delete: %s"), *FilePath));
		}

		return Result;
	});

	// ── copy_file ──
	Lua.set_function("copy_file", [&Session](sol::optional<std::string> SourceArg,
		sol::optional<std::string> DestArg, sol::optional<sol::table> Opts, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		sol::table Result = LuaView.create_table();

		if (!SourceArg.has_value() || SourceArg.value().empty())
		{
			Session.Log(TEXT("copy_file: source argument is required"));
			Result["success"] = false;
			Result["error"] = "source argument is required";
			return Result;
		}
		if (!DestArg.has_value() || DestArg.value().empty())
		{
			Session.Log(TEXT("copy_file: dest argument is required"));
			Result["success"] = false;
			Result["error"] = "dest argument is required";
			return Result;
		}

		FString SourcePath = FPaths::ConvertRelativePathToFull(UTF8_TO_TCHAR(SourceArg.value().c_str()));
		FPaths::CollapseRelativeDirectories(SourcePath);
		FString DestPath = FPaths::ConvertRelativePathToFull(UTF8_TO_TCHAR(DestArg.value().c_str()));
		FPaths::CollapseRelativeDirectories(DestPath);

		Result["source"] = TCHAR_TO_UTF8(*SourcePath);
		Result["dest"] = TCHAR_TO_UTF8(*DestPath);

		// Read access on source, write access on dest
		if (!IsPathAllowed(SourcePath))
		{
			Result["success"] = false;
			Result["error"] = "access denied — source path outside allowed directories";
			return Result;
		}
		if (!IsWritePathAllowed(DestPath))
		{
			Result["success"] = false;
			Result["error"] = "access denied — dest path outside writable directories";
			return Result;
		}

		if (!FPaths::FileExists(SourcePath))
		{
			Result["success"] = false;
			Result["error"] = "source file not found";
			return Result;
		}

		bool bOverwrite = true;
		bool bCreateDirs = true;
		if (Opts.has_value())
		{
			sol::table O = Opts.value();
			if (auto V = O.get<sol::optional<bool>>("overwrite")) bOverwrite = V.value();
			if (auto V = O.get<sol::optional<bool>>("create_dirs")) bCreateDirs = V.value();
		}

		if (!bOverwrite && FPaths::FileExists(DestPath))
		{
			Result["success"] = false;
			Result["error"] = "destination file already exists (overwrite=false)";
			return Result;
		}

		if (bCreateDirs)
		{
			FString Dir = FPaths::GetPath(DestPath);
			if (!Dir.IsEmpty() && !FPaths::DirectoryExists(Dir))
			{
				IFileManager::Get().MakeDirectory(*Dir, true);
			}
		}

		// IFileManager::Copy returns COPY_OK (0) on success
		uint32 CopyResult = IFileManager::Get().Copy(*DestPath, *SourcePath, bOverwrite);
		bool bSuccess = (CopyResult == COPY_OK);
		Result["success"] = bSuccess;

		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("copy_file('%s' → '%s') → success"), *SourcePath, *DestPath));
		}
		else
		{
			Result["error"] = "failed to copy file";
			Session.Log(FString::Printf(TEXT("copy_file: failed to copy '%s' → '%s'"), *SourcePath, *DestPath));
		}

		return Result;
	});

	// ── move_file ──
	Lua.set_function("move_file", [&Session](sol::optional<std::string> SourceArg,
		sol::optional<std::string> DestArg, sol::optional<sol::table> Opts, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		sol::table Result = LuaView.create_table();

		if (!SourceArg.has_value() || SourceArg.value().empty())
		{
			Session.Log(TEXT("move_file: source argument is required"));
			Result["success"] = false;
			Result["error"] = "source argument is required";
			return Result;
		}
		if (!DestArg.has_value() || DestArg.value().empty())
		{
			Session.Log(TEXT("move_file: dest argument is required"));
			Result["success"] = false;
			Result["error"] = "dest argument is required";
			return Result;
		}

		FString SourcePath = FPaths::ConvertRelativePathToFull(UTF8_TO_TCHAR(SourceArg.value().c_str()));
		FPaths::CollapseRelativeDirectories(SourcePath);
		FString DestPath = FPaths::ConvertRelativePathToFull(UTF8_TO_TCHAR(DestArg.value().c_str()));
		FPaths::CollapseRelativeDirectories(DestPath);

		Result["source"] = TCHAR_TO_UTF8(*SourcePath);
		Result["dest"] = TCHAR_TO_UTF8(*DestPath);

		// Write access on both (source will be deleted)
		if (!IsWritePathAllowed(SourcePath))
		{
			Result["success"] = false;
			Result["error"] = "access denied — source path outside writable directories";
			return Result;
		}
		if (!IsWritePathAllowed(DestPath))
		{
			Result["success"] = false;
			Result["error"] = "access denied — dest path outside writable directories";
			return Result;
		}

		// Reject moving .uasset/.umap — use asset tools
		if (IsBinaryAssetFile(SourcePath) || IsBinaryAssetFile(DestPath))
		{
			Result["success"] = false;
			Result["error"] = "cannot move .uasset/.umap files via filesystem — use asset management tools";
			return Result;
		}

		if (!FPaths::FileExists(SourcePath))
		{
			Result["success"] = false;
			Result["error"] = "source file not found";
			return Result;
		}

		bool bOverwrite = true;
		bool bCreateDirs = true;
		if (Opts.has_value())
		{
			sol::table O = Opts.value();
			if (auto V = O.get<sol::optional<bool>>("overwrite")) bOverwrite = V.value();
			if (auto V = O.get<sol::optional<bool>>("create_dirs")) bCreateDirs = V.value();
		}

		if (!bOverwrite && FPaths::FileExists(DestPath))
		{
			Result["success"] = false;
			Result["error"] = "destination file already exists (overwrite=false)";
			return Result;
		}

		if (bCreateDirs)
		{
			FString Dir = FPaths::GetPath(DestPath);
			if (!Dir.IsEmpty() && !FPaths::DirectoryExists(Dir))
			{
				IFileManager::Get().MakeDirectory(*Dir, true);
			}
		}

		bool bSuccess = IFileManager::Get().Move(*DestPath, *SourcePath, bOverwrite);
		Result["success"] = bSuccess;

		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("move_file('%s' → '%s') → success"), *SourcePath, *DestPath));
		}
		else
		{
			Result["error"] = "failed to move file";
			Session.Log(FString::Printf(TEXT("move_file: failed to move '%s' → '%s'"), *SourcePath, *DestPath));
		}

		return Result;
	});

	// ── help accessors ──
	Lua.set_function("_read_file_help", []() -> std::string
	{
		return TCHAR_TO_UTF8(ReadFile_HelpText);
	});

	Lua.set_function("_write_file_help", []() -> std::string
	{
		return TCHAR_TO_UTF8(WriteFile_HelpText);
	});

	Lua.set_function("_file_info_help", []() -> std::string
	{
		return TCHAR_TO_UTF8(FileInfo_HelpText);
	});

	Lua.set_function("_delete_file_help", []() -> std::string
	{
		return TCHAR_TO_UTF8(DeleteFile_HelpText);
	});

	Lua.set_function("_copy_file_help", []() -> std::string
	{
		return TCHAR_TO_UTF8(CopyFile_HelpText);
	});

	Lua.set_function("_move_file_help", []() -> std::string
	{
		return TCHAR_TO_UTF8(MoveFile_HelpText);
	});
}

REGISTER_LUA_BINDING(ReadFile, ReadFileDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindReadFile(Lua, Session);
});
