#include "LevelBuilder.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Engine/SkyLight.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/TextRenderActor.h"
#include "Engine/TriggerVolume.h"
#include "Engine/LevelBounds.h"
#include "Lightmass/LightmassImportanceVolume.h"
#include "GameFramework/KillZVolume.h"
#include "GameFramework/PhysicsVolume.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/BrushComponent.h"
#include "Particles/Emitter.h"
#include "Particles/ParticleSystem.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "LevelBuilder"

int32 FLevelBuilder::LastActorsSpawned = 0;
int32 FLevelBuilder::LastActorsSkipped = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static FVector PropToVector(const FLevelPropPtr& Prop)
{
	if (!Prop.IsValid() || Prop->Kind != FLevelPropertyValue::EKind::Struct)
		return FVector::ZeroVector;

	auto GetNum = [](const FLevelPropPtr& F) -> double
	{
		if (!F.IsValid()) return 0.0;
		if (F->Kind == FLevelPropertyValue::EKind::Float) return F->fValue;
		if (F->Kind == FLevelPropertyValue::EKind::Int)   return (double)F->iValue;
		return 0.0;
	};

	double X = 0, Y = 0, Z = 0;
	if (const FLevelPropPtr* F = Prop->Fields.Find(TEXT("X"))) X = GetNum(*F);
	if (const FLevelPropPtr* F = Prop->Fields.Find(TEXT("Y"))) Y = GetNum(*F);
	if (const FLevelPropPtr* F = Prop->Fields.Find(TEXT("Z"))) Z = GetNum(*F);
	return FVector(X, Y, Z);
}

static FRotator PropToRotator(const FLevelPropPtr& Prop)
{
	if (!Prop.IsValid() || Prop->Kind != FLevelPropertyValue::EKind::Struct)
		return FRotator::ZeroRotator;

	auto GetNum = [](const FLevelPropPtr& F) -> double
	{
		if (!F.IsValid()) return 0.0;
		if (F->Kind == FLevelPropertyValue::EKind::Float) return F->fValue;
		if (F->Kind == FLevelPropertyValue::EKind::Int)   return (double)F->iValue;
		return 0.0;
	};

	double Pitch = 0, Yaw = 0, Roll = 0;
	if (const FLevelPropPtr* F = Prop->Fields.Find(TEXT("Pitch"))) Pitch = GetNum(*F);
	if (const FLevelPropPtr* F = Prop->Fields.Find(TEXT("Yaw")))   Yaw   = GetNum(*F);
	if (const FLevelPropPtr* F = Prop->Fields.Find(TEXT("Roll")))  Roll  = GetNum(*F);
	return FRotator(Pitch, Yaw, Roll);
}

static FLinearColor PropToLinearColor(const FLevelPropPtr& Prop)
{
	if (!Prop.IsValid() || Prop->Kind != FLevelPropertyValue::EKind::Struct)
		return FLinearColor::White;

	auto GetNum = [](const FLevelPropPtr& F) -> float
	{
		if (!F.IsValid()) return 1.f;
		if (F->Kind == FLevelPropertyValue::EKind::Float) return (float)F->fValue;
		if (F->Kind == FLevelPropertyValue::EKind::Int)   return (float)F->iValue;
		return 1.f;
	};

	float R = 1, G = 1, B = 1, A = 1;
	if (const FLevelPropPtr* F = Prop->Fields.Find(TEXT("R"))) R = GetNum(*F);
	if (const FLevelPropPtr* F = Prop->Fields.Find(TEXT("G"))) G = GetNum(*F);
	if (const FLevelPropPtr* F = Prop->Fields.Find(TEXT("B"))) B = GetNum(*F);
	if (const FLevelPropPtr* F = Prop->Fields.Find(TEXT("A"))) A = GetNum(*F);
	return FLinearColor(R, G, B, A);
}

// ---------------------------------------------------------------------------
// Class resolution
// ---------------------------------------------------------------------------

UClass* FLevelBuilder::ResolveActorClass(const FString& Type, const FString& ClassPath)
{
	// --- Engine built-ins ---
	if (Type == TEXT("StaticMeshActor"))          return AStaticMeshActor::StaticClass();
	if (Type == TEXT("DirectionalLight"))          return ADirectionalLight::StaticClass();
	if (Type == TEXT("PointLight"))                return APointLight::StaticClass();
	if (Type == TEXT("SpotLight"))                 return ASpotLight::StaticClass();
	if (Type == TEXT("SkyLight"))                  return ASkyLight::StaticClass();
	if (Type == TEXT("Emitter"))                   return AEmitter::StaticClass();
	if (Type == TEXT("PostProcessVolume"))         return APostProcessVolume::StaticClass();
	if (Type == TEXT("ExponentialHeightFog"))      return AExponentialHeightFog::StaticClass();
	if (Type == TEXT("TextRenderActor"))           return ATextRenderActor::StaticClass();
	if (Type == TEXT("TriggerVolume"))             return ATriggerVolume::StaticClass();
	if (Type == TEXT("KillZVolume"))               return AKillZVolume::StaticClass();
	if (Type == TEXT("PhysicsVolume"))             return APhysicsVolume::StaticClass();
	if (Type == TEXT("LightmassImportanceVolume")) return ALightmassImportanceVolume::StaticClass();

	// --- Load Blueprint class from resolved asset path ---
	if (!ClassPath.IsEmpty() && ClassPath.StartsWith(TEXT("/Game/")))
	{
		UClass* C = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPath);
		if (C) return C;
	}

	// Memory search (handles game-specific volumes, BP actors, etc.)
	FString Clean = Type;
	Clean.RemoveFromEnd(TEXT("_C"));

	UClass* C = FindObject<UClass>(ANY_PACKAGE, *Clean);
	if (!C) C = FindObject<UClass>(ANY_PACKAGE, *FString::Printf(TEXT("A%s"), *Clean));
	if (!C) C = FindObject<UClass>(ANY_PACKAGE, *FString::Printf(TEXT("%s_C"), *Clean));

	// Safety: make sure FindObject didn't return a non-actor class
	// (e.g. UMaterialInstanceDynamic, UObject subclasses)
	if (C && !C->IsChildOf(AActor::StaticClass()))
		C = nullptr;

	return C;
}

// ---------------------------------------------------------------------------
// Property application — recursive, handles structs and object refs
// ---------------------------------------------------------------------------

static UObject* LoadAsset(const FLevelPropPtr& Val)
{
	if (!Val.IsValid() || !Val->IsObjectRef() || Val->AssetPath.IsEmpty())
		return nullptr;

	const FString& T = Val->AssetType;
	if (T == TEXT("StaticMesh"))
		return LoadObject<UStaticMesh>(nullptr, *Val->AssetPath);
	if (T == TEXT("SkeletalMesh"))
		return LoadObject<USkeletalMesh>(nullptr, *Val->AssetPath);
	if (T == TEXT("ParticleSystem"))
		return LoadObject<UParticleSystem>(nullptr, *Val->AssetPath);
	if (T.Contains(TEXT("Material")))
		return LoadObject<UMaterialInterface>(nullptr, *Val->AssetPath);

	return StaticLoadObject(UObject::StaticClass(), nullptr, *Val->AssetPath,
	                        nullptr, LOAD_NoWarn | LOAD_Quiet);
}

// Forward declaration
static void ApplyStructFields(void* StructData, UStruct* Struct,
                              const TMap<FString, FLevelPropPtr>& Fields);

static void ApplyValueToProperty(void* ContainerPtr, FProperty* Prop,
                                 const FLevelPropPtr& Val)
{
	// BUG FIX: null property values (e.g. "StaticMesh": null in JSON) were
	// parsed into FLevelPropertyValue with Kind::Null. When ApplyValueToProperty
	// tried to handle them as structs or object refs it crashed. Bail out early.
	if (!ContainerPtr || !Prop || !Val.IsValid()) return;
	if (Val->Kind == FLevelPropertyValue::EKind::Null) return;

	void* PropPtr = Prop->ContainerPtrToValuePtr<void>(ContainerPtr);

	// Bool
	if (FBoolProperty* BP = CastField<FBoolProperty>(Prop))
	{
		if (Val->Kind == FLevelPropertyValue::EKind::Bool)
			BP->SetPropertyValue(PropPtr, Val->bValue);
		return;
	}

	// Numerics
	if (FFloatProperty* FP = CastField<FFloatProperty>(Prop))
	{
		double V = (Val->Kind == FLevelPropertyValue::EKind::Float) ? Val->fValue :
		           (Val->Kind == FLevelPropertyValue::EKind::Int)   ? (double)Val->iValue : 0.0;
		FP->SetPropertyValue(PropPtr, (float)V);
		return;
	}
	if (FDoubleProperty* DP = CastField<FDoubleProperty>(Prop))
	{
		double V = (Val->Kind == FLevelPropertyValue::EKind::Float) ? Val->fValue :
		           (Val->Kind == FLevelPropertyValue::EKind::Int)   ? (double)Val->iValue : 0.0;
		DP->SetPropertyValue(PropPtr, V);
		return;
	}
	if (FIntProperty* IP = CastField<FIntProperty>(Prop))
	{
		int64 V = (Val->Kind == FLevelPropertyValue::EKind::Int)   ? Val->iValue :
		          (Val->Kind == FLevelPropertyValue::EKind::Float)  ? (int64)Val->fValue : 0;
		IP->SetPropertyValue(PropPtr, (int32)V);
		return;
	}

	// String / Name
	if (FStrProperty* SP = CastField<FStrProperty>(Prop))
	{
		if (Val->Kind == FLevelPropertyValue::EKind::String)
			SP->SetPropertyValue(PropPtr, Val->sValue);
		return;
	}
	if (FNameProperty* NP = CastField<FNameProperty>(Prop))
	{
		if (Val->Kind == FLevelPropertyValue::EKind::String)
			NP->SetPropertyValue(PropPtr, *Val->sValue);
		return;
	}

	// Enum
	if (FEnumProperty* EP = CastField<FEnumProperty>(Prop))
	{
		if (Val->Kind == FLevelPropertyValue::EKind::String && EP->GetEnum())
		{
			// Strip "EEnumName::" prefix if present
			FString EnumVal = Val->sValue;
			int32 ColonIdx;
			if (EnumVal.FindLastChar(':', ColonIdx))
				EnumVal = EnumVal.Mid(ColonIdx + 1);
			int64 V = EP->GetEnum()->GetValueByNameString(EnumVal);
			if (V != INDEX_NONE)
				EP->GetUnderlyingProperty()->SetIntPropertyValue(PropPtr, V);
		}
		return;
	}
	if (FByteProperty* ByteP = CastField<FByteProperty>(Prop))
	{
		if (ByteP->Enum && Val->Kind == FLevelPropertyValue::EKind::String)
		{
			FString EnumVal = Val->sValue;
			int32 ColonIdx;
			if (EnumVal.FindLastChar(':', ColonIdx))
				EnumVal = EnumVal.Mid(ColonIdx + 1);
			int64 V = ByteP->Enum->GetValueByNameString(EnumVal);
			if (V != INDEX_NONE) ByteP->SetPropertyValue(PropPtr, (uint8)V);
		}
		else if (Val->Kind == FLevelPropertyValue::EKind::Int)
			ByteP->SetPropertyValue(PropPtr, (uint8)Val->iValue);
		return;
	}

	// Object reference — skip silently if asset not found
	if (FObjectProperty* OP = CastField<FObjectProperty>(Prop))
	{
		if (Val->IsObjectRef())
		{
			UObject* Asset = LoadAsset(Val);
			if (Asset) OP->SetObjectPropertyValue(PropPtr, Asset);
			// else: skip — asset doesn't exist locally, don't error
		}
		return;
	}
	if (FSoftObjectProperty* SOP = CastField<FSoftObjectProperty>(Prop))
	{
		if (Val->IsObjectRef() && !Val->AssetPath.IsEmpty())
			SOP->SetPropertyValue(PropPtr, FSoftObjectPtr(FSoftObjectPath(Val->AssetPath)));
		return;
	}

	// Struct — recurse
	if (FStructProperty* StP = CastField<FStructProperty>(Prop))
	{
		if (Val->Kind != FLevelPropertyValue::EKind::Struct) return;

		FName SN = StP->Struct->GetFName();

		if (SN == NAME_Vector)
		{
			*static_cast<FVector*>(PropPtr) = PropToVector(Val);
		}
		else if (SN == NAME_Rotator)
		{
			*static_cast<FRotator*>(PropPtr) = PropToRotator(Val);
		}
		else if (SN == FName(TEXT("LinearColor")))
		{
			*static_cast<FLinearColor*>(PropPtr) = PropToLinearColor(Val);
		}
		else if (SN == FName(TEXT("Color")))
		{
			*static_cast<FColor*>(PropPtr) = PropToLinearColor(Val).ToFColor(true);
		}
		else if (SN == FName(TEXT("Vector4")) || SN == FName(TEXT("Plane")))
		{
			FVector V = PropToVector(Val);
			float W = 1.f;
			if (const FLevelPropPtr* WF = Val->Fields.Find(TEXT("W")))
				W = (WF->IsValid() && (*WF)->Kind == FLevelPropertyValue::EKind::Float)
				    ? (float)(*WF)->fValue : 1.f;
			*static_cast<FVector4*>(PropPtr) = FVector4(V.X, V.Y, V.Z, W);
		}
		else
		{
			// Generic recursive struct (e.g. FPostProcessSettings, FLightingChannels)
			ApplyStructFields(PropPtr, StP->Struct, Val->Fields);
		}
		return;
	}

	// Array
	if (FArrayProperty* AP = CastField<FArrayProperty>(Prop))
	{
		if (Val->Kind != FLevelPropertyValue::EKind::Array) return;
		FScriptArrayHelper Helper(AP, PropPtr);
		Helper.EmptyValues();
		for (const FLevelPropPtr& Elem : Val->Elements)
		{
			int32 Idx = Helper.AddValue();
			ApplyValueToProperty(Helper.GetRawPtr(Idx) - AP->Inner->GetOffset_ForInternal(),
			                     AP->Inner, Elem);
		}
		return;
	}
}

static void ApplyStructFields(void* StructData, UStruct* Struct,
                              const TMap<FString, FLevelPropPtr>& Fields)
{
	if (!StructData || !Struct) return;
	for (const TPair<FString, FLevelPropPtr>& KV : Fields)
	{
		FProperty* Prop = Struct->FindPropertyByName(*KV.Key);
		if (!Prop) continue;
		ApplyValueToProperty(StructData, Prop, KV.Value);
	}
}

void FLevelBuilder::ApplyProperties(UObject* Obj,
                                    const TMap<FString, FLevelPropPtr>& Properties,
                                    bool bIsComponent)
{
	if (!Obj || Obj->IsPendingKill()) return;
	UClass* Class = Obj->GetClass();

	for (const TPair<FString, FLevelPropPtr>& KV : Properties)
	{
		if (!KV.Value.IsValid()) continue;
		if (KV.Value->Kind == FLevelPropertyValue::EKind::Null) continue;
		// Transform handled separately via ApplyTransformFromBrush / spawn transform
		if (bIsComponent && (KV.Key == TEXT("RelativeLocation")
			|| KV.Key == TEXT("RelativeTranslation")
			|| KV.Key == TEXT("RelativeRotation")
			|| KV.Key == TEXT("RelativeScale3D")
			|| KV.Key == TEXT("RelativeScale")))
			continue;

		// Skip internal brush refs — we handle those explicitly
		if (KV.Key == TEXT("Brush") || KV.Key == TEXT("BrushComponent")
			|| KV.Key == TEXT("BrushBodySetup") || KV.Key == TEXT("RootComponent"))
			continue;

		FProperty* Prop = Class->FindPropertyByName(*KV.Key);
		if (!Prop)
		{
			for (TFieldIterator<FProperty> It(Class); It; ++It)
				if (It->GetFName() == *KV.Key) { Prop = *It; break; }
		}
		if (!Prop) continue;

		ApplyValueToProperty(Obj, Prop, KV.Value);
	}
}

// ---------------------------------------------------------------------------
// Component setup
// ---------------------------------------------------------------------------

void FLevelBuilder::ApplyComponents(AActor* Actor, const FLevelActorData& ActorData)
{
	if (!Actor || Actor->IsPendingKill()) return;

	TInlineComponentArray<UActorComponent*> ActorComps(Actor);

	for (const FLevelComponentData& CompData : ActorData.Components)
	{
		// Find existing component by name or type
		UActorComponent* TargetComp = nullptr;
		for (UActorComponent* C : ActorComps)
		{
			if (!C) continue;
			if (C->GetName() == CompData.Name) { TargetComp = C; break; }
		}
		if (!TargetComp)
		{
			for (UActorComponent* C : ActorComps)
			{
				if (!C) continue;
				if (C->GetClass()->GetName() == CompData.Type) { TargetComp = C; break; }
			}
		}
		if (!TargetComp) continue;

		// Never touch BrushComponent transform via SetRelativeLocationAndRotation —
		// it triggers PropagateTransformUpdate on a partially initialised brush actor
		// and causes an access violation. The actor spawn already placed it correctly;
		// we only need to set the scale (done in BuildLevel after spawn).
		bool bIsBrushComp = (CompData.Type == TEXT("BrushComponent")
		                     || CompData.Name == TEXT("BrushComponent0"));

		// Mesh references — applied before generic props
		if (UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(TargetComp))
		{
			if (const FLevelPropPtr* MP = CompData.Properties.Find(TEXT("StaticMesh")))
				if ((*MP)->IsObjectRef())
					if (UStaticMesh* M = Cast<UStaticMesh>(LoadAsset(*MP)))
						SMC->SetStaticMesh(M);
		}
		else if (USkeletalMeshComponent* SKMC = Cast<USkeletalMeshComponent>(TargetComp))
		{
			if (const FLevelPropPtr* MP = CompData.Properties.Find(TEXT("SkeletalMesh")))
				if ((*MP)->IsObjectRef())
					if (USkeletalMesh* M = Cast<USkeletalMesh>(LoadAsset(*MP)))
						SKMC->SetSkeletalMesh(M);
		}

		// Materials
		if (UMeshComponent* MC = Cast<UMeshComponent>(TargetComp))
		{
			if (const FLevelPropPtr* OMP = CompData.Properties.Find(TEXT("OverrideMaterials")))
				if ((*OMP)->Kind == FLevelPropertyValue::EKind::Array)
					for (int32 mi = 0; mi < (*OMP)->Elements.Num(); ++mi)
						if (UMaterialInterface* Mat = Cast<UMaterialInterface>(LoadAsset((*OMP)->Elements[mi])))
							MC->SetMaterial(mi, Mat);
		}

		// Apply transform — skip BrushComponent entirely to avoid crash
		if (!bIsBrushComp)
		{
			if (USceneComponent* SC = Cast<USceneComponent>(TargetComp))
			{
				if (SC && !SC->IsPendingKill() && SC->IsRegistered())
				{
					FVector Loc   = SC->GetRelativeLocation();
					FRotator Rot  = SC->GetRelativeRotation();
					FVector Scale = SC->GetRelativeScale3D();

					if (const FLevelPropPtr* P = CompData.Properties.Find(TEXT("RelativeLocation")))
						Loc = PropToVector(*P);
					if (const FLevelPropPtr* P = CompData.Properties.Find(TEXT("RelativeRotation")))
						Rot = PropToRotator(*P);
					if (const FLevelPropPtr* P = CompData.Properties.Find(TEXT("RelativeScale3D")))
						Scale = PropToVector(*P);
					else if (const FLevelPropPtr* P2 = CompData.Properties.Find(TEXT("RelativeScale")))
						Scale = PropToVector(*P2);

					// Use Teleport flag to skip physics/overlap checks during import
					SC->SetRelativeLocationAndRotation(Loc, Rot, false,
					                                  nullptr, ETeleportType::TeleportPhysics);
					SC->SetRelativeScale3D(Scale);
				}
			}
		}

		// Generic properties (skip transform keys for all components)
		ApplyProperties(TargetComp, CompData.Properties, /*bIsComponent=*/true);
	}
}

// ---------------------------------------------------------------------------
// Main build
// ---------------------------------------------------------------------------

void FLevelBuilder::BuildLevel(UWorld* World, ULevel* Level, const FLevelJsonData& LevelData)
{
	if (!World || !Level) return;

	GEditor->BeginTransaction(LOCTEXT("ImportLevelJsonTrans", "Import Level Layout From JSON"));

	LastActorsSpawned = 0;
	LastActorsSkipped = 0;

	for (const FLevelActorData& ActorData : LevelData.Actors)
	{
		// --- Resolve class, skip if unknown or not an actor ---
		UClass* ActorClass = ResolveActorClass(ActorData.Type, ActorData.ClassName);
		if (!ActorClass)
		{
			UE_LOG(LogTemp, Verbose,
				TEXT("LevelBuilder: skipping '%s' — unknown class '%s'"),
				*ActorData.Name, *ActorData.Type);
			++LastActorsSkipped;
			continue;
		}
		// Critical: ensure the class is actually an AActor subclass.
		// MaterialInstanceDynamic, UObject, etc. would crash SpawnActor.
		if (!ActorClass->IsChildOf(AActor::StaticClass()))
		{
			UE_LOG(LogTemp, Verbose,
				TEXT("LevelBuilder: skipping '%s' — class '%s' is not an AActor"),
				*ActorData.Name, *ActorClass->GetName());
			++LastActorsSkipped;
			continue;
		}

		// --- Determine spawn transform from BrushComponent (volumes) or root component ---
		FVector Loc   = FVector::ZeroVector;
		FRotator Rot  = FRotator::ZeroRotator;
		FVector Scale = FVector::OneVector;

		// For brush-based actors (volumes), transform lives in the BrushComponent
		bool bFoundTransform = false;
		for (const FLevelComponentData& Comp : ActorData.Components)
		{
			if (Comp.Type == TEXT("BrushComponent") || Comp.Name == TEXT("BrushComponent0"))
			{
				if (const FLevelPropPtr* P = Comp.Properties.Find(TEXT("RelativeLocation")))
					{ Loc   = PropToVector(*P);   bFoundTransform = true; }
				if (const FLevelPropPtr* P = Comp.Properties.Find(TEXT("RelativeRotation")))
					Rot   = PropToRotator(*P);
				if (const FLevelPropPtr* P = Comp.Properties.Find(TEXT("RelativeScale3D")))
					Scale = PropToVector(*P);
				else if (const FLevelPropPtr* P2 = Comp.Properties.Find(TEXT("RelativeScale")))
					Scale = PropToVector(*P2);
				break;
			}
		}

		// Fallback: root scene component transform
		if (!bFoundTransform)
		{
			for (const FLevelComponentData& Comp : ActorData.Components)
			{
				if (!Comp.Type.IsEmpty() && Comp.Type.EndsWith(TEXT("Component")))
				{
					if (const FLevelPropPtr* P = Comp.Properties.Find(TEXT("RelativeLocation")))
						Loc = PropToVector(*P);
					if (const FLevelPropPtr* P = Comp.Properties.Find(TEXT("RelativeRotation")))
						Rot = PropToRotator(*P);
					if (const FLevelPropPtr* P = Comp.Properties.Find(TEXT("RelativeScale3D")))
						Scale = PropToVector(*P);
					else if (const FLevelPropPtr* P2 = Comp.Properties.Find(TEXT("RelativeScale")))
						Scale = PropToVector(*P2);
					break;
				}
			}
		}

		FTransform SpawnTransform(Rot.Quaternion(), Loc, Scale);

		// --- Unique name collision avoidance ---
		FString SafeName = ActorData.Name;
		if (FindObject<AActor>(Level, *SafeName))
		{
			int32 N = 1;
			while (FindObject<AActor>(Level, *FString::Printf(TEXT("%s_%d"), *ActorData.Name, N))) ++N;
			SafeName = FString::Printf(TEXT("%s_%d"), *ActorData.Name, N);
		}

		// --- Spawn ---
		FActorSpawnParameters SP;
		SP.Name                          = *SafeName;
		SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SP.bNoFail                        = true;
		SP.OverrideLevel                  = Level;

		AActor* Actor = World->SpawnActor<AActor>(ActorClass, SpawnTransform, SP);
		if (!Actor)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("LevelBuilder: SpawnActor failed for '%s'"), *SafeName);
			++LastActorsSkipped;
			continue;
		}

		// --- Apply actor-level properties (bUnbound, Settings, BlendRadius, etc.) ---
		ApplyProperties(Actor, ActorData.Properties, /*bIsComponent=*/false);

		// --- Apply components (mesh refs, particle templates, materials, sub-transforms) ---
		ApplyComponents(Actor, ActorData);

		// --- For volumes: set the BrushComponent scale so the box is the right size ---
		// The BrushComponent's RelativeScale3D is what defines the volume's extents.
		// SpawnActor already positioned it but scale on the brush needs explicit set.
		if (ABrush* Brush = Cast<ABrush>(Actor))
		{
			for (const FLevelComponentData& Comp : ActorData.Components)
			{
				if (Comp.Type == TEXT("BrushComponent") || Comp.Name == TEXT("BrushComponent0"))
				{
					if (UBrushComponent* BC = Brush->GetBrushComponent())
					{
						if (const FLevelPropPtr* P = Comp.Properties.Find(TEXT("RelativeScale3D")))
							BC->SetRelativeScale3D(PropToVector(*P));
						else if (const FLevelPropPtr* P2 = Comp.Properties.Find(TEXT("RelativeScale")))
							BC->SetRelativeScale3D(PropToVector(*P2));
					}
					break;
				}
			}
		}

		Actor->MarkPackageDirty();
		Actor->PostEditChange();
		++LastActorsSpawned;
	}

	GEditor->EndTransaction();
	GEditor->RedrawLevelEditingViewports();
}

#undef LOCTEXT_NAMESPACE
