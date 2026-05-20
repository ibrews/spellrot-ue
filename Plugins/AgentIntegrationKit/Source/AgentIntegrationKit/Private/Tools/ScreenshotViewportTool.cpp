// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Tools/ScreenshotViewportTool.h"
#include "AgentIntegrationKitModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/Base64.h"

// Level viewport
#include "Editor.h"
#include "LevelEditorViewport.h"
#include "UnrealClient.h"
#include "ImageUtils.h"
#include "IAssetViewport.h"

// Asset editor viewport finding
#include "Subsystems/AssetEditorSubsystem.h"
#include "EditorViewportClient.h"
#include "EditorModeManager.h"
#include "SEditorViewport.h"
#include "Toolkits/ToolkitManager.h"
#include "Toolkits/IToolkit.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"

// Thumbnail fallback
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ThumbnailHelpers.h"
#include "CanvasTypes.h"
#include "EngineModule.h"
#include "LegacyScreenPercentageDriver.h"
#include "SceneView.h"

// Widget Blueprint designer capture
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditor.h"
#include "Slate/WidgetRenderer.h"

// Level editor tab management (bringing viewport to front)
#include "LevelEditor.h"
#include "Framework/Application/SlateApplication.h"

// Game thread dispatch
#include "Async/Async.h"

// Actor focus
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

namespace
{
	static bool IsWidgetDescendantOf(const TSharedPtr<SWidget>& CandidateChild, const TSharedPtr<SWidget>& CandidateParent)
	{
		if (!CandidateChild.IsValid() || !CandidateParent.IsValid())
		{
			return false;
		}

		TSharedPtr<SWidget> Current = CandidateChild;
		int32 RemainingDepth = 100;
		while (Current.IsValid() && RemainingDepth-- > 0)
		{
			if (Current == CandidateParent)
			{
				return true;
			}

			Current = Current->GetParentWidget();
		}

		return false;
	}

	static bool IsLevelSpecificField(const FString& FieldName)
	{
		return FieldName == TEXT("viewport_index")
			|| FieldName == TEXT("location")
			|| FieldName == TEXT("rotation")
			|| FieldName == TEXT("focus_actor")
			|| FieldName == TEXT("view_mode");
	}

	static bool IsAssetSpecificField(const FString& FieldName)
	{
		return FieldName == TEXT("orbit_yaw")
			|| FieldName == TEXT("orbit_pitch")
			|| FieldName == TEXT("orbit_distance");
	}

	static bool TryGetOptionalNumberField(const TSharedPtr<FJsonObject>& Args, const TCHAR* FieldName, double& OutValue, bool& bOutHasField, FString& OutError)
	{
		bOutHasField = false;

		if (!Args.IsValid())
		{
			OutError = TEXT("Invalid arguments");
			return false;
		}

		if (!Args->HasField(FieldName))
		{
			return true;
		}

		bOutHasField = true;
		if (!Args->TryGetNumberField(FieldName, OutValue))
		{
			OutError = FString::Printf(TEXT("Field '%s' must be a number"), FieldName);
			return false;
		}

		return true;
	}

	static bool ParseAssetCameraArgs(
		const TSharedPtr<FJsonObject>& Args,
		bool& bOutHasOrbitYaw, double& OutOrbitYaw,
		bool& bOutHasOrbitPitch, double& OutOrbitPitch,
		bool& bOutHasOrbitDistance, double& OutOrbitDistance,
		bool& bOutHasFOV, float& OutFOV,
		FString& OutError)
	{
		OutOrbitYaw = 0.0;
		OutOrbitPitch = 0.0;
		OutOrbitDistance = 0.0;
		OutFOV = 0.0f;
		bOutHasOrbitYaw = false;
		bOutHasOrbitPitch = false;
		bOutHasOrbitDistance = false;
		bOutHasFOV = false;

		double Tmp = 0.0;
		if (!TryGetOptionalNumberField(Args, TEXT("orbit_yaw"), Tmp, bOutHasOrbitYaw, OutError))
		{
			return false;
		}
		OutOrbitYaw = Tmp;

		if (!TryGetOptionalNumberField(Args, TEXT("orbit_pitch"), Tmp, bOutHasOrbitPitch, OutError))
		{
			return false;
		}
		OutOrbitPitch = Tmp;

		if (!TryGetOptionalNumberField(Args, TEXT("orbit_distance"), Tmp, bOutHasOrbitDistance, OutError))
		{
			return false;
		}
		OutOrbitDistance = Tmp;

		if (!TryGetOptionalNumberField(Args, TEXT("fov"), Tmp, bOutHasFOV, OutError))
		{
			return false;
		}
		OutFOV = FMath::Clamp(static_cast<float>(Tmp), 5.0f, 170.0f);

		return true;
	}

	static bool ParseWaitForReadyMs(const TSharedPtr<FJsonObject>& Args, int32& OutWaitMs, FString& OutError)
	{
		OutWaitMs = 1500;

		double WaitValue = 0.0;
		bool bHasWaitField = false;
		if (!TryGetOptionalNumberField(Args, TEXT("wait_for_ready_ms"), WaitValue, bHasWaitField, OutError))
		{
			return false;
		}

		if (bHasWaitField)
		{
			OutWaitMs = FMath::Clamp(static_cast<int32>(WaitValue), 0, 5000);
		}

		return true;
	}

	static bool IsLikelyNearBlackCapture(const TArray<FColor>& Pixels)
	{
		if (Pixels.Num() <= 0)
		{
			return true;
		}

		// Sample to keep checks cheap on large captures.
		const int32 SampleStep = FMath::Max(1, Pixels.Num() / 30000);
		int32 SampleCount = 0;
		int32 VeryDarkCount = 0;
		int32 NonDarkCount = 0;

		for (int32 Index = 0; Index < Pixels.Num(); Index += SampleStep)
		{
			const FColor& C = Pixels[Index];
			const int32 Luma = (54 * static_cast<int32>(C.R) + 183 * static_cast<int32>(C.G) + 19 * static_cast<int32>(C.B)) >> 8;

			if (Luma <= 4)
			{
				++VeryDarkCount;
			}

			if (Luma >= 20 || C.R >= 16 || C.G >= 16 || C.B >= 16)
			{
				++NonDarkCount;
			}

			++SampleCount;
		}

		if (SampleCount <= 0)
		{
			return true;
		}

		const float VeryDarkRatio = static_cast<float>(VeryDarkCount) / static_cast<float>(SampleCount);
		const int32 NonDarkThreshold = FMath::Max(12, SampleCount / 2000);
		return VeryDarkRatio > 0.995f && NonDarkCount <= NonDarkThreshold;
	}

	static bool ReadViewportPixelsAsColor(FViewport* Viewport, const FIntRect& CaptureRect, TArray<FColor>& OutPixels)
	{
		if (!Viewport || CaptureRect.Area() <= 0)
		{
			return false;
		}

		// Primary path used by engine screenshot flows.
		OutPixels.Reset();
		OutPixels.SetNumUninitialized(CaptureRect.Area());
		if (GetViewportScreenShot(Viewport, OutPixels, CaptureRect))
		{
			return true;
		}

		// HDR fallback: convert linear pixels to sRGB FColor for PNG output.
		if (Viewport->GetSceneHDREnabled())
		{
			TArray<FLinearColor> LinearPixels;
			LinearPixels.SetNumUninitialized(CaptureRect.Area());
			if (GetViewportScreenShotHDR(Viewport, LinearPixels, CaptureRect))
			{
				OutPixels.SetNumUninitialized(LinearPixels.Num());
				ConvertFLinearColorsToFColorSRGB(LinearPixels.GetData(), OutPixels.GetData(), LinearPixels.Num());
				return true;
			}
		}

		return false;
	}

	static void PumpViewportFrame(FEditorViewportClient* ViewportClient, FViewport* Viewport, bool bIsLevelViewport)
	{
		if (!ViewportClient || !Viewport)
		{
			return;
		}

		if (bIsLevelViewport && GEditor)
		{
			GEditor->RedrawLevelEditingViewports(false);
		}

		ViewportClient->Invalidate(false, true);
		Viewport->Draw();
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().Tick();
		}
		FlushRenderingCommands();
	}

	static void WarmupViewportForCapture(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 WaitForReadyMs, bool bIsLevelViewport, bool bPreferNonBlack)
	{
		if (!ViewportClient || !Viewport || WaitForReadyMs <= 0)
		{
			return;
		}

		const double EndTime = FPlatformTime::Seconds() + (static_cast<double>(WaitForReadyMs) / 1000.0);
		int32 ConsecutiveReadyFrames = 0;

		while (FPlatformTime::Seconds() < EndTime)
		{
			PumpViewportFrame(ViewportClient, Viewport, bIsLevelViewport);

			const FIntPoint RTSize = Viewport->GetRenderTargetTextureSizeXY();
			int32 Width = RTSize.X;
			int32 Height = RTSize.Y;
			if (Width <= 0 || Height <= 0)
			{
				const FIntPoint RawSize = Viewport->GetSizeXY();
				Width = RawSize.X;
				Height = RawSize.Y;
			}

			if (Width > 0 && Height > 0)
			{
				bool bReadyThisFrame = true;
				if (bPreferNonBlack)
				{
					const int32 ProbeWidth = FMath::Clamp(Width / 3, 64, 256);
					const int32 ProbeHeight = FMath::Clamp(Height / 3, 64, 256);
					const int32 ProbeMinX = FMath::Max(0, (Width - ProbeWidth) / 2);
					const int32 ProbeMinY = FMath::Max(0, (Height - ProbeHeight) / 2);
					const FIntRect ProbeRect(ProbeMinX, ProbeMinY, ProbeMinX + ProbeWidth, ProbeMinY + ProbeHeight);

					TArray<FColor> ProbePixels;
					bReadyThisFrame = ReadViewportPixelsAsColor(Viewport, ProbeRect, ProbePixels) && !IsLikelyNearBlackCapture(ProbePixels);
				}

				if (bReadyThisFrame)
				{
					++ConsecutiveReadyFrames;
					if (ConsecutiveReadyFrames >= 2)
					{
						break;
					}
				}
				else
				{
					ConsecutiveReadyFrames = 0;
				}
			}

			FPlatformProcess::Sleep(0.02f);
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Input Schema
// ─────────────────────────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FScreenshotTool::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// --- Mode selection ---
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"),
			TEXT("Capture mode: 'active' (default, captures currently focused viewport), "
				"'level' (force level viewport), or 'asset' (force asset editor viewport)."));
		Properties->SetObjectField(TEXT("mode"), Prop);
	}

	// --- Asset path (determines mode) ---
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"),
			TEXT("Asset path to capture in asset mode (e.g., /Game/Meshes/SM_Wall). "
				"If mode is omitted and this is provided, asset mode is used."));
		Properties->SetObjectField(TEXT("asset"), Prop);
	}

	// --- Shared ---
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("integer"));
		Prop->SetStringField(TEXT("description"),
			TEXT("Maximum dimension (width or height) in pixels. Image is downscaled if larger. Default: 1024, Max: 2048"));
		Properties->SetObjectField(TEXT("max_dimension"), Prop);
	}
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("number"));
		Prop->SetStringField(TEXT("description"),
			TEXT("Field of view in degrees (5-170). Lower values zoom in (telephoto), higher zoom out (wide-angle). Works in both modes."));
		Properties->SetObjectField(TEXT("fov"), Prop);
	}
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("integer"));
		Prop->SetStringField(TEXT("description"),
			TEXT("Wait time in milliseconds before capture while the viewport settles (shader compilation, first render, etc.). Default: 1500, Max: 5000."));
		Properties->SetObjectField(TEXT("wait_for_ready_ms"), Prop);
	}
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("boolean"));
		Prop->SetStringField(TEXT("description"),
			TEXT("If true, hides editor overlays (gizmos, grid, selection outlines, icons) before capture "
				"and restores them after. Produces a clean game-like view. Works in both level and asset modes. Default: false."));
		Properties->SetObjectField(TEXT("hide_overlays"), Prop);
	}

	// --- Level viewport params ---
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("integer"));
		Prop->SetStringField(TEXT("description"),
			TEXT("Which level viewport to capture (0-based index, default: 0). Level viewport mode only."));
		Properties->SetObjectField(TEXT("viewport_index"), Prop);
	}
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("object"));
		Prop->SetStringField(TEXT("description"),
			TEXT("Camera position as {x, y, z}. Level viewport mode only."));

		TSharedPtr<FJsonObject> LocProps = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> NumProp = MakeShared<FJsonObject>();
		NumProp->SetStringField(TEXT("type"), TEXT("number"));
		LocProps->SetObjectField(TEXT("x"), NumProp);
		LocProps->SetObjectField(TEXT("y"), MakeShared<FJsonObject>(*NumProp));
		LocProps->SetObjectField(TEXT("z"), MakeShared<FJsonObject>(*NumProp));
		Prop->SetObjectField(TEXT("properties"), LocProps);

		Properties->SetObjectField(TEXT("location"), Prop);
	}
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("object"));
		Prop->SetStringField(TEXT("description"),
			TEXT("Camera rotation as {pitch, yaw, roll} in degrees. Level viewport mode only."));

		TSharedPtr<FJsonObject> RotProps = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> NumProp = MakeShared<FJsonObject>();
		NumProp->SetStringField(TEXT("type"), TEXT("number"));
		RotProps->SetObjectField(TEXT("pitch"), NumProp);
		RotProps->SetObjectField(TEXT("yaw"), MakeShared<FJsonObject>(*NumProp));
		RotProps->SetObjectField(TEXT("roll"), MakeShared<FJsonObject>(*NumProp));
		Prop->SetObjectField(TEXT("properties"), RotProps);

		Properties->SetObjectField(TEXT("rotation"), Prop);
	}
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"),
			TEXT("Actor name or label to auto-frame (camera moves to show entire actor). "
				"Overrides location/rotation. Level viewport mode only."));
		Properties->SetObjectField(TEXT("focus_actor"), Prop);
	}
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"),
			TEXT("Rendering mode: lit, unlit, wireframe, detail_lighting, lighting_only, path_tracing. Level viewport mode only."));
		Properties->SetObjectField(TEXT("view_mode"), Prop);
	}

	// --- Asset editor viewport params ---
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("number"));
		Prop->SetStringField(TEXT("description"),
			TEXT("Orbit camera horizontal angle in degrees (0=front, 90=right, 180=back, 270=left). Asset editor mode only."));
		Properties->SetObjectField(TEXT("orbit_yaw"), Prop);
	}
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("number"));
		Prop->SetStringField(TEXT("description"),
			TEXT("Orbit camera vertical angle in degrees (-89=looking down from above, 0=level, 89=looking up from below). Asset editor mode only."));
		Properties->SetObjectField(TEXT("orbit_pitch"), Prop);
	}
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("number"));
		Prop->SetStringField(TEXT("description"),
			TEXT("Distance from the asset center point. Smaller values zoom in, larger zoom out. Asset editor mode only."));
		Properties->SetObjectField(TEXT("orbit_distance"), Prop);
	}

	Schema->SetObjectField(TEXT("properties"), Properties);
	Schema->SetArrayField(TEXT("required"), TArray<TSharedPtr<FJsonValue>>());

	return Schema;
}

// ─────────────────────────────────────────────────────────────────────────────
// Shared: EncodeAndReturn
// ─────────────────────────────────────────────────────────────────────────────

FToolResult FScreenshotTool::EncodeAndReturn(
	TArray<FColor>& Pixels, int32 Width, int32 Height,
	int32 MaxDimension, const FString& Message)
{
	if (Pixels.Num() != Width * Height || Width <= 0 || Height <= 0)
	{
		return FToolResult::Fail(TEXT("Invalid pixel data for encoding"));
	}

	// Viewport readback often contains zero alpha in editor paths.
	// Force opaque alpha so downstream PNG viewers do not display a black image.
	for (FColor& Pixel : Pixels)
	{
		Pixel.A = 255;
	}

	// Downscale if needed
	int32 OutWidth = Width;
	int32 OutHeight = Height;
	TArray<FColor>* PixelsToEncode = &Pixels;
	TArray<FColor> DownscaledPixels;

	int32 MaxDim = FMath::Max(Width, Height);
	if (MaxDim > MaxDimension)
	{
		float Scale = static_cast<float>(MaxDimension) / static_cast<float>(MaxDim);
		OutWidth = FMath::Max(1, static_cast<int32>(Width * Scale));
		OutHeight = FMath::Max(1, static_cast<int32>(Height * Scale));

		DownscaledPixels.SetNum(OutWidth * OutHeight);

		for (int32 Y = 0; Y < OutHeight; Y++)
		{
			for (int32 X = 0; X < OutWidth; X++)
			{
				int32 SrcX = FMath::Min(static_cast<int32>(X / Scale), Width - 1);
				int32 SrcY = FMath::Min(static_cast<int32>(Y / Scale), Height - 1);
				DownscaledPixels[Y * OutWidth + X] = Pixels[SrcY * Width + SrcX];
			}
		}

		PixelsToEncode = &DownscaledPixels;
	}

	// PNG encode
	TArray64<uint8> PNGData;
	FImageUtils::PNGCompressImageArray(OutWidth, OutHeight, TArrayView64<const FColor>(*PixelsToEncode), PNGData);
	if (PNGData.Num() == 0)
	{
		return FToolResult::Fail(TEXT("Failed to encode pixels to PNG"));
	}

	// Base64 encode — use TArrayView to avoid uint32 truncation on TArray64::Num()
	const uint32 PNGSize = static_cast<uint32>(FMath::Min(PNGData.Num(), static_cast<int64>(MAX_uint32)));
	FString Base64 = FBase64::Encode(PNGData.GetData(), PNGSize);

	FString FullMessage = FString::Printf(TEXT("%s (%dx%d)"), *Message, OutWidth, OutHeight);
	return FToolResult::OkWithImage(FullMessage, Base64, TEXT("image/png"), OutWidth, OutHeight);
}

bool FScreenshotTool::EnsureViewportIsReady(FEditorViewportClient* ViewportClient, FViewport* Viewport) const
{
	if (!ViewportClient || !Viewport)
	{
		return false;
	}

	for (int32 Attempt = 0; Attempt < 5; ++Attempt)
	{
		const FIntPoint RTSize = Viewport->GetRenderTargetTextureSizeXY();
		const FIntPoint RawSize = Viewport->GetSizeXY();
		if ((RTSize.X > 0 && RTSize.Y > 0) || (RawSize.X > 0 && RawSize.Y > 0))
		{
			return true;
		}

		ViewportClient->Invalidate(false, true);
		Viewport->Draw();
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().Tick();
		}
		FlushRenderingCommands();
	}

	return false;
}

FScreenshotTool::FEditorViewportInfo FScreenshotTool::FindFocusedEditorViewport() const
{
	FEditorViewportInfo Best;

	if (!GEditor)
	{
		return Best;
	}

	// Strong signal for asset viewport focus: keyboard-focused slate widget is inside a viewport widget.
	if (FSlateApplication::IsInitialized())
	{
		const int32 KeyboardUser = FSlateApplication::Get().GetUserIndexForKeyboard();
		const TSharedPtr<SWidget> FocusedWidget = FSlateApplication::Get().GetUserFocusedWidget(KeyboardUser);
		if (FocusedWidget.IsValid())
		{
			for (FEditorViewportClient* Client : GEditor->GetAllViewportClients())
			{
				if (!Client || !Client->Viewport)
				{
					continue;
				}

				const TSharedPtr<SEditorViewport> EditorViewportWidget = Client->GetEditorViewportWidget();
				if (!EditorViewportWidget.IsValid())
				{
					continue;
				}

				if (IsWidgetDescendantOf(FocusedWidget, EditorViewportWidget))
				{
					Best.ViewportClient = Client;
					Best.Viewport = Client->Viewport;
					return Best;
				}
			}
		}
	}

	// Fallback: if viewport itself reports focus state, pick the strongest focused one.
	int32 BestScore = TNumericLimits<int32>::Lowest();
	for (FEditorViewportClient* Client : GEditor->GetAllViewportClients())
	{
		if (!Client || !Client->Viewport)
		{
			continue;
		}

		TSharedPtr<SEditorViewport> EditorViewportWidget = Client->GetEditorViewportWidget();
		if (!EditorViewportWidget.IsValid())
		{
			continue;
		}

		const bool bHasDirectFocus = EditorViewportWidget->HasAnyUserFocus().IsSet();
		const bool bHasFocusedDescendants = EditorViewportWidget->HasAnyUserFocusOrFocusedDescendants();
		if (!bHasDirectFocus && !bHasFocusedDescendants)
		{
			continue;
		}

		int32 Score = 0;
		if (bHasDirectFocus)
		{
			Score += 120;
		}
		if (bHasFocusedDescendants)
		{
			Score += 90;
		}

		if (Score > BestScore)
		{
			BestScore = Score;
			Best.ViewportClient = Client;
			Best.Viewport = Client->Viewport;
		}
	}

	if (Best.ViewportClient && Best.Viewport)
	{
		return Best;
	}

	// Final fallback for level editor: use engine active level viewport only if its window is currently active.
	if (FViewport* ActiveViewport = GEditor->GetActiveViewport())
	{
		const TSharedPtr<SWindow> ActiveWindow = FSlateApplication::IsInitialized()
			? FSlateApplication::Get().GetActiveTopLevelWindow()
			: nullptr;

		for (FEditorViewportClient* Client : GEditor->GetAllViewportClients())
		{
			if (!Client || Client->Viewport != ActiveViewport)
			{
				continue;
			}

			if (!FSlateApplication::IsInitialized() || !ActiveWindow.IsValid())
			{
				Best.ViewportClient = Client;
				Best.Viewport = Client->Viewport;
				return Best;
			}

			const TSharedPtr<SEditorViewport> EditorViewportWidget = Client->GetEditorViewportWidget();
			if (!EditorViewportWidget.IsValid())
			{
				continue;
			}

			const TSharedPtr<SWindow> ViewportWindow = FSlateApplication::Get().FindWidgetWindow(EditorViewportWidget.ToSharedRef());
			if (ViewportWindow.IsValid() && ViewportWindow == ActiveWindow)
			{
				Best.ViewportClient = Client;
				Best.Viewport = Client->Viewport;
				return Best;
			}
		}
	}

	return Best;
}

UObject* FScreenshotTool::FindMostRecentlyActivatedEditedAsset() const
{
	if (!GEditor)
	{
		return nullptr;
	}

	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditorSubsystem)
	{
		return nullptr;
	}

	const TArray<UObject*> EditedAssets = AssetEditorSubsystem->GetAllEditedAssets();
	UObject* BestAsset = nullptr;
	double BestActivation = -TNumericLimits<double>::Max();

	for (UObject* Candidate : EditedAssets)
	{
		if (!IsValid(Candidate))
		{
			continue;
		}

		IAssetEditorInstance* Instance = AssetEditorSubsystem->FindEditorForAsset(Candidate, false);
		if (!Instance)
		{
			continue;
		}

		const double ActivationTime = Instance->GetLastActivationTime();
		if (ActivationTime > BestActivation)
		{
			BestActivation = ActivationTime;
			BestAsset = Candidate;
		}
	}

	return BestAsset;
}

FToolResult FScreenshotTool::CaptureAssetViewport(const TSharedPtr<FJsonObject>& Args, int32 MaxDimension, const FString& AssetPath)
{
	UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
	if (!Asset)
	{
		return FToolResult::Fail(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
	}

	bool bHasOrbitYaw = false;
	bool bHasOrbitPitch = false;
	bool bHasOrbitDistance = false;
	bool bHasFOV = false;
	double OrbitYaw = 0.0;
	double OrbitPitch = 0.0;
	double OrbitDistance = 0.0;
	float FOV = 0.0f;
	FString ParseError;
	if (!ParseAssetCameraArgs(
		Args,
		bHasOrbitYaw, OrbitYaw,
		bHasOrbitPitch, OrbitPitch,
		bHasOrbitDistance, OrbitDistance,
		bHasFOV, FOV,
		ParseError))
	{
		return FToolResult::Fail(ParseError);
	}

	int32 WaitForReadyMs = 1500;
	if (!ParseWaitForReadyMs(Args, WaitForReadyMs, ParseError))
	{
		return FToolResult::Fail(ParseError);
	}

	bool bHideOverlays = false;
	Args->TryGetBoolField(TEXT("hide_overlays"), bHideOverlays);

	FEditorViewportInfo ViewportInfo = FindAssetEditorViewport(Asset);
	if (ViewportInfo.ViewportClient && ViewportInfo.Viewport)
	{
		if (!EnsureViewportIsReady(ViewportInfo.ViewportClient, ViewportInfo.Viewport))
		{
			return FToolResult::Fail(TEXT("Asset editor viewport is not ready for capture. Ensure the asset viewport tab is visible."));
		}

		return CaptureEditorViewport(
			ViewportInfo.ViewportClient, ViewportInfo.Viewport,
			MaxDimension, Asset->GetName(),
			bHasOrbitYaw, OrbitYaw,
			bHasOrbitPitch, OrbitPitch,
			bHasOrbitDistance, OrbitDistance,
			bHasFOV, FOV,
			WaitForReadyMs,
			bHideOverlays);
	}

	// Widget Blueprint: designer is a 2D Slate panel, not a 3D viewport
	if (UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(Asset))
	{
		return CaptureWidgetDesigner(WidgetBP, MaxDimension);
	}

	// Fallback: thumbnail render for meshes
	if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
	{
		const int32 Resolution = FMath::Clamp(MaxDimension, 128, 1024);
		FStaticMeshThumbnailScene Scene;
		Scene.SetStaticMesh(StaticMesh);
		FToolResult Result = CaptureThumbnailScene(Scene, Resolution, StaticMesh->GetName(), TEXT("StaticMesh"));
		Scene.SetStaticMesh(nullptr);
		return Result;
	}

	if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset))
	{
		const int32 Resolution = FMath::Clamp(MaxDimension, 128, 1024);
		FSkeletalMeshThumbnailScene Scene;
		Scene.SetSkeletalMesh(SkeletalMesh);
		FToolResult Result = CaptureThumbnailScene(Scene, Resolution, SkeletalMesh->GetName(), TEXT("SkeletalMesh"));
		Scene.SetSkeletalMesh(nullptr);
		return Result;
	}

	return FToolResult::Fail(FString::Printf(
		TEXT("No open editor viewport found for asset '%s' (%s). Open the asset in its editor so the viewport can be captured."),
		*AssetPath, *Asset->GetClass()->GetName()));
}

FToolResult FScreenshotTool::CaptureActiveViewport(const TSharedPtr<FJsonObject>& Args, int32 MaxDimension)
{
	// If the user passed an explicit asset path, active mode should target that asset.
	FString AssetPath;
	if (Args->TryGetStringField(TEXT("asset"), AssetPath) && !AssetPath.IsEmpty())
	{
		return CaptureAssetViewport(Args, MaxDimension, AssetPath);
	}

	FEditorViewportInfo FocusedViewport = FindFocusedEditorViewport();
	if (FocusedViewport.ViewportClient && FocusedViewport.Viewport)
	{
		if (FocusedViewport.ViewportClient->IsLevelEditorClient())
		{
			return CaptureLevelViewport(Args, 0, MaxDimension);
		}

		bool bHasOrbitYaw = false;
		bool bHasOrbitPitch = false;
		bool bHasOrbitDistance = false;
		bool bHasFOV = false;
		double OrbitYaw = 0.0;
		double OrbitPitch = 0.0;
		double OrbitDistance = 0.0;
		float FOV = 0.0f;
		FString ParseError;
		if (!ParseAssetCameraArgs(
			Args,
			bHasOrbitYaw, OrbitYaw,
			bHasOrbitPitch, OrbitPitch,
			bHasOrbitDistance, OrbitDistance,
			bHasFOV, FOV,
			ParseError))
		{
			return FToolResult::Fail(ParseError);
		}

		int32 WaitForReadyMs = 1500;
		if (!ParseWaitForReadyMs(Args, WaitForReadyMs, ParseError))
		{
			return FToolResult::Fail(ParseError);
		}

		bool bHideOverlays = false;
		Args->TryGetBoolField(TEXT("hide_overlays"), bHideOverlays);

		if (!EnsureViewportIsReady(FocusedViewport.ViewportClient, FocusedViewport.Viewport))
		{
			return FToolResult::Fail(TEXT("Focused editor viewport is not ready for capture"));
		}

		return CaptureEditorViewport(
			FocusedViewport.ViewportClient, FocusedViewport.Viewport,
			MaxDimension, TEXT("FocusedAssetViewport"),
			bHasOrbitYaw, OrbitYaw,
			bHasOrbitPitch, OrbitPitch,
			bHasOrbitDistance, OrbitDistance,
			bHasFOV, FOV,
			WaitForReadyMs,
			bHideOverlays);
	}

	// Fallback to the most recently activated edited asset if available.
	if (UObject* RecentAsset = FindMostRecentlyActivatedEditedAsset())
	{
		return CaptureAssetViewport(Args, MaxDimension, RecentAsset->GetPathName());
	}

	// Final fallback: level viewport.
	return CaptureLevelViewport(Args, 0, MaxDimension);
}

// ─────────────────────────────────────────────────────────────────────────────
// Execute: Router
// ─────────────────────────────────────────────────────────────────────────────

FToolResult FScreenshotTool::Execute(const TSharedPtr<FJsonObject>& Args)
{
	if (!Args.IsValid())
	{
		return FToolResult::Fail(TEXT("Invalid arguments"));
	}

	if (!GEditor)
	{
		return FToolResult::Fail(TEXT("Editor not available"));
	}

	UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("Screenshot: IsInGameThread=%s"),
		IsInGameThread() ? TEXT("YES") : TEXT("NO"));

	// All viewport/Slate operations require the game thread.
	// MCP server calls tools from an HTTP background thread, so dispatch there.
	if (!IsInGameThread())
	{
		UE_LOG(LogAgentIntegrationKit, Verbose, TEXT("Screenshot: Dispatching to game thread..."));
		FToolResult GameThreadResult;
		FEvent* DoneEvent = FPlatformProcess::GetSynchEventFromPool();
		AsyncTask(ENamedThreads::GameThread, [this, &Args, &GameThreadResult, DoneEvent]()
		{
			GameThreadResult = Execute(Args);
			DoneEvent->Trigger();
		});
		DoneEvent->Wait();
		FPlatformProcess::ReturnSynchEventToPool(DoneEvent);
		return GameThreadResult;
	}

	// Parse shared params
	int32 MaxDimension = 1024;
	double MaxDimensionValue = 0.0;
	bool bHasMaxDimension = false;
	FString ParseError;
	if (!TryGetOptionalNumberField(Args, TEXT("max_dimension"), MaxDimensionValue, bHasMaxDimension, ParseError))
	{
		return FToolResult::Fail(ParseError);
	}
	if (bHasMaxDimension)
	{
		MaxDimension = FMath::Clamp(static_cast<int32>(MaxDimensionValue), 128, 2048);
	}

	FString AssetPath;
	bool bHasAsset = false;
	if (Args->HasField(TEXT("asset")))
	{
		if (!Args->TryGetStringField(TEXT("asset"), AssetPath))
		{
			return FToolResult::Fail(TEXT("Field 'asset' must be a string"));
		}
		AssetPath = AssetPath.TrimStartAndEnd();
		bHasAsset = !AssetPath.IsEmpty();
	}

	bool bHasLevelSpecific = false;
	bool bHasAssetSpecific = false;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Args->Values)
	{
		bHasLevelSpecific = bHasLevelSpecific || IsLevelSpecificField(Pair.Key);
		bHasAssetSpecific = bHasAssetSpecific || IsAssetSpecificField(Pair.Key);
	}

	EScreenshotMode Mode = EScreenshotMode::Active;
	FString ModeStr;
	const bool bHasModeField = Args->HasField(TEXT("mode"));
	if (bHasModeField)
	{
		if (!Args->TryGetStringField(TEXT("mode"), ModeStr))
		{
			return FToolResult::Fail(TEXT("Field 'mode' must be a string (active, level, or asset)"));
		}

		ModeStr = ModeStr.TrimStartAndEnd().ToLower();
		if (ModeStr == TEXT("active"))
		{
			Mode = EScreenshotMode::Active;
		}
		else if (ModeStr == TEXT("level"))
		{
			Mode = EScreenshotMode::Level;
		}
		else if (ModeStr == TEXT("asset"))
		{
			Mode = EScreenshotMode::Asset;
		}
		else
		{
			return FToolResult::Fail(FString::Printf(
				TEXT("Unknown mode '%s'. Valid values are: active, level, asset"), *ModeStr));
		}
	}
	else
	{
		// Backward compatibility:
		// - If asset path is provided, keep asset behavior.
		// - If level-specific camera fields are used, keep level behavior.
		// - Otherwise default to active viewport behavior.
		if (bHasAsset)
		{
			Mode = EScreenshotMode::Asset;
		}
		else if (bHasLevelSpecific && !bHasAssetSpecific)
		{
			Mode = EScreenshotMode::Level;
		}
		else
		{
			Mode = EScreenshotMode::Active;
		}
	}

	if (Mode == EScreenshotMode::Asset && !bHasAsset)
	{
		return FToolResult::Fail(TEXT("mode='asset' requires a non-empty 'asset' path"));
	}

	if (Mode == EScreenshotMode::Asset)
	{
		return CaptureAssetViewport(Args, MaxDimension, AssetPath);
	}

	if (Mode == EScreenshotMode::Active)
	{
		return CaptureActiveViewport(Args, MaxDimension);
	}

	int32 ViewportIndex = 0;
	double ViewportIndexValue = 0.0;
	bool bHasViewportIndex = false;
	if (!TryGetOptionalNumberField(Args, TEXT("viewport_index"), ViewportIndexValue, bHasViewportIndex, ParseError))
	{
		return FToolResult::Fail(ParseError);
	}
	if (bHasViewportIndex)
	{
		ViewportIndex = static_cast<int32>(ViewportIndexValue);
	}

	return CaptureLevelViewport(Args, ViewportIndex, MaxDimension);
}

// ─────────────────────────────────────────────────────────────────────────────
// Level Viewport Capture
// ─────────────────────────────────────────────────────────────────────────────

FToolResult FScreenshotTool::CaptureLevelViewport(
	const TSharedPtr<FJsonObject>& Args, int32 ViewportIndex, int32 MaxDimension)
{
	// Use the engine's active viewport tracking (GCurrentLevelEditingViewportClient)
	// to get the viewport the user is actually looking at — not just any viewport with dimensions.
	FLevelEditorViewportClient* ViewportClient = nullptr;
	FViewport* Viewport = nullptr;

	if (ViewportIndex == 0)
	{
		// Prefer LevelEditor module's active visible level viewport.
		if (FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>("LevelEditor"))
		{
			if (TSharedPtr<IAssetViewport> ActiveLevelViewport = LevelEditorModule->GetFirstActiveViewport())
			{
				FEditorViewportClient& ActiveClient = ActiveLevelViewport->GetAssetViewportClient();
				if (ActiveClient.IsLevelEditorClient())
				{
					FLevelEditorViewportClient* ActiveLevelClient = static_cast<FLevelEditorViewportClient*>(&ActiveClient);
					FViewport* ActiveViewport = ActiveLevelViewport->GetActiveViewport();
					if (!ActiveViewport && ActiveLevelClient)
					{
						ActiveViewport = ActiveLevelClient->Viewport;
					}

					if (ActiveLevelClient && ActiveLevelClient->IsVisible() && ActiveViewport)
					{
						ViewportClient = ActiveLevelClient;
						Viewport = ActiveViewport;
					}
				}
			}
		}

		// First: prefer the currently active editor viewport as reported by editor engine.
		if (!ViewportClient)
		{
			if (FViewport* ActiveViewport = GEditor->GetActiveViewport())
			{
				const TArray<FLevelEditorViewportClient*>& ViewportClients = GEditor->GetLevelViewportClients();
				for (FLevelEditorViewportClient* VC : ViewportClients)
				{
					if (VC && VC->IsVisible() && VC->Viewport == ActiveViewport)
					{
						ViewportClient = VC;
						Viewport = VC->Viewport;
						break;
					}
				}
			}
		}

		// Default: use the engine's active viewport (what the user is actually looking at)
		if (!ViewportClient)
		{
			ViewportClient = GCurrentLevelEditingViewportClient;
			if (ViewportClient)
			{
				Viewport = ViewportClient->Viewport;
			}
		}

		// Fallback: a cinematic-control viewport with valid dimensions.
		if (!ViewportClient || !Viewport || Viewport->GetSizeXY().X <= 0)
		{
			const TArray<FLevelEditorViewportClient*>& ViewportClients = GEditor->GetLevelViewportClients();
			for (FLevelEditorViewportClient* VC : ViewportClients)
			{
				if (VC && VC->IsVisible() && VC->Viewport && VC->AllowsCinematicControl() && VC->Viewport->GetSizeXY().X > 0)
				{
					ViewportClient = VC;
					Viewport = VC->Viewport;
					break;
				}
			}
		}

		// Final fallback: first viewport with valid dimensions.
		if (!ViewportClient || !Viewport || Viewport->GetSizeXY().X <= 0)
		{
			const TArray<FLevelEditorViewportClient*>& ViewportClients = GEditor->GetLevelViewportClients();
			for (FLevelEditorViewportClient* VC : ViewportClients)
			{
				if (VC && VC->IsVisible() && VC->Viewport && VC->Viewport->GetSizeXY().X > 0)
				{
					ViewportClient = VC;
					Viewport = VC->Viewport;
					break;
				}
			}
		}
	}
	else
	{
		// Explicit viewport_index — use it directly
		const TArray<FLevelEditorViewportClient*>& ViewportClients = GEditor->GetLevelViewportClients();
		if (ViewportClients.Num() == 0)
		{
			return FToolResult::Fail(TEXT("No level editor viewports available"));
		}
		int32 Idx = FMath::Clamp(ViewportIndex, 0, ViewportClients.Num() - 1);
		ViewportClient = ViewportClients[Idx];
		if (ViewportClient)
		{
			Viewport = ViewportClient->Viewport;
		}
	}

	if (!ViewportClient || !Viewport)
	{
		return FToolResult::Fail(TEXT("No active level viewport found"));
	}

	if (!ViewportClient->GetWorld())
	{
		return FToolResult::Fail(TEXT("Selected level viewport has no valid world"));
	}

	const bool bHasExplicitLevelCameraControls =
		Args->HasField(TEXT("location"))
		|| Args->HasField(TEXT("rotation"))
		|| Args->HasField(TEXT("focus_actor"))
		|| Args->HasField(TEXT("view_mode"))
		|| Args->HasField(TEXT("fov"));

	int32 WaitForReadyMs = 1500;
	FString WaitParseError;
	if (!ParseWaitForReadyMs(Args, WaitForReadyMs, WaitParseError))
	{
		return FToolResult::Fail(WaitParseError);
	}

	// If the selected viewport has 0 dimensions, bring its tab to the foreground.
	if (Viewport->GetSizeXY().X <= 0 || Viewport->GetSizeXY().Y <= 0)
	{
		FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>("LevelEditor");
		if (LevelEditorModule)
		{
			TSharedPtr<FTabManager> TabManager = LevelEditorModule->GetLevelEditorTabManager();
			if (TabManager.IsValid())
			{
				TabManager->TryInvokeTab(LevelEditorTabIds::LevelEditorViewport);

			}
		}
	}

	if (!EnsureViewportIsReady(ViewportClient, Viewport))
	{
		const FIntPoint RawSize = Viewport ? Viewport->GetSizeXY() : FIntPoint::ZeroValue;
		return FToolResult::Fail(FString::Printf(
			TEXT("Level viewport has invalid dimensions (%dx%d). Ensure the viewport tab is visible and not minimized."),
			RawSize.X, RawSize.Y));
	}

	// ── Apply view mode ──
	FString ViewModeStr;
	if (Args->TryGetStringField(TEXT("view_mode"), ViewModeStr) && !ViewModeStr.IsEmpty())
	{
		EViewModeIndex NewViewMode = VMI_Unknown;
		if (ViewModeStr == TEXT("lit")) NewViewMode = VMI_Lit;
		else if (ViewModeStr == TEXT("unlit")) NewViewMode = VMI_Unlit;
		else if (ViewModeStr == TEXT("wireframe")) NewViewMode = VMI_Wireframe;
		else if (ViewModeStr == TEXT("detail_lighting")) NewViewMode = VMI_Lit_DetailLighting;
		else if (ViewModeStr == TEXT("lighting_only")) NewViewMode = VMI_LightingOnly;
		else if (ViewModeStr == TEXT("path_tracing")) NewViewMode = VMI_PathTracing;

		if (NewViewMode != VMI_Unknown)
		{
			ViewportClient->SetViewMode(NewViewMode);
		}
		else
		{
			return FToolResult::Fail(FString::Printf(
				TEXT("Unknown view_mode: '%s'. Valid: lit, unlit, wireframe, detail_lighting, lighting_only, path_tracing"),
				*ViewModeStr));
		}
	}

	// ── Apply focus_actor (overrides manual location/rotation) ──
	FString FocusActorName;
	bool bFocusedOnActor = false;
	if (Args->TryGetStringField(TEXT("focus_actor"), FocusActorName) && !FocusActorName.IsEmpty())
	{
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			return FToolResult::Fail(TEXT("No editor world available for focus_actor"));
		}

		AActor* FoundActor = nullptr;

		// Exact label match (case-insensitive)
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (*It && (*It)->GetActorLabel().Equals(FocusActorName, ESearchCase::IgnoreCase))
			{
				FoundActor = *It;
				break;
			}
		}
		// Exact name match (case-insensitive)
		if (!FoundActor)
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (*It && (*It)->GetName().Equals(FocusActorName, ESearchCase::IgnoreCase))
				{
					FoundActor = *It;
					break;
				}
			}
		}
		// Partial match (contains)
		if (!FoundActor)
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (*It && ((*It)->GetActorLabel().Contains(FocusActorName) ||
					(*It)->GetName().Contains(FocusActorName)))
				{
					FoundActor = *It;
					break;
				}
			}
		}

		if (!FoundActor)
		{
			return FToolResult::Fail(FString::Printf(TEXT("Actor not found: '%s'"), *FocusActorName));
		}

		FBox BBox = FoundActor->GetComponentsBoundingBox(true);
		if (!BBox.IsValid)
		{
			return FToolResult::Fail(FString::Printf(
				TEXT("Actor '%s' has no valid bounding box"), *FocusActorName));
		}

		ViewportClient->FocusViewportOnBox(BBox, true);
		bFocusedOnActor = true;
	}

	// ── Apply manual location/rotation (only if not using focus_actor) ──
	if (!bFocusedOnActor)
	{
		const TSharedPtr<FJsonObject>* LocationPtr = nullptr;
		if (Args->TryGetObjectField(TEXT("location"), LocationPtr) && LocationPtr && (*LocationPtr).IsValid())
		{
			FVector NewLocation = FVector::ZeroVector;
			(*LocationPtr)->TryGetNumberField(TEXT("x"), NewLocation.X);
			(*LocationPtr)->TryGetNumberField(TEXT("y"), NewLocation.Y);
			(*LocationPtr)->TryGetNumberField(TEXT("z"), NewLocation.Z);
			ViewportClient->SetViewLocation(NewLocation);
		}

		const TSharedPtr<FJsonObject>* RotationPtr = nullptr;
		if (Args->TryGetObjectField(TEXT("rotation"), RotationPtr) && RotationPtr && (*RotationPtr).IsValid())
		{
			FRotator NewRotation = FRotator::ZeroRotator;
			(*RotationPtr)->TryGetNumberField(TEXT("pitch"), NewRotation.Pitch);
			(*RotationPtr)->TryGetNumberField(TEXT("yaw"), NewRotation.Yaw);
			(*RotationPtr)->TryGetNumberField(TEXT("roll"), NewRotation.Roll);
			ViewportClient->SetViewRotation(NewRotation);
		}
	}

	// ── Apply FOV ──
	{
		double FOVD = 0;
		if (Args->TryGetNumberField(TEXT("fov"), FOVD))
		{
			ViewportClient->ViewFOV = FMath::Clamp(static_cast<float>(FOVD), 5.0f, 170.0f);
		}
	}

	// ── Apply hide_overlays (game view) ──
	bool bHideOverlays = false;
	bool bWasInGameView = ViewportClient->IsInGameView();
	if (Args->HasField(TEXT("hide_overlays")))
	{
		bool bHideValue = false;
		if (Args->TryGetBoolField(TEXT("hide_overlays"), bHideValue) && bHideValue && !bWasInGameView)
		{
			ViewportClient->SetGameView(true);
			bHideOverlays = true;
		}
	}

	// Scope guard to restore game view on all exit paths
	struct FGameViewRestorer
	{
		FEditorViewportClient* Client;
		bool bShouldRestore;
		~FGameViewRestorer() { if (bShouldRestore) { Client->SetGameView(false); } }
	} GameViewRestorer{ ViewportClient, bHideOverlays && !bWasInGameView };

	auto RenderAndReadPixels = [&](FLevelEditorViewportClient* InViewportClient, FViewport* InViewport, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight) -> bool
	{
		if (!InViewportClient || !InViewport)
		{
			return false;
		}

		for (int32 Attempt = 0; Attempt < 3; ++Attempt)
		{
			PumpViewportFrame(InViewportClient, InViewport, true);

			FIntPoint RenderTargetSize = InViewport->GetRenderTargetTextureSizeXY();
			OutWidth = RenderTargetSize.X;
			OutHeight = RenderTargetSize.Y;
			if (OutWidth <= 0 || OutHeight <= 0)
			{
				const FIntPoint RawSize = InViewport->GetSizeXY();
				OutWidth = RawSize.X;
				OutHeight = RawSize.Y;
			}

			if (OutWidth <= 0 || OutHeight <= 0)
			{
				continue;
			}

			const FIntRect CaptureRect(0, 0, OutWidth, OutHeight);
			if (ReadViewportPixelsAsColor(InViewport, CaptureRect, OutPixels))
			{
				return true;
			}
		}

		return false;
	};

	WarmupViewportForCapture(ViewportClient, Viewport, WaitForReadyMs, true, true);

	int32 ViewWidth = 0;
	int32 ViewHeight = 0;
	TArray<FColor> Pixels;
	if (!RenderAndReadPixels(ViewportClient, Viewport, Pixels, ViewWidth, ViewHeight))
	{
		return FToolResult::Fail(TEXT("Failed to read pixels from level viewport"));
	}

	// Defensive fallback for stale/inactive viewport captures:
	// if default level mode produced an almost fully-black frame and the user did not request explicit camera controls,
	// try other level viewports before returning the bad frame.
	if (ViewportIndex == 0 && !bHasExplicitLevelCameraControls && IsLikelyNearBlackCapture(Pixels))
	{
		const TArray<FLevelEditorViewportClient*>& ViewportClients = GEditor->GetLevelViewportClients();
		for (FLevelEditorViewportClient* Candidate : ViewportClients)
		{
			if (!Candidate || Candidate == ViewportClient || !Candidate->IsVisible() || !Candidate->Viewport || !Candidate->GetWorld())
			{
				continue;
			}

			if (!EnsureViewportIsReady(Candidate, Candidate->Viewport))
			{
				continue;
			}

			TArray<FColor> CandidatePixels;
			int32 CandidateWidth = 0;
			int32 CandidateHeight = 0;
			if (!RenderAndReadPixels(Candidate, Candidate->Viewport, CandidatePixels, CandidateWidth, CandidateHeight))
			{
				continue;
			}

			if (!IsLikelyNearBlackCapture(CandidatePixels))
			{
				ViewportClient = Candidate;
				Viewport = Candidate->Viewport;
				ViewWidth = CandidateWidth;
				ViewHeight = CandidateHeight;
				Pixels = MoveTemp(CandidatePixels);
				break;
			}
		}
	}

	// Build response message
	FString ViewMode;
	switch (ViewportClient->GetViewMode())
	{
	case VMI_Lit: ViewMode = TEXT("Lit"); break;
	case VMI_Unlit: ViewMode = TEXT("Unlit"); break;
	case VMI_Wireframe: ViewMode = TEXT("Wireframe"); break;
	case VMI_Lit_DetailLighting: ViewMode = TEXT("DetailLighting"); break;
	case VMI_LightingOnly: ViewMode = TEXT("LightingOnly"); break;
	case VMI_PathTracing: ViewMode = TEXT("PathTracing"); break;
	default: ViewMode = TEXT("Other"); break;
	}

	FVector CamLoc = ViewportClient->GetViewLocation();
	FRotator CamRot = ViewportClient->GetViewRotation();

	FString Message = FString::Printf(
		TEXT("Level viewport screenshot captured (%s mode)\n"
			"Camera: Location=(%.1f, %.1f, %.1f) Rotation=(Pitch=%.1f, Yaw=%.1f, Roll=%.1f) FOV=%.1f"),
		*ViewMode,
		CamLoc.X, CamLoc.Y, CamLoc.Z,
		CamRot.Pitch, CamRot.Yaw, CamRot.Roll,
		ViewportClient->ViewFOV);

	if (bFocusedOnActor)
	{
		Message += FString::Printf(TEXT("\nFocused on actor: %s"), *FocusActorName);
	}

	if (bHideOverlays)
	{
		Message += TEXT("\nOverlays: hidden (game view)");
	}

	return EncodeAndReturn(Pixels, ViewWidth, ViewHeight, MaxDimension, Message);
}

// ─────────────────────────────────────────────────────────────────────────────
// Asset Editor Viewport Finding
// ─────────────────────────────────────────────────────────────────────────────

FScreenshotTool::FEditorViewportInfo FScreenshotTool::FindAssetEditorViewport(UObject* Asset)
{
	FEditorViewportInfo Result;

	if (!GEditor || !Asset)
	{
		return Result;
	}

	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditorSubsystem)
	{
		return Result;
	}

	IAssetEditorInstance* EditorInstance = AssetEditorSubsystem->FindEditorForAsset(Asset, true);
	if (!EditorInstance)
	{
		AssetEditorSubsystem->OpenEditorForAsset(Asset);
		EditorInstance = AssetEditorSubsystem->FindEditorForAsset(Asset, true);
		if (!EditorInstance)
		{
			return Result;
		}
	}

	// Primary path: use toolkit mode manager focus state for this asset editor.
	if (TSharedPtr<IToolkit> Toolkit = FToolkitManager::Get().FindEditorForAsset(Asset))
	{
		Toolkit->BringToolkitToFront();
		FEditorViewportClient* FocusedClient = Toolkit->GetEditorModeManager().GetFocusedViewportClient();
		if (FocusedClient && FocusedClient->Viewport && !FocusedClient->IsLevelEditorClient())
		{
			Result.ViewportClient = FocusedClient;
			Result.Viewport = FocusedClient->Viewport;
			return Result;
		}

		FEditorViewportClient* HoveredClient = Toolkit->GetEditorModeManager().GetHoveredViewportClient();
		if (HoveredClient && HoveredClient->Viewport && !HoveredClient->IsLevelEditorClient())
		{
			Result.ViewportClient = HoveredClient;
			Result.Viewport = HoveredClient->Viewport;
			return Result;
		}
	}

	// Secondary path: activate likely viewport tabs in this editor and correlate by window/focus/size.
	TSharedPtr<FTabManager> TabManager = EditorInstance->GetAssociatedTabManager();
	TArray<TSharedPtr<SWidget>> CandidateViewportRoots;
	if (TabManager.IsValid())
	{
		static const FName ViewportTabIds[] = {
			FName("Viewport"),                                      // Persona and many editors
			FName("Viewport1"), FName("Viewport2"), FName("Viewport3"),
			FName("StaticMeshEditor_Viewport"),                     // Static Mesh
			FName("SCSViewport"),                                   // Blueprint SCS
			FName("NiagaraSystemEditor_Viewport"),                  // Niagara System
			FName("NiagaraSimCacheEditor_Viewport"),                // Niagara Sim Cache
			FName("MaterialEditor_Preview"),                        // Material
			FName("MaterialInstanceEditor_Preview"),                // Material Instance
			FName("PoseSearchDatabaseEditorViewportTabID"),         // PoseSearch Database
			FName("PoseSearchInteractionAssetEditorViewportTabID"), // PoseSearch Interaction
		};

		for (const FName& TabId : ViewportTabIds)
		{
			TSharedPtr<SDockTab> Tab = TabManager->FindExistingLiveTab(FTabId(TabId));
			if (!Tab.IsValid())
			{
				Tab = TabManager->TryInvokeTab(FTabId(TabId));
			}
			if (Tab.IsValid())
			{
				Tab->ActivateInParent(ETabActivationCause::SetDirectly);

				const TSharedRef<SWidget> TabContent = Tab->GetContent();
				CandidateViewportRoots.Add(TabContent);
			}
		}

		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().Tick();
		}
		FlushRenderingCommands();
	}

	// First try strict ownership: viewport widget must be under one of the asset editor viewport tab roots.
	if (CandidateViewportRoots.Num() > 0)
	{
		int32 BestOwnedScore = TNumericLimits<int32>::Lowest();
		for (FEditorViewportClient* Client : GEditor->GetAllViewportClients())
		{
			if (!Client || !Client->Viewport || Client->IsLevelEditorClient())
			{
				continue;
			}

			const TSharedPtr<SEditorViewport> EditorViewportWidget = Client->GetEditorViewportWidget();
			if (!EditorViewportWidget.IsValid())
			{
				continue;
			}

			bool bIsOwnedByThisEditor = false;
			for (const TSharedPtr<SWidget>& Root : CandidateViewportRoots)
			{
				if (IsWidgetDescendantOf(EditorViewportWidget, Root))
				{
					bIsOwnedByThisEditor = true;
					break;
				}
			}

			if (!bIsOwnedByThisEditor)
			{
				continue;
			}

			int32 Score = 0;
			if (EditorViewportWidget->HasAnyUserFocus().IsSet())
			{
				Score += 120;
			}
			if (EditorViewportWidget->HasAnyUserFocusOrFocusedDescendants())
			{
				Score += 90;
			}

			const FIntPoint RTSize = Client->Viewport->GetRenderTargetTextureSizeXY();
			const FIntPoint RawSize = Client->Viewport->GetSizeXY();
			if ((RTSize.X > 0 && RTSize.Y > 0) || (RawSize.X > 0 && RawSize.Y > 0))
			{
				Score += 20;
			}

			if (Score > BestOwnedScore)
			{
				BestOwnedScore = Score;
				Result.ViewportClient = Client;
				Result.Viewport = Client->Viewport;
			}
		}

		if (Result.ViewportClient && Result.Viewport)
		{
			return Result;
		}
	}

	TSharedPtr<SWindow> EditorWindow;
	if (FSlateApplication::IsInitialized() && TabManager.IsValid())
	{
		const TSharedPtr<SDockTab> OwnerTab = TabManager->GetOwnerTab();
		if (OwnerTab.IsValid())
		{
			EditorWindow = FSlateApplication::Get().FindWidgetWindow(OwnerTab.ToSharedRef());
		}
	}

	int32 BestScore = TNumericLimits<int32>::Lowest();
	for (FEditorViewportClient* Client : GEditor->GetAllViewportClients())
	{
		if (!Client || !Client->Viewport)
		{
			continue;
		}

		// Asset capture must never fall back to a level viewport.
		if (Client->IsLevelEditorClient())
		{
			continue;
		}

		const TSharedPtr<SEditorViewport> EditorViewportWidget = Client->GetEditorViewportWidget();
		if (!EditorViewportWidget.IsValid())
		{
			continue;
		}

		int32 Score = 0;
		if (Client == GCurrentLevelEditingViewportClient)
		{
			Score -= 50;
		}

		if (EditorViewportWidget->HasAnyUserFocus().IsSet())
		{
			Score += 120;
		}
		if (EditorViewportWidget->HasAnyUserFocusOrFocusedDescendants())
		{
			Score += 80;
		}

		const FIntPoint RTSize = Client->Viewport->GetRenderTargetTextureSizeXY();
		const FIntPoint RawSize = Client->Viewport->GetSizeXY();
		if ((RTSize.X > 0 && RTSize.Y > 0) || (RawSize.X > 0 && RawSize.Y > 0))
		{
			Score += 20;
		}

		if (FSlateApplication::IsInitialized())
		{
			const TSharedPtr<SWindow> ViewportWindow = FSlateApplication::Get().FindWidgetWindow(EditorViewportWidget.ToSharedRef());
			if (EditorWindow.IsValid())
			{
				if (ViewportWindow.IsValid() && ViewportWindow == EditorWindow)
				{
					Score += 80;
				}
				else
				{
					Score -= 20;
				}
			}
		}

		if (Score > BestScore)
		{
			BestScore = Score;
			Result.ViewportClient = Client;
			Result.Viewport = Client->Viewport;
		}
	}

	return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Asset Editor Viewport Capture (with orbit camera)
// ─────────────────────────────────────────────────────────────────────────────

FToolResult FScreenshotTool::CaptureEditorViewport(
	FEditorViewportClient* ViewportClient, FViewport* Viewport,
	int32 MaxDimension, const FString& AssetName,
	bool bHasOrbitYaw, double OrbitYaw,
	bool bHasOrbitPitch, double OrbitPitch,
	bool bHasOrbitDistance, double OrbitDistance,
	bool bHasFOV, float FOV,
	int32 WaitForReadyMs,
	bool bHideOverlays)
{
	if (!ViewportClient || !Viewport)
	{
		return FToolResult::Fail(TEXT("Invalid editor viewport for capture"));
	}

	if (!EnsureViewportIsReady(ViewportClient, Viewport))
	{
		const FIntPoint RawSize = Viewport->GetSizeXY();
		return FToolResult::Fail(FString::Printf(
			TEXT("Editor viewport has invalid dimensions (%dx%d). Ensure the viewport tab is visible."),
			RawSize.X, RawSize.Y));
	}

	// ── Apply orbit camera ──
	if (bHasOrbitYaw || bHasOrbitPitch || bHasOrbitDistance)
	{
		// Get current look-at point (orbit center)
		FVector LookAt = ViewportClient->GetViewTransform().GetLookAt();

		// Determine distance
		float Distance;
		if (bHasOrbitDistance && OrbitDistance > 0)
		{
			Distance = static_cast<float>(OrbitDistance);
		}
		else
		{
			Distance = (ViewportClient->GetViewLocation() - LookAt).Size();
			if (Distance < 1.0f)
			{
				Distance = 256.0f; // Safety fallback
			}
		}

		// Use current values for unspecified parameters
		float Yaw = bHasOrbitYaw
			? static_cast<float>(OrbitYaw)
			: ViewportClient->GetViewRotation().Yaw;
		float Pitch = bHasOrbitPitch
			? FMath::Clamp(static_cast<float>(OrbitPitch), -89.0f, 89.0f)
			: ViewportClient->GetViewRotation().Pitch;

		// Spherical to Cartesian offset
		float YawRad = FMath::DegreesToRadians(Yaw);
		float PitchRad = FMath::DegreesToRadians(Pitch);

		FVector Offset;
		Offset.X = Distance * FMath::Cos(PitchRad) * FMath::Cos(YawRad);
		Offset.Y = Distance * FMath::Cos(PitchRad) * FMath::Sin(YawRad);
		Offset.Z = Distance * FMath::Sin(PitchRad);

		FVector NewLocation = LookAt + Offset;
		FRotator NewRotation = (LookAt - NewLocation).Rotation();

		ViewportClient->SetViewLocation(NewLocation);
		ViewportClient->SetViewRotation(NewRotation);
	}

	// ── Apply FOV ──
	if (bHasFOV)
	{
		ViewportClient->ViewFOV = FOV;
	}

	// ── Apply hide_overlays (game view) ──
	bool bWasInGameView = ViewportClient->IsInGameView();
	if (bHideOverlays && !bWasInGameView)
	{
		ViewportClient->SetGameView(true);
	}

	// Scope guard to restore game view on all exit paths
	struct FGameViewRestorer
	{
		FEditorViewportClient* Client;
		bool bShouldRestore;
		~FGameViewRestorer() { if (bShouldRestore) { Client->SetGameView(false); } }
	} GameViewRestorer{ ViewportClient, bHideOverlays && !bWasInGameView };

	WarmupViewportForCapture(ViewportClient, Viewport, WaitForReadyMs, false, false);

	// ── Force redraw and capture ──
	PumpViewportFrame(ViewportClient, Viewport, false);

	FIntPoint RenderTargetSize = Viewport->GetRenderTargetTextureSizeXY();
	int32 ViewWidth = RenderTargetSize.X;
	int32 ViewHeight = RenderTargetSize.Y;

	if (ViewWidth <= 0 || ViewHeight <= 0)
	{
		ViewWidth = Viewport->GetSizeXY().X;
		ViewHeight = Viewport->GetSizeXY().Y;
	}

	if (ViewWidth <= 0 || ViewHeight <= 0)
	{
		return FToolResult::Fail(FString::Printf(
			TEXT("Editor viewport has invalid dimensions (%dx%d). The viewport tab may be collapsed or minimized."),
			ViewWidth, ViewHeight));
	}

	FIntRect CaptureRect(0, 0, ViewWidth, ViewHeight);
	TArray<FColor> Pixels;
	if (!ReadViewportPixelsAsColor(Viewport, CaptureRect, Pixels))
	{
		return FToolResult::Fail(TEXT("Failed to read pixels from editor viewport"));
	}

	// Build response message
	FVector CamLoc = ViewportClient->GetViewLocation();
	FRotator CamRot = ViewportClient->GetViewRotation();

	FString Message = FString::Printf(
		TEXT("Asset editor screenshot captured: %s (from open editor viewport)\n"
			"Camera: Location=(%.1f, %.1f, %.1f) Rotation=(Pitch=%.1f, Yaw=%.1f, Roll=%.1f) FOV=%.1f"),
		*AssetName,
		CamLoc.X, CamLoc.Y, CamLoc.Z,
		CamRot.Pitch, CamRot.Yaw, CamRot.Roll,
		ViewportClient->ViewFOV);

	if (bHideOverlays)
	{
		Message += TEXT("\nOverlays: hidden (game view)");
	}

	return EncodeAndReturn(Pixels, ViewWidth, ViewHeight, MaxDimension, Message);
}

// ─────────────────────────────────────────────────────────────────────────────
// Widget Blueprint Designer Capture
// ─────────────────────────────────────────────────────────────────────────────

FToolResult FScreenshotTool::CaptureWidgetDesigner(UWidgetBlueprint* WidgetBP, int32 MaxDimension)
{
	if (!WidgetBP || !GEditor)
	{
		return FToolResult::Fail(TEXT("Invalid Widget Blueprint"));
	}

	// Ensure the Widget Blueprint editor is open
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditorSubsystem)
	{
		return FToolResult::Fail(TEXT("AssetEditorSubsystem not available"));
	}

	IAssetEditorInstance* EditorInstance = AssetEditorSubsystem->FindEditorForAsset(WidgetBP, true);
	if (!EditorInstance)
	{
		AssetEditorSubsystem->OpenEditorForAsset(WidgetBP);

		// Tick Slate so the editor can initialize
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().Tick();
		}
		FlushRenderingCommands();

		EditorInstance = AssetEditorSubsystem->FindEditorForAsset(WidgetBP, true);
		if (!EditorInstance)
		{
			return FToolResult::Fail(TEXT("Failed to open Widget Blueprint editor"));
		}
	}

	// Find the designer tab ("SlatePreview" is the tab ID for the UMG designer)
	TSharedPtr<FTabManager> TabManager = EditorInstance->GetAssociatedTabManager();
	if (!TabManager.IsValid())
	{
		return FToolResult::Fail(TEXT("Widget Blueprint editor has no tab manager"));
	}

	static const FName DesignerTabID(TEXT("SlatePreview"));
	TSharedPtr<SDockTab> DesignerTab = TabManager->FindExistingLiveTab(FTabId(DesignerTabID));
	if (!DesignerTab.IsValid())
	{
		DesignerTab = TabManager->TryInvokeTab(FTabId(DesignerTabID));
	}

	if (!DesignerTab.IsValid())
	{
		return FToolResult::Fail(TEXT("Could not find or open the Widget Blueprint Designer tab"));
	}

	DesignerTab->ActivateInParent(ETabActivationCause::SetDirectly);

	// Tick a few times to ensure the designer has fully laid out and rendered
	if (FSlateApplication::IsInitialized())
	{
		for (int32 i = 0; i < 3; ++i)
		{
			FSlateApplication::Get().Tick();
			FlushRenderingCommands();
		}
	}

	TSharedRef<SWidget> DesignerContent = DesignerTab->GetContent();

	// Get the desired size from the widget's cached geometry
	FVector2D DesiredSize = DesignerContent->GetDesiredSize();
	if (DesiredSize.X <= 0 || DesiredSize.Y <= 0)
	{
		// Fall back to the tab's paint geometry
		const FGeometry& Geometry = DesignerContent->GetPaintSpaceGeometry();
		DesiredSize = Geometry.GetLocalSize();
	}

	if (DesiredSize.X <= 0 || DesiredSize.Y <= 0)
	{
		return FToolResult::Fail(TEXT("Widget Blueprint Designer has zero size — ensure the Designer tab is visible and has been rendered at least once"));
	}

	// Clamp to reasonable capture dimensions
	const double MaxCaptureSize = 2048.0;
	if (DesiredSize.X > MaxCaptureSize || DesiredSize.Y > MaxCaptureSize)
	{
		double Scale = MaxCaptureSize / FMath::Max(DesiredSize.X, DesiredSize.Y);
		DesiredSize.X = FMath::CeilToDouble(DesiredSize.X * Scale);
		DesiredSize.Y = FMath::CeilToDouble(DesiredSize.Y * Scale);
	}

	// Use FWidgetRenderer to render the designer panel to a render target
	FWidgetRenderer WidgetRenderer(/*bUseGammaCorrection=*/ true);
	UTextureRenderTarget2D* RenderTarget = WidgetRenderer.DrawWidget(DesignerContent, DesiredSize);
	if (!RenderTarget)
	{
		return FToolResult::Fail(TEXT("Failed to render Widget Blueprint designer to texture"));
	}

	FlushRenderingCommands();

	// Read pixels from the render target
	FTextureRenderTargetResource* RenderTargetResource = RenderTarget->GameThread_GetRenderTargetResource();
	if (!RenderTargetResource)
	{
		return FToolResult::Fail(TEXT("Failed to get render target resource from widget capture"));
	}

	TArray<FColor> Pixels;
	if (!RenderTargetResource->ReadPixels(Pixels))
	{
		return FToolResult::Fail(TEXT("Failed to read pixels from widget designer capture"));
	}

	int32 CaptureWidth = static_cast<int32>(DesiredSize.X);
	int32 CaptureHeight = static_cast<int32>(DesiredSize.Y);

	FString Message = FString::Printf(TEXT("Widget Blueprint designer captured: %s (%dx%d)"),
		*WidgetBP->GetName(), CaptureWidth, CaptureHeight);

	return EncodeAndReturn(Pixels, CaptureWidth, CaptureHeight, MaxDimension, Message);
}

// ─────────────────────────────────────────────────────────────────────────────
// Thumbnail Fallback
// ─────────────────────────────────────────────────────────────────────────────

template<typename TScene>
FToolResult FScreenshotTool::CaptureThumbnailScene(
	TScene& ThumbnailScene, int32 Resolution,
	const FString& MeshName, const FString& MeshType)
{
	// Create render target
	UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>();
	RenderTarget->InitCustomFormat(Resolution, Resolution, PF_B8G8R8A8, false);
	RenderTarget->UpdateResourceImmediate(true);

	FTextureRenderTargetResource* RenderTargetResource = RenderTarget->GameThread_GetRenderTargetResource();
	if (!RenderTargetResource)
	{
		return FToolResult::Fail(TEXT("Failed to get render target resource"));
	}

	// Create canvas and clear
	FCanvas Canvas(RenderTargetResource, nullptr, FGameTime::GetTimeSinceAppStart(), GMaxRHIFeatureLevel);
	Canvas.Clear(FLinearColor::Black);
	Canvas.Flush_GameThread();

	// Set up view family
	FSceneViewFamilyContext ViewFamily(
		FSceneViewFamily::ConstructionValues(RenderTargetResource, ThumbnailScene.GetScene(), FEngineShowFlags(ESFIM_Game))
			.SetTime(FGameTime::GetTimeSinceAppStart())
	);

	ViewFamily.EngineShowFlags.DisableAdvancedFeatures();
	ViewFamily.EngineShowFlags.MotionBlur = 0;
	ViewFamily.EngineShowFlags.LOD = 0;
	ViewFamily.SetScreenPercentageInterface(new FLegacyScreenPercentageDriver(ViewFamily, 1.0f));

	// Render
	ThumbnailScene.GetScene()->UpdateSpeedTreeWind(0.0);
	auto* View = ThumbnailScene.CreateView(&ViewFamily, 0, 0, Resolution, Resolution);
	GetRendererModule().BeginRenderingViewFamily(&Canvas, &ViewFamily);
	FlushRenderingCommands();

	// Read pixels
	TArray<FColor> Pixels;
	if (!RenderTargetResource->ReadPixels(Pixels))
	{
		return FToolResult::Fail(TEXT("Failed to read pixels from render target"));
	}

	// Encode to PNG
	TArray64<uint8> PNGData;
	FImageUtils::PNGCompressImageArray(Resolution, Resolution, TArrayView64<const FColor>(Pixels), PNGData);
	if (PNGData.Num() == 0)
	{
		return FToolResult::Fail(TEXT("Failed to encode thumbnail to PNG"));
	}

	const uint32 ThumbPNGSize = static_cast<uint32>(FMath::Min(PNGData.Num(), static_cast<int64>(MAX_uint32)));
	FString Base64 = FBase64::Encode(PNGData.GetData(), ThumbPNGSize);

	FString Message = FString::Printf(TEXT("Thumbnail screenshot captured: %s (%dx%d %s, no editor open - basic thumbnail render)"),
		*MeshName, Resolution, Resolution, *MeshType);

	return FToolResult::OkWithImage(Message, Base64, TEXT("image/png"), Resolution, Resolution);
}
