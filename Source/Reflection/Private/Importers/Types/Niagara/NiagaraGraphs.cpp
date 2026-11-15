/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Niagara/NiagaraGraphs.h"

#include "Engine/EngineUtilities.h"
#include "Importers/Constructor/Importer.h"
#include "Serializers/PropertySerializer.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"

#include "NiagaraGraph.h"
#include "NiagaraScript.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScriptSource.h"
#if UE5_4_BEYOND
#include "EdGraphSchema_Niagara.h"
#include "Interfaces/IPluginManager.h"
#include "NiagaraScriptVariable.h"
#endif

namespace {
	/* The object a reference names, by the whole path under the package rather than its leaf.
	 *
	 * A script carries a graph per version of itself and they are copies, so the same node name
	 * appears once per graph. Told apart by the leaf they are the same node, and a link then lands
	 * on whichever graph was read last, which is a wire between two graphs. */
	FString ReferencedPath(const TSharedPtr<FJsonObject>& Reference) {
		FString Named;

		if (!Reference.IsValid() || !Reference->TryGetStringField(TEXT("ObjectName"), Named)) {
			return FString();
		}

		return StripObjectOuter(Named);
	}

	/* What a pin says of itself, given either as the text or as the pieces it was made from */
	FText TextOf(const TSharedPtr<FJsonValue>& Value) {
		if (!Value.IsValid()) return FText::GetEmpty();

		if (Value->Type == EJson::String) return FText::FromString(Value->AsString());

		const TSharedPtr<FJsonObject> Object = Value->AsObject();

		if (!Object.IsValid()) return FText::GetEmpty();

		FString Said;

		if (Object->TryGetStringField(TEXT("CultureInvariantString"), Said)) {
			return FText::AsCultureInvariant(Said);
		}

		if (Object->TryGetStringField(TEXT("SourceString"), Said)) {
			FString Namespace;
			FString Key;
			const FText Source = FText::FromString(Said);
			if (Object->TryGetStringField(TEXT("Namespace"), Namespace) && Object->TryGetStringField(TEXT("Key"), Key)) {
				return FText::ChangeKey(Namespace, Key, Source);
			}
			return Source;
		}

		return FText::GetEmpty();
	}

	template <typename TPin>
	auto RestoreSourceIndex(TPin* Pin, const FJsonObject& Entry, int) -> decltype(Pin->SourceIndex, void()) {
		Entry.TryGetNumberField(TEXT("SourceIndex"), Pin->SourceIndex);
	}

	template <typename TPin>
	void RestoreSourceIndex(TPin*, const FJsonObject&, long) {}

	/* The pins an export lists, or nothing where it lists none */
	const TArray<TSharedPtr<FJsonValue>>* PinsOf(const FUObjectExport* Export) {
		if (Export == nullptr || !Export->IsJsonValid()) return nullptr;

		const TArray<TSharedPtr<FJsonValue>>* Listed = nullptr;

		return Export->JsonObject->TryGetArrayField(TEXT("Pins"), Listed) ? Listed : nullptr;
	}

#if WITH_EDITORONLY_DATA
	/* The graph a function call node names.
	 *
	 * UE4 declares the node MinimalAPI, so the call that answers this is there to read and not
	 * there to link against. Walked by hand instead, the same two steps the engine takes: the
	 * script the node names, and the graph its source holds. */
	const UNiagaraGraph* CalledGraphOf(const UNiagaraNodeFunctionCall* Call) {
#if ENGINE_UE4
		if (Call->FunctionScript == nullptr) return nullptr;

		/* 4.27 keeps a script per version and names the newest one apart from the rest */
#if UE4_27
		const UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Call->FunctionScript->GetLatestSource());
#else
		const UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Call->FunctionScript->GetSource());
#endif

		return Source != nullptr ? Source->NodeGraph : nullptr;
#else
		return Call->GetCalledGraph();
#endif
	}

#if UE5_4_BEYOND
	void RestoreAddedFunctionOutputs(UNiagaraNodeFunctionCall* Call, const UNiagaraGraph* Called) {
		const UNiagaraNodeOutput* FunctionOutput = nullptr;
		for (const UEdGraphNode* Node : Called->Nodes) {
			const UNiagaraNodeOutput* Output = Cast<UNiagaraNodeOutput>(Node);
			if (Output == nullptr) continue;
			if (FunctionOutput != nullptr) return;
			FunctionOutput = Output;
		}
		if (FunctionOutput == nullptr) return;
		const auto& Outputs = FunctionOutput->Outputs;

		TArray<UEdGraphPin*> Pins;
		for (UEdGraphPin* Pin : Call->Pins) {
			if (Pin != nullptr && Pin->Direction == EGPD_Output && !Pin->bOrphanedPin) Pins.Add(Pin);
		}
		if (Pins.IsEmpty() || Pins.Num() >= Outputs.Num()) return;

		// Update only when the target added outputs and the full source signature
		// still matches its prefix. Existing pins keep their IDs and connections.
		for (int32 Index = 0; Index < Pins.Num(); ++Index) {
			if (Pins[Index]->PinName != Outputs[Index].GetName() ||
				Pins[Index]->PinType != UEdGraphSchema_Niagara::TypeDefinitionToPinType(Outputs[Index].GetType())) return;
		}
		for (int32 Index = Pins.Num(); Index < Outputs.Num(); ++Index) {
			const FNiagaraVariable& Output = Outputs[Index];
			UEdGraphPin* Pin = Call->CreatePin(EGPD_Output,
				UEdGraphSchema_Niagara::TypeDefinitionToPinType(Output.GetType()), Output.GetName());
			Pin->bDefaultValueIsIgnored = true;
		}
	}
#endif
#endif
}

bool FNiagaraGraphs::HasSerializedPins(const FUObjectExportContainer* Container) {
	if (Container == nullptr) return false;

	int32 Missing = 0;
	int32 Nodes = 0;
	for (FUObjectExport* Export : Container->Exports) {
		if (!Export->IsJsonValid()) continue;
		const UClass* Class = Export->GetClass();
		const auto* Pins = PinsOf(Export);
		if (Pins == nullptr && (Class == nullptr || !Class->IsChildOf(UEdGraphNode::StaticClass()))) continue;

		Nodes++;
		if (Pins == nullptr) Missing++;
	}

	if (Missing > 0 || Nodes == 0) {
		const FString Detail = Nodes == 0 ? TEXT("No graph nodes were present in the export.")
			: FString::Printf(TEXT("%d of %d node exports lack serialized pins."), Missing, Nodes);
		UE_LOG(LogReflection, Warning, TEXT("Niagara editor graph data could not be read. %s"), *Detail);
		FImportIssues::Report(EImportIssue::Data, TEXT("Niagara editor graph data could not be read"), Detail);
		return false;
	}
	return true;
}

void FNiagaraGraphs::Rebuild(FUObjectExportContainer* Container, UPackage* Package,
	UPropertySerializer* Serializer, FNiagaraGraphStats& OutStats) {
	if (Container == nullptr) return;

	// Keep the serialized node order where the references resolve, and recover
	// any missing entries from the exports actually owned by this graph.
	for (FUObjectExport* Export : Container->Exports) {
		UEdGraph* Graph = Cast<UEdGraph>(Export->Object);

		if (Graph == nullptr) continue;

		TSet<UEdGraphNode*> Placed;
		Graph->Nodes.RemoveAll([&](UEdGraphNode* Node) {
			if (Node == nullptr || Node->GetOuter() != Graph || Placed.Contains(Node)) return true;
			Placed.Add(Node);
			return false;
		});

		for (FUObjectExport* Under : Container->Exports) {
			UEdGraphNode* Beneath = Cast<UEdGraphNode>(Under->Object);

			if (Beneath != nullptr && Beneath->GetOuter() == Graph && !Placed.Contains(Beneath)) {
				Graph->Nodes.Add(Beneath);
				Placed.Add(Beneath);
			}
		}
		OutStats.Placed += Placed.Num();

#if UE5_4_BEYOND && WITH_EDITORONLY_DATA
		if (UNiagaraGraph* NiagaraGraph = Cast<UNiagaraGraph>(Graph)) {
			// Older graphs may mark the switch only on its node. Set the metadata
			// on the editable graph before the compiler duplicates it.
			TSet<FName> SwitchNames;
			for (const FNiagaraVariable& Switch : NiagaraGraph->FindStaticSwitchInputs()) {
				SwitchNames.Add(Switch.GetName());
			}
			for (const UEdGraphNode* Node : Graph->Nodes) {
				// Compiler constants and switches exposed as pins are excluded from
				// FindStaticSwitchInputs, but the node's PostLoad checks these too.
				const FNameProperty* InputName = FindFProperty<FNameProperty>(Node->GetClass(), TEXT("InputParameterName"));
				if (InputName != nullptr && FindFProperty<FStructProperty>(Node->GetClass(), TEXT("SwitchTypeData")) != nullptr) {
					SwitchNames.Add(InputName->GetPropertyValue_InContainer(Node));
				}
			}
			for (const auto& Entry : NiagaraGraph->GetAllMetaData()) {
				if (Entry.Value != nullptr && SwitchNames.Contains(Entry.Key.GetName())) {
					Entry.Value->SetIsStaticSwitch(true);
				}
			}
		}
#endif
	}

	/* Every pin the export lists, made on the node that owns it. Kept against the node and the id
	 * it was written with, because that pair is what a link names. */
	TMap<TPair<FString, FGuid>, UEdGraphPin*> Made;
	auto FindPin = [&Made](const TSharedPtr<FJsonValue>& Value) -> UEdGraphPin* {
		const TSharedPtr<FJsonObject>* Reference = nullptr;
		const TSharedPtr<FJsonObject>* Owner = nullptr;
		FString Id;
		if (!Value.IsValid() || !Value->TryGetObject(Reference)
			|| !(*Reference)->TryGetStringField(TEXT("PinId"), Id)
			|| !(*Reference)->TryGetObjectField(TEXT("OwningNode"), Owner)) return nullptr;

		return Made.FindRef(TPair<FString, FGuid>(ReferencedPath(*Owner), StringToGuid(Id)));
	};

	for (FUObjectExport* Export : Container->Exports) {
		UEdGraphNode* Node = Cast<UEdGraphNode>(Export->Object);

		if (Node == nullptr) continue;

		const TArray<TSharedPtr<FJsonValue>>* Listed = PinsOf(Export);
		if (Listed == nullptr) continue;

		/* A node holding pins already is one this has been over before, and it is gone over again
		 * whenever something asks for the asset a second time.
		 *
		 * Every pin is made from the export afresh, so a node kept as it was ends up with each of
		 * its pins twice under the one id. A link names that id against the node that owns it, so
		 * on the way back in only one of the two answers to the name and everything wired to the
		 * other reaches nothing. Taking off what is there first leaves a second pass where the
		 * first one left off. */
		TArray<UEdGraphPin*> Standing = MoveTemp(Node->Pins);

		Node->Pins.Reset();

		for (UEdGraphPin* Pin : Standing) {
			if (Pin == nullptr) continue;

			Pin->BreakAllPinLinks();

			/* Thrown away rather than removed. Removing one tells the node it lost a pin, which
			 * is a thing a person does to it and not a thing this is doing: a Niagara node hearing
			 * that goes and makes itself again from what it is a call to, and what it makes is
			 * built on whatever it can find under that name. Nothing is told anything here. */
			UEdGraphNode::DestroyPin(Pin);
		}

		for (const TSharedPtr<FJsonValue>& Value : *Listed) {
			const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;

			if (!Entry.IsValid()) continue;

			FString Named;
			FString Way;

			Entry->TryGetStringField(TEXT("PinName"), Named);
			Entry->TryGetStringField(TEXT("Direction"), Way);

			/* What the pin carries, read as the struct it is rather than picked apart by hand, so
			 * the type object it names is resolved the same way every other reference is */
			FEdGraphPinType Type;

			if (const TSharedPtr<FJsonObject>* Described = nullptr;
				Serializer != nullptr && Entry->TryGetObjectField(TEXT("PinType"), Described)) {
				Serializer->DeserializeStruct(FEdGraphPinType::StaticStruct(), Described->ToSharedRef(), &Type, nullptr);
			}

			UEdGraphPin* Pin = Node->CreatePin(Way == TEXT("EGPD_Output") ? EGPD_Output : EGPD_Input, Type, FName(*Named));

			if (Pin == nullptr) continue;

			RestoreSourceIndex(Pin, *Entry, 0);

			/* Kept as it was written, since a link names the id and not the name */
			if (FString Id; Entry->TryGetStringField(TEXT("PinId"), Id)) {
				Pin->PinId = StringToGuid(Id);
			}

			/* The name a pin keeps for good, which is not the one a link uses.
			 *
			 * A node that pairs its pins up records the pairs by this name rather than by the
			 * other: a parameter map's get node says which of its inputs stands for the default
			 * of which of its outputs, and a Niagara if node its own pairs. Left unset the pairs
			 * name nothing, and the node comes out with the pins it should have and no idea what
			 * any of them are to each other. */
#if WITH_EDITORONLY_DATA
			if (FString Persistent; Entry->TryGetStringField(TEXT("PersistentGuid"), Persistent)) {
				Pin->PersistentGuid = StringToGuid(Persistent);
			}
#endif

			/* What the pin is on the node, beside what it carries. The engine packs these into
			 * one number in an order it keeps fixed on purpose, and reads them back out of the
			 * same places. A pin made without them shows where the original hid it and takes a
			 * connection where the original refused one. */
#if WITH_EDITORONLY_DATA
			if (int32 Flags = 0; Entry->TryGetNumberField(TEXT("BitField"), Flags)) {
				Pin->bHidden = (Flags & 1 << 0) != 0;
				Pin->bNotConnectable = (Flags & 1 << 1) != 0;
				Pin->bDefaultValueIsReadOnly = (Flags & 1 << 2) != 0;
				Pin->bDefaultValueIsIgnored = (Flags & 1 << 3) != 0;
				Pin->bAdvancedView = (Flags & 1 << 4) != 0;
				Pin->bOrphanedPin = (Flags & 1 << 5) != 0;
			}
#endif

			if (const TSharedPtr<FJsonValue> Friendly = Entry->TryGetField(TEXT("PinFriendlyName")); Friendly.IsValid()) {
				Pin->PinFriendlyName = TextOf(Friendly);
			}
			if (const TSharedPtr<FJsonValue> Default = Entry->TryGetField(TEXT("DefaultTextValue")); Default.IsValid()) {
				Pin->DefaultTextValue = TextOf(Default);
			}
			if (const TSharedPtr<FJsonObject>* Default = nullptr; Entry->TryGetObjectField(TEXT("DefaultObject"), Default)) {
				FString Name;
				FString Path;
				TObjectPtr<UObject> Object;
				if ((*Default)->TryGetStringField(TEXT("ObjectName"), Name) && (*Default)->TryGetStringField(TEXT("ObjectPath"), Path)) {
					if (FUObjectExport* Local = Container->FindByFullObjectName(Name, Path); Local->IsJsonValid()) {
						Object = Local->Object;
					} else {
						IImporter Importer;
						Importer.LoadExport(Default, Object);
					}
				}
				Pin->DefaultObject = Object;
			}

			Entry->TryGetStringField(TEXT("DefaultValue"), Pin->DefaultValue);
			Entry->TryGetStringField(TEXT("AutogeneratedDefaultValue"), Pin->AutogeneratedDefaultValue);
			Entry->TryGetStringField(TEXT("PinToolTip"), Pin->PinToolTip);

			Made.Add(TPair<FString, FGuid>(Node->GetPathName(Package), Pin->PinId), Pin);

			OutStats.Pins++;
		}
	}

	/* What a node made of its own pins.
	 *
	 * Some nodes pair their pins off: a parameter map's get node says which of its inputs stands
	 * for the default of which of its outputs, and it says so by the name a pin keeps for good
	 * rather than the one a link uses. Both halves have to be a pin that is there, or the node
	 * drops the pairing the first time it looks at itself and the pin it named goes back to
	 * standing for nothing.
	 *
	 * Which of them does this is not asked. A pairing is a map of one such name to another, so
	 * that is what is looked for, and a node that keeps none has none to check. */
#if WITH_EDITORONLY_DATA
	for (FUObjectExport* Export : Container->Exports) {
		const UEdGraphNode* Node = Cast<UEdGraphNode>(Export->Object);

		if (Node == nullptr) continue;

		TSet<FGuid> Kept;

		for (const UEdGraphPin* Pin : Node->Pins) {
			if (Pin != nullptr && Pin->PersistentGuid.IsValid()) Kept.Add(Pin->PersistentGuid);
		}

		for (TFieldIterator<FMapProperty> Named(Node->GetClass()); Named; ++Named) {
			const FStructProperty* Says = CastField<FStructProperty>(Named->KeyProp);
			const FStructProperty* StandsVar = CastField<FStructProperty>(Named->ValueProp);

			if (Says == nullptr || StandsVar == nullptr
				|| Says->Struct != TBaseStructure<FGuid>::Get()
				|| StandsVar->Struct != TBaseStructure<FGuid>::Get()) {
				continue;
			}

			FScriptMapHelper Held(*Named, Named->ContainerPtrToValuePtr<void>(Node));

			for (int32 Index = 0; Index < Held.GetMaxIndex(); Index++) {
				if (!Held.IsValidIndex(Index)) continue;

				const FGuid& One = *reinterpret_cast<const FGuid*>(Held.GetKeyPtr(Index));
				const FGuid& Other = *reinterpret_cast<const FGuid*>(Held.GetValuePtr(Index));

				if (!Kept.Contains(One) || !Kept.Contains(Other)) OutStats.Unpaired++;
			}
		}
	}
#endif

	/* Where a call goes.
	 *
	 * A function call node stands for a module, and walking the graph means walking into the one
	 * it names. The engine reaches for that graph's output and goes through it without asking
	 * whether it found one, so a call naming a script that has no module output to it takes the
	 * editor down the moment anything walks the graph, which opening the asset does.
	 *
	 * Said here with both names in it, because a call is only wrong by what it points at. */
#if WITH_EDITORONLY_DATA
	for (FUObjectExport* Export : Container->Exports) {
		const UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Export->Object);

		if (Call == nullptr) continue;

		const UNiagaraGraph* Called = CalledGraphOf(Call);

		/* A call naming nothing is one the engine leaves alone */
		if (Called == nullptr) continue;

		bool bWalkable = false;

		for (const UEdGraphNode* Node : Called->Nodes) {
			const UNiagaraNodeOutput* Output = Cast<UNiagaraNodeOutput>(Node);

			if (Output == nullptr) continue;

			if (Output->ScriptType == ENiagaraScriptUsage::Function
				|| Output->ScriptType == ENiagaraScriptUsage::Module
				|| Output->ScriptType == ENiagaraScriptUsage::DynamicInput) {
				bWalkable = true;

				break;
			}
		}

		if (bWalkable) continue;

		OutStats.Uncalled++;

		UE_LOG(LogReflection, Warning, TEXT("\"%s\" calls \"%s\", whose graph \"%s\" holds %d node(s) and no module output to walk into"),
			*Call->GetPathName(Package), *GetPathNameSafe(Call->FunctionScript),
			*GetPathNameSafe(Called), Called->Nodes.Num());
	}
#endif

	/* Joined up once every pin exists, since a link reaches across nodes and there is no order
	 * that has both ends of all of them made first */
	for (FUObjectExport* Export : Container->Exports) {
		const TArray<TSharedPtr<FJsonValue>>* Listed = PinsOf(Export);

		if (Cast<UEdGraphNode>(Export->Object) == nullptr || Listed == nullptr) continue;

		for (const TSharedPtr<FJsonValue>& Value : *Listed) {
			const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;

			FString Id;

			if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("PinId"), Id)) continue;

			UEdGraphPin** From = Made.Find(TPair<FString, FGuid>(Export->Object->GetPathName(Package), StringToGuid(Id)));

			if (From == nullptr) continue;
			UEdGraphPin* Pin = *From;
			auto Resolve = [&](const TSharedPtr<FJsonValue>& Reference) {
				UEdGraphPin* Resolved = FindPin(Reference);
				if (Reference.IsValid() && !Reference->IsNull() && Resolved == nullptr) OutStats.Dangling++;
				return Resolved;
			};

			Pin->ParentPin = Resolve(Entry->TryGetField(TEXT("ParentPin")));
			Pin->ReferencePassThroughConnection = Resolve(Entry->TryGetField(TEXT("ReferencePassThroughConnection")));
			if (const TArray<TSharedPtr<FJsonValue>>* SubPins = nullptr; Entry->TryGetArrayField(TEXT("SubPins"), SubPins)) {
				for (const TSharedPtr<FJsonValue>& One : *SubPins) {
					if (UEdGraphPin* SubPin = Resolve(One)) Pin->SubPins.Add(SubPin);
				}
			}

			if (const TArray<TSharedPtr<FJsonValue>>* Linked = nullptr; Entry->TryGetArrayField(TEXT("LinkedTo"), Linked)) {
				for (const TSharedPtr<FJsonValue>& One : *Linked) {
					if (UEdGraphPin* To = Resolve(One)) {
						Pin->LinkedTo.Add(To);
						OutStats.Links++;
					}
				}
			}
		}
	}

#if UE5_4_BEYOND && WITH_EDITORONLY_DATA
	for (FUObjectExport* Export : Container->Exports) {
		UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Export->Object);
		if (Call == nullptr || Call->FunctionScript == nullptr || Call->FunctionScript->GetOutermost() == Package) continue;
		const FString FunctionPackage = Call->FunctionScript->GetOutermost()->GetName();
		FString Mount;
		if (!FunctionPackage.Mid(1).Split(TEXT("/"), &Mount, nullptr)) continue;
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Mount);
		if (Mount != TEXT("Engine") && (!Plugin.IsValid() || Plugin->GetLoadedFrom() != EPluginLoadedFrom::Engine)) continue;
		if (const UNiagaraGraph* Called = CalledGraphOf(Call)) RestoreAddedFunctionOutputs(Call, Called);
	}
#endif
}
