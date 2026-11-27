/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Audio/MetaSoundEditorGraph.h"

#if REFLECTION_METASOUND_EDITOR_GRAPH
#include "Engine/Log.h"
#include "MetasoundAssetBase.h"
#include "MetasoundAssetManager.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundUObjectRegistry.h"

namespace {
	/* The kinds of class the editor has no graph node for. A variable's own node is left out
	 * separately, by the variable naming it: the editor draws the nodes that get and set a
	 * variable, and asked to draw the variable itself it has nothing to draw it with. */
	bool IsDrawnClassType(const EMetasoundFrontendClassType Type) {
		switch (Type) {
		case EMetasoundFrontendClassType::Variable:
		case EMetasoundFrontendClassType::Graph:
		case EMetasoundFrontendClassType::Literal:
		case EMetasoundFrontendClassType::Invalid:
			return false;
		default:
			return true;
		}
	}

	int32 LongestName(const TArray<FMetasoundFrontendVertex>& Vertices) {
		int32 Longest = 0;
		for (const FMetasoundFrontendVertex& Vertex : Vertices) Longest = FMath::Max(Longest, Vertex.Name.ToString().Len());
		return Longest;
	}

	/* Stacks one column's nodes top to bottom in the order given, each as near the height it
	 * asks for as the one above it allows */
	void StackColumn(const TArray<int32>& Nodes, const TArray<double>& Wanted, const TArray<FVector2D>& Size, TArray<double>& Top) {
		constexpr double Gap = 48.0;
		double Below = -TNumericLimits<double>::Max();

		for (const int32 Node : Nodes) {
			const double Asked = Wanted[Node] == TNumericLimits<double>::Max() ? Below : Wanted[Node] - Size[Node].Y * 0.5;
			Top[Node] = Below == -TNumericLimits<double>::Max() ? (Asked == Below ? 0.0 : Asked) : FMath::Max(Asked, Below);
			Below = Top[Node] + Size[Node].Y + Gap;
		}
	}

	void EnsureGraph(UObject* MetaSound, const UObject* Asked, TSet<const UObject*>& Seen, TArray<UObject*>* Repaired) {
		using namespace Metasound::Frontend;

		if (MetaSound == nullptr || Seen.Contains(MetaSound)) return;
		Seen.Add(MetaSound);

		FMetasoundAssetBase* Asset = Metasound::IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(MetaSound);
		if (Asset == nullptr) return;

		IMetaSoundAssetManager* Manager = IMetaSoundAssetManager::Get();

		/* What it holds on to, which is what a cooked game loads alongside it. Each is made known
		 * by its class key as well, since that is how the editor looks them up when it walks
		 * them, and a MetaSound only just imported may not have been seen by the asset manager. */
		TArray<FMetasoundAssetBase*> Used = Asset->GetReferencedAssets();
		for (const FMetasoundAssetBase* Reference : Used) {
			if (Manager != nullptr && Reference != nullptr && Reference->GetOwningAsset() != nullptr) {
				Manager->AddOrUpdateAsset(*Reference->GetOwningAsset());
			}
		}

		TArray<FMetasoundAssetBase*> Keyed;
		if (Manager != nullptr && Asset->GetReferencedAssetClassKeys().Num() > 0 && Manager->TryLoadReferencedAssets(*Asset, Keyed)) {
			for (FMetasoundAssetBase* Reference : Keyed) Used.AddUnique(Reference);
		}

		/* The editor sets up what one uses before the one using it, and would set up any of them
		 * still without a graph as a new asset */
		for (FMetasoundAssetBase* Reference : Used) {
			if (Reference != nullptr) EnsureGraph(Reference->GetOwningAsset(), Asked, Seen, Repaired);
		}

		if (Asset->GetGraph() != nullptr) return;

		const int32 Placed = LayoutMetaSoundNodes(Asset->GetDocumentChecked());
		UMetaSoundEditorSubsystem::GetChecked().InitEdGraph(*MetaSound);

		UE_LOG(LogReflection, Display, TEXT("%s: set up its editor graph (%d node(s) placed)"), *MetaSound->GetPathName(), Placed);

		if (MetaSound != Asked && Repaired != nullptr) {
			auto _ = MetaSound->MarkPackageDirty();
			Repaired->Add(MetaSound);
		}
	}
}

int32 LayoutMetaSoundNodes(FMetasoundFrontendDocument& Document) {
	FMetasoundFrontendGraph& Graph = Document.RootGraph.Graph;

	TMap<FGuid, EMetasoundFrontendClassType> ClassTypes;
	for (const FMetasoundFrontendClass& Class : Document.Dependencies) ClassTypes.Add(Class.ID, Class.Metadata.GetType());

	TSet<FGuid> VariableNodes;
	for (const FMetasoundFrontendVariable& Variable : Graph.Variables) VariableNodes.Add(Variable.VariableNodeID);

	TArray<FMetasoundFrontendNode*> Drawn;
	TMap<FGuid, int32> IndexOf;
	for (FMetasoundFrontendNode& Node : Graph.Nodes) {
		const EMetasoundFrontendClassType* Type = ClassTypes.Find(Node.ClassID);
		if (Type == nullptr || !IsDrawnClassType(*Type) || VariableNodes.Contains(Node.GetID())) continue;
		IndexOf.Add(Node.GetID(), Drawn.Add(&Node));
	}

	if (!Drawn.ContainsByPredicate([](const FMetasoundFrontendNode* Node) { return Node->Style.Display.Locations.IsEmpty(); })) return 0;

	const int32 Count = Drawn.Num();
	TArray<TArray<int32>> Feeds, FedBy;
	Feeds.SetNum(Count);
	FedBy.SetNum(Count);
	for (const FMetasoundFrontendEdge& Edge : Graph.Edges) {
		const int32* From = IndexOf.Find(Edge.FromNodeID);
		const int32* To = IndexOf.Find(Edge.ToNodeID);
		if (From == nullptr || To == nullptr || *From == *To) continue;
		Feeds[*From].AddUnique(*To);
		FedBy[*To].AddUnique(*From);
	}

	/* Each node's column is the longest run of wires leading into it, worked out in an order
	 * where everything feeding a node comes before it */
	TArray<int32> Column, Waiting, Ordered;
	Column.Init(0, Count);
	Waiting.SetNumZeroed(Count);
	Ordered.Reserve(Count);
	for (int32 Node = 0; Node < Count; ++Node) {
		Waiting[Node] = FedBy[Node].Num();
		if (Waiting[Node] == 0) Ordered.Add(Node);
	}
	for (int32 Next = 0; Next < Ordered.Num(); ++Next) {
		for (const int32 Fed : Feeds[Ordered[Next]]) {
			Column[Fed] = FMath::Max(Column[Fed], Column[Ordered[Next]] + 1);
			if (--Waiting[Fed] == 0) Ordered.Add(Fed);
		}
	}

	/* A document can say anything, a loop included. What is in one still gets drawn, after the rest. */
	for (int32 Node = 0; Node < Count; ++Node) {
		if (Waiting[Node] > 0) Ordered.Add(Node);
	}

	/* What nothing feeds (an input, say) sits just before the first node it feeds, rather than
	 * all the way over at the left with a wire across the whole graph */
	for (const int32 Node : Ordered) {
		if (!FedBy[Node].IsEmpty() || Feeds[Node].IsEmpty()) continue;
		int32 First = MAX_int32;
		for (const int32 Fed : Feeds[Node]) First = FMath::Min(First, Column[Fed]);
		Column[Node] = FMath::Max(0, First - 1);
	}

	/* How big each draws: a row per pin, as wide as its longest names side by side */
	TArray<FVector2D> Size;
	Size.SetNum(Count);
	for (int32 Node = 0; Node < Count; ++Node) {
		const FMetasoundFrontendNodeInterface& Interface = Drawn[Node]->Interface;
		const int32 Rows = FMath::Max(Interface.Inputs.Num(), Interface.Outputs.Num());
		const int32 Characters = FMath::Max(Drawn[Node]->Name.ToString().Len(), LongestName(Interface.Inputs) + LongestName(Interface.Outputs));
		Size[Node] = FVector2D(FMath::Clamp(96.0 + 7.0 * Characters, 160.0, 560.0), 56.0 + 26.0 * Rows);
	}

	int32 Columns = 0;
	for (const int32 Each : Column) Columns = FMath::Max(Columns, Each + 1);

	TArray<TArray<int32>> InColumn;
	InColumn.SetNum(Columns);
	for (const int32 Node : Ordered) InColumn[Column[Node]].Add(Node);

	TArray<double> Top, Wanted;
	Top.Init(0.0, Count);
	Wanted.Init(TNumericLimits<double>::Max(), Count);
	auto Middle = [&Top, &Size](const int32 Node) { return Top[Node] + Size[Node].Y * 0.5; };

	/* Left to right, each node level with the middle of what feeds it */
	for (TArray<int32>& Nodes : InColumn) {
		for (const int32 Node : Nodes) {
			double Sum = 0.0;
			int32 Placed = 0;
			for (const int32 Feeder : FedBy[Node]) {
				if (Column[Feeder] < Column[Node]) {
					Sum += Middle(Feeder);
					Placed++;
				}
			}
			Wanted[Node] = Placed > 0 ? Sum / Placed : TNumericLimits<double>::Max();
		}
		Nodes.StableSort([&Wanted](const int32 A, const int32 B) { return Wanted[A] < Wanted[B]; });
		StackColumn(Nodes, Wanted, Size, Top);
	}

	/* Then right to left, for what nothing feeds: level with the middle of what it feeds */
	for (int32 Index = Columns - 1; Index >= 0; --Index) {
		TArray<int32>& Nodes = InColumn[Index];
		bool bMoved = false;
		for (const int32 Node : Nodes) {
			if (!FedBy[Node].IsEmpty() || Feeds[Node].IsEmpty()) {
				Wanted[Node] = Middle(Node);
				continue;
			}
			double Sum = 0.0;
			for (const int32 Fed : Feeds[Node]) Sum += Middle(Fed);
			Wanted[Node] = Sum / Feeds[Node].Num();
			bMoved = true;
		}
		if (!bMoved) continue;
		Nodes.StableSort([&Wanted](const int32 A, const int32 B) { return Wanted[A] < Wanted[B]; });
		StackColumn(Nodes, Wanted, Size, Top);
	}

	TArray<double> Left;
	Left.Init(0.0, Columns);
	for (int32 Index = 1; Index < Columns; ++Index) {
		double Widest = 0.0;
		for (const int32 Node : InColumn[Index - 1]) Widest = FMath::Max(Widest, Size[Node].X);
		Left[Index] = Left[Index - 1] + Widest + 120.0;
	}

	int32 Placed = 0;
	for (int32 Node = 0; Node < Count; ++Node) {
		TMap<FGuid, FVector2D>& Locations = Drawn[Node]->Style.Display.Locations;
		if (!Locations.IsEmpty()) continue;

		/* Named after the node, so importing the same MetaSound again draws it the same way */
		Locations.Add(FGuid::NewDeterministicGuid(Drawn[Node]->GetID().ToString() + TEXT("/Location")), FVector2D(Left[Column[Node]], Top[Node]));
		Placed++;
	}

	return Placed;
}

void EnsureMetaSoundEditorGraph(UObject* MetaSound, TArray<UObject*>* Repaired) {
	TSet<const UObject*> Seen;
	EnsureGraph(MetaSound, MetaSound, Seen, Repaired);
}
#endif
