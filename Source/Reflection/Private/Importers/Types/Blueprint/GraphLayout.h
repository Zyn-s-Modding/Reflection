/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

/* How much room a node takes up when it is drawn, and how far down it each of its pins sits.
 *
 * A wire runs from the middle of one pin to the middle of another, so two nodes sit level when their
 * pins do, not when their tops do. The size is what keeps nodes from landing on top of each other: a
 * call with a dozen inputs is several times the height of a variable read, and laid out as if they
 * were the same, the tall one covers whatever was put under it. */
struct FGraphNodeFootprint {
	FVector2D Size = FVector2D(200.0, 80.0);

	/* How far the node draws above its own top, which is where a comment bubble hangs */
	double TopOverhang = 0.0;

	/* The middle of each pin, measured down from the top of the node */
	TMap<const UEdGraphPin*, double> PinCenterY;

	/* Whether this came from the node's own widget, rather than worked out from its pins */
	bool bMeasured = false;

	/* The middle of a pin, or of the node where the pin was never drawn */
	double PinY(const UEdGraphPin* Pin) const;
};

/* Measures a node the way the graph panel does when there is a panel to ask, which is any time the
 * editor is open. Without one, in a commandlet, it is worked out from the title and the pins. */
FGraphNodeFootprint MeasureGraphNode(UEdGraphNode* Node);

/* Lays out a graph so it reads the way somebody would have drawn it.
 *
 * Every way into the graph is followed through its execution pins, left to right, each node placed
 * just past the one before it with its pins level. A second way out of a node (a branch's False, a
 * sequence's next step) goes underneath everything the first one led to. Whatever feeds a node a
 * value sits to its left, in the order the pins take them, and the node is pushed right far enough
 * for all of it to fit between it and the node before it. Each run then goes below the one before.
 *
 * Kept nodes are never moved. They are for graphs that already held something before this was asked,
 * an animation graph with its poses laid out, say: what is new is laid out around them, and whatever
 * is new and feeds one of them is put beside it. */
void LayoutGraph(UEdGraph* Graph, const TArray<UEdGraphNode*>& Ways, const TSet<UEdGraphNode*>& Kept);

/* Lays out again every graph of a blueprint that LayoutGraph laid out, the same way it did the first
 * time. For once the blueprint is finished: compiling it can lay nodes out afresh at a different size
 * (a cast told its type renames its pins, a node mended to compile grows a pin), and a layout made
 * against the old sizes has them sitting on top of each other. */
void RelayoutBlueprintGraphs(UBlueprint* Blueprint);
