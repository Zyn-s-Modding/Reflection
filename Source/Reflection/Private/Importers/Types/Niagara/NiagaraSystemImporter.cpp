/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Niagara/NiagaraSystemImporter.h"

#include "Engine/EngineUtilities.h"
#include "Importers/Types/Niagara/NiagaraCookedCache.h"
#include "Importers/Types/Niagara/NiagaraEmitterImporter.h"
#include "Importers/Types/Niagara/NiagaraGraphs.h"
#include "Importers/Types/Niagara/NiagaraScriptImporter.h"
#include "Importers/Types/Niagara/NiagaraVersions.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#if UE5_4_BEYOND
#include "NiagaraSimulationStageBase.h"
#endif
#include "NiagaraSystem.h"
#include "UObject/StructOnScope.h"

#if UE5_4_BEYOND && WITH_EDITORONLY_DATA
#include "Utilities/PrivateMemberAccessor.h"
#endif

#if UE5_4_BEYOND && WITH_EDITORONLY_DATA
DEFINE_PRIVATE_MEMBER_ACCESSOR(FNiagaraResolveParameterStoreBindings, &UNiagaraSystem::ResolveParameterStoreBindings, void (UNiagaraSystem::*)())
#endif

namespace {
#if UE5_1_BEYOND
	void RestoreEmitterCompiledData(UNiagaraSystem* System, const TArray<TSharedPtr<FJsonValue>>& Entries,
		UPropertySerializer* Serializer) {
		UScriptStruct* Struct = FNiagaraEmitterCompiledData::StaticStruct();
		TArray<TSharedRef<const FNiagaraEmitterCompiledData>> Data;
		Data.Reserve(Entries.Num());

		for (const TSharedPtr<FJsonValue>& Entry : Entries) {
			const TSharedPtr<FJsonObject>* Fields = nullptr;
			if (!Entry.IsValid() || !Entry->TryGetObject(Fields) || !Fields->IsValid()) {
				FImportIssues::Report(EImportIssue::Data, TEXT("Invalid Niagara emitter compiled data"));
				return;
			}

			// The struct's native constructor is private to the Niagara module on some engine versions.
			const TSharedRef<FStructOnScope> Storage = MakeShared<FStructOnScope>(Struct);
			Serializer->DeserializeStruct(Struct, Fields->ToSharedRef(), Storage->GetStructMemory(), System);
			Data.Emplace(Storage, reinterpret_cast<const FNiagaraEmitterCompiledData*>(Storage->GetStructMemory()));
		}

		// Niagara exposes the serialized array through a read-only accessor.
		using FCompiledData = TArray<TSharedRef<const FNiagaraEmitterCompiledData>>;
		const_cast<FCompiledData&>(System->GetEmitterCompiledData()) = MoveTemp(Data);
	}
#endif
}

bool INiagaraSystemImporter::Import() {
	return FNiagaraGraphs::HasSerializedPins(GetContainer()) && IObjectImporter::Import();
}

void INiagaraSystemImporter::Repair(UObject* Asset) const {
	UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset);
	FUObjectExportContainer* Container = GetContainer();
	if (System == nullptr || Container == nullptr) return;

	FNiagaraCookedCache::Restore(Container);
	FNiagaraGraphStats Stats;
	FNiagaraGraphs::Rebuild(Container, GetPackage(), GetPropertySerializer(), Stats);

	// Restore scripts first so the emitters can use their source references.
	for (FUObjectExport* Export : Container->Exports) {
		if (UNiagaraScript* Script = Cast<UNiagaraScript>(Export->Object)) {
			INiagaraScriptImporter::RestoreSource(Script, Container);
		}
	}

	for (FUObjectExport* Export : Container->Exports) {
		if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Export->Object)) {
			INiagaraEmitterImporter::RestoreVersions(Emitter, Container);
		}
	}

#if UE5_1_BEYOND
	const TArray<TSharedPtr<FJsonValue>>* Compiled = nullptr;
	if (AssetExport->JsonObject->TryGetArrayField(TEXT("EmitterCompiledStructs"), Compiled)) {
		RestoreEmitterCompiledData(System, *Compiled, GetPropertySerializer());
	} else if (GetAssetData()->TryGetArrayField(TEXT("EmitterCompiledData"), Compiled)) {
		RestoreEmitterCompiledData(System, *Compiled, GetPropertySerializer());
	}
#endif

	if (Stats.Dangling > 0 || Stats.Unpaired > 0) {
		FImportIssues::Report(EImportIssue::Data, TEXT("Some Niagara graph connections could not be restored"),
			FString::Printf(TEXT("%d missing links and %d missing pin pairings"), Stats.Dangling, Stats.Unpaired));
	}

#if UE5_1_BEYOND && WITH_EDITORONLY_DATA
	UNiagaraScript* Spawn = System->GetSystemSpawnScript();
	UNiagaraScript* Update = System->GetSystemUpdateScript();
	if (Spawn == nullptr || Update == nullptr) {
		FImportIssues::Report(EImportIssue::Data, TEXT("Niagara system is missing its spawn or update script"));
		return;
	}

	// The two system scripts reference the same graph, which may be owned by either script.
	UNiagaraScriptSourceBase* Source = Spawn->GetLatestSource();
	if (Source == nullptr) Source = Update->GetLatestSource();
	if (Source == nullptr) {
		Source = NewObject<UNiagaraScriptSource>(Spawn, NAME_None, RF_Transactional);
	}
	if (Spawn->GetLatestSource() == nullptr) Spawn->SetLatestSource(Source);
	if (Update->GetLatestSource() == nullptr) Update->SetLatestSource(Source);

	UNiagaraScriptSource* GraphSource = Cast<UNiagaraScriptSource>(Source);
	if (GraphSource == nullptr || GraphSource->NodeGraph == nullptr || Spawn->GetLatestSource() != Update->GetLatestSource()) {
		FImportIssues::Report(EImportIssue::Data, TEXT("Niagara system source graph is incomplete"));
		if (GraphSource != nullptr && GraphSource->NodeGraph == nullptr) {
			GraphSource->NodeGraph = NewObject<UNiagaraGraph>(GraphSource, NAME_None, RF_Transactional);
		}
	}

#if UE5_4_BEYOND
	// These editor scripts can be absent from a cooked CPU emitter. The target engine
	// traverses all of these slots during compile-ID and data interface resolution.
	for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles()) {
		FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
		if (Data == nullptr) continue;

		const FNiagaraVersion Version{Data, FVersionedNiagaraEmitterData::StaticStruct()};
		UNiagaraEmitter* Emitter = Handle.GetInstance().Emitter;
		UNiagaraScriptSourceBase* EmitterSource = Data->SpawnScriptProps.Script != nullptr
			? Data->SpawnScriptProps.Script->GetLatestSource() : nullptr;
		auto RestoreEditorScript = [&](const TCHAR* Field, ENiagaraScriptUsage Usage, bool bNested) {
			UObject* Existing = bNested ? FNiagaraVersions::GetObjectIn(Version, Field, TEXT("Script"))
				: FNiagaraVersions::GetObject(Version, Field);
			if (Existing != nullptr) return;

			UNiagaraScript* Script = NewObject<UNiagaraScript>(Emitter, NAME_None, RF_Transactional);
			Script->SetUsage(Usage);
			if (EmitterSource != nullptr) Script->SetLatestSource(EmitterSource);
			if (bNested) FNiagaraVersions::SetObjectIn(Version, Field, TEXT("Script"), Script);
			else FNiagaraVersions::SetObject(Version, Field, Script);
		};

		RestoreEditorScript(TEXT("EmitterSpawnScriptProps"), ENiagaraScriptUsage::EmitterSpawnScript, true);
		RestoreEditorScript(TEXT("EmitterUpdateScriptProps"), ENiagaraScriptUsage::EmitterUpdateScript, true);
		if (Data->SimTarget != ENiagaraSimTarget::GPUComputeSim) {
			RestoreEditorScript(TEXT("GPUComputeScript"), ENiagaraScriptUsage::ParticleGPUComputeScript, false);
		}
	}

	bool bCanResolve = true;
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles()) {
		const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
		if (!Handle.GetIsEnabled() || Data == nullptr) continue;

		bCanResolve &= Data->SpawnScriptProps.Script != nullptr && Data->UpdateScriptProps.Script != nullptr
			&& Data->GetGPUComputeScript() != nullptr;
		for (const FNiagaraEventScriptProperties& Event : Data->GetEventHandlers()) {
			bCanResolve &= Event.Script != nullptr;
		}
		for (const UNiagaraSimulationStageBase* Stage : Data->GetSimulationStages()) {
			bCanResolve &= Stage == nullptr || !Stage->bEnabled || Stage->Script != nullptr;
		}
	}
	if (!bCanResolve) {
		FImportIssues::Report(EImportIssue::Data, TEXT("Niagara emitter is missing a compiled script"));
		return;
	}
	FNiagaraResolveParameterStoreBindings::Access(System);
#endif

	if (Spawn->GetLatestSource() == Update->GetLatestSource()) {
#if UE5_4_BEYOND
		// Initialize the serialized emitter instances before the editor can try
		// to merge them with a parent from a different engine or import.
		struct FParentDuringLoad {
			FVersionedNiagaraEmitterData* Data;
			FVersionedNiagaraEmitter Parent;
		};
		TArray<FParentDuringLoad> Parents;
		TArray<UNiagaraEmitter*> Emitters;
		for (FUObjectExport* Export : Container->Exports) {
			UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Export->Object);
			if (Emitter == nullptr) continue;
			Emitters.Add(Emitter);
			for (const FNiagaraVersion& Version : FNiagaraVersions::All(Emitter)) {
				if (Version.Struct != FVersionedNiagaraEmitterData::StaticStruct()) continue;
				auto* Data = static_cast<FVersionedNiagaraEmitterData*>(Version.Data);
#if STATS
				// Imported emitter subobjects bypass UNiagaraEmitter::PostLoad.
				Data->GetStatData().Init();
#endif
				if (Data->VersionedParent.Emitter == nullptr) continue;
				Parents.Add({Data, Data->VersionedParent});
				Data->VersionedParent = FVersionedNiagaraEmitter();
			}
		}
		for (UNiagaraEmitter* Emitter : Emitters) Emitter->UpdateEmitterAfterLoad();
#endif
		System->UpdateSystemAfterLoad();
#if UE5_4_BEYOND
		for (const FParentDuringLoad& Entry : Parents) {
			Entry.Data->VersionedParent = Entry.Parent;
		}
#endif
	}
#endif
}
