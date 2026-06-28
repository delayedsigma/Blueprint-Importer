#pragma once

#include "CoreMinimal.h"
#include "BlueprintJsonParser.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UK2Node;
class UK2Node_CallFunction;

class BLUEPRINTIMPORTER_API FBlueprintGraphBuilder
{
public:
	static bool Build(UBlueprint *Blueprint, const FBlueprintJsonData &Data, FString &OutError);

	// Stats from the last Build() call — shown in the success notification
	static int32 LastNodesCreated;
	static int32 LastExecConnections;
	static int32 LastDataConnections;
	static int32 LastUnresolvedClasses;

private:
	// -----------------------------------------------------------------------
	// Per-graph context (used across the three passes)
	// -----------------------------------------------------------------------
	struct FGraphContext
	{
		UEdGraph *Graph = nullptr;

		// statement index -> node that owns it
		TMap<int32, UEdGraphNode *> StatementToNode;

		// statement index -> the instruction that produced the node
		TMap<int32, FBPInstructionPtr> StatementToInstruction;

		// byte offset -> statement index (for resolving jump targets)
		TMap<int32, int32> CodeOffsetToStatement;

		// variable name -> output pin that produces it (for data wiring)
		TMap<FString, UEdGraphPin *> VariableToOutputPin;

		TMap<int32, const FBPFunctionData *> EventEntryOffsets;
		TMap<int32, UEdGraphNode *> OffsetEntryNodes;
	};

	static bool IsEventStubFunction(const FBPFunctionData &F);
	static bool IsConstructionScript(const FBPFunctionData &F);
	static bool IsOverrideEventWithBody(const FBPFunctionData &F);

	static UEdGraphNode *MakeEventNode(
		UBlueprint *Blueprint,
		UEdGraph *EventGraph,
		const FBPFunctionData &EventFunc,
		const FVector2D &Pos);

	static void BuildEventGraph(
		UBlueprint *Blueprint,
		UEdGraph *EventGraph,
		const FBPFunctionData &Ubergraph,
		const TArray<const FBPFunctionData *> &EventStubs);

	// -----------------------------------------------------------------------
	// Top-level graph build (called once per function/event-graph)
	// -----------------------------------------------------------------------
	static void BuildGraph(UBlueprint *Blueprint, UEdGraph *Graph,
						   const FBPFunctionData &FuncData, UEdGraphNode *EntryEventNode = nullptr);

	// -----------------------------------------------------------------------
	// Pass 1 — node factory
	// -----------------------------------------------------------------------
	static UK2Node_CallFunction *MakeCallNode(UEdGraph *Graph,
											  const FBPObjectRef &FuncRef,
											  const FBPInstructionPtr &Inst,
											  const FVector2D &Pos,
											  UClass *ContextClass = nullptr);

	static UEdGraphNode *MakeNodeForInstruction(UBlueprint *Blueprint,
												UEdGraph *Graph,
												const FBPInstructionPtr &Inst,
												const FVector2D &Pos);

	// -----------------------------------------------------------------------
	// Pass 2 — exec pin wiring
	// -----------------------------------------------------------------------
	static void WireExecPins(FGraphContext &Ctx,
							 const TArray<FBPInstructionPtr> &Instructions);

	// -----------------------------------------------------------------------
	// Pass 3 — data pin wiring
	// -----------------------------------------------------------------------
	static void WireDataPins(FGraphContext &Ctx,
							 const TArray<FBPInstructionPtr> &Instructions);

	// -----------------------------------------------------------------------
	// Helpers
	// -----------------------------------------------------------------------
	static void ApplyLiteralToPin(UEdGraphPin *Pin, const FBPInstructionPtr &Inst);
	static void ImportVariables(UBlueprint *Blueprint, const TArray<FBPVariableDesc> &Variables);
	static void ImportLocalVariables(UBlueprint *Blueprint, UEdGraph *FuncGraph, const TArray<FBPVariableDesc> &LocalVars);
	static void MapPropertyToPinType(const FBPVariableDesc &Desc, FEdGraphPinType &OutPinType);

	static FString ExtractShortName(const FString &ObjectName);
	static FString ExtractClassName(const FString &ObjectName);
	static UClass *ResolveClass(const FString &ObjectName, const FString &ObjectPath);
	static UFunction *TryFindFunction(UClass *Class, const FString &FuncName);

	static UEdGraphPin *FindExecPin(UEdGraphNode *Node, EEdGraphPinDirection Direction,
									const FString &PinName = TEXT(""));
	static UEdGraphPin *FindDataPin(UEdGraphNode *Node, EEdGraphPinDirection Direction,
									const FString &PinName = TEXT(""));
	static FString GetVariableNameFromInstruction(const FBPInstructionPtr &Inst);
};
