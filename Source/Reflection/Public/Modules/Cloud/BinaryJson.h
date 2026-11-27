/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/* Reflection Binary JSON, the encoding Cloud answers in when asked.
 *
 * It is the same tree a json answer carries, and it comes out as the same FJsonObject, so nothing
 * that reads an export knows or cares which one arrived. What it saves is the reading: Unreal's json
 * reader goes a character at a time and builds every key, string and number out of text, which is
 * most of what importing a few thousand assets used to spend its time on. Here keys are sent once
 * and then named by number, repeated strings the same, and numbers arrive as numbers.
 *
 * Asked for in the Accept header and never assumed, so a Mirror that predates it answers with json
 * and that is read exactly as it always was. The layout is written out beside the encoder, in
 * Mirror's Objects/BinaryJson.cs. */
struct REFLECTION_API FReflectionBinaryJson {
	/* The content type Cloud labels it with */
	static const TCHAR* ContentType;

	/* What a request sends to say it can take this, json still being acceptable */
	static const TCHAR* AcceptHeader;

	/* Whether a response's content type says it is in this encoding */
	static bool IsContentType(const FString& InContentType);

	/* The tree the bytes hold, or null where they are not one whole document.
	 *
	 * Builds nothing but the tree it hands back, so it is safe on any thread, which is what lets a
	 * response be taken apart off the game thread while the game thread gets on with importing. */
	static TSharedPtr<FJsonValue> Decode(const uint8* Data, int64 Size);

	/* The same, for a document whose top level is an object, which is every Cloud answer */
	static TSharedPtr<FJsonObject> DecodeObject(const uint8* Data, int64 Size);
};
