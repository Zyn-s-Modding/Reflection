/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

#if REFLECTION_METASOUND_EDITOR_GRAPH
struct FMetasoundFrontendDocument;

/* Gives every node of a MetaSound's root graph that the MetaSound editor draws somewhere to be
 * drawn, where it has nowhere yet. Cooking strips where nodes sat, so an imported document starts
 * with none. Nodes are put in columns by how far along the wires they are, each one to the right
 * of everything that feeds it. Says how many nodes it placed. */
int32 LayoutMetaSoundNodes(FMetasoundFrontendDocument& Document);

/* Sets a MetaSound up with the editor graph that one made in the editor has, and before it every
 * MetaSound it uses, the way the editor will walk them when it is opened.
 *
 * Without a graph, opening a MetaSound in its editor takes it for one just made outside the asset
 * factory: the editor starts the document over (a new class name, so nothing that used it finds
 * it again, and fresh interface inputs that cut the old ones' wires), then tries to draw every node
 * including a variable's own, which it cannot. Cooking also only brings a MetaSound's nodes up to
 * this engine's when it has a graph, and a cooked game does not do that itself.
 *
 * MetaSounds that were already in the project without a graph, and only reached through this one,
 * are added to Repaired so they can be saved too. */
void EnsureMetaSoundEditorGraph(UObject* MetaSound, TArray<UObject*>* Repaired = nullptr);
#endif
