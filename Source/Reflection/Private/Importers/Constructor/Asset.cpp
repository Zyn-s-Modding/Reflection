/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/Asset.h"

#include "Animation/Skeleton.h"

#include "Importers/Constructor/Importer.h"

#include "Importers/Types/Texture/TextureImporter.h"
#include "Importers/Types/Texture/TextureTypes.h"

#include "Curves/CurveLinearColor.h"
#include "Engine/TextureLightProfile.h"
#include "Sound/SoundNode.h"
#include "Engine/SubsurfaceProfile.h"
#include "Materials/MaterialParameterCollection.h"
#include "Settings/ReflectionSettings.h"
#include "Settings/SettingsAccess.h"
#include "Dom/JsonObject.h"

#include "Engine/FontFace.h"
#include "Importers/Constructor/ImportIssues.h"
#include "Importers/Constructor/ImportReader.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/Cloud/Cloud.h"
#include "Settings/Runtime.h"
#include "Settings/Blacklist.h"

/* Takes the export directory off the front of a path, where the path has it.
 *
 * With no export directory known (it is read from FModel's settings, which only exist on
 * Windows), the separator being split on was a lone "/", which matched the first slash of
 * whatever was there and ate the leading one off a /Game/ path. */
static void StripExportDirectory(FString& Path) {
	const FString& Directory = GReflectionRuntime.ExportDirectory.Path;

	if (Directory.IsEmpty()) return;

	Path.Split(Directory + "/", nullptr, &Path, ESearchCase::IgnoreCase, ESearchDir::FromStart);
}

bool FAssetUtilities::TryGetEngineContentDestination(const FString& Path, FString& Destination) {
	const FRSettings& Settings = GetSettings()->AssetSettings;
	if (!Settings.bRedirectEngineContent) return false;

	FString Source = Path;
	StripExportDirectory(Source);
	Source.RemoveFromStart(TEXT("/"));

	if (Source.StartsWith(TEXT("Engine/Plugins/"))) {
		FString PluginPath, RelativePath;
		if (!Source.Split(TEXT("/Content/"), &PluginPath, &RelativePath) || RelativePath.IsEmpty()) return false;
		Source = FPaths::GetCleanFilename(PluginPath) / RelativePath;
	} else if (Source.StartsWith(TEXT("Engine/"))) {
		if (Source.StartsWith(TEXT("Engine/Content/"))) {
			Source = TEXT("Engine/") + Source.RightChop(FCString::Strlen(TEXT("Engine/Content/")));
		}
	} else {
		FString Mount;
		if (!Source.Split(TEXT("/"), &Mount, nullptr) || Mount.Equals(TEXT("Game"))) return false;

		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Mount);
		if (!Plugin.IsValid() || Plugin->GetLoadedFrom() != EPluginLoadedFrom::Engine) return false;
	}

	FString Folder = Settings.EngineContentFolder.TrimStartAndEnd();
	while (Folder.RemoveFromEnd(TEXT("/"))) {}
	const FString Package = Folder / TEXT("EngineContent");
	const FName Root = FPackageName::GetPackageMountPoint(Package);
	const TSharedPtr<IPlugin> ProjectPlugin = IPluginManager::Get().FindPlugin(Root.ToString());
	const bool bProjectFolder = Root == FName(TEXT("Game")) || (ProjectPlugin.IsValid() && ProjectPlugin->GetLoadedFrom() == EPluginLoadedFrom::Project);
	if (!bProjectFolder || !FPackageName::IsValidLongPackageName(Package)) {
		Folder = FRSettings::DefaultEngineContentFolder;
	}

	Destination = Folder / Source;
	return true;
}

/* Deferred saves ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
namespace {
	/* In the order they were first asked for, each once. Weak, since a package the run threw away
	 * is not one to write. */
	TArray<TWeakObjectPtr<UPackage>> GPendingSaves;
}

void FAssetUtilities::QueueSave(UPackage* Package) {
	if (Package == nullptr) return;

	if (ImportDepth <= 0) {
		SavePackage(Package);

		return;
	}

	GPendingSaves.AddUnique(Package);
}

void FAssetUtilities::FlushSaves() {
	/* Taken out first, since a save can reach code that queues another */
	while (GPendingSaves.Num() > 0) {
		TArray<TWeakObjectPtr<UPackage>> Saving = MoveTemp(GPendingSaves);
		GPendingSaves.Reset();

		for (const TWeakObjectPtr<UPackage>& Weak : Saving) {
			if (UPackage* Package = Weak.Get()) {
				SavePackage(Package);
			}
		}
	}
}

/* CreateAssetPackage Implementations ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
UPackage* FAssetUtilities::CreateAssetPackage(const FString& Path) {
	/* A path off disk rather than a package path gets this far when the export was reflected from
	 * somewhere outside the project, and the asset registry crashes on one that has no leading
	 * slash rather than refusing it. */
	if (!FPackageName::IsValidLongPackageName(Path)) {
		FImportIssues::Report(
			EImportIssue::Failed,
			TEXT("Not a package path"),
			FString::Printf(TEXT("\"%s\" is not one. Reflect from inside the export folder so the path resolves against the game's own."), *Path)
		);

		return nullptr;
	}

	if (!IsAllowedDestination(Path)) {
		FImportIssues::Report(
			EImportIssue::Failed,
			TEXT("Outside the folder this run may write to"),
			FString::Printf(TEXT("\"%s\" is not under %s, so it was not created"), *Path, *OnlyUnder)
		);

		return nullptr;
	}

	/* There now, whatever the run found when it asked before */
	MissingPackagesThisRun.Remove(Path);

	UPackage* Package = CreatePackage(
		/* 4.25, 4.26.0 and below need an Outer */
#if UE4_25_BELOW || (UE4_26_0)
		nullptr,
#endif
		*Path);
	Package->FullyLoad();

	/* Reflected assets land in folders this project has never had, and the Content Browser builds
	 * its folder tree from the asset registry's cached paths: a folder it has not been told about
	 * cannot be navigated to, which is what stops the jump at the end of an import.
	 *
	 * Told here rather than at the jump because being told is not the same as being ready. The
	 * browser builds the folder on a tick of its own, and doing it now gives it the whole length
	 * of the import to get there instead of no time at all. */
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	AssetRegistry.AddPath(FPackageName::GetLongPackagePath(Path));

	return Package;
}

UPackage* FAssetUtilities::CreateAssetPackage(const FString& Name, const FString& OutputPath, FString& FailureReason) {
	const FString& ProjectName = GReflectionRuntime.Profile.ProjectName;

	FString Destination;
	if (TryGetEngineContentDestination(OutputPath, Destination)) {
		const FString PackagePath = FPaths::GetPath(Destination) / Name;
		UPackage* Package = CreateAssetPackage(PackagePath);
		if (Package == nullptr) FailureReason = FString::Printf(TEXT("\"%s\" is not a valid engine content destination package."), *PackagePath);
		return Package;
	}

	FString ModifiablePath = OutputPath;

	/* References Automatically Formatted */
	if (!ModifiablePath.StartsWith("/Game/") && !ModifiablePath.StartsWith("/Plugins/") && ModifiablePath.Contains("/Content/")) {
		if (!ProjectName.IsEmpty()) {
			ModifiablePath = ModifiablePath.Replace(*(ProjectName + "/Content"), TEXT("/Game"));
			StripExportDirectory(ModifiablePath);
			ModifiablePath.Split("/", &ModifiablePath, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			ModifiablePath += "/";
		}

		if (!ModifiablePath.StartsWith("/Game/") && !ModifiablePath.StartsWith("/Plugins/") && ModifiablePath.Contains("/Content/")) {
			StripExportDirectory(ModifiablePath);
			ModifiablePath.Split("/", nullptr, &ModifiablePath, ESearchCase::IgnoreCase, ESearchDir::FromStart);
			ModifiablePath.Split("/", &ModifiablePath, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			/* Ex: RestPath: Plugins/Folder/BaseTextures */
			/* Ex: RestPath: Content/SecondaryFolder */
			const bool IsPlugin = ModifiablePath.StartsWith("Plugins");

			/* Plugins/Folder/BaseTextures -> Folder/BaseTextures */
			if (IsPlugin) {
				FString PluginName = ModifiablePath;
				FString RemainingPath;
				/* PluginName = TestName */
				/* RemainingPath = SetupAssets/Materials */
				ModifiablePath.Split("/Content/", &PluginName, &RemainingPath, ESearchCase::IgnoreCase, ESearchDir::FromStart);
				PluginName.Split("/", nullptr, &PluginName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);

				/* /PluginName/Materials */
				ModifiablePath = PluginName + "/" + RemainingPath;
			}
			/* Content/SecondaryFolder -> Game/SecondaryFolder */
			else {
				ModifiablePath = ModifiablePath.Replace(TEXT("Content"), TEXT("Game"));
			}

			ModifiablePath = "/" + ModifiablePath + "/";

			FRRedirects::Redirect(ModifiablePath, ModifiablePath + Name);

			/* Check if plugin exists */
			if (IsPlugin && !ModifiablePath.StartsWith("/Game/")) {
				FString PluginName;
				ModifiablePath.Split("/", nullptr, &PluginName, ESearchCase::IgnoreCase, ESearchDir::FromStart);
				PluginName.Split("/", &PluginName, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromStart);

				if (GetPlugin(PluginName) == nullptr) {
					if (IsAllowedDestination(ModifiablePath)) CreatePlugin(PluginName);
				}
			}
		}
		else {
			FRRedirects::Redirect(ModifiablePath, ModifiablePath + Name);

			if (!ModifiablePath.StartsWith("/Game/") && !ModifiablePath.StartsWith("/Engine/")) {
				FString PluginName;
				ModifiablePath.Split("/", nullptr, &PluginName, ESearchCase::IgnoreCase, ESearchDir::FromStart);
				PluginName.Split("/", &PluginName, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromStart);

				if (GetPlugin(PluginName) == nullptr) {
					if (IsAllowedDestination(ModifiablePath)) CreatePlugin(PluginName);
				}
			}
		}
	} else {
		FString RootName; {
			ModifiablePath.Split("/", nullptr, &RootName, ESearchCase::IgnoreCase, ESearchDir::FromStart);
			RootName.Split("/", &RootName, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromStart);
		}

		if (RootName != "Game" && RootName != "Engine" && GetPlugin(RootName) == nullptr) {
			if (IsAllowedDestination(ModifiablePath)) CreatePlugin(RootName);
		}

		ModifiablePath.Split("/", &ModifiablePath, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromEnd);

		ModifiablePath = ModifiablePath + "/";

		FRRedirects::Redirect(ModifiablePath, ModifiablePath + Name);
	}

	const FString PathWithGame = ModifiablePath + Name;

	if (PathWithGame.Contains(TEXT("//"), ESearchCase::CaseSensitive) || PathWithGame == "None" || PathWithGame.IsEmpty()) {
		FailureReason = "Attempted to create a package with name containing double slashes.\n\nUpdate your configuration to use a valid Export Directory.";
		return nullptr;
	}
	
	UPackage* Package = CreateAssetPackage(*PathWithGame);

	/* Null when the path is not a package path, which the overload above reports for itself */
	if (Package == nullptr) {
		FailureReason = "\"" + PathWithGame + "\" is not a package path.\n\nReflect from inside the export folder so the path resolves against the game's own.";

		return nullptr;
	}

	Package->FullyLoad();

	return Package;
}

UPackage* FAssetUtilities::CreateAssetPackage(const FString& Name, const FString& OutputPath) {
	FString StringIgnore = "";
	
	return CreateAssetPackage(Name, OutputPath, StringIgnore);
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
template bool FAssetUtilities::ConstructAsset<UMaterialInterface>(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<UMaterialInterface>& OutObject, bool& bSuccess);
template bool FAssetUtilities::ConstructAsset<USubsurfaceProfile>(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<USubsurfaceProfile>& OutObject, bool& bSuccess);
template bool FAssetUtilities::ConstructAsset<UTexture>(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<UTexture>& OutObject, bool& bSuccess);
template bool FAssetUtilities::ConstructAsset<UAnimSequence>(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<UAnimSequence>& OutObject, bool& bSuccess);
template bool FAssetUtilities::ConstructAsset<UMaterialParameterCollection>(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<UMaterialParameterCollection>& OutObject, bool& bSuccess);
template bool FAssetUtilities::ConstructAsset<USoundWave>(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<USoundWave>& OutObject, bool& bSuccess);
template bool FAssetUtilities::ConstructAsset<UObject>(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<UObject>& OutObject, bool& bSuccess);
template bool FAssetUtilities::ConstructAsset<UMaterialFunctionInterface>(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<UMaterialFunctionInterface>& OutObject, bool& bSuccess);
template bool FAssetUtilities::ConstructAsset<USoundNode>(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<USoundNode>& OutObject, bool& bSuccess);
template bool FAssetUtilities::ConstructAsset<UCurveLinearColor>(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<UCurveLinearColor>& OutObject, bool& bSuccess);
template bool FAssetUtilities::ConstructAsset<UTextureLightProfile>(const FString&, const FString&, const FString&, TObjectPtr<UTextureLightProfile>&, bool&);
template bool FAssetUtilities::ConstructAsset<UFontFace>(const FString&, const FString&, const FString&, TObjectPtr<UFontFace>&, bool&);

/* A skeletal mesh is skinned to its skeleton, so an import reaches for one the project may not
 * have yet */
template bool FAssetUtilities::ConstructAsset<USkeleton>(const FString&, const FString&, const FString&, TObjectPtr<USkeleton>&, bool&);

namespace {
	/* Packages with an import open further down the stack, innermost last. */
	TArray<FString> GAssetsUnderConstruction;

	/* The file a path names, without the asset inside it.
	 *
	 * Kept by the file rather than by the asset because a file is imported whole: every export in it
	 * is built by the one run. An HLOD proxy keeps four meshes, four materials and four textures
	 * under the one name, and each mesh reaches for the others while it is being built. Asked for by
	 * asset, those are four separate keys and none of them matches the run already open, so each one
	 * fetches the file again and builds all four again, four levels deeper every time. */
	FString FileOf(const FString& Path) {
		FString Named;

		return Path.Split(TEXT("."), &Named, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromStart) ? Named : Path;
	}

	/* Marks a file as being built for as long as the call constructing it is running, and reports
	 * whether that call is the one that opened it. */
	struct FConstructionScope {
		explicit FConstructionScope(const FString& InPath)
			: Path(FileOf(InPath))
			, bOwned(!GAssetsUnderConstruction.Contains(Path))
		{
			if (bOwned) {
				GAssetsUnderConstruction.Add(Path);
			}
		}

		~FConstructionScope() {
			if (bOwned) {
				GAssetsUnderConstruction.Remove(Path);
			}
		}

		FString Path;
		bool bOwned;
	};

	/* The asset a reference names, wherever it ended up.
	 *
	 * A reference says the file the asset was cooked in and the asset inside it. Kept together that
	 * is where it lands and the path reads straight through. Split apart it lands in a file of its
	 * own beside the others, and the same reference has to be read a second way: the folder that
	 * file is in, and the asset's own name. */
	template <typename T>
	TObjectPtr<T> FindByReference(const FString& Path) {
		FString Redirected;
		if (!FAssetUtilities::TryGetEngineContentDestination(Path, Redirected)) {
			Redirected = Path;
			FRRedirects::Redirect(Redirected);
		}

		if (TObjectPtr<T> Found = LoadObjectByPath<T>(Redirected)) {
			return Found;
		}

		FString Folder, File, Asset;

		if (!Redirected.Split(TEXT("."), &Folder, &Asset, ESearchCase::CaseSensitive, ESearchDir::FromEnd)) return nullptr;
		if (!Folder.Split(TEXT("/"), &Folder, &File, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) return nullptr;

		/* The file is already named for the asset, so there is no second spelling to try */
		if (File == Asset) return nullptr;

		/* Nor for a part of an asset. A subobject is named for the thing that holds it and never
		 * becomes a file of its own, so reading it as one would ask for a path nothing is at. */
		if (Asset.Contains(TEXT(":"))) return nullptr;

		return LoadObjectByPath<T>(Folder + TEXT("/") + Asset + TEXT(".") + Asset);
	}
}

/* Importing assets from Cloud */
template <typename T>
bool FAssetUtilities::ConstructAsset(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<T>& OutObject, bool& bSuccess) {
	if (Type.IsEmpty()) {
		return false;
	}

	/* Turned away before anything is asked of Cloud. Checked against where the game keeps it and
	 * where it lands here, since an entry can be written against either. Asked before the skeleton
	 * exception below: a skeleton somebody blacklisted is one they meant to keep out. */
	FString Blacklisted;
	FString Destination;
	const bool bRedirectedEngineContent = TryGetEngineContentDestination(RealPath, Destination);

	if (FRBlacklist::IsBlacklisted(Type, Path, FString(), nullptr, &Blacklisted) || FRBlacklist::IsPathBlacklisted(bRedirectedEngineContent ? Destination : RealPath, FString(), &Blacklisted)) {
		UE_LOG(LogReflection, Log, TEXT("Not reflecting \"%s\": %s"), *Path, *Blacklisted);

		return false;
	}

	/* References run both ways between materials and the functions they call, so a reference met
	 * partway through an import can name an asset whose import is already open further down the
	 * stack. Building it a second time is what breaks: the second pass empties the expression list
	 * and rebuilds the same expression names under the same outer, and NewObject on a name already
	 * taken destructs whatever holds it and constructs the replacement in that same allocation.
	 * The pass still running below is left holding those pointers, and finishes by walking freed
	 * memory: a virtual call on a dead vtable as soon as anything traces an input.
	 *
	 * The asset itself exists by this point, since it is created before its graph is filled in, so
	 * what is in memory is what the reference wants. It is handed back half built and the import
	 * that owns it finishes it. */
	/* The switch is for a run that turns them off, the setting for somebody who wants only what they asked for.
	 *
	 * Except for a Niagara script or an enum, which the setting does not keep out. A module, dynamic
	 * input or function a graph calls is part of what the graph is: without it the call has no
	 * signature and fails to compile, and so does every node after it on the same parameter map,
	 * which takes the whole script down with it (one missing helper in a System Update script was
	 * 160 errors). An enum is the type of a static switch or a pin, and a switch with no enum fails
	 * the same way. A texture or mesh left out still leaves an asset that works, a missing script or
	 * enum does not. So those are fetched with the setting off too, and the run says so. The run
	 * switch, which a commandlet turns off to fetch nothing at all, still keeps them out. */
	const bool bReferencesWanted = bImportReferences && GetSettings()->AssetSettings.ImportReferences;
	/* Decided by what asks for it, not by its type: anything a graph's own objects reference (the
	 * module a node calls, the enum a pin is typed by, a parameter collection a node reads) */
	const bool bNeededToCompile = GraphDependencyDepth > 0;
	const bool bScriptAnyway = bImportReferences && !bReferencesWanted && bNeededToCompile;

	if (!bReferencesWanted && Type != TEXT("Skeleton") && !bScriptAnyway) {
		return false;
	}

	if (bScriptAnyway) {
		FImportIssues::Report(
			EImportIssue::Setting,
			TEXT("Import References is off, but what the graphs reference was imported anyway: the graphs can't compile without it"),
			Path
		);
	}

	const bool IsTexture = FTextureTypes::IsSupported(Type);

	/* A kind of thing nothing can actually be. */
	const UClass* Meant = FindClassByType(Type);
	const bool bUnknowable = Meant != nullptr && (Meant->HasAnyClassFlags(CLASS_Abstract) || Meant == UClass::StaticClass());

	const FConstructionScope ConstructionScope(Path);

	/* Handed back unresolved only where the run that already has this file open is going to build it.
	 *
	 * That run builds the exports the reader takes and turns the rest down, and what it turns down
	 * is fetched here instead, one asset at a time, as each is referenced. A texture is the standing
	 * example: no package import has ever built one. Declining any of those waits on the open run to
	 * produce something it is never going to produce, and whatever asked comes out empty.
	 *
	 * So the question is not whether the file is open. It is whether the run holding it open builds
	 * this, which is the same question the reader asks of every export it is offered. Asked the same
	 * way here, a type nobody has taught the reader about still gets fetched rather than dropped. */
	if (!ConstructionScope.bOwned && (CanImport(Type) || bUnknowable)) {
		OutObject = FindByReference<T>(RealPath);
		bSuccess = OutObject != nullptr;

		return true;
	}

	/* Every path out of here is a request. With no Cloud to answer them, each reference would sit
	 * on a connection that is never going to be made, behind a scope announcing it, and end up
	 * exactly where it started. */
	const bool bConnectedCommandlet = IsRunningCommandlet() && !GReflectionRuntime.Profile.ProjectName.IsEmpty();
	if (!bConnectedCommandlet && !Cloud::Status::IsOpened()) {
		return false;
	}

	/* Reached from the middle of property deserialization, which has nowhere to put a callback,
	 * so the requests below have to be waited on. The scope is what keeps the editor drawn and
	 * cancellable while that happens. */
	const FBlockingRequestScope BlockingScope(FText::Format(
		NSLOCTEXT("Reflection", "CloudReflecting", "Reflecting {0}"),
		FText::FromString(Path)
	));

	FString GamePath = Path;

	/* Supported Assets */
	if (CanImport(Type, true) || IsTexture || bUnknowable) {
		if (IsTexture) {
			UTexture* Texture = nullptr;

			bSuccess = FTextureImport::FromCloud(RealPath, Path, Texture);
			if (bSuccess) OutObject = Cast<T>(Texture);

			return true;
		}

		const TSharedPtr<FJsonObject> Response = Cloud::Export::GetRawBlocking(Path);

		if (Response == nullptr || Path.IsEmpty()) return true;

		if (Response->HasField(TEXT("errored"))) {
			UE_LOG(LogReflection, Log, TEXT("Error from response \"%s\""), *Path);
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Exports = nullptr;
		if (!Response->TryGetArrayField(TEXT("exports"), Exports) || Exports->Num() == 0) return false;

		FString PackagePath;
		FString AssetName;
		if (!RealPath.Split(TEXT("."), &PackagePath, &AssetName)) return false;

		IImporter* OutImporter = nullptr;
		bSuccess = IImportReader::ReadExportsAndImport(*Exports, PackagePath + TEXT(".uasset"), OutImporter, true, false);
		OutObject = FindByReference<T>(RealPath);
		return OutObject != nullptr;
	}

	return false;
}

/* Textures live in FTextureImport, this is the seam other tools still reach through */
bool FAssetUtilities::Fast_Construct_TypeTexture(const TSharedPtr<FJsonObject>& JsonExport, const FString& Path, const FString& Type, TArray<uint8> Data, UTexture*& OutTexture) {
	return FTextureImport::FromExport(JsonExport, Path, Type, Data, OutTexture);
}
