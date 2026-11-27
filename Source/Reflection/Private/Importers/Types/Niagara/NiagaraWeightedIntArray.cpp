/* Copyright Reflection Contributors 2024-2026 */

#include "NiagaraWeightedIntArray.h"

#include "Engine/Compatibility.h"

#if UE5_1_BEYOND && WITH_EDITORONLY_DATA
#include "Containers/ExportContainer.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "Engine/Log.h"
#include "Importers/Constructor/ImportIssues.h"
#include "NiagaraDataInterfaceArrayInt.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeCustomHlsl.h"
#include "UObject/UnrealType.h"

namespace {
	constexpr TCHAR NativeArray[] = TEXT("NiagaraDataInterfaceArrayDistributionInt");
	constexpr TCHAR LegacyArray[] = TEXT("NiagaraDataInterfaceArrayInt32");
	constexpr TCHAR NativeEntry[] = TEXT("NDIDistributionIntArrayEntry");
	constexpr TCHAR OperationField[] = TEXT("ReflectionWeightedIntArrayOperation");

	// Each row stores the alias probability, alias index, integer value, and original weight.
	constexpr int32 WordsPerEntry = 4;

	enum class EWeightedArrayOperation { None, Num, Get, GetProbabilityAlias };

	struct FWeightedIntEntry {
		int32 Value = 0;
		float Weight = 1;
		float Probability = 1;
		int32 Alias = 0;
	};

	template <typename To, typename From>
	To WeightedArrayBitCast(From Value) {
		static_assert(sizeof(To) == sizeof(From));
		To Bits;
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		return Bits;
	}

	bool ReadCookedTable(const FJsonObject& Properties, TArray<FWeightedIntEntry>& Entries) {
		const TArray<TSharedPtr<FJsonValue>>* Bytes = nullptr;
		if (!Properties.TryGetArrayField(TEXT("BuiltTableData"), Bytes) || int64(Bytes->Num()) != int64(Entries.Num()) * 16) return false;

		for (int32 Index = 0; Index < Entries.Num(); ++Index) {
			uint32 Row[WordsPerEntry] = {};
			const int32 RowOffset = Index * 16;
			for (int32 ByteIndex = 0; ByteIndex < 16; ++ByteIndex) {
				double Byte = 0;
				const TSharedPtr<FJsonValue>& Item = (*Bytes)[RowOffset + ByteIndex];
				if (!Item.IsValid() || !Item->TryGetNumber(Byte) || Byte < 0 || Byte > 255 || Byte != FMath::FloorToDouble(Byte)) return false;
				Row[ByteIndex / 4] |= uint32(Byte) << ((ByteIndex % 4) * 8);
			}

			FWeightedIntEntry& Entry = Entries[Index];
			const float Probability = WeightedArrayBitCast<float>(Row[0]);
			const int32 Alias = WeightedArrayBitCast<int32>(Row[1]);
			if (!FMath::IsFinite(Probability) || Probability < 0 || Probability > 1 || !Entries.IsValidIndex(Alias)
				|| WeightedArrayBitCast<int32>(Row[2]) != Entry.Value
				|| WeightedArrayBitCast<float>(Row[3]) != Entry.Weight) return false;

			Entry.Probability = Probability;
			Entry.Alias = Alias;
		}
		return true;
	}

	void BuildAliasTable(TArray<FWeightedIntEntry>& Entries) {
		double Total = 0;
		for (int32 Index = 0; Index < Entries.Num(); ++Index) {
			Entries[Index].Probability = 1;
			Entries[Index].Alias = Index;
			Total += Entries[Index].Weight;
		}
		if (Total <= 0) return;

		TArray<double> ScaledWeights;
		TArray<int32> Small, Large;
		ScaledWeights.Reserve(Entries.Num());
		for (int32 Index = 0; Index < Entries.Num(); ++Index) {
			const double Scaled = double(Entries[Index].Weight) * Entries.Num() / Total;
			ScaledWeights.Add(Scaled);
			(Scaled < 1 ? Small : Large).Add(Index);
		}

		while (!Small.IsEmpty() && !Large.IsEmpty()) {
			const int32 Short = Small.Pop();
			const int32 Tall = Large.Pop();
			Entries[Short].Probability = float(ScaledWeights[Short]);
			Entries[Short].Alias = Tall;
			ScaledWeights[Tall] += ScaledWeights[Short] - 1;
			(ScaledWeights[Tall] < 1 ? Small : Large).Add(Tall);
		}
	}

	bool PackArray(FJsonObject& Properties) {
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!Properties.TryGetArrayField(TEXT("ArrayData"), Rows)) return false;
		if (Rows->Num() > MAX_int32 / WordsPerEntry) return false;

		TArray<FWeightedIntEntry> Entries;
		Entries.Reserve(Rows->Num());
		for (const TSharedPtr<FJsonValue>& Row : *Rows) {
			const TSharedPtr<FJsonObject>* Fields = nullptr;
			if (!Row.IsValid() || !Row->TryGetObject(Fields)) return false;
			double Value = 0, Weight = 1;
			if (((*Fields)->HasField(TEXT("Value")) && !(*Fields)->TryGetNumberField(TEXT("Value"), Value))
				|| ((*Fields)->HasField(TEXT("Weight")) && !(*Fields)->TryGetNumberField(TEXT("Weight"), Weight))
				|| !FMath::IsFinite(Value) || Value < MIN_int32 || Value > MAX_int32 || Value != FMath::FloorToDouble(Value)
				|| !FMath::IsFinite(Weight) || Weight < 0 || Weight > MAX_flt) return false;

			Entries.Add({int32(Value), float(Weight)});
		}

		if (!ReadCookedTable(Properties, Entries)) BuildAliasTable(Entries);

		TArray<TSharedPtr<FJsonValue>> Packed;
		Packed.Reserve(Entries.Num() * WordsPerEntry);
		for (const FWeightedIntEntry& Entry : Entries) {
			Packed.Add(MakeShared<FJsonValueNumber>(WeightedArrayBitCast<int32>(Entry.Probability)));
			Packed.Add(MakeShared<FJsonValueNumber>(Entry.Alias));
			Packed.Add(MakeShared<FJsonValueNumber>(Entry.Value));
			Packed.Add(MakeShared<FJsonValueNumber>(WeightedArrayBitCast<int32>(Entry.Weight)));
		}
		Properties.SetArrayField(TEXT("IntData"), MoveTemp(Packed));
		Properties.RemoveField(TEXT("ArrayData"));
		Properties.RemoveField(TEXT("BuiltTableData"));
		return true;
	}

	bool NamesNiagaraType(const FJsonObject& Reference, const TCHAR* Type) {
		FString Path, Name;
		return Reference.TryGetStringField(TEXT("ObjectPath"), Path) && Path == TEXT("/Script/Niagara")
			&& Reference.TryGetStringField(TEXT("ObjectName"), Name) && StripObjectOuter(Name) == Type;
	}

	const FJsonObject* WeightedPinType(const FJsonObject& Pin) {
		const TSharedPtr<FJsonObject>* Type = nullptr;
		const TSharedPtr<FJsonObject>* Object = nullptr;
		return Pin.TryGetObjectField(TEXT("PinType"), Type) && (*Type)->TryGetObjectField(TEXT("PinSubCategoryObject"), Object)
			? Object->Get() : nullptr;
	}

	EWeightedArrayOperation ReadOperation(const FUObjectExport& Export) {
		if (Export.GetType() != TEXT("NiagaraNodeFunctionCall")) return EWeightedArrayOperation::None;
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		if (Export.JsonObject->TryGetObjectField(TEXT("Properties"), Properties)
			&& (*Properties)->HasTypedField<EJson::Object>(TEXT("FunctionScript"))) return EWeightedArrayOperation::None;

		const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
		if (!Export.JsonObject->TryGetArrayField(TEXT("Pins"), Pins)) return EWeightedArrayOperation::None;

		bool bArray = false;
		EWeightedArrayOperation Operation = EWeightedArrayOperation::None;
		for (const TSharedPtr<FJsonValue>& Pin : *Pins) {
			const TSharedPtr<FJsonObject>* Fields = nullptr;
			if (!Pin.IsValid() || !Pin->TryGetObject(Fields)) continue;

			FString Direction, Name;
			(*Fields)->TryGetStringField(TEXT("Direction"), Direction);
			(*Fields)->TryGetStringField(TEXT("PinName"), Name);
			const FJsonObject* Type = WeightedPinType(**Fields);
			if (Direction == TEXT("EGPD_Input") && Type != nullptr && NamesNiagaraType(*Type, NativeArray)) {
				bArray = true;
			}
			if (Direction != TEXT("EGPD_Output")) continue;
			if (Name == TEXT("Num")) Operation = EWeightedArrayOperation::Num;
			if (Name == TEXT("Probability")) Operation = EWeightedArrayOperation::GetProbabilityAlias;
			if (Type != nullptr && NamesNiagaraType(*Type, NativeEntry)) Operation = EWeightedArrayOperation::Get;
		}
		return bArray ? Operation : EWeightedArrayOperation::None;
	}

	// Copy only changed branches. Other imports can reuse the same prefetched export data.
	TSharedPtr<FJsonValue> RemapWeightedReference(const TSharedPtr<FJsonValue>& Value, const FString& Field) {
		if (!Value.IsValid()) return nullptr;
		if (Value->Type == EJson::Array) {
			const auto& Children = Value->AsArray();
			TArray<TSharedPtr<FJsonValue>> Replacement;
			for (int32 Index = 0; Index < Children.Num(); ++Index) {
				if (TSharedPtr<FJsonValue> New = RemapWeightedReference(Children[Index], FString())) {
					if (Replacement.IsEmpty()) Replacement = Children;
					Replacement[Index] = MoveTemp(New);
				}
			}
			if (Replacement.IsEmpty()) return nullptr;
			return MakeShared<FJsonValueArray>(MoveTemp(Replacement));
		}
		if (Value->Type != EJson::Object) return nullptr;

		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		TSharedPtr<FJsonObject> Replacement;
		auto Edit = [&]() -> FJsonObject& {
			if (!Replacement.IsValid()) Replacement = MakeShared<FJsonObject>(*Object);
			return *Replacement;
		};

		FString Name;
		if (NamesNiagaraType(*Object, NativeArray)) {
			Edit().SetStringField(TEXT("ObjectName"), TEXT("Class'NiagaraDataInterfaceArrayInt32'"));
		} else if ((Field == TEXT("PinSubCategoryObject") || Field == TEXT("ClassStructOrEnum"))
			&& NamesNiagaraType(*Object, NativeEntry)) {
			// The Get/Break pair becomes a node with separate value and weight outputs.
			Edit().SetStringField(TEXT("ObjectName"), TEXT("Class'NiagaraInt32'"));
		} else if (Object->TryGetStringField(TEXT("ObjectName"), Name)
			&& Name.RemoveFromStart(FString(NativeArray) + TEXT("'"), ESearchCase::CaseSensitive)) {
			Edit().SetStringField(TEXT("ObjectName"), FString(LegacyArray) + TEXT("'") + Name);
		}

		for (const auto& Pair : Object->Values) {
			if (TSharedPtr<FJsonValue> New = RemapWeightedReference(Pair.Value, Pair.Key)) {
				Edit().SetField(Pair.Key, MoveTemp(New));
			}
		}
		if (!Replacement.IsValid()) return nullptr;
		return MakeShared<FJsonValueObject>(MoveTemp(Replacement));
	}

	void MoveWeightedPinLinks(UEdGraphPin& From, UEdGraphPin& To) {
		const TArray<UEdGraphPin*> Links = From.LinkedTo;
		From.BreakAllPinLinks();
		for (UEdGraphPin* Link : Links) To.MakeLinkTo(Link);
		To.DefaultValue = From.DefaultValue;
		To.DefaultObject = From.DefaultObject;
	}

	bool RestoreWeightedCall(FUObjectExport& Export, EWeightedArrayOperation Operation) {
		auto* Original = Cast<UNiagaraNodeFunctionCall>(Export.Object);
		auto* Graph = Original != nullptr ? Cast<UNiagaraGraph>(Original->GetOuter()) : nullptr;
		if (Graph == nullptr || !Graph->Nodes.Contains(Original)) return false;

		UEdGraphPin* ArrayPin = nullptr;
		for (UEdGraphPin* Pin : Original->Pins) {
			if (Pin != nullptr && Pin->Direction == EGPD_Input && Pin->PinType.PinSubCategoryObject.Get() == UNiagaraDataInterfaceArrayInt32::StaticClass()) {
				ArrayPin = Pin;
				break;
			}
		}
		UEdGraphPin* IndexPin = Original->FindPin(TEXT("Index"), EGPD_Input);
		if (ArrayPin == nullptr || (Operation != EWeightedArrayOperation::Num && IndexPin == nullptr)) return false;

		TArray<UEdGraphNode*> Breaks;
		TArray<UEdGraphPin*> ValuePins, WeightPins;
		if (Operation == EWeightedArrayOperation::Get) {
			const UEdGraphPin* Entry = Original->FindPin(TEXT("Value"), EGPD_Output);
			if (Entry == nullptr) return false;
			for (const UEdGraphPin* Link : Entry->LinkedTo) {
				UEdGraphNode* Break = Link != nullptr ? Link->GetOwningNode() : nullptr;
				if (Break == nullptr || Break->GetClass()->GetFName() != TEXT("NiagaraNodeConvert")
					|| Link->Direction != EGPD_Input || Link->LinkedTo.Num() != 1) return false;

				for (UEdGraphPin* Pin : Break->Pins) {
					if (Pin == nullptr || Pin->Direction != EGPD_Output || Pin->LinkedTo.IsEmpty()) continue;
					if (Pin->PinName == TEXT("Value")) ValuePins.Add(Pin);
					else if (Pin->PinName == TEXT("Weight")) WeightPins.Add(Pin);
					else return false;
				}
				Breaks.AddUnique(Break);
			}
		}

		const FStrProperty* Hlsl = FindFProperty<FStrProperty>(UNiagaraNodeCustomHlsl::StaticClass(), TEXT("CustomHlsl"));
		if (Hlsl == nullptr) return false;

		auto* Node = NewObject<UNiagaraNodeCustomHlsl>(Graph, NAME_None, RF_Transactional);
		Node->NodeGuid = Original->NodeGuid;
		Node->NodePosX = Original->NodePosX;
		Node->NodePosY = Original->NodePosY;
		Graph->AddNode(Node, false, false);

		auto AddPin = [&](EEdGraphPinDirection Direction, FName Name, const FNiagaraTypeDefinition& Type) {
			auto& Signature = Direction == EGPD_Input ? Node->Signature.Inputs : Node->Signature.Outputs;
			Signature.Emplace(Type, Name);
			return Node->CreatePin(Direction, UEdGraphSchema_Niagara::TypeDefinitionToPinType(Type), Name);
		};
		const auto& Int = FNiagaraTypeDefinition::GetIntDef();
		const auto& Float = FNiagaraTypeDefinition::GetFloatDef();
		MoveWeightedPinLinks(*ArrayPin, *AddPin(EGPD_Input, TEXT("Distribution"), FNiagaraTypeDefinition(UNiagaraDataInterfaceArrayInt32::StaticClass())));
		if (IndexPin != nullptr) MoveWeightedPinLinks(*IndexPin, *AddPin(EGPD_Input, TEXT("Index"), Int));

		auto AddOutput = [&](FName Name, const FNiagaraTypeDefinition& Type, const TArray<UEdGraphPin*>& Sources) {
			UEdGraphPin* Pin = AddPin(EGPD_Output, Name, Type);
			for (UEdGraphPin* Source : Sources) {
				if (Source != nullptr) MoveWeightedPinLinks(*Source, *Pin);
			}
		};

		FString Code = TEXT("int Words; Distribution.Length(Words); ");
		if (Operation == EWeightedArrayOperation::Num) {
			Node->Signature.Name = TEXT("WeightedArrayCount");
			AddOutput(TEXT("Num"), Int, {Original->FindPin(TEXT("Num"), EGPD_Output)});
			Code += TEXT("Num = Words / 4;");
		} else {
			Code += TEXT("int Base = clamp(Index, 0, max(0, Words / 4 - 1)) * 4; int Bits; ");
			if (Operation == EWeightedArrayOperation::Get) {
				Node->Signature.Name = TEXT("WeightedArrayEntry");
				AddOutput(TEXT("Value"), Int, ValuePins);
				AddOutput(TEXT("Weight"), Float, WeightPins);
				Code += TEXT("Distribution.Get(Base + 2, Value); Distribution.Get(Base + 3, Bits); Weight = asfloat(Bits);");
			} else {
				Node->Signature.Name = TEXT("WeightedArrayProbability");
				AddOutput(TEXT("Probability"), Float, {Original->FindPin(TEXT("Probability"), EGPD_Output)});
				AddOutput(TEXT("Alias"), Int, {Original->FindPin(TEXT("Alias"), EGPD_Output)});
				Code += TEXT("Distribution.Get(Base, Bits); Probability = asfloat(Bits); Distribution.Get(Base + 1, Alias);");
			}
		}
		Hlsl->SetPropertyValue_InContainer(Node, Code);

		for (UEdGraphNode* Break : Breaks) {
			Break->BreakAllNodeLinks();
			Graph->RemoveNode(Break);
		}
		Original->BreakAllNodeLinks();
		Graph->RemoveNode(Original);
		Export.Object = Node;
		return true;
	}
}
#endif

void FNiagaraWeightedIntArray::Prepare(FUObjectExportContainer& Container) {
#if UE5_1_BEYOND && WITH_EDITORONLY_DATA
	if (FindObject<UClass>(nullptr, TEXT("/Script/Niagara.NiagaraDataInterfaceArrayDistributionInt")) != nullptr) return;

	if (!Container.Exports.ContainsByPredicate([](const FUObjectExport* Export) { return Export->GetType() == NativeArray; })) return;

	int32 Converted = 0;
	for (FUObjectExport* Export : Container.Exports) {
		const EWeightedArrayOperation Operation = ReadOperation(*Export);
		const bool bArray = Export->GetType() == NativeArray;
		TSharedPtr<FJsonValue> Replacement = RemapWeightedReference(Container.JsonObjects[Export->Position], FString());
		if (!Replacement.IsValid() && !bArray && Operation == EWeightedArrayOperation::None) continue;

		TSharedPtr<FJsonObject> Object = Replacement.IsValid() ? Replacement->AsObject() : MakeShared<FJsonObject>(*Export->JsonObject);
		if (bArray) {
			const TSharedPtr<FJsonObject>* Fields = nullptr;
			const auto Properties = Object->TryGetObjectField(TEXT("Properties"), Fields) ? MakeShared<FJsonObject>(**Fields) : MakeShared<FJsonObject>();
			if (!PackArray(*Properties)) {
				FImportIssues::Report(EImportIssue::Data, TEXT("Could not convert a Niagara weighted integer array"), Export->GetName().ToString());
				continue;
			}
			Object->SetObjectField(TEXT("Properties"), Properties);
			Object->SetStringField(TEXT("Type"), LegacyArray);
			Object->SetStringField(TEXT("Class"), TEXT("Class'NiagaraDataInterfaceArrayInt32'"));
			++Converted;
		}
		if (Operation != EWeightedArrayOperation::None) Object->SetNumberField(OperationField, int32(Operation));

		Export->JsonObject = Object;
		Container.JsonObjects[Export->Position] = MakeShared<FJsonValueObject>(MoveTemp(Object));
	}
	if (Converted > 0) UE_LOG(LogReflection, Display, TEXT("Converted %d Niagara weighted integer array(s)"), Converted);
#endif
}

void FNiagaraWeightedIntArray::RestoreCalls(FUObjectExportContainer& Container) {
#if UE5_1_BEYOND && WITH_EDITORONLY_DATA
	for (FUObjectExport* Export : Container.Exports) {
		int32 Operation;
		if (Export->JsonObject->TryGetNumberField(OperationField, Operation)
			&& Operation >= int32(EWeightedArrayOperation::Num) && Operation <= int32(EWeightedArrayOperation::GetProbabilityAlias)
			&& !RestoreWeightedCall(*Export, EWeightedArrayOperation(Operation))) {
			FImportIssues::Report(EImportIssue::Data, TEXT("Could not convert a Niagara weighted integer array call"), Export->GetName().ToString());
		}
	}
#endif
}
