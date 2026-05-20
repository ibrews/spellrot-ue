// Copyright 2025 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Tools/NeoStackToolBase.h"

/**
 * Central registry for all NeoStack tools
 * Singleton that manages tool registration and execution
 */
class AGENTINTEGRATIONKIT_API FNeoStackToolRegistry
{
public:
	/** Get singleton instance */
	static FNeoStackToolRegistry& Get();

	/** Register a tool (takes shared ownership) */
	void Register(TSharedPtr<FNeoStackToolBase> Tool);

	/** Register the NeoStack Lua-backed tool */
	void RegisterNeoStackLuaTool();

	/** Execute a tool by name with JSON args string */
	FToolResult Execute(const FString& ToolName, const FString& ArgsJson);

	/** Execute a tool by name with parsed JSON args */
	FToolResult Execute(const FString& ToolName, const TSharedPtr<class FJsonObject>& Args);

	/** Check if a tool exists */
	bool HasTool(const FString& ToolName) const;

	/** Get tool by name (can be null) */
	FNeoStackToolBase* GetTool(const FString& ToolName) const;

	/** Get all registered tool names */
	TArray<FString> GetToolNames() const;

	/** Get tool count */
	int32 GetToolCount() const { return Tools.Num(); }

private:
	FNeoStackToolRegistry();
	~FNeoStackToolRegistry() = default;

	/** Register all built-in tools */
	void RegisterBuiltInTools();

	/** Map of tool name -> tool instance */
	TMap<FString, TSharedPtr<FNeoStackToolBase>> Tools;
};
