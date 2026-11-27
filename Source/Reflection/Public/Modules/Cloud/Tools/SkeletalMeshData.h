/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/Tools/SelectedAssetsBase.h"

class REFLECTION_API TSkeletalMeshData : public TSelectedAssetsBase {
public:
	virtual void Process(UObject* Object, const TArray<TSharedPtr<FJsonValue>>& Exports) override;

	virtual FName GetSupportedClass() const override { return FName("SkeletalMesh"); }

	virtual FText GetDisplayName() const override { return FText::FromString("Skeletal Meshes"); }
	virtual FText GetTooltip() const override { return FText::FromString("Reflects sockets and other properties"); }
	virtual FSlateIcon GetIcon() const override { return FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.SkeletalMeshComponent"); }

	/* Has the engine build the mesh's sampling data again from what was imported, and says what came out */
	static void RebuildSamplingData(USkeletalMesh* Mesh);

protected:
	/* By reference: the caller writes slots back into the mesh */
	static TArray<FSkeletalMaterial>& GetMaterials(USkeletalMesh* Mesh);

	/* Ports the per section flags onto the mesh's sections, returning how many it reached. Cloth's
	 * section fields are left to binding. */
	static int32 ApplySectionUserData(USkeletalMesh* Mesh, const TSharedPtr<FJsonObject>& MeshExport);

	/* Rebuilds the weights behind the profile entries, returning how many profiles got any.
	 * SourcePath is the mesh's path in the game, which is what the Cloud knows it by: a redirected
	 * import lives somewhere else here. */
	static int32 ApplySkinWeightProfiles(USkeletalMesh* Mesh, const FString& SourcePath);


	/* Writes one profile into a LOD's import data as alternate skinning, so a rebuild from it keeps
	 * the profile. Influences are per cooked vertex: skeleton bone and weight. */
	static void WriteProfileToImportData(USkeletalMesh* Mesh, int32 LodIndex, FName ProfileName, int32 SourceVertices,
		const TMap<int32, TArray<TPair<int32, float>>>& Influences);

	/* Logs profiles that only made it half way: an entry with no weights, or weights with no entry */
	static void ReportSkinWeightProfiles(USkeletalMesh* Mesh);
};

REGISTER_TOOL(TSkeletalMeshData)
