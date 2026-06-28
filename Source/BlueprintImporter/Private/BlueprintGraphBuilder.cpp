#include "BlueprintGraphBuilder.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Self.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_AssignDelegate.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "KismetCompiler.h"
#include "UObject/UnrealType.h"
#include "Engine/Engine.h"
#include "UObject/Class.h"

// Static member definitions
int32 FBlueprintGraphBuilder::LastNodesCreated = 0;
int32 FBlueprintGraphBuilder::LastExecConnections = 0;
int32 FBlueprintGraphBuilder::LastDataConnections = 0;
int32 FBlueprintGraphBuilder::LastUnresolvedClasses = 0;

static UClass *GImportParentClass = nullptr;

template <typename TNode>
static void FinalizeNode(UEdGraph *Graph, TNode *Node, const FVector2D &Pos)
{
    if (!Graph || !Node)
        return;

    Node->NodePosX = (int32)Pos.X;
    Node->NodePosY = (int32)Pos.Y;

    Graph->AddNode(Node, /*bFromUI=*/false, /*bSelectNewNode=*/false);

    Node->CreateNewGuid();
    Node->PostPlacedNewNode();
    Node->AllocateDefaultPins();

    Node->bHasCompilerMessage = false;
    Node->ErrorType = EMessageSeverity::Info;
    Node->ErrorMsg.Empty();
}

FString FBlueprintGraphBuilder::ExtractShortName(const FString &ObjectName)
{
    int32 ColonIdx;
    if (ObjectName.FindLastChar(':', ColonIdx))
    {
        FString After = ObjectName.Mid(ColonIdx + 1);
        After.RemoveFromEnd(TEXT("'"));
        return After;
    }
    FString Result = ObjectName;
    int32 QuoteIdx;
    if (Result.FindChar('\'', QuoteIdx))
        Result = Result.Mid(QuoteIdx + 1);
    Result.RemoveFromEnd(TEXT("'"));
    return Result;
}

FString FBlueprintGraphBuilder::ExtractClassName(const FString &ObjectName)
{
    int32 QuoteOpen = -1;
    for (int32 i = 0; i < ObjectName.Len(); ++i)
    {
        if (ObjectName[i] == '\'')
        {
            QuoteOpen = i;
            break;
        }
    }

    if (QuoteOpen < 0)
        return FString();

    int32 Colon = -1;
    ObjectName.FindLastChar(':', Colon);

    if (Colon > QuoteOpen)
        return ObjectName.Mid(QuoteOpen + 1, Colon - QuoteOpen - 1);

    FString Result = ObjectName.Mid(QuoteOpen + 1);
    Result.RemoveFromEnd(TEXT("'"));
    return Result;
}

UClass *FBlueprintGraphBuilder::ResolveClass(const FString &ObjectName, const FString &ObjectPath)
{
    FString ClassName = ExtractClassName(ObjectName);
    if (ClassName.IsEmpty())
        return nullptr;

    ClassName.RemoveFromStart(TEXT("Default__"));

    if (UClass *Found = FindObject<UClass>(ANY_PACKAGE, *ClassName))
        return Found;

    auto FindInScriptPackage = [](const FString& PkgPath, const FString& Cls) -> UClass*
        {
            FString FullPath = PkgPath + TEXT(".") + Cls;
            if (UClass* C = FindObject<UClass>(nullptr, *FullPath))
                return C;
            if (UClass* C = FindObject<UClass>(ANY_PACKAGE, *Cls))
                return C;
            if (UClass* C = LoadObject<UClass>(nullptr, *FullPath))
                return C;
            return nullptr;
        };

    if (!ObjectPath.IsEmpty())
    {
        if (UClass *C = FindInScriptPackage(ObjectPath, ClassName))
            return C;
    }

    static const TCHAR* CommonPackages[] = {
        TEXT("/Script/FortniteGame"),
        TEXT("/Script/Engine"),
        TEXT("/Script/CoreUObject"),
        TEXT("/Script/UMG"),
        TEXT("/Script/InputCore"),
        TEXT("/Script/GameplayAbilities"),
        TEXT("/Script/GameplayTags"),
        TEXT("/Script/Niagara"),
        TEXT("/Script/AnimGraphRuntime"),
        TEXT("/Script/MovieScene"),
        TEXT("/Script/Foliage"),
    };
    for (const TCHAR *Pkg : CommonPackages)
    {
        if (UClass *C = FindInScriptPackage(FString(Pkg), ClassName))
            return C;
    }

    UE_LOG(LogTemp, Warning, TEXT("BlueprintImporter: Could not resolve class '%s' (path: '%s')"), *ClassName, *ObjectPath);
    LastUnresolvedClasses++;
    return nullptr;
}

static FProperty* FindPropertyRecursive(UClass* Class, const FName& Name)
{
    while (Class)
    {
        if (FProperty* Prop = Class->FindPropertyByName(Name))
        {
            return Prop;
        }
        Class = Class->GetSuperClass();
    }
    return nullptr;
}

static UFunction* FindFunctionRecursive(UClass* Class, const FName& Name)
{
    while (Class)
    {
        if (UFunction* Func = Class->FindFunctionByName(Name))
        {
            return Func;
        }
        Class = Class->GetSuperClass();
    }
    return nullptr;
}

UFunction* FBlueprintGraphBuilder::TryFindFunction(UClass* Class, const FString& FuncName)
{
    if (!Class)
    {
        return nullptr;
    }

    const FName FuncFName(*FuncName);

    if (UFunction* Func = FindFunctionRecursive(Class, FuncFName))
    {
        return Func;
    }

    for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
    {
        if (It->GetName().Equals(FuncName, ESearchCase::IgnoreCase))
        {
            return *It;
        }
    }

    return nullptr;
}

static bool IsPureUbergraphThunk(const FBPFunctionData &F)
{
    bool bFoundThunk = false;
    for (const FBPInstructionPtr &I : F.Instructions)
    {
        if (!I.IsValid())
            continue;
        const FString &N = I->Inst;

        if (N == TEXT("EX_Return") || N == TEXT("EX_EndOfScript") ||
            N == TEXT("EX_LetValueOnPersistentFrame"))
        {
            continue;
        }

        if (N == TEXT("EX_FinalFunction") || N == TEXT("EX_LocalFinalFunction") ||
            N == TEXT("EX_VirtualFunction") || N == TEXT("EX_LocalVirtualFunction"))
        {
            if (I->Function.ObjectName.Contains(TEXT("ExecuteUbergraph")))
            {
                bFoundThunk = true;
                continue;
            }
        }
        return false;
    }
    return bFoundThunk;
}

bool FBlueprintGraphBuilder::IsConstructionScript(const FBPFunctionData &F)
{
    return F.Name == TEXT("UserConstructionScript");
}

static bool IsDelegateSignature(const FBPFunctionData &F)
{
    return F.FunctionFlags.Contains(TEXT("FUNC_Delegate")) ||
           F.Name.EndsWith(TEXT("__DelegateSignature"));
}

bool FBlueprintGraphBuilder::IsOverrideEventWithBody(const FBPFunctionData &F)
{
    if (F.Name.StartsWith(TEXT("ExecuteUbergraph_")))
        return false;
    if (IsConstructionScript(F))
        return false;
    if (IsDelegateSignature(F))
        return false;

    const bool bHasEventFlag = F.FunctionFlags.Contains(TEXT("FUNC_Event"));
    if (!bHasEventFlag)
        return false;
    if (IsPureUbergraphThunk(F))
        return false;
    if (F.Instructions.Num() == 0)
        return false;

    return !F.SuperStruct.IsEmpty();
}

bool FBlueprintGraphBuilder::IsEventStubFunction(const FBPFunctionData &F)
{
    if (F.Name.StartsWith(TEXT("ExecuteUbergraph_")))
        return false;
    if (IsConstructionScript(F))
        return false;
    if (IsDelegateSignature(F))
        return false;

    const bool bHasEventFlag = F.FunctionFlags.Contains(TEXT("FUNC_Event"));

    if (bHasEventFlag && IsPureUbergraphThunk(F))
        return true;

    if (bHasEventFlag && F.Instructions.Num() == 0)
        return true;

    if (bHasEventFlag)
    {
        bool bAllBookkeeping = true;
        for (const FBPInstructionPtr &I : F.Instructions)
        {
            if (!I.IsValid()) continue;
            const FString &N = I->Inst;
            if (N != TEXT("EX_Return") && N != TEXT("EX_EndOfScript"))
            {
                bAllBookkeeping = false;
                break;
            }
        }
        if (bAllBookkeeping)
            return true;
    }

    if (IsPureUbergraphThunk(F) && F.Instructions.Num() > 0)
        return true;

    return false;
}

static FString RemapFunctionNameForEngine(const FString &InName)
{
    static const TMap<FString, FString> Remap = {
        {TEXT("Conv_DoubleToInt"), TEXT("FTrunc")},
        {TEXT("Conv_IntToDouble"), TEXT("Conv_IntToFloat")},
        {TEXT("Conv_BoolToDouble"), TEXT("Conv_BoolToFloat")},
        {TEXT("Conv_ByteToDouble"), TEXT("Conv_ByteToFloat")},
        {TEXT("Conv_DoubleToString"), TEXT("Conv_FloatToString")},
        {TEXT("Conv_DoubleToText"), TEXT("Conv_FloatToText")},
        {TEXT("Conv_DoubleToVector"), TEXT("Conv_FloatToVector")},
        {TEXT("Array_IsEmpty"), TEXT("Array_Length")},
        {TEXT("Subtract_DoubleDouble"), TEXT("Subtract_FloatFloat")},
        {TEXT("Multiply_DoubleDouble"), TEXT("Multiply_FloatFloat")},
        {TEXT("Divide_DoubleDouble"), TEXT("Divide_FloatFloat")},
        {TEXT("Less_DoubleDouble"), TEXT("Less_FloatFloat")},
        {TEXT("Greater_DoubleDouble"), TEXT("Greater_FloatFloat")},
        {TEXT("LessEqual_DoubleDouble"), TEXT("LessEqual_FloatFloat")},
        {TEXT("GreaterEqual_DoubleDouble"), TEXT("GreaterEqual_FloatFloat")},
        {TEXT("EqualEqual_DoubleDouble"), TEXT("EqualEqual_FloatFloat")},
        {TEXT("NotEqual_DoubleDouble"), TEXT("NotEqual_FloatFloat")},
        {TEXT("K2_SetTimerForNextTickDelegate"), TEXT("K2_SetTimerForNextTick")},
        {TEXT("GetValueAtLevelWithContext"), TEXT("GetValueAtLevel")},
    };
    if (const FString *Found = Remap.Find(InName))
        return *Found;
    return InName;
}

static int32 GetUbergraphEntryOffset(const FBPFunctionData &F)
{
    for (const FBPInstructionPtr &I : F.Instructions)
    {
        if (!I.IsValid())
            continue;
        if (!I->Function.ObjectName.Contains(TEXT("ExecuteUbergraph")))
            continue;
        for (const FBPInstructionPtr &P : I->Parameters)
        {
            if (P.IsValid() && P->Inst == TEXT("EX_IntConst"))
                return P->IntValue;
        }
    }
    return INDEX_NONE;
}

UEdGraphPin *FBlueprintGraphBuilder::FindExecPin(UEdGraphNode *Node, EEdGraphPinDirection Direction, const FString &PinName)
{
    if (!Node)
        return nullptr;
    for (UEdGraphPin *Pin : Node->Pins)
    {
        if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
            continue;
        if (Pin->Direction != Direction)
            continue;
        if (!PinName.IsEmpty() && Pin->PinName.ToString() != PinName)
            continue;
        return Pin;
    }
    return nullptr;
}

UEdGraphPin *FBlueprintGraphBuilder::FindDataPin(UEdGraphNode *Node, EEdGraphPinDirection Direction, const FString &PinName)
{
    if (!Node)
        return nullptr;
    for (UEdGraphPin *Pin : Node->Pins)
    {
        if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            continue;
        if (Pin->Direction != Direction)
            continue;
        if (!PinName.IsEmpty() && Pin->PinName.ToString() != PinName)
            continue;
        return Pin;
    }
    return nullptr;
}

FString FBlueprintGraphBuilder::GetVariableNameFromInstruction(const FBPInstructionPtr &Inst)
{
    if (!Inst.IsValid())
        return FString();

    if (!Inst->Variable.Name.IsEmpty())
        return Inst->Variable.Name;
    if (Inst->Variable.Path.Num() > 0)
        return Inst->Variable.Path.Last();

    if (Inst->VariableExpression.IsValid())
    {
        if (!Inst->VariableExpression->Variable.Name.IsEmpty())
            return Inst->VariableExpression->Variable.Name;
        if (Inst->VariableExpression->Variable.Path.Num() > 0)
            return Inst->VariableExpression->Variable.Path.Last();
    }

    return FString();
}

UK2Node_CallFunction *FBlueprintGraphBuilder::MakeCallNode(
    UEdGraph *Graph,
    const FBPObjectRef &FuncRef,
    const FBPInstructionPtr &Inst,
    const FVector2D &Pos,
    UClass *ContextClass)
{
    const FString RawFuncName = ExtractShortName(FuncRef.ObjectName);
    FString ClassName = ExtractClassName(FuncRef.ObjectName);

    UClass *OwnerClass = ResolveClass(FuncRef.ObjectName, FuncRef.ObjectPath);

    auto FindOnClass = [&](UClass *C) -> UFunction *
    {
        if (!C)
            return nullptr;
        if (UFunction *F = TryFindFunction(C, RawFuncName))
            return F;
        const FString Remapped = RemapFunctionNameForEngine(RawFuncName);
        if (Remapped != RawFuncName)
        {
            if (UFunction *F = TryFindFunction(C, Remapped))
                return F;
        }
        return nullptr;
    };

    UFunction *Function = FindOnClass(OwnerClass);

    if (!Function && GImportParentClass)
    {
        if (UFunction *F = FindOnClass(GImportParentClass))
        {
            Function = F;
            OwnerClass = GImportParentClass;
        }
    }

    if (!Function && ContextClass)
    {
        if (UFunction *F = FindOnClass(ContextClass))
        {
            Function = F;
            OwnerClass = ContextClass;
        }
    }

    FString FuncName = RawFuncName;
    if (!Function)
    {
        const FString Remapped = RemapFunctionNameForEngine(RawFuncName);
        if (Remapped != RawFuncName)
            FuncName = Remapped;
    }
    else
    {
        FuncName = Function->GetName();
    }

    UK2Node_CallFunction *Node = NewObject<UK2Node_CallFunction>(Graph);

    if (Function)
    {
        const bool bClassWasNamed = !ClassName.IsEmpty();
        const bool bResolvedViaParentChain = (OwnerClass == GImportParentClass);
        const bool bSelfBind = !bClassWasNamed && bResolvedViaParentChain && GImportParentClass;

        if (bSelfBind)
            Node->FunctionReference.SetSelfMember(Function->GetFName());
        else
            Node->SetFromFunction(Function);
    }
    else if (OwnerClass)
    {
        Node->FunctionReference.SetExternalMember(*FuncName, OwnerClass);
    }
    else
    {
        bool bResolved = false;
        if (FuncRef.ObjectPath.StartsWith(TEXT("/Game/")))
        {
            FString PackagePath = FuncRef.ObjectPath;
            int32 DotIdx;
            if (PackagePath.FindLastChar('.', DotIdx))
                PackagePath = PackagePath.Left(DotIdx);

            UBlueprint *TargetBP = LoadObject<UBlueprint>(nullptr, *PackagePath);
            if (!TargetBP)
            {
                FString AssetName = FPaths::GetBaseFilename(PackagePath);
                FString FullPath = PackagePath + TEXT(".") + AssetName;
                TargetBP = LoadObject<UBlueprint>(nullptr, *FullPath);
            }

            if (TargetBP && TargetBP->GeneratedClass)
            {
                UFunction *TargetFunc = TargetBP->GeneratedClass->FindFunctionByName(*FuncName);
                if (TargetFunc)
                {
                    Node->SetFromFunction(TargetFunc);
                    bResolved = true;
                }
                else
                {
                    Node->FunctionReference.SetExternalMember(*FuncName, TargetBP->GeneratedClass);
                    bResolved = true;
                }
            }
        }

        if (!bResolved)
        {
            FString SanitizedFuncName = FuncName;
            SanitizedFuncName.ReplaceInline(TEXT(" "), TEXT("_"));
            SanitizedFuncName.ReplaceInline(TEXT("-"), TEXT("_"));
            Node->FunctionReference.SetSelfMember(*SanitizedFuncName);
            if (!ClassName.IsEmpty() && !FuncRef.ObjectPath.StartsWith(TEXT("/Game/")))
            {
                Node->NodeComment = FString::Printf(TEXT("Original class: %s"), *ClassName);
                Node->bCommentBubbleVisible = true;
            }
        }
    }

    FinalizeNode(Graph, Node, Pos);
    return Node;
}

UEdGraphNode *FBlueprintGraphBuilder::MakeNodeForInstruction(
    UBlueprint *Blueprint,
    UEdGraph *Graph,
    const FBPInstructionPtr &Inst,
    const FVector2D &Pos)
{
    if (!Inst.IsValid())
        return nullptr;

    const FString &N = Inst->Inst;

    if (N == TEXT("EX_CallMath") ||
        N == TEXT("EX_FinalFunction") ||
        N == TEXT("EX_LocalFinalFunction") ||
        N == TEXT("EX_VirtualFunction") ||
        N == TEXT("EX_LocalVirtualFunction"))
    {
        if (Inst->Function.ObjectName.Contains(TEXT("ExecuteUbergraph")))
            return nullptr;
        return MakeCallNode(Graph, Inst->Function, Inst, Pos);
    }

    if (N == TEXT("EX_Context") || N == TEXT("EX_Context_FailSilent"))
    {
        if (!Inst->ContextExpression.IsValid())
            return nullptr;

        UClass *CtxClass = nullptr;
        if (Inst->ObjectExpression.IsValid())
        {
            const FBPPropertyRef &VarRef = Inst->ObjectExpression->Variable;
            if (!VarRef.PropertyClass.IsEmpty())
                CtxClass = ResolveClass(VarRef.PropertyClass, TEXT(""));
        }

        const FString &CE = Inst->ContextExpression->Inst;
        if (CE == TEXT("EX_VirtualFunction") || CE == TEXT("EX_FinalFunction") ||
            CE == TEXT("EX_LocalFinalFunction") || CE == TEXT("EX_LocalVirtualFunction") ||
            CE == TEXT("EX_CallMath"))
        {
            return MakeCallNode(Graph, Inst->ContextExpression->Function,
                                Inst->ContextExpression, Pos, CtxClass);
        }

        return MakeNodeForInstruction(Blueprint, Graph, Inst->ContextExpression, Pos);
    }

    if (N == TEXT("EX_DynamicCast") ||
        N == TEXT("EX_ObjToInterfaceCast") ||
        N == TEXT("EX_Cast"))
    {
        UClass *CastClass = ResolveClass(Inst->InterfaceClass.ObjectName, Inst->InterfaceClass.ObjectPath);
        UK2Node_DynamicCast *CastNode = NewObject<UK2Node_DynamicCast>(Graph);
        CastNode->TargetType = CastClass ? CastClass : UObject::StaticClass();
        FinalizeNode(Graph, CastNode, Pos);
        return CastNode;
    }

    if (N == TEXT("EX_InstanceVariable") ||
        N == TEXT("EX_LocalVariable") ||
        N == TEXT("EX_LocalOutVariable"))
    {
        FString VarName = GetVariableNameFromInstruction(Inst);
        if (VarName.IsEmpty())
            return nullptr;

        UK2Node_VariableGet *GetNode = NewObject<UK2Node_VariableGet>(Graph);
        GetNode->VariableReference.SetSelfMember(*VarName);
        FinalizeNode(Graph, GetNode, Pos);
        return GetNode;
    }

    if (N == TEXT("EX_Self"))
    {
        UK2Node_Self *SelfNode = NewObject<UK2Node_Self>(Graph);
        FinalizeNode(Graph, SelfNode, Pos);
        return SelfNode;
    }

    if (N == TEXT("EX_Let") ||
        N == TEXT("EX_LetBool") ||
        N == TEXT("EX_LetObj") ||
        N == TEXT("EX_LetValueOnPersistentFrame"))
    {
        if (Inst->Expression.IsValid())
        {
            const FString &RhsType = Inst->Expression->Inst;

            if (RhsType == TEXT("EX_LocalVariable") ||
                RhsType == TEXT("EX_InstanceVariable") ||
                RhsType == TEXT("EX_LocalOutVariable") ||
                RhsType == TEXT("EX_Cast") ||
                RhsType == TEXT("EX_PrimitiveCast") ||
                RhsType == TEXT("EX_InterfaceCast"))
            {
                bool bIsRealVar = false;
                const FBPInstructionPtr &LhsExpr = Inst->VariableExpression;
                if (LhsExpr.IsValid() && !LhsExpr->Variable.Name.IsEmpty())
                {
                    const FString &LhsName = LhsExpr->Variable.Name;
                    bIsRealVar = !LhsName.StartsWith(TEXT("CallFunc_")) &&
                                 !LhsName.StartsWith(TEXT("K2Node_")) &&
                                 !LhsName.StartsWith(TEXT("Temp_")) &&
                                 !LhsName.EndsWith(TEXT("_ImplicitCast")) &&
                                 !LhsName.EndsWith(TEXT("_ReturnValue"));
                }
                else
                {
                    const FString &LhsName = Inst->Variable.Name;
                    bIsRealVar = !LhsName.IsEmpty() &&
                                 !LhsName.StartsWith(TEXT("CallFunc_")) &&
                                 !LhsName.StartsWith(TEXT("K2Node_")) &&
                                 !LhsName.StartsWith(TEXT("Temp_")) &&
                                 !LhsName.EndsWith(TEXT("_ImplicitCast")) &&
                                 !LhsName.EndsWith(TEXT("_ReturnValue"));
                }

                if (!bIsRealVar)
                    return nullptr;
            }

            if (RhsType == TEXT("EX_CallMath") ||
                RhsType == TEXT("EX_FinalFunction") ||
                RhsType == TEXT("EX_LocalFinalFunction") ||
                RhsType == TEXT("EX_VirtualFunction") ||
                RhsType == TEXT("EX_LocalVirtualFunction") ||
                RhsType == TEXT("EX_Context") ||
                RhsType == TEXT("EX_Context_FailSilent") ||
                RhsType == TEXT("EX_DynamicCast") ||
                RhsType == TEXT("EX_ObjToInterfaceCast"))
            {
                return MakeNodeForInstruction(Blueprint, Graph, Inst->Expression, Pos);
            }
        }

        FString VarName = GetVariableNameFromInstruction(Inst);
        if (VarName.IsEmpty() && Inst->VariableExpression.IsValid())
            VarName = GetVariableNameFromInstruction(Inst->VariableExpression);
        if (VarName.IsEmpty())
            return nullptr;

        if (VarName.StartsWith(TEXT("Temp_")) ||
            VarName.StartsWith(TEXT("CallFunc_")) ||
            VarName.StartsWith(TEXT("K2Node_")))
        {
            return nullptr;
        }

        {
            const FBPInstructionPtr &LhsExpr = Inst->VariableExpression;
            const FString &Flags = LhsExpr.IsValid() ? LhsExpr->Variable.PropertyFlags : Inst->Variable.PropertyFlags;
            if (Flags.Contains(TEXT("OutParm")) && Flags.Contains(TEXT("Parm")))
                return nullptr;
        }

        UK2Node_VariableSet *SetNode = NewObject<UK2Node_VariableSet>(Graph);
        SetNode->VariableReference.SetSelfMember(*VarName);
        FinalizeNode(Graph, SetNode, Pos);
        return SetNode;
    }

    if (N == TEXT("EX_JumpIfNot"))
    {
        UK2Node_IfThenElse *BranchNode = NewObject<UK2Node_IfThenElse>(Graph);
        FinalizeNode(Graph, BranchNode, Pos);
        return BranchNode;
    }

    if (N == TEXT("EX_PushExecutionFlow"))
    {
        UK2Node_ExecutionSequence *SeqNode = NewObject<UK2Node_ExecutionSequence>(Graph);
        FinalizeNode(Graph, SeqNode, Pos);
        return SeqNode;
    }

    if (N == TEXT("EX_StructMemberContext"))
    {
        if (Inst->StructExpression.IsValid())
            return MakeNodeForInstruction(Blueprint, Graph, Inst->StructExpression, Pos);
        return nullptr;
    }

    // ---- Delegate: Create (bind single delegate) ----
    // FIX 2: Added empty FuncName check to prevent compiler crash.
    if (N == TEXT("EX_BindDelegate"))
    {
        const FString FuncName = ExtractShortName(Inst->Function.ObjectName);
        if (FuncName.IsEmpty())
        {
            return nullptr;
        }

        UK2Node_CreateDelegate *Node = NewObject<UK2Node_CreateDelegate>(Graph);
        Node->SelectedFunctionName = *FuncName;
        FinalizeNode(Graph, Node, Pos);
        return Node;
    }

    auto ExtractDelegateRef = [&](const FBPInstructionPtr &Expr, FString &OutName, UClass *&OutClass)
    {
        OutName.Empty();
        OutClass = nullptr;
        if (!Expr.IsValid()) return;

        const FBPInstructionPtr &Inner = (Expr->Inst == TEXT("EX_Context") && Expr->ContextExpression.IsValid())
            ? Expr->ContextExpression : Expr;

        if (Inner->Variable.Path.Num() > 0)
            OutName = Inner->Variable.Path.Last();
        if (OutName.IsEmpty())
            OutName = Inner->Variable.Name;

        const FBPObjectRef &RVP = Expr->Inst == TEXT("EX_Context")
            ? Expr->RValuePointer : Inner->RValuePointer;
        if (!RVP.ObjectName.IsEmpty())
            OutClass = ResolveClass(RVP.ObjectName, RVP.ObjectPath);
    };

    if (N == TEXT("EX_AddMulticastDelegate"))
    {
        FString DelVarName;
        UClass *DelClass = nullptr;
        ExtractDelegateRef(Inst->ObjectExpression, DelVarName, DelClass);

        UK2Node_AddDelegate *Node = NewObject<UK2Node_AddDelegate>(Graph);
        if (!DelVarName.IsEmpty())
        {
            if (DelClass && DelClass != Blueprint->GeneratedClass)
                Node->DelegateReference.SetExternalMember(*DelVarName, DelClass);
            else
                Node->DelegateReference.SetSelfMember(*DelVarName);
        }
        FinalizeNode(Graph, Node, Pos);
        return Node;
    }

    if (N == TEXT("EX_RemoveMulticastDelegate"))
    {
        FString DelVarName;
        UClass *DelClass = nullptr;
        ExtractDelegateRef(Inst->ObjectExpression, DelVarName, DelClass);

        UK2Node_RemoveDelegate *Node = NewObject<UK2Node_RemoveDelegate>(Graph);
        if (!DelVarName.IsEmpty())
        {
            if (DelClass && DelClass != Blueprint->GeneratedClass)
                Node->DelegateReference.SetExternalMember(*DelVarName, DelClass);
            else
                Node->DelegateReference.SetSelfMember(*DelVarName);
        }
        FinalizeNode(Graph, Node, Pos);
        return Node;
    }

    if (N == TEXT("EX_CallMulticastDelegate"))
    {
        FString DelVarName;
        UClass *DelClass = nullptr;
        ExtractDelegateRef(Inst->Target, DelVarName, DelClass);

        UK2Node_CallDelegate *Node = NewObject<UK2Node_CallDelegate>(Graph);
        if (!DelVarName.IsEmpty())
        {
            if (DelClass && DelClass != Blueprint->GeneratedClass)
                Node->DelegateReference.SetExternalMember(*DelVarName, DelClass);
            else
                Node->DelegateReference.SetSelfMember(*DelVarName);
        }
        FinalizeNode(Graph, Node, Pos);
        return Node;
    }

    if (N == TEXT("EX_SetArray"))
    {
        FString ArrVarName;
        if (Inst->AssigningProperty.IsValid())
            ArrVarName = GetVariableNameFromInstruction(Inst->AssigningProperty);

        if (Inst->Elements.Num() == 0)
        {
            FBPObjectRef ClearRef;
            ClearRef.ObjectName = TEXT("Function'KismetArrayLibrary:Array_Clear'");
            ClearRef.ObjectPath = TEXT("/Script/Engine");
            UK2Node_CallFunction *ClearNode = MakeCallNode(Graph, ClearRef, Inst, Pos);
            return ClearNode;
        }
        else
        {
            if (!ArrVarName.IsEmpty())
            {
                UK2Node_VariableSet *SetNode = NewObject<UK2Node_VariableSet>(Graph);
                SetNode->VariableReference.SetSelfMember(*ArrVarName);
                FinalizeNode(Graph, SetNode, Pos);
                return SetNode;
            }
        }
        return nullptr;
    }

    return nullptr;
}

void FBlueprintGraphBuilder::WireExecPins(FGraphContext &Ctx, const TArray<FBPInstructionPtr> &Instructions)
{
    TArray<int32> ExecStmts;
    for (auto &Inst : Instructions)
    {
        if (!Inst.IsValid())
            continue;
        if (!Ctx.StatementToNode.Contains(Inst->StatementIndex))
            continue;
        bool bHasExec = false;
        for (UEdGraphPin *Pin : Ctx.StatementToNode[Inst->StatementIndex]->Pins)
        {
            if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                bHasExec = true;
                break;
            }
        }
        if (bHasExec)
            ExecStmts.Add(Inst->StatementIndex);
    }

    auto GetInst = [&](int32 Stmt) -> FBPInstructionPtr
    {
        FBPInstructionPtr *P = Ctx.StatementToInstruction.Find(Stmt);
        return P ? *P : nullptr;
    };
    auto GetNode = [&](int32 Stmt) -> UEdGraphNode *
    {
        UEdGraphNode **P = Ctx.StatementToNode.Find(Stmt);
        return P ? *P : nullptr;
    };

    auto ResolveTargetNode = [&](int32 TargetStmt) -> UEdGraphNode *
    {
        if (TargetStmt < 0)
            return nullptr;
        if (UEdGraphNode *Direct = GetNode(TargetStmt))
            return Direct;
        int32 Best = MAX_int32;
        UEdGraphNode *BestNode = nullptr;
        for (const auto &Pair : Ctx.StatementToNode)
        {
            if (Pair.Key >= TargetStmt && Pair.Key < Best && Pair.Value)
            {
                Best = Pair.Key;
                BestNode = Pair.Value;
            }
        }
        return BestNode;
    };

    auto ConnectExec = [&](UEdGraphNode *From, const FString &OutPinName, UEdGraphNode *To, const FString &InPinName = TEXT(""))
    {
        if (!From || !To)
            return;
        if (From == To)
            return;
        UEdGraphPin *Out = FindExecPin(From, EGPD_Output, OutPinName);
        UEdGraphPin *In = InPinName.IsEmpty() ? FindExecPin(To, EGPD_Input) : FindExecPin(To, EGPD_Input, InPinName);
        if (Out && In && Out->LinkedTo.Num() == 0)
        {
            Out->MakeLinkTo(In);
            LastExecConnections++;
        }
    };

    for (const auto &Pair : Ctx.OffsetEntryNodes)
    {
        const int32 EntryOffset = Pair.Key;
        UEdGraphNode *EventNode = Pair.Value;
        if (!EventNode)
            continue;

        UEdGraphNode *BodyNode = ResolveTargetNode(EntryOffset);
        if (BodyNode && BodyNode != EventNode)
            ConnectExec(EventNode, TEXT(""), BodyNode);
    }

    for (int32 i = 0; i < ExecStmts.Num(); ++i)
    {
        int32 Stmt = ExecStmts[i];
        FBPInstructionPtr Inst = GetInst(Stmt);
        UEdGraphNode *Node = GetNode(Stmt);
        if (!Inst.IsValid() || !Node)
            continue;

        const FString &N = Inst->Inst;

        if (N == TEXT("EX_PushExecutionFlow"))
        {
            int32 PushCount = 0;
            while (i + PushCount < ExecStmts.Num())
            {
                FBPInstructionPtr PushInst = GetInst(ExecStmts[i + PushCount]);
                if (PushInst.IsValid() && PushInst->Inst == TEXT("EX_PushExecutionFlow"))
                    PushCount++;
                else
                    break;
            }

            UK2Node_ExecutionSequence *SeqNode = Cast<UK2Node_ExecutionSequence>(Node);
            if (!SeqNode)
            {
                if (i + 1 < ExecStmts.Num())
                    ConnectExec(Node, TEXT(""), GetNode(ExecStmts[i + 1]));
                continue;
            }

            TArray<int32> PushedAddrs;
            for (int32 p = 0; p < PushCount; ++p)
            {
                FBPInstructionPtr PushInst = GetInst(ExecStmts[i + p]);
                if (PushInst.IsValid())
                    PushedAddrs.Add(PushInst->PushingAddress);
            }

            int32 TotalOutputs = PushCount + 1;
            while ((int32)SeqNode->Pins.Num() - 1 < TotalOutputs)
                SeqNode->AddInputPin();

            int32 NextIdx = i + PushCount;
            if (NextIdx < ExecStmts.Num())
                ConnectExec(SeqNode, TEXT("then_0"), GetNode(ExecStmts[NextIdx]));

            for (int32 p = 0; p < PushCount; ++p)
            {
                int32 Addr = PushedAddrs[PushCount - 1 - p];
                UEdGraphNode *TargetNode = ResolveTargetNode(Addr);
                if (!TargetNode)
                    continue;
                FString PinName = FString::Printf(TEXT("then_%d"), p + 1);
                ConnectExec(SeqNode, PinName, TargetNode);
            }

            i += PushCount - 1;
            continue;
        }

        if (N == TEXT("EX_JumpIfNot"))
        {
            if (i + 1 < ExecStmts.Num())
                ConnectExec(Node, TEXT("then"), GetNode(ExecStmts[i + 1]));

            UEdGraphNode *ElseNode = ResolveTargetNode(Inst->CodeOffset);
            ConnectExec(Node, TEXT("else"), ElseNode);
            continue;
        }

        if (N == TEXT("EX_PopExecutionFlowIfNot"))
        {
            if (i + 1 < ExecStmts.Num())
                ConnectExec(Node, TEXT("then"), GetNode(ExecStmts[i + 1]));
            continue;
        }

        if (N == TEXT("EX_PopExecutionFlow"))
            continue;

        if (N == TEXT("EX_Jump") || N == TEXT("EX_ComputedJump"))
        {
            UEdGraphNode *TargetNode = ResolveTargetNode(Inst->CodeOffset);
            ConnectExec(Node, TEXT(""), TargetNode);
            continue;
        }

        if (i + 1 < ExecStmts.Num())
        {
            UEdGraphNode *NextNode = GetNode(ExecStmts[i + 1]);
            if (NextNode && NextNode != Node)
            {
                UEdGraphPin *OutExec = nullptr;
                for (UEdGraphPin *P : Node->Pins)
                {
                    if (P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
                        P->Direction == EGPD_Output && P->LinkedTo.Num() == 0)
                    {
                        if (!OutExec || P->PinName == UEdGraphSchema_K2::PN_Then)
                            OutExec = P;
                    }
                }
                UEdGraphPin *InExec = FindExecPin(NextNode, EGPD_Input);
                if (OutExec && InExec)
                {
                    OutExec->MakeLinkTo(InExec);
                    LastExecConnections++;
                }
            }
        }
    }
}

void FBlueprintGraphBuilder::WireDataPins(FGraphContext &Ctx, const TArray<FBPInstructionPtr> &Instructions)
{
    for (auto &Inst : Instructions)
    {
        if (!Inst.IsValid())
            continue;

        UEdGraphNode **NodePtr = Ctx.StatementToNode.Find(Inst->StatementIndex);
        UEdGraphNode *Node = (NodePtr && *NodePtr) ? *NodePtr : nullptr;

        const FString &N = Inst->Inst;

        if (N == TEXT("EX_InstanceVariable") ||
            N == TEXT("EX_LocalVariable") ||
            N == TEXT("EX_LocalOutVariable"))
        {
            if (Node)
            {
                FString VarName = GetVariableNameFromInstruction(Inst);
                if (!VarName.IsEmpty())
                {
                    UEdGraphPin *OutPin = FindDataPin(Node, EGPD_Output, VarName);
                    if (!OutPin)
                        OutPin = FindDataPin(Node, EGPD_Output, TEXT(""));
                    if (OutPin)
                        Ctx.VariableToOutputPin.Add(VarName, OutPin);
                }
            }
        }

        if (N == TEXT("EX_Let") || N == TEXT("EX_LetBool") || N == TEXT("EX_LetObj") ||
            N == TEXT("EX_LetValueOnPersistentFrame"))
        {
            FString VarName = GetVariableNameFromInstruction(Inst);
            if (VarName.IsEmpty() && Inst->VariableExpression.IsValid())
                VarName = GetVariableNameFromInstruction(Inst->VariableExpression);

            if (Inst->Expression.IsValid())
            {
                const FString &RhsType = Inst->Expression->Inst;

                if (RhsType == TEXT("EX_LocalVariable") ||
                    RhsType == TEXT("EX_InstanceVariable") ||
                    RhsType == TEXT("EX_LocalOutVariable") ||
                    RhsType == TEXT("EX_Cast") ||
                    RhsType == TEXT("EX_PrimitiveCast") ||
                    RhsType == TEXT("EX_InterfaceCast"))
                {
                    FString SrcVarName = GetVariableNameFromInstruction(Inst->Expression);
                    if (!SrcVarName.IsEmpty() && !VarName.IsEmpty())
                    {
                        UEdGraphPin **SrcPin = Ctx.VariableToOutputPin.Find(SrcVarName);
                        if (SrcPin && *SrcPin)
                            Ctx.VariableToOutputPin.Add(VarName, *SrcPin);
                    }
                    if (!Node)
                        continue;
                }

                if (Node)
                {
                    if (!VarName.IsEmpty())
                    {
                        UEdGraphPin *OutPin = FindDataPin(Node, EGPD_Output, TEXT("ReturnValue"));
                        if (!OutPin)
                            OutPin = FindDataPin(Node, EGPD_Output, TEXT(""));
                        if (OutPin)
                            Ctx.VariableToOutputPin.Add(VarName, OutPin);
                    }
                }
            }

            UK2Node_VariableSet *SetNode = Cast<UK2Node_VariableSet>(Node);
            if (SetNode && Inst->Expression.IsValid())
            {
                FString RhsVarName = GetVariableNameFromInstruction(Inst->Expression);
                if (!RhsVarName.IsEmpty())
                {
                    UEdGraphPin **SrcPin = Ctx.VariableToOutputPin.Find(RhsVarName);
                    UEdGraphPin *ValueIn = FindDataPin(SetNode, EGPD_Input, VarName);
                    if (!ValueIn)
                        ValueIn = FindDataPin(SetNode, EGPD_Input, TEXT(""));
                    if (SrcPin && *SrcPin && ValueIn)
                    {
                        (*SrcPin)->MakeLinkTo(ValueIn);
                        LastDataConnections++;
                    }
                }
                else
                {
                    UEdGraphPin *ValueIn = FindDataPin(SetNode, EGPD_Input, VarName);
                    if (!ValueIn)
                        ValueIn = FindDataPin(SetNode, EGPD_Input, TEXT(""));
                    if (ValueIn)
                        ApplyLiteralToPin(ValueIn, Inst->Expression);
                }
            }
        }

        if (Node)
        {
            FBPInstructionPtr CtxInst = nullptr;
            FBPInstructionPtr *StoredInstPtr = Ctx.StatementToInstruction.Find(Inst->StatementIndex);
            if (StoredInstPtr && (*StoredInstPtr).IsValid())
            {
                const FString &ST = (*StoredInstPtr)->Inst;
                if (ST == TEXT("EX_Context") || ST == TEXT("EX_Context_FailSilent"))
                    CtxInst = *StoredInstPtr;
                else if (ST == TEXT("EX_Let") || ST == TEXT("EX_LetBool") || ST == TEXT("EX_LetObj") ||
                         ST == TEXT("EX_LetValueOnPersistentFrame"))
                {
                    const FBPInstructionPtr &Expr = (*StoredInstPtr)->Expression;
                    if (Expr.IsValid() && (Expr->Inst == TEXT("EX_Context") || Expr->Inst == TEXT("EX_Context_FailSilent")))
                        CtxInst = Expr;
                }
            }
            if (!CtxInst.IsValid() && (N == TEXT("EX_Context") || N == TEXT("EX_Context_FailSilent")))
                CtxInst = Inst;

            if (CtxInst.IsValid() && CtxInst->ObjectExpression.IsValid())
            {
                UK2Node_CallFunction *CallNode = Cast<UK2Node_CallFunction>(Node);
                if (CallNode)
                {
                    FString ObjVarName = GetVariableNameFromInstruction(CtxInst->ObjectExpression);
                    if (!ObjVarName.IsEmpty())
                    {
                        if (!Ctx.VariableToOutputPin.Contains(ObjVarName))
                        {
                            UK2Node_VariableGet *GetNode = NewObject<UK2Node_VariableGet>(Ctx.Graph);
                            GetNode->VariableReference.SetSelfMember(*ObjVarName);
                            FVector2D GetPos((float)(Node->NodePosX - 320), (float)(Node->NodePosY + 80));
                            FinalizeNode(Ctx.Graph, GetNode, GetPos);
                            UEdGraphPin *OutPin = FindDataPin(GetNode, EGPD_Output, TEXT(""));
                            if (OutPin)
                                Ctx.VariableToOutputPin.Add(ObjVarName, OutPin);
                            LastNodesCreated++;
                        }

                        UEdGraphPin **SrcPin = Ctx.VariableToOutputPin.Find(ObjVarName);
                        UEdGraphPin *TargetPin = FindDataPin(CallNode, EGPD_Input, TEXT("self"));
                        if (!TargetPin)
                            TargetPin = FindDataPin(CallNode, EGPD_Input, TEXT("Target"));
                        if (!TargetPin)
                            TargetPin = FindDataPin(CallNode, EGPD_Input, TEXT(""));
                        if (SrcPin && *SrcPin && TargetPin && TargetPin->LinkedTo.Num() == 0)
                        {
                            (*SrcPin)->MakeLinkTo(TargetPin);
                            LastDataConnections++;
                        }
                    }
                }
            }
        }

        auto WireCallNodeParams = [&](UK2Node_CallFunction *CallNode, const TArray<FBPInstructionPtr> &Params)
        {
            if (!CallNode) return;
            TArray<UEdGraphPin *> InputDataPins;
            for (UEdGraphPin *Pin : CallNode->Pins)
            {
                if (Pin->Direction == EGPD_Input &&
                    Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec &&
                    Pin->PinName != UEdGraphSchema_K2::PN_Self)
                {
                    InputDataPins.Add(Pin);
                }
            }
            for (int32 p = 0; p < Params.Num() && p < InputDataPins.Num(); ++p)
            {
                const FBPInstructionPtr &Param = Params[p];
                if (!Param.IsValid()) continue;
                if (Param->Inst == TEXT("EX_Self")) continue;
                FString ParamVarName = GetVariableNameFromInstruction(Param);
                if (!ParamVarName.IsEmpty())
                {
                    UEdGraphPin **SrcPin = Ctx.VariableToOutputPin.Find(ParamVarName);
                    if (SrcPin && *SrcPin)
                    {
                        (*SrcPin)->MakeLinkTo(InputDataPins[p]);
                        LastDataConnections++;
                    }
                }
                else
                {
                    ApplyLiteralToPin(InputDataPins[p], Param);
                }
            }
        };

        if (Node && (N == TEXT("EX_CallMath") ||
                     N == TEXT("EX_FinalFunction") ||
                     N == TEXT("EX_LocalFinalFunction") ||
                     N == TEXT("EX_VirtualFunction") ||
                     N == TEXT("EX_LocalVirtualFunction")))
        {
            WireCallNodeParams(Cast<UK2Node_CallFunction>(Node), Inst->Parameters);
        }

        if (Node && (N == TEXT("EX_Let") || N == TEXT("EX_LetBool") ||
                     N == TEXT("EX_LetObj") || N == TEXT("EX_LetValueOnPersistentFrame")))
        {
            if (Inst->Expression.IsValid())
            {
                const FString &RhsType = Inst->Expression->Inst;
                if (RhsType == TEXT("EX_CallMath") ||
                    RhsType == TEXT("EX_FinalFunction") ||
                    RhsType == TEXT("EX_LocalFinalFunction") ||
                    RhsType == TEXT("EX_VirtualFunction") ||
                    RhsType == TEXT("EX_LocalVirtualFunction"))
                {
                    WireCallNodeParams(Cast<UK2Node_CallFunction>(Node), Inst->Expression->Parameters);
                }
            }
        }
    }

}
// ─────────────────────────────────────────────────────────────────────────────
//  ApplyLiteralToPin
// ─────────────────────────────────────────────────────────────────────────────
void FBlueprintGraphBuilder::ApplyLiteralToPin(UEdGraphPin *Pin,
                                               const FBPInstructionPtr &Inst)
{
    if (!Pin || !Inst.IsValid())
        return;

    const FString &N = Inst->Inst;

    if (N == TEXT("EX_IntConst") || N == TEXT("EX_Int64Const") ||
        N == TEXT("EX_UInt64Const"))
    {
        Pin->DefaultValue = FString::FromInt(Inst->IntValue);
    }
    else if (N == TEXT("EX_FloatConst"))
    {
        Pin->DefaultValue = FString::SanitizeFloat(Inst->FloatValue);
    }
    else if (N == TEXT("EX_DoubleConst"))
    {
        Pin->DefaultValue = FString::SanitizeFloat((float)Inst->DoubleValue);
    }
    else if (N == TEXT("EX_True"))
    {
        Pin->DefaultValue = TEXT("true");
    }
    else if (N == TEXT("EX_False"))
    {
        Pin->DefaultValue = TEXT("false");
    }
    else if (N == TEXT("EX_ByteConst") || N == TEXT("EX_IntConstByte"))
    {
        Pin->DefaultValue = FString::FromInt((int32)Inst->ByteValue);
    }
    else if (N == TEXT("EX_NameConst"))
    {
        Pin->DefaultValue = Inst->NameValue.ToString();
    }
    else if (N == TEXT("EX_StringConst") || N == TEXT("EX_UnicodeStringConst"))
    {
        Pin->DefaultValue = Inst->StringValue;
    }
    else if (N == TEXT("EX_TextConst"))
    {
        Pin->DefaultValue = Inst->StringValue;
    }
    else if (N == TEXT("EX_ObjectConst") || N == TEXT("EX_SoftObjectConst"))
    {
        if (!Inst->ObjectValue.ObjectPath.IsEmpty())
            Pin->DefaultValue = Inst->ObjectValue.ObjectPath;
        else if (!Inst->ObjectValue.ObjectName.IsEmpty())
            Pin->DefaultValue = Inst->ObjectValue.ObjectName;
    }
    else if (N == TEXT("EX_NoObject") || N == TEXT("EX_NoInterface"))
    {
        Pin->DefaultValue = TEXT("None");
    }
    else if (N == TEXT("EX_Self"))
    {
        Pin->DefaultValue = TEXT("self");
    }
    // EX_StructConst and unknown types: leave pin default untouched
}

// ─────────────────────────────────────────────────────────────────────────────
//  MapPropertyToPinType
// ─────────────────────────────────────────────────────────────────────────────
void FBlueprintGraphBuilder::MapPropertyToPinType(const FBPVariableDesc &Desc,
                                                  FEdGraphPinType &OutPinType)
{
    // Safe default so the pin is never left in an invalid state
    OutPinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;

    const FString &T = Desc.Type;

    if (T == TEXT("BoolProperty"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    }
    else if (T == TEXT("IntProperty"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
    }
    else if (T == TEXT("Int64Property"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
    }
    else if (T == TEXT("FloatProperty") || T == TEXT("DoubleProperty"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Float;
    }
    else if (T == TEXT("NameProperty"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
    }
    else if (T == TEXT("StrProperty"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
    }
    else if (T == TEXT("TextProperty"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
    }
    else if (T == TEXT("ByteProperty") || T == TEXT("EnumProperty"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
        if (!Desc.EnumName.IsEmpty())
        {
            UEnum *Enum = FindObject<UEnum>(ANY_PACKAGE, *Desc.EnumName, true);
            if (!Enum && !Desc.EnumPath.IsEmpty())
                Enum = LoadObject<UEnum>(nullptr, *Desc.EnumPath);
            OutPinType.PinSubCategoryObject = Enum; // null is fine — pin stays Byte
        }
    }
    else if (T == TEXT("ObjectProperty") || T == TEXT("WeakObjectProperty") ||
             T == TEXT("LazyObjectProperty"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
        if (!Desc.PropertyClass.IsEmpty())
        {
            UClass *Cls = ResolveClass(Desc.PropertyClass, Desc.PropertyClassPath);
            OutPinType.PinSubCategoryObject = Cls ? (UObject *)Cls
                                                   : (UObject *)UObject::StaticClass();
        }
        else
        {
            OutPinType.PinSubCategoryObject = UObject::StaticClass();
        }
    }
    else if (T == TEXT("SoftObjectProperty"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_SoftObject;
        if (!Desc.PropertyClass.IsEmpty())
        {
            UClass *Cls = ResolveClass(Desc.PropertyClass, Desc.PropertyClassPath);
            OutPinType.PinSubCategoryObject = Cls ? (UObject *)Cls
                                                   : (UObject *)UObject::StaticClass();
        }
        else
        {
            OutPinType.PinSubCategoryObject = UObject::StaticClass();
        }
    }
    else if (T == TEXT("ClassProperty"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Class;
        if (!Desc.PropertyClass.IsEmpty())
        {
            UClass *Cls = ResolveClass(Desc.PropertyClass, Desc.PropertyClassPath);
            OutPinType.PinSubCategoryObject = Cls ? (UObject *)Cls
                                                   : (UObject *)UObject::StaticClass();
        }
        else
        {
            OutPinType.PinSubCategoryObject = UObject::StaticClass();
        }
    }
    else if (T == TEXT("SoftClassProperty"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_SoftClass;
        if (!Desc.PropertyClass.IsEmpty())
        {
            UClass *Cls = ResolveClass(Desc.PropertyClass, Desc.PropertyClassPath);
            OutPinType.PinSubCategoryObject = Cls ? (UObject *)Cls
                                                   : (UObject *)UObject::StaticClass();
        }
        else
        {
            OutPinType.PinSubCategoryObject = UObject::StaticClass();
        }
    }
    else if (T == TEXT("InterfaceProperty"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Interface;
        if (!Desc.PropertyClass.IsEmpty())
        {
            UClass *Cls = ResolveClass(Desc.PropertyClass, Desc.PropertyClassPath);
            OutPinType.PinSubCategoryObject = Cls; // null is acceptable here
        }
    }
    else if (T == TEXT("StructProperty"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
        if (!Desc.StructName.IsEmpty())
        {
            UScriptStruct *Struct = FindObject<UScriptStruct>(ANY_PACKAGE, *Desc.StructName, true);
            if (!Struct && !Desc.StructPath.IsEmpty())
                Struct = LoadObject<UScriptStruct>(nullptr, *Desc.StructPath);
            OutPinType.PinSubCategoryObject = Struct; // null will show as unresolved, not crash
        }
    }
    else if (T == TEXT("ArrayProperty"))
    {
        OutPinType.ContainerType = EPinContainerType::Array;
        if (Desc.InnerType.IsValid())
        {
            FEdGraphPinType InnerPin;
            MapPropertyToPinType(*Desc.InnerType, InnerPin);
            OutPinType.PinCategory          = InnerPin.PinCategory;
            OutPinType.PinSubCategoryObject = InnerPin.PinSubCategoryObject;
        }
        else
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
        }
    }
    else if (T == TEXT("MapProperty"))
    {
        OutPinType.ContainerType = EPinContainerType::Map;
        OutPinType.PinCategory   = UEdGraphSchema_K2::PC_Wildcard;
    }
    else if (T == TEXT("SetProperty"))
    {
        OutPinType.ContainerType = EPinContainerType::Set;
        OutPinType.PinCategory   = UEdGraphSchema_K2::PC_Wildcard;
    }
    else if (T == TEXT("DelegateProperty") || T == TEXT("MulticastDelegateProperty") ||
             T == TEXT("MulticastInlineDelegateProperty"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Delegate;
    }
    // Unknown types remain PC_Wildcard — won't crash, just shows as unresolved
}

// ─────────────────────────────────────────────────────────────────────────────
//  ImportVariables
// ─────────────────────────────────────────────────────────────────────────────
void FBlueprintGraphBuilder::ImportVariables(UBlueprint *Blueprint,
                                             const TArray<FBPVariableDesc> &Variables)
{
    if (!Blueprint || Variables.Num() == 0)
        return;

    for (const FBPVariableDesc &Desc : Variables)
    {
        if (Desc.Name.IsEmpty())
            continue;

        FEdGraphPinType PinType;
        MapPropertyToPinType(Desc, PinType);

        // Skip variables that already exist
        if (FBlueprintEditorUtils::FindMemberVariableGuidByName(Blueprint, *Desc.Name).IsValid())
            continue;

        FBlueprintEditorUtils::AddMemberVariable(Blueprint, *Desc.Name, PinType);

        // Apply flags — guard the index in case AddMemberVariable failed silently
        const int32 VarIdx = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, *Desc.Name);
        if (VarIdx != INDEX_NONE && Blueprint->NewVariables.IsValidIndex(VarIdx))
        {
            FBPVariableDescription &BPVar = Blueprint->NewVariables[VarIdx];
            if (Desc.bIsInstanceEditable)
                BPVar.PropertyFlags |= CPF_Edit;
            if (Desc.bIsReadOnly)
                BPVar.PropertyFlags |= CPF_BlueprintReadOnly;
            if (Desc.bIsAdvancedDisplay)
                BPVar.PropertyFlags |= CPF_AdvancedDisplay;
            if (Desc.bIsTransient)
                BPVar.PropertyFlags |= CPF_Transient;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  ImportLocalVariables
// ─────────────────────────────────────────────────────────────────────────────
void FBlueprintGraphBuilder::ImportLocalVariables(UBlueprint *Blueprint,
                                                  UEdGraph *FuncGraph,
                                                  const TArray<FBPVariableDesc> &LocalVars)
{
    if (!Blueprint || !FuncGraph || LocalVars.Num() == 0)
        return;

    // AddLocalVariable internally calls FBlueprintEditorUtils which asserts that the
    // graph contains at least one K2Node_FunctionEntry node (BlueprintEditorUtils.cpp:5268).
    // Event graphs and freshly-created function graphs may not have one yet, so we must
    // verify the entry node exists before attempting to add any local variables.
    bool bHasFunctionEntry = false;
    for (UEdGraphNode *Node : FuncGraph->Nodes)
    {
        if (Node && Node->IsA<UK2Node_FunctionEntry>())
        {
            bHasFunctionEntry = true;
            break;
        }
    }

    if (!bHasFunctionEntry)
    {
        UE_LOG(LogTemp, Warning,
               TEXT("BlueprintImporter: Skipping local variables for graph '%s' — no FunctionEntry node found. "
                    "Local variables can only be added to proper function graphs."),
               *FuncGraph->GetName());
        return;
    }

    for (const FBPVariableDesc &Desc : LocalVars)
    {
        if (Desc.Name.IsEmpty())
            continue;

        FEdGraphPinType PinType;
        MapPropertyToPinType(Desc, PinType);

        FBlueprintEditorUtils::AddLocalVariable(Blueprint, FuncGraph, *Desc.Name, PinType);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  MakeEventNode
// ─────────────────────────────────────────────────────────────────────────────
UEdGraphNode *FBlueprintGraphBuilder::MakeEventNode(UBlueprint *Blueprint,
                                                    UEdGraph *EventGraph,
                                                    const FBPFunctionData &EventFunc,
                                                    const FVector2D &Pos)
{
    if (!Blueprint || !EventGraph || EventFunc.Name.IsEmpty())
        return nullptr;

    const FName EventFuncName(*EventFunc.Name);

    // Return existing node if already placed (e.g. called twice for same event)
    for (UEdGraphNode *Node : EventGraph->Nodes)
    {
        if (!Node)
            continue;
        if (UK2Node_Event *EvNode = Cast<UK2Node_Event>(Node))
        {
            if (EvNode->EventReference.GetMemberName() == EventFuncName)
                return EvNode;
        }
    }

    // Try parent class override first
    UClass *ParentClass = Blueprint->ParentClass;
    if (ParentClass)
    {
        UFunction *Func = ParentClass->FindFunctionByName(EventFuncName);
        if (Func && Func->HasAllFunctionFlags(FUNC_BlueprintEvent))
        {
            UK2Node_Event *EvNode = NewObject<UK2Node_Event>(EventGraph);
            if (!EvNode)
                return nullptr;
            EvNode->EventReference.SetExternalMember(EventFuncName, ParentClass);
            EvNode->bOverrideFunction = true;
            EvNode->NodePosX = (int32)Pos.X;
            EvNode->NodePosY = (int32)Pos.Y;
            EvNode->CreateNewGuid();
            EvNode->PostPlacedNewNode();
            EvNode->AllocateDefaultPins();
            EventGraph->Nodes.Add(EvNode);
            return EvNode;
        }
    }

    // Fall back to custom event
    UK2Node_CustomEvent *CustNode =
        UK2Node_CustomEvent::CreateFromFunction(FVector2D(Pos.X, Pos.Y),
                                               EventGraph, EventFunc.Name,
                                               nullptr, false);
    return CustNode; // may be null — callers check
}

// ─────────────────────────────────────────────────────────────────────────────
//  BuildGraph
// ─────────────────────────────────────────────────────────────────────────────
void FBlueprintGraphBuilder::BuildGraph(UBlueprint *Blueprint,
                                        UEdGraph *Graph,
                                        const FBPFunctionData &FuncData,
                                        UEdGraphNode *EntryEventNode)
{
    if (!Blueprint || !Graph)
        return;

    FGraphContext Ctx;
    Ctx.Graph = Graph;

    // Build offset → statement map
    for (const FBPInstructionPtr &Inst : FuncData.Instructions)
    {
        if (Inst.IsValid() && Inst->CodeOffset >= 0 && Inst->StatementIndex >= 0)
            Ctx.CodeOffsetToStatement.Add(Inst->CodeOffset, Inst->StatementIndex);
    }

    // Pre-seed with the entry event node when building into an event graph
    if (EntryEventNode && FuncData.Instructions.Num() > 0)
    {
        for (const FBPInstructionPtr &Inst : FuncData.Instructions)
        {
            if (Inst.IsValid() && Inst->StatementIndex >= 0)
            {
                Ctx.StatementToNode.Add(Inst->StatementIndex, EntryEventNode);
                Ctx.StatementToInstruction.Add(Inst->StatementIndex, Inst);
                break;
            }
        }
    }

    // ── Pass 1: node factory ─────────────────────────────────────────────────
    const float ColWidth = 250.f;
    int32 Col = EntryEventNode ? 1 : 0;

    for (const FBPInstructionPtr &Inst : FuncData.Instructions)
    {
        if (!Inst.IsValid() || Inst->StatementIndex < 0)
            continue;
        if (Ctx.StatementToNode.Contains(Inst->StatementIndex))
            continue;

        UEdGraphNode *Node = MakeNodeForInstruction(Blueprint, Graph, Inst,
                                                    FVector2D((float)Col * ColWidth, 0.f));
        if (Node)
        {
            Ctx.StatementToNode.Add(Inst->StatementIndex, Node);
            Ctx.StatementToInstruction.Add(Inst->StatementIndex, Inst);
            LastNodesCreated++;
            ++Col;
        }
    }

    // ── Pass 2: exec wiring ──────────────────────────────────────────────────
    WireExecPins(Ctx, FuncData.Instructions);

    // ── Pass 3: data wiring ──────────────────────────────────────────────────
    WireDataPins(Ctx, FuncData.Instructions);

    // ── Local variables ──────────────────────────────────────────────────────
    if (FuncData.LocalVariables.Num() > 0)
        ImportLocalVariables(Blueprint, Graph, FuncData.LocalVariables);
}

// ─────────────────────────────────────────────────────────────────────────────
//  BuildEventGraph
// ─────────────────────────────────────────────────────────────────────────────
void FBlueprintGraphBuilder::BuildEventGraph(
    UBlueprint *Blueprint,
    UEdGraph *EventGraph,
    const FBPFunctionData &Ubergraph,
    const TArray<const FBPFunctionData *> &EventStubs)
{
    if (!Blueprint || !EventGraph)
        return;

    FGraphContext Ctx;
    Ctx.Graph = EventGraph;

    // Build offset → statement index map from the ubergraph
    for (const FBPInstructionPtr &Inst : Ubergraph.Instructions)
    {
        if (Inst.IsValid() && Inst->CodeOffset >= 0 && Inst->StatementIndex >= 0)
            Ctx.CodeOffsetToStatement.Add(Inst->CodeOffset, Inst->StatementIndex);
    }

    // Place one event entry node per stub and seed it into the context
    float EventY = 0.f;
    const float EventSpacingY = 200.f;

    for (const FBPFunctionData *StubPtr : EventStubs)
    {
        if (!StubPtr || StubPtr->Name.IsEmpty())
            continue;

        const int32 EntryOffset = GetUbergraphEntryOffset(*StubPtr);

        UEdGraphNode *EvNode = MakeEventNode(Blueprint, EventGraph, *StubPtr,
                                             FVector2D(0.f, EventY));
        EventY += EventSpacingY;

        if (!EvNode)
            continue;

        if (EntryOffset != INDEX_NONE)
        {
            if (const int32 *StmtPtr = Ctx.CodeOffsetToStatement.Find(EntryOffset))
            {
                const int32 Stmt = *StmtPtr;
                if (!Ctx.StatementToNode.Contains(Stmt))
                {
                    Ctx.StatementToNode.Add(Stmt, EvNode);
                    for (const FBPInstructionPtr &Inst : Ubergraph.Instructions)
                    {
                        if (Inst.IsValid() && Inst->StatementIndex == Stmt)
                        {
                            Ctx.StatementToInstruction.Add(Stmt, Inst);
                            break;
                        }
                    }
                }
            }
        }
    }

    // ── Pass 1 ───────────────────────────────────────────────────────────────
    const float ColWidth = 250.f;
    int32 Col = 1;

    for (const FBPInstructionPtr &Inst : Ubergraph.Instructions)
    {
        if (!Inst.IsValid() || Inst->StatementIndex < 0)
            continue;
        if (Ctx.StatementToNode.Contains(Inst->StatementIndex))
            continue;

        UEdGraphNode *Node = MakeNodeForInstruction(Blueprint, EventGraph, Inst,
                                                    FVector2D((float)Col * ColWidth, 0.f));
        if (Node)
        {
            Ctx.StatementToNode.Add(Inst->StatementIndex, Node);
            Ctx.StatementToInstruction.Add(Inst->StatementIndex, Inst);
            LastNodesCreated++;
            ++Col;
        }
    }

    // ── Pass 2 & 3 ───────────────────────────────────────────────────────────
    WireExecPins(Ctx, Ubergraph.Instructions);
    WireDataPins(Ctx, Ubergraph.Instructions);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Build  — public entry point
// ─────────────────────────────────────────────────────────────────────────────
bool FBlueprintGraphBuilder::Build(UBlueprint *Blueprint,
                                   const FBlueprintJsonData &Data,
                                   FString &OutError)
{
    if (!Blueprint)
    {
        OutError = TEXT("Blueprint asset is null.");
        return false;
    }

    LastNodesCreated      = 0;
    LastExecConnections   = 0;
    LastDataConnections   = 0;
    LastUnresolvedClasses = 0;

    // ── 1. Member variables ──────────────────────────────────────────────────
    ImportVariables(Blueprint, Data.Variables);

    // ── 2. Categorise functions ──────────────────────────────────────────────
    const FBPFunctionData *Ubergraph = nullptr;
    TArray<const FBPFunctionData *> EventStubs;
    TArray<const FBPFunctionData *> NormalFunctions;
    TArray<const FBPFunctionData *> OverrideEvents;
    TArray<const FBPFunctionData *> ConstructionScripts;

    for (const FBPFunctionData &F : Data.Functions)
    {
        if (F.Name.IsEmpty())
            continue;

        if (F.Name.StartsWith(TEXT("ExecuteUbergraph_")))
        {
            if (!Ubergraph)   // take the first one found
                Ubergraph = &F;
        }
        else if (IsConstructionScript(F))
        {
            ConstructionScripts.Add(&F);
        }
        else if (IsDelegateSignature(F))
        {
            // Skip delegate signature stubs — nothing to build
        }
        else if (IsEventStubFunction(F))
        {
            EventStubs.Add(&F);
        }
        else if (IsOverrideEventWithBody(F))
        {
            OverrideEvents.Add(&F);
        }
        else
        {
            NormalFunctions.Add(&F);
        }
    }

    // ── 3. Event / ubergraph ─────────────────────────────────────────────────
    if (Ubergraph)
    {
        UEdGraph *EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
        if (!EventGraph)
        {
            EventGraph = FBlueprintEditorUtils::CreateNewGraph(
                Blueprint,
                UEdGraphSchema_K2::GN_EventGraph,
                UEdGraph::StaticClass(),
                UEdGraphSchema_K2::StaticClass());
            if (EventGraph)
                Blueprint->UbergraphPages.Add(EventGraph);
        }

        if (EventGraph)
            BuildEventGraph(Blueprint, EventGraph, *Ubergraph, EventStubs);
    }

    // ── 4. Override events with a body ───────────────────────────────────────
    for (const FBPFunctionData *F : OverrideEvents)
    {
        if (!F) continue;
        UEdGraph *EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
        if (!EventGraph) continue;

        UEdGraphNode *EvNode = MakeEventNode(Blueprint, EventGraph, *F, FVector2D(0.f, 0.f));
        BuildGraph(Blueprint, EventGraph, *F, EvNode);
    }

    // ── 5. Normal functions ───────────────────────────────────────────────────
    for (const FBPFunctionData *F : NormalFunctions)
    {
        if (!F) continue;

        UEdGraph *FuncGraph = nullptr;
        for (UEdGraph *G : Blueprint->FunctionGraphs)
        {
            if (G && G->GetName() == F->Name)
            {
                FuncGraph = G;
                break;
            }
        }

        if (!FuncGraph)
        {
            FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
                Blueprint,
                *F->Name,
                UEdGraph::StaticClass(),
                UEdGraphSchema_K2::StaticClass());
            if (FuncGraph)
                FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, FuncGraph,
                                                                /*bIsUserCreated=*/true,
                                                                nullptr);
        }

        if (FuncGraph)
            BuildGraph(Blueprint, FuncGraph, *F);
    }

    // ── 6. Construction script ────────────────────────────────────────────────
    for (const FBPFunctionData *F : ConstructionScripts)
    {
        if (!F) continue;
        UEdGraph *CSGraph = FBlueprintEditorUtils::FindUserConstructionScript(Blueprint);
        if (CSGraph)
            BuildGraph(Blueprint, CSGraph, *F);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    return true;
}
