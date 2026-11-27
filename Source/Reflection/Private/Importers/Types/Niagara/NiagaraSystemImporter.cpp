/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Niagara/NiagaraSystemImporter.h"

#include "Engine/EngineUtilities.h"
#include "Importers/Types/Niagara/NiagaraCookedCache.h"
#include "Importers/Types/Niagara/NiagaraEmitterImporter.h"
#include "Importers/Types/Niagara/NiagaraGraphs.h"
#include "Importers/Types/Niagara/NiagaraScriptImporter.h"
#include "NiagaraStatelessFallback.h"
#include "Importers/Types/Niagara/NiagaraVersions.h"
#include "NiagaraCustomVersion.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#if UE5_1_BEYOND
#include "NiagaraSystemEditorData.h"
#endif
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
	bool RestoreEmitterHandles(UNiagaraSystem* System, FUObjectExportContainer* Container) {
		TArray<UNiagaraEmitter*> Owned;
		for (FUObjectExport* Export : Container->Exports) {
			if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Export->Object); Emitter != nullptr && Emitter->GetOuter() == System) {
				Owned.Add(Emitter);
			}
		}

		const FObjectPropertyBase* Stateless = FindFProperty<FObjectPropertyBase>(FNiagaraEmitterHandle::StaticStruct(), TEXT("StatelessEmitter"));
		int32 Restored = 0;
		TArray<FString> Missing;
		for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles()) {
			if (Stateless != nullptr && Stateless->GetObjectPropertyValue_InContainer(&Handle) != nullptr) continue;

			FVersionedNiagaraEmitter& Instance = Handle.GetInstance();
			if (Instance.Emitter == nullptr) {
				const FName Name = Handle.GetName();
				UNiagaraEmitter* Candidate = nullptr;
				bool bAmbiguous = false;
				for (UNiagaraEmitter* Emitter : Owned) {
					if (Emitter->GetFName() == Name) {
						Candidate = Emitter;
						bAmbiguous = false;
						break;
					}
					if (Emitter->GetFName().GetPlainNameString() != Name.ToString() && Emitter->GetUniqueEmitterName() != Name.ToString()) continue;
					bAmbiguous |= Candidate != nullptr;
					Candidate = Emitter;
				}
				if (!bAmbiguous && Candidate != nullptr) {
					Instance.Emitter = Candidate;
					Restored++;
				}
			}

			if (Instance.Emitter != nullptr && Handle.GetEmitterData() == nullptr) {
				const FGuid Exposed = Instance.Emitter->GetExposedVersion().VersionGuid;
				if (!Instance.Version.IsValid()) Instance.Version = Exposed;
			}
			if (Handle.GetEmitterData() == nullptr) Missing.Add(Handle.GetName().ToString());
		}

		if (Restored > 0) {
			UE_LOG(LogReflection, Display, TEXT("\"%s\" restored %d emitter handle(s) from editor exports"), *System->GetName(), Restored);
		}
		if (Missing.Num() == 0) return true;

		const FString Detail = FString::Join(Missing, TEXT(", "));
		UE_LOG(LogReflection, Warning, TEXT("\"%s\" is missing Niagara emitter instances or revisions: %s"), *System->GetName(), *Detail);
		FImportIssues::Report(EImportIssue::Data, TEXT("Niagara emitter instances or revisions are missing"), Detail);
		return false;
	}

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
	if (!FNiagaraGraphs::HasSerializedPins(GetContainer())) return false;

	UNiagaraSystem* System = Create<UNiagaraSystem>();
	if (System == nullptr) return false;

	FNiagaraGraphs::PrepareExports(*GetContainer(), *GetObjectSerializer());
	DeserializeExports(System);
	GetObjectSerializer()->DeserializeObjectProperties(GetAssetData(), System);
	if (!RestoreSystem(System)) {
		System->ClearFlags(RF_Public | RF_Standalone);
		MoveToTransientPackageAndRename(System);
		AssetExport->Object = nullptr;
		return false;
	}

	RestorePreviewPlaybackRange(*System);
	return OnAssetCreation(System);
}

bool INiagaraSystemImporter::RestorePreviewPlaybackRange(UNiagaraSystem& System) {
#if UE5_1_BEYOND && WITH_EDITORONLY_DATA
	UNiagaraSystemEditorData* EditorData = Cast<UNiagaraSystemEditorData>(System.GetEditorData());
	if (EditorData == nullptr) return false;

	const TRange<float> Range = EditorData->GetPlaybackRange();
	const float Start = Range.GetLowerBoundValue();
	const float End = Range.GetUpperBoundValue();
	if (FMath::IsFinite(Start) && FMath::IsFinite(End) && End > Start) return false;

	// Cooked editor data can carry a zero-length range, which keeps the preview at its first frame.
	EditorData->SetPlaybackRange(GetDefault<UNiagaraSystemEditorData>()->GetPlaybackRange());
	return true;
#else
	return false;
#endif
}

bool INiagaraSystemImporter::RestoreSystem(UNiagaraSystem* System) const {
	FUObjectExportContainer* Container = GetContainer();
	if (Container == nullptr) return false;

	FNiagaraCookedCache::Restore(Container);
	FNiagaraCookedCache::RestoreRendererBindings(Container);
	FNiagaraGraphs::ReportMissingScriptVersions(*Container);

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

	FNiagaraGraphStats Stats;
	FNiagaraGraphs::Rebuild(Container, GetPackage(), GetPropertySerializer(), Stats);
	for (FUObjectExport* Export : Container->Exports) {
		if (UNiagaraScript* Script = Cast<UNiagaraScript>(Export->Object)) {
			const TSharedPtr<FJsonObject>* Fields = nullptr;
			const bool bInferUsage = !Export->JsonObject->TryGetObjectField(TEXT("Properties"), Fields) || !(*Fields)->HasField(TEXT("Usage"));
			FNiagaraGraphs::RestoreOutputUsage(*Script, bInferUsage);
		}
	}

#if UE5_1_BEYOND
	bool bConvertedStateless = false;
	if (!FNiagaraStatelessFallback::Convert(*System, *GetAssetData(), *Container, *GetObjectSerializer(), bConvertedStateless)) return false;
	/* Again for the renderers the stateless conversion made or read in again */
	FNiagaraCookedCache::RestoreRendererBindings(Container);
	if (!RestoreEmitterHandles(System, Container)) return false;
	for (FUObjectExport* Export : Container->Exports) {
		if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Export->Object)) {
			INiagaraEmitterImporter::InitializeRuntime(Emitter);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Compiled = nullptr;
	if (!bConvertedStateless && AssetExport->JsonObject->TryGetArrayField(TEXT("EmitterCompiledStructs"), Compiled)) {
		RestoreEmitterCompiledData(System, *Compiled, GetPropertySerializer());
	} else if (!bConvertedStateless && GetAssetData()->TryGetArrayField(TEXT("EmitterCompiledData"), Compiled)) {
		RestoreEmitterCompiledData(System, *Compiled, GetPropertySerializer());
	}
#endif

	if (Stats.Dangling > 0 || Stats.Unpaired > 0) {
		FImportIssues::Report(EImportIssue::Data, TEXT("Some Niagara graph connections could not be restored"),
			FString::Printf(TEXT("%d missing links and %d missing pin pairings"), Stats.Dangling, Stats.Unpaired));
	}

#if UE5_1_BEYOND && WITH_EDITORONLY_DATA
	/* Set where cooked compile results were thrown away below, so the system is compiled again */
	bool bDiscardedCompiled = false;

	UNiagaraScript* Spawn = System->GetSystemSpawnScript();
	UNiagaraScript* Update = System->GetSystemUpdateScript();
	if (Spawn == nullptr || Update == nullptr) {
		FImportIssues::Report(EImportIssue::Data, TEXT("Niagara system is missing its spawn or update script"));
		return false;
	}

	// The two system scripts reference the same graph, which may be owned by either script.
	UNiagaraScriptSourceBase* Source = Spawn->GetLatestSource();
	if (Source == nullptr) Source = Update->GetLatestSource();
	if (Source == nullptr) {
		Source = NewObject<UNiagaraScriptSource>(Spawn, NAME_None, RF_Transactional);
	}
	if (Spawn->GetLatestSource() == nullptr) Spawn->SetLatestSource(Source);
	if (Update->GetLatestSource() == nullptr) Update->SetLatestSource(Source);
	FNiagaraGraphs::RestoreOutputUsage(*Spawn);
	FNiagaraGraphs::RestoreOutputUsage(*Update);

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
		return false;
	}

	/* The compiled data the game cooked says what each script reads and in what order, and some
	 * of it can be of types this engine doesn't have (a data interface a later engine added). The
	 * engine builds each script's parameter store from that data as the system loads, and an entry
	 * of no type is taken for a plain value: its byte offset is then used as an index into the
	 * data interfaces, which asserts (Array index out of bounds, in
	 * FNiagaraScriptExecutionParameterStore::AddScriptParams) and takes the editor down. Compiled
	 * data like that is thrown away. The scripts compile again from their graphs on save anyway,
	 * and the graphs have had those types taken out.
	 *
	 * So is compiled code built for a data interface that keeps its per-instance data differently
	 * here. The compiler passes an interface's per-instance slot as the first argument of every call
	 * only where the interface kept per-instance data (UserPtrIdx), and the engine binds the calls
	 * with the argument count that implies; one short or over fails a check in the interface's
	 * GetVMExternalFunction as the script first builds its parameter store, which takes the editor
	 * down. The interface's class can change on the way in (one a later engine added, read in as
	 * the nearest one this engine has), so the type resolving is no sign the code still fits it.
	 *
	 * And so is compiled code another Niagara compiler made. Each result carries the version of the
	 * compiler that produced it, and a script the engine loads with a different one is compiled
	 * again (UNiagaraScript::PostLoad, "Niagara compiler version changed"). An imported script is
	 * built rather than loaded, so that never happens to it: the game's code, from a later
	 * compiler, is what runs, against modules and renderer bindings this engine reads differently.
	 * The emitters it drew wrong (particles left black) are drawn right once compiled here. */
	const FGuid ThisCompiler = FNiagaraCustomVersion::GetLatestScriptCompileVersion();
	TArray<FString> Discarded;
	TArray<FString> FromOtherCompiler;
	for (FUObjectExport* Export : Container->Exports) {
		UNiagaraScript* Cooked = Cast<UNiagaraScript>(Export->Object);
		if (Cooked == nullptr) continue;
		const FNiagaraVMExecutableData& CookedData = Cooked->GetVMExecutableData();
		const TArray<FNiagaraScriptDataInterfaceInfo>& CachedInterfaces = Cooked->GetCachedDefaultDataInterfaces();
		bool bStale = CookedData.DataInterfaceInfo.Num() != CachedInterfaces.Num()
			|| CookedData.UObjectInfos.Num() != Cooked->GetCachedDefaultUObjects().Num();
		for (const FNiagaraScriptDataInterfaceCompileInfo& Info : CookedData.DataInterfaceInfo) {
			bStale |= !Info.Type.IsValid() || Info.Type.GetClass() == nullptr;
		}
		for (const FNiagaraVariable& Parameter : CookedData.Parameters.Parameters) {
			bStale |= !Parameter.GetType().IsValid();
		}
		for (const FNiagaraScriptDataInterfaceInfo& Cached : CachedInterfaces) {
			bStale |= Cached.DataInterface == nullptr;
		}
		/* The cached interfaces are made one for one from the compiled list, in its order */
		for (int32 Index = 0; !bStale && Index < CookedData.DataInterfaceInfo.Num(); ++Index) {
			const bool bCompiledWithInstanceData = CookedData.DataInterfaceInfo[Index].UserPtrIdx != INDEX_NONE;
			bStale |= bCompiledWithInstanceData != (CachedInterfaces[Index].DataInterface->PerInstanceDataSize() > 0);
		}
		if (bStale) {
			Cooked->InvalidateCompileResults(TEXT("compiled data names types or data interface layouts this engine doesn't have"));
			Discarded.Add(Cooked->GetName());
			continue;
		}

		const FGuid CompiledBy = Cooked->GetVMExecutableDataCompilationId().CompilerVersionID;
		if (CompiledBy.IsValid() && CompiledBy != ThisCompiler) {
			Cooked->InvalidateCompileResults(TEXT("compiled by another Niagara compiler version"));
			FromOtherCompiler.Add(Cooked->GetName());
		}
	}
	if (Discarded.Num() > 0) {
		FImportIssues::Report(EImportIssue::Data, TEXT("Cooked Niagara compile results named types or data interface layouts this engine doesn't have and were discarded"),
			FString::Printf(TEXT("%s. These scripts are compiled again from their graphs"), *FString::Join(Discarded, TEXT(", "))));
	}
	if (FromOtherCompiler.Num() > 0) {
		FImportIssues::Report(EImportIssue::Data, TEXT("Cooked Niagara compile results came from another Niagara compiler version and were discarded"),
			FString::Printf(TEXT("%d script(s). These are compiled again from their graphs, as the engine does when it loads an asset compiled by another version"), FromOtherCompiler.Num()));
	}
	bDiscardedCompiled = Discarded.Num() > 0 || FromOtherCompiler.Num() > 0;

	/* The system compiles when it is saved, and a GPU emitter whose compute script comes out of
	 * that compile without a base compile hash fails a check in FNiagaraShaderScript::SetScript,
	 * which takes the editor down mid-save. The hash is the graph's entry for the GPU usage, which
	 * the engine always files under the empty usage ID and builds from the particle spawn output.
	 * Checked here so such a system is refused and said, rather than saved and crashed on. */
	TArray<FString> Uncompilable;
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles()) {
		const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
		if (Data == nullptr || Data->SimTarget != ENiagaraSimTarget::GPUComputeSim) continue;

		const UNiagaraScript* Compute = Data->GetGPUComputeScript();
		const UNiagaraScriptSource* ComputeSource = Compute != nullptr ? Cast<UNiagaraScriptSource>(Compute->GetLatestSource()) : nullptr;
		const TCHAR* Why = nullptr;
		if (Compute == nullptr) Why = TEXT("has no GPU compute script");
		else if (Compute->GetUsageId().IsValid()) Why = TEXT("has a GPU compute script under a usage ID the engine never compiles it as");
		else if (ComputeSource == nullptr || ComputeSource->NodeGraph == nullptr) Why = TEXT("has a GPU compute script with no graph");
		else if (ComputeSource->NodeGraph->FindEquivalentOutputNode(ENiagaraScriptUsage::ParticleSpawnScript) == nullptr) Why = TEXT("has no particle spawn output to build the GPU compile hash from");
		if (Why != nullptr) Uncompilable.Add(Handle.GetName().ToString() + TEXT(" ") + Why);
	}
	if (Uncompilable.Num() > 0) {
		FImportIssues::Report(EImportIssue::Data, TEXT("A GPU Niagara emitter would fail its shader compile on save"),
			FString::Join(Uncompilable, TEXT("; ")) + TEXT(". The system was not saved, since saving it would crash the editor."));
		return false;
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
				if (Data->VersionedParent.Emitter == nullptr) continue;
				Parents.Add({Data, Data->VersionedParent});
				Data->VersionedParent = FVersionedNiagaraEmitter();
			}
		}
		for (UNiagaraEmitter* Emitter : Emitters) Emitter->UpdateEmitterAfterLoad();
#endif
		System->UpdateSystemAfterLoad();
#if UE5_1_BEYOND
		if (bConvertedStateless) {
			if (UNiagaraSystemEditorData* EditorData = Cast<UNiagaraSystemEditorData>(System->GetEditorData())) {
				EditorData->SynchronizeOverviewGraphWithSystem(*System);
			}
			System->RequestCompile(true);
		} else if (bDiscardedCompiled) {
			/* Nothing else asks for it before the save, and a system saved with its compile results
			 * thrown away runs nothing until something does */
			System->RequestCompile(false);
		}
#endif
#if UE5_4_BEYOND
		for (const FParentDuringLoad& Entry : Parents) {
			Entry.Data->VersionedParent = Entry.Parent;
		}
#endif
	}
#endif
	return true;
}
