// Copyright 2025 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "ContentBrowserDelegates.h"

AGENTINTEGRATIONKIT_API DECLARE_LOG_CATEGORY_EXTERN(LogAgentIntegrationKit, Log, All);

/** Plugin build fingerprint — used for crash reports and telemetry correlation.
 *  Format: (major << 24) | (minor << 16) | (patch << 8) | revision
 *  Current: 1.0.0 rev 0x42 */
constexpr uint32 AIK_BUILD_FINGERPRINT = (1 << 24) | (0 << 16) | (0 << 8) | 0x42;

class FToolBarBuilder;
class FMenuBuilder;
class UWebUIBridge;
struct FAssetData;

/** State machine for the auto-update process */
enum class EPluginUpdateState : uint8
{
	None,               // No update activity
	Checking,           // HTTP request in flight to check version
	UpdateAvailable,    // Update found, waiting for user action
	Downloading,        // Zip file downloading from signed URL
	Downloaded,         // Zip saved to disk, ready to install
	Installing,         // Updater script launched, waiting for editor close
	Failed              // Something went wrong (see ErrorMessage)
};

/** Cached result of the plugin version check + download/install state */
struct AGENTINTEGRATIONKIT_API FPluginUpdateInfo
{
	bool bUpdateAvailable = false;
	bool bDownloadAvailable = false;  // Server has a file for this engine+platform
	FString LatestVersion;
	FString Changelog;
	bool bChecked = false;
	bool bDismissed = false;

	// Download/install state
	EPluginUpdateState State = EPluginUpdateState::None;
	FString DownloadUrl;        // Signed GCS URL (temporary, 60 min)
	FString FileName;
	int64 FileSize = 0;
	FString Checksum;           // SHA-256 from server
	FString DownloadedVersion;  // Version string from download response
	FString DownloadedZipPath;  // Local path after download completes
	float DownloadProgress = 0.0f; // 0.0 to 1.0
	FString ErrorMessage;
};

class FAgentIntegrationKitModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Opens the Agent Chat window (WebUI) */
	void OpenAgentChatWindow();

	/** Access the cached update check result (populated async after startup) */
	static const FPluginUpdateInfo& GetUpdateInfo() { return CachedUpdateInfo; }
	static FPluginUpdateInfo& GetUpdateInfoMutable() { return CachedUpdateInfo; }

	/** Fire async HTTP request to check for plugin updates */
	static void CheckForPluginUpdate();

	/** Request signed download URL from betide.studio and download the update zip */
	static void DownloadUpdate();

	/** Generate and launch the platform-specific updater script */
	static void InstallUpdate();

	/** Directory where update zips are cached */
	static FString GetUpdateCacheDir();

	/** Project-local runtime discovery file for external IDE ACP integrations */
	static FString GetProjectDiscoveryFilePath();

	/** Extension module status — tracks which optional plugin integrations are active vs unavailable.
	 *  Populated during StartupModule, queried by BuildDescription to inform agents. */
	struct FExtensionStatus
	{
		FString DisplayName;   // e.g. "PCG", "Niagara", "ControlRig"
		FString ModuleName;    // e.g. "AIK_PCG"
		bool bLoaded;          // true = extension loaded, false = backing plugin not available
	};
	static TArray<FExtensionStatus> ExtensionStatuses;
	static void RegisterExtensionStatus(const FString& DisplayName, const FString& ModuleName, bool bLoaded);

	/** Get the current extension statuses (for tool descriptions, UI, etc.) */
	static const TArray<FExtensionStatus>& GetExtensionStatuses() { return ExtensionStatuses; }

private:
	void RegisterMenus();
	void RegisterNodeContextMenuExtension();
	void RegisterContentBrowserExtension();
	void BindWebUIInputMethodSystem(const TSharedPtr<class SWebBrowser>& Browser);
	void UnbindWebUIInputMethodSystem();

	/** Content Browser menu extender for Blueprint assets */
	TSharedRef<FExtender> OnExtendContentBrowserAssetMenu(const TArray<FAssetData>& SelectedAssets);

	TSharedPtr<class FUICommandList> PluginCommands;

	/** Handle for Content Browser menu extender (for cleanup) */
	FDelegateHandle ContentBrowserExtenderHandle;

	/** Spawn the Agent Chat tab (WebUI) */
	TSharedRef<SDockTab> SpawnAgentChatTab(const FSpawnTabArgs& SpawnTabArgs);

	static const FName AgentChatTabName;
	static FPluginUpdateInfo CachedUpdateInfo;

	/** Bridge UObject for JS<->C++ communication (prevent GC via AddToRoot) */
	UWebUIBridge* WebUIBridgeInstance = nullptr;

	/** Browser window reference for SetParentDockTab lifecycle (macOS z-ordering) */
	TSharedPtr<class IWebBrowserWindow> WebUIBrowserWindow;
	TWeakPtr<SWidget> WebUIBrowserWidget;
	TWeakPtr<class SWebBrowser> WebUIInputMethodBrowserWidget;
	FDelegateHandle InputMethodSystemSlatePreShutdownDelegateHandle;

	/** Local HTTP file server for serving WebUI build (avoids file:// CORS issues on Windows) */
	TSharedPtr<class IHttpRouter> WebUIFileRouter;
	FDelegateHandle WebUIFileServerPreprocessorHandle;
	int32 WebUIFileServerPort = 0;
	FString WebUIBuildDir;
	bool StartWebUIFileServer(const FString& BuildDir);
	void StopWebUIFileServer();

	/** Project-local runtime discovery for external IDE integrations */
	void WriteProjectDiscoveryFile();
	void RemoveProjectDiscoveryFile();
	FTSTicker::FDelegateHandle ProjectDiscoveryTickerHandle;
	FString ProjectDiscoveryInstanceId;
	FDateTime ProjectDiscoveryStartedAt;
};
