/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Tools/SelectedAssetsBase.h"

#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"

#include "Modules/Cloud/Cloud.h"
#include "Modules/UI/StyleModule.h"
#include "Engine/EngineUtilities.h"
#include "Settings/Redirector.h"

void TSelectedAssetsBase::Execute() {
	if (bRunning) {
		AppendNotification(
			FText::FromString("Already Running"),
			FText::FromString("Wait for the current Cloud run to finish."),
			3.0f,
			FReflectionStyle::Get().GetBrush("Toolbar.Icon"),
			SNotificationItem::CS_None,
			false,
			310.0f
		);

		return;
	}

	/* Reached straight off the Cloud menu, never through the reflect button, so the project name
	 * the exports get resolved against is fetched here */
	Cloud::EnsureMetadataBlocking();

	/* What is selected is a more specific request than the folder it happens to sit in, and all
	 * of it is the request. Reading one asset out of the selection and dropping the rest made
	 * selecting three of them the same as selecting the first. */
	TArray<FAssetData> AssetDataList;

	const FContentBrowserModule& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	ContentBrowser.Get().GetSelectedAssets(AssetDataList);

	/* Nothing picked out, so the folder that is open is what was meant */
	if (AssetDataList.Num() == 0) {
		AssetDataList = GetAssetsInSelectedFolder();
	}

	const FName SupportedClass = GetSupportedClass();

	Queue.Reset();

	for (const FAssetData& AssetData : AssetDataList) {
		if (!AssetData.IsValid()) continue;
		if (!SupportedClass.IsNone() && GetAssetDataClass(AssetData) != SupportedClass) continue;

		const FString ObjectPath = GetAssetObjectPath(AssetData);
		if (ObjectPath.IsEmpty()) continue;

		Queue.Add(ObjectPath);
	}

	if (Queue.Num() == 0) {
		return;
	}

	QueueIndex = 0;
	bRunning = true;

	RequestNext();
}

void TSelectedAssetsBase::RequestNext() {
	if (!Queue.IsValidIndex(QueueIndex)) {
		Finish();

		return;
	}

	/* The asset is where it sits in this project, which a redirect may have moved it to from where
	 * the game keeps it, and Cloud only answers to the game's path. So it is asked for where it
	 * is, then at each path that redirects here, until Cloud has one. */
	TArray<FString> Paths { Queue[QueueIndex] };
	FString Package = Queue[QueueIndex];
	if (int32 Dot; Package.FindChar(TEXT('.'), Dot)) Package.LeftInline(Dot);
	Paths.Append(FRRedirects::Sources(Package));

	/* One request at a time: Process deserializes into live assets, and the tools were written
	 * expecting to be the only thing doing that */
	FetchFirstHeld(MoveTemp(Paths), 0, [this](const TArray<TSharedPtr<FJsonValue>>& Exports) {
		/* Copied out of the response, which does not outlive a deferred call */
		const TArray<TSharedPtr<FJsonValue>> OwnedExports = Exports;

		Cloud::RunWhenSafe([this, OwnedExports]() {
			HandleExports(OwnedExports);
		});
	});
}

void TSelectedAssetsBase::FetchFirstHeld(TArray<FString> Paths, const int32 At, TFunction<void(const TArray<TSharedPtr<FJsonValue>>&)> OnExports) {
	Cloud::Export::GetRawExportsAsync(Paths[At], [Paths, At, OnExports](const TArray<TSharedPtr<FJsonValue>>& Exports) {
		if (Exports.Num() > 0 || !Paths.IsValidIndex(At + 1)) {
			OnExports(Exports);

			return;
		}

		FetchFirstHeld(Paths, At + 1, OnExports);
	});
}

void TSelectedAssetsBase::HandleExports(const TArray<TSharedPtr<FJsonValue>>& Exports) {
	const FString ObjectPath = Queue.IsValidIndex(QueueIndex) ? Queue[QueueIndex] : FString();

	QueueIndex++;

	if (Exports.Num() > 0 && !ObjectPath.IsEmpty()) {
		/* Looked up now rather than held from Execute: the asset could have been unloaded
		 * while the request was out */
		if (UObject* Object = LoadObjectByPath<UObject>(ObjectPath)) {
			/* The serializers are plain NewObject's that nothing keeps alive, and a run spans
			 * enough ticks for a collection to happen between two assets. Building them here
			 * puts their whole lifetime inside one call stack, and hands each asset a
			 * serializer with no leftover state from the last one. */
			CreateSerializer();

			Process(Object, Exports);
		}
	}

	RequestNext();
}

void TSelectedAssetsBase::BrowseToWhenFinished(const UObject* Object) {
	if (Object == nullptr) return;

	PendingBrowseTo = Object->GetPathName();
}

void TSelectedAssetsBase::Finish() {
	Queue.Reset();

	QueueIndex = 0;
	bRunning = false;

	if (!PendingBrowseTo.IsEmpty()) {
		/* Looked up now rather than held from Process: the asset could have been unloaded since */
		if (UObject* Object = LoadObjectByPath<UObject>(PendingBrowseTo)) {
			BrowseToAsset(Object);
		}

		PendingBrowseTo.Empty();
	}
}
