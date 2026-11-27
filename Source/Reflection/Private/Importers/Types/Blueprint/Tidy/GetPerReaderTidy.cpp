/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/GraphTidy.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_VariableGet.h"

DECLARE_LOG_CATEGORY_CLASS(LogReflectionGetPerReader, All, All);

/* A variable read where it is wanted, once for each node that wants it.
 *
 * The compiler reads a variable once per statement that uses it, and reading it back makes one get
 * for every place it was read. Where two nodes in different parts of the graph read the same
 * variable, the bytecode is free to show it as one read handed to both, and drawn that way it is a
 * wire run the length of the graph from one get to everything that ever reads it.
 *
 * Nobody draws it that way. A plain read does nothing but read, and the graph reads it again for
 * every node that takes it, so one get per node says exactly the same thing. A get that reads its
 * variable off something another plain get read (a widget's text block, off the widget) is copied
 * along with that get, so each reader has the whole short chain to itself. Anything else feeding a
 * get stays where it is, and so does the get. */
struct FGetPerReaderTidy final : FGraphTidy {
	virtual const TCHAR* GetName() const override { return TEXT("Get Per Reader"); }

	/* How long a chain of gets is copied whole. Longer than this somebody meant it to be shared. */
	static constexpr int32 Longest = 4;

	/* Whether a get, and every get it reads off, is a plain read that can be made again anywhere */
	static bool IsCopyable(const UK2Node_VariableGet* Get, const int32 Depth) {
		if (Get == nullptr || !Get->IsNodePure() || Depth > Longest) return false;

		for (const UEdGraphPin* Pin : Get->Pins) {
			if (Pin == nullptr || Pin->Direction != EGPD_Input) continue;

			for (const UEdGraphPin* From : Pin->LinkedTo) {
				const UK2Node_VariableGet* Reads = From != nullptr ? Cast<UK2Node_VariableGet>(From->GetOwningNode()) : nullptr;

				if (!IsCopyable(Reads, Depth + 1)) return false;
			}
		}

		return true;
	}

	/* The same read again, and whatever it reads off made again with it */
	static UK2Node_VariableGet* Copy(UEdGraph* Graph, const UK2Node_VariableGet* Get) {
		UK2Node_VariableGet* Own = NewObject<UK2Node_VariableGet>(Graph, UK2Node_VariableGet::StaticClass(), NAME_None, RF_Transactional);

		Graph->AddNode(Own, false, false);

		Own->CreateNewGuid();
		Own->PostPlacedNewNode();

		Own->VariableReference = Get->VariableReference;
		Own->AllocateDefaultPins();

		Own->NodePosX = Get->NodePosX;
		Own->NodePosY = Get->NodePosY;

		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

		for (const UEdGraphPin* Pin : Get->Pins) {
			if (Pin == nullptr || Pin->Direction != EGPD_Input) continue;

			UEdGraphPin* Mine = Own->FindPin(Pin->PinName, EGPD_Input);

			if (Mine == nullptr) continue;

			/* What was typed in, where nothing is wired */
			Mine->DefaultValue = Pin->DefaultValue;
			Mine->DefaultObject = Pin->DefaultObject;
			Mine->DefaultTextValue = Pin->DefaultTextValue;

			for (const UEdGraphPin* From : Pin->LinkedTo) {
				const UK2Node_VariableGet* Reads = From != nullptr ? Cast<UK2Node_VariableGet>(From->GetOwningNode()) : nullptr;

				if (Reads == nullptr) continue;

				UK2Node_VariableGet* Again = Copy(Graph, Reads);

				if (UEdGraphPin* Out = Again->FindPin(From->PinName, EGPD_Output)) {
					Schema->TryCreateConnection(Out, Mine);
				}
			}
		}

		return Own;
	}

	virtual int32 Apply(UEdGraph* Graph) const override {
		if (Graph == nullptr) return 0;

		int32 Given = 0;

		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

		/* Over a copy, since every split adds to the graph */
		const TArray<UEdGraphNode*> Nodes = Graph->Nodes;

		for (UEdGraphNode* Node : Nodes) {
			UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Node);

			/* A get that checks what it read has a way in and out, and is part of the run */
			if (!IsCopyable(Get, 0)) continue;

			/* Everybody after the first node to read it gets a get of their own */
			const UEdGraphNode* First = nullptr;
			TArray<TPair<UEdGraphPin*, FName>> Readers;

			for (UEdGraphPin* Pin : Get->Pins) {
				if (Pin == nullptr || Pin->Direction != EGPD_Output) continue;

				for (UEdGraphPin* Reader : Pin->LinkedTo) {
					const UEdGraphNode* By = Reader != nullptr ? Reader->GetOwningNode() : nullptr;

					if (By == nullptr) continue;

					if (First == nullptr) First = By;
					else if (By != First) Readers.Add(TPair<UEdGraphPin*, FName>(Reader, Pin->PinName));
				}
			}

			for (const TPair<UEdGraphPin*, FName>& Reader : Readers) {
				UK2Node_VariableGet* Own = Copy(Graph, Get);

				/* Asked of the schema rather than forced, since the reader lets go of the one it had */
				if (UEdGraphPin* Out = Own->FindPin(Reader.Value, EGPD_Output)) {
					Schema->TryCreateConnection(Out, Reader.Key);
				}

				Given++;
			}
		}

		if (Given > 0) {
			UE_LOG(LogReflectionGetPerReader, Display, TEXT("\"%s\": %d reader(s) given a get of their own"), *Graph->GetName(), Given);
		}

		return Given;
	}
};

REGISTER_TIDY(FGetPerReaderTidy)
