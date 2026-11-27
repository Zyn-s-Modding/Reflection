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
#include "Importers/Types/Blueprint/GraphLayout.h"
#include "Framework/Application/SlateApplication.h"
#include "Interfaces/ISlateNullRendererModule.h"
#include "Styling/AppStyle.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"

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
#include "FXSystem.h"
#include "SceneInterface.h"
#include "AssetCompilingManager.h"
#include "ShaderCompiler.h"
#include "ImageUtils.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "NiagaraComponent.h"
#include "NiagaraComponentSettings.h"
#include "NiagaraDataSet.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraSystemInstanceController.h"
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

/* MetaSoundCheck.cpp: -metasoundcheck=, which looks a MetaSound over and plays it */
bool RunMetaSoundCheck(const FString& Params, int32& OutResult);

/* Writes where every node of a graph sits, how big it draws, where its pins are and what each is
 * wired to, so a layout can be looked at without an editor window open to look at it in. Sizes are
 * measured the same way the layout measured them. Says how many nodes sit on top of one another. */
static void DumpGraph(const UEdGraph* Graph, const FString& Directory) {
	TArray<const UEdGraphNode*> Nodes;
	TMap<const UEdGraphNode*, int32> Index;

	for (const UEdGraphNode* Node : Graph->Nodes) {
		if (Node == nullptr) continue;

		Index.Add(Node, Nodes.Add(Node));
	}

	TArray<FGraphNodeFootprint> Prints;

	for (const UEdGraphNode* Node : Nodes) Prints.Add(MeasureGraphNode(const_cast<UEdGraphNode*>(Node)));

	TArray<TSharedPtr<FJsonValue>> Written;

	for (int32 At = 0; At < Nodes.Num(); ++At) {
		const UEdGraphNode* Node = Nodes[At];
		const FGraphNodeFootprint& Print = Prints[At];

		const TSharedRef<FJsonObject> One = MakeShared<FJsonObject>();

		One->SetStringField(TEXT("name"), Node->GetName());
		One->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		One->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		One->SetNumberField(TEXT("x"), Node->NodePosX);
		One->SetNumberField(TEXT("y"), Node->NodePosY);
		One->SetNumberField(TEXT("w"), Print.Size.X);
		One->SetNumberField(TEXT("h"), Print.Size.Y);
		One->SetNumberField(TEXT("overhang"), Print.TopOverhang);
		One->SetBoolField(TEXT("measured"), Print.bMeasured);

		TArray<TSharedPtr<FJsonValue>> Pins;

		for (const UEdGraphPin* Pin : Node->Pins) {
			if (Pin == nullptr) continue;

			const TSharedRef<FJsonObject> Held = MakeShared<FJsonObject>();

			Held->SetStringField(TEXT("name"), Pin->PinName.ToString());
			Held->SetStringField(TEXT("dir"), Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"));
			Held->SetStringField(TEXT("cat"), Pin->PinType.PinCategory.ToString());
			Held->SetBoolField(TEXT("hidden"), Pin->bHidden);
			Held->SetNumberField(TEXT("y"), Print.PinY(Pin));

			TArray<TSharedPtr<FJsonValue>> Links;

			for (const UEdGraphPin* Linked : Pin->LinkedTo) {
				const int32* Other = Linked != nullptr ? Index.Find(Linked->GetOwningNode()) : nullptr;

				if (Other == nullptr) continue;

				const TSharedRef<FJsonObject> Link = MakeShared<FJsonObject>();
				Link->SetNumberField(TEXT("node"), *Other);
				Link->SetStringField(TEXT("pin"), Linked->PinName.ToString());

				Links.Add(MakeShared<FJsonValueObject>(Link));
			}

			Held->SetArrayField(TEXT("links"), Links);

			Pins.Add(MakeShared<FJsonValueObject>(Held));
		}

		One->SetArrayField(TEXT("pins"), Pins);

		Written.Add(MakeShared<FJsonValueObject>(One));
	}

	/* Two nodes overlap where their boxes do, which is what a reader sees as one node hiding another */
	int32 Overlaps = 0;

	for (int32 A = 0; A < Nodes.Num(); ++A) {
		for (int32 B = A + 1; B < Nodes.Num(); ++B) {
			const double AL = Nodes[A]->NodePosX, AT = Nodes[A]->NodePosY - Prints[A].TopOverhang;
			const double BL = Nodes[B]->NodePosX, BT = Nodes[B]->NodePosY - Prints[B].TopOverhang;

			if (AL < BL + Prints[B].Size.X && BL < AL + Prints[A].Size.X && AT < BT + Prints[B].Size.Y + Prints[B].TopOverhang && BT < AT + Prints[A].Size.Y + Prints[A].TopOverhang) {
				Overlaps++;
			}
		}
	}

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("graph"), Graph->GetPathName());
	Root->SetStringField(TEXT("class"), Graph->GetClass()->GetName());
	Root->SetNumberField(TEXT("overlaps"), Overlaps);
	Root->SetArrayField(TEXT("nodes"), Written);

	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(Root, Writer);

	FString Named = Graph->GetPathName();

	for (TCHAR& Letter : Named) {
		if (!FChar::IsAlnum(Letter)) Letter = TEXT('_');
	}

	const FString File = FPaths::Combine(Directory, Named + TEXT(".json"));

	FFileHelper::SaveStringToFile(Text, *File);

	UE_LOG(LogReflectImport, Display, TEXT("graph \"%s\": %d node(s), %d overlapping pair(s), written to %s"), *Graph->GetPathName(), Nodes.Num(), Overlaps, *File);
}

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
	/* An ensure is logged and carried on from rather than reported. Reported from a commandlet on a Mac,
	 * the engine starts a reporter process, trips over itself while starting it, and waits forever on
	 * a lock it already holds, so one ensure in one asset stops every import after it. */
	static bool bEnsuresLogged = false;

	if (!bEnsuresLogged) {
		bEnsuresLogged = true;

		SetEnsureHandler([](const FEnsureHandlerArgs& Args) {
			UE_LOG(LogReflectImport, Warning, TEXT("ensure failed, carried on: %hs %s"), Args.Expression, Args.Message);

			return true;
		});
	}

	/* -slate: Slate started with a renderer that draws nothing, so a graph is laid out against the
	 * size each node's widget really is rather than one worked out from its pins. A commandlet starts
	 * no Slate of its own, and without it every layout measures nodes by guesswork. The null renderer
	 * still measures text with the real fonts, which is all a node's size comes down to. */
	if (FParse::Param(*Params, TEXT("slate")) && !FSlateApplication::IsInitialized()) {
		FSlateApplication::Create();

		ISlateNullRendererModule& Null = FModuleManager::Get().LoadModuleChecked<ISlateNullRendererModule>(TEXT("SlateNullRenderer"));
		FSlateApplication::Get().InitializeRenderer(Null.CreateSlateNullRenderer(), true);

		/* The brushes and fonts a node is drawn with, which are the editor's rather than Slate's own */
		FModuleManager::Get().LoadModule(TEXT("EditorStyle"));

		UE_LOG(LogReflectImport, Display, TEXT("Slate started with nothing to draw to, for measuring nodes (style \"%s\")"), *FAppStyle::GetAppStyleSetName().ToString());
	}

	/* Many assets in one session, one cloud path per line of a file: -pathlist=<file>. Each is
	 * imported as though it had been asked for with -path on its own, and the editor only starts
	 * once, which for a project this size is most of the time a single import takes. */
	if (FString Listed; FParse::Value(*Params, TEXT("pathlist="), Listed) && !Listed.IsEmpty()) {
		TArray<FString> Lines;

		if (!FFileHelper::LoadFileToStringArray(Lines, *Listed)) {
			UE_LOG(LogReflectImport, Error, TEXT("there is nothing to read at \"%s\""), *Listed);

			return 1;
		}

		const FString Rest = Params.Replace(*(TEXT("-pathlist=") + Listed), TEXT(""));

		int32 Failed = 0;
		int32 Done = 0;

		for (const FString& Line : Lines) {
			const FString One = Line.TrimStartAndEnd();

			if (One.IsEmpty() || One.StartsWith(TEXT("#"))) continue;

			UE_LOG(LogReflectImport, Display, TEXT("===== importing %d of %d: %s"), ++Done, Lines.Num(), *One);

			const int32 Said = Main(Rest + TEXT(" -path=") + One);

			if (Said != 0) Failed++;

			UE_LOG(LogReflectImport, Display, TEXT("===== done with %s: %s"), *One, Said == 0 ? TEXT("ok") : TEXT("failed"));

			/* Everything an import made is saved by now, and anything a later one needs is loaded back */
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}

		UE_LOG(LogReflectImport, Display, TEXT("%d imported, %d failed"), Done - Failed, Failed);

		return Failed > 0 ? 1 : 0;
	}

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

	/* Runs a Niagara system already in the project for a while with nothing drawing it, and says
	 * what each CPU emitter's particles hold, attribute by attribute: the mean, least and most of
	 * each component. For telling a particle drawn black because of its material from one drawn
	 * black because the attribute the material reads is zero, without a screen to look at:
	 * -particleprobe=/Mount/Path/System [-probeseconds=2] */
	if (FString Probed; FParse::Value(*Params, TEXT("particleprobe="), Probed) && !Probed.IsEmpty()) {
		if (!Probed.Contains(TEXT("."))) {
			Probed += TEXT(".") + FPackageName::GetShortName(Probed);
		}

		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *Probed);
		if (System == nullptr) {
			UE_LOG(LogReflectImport, Error, TEXT("there is no Niagara system at \"%s\""), *Probed);
			return 1;
		}

		System->RequestCompile(false);
		System->WaitForCompilationComplete(true, false);

		float Seconds = 2.0f;
		FParse::Value(*Params, TEXT("probeseconds="), Seconds);

		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("ParticleProbe"));
		FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
		Context.SetCurrentWorld(World);

		/* A world made in a commandlet is given no FX system (UWorld::CreateFXSystem), and a
		 * Niagara component waits for one before it activates */
		if (World->Scene != nullptr && World->Scene->GetFXSystem() == nullptr) {
			World->FXSystem = FFXSystemInterface::Create(World->GetFeatureLevel(), World->Scene);
			World->Scene->SetFXSystem(World->FXSystem);
		}

		ON_SCOPE_EXIT {
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		};

		/* Needs -AllowCommandletRendering: a component isn't made at all where nothing can render */
		UNiagaraComponent* Component = UNiagaraFunctionLibrary::SpawnSystemAtLocation(World, System, FVector::ZeroVector, FRotator::ZeroRotator, FVector(1.0f), false, false, ENCPoolMethod::None, false);
		if (Component == nullptr) {
			UE_LOG(LogReflectImport, Error, TEXT("\"%s\" could not be spawned (pass -AllowCommandletRendering)"), *Probed);
			return 1;
		}

		/* Nobody is looking, so nothing is close enough for scalability to keep it */
		Component->SetAllowScalability(false);

		/* User parameters the game sets as it plays, such as one an animation notify drives from a
		 * curve: -probeuser=BurstIndex=1,BurstIndex2=1 */
		if (FString Users; FParse::Value(*Params, TEXT("probeuser="), Users, false)) {
			TArray<FString> Pairs;
			Users.ParseIntoArray(Pairs, TEXT(","));
			for (const FString& Pair : Pairs) {
				FString Name, Value;
				if (Pair.Split(TEXT("="), &Name, &Value)) Component->SetVariableFloat(FName(*Name), FCString::Atof(*Value));
			}
		}

		Component->Activate(true);

		UE_LOG(LogReflectImport, Display, TEXT("spawned: registered %d, ready to run %d, world ready %d, allowed %d, scene %d, active %d"),
			Component->IsRegistered() ? 1 : 0, System->IsReadyToRun() ? 1 : 0, Component->IsWorldReadyToRun() ? 1 : 0,
			FNiagaraComponentSettings::IsSystemAllowedToRun(System) ? 1 : 0, World->Scene != nullptr ? 1 : 0, Component->IsActive() ? 1 : 0);

		const int32 Ticks = FMath::Max(1, FMath::RoundToInt(Seconds * 30.0f));
		Component->AdvanceSimulation(Ticks, 1.0f / 30.0f);

		FNiagaraSystemInstanceControllerPtr Controller = Component->GetSystemInstanceController();
		FNiagaraSystemInstance* Instance = Controller.IsValid() ? Controller->GetSystemInstance_Unsafe() : nullptr;
		if (Instance == nullptr) {
			UE_LOG(LogReflectImport, Error, TEXT("\"%s\" has no running instance after %.2fs (not ready to run?)"), *Probed, Seconds);
			return 1;
		}

		UE_LOG(LogReflectImport, Display, TEXT("particle probe of \"%s\" after %.2fs"), *Probed, Seconds);

		for (const TSharedRef<FNiagaraEmitterInstance, ESPMode::ThreadSafe>& Emitter : Instance->GetEmitters()) {
			const FNiagaraDataSet& Data = Emitter->GetData();
			const FNiagaraDataBuffer* Buffer = Data.GetCurrentData();
			const bool bCPU = Data.GetSimTarget() == ENiagaraSimTarget::CPUSim;
			const int32 Count = Buffer != nullptr && bCPU ? Buffer->GetNumInstances() : Emitter->GetNumParticles();

			UE_LOG(LogReflectImport, Display, TEXT("  %s: %d particle(s), %s"),
				*Emitter->GetEmitterHandle().GetName().ToString(), Count, bCPU ? TEXT("CPU") : TEXT("GPU, not read"));

			if (!bCPU || Buffer == nullptr || Count == 0) continue;

			const TArray<FNiagaraVariableBase>& Variables = Data.GetVariables();
			const TArray<FNiagaraVariableLayoutInfo>& Layouts = Data.GetVariableLayouts();

			for (int32 Index = 0; Index < Variables.Num() && Index < Layouts.Num(); ++Index) {
				const FNiagaraVariableLayoutInfo& Layout = Layouts[Index];
				const int32 Components = Layout.GetNumFloatComponents();
				if (Components == 0 || Components > 4) continue;

				TArray<FString> Parts;
				for (int32 Component_ = 0; Component_ < Components; ++Component_) {
					double Sum = 0;
					float Least = TNumericLimits<float>::Max();
					float Most = TNumericLimits<float>::Lowest();
					for (int32 Particle = 0; Particle < Count; ++Particle) {
						const float Value = *Buffer->GetInstancePtrFloat(Layout.GetFloatComponentStart() + Component_, Particle);
						Sum += Value;
						Least = FMath::Min(Least, Value);
						Most = FMath::Max(Most, Value);
					}
					Parts.Add(FString::Printf(TEXT("%.3g [%.3g..%.3g]"), Sum / Count, Least, Most));
				}

				UE_LOG(LogReflectImport, Display, TEXT("    %s = %s"), *Variables[Index].GetName().ToString(), *FString::Join(Parts, TEXT(", ")));
			}
		}

		/* And a picture of it, for what the numbers can't say: -probecapture=<file stem>. Taken
		 * from the front and the side against a mid grey wall, so a particle drawn black shows
		 * up as black rather than as nothing. */
		if (FString Stem; FParse::Value(*Params, TEXT("probecapture="), Stem) && !Stem.IsEmpty()) {
			AActor* Holder = World->GetWorldSettings();

			/* A wall behind the effect for each view, facing the camera */
			UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
			UMaterialInterface* Unlit = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineDebugMaterials/LevelColorationUnlitMaterial.LevelColorationUnlitMaterial"));
			if (Plane != nullptr && Unlit != nullptr) {
				UMaterialInstanceDynamic* Grey = UMaterialInstanceDynamic::Create(Unlit, Holder);
				Grey->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.25f, 0.25f, 0.25f));

				for (const FTransform& Placed : { FTransform(FRotator(90, 0, 0), FVector(700, 0, 100), FVector(40)), FTransform(FRotator(90, -90, 0), FVector(0, -700, 100), FVector(40)) }) {
					UStaticMeshComponent* Wall = NewObject<UStaticMeshComponent>(Holder);
					Wall->SetStaticMesh(Plane);
					Wall->SetMaterial(0, Grey);
					Wall->SetWorldTransform(Placed);
					Wall->RegisterComponentWithWorld(World);
				}
			}

			UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>();
			Target->RenderTargetFormat = RTF_RGBA8_SRGB;
			Target->InitAutoFormat(1024, 768);
			Target->UpdateResourceImmediate(true);

			USceneCaptureComponent2D* Capture = NewObject<USceneCaptureComponent2D>(Holder);
			Capture->TextureTarget = Target;
			Capture->CaptureSource = SCS_FinalColorLDR;
			Capture->bCaptureEveryFrame = false;
			Capture->bCaptureOnMovement = false;
			Capture->FOVAngle = 90.0f;
			/* Exposure held still, so the wall reads as the grey it is whatever the effect is doing */
			Capture->PostProcessSettings.bOverride_AutoExposureMethod = true;
			Capture->PostProcessSettings.AutoExposureMethod = AEM_Manual;
			Capture->PostProcessSettings.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
			Capture->PostProcessSettings.AutoExposureApplyPhysicalCameraExposure = false;
			Capture->PostProcessSettings.bOverride_AutoExposureBias = true;
			Capture->PostProcessSettings.AutoExposureBias = 0.0f;
			Capture->RegisterComponentWithWorld(World);

			/* Everything the picture draws with, compiled before it is drawn */
			GShaderCompilingManager->FinishAllCompilation();
			FAssetCompilingManager::Get().FinishAllCompilation();

			const TPair<const TCHAR*, FTransform> Views[] = {
				{ TEXT("front"), FTransform(FRotator(0, 0, 0), FVector(-450, 0, 100)) },
				{ TEXT("side"), FTransform(FRotator(0, -90, 0), FVector(0, 450, 100)) },
			};

			/* Taken at -probeseconds, then at each later time in -probelater=4,6,9 */
			TArray<float> Times { Seconds };
			if (FString Later; FParse::Value(*Params, TEXT("probelater="), Later, false)) {
				TArray<FString> Listed;
				Later.ParseIntoArray(Listed, TEXT(","));
				for (const FString& Time : Listed) Times.Add(FCString::Atof(*Time));
			}

			float Now = Seconds;
			for (const float Time : Times) {
				if (Time > Now) {
					Component->AdvanceSimulation(FMath::Max(1, FMath::RoundToInt((Time - Now) * 30.0f)), 1.0f / 30.0f);
					Now = Time;
					GShaderCompilingManager->FinishAllCompilation();
				}

				for (const TPair<const TCHAR*, FTransform>& View : Views) {
					Capture->SetWorldTransform(View.Value);
					Capture->CaptureScene();
					FlushRenderingCommands();

					TArray<FColor> Pixels;
					if (!Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels) || Pixels.Num() == 0) {
						UE_LOG(LogReflectImport, Error, TEXT("the %s capture came back empty"), View.Key);
						continue;
					}
					for (FColor& Pixel : Pixels) Pixel.A = 255;

					TArray64<uint8> Png;
					FImageUtils::PNGCompressImageArray(Target->SizeX, Target->SizeY, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
					const FString File = FString::Printf(TEXT("%s_%gs_%s.png"), *Stem, Now, View.Key);
					if (FFileHelper::SaveArrayToFile(Png, *File)) {
						UE_LOG(LogReflectImport, Display, TEXT("captured %s"), *File);
					}
				}
			}
		}

		return 0;
	}

	if (int32 Checked = 0; RunMetaSoundCheck(Params, Checked)) return Checked;

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

		/* A project path, which a redirect moved from the path Cloud answers to: asked for at
		 * each path that redirects there, the way re-reflecting an asset from the Content
		 * Browser does */
		if (!Response.IsValid() || !Response->HasField(TEXT("exports"))) {
			for (const FString& Source : FRRedirects::Sources(Path)) {
				TSharedPtr<FJsonObject> Answer = Cloud::Export::GetRawBlocking(Source);
				if (!Answer.IsValid() || !Answer->HasField(TEXT("exports"))) continue;

				UE_LOG(LogReflectImport, Display, TEXT("\"%s\" is read from \"%s\", the path the game keeps it at"), *Path, *Source);
				Path = Source;
				Response = MoveTemp(Answer);
				break;
			}
		}
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

		if (FString Dumped; FParse::Value(*Params, TEXT("graphdump="), Dumped) && !Dumped.IsEmpty()) {
			IFileManager::Get().MakeDirectory(*Dumped, true);

			for (UObject* Object : Held) {
				/* Not the copies the compiler works on, which nobody opens */
				if (const UEdGraph* Graph = Cast<UEdGraph>(Object); Graph != nullptr && Graph->Nodes.Num() > 0 && !Graph->HasAnyFlags(RF_Transient)) {
					DumpGraph(Graph, Dumped);
				}
			}
		}

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
