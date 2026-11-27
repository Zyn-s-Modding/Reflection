/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Settings/ReflectionSettings.h"
#include "Settings/Redirector.h"
#include "Settings/Runtime.h"
#include "Engine/EngineUtilities.h"
#include "Containers/ExportContainer.h"
#include "Importers/Constructor/Importer.h"
#include "Modules/Toolbar/Tools/ImportFromPath.h"
#include "UObject/CoreRedirects.h"
#include "Misc/ScopeExit.h"
#include "Interfaces/IPluginManager.h"
#include "Engine/Log.h"
#include "Modules/ModuleManager.h"

/* 4.25 and below build this module without the engine's shared PCH (see Reflection.Build.cs),
 * which is where the blueprint function library type used to come in from */
#if UE4_25_BELOW
#include "Kismet/BlueprintFunctionLibrary.h"
#endif

/* What a write into a struct member names, said either way round.
 *
 * A build spells the member as a way down to it and the thing it is in as a property of a property;
 * an older one spells the member as a reference to the property itself and the thing it is in as a
 * plain name. Read one way only, the other comes back naming nothing, and a write that names
 * nothing matches nothing. */
/* The struct a property is on and what it is called on it, said either way round.
 *
 * A build says the two apart the way down to the member, and the struct resolved beside it. An
 * older one says them together, as one reference to the property itself: the struct before the
 * colon and the member after it. Read the first way only, the older spelling answers nothing for
 * both, and a member of nothing on a struct of nothing is not something that can be laid out. */
inline void ReadPropertyReference(const FUObjectJsonValueExport& Property, FString& Owner, FString& Member) {
	if (!Property.JsonObject.IsValid()) return;

	if (Member.IsEmpty()) {
		Property.JsonObject->TryGetStringField(TEXT("Name"), Member);
	}

	if (Member.IsEmpty()) {
		if (const TArray<TSharedPtr<FJsonValue>>* Path = nullptr; Property.JsonObject->TryGetArrayField(TEXT("Path"), Path) && Path->Num() > 0) {
			Member = (*Path)[Path->Num() - 1]->AsString();
		}
	}

	/* Or as the property written out in full beside what holds it: the member is the inner one's
	 * name, and the struct is its owner. A member of a struct a blueprint declared is said this way,
	 * and read any other way it names nothing. */
	if (const TSharedPtr<FJsonObject>* Inner = nullptr; Property.JsonObject->TryGetObjectField(TEXT("Property"), Inner)) {
		if (Member.IsEmpty()) {
			(*Inner)->TryGetStringField(TEXT("Name"), Member);
		}

		const TSharedPtr<FJsonObject>* Holder = nullptr;
		FString Spelled;

		if (Owner.IsEmpty() && Property.JsonObject->TryGetObjectField(TEXT("Owner"), Holder) && (*Holder)->TryGetStringField(TEXT("ObjectName"), Spelled)) {
			/* UserDefinedStruct'SomeStruct' -> SomeStruct */
			if (FString Held; Spelled.Split(TEXT("'"), nullptr, &Held)) {
				Held.RemoveFromEnd(TEXT("'"));

				Owner = Held;
			}
		}
	}

	if (!Owner.IsEmpty() && !Member.IsEmpty()) return;

	FString Spelled;

	if (!Property.JsonObject->TryGetStringField(TEXT("ObjectName"), Spelled)) return;

	/* FloatProperty'SomeStruct:Member' -> SomeStruct, Member */
	FString Held;

	if (!Spelled.Split(TEXT("'"), nullptr, &Held)) return;

	Held.RemoveFromEnd(TEXT("'"));

	FString Before, After;

	if (!Held.Split(TEXT(":"), &Before, &After)) return;

	if (Owner.IsEmpty()) Owner = Before;
	if (Member.IsEmpty()) Member = After;
}

inline void ReadStructMemberContext(const FUObjectJsonValueExport& Variable, FString& Owner, FString& Member) {
	Owner.Empty();
	Member.Empty();

	const FUObjectJsonValueExport Inside = Variable.GetObject(TEXT("StructExpression")).GetObject(TEXT("Variable"));

	if (Inside.JsonObject.IsValid() && !Inside.JsonObject->TryGetStringField(TEXT("Name"), Owner)) {
		if (const TSharedPtr<FJsonObject> Held = Inside.GetObject(TEXT("Property")).JsonObject; Held.IsValid()) {
			Held->TryGetStringField(TEXT("Name"), Owner);
		}
	}

	const TSharedPtr<FJsonObject> Named = Variable.GetObject(TEXT("Property")).JsonObject;

	if (!Named.IsValid()) return;

	if (const TArray<TSharedPtr<FJsonValue>>* Path = nullptr; Named->TryGetArrayField(TEXT("Path"), Path) && Path->Num() == 1) {
		Member = (*Path)[0]->AsString();

		return;
	}

	/* Spelled as a reference, of which the member is whatever comes after the class it is on */
	if (FString Spelled; Named->TryGetStringField(TEXT("ObjectName"), Spelled)) {
		Spelled.Split(TEXT(":"), nullptr, &Spelled);

		Member = Spelled.Replace(TEXT("'"), TEXT(""));

		return;
	}

	/* Or spelled out in full, which is how a member of a struct the blueprint itself made is said:
	 * an animation blueprint's own slots for the values it folds away are one of those, and a write
	 * into one of them names nothing else */
	if (const TSharedPtr<FJsonObject>* Held = nullptr; Named->TryGetObjectField(TEXT("Property"), Held)) {
		(*Held)->TryGetStringField(TEXT("Name"), Member);
	}
}

/* Loads the code module a /Script path names, where it is in the project but not loaded yet.
 *
 * A game feature's modules only load when the game turns the feature on, which an editor importing
 * never does, so every class one declares looks like a class the project has not got. The plugin
 * that holds the module is mounted the way the feature would be, which loads its modules, and the
 * module is asked for by name if that did not. Answers whether the module is loaded afterwards. */
inline bool BringInScriptModule(const FString& ScriptPath) {
	FString Module = ScriptPath;

	if (!Module.RemoveFromStart(TEXT("/Script/"))) return false;

	Module.Split(TEXT("."), &Module, nullptr);

	if (Module.IsEmpty()) return false;

	const FName ModuleName(*Module);

	if (FModuleManager::Get().IsModuleLoaded(ModuleName)) return true;

	/* Asked once a module: one that will not load does not start loading on the second try */
	static TSet<FName> Tried;

	if (Tried.Contains(ModuleName)) return false;

	Tried.Add(ModuleName);

	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins()) {
		const bool bHolds = Plugin->GetDescriptor().Modules.ContainsByPredicate([&ModuleName](const FModuleDescriptor& One) {
			return One.Name == ModuleName;
		});

		if (!bHolds) continue;

		const bool bMounted = IPluginManager::Get().MountExplicitlyLoadedPlugin(Plugin->GetName());

		UE_LOG(LogReflection, Display, TEXT("module \"%s\" is in plugin \"%s\", which %s"), *Module, *Plugin->GetName(), bMounted ? TEXT("is now mounted") : TEXT("would not mount"));

		break;
	}

	if (!FModuleManager::Get().IsModuleLoaded(ModuleName) && FModuleManager::Get().ModuleExists(*Module)) {
		FModuleManager::Get().LoadModule(ModuleName);
	}

	return FModuleManager::Get().IsModuleLoaded(ModuleName);
}

inline TSubclassOf<UObject> LoadClassFromPath(const FString& ObjectName, const FString& ObjectPath) {
	const FString FullPath = ObjectPath + TEXT(".") + ObjectName;

	if (UObject* LoadedObject = LoadObjectByPath<UObject>(FullPath)) {
		if (UClass* LoadedClass = Cast<UClass>(LoadedObject)) {
			return LoadedClass;
		}
	}

	/* A class this engine has renamed, which the class redirects say and an object load never looks at */
	const FCoreRedirectObjectName Named(*FullPath);
	const FCoreRedirectObjectName Now = FCoreRedirects::GetRedirectedName(ECoreRedirectFlags::Type_Class, Named);

	if (Now != Named) {
		if (UClass* Renamed = LoadObjectByPath<UClass>(Now.ToString())) {
			return Renamed;
		}
	}

	/* Declared by a module that is there but was never loaded, which once loaded has it */
	if (BringInScriptModule(ObjectPath)) {
		if (UClass* Brought = LoadObjectByPath<UClass>(FullPath)) {
			return Brought;
		}
	}

	return nullptr;
}

inline UClass* LoadBlueprintClass(const FString& PackagePath) {
	if (const UBlueprint* Blueprint = LoadObjectByPath<UBlueprint>(PackagePath)) {
		return Blueprint->GeneratedClass;
	}

	const FString ClassPath = PackagePath + TEXT(".") + FPackageName::GetShortName(PackagePath) + TEXT("_C");
	return LoadObjectByPath<UClass>(ClassPath);
}

inline UClass* LoadOrImportBlueprintClass(const FString& ObjectPath) {
	const FString SourcePath = TToolImportFromPath::ToPackagePath(ObjectPath);
	if (SourcePath.IsEmpty()) return nullptr;

	FString PackagePath;
	if (!FAssetUtilities::TryGetEngineContentDestination(SourcePath, PackagePath)) {
		PackagePath = ToEditorPackagePath(SourcePath);
	}

	if (UClass* Loaded = LoadBlueprintClass(PackagePath)) return Loaded;
	if (PackageHoldsAsset(PackagePath)) return nullptr;

	// Recursive parent imports must stop if the export points back to a class already being resolved.
	static TSet<FString> ActivePackages;
	if (ActivePackages.Contains(PackagePath)) return nullptr;
	ActivePackages.Add(PackagePath);
	ON_SCOPE_EXIT { ActivePackages.Remove(PackagePath); };

	// Mirror needs the source package even when the editor puts the asset under a redirect.
	return TToolImportFromPath::Import(SourcePath) ? LoadBlueprintClass(PackagePath) : nullptr;
}

inline UClass* LoadClass(const TSharedPtr<FJsonObject>& SuperStruct) {
	const FString ObjectPath = SuperStruct->GetStringField(TEXT("ObjectPath"));
	if (ObjectPath.StartsWith(TEXT("/Script/"))) {
		FString ClassName = StripObjectOuter(SuperStruct->GetStringField(TEXT("ObjectName")));
		FString Module = ObjectPath;
		ObjectPath.Split(TEXT("."), &Module, &ClassName);
		return LoadClassFromPath(ClassName, Module);
	}

	return LoadOrImportBlueprintClass(ObjectPath);
}

/* What a class or a struct says it comes from, under whichever name it is written as.
 *
 * A struct carries Next, a class made from C++ carries SuperStruct, and one made from another
 * blueprint carries Super. All three name the same thing, and a class read without finding it is
 * left parented to nothing at all, which is not something the compiler survives. */
inline TSharedPtr<FJsonObject> GetSuperStructJsonObject(const TSharedPtr<FJsonObject>& JsonObject) {
	if (!JsonObject.IsValid()) return nullptr;

	for (const TCHAR* Named : { TEXT("Next"), TEXT("SuperStruct"), TEXT("Super") }) {
		if (const TSharedPtr<FJsonObject>* Found = nullptr; JsonObject->TryGetObjectField(Named, Found)) {
			return *Found;
		}
	}

	return nullptr;
}

/* The kind of blueprint an asset was, as the asset itself says.
 *
 * A cooked class keeps what the editor knew it as in its tags, and that is the only place some of
 * them are said at all: an animation layer interface is a class like any other with an ordinary
 * parent, and read off the parent it comes back as a normal blueprint with its layers turned into
 * functions nobody can call. Where nothing says, the parent is still the best that can be done. */
inline EBlueprintType GetBlueprintTypeSaid(const TSharedPtr<FJsonObject>& AssetData, const UClass* Class);

inline EBlueprintType GetBlueprintType(const UClass* Class) {
	EBlueprintType BlueprintType = BPTYPE_Normal;

	if (Class->HasAnyClassFlags(CLASS_Const)) {
		BlueprintType = BPTYPE_Const;
	}
	
	if (Class == UBlueprintFunctionLibrary::StaticClass()) {
		BlueprintType = BPTYPE_FunctionLibrary;
	}
	
	/* An animation layer interface is made on UAnimLayerInterface, which is itself an interface,
	 * and a cook keeps no editor tag saying so; anything made on an interface is one. */
	if (Class != nullptr && Class->IsChildOf(UInterface::StaticClass())) {
		BlueprintType = BPTYPE_Interface;
	}
	
	return BlueprintType;
}

inline EBlueprintType GetBlueprintTypeSaid(const TSharedPtr<FJsonObject>& AssetData, const UClass* Class) {
	const TSharedPtr<FJsonObject>* Tags = nullptr;

	if (AssetData.IsValid() && AssetData->TryGetObjectField(TEXT("EditorTags"), Tags)) {
		FString Says;

		if ((*Tags)->TryGetStringField(TEXT("BlueprintType"), Says) && !Says.IsEmpty()) {
			/* Spelled the way the engine spells it, so the engine is asked what it means */
			if (const UEnum* Kinds = StaticEnum<EBlueprintType>()) {
				const int64 Which = Kinds->GetValueByNameString(Says);

				if (Which != INDEX_NONE) return static_cast<EBlueprintType>(Which);
			}
		}
	}

	return GetBlueprintType(Class);
}

inline FUObjectExport* GetClassDefaultObject(FUObjectExportContainer* AssetContainer, const FUObjectJsonValueExport& JsonObject) {
	FUObjectExport* Export = AssetContainer->GetExportByObjectPath(JsonObject.GetObject("ClassDefaultObject"));
	if (!Export->IsJsonValid()) {
		Export = AssetContainer->GetExportStartingWith("Name", "Default__");
	}

	return Export;
}