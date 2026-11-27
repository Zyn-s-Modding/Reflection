/* Copyright Reflection Contributors 2024-2026 */

#include "NiagaraStatelessFallback.h"
#include "Engine/Compatibility.h"

#if UE5_1_BEYOND && WITH_EDITORONLY_DATA
#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/ExportContainer.h"
#include "Curves/RichCurve.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "Engine/EngineUtilities.h"
#include "Importers/Constructor/ImportIssues.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/ScopeExit.h"
#include "NiagaraCommon.h"
#include "NiagaraConstants.h"
#include "NiagaraDataInterfaceCurveBase.h"
#include "NiagaraDataInterfaceCurve.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraLightRendererProperties.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraSystem.h"
#include "Serializers/ObjectSerializer.h"
#include "UObject/UnrealType.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include <initializer_list>

namespace {
	/* The handle fields a 6.0 stateless emitter is named by, which Convert carries over itself */
	const struct FStatelessHandledFields {
		FStatelessHandledFields() {
			FImportIssues::MarkFieldHandled(TEXT("NiagaraEmitterHandle"), TEXT("EmitterMode"));
			FImportIssues::MarkFieldHandled(TEXT("NiagaraEmitterHandle"), TEXT("StatelessEmitter"));
		}
	} GStatelessHandledFields;

	/* What could not be carried over, said per input ("ShapeLocation.Ring Radius (range of this
	 * type)") rather than only per module, so the report names the setting that was lost. The
	 * fallback runs on the game thread and one emitter at a time. */
	TArray<FString>* GInputIssues = nullptr;
	TArray<FString>* GInputNotes = nullptr;
	FString GModule;

	bool Fail(const TCHAR* Input, const FString& Why) {
		if (GInputIssues != nullptr) GInputIssues->AddUnique(FString::Printf(TEXT("%s.%s (%s)"), *GModule, Input, *Why));
		return false;
	}

	/* A value that was used but is a guess, or a setting with nowhere to go, said apart from the
	 * failures since the input itself was still set */
	void Note(const TCHAR* Input, const FString& Why) {
		if (GInputNotes != nullptr) GInputNotes->AddUnique(FString::Printf(TEXT("%s.%s (%s)"), *GModule, Input, *Why));
	}

	/* An input typed as an enum (Coordinate Space) rather than switched */
	bool SetEnumInput(UNiagaraNodeFunctionCall* Node, const TCHAR* Name, const FString& Choice);

	/* Class defaults filled in for fields the export left out, said once per field so the import
	 * says which values came from the data file rather than from the asset */
	TArray<FString>* GDefaultsUsed = nullptr;

	/* The value a 6.0 stateless module has where its export says nothing.
	 *
	 * A cooked object is written as its differences from the class default, so a distribution
	 * whose Min is missing has the class default's Min, not no Min. The class is native and isn't
	 * in this engine, so its defaults are data, in Resources/NiagaraStatelessDefaults.json, keyed
	 * Module.Field: each is the value the 43.x corpus never carries for that field while often
	 * leaving the field out (92 systems, 412 stateless emitters), which is what a class default
	 * looks like from the outside. bApplyWhenOmitted is for the few where the 5.4 module's own
	 * default differs, so the input is set even when the export leaves it alone. A field with no
	 * entry is reported where its default would have been needed, so adding the entry is all it
	 * takes to carry it. */
	struct FDefault {
		bool bSet = false;
		FVector4f Value = FVector4f(0, 0, 0, 0);
		bool bApplyWhenOmitted = false;
		FString Key;

		FDefault() = default;
		explicit FDefault(float InValue, bool bApply = false) : bSet(true), Value(InValue, InValue, InValue, InValue), bApplyWhenOmitted(bApply) {}
		explicit FDefault(const FVector4f& InValue, bool bApply = false) : bSet(true), Value(InValue), bApplyWhenOmitted(bApply) {}
	};

	const TMap<FString, FDefault>& KnownDefaults() {
		static const TMap<FString, FDefault> Known = [] {
			TMap<FString, FDefault> Read;
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Reflection"));
			const FString File = Plugin.IsValid() ? Plugin->GetBaseDir() / TEXT("Resources/NiagaraStatelessDefaults.json") : FString();
			FString Text;
			TSharedPtr<FJsonObject> Root;
			const TSharedPtr<FJsonObject>* Defaults = nullptr;
			if (File.IsEmpty() || !FFileHelper::LoadFileToString(Text, *File) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root)
				|| !Root.IsValid() || !Root->TryGetObjectField(TEXT("Defaults"), Defaults)) {
				UE_LOG(LogReflection, Warning, TEXT("Stateless Niagara class defaults could not be read from \"%s\"; fields an export leaves out will be reported instead of filled in"), *File);
				return Read;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Defaults)->Values) {
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				if (!Pair.Value.IsValid() || !Pair.Value->TryGetObject(Entry) || !Entry->IsValid()) continue;
				const TSharedPtr<FJsonValue> Value = (*Entry)->TryGetField(TEXT("Value"));
				const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
				double Number = 0;
				FDefault Default;
				if (Value.IsValid() && Value->TryGetNumber(Number)) {
					Default = FDefault(float(Number));
				} else if (Value.IsValid() && Value->TryGetArray(Parts) && !Parts->IsEmpty()) {
					FVector4f Vector(0, 0, 0, 0);
					for (int32 Index = 0; Index < FMath::Min(4, Parts->Num()); ++Index) {
						double Part = 0;
						if ((*Parts)[Index].IsValid()) (*Parts)[Index]->TryGetNumber(Part);
						Vector[Index] = float(Part);
					}
					Default = FDefault(Vector);
				} else {
					UE_LOG(LogReflection, Warning, TEXT("Stateless Niagara class default \"%s\" in \"%s\" has no number or [x, y, z, w] Value; skipped"), *Pair.Key, *File);
					continue;
				}
				(*Entry)->TryGetBoolField(TEXT("ApplyWhenOmitted"), Default.bApplyWhenOmitted);
				Default.Key = Pair.Key;
				Read.Add(Pair.Key, MoveTemp(Default));
			}
			UE_LOG(LogReflection, Display, TEXT("Read %d stateless Niagara class default(s) from \"%s\""), Read.Num(), *File);
			return Read;
		}();
		return Known;
	}

	/* The default the data file has for a field of the module being converted, or none */
	FDefault DefaultFor(const TCHAR* Field) {
		const FDefault* Found = KnownDefaults().Find(GModule + TEXT(".") + Field);
		return Found != nullptr ? *Found : FDefault();
	}

	FString DescribeDefault(const FVector4f& Value) {
		if (Value.X == Value.Y && Value.X == Value.Z && Value.X == Value.W) return FString::SanitizeFloat(Value.X);
		return FString::Printf(TEXT("(%s, %s, %s, %s)"), *FString::SanitizeFloat(Value.X), *FString::SanitizeFloat(Value.Y),
			*FString::SanitizeFloat(Value.Z), *FString::SanitizeFloat(Value.W));
	}

	void UsedDefault(const FDefault& Default, const TCHAR* Why) {
		if (GDefaultsUsed == nullptr || !Default.bSet) return;
		GDefaultsUsed->AddUnique(FString::Printf(TEXT("%s = %s (%s)"), Default.Key.IsEmpty() ? *GModule : *Default.Key, *DescribeDefault(Default.Value), Why));
	}

	const FJsonObject* ObjectField(const FJsonObject& Json, const TCHAR* Name) {
		const TSharedPtr<FJsonObject>* Field = nullptr;
		return Json.TryGetObjectField(Name, Field) ? Field->Get() : nullptr;
	}

	UNiagaraScript* LoadNiagaraScript(const TCHAR* RelativePath) {
		const FString Path = FString(TEXT("/Niagara/")) + RelativePath;
		return LoadObject<UNiagaraScript>(nullptr, *(Path + TEXT(".") + FPaths::GetCleanFilename(Path)));
	}

	FString LeafEnum(const FString& Name) {
		FString Leaf;
		return Name.Split(TEXT("::"), nullptr, &Leaf, ESearchCase::CaseSensitive, ESearchDir::FromEnd) ? Leaf : Name;
	}

	UNiagaraScriptVariable* FindInput(UNiagaraNodeFunctionCall* Node, const TCHAR* Name) {
		UNiagaraGraph* Graph = Node != nullptr ? Node->GetCalledGraph() : nullptr;
		return Graph != nullptr ? Graph->GetScriptVariable(FName(*(FString(TEXT("Module.")) + Name))) : nullptr;
	}

	UEdGraphPin& OverrideInput(UNiagaraNodeFunctionCall& Node, const UNiagaraScriptVariable& Input) {
		const auto Handle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(FNiagaraParameterHandle(Input.Variable.GetName()), &Node);
		return FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(Node, Handle, Input.Variable.GetType(),
			Input.Metadata.GetVariableGuid(), FGuid());
	}

	bool ReadSample(const FJsonValue& Json, FVector4f& Sample) {
		double Number;
		if (Json.TryGetNumber(Number)) {
			Sample = FVector4f(Number, Number, Number, Number);
			return FMath::IsFinite(Number) && FMath::IsFinite(Sample.X);
		}

		const TSharedPtr<FJsonObject>* Tuple = nullptr;
		if (!Json.TryGetObject(Tuple) || !Tuple->IsValid()) return false;

		const bool bColor = (*Tuple)->HasField(TEXT("R"));
		const TCHAR* const Coordinates[] = {TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W")};
		const TCHAR* const Channels[] = {TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A")};
		const TCHAR* const* Names = bColor ? Channels : Coordinates;
		for (int32 Index = 0; Index < 4; ++Index) {
			Number = bColor && Index == 3 ? 1 : 0;
			if (!(*Tuple)->TryGetNumberField(Names[Index], Number) && Index == 0) return false;
			Sample[Index] = Number;
			if (!FMath::IsFinite(Sample[Index])) return false;
		}
		return true;
	}

	bool SetLiteral(UNiagaraNodeFunctionCall& Node, const UNiagaraScriptVariable& Input, const FVector4f& Value) {
		FNiagaraVariable Variable = Input.Variable;
		const FNiagaraTypeDefinition& Type = Variable.GetType();
		if (Type == FNiagaraTypeDefinition::GetFloatDef()) Variable.SetValue(float(Value.X));
		else if (Type.IsSameBaseDefinition(FNiagaraTypeDefinition::GetBoolDef())) Variable.SetValue(FNiagaraBool(Value.X != 0));
		else if (Type == FNiagaraTypeDefinition::GetIntDef()) Variable.SetValue(int32(FMath::Clamp(double(Value.X), double(MIN_int32), double(MAX_int32))));
		else if (Type == FNiagaraTypeDefinition::GetVec2Def()) Variable.SetValue(FVector2f(Value.X, Value.Y));
		else if (Type == FNiagaraTypeDefinition::GetVec3Def()) Variable.SetValue(FVector3f(Value.X, Value.Y, Value.Z));
		else if (Type == FNiagaraTypeDefinition::GetVec4Def()) Variable.SetValue(Value);
		else if (Type == FNiagaraTypeDefinition::GetColorDef()) Variable.SetValue(FLinearColor(Value.X, Value.Y, Value.Z, Value.W));
		else if (Type == FNiagaraTypeDefinition::GetPositionDef()) Variable.SetValue(FNiagaraPosition(Value.X, Value.Y, Value.Z));
		else return false;

		FString Default;
		if (!GetDefault<UEdGraphSchema_Niagara>()->TryGetPinDefaultValueFromNiagaraVariable(Variable, Default)) return false;
		OverrideInput(Node, Input).DefaultValue = MoveTemp(Default);
		return true;
	}

	bool SetLiteral(UNiagaraNodeFunctionCall* Node, const TCHAR* Name, const FVector4f& Value) {
		const UNiagaraScriptVariable* Input = FindInput(Node, Name);
		if (Input == nullptr) return Fail(Name, TEXT("no such input on the 5.4 module"));
		return SetLiteral(*Node, *Input, Value) || Fail(Name, TEXT("value of this type"));
	}

	bool SetSwitch(UNiagaraNodeFunctionCall* Node, const TCHAR* Name, const FString& Value) {
		if (Node == nullptr) return false;
		for (UEdGraphPin* Pin : Node->Pins) {
			if (Pin->Direction == EGPD_Input && Pin->PinName == FName(Name)) {
				Pin->DefaultValue = Value;
				return true;
			}
		}
		return Fail(Name, TEXT("no such switch on the 5.4 module"));
	}

	bool SetNamedSwitch(UNiagaraNodeFunctionCall* Node, const TCHAR* Name, const FString& Choice) {
		UNiagaraGraph* Graph = Node != nullptr ? Node->GetCalledGraph() : nullptr;
		const UNiagaraScriptVariable* Variable = Graph != nullptr ? Graph->GetScriptVariable(FName(Name)) : nullptr;
		const UEnum* Enum = Variable != nullptr ? Variable->Variable.GetType().GetEnum() : nullptr;
		if (Enum == nullptr) return Fail(Name, TEXT("no such switch on the 5.4 module"));
		for (int32 Index = 0; Index + 1 < Enum->NumEnums(); ++Index) {
			if (Enum->GetNameStringByIndex(Index).Equals(Choice) || Enum->GetDisplayNameTextByIndex(Index).ToString().Equals(Choice)) {
				return SetSwitch(Node, Name, Enum->GetNameStringByIndex(Index));
			}
		}
		return Fail(Name, FString::Printf(TEXT("no choice \"%s\""), *Choice));
	}

	void SetLink(UNiagaraNodeFunctionCall& Node, const UNiagaraScriptVariable& Input, FName Name) {
		const TSet<FNiagaraVariable> Known{FNiagaraVariable(Input.Variable.GetType(), Name)};
		FNiagaraStackGraphUtilities::SetLinkedValueHandleForFunctionInput(OverrideInput(Node, Input), FNiagaraParameterHandle(Name), Known);
	}

	/* The type a 6.0 distribution's ParameterBinding says its parameter is
	 * (TypeDef.ClassStructOrEnum: "Class'LinearColor'"), or an invalid type where it says nothing this knows */
	FNiagaraTypeDefinition BoundType(const FJsonObject* ParameterBinding) {
		const FJsonObject* TypeDef = ParameterBinding != nullptr ? ObjectField(*ParameterBinding, TEXT("TypeDef")) : nullptr;
		const FJsonObject* Class = TypeDef != nullptr ? ObjectField(*TypeDef, TEXT("ClassStructOrEnum")) : nullptr;
		FString Name;
		if (Class == nullptr || !Class->TryGetStringField(TEXT("ObjectName"), Name)) return FNiagaraTypeDefinition();
		Name.Split(TEXT("'"), nullptr, &Name);
		Name.ReplaceInline(TEXT("'"), TEXT(""));

		if (Name == TEXT("LinearColor")) return FNiagaraTypeDefinition::GetColorDef();
		if (Name == TEXT("Vector4f") || Name == TEXT("Vector4")) return FNiagaraTypeDefinition::GetVec4Def();
		if (Name == TEXT("Vector3f") || Name == TEXT("Vector")) return FNiagaraTypeDefinition::GetVec3Def();
		if (Name == TEXT("Vector2f") || Name == TEXT("Vector2D")) return FNiagaraTypeDefinition::GetVec2Def();
		if (Name == TEXT("NiagaraPosition")) return FNiagaraTypeDefinition::GetPositionDef();
		if (Name == TEXT("NiagaraFloat")) return FNiagaraTypeDefinition::GetFloatDef();
		if (Name == TEXT("NiagaraInt32")) return FNiagaraTypeDefinition::GetIntDef();
		if (Name == TEXT("NiagaraBool")) return FNiagaraTypeDefinition::GetBoolDef();
		return FNiagaraTypeDefinition();
	}

	/* An input of a called script, found by its type rather than its name: the one input of that type */
	const UNiagaraScriptVariable* FindInputOfType(UNiagaraNodeFunctionCall* Node, const FNiagaraTypeDefinition& Type) {
		UNiagaraGraph* Graph = Node != nullptr ? Node->GetCalledGraph() : nullptr;
		if (Graph == nullptr) return nullptr;

		const UNiagaraScriptVariable* Found = nullptr;
		for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Entry : Graph->GetAllMetaData()) {
			if (Entry.Value == nullptr || !Entry.Key.IsInNameSpace(FNiagaraConstants::ModuleNamespaceString) || Entry.Key.GetType() != Type) continue;
			if (Found != nullptr) return nullptr;
			Found = Entry.Value;
		}
		return Found;
	}

	/* Links an input to the parameter a distribution is bound to, as the parameter's own type.
	 *
	 * Linked as the input's type, a read of System.LWELightColor (a Linear Color) by ScaleColor's
	 * Scale RGBA (a Vector 4) is a read of a Vector 4 parameter of that name, which nothing writes:
	 * the system update script writes the color one. The compile's linker matches name and type, so
	 * every script reading it failed with "was read before being set". Where the two differ, the
	 * parameter is read as what it is and converted to what the input takes. */
	bool SetBinding(UNiagaraNodeFunctionCall& Node, const UNiagaraScriptVariable& Input, const TCHAR* InputName, FName Name, const FNiagaraTypeDefinition& Bound) {
		const FNiagaraTypeDefinition& Wanted = Input.Variable.GetType();
		if (!Bound.IsValid() || Bound == Wanted || FNiagaraUtilities::AreTypesAssignable(Bound, Wanted)) {
			SetLink(Node, Input, Name);
			return true;
		}

		const TCHAR* Path = nullptr;
		if (Wanted == FNiagaraTypeDefinition::GetVec4Def() && Bound == FNiagaraTypeDefinition::GetColorDef()) Path = TEXT("DynamicInputs/LinearColor/MakeVector4_FromLinearColor");
		else if (Wanted == FNiagaraTypeDefinition::GetColorDef() && Bound == FNiagaraTypeDefinition::GetVec4Def()) Path = TEXT("DynamicInputs/LinearColor/MakeLinearColorFromFloat4");

		UNiagaraScript* Script = Path != nullptr ? LoadNiagaraScript(Path) : nullptr;
		if (Script == nullptr) {
			SetLink(Node, Input, Name);
			Note(InputName, FString::Printf(TEXT("bound to %s, a %s, read as a %s"), *Name.ToString(), *Bound.GetName(), *Wanted.GetName()));
			return true;
		}

		UNiagaraNodeFunctionCall* Conversion = nullptr;
		FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput(OverrideInput(Node, Input), Script, Conversion);
		const UNiagaraScriptVariable* From = FindInputOfType(Conversion, Bound);
		if (From == nullptr) return Fail(InputName, FString::Printf(TEXT("no %s input on %s"), *Bound.GetName(), Path));

		SetLink(*Conversion, *From, Name);
		return true;
	}

	bool SetRange(UNiagaraNodeFunctionCall& Node, const UNiagaraScriptVariable& Input, const FVector4f& Min, const FVector4f& Max) {
		const FNiagaraTypeDefinition& Type = Input.Variable.GetType();
		const TCHAR* Path = nullptr;
		if (Type == FNiagaraTypeDefinition::GetFloatDef()) Path = TEXT("DynamicInputs/UniformRange/V2/RandomRangeFloat");
		else if (Type == FNiagaraTypeDefinition::GetIntDef()) Path = TEXT("DynamicInputs/UniformRange/V2/RandomRangeInt");
		else if (Type == FNiagaraTypeDefinition::GetVec2Def()) Path = TEXT("DynamicInputs/UniformRange/V2/RandomRangeVector2D");
		else if (Type == FNiagaraTypeDefinition::GetVec3Def()) Path = TEXT("DynamicInputs/UniformRange/V2/RandomRangeVector");
		else if (Type == FNiagaraTypeDefinition::GetVec4Def()) Path = TEXT("DynamicInputs/UniformRange/V2/RandomRangeVector4");
		else if (Type == FNiagaraTypeDefinition::GetColorDef()) Path = TEXT("DynamicInputs/UniformRange/V2/UniformRangedLinearColor");
		if (Path == nullptr) return false;

		UNiagaraScript* Script = LoadNiagaraScript(Path);
		if (Script == nullptr) return false;

		UNiagaraNodeFunctionCall* Dynamic = nullptr;
		FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput(OverrideInput(Node, Input), Script, Dynamic);
		const bool bMin = SetLiteral(Dynamic, TEXT("Minimum"), Min);
		const bool bMax = SetLiteral(Dynamic, TEXT("Maximum"), Max);
		return bMin && bMax;
	}

	/* The rich curves a curve data interface keeps, by class */
	bool CurveChannels(const UClass* Class, TArray<const TCHAR*, TInlineAllocator<4>>& Channels) {
		const FName Name = Class->GetFName();
		if (Name == TEXT("NiagaraDataInterfaceCurve")) Channels = {TEXT("Curve")};
		else if (Name == TEXT("NiagaraDataInterfaceVector2DCurve")) Channels = {TEXT("XCurve"), TEXT("YCurve")};
		else if (Name == TEXT("NiagaraDataInterfaceVectorCurve")) Channels = {TEXT("XCurve"), TEXT("YCurve"), TEXT("ZCurve")};
		else if (Name == TEXT("NiagaraDataInterfaceVector4Curve")) Channels = {TEXT("XCurve"), TEXT("YCurve"), TEXT("ZCurve"), TEXT("WCurve")};
		else if (Name == TEXT("NiagaraDataInterfaceColorCurve")) Channels = {TEXT("RedCurve"), TEXT("GreenCurve"), TEXT("BlueCurve"), TEXT("AlphaCurve")};
		else return false;
		return true;
	}

	bool SetCurve(UNiagaraNodeFunctionCall& Node, const UNiagaraScriptVariable& Input, const TArray<TSharedPtr<FJsonValue>>& Values, const FJsonObject* TimeRange) {
		TArray<FVector4f> Samples;
		for (const TSharedPtr<FJsonValue>& Value : Values) {
			FVector4f Sample;
			if (!Value.IsValid() || !ReadSample(*Value, Sample)) return false;
			Samples.Add(Sample);
		}

		const FNiagaraTypeDefinition& Type = Input.Variable.GetType();
		const TCHAR* ScriptPath = nullptr;
		const TCHAR* CurveInputName = nullptr;
		const UClass* InputClass = Type.GetClass();
		TArray<const TCHAR*, TInlineAllocator<4>> Channels;
		/* An input that is itself a curve data interface (ScaleColor's Linear Color Curve,
		 * ScaleSpriteSizeBySpeed's Scale Factor Curve) takes the samples directly */
		const bool bDirectColor = InputClass != nullptr && CurveChannels(InputClass, Channels);
		if (bDirectColor) {
			/* The channels are the input's own */
		} else if (Type == FNiagaraTypeDefinition::GetFloatDef()) {
			ScriptPath = TEXT("DynamicInputs/ValueFromCurve/FloatFromCurve"); CurveInputName = TEXT("FloatCurve"); Channels = {TEXT("Curve")};
		} else if (Type == FNiagaraTypeDefinition::GetVec2Def()) {
			ScriptPath = TEXT("DynamicInputs/ValueFromCurve/Vector2DFromCurve"); CurveInputName = TEXT("Vector2Curve"); Channels = {TEXT("XCurve"), TEXT("YCurve")};
		} else if (Type == FNiagaraTypeDefinition::GetVec3Def()) {
			ScriptPath = TEXT("DynamicInputs/ValueFromCurve/VectorFromCurve"); CurveInputName = TEXT("VectorCurve"); Channels = {TEXT("XCurve"), TEXT("YCurve"), TEXT("ZCurve")};
		} else if (Type == FNiagaraTypeDefinition::GetVec4Def()) {
			ScriptPath = TEXT("DynamicInputs/ValueFromCurve/Vector4FromCurve"); CurveInputName = TEXT("Vector4Curve"); Channels = {TEXT("XCurve"), TEXT("YCurve"), TEXT("ZCurve"), TEXT("WCurve")};
		} else if (Type == FNiagaraTypeDefinition::GetColorDef()) {
			ScriptPath = TEXT("DynamicInputs/ValueFromCurve/ColorFromCurve"); CurveInputName = TEXT("ColorCurve"); Channels = {TEXT("RedCurve"), TEXT("GreenCurve"), TEXT("BlueCurve"), TEXT("AlphaCurve")};
		}
		if (ScriptPath == nullptr && !bDirectColor) return false;

		UNiagaraNodeFunctionCall* CurveNode = &Node;
		const UNiagaraScriptVariable* CurveInput = &Input;
		if (!bDirectColor) {
			UNiagaraScript* Script = LoadNiagaraScript(ScriptPath);
			if (Script == nullptr) return false;
			FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput(OverrideInput(Node, Input), Script, CurveNode);
			CurveInput = FindInput(CurveNode, CurveInputName);
		}
		if (CurveInput == nullptr) return false;

		UClass* Class = const_cast<UClass*>(CurveInput->Variable.GetType().GetClass());
		if (Class == nullptr || !Class->IsChildOf(UNiagaraDataInterfaceCurveBase::StaticClass())) return false;
		UNiagaraDataInterface* Interface = nullptr;
		FNiagaraStackGraphUtilities::SetDataInterfaceValueForFunctionInput(OverrideInput(*CurveNode, *CurveInput), Class, TEXT("Curve"), Interface);
		auto* Curve = Cast<UNiagaraDataInterfaceCurveBase>(Interface);
		if (Curve == nullptr) return false;

		double First = 0, Last = 1;
		if (TimeRange != nullptr) {
			TimeRange->TryGetNumberField(TEXT("X"), First);
			TimeRange->TryGetNumberField(TEXT("Y"), Last);
		}
		for (int32 Channel = 0; Channel < Channels.Num(); ++Channel) {
			const auto* Property = FindFProperty<FStructProperty>(Class, Channels[Channel]);
			if (Property == nullptr || Property->Struct != FRichCurve::StaticStruct()) return false;
			FRichCurve* Data = Property->ContainerPtrToValuePtr<FRichCurve>(Curve);
			Data->Reset();
			for (int32 Index = 0; Index < Samples.Num(); ++Index) {
				const float Alpha = float(Index) / float(Samples.Num() - 1);
				Data->AddKey(FMath::Lerp(First, Last, Alpha), Samples[Index][Channel]);
			}
		}
		Curve->UpdateLUT();
		if (const UNiagaraScriptVariable* Index = FindInput(CurveNode, bDirectColor ? TEXT("Curve Index") : TEXT("CurveIndex"))) {
			SetLink(*CurveNode, *Index, FName(TEXT("Particles.NormalizedAge")));
		}
		return true;
	}

	bool SetDistribution(UNiagaraNodeFunctionCall* Node, const TCHAR* InputName, const FJsonObject* Distribution,
		const FDefault& Default = FDefault(), bool bDefaultToCurve = false) {
		if (Node == nullptr) return false;
		if (Distribution == nullptr) {
			if (!(Default.bSet && Default.bApplyWhenOmitted)) return true;
			UsedDefault(Default, TEXT("left out of the export"));
			return SetLiteral(Node, InputName, Default.Value);
		}

		FString Mode;
		Distribution->TryGetStringField(TEXT("Mode"), Mode);
		Mode = LeafEnum(Mode);

		FString Binding;
		const FJsonObject* BindingField = ObjectField(*Distribution, TEXT("ParameterBinding"));
		if (BindingField != nullptr) BindingField->TryGetStringField(TEXT("Name"), Binding);
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		Distribution->TryGetArrayField(TEXT("Values"), Values);
		const auto MinValue = Distribution->TryGetField(TEXT("Min"));
		const auto MaxValue = Distribution->TryGetField(TEXT("Max"));

		/* A binding is only what the value is in Binding mode. A distribution keeps the parameter
		 * it was last bound to after it is switched back to a constant or a range, and following
		 * it then wires the input to a parameter the value no longer comes from. */
		const bool bLink = Mode == TEXT("Binding") && !Binding.IsEmpty() && Binding != TEXT("None");
		if (Mode == TEXT("Binding") && !bLink) return Fail(InputName, TEXT("bound to no parameter"));

		if (!bLink && !MinValue.IsValid() && !MaxValue.IsValid() && (Values == nullptr || Values->IsEmpty())) {
			/* Nothing written means the class default, as a constant whatever the mode */
			if (Default.bSet && (Default.bApplyWhenOmitted || !Mode.IsEmpty())) {
				UsedDefault(Default, TEXT("no value written"));
				return SetLiteral(Node, InputName, Default.Value);
			}
			return Mode.IsEmpty() || Fail(InputName, TEXT("class default not known"));
		}

		const UNiagaraScriptVariable* Input = FindInput(Node, InputName);
		if (Input == nullptr) return Fail(InputName, TEXT("no such input on the 5.4 module"));
		if (bLink) {
			return SetBinding(*Node, *Input, InputName, FName(*Binding), BoundType(BindingField));
		}

		const bool bCurve = Mode.EndsWith(TEXT("Curve")) || Mode == TEXT("ColorGradient") || (Mode.IsEmpty() && bDefaultToCurve);
		if (bCurve && Values != nullptr && Values->Num() > 1) {
			return SetCurve(*Node, *Input, *Values, ObjectField(*Distribution, TEXT("ValuesTimeRange"))) || Fail(InputName, TEXT("curve of this type"));
		}

		FVector4f Min, Max;
		const bool bMin = MinValue.IsValid() && ReadSample(*MinValue, Min);
		const bool bMax = MaxValue.IsValid() && ReadSample(*MaxValue, Max);
		if (Values != nullptr && !Values->IsEmpty()) {
			if (!(*Values)[0].IsValid() || !ReadSample(*(*Values)[0], Min)) return Fail(InputName, TEXT("unreadable value"));
			Max = Min;
			if (Values->Num() > 1 && (!(*Values)[1].IsValid() || !ReadSample(*(*Values)[1], Max))) return Fail(InputName, TEXT("unreadable value"));
		} else if (!bMin && !bMax) {
			return Fail(InputName, TEXT("unreadable value"));
		} else if (!bMin || !bMax) {
			/* One end left out is that end at the class default */
			FVector4f& Missing = bMin ? Max : Min;
			if (Default.bSet) {
				Missing = Default.Value;
				UsedDefault(Default, bMin ? TEXT("Max left out") : TEXT("Min left out"));
			} else {
				Missing = bMin ? Min : Max;
				Note(InputName, TEXT("one end of the range is the class default, not known here; used the other end"));
			}
		}

		if (Min == Max || Mode.EndsWith(TEXT("Constant"))) return SetLiteral(*Node, *Input, Min) || Fail(InputName, TEXT("value of this type"));
		return SetRange(*Node, *Input, Min, Max) || Fail(InputName, TEXT("range of this type"));
	}

	UNiagaraNodeFunctionCall* AddModule(UNiagaraGraph& Graph, ENiagaraScriptUsage Usage, const TCHAR* RelativePath) {
		UNiagaraNodeOutput* Output = Graph.FindEquivalentOutputNode(Usage);
		UNiagaraScript* Script = LoadNiagaraScript(RelativePath);
		return Output != nullptr && Script != nullptr
			? FNiagaraStackGraphUtilities::AddScriptModuleToStack(Script, *Output, INDEX_NONE, FString(), Script->GetExposedVersion().VersionGuid) : nullptr;
	}

	using EUsage = ENiagaraScriptUsage;

	struct FModuleInput {
		const TCHAR* Field;
		const TCHAR* Input;
	};

	bool ApplyModule(UNiagaraGraph& Graph, EUsage Usage, const TCHAR* Script, const FJsonObject& Properties, std::initializer_list<FModuleInput> Inputs) {
		UNiagaraNodeFunctionCall* Node = AddModule(Graph, Usage, Script);
		if (Node == nullptr) return false;
		bool bComplete = true;
		for (const FModuleInput& Input : Inputs) {
			bComplete &= SetDistribution(Node, Input.Input, ObjectField(Properties, Input.Field), DefaultFor(Input.Field));
		}
		return bComplete;
	}

	FUObjectExport* ResolveExport(FUObjectExportContainer& Exports, const FJsonObject& Owner, const TCHAR* Key) {
		const TSharedPtr<FJsonObject>* Ref = nullptr;
		return Owner.TryGetObjectField(Key, Ref) ? Exports.GetExportByObjectPath(*Ref) : FUObjectExport::EmptyExport();
	}

	void RestoreRenderers(UNiagaraEmitter& Emitter, const FJsonObject& Properties, FUObjectExportContainer& Exports,
		UObjectSerializer& Serializer, TSet<FString>& Unmapped) {
		const TArray<TSharedPtr<FJsonValue>>* Renderers = nullptr;
		if (!Properties.TryGetArrayField(TEXT("RendererProperties"), Renderers)) return;
		for (const TSharedPtr<FJsonValue>& Ref : *Renderers) {
			const TSharedPtr<FJsonObject>* Fields = nullptr;
			if (!Ref.IsValid() || !Ref->TryGetObject(Fields)) continue;
			FUObjectExport* Export = Exports.GetExportByObjectPath(*Fields);
			const UClass* Class = Export->IsJsonValid() ? Export->GetClass() : nullptr;
			if (Class == nullptr || !Class->IsChildOf(UNiagaraRendererProperties::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract)) {
				Unmapped.Add(TEXT("a renderer could not be restored"));
				continue;
			}

			/* The ordinary export pass has already made this renderer. Its own outer, the stateless
			 * emitter, is a class this engine hasn't got, so it was put under the system, where it
			 * would stay as an orphan beside a second copy made here. It is moved instead.
			 *
			 * Only out from under the system, though. Stateless emitters name their renderers the
			 * same (each has a NiagaraLightRendererProperties_0), and put under one outer they are
			 * one object: three light emitters' exports all came back as the same renderer, moved
			 * from one new emitter to the next, so all three ended up sharing the last one's light,
			 * radius and exponent included. A renderer another emitter already took is made again
			 * here from this export's own properties. */
			auto* Renderer = Cast<UNiagaraRendererProperties>(Export->Object);
			const bool bMoved = Renderer != nullptr && Renderer->GetClass() == Class && Renderer->GetOuter() == Emitter.GetOuter()
				&& Renderer->Rename(nullptr, &Emitter, REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
			/* The one object several exports came back as holds whichever of them was read last, so
			 * the one moved is given this export's own properties again */
			if (bMoved && Export->JsonObject->HasTypedField<EJson::Object>(TEXT("Properties"))) {
				int32 Sharing = 0;
				for (const FUObjectExport* Other : Exports.Exports) {
					if (Other != nullptr && Other->Object == Renderer) Sharing++;
				}
				if (Sharing > 1) Serializer.DeserializeObjectProperties(Export->GetProperties(), Renderer);
			}
			if (!bMoved) {
				Renderer = NewObject<UNiagaraRendererProperties>(&Emitter, Class, MakeUniqueObjectName(&Emitter, Class, Export->GetName()), RF_Transactional);
				Export->Object = Renderer;
				if (Export->JsonObject->HasTypedField<EJson::Object>(TEXT("Properties"))) {
					Serializer.DeserializeObjectProperties(Export->GetProperties(), Renderer);
				}
			}
			Emitter.AddRenderer(Renderer, Emitter.GetExposedVersion().VersionGuid);
		}
	}

	void RestoreSpawning(UNiagaraGraph& Graph, const FJsonObject& Properties, TSet<FString>& Unmapped) {
		UNiagaraNodeFunctionCall* State = AddModule(Graph, EUsage::EmitterUpdateScript, TEXT("Modules/Emitter/EmitterState"));
		if (State == nullptr) {
			Unmapped.Add(TEXT("Emitter State"));
		} else if (const FJsonObject* Settings = ObjectField(Properties, TEXT("EmitterState")); Settings == nullptr) {
			GModule = TEXT("EmitterState");
			const FDefault Duration = DefaultFor(TEXT("LoopDuration"));
			if (Duration.bSet) {
				UsedDefault(Duration, TEXT("the export has no Emitter State"));
				SetLiteral(State, TEXT("Loop Duration"), Duration.Value);
			} else {
				Note(TEXT("Loop Duration"), TEXT("the export has no Emitter State and no class default is known; the 5.4 module's own is used"));
			}
		} else {
			FString Loop;
			Settings->TryGetStringField(TEXT("LoopBehavior"), Loop);
			Loop = LeafEnum(Loop);
			GModule = TEXT("EmitterState");
			const bool bLifecycle = SetNamedSwitch(State, TEXT("Life Cycle Mode"), TEXT("Self"));
			const bool bLoop = Loop.IsEmpty() || SetNamedSwitch(State, TEXT("Loop Behavior"), Loop);
			/* 6.0 loops every second by default and 5.4's module every five */
			const bool bDuration = SetDistribution(State, TEXT("Loop Duration"), ObjectField(*Settings, TEXT("LoopDuration")), DefaultFor(TEXT("LoopDuration")));
			double LoopCount = 0;
			if (Settings->TryGetNumberField(TEXT("LoopCount"), LoopCount)) SetLiteral(State, TEXT("Loop Count"), FVector4f(LoopCount, LoopCount, LoopCount, LoopCount));
			for (const TCHAR* Unhandled : {TEXT("LoopDurationMode"), TEXT("InactiveResponse"), TEXT("bEnableDistanceCulling"), TEXT("bMaxDistanceEnabled"), TEXT("bMinDistanceEnabled")}) {
				if (Settings->HasField(Unhandled)) Note(Unhandled, TEXT("not converted"));
			}
			bool bDelay = false;
			Settings->TryGetBoolField(TEXT("bLoopDelayEnabled"), bDelay);
			const bool bDelaySet = !bDelay || (SetSwitch(State, TEXT("UseLoopDelay"), FString(TEXT("true")))
				&& SetDistribution(State, TEXT("Loop Delay"), ObjectField(*Settings, TEXT("LoopDelay")), DefaultFor(TEXT("LoopDelay"))));
			if (!bLifecycle || !bLoop || !bDuration || !bDelaySet) Unmapped.Add(TEXT("some Emitter State inputs"));
		}

		const TArray<TSharedPtr<FJsonValue>>* Spawns = nullptr;
		if (!Properties.TryGetArrayField(TEXT("SpawnInfos"), Spawns)) return;
		for (const TSharedPtr<FJsonValue>& Spawn : *Spawns) {
			const TSharedPtr<FJsonObject>* Settings = nullptr;
			if (!Spawn.IsValid() || !Spawn->TryGetObject(Settings) || !Settings->IsValid()) continue;
			bool bEnabled = true;
			(*Settings)->TryGetBoolField(TEXT("bEnabled"), bEnabled);
			if (!bEnabled) continue;

			FString Kind;
			(*Settings)->TryGetStringField(TEXT("Type"), Kind);
			Kind = LeafEnum(Kind);
			bool bProbability = false;
			(*Settings)->TryGetBoolField(TEXT("bSpawnProbabilityEnabled"), bProbability);
			bool bLoopLimit = false;
			(*Settings)->TryGetBoolField(TEXT("bLoopCountLimitEnabled"), bLoopLimit);
			GModule = Kind == TEXT("Burst") ? TEXT("SpawnBurst") : TEXT("SpawnRate");
			if (bLoopLimit) Note(TEXT("LoopCountLimit"), TEXT("not converted"));
			if (Kind == TEXT("Burst")) {
				UNiagaraNodeFunctionCall* Node = AddModule(Graph, EUsage::EmitterUpdateScript, TEXT("Modules/Emitter/SpawnBurst_Instantaneous"));
				double Time = 0;
				(*Settings)->TryGetNumberField(TEXT("SpawnTime"), Time);
				const bool bCount = SetDistribution(Node, TEXT("Spawn Count"), ObjectField(**Settings, TEXT("Amount")), DefaultFor(TEXT("Amount")));
				const bool bTime = Node != nullptr && SetLiteral(Node, TEXT("Spawn Time"), FVector4f(Time, Time, Time, Time));
				const bool bChance = !bProbability || SetDistribution(Node, TEXT("Spawn Probability"), ObjectField(**Settings, TEXT("SpawnProbability")), DefaultFor(TEXT("SpawnProbability")));
				if (Node == nullptr || !bCount || !bTime || !bChance) Unmapped.Add(TEXT("some burst spawn inputs"));
			} else if (Kind == TEXT("Rate")) {
				UNiagaraNodeFunctionCall* Node = AddModule(Graph, EUsage::EmitterUpdateScript, TEXT("Modules/Emitter/SpawnRate"));
				const bool bRate = SetDistribution(Node, TEXT("SpawnRate"), ObjectField(**Settings, TEXT("Rate")), DefaultFor(TEXT("Rate")));
				const bool bChance = !bProbability || SetDistribution(Node, TEXT("Spawn Probability"), ObjectField(**Settings, TEXT("SpawnProbability")), DefaultFor(TEXT("SpawnProbability")));
				if (Node == nullptr || !bRate || !bChance) Unmapped.Add(TEXT("some rate spawn inputs"));
			} else {
				Unmapped.Add(TEXT("spawn type ") + Kind);
			}
		}
	}


	bool HasDistributionValue(const FJsonObject* Properties) {
		if (Properties == nullptr) return false;
		FString Mode;
		return Properties->HasField(TEXT("Min")) || Properties->HasField(TEXT("Max")) || Properties->HasField(TEXT("Values"))
			|| (Properties->TryGetStringField(TEXT("Mode"), Mode) && LeafEnum(Mode) == TEXT("Binding"));
	}

	/* The one value a float distribution holds, where it holds one: left out (the class default),
	 * a constant, or a range or pair of values whose ends are the same. False for a real range, a
	 * curve or a binding. */
	bool ConstantOf(const FJsonObject* Distribution, float Default, float& Out) {
		Out = Default;
		if (Distribution == nullptr) return true;

		FString Mode;
		Distribution->TryGetStringField(TEXT("Mode"), Mode);
		Mode = LeafEnum(Mode);
		if (Mode == TEXT("Binding") || Mode.EndsWith(TEXT("Curve"))) return false;

		FVector4f Min(Default, Default, Default, Default);
		FVector4f Max = Min;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Distribution->TryGetArrayField(TEXT("Values"), Values) && !Values->IsEmpty()) {
			if (Values->Num() > 2 || !(*Values)[0].IsValid() || !ReadSample(*(*Values)[0], Min)) return false;
			Max = Min;
			if (Values->Num() > 1 && (!(*Values)[1].IsValid() || !ReadSample(*(*Values)[1], Max))) return false;
		} else {
			if (const TSharedPtr<FJsonValue> Field = Distribution->TryGetField(TEXT("Min")); Field.IsValid() && !ReadSample(*Field, Min)) return false;
			if (const TSharedPtr<FJsonValue> Field = Distribution->TryGetField(TEXT("Max")); Field.IsValid() && !ReadSample(*Field, Max)) return false;
		}

		if (!FMath::IsNearlyEqual(Min.X, Max.X)) return false;
		Out = Min.X;
		return true;
	}

	/* 6.0's stateless LightAttributes, onto 5.4's light renderer.
	 *
	 * In 6.0 the module writes per-light values the light renderer reads: radius, falloff exponent,
	 * diffuse scale, specular scale and volumetric scattering, each only where its bApply flag is
	 * on. 5.4's light renderer (CPU simple lights) has a constant for two of them. The radius a
	 * light gets with no Particles.LightRadius written is 100 times RadiusScale, and the exponent
	 * with no Particles.LightExponent written is DefaultExponent. So a constant radius or exponent
	 * goes onto every light renderer the emitter has, as RadiusScale (value / 100) and
	 * DefaultExponent. 5.4's simple lights have no diffuse or specular scale at all, and volumetric
	 * scattering only as a particle attribute, so those are reported when they differ from 1.
	 *
	 * An omitted distribution is the 6.0 class default, which for these is the engine's own light
	 * default: radius 100, everything else 1. */
	bool RestoreLightAttributes(UNiagaraEmitter& Emitter, const FJsonObject& Json) {
		TArray<UNiagaraLightRendererProperties*> Lights;
		for (UNiagaraRendererProperties* Renderer : Emitter.GetLatestEmitterData()->GetRenderers()) {
			if (UNiagaraLightRendererProperties* Light = Cast<UNiagaraLightRendererProperties>(Renderer)) Lights.Add(Light);
		}
		if (Lights.IsEmpty()) return Fail(TEXT("Lights"), TEXT("no light renderer to carry them"));

		const float DefaultRadius = FNiagaraConstants::GetAttributeWithDefaultValue(SYS_PARAM_PARTICLES_LIGHT_RADIUS).GetValue<float>();
		bool bComplete = true;
		const auto Applies = [&Json](const FString& Flag) {
			bool bApply = false;
			Json.TryGetBoolField(Flag, bApply);
			return bApply;
		};

		if (Applies(TEXT("bApplyRadius"))) {
			float Radius = 0;
			if (!ConstantOf(ObjectField(Json, TEXT("Radius")), DefaultFor(TEXT("Radius")).bSet ? DefaultFor(TEXT("Radius")).Value.X : DefaultRadius, Radius)) {
				bComplete = Fail(TEXT("Radius"), TEXT("range, curve or binding: 5.4's light renderer takes one radius"));
			} else if (DefaultRadius > 0) {
				for (UNiagaraLightRendererProperties* Light : Lights) Light->RadiusScale = Radius / DefaultRadius;
				if (!Json.HasField(TEXT("Radius")) || !HasDistributionValue(ObjectField(Json, TEXT("Radius")))) {
					UsedDefault(DefaultFor(TEXT("Radius")), TEXT("left out of the export"));
				}
			}
		}

		if (Applies(TEXT("bApplyFalloffExponent"))) {
			float Exponent = 0;
			if (!ConstantOf(ObjectField(Json, TEXT("FalloffExponent")), DefaultFor(TEXT("FalloffExponent")).bSet ? DefaultFor(TEXT("FalloffExponent")).Value.X : 1.0f, Exponent)) {
				bComplete = Fail(TEXT("FalloffExponent"), TEXT("range, curve or binding: 5.4's light renderer takes one exponent"));
			} else {
				for (UNiagaraLightRendererProperties* Light : Lights) {
					Light->DefaultExponent = Exponent;
					if (Light->bUseInverseSquaredFalloff) Note(TEXT("FalloffExponent"), TEXT("set, but unused with inverse squared falloff, as in 6.0"));
				}
			}
		}

		for (const TCHAR* Field : {TEXT("DiffuseScale"), TEXT("SpecularScale"), TEXT("VolumetricScattering")}) {
			if (!Applies(FString(TEXT("bApply")) + Field)) continue;
			float Value = 1;
			const bool bConstant = ConstantOf(ObjectField(Json, Field), DefaultFor(Field).bSet ? DefaultFor(Field).Value.X : 1.0f, Value);
			if (bConstant && FMath::IsNearlyEqual(Value, 1.0f)) continue;
			const FString Shown = bConstant ? FString::SanitizeFloat(Value) : FString(TEXT("not a constant"));
			const FString Why = FString(Field) == TEXT("VolumetricScattering")
				? FString::Printf(TEXT("%s; 5.4's light renderer reads it only from a particle attribute, which isn't written"), *Shown)
				: FString::Printf(TEXT("%s; 5.4's simple lights have no %s"), *Shown, FString(Field) == TEXT("DiffuseScale") ? TEXT("diffuse scale") : TEXT("specular scale"));
			bComplete = Fail(Field, Why);
		}

		return bComplete;
	}

	bool RestoreInitializeParticle(UNiagaraGraph& Graph, const FJsonObject& Properties) {
		UNiagaraNodeFunctionCall* Node = AddModule(Graph, EUsage::ParticleSpawnScriptInterpolated, TEXT("Modules/Spawn/Initialization/V2/InitializeParticle"));
		if (Node == nullptr) return false;

		bool bComplete = true;
		/* The 6.0 defaults match the 5.4 module's own, so they only fill in an end of a range
		 * the export left out. Ribbon width's isn't known (one use in the corpus). */
		struct FInitializationInput {
			const TCHAR* Field;
			const TCHAR* Input;
			const TCHAR* Switch;
			const TCHAR* Choice;
		};
		const FInitializationInput Inputs[] = {
			{TEXT("LifetimeDistribution"), TEXT("Lifetime"), TEXT("Lifetime Mode"), TEXT("Direct Set")},
			{TEXT("ColorDistribution"), TEXT("Color"), TEXT("Color Mode"), TEXT("Direct Set")},
			{TEXT("MassDistribution"), TEXT("Mass"), TEXT("Mass Mode"), TEXT("Direct Set")},
			{TEXT("SpriteSizeDistribution"), TEXT("Sprite Size"), TEXT("Sprite Size Mode"), TEXT("Non-Uniform")},
			{TEXT("MeshScaleDistribution"), TEXT("Mesh Scale"), TEXT("Mesh Scale Mode"), TEXT("Non-Uniform")},
			{TEXT("RibbonWidthDistribution"), TEXT("Ribbon Width"), TEXT("Ribbon Width Mode"), TEXT("Direct Set")},
			{TEXT("SpriteRotationDistribution"), TEXT("Sprite Rotation Angle"), TEXT("Sprite Rotation Mode"), TEXT("Direct Set")},
		};
		for (const FInitializationInput& Input : Inputs) {
			const FJsonObject* Distribution = ObjectField(Properties, Input.Field);
			if (!HasDistributionValue(Distribution)) continue;

			bComplete &= SetNamedSwitch(Node, Input.Switch, Input.Choice);
			bComplete &= SetDistribution(Node, Input.Input, Distribution, DefaultFor(Input.Field));
		}

		const FJsonObject* Position = ObjectField(Properties, TEXT("InitialPositionDistribution"));
		if (HasDistributionValue(Position)) {
			bComplete &= SetNamedSwitch(Node, TEXT("Position Mode"), TEXT("Simulation Position"));
			bComplete &= SetSwitch(Node, TEXT("UsePositionOffset"), FString(TEXT("true")));
			bComplete &= SetDistribution(Node, TEXT("Position Offset"), Position, DefaultFor(TEXT("InitialPositionDistribution")));
		}
		return bComplete;
	}

	bool RestoreMaterialParameters(UNiagaraGraph& Graph, const FJsonObject& Properties) {
		UNiagaraNodeFunctionCall* Node = AddModule(Graph, EUsage::ParticleUpdateScript, TEXT("Modules/Update/Material/DynamicMaterialParameters"));
		if (Node == nullptr) return false;

		bool bComplete = true;
		const TCHAR* const Channels[] = {TEXT("XChannelDistribution"), TEXT("YChannelDistribution"), TEXT("ZChannelDistribution"), TEXT("WChannelDistribution")};
		for (int32 Index = 0; Index < 4; ++Index) {
			const FJsonObject* Parameters = ObjectField(Properties, *FString::Printf(TEXT("Parameter%d"), Index));
			if (Parameters == nullptr) continue;
			bool bEnabled = Index == 0;
			Properties.TryGetBoolField(*FString::Printf(TEXT("bParameter%dEnabled"), Index), bEnabled);
			bComplete &= SetSwitch(Node, *FString::Printf(TEXT("Write Parameter Index %d"), Index), FString(bEnabled ? TEXT("true") : TEXT("false")));
			if (!bEnabled) continue;
			for (int32 Channel = 0; Channel < UE_ARRAY_COUNT(Channels); ++Channel) {
				const FJsonObject* Distribution = ObjectField(*Parameters, Channels[Channel]);
				if (!HasDistributionValue(Distribution)) continue;
				bComplete &= SetLiteral(Node, *FString::Printf(TEXT("Index%dParam%dWrite"), Index, Channel), FVector4f(1, 1, 1, 1));
				bComplete &= SetDistribution(Node, *FString::Printf(TEXT("Index %d Param %d"), Index, Channel + 1), Distribution);
			}
		}
		return bComplete;
	}

	bool SetEnumInput(UNiagaraNodeFunctionCall* Node, const TCHAR* Name, const FString& Choice) {
		const UNiagaraScriptVariable* Input = FindInput(Node, Name);
		const UEnum* Enum = Input != nullptr ? Input->Variable.GetType().GetEnum() : nullptr;
		if (Enum == nullptr) return Fail(Name, TEXT("no such input on the 5.4 module"));
		for (int32 Index = 0; Index + 1 < Enum->NumEnums(); ++Index) {
			if (Enum->GetNameStringByIndex(Index).Equals(Choice) || Enum->GetDisplayNameTextByIndex(Index).ToString().Equals(Choice)) {
				OverrideInput(*Node, *Input).DefaultValue = Enum->GetNameStringByIndex(Index);
				return true;
			}
		}
		return Fail(Name, FString::Printf(TEXT("no choice \"%s\""), *Choice));
	}

	bool ReadVector(const FJsonObject* Json, FVector3f& Out) {
		double X = 0, Y = 0, Z = 0;
		if (Json == nullptr || !Json->TryGetNumberField(TEXT("X"), X)) return false;
		Json->TryGetNumberField(TEXT("Y"), Y);
		Json->TryGetNumberField(TEXT("Z"), Z);
		Out = FVector3f(X, Y, Z);
		return true;
	}

	/* 6.0 AddVelocity is linear, from a point or in a cone (VelocityType, Linear when left out:
	 * the corpus carries FromPoint and InCone and never Linear). 5.4's AddVelocity module has the
	 * same three under its Velocity Mode switch. */
	bool RestoreAddVelocity(UNiagaraGraph& Graph, const FJsonObject& Json) {
		UNiagaraNodeFunctionCall* Node = AddModule(Graph, EUsage::ParticleSpawnScriptInterpolated, TEXT("Modules/Spawn/Velocity/AddVelocity"));
		if (Node == nullptr) return false;

		FString Kind;
		Json.TryGetStringField(TEXT("VelocityType"), Kind);
		Kind = LeafEnum(Kind);
		bool bComplete = true;

		/* The speeds and the linear velocity are where 6.0's defaults (100, and 0,0,100) and the
		 * 5.4 module's (25, and zero) differ, so they are set even when left out */
		if (Kind.IsEmpty() || Kind == TEXT("Linear")) {
			bComplete &= SetNamedSwitch(Node, TEXT("Velocity Mode"), TEXT("Linear"));
			bComplete &= SetDistribution(Node, TEXT("Velocity"), ObjectField(Json, TEXT("LinearVelocityDistribution")), DefaultFor(TEXT("LinearVelocityDistribution")));
			if (HasDistributionValue(ObjectField(Json, TEXT("LinearVelocityScale")))) Note(TEXT("LinearVelocityScale"), TEXT("not converted"));
		} else if (Kind == TEXT("FromPoint")) {
			bComplete &= SetNamedSwitch(Node, TEXT("Velocity Mode"), TEXT("From Point"));
			bComplete &= SetDistribution(Node, TEXT("Velocity Speed"), ObjectField(Json, TEXT("PointVelocityDistribution")), DefaultFor(TEXT("PointVelocityDistribution")));
			FVector3f Origin;
			if (ReadVector(ObjectField(Json, TEXT("PointOrigin")), Origin)) bComplete &= SetLiteral(Node, TEXT("Velocity Origin"), FVector4f(Origin.X, Origin.Y, Origin.Z, 0));
			if (HasDistributionValue(ObjectField(Json, TEXT("PointVelocityScale")))) Note(TEXT("PointVelocityScale"), TEXT("not converted"));
		} else if (Kind == TEXT("InCone")) {
			bComplete &= SetNamedSwitch(Node, TEXT("Velocity Mode"), TEXT("In Cone"));
			bComplete &= SetDistribution(Node, TEXT("Velocity Speed"), ObjectField(Json, TEXT("ConeVelocityDistribution")), DefaultFor(TEXT("ConeVelocityDistribution")));
			double Angle = 0;
			if (Json.TryGetNumberField(TEXT("ConeAngle"), Angle)) bComplete &= SetLiteral(Node, TEXT("Cone Angle"), FVector4f(Angle, Angle, Angle, Angle));
			if (Json.TryGetNumberField(TEXT("InnerCone"), Angle)) bComplete &= SetLiteral(Node, TEXT("Inner Cone Angle"), FVector4f(Angle, Angle, Angle, Angle));

			/* The 6.0 cone opens around +Z (its shader builds the direction from the angle off Z and
			 * then rotates it), and the 5.4 module's axis defaults to +X, so the axis is always set */
			FVector3f Axis(0, 0, 1);
			FString RotationType;
			Json.TryGetStringField(TEXT("ConeRotationType"), RotationType);
			const FJsonObject* Direction = ObjectField(Json, TEXT("ConeDirection"));
			const FJsonObject* Rotation = ObjectField(Json, TEXT("ConeRotation"));
			FVector3f Read;
			if (LeafEnum(RotationType) == TEXT("Direction") && Direction != nullptr && ReadVector(ObjectField(*Direction, TEXT("Min")), Read) && !Read.IsNearlyZero()) {
				Axis = Read.GetSafeNormal();
			} else if (Rotation != nullptr && ReadVector(ObjectField(*Rotation, TEXT("Min")), Read)) {
				Axis = FVector3f(FRotator::MakeFromEuler(FVector(Read)).RotateVector(FVector::ZAxisVector));
				Note(TEXT("ConeRotation"), TEXT("turned into a cone axis, read as roll/pitch/yaw degrees"));
			}
			bComplete &= SetLiteral(Node, TEXT("Cone Axis"), FVector4f(Axis.X, Axis.Y, Axis.Z, 0));

			bool bFalloff = false;
			Json.TryGetBoolField(TEXT("bSpeedFalloffFromConeAxisEnabled"), bFalloff);
			double Falloff = 0;
			if (bFalloff && Json.TryGetNumberField(TEXT("SpeedFalloffFromConeAxis"), Falloff)) {
				bComplete &= SetLiteral(Node, TEXT("UseConeFalloff"), FVector4f(1, 1, 1, 1));
				bComplete &= SetLiteral(Node, TEXT("Speed Falloff From Cone Axis"), FVector4f(Falloff, Falloff, Falloff, Falloff));
			}
			if (HasDistributionValue(ObjectField(Json, TEXT("ConeVelocityScale")))) Note(TEXT("ConeVelocityScale"), TEXT("not converted"));
		} else {
			bComplete = Fail(TEXT("VelocityType"), FString::Printf(TEXT("type %s"), *Kind));
		}

		FString Space;
		if (Json.TryGetStringField(TEXT("CoordinateSpace"), Space)) bComplete &= SetEnumInput(Node, TEXT("Coordinate Space"), LeafEnum(Space));
		return bComplete;
	}

	/* A field or input name reduced to what it says: no namespace, spaces or underscores, no bool
	 * "b" prefix or "Distribution" suffix, lower case. "SpriteSizeDistribution" and "Sprite Size"
	 * are then the same name. */
	FString PlainName(FString Name) {
		Name.RemoveFromStart(TEXT("Module."));
		if (Name.Len() > 1 && Name[0] == TEXT('b') && FChar::IsUpper(Name[1])) Name.RightChopInline(1);
		Name.RemoveFromEnd(TEXT("Distribution"));
		Name.ReplaceInline(TEXT(" "), TEXT(""));
		Name.ReplaceInline(TEXT("_"), TEXT(""));
		return Name.ToLower();
	}

	/* The 5.4 module a 6.0 stateless module with no conversion written for it most likely is: a
	 * script under /Niagara/Modules with the same name, the newest version folder (V3 over V2
	 * over none) where there are several */
	UNiagaraScript* FindModuleByName(const FString& Type) {
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		static bool bScanned = false;
		if (!bScanned) {
			Registry.ScanPathsSynchronous(TArray<FString>{FString(TEXT("/Niagara/Modules"))}, false);
			bScanned = true;
		}
		FARFilter Filter;
		Filter.PackagePaths.Add(FName(TEXT("/Niagara/Modules")));
		Filter.bRecursivePaths = true;
		Filter.ClassPaths.Add(UNiagaraScript::StaticClass()->GetClassPathName());
		TArray<FAssetData> Assets;
		Registry.GetAssets(Filter, Assets);

		const FString Wanted = PlainName(Type);
		const FAssetData* Best = nullptr;
		int32 BestVersion = -1;
		for (const FAssetData& Asset : Assets) {
			if (PlainName(Asset.AssetName.ToString()) != Wanted) continue;
			int32 Version = 0;
			const FString Folder = Asset.PackagePath.ToString();
			FString Leaf = FPaths::GetCleanFilename(Folder);
			if (Leaf.Len() > 1 && (Leaf[0] == TEXT('V') || Leaf[0] == TEXT('v')) && Leaf.RightChop(1).IsNumeric()) Version = FCString::Atoi(*Leaf.RightChop(1));
			if (Version > BestVersion) {
				Best = &Asset;
				BestVersion = Version;
			}
		}
		return Best != nullptr ? Cast<UNiagaraScript>(Best->GetAsset()) : nullptr;
	}

	/* Fields every stateless module carries that are about the module, not what it does */
	bool IsModuleBookkeeping(const FString& Field) {
		return Field == TEXT("bEnabled") || Field == TEXT("bDebugDrawEnabled") || Field.StartsWith(TEXT("bShow")) || Field.EndsWith(TEXT("Guid"));
	}

	/* A 6.0 stateless module no conversion is written for, put on the stack as the 5.4 module of
	 * the same name with each field set on the input of the same name (PlainName), so a module
	 * added to the engine later still comes across as far as its names agree. Every field that
	 * finds no input is reported by name, and the module as a whole is noted as matched by name. */
	bool RestoreByName(UNiagaraGraph& Graph, const FString& Type, const FJsonObject& Json) {
		UNiagaraScript* Script = FindModuleByName(Type);
		if (Script == nullptr) return false;

		const FVersionedNiagaraScriptData* ScriptData = Script->GetLatestScriptData();
		if (ScriptData == nullptr) return false;
		const TArray<ENiagaraScriptUsage> Usages = ScriptData->GetSupportedUsageContexts();
		const bool bSpawn = Usages.Contains(EUsage::ParticleSpawnScript) || Usages.Contains(EUsage::ParticleSpawnScriptInterpolated);
		const bool bUpdate = Usages.Contains(EUsage::ParticleUpdateScript);
		if (!bSpawn && !bUpdate) return Fail(TEXT("Module"), FString::Printf(TEXT("%s runs in neither particle spawn nor update"), *Script->GetPathName()));
		/* A module both stacks take goes where the 6.0 name says it runs */
		const bool bOnSpawn = bSpawn && (!bUpdate || Type.StartsWith(TEXT("Initial")) || Type.StartsWith(TEXT("Shape")) || Type.StartsWith(TEXT("Spawn")) || Type.Contains(TEXT("Location")));
		UNiagaraNodeOutput* Output = Graph.FindEquivalentOutputNode(bOnSpawn ? EUsage::ParticleSpawnScriptInterpolated : EUsage::ParticleUpdateScript);
		UNiagaraNodeFunctionCall* Node = Output != nullptr
			? FNiagaraStackGraphUtilities::AddScriptModuleToStack(Script, *Output, INDEX_NONE, FString(), Script->GetExposedVersion().VersionGuid) : nullptr;
		if (Node == nullptr) return Fail(TEXT("Module"), FString::Printf(TEXT("%s could not be added to the stack"), *Script->GetPathName()));

		Note(TEXT("Module"), FString::Printf(TEXT("no conversion is written for it; matched by name to %s and its fields to inputs of the same name"), *Script->GetPathName()));

		/* Inputs and switches by plain name */
		TMap<FString, FString> Inputs;
		TMap<FString, FString> Switches;
		if (UNiagaraGraph* Called = Node->GetCalledGraph()) {
			for (const auto& Pair : Called->GetAllMetaData()) {
				const FString Name = Pair.Key.GetName().ToString();
				if (Name.StartsWith(TEXT("Module."))) Inputs.Add(PlainName(Name), Name.RightChop(7));
			}
		}
		for (const UEdGraphPin* Pin : Node->Pins) {
			if (Pin->Direction == EGPD_Input && !Pin->PinName.ToString().Contains(TEXT("."))) Switches.Add(PlainName(Pin->PinName.ToString()), Pin->PinName.ToString());
		}

		bool bComplete = true;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Json.Values) {
			if (!Field.Value.IsValid() || IsModuleBookkeeping(Field.Key)) continue;
			const FString Plain = PlainName(Field.Key);
			const FString* Input = Inputs.Find(Plain);
			const FString* Switch = Switches.Find(Plain);

			const TSharedPtr<FJsonObject>* Object = nullptr;
			bool bBool = false;
			double Number = 0;
			FString Text;
			if (Field.Value->TryGetObject(Object) && Object->IsValid()) {
				if (!HasDistributionValue(Object->Get())) continue;
				if (Input == nullptr) {
					bComplete = Fail(*Field.Key, FString::Printf(TEXT("no input of that name on %s"), *Script->GetName()));
					continue;
				}
				bComplete &= SetDistribution(Node, **Input, Object->Get(), DefaultFor(*Field.Key));
			} else if (Field.Value->Type == EJson::Boolean && Field.Value->TryGetBool(bBool)) {
				if (Switch != nullptr) bComplete &= SetSwitch(Node, **Switch, FString(bBool ? TEXT("true") : TEXT("false")));
				else if (Input != nullptr) bComplete &= SetLiteral(Node, **Input, FVector4f(bBool, bBool, bBool, bBool));
				else bComplete = Fail(*Field.Key, FString::Printf(TEXT("no input or switch of that name on %s"), *Script->GetName()));
			} else if (Field.Value->Type == EJson::Number && Field.Value->TryGetNumber(Number)) {
				if (Input != nullptr) bComplete &= SetLiteral(Node, **Input, FVector4f(Number, Number, Number, Number));
				else if (Switch != nullptr) bComplete &= SetSwitch(Node, **Switch, FString::FromInt(FMath::RoundToInt(Number)));
				else bComplete = Fail(*Field.Key, FString::Printf(TEXT("no input of that name on %s"), *Script->GetName()));
			} else if (Field.Value->TryGetString(Text)) {
				const FString Choice = LeafEnum(Text);
				if (Switch != nullptr) bComplete &= SetNamedSwitch(Node, **Switch, Choice);
				else if (Input != nullptr) bComplete &= SetEnumInput(Node, **Input, Choice);
				else bComplete = Fail(*Field.Key, FString::Printf(TEXT("no input or switch of that name on %s"), *Script->GetName()));
			}
		}
		return bComplete;
	}

	bool RestoreModule(UNiagaraGraph& Graph, const FString& Type, const FJsonObject& Json) {
		if (Type == TEXT("InitializeParticle")) return RestoreInitializeParticle(Graph, Json);
		if (Type == TEXT("DynamicMaterialParameters")) return RestoreMaterialParameters(Graph, Json);

		if (Type == TEXT("InitialMeshOrientation")) {
			UNiagaraNodeFunctionCall* Node = AddModule(Graph, EUsage::ParticleSpawnScriptInterpolated, TEXT("Modules/Spawn/Orientation/V3/InitialMeshOrientation"));
			if (Node == nullptr) return false;
			FString Mode;
			Json.TryGetStringField(TEXT("MeshOrientationMode"), Mode);
			const bool bAxis = LeafEnum(Mode) == TEXT("OrientToAxis");
			bool bComplete = SetNamedSwitch(Node, TEXT("Mesh Orientation Mode"), bAxis ? TEXT("Orient to Vector") : TEXT("System"));
			bComplete &= SetDistribution(Node, TEXT("Mesh Axis To Orient"), ObjectField(Json, TEXT("MeshAxisToOrient")), DefaultFor(TEXT("MeshAxisToOrient")));
			bComplete &= SetDistribution(Node, TEXT("Orientation Vector"), ObjectField(Json, TEXT("OrientationVector")), DefaultFor(TEXT("OrientationVector")));
			bComplete &= SetDistribution(Node, TEXT("Rotation"), ObjectField(Json, TEXT("Rotation")), DefaultFor(TEXT("Rotation")));
			return bComplete;
		}

		if (Type == TEXT("ShapeLocation")) {
			UNiagaraNodeFunctionCall* Node = AddModule(Graph, EUsage::ParticleSpawnScriptInterpolated, TEXT("Modules/Spawn/Location/V2/ShapeLocation"));
			if (Node == nullptr) return false;
			FString Kind;
			Json.TryGetStringField(TEXT("ShapePrimitive"), Kind);
			Kind = LeafEnum(Kind);
			/* Left out, the shape is the class default, a sphere (the corpus carries Ring, Cylinder
			 * and Box and never Sphere). Every size defaults to 100 in both engines. */
			if (Kind.IsEmpty()) Kind = TEXT("Sphere");
			const bool bBoxOrPlane = Kind == TEXT("Box") || Kind == TEXT("Plane");
			const bool bRing = Kind == TEXT("Ring") || Kind == TEXT("Disc");
			bool bComplete = SetNamedSwitch(Node, TEXT("Shape Primitive"), bBoxOrPlane ? TEXT("Box / Plane") : bRing ? TEXT("Ring / Disc") : *Kind);
			if (bBoxOrPlane) bComplete &= SetNamedSwitch(Node, TEXT("Box / Plane Mode"), Kind);
			bComplete &= SetDistribution(Node, TEXT("Box Size"), ObjectField(Json, TEXT("BoxSize")), DefaultFor(TEXT("BoxSize")));
			bComplete &= SetDistribution(Node, TEXT("Plane Size"), ObjectField(Json, TEXT("PlaneSize")), DefaultFor(TEXT("PlaneSize")));
			bComplete &= SetDistribution(Node, TEXT("Sphere Radius"), ObjectField(Json, TEXT("SphereRadius")), DefaultFor(TEXT("SphereRadius")));
			bComplete &= SetDistribution(Node, TEXT("Cylinder Radius"), ObjectField(Json, TEXT("CylinderRadius")), DefaultFor(TEXT("CylinderRadius")));
			bComplete &= SetDistribution(Node, TEXT("Cylinder Height"), ObjectField(Json, TEXT("CylinderHeight")), DefaultFor(TEXT("CylinderHeight")));
			bComplete &= SetDistribution(Node, TEXT("Cylinder Height Midpoint"), ObjectField(Json, TEXT("CylinderHeightMidpoint")), DefaultFor(TEXT("CylinderHeightMidpoint")));
			bComplete &= SetDistribution(Node, TEXT("Ring Radius"), ObjectField(Json, TEXT("RingRadius")), DefaultFor(TEXT("RingRadius")));
			bComplete &= SetDistribution(Node, TEXT("Disc Coverage"), ObjectField(Json, TEXT("DiscCoverage")), DefaultFor(TEXT("DiscCoverage")));
			for (const TCHAR* Unhandled : {TEXT("ShapeRotation"), TEXT("ShapeScale"), TEXT("TransformOffset"), TEXT("TransformOffsetScale"), TEXT("RingUDistribution"),
				TEXT("BoxSurfaceThickness"), TEXT("PlaneEdgeThickness"), TEXT("CylinderSurfaceThickness")}) {
				if (HasDistributionValue(ObjectField(Json, Unhandled))) Note(Unhandled, TEXT("not converted"));
			}
			for (const TCHAR* Unhandled : {TEXT("bBoxSurfaceOnly"), TEXT("BoxSurfaceExpansion"), TEXT("CoordinateSpace")}) {
				if (Json.HasField(Unhandled)) Note(Unhandled, TEXT("not converted"));
			}
			return bComplete;
		}

		if (Type == TEXT("ScaleColor")) {
			UNiagaraNodeFunctionCall* Node = AddModule(Graph, EUsage::ParticleUpdateScript, TEXT("Modules/Update/Color/ScaleColor"));
			const FJsonObject* Distribution = ObjectField(Json, TEXT("ScaleDistribution"));
			FString Mode;
			if (Distribution != nullptr) Distribution->TryGetStringField(TEXT("Mode"), Mode);
			Mode = LeafEnum(Mode);
			/* Any curve over the particle's age (per channel, uniform or a gradient) is what the
			 * 5.4 module's color curve mode is */
			const bool bCurve = Mode.EndsWith(TEXT("Curve")) || Mode == TEXT("ColorGradient");
			const bool bMode = SetNamedSwitch(Node, TEXT("Scale Mode"), bCurve ? TEXT("RGBA Linear Color Curve") : TEXT("RGBA Together"));
			const bool bValue = SetDistribution(Node, bCurve ? TEXT("Linear Color Curve") : TEXT("Scale RGBA"), Distribution, DefaultFor(TEXT("ScaleDistribution")), bCurve);
			return bMode && bValue;
		}

		if (Type == TEXT("ScaleSpriteSize")) {
			UNiagaraNodeFunctionCall* Node = AddModule(Graph, EUsage::ParticleUpdateScript, TEXT("Modules/Update/Size/ScaleSpriteSize"));
			const bool bMode = SetNamedSwitch(Node, TEXT("Scale Sprite Size Mode"), TEXT("Non-Uniform"));
			const bool bValue = SetDistribution(Node, TEXT("Scale Factor"), ObjectField(Json, TEXT("ScaleDistribution")), DefaultFor(TEXT("ScaleDistribution")));
			return bMode && bValue;
		}

		if (Type == TEXT("ScaleMeshSize")) return ApplyModule(Graph, EUsage::ParticleUpdateScript, TEXT("Modules/Update/Size/ScaleMeshSize"),
			Json, {{TEXT("ScaleDistribution"), TEXT("Scale Factor")}});
		if (Type == TEXT("AddVelocity")) return RestoreAddVelocity(Graph, Json);
		if (Type == TEXT("Drag")) return ApplyModule(Graph, EUsage::ParticleUpdateScript, TEXT("Modules/Update/Forces/Drag"),
			Json, {{TEXT("DragDistribution"), TEXT("Drag")}});
		if (Type == TEXT("CameraOffset")) return ApplyModule(Graph, EUsage::ParticleUpdateScript, TEXT("Modules/Update/Camera/CameraOffset"),
			Json, {{TEXT("CameraOffsetDistribution"), TEXT("Camera Offset Amount")}});
		if (Type == TEXT("GravityForce")) return ApplyModule(Graph, EUsage::ParticleUpdateScript, TEXT("Modules/Update/Forces/GravityForce"),
			Json, {{TEXT("GravityDistribution"), TEXT("Gravity")}});
		if (Type == TEXT("AccelerationForce")) return ApplyModule(Graph, EUsage::ParticleUpdateScript, TEXT("Modules/Update/Forces/AccelerationForce"),
			Json, {{TEXT("AccelerationDistribution"), TEXT("Acceleration")}});
		if (Type == TEXT("SolveVelocitiesAndForces")) return AddModule(Graph, EUsage::ParticleUpdateScript, TEXT("Modules/Solvers/SolveForcesAndVelocity")) != nullptr;
		if (Type == TEXT("MeshRotationRate")) {
			UNiagaraNodeFunctionCall* Node = AddModule(Graph, EUsage::ParticleUpdateScript, TEXT("Modules/Update/Orientation/MeshRotationRate"));
			if (Node == nullptr) return false;

			const FJsonObject* Rate = ObjectField(Json, TEXT("RotationRateDistribution"));
			const FJsonObject* Scale = ObjectField(Json, TEXT("RateScaleDistribution"));
			bool bUseRateScale = false;
			Json.TryGetBoolField(TEXT("bUseRateScale"), bUseRateScale);
			if (!bUseRateScale || !HasDistributionValue(Scale)) return SetDistribution(Node, TEXT("Rotation Rate"), Rate, DefaultFor(TEXT("RotationRateDistribution")));

			const UNiagaraScriptVariable* Input = FindInput(Node, TEXT("Rotation Rate"));
			UNiagaraScript* Multiply = LoadNiagaraScript(TEXT("DynamicInputs/Multiply/Multiply_Vector"));
			if (Input == nullptr || Multiply == nullptr) return false;

			UNiagaraNodeFunctionCall* Product = nullptr;
			FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput(OverrideInput(*Node, *Input), Multiply, Product);
			// Cooked rate-scale curves may keep their samples while omitting the default mode.
			const bool bRate = Rate != nullptr ? SetDistribution(Product, TEXT("A"), Rate, DefaultFor(TEXT("RotationRateDistribution"))) : SetLiteral(Product, TEXT("A"), FVector4f(0, 0, 0, 0));
			const bool bScale = SetDistribution(Product, TEXT("B"), Scale, DefaultFor(TEXT("RateScaleDistribution")), true);
			return bRate && bScale;
		}

		if (Type == TEXT("SpriteRotationRate")) {
			UNiagaraNodeFunctionCall* Node = AddModule(Graph, EUsage::ParticleUpdateScript, TEXT("Modules/Update/Orientation/SpriteRotationRate"));
			if (Node == nullptr) return false;
			const FJsonObject* Rate = ObjectField(Json, TEXT("RotationRateDistribution"));
			const FJsonObject* Scale = ObjectField(Json, TEXT("RateScaleDistribution"));
			if (!HasDistributionValue(Scale)) return SetDistribution(Node, TEXT("Rotation Rate"), Rate, DefaultFor(TEXT("RotationRateDistribution")));

			/* A rate scaled over the particle's age, as MeshRotationRate's is */
			const UNiagaraScriptVariable* Input = FindInput(Node, TEXT("Rotation Rate"));
			UNiagaraScript* Multiply = LoadNiagaraScript(TEXT("DynamicInputs/Multiply/Multiply_Float"));
			if (Input == nullptr || Multiply == nullptr) return Fail(TEXT("Rotation Rate"), TEXT("rate scale"));
			UNiagaraNodeFunctionCall* Product = nullptr;
			FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput(OverrideInput(*Node, *Input), Multiply, Product);
			const bool bRate = Rate != nullptr ? SetDistribution(Product, TEXT("A"), Rate, DefaultFor(TEXT("RotationRateDistribution"))) : SetLiteral(Product, TEXT("A"), FVector4f(0, 0, 0, 0));
			const bool bScale = SetDistribution(Product, TEXT("B"), Scale, DefaultFor(TEXT("RateScaleDistribution")), true);
			return bRate && bScale;
		}

		if (Type == TEXT("CurlNoiseForce")) {
			UNiagaraNodeFunctionCall* Node = AddModule(Graph, EUsage::ParticleUpdateScript, TEXT("Modules/Update/Forces/CurlNoiseForce"));
			if (Node == nullptr) return false;
			/* Both multiply the same way in the 6.0 shader (sample position times frequency,
			 * strength as distance over the lifetime) and in the 5.4 module, so they are taken as
			 * they are. A left-out one is the class default, which isn't known here. */
			bool bComplete = true;
			for (const TPair<const TCHAR*, const TCHAR*>& Field : {TPair<const TCHAR*, const TCHAR*>(TEXT("NoiseStrength"), TEXT("Noise Strength")),
				TPair<const TCHAR*, const TCHAR*>(TEXT("NoiseFrequency"), TEXT("Noise Frequency"))}) {
				double Value = 0;
				if (Json.TryGetNumberField(Field.Key, Value)) bComplete &= SetLiteral(Node, Field.Value, FVector4f(Value, Value, Value, Value));
				else Note(Field.Value, TEXT("class default not known; left at the 5.4 module's"));
			}
			bool bBias = false;
			Json.TryGetBoolField(TEXT("bBiasNoiseFieldEnabled"), bBias);
			if (bBias) Note(TEXT("BiasNoiseField"), TEXT("not converted"));
			return bComplete;
		}

		if (Type == TEXT("ScaleSpriteSizeBySpeed")) {
			UNiagaraNodeFunctionCall* Node = AddModule(Graph, EUsage::ParticleUpdateScript, TEXT("Modules/Update/Size/ScaleSpriteSizeBySpeed"));
			if (Node == nullptr) return false;
			bool bComplete = SetDistribution(Node, TEXT("Velocity Threshold"), ObjectField(Json, TEXT("VelocityThreshold")), DefaultFor(TEXT("VelocityThreshold")));
			bComplete &= SetDistribution(Node, TEXT("Min Scale Factor"), ObjectField(Json, TEXT("MinScaleFactor")), DefaultFor(TEXT("MinScaleFactor")));
			bComplete &= SetDistribution(Node, TEXT("Max Scale Factor"), ObjectField(Json, TEXT("MaxScaleFactor")), DefaultFor(TEXT("MaxScaleFactor")));
			bool bByCurve = false;
			Json.TryGetBoolField(TEXT("bSampleScaleFactorByCurve"), bByCurve);
			const FJsonObject* Curve = ObjectField(Json, TEXT("SampleFactorCurve"));
			if (bByCurve && HasDistributionValue(Curve)) {
				bComplete &= SetLiteral(Node, TEXT("Sample Scale Factor By Curve"), FVector4f(1, 1, 1, 1));
				bComplete &= SetDistribution(Node, TEXT("Scale Factor Curve"), Curve, DefaultFor(TEXT("SampleFactorCurve")), true);
			}
			return bComplete;
		}
		return RestoreByName(Graph, Type, Json);
	}


	void ConvertEmitter(UNiagaraEmitter& Emitter, const FJsonObject& Properties, FUObjectExportContainer& Exports,
		UObjectSerializer& Serializer, TSet<FString>& Unmapped) {
		FVersionedNiagaraEmitterData* Data = Emitter.GetLatestEmitterData();
		Properties.TryGetBoolField(TEXT("bLocalSpace"), Data->bLocalSpace);
		/* bDeterministic in 6.0, bDeterminism before */
		if (!Properties.TryGetBoolField(TEXT("bDeterministic"), Data->bDeterminism)) Properties.TryGetBoolField(TEXT("bDeterminism"), Data->bDeterminism);
		Properties.TryGetNumberField(TEXT("RandomSeed"), Data->RandomSeed);

		RestoreRenderers(Emitter, Properties, Exports, Serializer, Unmapped);

		UNiagaraGraph& Graph = *CastChecked<UNiagaraScriptSource>(Data->SpawnScriptProps.Script->GetLatestSource())->NodeGraph;
		RestoreSpawning(Graph, Properties, Unmapped);

		AddModule(Graph, EUsage::ParticleSpawnScriptInterpolated, TEXT("Modules/Spawn/Location/SystemLocation"));
		AddModule(Graph, EUsage::ParticleUpdateScript, TEXT("Modules/Update/Lifetime/ParticleState"));

		const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
		if (Properties.TryGetArrayField(TEXT("Modules"), Modules)) {
			for (const TSharedPtr<FJsonValue>& Ref : *Modules) {
				const TSharedPtr<FJsonObject>* Fields = nullptr;
				if (!Ref.IsValid() || !Ref->TryGetObject(Fields)) continue;

				FUObjectExport* Export = Exports.GetExportByObjectPath(*Fields);
				const FString Type = Export->GetType().ToString().RightChop(FCString::Strlen(TEXT("NiagaraStatelessModule_")));
				const FJsonObject* Module = Export->IsJsonValid() ? ObjectField(*Export->JsonObject, TEXT("Properties")) : nullptr;
				bool bEnabled = true;
				if (Module != nullptr) Module->TryGetBoolField(TEXT("bEnabled"), bEnabled);
				if (!bEnabled) continue;
				const FJsonObject Default;
				GModule = Type;
				const int32 Before = GInputIssues != nullptr ? GInputIssues->Num() : 0;
				const FJsonObject& ModuleJson = Module != nullptr ? *Module : Default;
				/* Light attributes go onto the emitter's light renderers rather than into the graph */
				const bool bKnown = Export->IsJsonValid() && (Type == TEXT("LightAttributes")
					? RestoreLightAttributes(Emitter, ModuleJson)
					: RestoreModule(Graph, Type, ModuleJson));
				/* A module that failed on named inputs is said by them; one with nothing named is
				 * a module there is no 5.4 equivalent mapped for */
				if (!bKnown && (GInputIssues == nullptr || GInputIssues->Num() == Before)) {
					Unmapped.Add(Type.IsEmpty() ? TEXT("an unresolved module") : Type);
				}
			}
		}
		Graph.NotifyGraphChanged();
	}

}
#endif

bool FNiagaraStatelessFallback::Convert(UNiagaraSystem& System, const FJsonObject& Properties, FUObjectExportContainer& Exports,
	UObjectSerializer& Serializer, bool& bConverted) {
	bConverted = false;
#if UE5_1_BEYOND && WITH_EDITORONLY_DATA
	if (FindFProperty<FObjectPropertyBase>(FNiagaraEmitterHandle::StaticStruct(), TEXT("StatelessEmitter")) != nullptr) return true;

	const TArray<TSharedPtr<FJsonValue>>* Handles = nullptr;
	if (!Properties.TryGetArrayField(TEXT("EmitterHandles"), Handles)) return true;

	int32 Converted = 0;
	TSet<FString> Unmapped;
	TArray<FString> InputIssues;
	TArray<FString> InputNotes;
	TArray<FString> DefaultsUsed;
	GInputIssues = &InputIssues;
	GInputNotes = &InputNotes;
	GDefaultsUsed = &DefaultsUsed;
	ON_SCOPE_EXIT {
		GInputIssues = nullptr;
		GInputNotes = nullptr;
		GDefaultsUsed = nullptr;
	};
	for (const TSharedPtr<FJsonValue>& Entry : *Handles) {
		const TSharedPtr<FJsonObject>* Fields = nullptr;
		if (!Entry.IsValid() || !Entry->TryGetObject(Fields) || !Fields->IsValid()) continue;

		FString Mode;
		(*Fields)->TryGetStringField(TEXT("EmitterMode"), Mode);
		if (LeafEnum(Mode) != TEXT("Stateless") && ObjectField(**Fields, TEXT("StatelessEmitter")) == nullptr) continue;

		const FName Name(*(*Fields)->GetStringField(TEXT("Name")));
		FNiagaraEmitterHandle* Handle = System.GetEmitterHandles().FindByPredicate([Name](const FNiagaraEmitterHandle& Item) { return Item.GetName() == Name; });
		if (Handle == nullptr) return false;

		FUObjectExport* Source = ResolveExport(Exports, **Fields, TEXT("StatelessEmitter"));
		const FJsonObject* EmitterProperties = Source->IsJsonValid() ? ObjectField(*Source->JsonObject, TEXT("Properties")) : nullptr;
		if (EmitterProperties == nullptr) {
			FImportIssues::Report(EImportIssue::Data, TEXT("Stateless Niagara emitter export is missing"), Name.ToString());
			return false;
		}

		/* Imported again into the package an earlier import wrote, the system still holds the
		 * emitters that import made for this handle. Nothing points at them any more (the handle is
		 * about to), but they would be saved with it, so they are moved out first. */
		const FString Plain = Source->GetName().GetPlainNameString();
		TArray<UObject*> Held;
		GetObjectsWithOuter(&System, Held, false);
		for (UObject* Object : Held) {
			UNiagaraEmitter* Stale = Cast<UNiagaraEmitter>(Object);
			if (Stale == nullptr || Stale->GetFName().GetPlainNameString() != Plain) continue;
			const bool bUsed = System.GetEmitterHandles().ContainsByPredicate([Stale](const FNiagaraEmitterHandle& Item) { return Item.GetInstance().Emitter == Stale; });
			if (bUsed) continue;
			Stale->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
			Stale->ClearFlags(RF_Public | RF_Standalone);
		}

		UNiagaraEmitter* Emitter = NewObject<UNiagaraEmitter>(&System,
			MakeUniqueObjectName(&System, UNiagaraEmitter::StaticClass(), Source->GetName()), RF_Transactional);
		FString UniqueName;
		EmitterProperties->TryGetStringField(TEXT("UniqueEmitterName"), UniqueName);
		Emitter->SetUniqueEmitterName(UniqueName.IsEmpty() ? Name.ToString() : UniqueName);
		UNiagaraEmitterFactoryNew::InitializeEmitter(Emitter, false);
		Source->Object = Emitter;
		Handle->GetInstance() = FVersionedNiagaraEmitter(Emitter, Emitter->GetExposedVersion().VersionGuid);
		ConvertEmitter(*Emitter, *EmitterProperties, Exports, Serializer, Unmapped);
		++Converted;
	}

	if (Converted > 0) {
		bConverted = true;
		UE_LOG(LogReflection, Display, TEXT("Converted %d stateless Niagara emitter(s) to standard emitters in %s"), Converted, *System.GetName());
		if (!Unmapped.IsEmpty() || !InputIssues.IsEmpty()) {
			TArray<FString> Details = Unmapped.Array();
			Details.Append(InputIssues);
			Details.Sort();
			const FString Detail = FString::Join(Details, TEXT(", "));
			UE_LOG(LogReflection, Warning, TEXT("Some stateless Niagara settings in %s could not be converted: %s"), *System.GetName(), *Detail);
			FImportIssues::Report(EImportIssue::Data, TEXT("Some stateless Niagara settings could not be converted"), Detail);
		}
		if (!InputNotes.IsEmpty()) {
			InputNotes.Sort();
			const FString Detail = FString::Join(InputNotes, TEXT(", "));
			UE_LOG(LogReflection, Display, TEXT("Stateless Niagara settings in %s carried over approximately: %s"), *System.GetName(), *Detail);
			FImportIssues::Report(EImportIssue::Data, TEXT("Some stateless Niagara settings were carried over approximately or left out"), Detail);
		}
		if (!DefaultsUsed.IsEmpty()) {
			DefaultsUsed.Sort();
			const FString Detail = FString::Join(DefaultsUsed, TEXT(", "));
			UE_LOG(LogReflection, Display, TEXT("Stateless Niagara fields left out of %s were filled with class defaults from NiagaraStatelessDefaults.json: %s"), *System.GetName(), *Detail);
			FImportIssues::Report(EImportIssue::Data, TEXT("Stateless Niagara fields the export left out were filled with known class defaults (Resources/NiagaraStatelessDefaults.json)"), Detail);
		}
	}
#endif
	return true;
}
