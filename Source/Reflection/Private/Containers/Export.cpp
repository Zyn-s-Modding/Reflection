/* Copyright Reflection Contributors 2024-2026 */

#include "Containers/Export.h"
#include "Engine/EngineUtilities.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Settings/Runtime.h"
#include "Importers/Types/Blueprint/BlueprintUtilities.h"
#include "Importers/Constructor/Importer.h"

FString ReadPathFromObject(const FUObjectJsonValueExport& PackageIndex) {
	FString ObjectType, ObjectName, ObjectPath, Outer;
	PackageIndex.GetString("ObjectName").Split("'", &ObjectType, &ObjectName);

	ObjectPath = PackageIndex.GetString("ObjectPath");
	ObjectPath.Split(".", &ObjectPath, nullptr);

	const FString& ProjectName = GReflectionRuntime.Profile.ProjectName;

	if (!ProjectName.IsEmpty()) {
		ObjectPath = ObjectPath.Replace(*(ProjectName + "/Content"), TEXT("/Game"));
	}

	ObjectPath = ObjectPath.Replace(TEXT("Engine/Content"), TEXT("/Engine"));
	ObjectName = ObjectName.Replace(TEXT("'"), TEXT(""));

	if (ObjectName.Contains(".")) {
		ObjectName.Split(".", nullptr, &ObjectName);
	}

	if (ObjectName.Contains(".")) {
		ObjectName.Split(".", &Outer, &ObjectName);
	}

	return ObjectPath + "." + ObjectName;
}

UClass* FUObjectExport::GetClass() {
	if (Class) return Class;
	
	FString ClassName = GetString("Class");

	if (Has("Template")) {
		ClassName = ReadPathFromObject(GetObject("Template")).Replace(TEXT("Default__"), TEXT(""));
	}

	if (ClassName.Contains("'")) {
		ClassName.Split("'", nullptr, &ClassName, ESearchCase::IgnoreCase, ESearchDir::FromStart);
		ClassName.Split("'", &ClassName, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromStart);
	}

	UClass* OutClass = FindClassByType(ClassName);
	if (!OutClass) {
		OutClass = FindClassByType(GetType().ToString());
	}

	if (!OutClass && ClassName.Contains(TEXT("/"))) {
		OutClass = LoadOrImportBlueprintClass(ClassName);
	}

	// A Blueprint class export describes its parent. Its export type is a class metadata object.
	TSharedPtr<FJsonObject> ParentReference = GetSuperStructJsonObject(GetProperties());
	if (!ParentReference.IsValid()) ParentReference = GetSuperStructJsonObject(JsonObject);

	if (UClass* ParentClass = ParentReference.IsValid() ? LoadClass(ParentReference) : nullptr) {
		OutClass = ParentClass;
	} else if (OutClass != nullptr && OutClass->IsChildOf(UBlueprintGeneratedClass::StaticClass())) {
		return nullptr;
	}

	if (!OutClass) return nullptr;

	Class = OutClass;
	return Class;
}
