/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Rig/ControlRigImporter.h"
#include "Importers/Types/Blueprint/BlueprintCompile.h"

#if REFLECTION_CONTROL_RIG

#include "ControlRig.h"
#include "Engine/ControlRigCompatibility.h"
#include "Rigs/RigHierarchyController.h"
#if UE5_4_BEYOND
#include "Rigs/RigHierarchyMetadata.h"
#endif

#include "Engine/SkeletalMesh.h"
#include "Kismet2/KismetEditorUtilities.h"

#include "Importers/Types/Blueprint/BlueprintUtilities.h"
#include "Importers/Types/Blueprint/BlueprintVariables.h"
#include "Importers/Types/Rig/RigVMGraph.h"
#include "Utilities/AssetPaths.h"

namespace {
	ERigElementType ToElementType(const FUObjectJsonValueExport& Key) {
		if (!Key.JsonObject.IsValid()) return ERigElementType::None;

		const TSharedPtr<FJsonValue> Type = Key.JsonObject->TryGetField(TEXT("Type"));
		if (!Type.IsValid()) return ERigElementType::None;

		const UEnum* Enum = StaticEnum<ERigElementType>();
		int64 Value = INDEX_NONE;
		if (Type->Type == EJson::String) {
			FString Name = Type->AsString();
			Name.Split(TEXT("::"), nullptr, &Name);
			if (Name == TEXT("Space")) Name = TEXT("Null");
			Value = Enum->GetValueByNameString(Name);
		} else if (Type->Type == EJson::Number) {
			Value = static_cast<int64>(Type->AsNumber());
		}

		return Enum->IsValidEnumValue(Value) ? static_cast<ERigElementType>(Value) : ERigElementType::None;
	}

	FRigElementKey ReadElementKey(const FUObjectJsonValueExport& Key) {
		if (!Key.JsonObject.IsValid() || !Key.Has(TEXT("Name"))) {
			return FRigElementKey();
		}

		const FString Name = Key.GetString(TEXT("Name"));
		const ERigElementType Type = ToElementType(Key);

		if (Name.IsEmpty() || Name == TEXT("None") || Type == ERigElementType::None) {
			return FRigElementKey();
		}

		return FRigElementKey(StringToName(Name), Type);
	}

	double ReadNumber(const FUObjectJsonValueExport& Object, const FString& Field) {
		return Object.Has(Field) ? Object.GetNumber(Field) : 0.0;
	}

	/* One transform out of an element's transform storage.
	 *
	 * Only the initial local one is read. A cooked rig's current pose is wherever the rig happened
	 * to be left, and both global poses are caches the hierarchy fills in as it evaluates: they
	 * come through zeroed, quaternion included, which is not a rotation. */
	bool ReadInitialLocalTransform(const FUObjectJsonValueExport& Storage, FTransform& OutTransform) {
		const FUObjectJsonValueExport Transform = Storage.GetObject(TEXT("Initial")).GetObject(TEXT("Local")).GetObject(TEXT("Transform"));

		if (!Transform.JsonObject.IsValid() || !Transform.Has(TEXT("Rotation"))) {
			return false;
		}

		const FUObjectJsonValueExport Rotation = Transform.GetObject(TEXT("Rotation"));
		const FUObjectJsonValueExport Translation = Transform.GetObject(TEXT("Translation"));
		const FUObjectJsonValueExport Scale = Transform.GetObject(TEXT("Scale3D"));

		const FQuat Quaternion(
			ReadNumber(Rotation, TEXT("X")),
			ReadNumber(Rotation, TEXT("Y")),
			ReadNumber(Rotation, TEXT("Z")),
			ReadNumber(Rotation, TEXT("W"))
		);

		/* Storage the rig never wrote to, which the identity stands in for */
		if (!Quaternion.IsNormalized()) {
			return false;
		}

		OutTransform = FTransform(
			Quaternion,
			FVector(ReadNumber(Translation, TEXT("X")), ReadNumber(Translation, TEXT("Y")), ReadNumber(Translation, TEXT("Z"))),
			FVector(ReadNumber(Scale, TEXT("X")), ReadNumber(Scale, TEXT("Y")), ReadNumber(Scale, TEXT("Z")))
		);

		return true;
	}

	/* A control's limits are written as the storage they sit in rather than through the value that
	 * holds it, so the object handed in here is that storage rather than an FRigControlValue */
	void ReadControlValue(UPropertySerializer* PropertySerializer, const FUObjectJsonValueExport& Json, FRigControlValue& OutValue) {
		if (!Json.JsonObject.IsValid() || !Json.Has(TEXT("Float00"))) {
			return;
		}

		PropertySerializer->DeserializeStruct(
			FRigControlValueStorage::StaticStruct(),
			Json.JsonObject.ToSharedRef(),
			&OutValue.GetRef<FRigControlValueStorage>()
		);
	}

	FRigElementWeight ReadWeight(const FUObjectJsonValueExport& Weight) {
		return Weight.JsonObject.IsValid()
			? FRigElementWeight(ReadNumber(Weight, TEXT("Location")), ReadNumber(Weight, TEXT("Rotation")), ReadNumber(Weight, TEXT("Scale")))
			: FRigElementWeight();
	}

#if UE5_4_BEYOND
	/* SetupValidMetadata is protected on some engines and public on others, and a derived type
	 * naming it through a member pointer is allowed to reach it on both. */
	struct FRigMetadataAccess : FRigBaseElement {
		static FRigBaseMetadata* Setup(FRigBaseElement* Element, const FName& Name, ERigMetadataType Type) {
			return (Element->*(&FRigMetadataAccess::SetupValidMetadata))(Name, Type);
		}
	};

	void RestoreHierarchyMetadata(URigHierarchy* Hierarchy, UPropertySerializer* Serializer,
		const FUObjectJsonValueExport& Export) {
		if (!Export.Has(TEXT("LoadedElementMetadata"))) return;

		const UEnum* Types = StaticEnum<ERigMetadataType>();
		int32 Missing = 0;
		for (const FUObjectJsonValueExport& Fields : Export.GetArray(TEXT("LoadedElementMetadata"))) {
			if (!Fields.JsonObject.IsValid()) continue;
			FRigBaseElement* Element = Hierarchy->Find(ReadElementKey(Fields.GetObject(TEXT("Key"))));
			const FUObjectJsonValueExport Data = Fields.GetObject(TEXT("Value")).GetObject(TEXT("MetadataMap"));
			if (!Data.JsonObject.IsValid()) continue;

			for (const auto& Pair : Data.JsonObject->Values) {
				if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object) continue;
				const FUObjectJsonValueExport Field(Pair.Value->AsObject());
				const TSharedPtr<FJsonValue> Type = Field.JsonObject->TryGetField(TEXT("Type"));
				const TSharedPtr<FJsonValue> Value = Field.JsonObject->TryGetField(TEXT("Value"));
				const int64 Number = Type.IsValid() && Type->Type == EJson::String
					? Types->GetValueByNameString(Type->AsString()) : Field.GetInteger(TEXT("Type"), INDEX_NONE);
				if (Element == nullptr || !Value.IsValid() || !Types->IsValidEnumValue(Number)
					|| Number == static_cast<int64>(ERigMetadataType::Invalid)) {
					Missing++;
					continue;
				}

				FRigBaseMetadata* Metadata = FRigMetadataAccess::Setup(Element, FName(*Pair.Key), static_cast<ERigMetadataType>(Number));
				UScriptStruct* Struct = Metadata != nullptr ? Metadata->GetMetadataStruct() : nullptr;
				FProperty* Property = Struct != nullptr ? Struct->FindPropertyByName(TEXT("Value")) : nullptr;
				if (Property == nullptr) {
					Missing++;
					continue;
				}

				Serializer->DeserializePropertyValue(Property, Value.ToSharedRef(),
					Property->ContainerPtrToValuePtr<void>(Metadata));
			}
		}
		if (Missing > 0) {
			FImportIssues::Report(EImportIssue::Data, TEXT("Some Control Rig hierarchy metadata could not be restored"),
				FString::Printf(TEXT("%d metadata value(s)"), Missing));
		}
	}
#endif

	struct FRigParentDescription {
		FRigElementKey Key;
		FRigElementWeight InitialWeight;
		FRigElementWeight Weight;
	};

	void AddLegacyMemory(const FUObjectExport* Class, FUObjectExportContainer* Container, const TSharedPtr<FJsonObject>& VM) {
		const FUObjectJsonValueExport Fields = Class->AsValueExport();
		const int32 MemoryType = Fields.GetInteger(TEXT("MemoryType"), INDEX_NONE);
		const TCHAR* Field = MemoryType == 0 ? TEXT("DefaultWorkMemoryStorage")
			: MemoryType == 1 ? TEXT("LiteralMemoryStorage") : nullptr;
		if (Field == nullptr || VM->HasField(Field)) return;

		const TArray<TSharedPtr<FJsonValue>>* Declared = nullptr;
		if (!Fields.JsonObject->TryGetArrayField(TEXT("ChildProperties"), Declared)) return;

		const FUObjectExport* Defaults = Container->GetExportByObjectPath(Fields.GetObject(TEXT("ClassDefaultObject")));
		const TSharedPtr<FJsonObject> Values = Defaults->IsJsonValid() ? Defaults->GetProperties() : nullptr;
		TArray<TSharedPtr<FJsonValue>> Registers;
		for (const TSharedPtr<FJsonValue>& Value : *Declared) {
			const TSharedPtr<FJsonObject>* Property = nullptr;
			FString Name;
			if (!Value.IsValid() || !Value->TryGetObject(Property) || !(*Property)->TryGetStringField(TEXT("Name"), Name)) {
				Registers.Add(MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
				continue;
			}

			const TSharedPtr<FJsonObject> Register = MakeShared<FJsonObject>();
			Register->SetStringField(TEXT("Name"), Name);
			if (Values.IsValid()) {
				if (const TSharedPtr<FJsonValue> Default = Values->TryGetField(Name); Default.IsValid()) {
					Register->SetField(TEXT("Tag"), Default);
				}
			}
			Registers.Add(MakeShared<FJsonValueObject>(Register));
		}

		const TSharedPtr<FJsonObject> Storage = MakeShared<FJsonObject>();
		Storage->SetArrayField(TEXT("Properties"), Registers);
		if (const TSharedPtr<FJsonValue> Paths = Fields.JsonObject->TryGetField(TEXT("PropertyPathDescriptions")); Paths.IsValid()) {
			Storage->SetField(TEXT("PropertyPathDescriptions"), Paths);
		}
		VM->SetObjectField(Field, Storage);
	}

	/* Everything one element of the export says, read once so the order they go in can be decided */
	struct FRigElementDescription {
		FRigElementKey Key;
		FRigElementKey Parent;
		TArray<FRigParentDescription> Parents;

		FUObjectJsonValueExport Json;
	};
}

UObject* IControlRigImporter::CreateAsset(UObject* CreatedAsset) {
	/* Reflected over an earlier run: the asset is already there, and the import rebuilds it in
	 * place rather than creating a second one beside it */
	if (UControlRigBlueprint* ExistingBlueprint = LoadObject<UControlRigBlueprint>(nullptr, *GetPackage()->GetPathName())) {
		return IImporter::CreateAsset(ExistingBlueprint);
	}

	UClass* ParentClass = GetAssetClass();

	/* A rig built on a class of the game's own is one this project doesn't have. The rest of the
	 * asset has nothing to do with that class, so it is built on the one every rig derives from
	 * and the missing parent is reported rather than failing the import. */
	if (ParentClass == nullptr || !ParentClass->IsChildOf(UControlRig::StaticClass()) || !FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass)) {
		FImportIssues::Report(
			EImportIssue::MissingClass,
			TEXT("The rig's parent class isn't in this project"),
			TEXT("The rig was rebuilt on ControlRig instead. Anything the parent class brought with it, including its own variables, is not part of the asset.")
		);

		ParentClass = UControlRig::StaticClass();
	}

	/* The factory is what knows to give the blueprint its rig graph, which nothing else creates */
	UFactory* Factory = RigBlueprintFactory(ParentClass);

	if (Factory == nullptr) {
		FImportIssues::Report(EImportIssue::MissingClass, TEXT("Control Rig editor factory is unavailable"));
		return nullptr;
	}

	UObject* Blueprint = Factory->FactoryCreateNew(
		UControlRigBlueprint::StaticClass(),
		GetPackage(),
		FName(*GetAssetName()),
		RF_Public | RF_Standalone,
		nullptr,
		GWarn
	);

	return IImporter::CreateAsset(Blueprint);
}

bool IControlRigImporter::Import() {
	UControlRigBlueprint* Blueprint = Create<UControlRigBlueprint>();
	if (Blueprint == nullptr) return false;

	Blueprint->MarkPackageDirty();
	ConstructHierarchy(Blueprint);
	ConstructSettings(Blueprint);

	/* The variables have to exist before their defaults can land anywhere, and they only become
	 * properties on the generated class once the blueprint has been compiled again. The graph
	 * below reads and writes them, so they also have to be there before it is built. */
	if (ConstructVariables(Blueprint) > 0) {
		CompileBlueprintGuarded(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	}
	ApplyVariableDefaults(Blueprint);

	ConstructGraph(Blueprint);

	/* The hierarchy is edited on the blueprint, and every rig built from it holds a copy of its
	 * own that has to be told */
	Blueprint->PropagateHierarchyFromBPToInstances();
	Blueprint->RecompileVM();

	return OnAssetCreation(Blueprint);
}

int32 IControlRigImporter::ConstructHierarchy(UControlRigBlueprint* Blueprint) const {
	URigHierarchy* Hierarchy = RigHierarchyOf(Blueprint);
	URigHierarchyController* Controller = Blueprint->GetHierarchyController();

	if (Hierarchy == nullptr || Controller == nullptr) return 0;

	/* The hierarchy is a subobject of the class default object rather than an export of its own,
	 * so it is reached through what the default object points at */
	FUObjectExport* DefaultObjectExport = GetClassDefaultObject(GetContainer(), GetAssetDataAsValue());
	FUObjectExport* HierarchyExport = FUObjectExport::EmptyExport();

	if (DefaultObjectExport->IsJsonValid() && DefaultObjectExport->JsonObject->HasField(TEXT("Properties"))) {
		HierarchyExport = GetContainer()->GetExportByObjectPath(DefaultObjectExport->GetPropertiesAsValue().GetObject(TEXT("DynamicHierarchy")));
	}

	if (HierarchyExport->IsJsonInvalid()) {
		HierarchyExport = GetContainer()->FindByType(FName(TEXT("RigHierarchy")));
	}

	const TArray<TSharedPtr<FJsonValue>>* Elements = nullptr;

	if (HierarchyExport->IsJsonInvalid() || !HierarchyExport->JsonObject->TryGetArrayField(TEXT("Elements"), Elements)) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The rig has no hierarchy to rebuild"),
			TEXT("The export carries no RigHierarchy, so the rig comes back with no bones, nulls or controls on it.")
		);

		return 0;
	}

	/* Reflected over an earlier run, the hierarchy still holds that run's elements. Leaves first,
	 * since removing a parent takes its children with it. */
	if (Hierarchy->Num() > 0) {
		TArray<FRigElementKey> ExistingKeys = Hierarchy->GetAllKeys(true);

		for (int32 Index = ExistingKeys.Num() - 1; Index >= 0; --Index) {
			Controller->RemoveElement(ExistingKeys[Index], false, false);
		}
	}

	TArray<FRigElementDescription> Descriptions;
	Descriptions.Reserve(Elements->Num());

	int32 Unsupported = 0;

	for (const TSharedPtr<FJsonValue>& Element : *Elements) {
		if (!Element.IsValid() || Element->Type != EJson::Object) continue;

		const FUObjectJsonValueExport Json(Element->AsObject());

		FRigElementDescription Description;
		Description.Json = Json;
		Description.Key = ReadElementKey(Json.GetObject(TEXT("LoadedKey")));

		Description.Parent = ReadElementKey(Json.GetObject(TEXT("ParentKey")));
		const TArray<TSharedPtr<FJsonValue>>* Constraints = nullptr;
		if (Json.JsonObject->TryGetArrayField(TEXT("ParentConstraints"), Constraints)) {
			for (const TSharedPtr<FJsonValue>& Value : *Constraints) {
				const TSharedPtr<FJsonObject>* Fields = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(Fields)) continue;

				const FUObjectJsonValueExport Constraint(*Fields);
				FRigElementKey Key = ReadElementKey(Constraint.GetObject(TEXT("ParentKey")));
				if (!Key.IsValid()) {
					const FUObjectJsonValueExport Parent = Constraint.GetObject(TEXT("ParentElement"));
					Key = ReadElementKey(Parent.GetObject(TEXT("LoadedKey")));
					if (!Key.IsValid()) Key = ReadElementKey(Parent.GetObject(TEXT("Key")));
					if (!Key.IsValid()) Key = ReadElementKey(Parent);
				}
				if (!Key.IsValid()) continue;

				Description.Parents.Add({Key, ReadWeight(Constraint.GetObject(TEXT("InitialWeight"))),
					ReadWeight(Constraint.GetObject(TEXT("Weight")))});
			}
		}

		// Older Mirror exports encode the constraint's parent as an entry in IndexLookup.
		const TArray<TSharedPtr<FJsonValue>>* Lookup = nullptr;
		if (Description.Parents.Num() == 0 && Constraints != nullptr && Json.JsonObject->TryGetArrayField(TEXT("IndexLookup"), Lookup)) {
			for (const TSharedPtr<FJsonValue>& Value : *Lookup) {
				const TSharedPtr<FJsonObject>* Fields = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(Fields)) continue;
				const FUObjectJsonValueExport Entry(*Fields);
				const int32 Index = Entry.GetInteger(TEXT("Value"), INDEX_NONE);
				const FRigElementKey Key = ReadElementKey(Entry.GetObject(TEXT("Key")));
				if (!Key.IsValid() || !Constraints->IsValidIndex(Index)) continue;
				const FUObjectJsonValueExport Constraint((*Constraints)[Index]->AsObject());
				Description.Parents.Add({Key, ReadWeight(Constraint.GetObject(TEXT("InitialWeight"))),
					ReadWeight(Constraint.GetObject(TEXT("Weight")))});
			}
		}

		if (!Description.Parent.IsValid() && Description.Parents.Num() > 0) {
			Description.Parent = Description.Parents[0].Key;
		}

		if (!Description.Key.IsValid()) {
			Unsupported++;

			continue;
		}

		Descriptions.Add(Description);
	}

	auto AddElement = [this, Controller](const FRigElementDescription& Description) -> bool {
		FTransform Transform = FTransform::Identity;
		ReadInitialLocalTransform(Description.Json.GetObject(TEXT("PoseStorage")), Transform);

		switch (Description.Key.Type) {
			case ERigElementType::Bone: {
				/* User bones are the ones added to the rig itself rather than imported off a
				 * skeleton, which is the difference the editor draws them by */
				const ERigBoneType BoneType = Description.Json.GetInteger(TEXT("BoneType"), 0) == 1
					? ERigBoneType::User
					: ERigBoneType::Imported;

				return Controller->AddBone(Description.Key.Name, Description.Parent, Transform, false, BoneType, false, false).IsValid();
			}

			case ERigElementType::Null: {
				return Controller->AddNull(Description.Key.Name, Description.Parent, Transform, false, false, false).IsValid();
			}

			case ERigElementType::Control: {
				FRigControlSettings Settings;

				const FUObjectJsonValueExport SettingsJson = Description.Json.GetObject(TEXT("Settings"));

				if (SettingsJson.JsonObject.IsValid()) {
					GetPropertySerializer()->DeserializeStruct(FRigControlSettings::StaticStruct(), SettingsJson.JsonObject.ToSharedRef(), &Settings);

					ReadControlValue(GetPropertySerializer(), SettingsJson.GetObject(TEXT("MinimumValue")), Settings.MinimumValue);
					ReadControlValue(GetPropertySerializer(), SettingsJson.GetObject(TEXT("MaximumValue")), Settings.MaximumValue);
				}

				FTransform Offset = FTransform::Identity;
				FTransform Shape = FTransform::Identity;

				ReadInitialLocalTransform(Description.Json.GetObject(TEXT("Offset")), Offset);
				ReadInitialLocalTransform(Description.Json.GetObject(TEXT("Shape")), Shape);

				/* A control's value is kept as the transform it puts the rig in, and what that
				 * transform means is up to the type of control: the conversion is the rig's */
				FRigControlValue Value;
				Value.SetFromTransform(Transform, Settings.ControlType, Settings.PrimaryAxis);

				return Controller->AddControl(Description.Key.Name, Description.Parent, Settings, Value, Offset, Shape, false, false).IsValid();
			}

			case ERigElementType::Curve: {
				const float Value = static_cast<float>(ReadNumber(Description.Json, TEXT("Value")));

				return Controller->AddCurve(Description.Key.Name, Value, false, false).IsValid();
			}

			case ERigElementType::RigidBody: {
				FRigRigidBodySettings Settings;
				const FUObjectJsonValueExport Fields = Description.Json.GetObject(TEXT("Settings"));
				if (Fields.JsonObject.IsValid()) {
					GetPropertySerializer()->DeserializeStruct(FRigRigidBodySettings::StaticStruct(), Fields.JsonObject.ToSharedRef(), &Settings);
				}
				return Controller->AddRigidBody(Description.Key.Name, Description.Parent, Settings, Transform, false, false).IsValid();
			}

			case ERigElementType::Reference:
				return Controller->AddReference(Description.Key.Name, Description.Parent, FRigReferenceGetWorldTransformDelegate(), false).IsValid();

#if UE5_3_BEYOND
			case ERigElementType::Connector: {
				FRigConnectorSettings Settings;
				const FUObjectJsonValueExport Fields = Description.Json.GetObject(TEXT("Settings"));
				if (Fields.JsonObject.IsValid()) {
					GetPropertySerializer()->DeserializeStruct(FRigConnectorSettings::StaticStruct(), Fields.JsonObject.ToSharedRef(), &Settings);
				}
#if UE5_4_API
				return Controller->AddConnector(Description.Key.Name, Settings, false, false).IsValid();
#else
				return Controller->AddConnector(Description.Key.Name, Transform, true, Settings, false, false).IsValid();
#endif
			}

#if UE5_4_API
			case ERigElementType::Socket: {
				FLinearColor Color = FLinearColor::White;
				const FUObjectJsonValueExport Fields = Description.Json.GetObject(TEXT("Color"));
				if (Fields.JsonObject.IsValid()) {
					GetPropertySerializer()->DeserializeStruct(TBaseStructure<FLinearColor>::Get(), Fields.JsonObject.ToSharedRef(), &Color);
				}
				FString DescriptionText;
				Description.Json.JsonObject->TryGetStringField(TEXT("Description"), DescriptionText);
				return Controller->AddSocket(Description.Key.Name, Description.Parent, Transform, false, Color, DescriptionText, false, false).IsValid();
			}
#endif
#endif

			default: {
				return false;
			}
		}
	};

	int32 Added = 0;
	int32 Orphaned = 0;

	/* An element can only go in once its parent is there to take it. The export lists a hierarchy
	 * in the order it was built, parents first, but that is the rig's own ordering rather than
	 * anything promised, so the passes below keep going for as long as one of them adds something. */
	TArray<const FRigElementDescription*> Remaining;
	Remaining.Reserve(Descriptions.Num());

	for (const FRigElementDescription& Description : Descriptions) {
		Remaining.Add(&Description);
	}

	while (Remaining.Num() > 0) {
		int32 AddedThisPass = 0;

		for (int32 Index = 0; Index < Remaining.Num();) {
			const FRigElementDescription& Description = *Remaining[Index];

			if (Description.Parent.IsValid() && !Hierarchy->Contains(Description.Parent)) {
				Index++;

				continue;
			}

			if (AddElement(Description)) {
				Added++;
				AddedThisPass++;
			} else {
				Unsupported++;
			}

			Remaining.RemoveAt(Index);
		}

		/* Everything left names a parent that never turns up, so it goes in unparented */
		if (AddedThisPass == 0) {
			for (const FRigElementDescription* Description : Remaining) {
				FRigElementDescription Unparented = *Description;
				Unparented.Parent = FRigElementKey();

				if (AddElement(Unparented)) {
					Added++;
					Orphaned++;
				} else {
					Unsupported++;
				}
			}

			break;
		}
	}


	int32 MissingConstraints = 0;
	for (const FRigElementDescription& Description : Descriptions) {
		if (!Hierarchy->Contains(Description.Key)) continue;
		for (const FRigParentDescription& Parent : Description.Parents) {
			if (!Hierarchy->Contains(Parent.Key)) {
				MissingConstraints++;
				continue;
			}

			if (Parent.Key != Description.Parent && !Controller->AddParent(Description.Key, Parent.Key, 0.f, false, false)) {
				MissingConstraints++;
				continue;
			}

			Hierarchy->SetParentWeight(Description.Key, Parent.Key, Parent.InitialWeight, true);
			Hierarchy->SetParentWeight(Description.Key, Parent.Key, Parent.Weight, false);
		}
	}
#if UE5_4_BEYOND
	RestoreHierarchyMetadata(Hierarchy, GetPropertySerializer(), HierarchyExport->AsValueExport());
#endif
	if (MissingConstraints > 0) {
		FImportIssues::Report(EImportIssue::Data, TEXT("Some Control Rig parent constraints could not be restored"),
			FString::Printf(TEXT("%d missing or invalid constraint(s)"), MissingConstraints));
	}

	if (Orphaned > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("Some rig elements name a parent the export doesn't have"),
			FString::Printf(TEXT("%d were added at the top of the hierarchy instead."), Orphaned)
		);
	}

	if (Unsupported > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The hierarchy holds elements Reflection can't rebuild"),
			FString::Printf(TEXT("%d element(s) have an unavailable type or could not be created."), Unsupported)
		);
	}

	return Added;
}

int32 IControlRigImporter::ConstructVariables(UControlRigBlueprint* Blueprint) {
	/* A rig that declared nothing of its own declares nothing here either */
	return FBlueprintVariables::Construct(Blueprint, FBlueprintVariables::GetDeclared(GetAssetExport(), GetContainer()));
}

void IControlRigImporter::ConstructGraph(UControlRigBlueprint* Blueprint) const {
	/* The VM is a subobject of the class default object, the same way the hierarchy is */
	FUObjectExport* DefaultObjectExport = GetClassDefaultObject(GetContainer(), GetAssetDataAsValue());
	FUObjectExport* VirtualMachineExport = FUObjectExport::EmptyExport();

	if (DefaultObjectExport->IsJsonValid() && DefaultObjectExport->JsonObject->HasField(TEXT("Properties"))) {
		VirtualMachineExport = GetContainer()->GetExportByObjectPath(DefaultObjectExport->GetPropertiesAsValue().GetObject(TEXT("VM")));
	}

	if (VirtualMachineExport->IsJsonInvalid()) {
		VirtualMachineExport = GetContainer()->FindByType(FName(TEXT("RigVM")));
	}

	const FUObjectJsonValueExport EmbeddedVM = GetAssetAsValue().GetObject(TEXT("VM"));
	FUObjectJsonValueExport VM = VirtualMachineExport->AsValueExport();
	if (!VM.Has(TEXT("ByteCodeStorage")) && EmbeddedVM.Has(TEXT("ByteCodeStorage"))) {
		VM = EmbeddedVM;
	}

	if (!VM.Has(TEXT("ByteCodeStorage"))) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The rig has no graph to rebuild"),
			TEXT("The export carries no RigVM, which is the only thing a cooked rig keeps of its graph, so the rig comes back with an empty Forwards Solve.")
		);

		return;
	}

	// Older RigVM packages serialize register declarations and defaults in generated memory classes.
	const TSharedPtr<FJsonObject> WithMemory = MakeShared<FJsonObject>(*VM.JsonObject);
	for (const FUObjectExport* Export : GetContainer()->Exports) {
		if (Export->GetType() == FName(TEXT("RigVMMemoryStorageGeneratorClass"))) {
			AddLegacyMemory(Export, GetContainer(), WithMemory);
		}
	}
	VM = FUObjectJsonValueExport(WithMemory);

	FRigVMGraphReconstruction Reconstruction(Blueprint, GetPropertySerializer());

	if (!Reconstruction.Build(VM)) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The rig's graph couldn't be rebuilt"),
			TEXT("The VM carries no bytecode to read the graph back out of.")
		);

		return;
	}

	const FRigVMGraphStats& Stats = Reconstruction.GetStats();

	UE_LOG(LogReflection, Log, TEXT("\"%s\" rebuilt %d nodes, %d links and %d pin defaults from %d instructions"),
		*GetAssetName(), Stats.Nodes, Stats.Links, Stats.Defaults, Stats.Instructions);

	/* Collapsed nodes and function references are compiled away long before the VM exists, so what
	 * comes back is the graph the VM runs rather than the one that was drawn */
	FImportIssues::Report(
		EImportIssue::Data,
		TEXT("The rig's graph was rebuilt from its bytecode"),
		FString::Printf(
			TEXT("%d nodes, %d links and %d pin defaults, out of %d instructions. A cook keeps only the compiled VM, so collapsed nodes and function references come back as the nodes they were compiled into, and comments and node positions are gone."),
			Stats.Nodes,
			Stats.Links,
			Stats.Defaults,
			Stats.Instructions
		)
	);

	if (Stats.MissingFunctions.Num() > 0) {
		FImportIssues::Report(
			EImportIssue::MissingClass,
			TEXT("The rig uses nodes this engine build doesn't have"),
			FString::Printf(
				TEXT("%d of them, starting with \"%s\". Either the plugin that declares the node isn't enabled, or the engine spells it differently to the one the rig was cooked from. They are left out of the graph, along with anything that was linked through them."),
				Stats.MissingFunctions.Num(),
				*Stats.MissingFunctions[0]
			)
		);
	}

	if (Stats.MismatchedFunctions.Num() > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("Some of the rig's nodes have pins this engine build doesn't"),
			FString::Printf(
				TEXT("%d of them, starting with \"%s\", %d pins in total. The game's engine gave those nodes pins this one doesn't declare: the node is rebuilt with the pins both versions share, and whatever fed the rest is left unlinked."),
				Stats.MismatchedFunctions.Num(),
				*Stats.MismatchedFunctions[0],
				Stats.DroppedPins
			)
		);
	}

	if (Stats.FailedNodes > 0 || Stats.FailedLinks > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("Parts of the rig's graph wouldn't go back together"),
			FString::Printf(TEXT("%d nodes and %d links the graph refused."), Stats.FailedNodes, Stats.FailedLinks)
		);
	}
}

void IControlRigImporter::ConstructSettings(UControlRigBlueprint* Blueprint) const {
	/* The mesh the rig is previewed and authored against. Not part of what the rig does, but a rig
	 * opened without one has nothing to show its controls on. */
	const FUObjectJsonValueExport PreviewMesh = GetAssetDataAsValue().GetObject(TEXT("PreviewSkeletalMesh"));

	if (PreviewMesh.Has(TEXT("AssetPathName"))) {
		const FString AssetPathName = PreviewMesh.GetString(TEXT("AssetPathName"));

		FString PackagePath;
		FString AssetName;

		if (AssetPathName.Split(TEXT("."), &PackagePath, &AssetName)) {
			TObjectPtr<UObject> Mesh = LoadObjectByPath<UObject>(ToEditorPackagePath(PackagePath) + TEXT(".") + AssetName);

			if (Mesh == nullptr) {
				Mesh = DownloadWrapper(Mesh, TEXT("SkeletalMesh"), AssetName, PackagePath);
			}

			if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Mesh)) {
				Blueprint->SetPreviewMesh(SkeletalMesh);
			}
		}
	}

#if WITH_EDITORONLY_DATA
	/* The libraries the shape names on the rig's controls are looked up in. A rig created from
	 * scratch starts with the engine's own, which is what most rigs are still on. */
	FUObjectExport* DefaultObjectExport = GetClassDefaultObject(GetContainer(), GetAssetDataAsValue());

	if (DefaultObjectExport->IsJsonValid() && DefaultObjectExport->JsonObject->HasField(TEXT("Properties"))) {
		const FUObjectJsonValueExport Properties = DefaultObjectExport->GetPropertiesAsValue();

		if (Properties.Has(TEXT("ShapeLibraries"))) {
			TArray<TSoftObjectPtr<UControlRigShapeLibrary>> ShapeLibraries;

			for (const FUObjectJsonValueExport& ShapeLibrary : Properties.GetArray(TEXT("ShapeLibraries"))) {
				if (!ShapeLibrary.Has(TEXT("AssetPathName"))) continue;

				const FString AssetPathName = ShapeLibrary.GetString(TEXT("AssetPathName"));
				if (AssetPathName.IsEmpty() || AssetPathName == TEXT("None")) continue;

				ShapeLibraries.Add(TSoftObjectPtr<UControlRigShapeLibrary>(FSoftObjectPath(AssetPathName)));
			}

			if (ShapeLibraries.Num() > 0) {
				Blueprint->ShapeLibraries = ShapeLibraries;
			}
		}
	}
#endif
}

void IControlRigImporter::ApplyVariableDefaults(const UControlRigBlueprint* Blueprint) const {
	UClass* GeneratedClass = Blueprint->GeneratedClass;
	if (GeneratedClass == nullptr) return;

	FUObjectExport* DefaultObjectExport = GetClassDefaultObject(GetContainer(), GetAssetDataAsValue());

	if (DefaultObjectExport->IsJsonInvalid() || !DefaultObjectExport->JsonObject->HasField(TEXT("Properties"))) {
		return;
	}

	const TSharedPtr<FJsonObject>& Properties = DefaultObjectExport->GetProperties();

	// The hierarchy and VM are reconstructed separately. Apply all other compatible defaults,
	// including inherited rig settings that are absent from the blueprint's variable list.
	const TSharedPtr<FJsonObject> Defaults = MakeShared<FJsonObject>();
	for (const auto& Pair : Properties->Values) {
		const FString Name = JsonKeyToString(Pair.Key);
		if (Name == TEXT("DynamicHierarchy") || Name == TEXT("VM")) continue;
		Defaults->SetField(Pair.Key, Pair.Value);
	}

	GetObjectSerializer()->DeserializeObjectProperties(Defaults, GeneratedClass->GetDefaultObject());
}

#endif
