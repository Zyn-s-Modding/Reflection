/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/GraphLayout.h"

#include "AnimGraphNode_Base.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "K2Node.h"
#include "K2Node_Knot.h"
#include "K2Node_VariableGet.h"
#include "Layout/ArrangedChildren.h"
#include "Modules/ModuleManager.h"
#include "NodeFactory.h"
#include "SGraphNode.h"
#include "SGraphPin.h"

double FGraphNodeFootprint::PinY(const UEdGraphPin* Pin) const {
	if (const double* Y = PinCenterY.Find(Pin)) return *Y;

	return Size.Y * 0.5;
}

namespace {
	bool IsExec(const UEdGraphPin* Pin) {
		return Pin != nullptr && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
	}

	/* Whether a pin is drawn at all. An advanced pin is folded away until somebody opens the node up. */
	bool IsShown(const UEdGraphNode* Node, const UEdGraphPin* Pin) {
		if (Pin == nullptr || Pin->bHidden) return false;

		return !(Pin->bAdvancedView && Node->AdvancedPinDisplay == ENodeAdvancedPins::Hidden);
	}

	/* What a node's widget is made from, where there is no widget to ask */
	namespace Drawn {
		/* One letter of a pin's name, and of a title, which is drawn bolder */
		constexpr double Letter = 7.0;
		constexpr double TitleLetter = 7.5;

		/* An operator drawn in place of a title, which is drawn large */
		constexpr double CompactLetter = 14.0;

		constexpr double PinRow = 26.0;

		/* A pin's icon and the room either side of it */
		constexpr double PinSide = 34.0;

		/* The title bar with one line in it, and each line under that: a call on a target says
		 * which kind of target under its name */
		constexpr double Title = 34.0;
		constexpr double TitleLine = 15.0;

		constexpr double Bottom = 10.0;

		/* The arrow that unfolds a node's advanced pins */
		constexpr double Advanced = 22.0;

		/* A comment bubble, which hangs above the node */
		constexpr double Bubble = 30.0;
	}

	/* How wide the box is that a pin's value is typed into, where it has one */
	double ValueWidth(const UEdGraphPin* Pin) {
		if (Pin == nullptr || Pin->Direction != EGPD_Input || Pin->LinkedTo.Num() > 0) return 0.0;
		if (Pin->bDefaultValueIsIgnored || Pin->PinType.IsContainer() || Pin->PinType.bIsReference) return 0.0;
		if (Pin->PinName == UEdGraphSchema_K2::PN_Self) return 0.0;

		const FName Category = Pin->PinType.PinCategory;

		if (Category == UEdGraphSchema_K2::PC_Boolean) return 22.0;
		if (Category == UEdGraphSchema_K2::PC_Int || Category == UEdGraphSchema_K2::PC_Int64 || Category == UEdGraphSchema_K2::PC_Real) return 60.0;

		if (Category == UEdGraphSchema_K2::PC_Byte || Category == UEdGraphSchema_K2::PC_Enum) {
			return Pin->PinType.PinSubCategoryObject.IsValid() ? 120.0 : 60.0;
		}

		if (Category == UEdGraphSchema_K2::PC_Name || Category == UEdGraphSchema_K2::PC_String) return 72.0;
		if (Category == UEdGraphSchema_K2::PC_Text) return 92.0;

		if (Category == UEdGraphSchema_K2::PC_Struct) {
			const UScriptStruct* Struct = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get());

			if (Struct == nullptr) return 0.0;

			/* A box for each of its parts, which the editor draws for the few structs it knows */
			static const FName Three[] = {TEXT("Vector"), TEXT("Vector3f"), TEXT("Vector3d"), TEXT("Rotator")};
			static const FName Two[] = {TEXT("Vector2D"), TEXT("Vector2f")};
			static const FName Four[] = {TEXT("Vector4"), TEXT("Vector4f"), TEXT("Quat"), TEXT("Quat4f")};
			static const FName Colors[] = {TEXT("LinearColor"), TEXT("Color")};

			const FName Named = Struct->GetFName();

			for (const FName& One : Three) if (Named == One) return 174.0;
			for (const FName& One : Two) if (Named == One) return 116.0;
			for (const FName& One : Four) if (Named == One) return 232.0;
			for (const FName& One : Colors) if (Named == One) return 40.0;

			return 0.0;
		}

		/* An asset picker. An actor is never an asset, and nothing is offered to pick one with. */
		if (Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_SoftObject
			|| Category == UEdGraphSchema_K2::PC_Class || Category == UEdGraphSchema_K2::PC_SoftClass) {
			const UClass* Class = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get());

			if (Class != nullptr && Class->IsChildOf(AActor::StaticClass()) && Category == UEdGraphSchema_K2::PC_Object) return 0.0;

			return 180.0;
		}

		return 0.0;
	}

	double LabelWidth(const UEdGraphNode* Node, const UEdGraphPin* Pin) {
		/* The plain way in and out are drawn without a name */
		if (IsExec(Pin) && (Pin->PinName == UEdGraphSchema_K2::PN_Execute || Pin->PinName == UEdGraphSchema_K2::PN_Then)) return 0.0;

		return Node->GetPinDisplayName(Pin).ToString().Len() * Drawn::Letter;
	}

	/* Worked out from the title and the pins, the way the default widget lays them out: a title bar,
	 * then the inputs down the left and the outputs down the right, a row each. */
	FGraphNodeFootprint Estimate(UEdGraphNode* Node) {
		FGraphNodeFootprint Print;

		if (Node->IsA<UK2Node_Knot>()) {
			Print.Size = FVector2D(42.0, 24.0);

			for (const UEdGraphPin* Pin : Node->Pins) {
				if (Pin != nullptr) Print.PinCenterY.Add(Pin, 12.0);
			}

			return Print;
		}

		const UK2Node* Written = Cast<UK2Node>(Node);

		const bool bCompact = Written != nullptr && Written->ShouldDrawCompact();

		/* A plain variable read is drawn as its pin and nothing else */
		const bool bBare = !bCompact && Node->IsA<UK2Node_VariableGet>() && Written->IsNodePure();

		double Header = 0.0;
		double TitleWidth = 0.0;

		const FString Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();

		if (bCompact) {
			Header = 8.0;
			TitleWidth = Title.Len() * Drawn::CompactLetter + 24.0;
		} else if (bBare) {
			Header = 4.0;
		} else {
			TArray<FString> Lines;
			Title.ParseIntoArrayLines(Lines, false);

			int32 Longest = 0;

			for (const FString& Line : Lines) Longest = FMath::Max(Longest, Line.Len());

			Header = Drawn::Title + FMath::Max(0, Lines.Num() - 1) * Drawn::TitleLine;
			TitleWidth = Longest * Drawn::TitleLetter + 64.0;
		}

		int32 Inputs = 0;
		int32 Outputs = 0;

		double InWidest = 0.0;
		double OutWidest = 0.0;

		for (const UEdGraphPin* Pin : Node->Pins) {
			if (!IsShown(Node, Pin)) continue;

			const double Label = bCompact ? 0.0 : LabelWidth(Node, Pin);

			if (Pin->Direction == EGPD_Input) {
				Print.PinCenterY.Add(Pin, Header + (Inputs + 0.5) * Drawn::PinRow);
				Inputs++;

				InWidest = FMath::Max(InWidest, Drawn::PinSide + Label + ValueWidth(Pin));
			} else {
				Print.PinCenterY.Add(Pin, Header + (Outputs + 0.5) * Drawn::PinRow);
				Outputs++;

				OutWidest = FMath::Max(OutWidest, Drawn::PinSide + Label);
			}
		}

		const bool bFolded = Node->AdvancedPinDisplay != ENodeAdvancedPins::NoPins;

		const double Height = Header + FMath::Max(Inputs, Outputs) * Drawn::PinRow + (bFolded ? Drawn::Advanced : 0.0) + (bBare ? 4.0 : Drawn::Bottom);
		const double Width = bCompact ? InWidest + TitleWidth + OutWidest : FMath::Max(TitleWidth, InWidest + OutWidest + 24.0);

		Print.Size = FVector2D(FMath::Max(Width, bBare ? 60.0 : 100.0), FMath::Max(Height, bCompact ? 40.0 : 32.0));

		if (Node->bCommentBubbleVisible && !Node->NodeComment.IsEmpty()) Print.TopOverhang = Drawn::Bubble;

		return Print;
	}

	/* Walks the widget tree the way Slate would, noting where each pin's widget lands */
	void FindPinCenters(const TSharedRef<SWidget>& Widget, const FGeometry& Geometry, const TMap<const SWidget*, const UEdGraphPin*>& PinWidgets, TMap<const UEdGraphPin*, double>& OutCenters) {
		if (OutCenters.Num() == PinWidgets.Num()) return;

		if (const UEdGraphPin* const* Pin = PinWidgets.Find(&Widget.Get())) {
			/* Arranged at a scale of one, so local and absolute are the same space */
			OutCenters.Add(*Pin, Geometry.GetAbsolutePosition().Y + Geometry.GetLocalSize().Y * 0.5);

			return;
		}

		/* Visible only: a folded pin still has a widget, but takes no room */
		FArrangedChildren Children(EVisibility::Visible);
		Widget->ArrangeChildren(Geometry, Children);

		for (int32 Index = 0; Index < Children.Num(); ++Index) {
			FindPinCenters(Children[Index].Widget, Children[Index].Geometry, PinWidgets, OutCenters);
		}
	}
}

FGraphNodeFootprint MeasureGraphNode(UEdGraphNode* Node) {
	if (Node == nullptr) return FGraphNodeFootprint();

	/* The widget a panel would make for the node, measured and dropped, which needs no window open.
	 * The panel itself does no more than this to size a node: build it, pre-pass it, read what it
	 * asks for. */
	if (FSlateApplication::IsInitialized()) {
		/* An animation node only gets its real widget through a factory the animation blueprint
		 * editor registers when it loads, and without it every one measures as a plain node */
		if (Node->IsA<UAnimGraphNode_Base>()) {
			static bool bAsked = false;

			if (!bAsked) {
				bAsked = true;

				FModuleManager::Get().LoadModule(TEXT("AnimationBlueprintEditor"));
			}
		}

		if (const TSharedPtr<SGraphNode> Widget = FNodeFactory::CreateNodeWidget(Node)) {
			Widget->SlatePrepass(1.0f);

			const FVector2D Desired = Widget->GetDesiredSize();

			if (Desired.X > 1.0 && Desired.Y > 1.0) {
				FGraphNodeFootprint Print;
				Print.Size = Desired;
				Print.bMeasured = true;

				const FGeometry Root = FGeometry::MakeRoot(Desired, FSlateLayoutTransform());

				/* Whatever the node draws above itself is arranged above its top, at a negative offset */
				FArrangedChildren Zones(EVisibility::Visible);
				Widget->ArrangeChildren(Root, Zones);

				for (int32 Index = 0; Index < Zones.Num(); ++Index) {
					Print.TopOverhang = FMath::Max(Print.TopOverhang, -static_cast<double>(Zones[Index].Geometry.GetAbsolutePosition().Y));
				}

				TMap<const SWidget*, const UEdGraphPin*> PinWidgets;

				for (UEdGraphPin* Pin : Node->Pins) {
					if (Pin == nullptr) continue;

					/* Null for a pin the node chose not to draw */
					if (const TSharedPtr<SGraphPin> PinWidget = Widget->FindWidgetForPin(Pin)) {
						PinWidgets.Add(PinWidget.Get(), Pin);
					}
				}

				if (PinWidgets.Num() > 0) {
					FindPinCenters(Widget.ToSharedRef(), Root, PinWidgets, Print.PinCenterY);
				}

				return Print;
			}
		}
	}

	return Estimate(Node);
}

namespace {
	namespace Spacing {
		/* Between a node and the next one along the run */
		constexpr double Along = 80.0;

		/* Between a node and whatever feeds it a value */
		constexpr double Feeding = 56.0;

		/* Between two nodes stacked one over the other */
		constexpr double Stacked = 20.0;

		/* Between one way out of a node and the next */
		constexpr double Branches = 48.0;

		/* Between one run and the next */
		constexpr double Runs = 160.0;

		/* How close two nodes can sit side by side and still not be in each other's way */
		constexpr double Beside = 16.0;

		/* How wide a row of left-over nodes runs before it starts another */
		constexpr double Shelf = 2400.0;
	}

	struct FArea {
		double L = 0.0;
		double T = 0.0;
		double R = 0.0;
		double B = 0.0;

		/* Whether two areas are in each other's way, with Gap left clear above and below */
		bool Crowds(const FArea& Other, const double Gap) const {
			return L < Other.R + Spacing::Beside && Other.L < R + Spacing::Beside && T < Other.B + Gap && Other.T < B + Gap;
		}

		void Grow(const FArea& Other) {
			L = FMath::Min(L, Other.L);
			T = FMath::Min(T, Other.T);
			R = FMath::Max(R, Other.R);
			B = FMath::Max(B, Other.B);
		}
	};

	class FLayout {
	public:
		FLayout(UEdGraph* InGraph, const TSet<UEdGraphNode*>& InKept)
			: Graph(InGraph)
		{
			for (const UEdGraphNode* Node : InKept) Kept.Add(Node);
		}

		void Arrange(const TArray<UEdGraphNode*>& Ways);

	private:
		UEdGraph* Graph = nullptr;
		TSet<const UEdGraphNode*> Kept;

		/* Measured once each, before anything is placed, so a reference into this stays good */
		TMap<const UEdGraphNode*, FGraphNodeFootprint> Footprints;

		/* Where each node's top left corner goes */
		TMap<const UEdGraphNode*, FVector2D> At;

		/* Given somewhere to be, by whatever reached it first */
		TSet<const UEdGraphNode*> Claimed;

		/* Everything down so far, which whatever comes next keeps clear of */
		TArray<UEdGraphNode*> Everything;

		/* A row along the bottom for whatever no run reaches */
		bool bShelved = false;
		double ShelfLeft = 0.0;
		double ShelfWidth = Spacing::Shelf;
		double ShelfX = 0.0;
		double ShelfTop = 0.0;
		double ShelfBottom = 0.0;

		static bool IsComment(const UEdGraphNode* Node) { return Node != nullptr && Node->IsA<UEdGraphNode_Comment>(); }

		bool IsMovable(const UEdGraphNode* Node) const { return Node != nullptr && !IsComment(Node) && !Kept.Contains(Node); }

		static bool Runs(const UEdGraphNode* Node) {
			for (const UEdGraphPin* Pin : Node->Pins) {
				if (IsExec(Pin)) return true;
			}

			return false;
		}

		static UEdGraphPin* FirstExec(UEdGraphNode* Node, const EEdGraphPinDirection Direction) {
			for (UEdGraphPin* Pin : Node->Pins) {
				if (IsExec(Pin) && Pin->Direction == Direction) return Pin;
			}

			return nullptr;
		}

		const FGraphNodeFootprint& FootprintOf(const UEdGraphNode* Node) const { return Footprints.FindChecked(Node); }

		FArea AreaOf(const UEdGraphNode* Node) const {
			const FGraphNodeFootprint& Print = FootprintOf(Node);
			const FVector2D& Corner = At.FindChecked(Node);

			return FArea{Corner.X, Corner.Y - Print.TopOverhang, Corner.X + Print.Size.X, Corner.Y + Print.Size.Y};
		}

		bool BoundsOf(const TArray<UEdGraphNode*>& Group, FArea& OutArea) const {
			bool bAny = false;

			for (const UEdGraphNode* Node : Group) {
				if (!At.Contains(Node)) continue;

				const FArea Area = AreaOf(Node);

				if (bAny) OutArea.Grow(Area);
				else OutArea = Area;

				bAny = true;
			}

			return bAny;
		}

		void Shift(const TArray<UEdGraphNode*>& Group, const FVector2D& By) {
			for (const UEdGraphNode* Node : Group) {
				if (FVector2D* Corner = At.Find(Node)) *Corner += By;
			}
		}

		/* How many steps back from the node being fed each of its feeders sits, counted the long way */
		TMap<const UEdGraphNode*, int32> Depth;

		bool CanFeed(const UEdGraphNode* Node) const {
			return IsMovable(Node) && !Claimed.Contains(Node) && Footprints.Contains(Node) && !Runs(Node);
		}

		void Feed(UEdGraphNode* Root, TArray<UEdGraphNode*>& Group);
		void FeedFrom(UEdGraphNode* Consumer, TArray<UEdGraphNode*>& Group);
		TArray<UEdGraphNode*> Follow(UEdGraphNode* Node, const UEdGraphPin* Entered, double Left, double Lane);
		void Settle(const TArray<UEdGraphNode*>& Moving, const TArray<UEdGraphNode*>& Still, double Gap);
		void Put(const TArray<UEdGraphNode*>& Run);
		void Shelve(const TArray<UEdGraphNode*>& Group);
	};

	/* Pushes a group straight down, all of it together, until none of it is in the way of anything
	 * still. Moved together, every wire inside the group stays exactly as straight as it was. */
	void FLayout::Settle(const TArray<UEdGraphNode*>& Moving, const TArray<UEdGraphNode*>& Still, const double Gap) {
		if (Moving.Num() == 0 || Still.Num() == 0) return;

		TArray<FArea> Fixed;
		Fixed.Reserve(Still.Num());

		FArea Around;

		for (const UEdGraphNode* Node : Still) {
			if (!At.Contains(Node)) continue;

			const FArea Area = AreaOf(Node);

			if (Fixed.Num() == 0) Around = Area;
			else Around.Grow(Area);

			Fixed.Add(Area);
		}

		if (Fixed.Num() == 0) return;

		TArray<FArea> Going;
		Going.Reserve(Moving.Num());

		for (const UEdGraphNode* Node : Moving) {
			if (At.Contains(Node)) Going.Add(AreaOf(Node));
		}

		double Moved = 0.0;

		/* Each pass clears everything it was in the way of, and can only land it in the way of
		 * something further down, so it gets there in as many passes as there are things below */
		for (int32 Pass = 0; Pass < 512; ++Pass) {
			double Need = 0.0;

			for (const FArea& Was : Going) {
				const FArea Area{Was.L, Was.T + Moved, Was.R, Was.B + Moved};

				if (!Area.Crowds(Around, Gap)) continue;

				for (const FArea& Other : Fixed) {
					if (Area.Crowds(Other, Gap)) Need = FMath::Max(Need, Other.B + Gap - Area.T);
				}
			}

			if (Need <= 0.0) break;

			Moved += Need;
		}

		if (Moved > 0.0) Shift(Moving, FVector2D(0.0, Moved));
	}

	/* Whatever feeds a node a value, to its left, in the order its pins take them.
	 *
	 * A node that feeds more than one of them goes where the longest way back from the root puts it,
	 * so it sits left of everything it feeds. Put beside the first one to ask for it instead, a read
	 * handed both to a call and to something else feeding that call lands between the two, and the
	 * wire into the second runs backwards. */
	void FLayout::Feed(UEdGraphNode* Root, TArray<UEdGraphNode*>& Group) {
		Depth.Reset();

		/* Everything new that feeds the root, however far back */
		TArray<UEdGraphNode*> Found;
		TSet<const UEdGraphNode*> Within;

		Found.Add(Root);
		Within.Add(Root);

		for (int32 Index = 0; Index < Found.Num(); ++Index) {
			for (UEdGraphPin* Pin : Found[Index]->Pins) {
				if (Pin == nullptr || Pin->Direction != EGPD_Input || IsExec(Pin)) continue;

				for (UEdGraphPin* Linked : Pin->LinkedTo) {
					UEdGraphNode* Feeder = Linked != nullptr ? Linked->GetOwningNode() : nullptr;

					if (!CanFeed(Feeder) || Within.Contains(Feeder)) continue;

					Within.Add(Feeder);
					Found.Add(Feeder);
				}
			}
		}

		if (Found.Num() == 1) return;

		/* The longest way back, worked out from the root outwards: a feeder is only settled once
		 * everything it feeds has been */
		TMap<const UEdGraphNode*, int32> Waiting;

		const auto EachFeeder = [this, &Within, Root](UEdGraphNode* Consumer, const TFunctionRef<void(UEdGraphNode*)> Visit) {
			for (UEdGraphPin* Pin : Consumer->Pins) {
				if (Pin == nullptr || Pin->Direction != EGPD_Input || IsExec(Pin)) continue;

				for (UEdGraphPin* Linked : Pin->LinkedTo) {
					UEdGraphNode* Feeder = Linked != nullptr ? Linked->GetOwningNode() : nullptr;

					if (Feeder != nullptr && Feeder != Root && Within.Contains(Feeder)) Visit(Feeder);
				}
			}
		};

		for (UEdGraphNode* Node : Found) {
			EachFeeder(Node, [&Waiting](UEdGraphNode* Feeder) { Waiting.FindOrAdd(Feeder)++; });
		}

		TArray<UEdGraphNode*> Ready;
		Ready.Add(Root);
		Depth.Add(Root, 0);

		int32 Deepest = 0;

		for (int32 Index = 0; Index < Ready.Num(); ++Index) {
			UEdGraphNode* Consumer = Ready[Index];
			const int32 Here = Depth.FindChecked(Consumer);

			EachFeeder(Consumer, [&](UEdGraphNode* Feeder) {
				int32& Steps = Depth.FindOrAdd(Feeder);
				Steps = FMath::Max(Steps, Here + 1);
				Deepest = FMath::Max(Deepest, Steps);

				if (--Waiting.FindChecked(Feeder) == 0) Ready.Add(Feeder);
			});
		}

		/* Only a loop leaves anything unsettled, and nothing written means one; past everything else */
		for (UEdGraphNode* Node : Found) {
			if (!Ready.Contains(Node)) Depth.Add(Node, Deepest + 1);
		}

		FeedFrom(Root, Group);
	}

	void FLayout::FeedFrom(UEdGraphNode* Consumer, TArray<UEdGraphNode*>& Group) {
		const FVector2D Where = At.FindChecked(Consumer);
		const FGraphNodeFootprint& Taking = FootprintOf(Consumer);
		const int32 Here = Depth.FindRef(Consumer);

		TArray<UEdGraphNode*> Fed;

		for (UEdGraphPin* Pin : Consumer->Pins) {
			if (Pin == nullptr || Pin->Direction != EGPD_Input || IsExec(Pin)) continue;

			for (UEdGraphPin* Linked : Pin->LinkedTo) {
				UEdGraphNode* Feeder = Linked != nullptr ? Linked->GetOwningNode() : nullptr;

				/* Something with a way in is put where the run reaches it, not where it is read */
				if (!CanFeed(Feeder)) continue;

				/* Wanted further back by something else it feeds, and put there instead */
				if (const int32* Steps = Depth.Find(Feeder); Steps != nullptr && *Steps != Here + 1) continue;

				Claimed.Add(Feeder);

				const FGraphNodeFootprint& Print = FootprintOf(Feeder);

				At.Add(Feeder, FVector2D(Where.X - Spacing::Feeding - Print.Size.X, Where.Y + Taking.PinY(Pin) - Print.PinY(Linked)));

				TArray<UEdGraphNode*> Branch;
				Branch.Add(Feeder);

				FeedFrom(Feeder, Branch);

				Settle(Branch, Fed, Spacing::Stacked);

				Fed.Append(Branch);
			}
		}

		Group.Append(Fed);
	}

	/* A node and everything its run goes on to, with its way in level with the pin that led to it.
	 *
	 * The node and what feeds it are one block, and the block starts at Left, so whatever feeds the
	 * node fits between it and the node before. The first way out carries on along the same line;
	 * every other one goes below whatever the ways before it led to. */
	TArray<UEdGraphNode*> FLayout::Follow(UEdGraphNode* Node, const UEdGraphPin* Entered, const double Left, const double Lane) {
		TArray<UEdGraphNode*> Group;

		Claimed.Add(Node);

		const FGraphNodeFootprint& Print = FootprintOf(Node);

		/* Level by the pin the run came in through, or for the start of a run, the one it leaves by */
		const UEdGraphPin* Level = Entered != nullptr ? Entered : FirstExec(Node, EGPD_Input);

		if (Level == nullptr) Level = FirstExec(Node, EGPD_Output);

		At.Add(Node, FVector2D(0.0, Lane - (Level != nullptr ? Print.PinY(Level) : 0.0)));

		Group.Add(Node);

		Feed(Node, Group);

		FArea Block;

		if (BoundsOf(Group, Block)) Shift(Group, FVector2D(Left - Block.L, 0.0));

		const FVector2D Here = At.FindChecked(Node);
		const double Next = Here.X + Print.Size.X + Spacing::Along;

		bool bFirst = true;

		for (UEdGraphPin* Out : Node->Pins) {
			if (Out == nullptr || Out->Direction != EGPD_Output || !IsExec(Out)) continue;

			for (UEdGraphPin* Linked : Out->LinkedTo) {
				UEdGraphNode* To = Linked != nullptr ? Linked->GetOwningNode() : nullptr;

				/* Already down means the run comes back to it, and the wire goes there */
				if (!IsMovable(To) || Claimed.Contains(To) || !Footprints.Contains(To)) continue;

				TArray<UEdGraphNode*> Led = Follow(To, Linked, Next, Here.Y + Print.PinY(Out));

				/* The first carries straight on past everything here, so it is in nobody's way */
				if (!bFirst) Settle(Led, Group, Spacing::Branches);

				bFirst = false;

				Group.Append(MoveTemp(Led));
			}
		}

		return Group;
	}

	/* A whole run, in the first room on the left that fits it. Where it runs into something already
	 * down it goes beside that instead, ending just short of it, so the wire into it is short. */
	void FLayout::Put(const TArray<UEdGraphNode*>& Run) {
		FArea Area;

		if (!BoundsOf(Run, Area)) return;

		const UEdGraphNode* Joins = nullptr;

		for (const UEdGraphNode* Node : Run) {
			for (const UEdGraphPin* Pin : Node->Pins) {
				if (Pin == nullptr || Pin->Direction != EGPD_Output || !IsExec(Pin)) continue;

				for (const UEdGraphPin* Linked : Pin->LinkedTo) {
					const UEdGraphNode* To = Linked != nullptr ? Linked->GetOwningNode() : nullptr;

					if (To != nullptr && At.Contains(To) && !Run.Contains(To) && Everything.Contains(To)) {
						Joins = To;

						break;
					}
				}

				if (Joins != nullptr) break;
			}

			if (Joins != nullptr) break;
		}

		FArea All;
		const bool bAnything = BoundsOf(Everything, All);

		if (Joins != nullptr) {
			const FArea Meets = AreaOf(Joins);

			Shift(Run, FVector2D(Meets.L - Spacing::Along - Area.R, Meets.B + Spacing::Branches - Area.T));

			Settle(Run, Everything, Spacing::Branches);
		} else if (bAnything) {
			/* Into the first space below the top that fits it, rather than under everything. A run
			 * that goes on a long way to the right and drops as it goes leaves the room under its
			 * start empty, and a short run stacked under the whole of it is drawn far from the rest
			 * across that empty room. */
			Shift(Run, FVector2D(All.L - Area.L, All.T - Area.T));

			Settle(Run, Everything, Spacing::Runs);
		} else {
			Shift(Run, FVector2D(-Area.L, -Area.T));
		}

		Everything.Append(Run);
	}

	/* Something no run reaches, along a row underneath everything else */
	void FLayout::Shelve(const TArray<UEdGraphNode*>& Group) {
		FArea Area;

		if (!BoundsOf(Group, Area)) return;

		if (!bShelved) {
			bShelved = true;

			if (FArea All; BoundsOf(Everything, All)) {
				ShelfLeft = All.L;
				ShelfTop = All.B + Spacing::Runs;
				ShelfWidth = FMath::Max(Spacing::Shelf, All.R - All.L);
			}

			ShelfX = ShelfLeft;
			ShelfBottom = ShelfTop;
		}

		const double Width = Area.R - Area.L;

		if (ShelfX > ShelfLeft && ShelfX + Width > ShelfLeft + ShelfWidth) {
			ShelfX = ShelfLeft;
			ShelfTop = ShelfBottom + Spacing::Runs * 0.5;
		}

		Shift(Group, FVector2D(ShelfX - Area.L, ShelfTop - Area.T));

		ShelfX += Width + Spacing::Along;
		ShelfBottom = FMath::Max(ShelfBottom, ShelfTop + (Area.B - Area.T));

		Everything.Append(Group);
	}

	void FLayout::Arrange(const TArray<UEdGraphNode*>& Ways) {
		for (UEdGraphNode* Node : Graph->Nodes) {
			if (Node == nullptr || IsComment(Node)) continue;

			Footprints.Add(Node, MeasureGraphNode(Node));
		}

		/* What is kept stays where it stands, and is in the way of everything new */
		for (UEdGraphNode* Node : Graph->Nodes) {
			if (Node == nullptr || IsComment(Node) || !Kept.Contains(Node)) continue;

			At.Add(Node, FVector2D(Node->NodePosX, Node->NodePosY));
			Claimed.Add(Node);
			Everything.Add(Node);
		}

		for (UEdGraphNode* Way : Ways) {
			if (!IsMovable(Way) || Claimed.Contains(Way) || !Footprints.Contains(Way)) continue;

			Put(Follow(Way, nullptr, 0.0, 0.0));
		}

		/* New nodes feeding one that was kept, beside it */
		for (UEdGraphNode* Node : Graph->Nodes) {
			if (Node == nullptr || !Kept.Contains(Node) || !At.Contains(Node)) continue;

			TArray<UEdGraphNode*> Group;

			Feed(Node, Group);

			if (Group.Num() == 0) continue;

			Settle(Group, Everything, Spacing::Stacked);

			Everything.Append(Group);
		}

		const auto IsLeft = [this](const UEdGraphNode* Node) {
			return IsMovable(Node) && !Claimed.Contains(Node) && Footprints.Contains(Node);
		};

		/* Runs nothing leads into, then any still left, which only a loop leads into */
		for (int32 Pass = 0; Pass < 2; ++Pass) {
			for (UEdGraphNode* Node : Graph->Nodes) {
				if (!IsLeft(Node) || !Runs(Node)) continue;

				if (Pass == 0) {
					bool bLedInto = false;

					for (const UEdGraphPin* Pin : Node->Pins) {
						if (IsExec(Pin) && Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() > 0) bLedInto = true;
					}

					if (bLedInto) continue;
				}

				Shelve(Follow(Node, nullptr, 0.0, 0.0));
			}
		}

		/* And whatever works something out that nothing placed reads, from the end of it back. What
		 * nothing left over reads first, so each is laid out from the node its working out ends in. */
		for (int32 Pass = 0; Pass < 2; ++Pass) {
			for (UEdGraphNode* Node : Graph->Nodes) {
				if (!IsLeft(Node)) continue;

				if (Pass == 0) {
					bool bRead = false;

					for (const UEdGraphPin* Pin : Node->Pins) {
						if (Pin == nullptr || Pin->Direction != EGPD_Output) continue;

						for (const UEdGraphPin* Linked : Pin->LinkedTo) {
							if (Linked != nullptr && IsLeft(Linked->GetOwningNode())) bRead = true;
						}
					}

					if (bRead) continue;
				}

				Claimed.Add(Node);
				At.Add(Node, FVector2D::ZeroVector);

				TArray<UEdGraphNode*> Group;
				Group.Add(Node);

				Feed(Node, Group);

				Shelve(Group);
			}
		}

		for (UEdGraphNode* Node : Graph->Nodes) {
			if (!IsMovable(Node)) continue;

			if (const FVector2D* Corner = At.Find(Node)) {
				Node->NodePosX = FMath::RoundToInt(Corner->X);
				Node->NodePosY = FMath::RoundToInt(Corner->Y);
			}
		}
	}
}

namespace {
	/* What each graph was laid out from, so it can be laid out again the same way */
	struct FRemembered {
		TArray<TWeakObjectPtr<UEdGraphNode>> Ways;
		TArray<TWeakObjectPtr<UEdGraphNode>> Kept;
	};

	TMap<TWeakObjectPtr<UEdGraph>, FRemembered>& Remembered() {
		static TMap<TWeakObjectPtr<UEdGraph>, FRemembered> Held;

		return Held;
	}
}

void LayoutGraph(UEdGraph* Graph, const TArray<UEdGraphNode*>& Ways, const TSet<UEdGraphNode*>& Kept) {
	if (Graph == nullptr) return;

	FRemembered& Said = Remembered().FindOrAdd(Graph);

	Said.Ways.Reset();
	Said.Kept.Reset();

	for (UEdGraphNode* Way : Ways) Said.Ways.Add(Way);
	for (UEdGraphNode* One : Kept) Said.Kept.Add(One);

	FLayout(Graph, Kept).Arrange(Ways);
}

void RelayoutBlueprintGraphs(UBlueprint* Blueprint) {
	if (Blueprint == nullptr) return;

	TArray<TWeakObjectPtr<UEdGraph>> Gone;

	for (TPair<TWeakObjectPtr<UEdGraph>, FRemembered>& One : Remembered()) {
		UEdGraph* Graph = One.Key.Get();

		if (Graph == nullptr) {
			Gone.Add(One.Key);

			continue;
		}

		if (Graph->GetTypedOuter<UBlueprint>() != Blueprint) continue;

		TArray<UEdGraphNode*> Ways;
		TSet<UEdGraphNode*> Kept;

		for (const TWeakObjectPtr<UEdGraphNode>& Way : One.Value.Ways) {
			if (UEdGraphNode* Node = Way.Get(); Node != nullptr && Graph->Nodes.Contains(Node)) Ways.Add(Node);
		}

		for (const TWeakObjectPtr<UEdGraphNode>& Held : One.Value.Kept) {
			if (UEdGraphNode* Node = Held.Get(); Node != nullptr && Graph->Nodes.Contains(Node)) Kept.Add(Node);
		}

		FLayout(Graph, Kept).Arrange(Ways);

		Gone.Add(One.Key);
	}

	for (const TWeakObjectPtr<UEdGraph>& One : Gone) Remembered().Remove(One);
}
