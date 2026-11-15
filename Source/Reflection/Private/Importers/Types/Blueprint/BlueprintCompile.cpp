/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/BlueprintCompile.h"

#include "Engine/EngineUtilities.h"
#include "Importers/Constructor/ImportIssues.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_MultiGate.h"
#include "K2Node_AssignmentStatement.h"
#include "K2Node_TemporaryVariable.h"
#include "Kismet/KismetNodeHelperLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FeedbackContext.h"
#include "Misc/SlowTask.h"

namespace {
	/* Where the slow tasks were before, and what to drop back to afterwards.
	 *
	 * A slow task puts itself on a stack the feedback context holds and takes itself off again when
	 * it goes out of scope. Coming back out through the guard is not going out of scope, so the
	 * compiler's own are left on the stack pointing at frames that no longer exist, and the next
	 * slow task to end reads one of them by name. Anything pushed past where the compile started is
	 * dropped rather than left there. */
	int32 SlowTasks() {
		return GWarn != nullptr ? GWarn->GetScopeStack().Num() : 0;
	}

	void DropSlowTasksTo(const int32 Depth) {
		if (GWarn == nullptr) return;

		FSlowTaskStack& Stack = const_cast<FSlowTaskStack&>(GWarn->GetScopeStack());

		if (Stack.Num() > Depth) {
			Stack.SetNum(Depth);
		}
	}

	/* Nothing of its own to unwind, which is what lets the guard sit around it. Everything the
	 * compiler makes is made further in, where the compiler is the one that owns it. */
	bool Attempt(UBlueprint* Blueprint, const EBlueprintCompileOptions Options) {
		/* PROBE: -compileunguarded lets a failing compile crash with its whole call stack */
		if (FParse::Param(FCommandLine::Get(), TEXT("compileunguarded"))) {
			FKismetEditorUtilities::CompileBlueprint(Blueprint, Options);

			return true;
		}

#if PLATFORM_WINDOWS && !PLATFORM_SEH_EXCEPTIONS_DISABLED
		__try {
			FKismetEditorUtilities::CompileBlueprint(Blueprint, Options);

			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#else
		FKismetEditorUtilities::CompileBlueprint(Blueprint, Options);

		return true;
#endif
	}

	/* Same shape, and for the same reason: making one runs the compiler over it before it comes
	 * back, so the making is no safer than the compiling. */
	UBlueprint* AttemptCreate(UClass* ParentClass, UObject* Outer, const FName Name,
		const EBlueprintType BlueprintType, UClass* BlueprintClassType, UClass* BlueprintGeneratedClassType) {
#if PLATFORM_WINDOWS && !PLATFORM_SEH_EXCEPTIONS_DISABLED
		__try {
			return FKismetEditorUtilities::CreateBlueprint(ParentClass, Outer, Name, BlueprintType,
				BlueprintClassType, BlueprintGeneratedClassType);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
#else
		return FKismetEditorUtilities::CreateBlueprint(ParentClass, Outer, Name, BlueprintType,
			BlueprintClassType, BlueprintGeneratedClassType);
#endif
	}
}

/* PROBE: every object property in the package holding a value no live object could have */
static void ProbeBadPointers(const UStruct* Struct, const void* Container, const FString& Path, int32 Depth) {
	if (Depth > 6 || Struct == nullptr || Container == nullptr) return;

	for (TFieldIterator<FProperty> It(Struct); It; ++It) {
		const FProperty* Property = *It;

		for (int32 Index = 0; Index < Property->ArrayDim; ++Index) {
			const void* Value = Property->ContainerPtrToValuePtr<void>(Container, Index);
			const FString Here = Path + TEXT(".") + Property->GetName();

			if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Property); Object && !Object->IsA<FWeakObjectProperty>() && !Object->IsA<FLazyObjectProperty>() && !Object->IsA<FSoftObjectProperty>()) {
				const UPTRINT Raw = *reinterpret_cast<const UPTRINT*>(Value);

				if (Raw != 0 && ((Raw & 7) != 0 || Raw < 0x10000 || (Raw >> 48) != 0 || !GUObjectArray.IsValid(reinterpret_cast<const UObjectBase*>(Raw)))) {
					UE_LOG(LogReflection, Warning, TEXT("PROBE bad pointer 0x%llx at %s (%s)"), (unsigned long long)Raw, *Here, *Property->GetCPPType());
				}
			} else if (const FStructProperty* Inner = CastField<FStructProperty>(Property)) {
				ProbeBadPointers(Inner->Struct, Value, Here, Depth + 1);
			} else if (const FArrayProperty* Array = CastField<FArrayProperty>(Property)) {
				FScriptArrayHelper Helper(Array, Value);

				for (int32 Element = 0; Element < Helper.Num(); ++Element) {
					if (const FStructProperty* ElementStruct = CastField<FStructProperty>(Array->Inner)) {
						ProbeBadPointers(ElementStruct->Struct, Helper.GetRawPtr(Element), FString::Printf(TEXT("%s[%d]"), *Here, Element), Depth + 1);
					} else if (CastField<FObjectProperty>(Array->Inner)) {
						const UPTRINT Raw = *reinterpret_cast<const UPTRINT*>(Helper.GetRawPtr(Element));

						if (Raw != 0 && ((Raw & 7) != 0 || Raw < 0x10000 || (Raw >> 48) != 0)) {
							UE_LOG(LogReflection, Warning, TEXT("PROBE bad pointer 0x%llx at %s[%d]"), (unsigned long long)Raw, *Here, Element);
						}
					}
				}
			}
		}
	}
}

/* An event a Create Event names, given the widths the delegate it is bound to asks for.
 *
 * Every real number a blueprint declares is drawn as a double, which is what the engine turns a
 * float into when it loads an older blueprint. An event that answers a delegate the engine itself
 * declares is the exception: the delegate says float, and an event drawn with a double there is
 * only bound through a function the compiler writes in between. Drawn as a float it is the event
 * somebody gets by dragging off the delegate, and it is bound as it stands. */
static void SettleDelegateEventFloats(UBlueprint* Blueprint) {
#if ENGINE_UE5
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);

	for (UEdGraph* Graph : Graphs) {
		if (Graph == nullptr) continue;

		TArray<UK2Node_CreateDelegate*> Makers;
		Graph->GetNodesOfClass<UK2Node_CreateDelegate>(Makers);

		for (const UK2Node_CreateDelegate* Maker : Makers) {
			const UFunction* Signature = Maker != nullptr ? Maker->GetDelegateSignature() : nullptr;
			const FName Bound = Maker != nullptr ? Maker->GetFunctionName() : NAME_None;

			if (Signature == nullptr || Bound.IsNone()) continue;

			UK2Node_CustomEvent* Event = nullptr;

			for (UEdGraph* Held : Graphs) {
				if (Held == nullptr) continue;

				TArray<UK2Node_CustomEvent*> Events;
				Held->GetNodesOfClass<UK2Node_CustomEvent>(Events);

				for (UK2Node_CustomEvent* Candidate : Events) {
					if (Candidate != nullptr && Candidate->CustomFunctionName == Bound) {
						Event = Candidate;

						break;
					}
				}

				if (Event != nullptr) break;
			}

			if (Event == nullptr) continue;

			bool bChanged = false;
			int32 Index = 0;

			for (TFieldIterator<FProperty> It(Signature); It && (It->PropertyFlags & CPF_Parm); ++It) {
				if (It->PropertyFlags & CPF_ReturnParm) continue;

				const int32 At = Index++;

				if (!Event->UserDefinedPins.IsValidIndex(At)) continue;

				const TSharedPtr<FUserPinInfo>& Pin = Event->UserDefinedPins[At];

				if (!Pin.IsValid()) continue;

				/* Handed over by reference where the delegate hands it by reference.
				 *
				 * The script an event was compiled to does not say which of its parameters were
				 * references, and the compiler will not bind an event whose parameters are passed
				 * differently from the delegate's: it takes the difference for one it can bridge and
				 * writes a function in between, which is not what the game had. */
				const bool bByReference = It->HasAnyPropertyFlags(CPF_ReferenceParm);
				const bool bConstant = bByReference && It->HasAnyPropertyFlags(CPF_ConstParm);

				if (Pin->PinType.bIsReference != bByReference || Pin->PinType.bIsConst != bConstant) {
					Pin->PinType.bIsReference = bByReference;
					Pin->PinType.bIsConst = bConstant;

					bChanged = true;
				}

				if (!It->IsA<FFloatProperty>()) continue;
				if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Real) continue;
				if (Pin->PinType.PinSubCategory == UEdGraphSchema_K2::PC_Float) continue;
				if (Pin->PinType.ContainerType != EPinContainerType::None) continue;

				Pin->PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;

				bChanged = true;
			}

			if (bChanged) {
				Event->ReconstructNode();

				UE_LOG(LogReflection, Display, TEXT("event \"%s\" takes its parameters the way \"%s\" hands them"), *Bound.ToString(), *Signature->GetName());
			}
		}
	}
#endif
}

namespace {
	/* A call to one of the functions a MultiGate is compiled into */
	const UK2Node_CallFunction* GateHelper(const UEdGraphNode* Node, const TCHAR* Function) {
		const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);

		if (Call == nullptr || Call->FunctionReference.GetMemberName() != FName(Function)) return nullptr;

		const UFunction* Target = Call->GetTargetFunction();

		return Target != nullptr && Target->GetOuterUClass() == UKismetNodeHelperLibrary::StaticClass() ? Call : nullptr;
	}

	UEdGraphNode* FedBy(const UEdGraphPin* Pin) {
		return Pin != nullptr && Pin->LinkedTo.Num() == 1 && Pin->LinkedTo[0] != nullptr ? Pin->LinkedTo[0]->GetOwningNode() : nullptr;
	}

	/* What a pin of the gate is set to, taken from the pin of the compiled call that carried it */
	void CarryOver(const UEdGraphPin* From, UEdGraphPin* Onto) {
		if (From == nullptr || Onto == nullptr) return;

		if (From->LinkedTo.Num() > 0) {
			for (UEdGraphPin* Source : TArray<UEdGraphPin*>(From->LinkedTo)) {
				if (Source != nullptr) Onto->MakeLinkTo(Source);
			}
		} else if (!From->DefaultValue.IsEmpty()) {
			Onto->DefaultValue = From->DefaultValue;
		}
	}
}

/* A MultiGate, put back as the one node it was.
 *
 * A MultiGate has no script of its own: the compiler writes it out as a run of calls that keep a
 * bit for each way out (is one left, pick one, mark it), a test for each way out against the one
 * picked, and a warning if none matched. Read back statement by statement that is a dozen nodes
 * nobody drew, and one that cannot be drawn at all: the bits are kept in a local every one of the
 * calls reads and writes, where a graph can only hand a value forward. Wired the only way the pins
 * allow, the calls feed each other in a circle and the compiler refuses to order them.
 *
 * So the run is recognised by its shape and replaced by the node, with what led into it leading
 * into the node and each test's way out leaving from the node's. */
static void SettleMultiGates(UBlueprint* Blueprint) {
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);

	for (UEdGraph* Graph : Graphs) {
		if (Graph == nullptr) continue;

		bool bFound = true;

		while (bFound) {
			bFound = false;

			for (UEdGraphNode* Node : TArray<UEdGraphNode*>(Graph->Nodes)) {
				const UK2Node_CallFunction* Marks = GateHelper(Node, TEXT("MarkBit"));

				if (Marks == nullptr) continue;

				/* The pick that is marked, and the test for whether any is left that guards it */
				const UK2Node_CallFunction* Picks = GateHelper(FedBy(Marks->FindPin(TEXT("Index"), EGPD_Input)), TEXT("GetUnmarkedBit"));

				if (Picks == nullptr || Picks->GetExecPin() == nullptr) continue;

				UK2Node_IfThenElse* AnyLeft = nullptr;
				const UK2Node_CallFunction* Tests = nullptr;

				for (const UEdGraphPin* Before : Picks->GetExecPin()->LinkedTo) {
					UK2Node_IfThenElse* Branch = Before != nullptr ? Cast<UK2Node_IfThenElse>(Before->GetOwningNode()) : nullptr;

					if (Branch == nullptr) continue;

					if (const UK2Node_CallFunction* Asked = GateHelper(FedBy(Branch->GetConditionPin()), TEXT("HasUnmarkedBit"))) {
						AnyLeft = Branch;
						Tests = Asked;

						break;
					}
				}

				if (AnyLeft == nullptr || Tests->GetExecPin() == nullptr) continue;

				/* Where the run begins: the test for the first time through, which leads to the rest */
				UK2Node_IfThenElse* First = nullptr;

				for (const UEdGraphPin* Before : Tests->GetExecPin()->LinkedTo) {
					UK2Node_IfThenElse* Branch = Before != nullptr ? Cast<UK2Node_IfThenElse>(Before->GetOwningNode()) : nullptr;

					if (Branch != nullptr && Before == Branch->GetElsePin()) {
						First = Branch;

						break;
					}
				}

				if (First == nullptr) continue;

				/* The ways out, in order: a test of the pick against each number in turn */
				TArray<UK2Node_IfThenElse*> Outs;

				UEdGraphNode* Next = FedBy(Marks->GetThenPin());

				while (UK2Node_IfThenElse* Branch = Cast<UK2Node_IfThenElse>(Next)) {
					const UK2Node_CallFunction* Equal = Cast<UK2Node_CallFunction>(FedBy(Branch->GetConditionPin()));

					if (Equal == nullptr || Equal->FunctionReference.GetMemberName() != GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, EqualEqual_IntInt)) break;
					if (FedBy(Equal->FindPin(TEXT("A"), EGPD_Input)) != Picks) break;

					Outs.Add(Branch);

					Next = FedBy(Branch->GetElsePin());
				}

				if (Outs.Num() == 0) continue;

				/* Everything the run is made of, walked from where it begins. A way out is where it ends,
				 * and anything met on the way that the compiler would not have written means this is not
				 * the run it looks like, so it is left as it was read. */
				TSet<UEdGraphNode*> Run;
				TArray<UEdGraphNode*> Walk;

				Walk.Add(First);

				bool bOnlyGate = true;

				while (Walk.Num() > 0 && bOnlyGate) {
					UEdGraphNode* At = Walk.Pop();

					if (Run.Contains(At)) continue;

					const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(At);
					const bool bWarns = Call != nullptr
						&& (Call->FunctionReference.GetMemberName() == TEXT("PrintWarning") || Call->FunctionReference.GetMemberName() == TEXT("PrintString"));
					const bool bHelper = Call != nullptr && Call->GetTargetFunction() != nullptr
						&& Call->GetTargetFunction()->GetOuterUClass() == UKismetNodeHelperLibrary::StaticClass();

					if (!At->IsA<UK2Node_IfThenElse>() && !bHelper && !bWarns) {
						bOnlyGate = false;

						break;
					}

					Run.Add(At);

					/* The warning that no way out matched is the last thing the gate does. Where the script
					 * goes from there is wherever it would have gone had the gate done nothing, which is
					 * not the gate's to keep. */
					if (bWarns) continue;

					const bool bWayOut = Outs.Contains(Cast<UK2Node_IfThenElse>(At));

					for (UEdGraphPin* Pin : At->Pins) {
						if (Pin == nullptr || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;

						/* Past here is whatever the gate leads to, which is not the gate */
						if (bWayOut && Pin == Cast<UK2Node_IfThenElse>(At)->GetThenPin()) continue;

						for (const UEdGraphPin* To : Pin->LinkedTo) {
							if (To != nullptr) Walk.Add(To->GetOwningNode());
						}
					}
				}

				if (!bOnlyGate || !Run.Contains(Marks) || !Run.Contains(AnyLeft)) continue;

				/* And what the tests were worked out with, which nothing else reads */
				for (UEdGraphNode* Held : Run.Array()) {
					for (const UEdGraphPin* Pin : Held->Pins) {
						if (Pin == nullptr || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;

						UEdGraphNode* Source = FedBy(Pin);
						const UK2Node* Pure = Cast<UK2Node>(Source);

						if (Pure == nullptr || !Pure->IsNodePure() || Run.Contains(Source)) continue;

						bool bOnlyHere = true;

						for (const UEdGraphPin* Out : Source->Pins) {
							if (Out == nullptr || Out->Direction != EGPD_Output) continue;

							for (const UEdGraphPin* Reader : Out->LinkedTo) {
								if (Reader != nullptr && !Run.Contains(Reader->GetOwningNode())) bOnlyHere = false;
							}
						}

						if (bOnlyHere) Run.Add(Source);
					}
				}

				UK2Node_MultiGate* Gate = NewObject<UK2Node_MultiGate>(Graph, NAME_None, RF_Transactional);

				Gate->CreateNewGuid();
				Gate->NodePosX = First->NodePosX;
				Gate->NodePosY = First->NodePosY;

				Graph->AddNode(Gate, false, false);

				Gate->AllocateDefaultPins();

				/* Its ways out, found by the names it gives them: the node keeps the functions that
				 * would hand them over to itself */
				const auto WaysOut = [Gate]() {
					TArray<UEdGraphPin*> Found;

					for (int32 Nth = 0; ; Nth++) {
						UEdGraphPin* Way = Gate->FindPin(FName(*FString::Printf(TEXT("Out %d"), Nth)), EGPD_Output);

						if (Way == nullptr) break;

						Found.Add(Way);
					}

					return Found;
				};

				TArray<UEdGraphPin*> Ways = WaysOut();

				while (Ways.Num() < Outs.Num() && Gate->CanAddPin()) {
					const int32 Had = Ways.Num();

					Gate->AddInputPin();

					Ways = WaysOut();

					if (Ways.Num() == Had) break;
				}

				if (Ways.Num() < Outs.Num()) {
					Graph->RemoveNode(Gate);

					continue;
				}

				/* What it was set to: random and where to start are said to the pick, and whether it
				 * goes round again is the test in front of clearing the bits */
				CarryOver(Picks->FindPin(TEXT("bRandom"), EGPD_Input), Gate->FindPin(TEXT("IsRandom"), EGPD_Input));
				CarryOver(Picks->FindPin(TEXT("StartIdx"), EGPD_Input), Gate->FindPin(TEXT("StartIndex"), EGPD_Input));

				if (UK2Node_IfThenElse* Again = Cast<UK2Node_IfThenElse>(FedBy(AnyLeft->GetElsePin())); Again != nullptr && GateHelper(FedBy(Again->GetThenPin()), TEXT("ClearAllBits")) != nullptr) {
					CarryOver(Again->GetConditionPin(), Gate->FindPin(TEXT("Loop"), EGPD_Input));
				} else if (UEdGraphPin* Loops = Gate->FindPin(TEXT("Loop"), EGPD_Input)) {
					Loops->DefaultValue = TEXT("false");
				}

				for (UEdGraphPin* Before : TArray<UEdGraphPin*>(First->GetExecPin()->LinkedTo)) {
					if (Before != nullptr && !Run.Contains(Before->GetOwningNode())) Before->MakeLinkTo(Gate->GetExecPin());
				}

				for (int32 Index = 0; Index < Outs.Num(); Index++) {
					for (UEdGraphPin* After : TArray<UEdGraphPin*>(Outs[Index]->GetThenPin()->LinkedTo)) {
						if (After != nullptr) Ways[Index]->MakeLinkTo(After);
					}
				}

				for (UEdGraphNode* Held : Run) {
					Held->BreakAllNodeLinks();

					Graph->RemoveNode(Held);
				}

				UE_LOG(LogReflection, Display, TEXT("%d node(s) in \"%s\" were a MultiGate with %d way(s) out, and are that node again"), Run.Num(), *Graph->GetName(), Outs.Num());

				bFound = true;

				break;
			}
		}
	}
}

/* A way into a node that has one for each entry of an enum, entered by that way.
 *
 * A function that takes an enum can ask for a way in per entry instead of a pin for the enum.
 * Compiled, each of those ways in is a write of its entry to a scratch variable and then the one
 * call, so read back it is an Assign and a Local the graph never had, both leading into whichever
 * way in happens to come first: every entry comes out as the first one. The Assign says which entry
 * it stood for, so whatever led into it is led into the way in of that name and the Assign is gone. */
static void SettleEnumExecs(UBlueprint* Blueprint) {
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);

	for (UEdGraph* Graph : Graphs) {
		if (Graph == nullptr) continue;

		TSet<UK2Node_TemporaryVariable*> Scratch;
		int32 Settled = 0;

		for (UEdGraphNode* Node : TArray<UEdGraphNode*>(Graph->Nodes)) {
			UK2Node_AssignmentStatement* Assign = Cast<UK2Node_AssignmentStatement>(Node);

			if (Assign == nullptr || Assign->GetVariablePin() == nullptr || Assign->GetValuePin() == nullptr || Assign->GetThenPin() == nullptr) continue;

			UK2Node_TemporaryVariable* Local = Cast<UK2Node_TemporaryVariable>(FedBy(Assign->GetVariablePin()));
			const UEnum* Enum = Local != nullptr ? Cast<UEnum>(Local->VariableType.PinSubCategoryObject.Get()) : nullptr;

			if (Enum == nullptr || Assign->GetValuePin()->LinkedTo.Num() > 0 || Assign->GetThenPin()->LinkedTo.Num() != 1) continue;

			/* What it leads into: a way in of a call that spreads this enum over its ways in */
			UEdGraphPin* Into = Assign->GetThenPin()->LinkedTo[0];
			UK2Node_CallFunction* Call = Into != nullptr ? Cast<UK2Node_CallFunction>(Into->GetOwningNode()) : nullptr;
			const UFunction* Function = Call != nullptr ? Call->GetTargetFunction() : nullptr;

			if (Function == nullptr || !Function->HasMetaData(FBlueprintMetadata::MD_ExpandEnumAsExecs)) continue;

			const FString Entry = Assign->GetValuePin()->DefaultValue;

			if (Entry.IsEmpty() || Enum->GetIndexByNameString(Entry) == INDEX_NONE) continue;

			UEdGraphPin* Way = Call->FindPin(*Entry, EGPD_Input);

			if (Way == nullptr || Way->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;

			for (UEdGraphPin* Before : TArray<UEdGraphPin*>(Assign->GetExecPin()->LinkedTo)) {
				if (Before != nullptr) Before->MakeLinkTo(Way);
			}

			Scratch.Add(Local);

			Assign->BreakAllNodeLinks();

			Graph->RemoveNode(Assign);

			Settled++;
		}

		for (UK2Node_TemporaryVariable* Local : Scratch) {
			if (Local == nullptr || Local->GetVariablePin() == nullptr || Local->GetVariablePin()->LinkedTo.Num() > 0) continue;

			Graph->RemoveNode(Local);
		}

		if (Settled > 0) {
			UE_LOG(LogReflection, Display, TEXT("%d way(s) into a node in \"%s\" are entered by the entry they stood for"), Settled, *Graph->GetName());
		}
	}
}

/* Where a gameplay effect keeps the shape its settings were last saved in.
 *
 * The engine holds it in a struct of one byte that it reads and writes itself, and deliberately gives
 * no property for, so there is nothing to ask for by name inside the struct: the byte is the struct.
 * Nothing is returned for a struct of any other size, since that would be an engine this was not
 * written against. */
static uint8* EffectVersionAt(const UClass* Class, UObject* Defaults) {
	const FStructProperty* Version = FindFProperty<FStructProperty>(Class, TEXT("DataVersion"));

	if (Version == nullptr || Version->Struct == nullptr || Version->Struct->GetStructureSize() != sizeof(uint8)) return nullptr;

	return Version->ContainerPtrToValuePtr<uint8>(Defaults);
}

/* The newest shape the engine will bring an effect up to, which is two unless it has been told less */
static uint8 NewestEffectVersion() {
	const IConsoleVariable* Most = IConsoleManager::Get().FindConsoleVariable(TEXT("AbilitySystem.GameplayEffects.MaxVersion"));

	return Most != nullptr ? static_cast<uint8>(FMath::Clamp(Most->GetInt(), 0, 255)) : 2;
}

/* The parts of a gameplay effect, named so a child finds its parent's.
 *
 * This engine keeps what an effect does in components, and makes them from the older settings the
 * first time the effect is compiled. A child's are matched to its parent's by name, and a component
 * made that way is named by whatever number came up. Bring the parent in again and its component has
 * a new name, the child still holds the old one, and loading the child copies the parent's in beside
 * it: two of the same kind, which the effect refuses. Named for the kind of component alone, the name
 * is the same every time either of them is brought in.
 *
 * One the parent already has keeps the parent's name, since that name is the match.
 *
 * Asked for by name, since this has no business knowing the abilities module is there. */
static void SettleEffectComponents(const UBlueprint* Blueprint) {
	UClass* Class = Blueprint != nullptr ? Blueprint->GeneratedClass.Get() : nullptr;

	if (Class == nullptr) return;

	bool bEffect = false;

	for (const UClass* Above = Class; Above != nullptr && !bEffect; Above = Above->GetSuperClass()) {
		bEffect = Above->GetFName() == TEXT("GameplayEffect") && Above->GetOutermost()->GetFName() == TEXT("/Script/GameplayAbilities");
	}

	UObject* Defaults = bEffect ? Class->GetDefaultObject(false) : nullptr;
	const FArrayProperty* Parts = bEffect ? FindFProperty<FArrayProperty>(Class, TEXT("GEComponents")) : nullptr;
	const FObjectPropertyBase* Part = Parts != nullptr ? CastField<FObjectPropertyBase>(Parts->Inner) : nullptr;

	if (Defaults == nullptr || Part == nullptr) return;

	/* Made out of the old settings, whatever the effect says of itself.
	 *
	 * What was read out of the game is the old settings. The engine turns those into components when
	 * the effect is compiled, but only for an effect that says it is still the old shape, and one whose
	 * package has never been saved starts out saying it is the new one: nothing would be made of its
	 * settings at all, and saving it would then clear them. Said to be old and asked again, it is made
	 * the same way every time. An effect that already has its components is only written to again. */
	uint8* Shape = EffectVersionAt(Class, Defaults);

	if (FParse::Param(FCommandLine::Get(), TEXT("effectparts"))) {
		FScriptArrayHelper Now(Parts, Parts->ContainerPtrToValuePtr<void>(Defaults));

		FString Names;

		for (int32 Index = 0; Index < Now.Num(); Index++) {
			Names += GetNameSafe(Part->GetObjectPropertyValue(Now.GetRawPtr(Index))) + TEXT(" ");
		}

		UE_LOG(LogReflection, Display, TEXT("\"%s\" says it is shape %d going in and holds: %s"), *Blueprint->GetName(), Shape != nullptr ? *Shape : -1, *Names);
	}

	if (Shape != nullptr) {
		*Shape = 0;

		Defaults->PostCDOCompiled(UObject::FPostCDOCompiledContext());
	}

	UObject* Parent = Defaults->GetArchetype();

	/* What the parent keeps, by kind: the name each of those goes by is the name the child's has to */
	TMap<const UClass*, FName> Above;

	if (Parent != nullptr && Parent->GetClass()->IsChildOf(Parts->GetOwnerClass())) {
		FScriptArrayHelper Theirs(Parts, Parts->ContainerPtrToValuePtr<void>(Parent));

		for (int32 Index = 0; Index < Theirs.Num(); Index++) {
			if (const UObject* Component = Part->GetObjectPropertyValue(Theirs.GetRawPtr(Index))) {
				if (!Above.Contains(Component->GetClass())) Above.Add(Component->GetClass(), Component->GetFName());
			}
		}
	}

	FScriptArrayHelper Held(Parts, Parts->ContainerPtrToValuePtr<void>(Defaults));

	const ERenameFlags Quietly = REN_DontCreateRedirectors | REN_NonTransactional | REN_ForceNoResetLoaders;

	/* One of a kind.
	 *
	 * A child that was already loaded when its parent was brought in again is left holding the
	 * component the parent had before, and compiling it copies the parent's new one in beside that.
	 * The first of a kind is the one the engine writes the effect's settings to and reads them back
	 * from, every time it is compiled, so that is the one kept, and it takes the parent's name below. */
	TMap<const UClass*, TArray<int32>> Kinds;

	for (int32 Index = 0; Index < Held.Num(); Index++) {
		if (const UObject* Component = Part->GetObjectPropertyValue(Held.GetRawPtr(Index))) {
			Kinds.FindOrAdd(Component->GetClass()).Add(Index);
		}
	}

	TArray<int32> Dropped;

	for (const TPair<const UClass*, TArray<int32>>& Kind : Kinds) {
		if (Kind.Value.Num() < 2) continue;

		const int32 Kept = Kind.Value[0];

		for (const int32 Index : Kind.Value) {
			if (Index != Kept) Dropped.Add(Index);
		}
	}

	Dropped.Sort([](const int32 A, const int32 B) { return A > B; });

	for (const int32 Index : Dropped) {
		UObject* Component = Part->GetObjectPropertyValue(Held.GetRawPtr(Index));

		Held.RemoveValues(Index, 1);

		if (Component != nullptr && Component->GetOuter() == Defaults) {
			Component->Rename(nullptr, GetTransientPackage(), Quietly);
		}
	}

	if (Dropped.Num() > 0) {
		UE_LOG(LogReflection, Display, TEXT("\"%s\" held %d component(s) of a kind it already had, and holds one of each again"), *Blueprint->GetName(), Dropped.Num());
	}

	for (int32 Index = 0; Index < Held.Num(); Index++) {
		UObject* Component = Part->GetObjectPropertyValue(Held.GetRawPtr(Index));

		if (Component == nullptr || Component->GetOuter() != Defaults) continue;

		/* The parent's name for it where the parent has one, and the kind of component where not */
		const FName* Theirs = Above.Find(Component->GetClass());
		const FName Wanted = Theirs != nullptr ? *Theirs : Component->GetClass()->GetFName();

		if (Component->GetFName() == Wanted) continue;

		/* Whatever was left behind under that name is nothing the effect holds any more */
		if (UObject* Stale = StaticFindObjectFast(nullptr, Defaults, Wanted)) {
			Stale->Rename(nullptr, GetTransientPackage(), Quietly);
		}

		Component->Rename(*Wanted.ToString(), Defaults, Quietly);

		UE_LOG(LogReflection, Display, TEXT("\"%s\" keeps its %s under the name \"%s\""), *Blueprint->GetName(), *Component->GetClass()->GetName(), *Wanted.ToString());
	}

	/* And said to be as new as it is.
	 *
	 * An effect remembers which shape its settings were last saved in, and the engine only marks one
	 * as brought up to date once it has been loaded from disk. One made here has never been loaded, so
	 * it would be saved still saying it is the old shape, and every load and every compile after that
	 * would make its components out of the old settings all over again, beside the ones it has. */
	if (Shape != nullptr) *Shape = NewestEffectVersion();
}

bool CompileBlueprintGuarded(UBlueprint* Blueprint, const EBlueprintCompileOptions Options) {
	if (Blueprint == nullptr) return false;

	SettleEnumExecs(Blueprint);

	SettleMultiGates(Blueprint);

	SettleDelegateEventFloats(Blueprint);

	const FString Named = Blueprint->GetName();

	/* PROBE */
	if (FParse::Param(FCommandLine::Get(), TEXT("compileunguarded"))) {
		TArray<UObject*> Held;
		GetObjectsWithPackage(Blueprint->GetOutermost(), Held, true);

		for (UObject* Object : Held) {
			if (Object != nullptr) ProbeBadPointers(Object->GetClass(), Object, Object->GetPathName(), 0);
		}

		UE_LOG(LogReflection, Warning, TEXT("PROBE walked %d object(s) before compiling %s"), Held.Num(), *Named);
	}

	const int32 Depth = SlowTasks();

	/* Before the compile as well as after.
	 *
	 * Compiling an effect compiles every effect made from it in the same breath, and each of those
	 * takes a copy of whatever components its parent holds at that moment, by name. A parent whose
	 * components were only just made out of its settings holds them under whatever numbered names came
	 * up, the children copy those in beside the ones they already had under the proper names, and are
	 * left with two of a kind. Made and named before anything is compiled, there is never a moment a
	 * child can see anything but the names that stay. */
	SettleEffectComponents(Blueprint);

	if (Attempt(Blueprint, Options)) {
		SettleEffectComponents(Blueprint);

		return true;
	}

	DropSlowTasksTo(Depth);

	/* Kept off disk, because what is here cannot be read back. */
	if (UPackage* Held = Blueprint->GetOutermost()) {
		Held->SetDirtyFlag(false);
		Held->SetFlags(RF_Transient);
	}

	UE_LOG(LogReflection, Error, TEXT("\"%s\" could not be compiled and nothing was written for it"), *Named);

	FImportIssues::Report(
		EImportIssue::Data,
		FString::Printf(TEXT("\"%s\" could not be compiled"), *Named),
		FString::Printf(
			TEXT("The blueprint was built from the export and the compiler could not read it back. That is the asset being wrong in a way nothing here checked for, most often a class it says it comes from or a type it names being something this engine hasn't got. What a compile leaves half built cannot be read back either, so nothing was written for '%s' rather than leaving a package that stops the editor whenever anything opens it. Everything importing alongside it carried on."),
			*Named)
	);

	return false;
}

bool IsBlueprintClassBuilt(const UBlueprint* Blueprint) {
	if (Blueprint == nullptr) return false;

	const UClass* Built = Blueprint->GeneratedClass;

	/* The engine's own test, asked here rather than waited for: StaticAllocateObject asserts on
	 * a class with neither of these the moment a default object is wanted for it */
	return Built != nullptr && Built->ClassWithin != nullptr && Built->ClassConstructor != nullptr;
}

UBlueprint* CreateBlueprintGuarded(UClass* ParentClass, UObject* Outer, const FName Name,
	const EBlueprintType BlueprintType, UClass* BlueprintClassType, UClass* BlueprintGeneratedClassType) {
	const int32 Depth = SlowTasks();

	if (UBlueprint* Made = AttemptCreate(ParentClass, Outer, Name, BlueprintType, BlueprintClassType, BlueprintGeneratedClassType)) {
		return Made;
	}

	DropSlowTasksTo(Depth);

	UE_LOG(LogReflection, Error, TEXT("\"%s\" could not be made and nothing was imported for it"), *Name.ToString());

	FImportIssues::Report(
		EImportIssue::Data,
		FString::Printf(TEXT("\"%s\" could not be made"), *Name.ToString()),
		FString::Printf(
			TEXT("Making a blueprint compiles it, and the compiler could not read what it was given. Usually that is the class it says it comes from being something this engine hasn't got, or having come across broken itself. Nothing was written for '%s' and the rest of the import carried on."),
			*Name.ToString())
	);

	return nullptr;
}
