/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/* Fetches Cloud answers before an import asks for them.
 *
 * Importing is one asset at a time and has to be: it builds assets, and only the game thread may.
 * What it waits on does not have to be. Every asset used to be asked for at the moment it was
 * needed, and then the editor sat idle while Mirror read the package, and Mirror sat idle while the
 * editor built it. Here the next few are asked for while the current one is being built, several at
 * once so Mirror reads them on as many cores, and each answer is taken apart on a worker thread as
 * soon as it lands. By the time the import reaches one, it is usually already a tree in memory.
 *
 * Nothing about what gets built changes. An answer fetched ahead is the same answer the import
 * would have got by asking, and anything never queued is asked for exactly as before. Only the
 * requests an export or a texture's pixels go through, and only the plain form of those, are ever
 * taken from here.
 *
 * Everything is held for the length of a run and let go when the outermost FScope closes, so
 * nothing fetched ahead outlives what asked for it.
 *
 * Game thread only. Called from anywhere else, every entry point does nothing and says so by
 * handing the caller back to asking for itself. */
class REFLECTION_API FCloudPrefetch {
public:
	/* Held for the length of a run, nested freely */
	struct REFLECTION_API FScope {
		FScope();
		~FScope();

		FScope(const FScope&) = delete;
		FScope& operator=(const FScope&) = delete;

	private:
		bool bCounted = false;
	};

	/* Raw exports a run is about to reach, in the order it will reach them. Fetched a few at a time
	 * ahead of where the run has got to, so a folder of thousands is never in memory all at once. */
	static void QueueRun(const TArray<FString>& Paths);

	/* Says the run is done with the next of the paths QueueRun was given, one call per path, taken
	 * or not. What it never took is let go, so it stops holding a place ahead of the rest. */
	static void AdvanceRun();

	/* Raw exports an import is likely to reach for while it runs. Fetched ahead of the run's own,
	 * since they are wanted sooner. Hands back what it queued, for Forget once the import is done. */
	static TArray<FString> QueueReferences(const TArray<FString>& Paths);

	/* Lets go of entries that were queued and never taken, cancelling any still on their way */
	static void Forget(const TArray<FString>& Keys);

	/* The raw export for Path, if it was queued, waited on where it is still on its way.
	 *
	 * True when this answered for the request, with OutResponse what the request itself would have
	 * come back with, null included. False when it was never queued, or never reached the Cloud,
	 * and the caller asks for itself. */
	static bool TakeRaw(const FString& Path, TSharedPtr<FJsonObject>& OutResponse);

	/* The same for the bytes Cloud::Export::GetBinaryBlocking asks for, with bOutResult what that
	 * would have returned */
	static bool TakeBinary(const FString& Path, const FString& ContentType, const FString& Named, TArray<uint8>& OutData, bool& bOutResult, FString* OutContentType);

	/* Starts what can start and picks up what finished. Called from every blocking wait, which is
	 * where the editor spends the time it is not building something. */
	static void Tick();

	/* Drops everything, cancelling whatever is on its way */
	static void Reset();
};
