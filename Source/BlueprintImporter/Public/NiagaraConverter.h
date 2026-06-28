#pragma once

#include "CoreMinimal.h"

/**
 * FNiagaraConverter
 *
 * C++ port of niagara_ue5_to_ue4.py.
 *
 * Converts a raw UEFN / UE5 Niagara script clipboard dump into the
 * UE 4.26-compatible node-text format.  The result can be imported
 * directly into a UNiagaraGraph via the editor API.
 *
 * Handles:
 *  - NiagaraClipboardContent wrapper (base-64 ExportedNodes decode)
 *  - PinSubCategoryObject path format (UE5 quoted → UE4 class-keyword)
 *  - Struct path remapping  (NiagaraPosition→Vector, Vector3f→Vector, etc.)
 *  - FunctionScript path conversion
 *  - RegisteredTypeIndex remapping (InputMap: 108→47, etc.)
 *  - SelectorPinType format conversion
 *  - INVTEXT / NSLOCTEXT PinFriendlyName clean-up
 *  - ExportPath stripping
 *  - UE5-only pin fields (bIsUObjectWrapper, bSerializeAsSinglePrecisionFloat)
 *
 * Also extracts the NiagaraScriptVariable subobject blocks that live
 * in the outer NiagaraClipboardContent wrapper (needed for static-switch
 * default values and metadata).
 */
struct FNiagaraScriptVarBlock
{
    FString ObjectName;           // e.g. "NiagaraScriptVariable_44"
    FString VariableName;         // from Variable=(Name="...")
    FString TypeHandle;           // "(RegisteredTypeIndex=47)"  already remapped
    FString DefaultMode;          // e.g. "FailIfPreviouslyNotSet"
    FString MetadataRaw;          // raw Metadata=(...) line value
    bool    bIsStaticSwitch         = false;
    int32   StaticSwitchDefaultValue = 0;
    FString ChangeId;
    // Raw DefaultValueVariant text for binary-default values
    FString DefaultValueVariantRaw;
};

struct FNiagaraConvertResult
{
    /** Node-graph text in UE4.26 format, ready for ImportNodesFromText */
    FString NodeText;

    /** ScriptVariable blocks extracted from the outer wrapper */
    TArray<FNiagaraScriptVarBlock> ScriptVars;

    bool    bSuccess = false;
    FString ErrorMessage;
    TArray<FString> Warnings;       // unknown types etc.
};

class BLUEPRINTIMPORTER_API FNiagaraConverter
{
public:
    /**
     * Convert an UEFN / UE5 clipboard string to UE4.26 node text.
     * Caller should check Result.bSuccess and display Result.ErrorMessage if false.
     */
    static FNiagaraConvertResult Convert(const FString& InputText);

private:
    /* ---- text transformations (mirrors Python functions) ---- */
    static FString UnwrapClipboardContent(const FString& Text,
                                          TArray<FNiagaraScriptVarBlock>& OutVars,
                                          bool& bWasWrapped);

    static FString ConvertNodes(const FString& RawNodeText,
                                TArray<FString>& OutWarnings);

    static FString RemapStructPath(const FString& Path,
                                   TArray<FString>& OutWarnings);

    static FString ConvertSubCategoryObject(const FString& Value,
                                            TArray<FString>& OutWarnings);

    static FString ConvertFunctionScript(const FString& Value);

    static FString RemapTypeIndices(const FString& Text);

    static FString StripUE5PinProperties(const FString& Text);

    static FString StripPinFriendlyNames(const FString& Text);

    static FString StripExportPaths(const FString& Text);

    static FString FixSelectorPinType(const FString& Text,
                                      TArray<FString>& OutWarnings);

    static FString RemapAllStructPaths(const FString& Text,
                                       TArray<FString>& OutWarnings);

    /* ---- script-variable parsing ---- */
    static void ParseScriptVarBlocks(const FString& WrapperText,
                                     TArray<FNiagaraScriptVarBlock>& OutVars);

    static void RemapScriptVarTypeIndices(FNiagaraScriptVarBlock& Var);

    /* ---- helpers ---- */
    static const TMap<FString,FString>& GetStructRemap();
};
