#pragma once

#include "CoreMinimal.h"

// ---------------------------------------------------------------------------
// Data structures produced by the AnimBP text parser
// ---------------------------------------------------------------------------

/** One transition inside a state */
struct FAnimBPTransitionData
{
	int32  TransitionIndex       = -1;
	int32  CanTakeDelegateIndex  = -1;
	int32  CustomResultNodeIndex = -1;
	bool   bDesiredReturnValue   = true;
};

/** One state inside a state machine */
struct FAnimBPStateData
{
	FString                        StateName;
	int32                          StateRootNodeIndex = -1;
	TArray<FAnimBPTransitionData>  Transitions;
	TArray<int32>                  PlayerNodeIndices;
	TArray<int32>                  LayerNodeIndices;
	bool                           bIsConduit          = false;
	bool                           bAlwaysResetOnEntry = false;
};

/** One transition record between states (the machine-level Transitions array) */
struct FAnimBPMachineTransitionData
{
	int32   PreviousState     = -1;
	int32   NextState         = -1;
	float   CrossfadeDuration = 0.2f;
	FString BlendMode;
	FString LogicType;
	FString StateName;
};

/** One entire state machine */
struct FAnimBPStateMachineData
{
	FString                                MachineName;
	int32                                  InitialState = 0;
	TArray<FAnimBPStateData>               States;
	TArray<FAnimBPMachineTransitionData>   Transitions;
};

/** One layer/interface function stub */
struct FAnimBPLayerFunctionData
{
	FString         FunctionName;
	TArray<FString> ParamNames;
	FString         ReturnParam;
};

// ---------------------------------------------------------------------------
// Member / local variable data
// ---------------------------------------------------------------------------

/** A member variable declared at class scope (public: or private:) */
struct FAnimBPMemberVarData
{
	FString TypeName;           // e.g. "FRotator", "FVector", "bool", "float", "int32"
	FString VarName;            // e.g. "HairRotator_01"
	FString DefaultValue;       // raw default string if present, may be empty
	bool    bIsPrivate = false;

	// Enriched from JSON — empty when parsed from text only
	FString JsonPropertyFlags;  // e.g. "Edit | BlueprintVisible | DisableEditOnInstance"
	FString JsonStructPath;     // asset path for user-defined structs e.g. "/Game/Animation/Libraries/GravityOverrideParamsStruct.0"
};

// ---------------------------------------------------------------------------
// Statement / instruction data for function bodies
// ---------------------------------------------------------------------------

/** Kinds of statement we reconstruct from the decompiled body */
enum class EAnimBPStmtKind : uint8
{
	Unknown,
	Label,          // Label_N:
	Goto,           // goto Label_N;
	Assignment,     // LHS = RHS;
	FunctionCall,   // possibly with out-params
	Return,         // return;
};

/** A single reconstructed statement from a decompiled function body */
struct FAnimBPStatement
{
	EAnimBPStmtKind Kind         = EAnimBPStmtKind::Unknown;
	FString         LabelName;   // for Label / Goto
	FString         LHS;         // for Assignment
	FString         RHS;         // for Assignment / full call expression
	FString         FuncObject;  // object/class the function is called on
	FString         FuncName;    // bare function name
	TArray<FString> Args;        // positional args (includes out-params)
	FString         RawLine;     // original source line, always filled
};

/** A local variable inferred from function body analysis */
struct FAnimBPLocalVarData
{
	FString VarName;
	FString InferredType; // best-effort: "FRotator","FVector","float","bool","int32","FString",""
};

/** A reconstructed function (event graph entry or named function) */
struct FAnimBPFunctionData
{
	FString                       FunctionName;
	TArray<FString>               ParamNames;    // declared params
	bool                          bIsPrivate = false;
	TArray<FAnimBPStatement>      Statements;
	TArray<FAnimBPLocalVarData>   LocalVars;     // inferred from body
};

// ---------------------------------------------------------------------------
// Full parse result
// ---------------------------------------------------------------------------

/** Full result of parsing a decompiled AnimBP text blob */
struct FAnimBPTextData
{
	FString                           ClassName;
	FString                           ParentClassName;
	TArray<FAnimBPStateMachineData>   StateMachines;
	TArray<FAnimBPLayerFunctionData>  LayerFunctions;
	TArray<FAnimBPMemberVarData>      MemberVars;     // NEW
	TArray<FAnimBPFunctionData>       Functions;      // NEW  (includes ubergraph + events)
};

// ---------------------------------------------------------------------------
// Parser class
// ---------------------------------------------------------------------------

class BLUEPRINTIMPORTER_API FAnimBPTextParser
{
public:
	static bool Parse(const FString& Text, FAnimBPTextData& OutData, FString& OutError);

private:
	struct FParseCtx
	{
		TArray<FString> Lines;
		int32           LineIdx = 0;

		bool    AtEnd()  const { return LineIdx >= Lines.Num(); }
		FString Peek()   const { return AtEnd() ? TEXT("") : Lines[LineIdx].TrimStartAndEnd(); }
		FString Consume()      { return AtEnd() ? TEXT("") : Lines[LineIdx++].TrimStartAndEnd(); }
		FString PeekRaw() const { return AtEnd() ? TEXT("") : Lines[LineIdx]; }

		FString PeekSkip() const
		{
			for (int32 i = LineIdx; i < Lines.Num(); ++i)
			{
				FString L = Lines[i].TrimStartAndEnd();
				if (!L.IsEmpty() && !L.StartsWith(TEXT("//")))
					return L;
			}
			return TEXT("");
		}

		void SkipBlanks()
		{
			while (!AtEnd())
			{
				FString L = Lines[LineIdx].TrimStartAndEnd();
				if (L.IsEmpty() || L.StartsWith(TEXT("//")))
					++LineIdx;
				else
					break;
			}
		}
	};

	static bool ParseClassHeader(FParseCtx& Ctx, FAnimBPTextData& Out);
	static bool ParseStateMachinesArray(FParseCtx& Ctx, TArray<FAnimBPStateMachineData>& Out);
	static bool ParseSingleMachine(FParseCtx& Ctx, FAnimBPStateMachineData& Out);
	static bool ParseStatesArray(FParseCtx& Ctx, TArray<FAnimBPStateData>& Out);
	static bool ParseSingleState(FParseCtx& Ctx, FAnimBPStateData& Out);
	static bool ParseTransitionsArray(FParseCtx& Ctx, TArray<FAnimBPTransitionData>& Out);
	static bool ParseMachineTransitionsArray(FParseCtx& Ctx, TArray<FAnimBPMachineTransitionData>& Out);
	static bool ParseLayerFunctions(FParseCtx& Ctx, TArray<FAnimBPLayerFunctionData>& Out);

	// NEW: member variable and function body parsing
	static void ParseMemberVars(FParseCtx& Ctx, TArray<FAnimBPMemberVarData>& Out, bool bPrivate);
	static bool ParseFunctionBody(FParseCtx& Ctx, FAnimBPFunctionData& Out);
	static void InferLocalVars(FAnimBPFunctionData& Func);
	static FAnimBPStatement ParseStatement(const FString& Line);

	// Field helpers
	static bool TryGetIntField(const FString& Line, const FString& Key, int32& OutVal);
	static bool TryGetFloatField(const FString& Line, const FString& Key, float& OutVal);
	static bool TryGetBoolField(const FString& Line, const FString& Key, bool& OutVal);
	static bool TryGetFNameField(const FString& Line, const FString& Key, FString& OutVal);
	static bool TryGetStringField(const FString& Line, const FString& Key, FString& OutVal);
	static TArray<int32> ParseIntSet(const FString& SetBlock);

	// Expression helpers
	static bool TryParseMemberVarLine(const FString& Line, FAnimBPMemberVarData& Out, bool bPrivate);
	static FString InferTypeFromName(const FString& VarName, const TArray<FAnimBPMemberVarData>& MemberVars);
};