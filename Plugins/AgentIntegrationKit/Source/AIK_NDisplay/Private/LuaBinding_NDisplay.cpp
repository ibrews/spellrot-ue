// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#if !defined(AIK_NDISPLAY_DISABLED) || !AIK_NDISPLAY_DISABLED

#include "Lua/LuaBindingRegistry.h"
#include <sol/sol.hpp>
#include "Tools/NeoStackToolUtils.h"

#include "Modules/ModuleManager.h"
#include "DisplayClusterConfigurationTypes.h"
#include "DisplayClusterConfigurationTypes_Viewport.h"
#include "DisplayClusterConfigurationTypes_ICVFX.h"
#include "ClusterConfiguration/DisplayClusterConfiguratorClusterUtils.h"
#include "IDisplayClusterConfiguration.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Editor.h"
#include "ScopedTransaction.h"

// ─── Documentation ───

static TArray<FLuaFunctionDoc> NDisplayDocs = {
	{ TEXT("ndisplay_get_config(actor_label?)"), TEXT("Get nDisplay configuration data from the level — returns node_count, primary_address, primary_node, custom_parameters, render_frame, stage_settings, booleans"), TEXT("table or nil") },
	{ TEXT("ndisplay_list_nodes(actor_label?)"), TEXT("List all cluster nodes with host, fullscreen, sound, window_rect, headless, graphics_adapter, viewport_count"), TEXT("table[]") },
	{ TEXT("ndisplay_list_viewports(node_id, actor_label?)"), TEXT("List all viewports with region, gpu_index, camera, overlap_order, buffer_ratio, projection_policy"), TEXT("table[]") },
	{ TEXT("ndisplay_add_node(params)"), TEXT("Add a new cluster node — params: name, host, window_x, window_y, window_w, window_h, fullscreen, sound_enabled, actor_label"), TEXT("string or nil") },
	{ TEXT("ndisplay_remove_node(node_id, actor_label?)"), TEXT("Remove a cluster node from the configuration"), TEXT("true or nil") },
	{ TEXT("ndisplay_rename_node(node_id, new_name, actor_label?)"), TEXT("Rename a cluster node"), TEXT("true or nil") },
	{ TEXT("ndisplay_set_primary_node(node_id, actor_label?)"), TEXT("Set the specified node as the primary cluster node"), TEXT("true or nil") },
	{ TEXT("ndisplay_configure_node(node_id, params, actor_label?)"), TEXT("Configure node properties — params: host, fullscreen, sound_enabled, headless, graphics_adapter, window_x, window_y, window_w, window_h"), TEXT("true or nil") },
	{ TEXT("ndisplay_add_viewport(params)"), TEXT("Add a viewport to a cluster node — params: node_id, name, x, y, w, h, gpu_index, camera, actor_label"), TEXT("string or nil") },
	{ TEXT("ndisplay_remove_viewport(node_id, viewport_id, actor_label?)"), TEXT("Remove a viewport from a cluster node"), TEXT("true or nil") },
	{ TEXT("ndisplay_rename_viewport(node_id, viewport_id, new_name, actor_label?)"), TEXT("Rename a viewport"), TEXT("true or nil") },
	{ TEXT("ndisplay_configure_viewport(node_id, viewport_id, params, actor_label?)"), TEXT("Configure viewport — params: enabled, x, y, w, h, gpu_index, camera, overlap_order, buffer_ratio"), TEXT("true or nil") },
	{ TEXT("ndisplay_get_projection_policy(node_id, viewport_id, actor_label?)"), TEXT("Get the projection policy for a viewport — returns type + parameters"), TEXT("table or nil") },
	{ TEXT("ndisplay_get_custom_params(actor_label?)"), TEXT("Get all custom parameters from the nDisplay configuration"), TEXT("table or nil") },
	{ TEXT("ndisplay_set_custom_params(params, actor_label?)"), TEXT("Set custom parameters — params: key=value pairs, clear_existing (optional bool)"), TEXT("true or nil") },
	{ TEXT("ndisplay_configure_render_frame(params, actor_label?)"), TEXT("Configure render frame — params: rtt_multiplier, inner_rtt_multiplier, outer_rtt_multiplier, screen_pct_multiplier, inner_screen_pct_multiplier, outer_screen_pct_multiplier, warp_blend"), TEXT("true or nil") },
	{ TEXT("ndisplay_assign_postprocess(params)"), TEXT("Assign/update a postprocess — params: node_id, postprocess_id, type, parameters (table), order, actor_label"), TEXT("true or nil") },
	{ TEXT("ndisplay_remove_postprocess(node_id, postprocess_id, actor_label?)"), TEXT("Remove a postprocess from a cluster node"), TEXT("true or nil") },
	{ TEXT("ndisplay_save_config(file_path, actor_label?)"), TEXT("Save the nDisplay configuration to a JSON file"), TEXT("true or nil") },
	{ TEXT("ndisplay_load_config(file_path)"), TEXT("Load an nDisplay configuration from a JSON file"), TEXT("string or nil") },
};

// ─── Helpers ───

static UDisplayClusterConfigurationData* FindNDisplayConfig(FLuaSessionData& Session, const std::string& ActorLabel = "")
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) return nullptr;

	FString FilterLabel = ActorLabel.empty() ? FString() : UTF8_TO_TCHAR(ActorLabel.c_str());

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!FilterLabel.IsEmpty() && Actor->GetActorLabel() != FilterLabel)
			continue;

		UDisplayClusterConfigurationData* Config = nullptr;
		ForEachObjectWithOuter(Actor, [&Config](UObject* Obj)
		{
			if (!Config)
			{
				Config = Cast<UDisplayClusterConfigurationData>(Obj);
			}
		});

		if (Config) return Config;
	}

	// Fallback: search all UDisplayClusterConfigurationData objects
	for (TObjectIterator<UDisplayClusterConfigurationData> It; It; ++It)
	{
		if (!FilterLabel.IsEmpty())
		{
			AActor* Owner = It->GetTypedOuter<AActor>();
			if (Owner && Owner->GetActorLabel() != FilterLabel)
				continue;
		}
		return *It;
	}

	return nullptr;
}

static void NDisplay_PopulatePrimaryNode(sol::table& Result, UDisplayClusterConfigurationData* Config)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	UDisplayClusterConfigurationClusterNode* Primary = Config->GetPrimaryNode();
	if (Primary)
	{
		Result["primary_node"] = std::string(TCHAR_TO_UTF8(*UE::DisplayClusterConfiguratorClusterUtils::GetClusterNodeName(Primary)));
	}
#endif
}

static UDisplayClusterConfigurationClusterNode* NDisplay_GetPrimaryNode(UDisplayClusterConfigurationData* Config)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	return Config->GetPrimaryNode();
#else
	return nullptr;
#endif
}

// ─── Binding ───

static void BindNDisplay(sol::state& Lua, FLuaSessionData& Session)
{
	// ---- ndisplay_get_config(actor_label?) ----
	Lua.set_function("ndisplay_get_config", [&Session](sol::optional<std::string> ActorLabel, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		std::string Label = ActorLabel.value_or("");

		UDisplayClusterConfigurationData* Config = FindNDisplayConfig(Session, Label);
		if (!Config || !Config->Cluster)
		{
			Session.Log(TEXT("[FAIL] ndisplay_get_config -> no nDisplay configuration found"));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();
		Result["node_count"] = Config->GetNumberOfClusterNodes();
		Result["primary_address"] = std::string(TCHAR_TO_UTF8(*Config->GetPrimaryNodeAddress()));

		NDisplay_PopulatePrimaryNode(Result, Config);

		Result["follow_local_player_camera"] = Config->bFollowLocalPlayerCamera;
		Result["exit_on_esc"] = Config->bExitOnEsc;
		Result["override_viewports_from_external"] = Config->bOverrideViewportsFromExternalConfig;
		Result["override_transforms_from_external"] = Config->bOverrideTransformsFromExternalConfig;

		// Info
		Result["description"] = std::string(TCHAR_TO_UTF8(*Config->Info.Description));
		Result["version"] = std::string(TCHAR_TO_UTF8(*Config->Info.Version));

		// Custom parameters
		if (Config->CustomParameters.Num() > 0)
		{
			sol::table Params = Lua.create_table();
			for (const auto& Pair : Config->CustomParameters)
			{
				Params[std::string(TCHAR_TO_UTF8(*Pair.Key))] = std::string(TCHAR_TO_UTF8(*Pair.Value));
			}
			Result["custom_parameters"] = Params;
		}

		// Render frame settings
		{
			sol::table RF = Lua.create_table();
			RF["rtt_multiplier"] = Config->RenderFrameSettings.ClusterRenderTargetRatioMult;
			RF["inner_rtt_multiplier"] = Config->RenderFrameSettings.ClusterICVFXInnerViewportRenderTargetRatioMult;
			RF["outer_rtt_multiplier"] = Config->RenderFrameSettings.ClusterICVFXOuterViewportRenderTargetRatioMult;
			RF["screen_pct_multiplier"] = Config->RenderFrameSettings.ClusterBufferRatioMult;
			RF["inner_screen_pct_multiplier"] = Config->RenderFrameSettings.ClusterICVFXInnerFrustumBufferRatioMult;
			RF["outer_screen_pct_multiplier"] = Config->RenderFrameSettings.ClusterICVFXOuterViewportBufferRatioMult;
			RF["warp_blend"] = Config->RenderFrameSettings.bAllowWarpBlend;
			Result["render_frame"] = RF;
		}

		// Network settings
		{
			const auto& Net = Config->Cluster->Network;
			sol::table NetTbl = Lua.create_table();
			NetTbl["connect_retries"] = Net.ConnectRetriesAmount;
			NetTbl["connect_retry_delay"] = Net.ConnectRetryDelay;
			NetTbl["game_start_barrier_timeout"] = Net.GameStartBarrierTimeout;
			NetTbl["frame_start_barrier_timeout"] = Net.FrameStartBarrierTimeout;
			NetTbl["frame_end_barrier_timeout"] = Net.FrameEndBarrierTimeout;
			NetTbl["render_sync_barrier_timeout"] = Net.RenderSyncBarrierTimeout;
			Result["network"] = NetTbl;
		}

		// Stage settings (ICVFX) summary
		{
			sol::table SS = Lua.create_table();
			SS["enable_inner_frustums"] = Config->StageSettings.bEnableInnerFrustums;
			SS["enable_inner_frustum_chromakey_overlap"] = Config->StageSettings.bEnableInnerFrustumChromakeyOverlap;
			SS["freeze_outer_viewports"] = Config->StageSettings.bFreezeRenderOuterViewports;
			SS["enable_color_grading"] = Config->StageSettings.EnableColorGrading;
			Result["stage_settings"] = SS;
		}

		// Diagnostics
		{
			sol::table Diag = Lua.create_table();
			Diag["simulate_lag"] = Config->Diagnostics.bSimulateLag;
			Diag["min_lag_time"] = Config->Diagnostics.MinLagTime;
			Diag["max_lag_time"] = Config->Diagnostics.MaxLagTime;
			Result["diagnostics"] = Diag;
		}

		Session.Log(FString::Printf(TEXT("[OK] ndisplay_get_config -> %d nodes"), Config->GetNumberOfClusterNodes()));
		return sol::make_object(Lua, Result);
	});

	// ---- ndisplay_list_nodes(actor_label?) ----
	Lua.set_function("ndisplay_list_nodes", [&Session](sol::optional<std::string> ActorLabel, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDisplayClusterConfigurationData* Config = FindNDisplayConfig(Session, ActorLabel.value_or(""));
		if (!Config || !Config->Cluster)
		{
			Session.Log(TEXT("[FAIL] ndisplay_list_nodes -> no nDisplay configuration found"));
			return sol::lua_nil;
		}

		UDisplayClusterConfigurationClusterNode* PrimaryNode = NDisplay_GetPrimaryNode(Config);

		sol::table Result = Lua.create_table();
		int32 Idx = 1;
		for (const auto& Pair : Config->Cluster->Nodes)
		{
			UDisplayClusterConfigurationClusterNode* Node = Pair.Value;
			if (!Node) continue;

			sol::table Entry = Lua.create_table();
			Entry["id"] = std::string(TCHAR_TO_UTF8(*Pair.Key));
			Entry["host"] = std::string(TCHAR_TO_UTF8(*Node->Host));
			Entry["is_primary"] = (Node == PrimaryNode);
			Entry["is_fullscreen"] = Node->bIsFullscreen;
			Entry["sound_enabled"] = Node->bIsSoundEnabled;
			Entry["headless"] = Node->bRenderHeadless;
			Entry["graphics_adapter"] = Node->GraphicsAdapter;
			Entry["texture_share"] = Node->bEnableTextureShare;

			// Window rect
			Entry["window_x"] = Node->WindowRect.X;
			Entry["window_y"] = Node->WindowRect.Y;
			Entry["window_w"] = Node->WindowRect.W;
			Entry["window_h"] = Node->WindowRect.H;

			TArray<FString> ViewportIds;
			Node->GetViewportIds(ViewportIds);
			Entry["viewport_count"] = ViewportIds.Num();

			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] ndisplay_list_nodes -> %d nodes"), Idx - 1));
		return sol::make_object(Lua, Result);
	});

	// ---- ndisplay_list_viewports(node_id, actor_label?) ----
	Lua.set_function("ndisplay_list_viewports", [&Session](const std::string& NodeId, sol::optional<std::string> ActorLabel, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDisplayClusterConfigurationData* Config = FindNDisplayConfig(Session, ActorLabel.value_or(""));
		if (!Config || !Config->Cluster)
		{
			Session.Log(TEXT("[FAIL] ndisplay_list_viewports -> no nDisplay configuration found"));
			return sol::lua_nil;
		}

		FString FNodeId = UTF8_TO_TCHAR(NodeId.c_str());
		UDisplayClusterConfigurationClusterNode* Node = Config->Cluster->GetNode(FNodeId);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] ndisplay_list_viewports -> node '%s' not found"), *FNodeId));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();
		int32 Idx = 1;
		for (const auto& Pair : Node->Viewports)
		{
			UDisplayClusterConfigurationViewport* VP = Pair.Value;
			if (!VP) continue;

			sol::table Entry = Lua.create_table();
			Entry["id"] = std::string(TCHAR_TO_UTF8(*Pair.Key));
			Entry["enabled"] = VP->bAllowRendering;
			Entry["region_x"] = VP->Region.X;
			Entry["region_y"] = VP->Region.Y;
			Entry["region_w"] = VP->Region.W;
			Entry["region_h"] = VP->Region.H;
			Entry["gpu_index"] = VP->GPUIndex;
			Entry["camera"] = std::string(TCHAR_TO_UTF8(*VP->Camera));
			Entry["overlap_order"] = VP->OverlapOrder;
			Entry["buffer_ratio"] = VP->RenderSettings.BufferRatio;
			Entry["cross_gpu_transfer"] = VP->RenderSettings.bEnableCrossGPUTransfer;

			// Projection policy
			Entry["projection_type"] = std::string(TCHAR_TO_UTF8(*VP->ProjectionPolicy.Type));

			// ICVFX per-viewport
			Entry["icvfx_enabled"] = VP->ICVFX.bAllowICVFX;
			Entry["icvfx_inner_frustum"] = VP->ICVFX.bAllowInnerFrustum;

			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] ndisplay_list_viewports -> %d viewports on node %s"), Idx - 1, *FNodeId));
		return sol::make_object(Lua, Result);
	});

	// ---- ndisplay_add_node(params) ----
	Lua.set_function("ndisplay_add_node", [&Session](sol::table Params, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		std::string Name = Params.get_or<std::string>("name", "NewNode");
		std::string Host = Params.get_or<std::string>("host", "127.0.0.1");
		std::string Label = Params.get_or<std::string>("actor_label", "");

		UDisplayClusterConfigurationData* Config = FindNDisplayConfig(Session, Label);
		if (!Config || !Config->Cluster)
		{
			Session.Log(TEXT("[FAIL] ndisplay_add_node -> no nDisplay configuration found"));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("nDisplay: Add Cluster Node")));

		UDisplayClusterConfigurationClusterNode* NewNode = NewObject<UDisplayClusterConfigurationClusterNode>(Config->Cluster);
		NewNode->Host = UTF8_TO_TCHAR(Host.c_str());
		NewNode->WindowRect.X = Params.get_or("window_x", 0);
		NewNode->WindowRect.Y = Params.get_or("window_y", 0);
		NewNode->WindowRect.W = Params.get_or("window_w", 1920);
		NewNode->WindowRect.H = Params.get_or("window_h", 1080);
		NewNode->bIsFullscreen = Params.get_or("fullscreen", false);
		NewNode->bIsSoundEnabled = Params.get_or("sound_enabled", false);

		FString NodeName = UTF8_TO_TCHAR(Name.c_str());
		UDisplayClusterConfigurationClusterNode* Added = UE::DisplayClusterConfiguratorClusterUtils::AddClusterNodeToCluster(
			NewNode, Config->Cluster, NodeName);

		if (!Added)
		{
			Session.Log(TEXT("[FAIL] ndisplay_add_node -> failed to add node"));
			return sol::lua_nil;
		}

		Config->MarkPackageDirty();

		FString ActualName = UE::DisplayClusterConfiguratorClusterUtils::GetClusterNodeName(Added);
		Session.Log(FString::Printf(TEXT("[OK] ndisplay_add_node -> added node '%s' at %s"), *ActualName, *Added->Host));
		return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*ActualName)));
	});

	// ---- ndisplay_remove_node(node_id, actor_label?) ----
	Lua.set_function("ndisplay_remove_node", [&Session](const std::string& NodeId, sol::optional<std::string> ActorLabel, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDisplayClusterConfigurationData* Config = FindNDisplayConfig(Session, ActorLabel.value_or(""));
		if (!Config || !Config->Cluster)
		{
			Session.Log(TEXT("[FAIL] ndisplay_remove_node -> no nDisplay configuration found"));
			return sol::lua_nil;
		}

		FString FNodeId = UTF8_TO_TCHAR(NodeId.c_str());
		UDisplayClusterConfigurationClusterNode* Node = Config->Cluster->GetNode(FNodeId);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] ndisplay_remove_node -> node '%s' not found"), *FNodeId));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("nDisplay: Remove Cluster Node")));

		bool bRemoved = UE::DisplayClusterConfiguratorClusterUtils::RemoveClusterNodeFromCluster(Node);
		if (!bRemoved)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] ndisplay_remove_node -> failed to remove '%s'"), *FNodeId));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] ndisplay_remove_node -> removed '%s'"), *FNodeId));
		return sol::make_object(Lua, true);
	});

	// ---- ndisplay_rename_node(node_id, new_name, actor_label?) ----
	Lua.set_function("ndisplay_rename_node", [&Session](const std::string& NodeId, const std::string& NewName, sol::optional<std::string> ActorLabel, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDisplayClusterConfigurationData* Config = FindNDisplayConfig(Session, ActorLabel.value_or(""));
		if (!Config || !Config->Cluster)
		{
			Session.Log(TEXT("[FAIL] ndisplay_rename_node -> no nDisplay configuration found"));
			return sol::lua_nil;
		}

		FString FNodeId = UTF8_TO_TCHAR(NodeId.c_str());
		UDisplayClusterConfigurationClusterNode* Node = Config->Cluster->GetNode(FNodeId);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] ndisplay_rename_node -> node '%s' not found"), *FNodeId));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("nDisplay: Rename Cluster Node")));

		FString FNewName = UTF8_TO_TCHAR(NewName.c_str());
		bool bRenamed = UE::DisplayClusterConfiguratorClusterUtils::RenameClusterNode(Node, FNewName);
		if (!bRenamed)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] ndisplay_rename_node -> failed to rename '%s'"), *FNodeId));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] ndisplay_rename_node -> renamed '%s' to '%s'"), *FNodeId, *FNewName));
		return sol::make_object(Lua, true);
	});

	// ---- ndisplay_set_primary_node(node_id, actor_label?) ----
	Lua.set_function("ndisplay_set_primary_node", [&Session](const std::string& NodeId, sol::optional<std::string> ActorLabel, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDisplayClusterConfigurationData* Config = FindNDisplayConfig(Session, ActorLabel.value_or(""));
		if (!Config || !Config->Cluster)
		{
			Session.Log(TEXT("[FAIL] ndisplay_set_primary_node -> no nDisplay configuration found"));
			return sol::lua_nil;
		}

		FString FNodeId = UTF8_TO_TCHAR(NodeId.c_str());
		UDisplayClusterConfigurationClusterNode* Node = Config->Cluster->GetNode(FNodeId);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] ndisplay_set_primary_node -> node '%s' not found"), *FNodeId));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("nDisplay: Set Primary Node")));

		bool bSet = UE::DisplayClusterConfiguratorClusterUtils::SetClusterNodeAsPrimary(Node);
		if (!bSet)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] ndisplay_set_primary_node -> failed for '%s'"), *FNodeId));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] ndisplay_set_primary_node -> '%s' is now primary"), *FNodeId));
		return sol::make_object(Lua, true);
	});

	// ---- ndisplay_configure_node(node_id, params, actor_label?) ----
	Lua.set_function("ndisplay_configure_node", [&Session](const std::string& NodeId, sol::table Params, sol::optional<std::string> ActorLabel, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDisplayClusterConfigurationData* Config = FindNDisplayConfig(Session, ActorLabel.value_or(""));
		if (!Config || !Config->Cluster)
		{
			Session.Log(TEXT("[FAIL] ndisplay_configure_node -> no nDisplay configuration found"));
			return sol::lua_nil;
		}

		FString FNodeId = UTF8_TO_TCHAR(NodeId.c_str());
		UDisplayClusterConfigurationClusterNode* Node = Config->Cluster->GetNode(FNodeId);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] ndisplay_configure_node -> node '%s' not found"), *FNodeId));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("nDisplay: Configure Cluster Node")));
		Node->Modify();

		if (auto V = Params.get<sol::optional<std::string>>("host"))
			Node->Host = UTF8_TO_TCHAR(V->c_str());
		if (auto V = Params.get<sol::optional<bool>>("fullscreen"))
			Node->bIsFullscreen = *V;
		if (auto V = Params.get<sol::optional<bool>>("sound_enabled"))
			Node->bIsSoundEnabled = *V;
		if (auto V = Params.get<sol::optional<bool>>("headless"))
			Node->bRenderHeadless = *V;
		if (auto V = Params.get<sol::optional<int>>("graphics_adapter"))
			Node->GraphicsAdapter = *V;
		if (auto V = Params.get<sol::optional<bool>>("texture_share"))
			Node->bEnableTextureShare = *V;
		if (auto V = Params.get<sol::optional<int>>("window_x"))
			Node->WindowRect.X = *V;
		if (auto V = Params.get<sol::optional<int>>("window_y"))
			Node->WindowRect.Y = *V;
		if (auto V = Params.get<sol::optional<int>>("window_w"))
			Node->WindowRect.W = *V;
		if (auto V = Params.get<sol::optional<int>>("window_h"))
			Node->WindowRect.H = *V;

		Node->MarkPackageDirty();
		Session.Log(FString::Printf(TEXT("[OK] ndisplay_configure_node -> configured '%s'"), *FNodeId));
		return sol::make_object(Lua, true);
	});

	// ---- ndisplay_add_viewport(params) ----
	Lua.set_function("ndisplay_add_viewport", [&Session](sol::table Params, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		std::string NodeId = Params.get_or<std::string>("node_id", "");
		std::string Name = Params.get_or<std::string>("name", "NewViewport");
		std::string Label = Params.get_or<std::string>("actor_label", "");

		if (NodeId.empty())
		{
			Session.Log(TEXT("[FAIL] ndisplay_add_viewport -> node_id required"));
			return sol::lua_nil;
		}

		UDisplayClusterConfigurationData* Config = FindNDisplayConfig(Session, Label);
		if (!Config || !Config->Cluster)
		{
			Session.Log(TEXT("[FAIL] ndisplay_add_viewport -> no nDisplay configuration found"));
			return sol::lua_nil;
		}

		FString FNodeId = UTF8_TO_TCHAR(NodeId.c_str());
		UDisplayClusterConfigurationClusterNode* Node = Config->Cluster->GetNode(FNodeId);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] ndisplay_add_viewport -> node '%s' not found"), *FNodeId));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("nDisplay: Add Viewport")));

		UDisplayClusterConfigurationViewport* NewVP = NewObject<UDisplayClusterConfigurationViewport>(Node);
		NewVP->Region.X = Params.get_or("x", 0);
		NewVP->Region.Y = Params.get_or("y", 0);
		NewVP->Region.W = Params.get_or("w", 1920);
		NewVP->Region.H = Params.get_or("h", 1080);
		NewVP->bAllowRendering = true;

		if (auto V = Params.get<sol::optional<int>>("gpu_index"))
			NewVP->GPUIndex = *V;
		if (auto V = Params.get<sol::optional<std::string>>("camera"))
			NewVP->Camera = UTF8_TO_TCHAR(V->c_str());

		FString VPName = UTF8_TO_TCHAR(Name.c_str());
		UDisplayClusterConfigurationViewport* Added = UE::DisplayClusterConfiguratorClusterUtils::AddViewportToClusterNode(
			NewVP, Node, VPName);

		if (!Added)
		{
			Session.Log(TEXT("[FAIL] ndisplay_add_viewport -> failed to add viewport"));
			return sol::lua_nil;
		}

		FString ActualName = UE::DisplayClusterConfiguratorClusterUtils::GetViewportName(Added);
		Session.Log(FString::Printf(TEXT("[OK] ndisplay_add_viewport -> added '%s' to node '%s'"), *ActualName, *FNodeId));
		return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*ActualName)));
	});

	// ---- ndisplay_remove_viewport(node_id, viewport_id, actor_label?) ----
	Lua.set_function("ndisplay_remove_viewport", [&Session](const std::string& NodeId, const std::string& ViewportId, sol::optional<std::string> ActorLabel, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDisplayClusterConfigurationData* Config = FindNDisplayConfig(Session, ActorLabel.value_or(""));
		if (!Config || !Config->Cluster)
		{
			Session.Log(TEXT("[FAIL] ndisplay_remove_viewport -> no nDisplay configuration found"));
			return sol::lua_nil;
		}

		FString FNodeId = UTF8_TO_TCHAR(NodeId.c_str());
		FString FVPId = UTF8_TO_TCHAR(ViewportId.c_str());

		UDisplayClusterConfigurationViewport* VP = Config->GetViewport(FNodeId, FVPId);
		if (!VP)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] ndisplay_remove_viewport -> viewport '%s' not found on node '%s'"), *FVPId, *FNodeId));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("nDisplay: Remove Viewport")));

		bool bRemoved = UE::DisplayClusterConfiguratorClusterUtils::RemoveViewportFromClusterNode(VP);
		if (!bRemoved)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] ndisplay_remove_viewport -> failed to remove '%s'"), *FVPId));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] ndisplay_remove_viewport -> removed '%s' from '%s'"), *FVPId, *FNodeId));
		return sol::make_object(Lua, true);
	});

	// ---- ndisplay_rename_viewport(node_id, viewport_id, new_name, actor_label?) ----
	Lua.set_function("ndisplay_rename_viewport", [&Session](const std::string& NodeId, const std::string& ViewportId, const std::string& NewName, sol::optional<std::string> ActorLabel, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDisplayClusterConfigurationData* Config = FindNDisplayConfig(Session, ActorLabel.value_or(""));
		if (!Config || !Config->Cluster)
		{
			Session.Log(TEXT("[FAIL] ndisplay_rename_viewport -> no nDisplay configuration found"));
			return sol::lua_nil;
		}

		FString FNodeId = UTF8_TO_TCHAR(NodeId.c_str());
		FString FVPId = UTF8_TO_TCHAR(ViewportId.c_str());

		UDisplayClusterConfigurationViewport* VP = Config->GetViewport(FNodeId, FVPId);
		if (!VP)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] ndisplay_rename_viewport -> viewport '%s' not found on node '%s'"), *FVPId, *FNodeId));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("nDisplay: Rename Viewport")));

		FString FNewName = UTF8_TO_TCHAR(NewName.c_str());
		bool bRenamed = UE::DisplayClusterConfiguratorClusterUtils::RenameViewport(VP, FNewName);
		if (!bRenamed)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] ndisplay_rename_viewport -> failed to rename '%s'"), *FVPId));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] ndisplay_rename_viewport -> renamed '%s' to '%s'"), *FVPId, *FNewName));
		return sol::make_object(Lua, true);
	});

	// ---- ndisplay_configure_viewport(node_id, viewport_id, params, actor_label?) ----
	Lua.set_function("ndisplay_configure_viewport", [&Session](const std::string& NodeId, const std::string& ViewportId, sol::table Params, sol::optional<std::string> ActorLabel, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDisplayClusterConfigurationData* Config = FindNDisplayConfig(Session, ActorLabel.value_or(""));
		if (!Config || !Config->Cluster)
		{
			Session.Log(TEXT("[FAIL] ndisplay_configure_viewport -> no nDisplay configuration found"));
			return sol::lua_nil;
		}

		FString FNodeId = UTF8_TO_TCHAR(NodeId.c_str());
		FString FVPId = UTF8_TO_TCHAR(ViewportId.c_str());

		UDisplayClusterConfigurationViewport* VP = Config->GetViewport(FNodeId, FVPId);
		if (!VP)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] ndisplay_configure_viewport -> viewport '%s' not found on node '%s'"), *FVPId, *FNodeId));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("nDisplay: Configure Viewport")));
		VP->Modify();

		if (auto V = Params.get<sol::optional<bool>>("enabled"))
			VP->bAllowRendering = *V;
		if (auto V = Params.get<sol::optional<int>>("x"))
			VP->Region.X = *V;
		if (auto V = Params.get<sol::optional<int>>("y"))
			VP->Region.Y = *V;
		if (auto V = Params.get<sol::optional<int>>("w"))
			VP->Region.W = *V;
		if (auto V = Params.get<sol::optional<int>>("h"))
			VP->Region.H = *V;
		if (auto V = Params.get<sol::optional<int>>("gpu_index"))
			VP->GPUIndex = *V;
		if (auto V = Params.get<sol::optional<std::string>>("camera"))
			VP->Camera = UTF8_TO_TCHAR(V->c_str());
		if (auto V = Params.get<sol::optional<int>>("overlap_order"))
			VP->OverlapOrder = *V;
		if (auto V = Params.get<sol::optional<double>>("buffer_ratio"))
			VP->RenderSettings.BufferRatio = static_cast<float>(*V);
		if (auto V = Params.get<sol::optional<bool>>("cross_gpu_transfer"))
			VP->RenderSettings.bEnableCrossGPUTransfer = *V;

		// Per-viewport ICVFX
		if (auto V = Params.get<sol::optional<bool>>("icvfx_enabled"))
			VP->ICVFX.bAllowICVFX = *V;
		if (auto V = Params.get<sol::optional<bool>>("icvfx_inner_frustum"))
			VP->ICVFX.bAllowInnerFrustum = *V;

		VP->MarkPackageDirty();
		Session.Log(FString::Printf(TEXT("[OK] ndisplay_configure_viewport -> configured '%s' on '%s'"), *FVPId, *FNodeId));
		return sol::make_object(Lua, true);
	});

	// ---- ndisplay_get_projection_policy(node_id, viewport_id, actor_label?) ----
	Lua.set_function("ndisplay_get_projection_policy", [&Session](const std::string& NodeId, const std::string& ViewportId, sol::optional<std::string> ActorLabel, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDisplayClusterConfigurationData* Config = FindNDisplayConfig(Session, ActorLabel.value_or(""));
		if (!Config || !Config->Cluster)
		{
			Session.Log(TEXT("[FAIL] ndisplay_get_projection_policy -> no nDisplay configuration found"));
			return sol::lua_nil;
		}

		FString FNodeId = UTF8_TO_TCHAR(NodeId.c_str());
		FString FVPId = UTF8_TO_TCHAR(ViewportId.c_str());

		FDisplayClusterConfigurationProjection OutProjection;
		if (!Config->GetProjectionPolicy(FNodeId, FVPId, OutProjection))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] ndisplay_get_projection_policy -> not found for '%s'/'%s'"), *FNodeId, *FVPId));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();
		Result["type"] = std::string(TCHAR_TO_UTF8(*OutProjection.Type));

		if (OutProjection.Parameters.Num() > 0)
		{
			sol::table ParamsTable = Lua.create_table();
			for (const auto& Pair : OutProjection.Parameters)
			{
				ParamsTable[std::string(TCHAR_TO_UTF8(*Pair.Key))] = std::string(TCHAR_TO_UTF8(*Pair.Value));
			}
			Result["parameters"] = ParamsTable;
		}

		Session.Log(FString::Printf(TEXT("[OK] ndisplay_get_projection_policy -> type='%s' for '%s'/'%s'"), *OutProjection.Type, *FNodeId, *FVPId));
		return sol::make_object(Lua, Result);
	});

	// ---- ndisplay_get_custom_params(actor_label?) ----
	Lua.set_function("ndisplay_get_custom_params", [&Session](sol::optional<std::string> ActorLabel, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDisplayClusterConfigurationData* Config = FindNDisplayConfig(Session, ActorLabel.value_or(""));
		if (!Config)
		{
			Session.Log(TEXT("[FAIL] ndisplay_get_custom_params -> no nDisplay configuration found"));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();
		for (const auto& Pair : Config->CustomParameters)
		{
			Result[std::string(TCHAR_TO_UTF8(*Pair.Key))] = std::string(TCHAR_TO_UTF8(*Pair.Value));
		}

		Session.Log(FString::Printf(TEXT("[OK] ndisplay_get_custom_params -> %d parameters"), Config->CustomParameters.Num()));
		return sol::make_object(Lua, Result);
	});

	// ---- ndisplay_set_custom_params(params, actor_label?) ----
	Lua.set_function("ndisplay_set_custom_params", [&Session](sol::table Params, sol::optional<std::string> ActorLabel, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		std::string LabelDefault = ActorLabel.value_or("");
		std::string Label = Params.get_or("actor_label", LabelDefault);

		UDisplayClusterConfigurationData* Config = FindNDisplayConfig(Session, Label);
		if (!Config)
		{
			Session.Log(TEXT("[FAIL] ndisplay_set_custom_params -> no nDisplay configuration found"));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("nDisplay: Set Custom Parameters")));
		Config->Modify();

		bool bClearExisting = Params.get_or("clear_existing", false);
		if (bClearExisting)
		{
			Config->CustomParameters.Empty();
		}

		int32 Count = 0;
		for (const auto& Pair : Params)
		{
			if (!Pair.first.is<std::string>()) continue;
			std::string Key = Pair.first.as<std::string>();
			if (Key == "actor_label" || Key == "clear_existing") continue;
			if (!Pair.second.is<std::string>()) continue;

			Config->CustomParameters.Add(UTF8_TO_TCHAR(Key.c_str()), UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str()));
			Count++;
		}

		Config->MarkPackageDirty();
		Session.Log(FString::Printf(TEXT("[OK] ndisplay_set_custom_params -> set %d parameters"), Count));
		return sol::make_object(Lua, true);
	});

	// ---- ndisplay_configure_render_frame(params, actor_label?) ----
	Lua.set_function("ndisplay_configure_render_frame", [&Session](sol::table Params, sol::optional<std::string> ActorLabel, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		std::string LabelDefault = ActorLabel.value_or("");
		std::string Label = Params.get_or("actor_label", LabelDefault);

		UDisplayClusterConfigurationData* Config = FindNDisplayConfig(Session, Label);
		if (!Config)
		{
			Session.Log(TEXT("[FAIL] ndisplay_configure_render_frame -> no nDisplay configuration found"));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("nDisplay: Configure Render Frame")));
		Config->Modify();

		FDisplayClusterConfigurationRenderFrame& RF = Config->RenderFrameSettings;

		if (auto V = Params.get<sol::optional<double>>("rtt_multiplier"))
			RF.ClusterRenderTargetRatioMult = static_cast<float>(*V);
		if (auto V = Params.get<sol::optional<double>>("inner_rtt_multiplier"))
			RF.ClusterICVFXInnerViewportRenderTargetRatioMult = static_cast<float>(*V);
		if (auto V = Params.get<sol::optional<double>>("outer_rtt_multiplier"))
			RF.ClusterICVFXOuterViewportRenderTargetRatioMult = static_cast<float>(*V);
		if (auto V = Params.get<sol::optional<double>>("screen_pct_multiplier"))
			RF.ClusterBufferRatioMult = static_cast<float>(*V);
		if (auto V = Params.get<sol::optional<double>>("inner_screen_pct_multiplier"))
			RF.ClusterICVFXInnerFrustumBufferRatioMult = static_cast<float>(*V);
		if (auto V = Params.get<sol::optional<double>>("outer_screen_pct_multiplier"))
			RF.ClusterICVFXOuterViewportBufferRatioMult = static_cast<float>(*V);
		if (auto V = Params.get<sol::optional<bool>>("warp_blend"))
			RF.bAllowWarpBlend = *V;

		Config->MarkPackageDirty();
		Session.Log(TEXT("[OK] ndisplay_configure_render_frame -> updated"));
		return sol::make_object(Lua, true);
	});

	// ---- ndisplay_assign_postprocess(params) ----
	Lua.set_function("ndisplay_assign_postprocess", [&Session](sol::table Params, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		std::string NodeIdStr = Params.get_or<std::string>("node_id", "");
		std::string PostprocessIdStr = Params.get_or<std::string>("postprocess_id", "");
		std::string TypeStr = Params.get_or<std::string>("type", "");
		std::string Label = Params.get_or<std::string>("actor_label", "");
		int Order = Params.get_or("order", -1);

		if (NodeIdStr.empty() || PostprocessIdStr.empty() || TypeStr.empty())
		{
			Session.Log(TEXT("[FAIL] ndisplay_assign_postprocess -> node_id, postprocess_id, and type are required"));
			return sol::lua_nil;
		}

		UDisplayClusterConfigurationData* Config = FindNDisplayConfig(Session, Label);
		if (!Config)
		{
			Session.Log(TEXT("[FAIL] ndisplay_assign_postprocess -> no nDisplay configuration found"));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("nDisplay: Assign Postprocess")));
		Config->Modify();

		TMap<FString, FString> ParamMap;
		sol::optional<sol::table> ParamsTable = Params.get<sol::optional<sol::table>>("parameters");
		if (ParamsTable)
		{
			for (const auto& Pair : *ParamsTable)
			{
				if (Pair.first.is<std::string>() && Pair.second.is<std::string>())
				{
					ParamMap.Add(UTF8_TO_TCHAR(Pair.first.as<std::string>().c_str()),
						UTF8_TO_TCHAR(Pair.second.as<std::string>().c_str()));
				}
			}
		}

		FString FNodeId = UTF8_TO_TCHAR(NodeIdStr.c_str());
		FString FPPId = UTF8_TO_TCHAR(PostprocessIdStr.c_str());
		FString FType = UTF8_TO_TCHAR(TypeStr.c_str());

		bool bOk = Config->AssignPostprocess(FNodeId, FPPId, FType, ParamMap, Order);
		if (!bOk)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] ndisplay_assign_postprocess -> failed for '%s' on node '%s'"), *FPPId, *FNodeId));
			return sol::lua_nil;
		}

		Config->MarkPackageDirty();
		Session.Log(FString::Printf(TEXT("[OK] ndisplay_assign_postprocess -> assigned '%s' type='%s' on node '%s'"), *FPPId, *FType, *FNodeId));
		return sol::make_object(Lua, true);
	});

	// ---- ndisplay_remove_postprocess(node_id, postprocess_id, actor_label?) ----
	Lua.set_function("ndisplay_remove_postprocess", [&Session](const std::string& NodeId, const std::string& PostprocessId, sol::optional<std::string> ActorLabel, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UDisplayClusterConfigurationData* Config = FindNDisplayConfig(Session, ActorLabel.value_or(""));
		if (!Config)
		{
			Session.Log(TEXT("[FAIL] ndisplay_remove_postprocess -> no nDisplay configuration found"));
			return sol::lua_nil;
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("nDisplay: Remove Postprocess")));
		Config->Modify();

		FString FNodeId = UTF8_TO_TCHAR(NodeId.c_str());
		FString FPPId = UTF8_TO_TCHAR(PostprocessId.c_str());

		bool bOk = Config->RemovePostprocess(FNodeId, FPPId);
		if (!bOk)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] ndisplay_remove_postprocess -> '%s' not found on node '%s'"), *FPPId, *FNodeId));
			return sol::lua_nil;
		}

		Config->MarkPackageDirty();
		Session.Log(FString::Printf(TEXT("[OK] ndisplay_remove_postprocess -> removed '%s' from node '%s'"), *FPPId, *FNodeId));
		return sol::make_object(Lua, true);
	});

	// ---- ndisplay_save_config(file_path, actor_label?) ----
	Lua.set_function("ndisplay_save_config", [&Session](const std::string& FilePath, sol::optional<std::string> ActorLabel, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);

		if (!IDisplayClusterConfiguration::IsAvailable())
		{
			Session.Log(TEXT("[FAIL] ndisplay_save_config -> DisplayClusterConfiguration module not available"));
			return sol::lua_nil;
		}

		UDisplayClusterConfigurationData* Config = FindNDisplayConfig(Session, ActorLabel.value_or(""));
		if (!Config)
		{
			Session.Log(TEXT("[FAIL] ndisplay_save_config -> no nDisplay configuration found"));
			return sol::lua_nil;
		}

		FString FPath = UTF8_TO_TCHAR(FilePath.c_str());
		bool bSaved = IDisplayClusterConfiguration::Get().SaveConfig(Config, FPath);
		if (!bSaved)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] ndisplay_save_config -> failed to save to %s"), *FPath));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] ndisplay_save_config -> saved to %s"), *FPath));
		return sol::make_object(Lua, true);
	});

	// ---- ndisplay_load_config(file_path) ----
	Lua.set_function("ndisplay_load_config", [&Session](const std::string& FilePath, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);

		if (!IDisplayClusterConfiguration::IsAvailable())
		{
			Session.Log(TEXT("[FAIL] ndisplay_load_config -> DisplayClusterConfiguration module not available"));
			return sol::lua_nil;
		}

		FString FPath = UTF8_TO_TCHAR(FilePath.c_str());
		UDisplayClusterConfigurationData* Config = IDisplayClusterConfiguration::Get().LoadConfig(FPath, GetTransientPackage());
		if (!Config)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] ndisplay_load_config -> failed to load from %s"), *FPath));
			return sol::lua_nil;
		}

		FString ConfigString;
		bool bOk = IDisplayClusterConfiguration::Get().ConfigAsString(Config, ConfigString);
		if (!bOk)
		{
			Session.Log(TEXT("[FAIL] ndisplay_load_config -> loaded but failed to serialize to string"));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] ndisplay_load_config -> loaded from %s"), *FPath));
		return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*ConfigString)));
	});
}

REGISTER_LUA_BINDING(NDisplay, NDisplayDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindNDisplay(Lua, Session);
});

#endif // !AIK_NDISPLAY_DISABLED
