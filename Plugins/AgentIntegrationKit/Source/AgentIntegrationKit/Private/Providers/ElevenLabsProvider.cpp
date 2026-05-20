// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Providers/GenerativeProvider.h"
#include "Providers/GenerativeProviderRegistry.h"
#include "Interfaces/IHttpRequest.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Misc/Base64.h"
#include "HAL/FileManager.h"

// ── Helpers ──────────────────────────────────────────────────────────
namespace ElevenLabsHelpers {
inline FString GetStr(const TSharedPtr<FJsonObject>& J, const FString& Key)
{
	FString V;
	if (J.IsValid()) J->TryGetStringField(Key, V);
	return V;
}
inline int32 GetInt(const TSharedPtr<FJsonObject>& J, const FString& Key, int32 Default = 0)
{
	int32 V = Default;
	if (J.IsValid()) J->TryGetNumberField(Key, V);
	return V;
}
inline double GetDouble(const TSharedPtr<FJsonObject>& J, const FString& Key, double Default = 0.0)
{
	double V = Default;
	if (J.IsValid()) J->TryGetNumberField(Key, V);
	return V;
}
inline bool GetBool(const TSharedPtr<FJsonObject>& J, const FString& Key, bool Default = false)
{
	bool V = Default;
	if (J.IsValid()) J->TryGetBoolField(Key, V);
	return V;
}
inline void SetIfNotEmpty(const TSharedPtr<FJsonObject>& Body, const FString& Key, const FString& Value)
{
	if (!Value.IsEmpty()) Body->SetStringField(Key, Value);
}
inline FString SaveToTempFile(const TArray<uint8>& Data, const FString& Extension)
{
	const FString TempDir = FPaths::ProjectSavedDir() / TEXT("Temp") / TEXT("ElevenLabs");
	IFileManager::Get().MakeDirectory(*TempDir, true);
	const FString TempFile = TempDir / FString::Printf(TEXT("gen_%s.%s"),
		*FGuid::NewGuid().ToString().Left(12), *Extension);
	FFileHelper::SaveArrayToFile(Data, *TempFile);
	return TempFile;
}
} // namespace ElevenLabsHelpers

// Determine file extension from content type
static FString ExtensionFromContentType(const FString& ContentType, const FString& RequestedFormat)
{
	if (ContentType.Contains(TEXT("wav"))) return TEXT("wav");
	if (ContentType.Contains(TEXT("ogg"))) return TEXT("ogg");
	if (ContentType.Contains(TEXT("opus"))) return TEXT("opus");
	if (ContentType.Contains(TEXT("pcm"))) return TEXT("pcm");
	if (ContentType.Contains(TEXT("flac"))) return TEXT("flac");
	// Try from requested format
	if (RequestedFormat.StartsWith(TEXT("wav"))) return TEXT("wav");
	if (RequestedFormat.StartsWith(TEXT("pcm"))) return TEXT("pcm");
	if (RequestedFormat.StartsWith(TEXT("opus"))) return TEXT("opus");
	if (RequestedFormat.StartsWith(TEXT("ulaw"))) return TEXT("ulaw");
	if (RequestedFormat.StartsWith(TEXT("alaw"))) return TEXT("alaw");
	return TEXT("mp3"); // default
}

// ═════════════════════════════════════════════════════════════════════
// ElevenLabsProvider — TTS, Sound Effects, Music, STT
// ═════════════════════════════════════════════════════════════════════

class FElevenLabsProvider : public FGenerativeProviderBase
{
public:
	FString GetId() const override { return TEXT("elevenlabs"); }
	FString GetDisplayName() const override { return TEXT("ElevenLabs"); }
	FString GetWebsite() const override { return TEXT("https://elevenlabs.io"); }
	FString GetDirectBaseUrl() const override { return TEXT("https://api.elevenlabs.io"); }
	FString GetProxyPath() const override { return TEXT("/api/proxy/elevenlabs"); }
	FString GetApiKeySettingName() const override { return TEXT("ElevenLabsApiKey"); }

	// ElevenLabs uses xi-api-key header instead of Bearer token
	void SetAuthHeaders(const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& Request) const override
	{
		Request->SetHeader(TEXT("xi-api-key"), GetAuthToken());
	}

	// ── Actions ──────────────────────────────────────────────────────

	TArray<FProviderActionDescriptor> GetActions() const override
	{
		return {
			// Text to Speech
			{TEXT("text_to_speech"), TEXT("Convert text to lifelike speech audio. Requires voice_id and text. "
				"Models: eleven_v3 (most expressive, 70+ languages), eleven_multilingual_v2 (stable, 29 languages), "
				"eleven_flash_v2_5 (fastest, ~75ms latency). Returns audio file path. "
				"Voice settings: stability (0-1), similarity_boost (0-1), style (0-1), speed (0.7-1.2)."),
				{TEXT("text")}, {TEXT("audio")}, BuildTTSSchema(), TEXT("1 credit/char"), true},

			{TEXT("text_to_speech_with_timestamps"), TEXT("Convert text to speech with character-level timing. "
				"Returns base64 audio + alignment data (character start/end times). "
				"Same voice/model options as text_to_speech."),
				{TEXT("text")}, {TEXT("audio"), TEXT("data")}, BuildTTSTimestampsSchema(), TEXT("1 credit/char"), true},

			// Sound Effects
			{TEXT("sound_effects"), TEXT("Generate sound effects from text descriptions. "
				"Examples: 'glass shattering', 'thunder rumbling', '90s hip-hop drum loop'. "
				"Options: duration_seconds (0.1-30, auto if unset), prompt_influence (0-1). Returns audio file."),
				{TEXT("text")}, {TEXT("audio")}, BuildSFXSchema(), TEXT("40 credits/sec"), true},

			// Music
			{TEXT("music"), TEXT("Generate studio-grade music from text prompts. "
				"Options: duration_seconds (3-300), instrumental (bool), prompt. Returns audio file."),
				{TEXT("text")}, {TEXT("audio")}, BuildMusicSchema(), TEXT("varies")},

			// Voices
			{TEXT("list_voices"), TEXT("List available voices. Returns array of voice objects with "
				"voice_id, name, category, labels, and preview_url. Use voice_id in TTS calls."),
				{}, {TEXT("data")}, nullptr, TEXT("free"), true},

			{TEXT("list_models"), TEXT("List available TTS models. Returns model_id, name, description, "
				"and supported languages."),
				{}, {TEXT("data")}, nullptr, TEXT("free"), true},
		};
	}

	// ── Submit ───────────────────────────────────────────────────────

	FGenerativeJob Submit(const FString& ActionId, const TSharedPtr<FJsonObject>& Params) override
	{
		if (ActionId == TEXT("text_to_speech"))           return SubmitTTS(Params);
		if (ActionId == TEXT("text_to_speech_with_timestamps")) return SubmitTTSTimestamps(Params);
		if (ActionId == TEXT("sound_effects"))            return SubmitSFX(Params);
		if (ActionId == TEXT("music"))                    return SubmitMusic(Params);
		if (ActionId == TEXT("list_voices"))              return SubmitListVoices();
		if (ActionId == TEXT("list_models"))              return SubmitListModels();
		return FGenerativeJob::MakeFail(FString::Printf(TEXT("Unknown ElevenLabs action: %s"), *ActionId));
	}

	// ElevenLabs actions are synchronous — no polling needed
	FGenerativeJob CheckStatus(const FString& JobId, const FString& ActionId) override
	{
		// All ElevenLabs actions are synchronous
		FGenerativeJob Job;
		Job.JobId = JobId;
		Job.Status = EGenerativeJobStatus::Succeeded;
		Job.Progress = 100;
		Job.ProviderId = TEXT("elevenlabs");
		return Job;
	}

private:

	// ── Text to Speech ──────────────────────────────────────────────

	FGenerativeJob SubmitTTS(const TSharedPtr<FJsonObject>& Params)
	{
		const FString Text = ElevenLabsHelpers::GetStr(Params, TEXT("text"));
		const FString VoiceId = ElevenLabsHelpers::GetStr(Params, TEXT("voice_id"));
		if (Text.IsEmpty())
			return FGenerativeJob::MakeFail(TEXT("text_to_speech requires 'text'"));
		if (VoiceId.IsEmpty())
			return FGenerativeJob::MakeFail(TEXT("text_to_speech requires 'voice_id'. Use list_voices to find available voices."));

		const FString OutputFormat = ElevenLabsHelpers::GetStr(Params, TEXT("output_format"));

		// Build URL with query params
		FString Path = FString::Printf(TEXT("/v1/text-to-speech/%s"), *VoiceId);
		if (!OutputFormat.IsEmpty())
			Path += FString::Printf(TEXT("?output_format=%s"), *OutputFormat);

		// Build body
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("text"), Text);
		ElevenLabsHelpers::SetIfNotEmpty(Body, TEXT("model_id"), ElevenLabsHelpers::GetStr(Params, TEXT("model_id")));
		ElevenLabsHelpers::SetIfNotEmpty(Body, TEXT("language_code"), ElevenLabsHelpers::GetStr(Params, TEXT("language_code")));

		if (Params->HasField(TEXT("seed")))
			Body->SetNumberField(TEXT("seed"), ElevenLabsHelpers::GetInt(Params, TEXT("seed")));

		// Voice settings
		if (Params->HasField(TEXT("stability")) || Params->HasField(TEXT("similarity_boost"))
			|| Params->HasField(TEXT("style")) || Params->HasField(TEXT("speed")))
		{
			TSharedPtr<FJsonObject> VoiceSettings = MakeShared<FJsonObject>();
			if (Params->HasField(TEXT("stability")))
				VoiceSettings->SetNumberField(TEXT("stability"), ElevenLabsHelpers::GetDouble(Params, TEXT("stability"), 0.5));
			if (Params->HasField(TEXT("similarity_boost")))
				VoiceSettings->SetNumberField(TEXT("similarity_boost"), ElevenLabsHelpers::GetDouble(Params, TEXT("similarity_boost"), 0.75));
			if (Params->HasField(TEXT("style")))
				VoiceSettings->SetNumberField(TEXT("style"), ElevenLabsHelpers::GetDouble(Params, TEXT("style"), 0.0));
			if (Params->HasField(TEXT("speed")))
				VoiceSettings->SetNumberField(TEXT("speed"), ElevenLabsHelpers::GetDouble(Params, TEXT("speed"), 1.0));
			if (Params->HasField(TEXT("use_speaker_boost")))
				VoiceSettings->SetBoolField(TEXT("use_speaker_boost"), ElevenLabsHelpers::GetBool(Params, TEXT("use_speaker_boost")));
			Body->SetObjectField(TEXT("voice_settings"), VoiceSettings);
		}

		ElevenLabsHelpers::SetIfNotEmpty(Body, TEXT("previous_text"), ElevenLabsHelpers::GetStr(Params, TEXT("previous_text")));
		ElevenLabsHelpers::SetIfNotEmpty(Body, TEXT("next_text"), ElevenLabsHelpers::GetStr(Params, TEXT("next_text")));

		// Make request — returns raw audio bytes
		FString Error;
		FString ContentType;
		TArray<uint8> AudioData = HttpPostRaw(Path, Body, Error, ContentType, 120.0f);
		if (AudioData.Num() == 0)
			return FGenerativeJob::MakeFail(FString::Printf(TEXT("TTS failed: %s"), *Error));

		// Save to temp file
		const FString Ext = ExtensionFromContentType(ContentType, OutputFormat);
		const FString FilePath = ElevenLabsHelpers::SaveToTempFile(AudioData, Ext);

		FGenerativeJob Job;
		Job.ProviderId = TEXT("elevenlabs");
		Job.ActionId = TEXT("text_to_speech");
		Job.Status = EGenerativeJobStatus::Succeeded;
		Job.Progress = 100;
		Job.ResultUrl = FilePath;
		Job.JobId = FGuid::NewGuid().ToString();
		return Job;
	}

	// ── Text to Speech with Timestamps ──────────────────────────────

	FGenerativeJob SubmitTTSTimestamps(const TSharedPtr<FJsonObject>& Params)
	{
		const FString Text = ElevenLabsHelpers::GetStr(Params, TEXT("text"));
		const FString VoiceId = ElevenLabsHelpers::GetStr(Params, TEXT("voice_id"));
		if (Text.IsEmpty())
			return FGenerativeJob::MakeFail(TEXT("text_to_speech_with_timestamps requires 'text'"));
		if (VoiceId.IsEmpty())
			return FGenerativeJob::MakeFail(TEXT("text_to_speech_with_timestamps requires 'voice_id'"));

		const FString OutputFormat = ElevenLabsHelpers::GetStr(Params, TEXT("output_format"));

		FString Path = FString::Printf(TEXT("/v1/text-to-speech/%s/with-timestamps"), *VoiceId);
		if (!OutputFormat.IsEmpty())
			Path += FString::Printf(TEXT("?output_format=%s"), *OutputFormat);

		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("text"), Text);
		ElevenLabsHelpers::SetIfNotEmpty(Body, TEXT("model_id"), ElevenLabsHelpers::GetStr(Params, TEXT("model_id")));
		ElevenLabsHelpers::SetIfNotEmpty(Body, TEXT("language_code"), ElevenLabsHelpers::GetStr(Params, TEXT("language_code")));

		if (Params->HasField(TEXT("seed")))
			Body->SetNumberField(TEXT("seed"), ElevenLabsHelpers::GetInt(Params, TEXT("seed")));

		// This endpoint returns JSON (not raw audio)
		TSharedPtr<FJsonObject> Json = HttpPost(Path, Body, 120.0f);
		if (!Json.IsValid())
			return FGenerativeJob::MakeFail(TEXT("TTS with timestamps failed"));

		// Decode base64 audio and save
		const FString AudioBase64 = ElevenLabsHelpers::GetStr(Json, TEXT("audio_base64"));
		FString FilePath;
		if (!AudioBase64.IsEmpty())
		{
			TArray<uint8> AudioData;
			FBase64::Decode(AudioBase64, AudioData);
			const FString Ext = ExtensionFromContentType(TEXT(""), OutputFormat);
			FilePath = ElevenLabsHelpers::SaveToTempFile(AudioData, Ext);
		}

		FGenerativeJob Job;
		Job.ProviderId = TEXT("elevenlabs");
		Job.ActionId = TEXT("text_to_speech_with_timestamps");
		Job.Status = EGenerativeJobStatus::Succeeded;
		Job.Progress = 100;
		Job.ResultUrl = FilePath;
		Job.RawResponse = Json; // Contains alignment data
		Job.JobId = FGuid::NewGuid().ToString();
		return Job;
	}

	// ── Sound Effects ───────────────────────────────────────────────

	FGenerativeJob SubmitSFX(const TSharedPtr<FJsonObject>& Params)
	{
		const FString Text = ElevenLabsHelpers::GetStr(Params, TEXT("text"));
		if (Text.IsEmpty())
			return FGenerativeJob::MakeFail(TEXT("sound_effects requires 'text' (description of the sound)"));

		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("text"), Text);

		if (Params->HasField(TEXT("duration_seconds")))
			Body->SetNumberField(TEXT("duration_seconds"), ElevenLabsHelpers::GetDouble(Params, TEXT("duration_seconds")));
		if (Params->HasField(TEXT("prompt_influence")))
			Body->SetNumberField(TEXT("prompt_influence"), ElevenLabsHelpers::GetDouble(Params, TEXT("prompt_influence"), 0.3));

		const FString OutputFormat = ElevenLabsHelpers::GetStr(Params, TEXT("output_format"));

		FString Path = TEXT("/v1/sound-generation");
		// SFX returns raw audio
		FString Error;
		FString ContentType;
		TArray<uint8> AudioData = HttpPostRaw(Path, Body, Error, ContentType, 120.0f);
		if (AudioData.Num() == 0)
			return FGenerativeJob::MakeFail(FString::Printf(TEXT("Sound effects failed: %s"), *Error));

		const FString Ext = ExtensionFromContentType(ContentType, OutputFormat);
		const FString FilePath = ElevenLabsHelpers::SaveToTempFile(AudioData, Ext);

		FGenerativeJob Job;
		Job.ProviderId = TEXT("elevenlabs");
		Job.ActionId = TEXT("sound_effects");
		Job.Status = EGenerativeJobStatus::Succeeded;
		Job.Progress = 100;
		Job.ResultUrl = FilePath;
		Job.JobId = FGuid::NewGuid().ToString();
		return Job;
	}

	// ── Music ───────────────────────────────────────────────────────

	FGenerativeJob SubmitMusic(const TSharedPtr<FJsonObject>& Params)
	{
		const FString Prompt = ElevenLabsHelpers::GetStr(Params, TEXT("prompt"));
		if (Prompt.IsEmpty())
			return FGenerativeJob::MakeFail(TEXT("music requires 'prompt'"));

		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("prompt"), Prompt);

		if (Params->HasField(TEXT("duration_seconds")))
			Body->SetNumberField(TEXT("duration_seconds"), ElevenLabsHelpers::GetDouble(Params, TEXT("duration_seconds")));
		if (Params->HasField(TEXT("instrumental")))
			Body->SetBoolField(TEXT("instrumental"), ElevenLabsHelpers::GetBool(Params, TEXT("instrumental")));
		ElevenLabsHelpers::SetIfNotEmpty(Body, TEXT("lyrics"), ElevenLabsHelpers::GetStr(Params, TEXT("lyrics")));

		// Music generation returns raw audio
		FString Error;
		FString ContentType;
		TArray<uint8> AudioData = HttpPostRaw(TEXT("/v1/music"), Body, Error, ContentType, 300.0f);
		if (AudioData.Num() == 0)
			return FGenerativeJob::MakeFail(FString::Printf(TEXT("Music generation failed: %s"), *Error));

		const FString FilePath = ElevenLabsHelpers::SaveToTempFile(AudioData, TEXT("mp3"));

		FGenerativeJob Job;
		Job.ProviderId = TEXT("elevenlabs");
		Job.ActionId = TEXT("music");
		Job.Status = EGenerativeJobStatus::Succeeded;
		Job.Progress = 100;
		Job.ResultUrl = FilePath;
		Job.JobId = FGuid::NewGuid().ToString();
		return Job;
	}

	// ── List Voices ─────────────────────────────────────────────────

	FGenerativeJob SubmitListVoices()
	{
		TSharedPtr<FJsonObject> Json = HttpGet(TEXT("/v1/voices"));
		if (!Json.IsValid())
			return FGenerativeJob::MakeFail(TEXT("Failed to list voices"));

		FGenerativeJob Job;
		Job.ProviderId = TEXT("elevenlabs");
		Job.ActionId = TEXT("list_voices");
		Job.Status = EGenerativeJobStatus::Succeeded;
		Job.Progress = 100;
		Job.RawResponse = Json;
		Job.JobId = FGuid::NewGuid().ToString();
		return Job;
	}

	// ── List Models ─────────────────────────────────────────────────

	FGenerativeJob SubmitListModels()
	{
		TSharedPtr<FJsonObject> Json = HttpGet(TEXT("/v1/models"));
		if (!Json.IsValid())
			return FGenerativeJob::MakeFail(TEXT("Failed to list models"));

		FGenerativeJob Job;
		Job.ProviderId = TEXT("elevenlabs");
		Job.ActionId = TEXT("list_models");
		Job.Status = EGenerativeJobStatus::Succeeded;
		Job.Progress = 100;
		Job.RawResponse = Json;
		Job.JobId = FGuid::NewGuid().ToString();
		return Job;
	}

	// ── Schema builders ──────────────────────────────────────────────

	static TSharedPtr<FJsonObject> BuildTTSSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("text"), SchemaString(TEXT("Text to convert to speech (max 5000 chars for v3, 40000 for flash)")));
		Props.Add(TEXT("voice_id"), SchemaString(TEXT("Voice ID. Use list_voices to find available voices.")));
		Props.Add(TEXT("model_id"), SchemaString(TEXT("TTS model"),
			{TEXT("eleven_v3"), TEXT("eleven_multilingual_v2"), TEXT("eleven_flash_v2_5"),
			 TEXT("eleven_flash_v2")}, TEXT("eleven_multilingual_v2")));
		Props.Add(TEXT("language_code"), SchemaString(TEXT("ISO 639-1 language code (e.g. 'en', 'ja', 'de')")));
		Props.Add(TEXT("output_format"), SchemaString(TEXT("Audio format: codec_samplerate_bitrate"),
			{TEXT("mp3_44100_128"), TEXT("mp3_44100_192"), TEXT("mp3_22050_32"),
			 TEXT("pcm_16000"), TEXT("pcm_22050"), TEXT("pcm_24000"), TEXT("pcm_44100"),
			 TEXT("wav_44100"), TEXT("wav_22050"),
			 TEXT("opus_48000_128"), TEXT("ulaw_8000"), TEXT("alaw_8000")}, TEXT("mp3_44100_128")));
		Props.Add(TEXT("stability"), SchemaInt(TEXT("Voice stability 0-100 (mapped to 0.0-1.0). Lower = more expressive."), 0, 100, 50));
		Props.Add(TEXT("similarity_boost"), SchemaInt(TEXT("Voice similarity 0-100. Higher = closer to original."), 0, 100, 75));
		Props.Add(TEXT("style"), SchemaInt(TEXT("Style exaggeration 0-100. Higher = more stylized. Increases latency."), 0, 100, 0));
		Props.Add(TEXT("speed"), SchemaInt(TEXT("Speech speed. 70-120 (mapped to 0.7-1.2). 100 = default."), 70, 120, 100));
		Props.Add(TEXT("use_speaker_boost"), SchemaBool(TEXT("Boost similarity to original speaker")));
		Props.Add(TEXT("seed"), SchemaInt(TEXT("Random seed for deterministic output (0-4294967295)")));
		Props.Add(TEXT("previous_text"), SchemaString(TEXT("Context text before current text for continuity")));
		Props.Add(TEXT("next_text"), SchemaString(TEXT("Context text after current text for continuity")));
		return BuildSchema(Props, {TEXT("text"), TEXT("voice_id")});
	}

	static TSharedPtr<FJsonObject> BuildTTSTimestampsSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("text"), SchemaString(TEXT("Text to convert to speech")));
		Props.Add(TEXT("voice_id"), SchemaString(TEXT("Voice ID")));
		Props.Add(TEXT("model_id"), SchemaString(TEXT("TTS model"),
			{TEXT("eleven_v3"), TEXT("eleven_multilingual_v2"), TEXT("eleven_flash_v2_5")}, TEXT("eleven_multilingual_v2")));
		Props.Add(TEXT("language_code"), SchemaString(TEXT("ISO 639-1 language code")));
		Props.Add(TEXT("output_format"), SchemaString(TEXT("Audio format"), {TEXT("mp3_44100_128"), TEXT("pcm_24000")}, TEXT("mp3_44100_128")));
		Props.Add(TEXT("seed"), SchemaInt(TEXT("Random seed for deterministic output")));
		return BuildSchema(Props, {TEXT("text"), TEXT("voice_id")});
	}

	static TSharedPtr<FJsonObject> BuildSFXSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("text"), SchemaString(TEXT("Description of the sound effect (e.g. 'glass shattering on concrete', '90s drum loop 90 BPM')")));
		Props.Add(TEXT("duration_seconds"), SchemaInt(TEXT("Duration in seconds (0.1-30). Auto if unset."), 0, 30));
		Props.Add(TEXT("prompt_influence"), SchemaInt(TEXT("How strictly to follow prompt 0-100 (mapped to 0.0-1.0). Higher = more literal."), 0, 100, 30));
		return BuildSchema(Props, {TEXT("text")});
	}

	static TSharedPtr<FJsonObject> BuildMusicSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("prompt"), SchemaString(TEXT("Describe the music: genre, mood, tempo, instruments, use case")));
		Props.Add(TEXT("duration_seconds"), SchemaInt(TEXT("Duration in seconds (3-300)"), 3, 300));
		Props.Add(TEXT("instrumental"), SchemaBool(TEXT("Generate without vocals")));
		Props.Add(TEXT("lyrics"), SchemaString(TEXT("Custom lyrics for the AI to sing")));
		return BuildSchema(Props, {TEXT("prompt")});
	}
};

// ── Auto-register ────────────────────────────────────────────────────

REGISTER_GENERATIVE_PROVIDER(FElevenLabsProvider);
