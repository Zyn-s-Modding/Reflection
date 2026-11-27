/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Niagara/NiagaraScriptImporter.h"
#include "Importers/Types/Niagara/NiagaraGraphs.h"
#include "Importers/Types/Niagara/NiagaraVersions.h"
#include "Importers/Types/Niagara/NiagaraCookedCache.h"

#include "Engine/EngineUtilities.h"

#include "EdGraph/EdGraphNode.h"
#include "NiagaraGraph.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"

bool INiagaraScriptImporter::Import() {
	if (!FNiagaraGraphs::HasSerializedPins(GetContainer())) return false;
	FNiagaraGraphs::PrepareExports(*GetContainer(), *GetObjectSerializer());
	return IObjectImporter::Import();
}

void INiagaraScriptImporter::Repair(UObject* Asset) const {
	FUObjectExportContainer* Container = GetContainer();

	if (Container == nullptr) return;

	FNiagaraGraphStats Stats;

	FNiagaraCookedCache::Restore(Container);
	FNiagaraGraphs::ReportMissingScriptVersions(*Container);

	if (!RestoreSource(Asset, Container)) {
		FImportIssues::Report(EImportIssue::Data, TEXT("Niagara script source graph is missing or ambiguous"));
	}

	FNiagaraGraphs::Rebuild(Container, GetPackage(), GetPropertySerializer(), Stats);
	if (UNiagaraScript* Script = Cast<UNiagaraScript>(Asset)) FNiagaraGraphs::RestoreOutputUsage(*Script, !GetAssetData()->HasField(TEXT("Usage")));

	if (Stats.Unpaired > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			FString::Printf(TEXT("%d pin pairing(s) name a pin that isn't there"), Stats.Unpaired),
			FString::Printf(
				TEXT("'%s' has nodes that pair their own pins off and name one that didn't come across, so the node drops the pairing and whatever fed the pin stops reaching what it stood for."),
				*GetAssetName())
		);
	}

	if (Stats.Dangling > 0) {
		FImportIssues::Report(EImportIssue::Data,
			FString::Printf(TEXT("%d Niagara graph link(s) could not be restored"), Stats.Dangling));
	}
}

bool INiagaraScriptImporter::RestoreSource(UObject* Script, FUObjectExportContainer* Container) {
	TArray<FNiagaraVersion> Versions = FNiagaraVersions::All(Script);
	if (Versions.Num() == 0) {
		if (const FNiagaraVersion Version = FNiagaraVersions::Only(Script); Version.IsValid()) Versions.Add(Version);
	}

	// On engines with an unversioned script, Source lives directly on the UObject.
	if (Versions.Num() == 0) Versions.Add({ Script, Script->GetClass() });

	TArray<UObject*> Owned;
	TArray<UObject*> Shared;
	UObject* Owner = Script->GetOuter();
	for (FUObjectExport* Export : Container->Exports) {
		UObject* Candidate = Export->Object;
		if (Candidate == nullptr || Export->GetType() != FName(TEXT("NiagaraScriptSource"))) continue;

		if (Candidate->GetOuter() == Script) Owned.Add(Candidate);
		else if (Owner != nullptr && Candidate->GetOuter() == Owner) Shared.Add(Candidate);
	}

	const TArray<UObject*>& Candidates = Owned.Num() > 0 ? Owned : Shared;
	UObject* Source = Candidates.Num() == 1 ? Candidates[0] : nullptr;

	/* The optional editor package can keep all the sources while stripping the version table.
	 * In that case use the newest numbered source from the same series for the one surviving
	 * revision. The original per-revision references still take precedence when available. */
	if (Source == nullptr && Versions.Num() == 1 && Owned.Num() > 1 && !FNiagaraVersions::HasObject(Versions[0], TEXT("Source"))) {
		const FName FirstName = Owned[0]->GetFName();
		UObject* Latest = Owned[0];
		bool bSameSeries = true;
		for (UObject* Candidate : Owned) {
			const FName Name = Candidate->GetFName();
			bSameSeries &= Name.GetPlainNameString() == FirstName.GetPlainNameString();
			if (Name.GetNumber() > Latest->GetFName().GetNumber()) Latest = Candidate;
		}
		if (bSameSeries) {
			Source = Latest;
			FImportIssues::Report(EImportIssue::Data, TEXT("Niagara script version links were stripped"),
				FString::Printf(TEXT("Used the newest exported source for %s"), *Script->GetName()));
		}
	}

	bool bComplete = true;
	for (const FNiagaraVersion& Version : Versions) {
		if (FNiagaraVersions::HasObject(Version, TEXT("Source"))) continue;
		bComplete &= Versions.Num() == 1 && FNiagaraVersions::SetObject(Version, TEXT("Source"), Source);
	}

	for (const FNiagaraVersion& Version : Versions) {
		UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(FNiagaraVersions::GetObject(Version, TEXT("Source")));
		if (ScriptSource == nullptr) continue;
		if (ScriptSource->NodeGraph == nullptr) {
			UNiagaraGraph* Graph = nullptr;
			for (FUObjectExport* Export : Container->Exports) {
				UNiagaraGraph* Candidate = Cast<UNiagaraGraph>(Export->Object);
				if (Candidate == nullptr || Candidate->GetOuter() != ScriptSource) continue;
				if (Graph != nullptr) {
					Graph = nullptr;
					break;
				}
				Graph = Candidate;
			}
			ScriptSource->NodeGraph = Graph;
		}
		bComplete &= ScriptSource->NodeGraph != nullptr;
	}
	return bComplete;
}

void INiagaraScriptImporter::Validate(UObject* Asset) const {
	FUObjectExportContainer* Container = GetContainer();

	if (Container == nullptr) return;

	/* A node the export gave pins for and that came out with none is a node nothing can reach, and
	 * it looks like an ordinary one until something follows a wire into it */
	int32 Nodes = 0;
	int32 Bare = 0;

	for (FUObjectExport* Export : Container->Exports) {
		const UEdGraphNode* Node = Cast<UEdGraphNode>(Export->Object);

		if (Node == nullptr) continue;

		Nodes++;

		const TArray<TSharedPtr<FJsonValue>>* Listed = nullptr;

		if (Export->IsJsonValid() && Export->JsonObject->TryGetArrayField(TEXT("Pins"), Listed)
			&& Listed->Num() > 0 && Node->Pins.Num() == 0) {
			Bare++;
		}
	}

	if (Bare > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			FString::Printf(TEXT("%d of %d nodes have no pins"), Bare, Nodes),
			FString::Printf(
				TEXT("'%s' has nodes the export gives pins for that came out with none, so nothing reaches them and nothing they fed is driven."),
				*GetAssetName())
		);
	}

	UE_LOG(LogReflection, Display, TEXT("\"%s\" carries %d node(s)%s"),
		*GetAssetName(), Nodes,
		Bare > 0 ? *FString::Printf(TEXT(", %d of them bare"), Bare) : TEXT(""));
}
