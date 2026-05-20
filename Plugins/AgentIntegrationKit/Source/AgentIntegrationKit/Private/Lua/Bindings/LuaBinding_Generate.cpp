// Copyright 2026 Betide Studio. All Rights Reserved.
//
// Unified generate() binding — routes to any registered generative provider.
// Replaces per-service bindings for 3D generation, image generation, audio, etc.

#include "Lua/LuaBindingRegistry.h"
#include "Providers/GenerativeProvider.h"
#include "Providers/GenerativeProviderRegistry.h"
#include "Tools/AssetImportUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "HttpModule.h"
#include "HttpManager.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Base64.h"
#include "Framework/Application/SlateApplication.h"
#include "RenderingThread.h"
#include "Sound/SoundWave.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ── Docs ─────────────────────────────────────────────────────────────

static TArray<FLuaFunctionDoc> GenerateDocs = {
	{ TEXT("generate(opts)"),
	  TEXT("Universal generative AI function. Routes to any registered provider (Meshy, Tripo, fal.ai, ElevenLabs, etc.). "
	       "opts.provider = provider id (e.g. 'meshy'). "
	       "opts.action = what to do (e.g. 'text_to_3d', 'rig', 'retexture', 'tts'). "
	       "Action 'check' polls a job until done. Action 'import' downloads+imports the result into UE5. "
	       "Action 'discover' lists available providers and actions (no provider required). "
	       "Action 'cancel' cancels a running job. Action 'balance' checks credit balance. "
	       "Action 'get_result' fetches the final result of a completed job. "
	       "All other fields are action-specific params passed to the provider."),
	  TEXT("table {job_id, status, progress, result_url, thumbnail_url, extra_urls, image_urls, error, balance, ...}") },
};

// ── Helpers ──────────────────────────────────────────────────────────

static constexpr int32 MaxTableDepth = 32;

// Convert sol::table to FJsonObject (supports nested tables and arrays)
static TSharedPtr<FJsonObject> TableToJson(const sol::table& T, int32 Depth = 0)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	if (Depth > MaxTableDepth) return Json;

	for (const auto& Pair : T)
	{
		if (!Pair.first.is<std::string>()) continue;
		const FString Key = UTF8_TO_TCHAR(Pair.first.as<std::string>().c_str());

		if (Pair.second.is<std::string>())
		{
			Json->SetStringField(Key, UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str()));
		}
		else if (Pair.second.is<bool>())
		{
			Json->SetBoolField(Key, Pair.second.as<bool>());
		}
		else if (Pair.second.is<double>())
		{
			Json->SetNumberField(Key, Pair.second.as<double>());
		}
		else if (Pair.second.is<int>())
		{
			Json->SetNumberField(Key, Pair.second.as<int>());
		}
		else if (Pair.second.is<sol::table>())
		{
			sol::table Sub = Pair.second.as<sol::table>();
			// Check if it's an array (sequential integer keys starting at 1)
			bool bIsArray = true;
			int32 ExpectedIdx = 1;
			for (const auto& SubPair : Sub)
			{
				if (!SubPair.first.is<int>() || SubPair.first.as<int>() != ExpectedIdx)
				{
					bIsArray = false;
					break;
				}
				ExpectedIdx++;
			}

			if (bIsArray)
			{
				TArray<TSharedPtr<FJsonValue>> Arr;
				for (const auto& SubPair : Sub)
				{
					if (SubPair.second.is<std::string>())
						Arr.Add(MakeShared<FJsonValueString>(UTF8_TO_TCHAR(SubPair.second.as<std::string>().c_str())));
					else if (SubPair.second.is<bool>())
						Arr.Add(MakeShared<FJsonValueBoolean>(SubPair.second.as<bool>()));
					else if (SubPair.second.is<double>())
						Arr.Add(MakeShared<FJsonValueNumber>(SubPair.second.as<double>()));
					else if (SubPair.second.is<int>())
						Arr.Add(MakeShared<FJsonValueNumber>(static_cast<double>(SubPair.second.as<int>())));
					else if (SubPair.second.is<sol::table>())
						Arr.Add(MakeShared<FJsonValueObject>(TableToJson(SubPair.second.as<sol::table>(), Depth + 1)));
				}
				Json->SetArrayField(Key, Arr);
			}
			else
			{
				Json->SetObjectField(Key, TableToJson(Sub, Depth + 1));
			}
		}
	}
	return Json;
}

// Convert FJsonObject to a JSON string for schema display
static FString JsonToString(const TSharedPtr<FJsonObject>& Json)
{
	if (!Json.IsValid()) return TEXT("");
	FString Out;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);
	return Out;
}

// Convert FGenerativeJob to sol::table
static sol::table JobToTable(sol::state& Lua, const FGenerativeJob& Job)
{
	sol::table T = Lua.create_table();
	if (!Job.ProviderId.IsEmpty()) T["provider"] = TCHAR_TO_UTF8(*Job.ProviderId);
	if (!Job.ActionId.IsEmpty())   T["action"] = TCHAR_TO_UTF8(*Job.ActionId);
	if (!Job.JobId.IsEmpty())      T["job_id"] = TCHAR_TO_UTF8(*Job.JobId);

	switch (Job.Status)
	{
	case EGenerativeJobStatus::Pending:   T["status"] = "PENDING"; break;
	case EGenerativeJobStatus::Running:   T["status"] = "IN_PROGRESS"; break;
	case EGenerativeJobStatus::Succeeded: T["status"] = "SUCCEEDED"; break;
	case EGenerativeJobStatus::Failed:    T["status"] = "FAILED"; break;
	case EGenerativeJobStatus::Cancelled: T["status"] = "CANCELLED"; break;
	}

	T["progress"] = Job.Progress;

	if (!Job.ResultUrl.IsEmpty())    T["result_url"] = TCHAR_TO_UTF8(*Job.ResultUrl);
	if (!Job.ThumbnailUrl.IsEmpty()) T["thumbnail_url"] = TCHAR_TO_UTF8(*Job.ThumbnailUrl);
	if (!Job.ErrorMessage.IsEmpty()) T["error"] = TCHAR_TO_UTF8(*Job.ErrorMessage);

	if (Job.ExtraUrls.Num() > 0)
	{
		sol::table Extra = Lua.create_table();
		for (const auto& Pair : Job.ExtraUrls)
		{
			Extra[TCHAR_TO_UTF8(*Pair.Key)] = TCHAR_TO_UTF8(*Pair.Value);
		}
		T["extra_urls"] = Extra;
	}

	if (Job.ImageUrls.Num() > 0)
	{
		sol::table Imgs = Lua.create_table();
		for (int32 i = 0; i < Job.ImageUrls.Num(); i++)
		{
			Imgs[i + 1] = TCHAR_TO_UTF8(*Job.ImageUrls[i]);
		}
		T["image_urls"] = Imgs;
	}

	// For balance action, include raw balance value
	if (Job.RawResponse.IsValid() && Job.RawResponse->HasField(TEXT("balance")))
	{
		T["balance"] = Job.RawResponse->GetIntegerField(TEXT("balance"));
	}

	return T;
}

// Download thumbnail and add to session images
static void DownloadThumbnail(FLuaSessionData& Session, const FString& ThumbnailUrl)
{
	if (ThumbnailUrl.IsEmpty()) return;

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(ThumbnailUrl);
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(30.0f);
	Request->ProcessRequestUntilComplete();

	const FHttpResponsePtr Response = Request->GetResponse();
	if (!Response.IsValid() || Response->GetResponseCode() < 200 || Response->GetResponseCode() >= 300) return;

	const TArray<uint8>& Content = Response->GetContent();
	const FString Base64 = FBase64::Encode(Content.GetData(), Content.Num());

	FString MimeType = TEXT("image/png");
	if (ThumbnailUrl.Contains(TEXT(".jpg")) || ThumbnailUrl.Contains(TEXT(".jpeg")))
		MimeType = TEXT("image/jpeg");
	else if (ThumbnailUrl.Contains(TEXT(".webp")))
		MimeType = TEXT("image/webp");

	Session.AddImage(Base64, MimeType, 0, 0);
}

// Pump Slate + rendering while waiting (keeps editor responsive on game thread)
static void PumpEditorTick()
{
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().PumpMessages();
		FSlateApplication::Get().Tick();
	}
	FHttpModule::Get().GetHttpManager().Tick(0.1f);
	FlushRenderingCommands();
}

// Strip query params and fragment identifiers from a URL extension
static FString CleanExtensionFromUrl(const FString& Url, const FString& FallbackFormat)
{
	FString Extension = FPaths::GetExtension(Url);
	// Strip ?query and #fragment
	int32 Pos = INDEX_NONE;
	if (Extension.FindChar(TEXT('?'), Pos))
		Extension = Extension.Left(Pos);
	if (Extension.FindChar(TEXT('#'), Pos))
		Extension = Extension.Left(Pos);
	if (Extension.IsEmpty())
		Extension = FallbackFormat;
	return Extension;
}

// ── Binding ──────────────────────────────────────────────────────────

REGISTER_LUA_BINDING(Generate, GenerateDocs,
[](sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("generate", [&Lua, &Session](sol::table Opts) -> sol::table
	{
		// Parse required fields
		const FString Provider = UTF8_TO_TCHAR(Opts.get_or<std::string>("provider", "").c_str());
		const FString Action = UTF8_TO_TCHAR(Opts.get_or<std::string>("action", "").c_str());

		// ── discover: list all providers and actions ──
		if (Action.Equals(TEXT("discover"), ESearchCase::IgnoreCase))
		{
			const FString OutputFilter = UTF8_TO_TCHAR(Opts.get_or<std::string>("output", "").c_str());
			auto AllActions = FGenerativeProviderRegistry::Get().GetAllActions(OutputFilter);

			sol::table Result = Lua.create_table();
			Result["status"] = "SUCCEEDED";

			sol::table ActionsList = Lua.create_table();
			int32 Idx = 1;
			for (const auto& Pair : AllActions)
			{
				sol::table Entry = Lua.create_table();
				Entry["provider"] = TCHAR_TO_UTF8(*Pair.Key);
				Entry["action"] = TCHAR_TO_UTF8(*Pair.Value.ActionId);
				Entry["description"] = TCHAR_TO_UTF8(*Pair.Value.Description);
				Entry["synchronous"] = Pair.Value.bIsSynchronous;

				sol::table Inputs = Lua.create_table();
				for (int32 i = 0; i < Pair.Value.InputHints.Num(); i++)
					Inputs[i + 1] = TCHAR_TO_UTF8(*Pair.Value.InputHints[i]);
				Entry["inputs"] = Inputs;

				sol::table Outputs = Lua.create_table();
				for (int32 i = 0; i < Pair.Value.OutputHints.Num(); i++)
					Outputs[i + 1] = TCHAR_TO_UTF8(*Pair.Value.OutputHints[i]);
				Entry["outputs"] = Outputs;

				if (!Pair.Value.CreditCost.IsEmpty())
					Entry["cost"] = TCHAR_TO_UTF8(*Pair.Value.CreditCost);

				if (Pair.Value.ParamsSchema.IsValid())
					Entry["params_schema"] = TCHAR_TO_UTF8(*JsonToString(Pair.Value.ParamsSchema));

				ActionsList[Idx++] = Entry;
			}
			Result["actions"] = ActionsList;

			// Also list providers summary
			sol::table ProvidersList = Lua.create_table();
			Idx = 1;
			for (const auto& Prov : FGenerativeProviderRegistry::Get().GetAll())
			{
				sol::table Entry = Lua.create_table();
				Entry["id"] = TCHAR_TO_UTF8(*Prov->GetId());
				Entry["name"] = TCHAR_TO_UTF8(*Prov->GetDisplayName());
				Entry["action_count"] = Prov->GetActions().Num();
				const FString Website = Prov->GetWebsite();
				if (!Website.IsEmpty())
					Entry["website"] = TCHAR_TO_UTF8(*Website);
				ProvidersList[Idx++] = Entry;
			}
			Result["providers"] = ProvidersList;

			Session.Log(FString::Printf(TEXT("[OK] discover -> %d actions across %d providers"),
				AllActions.Num(), FGenerativeProviderRegistry::Get().Num()));
			return Result;
		}

		// ── Resolve provider ──
		if (Provider.IsEmpty())
		{
			Session.Log(TEXT("[ERROR] generate: 'provider' is required"));
			sol::table Err = Lua.create_table();
			Err["status"] = "FAILED";
			Err["error"] = "provider is required";
			return Err;
		}

		auto ProviderPtr = FGenerativeProviderRegistry::Get().Find(Provider);
		if (!ProviderPtr.IsValid())
		{
			Session.Log(FString::Printf(TEXT("[ERROR] generate: unknown provider '%s'"), *Provider));
			sol::table Err = Lua.create_table();
			Err["status"] = "FAILED";
			Err["error"] = TCHAR_TO_UTF8(*FString::Printf(TEXT("Unknown provider: %s"), *Provider));
			return Err;
		}

		// ── balance: check credit balance ──
		if (Action.Equals(TEXT("balance"), ESearchCase::IgnoreCase))
		{
			const int32 Balance = ProviderPtr->GetBalance();
			sol::table Result = Lua.create_table();
			if (Balance < 0)
			{
				Result["status"] = "FAILED";
				Result["error"] = "This provider does not support balance checking";
				Session.Log(FString::Printf(TEXT("[ERROR] generate balance: provider '%s' does not support balance"), *Provider));
			}
			else
			{
				Result["status"] = "SUCCEEDED";
				Result["balance"] = Balance;
				Session.Log(FString::Printf(TEXT("[OK] generate balance -> %s: %d"), *Provider, Balance));
			}
			return Result;
		}

		// ── cancel: cancel a running job ──
		if (Action.Equals(TEXT("cancel"), ESearchCase::IgnoreCase))
		{
			const FString JobId = UTF8_TO_TCHAR(Opts.get_or<std::string>("job_id", "").c_str());
			if (JobId.IsEmpty())
			{
				Session.Log(TEXT("[ERROR] generate cancel: job_id is required"));
				sol::table Err = Lua.create_table();
				Err["status"] = "FAILED";
				Err["error"] = "job_id is required for cancel";
				return Err;
			}

			const bool bSuccess = ProviderPtr->CancelJob(JobId);
			sol::table Result = Lua.create_table();
			if (bSuccess)
			{
				Result["status"] = "SUCCEEDED";
				Result["job_id"] = TCHAR_TO_UTF8(*JobId);
				Session.Log(FString::Printf(TEXT("[OK] generate cancel -> %s"), *JobId));
			}
			else
			{
				Result["status"] = "FAILED";
				Result["error"] = "Cancel failed or not supported by this provider";
				Result["job_id"] = TCHAR_TO_UTF8(*JobId);
				Session.Log(FString::Printf(TEXT("[ERROR] generate cancel: failed for job %s"), *JobId));
			}
			return Result;
		}

		// ── get_result: fetch final result of a completed job ──
		if (Action.Equals(TEXT("get_result"), ESearchCase::IgnoreCase))
		{
			const FString JobId = UTF8_TO_TCHAR(Opts.get_or<std::string>("job_id", "").c_str());
			if (JobId.IsEmpty())
			{
				Session.Log(TEXT("[ERROR] generate get_result: job_id is required"));
				sol::table Err = Lua.create_table();
				Err["status"] = "FAILED";
				Err["error"] = "job_id is required for get_result";
				return Err;
			}

			const FString OrigAction = UTF8_TO_TCHAR(Opts.get_or<std::string>("original_action", "").c_str());
			FGenerativeJob Job = ProviderPtr->GetResult(JobId, OrigAction);
			Job.ProviderId = Provider;

			if (Job.IsSuccess() && !Job.ThumbnailUrl.IsEmpty())
			{
				DownloadThumbnail(Session, Job.ThumbnailUrl);
			}

			auto ResultTable = JobToTable(Lua, Job);
			Session.Log(FString::Printf(TEXT("[OK] generate get_result -> %s %s"),
				*JobId, Job.IsSuccess() ? TEXT("SUCCEEDED") : TEXT("not ready")));
			return ResultTable;
		}

		// ── check: poll job status ──
		if (Action.Equals(TEXT("check"), ESearchCase::IgnoreCase))
		{
			const FString JobId = UTF8_TO_TCHAR(Opts.get_or<std::string>("job_id", "").c_str());
			if (JobId.IsEmpty())
			{
				Session.Log(TEXT("[ERROR] generate check: job_id is required"));
				sol::table Err = Lua.create_table();
				Err["status"] = "FAILED";
				Err["error"] = "job_id is required for check";
				return Err;
			}

			const FString OrigAction = UTF8_TO_TCHAR(Opts.get_or<std::string>("original_action", "").c_str());
			const bool bWait = Opts.get_or("wait", true);
			const int32 Timeout = Opts.get_or("timeout", 300);
			const int32 PollInterval = FMath::Clamp(Opts.get_or("poll_interval", 10), 3, 60);

			FGenerativeJob Job;
			const double StartTime = FPlatformTime::Seconds();

			do
			{
				Job = ProviderPtr->CheckStatus(JobId, OrigAction);
				Job.ProviderId = Provider;

				if (Job.IsTerminal()) break;
				if (!bWait) break;
				if (FPlatformTime::Seconds() - StartTime > Timeout) break;

				// Wait between polls — pump editor to keep UI responsive
				for (int32 i = 0; i < PollInterval * 10; i++)
				{
					FPlatformProcess::Sleep(0.1f);
					PumpEditorTick();
				}
			} while (true);

			// Download thumbnail preview if available
			if (Job.IsSuccess() && !Job.ThumbnailUrl.IsEmpty())
			{
				DownloadThumbnail(Session, Job.ThumbnailUrl);
			}

			auto ResultTable = JobToTable(Lua, Job);
			Session.Log(FString::Printf(TEXT("[OK] generate check -> %s %d%%"),
				*Job.JobId, Job.Progress));
			return ResultTable;
		}

		// ── import: download result and import into UE5 ──
		if (Action.Equals(TEXT("import"), ESearchCase::IgnoreCase))
		{
			const FString JobId = UTF8_TO_TCHAR(Opts.get_or<std::string>("job_id", "").c_str());
			const FString ResultUrl = UTF8_TO_TCHAR(Opts.get_or<std::string>("result_url", "").c_str());
			const FString AssetPath = UTF8_TO_TCHAR(Opts.get_or<std::string>("asset_path", "/Game/Generated").c_str());
			const FString AssetName = UTF8_TO_TCHAR(Opts.get_or<std::string>("asset_name", "").c_str());
			const FString OrigAction = UTF8_TO_TCHAR(Opts.get_or<std::string>("original_action", "").c_str());
			const FString Format = UTF8_TO_TCHAR(Opts.get_or<std::string>("format", "glb").c_str());

			// Validate asset path
			if (!AssetPath.StartsWith(TEXT("/")))
			{
				Session.Log(FString::Printf(TEXT("[ERROR] generate import: invalid asset_path '%s' (must start with /)"), *AssetPath));
				sol::table Err = Lua.create_table();
				Err["status"] = "FAILED";
				Err["error"] = TCHAR_TO_UTF8(*FString::Printf(TEXT("Invalid asset_path '%s'. Must start with / (e.g., /Game/Generated)"), *AssetPath));
				return Err;
			}

			// Get result URL — either provided directly or fetched from job
			FString DownloadUrl = ResultUrl;
			if (DownloadUrl.IsEmpty() && !JobId.IsEmpty())
			{
				FGenerativeJob Job = ProviderPtr->GetResult(JobId, OrigAction);
				if (!Job.IsSuccess())
				{
					Session.Log(FString::Printf(TEXT("[ERROR] generate import: job not ready: %s"), *Job.ErrorMessage));
					sol::table Err = Lua.create_table();
					Err["status"] = "FAILED";
					Err["error"] = TCHAR_TO_UTF8(*FString::Printf(TEXT("Job not ready: %s"), *Job.ErrorMessage));
					return Err;
				}
				// Try requested format first, then primary URL
				if (Job.ExtraUrls.Contains(Format))
					DownloadUrl = Job.ExtraUrls[Format];
				else
					DownloadUrl = Job.ResultUrl;
			}

			if (DownloadUrl.IsEmpty())
			{
				Session.Log(TEXT("[ERROR] generate import: no download URL"));
				sol::table Err = Lua.create_table();
				Err["status"] = "FAILED";
				Err["error"] = "No download URL available. Provide result_url or job_id.";
				return Err;
			}

			// Determine file extension from URL
			const FString Extension = CleanExtensionFromUrl(DownloadUrl, Format);

			// Download to temp
			const FString TempDir = FPaths::ProjectSavedDir() / TEXT("Temp");
			const FString TempFile = TempDir / FString::Printf(TEXT("gen_%s.%s"),
				*FGuid::NewGuid().ToString(), *Extension);
			IFileManager::Get().MakeDirectory(*TempDir, true);

			TSharedRef<IHttpRequest, ESPMode::ThreadSafe> DlReq = FHttpModule::Get().CreateRequest();
			DlReq->SetURL(DownloadUrl);
			DlReq->SetVerb(TEXT("GET"));
			DlReq->SetTimeout(300.0f);
			DlReq->ProcessRequestUntilComplete();

			const FHttpResponsePtr DlResp = DlReq->GetResponse();
			if (!DlResp.IsValid() || DlResp->GetResponseCode() < 200 || DlResp->GetResponseCode() >= 300)
			{
				const int32 Code = DlResp.IsValid() ? DlResp->GetResponseCode() : 0;
				Session.Log(FString::Printf(TEXT("[ERROR] generate import: download failed (HTTP %d)"), Code));
				sol::table Err = Lua.create_table();
				Err["status"] = "FAILED";
				Err["error"] = TCHAR_TO_UTF8(*FString::Printf(TEXT("Failed to download result file (HTTP %d)"), Code));
				return Err;
			}

			FFileHelper::SaveArrayToFile(DlResp->GetContent(), *TempFile);

			// Import based on file type
			FString FinalName = AssetName;
			if (FinalName.IsEmpty())
			{
				FinalName = FString::Printf(TEXT("Generated_%s"), *FGuid::NewGuid().ToString().Left(8));
			}

			FString ImportedPath;
			FString ImportError;
			const FString ExtLower = Extension.ToLower();

			if (ExtLower == TEXT("glb") || ExtLower == TEXT("gltf") || ExtLower == TEXT("fbx") || ExtLower == TEXT("obj"))
			{
				UStaticMesh* Mesh = AssetImportUtils::ImportStaticMesh(TempFile, AssetPath, FinalName, ImportError);
				if (Mesh)
					ImportedPath = Mesh->GetPathName();
				else
					ImportedPath = FString::Printf(TEXT("ERROR: %s. File saved to: %s"), *ImportError, *TempFile);
			}
			else if (ExtLower == TEXT("png") || ExtLower == TEXT("jpg") || ExtLower == TEXT("jpeg") || ExtLower == TEXT("webp"))
			{
				UTexture2D* Tex = AssetImportUtils::ImportTexture(TempFile, AssetPath, FinalName, ImportError);
				if (Tex)
					ImportedPath = Tex->GetPathName();
				else
					ImportedPath = FString::Printf(TEXT("ERROR: %s. File saved to: %s"), *ImportError, *TempFile);
			}
			else if (ExtLower == TEXT("wav") || ExtLower == TEXT("ogg"))
			{
				USoundWave* Sound = AssetImportUtils::ImportAudio(TempFile, AssetPath, FinalName, ImportError);
				if (Sound)
					ImportedPath = Sound->GetPathName();
				else
					ImportedPath = FString::Printf(TEXT("ERROR: %s. File saved to: %s"), *ImportError, *TempFile);
			}
			else if (ExtLower == TEXT("mp3"))
			{
				// MP3 not directly importable by UE — agent should convert to WAV first
				ImportedPath = FString::Printf(TEXT("MP3 format not directly supported by UE5. Convert to WAV first. File saved to: %s"), *TempFile);
			}
			else
			{
				ImportedPath = FString::Printf(TEXT("Unknown format '%s'. File saved to: %s"), *Extension, *TempFile);
			}

			// Cleanup temp file only if import succeeded (keep on failure for debugging)
			if (ImportedPath.StartsWith(TEXT("/")) && !ImportedPath.StartsWith(TEXT("ERROR")) && !ImportedPath.StartsWith(TEXT("MP3")) && !ImportedPath.StartsWith(TEXT("Unknown")))
			{
				IFileManager::Get().Delete(*TempFile);
			}

			sol::table Result = Lua.create_table();
			const bool bImportOk = ImportedPath.StartsWith(TEXT("/")) && !ImportedPath.Contains(TEXT("ERROR"));
			Result["status"] = bImportOk ? "SUCCEEDED" : "FAILED";
			Result["asset_path"] = TCHAR_TO_UTF8(*ImportedPath);
			if (!bImportOk && !ImportError.IsEmpty())
				Result["error"] = TCHAR_TO_UTF8(*ImportError);
			Session.Log(FString::Printf(TEXT("[%s] generate import -> %s"),
				bImportOk ? TEXT("OK") : TEXT("ERROR"), *ImportedPath));
			return Result;
		}

		// ── Regular submit action ──
		if (Action.IsEmpty())
		{
			Session.Log(TEXT("[ERROR] generate: 'action' is required"));
			sol::table Err = Lua.create_table();
			Err["status"] = "FAILED";
			Err["error"] = "action is required";
			return Err;
		}

		if (!ProviderPtr->SupportsAction(Action))
		{
			Session.Log(FString::Printf(TEXT("[ERROR] generate: provider '%s' does not support action '%s'"),
				*Provider, *Action));
			sol::table Err = Lua.create_table();
			Err["status"] = "FAILED";
			Err["error"] = TCHAR_TO_UTF8(*FString::Printf(TEXT("Provider '%s' does not support action '%s'"),
				*Provider, *Action));
			return Err;
		}

		// Convert remaining opts to JSON params (skip provider/action meta-keys)
		auto Params = TableToJson(Opts);
		// Remove meta-keys that aren't meant for the provider
		Params->RemoveField(TEXT("provider"));
		Params->RemoveField(TEXT("action"));

		FGenerativeJob Job = ProviderPtr->Submit(Action, Params);
		Job.ProviderId = Provider;
		Job.ActionId = Action;

		auto ResultTable = JobToTable(Lua, Job);

		if (Job.IsSuccess())
		{
			Session.Log(FString::Printf(TEXT("[OK] generate %s/%s -> SUCCEEDED"), *Provider, *Action));
			// Download thumbnail if available
			if (!Job.ThumbnailUrl.IsEmpty())
			{
				DownloadThumbnail(Session, Job.ThumbnailUrl);
			}
		}
		else if (Job.Status == EGenerativeJobStatus::Failed)
		{
			Session.Log(FString::Printf(TEXT("[ERROR] generate %s/%s -> %s"),
				*Provider, *Action, *Job.ErrorMessage));
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[OK] generate %s/%s -> job_id=%s status=%s"),
				*Provider, *Action, *Job.JobId,
				Job.Status == EGenerativeJobStatus::Pending ? TEXT("PENDING") : TEXT("IN_PROGRESS")));
		}

		return ResultTable;
	});
});
