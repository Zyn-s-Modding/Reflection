/* Copyright Reflection Contributors 2024-2026 */

#include "Settings/Redirector.h"

#include "Importers/Constructor/Asset.h"
#include "Misc/PackageName.h"

#include "Settings/ReflectionSettings.h"
#include "Settings/Runtime.h"
#include "Importers/Constructor/Importer.h"

/********* Redirect History ************ */
TMap<FString, FString> FRRedirects::History;

bool FRRedirector::IsEnabled() const {
	bool bIsEnabled = Enable;

	if (!GReflectionRuntime.Profile.Name.IsEmpty()) {
		if (Profiles.Num() > 0 && !Profiles.Contains(GReflectionRuntime.Profile.Name)) {
			bIsEnabled = false;
		}
	}

	return bIsEnabled;
}

void FRRedirects::Clear() {
	History.Empty();
}

static bool ApplyPoint(FString& Path, const FString& From, const FString& To) {
	if (From.IsEmpty()) return false;

	const bool bRooted = From.StartsWith(TEXT("/"));
	const int32 At = bRooted ? (Path.StartsWith(From, ESearchCase::IgnoreCase) ? 0 : INDEX_NONE)
		: Path.Find(From, ESearchCase::IgnoreCase, ESearchDir::FromStart);
	if (At == INDEX_NONE) return false;
	const int32 End = At + From.Len();
	if (bRooted && !From.EndsWith(TEXT("/")) && End < Path.Len()
		&& Path[End] != TEXT('/') && Path[End] != TEXT('.') && Path[End] != TEXT(':')) return false;

	FString Replacement = To;
	FString Suffix = Path.RightChop(End);
	if (bRooted && !Replacement.IsEmpty()) {
		if (Replacement.EndsWith(TEXT("/")) && Suffix.StartsWith(TEXT("/"))) Suffix.RightChopInline(1);
		else if (From.EndsWith(TEXT("/")) && !Replacement.EndsWith(TEXT("/"))) Replacement += TEXT("/");
	}
	Path = Path.Left(At) + Replacement + Suffix;

	return true;
}

static bool HasOriginalAsset(const FString& Path) {
	if (Path.IsEmpty() || Path.EndsWith(TEXT("/"))) return false;

	const FString Package = FPackageName::ObjectPathToPackageName(Path);
	if (!FPackageName::IsValidLongPackageName(Package)) return false;

	if (const UPackage* InMemory = FindPackage(nullptr, *Package)) {
		TArray<UObject*> Objects;
		GetObjectsWithOuter(InMemory, Objects, false);
		for (const UObject* Object : Objects) {
			if (IsValid(Object) && Object->IsAsset() && !Object->IsA<UObjectRedirector>()) return true;
		}
		if (InMemory->IsFullyLoaded()) return false;
	}

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> Assets;
	const FName PackageName(*Package);
	Registry.GetAssetsByPackageName(PackageName, Assets, true);
	if (Assets.IsEmpty()) {
		FString Filename;
		if (!PackageFileOf(Package, Filename)) return false;
		Registry.ScanFilesSynchronous({Filename});
		Registry.GetAssetsByPackageName(PackageName, Assets, true);
	}
	return Assets.ContainsByPredicate([](const FAssetData& Asset) { return !Asset.IsRedirector(); });
}

void FRRedirects::Redirect(FString& Path, const FString& AlternatePackage) {
	FString Destination = Path;

	const UReflectionSettings* Settings = GetSettings();

	for (const FRRedirector& Redirect : Settings->Redirectors) {
		if (!Redirect.IsEnabled()) continue;

		for (const FRRedirectorPoint& Point : Redirect.Points) {
			ApplyPoint(Destination, Point.From, Point.To);
		}
	}

	/* A run confined to one folder (a commandlet's -onlyunder=) takes in what the game keeps under
	 * a mount this project doesn't have, a game plugin a normal import would create, under that
	 * folder too, and looks for it there. A test import then reaches everything a real one does
	 * without writing anywhere else. Mounts the project has are left to the refusal. */
	if (!FAssetUtilities::OnlyUnder.IsEmpty() && Destination.StartsWith(TEXT("/")) && !FAssetUtilities::IsAllowedDestination(Destination)) {
		const int32 Slash = Destination.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
		const FString Root = Slash == INDEX_NONE ? Destination + TEXT("/") : Destination.Left(Slash + 1);
		if (Root.Len() > 2 && !FPackageName::MountPointExists(Root)) {
			FString Under = FAssetUtilities::OnlyUnder;
			while (Under.RemoveFromEnd(TEXT("/"))) {}
			Destination = Under + Destination;
		}
	}

	if (Destination.Equals(Path, ESearchCase::CaseSensitive)) return;
	if (Settings->bRedirectOnlyMissingAssets && (HasOriginalAsset(Path) || HasOriginalAsset(AlternatePackage))) return;

	History.Add(Destination, Path);
	Path = MoveTemp(Destination);
}

FRRedirectPreview FRRedirects::Preview(const FString& InPath) {
	FRRedirectPreview Preview;
	Preview.Path = InPath;

	const UReflectionSettings* Settings = GetSettings();

	if (Settings == nullptr) return Preview;

	/* The same order Redirect goes in, since a point works on what the one before it left */
	for (int32 At = 0; At < Settings->Redirectors.Num(); ++At) {
		const FRRedirector& Redirect = Settings->Redirectors[At];

		if (!Redirect.IsEnabled()) continue;

		bool Matched = false;

		for (const FRRedirectorPoint& Point : Redirect.Points) {
			if (ApplyPoint(Preview.Path, Point.From, Point.To)) {
				Matched = true;
			}
		}

		/* Named once however many of its points matched */
		if (Matched) {
			Preview.By.AddUnique(Redirect.GetName());
			Preview.Indices.AddUnique(At);
		}
	}

	if (Preview.Path.Equals(InPath, ESearchCase::CaseSensitive)
		|| (Settings->bRedirectOnlyMissingAssets && HasOriginalAsset(InPath))) {
		return {InPath, {}, {}};
	}

	return Preview;
}

void FRRedirects::Reverse(FString& Path) {
	if (const FString* Original = History.Find(Path)) Path = *Original;
}
