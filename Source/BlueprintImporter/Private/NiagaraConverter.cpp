#include "NiagaraConverter.h"

#include "Misc/Base64.h"
#include "Internationalization/Regex.h"

// ────────────────────────────────────────────────────────────────
//  Struct-path remap table  (UE5 → UE4)
// ────────────────────────────────────────────────────────────────
const TMap<FString,FString>& FNiagaraConverter::GetStructRemap()
{
    static TMap<FString,FString> Map = []
    {
        TMap<FString,FString> M;
        // UE5 single-precision → UE4 double-precision
        M.Add(TEXT("/Script/CoreUObject.Vector3f"),    TEXT("/Script/CoreUObject.Vector"));
        M.Add(TEXT("/Script/CoreUObject.Vector2f"),    TEXT("/Script/CoreUObject.Vector2D"));
        M.Add(TEXT("/Script/CoreUObject.Quat4f"),      TEXT("/Script/CoreUObject.Quat"));
        // UE5-only Niagara types
        M.Add(TEXT("/Script/Niagara.NiagaraPosition"), TEXT("/Script/CoreUObject.Vector"));
        M.Add(TEXT("/Script/Niagara.NiagaraMatrix"),   TEXT("/Script/CoreUObject.Matrix"));
        M.Add(TEXT("/Script/Niagara.NiagaraVector"),   TEXT("/Script/CoreUObject.Vector"));
        M.Add(TEXT("/Script/Niagara.NiagaraVector2"),  TEXT("/Script/CoreUObject.Vector2D"));
        M.Add(TEXT("/Script/Niagara.NiagaraVector4"),  TEXT("/Script/CoreUObject.Vector4"));
        // pass-throughs (exist in both)
        M.Add(TEXT("/Script/CoreUObject.Vector"),      TEXT("/Script/CoreUObject.Vector"));
        M.Add(TEXT("/Script/CoreUObject.Vector2D"),    TEXT("/Script/CoreUObject.Vector2D"));
        M.Add(TEXT("/Script/CoreUObject.Vector4"),     TEXT("/Script/CoreUObject.Vector4"));
        M.Add(TEXT("/Script/CoreUObject.Quat"),        TEXT("/Script/CoreUObject.Quat"));
        M.Add(TEXT("/Script/CoreUObject.Matrix"),      TEXT("/Script/CoreUObject.Matrix"));
        M.Add(TEXT("/Script/CoreUObject.LinearColor"), TEXT("/Script/CoreUObject.LinearColor"));
        M.Add(TEXT("/Script/CoreUObject.Color"),       TEXT("/Script/CoreUObject.Color"));
        M.Add(TEXT("/Script/CoreUObject.Rotator"),     TEXT("/Script/CoreUObject.Rotator"));
        M.Add(TEXT("/Script/CoreUObject.Transform"),   TEXT("/Script/CoreUObject.Transform"));
        M.Add(TEXT("/Script/Niagara.NiagaraFloat"),        TEXT("/Script/Niagara.NiagaraFloat"));
        M.Add(TEXT("/Script/Niagara.NiagaraBool"),         TEXT("/Script/Niagara.NiagaraBool"));
        M.Add(TEXT("/Script/Niagara.NiagaraInt32"),        TEXT("/Script/Niagara.NiagaraInt32"));
        M.Add(TEXT("/Script/Niagara.NiagaraNumeric"),      TEXT("/Script/Niagara.NiagaraNumeric"));
        M.Add(TEXT("/Script/Niagara.NiagaraParameterMap"), TEXT("/Script/Niagara.NiagaraParameterMap"));
        M.Add(TEXT("/Script/Niagara.NiagaraID"),           TEXT("/Script/Niagara.NiagaraID"));
        M.Add(TEXT("/Script/Niagara.ENiagaraCoordinateSpace"), TEXT("/Script/Niagara.ENiagaraCoordinateSpace"));
        return M;
    }();
    return Map;
}

// ────────────────────────────────────────────────────────────────
//  RemapStructPath
// ────────────────────────────────────────────────────────────────
FString FNiagaraConverter::RemapStructPath(const FString& Path, TArray<FString>& OutWarnings)
{
    const TMap<FString,FString>& Map = GetStructRemap();
    if (const FString* Found = Map.Find(Path))
        return *Found;

    // unknown — warn once and pass through
    static TSet<FString> Warned;
    if (!Warned.Contains(Path))
    {
        Warned.Add(Path);
        OutWarnings.Add(FString::Printf(
            TEXT("Unknown struct type (passing through, may crash UE4): %s"), *Path));
    }
    return Path;
}

// ────────────────────────────────────────────────────────────────
//  ConvertSubCategoryObject
//  Mirrors: convert_subcategory_object() in the Python script
// ────────────────────────────────────────────────────────────────
FString FNiagaraConverter::ConvertSubCategoryObject(const FString& Value,
                                                    TArray<FString>& OutWarnings)
{
    if (Value == TEXT("None"))
        return TEXT("PinType.PinSubCategoryObject=None");

    // UE5 format: "/Script/CoreUObject.ScriptStruct'/Script/Niagara.NiagaraFloat'"
    {
        FRegexPattern Pat(TEXT("^\"/Script/CoreUObject\\.(\\w+)'([^']+)'\"$"));
        FRegexMatcher M(Pat, Value);
        if (M.FindNext())
        {
            FString ClassName = M.GetCaptureGroup(1);
            FString Path      = RemapStructPath(M.GetCaptureGroup(2), OutWarnings);
            return FString::Printf(TEXT("PinType.PinSubCategoryObject=%s'\"%s\"'"),
                                   *ClassName, *Path);
        }
    }

    // UE4 format already: ScriptStruct'"/Script/Niagara.NiagaraFloat"'
    {
        FRegexPattern Pat(TEXT("^(\\w+)'\"([^\"]+)\"'$"));
        FRegexMatcher M(Pat, Value);
        if (M.FindNext())
        {
            FString ClassName = M.GetCaptureGroup(1);
            FString Path      = RemapStructPath(M.GetCaptureGroup(2), OutWarnings);
            return FString::Printf(TEXT("PinType.PinSubCategoryObject=%s'\"%s\"'"),
                                   *ClassName, *Path);
        }
    }

    // Class/UserDefinedEnum/etc. — keep as-is  
    return FString::Printf(TEXT("PinType.PinSubCategoryObject=%s"), *Value);
}

// ────────────────────────────────────────────────────────────────
//  ConvertFunctionScript
// ────────────────────────────────────────────────────────────────
FString FNiagaraConverter::ConvertFunctionScript(const FString& Line)
{
    // UE5: FunctionScript="/Script/Niagara.NiagaraScript'/Niagara/Functions/Foo.Foo'"
    // UE4: FunctionScript=NiagaraScript'"/Niagara/Functions/Foo.Foo"'
    FRegexPattern Pat(TEXT("FunctionScript=\"/Script/Niagara\\.NiagaraScript'([^']+)'\""));
    FRegexMatcher M(Pat, Line);
    FString Out = Line;
    // We only expect one match per line but iterate to be safe
    int32 Offset = 0;
    while (M.FindNext())
    {
        FString Inner = M.GetCaptureGroup(1);
        FString Old   = M.GetCaptureGroup(0);
        FString New   = FString::Printf(TEXT("FunctionScript=NiagaraScript'\"%s\"'"), *Inner);
        Out = Out.Replace(*Old, *New);
    }
    return Out;
}

// ────────────────────────────────────────────────────────────────
//  RemapTypeIndices
//  Only remaps in safe, unambiguous contexts.
// ────────────────────────────────────────────────────────────────
FString FNiagaraConverter::RemapTypeIndices(const FString& InText)
{
    // Index mapping  UE5 → UE4
    const TMap<int32,int32> IndexMap =
    {
        {108, 47},   // NiagaraParameterMap
        {112, 50},   // NiagaraFloat
        {114, 52},   // NiagaraInt32
        {115, 53},   // NiagaraBool
        {117, 55},   // Vector3f / NiagaraPosition → Vector
    };

    auto RemapInMatch = [&](const FString& Block) -> FString
    {
        FString Out = Block;
        for (auto& KV : IndexMap)
        {
            FString From = FString::Printf(TEXT("RegisteredTypeIndex=%d"), KV.Key);
            FString To   = FString::Printf(TEXT("RegisteredTypeIndex=%d"), KV.Value);
            Out = Out.Replace(*From, *To);
        }
        return Out;
    };

    FString Text = InText;

    // InputMap specifically
    Text = Text.Replace(
        TEXT("Name=\"InputMap\",TypeDefHandle=(RegisteredTypeIndex=108)"),
        TEXT("Name=\"InputMap\",TypeDefHandle=(RegisteredTypeIndex=47)"));

    // OutputVars(N)=(...)
    {
        FRegexPattern Pat(TEXT("OutputVars\\(\\d+\\)=\\([^)]+\\)"));
        FRegexMatcher Mat(Pat, Text);
        FString Result;
        int32 LastEnd = 0;
        while (Mat.FindNext())
        {
            Result += Text.Mid(LastEnd, Mat.GetMatchBeginning() - LastEnd);
            Result += RemapInMatch(Mat.GetCaptureGroup(0));
            LastEnd = Mat.GetMatchEnding();
        }
        Result += Text.Mid(LastEnd);
        Text = Result;
    }

    // Variable=(...)
    {
        FRegexPattern Pat(TEXT("Variable=\\([^)]+\\)"));
        FRegexMatcher Mat(Pat, Text);
        FString Result;
        int32 LastEnd = 0;
        while (Mat.FindNext())
        {
            Result += Text.Mid(LastEnd, Mat.GetMatchBeginning() - LastEnd);
            Result += RemapInMatch(Mat.GetCaptureGroup(0));
            LastEnd = Mat.GetMatchEnding();
        }
        Result += Text.Mid(LastEnd);
        Text = Result;
    }

    // Input=(...TypeDefHandle=...)
    {
        FRegexPattern Pat(TEXT("Input=\\(.*?TypeDefHandle=\\(RegisteredTypeIndex=\\d+\\)\\)"));
        FRegexMatcher Mat(Pat, Text);
        FString Result;
        int32 LastEnd = 0;
        while (Mat.FindNext())
        {
            Result += Text.Mid(LastEnd, Mat.GetMatchBeginning() - LastEnd);
            Result += RemapInMatch(Mat.GetCaptureGroup(0));
            LastEnd = Mat.GetMatchEnding();
        }
        Result += Text.Mid(LastEnd);
        Text = Result;
    }

    return Text;
}

// ────────────────────────────────────────────────────────────────
//  StripUE5PinProperties
// ────────────────────────────────────────────────────────────────
FString FNiagaraConverter::StripUE5PinProperties(const FString& Text)
{
    FString Out = Text;
    Out = Out.Replace(TEXT("PinType.bIsUObjectWrapper=False,"), TEXT(""));
    Out = Out.Replace(TEXT("PinType.bIsUObjectWrapper=True,"),  TEXT(""));
    Out = Out.Replace(TEXT("PinType.bSerializeAsSinglePrecisionFloat=False,"), TEXT(""));
    Out = Out.Replace(TEXT("PinType.bSerializeAsSinglePrecisionFloat=True,"),  TEXT(""));
    return Out;
}

// ────────────────────────────────────────────────────────────────
//  StripPinFriendlyNames
//  INVTEXT("foo") → "foo"
//  NSLOCTEXT(...) → removed
// ────────────────────────────────────────────────────────────────
FString FNiagaraConverter::StripPinFriendlyNames(const FString& InText)
{
    // Remove NSLOCTEXT friendly names entirely
    FString Text;
    {
        FRegexPattern Pat(TEXT("PinFriendlyName=NSLOCTEXT\\([^)]+\\),"));
        FRegexMatcher M(Pat, InText);
        int32 Last = 0;
        while (M.FindNext())
        {
            Text += InText.Mid(Last, M.GetMatchBeginning() - Last);
            Last  = M.GetMatchEnding();
        }
        Text += InText.Mid(Last);
    }

    // Convert INVTEXT("...") → "..."
    {
        FString Out;
        FRegexPattern Pat(TEXT("PinFriendlyName=INVTEXT\\(\"([^\"]+)\"\\)"));
        FRegexMatcher M(Pat, Text);
        int32 Last = 0;
        while (M.FindNext())
        {
            Out += Text.Mid(Last, M.GetMatchBeginning() - Last);
            Out += FString::Printf(TEXT("PinFriendlyName=\"%s\""), *M.GetCaptureGroup(1));
            Last  = M.GetMatchEnding();
        }
        Out += Text.Mid(Last);
        Text = Out;
    }

    return Text;
}

// ────────────────────────────────────────────────────────────────
//  StripExportPaths
//  Remove ExportPath="..." from Begin Object lines
// ────────────────────────────────────────────────────────────────
FString FNiagaraConverter::StripExportPaths(const FString& InText)
{
    FString Out;
    FRegexPattern Pat(TEXT(" ExportPath=\"[^\"]*\""));
    FRegexMatcher M(Pat, InText);
    int32 Last = 0;
    while (M.FindNext())
    {
        Out += InText.Mid(Last, M.GetMatchBeginning() - Last);
        Last  = M.GetMatchEnding();
    }
    Out += InText.Mid(Last);
    return Out;
}

// ────────────────────────────────────────────────────────────────
//  FixSelectorPinType
//  UE5: SelectorPinType=(ClassStructOrEnum="/Script/CoreUObject.ScriptStruct'/Script/Niagara.NiagaraBool'",UnderlyingType=2)
//  UE4: SelectorPinType=(ClassStructOrEnum=ScriptStruct'"/Script/Niagara.NiagaraBool"')
// ────────────────────────────────────────────────────────────────
FString FNiagaraConverter::FixSelectorPinType(const FString& InText, TArray<FString>& OutWarnings)
{
    FString Out;
    FRegexPattern Pat(TEXT("SelectorPinType=\\([^)]+\\)"));
    FRegexMatcher M(Pat, InText);
    int32 Last = 0;

    while (M.FindNext())
    {
        Out += InText.Mid(Last, M.GetMatchBeginning() - Last);

        FString Full = M.GetCaptureGroup(0);

        // Try UE5 quoted format
        FRegexPattern InnerPat(
            TEXT("ClassStructOrEnum=\"/Script/CoreUObject\\.(\\w+)'([^']+)'\""));
        FRegexMatcher InnerM(InnerPat, Full);
        if (InnerM.FindNext())
        {
            FString ClassName = InnerM.GetCaptureGroup(1);
            FString Path      = RemapStructPath(InnerM.GetCaptureGroup(2), OutWarnings);
            Out += FString::Printf(
                TEXT("SelectorPinType=(ClassStructOrEnum=%s'\"%s\"')"),
                *ClassName, *Path);
        }
        else
        {
            Out += Full;
        }

        Last = M.GetMatchEnding();
    }
    Out += InText.Mid(Last);
    return Out;
}

// ────────────────────────────────────────────────────────────────
//  RemapAllStructPaths  (catch-all pass)
// ────────────────────────────────────────────────────────────────
FString FNiagaraConverter::RemapAllStructPaths(const FString& InText,
                                               TArray<FString>& OutWarnings)
{
    FString Text = InText;
    for (auto& KV : GetStructRemap())
    {
        if (KV.Key != KV.Value && KV.Key.StartsWith(TEXT("/Script/")))
            Text = Text.Replace(*KV.Key, *KV.Value);
    }
    return Text;
}

// ────────────────────────────────────────────────────────────────
//  ConvertNodes  — applies all line-level transforms
// ────────────────────────────────────────────────────────────────
FString FNiagaraConverter::ConvertNodes(const FString& RawNodeText,
                                        TArray<FString>& OutWarnings)
{
    FString Text = RawNodeText;

    Text = StripExportPaths(Text);
    Text = StripUE5PinProperties(Text);
    Text = StripPinFriendlyNames(Text);

    // PinSubCategoryObject conversion — per occurrence
    {
        FString Out;
        FRegexPattern Pat(TEXT("PinType\\.PinSubCategoryObject=([^,\\r\\n]+)"));
        FRegexMatcher M(Pat, Text);
        int32 Last = 0;
        while (M.FindNext())
        {
            Out += Text.Mid(Last, M.GetMatchBeginning() - Last);
            Out += ConvertSubCategoryObject(M.GetCaptureGroup(1), OutWarnings);
            Last  = M.GetMatchEnding();
        }
        Out += Text.Mid(Last);
        Text = Out;
    }

    // FunctionScript path conversion
    {
        TArray<FString> Lines;
        Text.ParseIntoArrayLines(Lines, false);
        for (FString& Line : Lines)
        {
            if (Line.Contains(TEXT("FunctionScript=")))
                Line = ConvertFunctionScript(Line);
        }
        Text = FString::Join(Lines, TEXT("\n"));
    }

    Text = RemapTypeIndices(Text);
    Text = RemapAllStructPaths(Text, OutWarnings);
    Text = FixSelectorPinType(Text, OutWarnings);

    // Collapse runs of multiple spaces left by removals  
    {
        FString Out;
        FRegexPattern Pat(TEXT("  +"));
        FRegexMatcher M(Pat, Text);
        int32 Last = 0;
        while (M.FindNext())
        {
            Out += Text.Mid(Last, M.GetMatchBeginning() - Last);
            Out += TEXT(" ");
            Last  = M.GetMatchEnding();
        }
        Out += Text.Mid(Last);
        Text = Out;
    }

    return Text;
}

// ────────────────────────────────────────────────────────────────
//  ParseScriptVarBlocks
//  Extract NiagaraScriptVariable sub-objects from the outer
//  NiagaraClipboardContent wrapper text.
// ────────────────────────────────────────────────────────────────
void FNiagaraConverter::ParseScriptVarBlocks(const FString& WrapperText,
                                             TArray<FNiagaraScriptVarBlock>& OutVars)
{
    TArray<FString> Lines;
    WrapperText.ParseIntoArrayLines(Lines, false);

    bool InVarBlock = false;
    FNiagaraScriptVarBlock Cur;

    for (const FString& RawLine : Lines)
    {
        FString Line = RawLine.TrimStart();

        // Start of a script-variable fill-in block
        if (Line.StartsWith(TEXT("Begin Object Name=\"NiagaraScriptVariable_")))
        {
            InVarBlock = true;
            Cur = FNiagaraScriptVarBlock();
            FRegexPattern P(TEXT("Name=\"(NiagaraScriptVariable_[^\"]+)\""));
            FRegexMatcher M(P, Line);
            if (M.FindNext()) Cur.ObjectName = M.GetCaptureGroup(1);
            continue;
        }
        if (InVarBlock && Line == TEXT("End Object"))
        {
            RemapScriptVarTypeIndices(Cur);
            OutVars.Add(Cur);
            InVarBlock = false;
            continue;
        }
        if (!InVarBlock) continue;

        // Properties
        if (Line.StartsWith(TEXT("Variable=")))
        {
            Cur.TypeHandle = TEXT("");
            // Extract Name="..."
            FRegexPattern PN(TEXT("Name=\"([^\"]+)\""));
            FRegexMatcher MN(PN, Line);
            if (MN.FindNext()) Cur.VariableName = MN.GetCaptureGroup(1);
            // Extract TypeDefHandle=(...)
            FRegexPattern PT(TEXT("TypeDefHandle=(\\([^)]+\\))"));
            FRegexMatcher MT(PT, Line);
            if (MT.FindNext()) Cur.TypeHandle = MT.GetCaptureGroup(1);
        }
        else if (Line.StartsWith(TEXT("DefaultMode=")))
        {
            Cur.DefaultMode = Line.Mid(12).TrimEnd();
        }
        else if (Line.StartsWith(TEXT("Metadata=")))
        {
            Cur.MetadataRaw = Line.Mid(9).TrimEnd();
        }
        else if (Line.StartsWith(TEXT("DefaultValueVariant=")))
        {
            Cur.DefaultValueVariantRaw = Line.Mid(20).TrimEnd();
        }
        else if (Line.StartsWith(TEXT("bIsStaticSwitch=")))
        {
            Cur.bIsStaticSwitch = Line.Contains(TEXT("True"));
        }
        else if (Line.StartsWith(TEXT("StaticSwitchDefaultValue=")))
        {
            Cur.StaticSwitchDefaultValue = FCString::Atoi(
                *Line.Mid(25).TrimEnd());
        }
        else if (Line.StartsWith(TEXT("ChangeId=")))
        {
            Cur.ChangeId = Line.Mid(9).TrimEnd();
        }
    }
}

void FNiagaraConverter::RemapScriptVarTypeIndices(FNiagaraScriptVarBlock& Var)
{
    const TMap<int32,int32> IndexMap =
    {
        {108, 47}, {112, 50}, {114, 52}, {115, 53}, {117, 55}, {99, 99}
    };
    for (auto& KV : IndexMap)
    {
        FString From = FString::Printf(TEXT("RegisteredTypeIndex=%d"), KV.Key);
        FString To   = FString::Printf(TEXT("RegisteredTypeIndex=%d"), KV.Value);
        Var.TypeHandle = Var.TypeHandle.Replace(*From, *To);
    }
}

// ────────────────────────────────────────────────────────────────
//  UnwrapClipboardContent
//  If the text is a NiagaraClipboardContent wrapper, extract and
//  base-64 decode ExportedNodes. Also parse ScriptVariable blocks.
// ────────────────────────────────────────────────────────────────
FString FNiagaraConverter::UnwrapClipboardContent(const FString& Text,
                                                  TArray<FNiagaraScriptVarBlock>& OutVars,
                                                  bool& bWasWrapped)
{
    bWasWrapped = false;
    if (!Text.Contains(TEXT("NiagaraClipboardContent")))
        return Text;

    bWasWrapped = true;

    // Parse ScriptVariable blocks from the wrapper
    ParseScriptVarBlocks(Text, OutVars);

    // Find ExportedNodes="<base64>"
    FRegexPattern Pat(TEXT("ExportedNodes=\"([^\"]+)\""));
    FRegexMatcher M(Pat, Text);
    if (!M.FindNext())
        return Text;   // no ExportedNodes — treat as raw

    FString B64 = M.GetCaptureGroup(1);
    TArray<uint8> Decoded;
    if (!FBase64::Decode(B64, Decoded))
        return TEXT("");   // decode failure — caller must error-out

    FString NodeText = FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(Decoded.GetData())));

    // ── NiagaraNodeIf OutputVars patch ──────────────────────────────────────
    //
    //  UEFN clipboard format only serializes OutputVars(0) on NiagaraNodeIf
    //  nodes.  The engine's UNiagaraNodeIf::ResolveNumerics unconditionally
    //  reads OutputVars[1], crashing with an out-of-bounds assert if the array
    //  has fewer than 2 entries.
    //
    //  Fix: after every OutputVars(0)=(...) line that appears inside a
    //  NiagaraNodeIf block, synthesize a matching OutputVars(1)=(...) line
    //  with the same type so the engine always finds two entries.
    {
        // Regex: capture the OutputVars(0)=(...) line (with its line ending)
        // We match only when preceded by a NiagaraNodeIf Begin Object, and
        // only when OutputVars(1) is NOT already present right after.
        // Simpler approach: scan block by block.
        TArray<FString> Lines;
        NodeText.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);

        TArray<FString> OutLines;
        OutLines.Reserve(Lines.Num() + 8);

        bool bInNodeIf = false;
        bool bInjectedVar1 = false;

        for (int32 i = 0; i < Lines.Num(); ++i)
        {
            const FString& Line = Lines[i];
            FString Trimmed = Line.TrimStart();

            // Track entry/exit of NiagaraNodeIf blocks
            if (Trimmed.StartsWith(TEXT("Begin Object Class=/Script/NiagaraEditor.NiagaraNodeIf")))
            {
                bInNodeIf = true;
                bInjectedVar1 = false;
            }
            else if (bInNodeIf && Trimmed == TEXT("End Object"))
            {
                bInNodeIf = false;
            }

            OutLines.Add(Line);

            // After OutputVars(0)=(...) inside a NodeIf block, inject (1) if missing
            if (bInNodeIf && !bInjectedVar1 && Trimmed.StartsWith(TEXT("OutputVars(0)=")))
            {
                // Peek ahead — only inject if OutputVars(1) is not already there
                bool bAlreadyHas1 = (i + 1 < Lines.Num()) &&
                    Lines[i + 1].TrimStart().StartsWith(TEXT("OutputVars(1)="));

                if (!bAlreadyHas1)
                {
                    // Extract the value portion: everything after "OutputVars(0)="
                    FString ValuePart = Trimmed.Mid(FString(TEXT("OutputVars(0)=")).Len());
                    // Build the injected line with the same indentation
                    FString Indent = Line.Left(Line.Len() - Line.TrimStart().Len());
                    OutLines.Add(Indent + TEXT("OutputVars(1)=") + ValuePart);
                    bInjectedVar1 = true;
                }
                else
                {
                    bInjectedVar1 = true; // already present, nothing to do
                }
            }
        }

        NodeText = FString::Join(OutLines, TEXT("\n"));
    }

    return NodeText;
}

// ────────────────────────────────────────────────────────────────
//  Convert  — public entry point
// ────────────────────────────────────────────────────────────────
FNiagaraConvertResult FNiagaraConverter::Convert(const FString& InputText)
{
    FNiagaraConvertResult Result;

    bool bWasWrapped = false;
    FString RawNodes = UnwrapClipboardContent(InputText, Result.ScriptVars, bWasWrapped);

    if (RawNodes.IsEmpty())
    {
        Result.ErrorMessage = TEXT("Failed to decode ExportedNodes base64 blob from NiagaraClipboardContent.");
        return Result;
    }

    Result.NodeText = ConvertNodes(RawNodes, Result.Warnings);
    Result.bSuccess = true;
    return Result;
}