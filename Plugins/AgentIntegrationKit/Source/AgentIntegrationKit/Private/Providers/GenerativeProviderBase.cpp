// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Providers/GenerativeProvider.h"
#include "ACPSettings.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"

// ── IGenerativeProvider base methods ─────────────────────────────────

bool IGenerativeProvider::SupportsAction(const FString& ActionId) const
{
	return FindAction(ActionId) != nullptr;
}

const FProviderActionDescriptor* IGenerativeProvider::FindAction(const FString& ActionId) const
{
	const auto& Actions = GetCachedActions();
	for (const auto& Action : Actions)
	{
		if (Action.ActionId == ActionId)
		{
			return &Action;
		}
	}
	return nullptr;
}

const TArray<FProviderActionDescriptor>& IGenerativeProvider::GetCachedActions() const
{
	if (!bActionsCached)
	{
		CachedActions = GetActions();
		bActionsCached = true;
	}
	return CachedActions;
}

// ── Auth & URL routing ───────────────────────────────────────────────

FString FGenerativeProviderBase::GetAuthToken() const
{
	const UACPSettings* Settings = UACPSettings::Get();
	if (Settings->ShouldUseBetideCredits())
	{
		return Settings->GetBetideApiToken();
	}
	// Look up provider-specific key from settings
	const FString KeyName = GetApiKeySettingName();
	// Use reflection to read the property by name
	FProperty* Prop = UACPSettings::StaticClass()->FindPropertyByName(*KeyName);
	if (Prop)
	{
		FString Value;
		const FStrProperty* StrProp = CastField<FStrProperty>(Prop);
		if (StrProp)
		{
			Value = StrProp->GetPropertyValue_InContainer(Settings);
		}
		return Value;
	}
	return TEXT("");
}

FString FGenerativeProviderBase::GetBaseUrl() const
{
	if (IsUsingProxy())
	{
		return FString::Printf(TEXT("https://betide.studio%s"), *GetProxyPath());
	}
	return GetDirectBaseUrl();
}

bool FGenerativeProviderBase::IsUsingProxy() const
{
	const UACPSettings* Settings = UACPSettings::Get();
	return Settings->ShouldUseBetideCredits();
}

// ── Auth header ──────────────────────────────────────────────────────

void FGenerativeProviderBase::SetAuthHeaders(const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& Request) const
{
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *GetAuthToken()));
}

// ── HTTP helpers ─────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FGenerativeProviderBase::MakeJsonRequest(
	const FString& Verb,
	const FString& FullUrl,
	const TSharedPtr<FJsonObject>& Body,
	FString& OutError,
	float TimeoutSeconds) const
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(FullUrl);
	Request->SetVerb(Verb);
	SetAuthHeaders(Request);
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetTimeout(TimeoutSeconds);

	if (Body.IsValid())
	{
		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);
		Request->SetContentAsString(BodyStr);
	}

	Request->ProcessRequestUntilComplete();

	const FHttpResponsePtr Response = Request->GetResponse();
	if (!Response.IsValid())
	{
		OutError = FString::Printf(TEXT("HTTP request failed (no response) for %s %s"), *Verb, *FullUrl);
		return nullptr;
	}

	const int32 Code = Response->GetResponseCode();
	const FString ResponseBody = Response->GetContentAsString();

	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
	FJsonSerializer::Deserialize(Reader, Json);

	if (Code < 200 || Code >= 300)
	{
		FString ApiMsg;
		if (Json.IsValid())
		{
			if (Json->HasField(TEXT("message")))
			{
				ApiMsg = Json->GetStringField(TEXT("message"));
			}
			else if (Json->HasTypedField<EJson::Object>(TEXT("error")))
			{
				ApiMsg = Json->GetObjectField(TEXT("error"))->GetStringField(TEXT("message"));
			}
		}
		OutError = FString::Printf(TEXT("HTTP %d: %s"), Code,
			ApiMsg.IsEmpty() ? *ResponseBody.Left(500) : *ApiMsg);
		return nullptr;
	}

	return Json;
}

TSharedPtr<FJsonObject> FGenerativeProviderBase::HttpPost(
	const FString& Path,
	const TSharedPtr<FJsonObject>& Body,
	float TimeoutSeconds) const
{
	const FString FullUrl = GetBaseUrl() + Path;
	FString Error;
	auto Result = MakeJsonRequest(TEXT("POST"), FullUrl, Body, Error, TimeoutSeconds);
	if (!Result.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("[%s] POST %s failed: %s"), *GetId(), *Path, *Error);
	}
	return Result;
}

TSharedPtr<FJsonObject> FGenerativeProviderBase::HttpGet(
	const FString& Path,
	float TimeoutSeconds) const
{
	const FString FullUrl = GetBaseUrl() + Path;
	FString Error;
	auto Result = MakeJsonRequest(TEXT("GET"), FullUrl, nullptr, Error, TimeoutSeconds);
	if (!Result.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("[%s] GET %s failed: %s"), *GetId(), *Path, *Error);
	}
	return Result;
}

TSharedPtr<FJsonObject> FGenerativeProviderBase::HttpDelete(
	const FString& Path,
	float TimeoutSeconds) const
{
	const FString FullUrl = GetBaseUrl() + Path;
	FString Error;
	return MakeJsonRequest(TEXT("DELETE"), FullUrl, nullptr, Error, TimeoutSeconds);
}

bool FGenerativeProviderBase::HttpDownload(
	const FString& Url,
	const FString& OutputPath,
	FString& OutError,
	float TimeoutSeconds) const
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(TimeoutSeconds);
	Request->ProcessRequestUntilComplete();

	const FHttpResponsePtr Response = Request->GetResponse();
	if (!Response.IsValid() || Response->GetResponseCode() < 200 || Response->GetResponseCode() >= 300)
	{
		OutError = FString::Printf(TEXT("Download failed: HTTP %d"),
			Response.IsValid() ? Response->GetResponseCode() : 0);
		return false;
	}

	if (!FFileHelper::SaveArrayToFile(Response->GetContent(), *OutputPath))
	{
		OutError = FString::Printf(TEXT("Failed to save to %s"), *OutputPath);
		return false;
	}

	return true;
}

// ── Raw binary POST ──────────────────────────────────────────────────

TArray<uint8> FGenerativeProviderBase::HttpPostRaw(
	const FString& Path,
	const TSharedPtr<FJsonObject>& Body,
	FString& OutError,
	FString& OutContentType,
	float TimeoutSeconds) const
{
	const FString FullUrl = GetBaseUrl() + Path;

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(FullUrl);
	Request->SetVerb(TEXT("POST"));
	SetAuthHeaders(Request);
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetTimeout(TimeoutSeconds);

	if (Body.IsValid())
	{
		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);
		Request->SetContentAsString(BodyStr);
	}

	Request->ProcessRequestUntilComplete();

	const FHttpResponsePtr Response = Request->GetResponse();
	if (!Response.IsValid())
	{
		OutError = FString::Printf(TEXT("HTTP request failed (no response) for POST %s"), *Path);
		return {};
	}

	const int32 Code = Response->GetResponseCode();
	if (Code < 200 || Code >= 300)
	{
		// Try to parse JSON error body
		const FString ResponseBody = Response->GetContentAsString();
		TSharedPtr<FJsonObject> Json;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
		FJsonSerializer::Deserialize(Reader, Json);

		FString ApiMsg;
		if (Json.IsValid())
		{
			if (Json->HasTypedField<EJson::Object>(TEXT("detail")))
			{
				ApiMsg = Json->GetObjectField(TEXT("detail"))->GetStringField(TEXT("message"));
			}
			else if (Json->HasField(TEXT("message")))
			{
				ApiMsg = Json->GetStringField(TEXT("message"));
			}
		}
		OutError = FString::Printf(TEXT("HTTP %d: %s"), Code,
			ApiMsg.IsEmpty() ? *ResponseBody.Left(500) : *ApiMsg);
		return {};
	}

	OutContentType = Response->GetContentType();
	return TArray<uint8>(Response->GetContent());
}

// ── Status parsing ───────────────────────────────────────────────────

EGenerativeJobStatus FGenerativeProviderBase::ParseStatus(const FString& StatusStr)
{
	const FString Upper = StatusStr.ToUpper();
	if (Upper == TEXT("SUCCEEDED") || Upper == TEXT("SUCCESS") || Upper == TEXT("COMPLETED"))
		return EGenerativeJobStatus::Succeeded;
	if (Upper == TEXT("FAILED") || Upper == TEXT("ERROR"))
		return EGenerativeJobStatus::Failed;
	if (Upper == TEXT("CANCELED") || Upper == TEXT("CANCELLED"))
		return EGenerativeJobStatus::Cancelled;
	if (Upper == TEXT("IN_PROGRESS") || Upper == TEXT("RUNNING") || Upper == TEXT("PROCESSING"))
		return EGenerativeJobStatus::Running;
	return EGenerativeJobStatus::Pending; // PENDING, QUEUED, or unknown
}

// ── Schema builders ──────────────────────────────────────────────────

TSharedPtr<FJsonObject> FGenerativeProviderBase::SchemaString(
	const FString& Desc, const TArray<FString>& Enum, const FString& Default)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("type"), TEXT("string"));
	Obj->SetStringField(TEXT("description"), Desc);
	if (Enum.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> EnumArr;
		for (const auto& E : Enum) EnumArr.Add(MakeShared<FJsonValueString>(E));
		Obj->SetArrayField(TEXT("enum"), EnumArr);
	}
	if (!Default.IsEmpty())
	{
		Obj->SetStringField(TEXT("default"), Default);
	}
	return Obj;
}

TSharedPtr<FJsonObject> FGenerativeProviderBase::SchemaInt(
	const FString& Desc, int32 Min, int32 Max, int32 Default)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("type"), TEXT("integer"));
	Obj->SetStringField(TEXT("description"), Desc);
	if (Min != 0 || Max != 0)
	{
		if (Min != 0) Obj->SetNumberField(TEXT("minimum"), Min);
		if (Max != 0) Obj->SetNumberField(TEXT("maximum"), Max);
	}
	if (Default != 0) Obj->SetNumberField(TEXT("default"), Default);
	return Obj;
}

TSharedPtr<FJsonObject> FGenerativeProviderBase::SchemaBool(const FString& Desc, bool Default)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("type"), TEXT("boolean"));
	Obj->SetStringField(TEXT("description"), Desc);
	Obj->SetBoolField(TEXT("default"), Default);
	return Obj;
}

TSharedPtr<FJsonObject> FGenerativeProviderBase::SchemaStringArray(const FString& Desc)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("type"), TEXT("array"));
	Obj->SetStringField(TEXT("description"), Desc);
	TSharedPtr<FJsonObject> Items = MakeShared<FJsonObject>();
	Items->SetStringField(TEXT("type"), TEXT("string"));
	Obj->SetObjectField(TEXT("items"), Items);
	return Obj;
}

TSharedPtr<FJsonObject> FGenerativeProviderBase::BuildSchema(
	const TMap<FString, TSharedPtr<FJsonObject>>& Properties,
	const TArray<FString>& Required)
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	for (const auto& Pair : Properties)
	{
		Props->SetObjectField(Pair.Key, Pair.Value);
	}
	Schema->SetObjectField(TEXT("properties"), Props);

	if (Required.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ReqArr;
		for (const auto& R : Required) ReqArr.Add(MakeShared<FJsonValueString>(R));
		Schema->SetArrayField(TEXT("required"), ReqArr);
	}

	return Schema;
}
