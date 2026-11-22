/* Copyright Reflection Contributors 2024-2026 */

#include "Settings/Blacklist.h"

#include "Containers/Export.h"
#include "Engine/Compatibility.h"
#include "Misc/Paths.h"
#include "Settings/Redirector.h"
#include "Settings/SettingsAccess.h"

namespace {
	/* The blacklist, or nothing where it is switched off */
	const FRBlacklistSettings* Active() {
		const UReflectionSettings* Settings = GetSettings();

		if (Settings == nullptr || !Settings->Blacklist.Enabled) return nullptr;

		return &Settings->Blacklist;
	}

	/* A path in the one spelling everything here is compared in: the editor's, with no extension.
	 *
	 * Done here rather than through ToEditorPackagePath, which records a redirect as having happened,
	 * and which only knows a Cloud path once a profile has said what the project is called. The root
	 * is read off the path instead, the same way a package is made for one: whatever sits in front of
	 * Content is the project or a plugin, and only a plugin keeps its name. */
	FString Normalize(const FString& In) {
		FString Path = StripObjectOuter(In.TrimStartAndEnd());

		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		Path.ReplaceInline(TEXT("\""), TEXT(""));
		Path.TrimStartAndEndInline();

		if (Path.IsEmpty()) return Path;

		/* Only the extensions a file comes with. Anything else after a dot is the object. */
		static const TCHAR* Extensions[] = { TEXT(".uasset"), TEXT(".umap"), TEXT(".uexp"), TEXT(".json") };

		for (const TCHAR* Extension : Extensions) {
			if (Path.EndsWith(Extension, ESearchCase::IgnoreCase)) {
				Path.LeftChopInline(FCString::Strlen(Extension));

				break;
			}
		}

		const bool bEditorPath = Path.StartsWith(TEXT("/Game/"), ESearchCase::IgnoreCase) || Path.StartsWith(TEXT("/Engine/"), ESearchCase::IgnoreCase);
		const int32 Content = bEditorPath ? INDEX_NONE : Path.Find(TEXT("/Content/"), ESearchCase::IgnoreCase, ESearchDir::FromStart);

		if (Content != INDEX_NONE) {
			const FString Before = Path.Left(Content);
			const FString After = Path.RightChop(Content + 9);

			FString Root = Before;
			Before.Split(TEXT("/"), nullptr, &Root, ESearchCase::IgnoreCase, ESearchDir::FromEnd);

			const bool bPlugin = Before.StartsWith(TEXT("Plugins/"), ESearchCase::IgnoreCase) || Before.Contains(TEXT("/Plugins/"), ESearchCase::IgnoreCase);

			if (bPlugin) {
				Path = TEXT("/") + Root + TEXT("/") + After;
			} else if (Root.Equals(TEXT("Engine"), ESearchCase::IgnoreCase)) {
				Path = TEXT("/Engine/") + After;
			} else {
				Path = TEXT("/Game/") + After;
			}
		}

		if (!Path.StartsWith(TEXT("/"))) {
			Path = TEXT("/") + Path;
		}

		while (Path.ReplaceInline(TEXT("//"), TEXT("/")) > 0) {}

		return Path;
	}

	/* The package a path is in, and the asset it names inside it, if it names one. A part of an
	 * asset is held to the asset it is part of. */
	void SplitObject(const FString& Path, FString& OutPackage, FString& OutObject) {
		if (!Path.Split(TEXT("."), &OutPackage, &OutObject, ESearchCase::CaseSensitive, ESearchDir::FromStart)) {
			OutPackage = Path;
			OutObject.Empty();
		}

		int32 At;

		if (OutObject.FindChar(TEXT(':'), At)) OutObject.LeftInline(At);
		if (OutObject.FindChar(TEXT('.'), At)) OutObject.LeftInline(At);

		while (OutPackage.RemoveFromEnd(TEXT("/"))) {}
	}

	/* A blueprint's class carries a _C its asset does not, and either may be the one written down */
	bool SameObject(const FString& A, const FString& B) {
		return A.Equals(B, ESearchCase::IgnoreCase)
			|| (A + TEXT("_C")).Equals(B, ESearchCase::IgnoreCase)
			|| A.Equals(B + TEXT("_C"), ESearchCase::IgnoreCase);
	}

	/* A type as written down, without a module or a prefix in front of it */
	FString TypeName(const FString& In) {
		FString Name = StripObjectOuter(In.TrimStartAndEnd());

		/* /Script/Engine.Texture2D */
		Name.Split(TEXT("."), nullptr, &Name, ESearchCase::CaseSensitive, ESearchDir::FromEnd);

		return Name.TrimStartAndEnd();
	}

	bool MatchesPath(const FRBlacklistSettings& Blacklist, const FString& Path, const FString& InObject, FString* OutReason) {
		FString Package, Object;
		SplitObject(Path, Package, Object);

		if (Package.IsEmpty() || Package == TEXT("/")) return false;

		if (!InObject.IsEmpty()) Object = InObject;

		/* Named after its package where nothing says otherwise, which is what the file's own asset is */
		const FString Asset = Object.IsEmpty() ? FPaths::GetCleanFilename(Package) : Object;

		const FString InFolder = Package + TEXT("/");

		for (const FString& Entry : Blacklist.Folders) {
			FString Folder = Normalize(Entry);

			if (Folder.IsEmpty() || Folder == TEXT("/")) continue;

			/* Without the object a folder entry could be read as one, so it never gets one */
			int32 Dot;
			if (Folder.FindChar(TEXT('.'), Dot)) Folder.LeftInline(Dot);

			if (!Folder.EndsWith(TEXT("/"))) Folder += TEXT("/");

			if (InFolder.StartsWith(Folder, ESearchCase::IgnoreCase)) {
				if (OutReason) *OutReason = FString::Printf(TEXT("In the blacklisted folder \"%s\""), *Entry.TrimStartAndEnd());

				return true;
			}
		}

		for (const FString& Entry : Blacklist.Assets) {
			const FString Normalized = Normalize(Entry);

			if (Normalized.IsEmpty() || Normalized == TEXT("/")) continue;

			FString EntryPackage, EntryObject;
			SplitObject(Normalized, EntryPackage, EntryObject);

			if (!EntryPackage.Equals(Package, ESearchCase::IgnoreCase)) continue;

			/* No object, so the whole package */
			if (EntryObject.IsEmpty() || SameObject(EntryObject, Asset)) {
				if (OutReason) *OutReason = FString::Printf(TEXT("The asset \"%s\" is blacklisted"), *Entry.TrimStartAndEnd());

				return true;
			}
		}

		return false;
	}
}

bool FRBlacklist::IsTypeBlacklisted(const FString& InType, const UClass* Class, FString* OutReason) {
	const FRBlacklistSettings* Blacklist = Active();

	if (Blacklist == nullptr || Blacklist->Types.Num() == 0) return false;

	const FString Type = TypeName(InType);

	if (Type.IsEmpty()) return false;

	/* A blueprint is asked for by its class, and the class is not the asset anyone would write down */
	FString Generating;
	const bool bGenerated = Type.EndsWith(TEXT("GeneratedClass")) && Type.Split(TEXT("GeneratedClass"), &Generating, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);

	bool bLookedUp = Class != nullptr;

	for (const FString& Entry : Blacklist->Types) {
		const FString Blocked = TypeName(Entry);

		if (Blocked.IsEmpty()) continue;

		bool bMatched = Blocked.Equals(Type, ESearchCase::IgnoreCase)
			|| (bGenerated && !Generating.IsEmpty() && Blocked.Equals(Generating, ESearchCase::IgnoreCase));

		if (!bMatched && Blacklist->IncludeChildTypes) {
			if (!bLookedUp) {
				Class = FindClassByType(Type);
				bLookedUp = true;
			}

			if (Class != nullptr) {
				const UClass* BlockedClass = FindClassByType(Blocked);

				bMatched = BlockedClass != nullptr && Class->IsChildOf(BlockedClass);
			}
		}

		if (bMatched) {
			if (OutReason) *OutReason = FString::Printf(TEXT("The type \"%s\" is blacklisted"), *Blocked);

			return true;
		}
	}

	return false;
}

bool FRBlacklist::IsPathBlacklisted(const FString& InPath, const FString& Object, FString* OutReason) {
	const FRBlacklistSettings* Blacklist = Active();

	if (Blacklist == nullptr || (Blacklist->Assets.Num() == 0 && Blacklist->Folders.Num() == 0)) return false;

	const FString Path = Normalize(InPath);

	if (Path.IsEmpty()) return false;

	if (MatchesPath(*Blacklist, Path, Object, OutReason)) return true;

	/* And where it would land, for an entry written against this project rather than the game */
	const FRRedirectPreview Redirected = FRRedirects::Preview(Path);

	return Redirected.Moved() && MatchesPath(*Blacklist, Redirected.Path, Object, OutReason);
}

bool FRBlacklist::IsBlacklisted(const FString& Type, const FString& Path, const FString& Object, const UClass* Class, FString* OutReason) {
	return IsTypeBlacklisted(Type, Class, OutReason) || IsPathBlacklisted(Path, Object, OutReason);
}
