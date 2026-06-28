#pragma once

#include "CoreMinimal.h"
#include "LevelJsonParser.h"

class ULevel;
class UWorld;
class AActor;
class USceneComponent;

class BLUEPRINTIMPORTER_API FLevelBuilder
{
public:
	static void BuildLevel(UWorld* World, ULevel* Level, const FLevelJsonData& LevelData);

	static int32 LastActorsSpawned;
	static int32 LastActorsSkipped;

private:
	static UClass* ResolveActorClass(const FString& Type, const FString& ClassPath);

	static void ApplyProperties(UObject* Obj,
	                            const TMap<FString, FLevelPropPtr>& Properties,
	                            bool bIsComponent);

	// Recursive single-property applier — handles nested structs (PostProcessSettings etc.)
	static void ApplyPropertyValue(UObject* Obj, FProperty* Prop, void* ValuePtr,
	                               const FLevelPropPtr& Val);

	static void ApplyComponents(AActor* Actor, const FLevelActorData& ActorData);
};
