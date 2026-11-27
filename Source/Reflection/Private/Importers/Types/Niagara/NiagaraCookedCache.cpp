/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Niagara/NiagaraCookedCache.h"

#include "Engine/EngineUtilities.h"

#include "UObject/UnrealType.h"

#include "Containers/ExportContainer.h"
#include "NiagaraCommon.h"
#include "NiagaraRendererProperties.h"
#include "Importers/Constructor/ImportIssues.h"

namespace {
	/* Fields RestoreRendererBindings carries over itself, so the unplaced-field report leaves them out */
	const struct FHandledHere {
		FHandledHere() {
			FImportIssues::MarkFieldHandled(TEXT("NiagaraVariableAttributeBinding"), TEXT("RootName"));
			FImportIssues::MarkFieldHandled(TEXT("NiagaraSpriteRendererProperties"), TEXT("CutoutMode"));
		}
	} GHandledHere;
}

int32 FNiagaraCookedCache::Restore(FUObjectExportContainer* Container) {
#if WITH_EDITORONLY_DATA
	if (Container == nullptr) return 0;

	static const FString Aside = TEXT("CookedEditorCache");

	int32 Moved = 0;

	for (FUObjectExport* Export : Container->Exports) {
		UObject* Object = Export->Object;

		if (Object == nullptr) continue;

		UClass* Class = Object->GetClass();

		const UObject* Untouched = Class->GetDefaultObject();

		if (Untouched == nullptr) continue;

		for (TFieldIterator<FProperty> Held(Class); Held; ++Held) {
			const FString Name = Held->GetName();

			if (!Name.EndsWith(Aside)) continue;

			FProperty* Reads = FindFProperty<FProperty>(Class, *Name.LeftChop(Aside.Len()));

			/* The two have to be the same thing said twice, or the name is a coincidence */
			if (Reads == nullptr || !Reads->SameType(*Held)) continue;

			void* Cached = Held->ContainerPtrToValuePtr<void>(Object);

			/* An asset that was not cooked this way keeps the cache as it was made, and there is
			 * nothing in it to put anywhere */
			if (Held->Identical(Cached, Held->ContainerPtrToValuePtr<void>(Untouched))) continue;

			Reads->CopyCompleteValue(Reads->ContainerPtrToValuePtr<void>(Object), Cached);

			/* Left empty after, the way the engine leaves it, so the same values are not sitting
			 * in two places for something later to disagree over */
			Held->CopyCompleteValue(Cached, Held->ContainerPtrToValuePtr<void>(Untouched));

			Moved++;
		}
	}

	return Moved;
#else
	return 0;
#endif
}

int32 FNiagaraCookedCache::RestoreRendererBindings(FUObjectExportContainer* Container) {
#if WITH_EDITORONLY_DATA
	if (Container == nullptr) return 0;

	UScriptStruct* BindingStruct = FNiagaraVariableAttributeBinding::StaticStruct();
	const FStructProperty* RootProperty = CastField<FStructProperty>(BindingStruct->FindPropertyByName(TEXT("RootVariable")));
	const FStructProperty* ParamMapProperty = CastField<FStructProperty>(BindingStruct->FindPropertyByName(TEXT("ParamMapVariable")));
	if (RootProperty == nullptr || ParamMapProperty == nullptr) return 0;

	int32 Restored = 0;

	for (FUObjectExport* Export : Container->Exports) {
		UNiagaraRendererProperties* Renderer = Cast<UNiagaraRendererProperties>(Export->Object);
		if (Renderer == nullptr || !Export->IsJsonValid()) continue;

		const TSharedPtr<FJsonObject>* Properties = nullptr;
		if (!Export->JsonObject->TryGetObjectField(TEXT("Properties"), Properties)) continue;

		for (TFieldIterator<FStructProperty> Held(Renderer->GetClass()); Held; ++Held) {
			if (Held->Struct != BindingStruct) continue;

			const TSharedPtr<FJsonObject>* Said = nullptr;
			if (!(*Properties)->TryGetObjectField(Held->GetName(), Said)) continue;

			FString RootName;
			if (!(*Said)->TryGetStringField(TEXT("RootName"), RootName) || RootName.IsEmpty() || RootName == TEXT("None")) continue;

			/* Already said the old way, and so already right */
			if ((*Said)->HasField(TEXT("RootVariable"))) continue;

			void* Binding = Held->ContainerPtrToValuePtr<void>(Renderer);
			FNiagaraVariable* Root = RootProperty->ContainerPtrToValuePtr<FNiagaraVariable>(Binding);
			const FNiagaraVariableBase* ParamMap = ParamMapProperty->ContainerPtrToValuePtr<FNiagaraVariableBase>(Binding);

			const FNiagaraTypeDefinition Type = ParamMap->GetType().IsValid() ? ParamMap->GetType() : Root->GetType();
			if (!Type.IsValid()) continue;

			/* RootVariable is more than a name: it carries the value the renderer falls back to when
			 * the particles don't write the attribute, and the renderer reads that value every time
			 * it builds its scene proxy (RadiusBinding.GetDefaultValue<float>() for a light). A
			 * variable made from a type and a name has no value, and reading it asserts
			 * (IsDataAllocated, NiagaraTypes.h) and takes the editor down the moment a component
			 * with this system is drawn. The default the binding already holds is kept where the
			 * type is the same; otherwise the type's zero value is put there. */
			FNiagaraVariable Pointed(Type, FName(*RootName));
			if (Root->IsDataAllocated() && Root->GetType() == Type) {
				Pointed.SetData(Root->GetData());
			} else {
				Pointed.AllocateData();
			}
			*Root = Pointed;
			Restored++;
		}

		/* CutoutMode: Material uses the material's own opacity for the cutout, Texture the one set */
		FString Cutout;
		if ((*Properties)->TryGetStringField(TEXT("CutoutMode"), Cutout)) {
			if (const FBoolProperty* UseMaterial = FindFProperty<FBoolProperty>(Renderer->GetClass(), TEXT("bUseMaterialCutoutTexture"))) {
				UseMaterial->SetPropertyValue_InContainer(Renderer, Cutout.EndsWith(TEXT("Material")));
			}
		}
	}

	return Restored;
#else
	return 0;
#endif
}
