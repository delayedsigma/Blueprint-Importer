#include "BlueprintJsonParser.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static FBPObjectRef ParseObjectRefStatic(const TSharedPtr<FJsonObject> &Obj)
{
	FBPObjectRef Ref;
	if (!Obj.IsValid())
		return Ref;
	Obj->TryGetStringField(TEXT("ObjectName"), Ref.ObjectName);
	Obj->TryGetStringField(TEXT("ObjectPath"), Ref.ObjectPath);
	return Ref;
}

static FBPPropertyRef ParsePropertyRefStatic(const TSharedPtr<FJsonObject> &Obj)
{
	FBPPropertyRef Ref;
	if (!Obj.IsValid())
		return Ref;

	Obj->TryGetStringField(TEXT("Type"), Ref.Type);
	Obj->TryGetStringField(TEXT("Name"), Ref.Name);
	Obj->TryGetStringField(TEXT("PropertyFlags"), Ref.PropertyFlags);

	// PropertyClass (object/interface props)
	const TSharedPtr<FJsonObject> *PCObj = nullptr;
	if (Obj->TryGetObjectField(TEXT("PropertyClass"), PCObj))
	{
		(*PCObj)->TryGetStringField(TEXT("ObjectName"), Ref.PropertyClass);
	}

	// Struct (struct props)
	const TSharedPtr<FJsonObject> *StObj = nullptr;
	if (Obj->TryGetObjectField(TEXT("Struct"), StObj))
	{
		(*StObj)->TryGetStringField(TEXT("ObjectName"), Ref.Struct);
	}

	// Path array (InstanceVariable / StructMemberContext)
	const TArray<TSharedPtr<FJsonValue>> *PathArr = nullptr;
	if (Obj->TryGetArrayField(TEXT("Path"), PathArr))
	{
		for (auto &V : *PathArr)
		{
			Ref.Path.Add(V->AsString());
		}
	}

	// Nested layout: { "Owner": {...}, "Property": { "Name": "Foo", "Type": "...", ... } }
	// EX_InstanceVariable and EX_LocalVariable use this format.
	// Read the Property sub-object and promote its fields if the flat Name is still empty.
	const TSharedPtr<FJsonObject> *PropObj = nullptr;
	if (Obj->TryGetObjectField(TEXT("Property"), PropObj))
	{
		FBPPropertyRef Inner = ParsePropertyRefStatic(*PropObj);
		if (Ref.Name.IsEmpty())        Ref.Name          = Inner.Name;
		if (Ref.Type.IsEmpty())        Ref.Type          = Inner.Type;
		if (Ref.PropertyClass.IsEmpty()) Ref.PropertyClass = Inner.PropertyClass;
		if (Ref.Struct.IsEmpty())      Ref.Struct        = Inner.Struct;
		if (Ref.PropertyFlags.IsEmpty()) Ref.PropertyFlags = Inner.PropertyFlags;
		if (Ref.Path.Num() == 0)       Ref.Path          = Inner.Path;
	}

	return Ref;
}

/**
 * Extract the property name from a Variable sub-expression.
 * Handles the nested format:
 *   { "Inst": "EX_LocalVariable", "Variable": { "Owner": {...}, "Property": { "Name": "Foo" } } }
 */
static FBPPropertyRef ExtractPropertyRefFromVariableExpression(const TSharedPtr<FJsonObject> &Obj)
{
	FBPPropertyRef Ref;
	if (!Obj.IsValid())
		return Ref;

	// Try the nested Variable.Property path first
	const TSharedPtr<FJsonObject> *InnerVarObj = nullptr;
	if (Obj->TryGetObjectField(TEXT("Variable"), InnerVarObj))
	{
		const TSharedPtr<FJsonObject> *PropObj = nullptr;
		if ((*InnerVarObj)->TryGetObjectField(TEXT("Property"), PropObj))
		{
			return ParsePropertyRefStatic(*PropObj);
		}
		// Flat layout
		return ParsePropertyRefStatic(*InnerVarObj);
	}

	return Ref;
}

// ---------------------------------------------------------------------------
// Recursive instruction parser
// ---------------------------------------------------------------------------

FBPInstructionPtr FBlueprintJsonParser::ParseInstruction(const TSharedPtr<FJsonObject> &Obj)
{
	if (!Obj.IsValid())
		return nullptr;

	FBPInstructionPtr Inst = MakeShared<FBPInstruction>();

	Obj->TryGetStringField(TEXT("Inst"), Inst->Inst);

	double StmtIdx = -1;
	if (Obj->TryGetNumberField(TEXT("StatementIndex"), StmtIdx))
	{
		Inst->StatementIndex = (int32)StmtIdx;
	}

	// ---- Function reference (EX_CallMath, EX_FinalFunction, EX_LocalFinalFunction) ----
	const TSharedPtr<FJsonObject> *FuncObj = nullptr;
	if (Obj->TryGetObjectField(TEXT("Function"), FuncObj))
	{
		Inst->Function = ParseObjectRefStatic(*FuncObj);
	}
	else
	{
		// EX_VirtualFunction / EX_LocalVirtualFunction store just a function name string
		FString FuncName;
		if (Obj->TryGetStringField(TEXT("Function"), FuncName))
		{
			Inst->Function.ObjectName = FuncName;
		}
	}

	// ---- Variable field ----
	// EX_LetValueOnPersistentFrame uses "DestinationProperty" instead of "Variable"
	// and "AssignmentExpression" instead of "Expression".  Normalise them here
	// so the rest of the parser sees a uniform layout.
	if (Inst->Inst == TEXT("EX_LetValueOnPersistentFrame"))
	{
		const TSharedPtr<FJsonObject> *DestObj = nullptr;
		if (Obj->TryGetObjectField(TEXT("DestinationProperty"), DestObj))
		{
			// DestinationProperty contains Owner + Property — same as a Variable ref
			const TSharedPtr<FJsonObject> *PropObj = nullptr;
			if ((*DestObj)->TryGetObjectField(TEXT("Property"), PropObj))
				Inst->Variable = ParsePropertyRefStatic(*PropObj);
			else
				Inst->Variable = ParsePropertyRefStatic(*DestObj);
		}
		const TSharedPtr<FJsonObject> *AssignObj = nullptr;
		if (Obj->TryGetObjectField(TEXT("AssignmentExpression"), AssignObj))
			Inst->Expression = ParseInstruction(*AssignObj);
	}

	// For EX_Let/EX_LetBool/EX_LetObj, Variable is a sub-expression (EX_LocalVariable etc.)
	// For EX_LocalVariable/EX_InstanceVariable, Variable is { Owner: {...}, Property: {...} }
	const TSharedPtr<FJsonObject> *VarObj = nullptr;
	if (Obj->TryGetObjectField(TEXT("Variable"), VarObj))
	{
		FString VarInst;
		if ((*VarObj)->TryGetStringField(TEXT("Inst"), VarInst))
		{
			// This is a sub-expression (EX_LocalVariable, EX_InstanceVariable, etc.)
			Inst->VariableExpression = ParseInstruction(*VarObj);
			// Also extract the property name for convenience
			Inst->Variable = ExtractPropertyRefFromVariableExpression(*VarObj);
		}
		else
		{
			// Regular property ref: { "Owner": {...}, "Property": {...} } or flat layout
			const TSharedPtr<FJsonObject> *PropObj = nullptr;
			if ((*VarObj)->TryGetObjectField(TEXT("Property"), PropObj))
			{
				Inst->Variable = ParsePropertyRefStatic(*PropObj);
			}
			else
			{
				Inst->Variable = ParsePropertyRefStatic(*VarObj);
			}
		}
	}

	// ---- Property (EX_StructMemberContext) ----
	const TSharedPtr<FJsonObject> *PropRefObj = nullptr;
	if (Obj->TryGetObjectField(TEXT("Property"), PropRefObj))
	{
		Inst->Property = ParsePropertyRefStatic(*PropRefObj);
	}

	// ---- InterfaceClass (EX_DynamicCast, EX_ObjToInterfaceCast) ----
	const TSharedPtr<FJsonObject> *ICObj = nullptr;
	if (Obj->TryGetObjectField(TEXT("InterfaceClass"), ICObj))
	{
		Inst->InterfaceClass = ParseObjectRefStatic(*ICObj);
	}

	// ---- ClassPtr (EX_DynamicCast alternate field name) ----
	const TSharedPtr<FJsonObject> *CPObj = nullptr;
	if (Obj->TryGetObjectField(TEXT("ClassPtr"), CPObj))
	{
		// Store in InterfaceClass for cast resolution
		if (Inst->InterfaceClass.ObjectName.IsEmpty())
		{
			Inst->InterfaceClass = ParseObjectRefStatic(*CPObj);
		}
	}

	// ---- Parameters array ----
	const TArray<TSharedPtr<FJsonValue>> *ParamArr = nullptr;
	if (Obj->TryGetArrayField(TEXT("Parameters"), ParamArr))
	{
		Inst->Parameters = ParseInstructionArray(*ParamArr);
	}

	// ---- Properties array (EX_StructConst member values) ----
	// EX_StructConst uses "Properties" instead of "Parameters".
	const TArray<TSharedPtr<FJsonValue>> *PropsArr = nullptr;
	if (Obj->TryGetArrayField(TEXT("Properties"), PropsArr) && Inst->Parameters.Num() == 0)
	{
		Inst->Parameters = ParseInstructionArray(*PropsArr);
	}

	// ---- Struct field (EX_StructConst type reference) ----
	const TSharedPtr<FJsonObject> *StructRefObj = nullptr;
	if (Obj->TryGetObjectField(TEXT("Struct"), StructRefObj))
	{
		// Reuse Function field to store the struct type for EX_StructConst.
		Inst->Function = ParseObjectRefStatic(*StructRefObj);
	}

	// ---- Sub-expressions ----
	// NOTE: Skip "Expression" for EX_LetValueOnPersistentFrame — it was already
	// normalised from "AssignmentExpression" above; a second pass would overwrite it
	// if the JSON happens to also carry the "Expression" key.
	auto ParseSubExpr = [&](const TCHAR *FieldName, FBPInstructionPtr &OutExpr)
	{
		const TSharedPtr<FJsonObject> *SubObj = nullptr;
		if (Obj->TryGetObjectField(FieldName, SubObj))
		{
			OutExpr = ParseInstruction(*SubObj);
		}
	};

	if (Inst->Inst != TEXT("EX_LetValueOnPersistentFrame"))
		ParseSubExpr(TEXT("Expression"), Inst->Expression);
	ParseSubExpr(TEXT("OffsetExpression"), Inst->OffsetExpression);
	ParseSubExpr(TEXT("BooleanExpression"), Inst->BooleanExpression);
	ParseSubExpr(TEXT("ObjectExpression"), Inst->ObjectExpression);
	ParseSubExpr(TEXT("ContextExpression"), Inst->ContextExpression);
	ParseSubExpr(TEXT("StructExpression"), Inst->StructExpression);
	ParseSubExpr(TEXT("Target"), Inst->Target);
	ParseSubExpr(TEXT("AssigningProperty"), Inst->AssigningProperty);

	// ---- Elements array (EX_SetArray) ----
	const TArray<TSharedPtr<FJsonValue>> *ElemArr = nullptr;
	if (Obj->TryGetArrayField(TEXT("Elements"), ElemArr))
	{
		Inst->Elements = ParseInstructionArray(*ElemArr);
	}

	// ---- ConversionType (EX_Cast) ----
	Obj->TryGetStringField(TEXT("ConversionType"), Inst->ConversionType);

	// ---- Constant values ----
	// The "Value" field can be a number, string, or object depending on the instruction type
	double NumVal = 0.0;

	// Try numeric value
	if (Obj->TryGetNumberField(TEXT("Value"), NumVal))
	{
		if (Inst->Inst == TEXT("EX_IntConst") || Inst->Inst == TEXT("EX_SkipOffsetConst"))
		{
			Inst->IntValue = (int32)NumVal;
		}
		else if (Inst->Inst == TEXT("EX_FloatConst"))
		{
			Inst->FloatValue = (float)NumVal;
		}
		else if (Inst->Inst == TEXT("EX_DoubleConst"))
		{
			Inst->DoubleValue = NumVal;
		}
		else if (Inst->Inst == TEXT("EX_ByteConst"))
		{
			Inst->ByteValue = (uint8)NumVal;
		}
		else
		{
			Inst->IntValue = (int32)NumVal;
		}
	}

	// Try string value
	FString StrVal;
	if (Obj->TryGetStringField(TEXT("Value"), StrVal))
	{
		if (Inst->Inst == TEXT("EX_NameConst"))
		{
			Inst->NameValue = FName(*StrVal);
		}
		else
		{
			Inst->StringValue = StrVal;
		}
	}

	// Try object value (for EX_ObjectConst, EX_VectorConst, EX_RotationConst)
	const TSharedPtr<FJsonObject> *ValObj = nullptr;
	if (Obj->TryGetObjectField(TEXT("Value"), ValObj))
	{
		// Check if it's an object reference (has ObjectName)
		FString ObjName;
		if ((*ValObj)->TryGetStringField(TEXT("ObjectName"), ObjName))
		{
			Inst->ObjectValue = ParseObjectRefStatic(*ValObj);
		}
		else
		{
			// Vector or Rotator
			double X = 0, Y = 0, Z = 0;
			(*ValObj)->TryGetNumberField(TEXT("X"), X);
			(*ValObj)->TryGetNumberField(TEXT("Y"), Y);
			(*ValObj)->TryGetNumberField(TEXT("Z"), Z);
			if (Inst->Inst == TEXT("EX_VectorConst"))
			{
				Inst->VectorValue = FVector((float)X, (float)Y, (float)Z);
			}
			else if (Inst->Inst == TEXT("EX_RotationConst"))
			{
				double Pitch = 0, Yaw = 0, Roll = 0;
				(*ValObj)->TryGetNumberField(TEXT("Pitch"), Pitch);
				(*ValObj)->TryGetNumberField(TEXT("Yaw"), Yaw);
				(*ValObj)->TryGetNumberField(TEXT("Roll"), Roll);
				Inst->RotatorValue = FRotator((float)Pitch, (float)Yaw, (float)Roll);
			}
		}
	}

	// Jump targets
	double CodeOff = -1, PushAddr = -1;
	if (Obj->TryGetNumberField(TEXT("CodeOffset"), CodeOff))
		Inst->CodeOffset = (int32)CodeOff;
	if (Obj->TryGetNumberField(TEXT("PushingAddress"), PushAddr))
		Inst->PushingAddress = (int32)PushAddr;

	// EX_Context byte offset
	double CtxOffset = 0;
	if (Obj->TryGetNumberField(TEXT("Offset"), CtxOffset))
		Inst->Offset = (int32)CtxOffset;

	// ---- FunctionName (EX_BindDelegate, EX_CallMulticastDelegate) ----
	// Stored as either a plain string or an object reference.
	{
		FString FuncNameStr;
		if (Obj->TryGetStringField(TEXT("FunctionName"), FuncNameStr))
		{
			if (Inst->Function.ObjectName.IsEmpty())
				Inst->Function.ObjectName = FuncNameStr;
		}
		else
		{
			const TSharedPtr<FJsonObject> *FNObj = nullptr;
			if (Obj->TryGetObjectField(TEXT("FunctionName"), FNObj))
			{
				FBPObjectRef FNRef = ParseObjectRefStatic(*FNObj);
				if (Inst->Function.ObjectName.IsEmpty())
					Inst->Function = FNRef;
			}
		}
	}

	// ---- Delegate sub-expression ----
	// EX_BindDelegate, EX_AddMulticastDelegate, EX_RemoveMulticastDelegate,
	// EX_CallMulticastDelegate all carry a "Delegate" field that is the
	// delegate variable expression (usually EX_LocalVariable).
	ParseSubExpr(TEXT("Delegate"), Inst->Target);

	// ---- MulticastDelegate sub-expression ----
	// EX_AddMulticastDelegate / EX_RemoveMulticastDelegate carry the
	// multicast variable in "MulticastDelegate" (an EX_Context or EX_InstanceVariable).
	// Store it in ObjectExpression so WireDataPins can access the target object.
	ParseSubExpr(TEXT("MulticastDelegate"), Inst->ObjectExpression);

	// ---- ObjectTerm (EX_BindDelegate) — the object we bind 'self' to ----
	ParseSubExpr(TEXT("ObjectTerm"), Inst->StructExpression);

	// RValuePointer (EX_Context)
	const TSharedPtr<FJsonObject> *RVPObj = nullptr;
	if (Obj->TryGetObjectField(TEXT("RValuePointer"), RVPObj) && RVPObj && (*RVPObj).IsValid())
	{
		const TSharedPtr<FJsonObject> *RVPropObj = nullptr;
		if ((*RVPObj)->TryGetObjectField(TEXT("Property"), RVPropObj))
		{
			FBPPropertyRef RVProp = ParsePropertyRefStatic(*RVPropObj);
			Inst->RValuePointer.ObjectName = RVProp.Name;
		}
	}

	return Inst;
}

TArray<FBPInstructionPtr> FBlueprintJsonParser::ParseInstructionArray(
	const TArray<TSharedPtr<FJsonValue>> &Arr)
{
	TArray<FBPInstructionPtr> Result;
	for (auto &Val : Arr)
	{
		const TSharedPtr<FJsonObject> *ObjPtr = nullptr;
		if (Val->TryGetObject(ObjPtr))
		{
			FBPInstructionPtr Inst = ParseInstruction(*ObjPtr);
			if (Inst.IsValid())
				Result.Add(Inst);
		}
	}
	return Result;
}

FBPPropertyRef FBlueprintJsonParser::ParsePropertyRef(const TSharedPtr<FJsonObject> &Obj)
{
	return ParsePropertyRefStatic(Obj);
}

FBPObjectRef FBlueprintJsonParser::ParseObjectRef(const TSharedPtr<FJsonObject> &Obj)
{
	return ParseObjectRefStatic(Obj);
}

// ---------------------------------------------------------------------------
// Top-level parse
// ---------------------------------------------------------------------------

bool FBlueprintJsonParser::Parse(const FString &JsonString, FBlueprintJsonData &OutData, FString &OutError)
{
	TSharedPtr<FJsonValue> RootValue;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

	if (!FJsonSerializer::Deserialize(Reader, RootValue) || !RootValue.IsValid())
	{
		OutError = TEXT("Invalid JSON – could not deserialize root value.");
		return false;
	}

	// Root must be an array of export objects
	const TArray<TSharedPtr<FJsonValue>> *RootArr = nullptr;
	if (!RootValue->TryGetArray(RootArr))
	{
		OutError = TEXT("Expected a JSON array at root level (UAssetAPI export format).");
		return false;
	}

	// --- Find the BlueprintGeneratedClass entry to get the BP name ---
	for (auto &Val : *RootArr)
	{
		const TSharedPtr<FJsonObject> *ObjPtr = nullptr;
		if (!Val->TryGetObject(ObjPtr))
			continue;

		FString Type;
		(*ObjPtr)->TryGetStringField(TEXT("Type"), Type);

		const bool bIsBGC  = (Type == TEXT("BlueprintGeneratedClass"));
		const bool bIsWBGC = (Type == TEXT("WidgetBlueprintGeneratedClass"));
		const bool bIsABGC = (Type == TEXT("AnimBlueprintGeneratedClass"));

		if (bIsBGC || bIsWBGC || bIsABGC)
		{
			OutData.bIsWidgetBlueprint = bIsWBGC;
			OutData.bIsAnimBlueprint   = bIsABGC;

			(*ObjPtr)->TryGetStringField(TEXT("Name"), OutData.BlueprintName);
			// Remove trailing "_C" suffix to get the friendly name
			if (OutData.BlueprintName.EndsWith(TEXT("_C")))
			{
				OutData.BlueprintName = OutData.BlueprintName.LeftChop(2);
			}

			// Extract parent class.
			// NOTE: a BlueprintGeneratedClass stores its parent under "Super"
			// (an export reference), NOT "SuperStruct". The old code read
			// "SuperStruct" only, so ParentClass came back empty and every
			// imported BP fell back to AActor — which broke parent-chain
			// function resolution and produced "this blueprint is not a
			// FortPlayerPawn" type errors. Read "Super" first.
			const TSharedPtr<FJsonObject> *SuperObj = nullptr;
			if ((*ObjPtr)->TryGetObjectField(TEXT("Super"), SuperObj) ||
				(*ObjPtr)->TryGetObjectField(TEXT("SuperStruct"), SuperObj))
			{
				FString SuperName;
				(*SuperObj)->TryGetStringField(TEXT("ObjectName"), SuperName);
				OutData.ParentClass = SuperName;

				FString SuperPath;
				(*SuperObj)->TryGetStringField(TEXT("ObjectPath"), SuperPath);
				OutData.ParentClassPath = SuperPath;
			}
			// Parse Blueprint-level variables from ChildProperties
			const TArray<TSharedPtr<FJsonValue>> *BGCPropsArr = nullptr;
			if ((*ObjPtr)->TryGetArrayField(TEXT("ChildProperties"), BGCPropsArr))
			{
				OutData.Variables = ParseVariableArray(*BGCPropsArr);
			}

			break;
		}
	}

	if (OutData.BlueprintName.IsEmpty())
	{
		OutError = TEXT("Could not find a BlueprintGeneratedClass, WidgetBlueprintGeneratedClass, or AnimBlueprintGeneratedClass entry in the JSON.");
		return false;
	}

	// --- Parse all Function export objects ---
	for (auto &Val : *RootArr)
	{
		const TSharedPtr<FJsonObject> *ObjPtr = nullptr;
		if (!Val->TryGetObject(ObjPtr))
			continue;

		FString Type;
		(*ObjPtr)->TryGetStringField(TEXT("Type"), Type);
		if (Type != TEXT("Function"))
			continue;

		FBPFunctionData FuncData;
		(*ObjPtr)->TryGetStringField(TEXT("Name"), FuncData.Name);
		(*ObjPtr)->TryGetStringField(TEXT("Outer"), FuncData.Outer);
		(*ObjPtr)->TryGetStringField(TEXT("FunctionFlags"), FuncData.FunctionFlags);

		const TSharedPtr<FJsonObject> *SuperObj = nullptr;
		if ((*ObjPtr)->TryGetObjectField(TEXT("SuperStruct"), SuperObj))
		{
			(*SuperObj)->TryGetStringField(TEXT("ObjectName"), FuncData.SuperStruct);
		}

		// Parse local variables / parameters from ChildProperties
		const TArray<TSharedPtr<FJsonValue>> *FuncPropsArr = nullptr;
		if ((*ObjPtr)->TryGetArrayField(TEXT("ChildProperties"), FuncPropsArr))
		{
			FuncData.LocalVariables = ParseVariableArray(*FuncPropsArr);
		}

		const TArray<TSharedPtr<FJsonValue>> *BytecodeArr = nullptr;
		if ((*ObjPtr)->TryGetArrayField(TEXT("ScriptBytecode"), BytecodeArr))
		{
			FuncData.Instructions = ParseInstructionArray(*BytecodeArr);
		}

		// AnimBP event graph functions: no ScriptBytecode, but have EventGraphCallOffset.
		// Store the offset so the builder can create the correct event node.
		double EvtOffset = -1.0;
		if ((*ObjPtr)->TryGetNumberField(TEXT("EventGraphCallOffset"), EvtOffset))
		{
			FuncData.EventGraphCallOffset = (int32)EvtOffset;
		}

		// Skip if no instructions, no local variables, AND not an AnimBP event graph stub
		if (FuncData.Instructions.Num() == 0 && FuncData.LocalVariables.Num() == 0
			&& FuncData.EventGraphCallOffset == INDEX_NONE)
			continue;

		OutData.Functions.Add(MoveTemp(FuncData));
	}

	if (OutData.Functions.Num() == 0 && OutData.Variables.Num() == 0)
	{
		OutError = TEXT("No Function entries with ScriptBytecode, EventGraphCallOffset, or variables found in JSON.");
		return false;
	}

	return true;
}

// ---------------------------------------------------------------------------
// Variable descriptor parser
// ---------------------------------------------------------------------------

FBPVariableDesc FBlueprintJsonParser::ParseVariableDesc(const TSharedPtr<FJsonObject> &Obj)
{
	FBPVariableDesc Desc;
	if (!Obj.IsValid())
		return Desc;

	Obj->TryGetStringField(TEXT("Type"), Desc.Type);
	Obj->TryGetStringField(TEXT("Name"), Desc.Name);
	Obj->TryGetStringField(TEXT("PropertyFlags"), Desc.PropertyFlags);

	// Derive editor flags from PropertyFlags string
	Desc.bIsInstanceEditable = Desc.PropertyFlags.Contains(TEXT("Edit")) &&
							   !Desc.PropertyFlags.Contains(TEXT("DisableEditOnInstance"));
	Desc.bIsReadOnly = Desc.PropertyFlags.Contains(TEXT("BlueprintReadOnly"));
	Desc.bIsAdvancedDisplay = Desc.PropertyFlags.Contains(TEXT("AdvancedDisplay"));
	Desc.bIsTransient = Desc.PropertyFlags.Contains(TEXT("Transient"));

	// PropertyClass (ObjectProperty, SoftObjectProperty, InterfaceProperty)
	const TSharedPtr<FJsonObject> *PCObj = nullptr;
	if (Obj->TryGetObjectField(TEXT("PropertyClass"), PCObj))
	{
		(*PCObj)->TryGetStringField(TEXT("ObjectName"), Desc.PropertyClass);
		(*PCObj)->TryGetStringField(TEXT("ObjectPath"), Desc.PropertyClassPath);
	}

	// Struct (StructProperty)
	const TSharedPtr<FJsonObject> *StObj = nullptr;
	if (Obj->TryGetObjectField(TEXT("Struct"), StObj))
	{
		(*StObj)->TryGetStringField(TEXT("ObjectName"), Desc.StructName);
		(*StObj)->TryGetStringField(TEXT("ObjectPath"), Desc.StructPath);
	}

	// Enum (EnumProperty)
	const TSharedPtr<FJsonObject> *EnObj = nullptr;
	if (Obj->TryGetObjectField(TEXT("Enum"), EnObj) && EnObj && (*EnObj).IsValid())
	{
		(*EnObj)->TryGetStringField(TEXT("ObjectName"), Desc.EnumName);
		(*EnObj)->TryGetStringField(TEXT("ObjectPath"), Desc.EnumPath);
	}

	// Inner (ArrayProperty)
	const TSharedPtr<FJsonObject> *InnerObj = nullptr;
	if (Obj->TryGetObjectField(TEXT("Inner"), InnerObj))
	{
		Desc.InnerType = MakeShared<FBPVariableDesc>(ParseVariableDesc(*InnerObj));
	}

	return Desc;
}

TArray<FBPVariableDesc> FBlueprintJsonParser::ParseVariableArray(const TArray<TSharedPtr<FJsonValue>> &Arr)
{
	TArray<FBPVariableDesc> Result;
	for (auto &Val : Arr)
	{
		const TSharedPtr<FJsonObject> *ObjPtr = nullptr;
		if (!Val->TryGetObject(ObjPtr))
			continue;

		FBPVariableDesc Desc = ParseVariableDesc(*ObjPtr);
		if (Desc.Name.IsEmpty())
			continue;

		// Skip UE internal bookkeeping
		if (Desc.Name == TEXT("UberGraphFrame"))
			continue;
		if (Desc.bIsTransient)
			continue;

		// Skip compiler-generated temporaries — recognisable by their name patterns
		if (Desc.Name.StartsWith(TEXT("CallFunc_")))
			continue;
		if (Desc.Name.StartsWith(TEXT("K2Node_")))
			continue;
		if (Desc.Name.StartsWith(TEXT("Temp_")))
			continue;
		if (Desc.Name.EndsWith(TEXT("_ImplicitCast")))
			continue;
		if (Desc.Name.EndsWith(TEXT("_ReturnValue")))
			continue;
		// Loop counter / array index temporaries
		if (Desc.Name.Contains(TEXT("_Loop_Counter_")))
			continue;
		if (Desc.Name.Contains(TEXT("_Array_Index_")))
			continue;

		Result.Add(MoveTemp(Desc));
	}
	return Result;
}