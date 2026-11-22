/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

/* Asks the blacklist in the settings whether something may be reflected.
 *
 * Paths are taken in any spelling the plugin meets them in: an editor path, a Cloud path, a path
 * off disk, with or without an extension, with or without the object on the end, or a copied
 * reference with its type around it. Each is turned into the editor's spelling before it is
 * compared, and compared both before and after the redirectors move it. */
struct REFLECTION_API FRBlacklist {
	/* Whether the type is turned away. Class, where the caller already has it, saves looking it up
	 * again for Include Child Types. */
	static bool IsTypeBlacklisted(const FString& Type, const UClass* Class = nullptr, FString* OutReason = nullptr);

	/* Whether the path is turned away, by folder or by asset. Object names the asset where the
	 * path only names its package; left empty, the asset named after the package is meant. */
	static bool IsPathBlacklisted(const FString& Path, const FString& Object = FString(), FString* OutReason = nullptr);

	/* Either of the above */
	static bool IsBlacklisted(const FString& Type, const FString& Path, const FString& Object = FString(), const UClass* Class = nullptr, FString* OutReason = nullptr);
};
