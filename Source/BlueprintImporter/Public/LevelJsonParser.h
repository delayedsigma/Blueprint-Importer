#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

/** A single property value — can be scalar, struct, object ref, or array */
struct FLevelPropertyValue
{
	enum class EKind { Null, Bool, Int, Float, String, Struct, Array };
	EKind Kind = EKind::Null;

	bool    bValue  = false;
	int64   iValue  = 0;
	double  fValue  = 0.0;
	FString sValue;

	// Struct: named sub-properties
	TMap<FString, TSharedPtr<FLevelPropertyValue>> Fields;

	// Array elements
	TArray<TSharedPtr<FLevelPropertyValue>> Elements;

	// Object reference (ObjectName / ObjectPath pair — resolved to a UE asset path)
	FString AssetPath;   // e.g. "/Game/VFX/Ether/Radianite_Tower_MainMenu"
	FString AssetName;   // e.g. "Radianite_Tower_MainMenu"
	FString AssetType;   // e.g. "ParticleSystem", "StaticMesh", "Material"

	bool IsObjectRef() const { return !AssetPath.IsEmpty(); }
};
using FLevelPropPtr = TSharedPtr<FLevelPropertyValue>;

/** A component attached to an actor (e.g. StaticMeshComponent, ParticleSystemComponent) */
struct FLevelComponentData
{
	FString Type;        // "StaticMeshComponent", "ParticleSystemComponent", etc.
	FString Name;        // e.g. "StaticMeshComponent0"
	TMap<FString, FLevelPropPtr> Properties;
};

/** An actor in the level */
struct FLevelActorData
{
	FString Type;        // "StaticMeshActor", "Emitter", "PostProcessVolume", etc.
	FString Name;        // e.g. "ABuilding_13"
	FString ClassName;   // resolved UE class path, e.g. "/Script/Engine.StaticMeshActor"

	// All components whose Outer is this actor
	TArray<FLevelComponentData> Components;

	// Actor-level properties (transform, bHidden, bUnbound, Settings, etc.)
	TMap<FString, FLevelPropPtr> Properties;
};

/** Top-level parsed level data */
struct FLevelJsonData
{
	FString LevelName;   // e.g. "MainMenu_VFX_Tinari"
	FString GameContentRoot; // e.g. "ShooterGame/Content" — derived from ObjectPaths
	TArray<FLevelActorData> Actors;
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

class BLUEPRINTIMPORTER_API FLevelJsonParser
{
public:
	static bool Parse(const FString& JsonString, FLevelJsonData& OutData, FString& OutError);

private:
	// Convert a raw ObjectPath like "ShooterGame/Content/VFX/Foo.Bar.0" to "/Game/VFX/Foo"
	static FString ResolveAssetPath(const FString& ObjectPath, const FString& GameContentRoot);

	// Detect the game content root prefix from any ObjectPath in the data
	static FString DetectGameContentRoot(const TArray<TSharedPtr<FJsonValue>>& Root);

	// Parse a JSON value (any type) into our property tree
	static FLevelPropPtr ParseValue(const TSharedPtr<FJsonValue>& Val, const FString& Root);

	// Parse a JSON object as a property struct
	static FLevelPropPtr ParseObject(const TSharedPtr<FJsonObject>& Obj, const FString& Root);

	// Check whether a JSON object is an asset reference {ObjectName, ObjectPath}
	static bool IsObjectRef(const TSharedPtr<FJsonObject>& Obj, FString& OutPath, FString& OutName, FString& OutType);

	// Extract the actor Type name from an outer ObjectName string
	// e.g. "Emitter'Map:PersistentLevel.Ether_Tower_4'" -> "Emitter"
	static FString ExtractTypeFromOuter(const FString& OuterObjectName);

	// True when this object type should be collected as a top-level actor
	static bool IsLevelActor(const FString& Type);

	// True when this object type is a component that gets nested under its parent actor
	static bool IsComponent(const FString& Type);

	// Internal skip types (Model, BodySetup, Level, World, etc.)
	static bool ShouldSkip(const FString& Type);
};
