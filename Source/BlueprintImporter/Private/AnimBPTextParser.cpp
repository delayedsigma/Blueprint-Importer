#include "AnimBPTextParser.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static FString TrimQuotes(const FString& S)
{
    FString T = S.TrimStartAndEnd();
    if (T.StartsWith(TEXT("\"")) && T.EndsWith(TEXT("\"")))
        T = T.Mid(1, T.Len() - 2);
    return T;
}

static FString ExtractValue(const FString& Line, const FString& Key)
{
    FString Search1 = FString::Printf(TEXT("\"%s\":"), *Key);
    FString Search2 = FString::Printf(TEXT("%s ="), *Key);
    FString Search3 = FString::Printf(TEXT("%s:"), *Key);

    int32 Idx = INDEX_NONE;
    int32 Advance = 0;

    auto TryFind = [&](const FString& Pat) -> bool
    {
        int32 Found = Line.Find(Pat, ESearchCase::IgnoreCase);
        if (Found != INDEX_NONE) { Idx = Found; Advance = Pat.Len(); return true; }
        return false;
    };

    if (!TryFind(Search1) && !TryFind(Search2) && !TryFind(Search3))
        return TEXT("");

    FString Rest = Line.Mid(Idx + Advance).TrimStart();
    while (!Rest.IsEmpty() && (Rest[Rest.Len() - 1] == ',' ||
                               Rest[Rest.Len() - 1] == ';' ||
                               Rest[Rest.Len() - 1] == '}'))
        Rest = Rest.LeftChop(1).TrimEnd();
    return Rest.TrimStartAndEnd();
}

bool FAnimBPTextParser::TryGetIntField(const FString& Line, const FString& Key, int32& OutVal)
{
    FString V = ExtractValue(Line, Key);
    if (V.IsEmpty()) return false;
    OutVal = FCString::Atoi(*V);
    return true;
}

bool FAnimBPTextParser::TryGetFloatField(const FString& Line, const FString& Key, float& OutVal)
{
    FString V = ExtractValue(Line, Key);
    if (V.IsEmpty()) return false;
    OutVal = FCString::Atof(*V);
    return true;
}

bool FAnimBPTextParser::TryGetBoolField(const FString& Line, const FString& Key, bool& OutVal)
{
    FString V = ExtractValue(Line, Key);
    if (V.IsEmpty()) return false;
    V = V.ToLower();
    OutVal = (V == TEXT("true") || V == TEXT("1"));
    return true;
}

bool FAnimBPTextParser::TryGetFNameField(const FString& Line, const FString& Key, FString& OutVal)
{
    FString V = ExtractValue(Line, Key);
    if (V.IsEmpty()) return false;
    if (V.StartsWith(TEXT("FName(")))
    {
        V = V.Mid(6);
        V.RemoveFromEnd(TEXT(")"));
    }
    OutVal = TrimQuotes(V);
    return true;
}

bool FAnimBPTextParser::TryGetStringField(const FString& Line, const FString& Key, FString& OutVal)
{
    FString V = ExtractValue(Line, Key);
    if (V.IsEmpty()) return false;
    OutVal = TrimQuotes(V);
    return true;
}

TArray<int32> FAnimBPTextParser::ParseIntSet(const FString& SetBlock)
{
    TArray<int32> Result;
    FString Inner = SetBlock;
    Inner.RemoveFromStart(TEXT("{"));
    Inner.RemoveFromEnd(TEXT("}"));
    TArray<FString> Parts;
    Inner.ParseIntoArray(Parts, TEXT(","), true);
    for (FString& P : Parts)
    {
        P = P.TrimStartAndEnd();
        if (!P.IsEmpty() && P[0] != '}' && P[0] != '{')
            Result.Add(FCString::Atoi(*P));
    }
    return Result;
}

// ---------------------------------------------------------------------------
// Member variable line parser
// ---------------------------------------------------------------------------

bool FAnimBPTextParser::TryParseMemberVarLine(const FString& Line, FAnimBPMemberVarData& Out, bool bPrivate)
{
    FString L = Line.TrimStartAndEnd();

    if (L.IsEmpty() || L.StartsWith(TEXT("//")) || L.StartsWith(TEXT("{"))
        || L.StartsWith(TEXT("}")) || L.StartsWith(TEXT("public:"))
        || L.StartsWith(TEXT("private:")) || L.StartsWith(TEXT("TArray<"))
        || L.StartsWith(TEXT("TMap<")))
        return false;

    FString Work = L;
    Work.RemoveFromStart(TEXT("public "));
    Work.RemoveFromStart(TEXT("private "));

    FString TypePrefix;
    if (Work.StartsWith(TEXT("struct ")))
    {
        TypePrefix = TEXT("struct ");
        Work = Work.Mid(7).TrimStart();
    }
    else if (Work.StartsWith(TEXT("class ")))
    {
        TypePrefix = TEXT("class ");
        Work = Work.Mid(6).TrimStart();
    }

    TArray<FString> Tokens;
    Work.ParseIntoArray(Tokens, TEXT(" "), true);

    if (Tokens.Num() < 2)
        return false;

    FString TypeToken = Tokens[0];
    TypeToken.ReplaceInline(TEXT("*"), TEXT(""));

    if (TypeToken.IsEmpty() || (!FChar::IsUpper(TypeToken[0]) && TypeToken[0] != '_'))
        return false;

    FString NameToken = Tokens[1];
    NameToken.RemoveFromEnd(TEXT(";"));
    NameToken.RemoveFromEnd(TEXT(","));

    int32 EqIdx;
    FString VarName = NameToken;
    if (VarName.FindChar('=', EqIdx))
        VarName = VarName.Left(EqIdx).TrimEnd();

    bool bValidName = !VarName.IsEmpty();
    for (TCHAR C : VarName)
    {
        if (!FChar::IsAlnum(C) && C != '_')
        {
            bValidName = false;
            break;
        }
    }
    if (!bValidName) return false;

    FString Default;
    int32 FirstEq = L.Find(TEXT(" = "));
    if (FirstEq != INDEX_NONE)
    {
        Default = L.Mid(FirstEq + 3).TrimStartAndEnd();
        Default.RemoveFromEnd(TEXT(";"));
        Default = Default.TrimEnd();
    }

    Out.TypeName    = TypeToken;
    Out.VarName     = VarName;
    Out.DefaultValue = Default;
    Out.bIsPrivate  = bPrivate;
    return true;
}

// ---------------------------------------------------------------------------
// Type inference for local variables
// ---------------------------------------------------------------------------

FString FAnimBPTextParser::InferTypeFromName(const FString& VarName, const TArray<FAnimBPMemberVarData>& MemberVars)
{
    for (const FAnimBPMemberVarData& MV : MemberVars)
    {
        if (MV.VarName == VarName)
            return MV.TypeName;
    }

    FString N = VarName.ToLower();

    if (N.Contains(TEXT("rotator")) || N.EndsWith(TEXT("_rot")) || N.StartsWith(TEXT("hairrotator")))
        return TEXT("FRotator");
    if (N.Contains(TEXT("vector")) || N.Contains(TEXT("gravity")) || N.EndsWith(TEXT("_vec")))
        return TEXT("FVector");
    if (N.Contains(TEXT("transform")))
        return TEXT("FTransform");
    if (N.Contains(TEXT("color")) || N.Contains(TEXT("colour")))
        return TEXT("FLinearColor");
    if (N.StartsWith(TEXT("b")) && N.Len() > 1 && FChar::IsUpper(VarName[1]))
        return TEXT("bool");
    if (N.Contains(TEXT("time")) || N.Contains(TEXT("speed")) || N.Contains(TEXT("alpha"))
        || N.Contains(TEXT("weight")) || N.Contains(TEXT("scale")) || N.Contains(TEXT("delta")))
        return TEXT("float");
    if (N.Contains(TEXT("index")) || N.Contains(TEXT("count")) || N.EndsWith(TEXT("_i")))
        return TEXT("int32");
    if (N.Contains(TEXT("name")) || N.Contains(TEXT("string")) || N.Contains(TEXT("text")))
        return TEXT("FString");

    if (VarName.StartsWith(TEXT("CallFunc_")))
    {
        FString Suffix = VarName.Mid(9).ToLower();
        if (Suffix.Contains(TEXT("rotator")))  return TEXT("FRotator");
        if (Suffix.Contains(TEXT("vector")))   return TEXT("FVector");
        if (Suffix.Contains(TEXT("transform")))return TEXT("FTransform");
        if (Suffix.Contains(TEXT("bool")))     return TEXT("bool");
        if (Suffix.Contains(TEXT("float")) || Suffix.Contains(TEXT("time")) || Suffix.Contains(TEXT("seconds")))
            return TEXT("float");
        if (Suffix.Contains(TEXT("int")))      return TEXT("int32");
        if (Suffix.Contains(TEXT("string")) || Suffix.Contains(TEXT("name")))
            return TEXT("FString");
    }

    return TEXT("");
}

// ---------------------------------------------------------------------------
// Statement parser
// ---------------------------------------------------------------------------

FAnimBPStatement FAnimBPTextParser::ParseStatement(const FString& Line)
{
    FAnimBPStatement Stmt;
    Stmt.RawLine = Line;

    FString L = Line.TrimStartAndEnd();

    FString Work = L;
    if (Work.EndsWith(TEXT(";")))
        Work = Work.LeftChop(1).TrimEnd();

    if (Work.EndsWith(TEXT(":")) && !Work.Contains(TEXT("(")) && !Work.Contains(TEXT("=")))
    {
        Stmt.Kind      = EAnimBPStmtKind::Label;
        Stmt.LabelName = Work.LeftChop(1).TrimEnd();
        return Stmt;
    }

    if (Work.StartsWith(TEXT("goto ")))
    {
        Stmt.Kind      = EAnimBPStmtKind::Goto;
        Stmt.LabelName = Work.Mid(5).TrimStartAndEnd();
        return Stmt;
    }

    if (Work == TEXT("return") || Work.StartsWith(TEXT("return ")))
    {
        Stmt.Kind = EAnimBPStmtKind::Return;
        return Stmt;
    }

    int32 AssignIdx = INDEX_NONE;
    {
        int32 Depth = 0;
        for (int32 i = 0; i < Work.Len(); ++i)
        {
            TCHAR C = Work[i];
            if (C == '(' || C == '<') ++Depth;
            else if (C == ')' || C == '>') --Depth;
            else if (C == '=' && Depth == 0)
            {
                bool bPrevBang  = (i > 0 && Work[i-1] == '!');
                bool bNextEq    = (i + 1 < Work.Len() && Work[i+1] == '=');
                bool bPrevEq    = (i > 0 && Work[i-1] == '=');
                if (!bPrevBang && !bNextEq && !bPrevEq)
                {
                    AssignIdx = i;
                    break;
                }
            }
        }
    }

    if (AssignIdx != INDEX_NONE)
    {
        Stmt.Kind = EAnimBPStmtKind::Assignment;
        Stmt.LHS  = Work.Left(AssignIdx).TrimEnd();
        Stmt.RHS  = Work.Mid(AssignIdx + 1).TrimStart();

        int32 ParenOpen = INDEX_NONE;
        int32 Depth = 0;
        for (int32 i = 0; i < Stmt.RHS.Len(); ++i)
        {
            TCHAR C = Stmt.RHS[i];
            if (C == '<') ++Depth;
            else if (C == '>') --Depth;
            else if (C == '(' && Depth == 0) { ParenOpen = i; break; }
        }

        if (ParenOpen != INDEX_NONE)
        {
            FString CallName = Stmt.RHS.Left(ParenOpen).TrimEnd();
            if (CallName.Len() > 1 && CallName[0] == 'F' && FChar::IsUpper(CallName[1]) && !CallName.Contains(TEXT("->")))
                ParenOpen = INDEX_NONE;
        }

        if (ParenOpen != INDEX_NONE)
        {
            int32 LastArrowR = INDEX_NONE, LastColonR = INDEX_NONE;
            int32 DepthR = 0;
            for (int32 i = 0; i < Stmt.RHS.Len(); ++i)
            {
                TCHAR C = Stmt.RHS[i];
                if (C == '(' || C == '<' || C == '{') ++DepthR;
                else if (C == ')' || C == '>' || C == '}') --DepthR;
                else if (DepthR == 0)
                {
                    if (C == '-' && i+1 < Stmt.RHS.Len() && Stmt.RHS[i+1] == '>')
                        LastArrowR = i;
                    else if (C == ':' && i+1 < Stmt.RHS.Len() && Stmt.RHS[i+1] == ':')
                        LastColonR = i;
                }
            }

            bool bHasSep = (LastArrowR != INDEX_NONE || LastColonR != INDEX_NONE);
            if (bHasSep)
            {
                int32 SepIdx = INDEX_NONE;
                if (LastArrowR != INDEX_NONE && (LastColonR == INDEX_NONE || LastArrowR > LastColonR))
                    SepIdx = LastArrowR;
                else
                    SepIdx = LastColonR;

                Stmt.FuncObject = Stmt.RHS.Left(SepIdx).TrimEnd();
                FString AfterSep = Stmt.RHS.Mid(SepIdx + 2).TrimStart();
                int32 FuncParen = AfterSep.Find(TEXT("("));
                if (FuncParen != INDEX_NONE)
                {
                    Stmt.FuncName = AfterSep.Left(FuncParen).TrimEnd();
                    FString ArgsStr2 = AfterSep.Mid(FuncParen + 1);
                    int32 LP2;
                    if (ArgsStr2.FindLastChar(')', LP2)) ArgsStr2 = ArgsStr2.Left(LP2);
                    FString CA2; int32 AD2 = 0;
                    for (TCHAR C : ArgsStr2)
                    {
                        if (C=='('||C=='<'||C=='{') ++AD2;
                        else if (C==')'||C=='>'||C=='}') --AD2;
                        else if (C==',' && AD2==0) { FString A=CA2.TrimStartAndEnd(); if(!A.IsEmpty()) Stmt.Args.Add(A); CA2.Empty(); continue; }
                        CA2.AppendChar(C);
                    }
                    FString A2=CA2.TrimStartAndEnd(); if(!A2.IsEmpty()) Stmt.Args.Add(A2);
                }
                else Stmt.FuncName = AfterSep;
            }
            else
            {
                FString CallExpr = Stmt.RHS.Left(ParenOpen).TrimEnd();
                Stmt.FuncName = CallExpr;

                FString ArgsStr = Stmt.RHS.Mid(ParenOpen + 1);
                int32 LastParen;
                if (ArgsStr.FindLastChar(')', LastParen))
                    ArgsStr = ArgsStr.Left(LastParen);

                FString CurrentArg;
                int32 ArgDepth = 0;
                for (TCHAR C : ArgsStr)
                {
                    if (C == '(' || C == '<' || C == '{') ++ArgDepth;
                    else if (C == ')' || C == '>' || C == '}') --ArgDepth;
                    else if (C == ',' && ArgDepth == 0)
                    {
                        FString A = CurrentArg.TrimStartAndEnd();
                        if (!A.IsEmpty()) Stmt.Args.Add(A);
                        CurrentArg.Empty();
                        continue;
                    }
                    CurrentArg.AppendChar(C);
                }
                FString A = CurrentArg.TrimStartAndEnd();
                if (!A.IsEmpty()) Stmt.Args.Add(A);
            }

            Stmt.Kind = EAnimBPStmtKind::FunctionCall;
        }
        return Stmt;
    }

    {
        int32 LastArrow = INDEX_NONE;
        int32 LastColon = INDEX_NONE;
        int32 Depth2 = 0;
        for (int32 i = 0; i < Work.Len(); ++i)
        {
            TCHAR C = Work[i];
            if (C == '(' || C == '<' || C == '{') ++Depth2;
            else if (C == ')' || C == '>' || C == '}') --Depth2;
            else if (Depth2 == 0)
            {
                if (C == '-' && i + 1 < Work.Len() && Work[i+1] == '>')
                    LastArrow = i;
                else if (C == ':' && i + 1 < Work.Len() && Work[i+1] == ':')
                    LastColon = i;
            }
        }

        int32 SepIdx = INDEX_NONE;
        bool bIsArrow = false;
        if (LastArrow != INDEX_NONE && (LastColon == INDEX_NONE || LastArrow > LastColon))
            { SepIdx = LastArrow; bIsArrow = true; }
        else if (LastColon != INDEX_NONE)
            { SepIdx = LastColon; bIsArrow = false; }

        if (SepIdx != INDEX_NONE)
        {
            Stmt.Kind = EAnimBPStmtKind::FunctionCall;
            Stmt.FuncObject = Work.Left(SepIdx).TrimEnd();
            FString AfterSep = Work.Mid(SepIdx + 2).TrimStart();
            int32 FuncParen = AfterSep.Find(TEXT("("));
            if (FuncParen != INDEX_NONE)
            {
                Stmt.FuncName = AfterSep.Left(FuncParen).TrimEnd();
                FString ArgsStr = AfterSep.Mid(FuncParen + 1);
                int32 LastParen;
                if (ArgsStr.FindLastChar(')', LastParen))
                    ArgsStr = ArgsStr.Left(LastParen);

                FString CurrentArg;
                int32 ArgDepth = 0;
                for (TCHAR C : ArgsStr)
                {
                    if (C == '(' || C == '<' || C == '{') ++ArgDepth;
                    else if (C == ')' || C == '>' || C == '}') --ArgDepth;
                    else if (C == ',' && ArgDepth == 0)
                    {
                        FString A = CurrentArg.TrimStartAndEnd();
                        if (!A.IsEmpty()) Stmt.Args.Add(A);
                        CurrentArg.Empty();
                        continue;
                    }
                    CurrentArg.AppendChar(C);
                }
                FString A = CurrentArg.TrimStartAndEnd();
                if (!A.IsEmpty()) Stmt.Args.Add(A);
            }
            else
            {
                Stmt.FuncName = AfterSep;
            }
            return Stmt;
        }
    }

    {
        int32 ParenOpen = Work.Find(TEXT("("));
        if (ParenOpen != INDEX_NONE)
        {
            Stmt.Kind = EAnimBPStmtKind::FunctionCall;
            Stmt.FuncName = Work.Left(ParenOpen).TrimEnd();
            FString ArgsStr = Work.Mid(ParenOpen + 1);
            int32 LastParen;
            if (ArgsStr.FindLastChar(')', LastParen))
                ArgsStr = ArgsStr.Left(LastParen);

            FString CurrentArg;
            int32 ArgDepth = 0;
            for (TCHAR C : ArgsStr)
            {
                if (C == '(' || C == '<' || C == '{') ++ArgDepth;
                else if (C == ')' || C == '>' || C == '}') --ArgDepth;
                else if (C == ',' && ArgDepth == 0)
                {
                    FString A = CurrentArg.TrimStartAndEnd();
                    if (!A.IsEmpty()) Stmt.Args.Add(A);
                    CurrentArg.Empty();
                    continue;
                }
                CurrentArg.AppendChar(C);
            }
            {
                FString A = CurrentArg.TrimStartAndEnd();
                if (!A.IsEmpty()) Stmt.Args.Add(A);
            }
            return Stmt;
        }
    }

    Stmt.Kind = EAnimBPStmtKind::Unknown;
    return Stmt;
}

// ---------------------------------------------------------------------------
// Local variable inference
// ---------------------------------------------------------------------------

void FAnimBPTextParser::InferLocalVars(FAnimBPFunctionData& Func)
{
    TSet<FString> Known;
    for (const FString& P : Func.ParamNames)
        Known.Add(P);

    TArray<FAnimBPLocalVarData> Found;
    TSet<FString> Seen;

    auto AddVar = [&](const FString& Name, const FString& Type)
    {
        if (Name.IsEmpty() || Seen.Contains(Name)) return;
        if (Name == TEXT("this") || Name == TEXT("true") || Name == TEXT("false")) return;
        if (Name[0] == '"' || FChar::IsDigit(Name[0]) || Name[0] == '-') return;
        if (Name.Contains(TEXT("("))) return;
        if (Known.Contains(Name)) return;

        Seen.Add(Name);
        FAnimBPLocalVarData LV;
        LV.VarName     = Name;
        LV.InferredType = Type;
        Found.Add(LV);
    };

    for (const FAnimBPStatement& S : Func.Statements)
    {
        if (S.Kind == EAnimBPStmtKind::Assignment || S.Kind == EAnimBPStmtKind::FunctionCall)
        {
            if (!S.LHS.IsEmpty())
                AddVar(S.LHS, TEXT(""));
            for (const FString& Arg : S.Args)
            {
                FString A = Arg.TrimStartAndEnd();
                bool bLooksLikeVar = !A.IsEmpty();
                for (TCHAR C : A)
                {
                    if (!FChar::IsAlnum(C) && C != '_')
                    {
                        bLooksLikeVar = false;
                        break;
                    }
                }
                if (bLooksLikeVar && !A.IsEmpty() && !FChar::IsDigit(A[0]))
                    AddVar(A, TEXT(""));
            }
        }
    }

    Func.LocalVars = Found;
}

// ---------------------------------------------------------------------------
// Member variable block parsing
// ---------------------------------------------------------------------------

void FAnimBPTextParser::ParseMemberVars(FParseCtx& Ctx, TArray<FAnimBPMemberVarData>& Out, bool bPrivate)
{
    while (!Ctx.AtEnd())
    {
        FString Line = Ctx.Peek();

        if (Line == TEXT("public:") || Line == TEXT("private:") || Line == TEXT("protected:"))
            break;
        if (Line.StartsWith(TEXT("public void ")) || Line.StartsWith(TEXT("private void "))
            || Line.StartsWith(TEXT("public ")) || Line.StartsWith(TEXT("private ")))
        {
            if (Line.Contains(TEXT("(")))
                break;
        }
        if (Line == TEXT("};") || Line == TEXT("}"))
            break;

        Ctx.Consume();

        int32 Depth = 0;
        for (TCHAR C : Line) { if (C == '{') ++Depth; else if (C == '}') --Depth; }

        while (Depth > 0 && !Ctx.AtEnd())
        {
            FString Next = Ctx.Consume();
            for (TCHAR C : Next) { if (C == '{') ++Depth; else if (C == '}') --Depth; }
        }

        if ((Line.StartsWith(TEXT("TArray<")) || Line.StartsWith(TEXT("TMap<")))
            && Line.Contains(TEXT("{")))
        {
            continue;
        }

        FAnimBPMemberVarData Var;
        if (TryParseMemberVarLine(Line, Var, bPrivate))
            Out.Add(Var);
    }
}

// ---------------------------------------------------------------------------
// Function body parsing
// ---------------------------------------------------------------------------

bool FAnimBPTextParser::ParseFunctionBody(FParseCtx& Ctx, FAnimBPFunctionData& Out)
{
    int32 Depth = 0;
    while (!Ctx.AtEnd())
    {
        FString L = Ctx.Peek();
        if (L.Contains(TEXT("{")))
        {
            Ctx.Consume();
            for (TCHAR C : L) { if (C == '{') ++Depth; else if (C == '}') --Depth; }
            break;
        }
        Ctx.Consume();
    }

    while (!Ctx.AtEnd() && Depth > 0)
    {
        FString Raw = Ctx.Consume();
        FString Line = Raw.TrimStartAndEnd();

        for (TCHAR C : Line) { if (C == '{') ++Depth; else if (C == '}') --Depth; }
        if (Depth <= 0) break;

        if (Line.IsEmpty() || Line.StartsWith(TEXT("//")))
            continue;

        FAnimBPStatement Stmt = ParseStatement(Line);
        Out.Statements.Add(Stmt);
    }

    InferLocalVars(Out);
    return true;
}

// ---------------------------------------------------------------------------
// Main entry
// ---------------------------------------------------------------------------

bool FAnimBPTextParser::Parse(const FString& Text, FAnimBPTextData& OutData, FString& OutError)
{
    if (Text.IsEmpty())
    {
        OutError = TEXT("Input text is empty.");
        return false;
    }

    FParseCtx Ctx;
    Text.ParseIntoArrayLines(Ctx.Lines, false);

    if (!ParseClassHeader(Ctx, OutData))
    {
        OutData.ClassName       = TEXT("UnknownAnimBP_C");
        OutData.ParentClassName = TEXT("AnimInstance");
        Ctx.LineIdx = 0;
    }

    bool bInPublic  = true;

    while (!Ctx.AtEnd())
    {
        FString Line = Ctx.Peek();

        if (Line == TEXT("public:"))
        {
            bInPublic = true;
            Ctx.Consume();
            ParseMemberVars(Ctx, OutData.MemberVars, false);
            continue;
        }
        if (Line == TEXT("private:"))
        {
            bInPublic = false;
            Ctx.Consume();
            ParseMemberVars(Ctx, OutData.MemberVars, true);
            continue;
        }

        if (Line.Contains(TEXT("BakedStateMachines")))
        {
            ParseStateMachinesArray(Ctx, OutData.StateMachines);
            continue;
        }

        bool bPrivateFunc = Line.StartsWith(TEXT("private void ")) || Line.StartsWith(TEXT("// (")) ;
        bool bPublicFunc  = Line.StartsWith(TEXT("public void "));
        if (bPrivateFunc || bPublicFunc)
        {
            if (Line.StartsWith(TEXT("//")))
            {
                Ctx.Consume();
                continue;
            }

            Ctx.Consume();

            FAnimBPFunctionData Func;
            Func.bIsPrivate = bPrivateFunc;

            FString Sig = bPrivateFunc
                ? Line.Mid(Line.Find(TEXT("private void ")) + 13)
                : Line.Mid(Line.Find(TEXT("public void "))  + 12);

            int32 ParenOpen;
            if (Sig.FindChar('(', ParenOpen))
            {
                Func.FunctionName = Sig.Left(ParenOpen).TrimStartAndEnd();

                FString ParamStr = Sig.Mid(ParenOpen + 1);
                int32 ParenClose;
                if (ParamStr.FindChar(')', ParenClose))
                    ParamStr = ParamStr.Left(ParenClose);

                TArray<FString> Params;
                ParamStr.ParseIntoArray(Params, TEXT(","), true);
                for (FString& P : Params)
                {
                    P = P.TrimStartAndEnd();
                    P.RemoveFromStart(TEXT("struct "));
                    P.RemoveFromStart(TEXT("const "));
                    P.ReplaceInline(TEXT("&"), TEXT(""));
                    TArray<FString> Words;
                    P.ParseIntoArray(Words, TEXT(" "), true);
                    if (Words.Num() > 0)
                        Func.ParamNames.Add(Words.Last().TrimStartAndEnd());
                }
            }
            else
            {
                Func.FunctionName = Sig.TrimStartAndEnd();
            }

            if (!Func.FunctionName.IsEmpty())
            {
                ParseFunctionBody(Ctx, Func);
                OutData.Functions.Add(Func);

                static const TArray<FString> Reserved = {
                    TEXT("ExecuteUbergraph"), TEXT("AnimGraph"),
                    TEXT("BlueprintUpdateAnimation"), TEXT("NativeUpdateAnimation")
                };
                bool bReserved = false;
                for (const FString& R : Reserved)
                    if (Func.FunctionName.StartsWith(R)) { bReserved = true; break; }

                if (!bReserved)
                {
                    FAnimBPLayerFunctionData LF;
                    LF.FunctionName = Func.FunctionName;
                    LF.ParamNames   = Func.ParamNames;
                    OutData.LayerFunctions.Add(LF);
                }
            }
            continue;
        }

        {
            FString CheckLine = Line;
            if (CheckLine.StartsWith(TEXT("struct ")) || CheckLine.StartsWith(TEXT("class ")))
            {
                FAnimBPMemberVarData Var;
                if (TryParseMemberVarLine(CheckLine, Var, !bInPublic))
                {
                    Ctx.Consume();
                    int32 Depth = 0;
                    for (TCHAR C : CheckLine) { if (C == '{') ++Depth; else if (C == '}') --Depth; }
                    while (Depth > 0 && !Ctx.AtEnd())
                    {
                        FString Next = Ctx.Consume();
                        for (TCHAR C : Next) { if (C == '{') ++Depth; else if (C == '}') --Depth; }
                    }
                    OutData.MemberVars.Add(Var);
                    continue;
                }
            }
        }

        Ctx.Consume();
    }

    if (OutData.ClassName.IsEmpty())
    {
        OutError = TEXT("No class name found.");
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Class header
// ---------------------------------------------------------------------------

bool FAnimBPTextParser::ParseClassHeader(FParseCtx& Ctx, FAnimBPTextData& Out)
{
    while (!Ctx.AtEnd())
    {
        FString Line = Ctx.Consume();
        if (Line.StartsWith(TEXT("class ")) || Line.Contains(TEXT("class U")))
        {
            FString Rest = Line.Mid(6).TrimStart();
            int32 ColonIdx;
            if (Rest.FindChar(':', ColonIdx))
            {
                Out.ClassName = Rest.Left(ColonIdx).TrimEnd();
                FString AfterColon = Rest.Mid(ColonIdx + 1).TrimStart();
                AfterColon.RemoveFromStart(TEXT("public "));
                AfterColon.RemoveFromStart(TEXT("private "));
                AfterColon.RemoveFromStart(TEXT("protected "));
                int32 BraceIdx;
                if (AfterColon.FindChar('{', BraceIdx))
                    AfterColon = AfterColon.Left(BraceIdx);
                Out.ParentClassName = AfterColon.TrimStartAndEnd();
            }
            else
            {
                Out.ClassName = Rest.TrimStartAndEnd();
            }
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// BakedStateMachines
// ---------------------------------------------------------------------------

bool FAnimBPTextParser::ParseStateMachinesArray(FParseCtx& Ctx, TArray<FAnimBPStateMachineData>& Out)
{
    while (!Ctx.AtEnd())
    {
        FString Line = Ctx.Peek();
        if (Line.Contains(TEXT("BakedStateMachines")))
        {
            Ctx.Consume();
            while (!Ctx.AtEnd())
            {
                FString L = Ctx.Peek();
                if (L.Contains(TEXT("{"))) { Ctx.Consume(); break; }
                if (!L.IsEmpty() && !L.StartsWith(TEXT("//"))) break;
                Ctx.Consume();
            }
            while (!Ctx.AtEnd())
            {
                Ctx.SkipBlanks();
                if (Ctx.AtEnd()) break;
                FString L = Ctx.Peek();
                if (L == TEXT("}") || L == TEXT("};") || L == TEXT("},"))
                {
                    Ctx.Consume(); break;
                }
                if (L == TEXT("{"))
                {
                    Ctx.Consume();
                    FAnimBPStateMachineData Machine;
                    ParseSingleMachine(Ctx, Machine);
                    if (!Machine.MachineName.IsEmpty())
                        Out.Add(Machine);
                }
                else
                {
                    Ctx.Consume();
                }
            }
            return true;
        }
        else if (Line.StartsWith(TEXT("}")) || Line.Contains(TEXT("private void")) || Line.Contains(TEXT("public void")))
            break;
        else
            Ctx.Consume();
    }
    return false;
}

bool FAnimBPTextParser::ParseSingleMachine(FParseCtx& Ctx, FAnimBPStateMachineData& Out)
{
    int32 Depth = 1;
    while (!Ctx.AtEnd() && Depth > 0)
    {
        FString Line = Ctx.Consume();
        for (TCHAR C : Line) { if (C == '{') ++Depth; else if (C == '}') --Depth; }
        if (Depth <= 0) break;

        FString Name;
        if (TryGetFNameField(Line, TEXT("MachineName"), Name)) Out.MachineName = Name;
        int32 InitState;
        if (TryGetIntField(Line, TEXT("InitialState"), InitState)) Out.InitialState = InitState;
        if (Line.Contains(TEXT("\"States\":"))) ParseStatesArray(Ctx, Out.States);
        if (Line.Contains(TEXT("\"Transitions\":")) && !Line.Contains(TEXT("\"CanTakeDelegateIndex\":")))
            ParseMachineTransitionsArray(Ctx, Out.Transitions);
    }
    return true;
}

bool FAnimBPTextParser::ParseStatesArray(FParseCtx& Ctx, TArray<FAnimBPStateData>& Out)
{
    while (!Ctx.AtEnd())
    {
        FString L = Ctx.Peek();
        if (L.Contains(TEXT("{"))) { Ctx.Consume(); break; }
        if (!L.IsEmpty() && !L.StartsWith(TEXT("//"))) break;
        Ctx.Consume();
    }
    while (!Ctx.AtEnd())
    {
        Ctx.SkipBlanks();
        if (Ctx.AtEnd()) break;
        FString L = Ctx.Peek();
        if (L == TEXT("}") || L == TEXT("},") || L == TEXT("};")) { Ctx.Consume(); break; }
        if (L == TEXT("{")) { Ctx.Consume(); FAnimBPStateData State; ParseSingleState(Ctx, State); Out.Add(State); }
        else Ctx.Consume();
    }
    return true;
}

bool FAnimBPTextParser::ParseSingleState(FParseCtx& Ctx, FAnimBPStateData& Out)
{
    int32 Depth = 1;
    while (!Ctx.AtEnd() && Depth > 0)
    {
        FString Line = Ctx.Peek();
        int32 O = 0, C = 0;
        for (TCHAR Ch : Line) { if (Ch == '{') ++O; else if (Ch == '}') ++C; }
        Depth += O - C;
        if (Depth <= 0) { Ctx.Consume(); break; }
        Ctx.Consume();

        FString Name;
        if (TryGetFNameField(Line, TEXT("StateName"), Name)) Out.StateName = Name;
        int32 RootIdx;
        if (TryGetIntField(Line, TEXT("StateRootNodeIndex"), RootIdx)) Out.StateRootNodeIndex = RootIdx;
        bool bCond;
        if (TryGetBoolField(Line, TEXT("bIsAConduit"), bCond)) Out.bIsConduit = bCond;
        bool bReset;
        if (TryGetBoolField(Line, TEXT("bAlwaysResetOnEntry"), bReset)) Out.bAlwaysResetOnEntry = bReset;
        if (Line.Contains(TEXT("\"Transitions\":"))) ParseTransitionsArray(Ctx, Out.Transitions);
        if (Line.Contains(TEXT("\"PlayerNodeIndices\":")))
        {
            FString Block;
            while (!Ctx.AtEnd()) { FString BL = Ctx.Peek(); Block += BL; Ctx.Consume(); if (BL.Contains(TEXT("}"))) break; }
            Out.PlayerNodeIndices = ParseIntSet(Block);
        }
        if (Line.Contains(TEXT("\"LayerNodeIndices\":")))
        {
            FString Block;
            while (!Ctx.AtEnd()) { FString BL = Ctx.Peek(); Block += BL; Ctx.Consume(); if (BL.Contains(TEXT("}"))) break; }
            Out.LayerNodeIndices = ParseIntSet(Block);
        }
    }
    return true;
}

bool FAnimBPTextParser::ParseTransitionsArray(FParseCtx& Ctx, TArray<FAnimBPTransitionData>& Out)
{
    while (!Ctx.AtEnd()) { FString L = Ctx.Peek(); if (L.Contains(TEXT("{"))) { Ctx.Consume(); break; } if (!L.IsEmpty() && !L.StartsWith(TEXT("//"))) break; Ctx.Consume(); }
    while (!Ctx.AtEnd())
    {
        Ctx.SkipBlanks(); if (Ctx.AtEnd()) break;
        FString L = Ctx.Peek();
        if (L == TEXT("}") || L == TEXT("},") || L == TEXT("};")) { Ctx.Consume(); break; }
        if (L == TEXT("{"))
        {
            Ctx.Consume();
            FAnimBPTransitionData T;
            int32 Depth = 1;
            while (!Ctx.AtEnd() && Depth > 0)
            {
                FString TL = Ctx.Consume();
                for (TCHAR C : TL) { if (C == '{') ++Depth; else if (C == '}') --Depth; }
                if (Depth <= 0) break;
                TryGetIntField(TL, TEXT("TransitionIndex"), T.TransitionIndex);
                TryGetIntField(TL, TEXT("CanTakeDelegateIndex"), T.CanTakeDelegateIndex);
                TryGetIntField(TL, TEXT("CustomResultNodeIndex"), T.CustomResultNodeIndex);
                TryGetBoolField(TL, TEXT("bDesiredTransitionReturnValue"), T.bDesiredReturnValue);
            }
            Out.Add(T);
        }
        else Ctx.Consume();
    }
    return true;
}

bool FAnimBPTextParser::ParseMachineTransitionsArray(FParseCtx& Ctx, TArray<FAnimBPMachineTransitionData>& Out)
{
    while (!Ctx.AtEnd()) { FString L = Ctx.Peek(); if (L.Contains(TEXT("{"))) { Ctx.Consume(); break; } if (!L.IsEmpty() && !L.StartsWith(TEXT("//"))) break; Ctx.Consume(); }
    while (!Ctx.AtEnd())
    {
        Ctx.SkipBlanks(); if (Ctx.AtEnd()) break;
        FString L = Ctx.Peek();
        if (L == TEXT("}") || L == TEXT("},") || L == TEXT("};")) { Ctx.Consume(); break; }
        if (L == TEXT("{"))
        {
            Ctx.Consume();
            FAnimBPMachineTransitionData T;
            int32 Depth = 1;
            while (!Ctx.AtEnd() && Depth > 0)
            {
                FString TL = Ctx.Consume();
                for (TCHAR C : TL) { if (C == '{') ++Depth; else if (C == '}') --Depth; }
                if (Depth <= 0) break;
                TryGetIntField(TL, TEXT("PreviousState"), T.PreviousState);
                TryGetIntField(TL, TEXT("NextState"), T.NextState);
                TryGetFloatField(TL, TEXT("CrossfadeDuration"), T.CrossfadeDuration);
                FString BM, LT, SN;
                if (TryGetStringField(TL, TEXT("BlendMode"), BM))  T.BlendMode = BM;
                if (TryGetStringField(TL, TEXT("LogicType"), LT))  T.LogicType = LT;
                if (TryGetFNameField (TL, TEXT("StateName"),  SN)) T.StateName  = SN;
            }
            Out.Add(T);
        }
        else Ctx.Consume();
    }
    return true;
}

bool FAnimBPTextParser::ParseLayerFunctions(FParseCtx& Ctx, TArray<FAnimBPLayerFunctionData>& Out)
{
    return true;
}