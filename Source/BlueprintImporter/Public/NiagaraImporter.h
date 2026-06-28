#pragma once

#include "CoreMinimal.h"
#include "NiagaraConverter.h"

/**
 * FNiagaraImporter
 *
 * Creates a UNiagaraScript asset and populates its graph by building
 * every node from scratch using the UE4 C++ Niagara API.
 *
 * Strategy for each node type:
 *   1. NewObject<UNiagaraNodeXxx>(Graph, FName(*ObjectName))
 *   2. Set common props: NodePosX, NodePosY, NodeGuid, ChangeId
 *   3. Apply node-specific properties via UObject property reflection
 *      (FProperty::ImportText) — handles FunctionScript, OpName, Input,
 *      InputParameterName, SwitchTypeData, OutputVars, Signature, etc.
 *   4. Add node to graph: Graph->AddNode() + CreateNewGuid() + PostPlacedNewNode()
 *   5. Import pin data: UEdGraphNode::ImportCustomProperties() per pin line
 *   6. After all nodes are created: wire connections by matching PinId GUIDs
 *
 * ScriptVariables from the outer clipboard wrapper are registered with
 * the graph so that static-switch defaults and metadata are preserved.
 */
class BLUEPRINTIMPORTER_API FNiagaraImporter
{
public:
    /** Stats from the last successful import */
    static int32 LastNodesCreated;
    static int32 LastCommentsCreated;
    static int32 LastLinksWired;

    /**
     * Build a UNiagaraScript under /Game/ImportedNiagara/<ScriptName>.
     * Returns the created asset, or nullptr on failure (OutError filled in).
     */
    static class UNiagaraScript* Import(
        const FNiagaraConvertResult& Converted,
        const FString& ScriptName,
        FString& OutError);

private:
    /* ---- internal helpers ---- */

    /** Parse the converted node-text into individual "Begin/End Object" blocks */
    struct FNodeBlock
    {
        FString ClassName;     // e.g. "NiagaraNodeFunctionCall"
        FString ObjectName;    // e.g. "NiagaraNodeFunctionCall_29"
        FString PropsText;     // everything between Begin/End (NOT CustomProperties lines)
        TArray<FString> PinLines; // each "CustomProperties Pin (...)" line
        int32 NodePosX = 0;
        int32 NodePosY = 0;
        int32 NodeWidth  = 800;
        int32 NodeHeight = 200;
        FString NodeGuid;
        FString ChangeId;
        FString NodeComment;
        FString CommentColorLine; // e.g. "(R=0.1,G=0.8,B=0.7,A=0.4)"
    };

    static TArray<FNodeBlock> ParseNodeBlocks(const FString& NodeText);

    /** Create one Niagara node of the correct class and set all properties */
    static UEdGraphNode* CreateNode(class UNiagaraGraph* Graph,
                                    const FNodeBlock& Block);

    /** Apply non-pin properties to a freshly-created node via ImportText */
    static void ApplyNodeProperties(UEdGraphNode* Node, const FNodeBlock& Block);

    /** Register all ScriptVariables parsed from the clipboard wrapper */
    static void RegisterScriptVars(class UNiagaraGraph* Graph,
                                   const TArray<FNiagaraScriptVarBlock>& Vars);

    /** Second pass: wire all LinkedTo connections */
    static int32 WireConnections(
        const TMap<FString, UEdGraphNode*>& NodeMap,
        const TArray<FNodeBlock>& Blocks);

    /** Find a pin on Node whose PinId (as hex string) matches PinIdHex */
    static UEdGraphPin* FindPinByIdHex(UEdGraphNode* Node, const FString& PinIdHex);
};
