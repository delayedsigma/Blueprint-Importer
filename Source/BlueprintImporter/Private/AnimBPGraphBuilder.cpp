#include "AnimBPGraphBuilder.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"

#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

#include "AnimGraphNode_StateMachine.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimStateConduitNode.h"
#include "AnimationGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationStateMachineSchema.h"
#include "AnimationGraphSchema.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Event.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetMathLibrary.h"

#include "Engine/Blueprint.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "FAnimBPGraphBuilder"

// ---------------------------------------------------------------------------
// Statics
// ---------------------------------------------------------------------------
int32 FAnimBPGraphBuilder::LastStateMachinesCreated  = 0;
int32 FAnimBPGraphBuilder::LastStatesCreated         = 0;
int32 FAnimBPGraphBuilder::LastTransitionsCreated    = 0;
int32 FAnimBPGraphBuilder::LastLayerFunctionsCreated = 0;
int32 FAnimBPGraphBuilder::LastEventGraphNodesCreated = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static UEdGraph* FindOrCreateAnimGraph(UAnimBlueprint* AnimBP)
{
	for (UEdGraph* G : AnimBP->FunctionGraphs)
		if (G && G->IsA<UAnimationGraph>())
			return G;

	UAnimationGraph* NewGraph = Cast<UAnimationGraph>(
		FBlueprintEditorUtils::CreateNewGraph(
			AnimBP, TEXT("AnimGraph"),
			UAnimationGraph::StaticClass(),
			UAnimationGraphSchema::StaticClass()));

	if (NewGraph)
		AnimBP->FunctionGraphs.Add(NewGraph);

	return NewGraph;
}

// ---------------------------------------------------------------------------
// Map a type name string to an FEdGraphPinType
// ---------------------------------------------------------------------------

static FEdGraphPinType PinTypeForName(const FString& TypeName)
{
	FEdGraphPinType PT;
	PT.PinCategory = UEdGraphSchema_K2::PC_Wildcard; // default

	if (TypeName == TEXT("bool"))
	{
		PT.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	}
	else if (TypeName == TEXT("int32") || TypeName == TEXT("int"))
	{
		PT.PinCategory = UEdGraphSchema_K2::PC_Int;
	}
	else if (TypeName == TEXT("float"))
	{
		PT.PinCategory = UEdGraphSchema_K2::PC_Float;
	}
	else if (TypeName == TEXT("FString"))
	{
		PT.PinCategory = UEdGraphSchema_K2::PC_String;
	}
	else if (TypeName == TEXT("FName"))
	{
		PT.PinCategory = UEdGraphSchema_K2::PC_Name;
	}
	else if (TypeName == TEXT("FText"))
	{
		PT.PinCategory = UEdGraphSchema_K2::PC_Text;
	}
	else if (!TypeName.IsEmpty())
	{
		// BUG FIX B: When the struct type can't be resolved, AddMemberVariable silently
		// rejects variables with PC_Struct + null SubCategoryObject. Fall back to
		// PC_Wildcard for unresolved types so the variable IS created (shown as wildcard/any)
		// rather than silently dropped.
		UScriptStruct* FoundStruct = FindObject<UScriptStruct>(ANY_PACKAGE, *TypeName);
		if (FoundStruct)
		{
			PT.PinCategory = UEdGraphSchema_K2::PC_Struct;
			PT.PinSubCategoryObject = FoundStruct;
		}
		else
		{
			// Keep as PC_Wildcard — the variable will still be created and visible,
			// just without a resolved type. The user can fix the type manually.
			PT.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
		}
	}

	return PT;
}

// ---------------------------------------------------------------------------
// JSON helpers — must be defined before AddMemberVariables which calls them
// ---------------------------------------------------------------------------

// Convert a JSON FBPVariableDesc type + struct name to the type string PinTypeForName() expects.
static FString TypeNameFromJsonDesc(const FBPVariableDesc& Desc)
{
	const FString& T = Desc.Type;

	if (T == TEXT("BoolProperty"))            return TEXT("bool");
	if (T == TEXT("FloatProperty"))           return TEXT("float");
	if (T == TEXT("DoubleProperty"))          return TEXT("double");
	if (T == TEXT("IntProperty"))             return TEXT("int32");
	if (T == TEXT("Int64Property"))           return TEXT("int64");
	if (T == TEXT("ByteProperty"))            return TEXT("uint8");
	if (T == TEXT("StrProperty"))             return TEXT("FString");
	if (T == TEXT("NameProperty"))            return TEXT("FName");
	if (T == TEXT("TextProperty"))            return TEXT("FText");

	if (T == TEXT("StructProperty"))
	{
		FString S = Desc.StructName;
		int32 Quote;
		if (S.FindChar('\'', Quote))
		{
			S = S.Mid(Quote + 1);
			S.RemoveFromEnd(TEXT("'"));
		}
		if (S == TEXT("Vector"))       return TEXT("FVector");
		if (S == TEXT("Vector2D"))     return TEXT("FVector2D");
		if (S == TEXT("Vector4"))      return TEXT("FVector4");
		if (S == TEXT("Rotator"))      return TEXT("FRotator");
		if (S == TEXT("Transform"))    return TEXT("FTransform");
		if (S == TEXT("LinearColor"))  return TEXT("FLinearColor");
		if (S == TEXT("Color"))        return TEXT("FColor");
		if (S == TEXT("Quat"))         return TEXT("FQuat");
		if (S == TEXT("Box"))          return TEXT("FBox");
		if (S == TEXT("Box2D"))        return TEXT("FBox2D");
		if (S == TEXT("Plane"))        return TEXT("FPlane");
		if (S == TEXT("Matrix"))       return TEXT("FMatrix");
		if (S == TEXT("IntPoint"))     return TEXT("FIntPoint");
		if (S == TEXT("IntVector"))    return TEXT("FIntVector");
		if (S == TEXT("PoseLink"))     return TEXT("FPoseLink");
		if (S == TEXT("GameplayTag"))  return TEXT("FGameplayTag");
		if (S == TEXT("GameplayTagContainer")) return TEXT("FGameplayTagContainer");
		return S; // user-defined struct — try FindObject by name
	}

	if (T == TEXT("ObjectProperty") || T == TEXT("SoftObjectProperty")
		|| T == TEXT("WeakObjectProperty") || T == TEXT("LazyObjectProperty"))
	{
		return Desc.PropertyClass.IsEmpty() ? TEXT("UObject") : Desc.PropertyClass;
	}

	if (T == TEXT("ClassProperty") || T == TEXT("SoftClassProperty"))
		return TEXT("UClass");

	if (T == TEXT("EnumProperty"))
		return Desc.EnumName.IsEmpty() ? TEXT("uint8") : Desc.EnumName;

	if (T == TEXT("ArrayProperty"))  return TEXT("TArray");
	if (T == TEXT("MapProperty"))    return TEXT("TMap");
	if (T == TEXT("SetProperty"))    return TEXT("TSet");
	if (T == TEXT("DelegateProperty"))          return TEXT("FDelegate");
	if (T == TEXT("MulticastDelegateProperty")) return TEXT("FMulticastDelegate");
	if (T == TEXT("InterfaceProperty"))         return TEXT("TScriptInterface");

	return TEXT("");
}

// Convert JSON PropertyFlags string to EPropertyFlags bitmask.
static EPropertyFlags CpfFromJsonFlags(const FString& Flags)
{
	EPropertyFlags Out = CPF_None;
	if (Flags.Contains(TEXT("BlueprintVisible")))       Out |= CPF_BlueprintVisible;
	if (Flags.Contains(TEXT("BlueprintReadOnly")))      Out |= CPF_BlueprintReadOnly;
	if (Flags.Contains(TEXT("Edit")))                   Out |= CPF_Edit;
	if (Flags.Contains(TEXT("EditConst")))              Out |= CPF_EditConst;
	if (Flags.Contains(TEXT("DisableEditOnInstance")))  Out |= CPF_DisableEditOnInstance;
	if (Flags.Contains(TEXT("DisableEditOnTemplate")))  Out |= CPF_DisableEditOnTemplate;
	if (Flags.Contains(TEXT("Transient")))              Out |= CPF_Transient;
	if (Flags.Contains(TEXT("AdvancedDisplay")))        Out |= CPF_AdvancedDisplay;
	if (Flags.Contains(TEXT("SaveGame")))               Out |= CPF_SaveGame;
	if (Flags.Contains(TEXT("Net")))                    Out |= CPF_Net;
	if (Flags.Contains(TEXT("Interp")))                 Out |= CPF_Interp;
	return Out;
}

// ---------------------------------------------------------------------------
// Add member variables to the Blueprint
// ---------------------------------------------------------------------------

static void AddMemberVariables(UBlueprint* BP, const TArray<FAnimBPMemberVarData>& Vars)
{
	for (const FAnimBPMemberVarData& Var : Vars)
	{
		// Skip internal anim node structs and engine-managed fields — not user variables
		if (Var.VarName.StartsWith(TEXT("AnimGraphNode_"))
			|| Var.VarName.StartsWith(TEXT("AnimBlueprintExtension_"))
			|| Var.VarName.StartsWith(TEXT("FortAnimGraphNode_"))
			|| Var.VarName == TEXT("UberGraphFrame")
			|| Var.VarName == TEXT("UberGraphFunction")
			|| Var.VarName == TEXT("TargetSkeleton")
			|| Var.VarName == TEXT("AnimNodeData")
			|| Var.VarName == TEXT("NodeTypeMap")
			|| Var.VarName == TEXT("AnimBlueprintClassSubsystem_PropertyAccess")
			|| Var.VarName == TEXT("InheritableComponentHandler"))
			continue;

		// BUG FIX A: Skip UObject pointer fields (TypeName starts with 'U' or 'A').
		// These are things like "class UFunction* UberGraphFunction" or "class USkeleton* TargetSkeleton"
		// which are engine-owned references, not user BP variables. AddMemberVariable with
		// PC_Object and a null SubCategoryObject crashes or silently produces a broken variable.
		if (!Var.TypeName.IsEmpty()
			&& (Var.TypeName[0] == 'U' || Var.TypeName[0] == 'A')
			&& Var.TypeName.Len() > 1 && FChar::IsUpper(Var.TypeName[1]))
		{
			// Only skip if it looks like a UClass (not FFoo or plain types).
			// We still want to keep e.g. "FVector" which starts with F.
			continue;
		}

		// Skip if already exists
		bool bExists = false;
		for (const FBPVariableDescription& Existing : BP->NewVariables)
		{
			if (Existing.VarName == *Var.VarName)
			{
				bExists = true;
				break;
			}
		}
		if (bExists) continue;

		FEdGraphPinType PinType = PinTypeForName(Var.TypeName);

		// If JSON gave us a struct asset path, try to load the UScriptStruct from it.
		// This resolves UserDefinedStruct types (e.g. GravityOverrideParamsStruct) that
		// FindObject(ANY_PACKAGE) can't find from the name alone at import time.
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Struct
			&& !PinType.PinSubCategoryObject.IsValid()
			&& !Var.JsonStructPath.IsEmpty())
		{
			// JsonStructPath is e.g. "/Game/Animation/Libraries/GravityOverrideParamsStruct.0"
			// UScriptStruct lives at the outer package path
			FString StructObjPath = Var.JsonStructPath;
			// Strip trailing ".N" index suffix — the struct object is at the package level
			int32 LastDot;
			if (StructObjPath.FindLastChar('.', LastDot))
			{
				FString Suffix = StructObjPath.Mid(LastDot + 1);
				// If everything after the last dot is digits it's an index, not a subobject
				bool bIsIndex = !Suffix.IsEmpty();
				for (TCHAR C : Suffix) { if (!FChar::IsDigit(C)) { bIsIndex = false; break; } }
				if (bIsIndex)
					StructObjPath = StructObjPath.Left(LastDot);
			}
			// Try load
			if (UScriptStruct* S = LoadObject<UScriptStruct>(nullptr, *StructObjPath))
				PinType.PinSubCategoryObject = S;
		}

		FBlueprintEditorUtils::AddMemberVariable(BP, *Var.VarName, PinType);

		// Apply property flags — JSON gives us the exact set
		for (FBPVariableDescription& VarDesc : BP->NewVariables)
		{
			if (VarDesc.VarName != *Var.VarName) continue;

			if (!Var.JsonPropertyFlags.IsEmpty())
			{
				// Apply UE CPF_ flags from the JSON flags string
				const EPropertyFlags Cpf = CpfFromJsonFlags(Var.JsonPropertyFlags);
				VarDesc.PropertyFlags |= Cpf;

				// Instance-editable / details panel visibility
				if (Var.JsonPropertyFlags.Contains(TEXT("Edit"))
					&& !Var.JsonPropertyFlags.Contains(TEXT("DisableEditOnInstance")))
				{
					VarDesc.PropertyFlags |= CPF_Edit;
					// Mark as instance editable in the BP editor
					VarDesc.PropertyFlags &= ~CPF_DisableEditOnInstance;
				}
				if (Var.JsonPropertyFlags.Contains(TEXT("BlueprintVisible")))
					VarDesc.PropertyFlags |= CPF_BlueprintVisible;
				if (Var.JsonPropertyFlags.Contains(TEXT("BlueprintReadOnly")))
					VarDesc.PropertyFlags |= CPF_BlueprintReadOnly;
			}
			else if (Var.bIsPrivate)
			{
				VarDesc.PropertyFlags |= CPF_BlueprintReadOnly;
			}
			break;
		}
	}
}

// ---------------------------------------------------------------------------
// Add local variables to a function graph
// ---------------------------------------------------------------------------

static void AddLocalVariables(UBlueprint* BP, UEdGraph* FuncGraph,
                               const TArray<FAnimBPLocalVarData>& Locals,
                               const TArray<FAnimBPMemberVarData>& MemberVars)
{
	// Build set of member var names so we don't duplicate them as locals
	TSet<FString> MemberNames;
	for (const FAnimBPMemberVarData& MV : MemberVars)
		MemberNames.Add(MV.VarName);

	for (const FAnimBPLocalVarData& LV : Locals)
	{
		if (MemberNames.Contains(LV.VarName)) continue;

		// Skip obviously non-variable identifiers
		if (LV.VarName == TEXT("this") || LV.VarName.IsEmpty()) continue;
		if (LV.VarName.StartsWith(TEXT("Label_"))) continue;
		if (LV.VarName.StartsWith(TEXT("UKismet")) || LV.VarName.StartsWith(TEXT("FindObject"))) continue;

		FBPVariableDescription LocalVar;
		LocalVar.VarName     = *LV.VarName;
		LocalVar.VarType     = PinTypeForName(LV.InferredType);
		LocalVar.FriendlyName = FName::NameToDisplayString(LV.VarName, false);
		LocalVar.VarGuid     = FGuid::NewGuid();

		FBlueprintEditorUtils::AddLocalVariable(
			BP,
			FuncGraph,
			LocalVar.VarName,
			LocalVar.VarType,
			FString()
		);
	}
}

// ---------------------------------------------------------------------------
// Helpers for PopulateFunctionGraph
// ---------------------------------------------------------------------------

// Resolve a class reference string like "UKismetSystemLibrary" or
// "FindObject<UFortAnimationBPFunctionLibrary_C>(...)" to just the bare class name.
static FString ExtractClassName(const FString& FuncObject)
{
	// Strip FindObject<ClassName>(...) wrapper
	FString S = FuncObject;
	int32 AngleOpen;
	if (S.FindChar('<', AngleOpen))
	{
		int32 AngleClose;
		if (S.FindChar('>', AngleClose) && AngleClose > AngleOpen)
			S = S.Mid(AngleOpen + 1, AngleClose - AngleOpen - 1);
	}
	// Strip leading U/A prefix if it looks like a UClass name
	return S.TrimStartAndEnd();
}

// Robustly resolve a class name to UClass*, searching all known script packages.
// Also handles Blueprint _C classes via LoadObject if an asset path is embedded.
// Mirrors BlueprintGraphBuilder::ResolveClass.
static UClass* ResolveAnimClass(const FString& RawClassName)
{
	if (RawClassName.IsEmpty()) return nullptr;

	FString ClassName = RawClassName;
	// Strip "Default__" prefix (CDO references)
	ClassName.RemoveFromStart(TEXT("Default__"));

	// Fast path: ANY_PACKAGE search (works if the class object is in GUObjectArray)
	if (UClass* C = FindObject<UClass>(ANY_PACKAGE, *ClassName))
		return C;

	// Try without U/A prefix
	FString Bare = ClassName;
	if (Bare.StartsWith(TEXT("U")) || Bare.StartsWith(TEXT("A")) || Bare.StartsWith(TEXT("F")))
		Bare = Bare.Mid(1);

	if (UClass* C = FindObject<UClass>(ANY_PACKAGE, *Bare))
		return C;

	// Search common script packages by full path
	auto TryScriptPkg = [&](const FString& Pkg) -> UClass*
	{
		for (const FString& Name : TArray<FString>{ClassName, Bare})
		{
			FString FullPath = Pkg + TEXT(".") + Name;
			if (UClass* C = FindObject<UClass>(nullptr, *FullPath))
				return C;
		}
		return nullptr;
	};

	static const TCHAR* CommonPackages[] = {
		TEXT("/Script/Engine"),
		TEXT("/Script/CoreUObject"),
		TEXT("/Script/KismetCore"),
		TEXT("/Script/AnimGraphRuntime"),
		TEXT("/Script/UMG"),
		TEXT("/Script/InputCore"),
		TEXT("/Script/GameplayAbilities"),
		TEXT("/Script/GameplayTags"),
		TEXT("/Script/Niagara"),
		TEXT("/Script/MovieScene"),
		TEXT("/Script/Foliage"),
		TEXT("/Script/FortniteGame"),
	};
	for (const TCHAR* Pkg : CommonPackages)
	{
		if (UClass* C = TryScriptPkg(FString(Pkg)))
			return C;
	}

	return nullptr;
}

// Extract the asset path from a FindObject<T>(outer, "path") expression.
// e.g. FindObject<UFortAnimBPLib_C>(nullptr, "/Game/Foo/Bar.Default__Bar_C")
// returns "/Game/Foo/Bar.Bar_C" (normalized to the generated class path).
static FString ExtractFindObjectAssetPath(const FString& FuncObject)
{
	// Find a quoted string inside the expression
	int32 Q1 = INDEX_NONE, Q2 = INDEX_NONE;
	for (int32 i = 0; i < FuncObject.Len(); ++i)
	{
		if (FuncObject[i] == '"')
		{
			if (Q1 == INDEX_NONE) Q1 = i;
			else { Q2 = i; break; }
		}
	}
	if (Q1 == INDEX_NONE || Q2 == INDEX_NONE) return TEXT("");
	FString Path = FuncObject.Mid(Q1 + 1, Q2 - Q1 - 1);
	// Strip Default__ from the last segment so we get the class path
	int32 LastDot;
	if (Path.FindLastChar('.', LastDot))
	{
		FString Pkg = Path.Left(LastDot);
		FString Cls = Path.Mid(LastDot + 1);
		Cls.RemoveFromStart(TEXT("Default__"));
		if (!Cls.EndsWith(TEXT("_C"))) Cls += TEXT("_C");
		return Pkg + TEXT(".") + Cls;
	}
	return Path;
}

// Try to load a Blueprint-generated UClass from an asset path.
static UClass* LoadBlueprintClass(const FString& AssetPath)
{
	if (AssetPath.IsEmpty()) return nullptr;
	if (UClass* C = FindObject<UClass>(nullptr, *AssetPath)) return C;
	// Blueprint classes use Class'...' reference format sometimes
	return LoadObject<UClass>(nullptr, *AssetPath);
}

// Build a UK2Node_CallFunction for any function - resolved or not.
// For unresolved functions we set the member reference by name so the node
// appears correctly in the graph and can be fixed up when the class is available.
static UK2Node_CallFunction* MakeCallNode(UEdGraph* Graph, const FAnimBPStatement& Stmt, int32 X, int32 Y)
{
	UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
	CallNode->NodePosX = X;
	CallNode->NodePosY = Y;
	CallNode->CreateNewGuid();

	// Try to resolve the UFunction via reflection first
	UFunction* ResolvedFunc = nullptr;
	UClass* ResolvedClass = nullptr;
	if (!Stmt.FuncName.IsEmpty())
	{
		// Resolve class from FuncObject string first
		if (!Stmt.FuncObject.IsEmpty())
		{
			FString ClassName = ExtractClassName(Stmt.FuncObject);
			ResolvedClass = ResolveAnimClass(ClassName);

			// If not found via script packages, try loading as a Blueprint asset
			if (!ResolvedClass)
			{
				FString AssetPath = ExtractFindObjectAssetPath(Stmt.FuncObject);
				ResolvedClass = LoadBlueprintClass(AssetPath);
			}
			if (ResolvedClass)
			{
				// Walk the class hierarchy to find the function
				UClass* Search = ResolvedClass;
				while (Search && !ResolvedFunc)
				{
					ResolvedFunc = Search->FindFunctionByName(*Stmt.FuncName);
					Search = Search->GetSuperClass();
				}
			}
		}

		// Fallback: global search (sometimes works for statically registered functions)
		if (!ResolvedFunc)
			ResolvedFunc = FindObject<UFunction>(ANY_PACKAGE, *Stmt.FuncName);
	}

	if (ResolvedFunc)
	{
		CallNode->SetFromFunction(ResolvedFunc);
	}
	else
	{
		// Still a real UK2Node_CallFunction — it will auto-resolve when the class loads.
		if (ResolvedClass)
			CallNode->FunctionReference.SetExternalMember(*Stmt.FuncName, ResolvedClass);
		else
			CallNode->FunctionReference.SetSelfMember(*Stmt.FuncName);
	}

	CallNode->PostPlacedNewNode();
	CallNode->AllocateDefaultPins();
	return CallNode;
}

// Resolve an expression string to a pin on an already-placed node.
// Returns the output pin that produces this value, or nullptr.
// PinMap: varName -> output pin from a previously placed node.
static UEdGraphPin* ResolveToPinOrLiteral(
	const FString& Expr,
	UEdGraph* Graph,
	const TMap<FString, UEdGraphPin*>& PinMap,
	const TSet<FString>& MemberVarNames,
	int32& CurX, int32 CurY,
	const int32 StepX,
	const TSet<FString>* LocalVarNames = nullptr)
{
	FString E = Expr.TrimStartAndEnd();

	// Direct variable reference in the pin map (output of a prior call / event param)
	if (UEdGraphPin* const* Found = PinMap.Find(E))
		return *Found;

	// Helper: create a VariableGet node and return its output pin
	auto MakeVarGet = [&](const FString& VarName, bool bSelfMember) -> UEdGraphPin*
	{
		UK2Node_VariableGet* GetNode = NewObject<UK2Node_VariableGet>(Graph);
		if (bSelfMember)
			GetNode->VariableReference.SetSelfMember(*VarName);
		else
			GetNode->VariableReference.SetSelfMember(*VarName); // locals are also self-scoped in BP
		GetNode->NodePosX = CurX;
		GetNode->NodePosY = CurY + 120;
		GetNode->CreateNewGuid();
		GetNode->PostPlacedNewNode();
		GetNode->AllocateDefaultPins();
		Graph->Nodes.Add(GetNode);
		for (UEdGraphPin* P : GetNode->Pins)
			if (P->Direction == EGPD_Output && P->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				return P;
		return nullptr;
	};

	// Member variable -> VariableGet node
	if (MemberVarNames.Contains(E))
		return MakeVarGet(E, true);

	// Local variable (e.g. a local object ref used as FuncObject for ->calls)
	// These are in scope but not yet in PinMap because they were set by a prior
	// assignment that the text parser recorded as a local var, not a member var.
	if (LocalVarNames && LocalVarNames->Contains(E))
		return MakeVarGet(E, false);

	// FVector(x, y, z) literal -> KismetMathLibrary::MakeVector
	if (E.StartsWith(TEXT("FVector(")))
	{
		UClass* MathLib = ResolveAnimClass(TEXT("KismetMathLibrary"));
		UFunction* MakeVecFunc = MathLib ? MathLib->FindFunctionByName(TEXT("MakeVector")) : nullptr;
		if (!MakeVecFunc)
			MakeVecFunc = FindObject<UFunction>(ANY_PACKAGE, TEXT("MakeVector"));
		if (MakeVecFunc)
		{
			// Parse components
			FString Inner = E.Mid(8); // skip "FVector("
			Inner.RemoveFromEnd(TEXT(")"));
			TArray<FString> Parts;
			Inner.ParseIntoArray(Parts, TEXT(","));

			UK2Node_CallFunction* VecNode = NewObject<UK2Node_CallFunction>(Graph);
			VecNode->SetFromFunction(MakeVecFunc);
			VecNode->NodePosX = CurX; VecNode->NodePosY = CurY + 200;
			VecNode->CreateNewGuid(); VecNode->PostPlacedNewNode(); VecNode->AllocateDefaultPins();
			if (Parts.IsValidIndex(0)) { UEdGraphPin* P = VecNode->FindPin(TEXT("X")); if (P) P->DefaultValue = Parts[0].TrimStartAndEnd(); }
			if (Parts.IsValidIndex(1)) { UEdGraphPin* P = VecNode->FindPin(TEXT("Y")); if (P) P->DefaultValue = Parts[1].TrimStartAndEnd(); }
			if (Parts.IsValidIndex(2)) { UEdGraphPin* P = VecNode->FindPin(TEXT("Z")); if (P) P->DefaultValue = Parts[2].TrimStartAndEnd(); }
			Graph->Nodes.Add(VecNode);
			for (UEdGraphPin* P : VecNode->Pins)
				if (P->Direction == EGPD_Output && P->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
					return P;
		}
	}

	// FRotator(pitch, yaw, roll) literal -> KismetMathLibrary::MakeRotator
	if (E.StartsWith(TEXT("FRotator(")))
	{
		UClass* MathLib = ResolveAnimClass(TEXT("KismetMathLibrary"));
		UFunction* MakeRotFunc = MathLib ? MathLib->FindFunctionByName(TEXT("MakeRotator")) : nullptr;
		if (!MakeRotFunc)
			MakeRotFunc = FindObject<UFunction>(ANY_PACKAGE, TEXT("MakeRotator"));
		if (MakeRotFunc)
		{
			FString Inner = E.Mid(9);
			Inner.RemoveFromEnd(TEXT(")"));
			TArray<FString> Parts;
			Inner.ParseIntoArray(Parts, TEXT(","));

			UK2Node_CallFunction* RotNode = NewObject<UK2Node_CallFunction>(Graph);
			RotNode->SetFromFunction(MakeRotFunc);
			RotNode->NodePosX = CurX; RotNode->NodePosY = CurY + 280;
			RotNode->CreateNewGuid(); RotNode->PostPlacedNewNode(); RotNode->AllocateDefaultPins();
			if (Parts.IsValidIndex(0)) { UEdGraphPin* P = RotNode->FindPin(TEXT("Pitch")); if (!P) P = RotNode->FindPin(TEXT("X")); if (P) P->DefaultValue = Parts[0].TrimStartAndEnd(); }
			if (Parts.IsValidIndex(1)) { UEdGraphPin* P = RotNode->FindPin(TEXT("Yaw"));   if (!P) P = RotNode->FindPin(TEXT("Y")); if (P) P->DefaultValue = Parts[1].TrimStartAndEnd(); }
			if (Parts.IsValidIndex(2)) { UEdGraphPin* P = RotNode->FindPin(TEXT("Roll"));  if (!P) P = RotNode->FindPin(TEXT("Z")); if (P) P->DefaultValue = Parts[2].TrimStartAndEnd(); }
			Graph->Nodes.Add(RotNode);
			for (UEdGraphPin* P : RotNode->Pins)
				if (P->Direction == EGPD_Output && P->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
					return P;
		}
	}

	return nullptr;
}

// Wire data input pins of a call node from parsed args + the pin map.
// Out-param args (like "this", output variables) are recorded into PinMap.
static void WireCallNodePins(
	UK2Node_CallFunction* CallNode,
	const FAnimBPStatement& Stmt,
	UEdGraph* Graph,
	TMap<FString, UEdGraphPin*>& PinMap,
	const TSet<FString>& MemberVarNames,
	const TSet<FString>& LocalVarNames,
	int32& CurX, int32 CurY,
	const int32 StepX)
{
	// Collect non-exec input pins in order, separating out the self/Target pin
	TArray<UEdGraphPin*> InputPins;
	TArray<UEdGraphPin*> OutputPins;
	UEdGraphPin*         SelfPin = nullptr;
	for (UEdGraphPin* P : CallNode->Pins)
	{
		if (P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
		if (P->PinName == TEXT("self") || P->PinName == TEXT("Self"))
		{
			SelfPin = P; // collect it — we'll wire FuncObject to it below
			continue;
		}
		if (P->Direction == EGPD_Input)  InputPins.Add(P);
		if (P->Direction == EGPD_Output) OutputPins.Add(P);
	}

	// Wire FuncObject (the thing before ->) to the self/Target pin.
	// This is the fix for "Target must have a connection" errors: the wire was
	// visible in the graph but never actually linked because SelfPin was skipped.
	if (SelfPin && !Stmt.FuncObject.IsEmpty())
	{
		FString Obj = Stmt.FuncObject.TrimStartAndEnd();

		// "this" or empty means self — leave the pin unconnected (default self context)
		if (Obj != TEXT("this") && Obj != TEXT("self") && Obj != TEXT("Self"))
		{
			UEdGraphPin* ObjSrc = ResolveToPinOrLiteral(Obj, Graph, PinMap, MemberVarNames, CurX, CurY, StepX, &LocalVarNames);
			if (ObjSrc)
				ObjSrc->MakeLinkTo(SelfPin);
			// If we can't resolve it (e.g. it's a local not yet in PinMap), store for
			// a deferred link — record the target pin so a later pass can wire it.
			// For now, leave unconnected rather than silently misfiring.
		}
	}

	// Try to match args to input pins by position
	int32 PinIdx = 0;
	for (int32 ArgIdx = 0; ArgIdx < Stmt.Args.Num() && PinIdx < InputPins.Num(); ++ArgIdx)
	{
		const FString& Arg = Stmt.Args[ArgIdx].TrimStartAndEnd();

		// Skip "this" - it's the self pin
		if (Arg == TEXT("this")) continue;

		UEdGraphPin* TargetPin = InputPins[PinIdx];
		++PinIdx;

		// Try to find a source pin for this arg
		UEdGraphPin* SrcPin = ResolveToPinOrLiteral(Arg, Graph, PinMap, MemberVarNames, CurX, CurY, StepX, &LocalVarNames);
		if (SrcPin)
		{
			SrcPin->MakeLinkTo(TargetPin);
		}
		else
		{
			// Set as literal default if it looks like a simple value
			if (!Arg.Contains(TEXT("(")) && !Arg.Contains(TEXT("->")))
				TargetPin->DefaultValue = Arg;
		}
	}

	// Record out-param output pins into PinMap so subsequent statements can use them.
	// Output pins are named CallFunc_FuncName_PinName in decompiled code.
	// We map them by the variable names that appear in the parsed args after the inputs.
	int32 OutArgStart = InputPins.Num(); // rough heuristic
	for (int32 OPIdx = 0; OPIdx < OutputPins.Num(); ++OPIdx)
	{
		UEdGraphPin* OutPin = OutputPins[OPIdx];
		FString PinName = OutPin->PinName.ToString();

		// Record under the pin name itself
		PinMap.Add(PinName, OutPin);

		// Also record under "CallFunc_<FuncName>_<PinName>" which is the decompiled naming
		FString DecompName = FString::Printf(TEXT("CallFunc_%s_%s"), *Stmt.FuncName, *PinName);
		PinMap.Add(DecompName, OutPin);

		// Record under any arg that looks like an out-param variable
		int32 OutArgIdx = OutArgStart + OPIdx;
		if (Stmt.Args.IsValidIndex(OutArgIdx))
		{
			FString OutArgName = Stmt.Args[OutArgIdx].TrimStartAndEnd();
			if (!OutArgName.IsEmpty() && !OutArgName.Contains(TEXT("(")))
				PinMap.Add(OutArgName, OutPin);
		}
	}

	// Also record return value pin under the LHS variable name if set
	if (!Stmt.LHS.IsEmpty())
	{
		for (UEdGraphPin* OutPin : OutputPins)
		{
			// The "first" non-exec output is usually the return value
			PinMap.Add(Stmt.LHS, OutPin);
			break;
		}
	}
}

// Handle math binary expressions in RHS, e.g. "GravityOverride * FVector(0.4, 0.4, 2)"
// Returns the output pin of the math node, or nullptr if not recognized.
static UEdGraphPin* TryBuildMathNode(
	const FString& RHS,
	UEdGraph* Graph,
	TMap<FString, UEdGraphPin*>& PinMap,
	const TSet<FString>& MemberVarNames,
	int32 CurX, int32 CurY)
{
	// Look for * operator at depth 0
	int32 Depth = 0;
	int32 OpIdx = INDEX_NONE;
	TCHAR OpChar = 0;
	for (int32 i = 0; i < RHS.Len(); ++i)
	{
		TCHAR C = RHS[i];
		if (C == '(' || C == '<') ++Depth;
		else if (C == ')' || C == '>') --Depth;
		else if (Depth == 0 && (C == '*' || C == '+' || C == '-'))
		{
			// Make sure it's not a pointer dereference (preceded by non-space)
			if (i > 0 && RHS[i-1] != ' ') continue;
			OpIdx = i; OpChar = C; break;
		}
	}
	if (OpIdx == INDEX_NONE) return nullptr;

	FString LhsExpr = RHS.Left(OpIdx).TrimEnd();
	FString RhsExpr = RHS.Mid(OpIdx + 1).TrimStart();

	// Determine types from the expressions
	bool bLhsVec = LhsExpr.Contains(TEXT("FVector")) || (MemberVarNames.Contains(LhsExpr));
	bool bRhsVec = RhsExpr.StartsWith(TEXT("FVector("));

	FString FuncName;
	if (OpChar == '*' && (bLhsVec || bRhsVec))
		FuncName = TEXT("Multiply_VectorVector");
	else if (OpChar == '*')
		FuncName = TEXT("Multiply_FloatFloat");
	else if (OpChar == '+' && (bLhsVec || bRhsVec))
		FuncName = TEXT("Add_VectorVector");
	else if (OpChar == '+')
		FuncName = TEXT("Add_FloatFloat");
	else if (OpChar == '-' && (bLhsVec || bRhsVec))
		FuncName = TEXT("Subtract_VectorVector");
	else
		return nullptr;

	UFunction* MathFunc = nullptr;
	UClass* MathLib = ResolveAnimClass(TEXT("KismetMathLibrary"));
	if (!MathLib) MathLib = FindObject<UClass>(ANY_PACKAGE, TEXT("KismetMathLibrary"));
	if (MathLib) MathFunc = MathLib->FindFunctionByName(*FuncName);
	if (!MathFunc) return nullptr;

	UK2Node_CallFunction* MathNode = NewObject<UK2Node_CallFunction>(Graph);
	MathNode->SetFromFunction(MathFunc);
	MathNode->NodePosX = CurX; MathNode->NodePosY = CurY + 160;
	MathNode->CreateNewGuid(); MathNode->PostPlacedNewNode(); MathNode->AllocateDefaultPins();
	Graph->Nodes.Add(MathNode);

	// Wire A and B
	const int32 StepX = 280;
	TArray<UEdGraphPin*> InPins;
	for (UEdGraphPin* P : MathNode->Pins)
		if (P->Direction == EGPD_Input && P->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			InPins.Add(P);

	if (InPins.IsValidIndex(0))
	{
		UEdGraphPin* SrcA = ResolveToPinOrLiteral(LhsExpr, Graph, PinMap, MemberVarNames, CurX, CurY, StepX);
		if (SrcA) SrcA->MakeLinkTo(InPins[0]);
		else InPins[0]->DefaultValue = LhsExpr;
	}
	if (InPins.IsValidIndex(1))
	{
		UEdGraphPin* SrcB = ResolveToPinOrLiteral(RhsExpr, Graph, PinMap, MemberVarNames, CurX, CurY, StepX);
		if (SrcB) SrcB->MakeLinkTo(InPins[1]);
		else InPins[1]->DefaultValue = RhsExpr;
	}

	// Return the output pin
	for (UEdGraphPin* P : MathNode->Pins)
		if (P->Direction == EGPD_Output && P->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			return P;

	return nullptr;
}

// ---------------------------------------------------------------------------
// Populate a function/event graph from parsed statement data.
//
// Strategy:
//   - For every FunctionCall, place a real UK2Node_CallFunction (resolved or
//     unresolved by name).  Unresolved nodes show as "unknown" in the graph
//     but are still proper call nodes - they resolve when the class loads.
//   - Track a PinMap (varName -> output pin) to wire data flows between nodes.
//   - Handle math expressions (*, +, -) via KismetMathLibrary nodes.
//   - Handle struct literals (FVector, FRotator) via MakeVector / MakeRotator.
//   - Wire all exec pins sequentially.
//   - For assignments, if RHS is a previously-recorded pin, wire a VariableSet.
// ---------------------------------------------------------------------------

static void PopulateFunctionGraph(UBlueprint* BP, UEdGraph* Graph,
                                   const FAnimBPFunctionData& Func,
                                   const TArray<FAnimBPMemberVarData>& MemberVars)
{
	TSet<FString> MemberVarNames;
	for (const FAnimBPMemberVarData& MV : MemberVars)
		MemberVarNames.Add(MV.VarName);

	// Local variable names for this function — used to resolve FuncObject targets
	// (e.g. "ObscuringLoopEmitter->DestroyComponent()" where ObscuringLoopEmitter
	// is a local UObject reference, not a member var).
	TSet<FString> LocalVarNames;
	for (const FAnimBPLocalVarData& LV : Func.LocalVars)
		LocalVarNames.Add(LV.VarName);
	// Params count as locals too
	for (const FString& P : Func.ParamNames)
		LocalVarNames.Add(P);

	const int32 StepX = 300;

	// Maps variable/pin names to the output pin that produces them.
	// Declared early so the EventNode branch below can populate it.
	TMap<FString, UEdGraphPin*> PinMap;

	// Find the function entry node — either a FunctionEntry (for function graphs)
	// or a K2Node_Event (for event graphs / ubergraph).
	// BUG FIX C: The original code only searched for UK2Node_FunctionEntry.
	// Event graphs (ubergraph, BlueprintUpdateAnimation, AnimNotify_* etc.) use
	// UK2Node_Event instead. When no K2Node_FunctionEntry existed, EntryNode was
	// null, LastExecOut was null, and NOTHING in the function body was ever wired.
	UK2Node_FunctionEntry* EntryNode = nullptr;
	UK2Node_Event*         EventNode = nullptr;
	for (UEdGraphNode* N : Graph->Nodes)
	{
		if (!EntryNode) EntryNode = Cast<UK2Node_FunctionEntry>(N);
		if (!EventNode) EventNode = Cast<UK2Node_Event>(N);
		if (EntryNode && EventNode) break;
	}

	UEdGraphPin* LastExecOut = nullptr;
	int32 CurX = 300;
	int32 CurY = 100;

	if (EntryNode)
	{
		LastExecOut = EntryNode->FindPin(UEdGraphSchema_K2::PN_Then);
		CurX = EntryNode->NodePosX + StepX;
		CurY = EntryNode->NodePosY;
	}
	else if (EventNode)
	{
		LastExecOut = EventNode->FindPin(UEdGraphSchema_K2::PN_Then);
		CurX = EventNode->NodePosX + StepX;
		CurY = EventNode->NodePosY;

		// Record event output data pins into PinMap so the function body can
		// reference them (e.g. DeltaTimeX from BlueprintUpdateAnimation).
		for (UEdGraphPin* P : EventNode->Pins)
			if (P->Direction == EGPD_Output && P->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				PinMap.Add(P->PinName.ToString(), P);
	}

	for (const FAnimBPStatement& Stmt : Func.Statements)
	{
		if (Stmt.Kind == EAnimBPStmtKind::Label
			|| Stmt.Kind == EAnimBPStmtKind::Goto
			|| Stmt.Kind == EAnimBPStmtKind::Unknown)
			continue;

		if (Stmt.Kind == EAnimBPStmtKind::Return)
		{
			UK2Node_FunctionResult* ResultNode = nullptr;
			for (UEdGraphNode* N : Graph->Nodes)
				if (UK2Node_FunctionResult* R = Cast<UK2Node_FunctionResult>(N))
					{ ResultNode = R; break; }
			if (ResultNode && LastExecOut)
			{
				UEdGraphPin* ResultIn = ResultNode->FindPin(UEdGraphSchema_K2::PN_Execute);
				if (ResultIn) LastExecOut->MakeLinkTo(ResultIn);
			}
			break;
		}

		// Pure assignment (no function call on RHS)
		if (Stmt.Kind == EAnimBPStmtKind::Assignment && Stmt.FuncName.IsEmpty())
		{
			// BUG FIX D: UberGraph event parameter stores look like:
			//   "UberGraphFrame->K2Node_Event_DeltaTimeX = DeltaTimeX"
			// The LHS is not a member variable, so the original code skipped it entirely.
			// Instead, map the RHS param name → the event output pin so downstream
			// statements that use DeltaTimeX can resolve it from PinMap.
			FString LhsStripped = Stmt.LHS;
			bool bIsEventParamStore = false;
			if (LhsStripped.Contains(TEXT("->")))
			{
				// e.g. "UberGraphFrame->K2Node_Event_DeltaTimeX"
				// The suffix after K2Node_Event_ is the actual parameter name.
				int32 ArrowIdx;
				if (LhsStripped.FindLastChar('>', ArrowIdx))
				{
					FString FieldName = LhsStripped.Mid(ArrowIdx + 1);
					static const FString Prefix = TEXT("K2Node_Event_");
					if (FieldName.StartsWith(Prefix))
					{
						FString ParamName = FieldName.Mid(Prefix.Len());
						// If the RHS is already in PinMap (event output pin), alias it
						if (UEdGraphPin* const* P = PinMap.Find(Stmt.RHS))
							PinMap.Add(ParamName, *P);
						else if (UEdGraphPin* const* P2 = PinMap.Find(ParamName)) { /* already mapped */ }
						bIsEventParamStore = true;
					}
				}
			}
			if (bIsEventParamStore) continue;

			if (!Stmt.LHS.IsEmpty() && MemberVarNames.Contains(Stmt.LHS))
			{
				// Try math expression first
				UEdGraphPin* RhsPin = TryBuildMathNode(Stmt.RHS, Graph, PinMap, MemberVarNames, CurX, CurY);
				if (!RhsPin)
					RhsPin = ResolveToPinOrLiteral(Stmt.RHS, Graph, PinMap, MemberVarNames, CurX, CurY, StepX);

				// Place a VariableSet
				UK2Node_VariableSet* SetNode = NewObject<UK2Node_VariableSet>(Graph);
				SetNode->VariableReference.SetSelfMember(*Stmt.LHS);
				SetNode->NodePosX = CurX; SetNode->NodePosY = CurY;
				SetNode->CreateNewGuid(); SetNode->PostPlacedNewNode(); SetNode->AllocateDefaultPins();
				Graph->Nodes.Add(SetNode);

				// Wire exec
				if (LastExecOut)
				{
					UEdGraphPin* ExecIn = SetNode->FindPin(UEdGraphSchema_K2::PN_Execute);
					if (ExecIn) LastExecOut->MakeLinkTo(ExecIn);
				}
				LastExecOut = SetNode->FindPin(UEdGraphSchema_K2::PN_Then);

				// Wire data
				if (RhsPin)
				{
					for (UEdGraphPin* P : SetNode->Pins)
						if (P->Direction == EGPD_Input && P->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
							{ RhsPin->MakeLinkTo(P); break; }
				}
				else if (!Stmt.RHS.IsEmpty())
				{
					for (UEdGraphPin* P : SetNode->Pins)
						if (P->Direction == EGPD_Input && P->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
							{ P->DefaultValue = Stmt.RHS; break; }
				}

				CurX += StepX;
			}
			continue;
		}

		// Function call (with or without LHS assignment)
		if (Stmt.Kind == EAnimBPStmtKind::FunctionCall && !Stmt.FuncName.IsEmpty())
		{
			// BUG FIX E: "ExecuteUbergraph_X(entryPoint)" calls appear in the bodies of
			// event stub functions (BlueprintUpdateAnimation, AnimNotify_*, etc.).
			// These are NOT real function calls — they are the dispatch mechanism into the
			// ubergraph at a specific label index. Emitting a UK2Node_CallFunction for them
			// produces a disconnected dead node. Skip them; the ubergraph body is populated
			// separately by the ExecuteUbergraph_ function data.
			if (Stmt.FuncName.StartsWith(TEXT("ExecuteUbergraph_")) || Stmt.FuncName == TEXT("ExecuteUbergraph"))
				continue;

			// Skip struct constructors used as RHS values — FPoseLink(-1), FVector(...), etc.
			// These are not real function calls; they're just default/stub return values.
			bool bIsStructCtor = Stmt.FuncName.StartsWith(TEXT("F")) && Stmt.FuncObject.IsEmpty();
			if (bIsStructCtor)
				continue;
			UK2Node_CallFunction* CallNode = MakeCallNode(Graph, Stmt, CurX, CurY);
			Graph->Nodes.Add(CallNode);

			// Wire exec
			if (LastExecOut)
			{
				UEdGraphPin* ExecIn = CallNode->FindPin(UEdGraphSchema_K2::PN_Execute);
				if (ExecIn) LastExecOut->MakeLinkTo(ExecIn);
			}
			LastExecOut = CallNode->FindPin(UEdGraphSchema_K2::PN_Then);

			// Wire data pins and record outputs
			WireCallNodePins(CallNode, Stmt, Graph, PinMap, MemberVarNames, LocalVarNames, CurX, CurY, StepX);

			// If there is an LHS and it is a member var, wire a VariableSet after the call
			if (!Stmt.LHS.IsEmpty() && MemberVarNames.Contains(Stmt.LHS))
			{
				CurX += StepX;
				UK2Node_VariableSet* SetNode = NewObject<UK2Node_VariableSet>(Graph);
				SetNode->VariableReference.SetSelfMember(*Stmt.LHS);
				SetNode->NodePosX = CurX; SetNode->NodePosY = CurY;
				SetNode->CreateNewGuid(); SetNode->PostPlacedNewNode(); SetNode->AllocateDefaultPins();
				Graph->Nodes.Add(SetNode);

				if (LastExecOut)
				{
					UEdGraphPin* ExecIn = SetNode->FindPin(UEdGraphSchema_K2::PN_Execute);
					if (ExecIn) LastExecOut->MakeLinkTo(ExecIn);
				}
				LastExecOut = SetNode->FindPin(UEdGraphSchema_K2::PN_Then);

				// Wire the return value to the set node
				if (UEdGraphPin* const* RetPin = PinMap.Find(Stmt.LHS))
				{
					for (UEdGraphPin* P : SetNode->Pins)
						if (P->Direction == EGPD_Input && P->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
							{ (*RetPin)->MakeLinkTo(P); break; }
				}
			}

			CurX += StepX;
			continue;
		}
	}
}

// ---------------------------------------------------------------------------
// Main build entry
// ---------------------------------------------------------------------------

bool FAnimBPGraphBuilder::Build(UAnimBlueprint* AnimBP, const FAnimBPTextData& Data, FString& OutError)
{
	if (!AnimBP)
	{
		OutError = TEXT("AnimBlueprint asset is null.");
		return false;
	}

	LastStateMachinesCreated  = 0;
	LastStatesCreated         = 0;
	LastTransitionsCreated    = 0;
	LastLayerFunctionsCreated = 0;
	LastEventGraphNodesCreated = 0;

	// -----------------------------------------------------------------------
	// 0. Member variables
	// -----------------------------------------------------------------------
	AddMemberVariables(AnimBP, Data.MemberVars);

	UEdGraph* AnimGraph = FindOrCreateAnimGraph(AnimBP);
	if (!AnimGraph)
	{
		OutError = TEXT("Could not find or create an AnimGraph.");
		return false;
	}

	// -----------------------------------------------------------------------
	// 1. State machines
	// -----------------------------------------------------------------------
	const float MachineSpacingX = 400.f;
	float MachineX = 200.f;

	for (const FAnimBPStateMachineData& MachineData : Data.StateMachines)
	{
		UAnimGraphNode_StateMachine* SMNode =
			NewObject<UAnimGraphNode_StateMachine>(AnimGraph);
		SMNode->NodePosX = (int32)MachineX;
		SMNode->NodePosY = 200;
		SMNode->CreateNewGuid();
		SMNode->PostPlacedNewNode();
		SMNode->AllocateDefaultPins();
		AnimGraph->Nodes.Add(SMNode);
		MachineX += MachineSpacingX;
		++LastStateMachinesCreated;

		UAnimationStateMachineGraph* SMGraph =
			Cast<UAnimationStateMachineGraph>(
				FBlueprintEditorUtils::CreateNewGraph(
					AnimBP, *MachineData.MachineName,
					UAnimationStateMachineGraph::StaticClass(),
					UAnimationStateMachineSchema::StaticClass()));
		if (!SMGraph) continue;

		SMNode->EditorStateMachineGraph = SMGraph;
		SMGraph->OwnerAnimGraphNode = SMNode;
		AnimBP->FunctionGraphs.Add(SMGraph);

		// States
		const float StateSpacingX = 250.f;
		const float StateSpacingY = 180.f;
		TMap<int32, UEdGraphNode*> IndexToStateNode;

		for (int32 StateIdx = 0; StateIdx < MachineData.States.Num(); ++StateIdx)
		{
			const FAnimBPStateData& StateData = MachineData.States[StateIdx];
			const int32 PosX = (int32)(StateIdx % 4 * StateSpacingX);
			const int32 PosY = (int32)(StateIdx / 4 * StateSpacingY);
			UEdGraphNode* PlacedNode = nullptr;

			if (StateData.bIsConduit)
			{
				UAnimStateConduitNode* ConduitNode = NewObject<UAnimStateConduitNode>(SMGraph);
				ConduitNode->NodePosX = PosX; ConduitNode->NodePosY = PosY;
				ConduitNode->CreateNewGuid(); ConduitNode->PostPlacedNewNode(); ConduitNode->AllocateDefaultPins();
				if (ConduitNode->BoundGraph)
					FBlueprintEditorUtils::RenameGraph(ConduitNode->BoundGraph, StateData.StateName);
				SMGraph->Nodes.Add(ConduitNode);
				PlacedNode = ConduitNode;
			}
			else
			{
				UAnimStateNode* StateNode = NewObject<UAnimStateNode>(SMGraph);
				StateNode->NodePosX = PosX; StateNode->NodePosY = PosY;
				StateNode->CreateNewGuid(); StateNode->PostPlacedNewNode(); StateNode->AllocateDefaultPins();
				if (StateNode->BoundGraph)
					FBlueprintEditorUtils::RenameGraph(StateNode->BoundGraph, StateData.StateName);
				SMGraph->Nodes.Add(StateNode);
				PlacedNode = StateNode;
			}

			if (PlacedNode) IndexToStateNode.Add(StateIdx, PlacedNode);
			++LastStatesCreated;
		}

		// Transitions
		for (const FAnimBPMachineTransitionData& Trans : MachineData.Transitions)
		{
			UEdGraphNode** FromPtr = IndexToStateNode.Find(Trans.PreviousState);
			UEdGraphNode** ToPtr   = IndexToStateNode.Find(Trans.NextState);
			if (!FromPtr || !ToPtr) continue;

			UEdGraphNode* FromNode = *FromPtr;
			UEdGraphNode* ToNode   = *ToPtr;

			UAnimStateTransitionNode* TransNode = NewObject<UAnimStateTransitionNode>(SMGraph);
			TransNode->CreateNewGuid(); TransNode->PostPlacedNewNode(); TransNode->AllocateDefaultPins();
			TransNode->CrossfadeDuration = Trans.CrossfadeDuration;
			TransNode->NodePosX = (FromNode->NodePosX + ToNode->NodePosX) / 2;
			TransNode->NodePosY = (FromNode->NodePosY + ToNode->NodePosY) / 2 - 60;
			SMGraph->Nodes.Add(TransNode);

			auto GetOutputPin = [](UEdGraphNode* N) -> UEdGraphPin*
			{
				if (UAnimStateNode* S = Cast<UAnimStateNode>(N)) return S->GetOutputPin();
				if (UAnimStateConduitNode* C = Cast<UAnimStateConduitNode>(N)) return C->GetOutputPin();
				return nullptr;
			};
			auto GetInputPin = [](UEdGraphNode* N) -> UEdGraphPin*
			{
				if (UAnimStateNode* S = Cast<UAnimStateNode>(N)) return S->GetInputPin();
				if (UAnimStateConduitNode* C = Cast<UAnimStateConduitNode>(N)) return C->GetInputPin();
				return nullptr;
			};

			UEdGraphPin* FromOut = GetOutputPin(FromNode);
			UEdGraphPin* TransIn = TransNode->GetInputPin();
			if (FromOut && TransIn) FromOut->MakeLinkTo(TransIn);

			UEdGraphPin* TransOut = TransNode->GetOutputPin();
			UEdGraphPin* ToIn     = GetInputPin(ToNode);
			if (TransOut && ToIn) TransOut->MakeLinkTo(ToIn);

			++LastTransitionsCreated;
		}
	}

	// -----------------------------------------------------------------------
	// 2. Layer interface functions + function body population
	// -----------------------------------------------------------------------

	TSet<FString> ExistingGraphNames;
	for (UEdGraph* G : AnimBP->FunctionGraphs)  if (G) ExistingGraphNames.Add(G->GetName());
	for (UEdGraph* G : AnimBP->EventGraphs)      if (G) ExistingGraphNames.Add(G->GetName());
	for (UEdGraph* G : AnimBP->UbergraphPages)   if (G) ExistingGraphNames.Add(G->GetName());

	static const TArray<FString> ReservedNames = {
		TEXT("AnimGraph"), TEXT("BlueprintUpdateAnimation"),
		TEXT("NativeUpdateAnimation"), TEXT("BlueprintBeginPlay"),
		TEXT("ReceiveBeginPlay"), TEXT("UserConstructionScript"),
	};
	auto IsUbergraphName = [](const FString& Name) -> bool
	{
		return Name.StartsWith(TEXT("ExecuteUbergraph_")) || Name.StartsWith(TEXT("ExecuteUbergraph"));
	};
	auto ExistsInParentChain = [&](const FString& FuncName) -> bool
	{
		if (!AnimBP->ParentClass) return false;
		return AnimBP->ParentClass->FindFunctionByName(*FuncName) != nullptr;
	};

	// Build a map of FunctionName -> FAnimBPFunctionData for quick lookup
	TMap<FString, const FAnimBPFunctionData*> FuncDataMap;
	for (const FAnimBPFunctionData& FD : Data.Functions)
		FuncDataMap.Add(FD.FunctionName, &FD);

	for (const FAnimBPLayerFunctionData& LayerFunc : Data.LayerFunctions)
	{
		const FString& Name = LayerFunc.FunctionName;
		if (Name.IsEmpty())                    continue;
		if (ReservedNames.Contains(Name))      continue;
		if (IsUbergraphName(Name))             continue;
		if (ExistingGraphNames.Contains(Name)) continue;
		if (ExistsInParentChain(Name))         continue;

		UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
			AnimBP, *Name,
			UEdGraph::StaticClass(),
			UEdGraphSchema_K2::StaticClass());
		if (!FuncGraph) continue;

		FBlueprintEditorUtils::AddFunctionGraph<UClass>(
			AnimBP, FuncGraph, /*bIsUserCreated=*/true, nullptr);

		ExistingGraphNames.Add(Name);
		++LastLayerFunctionsCreated;

		// Add local variables and populate the graph body if we have parse data
		if (const FAnimBPFunctionData* const* FDPtr = FuncDataMap.Find(Name))
		{
			const FAnimBPFunctionData* FD = *FDPtr;
			AddLocalVariables(AnimBP, FuncGraph, FD->LocalVars, Data.MemberVars);
			PopulateFunctionGraph(AnimBP, FuncGraph, *FD, Data.MemberVars);
		}
	}

	// -----------------------------------------------------------------------
	// 3. Event graph (ubergraph) population
	//    BlueprintUpdateAnimation is an event that drives the ubergraph
	// -----------------------------------------------------------------------

	// Find the ubergraph page
	UEdGraph* UberGraph = AnimBP->UbergraphPages.Num() > 0 ? AnimBP->UbergraphPages[0] : nullptr;
	if (!UberGraph && AnimBP->EventGraphs.Num() > 0)
		UberGraph = AnimBP->EventGraphs[0];

	if (UberGraph)
	{
		int32 NodesBefore = UberGraph->Nodes.Num();
		int32 EventNodeX  = 100;
		int32 EventNodeY  = 0;
		const int32 EventNodeSpacingY = 200;

		// Helper: find or create a K2Node_Event for a given function name in the ubergraph.
		// BUG FIX F: The original code called PopulateFunctionGraph on BlueprintUpdateAnimation
		// and the ExecuteUbergraph function sharing the single pre-existing K2Node_Event.
		// Multiple event stubs (AnimNotify_*, custom events) all need their own event nodes.
		auto FindOrCreateEventNode = [&](const FString& EventFuncName, const FAnimBPFunctionData* FD) -> UK2Node_Event*
		{
			// Check if a node already exists for this event
			for (UEdGraphNode* N : UberGraph->Nodes)
			{
				if (UK2Node_Event* EN = Cast<UK2Node_Event>(N))
					if (EN->EventReference.GetMemberName() == *EventFuncName || EN->CustomFunctionName == *EventFuncName)
						return EN;
			}

			// Create a new custom event node
			UK2Node_Event* NewEvent = NewObject<UK2Node_Event>(UberGraph);
			NewEvent->EventReference.SetExternalMember(*EventFuncName, nullptr);
			NewEvent->CustomFunctionName = *EventFuncName;
			NewEvent->bIsEditable = true;
			NewEvent->NodePosX = EventNodeX;
			NewEvent->NodePosY = EventNodeY;
			NewEvent->CreateNewGuid();
			NewEvent->PostPlacedNewNode();
			NewEvent->AllocateDefaultPins();
			UberGraph->Nodes.Add(NewEvent);
			EventNodeY += EventNodeSpacingY;
			return NewEvent;
		};

		// Populate BlueprintUpdateAnimation event
		static const FString UpdateAnimName = TEXT("BlueprintUpdateAnimation");
		if (const FAnimBPFunctionData* const* FDPtr = FuncDataMap.Find(UpdateAnimName))
		{
			const FAnimBPFunctionData* FD = *FDPtr;
			// Ensure there's a K2Node_Event for it
			FindOrCreateEventNode(UpdateAnimName, FD);
			AddLocalVariables(AnimBP, UberGraph, FD->LocalVars, Data.MemberVars);
			PopulateFunctionGraph(AnimBP, UberGraph, *FD, Data.MemberVars);
		}

		// Populate the ExecuteUbergraph body (the actual logic)
		for (auto& KV : FuncDataMap)
		{
			if (IsUbergraphName(KV.Key))
			{
				AddLocalVariables(AnimBP, UberGraph, KV.Value->LocalVars, Data.MemberVars);
				PopulateFunctionGraph(AnimBP, UberGraph, *KV.Value, Data.MemberVars);
				break;
			}
		}

		// Populate AnimNotify_* and other event stubs — each gets its own K2Node_Event
		for (auto& KV : FuncDataMap)
		{
			const FString& FName = KV.Key;
			if (FName == UpdateAnimName) continue;
			if (IsUbergraphName(FName)) continue;
			if (ReservedNames.Contains(FName)) continue;

			// Only process event stubs (their bodies only contain ExecuteUbergraph_ calls
			// and a return, so they're very short)
			const FAnimBPFunctionData* FD = KV.Value;
			bool bIsEventStub = false;
			for (const FAnimBPStatement& S : FD->Statements)
			{
				if (S.Kind == EAnimBPStmtKind::FunctionCall &&
					(S.FuncName.StartsWith(TEXT("ExecuteUbergraph_")) || S.FuncName == TEXT("ExecuteUbergraph")))
				{
					bIsEventStub = true;
					break;
				}
			}
			if (!bIsEventStub) continue;

			FindOrCreateEventNode(FName, FD);
			// Don't call PopulateFunctionGraph for stubs — their only statement is
			// ExecuteUbergraph_X(N) which Fix E now skips, so there's nothing to wire.
			// The event node itself is what we needed.
		}

		LastEventGraphNodesCreated = UberGraph->Nodes.Num() - NodesBefore;
	}

	// -----------------------------------------------------------------------
	// 4. Refresh & mark dirty
	// -----------------------------------------------------------------------
	FBlueprintEditorUtils::RefreshAllNodes(AnimBP);
	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);

	return true;
}

// ---------------------------------------------------------------------------
// JSON property type → FAnimBPMemberVarData type string
//
// ---------------------------------------------------------------------------
// MergeVariables
//
// Rules:
//  1. Build a map of JSON vars keyed by Name.
//  2. For every text var, if a JSON entry with the same name exists:
//       - Replace TypeName with the JSON-derived type (more precise)
//       - Set bIsPrivate from JSON flags (BlueprintVisible → not private)
//       - Carry over DefaultValue if JSON CDO had one
//  3. Add JSON vars that the text parser missed entirely.
//  4. Strip variables that are internal (AnimGraphNode_, extensions, etc.)
//     using the same rules as AddMemberVariables — keeping this list in
//     one place so both paths stay in sync.
// ---------------------------------------------------------------------------

TArray<FAnimBPMemberVarData> FAnimBPGraphBuilder::MergeVariables(
	const TArray<FAnimBPMemberVarData>& TextVars,
	const TArray<FBPVariableDesc>& JsonVars)
{
	// Internal prefixes/names to always skip
	auto ShouldSkip = [](const FString& VarName, const FString& TypeName) -> bool
	{
		if (VarName.StartsWith(TEXT("AnimGraphNode_")))           return true;
		if (VarName.StartsWith(TEXT("AnimBlueprintExtension_")))  return true;
		if (VarName.StartsWith(TEXT("FortAnimGraphNode_")))       return true;
		if (VarName == TEXT("UberGraphFrame"))                     return true;
		if (VarName == TEXT("UberGraphFunction"))                  return true;
		if (VarName == TEXT("TargetSkeleton"))                     return true;
		if (VarName == TEXT("AnimNodeData"))                       return true;
		if (VarName == TEXT("NodeTypeMap"))                        return true;
		if (VarName == TEXT("AnimBlueprintClassSubsystem_PropertyAccess")) return true;
		if (VarName == TEXT("InheritableComponentHandler"))        return true;
		// UObject pointer fields (USomething*, ASomething*)
		if (!TypeName.IsEmpty()
			&& (TypeName[0] == 'U' || TypeName[0] == 'A')
			&& TypeName.Len() > 1 && FChar::IsUpper(TypeName[1]))
			return true;
		return false;
	};

	// Build JSON lookup: Name -> FBPVariableDesc
	TMap<FString, const FBPVariableDesc*> JsonMap;
	for (const FBPVariableDesc& JV : JsonVars)
		if (!JV.Name.IsEmpty())
			JsonMap.Add(JV.Name, &JV);

	TArray<FAnimBPMemberVarData> Result;
	TSet<FString> Seen;

	// Pass 1 — text vars, enriched from JSON
	for (const FAnimBPMemberVarData& TV : TextVars)
	{
		if (TV.VarName.IsEmpty()) continue;
		if (Seen.Contains(TV.VarName)) continue;

		FAnimBPMemberVarData Out = TV;

		if (const FBPVariableDesc* const* JPtr = JsonMap.Find(TV.VarName))
		{
			const FBPVariableDesc& JV = **JPtr;
			FString JsonType = TypeNameFromJsonDesc(JV);
			if (!JsonType.IsEmpty())
				Out.TypeName = JsonType;

			// Reflect BlueprintVisible → not private
			if (JV.PropertyFlags.Contains(TEXT("BlueprintVisible")))
				Out.bIsPrivate = false;
			if (JV.PropertyFlags.Contains(TEXT("BlueprintReadOnly")))
				Out.bIsPrivate = false; // readable at least

			// Store flags string so AddMemberVariables can apply CPF_ correctly
			Out.JsonPropertyFlags = JV.PropertyFlags;

			// Store struct asset path for FindObject resolution
			Out.JsonStructPath = JV.StructPath;
		}

		if (ShouldSkip(Out.VarName, Out.TypeName)) continue;

		Seen.Add(Out.VarName);
		Result.Add(Out);
	}

	// Pass 2 — JSON vars not seen in the text dump
	for (const FBPVariableDesc& JV : JsonVars)
	{
		if (JV.Name.IsEmpty()) continue;
		if (Seen.Contains(JV.Name)) continue;

		// Skip internal & transient
		if (JV.bIsTransient) continue;
		FString JsonType = TypeNameFromJsonDesc(JV);
		if (ShouldSkip(JV.Name, JsonType)) continue;

		// Skip compiler temporaries
		if (JV.Name.StartsWith(TEXT("CallFunc_")))  continue;
		if (JV.Name.StartsWith(TEXT("K2Node_")))    continue;
		if (JV.Name.StartsWith(TEXT("Temp_")))      continue;

		FAnimBPMemberVarData Out;
		Out.VarName           = JV.Name;
		Out.TypeName          = JsonType;
		Out.bIsPrivate        = !JV.PropertyFlags.Contains(TEXT("BlueprintVisible"));
		Out.JsonPropertyFlags = JV.PropertyFlags;
		Out.JsonStructPath    = JV.StructPath;

		Seen.Add(Out.VarName);
		Result.Add(Out);
	}

	return Result;
}

// ---------------------------------------------------------------------------
// BuildWithJson — merges JSON data into text data then calls Build
// ---------------------------------------------------------------------------

bool FAnimBPGraphBuilder::BuildWithJson(UAnimBlueprint* AnimBP,
                                         const FAnimBPTextData& TextData,
                                         const FBlueprintJsonData& JsonData,
                                         FString& OutError)
{
	// Make a mutable copy of the text data so we can enrich it
	FAnimBPTextData Merged = TextData;

	// 1. Merge member variables
	Merged.MemberVars = MergeVariables(TextData.MemberVars, JsonData.Variables);

	// 2. Enrich local variable types in each function using the JSON's
	//    ChildProperties for that function (the CallFunc_* / Temp_* locals).
	//    The text parser infers types heuristically; the JSON has exact types.
	for (FAnimBPFunctionData& Func : Merged.Functions)
	{
		// Find the matching JSON function by name
		const FBPFunctionData* JsonFunc = nullptr;
		for (const FBPFunctionData& JF : JsonData.Functions)
			if (JF.Name == Func.FunctionName) { JsonFunc = &JF; break; }
		if (!JsonFunc) continue;

		// Build a map of JSON local var name -> FBPVariableDesc
		TMap<FString, const FBPVariableDesc*> LocalMap;
		for (const FBPVariableDesc& LV : JsonFunc->LocalVariables)
			if (!LV.Name.IsEmpty())
				LocalMap.Add(LV.Name, &LV);

		for (FAnimBPLocalVarData& LVar : Func.LocalVars)
		{
			if (const FBPVariableDesc* const* JPtr = LocalMap.Find(LVar.VarName))
			{
				FString T = TypeNameFromJsonDesc(**JPtr);
				if (!T.IsEmpty())
					LVar.InferredType = T;
			}
		}

		// Also add locals that appear in the JSON but not in the text parse
		TSet<FString> ExistingLocals;
		for (const FAnimBPLocalVarData& LV : Func.LocalVars)
			ExistingLocals.Add(LV.VarName);

		for (const FBPVariableDesc& JLV : JsonFunc->LocalVariables)
		{
			if (JLV.Name.IsEmpty() || ExistingLocals.Contains(JLV.Name)) continue;
			// Skip compiler temporaries and params
			if (JLV.Name.StartsWith(TEXT("CallFunc_")))  continue;
			if (JLV.Name.StartsWith(TEXT("K2Node_")))    continue;
			if (JLV.Name.StartsWith(TEXT("Temp_")))      continue;
			if (JLV.PropertyFlags.Contains(TEXT("Parm"))) continue;

			FAnimBPLocalVarData LV;
			LV.VarName      = JLV.Name;
			LV.InferredType = TypeNameFromJsonDesc(JLV);
			Func.LocalVars.Add(LV);
		}
	}

	// 3. If JSON has functions that the text parser missed entirely
	//    (e.g. the text was truncated), add stub entries so event nodes get created
	{
		TSet<FString> ExistingFuncs;
		for (const FAnimBPFunctionData& F : Merged.Functions)
			ExistingFuncs.Add(F.FunctionName);

		for (const FBPFunctionData& JF : JsonData.Functions)
		{
			if (JF.Name.IsEmpty() || ExistingFuncs.Contains(JF.Name)) continue;
			// Skip internal functions
			if (JF.Name.StartsWith(TEXT("ExecuteUbergraph_"))) continue;
			if (JF.Name == TEXT("AnimGraph")) continue;

			FAnimBPFunctionData Stub;
			Stub.FunctionName = JF.Name;
			Stub.bIsPrivate   = true;
			for (const FBPVariableDesc& P : JF.LocalVariables)
				if (P.PropertyFlags.Contains(TEXT("Parm")) && !P.PropertyFlags.Contains(TEXT("OutParm")))
					Stub.ParamNames.Add(P.Name);
			Merged.Functions.Add(Stub);
		}
	}

	return Build(AnimBP, Merged, OutError);
}

#undef LOCTEXT_NAMESPACE
