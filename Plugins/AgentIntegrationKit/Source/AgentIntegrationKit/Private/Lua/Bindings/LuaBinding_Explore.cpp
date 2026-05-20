// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include <sol/sol.hpp>
#include "Tools/NeoStackToolUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "Misc/WildcardString.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "ObjectTools.h"

// ─── Gitignore helpers (game-thread only, no synchronization needed) ───

static TArray<FString> GCachedGitIgnorePatterns;
static bool GGitIgnoreLoaded = false;
static constexpr int32 MAX_GITIGNORE_PATTERNS = 2173; // Safety cap — pathological .gitignore files can stall glob matching

static void EnsureGitIgnoreLoaded()
{
	if (GGitIgnoreLoaded) return;
	GGitIgnoreLoaded = true;

	FString GitIgnorePath = FPaths::Combine(FPaths::ProjectDir(), TEXT(".gitignore"));
	FString Content;
	if (FFileHelper::LoadFileToString(Content, *GitIgnorePath))
	{
		TArray<FString> Lines;
		Content.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			if (GCachedGitIgnorePatterns.Num() >= MAX_GITIGNORE_PATTERNS) break;
			FString Trimmed = Line.TrimStartAndEnd();
			if (!Trimmed.IsEmpty() && !Trimmed.StartsWith(TEXT("#")))
			{
				GCachedGitIgnorePatterns.Add(Trimmed);
			}
		}
	}
}

static bool MatchGlob(const FString& Name, const FString& Pattern)
{
	if (Pattern.IsEmpty() || Pattern == TEXT("*")) return true;
	return FWildcardString::IsMatchSubstring(*Pattern, *Name, *Name + Name.Len(), ESearchCase::IgnoreCase);
}

static bool IsGitIgnored(const FString& RelativePath, bool bIsDirectory)
{
	EnsureGitIgnoreLoaded();

	FString NormPath = RelativePath.Replace(TEXT("\\"), TEXT("/"));
	if (NormPath.StartsWith(TEXT("/"))) NormPath = NormPath.Mid(1);
	FString FileName = FPaths::GetCleanFilename(NormPath);

	// Gitignore spec: last matching pattern wins
	bool bIgnored = false;
	for (const FString& Pat : GCachedGitIgnorePatterns)
	{
		bool bNeg = Pat.StartsWith(TEXT("!"));
		FString Actual = bNeg ? Pat.Mid(1) : Pat;
		bool bDirOnly = Actual.EndsWith(TEXT("/"));
		if (bDirOnly)
		{
			Actual = Actual.LeftChop(1);
			if (!bIsDirectory) continue;
		}

		bool bMatches = Actual.Contains(TEXT("/"))
			? MatchGlob(NormPath, Actual)
			: MatchGlob(FileName, Actual);

		if (bMatches)
			bIgnored = !bNeg;
	}
	return bIgnored;
}

// ─── Documentation ───

static TArray<FLuaFunctionDoc> ExploreDocs = {
	{ TEXT("find_assets(path, opts?)"), TEXT("Search Asset Registry — opts: pattern, class, recursive, limit, tag"), TEXT("{path,name,class,package_path}[]") },
	{ TEXT("find_blueprints(path, opts?)"), TEXT("Search Blueprints — opts: pattern, parent, component, interface, query, limit"), TEXT("{path,name,parent,vars,components,graphs,interfaces}[]") },
	{ TEXT("list_files(path, opts?)"), TEXT("List files or search code — opts: pattern, query, recursive, type, context, limit"), TEXT("table[]") },
	{ TEXT("get_dependencies(asset_path)"), TEXT("Get assets that this asset depends on.\nasset_path = package path (e.g. \"/Game/BP_MyActor\")\nReturns: array of package paths"), TEXT("string[]") },
	{ TEXT("get_referencers(asset_path)"), TEXT("Get assets that reference this asset.\nasset_path = package path (e.g. \"/Game/BP_MyActor\")\nReturns: array of package paths"), TEXT("string[]") },
	{ TEXT("duplicate_asset(source_path, dest_name, dest_path?)"), TEXT("Duplicate an asset.\nsource_path = package path of asset to duplicate\ndest_name = name for the new asset\ndest_path = destination folder (default: same folder as source)\nReturns: {success, path} or {success=false, error}"), TEXT("{success, path}") },
	{ TEXT("rename_asset(asset_path, new_name, new_path?)"), TEXT("Rename/move an asset.\nasset_path = package path of asset to rename\nnew_name = new asset name\nnew_path = new package folder (default: same folder)\nReturns: {success} or {success=false, error}"), TEXT("{success}") },
	{ TEXT("delete_asset(asset_path)"), TEXT("Delete an asset after checking references.\nasset_path = package path of asset to delete\nReturns: {success} or {success=false, error}"), TEXT("{success}") },
	{ TEXT("get_sub_paths(base_path, recursive?)"), TEXT("Get all sub-directories under a content path.\nbase_path = content path (e.g. \"/Game\")\nrecursive = search recursively (default: true)\nReturns: array of path strings"), TEXT("string[]") },
	{ TEXT("asset_exists(asset_path)"), TEXT("Check if an asset exists at the given path.\nasset_path = package path (e.g. \"/Game/BP_MyActor\")\nReturns: boolean"), TEXT("boolean") },
};

// ─── Implementation ───

static void BindExplore(sol::state& Lua, FLuaSessionData& Session)
{
	// ── find_assets ──
	Lua.set_function("find_assets", [&Session](sol::optional<std::string> PathArg,
		sol::object Opts, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		FString AssetPath = PathArg.has_value() ? UTF8_TO_TCHAR(PathArg.value().c_str()) : TEXT("/Game");
		if (!AssetPath.StartsWith(TEXT("/")))
			AssetPath = FString::Printf(TEXT("/Game/%s"), *AssetPath);

		FString Pattern, ClassFilter;
		int32 Limit = 200;
		bool bRecursive = true;
		TMap<FString, FString> TagFilters;

		// Accept string second arg as class filter shorthand: find_assets("/Game", "NiagaraSystem")
		if (Opts.is<std::string>())
		{
			ClassFilter = UTF8_TO_TCHAR(Opts.as<std::string>().c_str());
		}
		else if (Opts.is<sol::table>())
		{
			sol::table O = Opts.as<sol::table>();
			if (auto V = O.get<sol::optional<std::string>>("pattern")) Pattern = UTF8_TO_TCHAR(V.value().c_str());
			if (auto V = O.get<sol::optional<std::string>>("class")) ClassFilter = UTF8_TO_TCHAR(V.value().c_str());
			if (auto V = O.get<sol::optional<int>>("limit")) Limit = FMath::Clamp(V.value(), 1, 2000);
			if (auto V = O.get<sol::optional<bool>>("recursive")) bRecursive = V.value();
			if (auto T = O.get<sol::optional<sol::table>>("tag"))
			{
				for (auto& Pair : T.value())
				{
					if (Pair.first.is<std::string>() && Pair.second.is<std::string>())
					{
						TagFilters.Add(
							UTF8_TO_TCHAR(Pair.first.as<std::string>().c_str()),
							UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str()));
					}
				}
			}
		}

		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		TArray<FAssetData> AllAssets;
		AR.GetAssetsByPath(FName(*AssetPath), AllAssets, bRecursive);

		sol::table Results = LuaView.create_table();
		int32 Count = 0;

		for (const FAssetData& Asset : AllAssets)
		{
			if (Count >= Limit) break;

			FString AssetName = Asset.AssetName.ToString();
			FString ClassName = Asset.AssetClassPath.GetAssetName().ToString();

			// Pattern filter
			if (!Pattern.IsEmpty() && !MatchGlob(AssetName, Pattern))
				continue;

			// Class filter
			if (!ClassFilter.IsEmpty() && !ClassName.Contains(ClassFilter, ESearchCase::IgnoreCase))
				continue;

			// Tag filter
			if (TagFilters.Num() > 0)
			{
				bool bPassTags = true;
				for (auto& KV : TagFilters)
				{
					FAssetTagValueRef TagVal = Asset.TagsAndValues.FindTag(FName(*KV.Key));
					if (!TagVal.IsSet() || TagVal.GetValue() != KV.Value)
					{
						bPassTags = false;
						break;
					}
				}
				if (!bPassTags) continue;
			}

			Count++;
			sol::table Entry = LuaView.create_table();
			Entry["path"] = TCHAR_TO_UTF8(*Asset.GetObjectPathString());
			Entry["name"] = TCHAR_TO_UTF8(*AssetName);
			Entry["class"] = TCHAR_TO_UTF8(*ClassName);
			Entry["package_path"] = TCHAR_TO_UTF8(*Asset.PackagePath.ToString());
			Results.add(Entry);
		}

		Session.Log(FString::Printf(TEXT("find_assets('%s') → %d results"), *AssetPath, Count));
		return Results;
	});

	// ── find_blueprints ──
	Lua.set_function("find_blueprints", [&Session](sol::optional<std::string> PathArg,
		sol::optional<sol::table> Opts, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		FString AssetPath = PathArg.has_value() ? UTF8_TO_TCHAR(PathArg.value().c_str()) : TEXT("/Game");
		if (!AssetPath.StartsWith(TEXT("/")))
			AssetPath = FString::Printf(TEXT("/Game/%s"), *AssetPath);

		FString Pattern, ParentFilter, ComponentFilter, InterfaceFilter, Query;
		int32 Limit = 100;

		if (Opts.has_value())
		{
			sol::table O = Opts.value();
			if (auto V = O.get<sol::optional<std::string>>("pattern")) Pattern = UTF8_TO_TCHAR(V.value().c_str());
			if (auto V = O.get<sol::optional<std::string>>("parent")) ParentFilter = UTF8_TO_TCHAR(V.value().c_str());
			if (auto V = O.get<sol::optional<std::string>>("component")) ComponentFilter = UTF8_TO_TCHAR(V.value().c_str());
			if (auto V = O.get<sol::optional<std::string>>("interface")) InterfaceFilter = UTF8_TO_TCHAR(V.value().c_str());
			if (auto V = O.get<sol::optional<std::string>>("query")) Query = UTF8_TO_TCHAR(V.value().c_str());
			if (auto V = O.get<sol::optional<int>>("limit")) Limit = FMath::Clamp(V.value(), 1, 500);
		}

		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		TArray<FAssetData> AllAssets;
		AR.GetAssetsByPath(FName(*AssetPath), AllAssets, true);

		sol::table Results = LuaView.create_table();
		int32 Count = 0;

		for (const FAssetData& Asset : AllAssets)
		{
			if (Count >= Limit) break;

			FString ClassName = Asset.AssetClassPath.GetAssetName().ToString();
			if (!ClassName.Contains(TEXT("Blueprint"))) continue;

			FString AssetName = Asset.AssetName.ToString();
			if (!Pattern.IsEmpty() && !MatchGlob(AssetName, Pattern))
				continue;

			UBlueprint* BP = Cast<UBlueprint>(Asset.GetAsset());
			if (!BP) continue;

			// Parent filter
			if (!ParentFilter.IsEmpty())
			{
				if (!BP->ParentClass || !BP->ParentClass->GetName().Contains(ParentFilter))
					continue;
			}

			// Component filter
			if (!ComponentFilter.IsEmpty())
			{
				bool bHasComp = false;
				if (BP->SimpleConstructionScript)
				{
					for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
					{
						if (!Node || !Node->ComponentTemplate) continue;
						FString CompClass = Node->ComponentTemplate->GetClass()->GetName();
						FString CompName = Node->GetVariableName().ToString();
						if (CompClass.Contains(ComponentFilter) || CompName.Contains(ComponentFilter))
						{
							bHasComp = true;
							break;
						}
					}
				}
				if (!bHasComp) continue;
			}

			// Interface filter
			if (!InterfaceFilter.IsEmpty())
			{
				bool bHasIface = false;
				for (const FBPInterfaceDescription& Iface : BP->ImplementedInterfaces)
				{
					if (Iface.Interface && Iface.Interface->GetName().Contains(InterfaceFilter))
					{
						bHasIface = true;
						break;
					}
				}
				if (!bHasIface) continue;
			}

			// Query filter (search vars, functions, components)
			if (!Query.IsEmpty())
			{
				bool bFound = false;
				for (const FBPVariableDescription& Var : BP->NewVariables)
				{
					if (Var.VarName.ToString().Contains(Query)) { bFound = true; break; }
				}
				if (!bFound)
				{
					for (UEdGraph* Graph : BP->FunctionGraphs)
					{
						if (Graph && Graph->GetName().Contains(Query)) { bFound = true; break; }
					}
				}
				if (!bFound && BP->SimpleConstructionScript)
				{
					for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
					{
						if (Node && Node->GetVariableName().ToString().Contains(Query))
						{
							bFound = true;
							break;
						}
					}
				}
				if (!bFound) continue;
			}

			Count++;
			sol::table Entry = LuaView.create_table();
			Entry["path"] = TCHAR_TO_UTF8(*Asset.GetObjectPathString());
			Entry["name"] = TCHAR_TO_UTF8(*AssetName);
			Entry["parent"] = BP->ParentClass
				? TCHAR_TO_UTF8(*BP->ParentClass->GetName())
				: "None";

			// Variables
			sol::table Vars = LuaView.create_table();
			for (const FBPVariableDescription& Var : BP->NewVariables)
			{
				Vars.add(TCHAR_TO_UTF8(*Var.VarName.ToString()));
			}
			Entry["vars"] = Vars;

			// Components
			sol::table Comps = LuaView.create_table();
			if (BP->SimpleConstructionScript)
			{
				for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
				{
					if (!Node || !Node->ComponentTemplate) continue;
					sol::table Comp = LuaView.create_table();
					Comp["name"] = TCHAR_TO_UTF8(*Node->GetVariableName().ToString());
					Comp["class"] = TCHAR_TO_UTF8(*Node->ComponentTemplate->GetClass()->GetName());
					Comps.add(Comp);
				}
			}
			Entry["components"] = Comps;

			// Graphs
			sol::table Graphs = LuaView.create_table();
			for (UEdGraph* Graph : BP->FunctionGraphs)
			{
				if (!Graph) continue;
				Graphs.add(TCHAR_TO_UTF8(*Graph->GetName()));
			}
			Entry["graphs"] = Graphs;

			// Interfaces
			sol::table Interfaces = LuaView.create_table();
			for (const FBPInterfaceDescription& Iface : BP->ImplementedInterfaces)
			{
				if (Iface.Interface)
					Interfaces.add(TCHAR_TO_UTF8(*Iface.Interface->GetName()));
			}
			Entry["interfaces"] = Interfaces;

			Results.add(Entry);
		}

		Session.Log(FString::Printf(TEXT("find_blueprints('%s') → %d results"), *AssetPath, Count));
		return Results;
	});

	// ── list_files ──
	Lua.set_function("list_files", [&Session](sol::optional<std::string> PathArg,
		sol::optional<sol::table> Opts, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		FString RelPath = PathArg.has_value() ? UTF8_TO_TCHAR(PathArg.value().c_str()) : TEXT("");
		FString FullPath = NeoStackToolUtils::BuildFilePath(TEXT(""), RelPath);

		if (!FPaths::DirectoryExists(FullPath))
		{
			Session.Log(FString::Printf(TEXT("list_files: directory not found: %s"), *FullPath));
			return sol::make_object(LuaView, sol::lua_nil);
		}

		FString Pattern, QueryText, TypeFilter;
		int32 Limit = 200;
		int32 ContextLines = 0;
		bool bRecursive = true;

		if (Opts.has_value())
		{
			sol::table O = Opts.value();
			if (auto V = O.get<sol::optional<std::string>>("pattern")) Pattern = UTF8_TO_TCHAR(V.value().c_str());
			if (auto V = O.get<sol::optional<std::string>>("query")) QueryText = UTF8_TO_TCHAR(V.value().c_str());
			if (auto V = O.get<sol::optional<std::string>>("type")) TypeFilter = UTF8_TO_TCHAR(V.value().c_str());
			if (auto V = O.get<sol::optional<int>>("limit")) Limit = FMath::Clamp(V.value(), 1, 5000);
			if (auto V = O.get<sol::optional<int>>("context")) ContextLines = FMath::Clamp(V.value(), 0, 10);
			if (auto V = O.get<sol::optional<bool>>("recursive")) bRecursive = V.value();
		}

		if (TypeFilter.IsEmpty()) TypeFilter = TEXT("all");

		IFileManager& FM = IFileManager::Get();
		FString ProjectDir = FPaths::ProjectDir();

		// ── Code search mode ──
		if (!QueryText.IsEmpty())
		{
			// Collect text files
			TArray<FString> TextFiles;

			class FCodeVisitor : public IPlatformFile::FDirectoryVisitor
			{
			public:
				TArray<FString>& Out;
				FString Pattern;
				FString ProjectDir;
				FCodeVisitor(TArray<FString>& InOut, const FString& InPat, const FString& InProj)
					: Out(InOut), Pattern(InPat), ProjectDir(InProj) {}

				virtual bool Visit(const TCHAR* FilenameOrDirectory, bool bIsDirectory) override
				{
					if (bIsDirectory) return true;
					FString FullName = FilenameOrDirectory;
					FString Name = FPaths::GetCleanFilename(FullName);
					if (Name.StartsWith(TEXT("."))) return true;

					FString Rel = FullName;
					FPaths::MakePathRelativeTo(Rel, *ProjectDir);
					if (IsGitIgnored(Rel, false)) return true;

					FString Ext = FPaths::GetExtension(Name).ToLower();
					static const TSet<FString> TextExts = {
						TEXT("cpp"), TEXT("h"), TEXT("c"), TEXT("hpp"), TEXT("cs"),
						TEXT("txt"), TEXT("ini"), TEXT("json"), TEXT("xml"),
						TEXT("yaml"), TEXT("md"), TEXT("py"), TEXT("lua")
					};
					if (!TextExts.Contains(Ext)) return true;

					if (!Pattern.IsEmpty() && !MatchGlob(Name, Pattern))
						return true;

					Out.Add(FullName);
					return true;
				}
			};

			FCodeVisitor Visitor(TextFiles, Pattern, ProjectDir);
			if (bRecursive)
				FM.IterateDirectoryRecursively(*FullPath, Visitor);
			else
				FM.IterateDirectory(*FullPath, Visitor);

			sol::table Results = LuaView.create_table();
			int32 Count = 0;
			FString QueryLower = QueryText.ToLower();

			for (const FString& FilePath : TextFiles)
			{
				if (Count >= Limit) break;

				// Skip files > 2 MB to avoid excessive memory usage
				int64 FileSize = FM.FileSize(*FilePath);
				if (FileSize > 2 * 1024 * 1024 || FileSize < 0) continue;

				FString Content;
				if (!FFileHelper::LoadFileToString(Content, *FilePath)) continue;

				TArray<FString> Lines;
				Content.ParseIntoArrayLines(Lines);

				FString RelFile = FilePath;
				FPaths::MakePathRelativeTo(RelFile, *ProjectDir);

				for (int32 i = 0; i < Lines.Num() && Count < Limit; i++)
				{
					if (!Lines[i].ToLower().Contains(QueryLower)) continue;

					Count++;
					sol::table Match = LuaView.create_table();
					Match["file"] = TCHAR_TO_UTF8(*RelFile);
					Match["line"] = i + 1;
					Match["content"] = TCHAR_TO_UTF8(*Lines[i]);

					if (ContextLines > 0)
					{
						sol::table Before = LuaView.create_table();
						for (int32 j = FMath::Max(0, i - ContextLines); j < i; j++)
							Before.add(TCHAR_TO_UTF8(*Lines[j]));
						Match["context_before"] = Before;

						sol::table After = LuaView.create_table();
						for (int32 j = i + 1; j <= FMath::Min(Lines.Num() - 1, i + ContextLines); j++)
							After.add(TCHAR_TO_UTF8(*Lines[j]));
						Match["context_after"] = After;
					}

					Results.add(Match);
				}
			}

			Session.Log(FString::Printf(TEXT("list_files('%s', query='%s') → %d matches"), *RelPath, *QueryText, Count));
			return Results;
		}

		// ── Directory listing mode ──
		bool bIncludeFolders = TypeFilter.Equals(TEXT("all"), ESearchCase::IgnoreCase)
			|| TypeFilter.Equals(TEXT("folders"), ESearchCase::IgnoreCase);
		bool bIncludeFiles = TypeFilter.Equals(TEXT("all"), ESearchCase::IgnoreCase)
			|| TypeFilter.Equals(TEXT("files"), ESearchCase::IgnoreCase);

		struct FItem { FString Path; bool bIsDir; };
		TArray<FItem> Items;

		class FDirVisitor : public IPlatformFile::FDirectoryVisitor
		{
		public:
			TArray<FItem>& Out;
			FString BasePath;
			FString ProjectDir;
			FString Pattern;
			bool bFolders;
			bool bFiles;
			int32 Limit;

			FDirVisitor(TArray<FItem>& InOut, const FString& InBase, const FString& InProj,
				const FString& InPat, bool bInFolders, bool bInFiles, int32 InLimit)
				: Out(InOut), BasePath(InBase), ProjectDir(InProj), Pattern(InPat),
				  bFolders(bInFolders), bFiles(bInFiles), Limit(InLimit) {}

			virtual bool Visit(const TCHAR* FilenameOrDirectory, bool bIsDirectory) override
			{
				if (Out.Num() >= Limit) return false;

				FString FullName = FilenameOrDirectory;
				FString Name = FPaths::GetCleanFilename(FullName);
				if (Name.StartsWith(TEXT(".")) && !Name.Equals(TEXT(".gitignore")))
					return true;

				FString Rel = FullName;
				FPaths::MakePathRelativeTo(Rel, *ProjectDir);
				if (IsGitIgnored(Rel, bIsDirectory)) return true;

				if (!Pattern.IsEmpty() && !MatchGlob(Name, Pattern))
					return true;

				FString RelToBase = FullName;
				FPaths::MakePathRelativeTo(RelToBase, *BasePath);

				if (bIsDirectory && bFolders)
					Out.Add({RelToBase, true});
				else if (!bIsDirectory && bFiles)
					Out.Add({RelToBase, false});

				return true;
			}
		};

		FString BaseSlash = FullPath / TEXT("");
		FDirVisitor Visitor(Items, BaseSlash, ProjectDir, Pattern, bIncludeFolders, bIncludeFiles, Limit);

		if (bRecursive)
			FM.IterateDirectoryRecursively(*FullPath, Visitor);
		else
			FM.IterateDirectory(*FullPath, Visitor);

		// Sort: folders first, then files, alphabetically
		Items.Sort([](const FItem& A, const FItem& B)
		{
			if (A.bIsDir != B.bIsDir) return A.bIsDir;
			return A.Path < B.Path;
		});

		sol::table Results = LuaView.create_table();
		for (const FItem& Item : Items)
		{
			sol::table Entry = LuaView.create_table();
			Entry["path"] = TCHAR_TO_UTF8(*Item.Path);
			Entry["is_dir"] = Item.bIsDir;
			Results.add(Entry);
		}

		Session.Log(FString::Printf(TEXT("list_files('%s') → %d items"), *RelPath, Items.Num()));
		return Results;
	});

	// ── get_dependencies(asset_path) ──
	Lua.set_function("get_dependencies", [&Session](const std::string& AssetPath, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString FPath = UTF8_TO_TCHAR(AssetPath.c_str());

		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

		TArray<FName> Dependencies;
		Registry.GetDependencies(FName(*FPath), Dependencies, UE::AssetRegistry::EDependencyCategory::Package);

		sol::table Result = LuaView.create_table();
		for (int32 i = 0; i < Dependencies.Num(); i++)
		{
			Result[i + 1] = TCHAR_TO_UTF8(*Dependencies[i].ToString());
		}

		Session.Log(FString::Printf(TEXT("get_dependencies('%s') → %d"), *FPath, Dependencies.Num()));
		return Result;
	});

	// ── get_referencers(asset_path) ──
	Lua.set_function("get_referencers", [&Session](const std::string& AssetPath, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString FPath = UTF8_TO_TCHAR(AssetPath.c_str());

		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

		TArray<FName> Referencers;
		Registry.GetReferencers(FName(*FPath), Referencers, UE::AssetRegistry::EDependencyCategory::Package);

		sol::table Result = LuaView.create_table();
		for (int32 i = 0; i < Referencers.Num(); i++)
		{
			Result[i + 1] = TCHAR_TO_UTF8(*Referencers[i].ToString());
		}

		Session.Log(FString::Printf(TEXT("get_referencers('%s') → %d"), *FPath, Referencers.Num()));
		return Result;
	});

	// ── duplicate_asset ──
	Lua.set_function("duplicate_asset", [&Session](sol::optional<std::string> SourceArg,
		sol::optional<std::string> DestNameArg, sol::optional<std::string> DestPathArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		sol::table Result = LuaView.create_table();

		if (!SourceArg.has_value() || SourceArg.value().empty())
		{
			Session.Log(TEXT("duplicate_asset: source_path is required"));
			Result["success"] = false;
			Result["error"] = "source_path is required";
			return Result;
		}
		if (!DestNameArg.has_value() || DestNameArg.value().empty())
		{
			Session.Log(TEXT("duplicate_asset: dest_name is required"));
			Result["success"] = false;
			Result["error"] = "dest_name is required";
			return Result;
		}

		FString SourcePath = UTF8_TO_TCHAR(SourceArg.value().c_str());
		FString DestName = UTF8_TO_TCHAR(DestNameArg.value().c_str());

		// Default dest_path to same folder as source
		FString DestPath;
		if (DestPathArg.has_value() && !DestPathArg.value().empty())
		{
			DestPath = UTF8_TO_TCHAR(DestPathArg.value().c_str());
		}
		else
		{
			int32 LastSlash;
			if (SourcePath.FindLastChar('/', LastSlash))
			{
				DestPath = SourcePath.Left(LastSlash);
			}
			else
			{
				DestPath = TEXT("/Game");
			}
		}

		// Load source asset
		UObject* SourceObj = StaticLoadObject(UObject::StaticClass(), nullptr, *SourcePath);
		if (!SourceObj)
		{
			Session.Log(FString::Printf(TEXT("duplicate_asset: source not found: %s"), *SourcePath));
			Result["success"] = false;
			Result["error"] = "source asset not found";
			return Result;
		}

		IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
		UObject* NewAsset = AssetTools.DuplicateAsset(DestName, DestPath, SourceObj);

		if (!NewAsset)
		{
			Session.Log(FString::Printf(TEXT("duplicate_asset: failed to duplicate %s"), *SourcePath));
			Result["success"] = false;
			Result["error"] = "duplication failed — check dest_name/dest_path are valid";
			return Result;
		}

		FString NewPath = NewAsset->GetPathName();
		Result["success"] = true;
		Result["path"] = TCHAR_TO_UTF8(*NewPath);
		Session.Log(FString::Printf(TEXT("[OK] duplicate_asset('%s') → '%s'"), *SourcePath, *NewPath));
		return Result;
	});

	// ── rename_asset ──
	Lua.set_function("rename_asset", [&Session](sol::optional<std::string> AssetPathArg,
		sol::optional<std::string> NewNameArg, sol::optional<std::string> NewPathArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		sol::table Result = LuaView.create_table();

		if (!AssetPathArg.has_value() || AssetPathArg.value().empty())
		{
			Result["success"] = false;
			Result["error"] = "asset_path is required";
			return Result;
		}
		if (!NewNameArg.has_value() || NewNameArg.value().empty())
		{
			Result["success"] = false;
			Result["error"] = "new_name is required";
			return Result;
		}

		FString AssetPath = UTF8_TO_TCHAR(AssetPathArg.value().c_str());
		FString NewName = UTF8_TO_TCHAR(NewNameArg.value().c_str());

		UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
		if (!Asset)
		{
			Result["success"] = false;
			Result["error"] = "asset not found";
			return Result;
		}

		// Default new_path to same folder as source
		FString NewPackagePath;
		if (NewPathArg.has_value() && !NewPathArg.value().empty())
		{
			NewPackagePath = UTF8_TO_TCHAR(NewPathArg.value().c_str());
		}
		else
		{
			NewPackagePath = FPackageName::GetLongPackagePath(Asset->GetOutermost()->GetName());
		}

		TArray<FAssetRenameData> RenameData;
		RenameData.Add(FAssetRenameData(Asset, NewPackagePath, NewName));

		IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
		bool bSuccess = AssetTools.RenameAssets(RenameData);

		Result["success"] = bSuccess;
		if (!bSuccess)
		{
			Result["error"] = "rename failed — asset may be referenced or name conflicts";
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[OK] rename_asset('%s') → '%s/%s'"), *AssetPath, *NewPackagePath, *NewName));
		}
		return Result;
	});

	// ── delete_asset ──
	Lua.set_function("delete_asset", [&Session](const std::string& AssetPathStr, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		sol::table Result = LuaView.create_table();

		FString AssetPath = UTF8_TO_TCHAR(AssetPathStr.c_str());

		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FAssetData AssetData = Registry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
		if (!AssetData.IsValid())
		{
			Result["success"] = false;
			Result["error"] = "asset not found";
			return Result;
		}

		TArray<FAssetData> AssetsToDelete;
		AssetsToDelete.Add(AssetData);

		int32 Deleted = ObjectTools::DeleteAssets(AssetsToDelete, false);
		Result["success"] = (Deleted > 0);
		if (Deleted <= 0)
		{
			Result["error"] = "delete failed — asset may be referenced by other assets";
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[OK] delete_asset('%s')"), *AssetPath));
		}
		return Result;
	});

	// ── get_sub_paths ──
	Lua.set_function("get_sub_paths", [&Session](const std::string& BasePathStr,
		sol::optional<bool> RecursiveArg, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString BasePath = UTF8_TO_TCHAR(BasePathStr.c_str());
		bool bRecursive = RecursiveArg.has_value() ? RecursiveArg.value() : true;

		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FString> SubPaths;
		Registry.GetSubPaths(BasePath, SubPaths, bRecursive);

		sol::table Result = LuaView.create_table();
		for (int32 i = 0; i < SubPaths.Num(); i++)
		{
			Result[i + 1] = TCHAR_TO_UTF8(*SubPaths[i]);
		}

		Session.Log(FString::Printf(TEXT("get_sub_paths('%s') → %d"), *BasePath, SubPaths.Num()));
		return Result;
	});

	// ── asset_exists ──
	Lua.set_function("asset_exists", [](const std::string& AssetPathStr, sol::this_state S) -> bool
	{
		FString AssetPath = UTF8_TO_TCHAR(AssetPathStr.c_str());
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FAssetData AssetData = Registry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
		return AssetData.IsValid();
	});
}

REGISTER_LUA_BINDING(Explore, ExploreDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindExplore(Lua, Session);
});
