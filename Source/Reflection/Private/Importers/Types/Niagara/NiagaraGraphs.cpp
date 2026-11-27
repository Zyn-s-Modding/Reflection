/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Niagara/NiagaraGraphs.h"
#include "NiagaraWeightedIntArray.h"

#include "Engine/EngineUtilities.h"
#include "Importers/Constructor/Importer.h"
#include "Serializers/PropertySerializer.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"

#include "NiagaraGraph.h"
#include "NiagaraParameterCollection.h"
#include "NiagaraDataInterfaceCurveBase.h"
#include "NiagaraNode.h"
#include "NiagaraScript.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScriptSource.h"
#if UE5_4_BEYOND
#include "EdGraphSchema_Niagara.h"
#include "Interfaces/IPluginManager.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraDataInterface.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeInput.h"
#include "NiagaraSystem.h"
#include "Importers/Constructor/ImportIssues.h"
#endif

namespace {
	FName NiagaraObjectPath(FString Path) {
		if (Path.StartsWith(TEXT("/"))) {
			Path.Split(TEXT("."), nullptr, &Path);
		}
		Path.ReplaceInline(TEXT("."), TEXT(":"));
		return FName(*Path);
	}

	FName NiagaraExportPath(const FUObjectExport* Export) {
		const FName Outer = Export->GetOuterTree();
		return Outer.IsNone() ? Export->GetName() : NiagaraObjectPath(Outer.ToString() + TEXT(":") + Export->GetName().ToString());
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

		if (Export->JsonObject->TryGetArrayField(TEXT("Pins"), Listed)) return Listed;

		const TSharedPtr<FJsonObject>* Properties = nullptr;
		return Export->JsonObject->TryGetObjectField(TEXT("Properties"), Properties)
			&& (*Properties)->TryGetArrayField(TEXT("Pins"), Listed) ? Listed : nullptr;
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

	/* The other way round: the game's version of an engine function has outputs this engine's
	 * version hasn't. RGBToHSV is the standing example: 5.4 gives HSV, newer engines give HSV, Hue,
	 * Saturation, Value and Alpha. The call comes across with the newer pins and the 5.4 script under
	 * it, and the compile counts outputs: "Incorrect number of outputs", and everything downstream of
	 * the call fails with it.
	 *
	 * Where the outputs this engine's function has are the call's own, in order, and nothing is
	 * wired to the ones it lacks, those pins are dropped and the call is the 5.4 one. Where something
	 * is wired to one, the call is left alone and reported, since this engine's function can't give
	 * that value. Returns the names it dropped, or reports and returns nothing. */
	TArray<FString> DropRemovedFunctionOutputs(UNiagaraNodeFunctionCall* Call, const UNiagaraGraph* Called, TArray<FString>& OutUnserved) {
		const UNiagaraNodeOutput* FunctionOutput = nullptr;
		for (const UEdGraphNode* Node : Called->Nodes) {
			const UNiagaraNodeOutput* Output = Cast<UNiagaraNodeOutput>(Node);
			if (Output == nullptr) continue;
			if (FunctionOutput != nullptr) return {};
			FunctionOutput = Output;
		}
		if (FunctionOutput == nullptr || FunctionOutput->ScriptType != ENiagaraScriptUsage::Function) return {};
		const auto& Outputs = FunctionOutput->Outputs;

		TArray<UEdGraphPin*> Pins;
		for (UEdGraphPin* Pin : Call->Pins) {
			if (Pin != nullptr && Pin->Direction == EGPD_Output && !Pin->bOrphanedPin) Pins.Add(Pin);
		}
		if (Pins.Num() <= Outputs.Num()) return {};

		const auto Matches = [](const UEdGraphPin* Pin, const FNiagaraVariable& Output) {
			return Pin->PinName == Output.GetName() && Pin->PinType == UEdGraphSchema_Niagara::TypeDefinitionToPinType(Output.GetType());
		};

		/* This engine's outputs, in its order, have to be the call's, in the call's */
		TArray<UEdGraphPin*> Kept;
		TArray<UEdGraphPin*> Extra;
		int32 Next = 0;
		for (UEdGraphPin* Pin : Pins) {
			if (Next < Outputs.Num() && Matches(Pin, Outputs[Next])) {
				Kept.Add(Pin);
				Next++;
			} else {
				Extra.Add(Pin);
			}
		}
		if (Next != Outputs.Num()) return {};

		TArray<FString> Dropped;
		for (const UEdGraphPin* Pin : Extra) {
			if (Pin->LinkedTo.Num() > 0) {
				OutUnserved.Add(FString::Printf(TEXT("%s.%s"), *GetNameSafe(Call->FunctionScript), *Pin->PinName.ToString()));
				return {};
			}
			Dropped.Add(Pin->PinName.ToString());
		}

		for (UEdGraphPin* Pin : Extra) {
			Call->RemovePin(Pin);
		}

		return Dropped;
	}

	void RestoreLegacyMaterialParameterSwitches(UNiagaraNodeFunctionCall& Call, const UNiagaraGraph& Called) {
		if (Call.FunctionScript->GetOutermost()->GetFName() != TEXT("/Niagara/Modules/Update/Material/DynamicMaterialParameters")) return;
		const TArray<FNiagaraVariable> Switches = Called.FindStaticSwitchInputs();
		const FNiagaraTypeDefinition& BoolType = FNiagaraTypeDefinition::GetBoolDef();
		for (UEdGraphPin* Pin : Call.Pins) {
			if (Pin == nullptr || Pin->Direction != EGPD_Input || Pin->LinkedTo.Num() != 0
				|| Pin->PinType.PinCategory != TEXT("Enum")) continue;
			const FString Name = Pin->PinName.ToString();
			if (!Name.StartsWith(TEXT("Write Parameter Index "))) continue;
			const UObject* Enum = Pin->PinType.PinSubCategoryObject.Get();
			if (Enum != nullptr && Enum->GetFName() != TEXT("ENiagara_DynamicMaterialParametersInputMode")) continue;
			if (!Switches.ContainsByPredicate([&](const FNiagaraVariable& Switch) {
				return Switch.GetType() == BoolType && Switch.GetName() == Pin->PinName;
			})) continue;

			// The old module exposes a boolean for Off and scalar Float. Its switch cannot represent
			// the vector and color modes in the newer enum.
			const auto ToBool = [](const FString& Value) -> const TCHAR* {
				if (Value == TEXT("NewEnumerator0")) return TEXT("false");
				if (Value == TEXT("NewEnumerator1")) return TEXT("true");
				return nullptr;
			};
			const TCHAR* Default = ToBool(Pin->DefaultValue);
			const TCHAR* Generated = ToBool(Pin->AutogeneratedDefaultValue);
			if (Default == nullptr || (Generated == nullptr && !Pin->AutogeneratedDefaultValue.IsEmpty())) continue;
			Pin->PinType = UEdGraphSchema_Niagara::TypeDefinitionToPinType(BoolType);
			Pin->DefaultValue = Default;
			if (Generated != nullptr) Pin->AutogeneratedDefaultValue = Generated;
		}
	}
#endif
#endif
#if UE5_4_BEYOND && WITH_EDITORONLY_DATA
	/* What fitting a data interface call to this engine did, for the report */
	struct FDataInterfaceFit {
		TArray<FString> Changed;
		TArray<FString> LostWired;
	};

	UEdGraphPin* TakePinNamed(TArray<UEdGraphPin*>& Pins, const FName Name) {
		const int32 Index = Pins.IndexOfByPredicate([Name](const UEdGraphPin* Pin) { return Pin->PinName == Name; });
		if (Index == INDEX_NONE) return nullptr;
		UEdGraphPin* Pin = Pins[Index];
		Pins.RemoveAt(Index);
		return Pin;
	}

	/* A call to a data interface function whose signature is the one a later engine gave it.
	 *
	 * Functions on engine data interfaces gain and lose inputs and outputs between versions
	 * (Collision Query's async query, Camera's properties), and the compiler takes a call only
	 * when its signature is exactly one the interface offers here: otherwise "Function call
	 * signature does not match DataInterface possible signatures" or "Incorrect number of outputs",
	 * and the script, and so the system, doesn't compile. So the call takes this engine's
	 * signature of the same name, keeping the specifiers it chose, and its pins are fitted to it
	 * by name: the interface pin is kept whatever it is called, a pin this engine has too keeps
	 * its links and value, one it adds comes in at its default, and one it doesn't have goes,
	 * said by name where something was wired to it. Pins end up in the signature's order, which
	 * is how the compiler pairs outputs to results. A function this engine doesn't have at all is
	 * left for the caller to take out. Returns false when the call needed nothing. */
	bool FitDataInterfaceCall(UNiagaraNodeFunctionCall& Call, bool& bNoSuchFunction, FDataInterfaceFit& Fit) {
		bNoSuchFunction = false;
		FNiagaraFunctionSignature& Signature = Call.Signature;
		if (Call.FunctionScript != nullptr || !Signature.IsValid() || Signature.Inputs.Num() == 0) return false;
		const FNiagaraTypeDefinition Owner = Signature.Inputs[0].GetType();
		if (!Owner.IsValid() || !Owner.IsDataInterface() || Owner.GetClass() == nullptr) return false;
		UNiagaraDataInterface* Interface = Cast<UNiagaraDataInterface>(Owner.GetClass()->GetDefaultObject());
		if (Interface == nullptr) return false;

		TArray<FNiagaraFunctionSignature> Functions;
		Interface->GetFunctions(Functions);
		const auto Offered = [&Functions](const FNiagaraFunctionSignature& Sig) {
			return Functions.ContainsByPredicate([&Sig](const FNiagaraFunctionSignature& Here) { return Here.EqualsIgnoringSpecifiers(Sig); });
		};
		if (Offered(Signature)) return false;

		/* The engine's own way forward first, for a call written against an older version */
		FNiagaraFunctionSignature Upgraded = Signature;
		const FNiagaraFunctionSignature* Best = nullptr;
		if (Interface->UpgradeFunctionCall(Upgraded) && Offered(Upgraded)) {
			Best = Functions.FindByPredicate([&Upgraded](const FNiagaraFunctionSignature& Here) { return Here.EqualsIgnoringSpecifiers(Upgraded); });
		}
		if (Best == nullptr) {
			int32 BestScore = -1;
			for (const FNiagaraFunctionSignature& Here : Functions) {
				if (!Here.Name.ToString().Equals(Signature.Name.ToString()) || Here.VariadicInput() || Here.VariadicOutput()) continue;
				int32 Score = 0;
				for (const FNiagaraVariable& Input : Here.Inputs) Score += Signature.Inputs.Contains(Input) ? 2 : 0;
				for (const FNiagaraVariable& Output : Here.Outputs) Score += Signature.Outputs.Contains(Output) ? 2 : 0;
				Score += Here.bRequiresExecPin == Signature.bRequiresExecPin ? 1 : 0;
				if (Score > BestScore) {
					Best = &Here;
					BestScore = Score;
				}
			}
		}
		if (Best == nullptr) {
			bNoSuchFunction = true;
			return false;
		}

		FNiagaraFunctionSignature Fitted = *Best;
		for (TPair<FName, FName>& Specifier : Fitted.FunctionSpecifiers) {
			if (const FName* Chosen = Signature.FunctionSpecifiers.Find(Specifier.Key)) Specifier.Value = *Chosen;
		}
		const FName OldInterfaceName = Signature.Inputs[0].GetName();

		const UEdGraphSchema_Niagara* Schema = GetDefault<UEdGraphSchema_Niagara>();
		TArray<UEdGraphPin*> Inputs, Outputs, Rest;
		UEdGraphPin* MapIn = nullptr;
		UEdGraphPin* MapOut = nullptr;
		for (UEdGraphPin* Pin : Call.Pins) {
			if (Pin == nullptr) continue;
			if (Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryMisc) Rest.Add(Pin);
			else if (Call.IsParameterMapPin(Pin)) (Pin->Direction == EGPD_Input ? MapIn : MapOut) = Pin;
			else (Pin->Direction == EGPD_Input ? Inputs : Outputs).Add(Pin);
		}

		TArray<FString> Changes;
		TArray<UEdGraphPin*> Ordered;
		const auto Place = [&](UEdGraphPin* Kept, const FNiagaraVariable& Variable, EEdGraphPinDirection Direction) {
			UEdGraphPin* Pin = Kept;
			if (Pin == nullptr) {
				Pin = Call.CreatePin(Direction, Schema->TypeDefinitionToPinType(Variable.GetType()), Variable.GetName());
				Pin->bDefaultValueIsIgnored = Direction == EGPD_Output;
				FString Default;
				if (Direction == EGPD_Input && Schema->TryGetPinDefaultValueFromNiagaraVariable(Variable, Default)) {
					Pin->AutogeneratedDefaultValue = Default;
					Pin->DefaultValue = Default;
				}
				Changes.Add(FString::Printf(TEXT("added %s %s"), Direction == EGPD_Input ? TEXT("input") : TEXT("output"), *Variable.GetName().ToString()));
			} else {
				Pin->PinName = Variable.GetName();
				if (Schema->PinToTypeDefinition(Pin) != Variable.GetType()) {
					Pin->PinType = Schema->TypeDefinitionToPinType(Variable.GetType());
					Changes.Add(FString::Printf(TEXT("%s retyped"), *Variable.GetName().ToString()));
				}
			}
			Ordered.Add(Pin);
		};

		if (Fitted.bRequiresExecPin) {
			if (MapIn == nullptr) {
				MapIn = Call.CreatePin(EGPD_Input, Schema->TypeDefinitionToPinType(FNiagaraTypeDefinition::GetParameterMapDef()), FName());
				MapIn->bDefaultValueIsIgnored = true;
			}
			Ordered.Add(MapIn);
		}
		for (int32 Index = 0; Index < Fitted.Inputs.Num(); ++Index) {
			UEdGraphPin* Kept = Index == 0 ? TakePinNamed(Inputs, OldInterfaceName) : nullptr;
			if (Kept == nullptr) Kept = TakePinNamed(Inputs, Fitted.Inputs[Index].GetName());
			Place(Kept, Fitted.Inputs[Index], EGPD_Input);
		}
		if (Fitted.bRequiresExecPin) {
			if (MapOut == nullptr) {
				MapOut = Call.CreatePin(EGPD_Output, Schema->TypeDefinitionToPinType(FNiagaraTypeDefinition::GetParameterMapDef()), FName());
				MapOut->bDefaultValueIsIgnored = true;
			}
			Ordered.Add(MapOut);
		}
		for (const FNiagaraVariable& Output : Fitted.Outputs) {
			Place(TakePinNamed(Outputs, Output.GetName()), Output, EGPD_Output);
		}

		/* An execution pin pair the call no longer has: what came in is passed on past it */
		if (!Fitted.bRequiresExecPin && MapIn != nullptr && MapOut != nullptr) {
			if (MapIn->LinkedTo.Num() == 1) {
				UEdGraphPin* Source = MapIn->LinkedTo[0];
				for (UEdGraphPin* Target : TArray<UEdGraphPin*>(MapOut->LinkedTo)) {
					MapOut->BreakLinkTo(Target);
					Source->MakeLinkTo(Target);
				}
			}
			Inputs.Add(MapIn);
			Outputs.Add(MapOut);
		}

		for (UEdGraphPin* Gone : Inputs) {
			if (Gone->LinkedTo.Num() > 0 && !Gone->PinName.IsNone()) Fit.LostWired.AddUnique(FString::Printf(TEXT("%s input %s"), *Fitted.Name.ToString(), *Gone->PinName.ToString()));
			if (!Gone->PinName.IsNone()) Changes.Add(FString::Printf(TEXT("dropped input %s"), *Gone->PinName.ToString()));
		}
		for (UEdGraphPin* Gone : Outputs) {
			if (Gone->LinkedTo.Num() > 0 && !Gone->PinName.IsNone()) Fit.LostWired.AddUnique(FString::Printf(TEXT("%s output %s"), *Fitted.Name.ToString(), *Gone->PinName.ToString()));
			if (!Gone->PinName.IsNone()) Changes.Add(FString::Printf(TEXT("dropped output %s"), *Gone->PinName.ToString()));
		}
		Call.Pins = Ordered;
		Call.Pins.Append(Rest);
		for (UEdGraphPin* Gone : Inputs) { Gone->BreakAllPinLinks(); UEdGraphNode::DestroyPin(Gone); }
		for (UEdGraphPin* Gone : Outputs) { Gone->BreakAllPinLinks(); UEdGraphNode::DestroyPin(Gone); }

		Signature = Fitted;
		Fit.Changed.AddUnique(FString::Printf(TEXT("%s.%s (%s)"), *Owner.GetName(), *Fitted.Name.ToString(),
			Changes.IsEmpty() ? TEXT("signature only") : *FString::Join(Changes, TEXT(", "))));
		return true;
	}

	/* A call taken out of its graph, its parameter map passed straight on, so what comes after it
	 * still runs */
	void TakeOutCall(UNiagaraNodeFunctionCall& Call) {
		UEdGraphPin* MapIn = nullptr;
		UEdGraphPin* MapOut = nullptr;
		for (UEdGraphPin* Pin : Call.Pins) {
			if (Pin != nullptr && Call.IsParameterMapPin(Pin)) (Pin->Direction == EGPD_Input ? MapIn : MapOut) = Pin;
		}
		if (MapIn != nullptr && MapOut != nullptr && MapIn->LinkedTo.Num() == 1) {
			UEdGraphPin* Source = MapIn->LinkedTo[0];
			for (UEdGraphPin* Target : TArray<UEdGraphPin*>(MapOut->LinkedTo)) {
				MapOut->BreakLinkTo(Target);
				Source->MakeLinkTo(Target);
			}
		}
		Call.BreakAllNodeLinks();
		if (UEdGraph* Graph = Call.GetGraph()) Graph->RemoveNode(&Call);
	}
#endif

#if UE5_4_BEYOND && WITH_EDITORONLY_DATA
	using FMissingTypeNames = TMap<TPair<const UEdGraphNode*, FName>, FString>;

	/* A pin typed as a class or struct this engine doesn't have: the type object it names didn't
	 * resolve. Nothing else makes a Niagara pin of these categories with no object. */
	bool IsMissingType(const UEdGraphPin* Pin) {
		return Pin != nullptr && Pin->PinType.PinSubCategoryObject == nullptr
			&& (Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryClass || Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryType);
	}

	/* "Class'NiagaraDataInterfaceSocketReader'" -> "NiagaraDataInterfaceSocketReader" */
	FString TypeNameOf(const FJsonObject& Entry) {
		const TSharedPtr<FJsonObject>* Type = nullptr;
		const TSharedPtr<FJsonObject>* Object = nullptr;
		FString Name;
		if (!Entry.TryGetObjectField(TEXT("PinType"), Type) || !(*Type)->TryGetObjectField(TEXT("PinSubCategoryObject"), Object)
			|| !(*Object)->TryGetStringField(TEXT("ObjectName"), Name)) return FString();
		FString Inner;
		if (Name.Split(TEXT("'"), nullptr, &Inner)) Name = Inner.LeftChop(Inner.EndsWith(TEXT("'")) ? 1 : 0);
		return Name;
	}

	void DropPinQuietly(UEdGraphNode& Node, UEdGraphPin* Pin) {
		Pin->BreakAllPinLinks();
		Node.Pins.Remove(Pin);
		UEdGraphNode::DestroyPin(Pin);
	}

	/* What a graph does with a type this engine doesn't have, taken out so the rest of the script
	 * compiles. A data interface or struct added in a later engine (6.0's Socket Reader, say) comes
	 * in as pins and variables of a type that resolves to nothing, and every script that carries
	 * one fails to compile, which stops the whole system, not just the part that used it. So:
	 *   - an assignment (Set Variables) loses the targets of that type, the engine's own way;
	 *   - an input node of that type goes, and what it fed reads its default;
	 *   - a call that takes or makes that type goes, its parameter map passed straight through;
	 *   - any other pin of that type goes from its node, and its variable from the graph.
	 * Everything taken out is reported by type and name, so what the system lost is said. */
	void TakeOutMissingTypes(FUObjectExportContainer& Container, const FMissingTypeNames& Names) {
		TMap<FString, TSet<FString>> Lost;
		const auto Lose = [&Lost](const FString& Type, const FString& What) {
			Lost.FindOrAdd(Type.IsEmpty() ? FString(TEXT("a type this engine doesn't have")) : Type).Add(What);
		};
		const auto NameOf = [&Names](const UEdGraphNode* Node, const UEdGraphPin* Pin) {
			const FString* Found = Names.Find(TPair<const UEdGraphNode*, FName>(Node, Pin->PinName));
			return Found != nullptr ? *Found : FString();
		};

		TArray<UNiagaraGraph*> Graphs;
		for (FUObjectExport* Export : Container.Exports) {
			if (UNiagaraGraph* Graph = Cast<UNiagaraGraph>(Export->Object)) Graphs.AddUnique(Graph);
		}

		/* Assignments first: removing a target makes the node build its function again, and the
		 * pins of that type in what it built before go with it */
		for (UNiagaraGraph* Graph : Graphs) {
			TArray<UNiagaraNodeAssignment*> Assignments;
			Graph->GetNodesOfClass(Assignments);
			for (UNiagaraNodeAssignment* Assignment : Assignments) {
				TArray<FNiagaraVariable> Unknown;
				for (const FNiagaraVariable& Target : Assignment->GetAssignmentTargets()) {
					if (!Target.GetType().IsValid()) Unknown.Add(Target);
				}
				for (const FNiagaraVariable& Target : Unknown) {
					FString Type;
					for (const UEdGraphPin* Pin : Assignment->Pins) {
						if (IsMissingType(Pin) && Pin->PinName.ToString().EndsWith(Target.GetName().ToString())) Type = NameOf(Assignment, Pin);
					}
					Assignment->RemoveParameter(Target);
					Lose(Type, FString::Printf(TEXT("%s set by %s"), *Target.GetName().ToString(), *Assignment->GetFunctionName()));
				}
			}
		}

		for (UNiagaraGraph* Graph : Graphs) {
			bool bChanged = false;
			const TArray<TObjectPtr<UEdGraphNode>> Standing = Graph->Nodes;
			for (UEdGraphNode* Node : Standing) {
				if (Node == nullptr) continue;
				const UEdGraphPin* Missing = nullptr;
				for (const UEdGraphPin* Pin : Node->Pins) {
					if (IsMissingType(Pin)) { Missing = Pin; break; }
				}
				if (Missing == nullptr) continue;
				const FString Type = NameOf(Node, Missing);

				if (UNiagaraNodeInput* Input = Cast<UNiagaraNodeInput>(Node)) {
					Lose(Type, FString::Printf(TEXT("input %s"), *Input->Input.GetName().ToString()));
					Node->BreakAllNodeLinks();
					Graph->RemoveNode(Node);
					bChanged = true;
					continue;
				}

				UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Node);
				if (Call != nullptr && Cast<UNiagaraNodeAssignment>(Node) == nullptr) {
					Lose(Type, FString::Printf(TEXT("call %s"), *Call->GetFunctionName()));
					TakeOutCall(*Call);
					bChanged = true;
					continue;
				}

				for (UEdGraphPin* Pin : TArray<UEdGraphPin*>(Node->Pins)) {
					if (!IsMissingType(Pin)) continue;
					if (!Pin->PinName.IsNone()) Lose(NameOf(Node, Pin), Pin->PinName.ToString());
					DropPinQuietly(*Node, Pin);
					bChanged = true;
				}
			}

			TArray<FNiagaraVariable> Unknown;
			for (const auto& Entry : Graph->GetAllMetaData()) {
				if (!Entry.Key.GetType().IsValid()) Unknown.Add(Entry.Key);
			}
			for (const FNiagaraVariable& Variable : Unknown) {
				Graph->GetAllMetaData().Remove(Variable);
				bChanged = true;
			}
			if (bChanged) Graph->NotifyGraphChanged();
		}

		/* User parameters of that type, which the system exposes and nothing can set */
		for (FUObjectExport* Export : Container.Exports) {
			UNiagaraSystem* System = Cast<UNiagaraSystem>(Export->Object);
			if (System == nullptr) continue;
			TArray<FNiagaraVariable> Parameters;
			System->GetExposedParameters().GetParameters(Parameters);
			for (const FNiagaraVariable& Parameter : Parameters) {
				if (Parameter.GetType().IsValid()) continue;
				System->GetExposedParameters().RemoveParameter(Parameter);
				Lose(FString(), FString::Printf(TEXT("user parameter %s"), *Parameter.GetName().ToString()));
			}
		}

		if (Lost.IsEmpty()) return;
		TArray<FString> Said;
		for (const TPair<FString, TSet<FString>>& Pair : Lost) {
			TArray<FString> What = Pair.Value.Array();
			What.Sort();
			Said.Add(FString::Printf(TEXT("%s (%s)"), *Pair.Key, *FString::Join(What, TEXT(", "))));
		}
		Said.Sort();
		const FString Detail = FString::Join(Said, TEXT("; "));
		UE_LOG(LogReflection, Warning, TEXT("Niagara uses types this engine doesn't have; what used them was taken out so the rest compiles: %s"), *Detail);
		FImportIssues::Report(EImportIssue::MissingClass, TEXT("Niagara uses types this engine doesn't have, so what used them was taken out and the rest compiles"), Detail);
	}
#endif

}

void FNiagaraGraphs::RestoreOutputUsage(UNiagaraScript& Script, bool bInferUsage) {
#if UE5_1_BEYOND && WITH_EDITORONLY_DATA
	const UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script.GetLatestSource());
	if (Source == nullptr || Source->NodeGraph == nullptr) return;

	const ENiagaraScriptUsage ScriptUsage = Script.GetUsage();
	const FGuid ScriptUsageId = Script.GetUsageId();
	const bool bPerInstance = ScriptUsage == ENiagaraScriptUsage::ParticleEventScript || ScriptUsage == ENiagaraScriptUsage::ParticleSimulationStageScript;

	const UNiagaraNodeOutput* Candidate = nullptr;
	const UNiagaraNodeOutput* FunctionOutput = nullptr;
	bool bAmbiguousFunction = false;
	bool bMatched = false;
	bool bIdOnAnyOutput = false;
	int32 Equivalent = 0;
	for (const UEdGraphNode* Node : Source->NodeGraph->Nodes) {
		const UNiagaraNodeOutput* Output = Cast<UNiagaraNodeOutput>(Node);
		if (Output == nullptr) continue;
		const ENiagaraScriptUsage Usage = Output->GetUsage();
		bIdOnAnyOutput |= Output->GetUsageId() == ScriptUsageId;
		if (Usage == ENiagaraScriptUsage::Module || Usage == ENiagaraScriptUsage::Function || Usage == ENiagaraScriptUsage::DynamicInput) {
			bAmbiguousFunction |= FunctionOutput != nullptr;
			FunctionOutput = Output;
		}
		if (!Script.IsEquivalentUsage(Usage)) continue;
		Equivalent++;
		bMatched |= Output->GetUsageId() == ScriptUsageId;
		Candidate = Output;
	}
	if (bMatched) return;

	/* A usage ID that names no output in the graph compiles as a script with no graph behind it.
	 * The compile hash is looked up by usage and ID, so it comes back empty, and a GPU script
	 * then fails a check when its shaders are cached. Read through a layout that doesn't match
	 * the package, the ID is whatever sat in its slot (a version guid one byte along), so the
	 * one the graph does have is put back. */
	if (Equivalent == 1) {
		/* Single-output graphs in older editors often address their output without a usage ID. */
		if (bPerInstance || ScriptUsageId.IsValid()) {
			FImportIssues::Report(EImportIssue::Data, TEXT("Niagara script usage IDs did not match their graphs"),
				FString::Printf(TEXT("Reset to the graph's own output on %s"), *Script.GetName()));
		}
		Script.SetUsageId(Candidate->GetUsageId());
	} else if (Equivalent == 0 && bInferUsage && FunctionOutput != nullptr && !bAmbiguousFunction) {
		/* A scratch script can omit its usage even though its output identifies it as a module. */
		Script.SetUsage(FunctionOutput->GetUsage());
		Script.SetUsageId(FunctionOutput->GetUsageId());
	} else if (Equivalent == 0 && ScriptUsageId.IsValid() && !bIdOnAnyOutput) {
		/* No output of its own, which is what a GPU compute script is: the engine builds its
		 * entry from the spawn and update outputs and always under the empty ID */
		FImportIssues::Report(EImportIssue::Data, TEXT("Niagara script usage IDs did not match their graphs"),
			FString::Printf(TEXT("Cleared on %s, whose usage has no output of its own"), *Script.GetName()));
		Script.SetUsageId(FGuid());
	} else if (Equivalent > 1 && bPerInstance) {
		/* Several event handlers or stages and the ID names none of them: which one this script
		 * is can't be told from the graph, so it is said rather than guessed */
		FImportIssues::Report(EImportIssue::Data, TEXT("A Niagara event or stage script matches no output in its graph"),
			FString::Printf(TEXT("%s names none of the %d outputs of its kind"), *Script.GetName(), Equivalent));
	}
#endif
}

bool FNiagaraGraphs::HasSerializedPins(const FUObjectExportContainer* Container) {
	if (Container == nullptr) return false;

	TSet<FName> GraphPaths;
	for (const FUObjectExport* Export : Container->Exports) {
		if (Export->IsJsonValid() && Export->GetType() == TEXT("NiagaraGraph")) GraphPaths.Add(NiagaraExportPath(Export));
	}

	int32 Missing = 0;
	int32 Nodes = 0;
	int32 IncompleteProperties = 0;
	for (FUObjectExport* Export : Container->Exports) {
		if (!Export->IsJsonValid()) continue;

		const FName Outer = Export->GetOuterTree();
		if (Outer.IsNone() || !GraphPaths.Contains(NiagaraObjectPath(Outer.ToString()))) continue;

		const UClass* Class = Export->GetClass();
		const auto* Pins = PinsOf(Export);
		if (Pins == nullptr && (Class == nullptr || !Class->IsChildOf(UNiagaraNode::StaticClass()))) continue;

		Nodes++;
		if (Pins == nullptr) Missing++;
		bool bIncomplete = false;
		if (Export->JsonObject->TryGetBoolField(TEXT("EditorPropertiesIncomplete"), bIncomplete) && bIncomplete) IncompleteProperties++;
	}

	if (Missing > 0 || Nodes == 0) {
		const FString Detail = Nodes == 0 ? TEXT("No graph nodes were present in the export.")
			: FString::Printf(TEXT("%d of %d node exports lack serialized pins."), Missing, Nodes);
		UE_LOG(LogReflection, Warning, TEXT("Niagara editor graph data could not be read. %s"), *Detail);
		FImportIssues::Report(EImportIssue::Data, TEXT("Niagara editor graph data could not be read"), Detail);
		return false;
	}
	if (IncompleteProperties > 0) {
		FImportIssues::Report(EImportIssue::Data, TEXT("Niagara node properties were partially recovered"),
			FString::Printf(TEXT("The source mappings could only read part of the reflected properties on %d node(s). Their serialized pins were recovered."), IncompleteProperties));
	}
	return true;
}

void FNiagaraGraphs::PrepareExports(FUObjectExportContainer& Container, UObjectSerializer& Serializer) {
	for (FUObjectExport* Export : Container.Exports) {
		const FName Type = Export->GetType();
		if (Type == TEXT("Texture2D")) {
			const TSharedPtr<FJsonObject>* Outer = nullptr;
			FString Owner, Format;
			int32 Width = 0, Height = 0;
			if (Export->JsonObject->TryGetObjectField(TEXT("Outer"), Outer)
				&& (*Outer)->TryGetStringField(TEXT("ObjectName"), Owner)
				&& (Owner.StartsWith(TEXT("NiagaraSystem'")) || Owner.StartsWith(TEXT("NiagaraEmitter'"))
					|| Owner.StartsWith(TEXT("NiagaraStatelessEmitter'")))
				&& Export->JsonObject->TryGetStringField(TEXT("PixelFormat"), Format) && Format == TEXT("PF_Unknown")
				&& Export->JsonObject->TryGetNumberField(TEXT("SizeX"), Width) && Width == 0
				&& Export->JsonObject->TryGetNumberField(TEXT("SizeY"), Height) && Height == 0) {
				// The editor regenerates thumbnails. Empty texture shells cannot be saved as useful assets.
				Serializer.SkippedExports.Add(Export);
			}
			continue;
		}

		const TSharedPtr<FJsonObject>* Properties = nullptr;
		if (!Export->JsonObject->TryGetObjectField(TEXT("Properties"), Properties)) continue;

		// Rebuild uses the serialized graph pins. Loading deprecated pin objects as well
		// would make UEdGraphNode::PostLoad try to migrate their unresolved references.
		if (PinsOf(Export) != nullptr) (*Properties)->RemoveField(TEXT("DeprecatedPins"));

		if (Type == TEXT("NiagaraScript")) {
			const TSharedPtr<FJsonObject>* Outer = nullptr;
			FString Owner;
			if (!(*Properties)->HasField(TEXT("Usage"))
				&& Export->JsonObject->TryGetObjectField(TEXT("Outer"), Outer)
				&& (*Outer)->TryGetStringField(TEXT("ObjectName"), Owner)
				&& Owner.StartsWith(TEXT("NiagaraEmitter'"))) {
				static const TMap<FName, const TCHAR*> Usages = {
					{ TEXT("SpawnScript"), TEXT("ENiagaraScriptUsage::ParticleSpawnScript") },
					{ TEXT("UpdateScript"), TEXT("ENiagaraScriptUsage::ParticleUpdateScript") },
					{ TEXT("EmitterSpawnScript"), TEXT("ENiagaraScriptUsage::EmitterSpawnScript") },
					{ TEXT("EmitterUpdateScript"), TEXT("ENiagaraScriptUsage::EmitterUpdateScript") },
					{ TEXT("GPUComputeScript"), TEXT("ENiagaraScriptUsage::ParticleGPUComputeScript") }
				};
				// Duplicated emitters can export the built-in scripts with numeric suffixes.
				const FName ScriptName(*Export->GetName().GetPlainNameString());
				if (const TCHAR* const* Usage = Usages.Find(ScriptName)) {
					const TCHAR* Recovered = *Usage;
					if (ScriptName == TEXT("SpawnScript")) {
						const FUObjectExport* Emitter = Container.GetExportByObjectPath(*Outer);
						const TArray<TSharedPtr<FJsonValue>>* Versions = nullptr;
						if (Emitter->IsJsonValid() && Emitter->GetProperties()->TryGetArrayField(TEXT("VersionData"), Versions)) {
							for (const TSharedPtr<FJsonValue>& Version : *Versions) {
								const TSharedPtr<FJsonObject>* Data = nullptr;
								const TSharedPtr<FJsonObject>* Spawn = nullptr;
								const TSharedPtr<FJsonObject>* Script = nullptr;
								if (!Version->TryGetObject(Data) || !(*Data)->TryGetObjectField(TEXT("SpawnScriptProps"), Spawn)
									|| !(*Spawn)->TryGetObjectField(TEXT("Script"), Script) || Container.GetExportByObjectPath(*Script) != Export) continue;

								FString Mode;
								bool bInterpolated = false;
								(*Data)->TryGetBoolField(TEXT("bInterpolatedSpawning"), bInterpolated);
								(*Data)->TryGetStringField(TEXT("InterpolatedSpawnMode"), Mode);
								if (bInterpolated || Mode.EndsWith(TEXT("::Interpolation"))) Recovered = TEXT("ENiagaraScriptUsage::ParticleSpawnScriptInterpolated");
								break;
							}
						}
					}
					(*Properties)->SetStringField(TEXT("Usage"), Recovered);
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* Collections = nullptr;
			if (!(*Properties)->TryGetArrayField(TEXT("CachedParameterCollectionReferences"), Collections)) continue;

			const auto Valid = Collections->FilterByPredicate([](const TSharedPtr<FJsonValue>& Entry) {
				if (!Entry.IsValid() || Entry->IsNull()) return false;
				const TSharedPtr<FJsonObject>* Reference = nullptr;
				FString Name, ClassName;
				if (Entry->TryGetObject(Reference) && (*Reference)->TryGetStringField(TEXT("ObjectName"), Name)
					&& Name.Split(TEXT("'"), &ClassName, nullptr)) {
					if (const UClass* Class = FindClassByType(ClassName)) {
						return Class->IsChildOf(UNiagaraParameterCollection::StaticClass());
					}
				}
				return true;
			});
			if (Valid.Num() != Collections->Num()) (*Properties)->SetArrayField(TEXT("CachedParameterCollectionReferences"), Valid);
		} else if (Type.ToString().StartsWith(TEXT("NiagaraDataInterface")) && (*Properties)->HasField(TEXT("ExposedTexture"))) {
			// An unexposed curve has no texture. Some mismatched mappings decode this slot as a graph reference.
			bool bExposeCurve = true;
			if ((*Properties)->TryGetBoolField(TEXT("bExposeCurve"), bExposeCurve) && !bExposeCurve) {
				(*Properties)->RemoveField(TEXT("ExposedTexture"));
			}
		}
	}
}

void FNiagaraGraphs::Rebuild(FUObjectExportContainer* Container, UPackage* Package,
	UPropertySerializer* Serializer, FNiagaraGraphStats& OutStats) {
	if (Container == nullptr) return;

#if WITH_EDITORONLY_DATA
	for (const FUObjectExport* Export : Container->Exports) {
		UNiagaraDataInterfaceCurveBase* Curve = Cast<UNiagaraDataInterfaceCurveBase>(Export->Object);
		if (Curve != nullptr && !Curve->bExposeCurve) Curve->UpdateExposedTexture();
	}
#endif

#if UE5_1_BEYOND && WITH_EDITORONLY_DATA
	/* A source whose reference to its graph did not come across still has that graph under it.
	 *
	 * The graph is made as the source's own subobject, so where exactly one graph sits beneath a
	 * source with no graph named, that one is it. More than one is left alone: which of them the
	 * source used is not something the package says. */
	{
		TMap<UNiagaraScriptSource*, TArray<UNiagaraGraph*>> GraphsBySource;

		for (const FUObjectExport* Export : Container->Exports) {
			UNiagaraGraph* Graph = Cast<UNiagaraGraph>(Export->Object);
			if (Graph == nullptr) continue;

			if (UNiagaraScriptSource* Owner = Cast<UNiagaraScriptSource>(Graph->GetOuter())) {
				GraphsBySource.FindOrAdd(Owner).Add(Graph);
			}
		}

		for (const FUObjectExport* Export : Container->Exports) {
			UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Export->Object);
			if (Source == nullptr || Source->NodeGraph != nullptr) continue;

			const TArray<UNiagaraGraph*>* Beneath = GraphsBySource.Find(Source);
			if (Beneath != nullptr && Beneath->Num() == 1) Source->NodeGraph = (*Beneath)[0];
		}
	}
#endif

	TMap<UEdGraph*, TArray<UEdGraphNode*>> OwnedNodes;
	for (const FUObjectExport* Export : Container->Exports) {
		if (UEdGraphNode* Node = Cast<UEdGraphNode>(Export->Object)) {
			if (UEdGraph* Graph = Cast<UEdGraph>(Node->GetOuter())) OwnedNodes.FindOrAdd(Graph).Add(Node);
		}
	}

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

		if (const TArray<UEdGraphNode*>* Owned = OwnedNodes.Find(Graph)) {
			for (UEdGraphNode* Beneath : *Owned) {
				if (!Placed.Contains(Beneath)) {
					Graph->Nodes.Add(Beneath);
					Placed.Add(Beneath);
				}
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
	TMap<FName, UEdGraphNode*> NodesByPath;
	TMap<TPair<const UEdGraphNode*, FGuid>, UEdGraphPin*> Made;
#if UE5_4_BEYOND && WITH_EDITORONLY_DATA
	FMissingTypeNames MissingTypeNames;
#endif
	auto FindPin = [&Made, &NodesByPath, Container](const UEdGraphNode* From, const TSharedPtr<FJsonValue>& Value) -> UEdGraphPin* {
		const TSharedPtr<FJsonObject>* Reference = nullptr;
		const TSharedPtr<FJsonObject>* Owner = nullptr;
		FString Id;
		if (!Value.IsValid() || !Value->TryGetObject(Reference)
			|| !(*Reference)->TryGetStringField(TEXT("PinId"), Id)
			|| !(*Reference)->TryGetObjectField(TEXT("OwningNode"), Owner)) return nullptr;

		FString Named;
		UEdGraphNode* Node = (*Owner)->TryGetStringField(TEXT("ObjectName"), Named)
			? NodesByPath.FindRef(NiagaraObjectPath(StripObjectOuter(Named))) : nullptr;
		if (Node == nullptr) Node = Cast<UEdGraphNode>(Container->GetExportByObjectPath(*Owner)->Object);
		if (Node == nullptr || Node->GetOuter() != From->GetOuter()) return nullptr;

		return Made.FindRef(TPair<const UEdGraphNode*, FGuid>(Node, StringToGuid(Id)));
	};

	for (FUObjectExport* Export : Container->Exports) {
		UEdGraphNode* Node = Cast<UEdGraphNode>(Export->Object);

		if (Node == nullptr) continue;

		if (!Node->NodeGuid.IsValid()) Node->CreateNewGuid();
		const TArray<TSharedPtr<FJsonValue>>* Listed = PinsOf(Export);
		if (Listed == nullptr) continue;
		NodesByPath.Add(NiagaraExportPath(Export), Node);
		NodesByPath.Add(NiagaraObjectPath(Node->GetPathName(Package)), Node);

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

#if UE5_4_BEYOND && WITH_EDITORONLY_DATA
			if (IsMissingType(Pin)) MissingTypeNames.Add(TPair<const UEdGraphNode*, FName>(Node, Pin->PinName), TypeNameOf(*Entry));
#endif

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
				if (Pin->bAdvancedView && Node->AdvancedPinDisplay == ENodeAdvancedPins::NoPins) {
					Node->AdvancedPinDisplay = ENodeAdvancedPins::Hidden;
				}
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

			Made.Add(TPair<const UEdGraphNode*, FGuid>(Node, Pin->PinId), Pin);

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
	int32 RemovedStaleDefaults = 0;
	for (FUObjectExport* Export : Container->Exports) {
		UEdGraphNode* Node = Cast<UEdGraphNode>(Export->Object);

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
			const bool bDefaultPairs = Named->GetFName() == TEXT("PinOutputToPinDefaultPersistentId");
			TSet<FGuid> OutputsWithoutDefaults;
			if (bDefaultPairs) {
				for (const UEdGraphPin* Pin : Node->Pins) {
					if (Pin != nullptr && Pin->Direction == EGPD_Output && !Pin->bOrphanedPin
						&& Pin->PinName != TEXT("Add")) OutputsWithoutDefaults.Add(Pin->PersistentGuid);
				}
			}
			TArray<int32> Stale;

			for (int32 Index = 0; Index < Held.GetMaxIndex(); Index++) {
				if (!Held.IsValidIndex(Index)) continue;

				const FGuid& One = *reinterpret_cast<const FGuid*>(Held.GetKeyPtr(Index));
				const FGuid& Other = *reinterpret_cast<const FGuid*>(Held.GetValuePtr(Index));

				const bool bKeepsOutput = Kept.Contains(One);
				const bool bKeepsDefault = Kept.Contains(Other);
				if (bKeepsOutput && bKeepsDefault) {
					OutputsWithoutDefaults.Remove(One);
				} else if (bDefaultPairs && !bKeepsOutput && !bKeepsDefault) {
					Stale.Add(Index);
				} else {
					OutStats.Unpaired++;
				}
			}

			// A copied node can keep records for deleted pins. Only remove records with neither end
			// present when all surviving outputs still have their defaults.
			if (OutputsWithoutDefaults.Num() == 0) {
				for (const int32 Index : Stale) Held.RemoveAt(Index);
				RemovedStaleDefaults += Stale.Num();
			} else {
				OutStats.Unpaired += Stale.Num();
			}
		}
	}
	if (RemovedStaleDefaults > 0) {
		UE_LOG(LogReflection, Display, TEXT("Removed %d stale Niagara default-pin records from %s"), RemovedStaleDefaults, *GetNameSafe(Package));
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
	int32 RemappedVersions = 0;
	for (FUObjectExport* Export : Container->Exports) {
		UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Export->Object);

		if (Call == nullptr) continue;

#if UE5_4_BEYOND
		if (UNiagaraScript* Script = Call->FunctionScript; Script != nullptr && Call->SelectedScriptVersion.IsValid()
			&& Script->GetScriptData(Call->SelectedScriptVersion) == nullptr) {
			Call->SelectedScriptVersion = Script->GetExposedVersion().VersionGuid;
			RemappedVersions++;
		}
#endif
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
	if (RemappedVersions > 0) {
		FImportIssues::Report(EImportIssue::Data, TEXT("Niagara calls used the available script version"),
			FString::Printf(TEXT("%d call(s) selected a revision missing from the target editor."), RemappedVersions));
	}
#endif

	/* Joined up once every pin exists, since a link reaches across nodes and there is no order
	 * that has both ends of all of them made first */
	for (FUObjectExport* Export : Container->Exports) {
		const TArray<TSharedPtr<FJsonValue>>* Listed = PinsOf(Export);

		const UEdGraphNode* Node = Cast<UEdGraphNode>(Export->Object);
		if (Node == nullptr || Listed == nullptr) continue;

		for (const TSharedPtr<FJsonValue>& Value : *Listed) {
			const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;

			FString Id;

			if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("PinId"), Id)) continue;

			UEdGraphPin** From = Made.Find(TPair<const UEdGraphNode*, FGuid>(Node, StringToGuid(Id)));

			if (From == nullptr) continue;
			UEdGraphPin* Pin = *From;
			auto Resolve = [&](const TSharedPtr<FJsonValue>& Reference) {
				UEdGraphPin* Resolved = FindPin(Node, Reference);
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
					if (UEdGraphPin* To = Resolve(One); To != nullptr && To != Pin && !Pin->LinkedTo.Contains(To)) {
						Pin->LinkedTo.Add(To);
						To->LinkedTo.AddUnique(Pin);
						OutStats.Links++;
					}
				}
			}
		}
	}

#if UE5_4_BEYOND && WITH_EDITORONLY_DATA
	TArray<FString> TrimmedCalls;
	TArray<FString> UnservedOutputs;
	for (FUObjectExport* Export : Container->Exports) {
		UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Export->Object);
		if (Call == nullptr || Call->FunctionScript == nullptr || Call->FunctionScript->GetOutermost() == Package) continue;
		const FString FunctionPackage = Call->FunctionScript->GetOutermost()->GetName();
		FString Mount;
		if (!FunctionPackage.Mid(1).Split(TEXT("/"), &Mount, nullptr)) continue;
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Mount);
		if (Mount != TEXT("Engine") && (!Plugin.IsValid() || Plugin->GetLoadedFrom() != EPluginLoadedFrom::Engine)) continue;
		if (const UNiagaraGraph* Called = CalledGraphOf(Call)) {
			RestoreAddedFunctionOutputs(Call, Called);
			RestoreLegacyMaterialParameterSwitches(*Call, *Called);

			const TArray<FString> Dropped = DropRemovedFunctionOutputs(Call, Called, UnservedOutputs);
			if (!Dropped.IsEmpty()) {
				TrimmedCalls.AddUnique(FString::Printf(TEXT("%s (%s)"), *GetNameSafe(Call->FunctionScript), *FString::Join(Dropped, TEXT(", "))));
			}
		}
	}
	if (TrimmedCalls.Num() > 0) {
		FImportIssues::Report(EImportIssue::Data, TEXT("Niagara calls to engine functions were fitted to this engine's version"),
			FString::Printf(TEXT("%s: outputs this engine's function doesn't have, and nothing used, were dropped"), *FString::Join(TrimmedCalls, TEXT(", "))));
	}
	if (UnservedOutputs.Num() > 0) {
		FImportIssues::Report(EImportIssue::MissingClass, TEXT("Niagara calls use engine function outputs this engine doesn't have"),
			FString::Printf(TEXT("%s. These calls compile as \"Incorrect number of outputs\""), *FString::Join(UnservedOutputs, TEXT(", "))));
	}
#endif
	FNiagaraWeightedIntArray::RestoreCalls(*Container);

	/* A call that ends up naming neither a script nor a data interface signature compiles as
	 * "Unknown Function Call", and that error fails the whole script it is in, which stops the
	 * system: one missing module takes down every emitter after it on the same stack. Such a call is
	 * taken out (its parameter map passed straight on, so what comes after it still runs) and said
	 * by name, in one of three ways: its export named nothing (the module was never in the game's
	 * files), its properties could not be read (which module it calls is unknown), or it named a
	 * script that could not be imported (the missing-reference report says which). */
#if WITH_EDITORONLY_DATA
	TArray<FString> NamedNothing;
	TArray<FString> UnreadCalls;
	TArray<FString> Unresolved;
	TArray<UNiagaraNodeFunctionCall*> CallsToNothing;
	for (FUObjectExport* Export : Container->Exports) {
		UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Export->Object);
		if (Call == nullptr || Call->GetClass() != UNiagaraNodeFunctionCall::StaticClass() || !Export->IsJsonValid()) continue;
		if (Call->FunctionScript != nullptr || Call->Signature.IsValid()) continue;
		if (Call->GetGraph() == nullptr) continue;

		const TSharedPtr<FJsonObject>* Properties = nullptr;
		const bool bHasProperties = Export->JsonObject->TryGetObjectField(TEXT("Properties"), Properties);

		FString Name;
		if (bHasProperties) (*Properties)->TryGetStringField(TEXT("FunctionDisplayName"), Name);
		if (Name.IsEmpty()) Name = Export->GetName().ToString();

		bool bIncomplete = false;
		Export->JsonObject->TryGetBoolField(TEXT("EditorPropertiesIncomplete"), bIncomplete);
		if (bHasProperties && (*Properties)->HasTypedField<EJson::Object>(TEXT("FunctionScript"))) Unresolved.Add(Name);
		else (bIncomplete || !bHasProperties ? UnreadCalls : NamedNothing).Add(Name);
		CallsToNothing.Add(Call);
	}
#if UE5_4_BEYOND
	for (UNiagaraNodeFunctionCall* Call : CallsToNothing) TakeOutCall(*Call);
	const TCHAR* Outcome = TEXT("so these calls were taken out and the rest of each script compiles; what they fed now reads its default");
#else
	const TCHAR* Outcome = TEXT("so these calls compile as Unknown Function Call");
#endif
	if (NamedNothing.Num() > 0) {
		FImportIssues::Report(EImportIssue::MissingAsset, TEXT("Niagara function calls name no script or data interface"),
			FString::Printf(TEXT("%s. The modules were not in the game's files, %s"), *FString::Join(NamedNothing, TEXT(", ")), Outcome));
	}
	if (UnreadCalls.Num() > 0) {
		FImportIssues::Report(EImportIssue::Data, TEXT("Niagara function calls lost their script reference"),
			FString::Printf(TEXT("%s. The node's properties could not be read from the source, so which module it calls is unknown, %s"),
				*FString::Join(UnreadCalls, TEXT(", ")), Outcome));
	}
	if (Unresolved.Num() > 0) {
		FImportIssues::Report(EImportIssue::MissingAsset, TEXT("Niagara function calls name scripts that could not be imported"),
			FString::Printf(TEXT("%s. See the missing references for which, %s"), *FString::Join(Unresolved, TEXT(", ")), Outcome));
	}
#endif
#if UE5_4_BEYOND && WITH_EDITORONLY_DATA
	{
		FDataInterfaceFit Fit;
		TArray<FString> NoSuchFunction;
		for (FUObjectExport* Export : Container->Exports) {
			UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Export->Object);
			if (Call == nullptr || Call->GetClass() != UNiagaraNodeFunctionCall::StaticClass()) continue;
			bool bNoSuchFunction = false;
			if (FitDataInterfaceCall(*Call, bNoSuchFunction, Fit) || !bNoSuchFunction) continue;
			NoSuchFunction.AddUnique(FString::Printf(TEXT("%s.%s"), *Call->Signature.Inputs[0].GetType().GetName(), *Call->Signature.Name.ToString()));
			TakeOutCall(*Call);
		}
		if (Fit.Changed.Num() > 0) {
			FImportIssues::Report(EImportIssue::Data, TEXT("Niagara calls to data interface functions were fitted to this engine's version"), FString::Join(Fit.Changed, TEXT(", ")));
		}
		if (Fit.LostWired.Num() > 0) {
			FImportIssues::Report(EImportIssue::MissingClass, TEXT("Niagara graphs used data interface pins this engine's functions don't have"),
				FString::Printf(TEXT("%s. What they fed now reads its default"), *FString::Join(Fit.LostWired, TEXT(", "))));
		}
		if (NoSuchFunction.Num() > 0) {
			FImportIssues::Report(EImportIssue::MissingClass, TEXT("Niagara calls data interface functions this engine doesn't have, so those calls were taken out and the rest compiles"),
				FString::Printf(TEXT("%s. What they fed now reads its default"), *FString::Join(NoSuchFunction, TEXT(", "))));
		}
	}
	TakeOutMissingTypes(*Container, MissingTypeNames);
#endif
}

void FNiagaraGraphs::ReportMissingScriptVersions(const FUObjectExportContainer& Container) {
	/* The version table is editor-only data. Read without a layout for it (no editor schema for
	 * the build the asset came from), every script comes across without one and is given the
	 * default revision: a new version guid, and no category, description, module usage, input
	 * metadata or last compile ID. Its graph is unaffected. */
	int32 Without = 0;
	FString First;
	for (const FUObjectExport* Export : Container.Exports) {
		if (!Export->IsJsonValid() || Export->GetType() != TEXT("NiagaraScript")) continue;
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		if (Export->JsonObject->TryGetObjectField(TEXT("Properties"), Properties) && (*Properties)->HasField(TEXT("VersionData"))) continue;
		if (Without++ == 0) First = Export->GetName().ToString();
	}
	if (Without == 0) return;

	FImportIssues::Report(EImportIssue::Data, TEXT("Niagara script version data was not exported"),
		FString::Printf(TEXT("%d script(s), starting with %s, use the default revision: version guid, category, description, module usage and input metadata are lost"),
			Without, *First));
}
