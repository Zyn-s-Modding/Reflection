/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "BlacklistSettings.generated.h"

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

	/* Types that are never reflected, named as the game names them (not case-sensitive).
	 *
	 * Examples: "Texture2D", "SoundWave", "AnimSequence", "BlueprintGeneratedClass" */
	UPROPERTY(EditAnywhere, DisplayName = "Asset Types", Config, Category = Blacklist)
	TArray<FString> Types;

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

	/* Folders whose contents are never reflected, down through every folder beneath them
	 * (not case-sensitive).
	 *
	 * Either side of a redirect can be given, and the game's own spelling works as well.
	 *
	 * Examples: "/Game/Athena/Sounds/", "FortniteGame/Content/Environments/" */
	UPROPERTY(EditAnywhere, DisplayName = "Folders", Config, Category = Blacklist)
	TArray<FString> Folders;
};
