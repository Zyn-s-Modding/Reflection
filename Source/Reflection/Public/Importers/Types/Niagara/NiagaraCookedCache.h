/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

struct FUObjectExportContainer;

/* What the cook moved aside, put back where the asset reads it from.
 *
 * Niagara keeps a handful of things twice over. A script holds the values its module inputs were
 * left at, a curve data interface holds its curve, and cooking moves each of them out of the
 * place the editor reads and into a second one beside it, named for the first with
 * "CookedEditorCache" on the end. The engine puts them back as it reads a package it knows was
 * cooked, and only then.
 *
 * What comes out of an import is a package the engine has just been handed and does not think is
 * cooked, so nothing puts them back: a module reads its inputs at whatever its own graph says
 * rather than what the emitter set them to, and a curve comes up flat.
 *
 * Which fields those are is not written down here. The pairing is the name, so that is what is
 * looked for, and anything else Niagara comes to keep this way is already covered. */
class REFLECTION_API FNiagaraCookedCache {
public:
	/* Answers with the number of fields moved back */
	static int32 Restore(FUObjectExportContainer* Container);

	/* Renderer attribute bindings and sprite cutout, in the newer layout, onto this engine's.
	 *
	 * A renderer reads a particle attribute through a binding, and this engine's binding keeps the
	 * attribute it was pointed at as RootVariable: everything else in it (ParamMapVariable,
	 * DataSetName) is worked out from that again whenever the emitter is set up. Newer engines keep
	 * the same thing as RootName, a name with no type, and this engine has no field of that name, so
	 * the import left RootVariable at its default. The binding still read right until the emitter
	 * was set up, then fell back to the default attribute: a flare sized by Particles.LightSize was
	 * drawn at Particles.SpriteSize, a smoke flipbook lost its SubUV frame index, a glow its
	 * orientation and dynamic material values. RootName is put into RootVariable, typed as the
	 * binding's ParamMapVariable.
	 *
	 * And CutoutMode (Material / Texture / None), which this engine says as bUseMaterialCutoutTexture
	 * beside CutoutTexture.
	 *
	 * Answers with the number of bindings restored. */
	static int32 RestoreRendererBindings(FUObjectExportContainer* Container);
};
