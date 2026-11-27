/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/* Selects a factory image, a cooked mip, or volume source data for the texture class. */
struct REFLECTION_API FTextureTypes {
	static constexpr const TCHAR* VolumeSourceContentType = TEXT("application/x-reflection-volume");

	/* Every texture class with a creator behind it */
	static const TArray<FString>& All();

	static bool IsSupported(const FString& Type);

	/* Render targets are created empty, every other texture arrives with pixels */
	static bool HasPixelPayload(const FString& Type);

	static const TCHAR* PixelContentType(const FString& Type, const TSharedPtr<FJsonObject>& Export);

	/* Whether the pixels have to come down as raw mip bytes rather than an encoded image */
	static bool RequiresRawMipData(const FString& Type, bool bIsVectorDisplacementMap);

	/* Compression settings an encoded image can't carry, read straight off the export */
	static bool IsVectorDisplacementMap(const TSharedPtr<FJsonObject>& Export);
};
