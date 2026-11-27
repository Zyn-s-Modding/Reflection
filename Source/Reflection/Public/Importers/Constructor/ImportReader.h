/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importer.h"
#include "Dom/JsonValue.h"
#include "Containers/Export.h"

class REFLECTION_API IImportReader {
public:
	static bool ReadExportsAndImport(const TArray<TSharedPtr<FJsonValue>>& Exports, const FString& File, IImporter*& OutImporter, bool HideNotifications = false, bool bUseRelativePath = true);
	static IImporter* ReadExportAndImport(FUObjectExportContainer* Container, FUObjectExport* Export, FString File, bool HideNotifications = false, bool bUseRelativePath = true, bool* bImportAttempted = nullptr);
	static IImporter* ImportReference(const FString& File);

	/* Has Cloud fetch, ahead of time, the references in Container an import would fetch as it
	 * reached them. Hands back what was queued, for FCloudPrefetch::Forget once the import is done
	 * with it, and only queues anything inside an FCloudPrefetch::FScope. */
	static TArray<FString> PrefetchReferencesOf(FUObjectExportContainer* Container);
};
