// Copyright 2025 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Tools/NeoStackToolBase.h"

/**
 * Tool for executing Python code in the Unreal Editor.
 *
 * This tool provides access to the full Python scripting capabilities of Unreal Engine,
 * including the 'unreal' module with 1000+ APIs for:
 *   - Asset operations (create, load, save, delete, duplicate, rename)
 *   - Level/Actor manipulation (spawn, transform, select, destroy)
 *   - Material editing (create expressions, wire nodes, set parameters)
 *   - Blueprint modification (add functions, variables, compile)
 *   - Static/Skeletal mesh operations (LODs, collision, UVs)
 *   - DataTable operations (read/write rows, import/export CSV/JSON)
 *   - Editor subsystem access (actor, asset, level, import subsystems)
 *   - And much more...
 *
 * Parameters:
 *   - code: Python code to execute (required)
 *           Use 'import unreal' to access UE APIs.
 *           Print statements will be captured as output.
 *
 * Example usage:
 *   code: "import unreal\nasset = unreal.EditorAssetLibrary.load_asset('/Game/MyAsset')\nprint(asset.get_name())"
 *
 * The tool automatically handles:
 *   - Python initialization checking
 *   - GIL (Global Interpreter Lock) management
 *   - Output/error capture
 *   - Thread safety
 */
class AGENTINTEGRATIONKIT_API FExecutePythonTool : public FNeoStackToolBase
{
public:
	virtual FString GetName() const override { return TEXT("execute_python"); }
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Args) override;

private:
	/** Check if Python plugin is available and initialized */
	bool IsPythonAvailable(FString& OutError) const;

	/** Format Python output for display */
	FString FormatPythonOutput(const TArray<struct FPythonLogOutputEntry>& LogOutput, const FString& CommandResult, bool bSuccess) const;
};
