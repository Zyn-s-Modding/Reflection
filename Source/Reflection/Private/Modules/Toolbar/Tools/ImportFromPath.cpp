/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Tools/ImportFromPath.h"

#include "Containers/Export.h"
#include "Importers/Constructor/ImportIssues.h"
#include "Importers/Constructor/ImportReader.h"
#include "Importers/Types/Texture/TextureImporter.h"
#include "Importers/Types/World/LevelRead.h"
#include "Importers/Types/World/LevelRead.h"
#include "Importers/Types/Texture/TextureTypes.h"
#include "Modules/Cloud/Cloud.h"
#include "Modules/Cloud/Remote.h"
#include "Modules/Cloud/Prefetch.h"
#include "Modules/Toolbar/Tools/ImportFolder.h"
#include "Modules/UI/Reflect/SReflectPathsDialog.h"
#include "Engine/EngineUtilities.h"
#include "Settings/Blacklist.h"
#include "Settings/Redirector.h"
#include "Utilities/ContentBrowser.h"

void TToolImportFromPath::Execute() {
	/* Nothing is asked of Cloud to put a list together, so it is not asked for until Reflect is */
	TArray<FString> Paths;

	/* Only the folder page fills these, and only with what it was told to let through */
	TSet<FString> AllowedTypes;

	const EReflectPathsChoice Choice = SReflectPathsDialog::Open(Paths, AllowedTypes);

	if (Choice == EReflectPathsChoice::Cancelled) {
		return;
	}

	/* Asked for now the reader has said go, rather than for the window to open */
	if (!Cloud::Status::IsOpened()) {
		Cloud::Status::NotifyNotRunning();

		return;
	}

	/* Nothing here goes through the reflect button, so this is where the project name gets fetched */
	if (!Cloud::EnsureMetadataBlocking()) {
		return;
	}

	/* The folder page did its own listing, so what came back is a folder rather than a queue: it
	 * is counted as one run and cancelled as one */
	if (Choice == EReflectPathsChoice::Folder) {
		TToolImportFolder::Run(Paths, AllowedTypes);

		return;
	}

	int32 Reflected = 0;
	const int32 Attempted = Paths.Num();

	FImportIssues::Begin();

	{
		const FScopedImportBrowse BrowseScope;
		/* The next few are fetched while the one before them is being built */
		const FCloudPrefetch::FScope PrefetchScope;
		QueueAhead(Paths);

		for (const FString& Path : Paths) {
			if (Import(Path)) {
				Reflected++;
			}

			FCloudPrefetch::AdvanceRun();
		}
	}

	const bool Successful = Reflected == Attempted;

	AppendNotification(
		FText::FromString(Successful ? "Reflected From Path" : "Reflected With Failures"),
		FText::FromString(FString::Printf(TEXT("%d of %d"), Reflected, Attempted)),
		Successful ? 2.0f : 5.0f,
		Successful ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail,
		true,
		310.0f
	);

	FImportIssues::Finish();
}

FString TToolImportFromPath::ToPackagePath(const FString& InPath) {
	/* Take whatever gets pasted: Type'/Game/Path/Asset.Asset', a path with an extension on it,
	 * or a bare package path */
	FString PackagePath = StripObjectOuter(InPath.TrimStartAndEnd());
	PackagePath.ReplaceInline(TEXT("\\"), TEXT("/"));

	/* Cloud cuts everything from the first dot, so an export name or an extension makes no
	 * difference to it, and the editor path below has to be cut the same way to match */
	int32 Dot;

	if (PackagePath.FindChar(TEXT('.'), Dot)) {
		LeftInline(PackagePath, Dot);
	}

	PackagePath.TrimStartAndEndInline();

	return PackagePath;
}

void TToolImportFromPath::QueueAhead(const TArray<FString>& Paths) {
	TArray<FString> Ahead;
	Ahead.Reserve(Paths.Num());

	for (const FString& Path : Paths) {
		FString PackagePath = ToPackagePath(Path);

		/* Never fetched by Import, so never fetched ahead of it either. Left as an empty slot
		 * rather than taken out, so the run's paths still line up one for one. */
		FString AssetName;
		PackagePath.Split(TEXT("/"), nullptr, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);

		if (PackagePath.IsEmpty() || FRBlacklist::IsPathBlacklisted(PackagePath, AssetName)) {
			PackagePath.Reset();
		}

		Ahead.Add(MoveTemp(PackagePath));
	}

	FCloudPrefetch::QueueRun(Ahead);
}

bool TToolImportFromPath::Import(const FString& InPath, const TSet<FString>& AllowedTypes, bool* bOutFiltered) {
	FString PackagePath = ToPackagePath(InPath);

	if (PackagePath.IsEmpty()) {
		return false;
	}

	FString AssetName;
	PackagePath.Split(TEXT("/"), nullptr, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);

	/* Put on the report as well as in the log. A path Cloud has nothing at is the ordinary way a
	 * reflect comes back with nothing, and the notification only ever says how many, so the reason
	 * lived in the output log and nowhere a person would think to look. */
	const auto ReportUnreachable = [&AssetName, &PackagePath, &InPath](const EImportIssue Kind, const TCHAR* Summary) {
		UE_LOG(LogReflection, Error, TEXT("%s: \"%s\""), Summary, *PackagePath);

		const FString Pasted = InPath.TrimStartAndEnd();

		FImportIssues::ReportFor(
			AssetName.IsEmpty() ? PackagePath : AssetName,
			PackagePath,
			FString(),
			Kind,
			Summary,
			/* Only worth saying when the path was cut down to get here */
			Pasted == PackagePath ? FString() : TEXT("Reflected from ") + Pasted
		);
	};

	/* Turned away by the blacklist. A folder run counts it with what its own filter let past, since
	 * both are things somebody asked to be left out. Asked for by name, it is worth saying why
	 * nothing came of it. */
	const auto TurnAway = [&](const FString& Type, const FString& Reason) {
		UE_LOG(LogReflection, Log, TEXT("Not reflecting \"%s\": %s"), *PackagePath, *Reason);

		if (bOutFiltered != nullptr) {
			*bOutFiltered = true;

			return;
		}

		FImportIssues::ReportFor(
			AssetName.IsEmpty() ? PackagePath : AssetName,
			PackagePath,
			Type,
			EImportIssue::Setting,
			TEXT("Blacklisted"),
			Reason + TEXT(". Take it off the blacklist in the settings to reflect it.")
		);
	};

	/* Before anything is fetched, since a path is all a folder or an asset entry needs */
	FString Blacklisted;

	if (FRBlacklist::IsPathBlacklisted(PackagePath, AssetName, &Blacklisted)) {
		TurnAway(FString(), Blacklisted);

		return false;
	}

	/* Reached straight off a menu click, so nothing here has a continuation to hand a callback to.
	 * The scope is what keeps the editor drawn and cancellable while the requests run. */
	const FBlockingRequestScope BlockingScope(FText::Format(
		NSLOCTEXT("Reflection", "ReflectingPath", "Reflecting {0}"),
		FText::FromString(PackagePath)
	));

	TSharedPtr<FJsonObject> Response = Cloud::Export::GetRawBlocking(PackagePath);

	/* A path in this project, such as an asset right-clicked in the Content Browser, is where a
	 * redirect put the asset rather than where the game keeps it, and Cloud only answers to the
	 * game's. Each path that redirects here is asked for in turn, and the first Cloud has is the
	 * one reflected; the redirect then puts it back where it was. */
	const auto HasExports = [](const TSharedPtr<FJsonObject>& Answer) {
		const TArray<TSharedPtr<FJsonValue>>* Listed = nullptr;
		return Answer.IsValid() && Answer->TryGetArrayField(TEXT("exports"), Listed) && Listed->Num() > 0;
	};

	if (!HasExports(Response)) {
		for (const FString& Source : FRRedirects::Sources(PackagePath)) {
			if (FRBlacklist::IsPathBlacklisted(Source, AssetName, &Blacklisted)) continue;

			TSharedPtr<FJsonObject> Answer = Cloud::Export::GetRawBlocking(Source);

			if (!HasExports(Answer)) continue;

			UE_LOG(LogReflection, Display, TEXT("\"%s\" is reflected from \"%s\", the path the game keeps it at"), *PackagePath, *Source);

			PackagePath = Source;
			Response = MoveTemp(Answer);

			break;
		}
	}

	if (Response == nullptr || !Response->HasField(TEXT("exports"))) {
		ReportUnreachable(EImportIssue::MissingAsset, TEXT("Cloud has nothing at this path"));

		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>& Exports = Response->GetArrayField(TEXT("exports"));
	if (Exports.Num() == 0) {
		ReportUnreachable(EImportIssue::MissingAsset, TEXT("Cloud has nothing at this path"));

		return false;
	}

	const TSharedPtr<FJsonObject> Export = Exports[0]->AsObject();

	FString Type;
	if (!Export.IsValid() || !Export->TryGetStringField(TEXT("Type"), Type)) {
		ReportUnreachable(EImportIssue::Data, TEXT("Cloud returned an export with no type"));

		return false;
	}

	if (FRBlacklist::IsTypeBlacklisted(Type, nullptr, &Blacklisted)) {
		TurnAway(Type, Blacklisted);

		return false;
	}

	if (AllowedTypes.Num() > 0 && !AllowedTypes.Contains(Type)) {
		if (bOutFiltered != nullptr) {
			*bOutFiltered = true;
		}

		return false;
	}

	/* A level, which reads into the open level rather than into a package of its own */
	if (FLevelRead::Handles(Exports)) {
		return FLevelRead::FromCloud(Exports, PackagePath);
	}

	if (FTextureTypes::IsSupported(Type)) {
		UTexture* Texture = nullptr;

		/* Built from the answer already in hand rather than asked for again */
		return FTextureImport::FromResponse(Response, PackagePath + "." + AssetName, PackagePath, Texture);
	}

	IImporter* OutImporter = nullptr;

	return IImportReader::ReadExportsAndImport(Exports, PackagePath, OutImporter, false, false);
}
