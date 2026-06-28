#pragma once

#include "CoreMinimal.h"
#include "AnimBPTextParser.h"
#include "BlueprintJsonParser.h"

class UAnimBlueprint;

class BLUEPRINTIMPORTER_API FAnimBPGraphBuilder
{
public:
	/**
	 * Reconstruct an AnimBlueprint from parsed text data alone.
	 */
	static bool Build(UAnimBlueprint* AnimBP, const FAnimBPTextData& Data, FString& OutError);

	/**
	 * Reconstruct an AnimBlueprint using both the text dump and a JSON export.
	 *
	 * The JSON is used to:
	 *   - Fix variable types that the text parser could only guess (FVector, FRotator,
	 *     UserDefinedStruct, ObjectProperty, etc.)
	 *   - Apply correct property flags (BlueprintVisible, BlueprintReadOnly, EditAnywhere, etc.)
	 *   - Resolve exact struct asset paths for FindObject lookups
	 *   - Add variables present in the JSON but missed by the text parser
	 *   - Type local function variables from their CallFunc_* entries in ChildProperties
	 */
	static bool BuildWithJson(UAnimBlueprint* AnimBP,
	                          const FAnimBPTextData& TextData,
	                          const FBlueprintJsonData& JsonData,
	                          FString& OutError);

	// Stats exposed for the notification toast
	static int32 LastStateMachinesCreated;
	static int32 LastStatesCreated;
	static int32 LastTransitionsCreated;
	static int32 LastLayerFunctionsCreated;
	static int32 LastEventGraphNodesCreated;

private:
	/**
	 * Merge JSON variable descriptors into TextData's MemberVars in-place.
	 * Returns the merged list. JSON wins on type/flags; text wins on existence
	 * (vars only in text are kept; vars only in JSON are added).
	 */
	static TArray<FAnimBPMemberVarData> MergeVariables(
		const TArray<FAnimBPMemberVarData>& TextVars,
		const TArray<FBPVariableDesc>& JsonVars);
};
