/* Copyright Reflection Contributors 2024-2026 */

#include "ReflectImportCommandlet.h"

#include "Engine/World.h"
#include "FileHelpers.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"

#include "Importers/Constructor/ImportReader.h"
#include "Importers/Constructor/Importer.h"
#include "Importers/Types/Texture/TextureImporter.h"
#include "Importers/Types/Texture/TextureTypes.h"
#include "Importers/Types/World/LevelRead.h"
#include "Modules/Cloud/Cloud.h"
#include "Importers/Constructor/Asset.h"
#include "Settings/Redirector.h"

#include "NiagaraCommon.h"
#include "NiagaraEmitter.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "Engine/SkeletalMesh.h"
#include "SkinnedAssetCompiler.h"
#include "Modules/Cloud/Tools/SkeletalMeshData.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Animation/SkinWeightProfile.h"
#include "Engine/SkeletalMeshSampling.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
/* 5.0 gathered what a save is told into a header of its own */
#if ENGINE_UE5
#include "UObject/SavePackage.h"
#endif

DECLARE_LOG_CATEGORY_CLASS(LogReflectImport, All, All);

/* Save the generated project packages. Loading a dependency can also dirty built-in engine assets. */
static int32 SaveDirtyPackages() {
	TArray<UPackage*> Dirty;
	FEditorFileUtils::GetDirtyContentPackages(Dirty);
	FEditorFileUtils::GetDirtyWorldPackages(Dirty);

	const FString EngineDirectory = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
	int32 Saved = 0;

	for (UPackage* One : Dirty) {
		/* A package holding a world is a map, and the engine only opens a map from a .umap */
		const FString& Extension = UWorld::FindWorldInPackage(One) != nullptr ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
		const FString FileName = FPackageName::LongPackageNameToFilename(One->GetName(), Extension);
		if (FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(FileName), EngineDirectory)) continue;

		if (!FAssetUtilities::IsAllowedDestination(One->GetName())) {
			UE_LOG(LogReflectImport, Warning, TEXT("not saving %s: outside %s"), *One->GetName(), *FAssetUtilities::OnlyUnder);
			continue;
		}

#if ENGINE_UE5
		/* A package saved empty once keeps PKG_ContainsNoAsset through every later save, which only ever
		 * adds it; the save sets it again if this one really holds no asset. */
		One->ClearPackageFlags(PKG_ContainsNoAsset);

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

		if (UPackage::SavePackage(One, nullptr, *FileName, SaveArgs)) {
#else
		if (UPackage::SavePackage(One, nullptr, RF_Public | RF_Standalone, *FileName)) {
#endif
			Saved++;
		} else {
			UE_LOG(LogReflectImport, Warning, TEXT("could not save %s"), *FileName);
		}
	}

	UE_LOG(LogReflectImport, Display, TEXT("%d package(s) saved"), Saved);

	return Saved;
}

/* The object path a cloud path names: <Project>/Content/A/B.uasset is /Game/A/B.B, and
 * .../<Plugin>/Content/A/B.uasset is /<Plugin>/A/B.B */
static FString ObjectPathFor(const FString& CloudPath) {
	FString Root;
	FString Rest;

	if (!CloudPath.Split(TEXT("/Content/"), &Root, &Rest)) {
		return FString();
	}

	Rest.RemoveFromEnd(TEXT(".uasset"));

	const FString Mount = Root.Contains(TEXT("/")) ? FPaths::GetCleanFilename(Root) : TEXT("Game");

	return TEXT("/") + Mount + TEXT("/") + Rest + TEXT(".") + FPaths::GetBaseFilename(Rest);
}

/* What compiling a Niagara system said, script by script, counted the way the system editor counts
 * it ("failed to compile with N errors"): every event of every script the system and its enabled
 * emitters compile. Waits for the compile first, so it is only worth calling on a system that was
 * asked to compile. Returns the number of errors. */
static int32 ReportNiagaraCompile(UNiagaraSystem* System) {
	if (System == nullptr) return 0;

	System->WaitForCompilationComplete(true, false);

	int32 Errors = 0;
	int32 Warnings = 0;
	int32 Failed = 0;

	auto One = [&](const FString& Where, const UNiagaraScript* Script) {
		if (Script == nullptr) return;

		const ENiagaraScriptCompileStatus Status = Script->GetLastCompileStatus();
		const TArray<FNiagaraCompileEvent>& Events = Script->GetVMExecutableData().LastCompileEvents;

		if (Status == ENiagaraScriptCompileStatus::NCS_Error) Failed++;

		UE_LOG(LogReflectImport, Display, TEXT("compile: %s, %s: %s, %d event(s)"),
			*Where, *Script->GetName(), *StaticEnum<ENiagaraScriptCompileStatus>()->GetNameStringByValue(static_cast<int64>(Status)), Events.Num());

		for (const FNiagaraCompileEvent& Event : Events) {
			const TCHAR* Severity = TEXT("Info");

			if (Event.Severity == FNiagaraCompileEventSeverity::Error) {
				Severity = TEXT("Error");
				Errors++;
			} else if (Event.Severity == FNiagaraCompileEventSeverity::Warning) {
				Severity = TEXT("Warning");
				Warnings++;
			}

			UE_LOG(LogReflectImport, Display, TEXT("compile:     [%s] %s"), Severity, *Event.Message);
		}
	};

	One(TEXT("System"), System->GetSystemSpawnScript());
	One(TEXT("System"), System->GetSystemUpdateScript());

	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles()) {
		const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();

		if (Data == nullptr) continue;

		if (!Handle.GetIsEnabled()) {
			UE_LOG(LogReflectImport, Display, TEXT("compile: %s is disabled, not compiled"), *Handle.GetName().ToString());

			continue;
		}

		TArray<UNiagaraScript*> Scripts;
		Data->GetScripts(Scripts, true);

		for (const UNiagaraScript* Script : Scripts) {
			One(Handle.GetName().ToString(), Script);
		}
	}

	UE_LOG(LogReflectImport, Display, TEXT("compile summary for %s: %d error(s), %d warning(s), %d script(s) failed"),
		*System->GetPathName(), Errors, Warnings, Failed);

	/* A light renderer reads the value its Color, Radius and Volumetric Scattering bindings fall back
	 * to each time it builds a scene proxy, and one without a value asserts (IsDataAllocated) as soon
	 * as the system is drawn. Counted here, since a commandlet never draws anything. */
	{
		UScriptStruct* BindingStruct = FNiagaraVariableAttributeBinding::StaticStruct();
		const FStructProperty* RootProperty = CastField<FStructProperty>(BindingStruct->FindPropertyByName(TEXT("RootVariable")));
		int32 Bindings = 0;
		int32 Empty = 0;
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles()) {
			const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
			if (Data == nullptr || RootProperty == nullptr) continue;
			for (const UNiagaraRendererProperties* Renderer : Data->GetRenderers()) {
				if (Renderer == nullptr) continue;
				/* The light renderer is the one that reads them: Color, Radius and VolumetricScattering */
				if (!Renderer->GetClass()->GetName().Contains(TEXT("LightRenderer"))) continue;
				for (TFieldIterator<FStructProperty> Held(Renderer->GetClass()); Held; ++Held) {
					if (Held->Struct != BindingStruct) continue;
					if (Held->GetName() != TEXT("ColorBinding") && Held->GetName() != TEXT("RadiusBinding") && Held->GetName() != TEXT("VolumetricScatteringBinding")) continue;
					const FNiagaraVariable* Root = RootProperty->ContainerPtrToValuePtr<FNiagaraVariable>(Held->ContainerPtrToValuePtr<void>(Renderer));
					if (Root->GetName().IsNone()) continue;
					Bindings++;
					if (!Root->IsDataAllocated()) {
						Empty++;
						UE_LOG(LogReflectImport, Warning, TEXT("binding without a default value: %s %s.%s (%s)"),
							*Handle.GetName().ToString(), *Renderer->GetName(), *Held->GetName(), *Root->GetName().ToString());
					}
				}
			}
		}
		UE_LOG(LogReflectImport, Display, TEXT("binding check for %s: %d binding(s), %d without a default value%s"),
			*System->GetPathName(), Bindings, Empty, Empty > 0 ? TEXT(" (drawing this system asserts)") : TEXT(""));
	}

	/* Validation that needs nothing named: every renderer binding that was pointed somewhere other
	 * than its class default has to read an attribute the emitter writes (otherwise it silently
	 * reads the default, the way a lost binding did), every function call needs a script or a
	 * signature, and every emitter needs its graph. */
	{
		UScriptStruct* BindingStruct = FNiagaraVariableAttributeBinding::StaticStruct();
		const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
		const auto& Compiled = System->GetEmitterCompiledData();
		int32 Custom = 0, Unwritten = 0, NoGraph = 0, NoFunction = 0;

		for (int32 Index = 0; Index < Handles.Num(); ++Index) {
			const FNiagaraEmitterHandle& Handle = Handles[Index];
			const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
			if (Data == nullptr) continue;

			const UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Data->GraphSource);
			if (Source == nullptr || Source->NodeGraph == nullptr) {
				NoGraph++;
				UE_LOG(LogReflectImport, Warning, TEXT("validation: %s has no graph"), *Handle.GetName().ToString());
			} else {
				for (const UEdGraphNode* Node : Source->NodeGraph->Nodes) {
					const UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Node);
					if (Call != nullptr && Call->GetClass() == UNiagaraNodeFunctionCall::StaticClass() && Call->FunctionScript == nullptr && !Call->Signature.IsValid()) {
						NoFunction++;
						UE_LOG(LogReflectImport, Warning, TEXT("validation: %s calls nothing at %s"), *Handle.GetName().ToString(), *Call->GetFunctionName());
					}
				}
			}

			if (!Handle.GetIsEnabled() || !Compiled.IsValidIndex(Index)) continue;
			const TArray<FNiagaraVariableBase>& Written = Compiled[Index]->DataSetCompiledData.Variables;

			for (const UNiagaraRendererProperties* Renderer : Data->GetRenderers()) {
				if (Renderer == nullptr || !Renderer->GetIsEnabled()) continue;
				const UObject* Defaults = Renderer->GetClass()->GetDefaultObject();
				for (TFieldIterator<FStructProperty> Held(Renderer->GetClass()); Held; ++Held) {
					if (Held->Struct != BindingStruct) continue;
					const auto* Binding = Held->ContainerPtrToValuePtr<FNiagaraVariableAttributeBinding>(Renderer);
					const auto* Default = Held->ContainerPtrToValuePtr<FNiagaraVariableAttributeBinding>(Defaults);
					if (!Binding->IsParticleBinding() || !Binding->IsValid()) continue;
					if (Binding->GetDataSetBindableVariable().GetName() == Default->GetDataSetBindableVariable().GetName()) continue;
					Custom++;
					const FName Attribute = Binding->GetDataSetBindableVariable().GetName();
					/* A Previous.X binding is for motion vectors; with X written and no Previous.X the
					 * renderer uses X, so it reads nothing missing */
					FString Current = Attribute.ToString();
					const bool bPrevious = Current.RemoveFromStart(TEXT("Previous."));
					const auto Writes = [&Written](const FName Name) { return Written.ContainsByPredicate([Name](const FNiagaraVariableBase& Var) { return Var.GetName() == Name; }); };
					if (!Writes(Attribute) && !(bPrevious && Writes(FName(*Current)))) {
						Unwritten++;
						UE_LOG(LogReflectImport, Warning, TEXT("validation: %s %s.%s reads Particles.%s, which the emitter never writes"),
							*Handle.GetName().ToString(), *Renderer->GetName(), *Held->GetName(), *Attribute.ToString());
					}
				}
			}
		}

		UE_LOG(LogReflectImport, Display, TEXT("validation for %s: %d custom binding(s), %d reading an attribute nothing writes, %d emitter(s) without a graph, %d call(s) to nothing"),
			*System->GetPathName(), Custom, Unwritten, NoGraph, NoFunction);
	}

	return Errors;
}

/* What a skeletal mesh can be sampled with: per LOD, the triangles its render data has against the
 * whole-mesh area-weighted sampler built for it (Niagara's uniformly distributed sampling reads
 * that sampler, and only for a LOD that has bSupportUniformlyDistributedSampling), plus the named
 * sampling regions against the region data built for them. */
static int32 ReportSkeletalMeshSampling(const USkeletalMesh* Mesh) {
	if (Mesh == nullptr) return 0;
	const FSkeletalMeshSamplingInfo& Sampling = Mesh->GetSamplingInfo();
	const FSkeletalMeshSamplingBuiltData& Built = Sampling.GetBuiltData();
	const FSkeletalMeshRenderData* Render = Mesh->GetResourceForRendering();
	const int32 Lods = Mesh->GetLODNum();
	int32 Uniform = 0;
	int32 Unbuilt = 0;
	for (int32 Lod = 0; Lod < Lods; ++Lod) {
		const FSkeletalMeshLODInfo* Info = Mesh->GetLODInfo(Lod);
		const bool bUniform = Info != nullptr && Info->bSupportUniformlyDistributedSampling;
		const int32 Triangles = Render != nullptr && Render->LODRenderData.IsValidIndex(Lod) ? Render->LODRenderData[Lod].GetTotalFaces() : -1;
		const bool bHasSampler = Built.WholeMeshBuiltData.IsValidIndex(Lod);
		const int32 Entries = bHasSampler ? Built.WholeMeshBuiltData[Lod].AreaWeightedTriangleSampler.GetNumEntries() : -1;
		Uniform += bUniform ? 1 : 0;
		const bool bMissing = bUniform && Entries != Triangles;
		Unbuilt += bMissing ? 1 : 0;
		UE_LOG(LogReflectImport, Display, TEXT("sampling: %s LOD %d: %d triangle(s), uniform sampling %s, whole-mesh sampler %s"),
			*Mesh->GetName(), Lod, Triangles, bUniform ? TEXT("on") : TEXT("off"),
			bHasSampler ? *FString::Printf(TEXT("%d entr%s, weight %.3f"), Entries, Entries == 1 ? TEXT("y") : TEXT("ies"),
				Built.WholeMeshBuiltData[Lod].AreaWeightedTriangleSampler.GetTotalWeight()) : TEXT("not built"));
	}
	UE_LOG(LogReflectImport, Display, TEXT("sampling check for %s: %d LOD(s), %d with uniform sampling, %d whose sampler doesn't cover its triangles; %d region(s), %d built"),
		*Mesh->GetPathName(), Lods, Uniform, Unbuilt, Sampling.Regions.Num(), Built.RegionBuiltData.Num());

	/* Skin weight profiles: the entries the mesh lists, against the weights each imported LOD holds */
#if WITH_EDITORONLY_DATA
	TArray<FString> Profiles;
	for (const FSkinWeightProfileInfo& Info : Mesh->GetSkinWeightProfiles()) Profiles.Add(Info.Name.ToString());
	int32 WithWeights = 0;
	if (const FSkeletalMeshModel* Imported = const_cast<USkeletalMesh*>(Mesh)->GetImportedModel()) {
		for (int32 Lod = 0; Lod < Imported->LODModels.Num(); ++Lod) {
			const FSkeletalMeshLODModel& Model = Imported->LODModels[Lod];
			for (const TPair<FName, FImportedSkinWeightProfileData>& Pair : Model.SkinWeightProfiles) {
				WithWeights++;
				/* Every vertex's influences have to add up to full weight, or it skins short */
				int32 Short = 0;
				int32 Differing = 0;
				for (int32 Vertex = 0; Vertex < Pair.Value.SkinWeights.Num(); ++Vertex) {
					const FRawSkinWeight& Weight = Pair.Value.SkinWeights[Vertex];
					uint32 Sum = 0;
					for (int32 Influence = 0; Influence < MAX_TOTAL_INFLUENCES; ++Influence) Sum += Weight.InfluenceWeights[Influence];
					Short += Sum == TNumericLimits<uint16>::Max() ? 0 : 1;

					/* Against the LOD's own skinning: what the profile actually changes */
					int32 Section = INDEX_NONE, InSection = INDEX_NONE;
					Model.GetSectionFromVertexIndex(Vertex, Section, InSection);
					if (!Model.Sections.IsValidIndex(Section) || !Model.Sections[Section].SoftVertices.IsValidIndex(InSection)) continue;
					const FSoftSkinVertex& Own = Model.Sections[Section].SoftVertices[InSection];
					for (int32 Influence = 0; Influence < MAX_TOTAL_INFLUENCES; ++Influence) {
						if (Own.InfluenceWeights[Influence] != Weight.InfluenceWeights[Influence]
							|| (Weight.InfluenceWeights[Influence] > 0 && Own.InfluenceBones[Influence] != Weight.InfluenceBones[Influence])) {
							Differing++;
							break;
						}
					}
				}
				UE_LOG(LogReflectImport, Display, TEXT("skin weights: %s LOD %d: profile %s, %d weight(s) for %d vertices, %d not summing to full, %d differing from the LOD's own skinning"),
					*Mesh->GetName(), Lod, *Pair.Key.ToString(), Pair.Value.SkinWeights.Num(), Model.NumVertices, Short, Differing);
			}
		}
	}
	UE_LOG(LogReflectImport, Display, TEXT("skin weight check for %s: %d profile(s) listed (%s), %d LOD profile(s) with weights"),
		*Mesh->GetPathName(), Profiles.Num(), *FString::Join(Profiles, TEXT(", ")), WithWeights);
#endif
	return Unbuilt;
}

int32 UReflectImportCommandlet::Main(const FString& Params) {
	/* Loads a skeletal mesh already in the project, the way a fresh editor session does, and says
	 * what it can be sampled with: -meshcheck=/Mount/Path/Mesh */
	if (FString Checked; FParse::Value(*Params, TEXT("meshcheck="), Checked) && !Checked.IsEmpty()) {
		if (!Checked.Contains(TEXT("."))) Checked += TEXT(".") + FPackageName::GetShortName(Checked);
		USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *Checked);
		if (Mesh == nullptr) {
			UE_LOG(LogReflectImport, Error, TEXT("there is no skeletal mesh at \"%s\""), *Checked);
			return 1;
		}
		ReportSkeletalMeshSampling(Mesh);

		/* -samplingprobe: uniform sampling switched on for every LOD in memory and the sampling data
		 * rebuilt the way an import rebuilds it, to see the samplers come out covering the triangles.
		 * Nothing is saved. */
		if (FParse::Param(*Params, TEXT("samplingprobe"))) {
			for (int32 Lod = 0; Lod < Mesh->GetLODNum(); ++Lod) {
				if (FSkeletalMeshLODInfo* Info = Mesh->GetLODInfo(Lod)) Info->bSupportUniformlyDistributedSampling = true;
			}
			TSkeletalMeshData::RebuildSamplingData(Mesh);
			ReportSkeletalMeshSampling(Mesh);
		}

		/* -rebuildprobe: a LOD build setting changed on every LOD, so the editor builds each LOD
		 * again from its import data, the way editing a LOD setting does, and what came out is
		 * reported. Nothing is saved. */
		if (FParse::Param(*Params, TEXT("rebuildprobe"))) {
			for (int32 Lod = 0; Lod < Mesh->GetLODNum(); ++Lod) {
				if (FSkeletalMeshLODInfo* Info = Mesh->GetLODInfo(Lod)) Info->BuildSettings.bUseHighPrecisionTangentBasis = !Info->BuildSettings.bUseHighPrecisionTangentBasis;
			}
			UE_LOG(LogReflectImport, Display, TEXT("rebuild probe: LOD build settings changed, rebuilding %s from its import data"), *Mesh->GetName());
			Mesh->Build();
			TArray<USkinnedAsset*> Building{Mesh};
			FSkinnedAssetCompilingManager::Get().FinishCompilation(Building);
			ReportSkeletalMeshSampling(Mesh);
		}
		return 0;
	}

	/* Compiles a Niagara system already in the project and says what the compile said, without
	 * importing or saving anything: -compilecheck=/Mount/Path/System */
	if (FString Checked; FParse::Value(*Params, TEXT("compilecheck="), Checked) && !Checked.IsEmpty()) {
		if (!Checked.Contains(TEXT("."))) {
			Checked += TEXT(".") + FPackageName::GetShortName(Checked);
		}

		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *Checked);

		if (System == nullptr) {
			UE_LOG(LogReflectImport, Error, TEXT("there is no Niagara system at \"%s\""), *Checked);

			return 1;
		}

		System->RequestCompile(true);
		ReportNiagaraCompile(System);

		return 0;
	}

	FString Path;

	if (!FParse::Value(*Params, TEXT("path="), Path) || Path.IsEmpty()) {
		UE_LOG(LogReflectImport, Error, TEXT("nothing to import: give it -path=<cloud asset path>"));

		return 1;
	}
	/* Nothing is created or saved outside this package path. Checked before anything is fetched
	 * against where the asset asked for lands, so a redirect that doesn't take turns the run down
	 * rather than writing over the project's own copy. */
	if (FString Under; FParse::Value(*Params, TEXT("onlyunder="), Under) && !Under.IsEmpty()) {
		FAssetUtilities::OnlyUnder = Under;

		const FString ObjectPath = ObjectPathFor(Path);
		const FString Lands = FRRedirects::Preview(FPackageName::ObjectPathToPackageName(ObjectPath)).Path;

		if (ObjectPath.IsEmpty() || !FAssetUtilities::IsAllowedDestination(Lands)) {
			UE_LOG(LogReflectImport, Error, TEXT("\"%s\" would land at \"%s\", which is not under %s: not importing"), *Path, *Lands, *Under);

			return 1;
		}

		UE_LOG(LogReflectImport, Display, TEXT("\"%s\" lands at \"%s\", under %s"), *Path, *Lands, *Under);
	}

	FImportIssues::Begin();
	ON_SCOPE_EXIT {
		FImportIssues::Finish();
		for (const TSharedPtr<FImportIssueAsset>& Asset : FImportIssues::GetAssets()) {
			UE_LOG(LogReflectImport, Display, TEXT("  %s"), *Asset->Name);

			for (const TSharedPtr<FImportIssue>& Issue : Asset->Issues) {
				UE_LOG(LogReflectImport, Display, TEXT("    [%s] %s%s%s"),
					*GetImportIssueText(Issue->Kind).ToString(),
					*Issue->Summary,
					Issue->Count > 1 ? *FString::Printf(TEXT(" (x%d)"), Issue->Count) : TEXT(""),
					Issue->Detail.IsEmpty() ? TEXT("") : *(TEXT(": ") + Issue->Detail));
			}
		}
	};

	/* A profile Mirror has mounted but is not showing, named by its file id. Every request this run makes is
	 * pinned to it, the way picking it in the profiles panel pins the editor's */
	if (FString Pinned; FParse::Value(*Params, TEXT("build="), Pinned) && !Pinned.IsEmpty()) {
		Cloud::Build = Pinned;

		UE_LOG(LogReflectImport, Display, TEXT("reading from profile %s"), *Pinned);
	}

	/* Who the exports belong to, which every path read out of them is said in terms of.
	 *
	 * The editor asks for this when it connects, so anything importing there already knows it. A
	 * commandlet connects to nothing and asks for one asset, and without the name none of the
	 * paths inside can be turned into the ones the editor uses. */
	if (!Cloud::EnsureMetadataBlocking()) {
		UE_LOG(LogReflectImport, Error, TEXT("the cloud would not say what project it is serving"));

		return 1;
	}

	TSharedPtr<FJsonObject> Response;

	/* Read out of a file rather than off the cloud.
	 *
	 * What the cloud gives for a path is the same every time, which is what makes it worth reading
	 * and no use for trying anything. Kept with -json and read back with -from, an export can be
	 * changed and the change put through the same importer: an asset whose class this engine has
	 * not got is reparented to one it has, and everything else about it is read the way it always
	 * would be. The path is still said, since that is where what comes out is written. */
	if (FString From; FParse::Value(*Params, TEXT("from="), From) && !From.IsEmpty()) {
		FString Held;

		if (!FFileHelper::LoadFileToString(Held, *From)) {
			UE_LOG(LogReflectImport, Error, TEXT("there is nothing to read at \"%s\""), *From);

			return 1;
		}

		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Held), Response) || !Response.IsValid()) {
			UE_LOG(LogReflectImport, Error, TEXT("\"%s\" is not something that reads as json"), *From);

			return 1;
		}

		UE_LOG(LogReflectImport, Display, TEXT("reading \"%s\" rather than asking the cloud"), *From);
	} else {
		Response = Cloud::Export::GetRawBlocking(Path);
	}

	if (!Response.IsValid() || !Response->HasField(TEXT("exports"))) {
		UE_LOG(LogReflectImport, Error, TEXT("there was nothing at \"%s\""), *Path);

		return 1;
	}

	const TArray<TSharedPtr<FJsonValue>> Exports = Response->GetArrayField(TEXT("exports"));

	UE_LOG(LogReflectImport, Display, TEXT("importing \"%s\", %d export(s)"), *Path, Exports.Num());

	/* What came back, kept where it was asked for, since what the cloud gave is the only thing the
	 * result is worth checking against */
	if (FString Dump; FParse::Value(*Params, TEXT("json="), Dump) && !Dump.IsEmpty()) {
		FString Written;

		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Written);

		if (FJsonSerializer::Serialize(Response.ToSharedRef(), Writer) && FFileHelper::SaveStringToFile(Written, *Dump)) {
			UE_LOG(LogReflectImport, Display, TEXT("what the cloud gave back was kept at \"%s\""), *Dump);
		}
	}

	/* A level writes no package, so there is nothing below for it to say anything about */
	if (FLevelRead::Handles(Exports)) {
		FRLevelReadResult Result;

		if (!FLevelRead::FromCloud(Exports, Path, &Result)) {
			UE_LOG(LogReflectImport, Error, TEXT("nothing was read from \"%s\""), *Path);

			return 1;
		}

		UE_LOG(LogReflectImport, Display, TEXT("%d placed, %d missing (%d blueprint, %d class, %d refused), %d left out"),
			Result.Placed, Result.Missing(), Result.MissingBlueprints, Result.UnusableClasses, Result.Failed, Result.LeftOut);

		/* The level and every blueprint fetched to place its actors are left dirty when Save Assets is off,
		 * and a commandlet has no editor to save them later */
		SaveDirtyPackages();

		return 0;
	}

	/* No package import builds a texture: one is only ever made as the reference of something else.
	 * Asked for on its own, it is fetched the way that reference would fetch it */
	if (const TSharedPtr<FJsonObject> First = Exports.Num() > 0 ? Exports[0]->AsObject() : nullptr; First.IsValid() && FTextureTypes::IsSupported(First->GetStringField(TEXT("Type")))) {
		UTexture* Texture = nullptr;

		if (!FTextureImport::FromResponse(Response, ObjectPathFor(Path), Path, Texture) || Texture == nullptr) {
			UE_LOG(LogReflectImport, Error, TEXT("nothing was made from \"%s\""), *Path);

			return 1;
		}

		SaveDirtyPackages();

		return 0;
	}

	IImporter* Importer = nullptr;

	const bool bImported = IImportReader::ReadExportsAndImport(Exports, Path, Importer, true, false);

	if (!bImported || Importer == nullptr) {
		UE_LOG(LogReflectImport, Error, TEXT("nothing was made from \"%s\""), *Path);

		return 1;
	}

	/* What the package ended up holding, and how much of any graph in it is wired */
	UPackage* Package = Importer->GetPackage();

	if (Package != nullptr) {
		TArray<UObject*> Held;
		GetObjectsWithOuter(Package, Held, true);

		TMap<FName, int32> Kinds;

		int32 Graphs = 0;
		int32 Nodes = 0;
		int32 Pins = 0;
		int32 Links = 0;

		for (UObject* Object : Held) {
			if (Object == nullptr) continue;

			Kinds.FindOrAdd(Object->GetClass()->GetFName())++;

			const UEdGraph* Graph = Cast<UEdGraph>(Object);

			if (Graph == nullptr) continue;

			Graphs++;

			for (const UEdGraphNode* Node : Graph->Nodes) {
				if (Node == nullptr) continue;

				Nodes++;

				for (const UEdGraphPin* Pin : Node->Pins) {
					if (Pin == nullptr) continue;

					Pins++;

					Links += Pin->LinkedTo.Num();
				}
			}
		}

		UE_LOG(LogReflectImport, Display, TEXT("\"%s\" holds %d object(s) of %d kind(s)"),
			*Package->GetName(), Held.Num(), Kinds.Num());

		Kinds.ValueSort([](const int32 A, const int32 B) { return A > B; });

		for (const TPair<FName, int32>& Kind : Kinds) {
			UE_LOG(LogReflectImport, Display, TEXT("    %4d  %s"), Kind.Value, *Kind.Key.ToString());
		}

		UE_LOG(LogReflectImport, Display, TEXT("%d graph(s), %d node(s), %d pin(s), %d link(s)"),
			Graphs, Nodes, Pins, Links);

		/* The same again over every node in the package rather than only the ones a graph lists, so
		 * a node a graph forgot shows up as the difference between the two */
		int32 Loose = 0;
		int32 LoosePins = 0;
		int32 LooseLinks = 0;

		for (UObject* Object : Held) {
			const UEdGraphNode* Node = Cast<UEdGraphNode>(Object);

			if (Node == nullptr) continue;

			Loose++;

			for (const UEdGraphPin* Pin : Node->Pins) {
				if (Pin == nullptr) continue;

				LoosePins++;

				LooseLinks += Pin->LinkedTo.Num();
			}
		}

		UE_LOG(LogReflectImport, Display, TEXT("every node: %d node(s), %d pin(s), %d link(s)"),
			Loose, LoosePins, LooseLinks);
	}

	/* What was asked for is written whether or not its importer remembered to dirty it; the rig importer does not */
	if (Package != nullptr) {
		Package->MarkPackageDirty();
	}

	SaveDirtyPackages();

	/* A Niagara system has compiled by now (saving one waits for it), so what that said is here to read */
	if (Package != nullptr) {
		TArray<UObject*> Held;
		GetObjectsWithOuter(Package, Held, false);

		for (UObject* Object : Held) {
			if (UNiagaraSystem* System = Cast<UNiagaraSystem>(Object)) {
				ReportNiagaraCompile(System);
			}
			if (const USkeletalMesh* Mesh = Cast<USkeletalMesh>(Object)) {
				ReportSkeletalMeshSampling(Mesh);
			}
		}
	}

	return 0;
}
