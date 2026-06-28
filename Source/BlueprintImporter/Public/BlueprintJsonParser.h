#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// Variable descriptor — built from ChildProperties on the BGC entry
// ---------------------------------------------------------------------------

struct FBPVariableDesc
{
	FString  Name;
	FString  Type;           // e.g. "BoolProperty", "ObjectProperty"
	FString  PropertyFlags;  // raw flags string from JSON

	// Object / interface / soft-object
	FString  PropertyClass;  // ObjectName of PropertyClass
	FString  PropertyClassPath;

	// Struct
	FString  StructName;     // ObjectName of Struct
	FString  StructPath;

	// Enum
	FString  EnumName;
	FString  EnumPath;

	// Array inner type (recursive)
	TSharedPtr<FBPVariableDesc> InnerType;

	// Derived flags
	bool bIsInstanceEditable  = false;
	bool bIsReadOnly          = false;
	bool bIsAdvancedDisplay   = false;
	bool bIsTransient         = false;
};

// ---------------------------------------------------------------------------
// Instruction structs (unchanged from before)
// ---------------------------------------------------------------------------

struct FBPInstruction;
using FBPInstructionPtr = TSharedPtr<FBPInstruction>;

struct FBPPropertyRef
{
	FString Type;
	FString Name;
	FString PropertyClass;
	FString Struct;
	FString PropertyFlags;  // raw flags string, e.g. "Parm | OutParm"
	TArray<FString> Path;
};

struct FBPObjectRef
{
	FString ObjectName;
	FString ObjectPath;
};

struct FBPInstruction
{
	FString Inst;
	int32   StatementIndex = -1;

	FBPObjectRef   Function;
	FBPObjectRef   InterfaceClass;
	FBPPropertyRef Variable;
	FBPPropertyRef Property;

	TArray<FBPInstructionPtr> Parameters;
	FBPInstructionPtr         Expression;
	FBPInstructionPtr         OffsetExpression;
	FBPInstructionPtr         BooleanExpression;
	FBPInstructionPtr         ObjectExpression;
	FBPInstructionPtr         ContextExpression;
	FBPInstructionPtr         StructExpression;
	FBPInstructionPtr         Target;
	FBPInstructionPtr         VariableExpression;
	FBPInstructionPtr         AssigningProperty;
	TArray<FBPInstructionPtr> Elements;

	FString      ConversionType;
	FBPObjectRef ObjectValue;

	int32    IntValue    = 0;
	float    FloatValue  = 0.f;
	double   DoubleValue = 0.0;
	uint8    ByteValue   = 0;
	FName    NameValue;
	FString  StringValue;
	FVector  VectorValue;
	FRotator RotatorValue;

	int32  CodeOffset     = -1;
	int32  PushingAddress = -1;
	int32  Offset         = 0;

	FBPObjectRef RValuePointer;
};

struct FBPFunctionData
{
	FString   Name;
	FString   Outer;
	FString   SuperStruct;
	FString   FunctionFlags;
	TArray<FBPInstructionPtr>  Instructions;
	TArray<FBPVariableDesc>    LocalVariables; // from ChildProperties on the Function entry

	// AnimBP: functions that route through the event graph use EventGraphCallOffset
	// instead of inline ScriptBytecode. INDEX_NONE means not an event-graph function.
	int32     EventGraphCallOffset = INDEX_NONE;
};

struct FBlueprintJsonData
{
	FString   BlueprintName;
	FString   ParentClass;
	FString   ParentClassPath;
	TArray<FBPFunctionData>  Functions;
	TArray<FBPVariableDesc>  Variables; // from ChildProperties on the BlueprintGeneratedClass entry

	// True when the JSON root contains a WidgetBlueprintGeneratedClass export
	// (i.e. the source asset was a UMG Widget Blueprint).
	bool bIsWidgetBlueprint = false;

	// True when the JSON root contains an AnimBlueprintGeneratedClass export.
	bool bIsAnimBlueprint = false;
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

class BLUEPRINTIMPORTER_API FBlueprintJsonParser
{
public:
	static bool Parse(const FString& JsonString, FBlueprintJsonData& OutData, FString& OutError);

private:
	static FBPInstructionPtr             ParseInstruction(const TSharedPtr<FJsonObject>& Obj);
	static FBPPropertyRef                ParsePropertyRef(const TSharedPtr<FJsonObject>& Obj);
	static FBPObjectRef                  ParseObjectRef(const TSharedPtr<FJsonObject>& Obj);
	static TArray<FBPInstructionPtr>     ParseInstructionArray(const TArray<TSharedPtr<FJsonValue>>& Arr);
	static FBPVariableDesc               ParseVariableDesc(const TSharedPtr<FJsonObject>& Obj);
	static TArray<FBPVariableDesc>       ParseVariableArray(const TArray<TSharedPtr<FJsonValue>>& Arr);
};
