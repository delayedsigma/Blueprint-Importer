#include "LevelJsonParser.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

FString FLevelJsonParser::DetectGameContentRoot(const TArray<TSharedPtr<FJsonValue>>& Root)
{
	for (const TSharedPtr<FJsonValue>& V : Root)
	{
		if (!V.IsValid() || V->Type != EJson::Object) continue;
		const TSharedPtr<FJsonObject>& Obj = V->AsObject();
		const TSharedPtr<FJsonObject>* OuterObj = nullptr;
		if (Obj->TryGetObjectField(TEXT("Outer"), OuterObj))
		{
			FString Path;
			if ((*OuterObj)->TryGetStringField(TEXT("ObjectPath"), Path))
			{
				int32 Idx = Path.Find(TEXT("/Content/"));
				if (Idx != INDEX_NONE)
					return Path.Left(Idx + 8);
			}
		}
	}
	return TEXT("ShooterGame/Content");
}

FString FLevelJsonParser::ResolveAssetPath(const FString& ObjectPath, const FString& GameContentRoot)
{
	if (ObjectPath.IsEmpty()) return TEXT("");
	FString Path = ObjectPath;

	// Strip trailing ".N" numeric index
	int32 LastDot;
	if (Path.FindLastChar('.', LastDot))
	{
		FString Suffix = Path.Mid(LastDot + 1);
		bool bIsIndex = !Suffix.IsEmpty();
		for (TCHAR C : Suffix) { if (!FChar::IsDigit(C)) { bIsIndex = false; break; } }
		if (bIsIndex) Path = Path.Left(LastDot);
	}

	if (!GameContentRoot.IsEmpty() && Path.StartsWith(GameContentRoot))
		Path = TEXT("/Game") + Path.Mid(GameContentRoot.Len());

	if (!Path.StartsWith(TEXT("/")))
		Path = TEXT("/Game/") + Path;

	return Path;
}

bool FLevelJsonParser::IsObjectRef(const TSharedPtr<FJsonObject>& Obj,
                                   FString& OutPath, FString& OutName, FString& OutType)
{
	FString ObjName;
	if (!Obj->TryGetStringField(TEXT("ObjectName"), ObjName)) return false;

	OutName = ObjName;
	OutPath = TEXT("");
	OutType = TEXT("");

	int32 Quote;
	if (ObjName.FindChar('\'', Quote))
	{
		OutType = ObjName.Left(Quote);
		FString Inner = ObjName.Mid(Quote + 1);
		Inner.RemoveFromEnd(TEXT("'"));
		int32 LastDot;
		OutName = Inner.FindLastChar('.', LastDot) ? Inner.Mid(LastDot + 1) : Inner;
	}

	FString ObjPath;
	if (Obj->TryGetStringField(TEXT("ObjectPath"), ObjPath) && !ObjPath.IsEmpty())
		OutPath = ObjPath;

	return true;
}

FString FLevelJsonParser::ExtractTypeFromOuter(const FString& OuterObjectName)
{
	int32 Quote;
	if (OuterObjectName.FindChar('\'', Quote))
		return OuterObjectName.Left(Quote);
	return TEXT("");
}

bool FLevelJsonParser::IsLevelActor(const FString& Type)
{
	// Must be a direct actor class — not a component, not a data object
	if (Type.IsEmpty() || !FChar::IsUpper(Type[0])) return false;
	if (IsComponent(Type))  return false;
	if (ShouldSkip(Type))   return false;

	// Explicitly skip non-actor UObject subclasses that sneak through
	static const TSet<FString> NonActors = {
		TEXT("MaterialInstanceDynamic"), TEXT("MaterialInstanceConstant"),
		TEXT("Material"), TEXT("StaticMesh"), TEXT("SkeletalMesh"),
		TEXT("Texture2D"), TEXT("ParticleSystem"), TEXT("SoundBase"),
		TEXT("SoundCue"), TEXT("SoundWave"), TEXT("CurveFloat"),
		TEXT("DataTable"), TEXT("UObject"), TEXT("BlueprintGeneratedClass"),
		TEXT("AnimBlueprintGeneratedClass"), TEXT("AnimSequence"),
		TEXT("PhysicsAsset"), TEXT("Skeleton"),
	};
	if (NonActors.Contains(Type)) return false;

	return true;
}

bool FLevelJsonParser::IsComponent(const FString& Type)
{
	return Type.EndsWith(TEXT("Component"));
}

bool FLevelJsonParser::ShouldSkip(const FString& Type)
{
	static const TSet<FString> SkipTypes = {
		TEXT("Model"), TEXT("BodySetup"), TEXT("Level"), TEXT("World"),
		TEXT("BlueprintGeneratedClass"), TEXT("AnimBlueprintGeneratedClass"),
		TEXT("Function"), TEXT("UmbraPrecomputedData"), TEXT("Package"),
		TEXT("SimpleConstructionScript"), TEXT("SCS_Node"), TEXT("Polys"),
	};
	return SkipTypes.Contains(Type);
}

// ---------------------------------------------------------------------------
// Property parsing — handles null safely everywhere
// ---------------------------------------------------------------------------

FLevelPropPtr FLevelJsonParser::ParseValue(const TSharedPtr<FJsonValue>& Val,
                                           const FString& Root)
{
	auto Null = []() -> FLevelPropPtr
	{
		auto P = MakeShared<FLevelPropertyValue>();
		P->Kind = FLevelPropertyValue::EKind::Null;
		return P;
	};

	if (!Val.IsValid() || Val->Type == EJson::Null) return Null();

	switch (Val->Type)
	{
	case EJson::Boolean:
	{
		auto P = MakeShared<FLevelPropertyValue>();
		P->Kind   = FLevelPropertyValue::EKind::Bool;
		P->bValue = Val->AsBool();
		return P;
	}
	case EJson::Number:
	{
		auto P = MakeShared<FLevelPropertyValue>();
		double D = Val->AsNumber();
		if (D == FMath::FloorToDouble(D) && FMath::Abs(D) < 2147483648.0)
		{
			P->Kind   = FLevelPropertyValue::EKind::Int;
			P->iValue = (int64)D;
		}
		else
		{
			P->Kind   = FLevelPropertyValue::EKind::Float;
			P->fValue = D;
		}
		return P;
	}
	case EJson::String:
	{
		auto P = MakeShared<FLevelPropertyValue>();
		P->Kind   = FLevelPropertyValue::EKind::String;
		P->sValue = Val->AsString();
		return P;
	}
	case EJson::Array:
	{
		auto P = MakeShared<FLevelPropertyValue>();
		P->Kind = FLevelPropertyValue::EKind::Array;
		for (const TSharedPtr<FJsonValue>& Elem : Val->AsArray())
		{
			// BUG FIX 1: null elements inside arrays (e.g. "InstanceComponents": [null])
			// were being added as FLevelPropPtr with Kind::Null and then crashed
			// in ApplyValueToProperty when treated as object refs.
			// Skip null array elements entirely.
			if (!Elem.IsValid() || Elem->Type == EJson::Null) continue;
			P->Elements.Add(ParseValue(Elem, Root));
		}
		return P;
	}
	case EJson::Object:
		return ParseObject(Val->AsObject(), Root);
	default:
		return Null();
	}
}

FLevelPropPtr FLevelJsonParser::ParseObject(const TSharedPtr<FJsonObject>& Obj,
                                            const FString& Root)
{
	auto P = MakeShared<FLevelPropertyValue>();

	// Object reference check
	FString AssetPath, AssetName, AssetType;
	if (IsObjectRef(Obj, AssetPath, AssetName, AssetType))
	{
		P->Kind      = FLevelPropertyValue::EKind::Struct;
		P->AssetType = AssetType;
		P->AssetName = AssetName;
		P->AssetPath = ResolveAssetPath(AssetPath, Root);

		for (const TPair<FString, TSharedPtr<FJsonValue>>& KV : Obj->Values)
		{
			if (KV.Key == TEXT("ObjectName") || KV.Key == TEXT("ObjectPath")) continue;
			// BUG FIX 2: null values inside object refs also need guarding
			if (!KV.Value.IsValid() || KV.Value->Type == EJson::Null) continue;
			P->Fields.Add(KV.Key, ParseValue(KV.Value, Root));
		}
		return P;
	}

	FString AssetPathName;
	if (Obj->TryGetStringField(TEXT("AssetPathName"), AssetPathName))
	{
		P->Kind      = FLevelPropertyValue::EKind::Struct;
		P->AssetPath = AssetPathName;
		int32 LastDot;
		P->AssetName = AssetPathName.FindLastChar('.', LastDot)
		               ? AssetPathName.Mid(LastDot + 1) : AssetPathName;
		return P;
	}

	P->Kind = FLevelPropertyValue::EKind::Struct;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& KV : Obj->Values)
	{
		if (!KV.Value.IsValid() || KV.Value->Type == EJson::Null) continue;
		P->Fields.Add(KV.Key, ParseValue(KV.Value, Root));
	}
	return P;
}

// ---------------------------------------------------------------------------
// Main Parse
// ---------------------------------------------------------------------------

bool FLevelJsonParser::Parse(const FString& JsonString, FLevelJsonData& OutData,
                             FString& OutError)
{
	TArray<TSharedPtr<FJsonValue>> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, Root) || Root.Num() == 0)
	{
		OutError = TEXT("Failed to parse JSON: invalid or empty array.");
		return false;
	}

	const FString GameRoot = DetectGameContentRoot(Root);
	OutData.GameContentRoot = GameRoot;

	struct FRawEntry
	{
		FString Type;
		FString Name;
		FString OuterActorName;  // name of the ACTOR this object belongs to
		FString OuterActorType;  // type token before ' in outer ObjectName
		bool    bOuterIsActor;   // true if Outer is directly under PersistentLevel
		TSharedPtr<FJsonObject> Obj;
	};

	TArray<FRawEntry> Entries;
	TMap<FString, int32> ActorIndexByName;

	for (const TSharedPtr<FJsonValue>& V : Root)
	{
		if (!V.IsValid() || V->Type != EJson::Object) continue;
		const TSharedPtr<FJsonObject>& Obj = V->AsObject();

		FString Type, Name;
		if (!Obj->TryGetStringField(TEXT("Type"), Type)) continue;
		if (!Obj->TryGetStringField(TEXT("Name"), Name)) continue;
		if (ShouldSkip(Type)) continue;
		if (Name.StartsWith(TEXT("Default__"))) continue;

		FRawEntry Entry;
		Entry.Type          = Type;
		Entry.Name          = Name;
		Entry.Obj           = Obj;
		Entry.bOuterIsActor = false;

		const TSharedPtr<FJsonObject>* OuterObjPtr = nullptr;
		if (Obj->TryGetObjectField(TEXT("Outer"), OuterObjPtr))
		{
			FString OuterObjName;
			(*OuterObjPtr)->TryGetStringField(TEXT("ObjectName"), OuterObjName);
			Entry.OuterActorType = ExtractTypeFromOuter(OuterObjName);

			// Two outer formats exist:
			//
			// Format A — direct level actor:
			//   "Level'MapName:PersistentLevel'"
			//   No dot after PersistentLevel. The object IS the actor.
			//   bOuterIsActor = true, OuterActorName = ""
			//
			// Format B — component/sub-object of an actor:
			//   "ActorType'MapName:PersistentLevel.ActorName'"       -> direct component
			//   "ActorType'MapName:PersistentLevel.ActorName.Comp'"  -> sub-component
			//   bOuterIsActor = false, OuterActorName = "ActorName"

			// Check for ":PersistentLevel'" (Format A — ends right after PersistentLevel)
			if (OuterObjName.Contains(TEXT(":PersistentLevel'"))
				|| OuterObjName.Contains(TEXT(":PersistentLevel\""))
				|| OuterObjName.EndsWith(TEXT(":PersistentLevel")))
			{
				// Direct child of the level — this IS a top-level actor
				Entry.OuterActorName = TEXT("");
				Entry.bOuterIsActor  = (Entry.OuterActorType == TEXT("Level"));
			}
			else
			{
				// Format B — find actor name after "PersistentLevel."
				int32 PL = OuterObjName.Find(TEXT("PersistentLevel."));
				if (PL != INDEX_NONE)
				{
					FString AfterPL = OuterObjName.Mid(PL + 16);

					// Strip trailing quote
					int32 Q;
					if (AfterPL.FindChar('\'', Q)) AfterPL = AfterPL.Left(Q);
					if (AfterPL.FindChar('"',  Q)) AfterPL = AfterPL.Left(Q);

					// Only take the first segment — second dot means sub-component
					int32 FirstDot;
					if (AfterPL.FindChar('.', FirstDot))
					{
						Entry.OuterActorName = AfterPL.Left(FirstDot);
						Entry.bOuterIsActor  = false;
					}
					else
					{
						Entry.OuterActorName = AfterPL;
						Entry.bOuterIsActor  = false; // component of the named actor
					}
				}
			}
		}

		Entries.Add(Entry);
	}

	// Detect level name
	for (const FRawEntry& E : Entries)
	{
		if (E.Type == TEXT("World") || E.Type == TEXT("Level"))
		{
			OutData.LevelName = E.Name;
			break;
		}
	}

	// Pass 1 — collect top-level actors
	// An object is a top-level actor if:
	//   OuterActorType == "Level"  (direct child of PersistentLevel)
	//   AND its Type is a valid actor type (not a component, not a pure data object)
	//
	// BUG FIX 4: GameMode_Color.json has actors nested under other actors
	// (Outer = "ColorPlatform_C'Map:PersistentLevel.ColorPlatform_5'") rather
	// than under Level directly. The old code accepted OuterActorType.IsEmpty()
	// as a level child, which caused BP class children and sub-objects to be
	// collected as top-level actors. Now we require OuterActorType == "Level".
	for (const FRawEntry& E : Entries)
	{
		if (!E.bOuterIsActor) continue;
		if (!IsLevelActor(E.Type)) continue;
		if (E.Name.StartsWith(TEXT("Default__"))) continue;
		if (E.OuterActorName.StartsWith(TEXT("Default__"))) continue;

		// Resolve class path
		FString ClassName;
		if (E.Type.EndsWith(TEXT("_C")))
		{
			// Blueprint actor — try to get path from Template field
			const TSharedPtr<FJsonObject>* TplPtr = nullptr;
			if (E.Obj->TryGetObjectField(TEXT("Template"), TplPtr))
			{
				FString TplPath;
				if ((*TplPtr)->TryGetStringField(TEXT("ObjectPath"), TplPath))
				{
					int32 DotIdx;
					FString Base = TplPath.FindLastChar('.', DotIdx)
					               ? TplPath.Left(DotIdx) : TplPath;
					Base = ResolveAssetPath(Base, GameRoot);
					ClassName = FString::Printf(TEXT("%s.%s_C"), *Base, *E.Type);
				}
			}
			if (ClassName.IsEmpty())
				ClassName = E.Type; // fallback — ResolveActorClass will try FindObject
		}
		else
		{
			ClassName = FString::Printf(TEXT("/Script/Engine.%s"), *E.Type);
		}

		FLevelActorData Actor;
		Actor.Type      = E.Type;
		Actor.Name      = E.Name;
		Actor.ClassName = ClassName;

		const TSharedPtr<FJsonObject>* PropsObj = nullptr;
		if (E.Obj->TryGetObjectField(TEXT("Properties"), PropsObj))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& KV : (*PropsObj)->Values)
			{
				if (!KV.Value.IsValid() || KV.Value->Type == EJson::Null) continue;
				if (KV.Key == TEXT("RootComponent") && KV.Value->Type == EJson::Object)
				{
					// Inline transform from RootComponent
					const TSharedPtr<FJsonObject>& RC = KV.Value->AsObject();
					const TSharedPtr<FJsonObject>* Inner = nullptr;
					if (RC->TryGetObjectField(TEXT("Properties"), Inner))
						for (const TPair<FString, TSharedPtr<FJsonValue>>& TKV : (*Inner)->Values)
							if (TKV.Value.IsValid() && TKV.Value->Type != EJson::Null)
								if (TKV.Key.Contains(TEXT("Location"))
									|| TKV.Key.Contains(TEXT("Rotation"))
									|| TKV.Key.Contains(TEXT("Scale")))
									Actor.Properties.Add(TKV.Key, ParseValue(TKV.Value, GameRoot));
					continue;
				}
				Actor.Properties.Add(KV.Key, ParseValue(KV.Value, GameRoot));
			}
		}

		ActorIndexByName.Add(E.Name, OutData.Actors.Num());
		OutData.Actors.Add(MoveTemp(Actor));
	}

	// Pass 2 — attach components to their actor
	// Only attach if OuterActorName resolves to a known actor.
	// Sub-objects of components (MaterialInstanceDynamic etc.) are silently dropped.
	for (const FRawEntry& E : Entries)
	{
		if (!IsComponent(E.Type))         continue;
		if (E.OuterActorName.IsEmpty())   continue;
		if (E.OuterActorName.StartsWith(TEXT("Default__"))) continue;

		// BUG FIX 3 (cont): MaterialInstanceDynamic entries have outer like
		// "PersistentLevel.ActorName.StaticMeshComponent0" — OuterActorName is
		// "ActorName" but IsComponent("MaterialInstanceDynamic") returns false
		// so they never reach here. However StaticMeshComponent whose outer is
		// "PersistentLevel.ActorName.SomeOtherComp" would have OuterActorName =
		// "ActorName" (correctly extracted) and would safely attach.
		// The key safety: ActorIndexByName.Find returns nullptr for anything
		// that didn't get registered as an actor, so we skip silently.
		int32* ActorIdx = ActorIndexByName.Find(E.OuterActorName);
		if (!ActorIdx) continue;

		FLevelActorData& ActorRef = OutData.Actors[*ActorIdx];

		FLevelComponentData Comp;
		Comp.Type = E.Type;
		Comp.Name = E.Name;

		const TSharedPtr<FJsonObject>* PropsObj = nullptr;
		if (E.Obj->TryGetObjectField(TEXT("Properties"), PropsObj))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& KV : (*PropsObj)->Values)
			{
				if (!KV.Value.IsValid() || KV.Value->Type == EJson::Null) continue;
				if (KV.Key == TEXT("Brush") || KV.Key == TEXT("BrushBodySetup")) continue;
				Comp.Properties.Add(KV.Key, ParseValue(KV.Value, GameRoot));
			}
		}

		// Promote transform to actor properties for spawn positioning
		for (const TPair<FString, FLevelPropPtr>& CP : Comp.Properties)
			if ((CP.Key.Contains(TEXT("Location"))
			     || CP.Key.Contains(TEXT("Rotation"))
			     || CP.Key.Contains(TEXT("Scale")))
			    && !ActorRef.Properties.Contains(CP.Key))
				ActorRef.Properties.Add(CP.Key, CP.Value);

		ActorRef.Components.Add(MoveTemp(Comp));
	}

	if (OutData.Actors.Num() == 0)
	{
		OutError = TEXT("No level actors found. Make sure this is a level JSON export "
		                "(not a Blueprint class export). Actors must be direct children "
		                "of PersistentLevel.");
		return false;
	}

	return true;
}
