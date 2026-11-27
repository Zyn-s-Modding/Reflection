/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Cloud/Tools/SkeletalMeshData.h"

#include "Engine/EngineUtilities.h"

#include "Dom/JsonObject.h"
#include "Animation/AnimSequence.h"

#include "Engine/SkeletalMeshSocket.h"

/* Profiles are two halves: the entries on the mesh, the weights on the imported model */
#if UE4_27_AND_UE5
#if UE5_8_BEYOND
#include "Rendering/SkinWeightProfile.h"
#else
#include "Animation/SkinWeightProfile.h"
#endif
#include "GPUSkinPublicDefs.h"
#include "Modules/Cloud/Cloud.h"
#include "Importers/Constructor/ImportIssues.h"
#include "Rendering/SkeletalMeshLODImporterData.h"
#include "Settings/Redirector.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "SkinnedAssetCompiler.h"
#include "Engine/SkeletalMeshSampling.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#endif

/* The skinned-asset split that created this header landed in 5.1; before that the types it
 * carries (FSkeletalMaterial and friends) still come in with SkeletalMesh.h */
#if UE5_1_BEYOND
#include "Engine/SkinnedAssetCommon.h"
#endif

#include "EditorFramework/AssetImportData.h"
#include "Importers/Constructor/Importer.h"
#include "Utilities/JsonHelpers.h"

#if ENGINE_UE5
#include "Animation/AnimData/IAnimationDataController.h"
#if ENGINE_MINOR_VERSION >= 4
#include "Animation/AnimData/IAnimationDataModel.h"
#endif
#include "AnimDataController.h"
#endif

void TSkeletalMeshData::Process(UObject* Object, const TArray<TSharedPtr<FJsonValue>>& Exports) {
	USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Object);
	if (SkeletalMesh == nullptr) return;

	for (const TSharedPtr<FJsonValue>& Export : Exports) {
		if (!Export.IsValid() || !Export->AsObject().IsValid()) {
			continue;
		}

		const TSharedPtr<FJsonObject> JsonObject = Export->AsObject();
		if (!IsProperExportData(JsonObject)) continue;

		TSharedPtr<FJsonObject> Properties = JsonObject->GetObjectField(TEXT("Properties"));
		const FString Type = JsonObject->GetStringField(TEXT("Type"));
		const FString Name = JsonObject->GetStringField(TEXT("Name"));

		if (Name != Object->GetName()) continue;

		if (Type == "SkeletalMesh") {
			TArray<TSharedPtr<FJsonValue>> SkeletalMaterials = JsonObject->GetArrayField(TEXT("SkeletalMaterials"));

			TArray<FSkeletalMaterial>& Materials = GetMaterials(SkeletalMesh);

			int SkeletalMaterialIndex = 0;

			IImporter Importer;

			for (const TSharedPtr<FJsonValue>& SkeletalMaterialExport : SkeletalMaterials) {
				if (!SkeletalMaterialExport.IsValid() || !SkeletalMaterialExport->AsObject().IsValid()) {
					continue;
				}

				const TSharedPtr<FJsonObject> SkeletalMaterialObject = SkeletalMaterialExport->AsObject();

				if (Materials.IsValidIndex(SkeletalMaterialIndex)) {
					FSkeletalMaterial& MaterialSlot = Materials[SkeletalMaterialIndex];

					/* Only a name worth having: a cooked slot is called None, and letting that back
					 * over the one the import gave it puts every slot under the same name again,
					 * which is what a rebuild reads to decide which slot a section draws with. */
					const FString ExportedSlotName = SkeletalMaterialObject->GetStringField(TEXT("MaterialSlotName"));

					if (!ExportedSlotName.IsEmpty() && ExportedSlotName != TEXT("None")) {
						MaterialSlot.MaterialSlotName = FName(*ExportedSlotName);
						MaterialSlot.ImportedMaterialSlotName = MaterialSlot.MaterialSlotName;
					}

					TSharedPtr<FJsonObject> SkeletalMaterial = SkeletalMaterialObject->GetObjectField(TEXT("Material"));

					TObjectPtr<UObject> LoadedObject;
					Importer.LoadExport<UObject>(&SkeletalMaterial, LoadedObject);

					if (IsObjectPtrValid(LoadedObject)) MaterialSlot.MaterialInterface = Cast<UMaterialInterface>(LoadedObject.Get());
				} else break;

				SkeletalMaterialIndex++;
			}

			if (Properties->HasField(TEXT("LODInfo"))) {
				TArray<TSharedPtr<FJsonValue>> LODInfo = Properties->GetArrayField(TEXT("LODInfo"));

				for (const TSharedPtr<FJsonValue>& LOD : LODInfo) {
					if (!LOD.IsValid() || !LOD->AsObject().IsValid()) {
						continue;
					}

					const TSharedPtr<FJsonObject> LODObject = LOD->AsObject();
					if (!LODObject->HasField(TEXT("SourceImportFilename"))) continue;

					FString SourceImportFilename = LODObject->GetStringField(TEXT("SourceImportFilename"));
					if (SourceImportFilename.IsEmpty()) continue;

					SetAssetImportData(SkeletalMesh, NewObject<UAssetImportData>(SkeletalMesh, TEXT("AssetImportData")));
					GetAssetImportData(SkeletalMesh)->SourceData.SourceFiles.Add(SourceImportFilename);
				}
			}

			/* Create an object serializer */
			GetObjectSerializer()->ExportsToNotDeserialize.Empty();
			GetObjectSerializer()->SetExportForDeserialization(JsonObject, SkeletalMesh);
			GetObjectSerializer()->Parent = SkeletalMesh;
			
			SkeletalMesh->GetMeshOnlySocketList().Empty();

			FUObjectExportContainer* Container = new FUObjectExportContainer(Exports);
			GetObjectSerializer()->DeserializeExports(Container);

			for (const FUObjectExport* UObjectExport : GetObjectSerializer()->GetPropertySerializer()->ExportsContainer->Exports) {
				if (USkeletalMeshSocket* Socket = Cast<USkeletalMeshSocket>(UObjectExport->Object)) {
					SkeletalMesh->GetMeshOnlySocketList().Add(Socket);
				}
			}

			/* MeshClothingAssets is TClothingData's job: binding is destructive */
			GetObjectSerializer()->DeserializeObjectProperties(KeepPropertiesShared(Properties, {
				"PhysicsAsset",
				"PostProcessAnimBlueprint",
				"ShadowPhysicsAsset",
				"PositiveBoundsExtension",
				"NegativeBoundsExtension",
				"MorphTargets",

				"Sockets",
				"SamplingInfo",
				"LODModels",
				"NaniteResources",

				/* Entries only. The weights behind them are not exported. */
				"SkinWeightProfiles",
			}), SkeletalMesh);

			const int32 SectionsChanged = ApplySectionUserData(SkeletalMesh, JsonObject);
			/* The game's path for the mesh, from the export's own header: the mesh's path here is
			 * wherever the redirects put it, and the Cloud has nothing there */
			FString SourcePath;
			if (JsonObject->TryGetStringField(TEXT("Package"), SourcePath) && !SourcePath.IsEmpty()) {
				SourcePath += TEXT(".") + Name;
			} else {
				SourcePath = SkeletalMesh->GetPathName();
				FRRedirects::Reverse(SourcePath);
			}
			const int32 SkinWeightProfiles = ApplySkinWeightProfiles(SkeletalMesh, SourcePath);

			ReportSkinWeightProfiles(SkeletalMesh);

			SkeletalMesh->Modify();

			/* Both are read when the render data is built, so the mesh has to go around again */
			if (SectionsChanged > 0 || Properties->HasField(TEXT("SkinWeightProfiles"))) {
				/* Section flags move the key on their own, skin weights do not */
				if (SkinWeightProfiles > 0) {
					InvalidateSkeletalMeshDerivedData(SkeletalMesh);
				}

				SkeletalMesh->PostEditChange();
			}

			RebuildSamplingData(SkeletalMesh);

			BrowseToWhenFinished(SkeletalMesh);
		}
	}
}

int32 TSkeletalMeshData::ApplySectionUserData(USkeletalMesh* Mesh, const TSharedPtr<FJsonObject>& MeshExport) {
#if UE4_27_AND_UE5 && WITH_EDITORONLY_DATA
	FSkeletalMeshModel* ImportedModel = Mesh->GetImportedModel();
	if (ImportedModel == nullptr) return 0;

	const TArray<TSharedPtr<FJsonValue>>* LodModels;
	if (!MeshExport->TryGetArrayField(TEXT("LODModels"), LodModels)) return 0;

	int32 AppliedSections = 0;

	for (int32 LodIndex = 0; LodIndex < LodModels->Num(); ++LodIndex) {
		if (!ImportedModel->LODModels.IsValidIndex(LodIndex)) break;

		const TSharedPtr<FJsonObject> LodModelJson = (*LodModels)[LodIndex]->AsObject();
		if (!LodModelJson.IsValid()) continue;

		const TArray<TSharedPtr<FJsonValue>>* Sections;
		if (!LodModelJson->TryGetArrayField(TEXT("Sections"), Sections)) continue;

		FSkeletalMeshLODModel& LodModel = ImportedModel->LODModels[LodIndex];

		for (int32 SectionIndex = 0; SectionIndex < Sections->Num(); ++SectionIndex) {
			if (!LodModel.Sections.IsValidIndex(SectionIndex)) break;

			const TSharedPtr<FJsonObject> SectionJson = (*Sections)[SectionIndex]->AsObject();
			if (!SectionJson.IsValid()) continue;

			FSkelMeshSection& Section = LodModel.Sections[SectionIndex];

			/* UserSectionsData is the copy that lasts: a build syncs it back over the sections,
			 * and the derived data key is hashed from it. The section is written too so the
			 * change shows without waiting for that build. */
			FSkelMeshSourceSectionUserData& SectionUserData = LodModel.UserSectionsData.FindOrAdd(Section.OriginalDataSectionIndex);

			bool bFlag = false;

			/* A cloth sim cage ships as a disabled section, so without this it draws */
			if (SectionJson->TryGetBoolField(TEXT("bDisabled"), bFlag)) {
				Section.bDisabled = bFlag;
				SectionUserData.bDisabled = bFlag;
			}

			if (SectionJson->TryGetBoolField(TEXT("bCastShadow"), bFlag)) {
				Section.bCastShadow = bFlag;
				SectionUserData.bCastShadow = bFlag;
			}

			if (SectionJson->TryGetBoolField(TEXT("bRecomputeTangent"), bFlag)) {
				Section.bRecomputeTangent = bFlag;
				SectionUserData.bRecomputeTangent = bFlag;
			}

#if UE5_1_BEYOND
			if (SectionJson->TryGetBoolField(TEXT("bVisibleInRayTracing"), bFlag)) {
				Section.bVisibleInRayTracing = bFlag;
				SectionUserData.bVisibleInRayTracing = bFlag;
			}
#endif

			int32 GenerateUpToLodIndex = INDEX_NONE;

			if (SectionJson->TryGetNumberField(TEXT("GenerateUpToLodIndex"), GenerateUpToLodIndex)) {
				Section.GenerateUpToLodIndex = GenerateUpToLodIndex;
				SectionUserData.GenerateUpToLodIndex = GenerateUpToLodIndex;
			}

			/* Cloth's section fields are left to binding, which owns them */

			AppliedSections++;
		}
	}

	return AppliedSections;
#else
	return 0;
#endif
}

void TSkeletalMeshData::RebuildSamplingData(USkeletalMesh* Mesh) {
#if WITH_EDITORONLY_DATA
	/* Sampling data is built data: for each LOD a whole-mesh area-weighted triangle sampler, for each
	 * named region its triangles, vertices, bones and sampler, all made from the mesh's triangles.
	 * The samplers aren't properties, so the source's copy deserializes to empty ones, and the
	 * triangles it indexes are the game's, which the rebuilt LODs needn't match one for one. So what
	 * the properties left (the region definitions and the per-LOD uniform sampling switch, both
	 * carried) is built again by the engine itself, against the mesh as it now is. */
	if (Mesh == nullptr) return;
#if WITH_EDITOR
	TArray<USkinnedAsset*> Building{Mesh};
	FSkinnedAssetCompilingManager::Get().FinishCompilation(Building);
#endif
	const FSkeletalMeshRenderData* Render = Mesh->GetResourceForRendering();
	if (Render == nullptr || Render->LODRenderData.IsEmpty()) {
		FImportIssues::Report(EImportIssue::Data, TEXT("Skeletal mesh sampling data could not be rebuilt"),
			FString::Printf(TEXT("%s has no render data to build it from"), *Mesh->GetName()));
		return;
	}

	FSkeletalMeshSamplingInfo Sampling = Mesh->GetSamplingInfo();
	Sampling.BuildRegions(Mesh);
	Sampling.BuildWholeMesh(Mesh);
	Mesh->SetSamplingInfo(Sampling);

	int32 Uniform = 0;
	int32 Sampled = 0;
	const FSkeletalMeshSamplingBuiltData& Built = Mesh->GetSamplingInfo().GetBuiltData();
	for (int32 Lod = 0; Lod < Built.WholeMeshBuiltData.Num(); ++Lod) {
		const FSkeletalMeshLODInfo* Info = Mesh->GetLODInfo(Lod);
		if (Info == nullptr || !Info->bSupportUniformlyDistributedSampling) continue;
		Uniform++;
		Sampled += Built.WholeMeshBuiltData[Lod].AreaWeightedTriangleSampler.GetNumEntries();
	}
	UE_LOG(LogReflection, Display, TEXT("\"%s\" sampling data rebuilt by the engine: %d LOD(s), %d with uniform sampling (%d triangle(s) in their samplers), %d region(s)"),
		*Mesh->GetName(), Built.WholeMeshBuiltData.Num(), Uniform, Sampled, Built.RegionBuiltData.Num());
#endif
}

void TSkeletalMeshData::WriteProfileToImportData(USkeletalMesh* Mesh, int32 LodIndex, FName ProfileName, int32 SourceVertices,
	const TMap<int32, TArray<TPair<int32, float>>>& Influences) {
#if UE4_27_AND_UE5 && WITH_EDITORONLY_DATA
	/* The LOD model's profile is what the next build is made from only while nothing rebuilds the
	 * LOD from its import data. A build that does (a LOD setting changed, the mesh reimported)
	 * makes each profile again from the import data's alternate skinning, and a profile with none
	 * is dropped ("the alternate skinning imported source data is not available"). So the profile
	 * goes into the import data too: the LOD's own points, wedges and faces, with the influences of
	 * the vertices it overrides replaced by its own. The import data was written one point per
	 * cooked vertex, which is what the overrides name, so no mapping is needed here. */
	if (Mesh->IsLODImportedDataEmpty(LodIndex)) return;

	FSkeletalMeshImportData Base;
	Mesh->LoadLODImportedData(LodIndex, Base);
	if (Base.Points.Num() != SourceVertices) {
		FImportIssues::Report(EImportIssue::Data, TEXT("A skin weight profile could not be written into a LOD's import data"),
			FString::Printf(TEXT("%s LOD%d: the import data has %d points against %d cooked vertices, so a rebuild from it may drop the profile"),
				*ProfileName.ToString(), LodIndex, Base.Points.Num(), SourceVertices));
		return;
	}

	/* Laid out the way an imported alternate mesh is: one point per position. The engine lines the
	 * alternate skinning up with the LOD by matching positions (MatchVertexIndexUsingPosition) and
	 * then needs every alternate point to have been matched; the import data has a point per
	 * cooked vertex, so a seam leaves two points at one position, the match takes one, and the
	 * other is looked for and not found (asserts in FLODUtilities::UpdateAlternateSkinWeights). So
	 * points at the same position are one point here, the wedges point at it, and it takes the
	 * weights of the vertices there: the profile's where the profile overrides any of them. Only
	 * points a triangle uses come in, since the rest made no vertex to match. */
	TBitArray<> Used(false, Base.Points.Num());
	for (const SkeletalMeshImportData::FVertex& Wedge : Base.Wedges) {
		if (Used.IsValidIndex(Wedge.VertexIndex)) Used[Wedge.VertexIndex] = true;
	}

	TMap<FVector3f, int32> PointAt;
	TArray<int32> MergedOf;
	MergedOf.Init(INDEX_NONE, Base.Points.Num());
	TArray<int32> SourceOf;
	for (int32 Point = 0; Point < Base.Points.Num(); ++Point) {
		if (!Used[Point]) continue;
		if (const int32* Merged = PointAt.Find(Base.Points[Point])) {
			MergedOf[Point] = *Merged;
			/* The merged point skins like the profile wherever the profile overrides one of them */
			if (Influences.Contains(Point) && !Influences.Contains(SourceOf[*Merged])) SourceOf[*Merged] = Point;
			continue;
		}
		MergedOf[Point] = SourceOf.Num();
		PointAt.Add(Base.Points[Point], SourceOf.Num());
		SourceOf.Add(Point);
	}

	FSkeletalMeshImportData Alternate = Base;
	Alternate.AlternateInfluences.Empty();
	Alternate.AlternateInfluenceProfileNames.Empty();
	Alternate.MorphTargets.Empty();
	Alternate.MorphTargetModifiedPoints.Empty();
	Alternate.MorphTargetNames.Empty();
	Alternate.Points.Reset(SourceOf.Num());
	Alternate.PointToRawMap.Reset(SourceOf.Num());
	for (int32 Merged = 0; Merged < SourceOf.Num(); ++Merged) {
		Alternate.Points.Add(Base.Points[SourceOf[Merged]]);
		Alternate.PointToRawMap.Add(Merged);
	}
	for (SkeletalMeshImportData::FVertex& Wedge : Alternate.Wedges) {
		Wedge.VertexIndex = MergedOf.IsValidIndex(Wedge.VertexIndex) ? FMath::Max(MergedOf[Wedge.VertexIndex], 0) : 0;
	}

	TMap<int32, TArray<const SkeletalMeshImportData::FRawBoneInfluence*>> BaseInfluencesOf;
	for (const SkeletalMeshImportData::FRawBoneInfluence& Influence : Base.Influences) {
		BaseInfluencesOf.FindOrAdd(Influence.VertexIndex).Add(&Influence);
	}

	Alternate.Influences.Reset();
	for (int32 Merged = 0; Merged < SourceOf.Num(); ++Merged) {
		const int32 Point = SourceOf[Merged];
		if (const TArray<TPair<int32, float>>* Override = Influences.Find(Point)) {
			float Total = 0.0f;
			for (const TPair<int32, float>& Each : *Override) Total += Each.Value;
			if (Total > 0.0f) {
				for (const TPair<int32, float>& Each : *Override) {
					SkeletalMeshImportData::FRawBoneInfluence& Influence = Alternate.Influences.AddDefaulted_GetRef();
					Influence.VertexIndex = Merged;
					Influence.BoneIndex = Each.Key;
					Influence.Weight = Each.Value / Total;
				}
				continue;
			}
		}
		if (const auto* Own = BaseInfluencesOf.Find(Point)) {
			for (const SkeletalMeshImportData::FRawBoneInfluence* Each : *Own) {
				SkeletalMeshImportData::FRawBoneInfluence& Influence = Alternate.Influences.Add_GetRef(*Each);
				Influence.VertexIndex = Merged;
			}
		}
	}

	const FString Name = ProfileName.ToString();
	const int32 Existing = Base.AlternateInfluenceProfileNames.Find(Name);
	if (Existing != INDEX_NONE && Base.AlternateInfluences.IsValidIndex(Existing)) {
		Base.AlternateInfluences[Existing] = MoveTemp(Alternate);
	} else {
		Base.AlternateInfluences.Add(MoveTemp(Alternate));
		Base.AlternateInfluenceProfileNames.Add(Name);
	}
	Mesh->SaveLODImportedData(LodIndex, Base);
#endif
}

int32 TSkeletalMeshData::ApplySkinWeightProfiles(USkeletalMesh* Mesh, const FString& SourcePath) {
#if UE4_27_AND_UE5 && WITH_EDITORONLY_DATA
	FSkeletalMeshModel* ImportedModel = Mesh->GetImportedModel();
	if (ImportedModel == nullptr) return 0;

	const TArray<TSharedPtr<FJsonValue>> Profiles = Cloud::Export::GetSkinWeightsBlocking(SourcePath);
	if (Profiles.Num() == 0) return 0;

	const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();
	int32 AppliedProfiles = 0;

	for (const TSharedPtr<FJsonValue>& ProfileValue : Profiles) {
		const TSharedPtr<FJsonObject> Profile = ProfileValue.IsValid() ? ProfileValue->AsObject() : nullptr;
		if (!Profile.IsValid() || !Profile->HasField(TEXT("Name"))) continue;

		const FName ProfileName = FName(*Profile->GetStringField(TEXT("Name")));

		const TArray<TSharedPtr<FJsonValue>>* Lods;
		if (!Profile->TryGetArrayField(TEXT("Lods"), Lods)) continue;

		bool bAppliedAnyLod = false;

		for (const TSharedPtr<FJsonValue>& LodValue : *Lods) {
			const TSharedPtr<FJsonObject> Lod = LodValue.IsValid() ? LodValue->AsObject() : nullptr;
			if (!Lod.IsValid()) continue;

			const int32 LodIndex = Lod->GetIntegerField(TEXT("Index"));
			if (!ImportedModel->LODModels.IsValidIndex(LodIndex)) continue;

			FSkeletalMeshLODModel& LodModel = ImportedModel->LODModels[LodIndex];

			/* Overrides name cooked vertices. Each vertex the build made names the import point it
			 * came from (MeshToImportVertexMap), and the points were written one per cooked vertex,
			 * so that map says which cooked vertex every LOD vertex is: a build that reorders or
			 * splits vertices (a seam made into two) still gets each one its cooked twin's weights.
			 * Only where the map isn't whole are they lined up by index, which needs the counts to
			 * agree. */
			const int32 SourceVertices = Lod->GetIntegerField(TEXT("Vertices"));
			TMap<int32, TArray<int32, TInlineAllocator<2>>> LodVerticesOf;
			const bool bMapped = LodModel.MeshToImportVertexMap.Num() == static_cast<int32>(LodModel.NumVertices);
			if (bMapped) {
				for (int32 LodVertex = 0; LodVertex < LodModel.MeshToImportVertexMap.Num(); ++LodVertex) {
					const int32 Cooked = LodModel.MeshToImportVertexMap[LodVertex];
					if (Cooked >= 0 && Cooked < SourceVertices) LodVerticesOf.FindOrAdd(Cooked).Add(LodVertex);
				}
			} else if (SourceVertices != static_cast<int32>(LodModel.NumVertices)) {
				UE_LOG(LogReflection, Warning, TEXT("Skin weight profile '%s' was cooked against %d vertices at LOD%d, and '%s' has %d there with no map back to them. Skipped, as the weights are keyed by vertex."), *ProfileName.ToString(), SourceVertices, LodIndex, *Mesh->GetName(), LodModel.NumVertices);
				FImportIssues::Report(EImportIssue::Data, TEXT("Skin weight profiles could not be lined up with a LOD"),
					FString::Printf(TEXT("%s LOD%d: %d cooked vertices, %d here, and no map between them"), *ProfileName.ToString(), LodIndex, SourceVertices, LodModel.NumVertices));
				continue;
			}
			int32 UnmatchedVertices = 0;
			TSet<FString> AddedBones;
			/* The profile against the cooked vertices, skeleton bones and plain weights, for the
			 * LOD's import data below */
			TMap<int32, TArray<TPair<int32, float>>> SourceInfluences;

			/* Bones travel as names: this mesh numbers its skeleton however it likes */
			const TArray<TSharedPtr<FJsonValue>>* BoneNames;
			if (!Lod->TryGetArrayField(TEXT("Bones"), BoneNames)) continue;

			TArray<int32> BoneIndices;
			BoneIndices.Reserve(BoneNames->Num());

			for (const TSharedPtr<FJsonValue>& BoneName : *BoneNames) {
				BoneIndices.Add(RefSkeleton.FindBoneIndex(FName(*BoneName->AsString())));
			}

			FImportedSkinWeightProfileData& ProfileData = LodModel.SkinWeightProfiles.FindOrAdd(ProfileName);

			/* A profile only stores what it changes, but the build reads every vertex, so the
			 * rest are seeded from the mesh's own weights */
			ProfileData.SkinWeights.Reset();
			ProfileData.SkinWeights.AddDefaulted(LodModel.NumVertices);

			/* Regenerated from SkinWeights by the build */
			ProfileData.SourceModelInfluences.Reset();

			for (const FSkelMeshSection& Section : LodModel.Sections) {
				for (int32 VertexIndex = 0; VertexIndex < Section.SoftVertices.Num(); ++VertexIndex) {
					const int32 GlobalIndex = Section.BaseVertexIndex + VertexIndex;
					if (!ProfileData.SkinWeights.IsValidIndex(GlobalIndex)) continue;

					const FSoftSkinVertex& Vertex = Section.SoftVertices[VertexIndex];
					FRawSkinWeight& SkinWeight = ProfileData.SkinWeights[GlobalIndex];

					FMemory::Memcpy(SkinWeight.InfluenceBones, Vertex.InfluenceBones, sizeof(SkinWeight.InfluenceBones));
					FMemory::Memcpy(SkinWeight.InfluenceWeights, Vertex.InfluenceWeights, sizeof(SkinWeight.InfluenceWeights));
				}
			}

			int32 AppliedVertices = 0;

			ProcessJsonArrayField(Lod, TEXT("Overrides"), [&](const TSharedPtr<FJsonObject>& Override) {
				const int32 CookedIndex = Override->GetIntegerField(TEXT("Vertex"));
				if (const TArray<TSharedPtr<FJsonValue>>* SourceBones = nullptr, *SourceWeights = nullptr;
					Override->TryGetArrayField(TEXT("Bones"), SourceBones) && Override->TryGetArrayField(TEXT("Weights"), SourceWeights)) {
					TArray<TPair<int32, float>>& Into = SourceInfluences.FindOrAdd(CookedIndex);
					for (int32 Index = 0; Index < SourceBones->Num() && Index < SourceWeights->Num(); ++Index) {
						const int32 NameIndex = static_cast<int32>((*SourceBones)[Index]->AsNumber());
						const float Weight = static_cast<float>((*SourceWeights)[Index]->AsNumber());
						if (BoneIndices.IsValidIndex(NameIndex) && BoneIndices[NameIndex] != INDEX_NONE && Weight > 0.0f) Into.Emplace(BoneIndices[NameIndex], Weight);
					}
				}
				TArray<int32, TInlineAllocator<2>> Targets;
				if (!bMapped) Targets.Add(CookedIndex);
				else if (const auto* Found = LodVerticesOf.Find(CookedIndex)) Targets = *Found;
				if (Targets.IsEmpty()) {
					UnmatchedVertices++;
					return;
				}
				for (const int32 GlobalIndex : Targets) {
				if (!ProfileData.SkinWeights.IsValidIndex(GlobalIndex)) continue;

				const TArray<TSharedPtr<FJsonValue>>* Bones;
				const TArray<TSharedPtr<FJsonValue>>* Weights;

				if (!Override->TryGetArrayField(TEXT("Bones"), Bones)) continue;
				if (!Override->TryGetArrayField(TEXT("Weights"), Weights)) continue;

				/* Weights are stored against the section's bone list, not the skeleton */
				int32 SectionIndex = INDEX_NONE;
				int32 SectionVertexIndex = INDEX_NONE;

				LodModel.GetSectionFromVertexIndex(GlobalIndex, SectionIndex, SectionVertexIndex);
				if (!LodModel.Sections.IsValidIndex(SectionIndex)) continue;

				FSkelMeshSection& Section = LodModel.Sections[SectionIndex];

				FBoneIndexType InfluenceBones[MAX_TOTAL_INFLUENCES] = { };
				float InfluenceWeights[MAX_TOTAL_INFLUENCES] = { };

				int32 Influence = 0;
				float TotalWeight = 0.0f;

				for (int32 Index = 0; Index < Bones->Num() && Index < Weights->Num() && Influence < MAX_TOTAL_INFLUENCES; ++Index) {
					const int32 NameIndex = static_cast<int32>((*Bones)[Index]->AsNumber());
					if (!BoneIndices.IsValidIndex(NameIndex)) continue;

					const int32 BoneIndex = BoneIndices[NameIndex];
					if (BoneIndex == INDEX_NONE) continue;

					/* A bone only the profile skins to has no slot in the section yet: the build made
					 * the section's bone map from the base weights, and the game's map had the
					 * profile's bones in it too. Given one here, the way the game's section had it,
					 * rather than dropped, and the LOD is told it uses the bone (and its parents). */
					int32 BoneMapIndex = Section.BoneMap.IndexOfByKey(static_cast<FBoneIndexType>(BoneIndex));
					if (BoneMapIndex == INDEX_NONE) {
						BoneMapIndex = Section.BoneMap.Add(static_cast<FBoneIndexType>(BoneIndex));
						for (int32 Parent = BoneIndex; Parent != INDEX_NONE; Parent = RefSkeleton.GetParentIndex(Parent)) {
							LodModel.ActiveBoneIndices.AddUnique(static_cast<FBoneIndexType>(Parent));
							LodModel.RequiredBones.AddUnique(static_cast<FBoneIndexType>(Parent));
						}
						AddedBones.Add(RefSkeleton.GetBoneName(BoneIndex).ToString());
					}

					const float Weight = static_cast<float>((*Weights)[Index]->AsNumber());
					if (Weight <= 0.0f) continue;

					InfluenceBones[Influence] = static_cast<FBoneIndexType>(BoneMapIndex);
					InfluenceWeights[Influence] = Weight;

					TotalWeight += Weight;
					Influence++;
				}

				/* Nothing resolved, so the vertex keeps what it was seeded with */
				if (Influence == 0 || TotalWeight <= 0.0f) continue;

				FRawSkinWeight& SkinWeight = ProfileData.SkinWeights[GlobalIndex];

				FMemory::Memzero(SkinWeight.InfluenceBones, sizeof(SkinWeight.InfluenceBones));
				FMemory::Memzero(SkinWeight.InfluenceWeights, sizeof(SkinWeight.InfluenceWeights));

				uint16 Remaining = TNumericLimits<uint16>::Max();

				for (int32 Index = 0; Index < Influence; ++Index) {
					SkinWeight.InfluenceBones[Index] = InfluenceBones[Index];

					/* The last takes the remainder so the vertex sums to exactly full weight */
					const uint16 Quantized = Index == Influence - 1
						? Remaining
						: static_cast<uint16>(FMath::RoundToInt(InfluenceWeights[Index] / TotalWeight * TNumericLimits<uint16>::Max()));

					SkinWeight.InfluenceWeights[Index] = FMath::Min(Quantized, Remaining);
					Remaining -= SkinWeight.InfluenceWeights[Index];
				}

				AppliedVertices++;
				}
			});

			if (AddedBones.Num() > 0) {
				LodModel.ActiveBoneIndices.Sort();
				LodModel.RequiredBones.Sort();
				UE_LOG(LogReflection, Display, TEXT("Skin weight profile '%s' LOD%d skins to %d bone(s) the rebuilt sections didn't map, now added to them: %s"),
					*ProfileName.ToString(), LodIndex, AddedBones.Num(), *FString::Join(AddedBones.Array(), TEXT(", ")));
			}

			if (UnmatchedVertices > 0) {
				UE_LOG(LogReflection, Warning, TEXT("Skin weight profile '%s' LOD%d: %d overridden cooked vertex(es) have no vertex in '%s', so their weights went nowhere"), *ProfileName.ToString(), LodIndex, UnmatchedVertices, *Mesh->GetName());
				FImportIssues::Report(EImportIssue::Data, TEXT("Some skin weight profile vertices matched no vertex here"),
					FString::Printf(TEXT("%s LOD%d: %d cooked vertex(es)"), *ProfileName.ToString(), LodIndex, UnmatchedVertices));
			}

			if (AppliedVertices > 0) {
				WriteProfileToImportData(Mesh, LodIndex, ProfileName, SourceVertices, SourceInfluences);
			}

			if (AppliedVertices == 0) {
				LodModel.SkinWeightProfiles.Remove(ProfileName);

				continue;
			}

			bAppliedAnyLod = true;
		}

		if (bAppliedAnyLod) {
			AppliedProfiles++;
		}
	}

	return AppliedProfiles;
#else
	return 0;
#endif
}

void TSkeletalMeshData::ReportSkinWeightProfiles(USkeletalMesh* Mesh) {
	/* Older engines still get the entries; only this reconciliation needs 4.27 types */
#if UE4_27_AND_UE5 && WITH_EDITORONLY_DATA
	const FSkeletalMeshModel* ImportedModel = Mesh->GetImportedModel();
	if (ImportedModel == nullptr) return;

	/* Either half alone is a profile that does nothing, and neither complains about the other */
	TSet<FName> NamesWithWeights;

	for (const FSkeletalMeshLODModel& LodModel : ImportedModel->LODModels) {
		for (const TPair<FName, FImportedSkinWeightProfileData>& Profile : LodModel.SkinWeightProfiles) {
			NamesWithWeights.Add(Profile.Key);
		}
	}

	const TArray<FSkinWeightProfileInfo>& Profiles = Mesh->GetSkinWeightProfiles();

	for (const FSkinWeightProfileInfo& Profile : Profiles) {
		if (NamesWithWeights.Contains(Profile.Name)) continue;

		UE_LOG(LogReflection, Warning, TEXT("Skin weight profile '%s' was reflected onto '%s', which has no weights imported under that name. Re-import the mesh from a source that carries the profile."), *Profile.Name.ToString(), *Mesh->GetName());
	}

	for (const FName& Name : NamesWithWeights) {
		const bool bHasEntry = Profiles.ContainsByPredicate([&Name](const FSkinWeightProfileInfo& Profile) {
			return Profile.Name == Name;
		});

		if (bHasEntry) continue;

		UE_LOG(LogReflection, Warning, TEXT("'%s' has skin weights imported under '%s', which the export has no profile for. Those weights are now unreachable."), *Mesh->GetName(), *Name.ToString());
	}
#endif
}

TArray<FSkeletalMaterial>& TSkeletalMeshData::GetMaterials(USkeletalMesh* Mesh) {
#if UE4_27 || ENGINE_UE5
	return Mesh->GetMaterials();
#else
	return Mesh->Materials;
#endif
}