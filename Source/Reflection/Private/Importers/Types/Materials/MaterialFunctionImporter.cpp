/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Materials/MaterialFunctionImporter.h"
#include "RenderingThread.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#if !UE4_25_BELOW
#include "MaterialEditingLibrary.h"
#endif

/* 4.25 and below build this module without the engine's shared PCH (see Reflection.Build.cs),
 * which is where the material function type used to come in from */
#if UE4_25_BELOW
#include "Materials/MaterialFunction.h"
#endif

UObject* IMaterialFunctionImporter::CreateAsset(UObject* CreatedAsset) {
	/* Reflected again over one that is already loaded: rebuilt in place rather than constructed in
	 * the old one's memory, which would pull it out from under every material compiled against it
	 * (see IMaterialImporter::CreateAsset) */
	if (UMaterialFunction* Existing = FindObject<UMaterialFunction>(GetPackage(), *GetAssetName())) {
		FlushRenderingCommands();

		return IImporter::CreateAsset(Existing);
	}

	return IImporter::CreateAsset(Cast<UMaterialFunction>(
		NewObject<UMaterialFunctionFactoryNew>()->FactoryCreateNew(
			UMaterialFunction::StaticClass(),
			GetPackage(),
			*GetAssetName(),
			RF_Standalone | RF_Public,
			nullptr,
			GWarn)
	));
}

bool IMaterialFunctionImporter::Import() {
	/* Reflected again over one already loaded, so loaded materials were compiled against the old one */
	const bool bWasLoaded = FindObject<UMaterialFunction>(GetPackage(), *GetAssetName()) != nullptr;

	UMaterialFunction* MaterialFunction = Create<UMaterialFunction>();

	/* Empty all expressions, we create them */
#if UE5_1_BEYOND
	MaterialFunction->GetExpressionCollection().Empty();
#else
	MaterialFunction->FunctionExpressions.Empty();
#endif

	/* Handle edit changes, and add it to the content browser */
	/* Finished and saved once at the end, when its expressions are in */
	if (!OnAssetCreation(MaterialFunction, true)) return false;

	/* Define editor only data from the JSON */
	FUObjectExportContainer* ExpressionContainer = new FUObjectExportContainer();
	const TSharedPtr<FJsonObject> Props = FindMaterialData(GetAssetType(), ExpressionContainer);

	/* Map out each expression for easier access */
	ConstructExpressions(ExpressionContainer);

	/* If Missing Material Data */
	if (ExpressionContainer->Num() == 0) {
		ReportMaterialDataMissing();

		/* Kept as it was before the finish moved to the end: the empty function is still written */
		MaterialFunction->PostEditChange();
		Save();

		return false;
	}

	/* Iterate through all the expressions, and set properties */
	PropagateExpressions(ExpressionContainer);

	/* Deserialize any properties */
	GetObjectSerializer()->DeserializeObjectProperties(GetAssetData(), MaterialFunction);

	/* Everything is wired by this point, which is what dropping an output needs */
	PruneMissingMaterialAttributes(MaterialFunction);

	/* Expressions built by hand leave these two caches stale, and the engine only refills them in
	 * PostLoad or ForceRecompileForRendering. DependentFunctionExpressionCandidates is the one that
	 * matters: FMaterialCachedExpressionData reaches functions nested below this one only through
	 * IterateDependentFunctions, which iterates nothing else. Left empty, a parent material misses
	 * every texture one level deep, then fails to compile on Compiler->Texture(). */
	if (HasMaterialEditorOnlyData(MaterialFunction)) {
		MaterialFunction->UpdateInputOutputTypes();
		/* Neither the candidate list nor the cached expression data it feeds exist before 4.25,
		 * where a parent material walks the function's expressions directly instead */
#if !UE4_24_BELOW
		MaterialFunction->UpdateDependentFunctionCandidates();
#endif
	} else {
		/* The function came out without the half of itself the editor keeps, so there is nothing
		 * to refresh and nothing that reaches through it can be let near it */
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The function has no editor data"),
			FString::Printf(
				TEXT("'%s' came out without the object its expressions live on, so it has no inputs, no outputs, and nothing that uses it can compile against it."),
				*GetAssetName())
		);
	}

	MaterialFunction->PreEditChange(nullptr);
	MaterialFunction->PostEditChange();

	/* A material keeps the shader map it compiled against the function's old expressions, and
	 * nothing about rebuilding the function tells it otherwise: one that failed to compile against
	 * the old ones (and draws as the world grid) goes on failing until the editor restarts. The
	 * material editor recompiles every loaded material and instance using a function it applies;
	 * this is that same call. Only on a function reflected again, since a new one has nothing
	 * compiled against it, and a material still being built around it is not to be recompiled. */
#if !UE4_25_BELOW
	if (bWasLoaded) {
		UMaterialEditingLibrary::UpdateMaterialFunction(MaterialFunction, nullptr);
	}
#endif

	Save();

	return true;
}
