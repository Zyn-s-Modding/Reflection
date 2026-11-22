/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "BlacklistSettings.generated.h"

/* One type on the blacklist.
 *
 * A struct of its own rather than a bare string so the settings page can draw it as a picker: every
 * kind of asset the editor knows of, every type Reflection imports, and every data asset class, with
 * room to type one none of those name. */
USTRUCT()
struct FRBlacklistType {
	GENERATED_BODY()
public:
	FRBlacklistType() = default;
	FRBlacklistType(const FString& InType) : Type(InType) {}

	/* The type, named as the game names it (not case-sensitive). Example: "Texture2D" */
	UPROPERTY(EditAnywhere, Config, Category = Blacklist)
	FString Type;
};

/* One folder on the blacklist.
 *
 * Drawn as a path box with a folder picker beside it. The picker only knows the folders this project
 * already has, and the box takes any path at all, so a game folder nothing has been reflected into
 * yet can still be written in. */
USTRUCT()
struct FRBlacklistFolder {
	GENERATED_BODY()
public:
	FRBlacklistFolder() = default;
	FRBlacklistFolder(const FString& InPath) : Path(InPath) {}

	/* The folder, down through every folder beneath it (not case-sensitive).
	 * Examples: "/Game/Athena/Sounds/", "FortniteGame/Content/Environments/" */
	UPROPERTY(EditAnywhere, Config, Category = Blacklist)
	FString Path;
};

/* What is never reflected, however it is reached.
 *
 * Asked of everything: an asset reflected by path, every asset in a folder run, a local json, and
 * every reference an import reaches for along the way. Something turned away here is not fetched
 * and not built. What the project already has is still used, so a blacklisted reference to an asset
 * that is already in the project resolves the same as it always did, and one that is not comes out
 * empty. */
USTRUCT()
struct FRBlacklistSettings {
	GENERATED_BODY()
public:
	/* Master toggle. Off, nothing below is asked and everything reflects as it would without it. */
	UPROPERTY(EditAnywhere, Config, Category = Blacklist)
	bool Enabled = true;

	/* Types that are never reflected. Pick one from the list, or type a name it doesn't have. */
	UPROPERTY(EditAnywhere, DisplayName = "Asset Types", Config, Category = Blacklist, meta = (TitleProperty = "Type"))
	TArray<FRBlacklistType> Types;

	/* A type also covers every type made from it: "Texture" takes Texture2D and TextureCube with it,
	 * and "Blueprint" takes BlueprintGeneratedClass. Only reaches types this engine has a class for. */
	UPROPERTY(EditAnywhere, DisplayName = "Include Child Types", Config, Category = Blacklist)
	bool IncludeChildTypes = true;

	/* Single assets that are never reflected, by their full path with the object on the end
	 * (not case-sensitive). A path without the object covers everything in that package.
	 *
	 * Either side of a redirect can be given: where the game keeps it, or where it would land here.
	 * A copied reference works as it is.
	 *
	 * Examples: "/Game/Athena/Items/Weapons/WID_Pistol.WID_Pistol",
	 *           "Texture2D'/Game/Textures/T_Noise.T_Noise'" */
	UPROPERTY(EditAnywhere, DisplayName = "Assets", Config, Category = Blacklist)
	TArray<FString> Assets;

	/* Folders whose contents are never reflected. Pick one this project has, or type any path,
	 * including a game folder that hasn't been reflected into yet. Either side of a redirect can be
	 * given, and the game's own spelling works as well. */
	UPROPERTY(EditAnywhere, DisplayName = "Folders", Config, Category = Blacklist, meta = (TitleProperty = "Path"))
	TArray<FRBlacklistFolder> Folders;
};
