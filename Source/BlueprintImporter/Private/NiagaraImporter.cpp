#include "NiagaraImporter.h"

// UE core
#include "UObject/Package.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "AssetRegistryModule.h"

// Editor graph
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphNode_Comment.h"

// Niagara runtime  (these are in the Niagara *runtime* plugin — always present)
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraTypes.h"

// Niagara editor base headers (stable across engine forks)
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
// NOTE: individual node type headers (NiagaraNodeFunctionCall.h etc.) are
// intentionally NOT included here because their presence/name varies across
// engine forks. We use FindObject<UClass> at runtime instead — see
// FindNiagaraNodeClass() below. Only UNiagaraScriptVariable is used directly
// (declared in NiagaraGraph.h in most forks).

// regex
#include "Internationalization/Regex.h"

// ────────────────────────────────────────────────────────────────
//  Runtime class lookup — avoids hard-coding node header names
// ────────────────────────────────────────────────────────────────
static UClass* FindNiagaraNodeClass(const FString& ShortClassName)
{
    // 1. ANY_PACKAGE short-name (catches already-loaded classes)
    if (UClass* Cls = FindObject<UClass>(ANY_PACKAGE, *ShortClassName))
        return Cls;

    // 2. Known Niagara packages — try each in order
    static const TCHAR* Packages[] = {
        TEXT("/Script/NiagaraEditor"),
        TEXT("/Script/Niagara"),
        TEXT("/Script/NiagaraCore"),
        TEXT("/Script/NiagaraShader"),
    };
    for (const TCHAR* Pkg : Packages)
    {
        const FString FullPath = FString::Printf(TEXT("%s.%s"), Pkg, *ShortClassName);
        if (UClass* Cls = StaticLoadClass(UObject::StaticClass(), nullptr, *FullPath))
            return Cls;
    }

    // 3. TObjectIterator scan — catches classes in loaded-but-not-yet-registered packages
    for (TObjectIterator<UClass> It; It; ++It)
    {
        if (It->GetName() == ShortClassName &&
            It->IsChildOf(UEdGraphNode::StaticClass()))
        {
            return *It;
        }
    }

    return nullptr;
}

// ────────────────────────────────────────────────────────────────
int32 FNiagaraImporter::LastNodesCreated    = 0;
int32 FNiagaraImporter::LastCommentsCreated = 0;
int32 FNiagaraImporter::LastLinksWired      = 0;

// ────────────────────────────────────────────────────────────────
//  ParseNodeBlocks
// ────────────────────────────────────────────────────────────────
TArray<FNiagaraImporter::FNodeBlock>
FNiagaraImporter::ParseNodeBlocks(const FString& NodeText)
{
    TArray<FNodeBlock> Blocks;
    TArray<FString> Lines;
    NodeText.ParseIntoArrayLines(Lines, false);

    int32 i = 0;
    while (i < Lines.Num())
    {
        FString Line = Lines[i].TrimStart();

        // ── outer Begin Object ──────────────────────────────────
        if (Line.StartsWith(TEXT("Begin Object Class=")))
        {
            FNodeBlock B;

            // Class name
            {
                FRegexPattern P(TEXT("Class=/Script/(?:NiagaraEditor|UnrealEd)\\.(\\w+)"));
                FRegexMatcher M(P, Line);
                if (M.FindNext()) B.ClassName = M.GetCaptureGroup(1);
            }
            // Object name
            {
                FRegexPattern P(TEXT("Name=\"([^\"]+)\""));
                FRegexMatcher M(P, Line);
                if (M.FindNext()) B.ObjectName = M.GetCaptureGroup(1);
            }

            FString PropsAccum;
            ++i;

            while (i < Lines.Num())
            {
                FString L = Lines[i];
                FString T = L.TrimStart();

                if (T == TEXT("End Object"))
                {
                    ++i;
                    break;
                }

                if (T.StartsWith(TEXT("CustomProperties Pin (")))
                {
                    B.PinLines.Add(T);
                }
                else if (T.StartsWith(TEXT("NodePosX=")))
                {
                    B.NodePosX = FCString::Atoi(*T.Mid(9).TrimEnd());
                    PropsAccum += T + TEXT("\n");
                }
                else if (T.StartsWith(TEXT("NodePosY=")))
                {
                    B.NodePosY = FCString::Atoi(*T.Mid(9).TrimEnd());
                    PropsAccum += T + TEXT("\n");
                }
                else if (T.StartsWith(TEXT("NodeWidth=")))
                {
                    B.NodeWidth  = FCString::Atoi(*T.Mid(10).TrimEnd());
                    PropsAccum += T + TEXT("\n");
                }
                else if (T.StartsWith(TEXT("NodeHeight=")))
                {
                    B.NodeHeight = FCString::Atoi(*T.Mid(11).TrimEnd());
                    PropsAccum += T + TEXT("\n");
                }
                else if (T.StartsWith(TEXT("NodeGuid=")))
                {
                    B.NodeGuid = T.Mid(9).TrimEnd();
                }
                else if (T.StartsWith(TEXT("ChangeId=")))
                {
                    B.ChangeId = T.Mid(9).TrimEnd();
                }
                else if (T.StartsWith(TEXT("NodeComment=")))
                {
                    // strip surrounding quotes
                    B.NodeComment = T.Mid(12).TrimEnd().TrimQuotes();
                }
                else if (T.StartsWith(TEXT("CommentColor=")))
                {
                    B.CommentColorLine = T.Mid(13).TrimEnd();
                }
                else if (!T.IsEmpty())
                {
                    PropsAccum += T + TEXT("\n");
                }

                ++i;
            }

            B.PropsText = PropsAccum;
            if (!B.ClassName.IsEmpty() && !B.ObjectName.IsEmpty())
                Blocks.Add(B);

            continue;
        }

        ++i;
    }

    return Blocks;
}

// ────────────────────────────────────────────────────────────────
//  ApplyNodeProperties  — reflection-based property import
// ────────────────────────────────────────────────────────────────
void FNiagaraImporter::ApplyNodeProperties(UEdGraphNode* Node,
                                            const FNodeBlock& Block)
{
    if (!Node) return;
    UClass* Cls = Node->GetClass();

    // Accumulate array element lines: PropName(N)=Value  →  arrays
    TMap<FString, TMap<int32,FString>> ArrayProps;

    TArray<FString> Lines;
    Block.PropsText.ParseIntoArrayLines(Lines, false);

    for (const FString& RawLine : Lines)
    {
        FString Line = RawLine.TrimStart().TrimEnd();
        if (Line.IsEmpty()) continue;

        // Array element: PropName(N)=Value
        {
            FRegexPattern P(TEXT("^(\\w+)\\((\\d+)\\)=(.+)$"));
            FRegexMatcher M(P, Line);
            if (M.FindNext())
            {
                FString PropName = M.GetCaptureGroup(1);
                int32   Idx      = FCString::Atoi(*M.GetCaptureGroup(2));
                FString Val      = M.GetCaptureGroup(3);
                ArrayProps.FindOrAdd(PropName).Add(Idx, Val);
                continue;
            }
        }

        // Key=Value
        int32 EqIdx;
        if (!Line.FindChar(TEXT('='), EqIdx)) continue;
        FString Key = Line.Left(EqIdx).TrimEnd();
        FString Val = Line.Mid(EqIdx + 1).TrimStart();

        // Skip the common positional props already handled
        if (Key == TEXT("NodePosX") || Key == TEXT("NodePosY")
            || Key == TEXT("NodeGuid") || Key == TEXT("ChangeId")
            || Key == TEXT("NodeWidth") || Key == TEXT("NodeHeight")
            || Key == TEXT("NodeComment") || Key == TEXT("CommentColor"))
            continue;

        // Try to import via reflection
        if (FProperty* Prop = Cls->FindPropertyByName(*Key))
        {
            void* Ptr = Prop->ContainerPtrToValuePtr<void>(Node);
            Prop->ImportText(*Val, Ptr, PPF_None, Node);
        }
    }

    // Apply array properties
    for (auto& APair : ArrayProps)
    {
        FProperty* Prop = Cls->FindPropertyByName(*APair.Key);
        FArrayProperty* ArrProp = CastField<FArrayProperty>(Prop);
        if (!ArrProp) continue;

        void* ArrPtr = ArrProp->ContainerPtrToValuePtr<void>(Node);
        FScriptArrayHelper Helper(ArrProp, ArrPtr);

        for (auto& IdxVal : APair.Value)
        {
            int32 Idx = IdxVal.Key;
            if (Idx >= Helper.Num()) Helper.Resize(Idx + 1);

            // UE serializes struct array elements wrapped in outer parens:
            //   OutputVars(0)=(Name="float",TypeDefHandle=(...))
            // FProperty::ImportText on the *inner* struct property does NOT
            // expect those outer parens — it needs the raw struct body.
            // Passing them causes a silent import failure, leaving the struct
            // zero-initialised (which is why OutputVars stays empty).
            FString Val = IdxVal.Value.TrimStart().TrimEnd();
            if (CastField<FStructProperty>(ArrProp->Inner))
            {
                if (Val.StartsWith(TEXT("(")) && Val.EndsWith(TEXT(")")))
                    Val = Val.Mid(1, Val.Len() - 2);
            }

            ArrProp->Inner->ImportText(*Val,
                                       Helper.GetRawPtr(Idx),
                                       PPF_None, Node);
        }
    }
}

// ────────────────────────────────────────────────────────────────
//  CreateNode
// ────────────────────────────────────────────────────────────────
UEdGraphNode* FNiagaraImporter::CreateNode(UNiagaraGraph* Graph,
                                            const FNodeBlock& Block)
{
    const FString& CN = Block.ClassName;

    // ── Comment nodes use a stable UE header (EdGraphNode_Comment.h) ──
    if (CN == TEXT("EdGraphNode_Comment"))
    {
        UEdGraphNode_Comment* Comment =
            NewObject<UEdGraphNode_Comment>(Graph, FName(*Block.ObjectName));
        Comment->NodePosX    = Block.NodePosX;
        Comment->NodePosY    = Block.NodePosY;
        Comment->NodeWidth   = Block.NodeWidth;
        Comment->NodeHeight  = Block.NodeHeight;
        Comment->NodeComment = Block.NodeComment;

        if (!Block.CommentColorLine.IsEmpty())
        {
            if (FProperty* CP = Comment->GetClass()->FindPropertyByName(TEXT("CommentColor")))
                CP->ImportText(*Block.CommentColorLine,
                               CP->ContainerPtrToValuePtr<void>(Comment),
                               PPF_None, Comment);
        }

        ApplyNodeProperties(Comment, Block);
        Graph->AddNode(Comment, false, false);
        return Comment;
    }

    // ── All Niagara node types: look up the UClass at runtime ──────
    // This avoids including individual node headers whose names vary
    // across engine forks (e.g. Fortnite, shipped UE4.26, etc.).
    UClass* NodeClass = FindNiagaraNodeClass(CN);
    if (!NodeClass)
    {
        // Not a recognised / loaded class — skip silently
        return nullptr;
    }

    // Verify it is a UEdGraphNode subclass before creating
    if (!NodeClass->IsChildOf(UEdGraphNode::StaticClass()))
        return nullptr;

    UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, NodeClass,
                                                  FName(*Block.ObjectName));
    if (!Node) return nullptr;

    // ── Step 1: set positional props ────────────────────────────
    Node->NodePosX = Block.NodePosX;
    Node->NodePosY = Block.NodePosY;

    // ── Step 2: apply class-specific data properties (OpName,
    //    FunctionScript, etc.) so AllocateDefaultPins can use them.
    //    Do NOT import pin lines yet — AllocateDefaultPins would trash them.
    ApplyNodeProperties(Node, Block);

    // ── Step 3: add to graph and run PostPlacedNewNode, which calls
    //    AllocateDefaultPins internally to create the canonical pin set.
    Graph->AddNode(Node, false, false);
    Node->CreateNewGuid();
    Node->PostPlacedNewNode();

    // ── Step 3b: NiagaraNodeIf special case ─────────────────────
    //
    //    UNiagaraNodeIf::AllocateDefaultPins (called inside PostPlacedNewNode)
    //    rebuilds OutputVars from its internal type list, which may be empty
    //    for a freshly-constructed node. UNiagaraNodeIf::ResolveNumerics then
    //    unconditionally accesses OutputVars[1], crashing on an empty array.
    //
    //    Fix: re-apply array properties from the serialised block AFTER
    //    PostPlacedNewNode so they overwrite whatever AllocateDefaultPins left.
    //    Then, for NiagaraNodeIf specifically, guarantee at least 2 entries in
    //    OutputVars to satisfy the engine's assumption.
    if (CN == TEXT("NiagaraNodeIf"))
    {
        // Re-apply so OutputVars(0)=..., OutputVars(1)=... from PropsText win.
        ApplyNodeProperties(Node, Block);

        // Safety net: if OutputVars still has fewer than 2 entries after the
        // re-apply (e.g. the source data only had 0 or 1), pad with wildcards
        // so ResolveNumerics doesn't crash with an out-of-bounds access.
        if (FArrayProperty* OVProp = CastField<FArrayProperty>(
                Node->GetClass()->FindPropertyByName(TEXT("OutputVars"))))
        {
            void* ArrPtr = OVProp->ContainerPtrToValuePtr<void>(Node);
            FScriptArrayHelper Helper(OVProp, ArrPtr);
            while (Helper.Num() < 2)
            {
                Helper.AddValue();
                // Leave the added element default-initialised (wildcard type).
            }
        }
    }

    // ── Step 4: restore original NodeGuid (wiring uses it indirectly
    //    via object name, but keep it accurate for any future re-export).
    if (!Block.NodeGuid.IsEmpty())
        FGuid::Parse(Block.NodeGuid, Node->NodeGuid);

    // ── Step 5: reconcile pins from the serialised CustomProperties lines.
    //
    //    Strategy: strip LinkedTo from the pin text (we wire separately in
    //    WireConnections using PinId matching), then update PinId on each
    //    existing pin so our WireConnections pass can match them by hex ID.
    //
    //    Why not call ImportCustomProperties?
    //    ImportCustomProperties *creates new pins* on top of the ones
    //    AllocateDefaultPins already made.  The duplicates are then marked
    //    "trashed" by the engine during serialisation/compile, which triggers
    //    the "serialized while trashed" assert.
    //
    //    Instead we match imported pin lines to existing pins by name and
    //    overwrite just the PinId so WireConnections can find them.
    {
        // Build a map: PinName -> existing UEdGraphPin*
        TMap<FString, UEdGraphPin*> PinByName;
        for (UEdGraphPin* P : Node->Pins)
        {
            if (P)
                PinByName.FindOrAdd(P->PinName.ToString(), P);
        }

        FRegexPattern PinIdPat(TEXT("PinId=([0-9A-Fa-f]{32})"));
        FRegexPattern PinNamePat(TEXT("PinName=\"([^\"]+)\""));

        for (const FString& PinLine : Block.PinLines)
        {
            // Extract PinId and PinName from this line
            FString PinIdHex, PinNameStr;
            {
                FRegexMatcher M(PinIdPat, PinLine);
                if (M.FindNext()) PinIdHex = M.GetCaptureGroup(1);
            }
            {
                FRegexMatcher M(PinNamePat, PinLine);
                if (M.FindNext()) PinNameStr = M.GetCaptureGroup(1);
            }

            if (PinIdHex.IsEmpty() || PinNameStr.IsEmpty())
                continue;

            // Parse the hex id
            FGuid NewId;
            {
                FString WithDashes = FString::Printf(TEXT("%s-%s-%s-%s"),
                    *PinIdHex.Left(8), *PinIdHex.Mid(8,4),
                    *PinIdHex.Mid(12,4), *PinIdHex.Mid(16));
                FGuid::Parse(WithDashes, NewId);
            }
            if (!NewId.IsValid())
                continue;

            // Find the matching pin by name and stamp its PinId
            if (UEdGraphPin** Found = PinByName.Find(PinNameStr))
            {
                (*Found)->PinId = NewId;
            }
        }
    }

    return Node;
}

// ────────────────────────────────────────────────────────────────
//  FindPinByIdHex
// ────────────────────────────────────────────────────────────────
UEdGraphPin* FNiagaraImporter::FindPinByIdHex(UEdGraphNode* Node,
                                               const FString& PinIdHex)
{
    if (!Node || PinIdHex.IsEmpty()) return nullptr;

    // Always try the 32-char hex form (no dashes) first
    FGuid Target;
    {
        FString WithDashes = FString::Printf(TEXT("%s-%s-%s-%s"),
            *PinIdHex.Left(8),
            *PinIdHex.Mid(8, 4),
            *PinIdHex.Mid(12, 4),
            *PinIdHex.Mid(16));
        if (!FGuid::Parse(WithDashes, Target))
            FGuid::Parse(PinIdHex, Target);  // try with dashes as-is
    }

    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->PinId == Target)
            return Pin;
    }
    return nullptr;
}

// ────────────────────────────────────────────────────────────────
//  WireConnections
//  Parse "LinkedTo=(NodeName_N HEXGUID,...)" from each pin line
//  and call MakeLinkTo.
// ────────────────────────────────────────────────────────────────
int32 FNiagaraImporter::WireConnections(
    const TMap<FString, UEdGraphNode*>& NodeMap,
    const TArray<FNodeBlock>& Blocks)
{
    int32 Count = 0;

    // Get the graph schema for validated connection creation.
    // We derive it from the first node we find.
    const UEdGraphSchema* Schema = nullptr;
    for (auto& KV : NodeMap)
    {
        if (KV.Value && KV.Value->GetGraph())
        {
            Schema = KV.Value->GetGraph()->GetSchema();
            break;
        }
    }

    FRegexPattern LinkedToPattern(TEXT("LinkedTo=\\([^)]+\\)"));
    FRegexPattern EntryPattern(TEXT("(\\w+)\\s+([0-9A-Fa-f]{32})"));

    for (const FNodeBlock& B : Blocks)
    {
        UEdGraphNode* const* SrcNodePtr = NodeMap.Find(B.ObjectName);
        if (!SrcNodePtr || !*SrcNodePtr) continue;
        UEdGraphNode* SrcNode = *SrcNodePtr;

        for (const FString& PinLine : B.PinLines)
        {
            FString OwnerPinId;
            {
                FRegexPattern P(TEXT("PinId=([0-9A-Fa-f]{32})"));
                FRegexMatcher M(P, PinLine);
                if (M.FindNext()) OwnerPinId = M.GetCaptureGroup(1);
            }
            if (OwnerPinId.IsEmpty()) continue;

            UEdGraphPin* SrcPin = FindPinByIdHex(SrcNode, OwnerPinId);
            if (!SrcPin)
            {
                // Fallback: match by PinName extracted from the line
                FRegexPattern PN(TEXT("PinName=\"([^\"]+)\""));
                FRegexMatcher MN(PN, PinLine);
                if (MN.FindNext())
                {
                    const FString SrcPinName = MN.GetCaptureGroup(1);
                    for (UEdGraphPin* P : SrcNode->Pins)
                    {
                        if (P && P->PinName.ToString() == SrcPinName)
                        {
                            SrcPin = P;
                            break;
                        }
                    }
                }
            }
            if (!SrcPin) continue;

            FRegexMatcher LM(LinkedToPattern, PinLine);
            if (!LM.FindNext()) continue;
            FString LinkedList = LM.GetCaptureGroup(1);

            FRegexMatcher EM(EntryPattern, LinkedList);
            while (EM.FindNext())
            {
                FString TargetName  = EM.GetCaptureGroup(1);
                FString TargetPinId = EM.GetCaptureGroup(2);

                UEdGraphNode* const* TargetNodePtr = NodeMap.Find(TargetName);
                if (!TargetNodePtr || !*TargetNodePtr) continue;

                UEdGraphPin* TargetPin = FindPinByIdHex(*TargetNodePtr, TargetPinId);
                if (!TargetPin) continue;

                // Skip if already wired (link is listed on both pin ends)
                if (SrcPin->LinkedTo.Contains(TargetPin))
                    continue;

                // Use schema-validated connection when available.
                // This ensures both LinkedTo arrays are updated consistently
                // and prevents the pin integrity assert that fires on hover.
                if (Schema)
                {
                    const FPinConnectionResponse Resp =
                        Schema->CanCreateConnection(SrcPin, TargetPin);
                    if (Resp.Response != CONNECT_RESPONSE_DISALLOW)
                    {
                        Schema->TryCreateConnection(SrcPin, TargetPin);
                        ++Count;
                    }
                }
                else
                {
                    // Fallback: manually wire both ends to keep arrays in sync
                    SrcPin->LinkedTo.AddUnique(TargetPin);
                    TargetPin->LinkedTo.AddUnique(SrcPin);
                    ++Count;
                }
            }
        }
    }

    return Count;
}

// ────────────────────────────────────────────────────────────────
//  RegisterScriptVars
// ────────────────────────────────────────────────────────────────
void FNiagaraImporter::RegisterScriptVars(UNiagaraGraph* Graph,
                                           const TArray<FNiagaraScriptVarBlock>& Vars)
{
    if (!Graph || Vars.Num() == 0) return;

    // Look up UNiagaraScriptVariable class dynamically — avoids a fragile include
    UClass* ScriptVarClass = FindObject<UClass>(ANY_PACKAGE, TEXT("NiagaraScriptVariable"));
    if (!ScriptVarClass)
        ScriptVarClass = StaticLoadClass(UObject::StaticClass(), nullptr,
                                         TEXT("/Script/NiagaraEditor.NiagaraScriptVariable"));
    if (!ScriptVarClass) return;   // can't register without the class

    for (const FNiagaraScriptVarBlock& V : Vars)
    {
        if (V.VariableName.IsEmpty()) continue;

        UObject* ScriptVar = NewObject<UObject>(Graph, ScriptVarClass,
                                                FName(*V.ObjectName));
        if (!ScriptVar) continue;

        UClass* VarClass = ScriptVar->GetClass();

        // Variable field: Variable=(VarData=(...),Name="...",TypeDefHandle=(...))
        if (!V.VariableName.IsEmpty() && !V.TypeHandle.IsEmpty())
        {
            FString VarText = FString::Printf(
                TEXT("(VarData=(0),Name=\"%s\",TypeDefHandle=%s)"),
                *V.VariableName, *V.TypeHandle);
            if (FProperty* P = VarClass->FindPropertyByName(TEXT("Variable")))
                P->ImportText(*VarText, P->ContainerPtrToValuePtr<void>(ScriptVar),
                               PPF_None, ScriptVar);
        }

        if (!V.DefaultMode.IsEmpty())
        {
            if (FProperty* P = VarClass->FindPropertyByName(TEXT("DefaultMode")))
                P->ImportText(*V.DefaultMode,
                               P->ContainerPtrToValuePtr<void>(ScriptVar),
                               PPF_None, ScriptVar);
        }

        if (V.bIsStaticSwitch)
        {
            if (FProperty* P = VarClass->FindPropertyByName(TEXT("bIsStaticSwitch")))
                P->ImportText(TEXT("True"),
                               P->ContainerPtrToValuePtr<void>(ScriptVar),
                               PPF_None, ScriptVar);

            FString SwitchVal = FString::FromInt(V.StaticSwitchDefaultValue);
            if (FProperty* P = VarClass->FindPropertyByName(TEXT("StaticSwitchDefaultValue")))
                P->ImportText(*SwitchVal,
                               P->ContainerPtrToValuePtr<void>(ScriptVar),
                               PPF_None, ScriptVar);
        }

        if (!V.ChangeId.IsEmpty())
        {
            if (FProperty* P = VarClass->FindPropertyByName(TEXT("ChangeId")))
                P->ImportText(*V.ChangeId,
                               P->ContainerPtrToValuePtr<void>(ScriptVar),
                               PPF_None, ScriptVar);
        }

        // Add to graph if the API exists
        // (UNiagaraGraph::AddScriptVariable was introduced in a 4.26 patch)
        UFunction* AddFn = Graph->GetClass()->FindFunctionByName(
            TEXT("AddScriptVariable"));
        if (AddFn)
        {
            // Pass as UObject* — the function expects the script-variable object
            struct { UObject* Var; } Params{ ScriptVar };
            Graph->ProcessEvent(AddFn, &Params);
        }
    }
}

// ────────────────────────────────────────────────────────────────
//  Import  — main entry point
// ────────────────────────────────────────────────────────────────
UNiagaraScript* FNiagaraImporter::Import(const FNiagaraConvertResult& Converted,
                                          const FString& ScriptName,
                                          FString& OutError)
{
    LastNodesCreated    = 0;
    LastCommentsCreated = 0;
    LastLinksWired      = 0;

    if (!Converted.bSuccess || Converted.NodeText.IsEmpty())
    {
        OutError = Converted.ErrorMessage.IsEmpty()
            ? TEXT("Converter produced no output.") : Converted.ErrorMessage;
        return nullptr;
    }

    // ── Create UPackage ─────────────────────────────────────────
    const FString PackagePath = UPackageTools::SanitizePackageName(
        FString::Printf(TEXT("/Game/ImportedNiagara/%s"), *ScriptName));

    // ── Find or create the package ──────────────────────────────
    //
    // Do NOT call FullyLoad() on a package that may already contain a
    // NiagaraNodeIf with an empty OutputVars array (written by a previous
    // failed import). FullyLoad triggers PostLoad → RebuildNumericCache →
    // ResolveNumerics → OutputVars[1] → crash before we get a chance to fix
    // anything.
    //
    // Strategy:
    //   1. If the package is already in memory, rename away any stale
    //      UNiagaraScript it contains so NewObject gives us a clean slate.
    //   2. Never call FullyLoad() — we are always writing a brand-new asset.
    UPackage* Package = FindObject<UPackage>(nullptr, *PackagePath);
    if (Package)
    {
        // Rename the old script object out of the way so we can recreate it.
        UNiagaraScript* OldScript = FindObject<UNiagaraScript>(Package, *ScriptName);
        if (OldScript)
        {
            OldScript->Rename(
                *FString::Printf(TEXT("__stale_%s"), *ScriptName),
                GetTransientPackage(),
                REN_DontCreateRedirectors | REN_ForceNoResetLoaders);
        }
    }
    else
    {
        Package = CreatePackage(*PackagePath);
    }

    if (!Package)
    {
        OutError = TEXT("Failed to create package.");
        return nullptr;
    }
    // Do NOT call Package->FullyLoad() here. See comment above.

    // ── Create UNiagaraScript ───────────────────────────────────
    UNiagaraScript* Script = NewObject<UNiagaraScript>(Package, *ScriptName,
                                                        RF_Public | RF_Standalone);
    if (!Script)
    {
        OutError = TEXT("Failed to create UNiagaraScript object.");
        return nullptr;
    }

    // ── Create source and graph ─────────────────────────────────
    UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(Script);
    UNiagaraGraph*        Graph  = NewObject<UNiagaraGraph>(Source);
    Source->NodeGraph = Graph;

    // Usage = Module (most UEFN Niagara scripts are modules)
    if (FProperty* UsageProp = Script->GetClass()->FindPropertyByName(TEXT("Usage")))
        UsageProp->ImportText(TEXT("Module"), 
                               UsageProp->ContainerPtrToValuePtr<void>(Script),
                               PPF_None, Script);

    // ── Attach source to script ─────────────────────────────────
    // Strategy: iterate FObjectProperty fields on UNiagaraScript and set the
    // first one whose PropertyClass is (or inherits from) UNiagaraScriptSource.
    // This is more reliable than ProcessEvent or ImportText for non-UFUNCTION setters.
    {
        bool bSourceAttached = false;

        // Pass 1: exact field-name match ("Source" or "ScriptSource")
        static const FName KnownNames[] = { TEXT("Source"), TEXT("ScriptSource") };
        for (const FName& FieldName : KnownNames)
        {
            if (FObjectProperty* ObjProp = CastField<FObjectProperty>(
                    Script->GetClass()->FindPropertyByName(FieldName)))
            {
                ObjProp->SetObjectPropertyValue(
                    ObjProp->ContainerPtrToValuePtr<void>(Script), Source);
                bSourceAttached = true;
                break;
            }
        }

        // Pass 2: scan all FObjectProperty fields for a UNiagaraScriptSource slot
        if (!bSourceAttached)
        {
            for (TFieldIterator<FObjectProperty> It(Script->GetClass()); It; ++It)
            {
                if (It->PropertyClass &&
                    It->PropertyClass->IsChildOf(UNiagaraScriptSource::StaticClass()))
                {
                    It->SetObjectPropertyValue(
                        It->ContainerPtrToValuePtr<void>(Script), Source);
                    bSourceAttached = true;
                    break;
                }
            }
        }

        // Pass 3: try ProcessEvent as last resort (requires UFUNCTION)
        if (!bSourceAttached)
        {
            if (UFunction* SetSrcFn = Script->GetClass()->FindFunctionByName(TEXT("SetSource")))
            {
                struct { UNiagaraScriptSource* S; } P{ Source };
                Script->ProcessEvent(SetSrcFn, &P);
                bSourceAttached = true;
            }
        }

        if (!bSourceAttached)
        {
            OutError = TEXT("Could not attach NiagaraScriptSource to UNiagaraScript — source field not found.");
            return nullptr;
        }
    }

    // ── Register ScriptVariables ────────────────────────────────
    RegisterScriptVars(Graph, Converted.ScriptVars);

    // ── Parse node blocks ───────────────────────────────────────
    TArray<FNodeBlock> Blocks = ParseNodeBlocks(Converted.NodeText);

    // ── Create all nodes ────────────────────────────────────────
    TMap<FString, UEdGraphNode*> NodeMap;

    for (const FNodeBlock& B : Blocks)
    {
        UEdGraphNode* Node = CreateNode(Graph, B);
        if (!Node) continue;

        NodeMap.Add(B.ObjectName, Node);

        if (B.ClassName == TEXT("EdGraphNode_Comment"))
            ++LastCommentsCreated;
        else
            ++LastNodesCreated;
    }

    // ── Wire connections ────────────────────────────────────────
    LastLinksWired = WireConnections(NodeMap, Blocks);

    // ── Finalize asset ──────────────────────────────────────────
    Package->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(Script);

    const FString PkgFilename = FPackageName::LongPackageNameToFilename(
        PackagePath, FPackageName::GetAssetPackageExtension());

    UPackage::SavePackage(Package, Script, RF_Public | RF_Standalone,
                          *PkgFilename, GError, nullptr, false, true, SAVE_NoError);

    return Script;
}