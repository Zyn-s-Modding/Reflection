/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/* TObjectPtr below is the engine's own from 5.0 and the shim from here before that */
#include "Engine/Compatibility.h"

struct REFLECTION_API FAssetUtilities {
public:
	/* Creates a UPackage to create assets in the Content Browser. */
	static UPackage* CreateAssetPackage(const FString& FullPath);
	static UPackage* CreateAssetPackage(const FString& Name, const FString& OutputPath);
	static UPackage* CreateAssetPackage(const FString& Name, const FString& OutputPath, FString& FailureReason);
	static bool TryGetEngineContentDestination(const FString& Path, FString& Destination);
	
public:
	/* Off, a reference to an asset the project doesn't have is left null rather than reflected.
	 * A skeleton is still fetched: a skeletal mesh has nothing to build against without one. */
	static inline bool bImportReferences = true;

	/* What has already been reflected during the import now running.
	 *
	 * A material naming the same texture in two slots asks for it twice, and with existing textures
	 * set to reflect anyway there is nothing to say the second ask is the same as the first. Kept
	 * for the length of one import rather than the session, so reflecting the same asset again
	 * later still fetches it. */
	static inline TSet<FString> ReflectedThisRun;

	static inline int32 ImportDepth = 0;

	/* Above nought while the properties of a graph's own objects (nodes, the graph, its script
	 * variables) are being read: what those reference is needed for the graph to compile */
	static inline int32 GraphDependencyDepth = 0;

	/* A package path the run may write under, set by a commandlet's -onlyunder=. Empty, anywhere.
	 * Creating or saving a package outside it is refused, so a run pointed at a test folder can't
	 * write over what the project already has, whatever the redirects end up doing. */
	static inline FString OnlyUnder;

	static bool IsAllowedDestination(const FString& PackageName) {
		if (OnlyUnder.IsEmpty()) return true;
		FString Root = OnlyUnder;
		while (Root.RemoveFromEnd(TEXT("/"))) {}
		return PackageName.Equals(Root, ESearchCase::IgnoreCase) || PackageName.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase);
	}

	/* Package names asked about during the import now running and found to be nowhere, on disk or
	 * in memory.
	 *
	 * Every reference is looked for under several spellings before it is fetched, and each of those
	 * is a trip to the file system. One import reaching through a few hundred references asks about
	 * the same missing packages over and over. Nothing lands on disk during a run that was not
	 * created in memory first, and creating a package takes it off this list, so a miss holds for
	 * the length of the run. */
	static inline TSet<FString> MissingPackagesThisRun;

	/* Whether a package name is known, this run, to be nowhere. Only remembered inside a run. */
	static bool IsKnownMissing(const FString& PackageName) {
		return ImportDepth > 0 && MissingPackagesThisRun.Contains(PackageName);
	}

	static void RememberMissing(const FString& PackageName) {
		if (ImportDepth > 0) MissingPackagesThisRun.Add(PackageName);
	}

	/* Saves asked for during a run, written once when the outermost import is done.
	 *
	 * A package kept together is built by several passes and each one used to save the whole
	 * package again, and a material was saved once half built and again when finished. Written
	 * at the end each package is saved once, and the saving no longer sits between one reference
	 * and the next. Outside a run a save is written straight away. */
	static void QueueSave(UPackage* Package);
	static void FlushSaves();

	/* Held for the length of one import. The outermost one is what clears the list: references
	 * reflect through the same path, and each would otherwise wipe what its parent had recorded. */
	struct REFLECTION_API FRunScope {
		FRunScope() {
			if (ImportDepth++ == 0) {
				ReflectedThisRun.Empty();
				MissingPackagesThisRun.Empty();
			}
		}

		~FRunScope() {
			if (ImportDepth == 1) {
				/* Still counted as inside the run while saving, so anything the save reaches
				 * that asks to save again is queued rather than saved halfway through a flush */
				FlushSaves();
			}

			if (--ImportDepth == 0) {
				ReflectedThisRun.Empty();
				MissingPackagesThisRun.Empty();
			}
		}
	};

	/* Importing assets from Cloud */
	template <class T = UObject>
	static bool ConstructAsset(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<T>& OutObject, bool& bSuccess);
	
	/* Kept for the tools already calling it, see FTextureImport for the rest of the texture path */
	static bool Fast_Construct_TypeTexture(const TSharedPtr<FJsonObject>& JsonExport, const FString& Path, const FString& Type, TArray<uint8> Data, UTexture*& OutTexture);
};
