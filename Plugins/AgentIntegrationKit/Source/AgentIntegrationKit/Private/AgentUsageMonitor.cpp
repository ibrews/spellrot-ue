// Copyright 2026 Betide Studio. All Rights Reserved.

#include "AgentUsageMonitor.h"
#include "AgentIntegrationKitModule.h"
#include "ACPSettings.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"

const FAgentRateLimitData FAgentUsageMonitor::EmptyData;

FAgentUsageMonitor& FAgentUsageMonitor::Get()
{
	static FAgentUsageMonitor Instance;
	return Instance;
}

void FAgentUsageMonitor::Initialize()
{
	if (bInitialized)
	{
		return;
	}
	bInitialized = true;

	// 1.173s interval reduces overlap with engine's 1.0s stat flush cycle
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FAgentUsageMonitor::Tick), 1.173f);
}

void FAgentUsageMonitor::Shutdown()
{
	if (!bInitialized)
	{
		return;
	}
	bInitialized = false;

	FTSTicker::RemoveTicker(TickerHandle);
	TickerHandle.Reset();
	CachedData.Empty();
	ActiveAgentName.Empty();
}

bool FAgentUsageMonitor::IsAgentSupported(const FString& AgentName)
{
	return AgentName == TEXT("Claude Code") || AgentName == TEXT("Codex CLI");
}

void FAgentUsageMonitor::RequestUsageUpdate(const FString& AgentName)
{
	ActiveAgentName = AgentName;

	if (!IsAgentSupported(AgentName))
	{
		return;
	}

	// Set loading state
	FAgentRateLimitData& Data = CachedData.FindOrAdd(AgentName);
	Data.bIsLoading = true;
	Data.AgentName = AgentName;
	OnUsageDataUpdated.Broadcast(AgentName, Data);

	// Do the actual fetch
	if (AgentName == TEXT("Claude Code"))
	{
		FClaudeCredentials Creds = ReadClaudeCredentials();
		if (!Creds.bIsValid)
		{
			Data.bIsLoading = false;
			Data.ErrorMessage = Creds.ErrorMessage;
			OnUsageDataUpdated.Broadcast(AgentName, Data);
			return;
		}

		// Check expiry
		if (Creds.ExpiresAtEpochMs > 0.0)
		{
			double NowMs = FDateTime::UtcNow().ToUnixTimestamp() * 1000.0;
			if (NowMs >= Creds.ExpiresAtEpochMs)
			{
				Data.bIsLoading = false;
				Data.ErrorMessage = TEXT("Token expired. Run 'claude' in terminal to re-authenticate.");
				OnUsageDataUpdated.Broadcast(AgentName, Data);
				return;
			}
		}

		CachedClaudeRateLimitTier = Creds.RateLimitTier;
		FetchClaudeUsage(Creds.AccessToken);
	}
	else if (AgentName == TEXT("Codex CLI"))
	{
		FCodexCredentials Creds = ReadCodexCredentials();
		if (!Creds.bIsValid)
		{
			Data.bIsLoading = false;
			Data.ErrorMessage = Creds.ErrorMessage;
			OnUsageDataUpdated.Broadcast(AgentName, Data);
			return;
		}

		FetchCodexUsage(Creds.AccessToken, Creds.AccountId);
	}

	LastFetchTimeSeconds = FPlatformTime::Seconds();
	bFetchInFlight = true;
}

const FAgentRateLimitData& FAgentUsageMonitor::GetCachedUsage(const FString& AgentName) const
{
	const FAgentRateLimitData* Found = CachedData.Find(AgentName);
	return Found ? *Found : EmptyData;
}

// ============================================================================
// Ticker
// ============================================================================

bool FAgentUsageMonitor::Tick(float DeltaTime)
{
	double Now = FPlatformTime::Seconds();

	// ── Agent usage polling ──
	if (!ActiveAgentName.IsEmpty() && IsAgentSupported(ActiveAgentName) && !bFetchInFlight)
	{
		const FAgentRateLimitData& Data = GetCachedUsage(ActiveAgentName);
		double Interval = Data.ErrorMessage.IsEmpty() ? PollIntervalSeconds : RetryIntervalSeconds;

		if ((Now - LastFetchTimeSeconds) >= Interval)
		{
			RequestUsageUpdate(ActiveAgentName);
		}
	}

	// ── Meshy balance polling ──
	if (!bMeshyFetchInFlight)
	{
		UACPSettings* Settings = UACPSettings::Get();
		if (Settings && Settings->HasMeshyAuth())
		{
			double MeshyInterval = MeshyBalanceError.IsEmpty() ? PollIntervalSeconds : RetryIntervalSeconds;
			if ((Now - LastMeshyFetchTimeSeconds) >= MeshyInterval)
			{
				FetchMeshyBalance();
			}
		}
	}

	return true;
}

// ============================================================================
// Credential Reading
// ============================================================================

FAgentUsageMonitor::FClaudeCredentials FAgentUsageMonitor::ReadClaudeCredentials() const
{
	FClaudeCredentials Result;

	// Try reading credentials JSON from multiple sources:
	// 1. File on disk: ~/.claude/.credentials.json
	// 2. macOS Keychain: service "Claude Code-credentials" (Claude CLI stores creds here on macOS)
	FString CredentialsJson;

	// Source 1: File on disk
	FString HomePath = FPlatformProcess::UserHomeDir();
	FString CredentialsPath = FPaths::Combine(HomePath, TEXT(".claude"), TEXT(".credentials.json"));
	bool bFoundCredentials = FFileHelper::LoadFileToString(CredentialsJson, *CredentialsPath);

#if PLATFORM_MAC
	// Source 2: macOS Keychain via security CLI
	if (!bFoundCredentials)
	{
		FString StdOut, StdErr;
		int32 ReturnCode = -1;
		bool bLaunched = FPlatformProcess::ExecProcess(
			TEXT("/usr/bin/security"),
			TEXT("find-generic-password -s \"Claude Code-credentials\" -w"),
			&ReturnCode, &StdOut, &StdErr);

		if (bLaunched && ReturnCode == 0 && !StdOut.IsEmpty())
		{
			CredentialsJson = StdOut.TrimStartAndEnd();
			bFoundCredentials = true;
			UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[UsageMonitor] Read Claude credentials from macOS Keychain"));
		}
	}
#endif

	if (!bFoundCredentials)
	{
		Result.ErrorMessage = TEXT("Not authenticated. Run 'claude' in terminal to log in.");
		return Result;
	}

	TSharedPtr<FJsonObject> RootJson;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(CredentialsJson);
	if (!FJsonSerializer::Deserialize(Reader, RootJson) || !RootJson.IsValid())
	{
		Result.ErrorMessage = TEXT("Claude credentials are corrupted. Run 'claude' to re-authenticate.");
		return Result;
	}

	// Parse claudeAiOauth object
	const TSharedPtr<FJsonObject>* OAuthObj = nullptr;
	if (!RootJson->TryGetObjectField(TEXT("claudeAiOauth"), OAuthObj) || !OAuthObj || !(*OAuthObj).IsValid())
	{
		Result.ErrorMessage = TEXT("Claude credentials missing OAuth data. Run 'claude' to authenticate.");
		return Result;
	}

	FString AccessToken;
	if (!(*OAuthObj)->TryGetStringField(TEXT("accessToken"), AccessToken) || AccessToken.IsEmpty())
	{
		Result.ErrorMessage = TEXT("Claude access token is empty. Run 'claude' to re-authenticate.");
		return Result;
	}

	Result.AccessToken = AccessToken;
	Result.bIsValid = true;

	// Optional: expiry
	double ExpiresAt = 0.0;
	if ((*OAuthObj)->TryGetNumberField(TEXT("expiresAt"), ExpiresAt))
	{
		Result.ExpiresAtEpochMs = ExpiresAt;
	}

	// Optional: rate limit tier (used to infer plan type)
	FString RateLimitTier;
	if ((*OAuthObj)->TryGetStringField(TEXT("rateLimitTier"), RateLimitTier))
	{
		Result.RateLimitTier = RateLimitTier;
	}

	return Result;
}

FAgentUsageMonitor::FCodexCredentials FAgentUsageMonitor::ReadCodexCredentials() const
{
	FCodexCredentials Result;

	// Respect CODEX_HOME env var
	FString CodexHome = FPlatformMisc::GetEnvironmentVariable(TEXT("CODEX_HOME"));
	FString AuthPath;
	if (!CodexHome.IsEmpty())
	{
		AuthPath = FPaths::Combine(CodexHome, TEXT("auth.json"));
	}
	else
	{
		FString HomePath = FPlatformProcess::UserHomeDir();
		AuthPath = FPaths::Combine(HomePath, TEXT(".codex"), TEXT("auth.json"));
	}

	FString FileContents;
	if (!FFileHelper::LoadFileToString(FileContents, *AuthPath))
	{
		Result.ErrorMessage = TEXT("Not authenticated. Run 'codex' in terminal to log in.");
		return Result;
	}

	TSharedPtr<FJsonObject> RootJson;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContents);
	if (!FJsonSerializer::Deserialize(Reader, RootJson) || !RootJson.IsValid())
	{
		Result.ErrorMessage = TEXT("Codex auth file is corrupted. Run 'codex' to re-authenticate.");
		return Result;
	}

	// Check for direct API key first
	FString ApiKey;
	if (RootJson->TryGetStringField(TEXT("OPENAI_API_KEY"), ApiKey) && !ApiKey.IsEmpty())
	{
		Result.AccessToken = ApiKey;
		Result.bIsApiKey = true;
		Result.bIsValid = true;
		return Result;
	}

	// Parse tokens object
	const TSharedPtr<FJsonObject>* TokensObj = nullptr;
	if (!RootJson->TryGetObjectField(TEXT("tokens"), TokensObj) || !TokensObj || !(*TokensObj).IsValid())
	{
		Result.ErrorMessage = TEXT("Codex auth file missing tokens. Run 'codex' to authenticate.");
		return Result;
	}

	FString AccessToken;
	if (!(*TokensObj)->TryGetStringField(TEXT("access_token"), AccessToken) || AccessToken.IsEmpty())
	{
		Result.ErrorMessage = TEXT("Codex access token is empty. Run 'codex' to re-authenticate.");
		return Result;
	}

	Result.AccessToken = AccessToken;
	Result.bIsValid = true;

	// Optional: account_id
	FString AccountId;
	if ((*TokensObj)->TryGetStringField(TEXT("account_id"), AccountId))
	{
		Result.AccountId = AccountId;
	}

	return Result;
}

// ============================================================================
// HTTP Fetching
// ============================================================================

void FAgentUsageMonitor::FetchClaudeUsage(const FString& AccessToken)
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(TEXT("https://api.anthropic.com/api/oauth/usage"));
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AccessToken));
	Request->SetHeader(TEXT("anthropic-beta"), TEXT("oauth-2025-04-20"));
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->SetHeader(TEXT("User-Agent"), TEXT("AgentIntegrationKit"));
	Request->OnProcessRequestComplete().BindRaw(this, &FAgentUsageMonitor::HandleClaudeResponse);
	Request->ProcessRequest();
}

void FAgentUsageMonitor::FetchCodexUsage(const FString& AccessToken, const FString& AccountId)
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(TEXT("https://chatgpt.com/backend-api/wham/usage"));
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AccessToken));
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->SetHeader(TEXT("User-Agent"), TEXT("codex-cli"));

	if (!AccountId.IsEmpty())
	{
		Request->SetHeader(TEXT("ChatGPT-Account-Id"), AccountId);
	}

	Request->OnProcessRequestComplete().BindRaw(this, &FAgentUsageMonitor::HandleCodexResponse);
	Request->ProcessRequest();
}

void FAgentUsageMonitor::HandleClaudeResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess)
{
	bFetchInFlight = false;
	const FString AgentName = TEXT("Claude Code");

	FAgentRateLimitData& Data = CachedData.FindOrAdd(AgentName);
	Data.bIsLoading = false;
	Data.AgentName = AgentName;

	if (!bSuccess || !Response.IsValid())
	{
		Data.ErrorMessage = TEXT("Unable to reach Anthropic API. Check your network connection.");
		OnUsageDataUpdated.Broadcast(AgentName, Data);
		return;
	}

	int32 ResponseCode = Response->GetResponseCode();
	if (ResponseCode == 401 || ResponseCode == 403)
	{
		Data.ErrorMessage = TEXT("Authentication failed. Run 'claude' in terminal to refresh your token.");
		OnUsageDataUpdated.Broadcast(AgentName, Data);
		return;
	}

	if (ResponseCode != 200)
	{
		Data.ErrorMessage = FString::Printf(TEXT("Anthropic API returned HTTP %d. Retrying..."), ResponseCode);
		OnUsageDataUpdated.Broadcast(AgentName, Data);
		return;
	}

	FString ResponseBody = Response->GetContentAsString();
	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
	if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
	{
		Data.ErrorMessage = TEXT("Invalid response from Anthropic API.");
		OnUsageDataUpdated.Broadcast(AgentName, Data);
		return;
	}

	Data = ParseClaudeUsageResponse(JsonObj);
	UE_LOG(LogAgentIntegrationKit, Log, TEXT("[UsageMonitor] Claude usage: 5h=%.0f%%, 7d=%.0f%%, %s=%.0f%%, plan=%s"),
		Data.Primary.UsedPercent, Data.Secondary.UsedPercent,
		Data.ModelSpecificLabel.IsEmpty() ? TEXT("model") : *Data.ModelSpecificLabel,
		Data.ModelSpecific.UsedPercent, *Data.PlanType);
	OnUsageDataUpdated.Broadcast(AgentName, Data);
}

void FAgentUsageMonitor::HandleCodexResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess)
{
	bFetchInFlight = false;
	const FString AgentName = TEXT("Codex CLI");

	FAgentRateLimitData& Data = CachedData.FindOrAdd(AgentName);
	Data.bIsLoading = false;
	Data.AgentName = AgentName;

	if (!bSuccess || !Response.IsValid())
	{
		Data.ErrorMessage = TEXT("Unable to reach OpenAI API. Check your network connection.");
		OnUsageDataUpdated.Broadcast(AgentName, Data);
		return;
	}

	int32 ResponseCode = Response->GetResponseCode();
	if (ResponseCode == 401 || ResponseCode == 403)
	{
		Data.ErrorMessage = TEXT("Authentication failed. Run 'codex' in terminal to refresh your token.");
		OnUsageDataUpdated.Broadcast(AgentName, Data);
		return;
	}

	if (ResponseCode != 200)
	{
		Data.ErrorMessage = FString::Printf(TEXT("OpenAI API returned HTTP %d. Retrying..."), ResponseCode);
		OnUsageDataUpdated.Broadcast(AgentName, Data);
		return;
	}

	FString ResponseBody = Response->GetContentAsString();
	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
	if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
	{
		Data.ErrorMessage = TEXT("Invalid response from OpenAI API.");
		OnUsageDataUpdated.Broadcast(AgentName, Data);
		return;
	}

	Data = ParseCodexUsageResponse(JsonObj);
	OnUsageDataUpdated.Broadcast(AgentName, Data);
}

// ============================================================================
// Response Parsing
// ============================================================================

FAgentRateLimitData FAgentUsageMonitor::ParseClaudeUsageResponse(const TSharedPtr<FJsonObject>& Json) const
{
	FAgentRateLimitData Data;
	Data.bHasData = true;
	Data.AgentName = TEXT("Claude Code");
	Data.LastUpdated = FDateTime::UtcNow();

	// Helper: parse a usage window object (utilization + resets_at)
	auto ParseWindow = [](const TSharedPtr<FJsonObject>& WindowObj, int32 DurationMinutes) -> FAgentRateLimitWindow
	{
		FAgentRateLimitWindow Window;
		Window.WindowDurationMinutes = DurationMinutes;

		double Utilization = 0.0;
		if (WindowObj->TryGetNumberField(TEXT("utilization"), Utilization))
		{
			Window.UsedPercent = Utilization; // Already 0-100 percentage
		}

		FString ResetsAtStr;
		if (WindowObj->TryGetStringField(TEXT("resets_at"), ResetsAtStr) && !ResetsAtStr.IsEmpty())
		{
			FDateTime ParsedTime;
			if (FDateTime::ParseIso8601(*ResetsAtStr, ParsedTime))
			{
				Window.ResetsAt = ParsedTime;
			}
		}

		return Window;
	};

	// Parse five_hour (session) window
	const TSharedPtr<FJsonObject>* FiveHourObj = nullptr;
	if (Json->TryGetObjectField(TEXT("five_hour"), FiveHourObj) && FiveHourObj && (*FiveHourObj).IsValid())
	{
		Data.Primary = ParseWindow(*FiveHourObj, 300);
	}

	// Parse seven_day (all models weekly) window
	const TSharedPtr<FJsonObject>* SevenDayObj = nullptr;
	if (Json->TryGetObjectField(TEXT("seven_day"), SevenDayObj) && SevenDayObj && (*SevenDayObj).IsValid())
	{
		Data.Secondary = ParseWindow(*SevenDayObj, 10080);
	}

	// Parse model-specific weekly windows (Sonnet preferred over Opus, matching CodexBar)
	const TSharedPtr<FJsonObject>* SonnetObj = nullptr;
	const TSharedPtr<FJsonObject>* OpusObj = nullptr;
	if (Json->TryGetObjectField(TEXT("seven_day_sonnet"), SonnetObj) && SonnetObj && (*SonnetObj).IsValid())
	{
		Data.ModelSpecific = ParseWindow(*SonnetObj, 10080);
		Data.ModelSpecificLabel = TEXT("Sonnet");
	}
	else if (Json->TryGetObjectField(TEXT("seven_day_opus"), OpusObj) && OpusObj && (*OpusObj).IsValid())
	{
		Data.ModelSpecific = ParseWindow(*OpusObj, 10080);
		Data.ModelSpecificLabel = TEXT("Opus");
	}

	// Parse extra_usage (Claude Extra cost tracking)
	const TSharedPtr<FJsonObject>* ExtraObj = nullptr;
	if (Json->TryGetObjectField(TEXT("extra_usage"), ExtraObj) && ExtraObj && (*ExtraObj).IsValid())
	{
		bool bEnabled = false;
		if ((*ExtraObj)->TryGetBoolField(TEXT("is_enabled"), bEnabled))
		{
			Data.ExtraUsage.bIsEnabled = bEnabled;
		}

		// API returns amounts in cents (minor units) — convert to dollars
		double MonthlyLimit = 0.0;
		if ((*ExtraObj)->TryGetNumberField(TEXT("monthly_limit"), MonthlyLimit))
		{
			Data.ExtraUsage.LimitAmount = MonthlyLimit / 100.0;
		}

		double UsedCredits = 0.0;
		if ((*ExtraObj)->TryGetNumberField(TEXT("used_credits"), UsedCredits))
		{
			Data.ExtraUsage.UsedAmount = UsedCredits / 100.0;
		}

		FString Currency;
		if ((*ExtraObj)->TryGetStringField(TEXT("currency"), Currency) && !Currency.IsEmpty())
		{
			Data.ExtraUsage.CurrencyCode = Currency;
		}
	}

	// Infer plan type from cached rateLimitTier credential field
	Data.PlanType = InferPlanFromTier(CachedClaudeRateLimitTier);

	return Data;
}

FString FAgentUsageMonitor::InferPlanFromTier(const FString& Tier)
{
	if (Tier.IsEmpty())
	{
		return TEXT("Pro");
	}

	FString Lower = Tier.ToLower();
	if (Lower.Contains(TEXT("max")))
	{
		return TEXT("Max");
	}
	if (Lower.Contains(TEXT("pro")))
	{
		return TEXT("Pro");
	}
	if (Lower.Contains(TEXT("team")))
	{
		return TEXT("Team");
	}
	if (Lower.Contains(TEXT("enterprise")))
	{
		return TEXT("Enterprise");
	}

	return TEXT("Pro");
}

FAgentRateLimitData FAgentUsageMonitor::ParseCodexUsageResponse(const TSharedPtr<FJsonObject>& Json) const
{
	FAgentRateLimitData Data;
	Data.bHasData = true;
	Data.AgentName = TEXT("Codex CLI");
	Data.LastUpdated = FDateTime::UtcNow();

	// Plan type
	FString PlanType;
	if (Json->TryGetStringField(TEXT("plan_type"), PlanType))
	{
		// Capitalize first letter for display
		if (!PlanType.IsEmpty())
		{
			Data.PlanType = PlanType.Left(1).ToUpper() + PlanType.Mid(1);
		}
	}

	// Parse rate_limit object
	const TSharedPtr<FJsonObject>* RateLimitObj = nullptr;
	if (Json->TryGetObjectField(TEXT("rate_limit"), RateLimitObj) && RateLimitObj && (*RateLimitObj).IsValid())
	{
		// Primary window
		const TSharedPtr<FJsonObject>* PrimaryObj = nullptr;
		if ((*RateLimitObj)->TryGetObjectField(TEXT("primary_window"), PrimaryObj) && PrimaryObj && (*PrimaryObj).IsValid())
		{
			double UsedPercent = 0.0;
			if ((*PrimaryObj)->TryGetNumberField(TEXT("used_percent"), UsedPercent))
			{
				Data.Primary.UsedPercent = UsedPercent;
			}

			double ResetAt = 0.0;
			if ((*PrimaryObj)->TryGetNumberField(TEXT("reset_at"), ResetAt) && ResetAt > 0.0)
			{
				Data.Primary.ResetsAt = FDateTime::FromUnixTimestamp(static_cast<int64>(ResetAt));
			}

			double WindowSeconds = 0.0;
			if ((*PrimaryObj)->TryGetNumberField(TEXT("limit_window_seconds"), WindowSeconds) && WindowSeconds > 0.0)
			{
				Data.Primary.WindowDurationMinutes = static_cast<int32>(WindowSeconds / 60.0);
			}
		}

		// Secondary window
		const TSharedPtr<FJsonObject>* SecondaryObj = nullptr;
		if ((*RateLimitObj)->TryGetObjectField(TEXT("secondary_window"), SecondaryObj) && SecondaryObj && (*SecondaryObj).IsValid())
		{
			double UsedPercent = 0.0;
			if ((*SecondaryObj)->TryGetNumberField(TEXT("used_percent"), UsedPercent))
			{
				Data.Secondary.UsedPercent = UsedPercent;
			}

			double ResetAt = 0.0;
			if ((*SecondaryObj)->TryGetNumberField(TEXT("reset_at"), ResetAt) && ResetAt > 0.0)
			{
				Data.Secondary.ResetsAt = FDateTime::FromUnixTimestamp(static_cast<int64>(ResetAt));
			}

			double WindowSeconds = 0.0;
			if ((*SecondaryObj)->TryGetNumberField(TEXT("limit_window_seconds"), WindowSeconds) && WindowSeconds > 0.0)
			{
				Data.Secondary.WindowDurationMinutes = static_cast<int32>(WindowSeconds / 60.0);
			}
		}
	}

	return Data;
}

// ============================================================================
// Meshy Balance
// ============================================================================

void FAgentUsageMonitor::RequestMeshyBalanceUpdate()
{
	if (bMeshyFetchInFlight)
	{
		return;
	}

	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings || !Settings->HasMeshyAuth())
	{
		return;
	}

	FetchMeshyBalance();
}

void FAgentUsageMonitor::FetchMeshyBalance()
{
	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings || !Settings->HasMeshyAuth())
	{
		return;
	}

	bMeshyFetchInFlight = true;
	LastMeshyFetchTimeSeconds = FPlatformTime::Seconds();

	const FString MeshyToken = Settings->GetMeshyAuthToken();
	const FString BalanceUrl = Settings->GetMeshyBaseUrl() + TEXT("/openapi/v1/balance");

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(BalanceUrl);
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *MeshyToken));
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->OnProcessRequestComplete().BindRaw(this, &FAgentUsageMonitor::HandleMeshyBalanceResponse);
	Request->ProcessRequest();
}

void FAgentUsageMonitor::HandleMeshyBalanceResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess)
{
	bMeshyFetchInFlight = false;

	if (!bSuccess || !Response.IsValid())
	{
		MeshyBalanceError = TEXT("Unable to reach Meshy API.");
		OnMeshyBalanceUpdated.Broadcast(false, CachedMeshyBalance);
		return;
	}

	int32 ResponseCode = Response->GetResponseCode();
	if (ResponseCode == 401 || ResponseCode == 403)
	{
		MeshyBalanceError = TEXT("Invalid credentials.");
		OnMeshyBalanceUpdated.Broadcast(false, CachedMeshyBalance);
		return;
	}

	if (ResponseCode != 200)
	{
		MeshyBalanceError = FString::Printf(TEXT("HTTP %d"), ResponseCode);
		OnMeshyBalanceUpdated.Broadcast(false, CachedMeshyBalance);
		return;
	}

	FString ResponseBody = Response->GetContentAsString();
	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
	if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
	{
		MeshyBalanceError = TEXT("Invalid response format.");
		OnMeshyBalanceUpdated.Broadcast(false, CachedMeshyBalance);
		return;
	}

	int32 Balance = 0;
	if (JsonObj->TryGetNumberField(TEXT("balance"), Balance))
	{
		CachedMeshyBalance = Balance;
		MeshyBalanceError.Empty();
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("[UsageMonitor] Meshy balance: %d credits"), Balance);
		OnMeshyBalanceUpdated.Broadcast(true, Balance);
	}
	else
	{
		MeshyBalanceError = TEXT("Unexpected response format.");
		OnMeshyBalanceUpdated.Broadcast(false, CachedMeshyBalance);
	}
}
