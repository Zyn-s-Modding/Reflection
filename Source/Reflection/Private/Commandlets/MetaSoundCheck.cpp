/* Copyright Reflection Contributors 2024-2026 */

#include "CoreMinimal.h"
#include "Misc/Parse.h"

#if REFLECTION_METASOUND_EDITOR_GRAPH
#include "HAL/PlatformProcess.h"
#include "Importers/Types/Audio/MetaSoundEditorGraph.h"
#include "MetasoundAssetBase.h"
#include "MetasoundAssetManager.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendRegistries.h"
#include "MetasoundGenerator.h"
#include "MetasoundSource.h"
#include "MetasoundUObjectRegistry.h"
#include "Sound/SoundGenerator.h"

DECLARE_LOG_CATEGORY_CLASS(LogReflectMetaSound, All, All);

namespace {
	FString ClassTypeName(const EMetasoundFrontendClassType Type) {
		const UEnum* Enum = StaticEnum<EMetasoundFrontendClassType>();
		return Enum != nullptr ? Enum->GetNameStringByValue(static_cast<int64>(Type)) : FString::FromInt(static_cast<int32>(Type));
	}

	/* What a MetaSound's document holds, and the same for every MetaSound it uses */
	void ReportMetaSound(UObject* MetaSound, TSet<const UObject*>& Seen, const int32 Depth) {
		if (MetaSound == nullptr || Seen.Contains(MetaSound)) return;
		Seen.Add(MetaSound);

		FMetasoundAssetBase* Asset = Metasound::IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(MetaSound);
		if (Asset == nullptr) {
			UE_LOG(LogReflectMetaSound, Warning, TEXT("%*s%s is not a MetaSound"), Depth * 2, TEXT(""), *MetaSound->GetPathName());
			return;
		}

		const FMetasoundFrontendDocument& Document = Asset->GetDocumentChecked();
		const FMetasoundFrontendGraph& Graph = Document.RootGraph.Graph;

		TArray<FString> Interfaces;
		for (const FMetasoundFrontendVersion& Interface : Document.Interfaces) Interfaces.Add(Interface.ToString());

		TMap<FGuid, const FMetasoundFrontendClass*> Classes;
		for (const FMetasoundFrontendClass& Class : Document.Dependencies) Classes.Add(Class.ID, &Class);

		TMap<FString, int32> Kinds;
		int32 Placed = 0, Unknown = 0;
		for (const FMetasoundFrontendNode& Node : Graph.Nodes) {
			const FMetasoundFrontendClass* const* Class = Classes.Find(Node.ClassID);
			if (Class == nullptr) {
				Unknown++;
				continue;
			}
			const FString Kind = ClassTypeName((*Class)->Metadata.GetType());
			Kinds.FindOrAdd(Kind)++;
			if (!Node.Style.Display.Locations.IsEmpty()) Placed++;
		}

		TArray<FString> KindText;
		for (const TPair<FString, int32>& Kind : Kinds) KindText.Add(FString::Printf(TEXT("%s %d"), *Kind.Key, Kind.Value));

		UE_LOG(LogReflectMetaSound, Display, TEXT("%*s%s: class %s v%s, editor graph %s, %d node(s) (%s%s), %d placed, %d edge(s), %d variable(s), interfaces [%s]"),
			Depth * 2, TEXT(""), *MetaSound->GetPathName(), *Document.RootGraph.Metadata.GetClassName().ToString(),
			*Document.RootGraph.Metadata.GetVersion().ToString(), Asset->GetGraph() != nullptr ? TEXT("yes") : TEXT("no"),
			Graph.Nodes.Num(), *FString::Join(KindText, TEXT(", ")), Unknown > 0 ? *FString::Printf(TEXT(", %d with no class"), Unknown) : TEXT(""),
			Placed, Graph.Edges.Num(), Graph.Variables.Num(), *FString::Join(Interfaces, TEXT(", ")));

		TArray<FString> Keys;
		for (const FString& Key : Asset->GetReferencedAssetClassKeys()) Keys.Add(Key);
		if (Keys.Num() > 0) UE_LOG(LogReflectMetaSound, Display, TEXT("%*s  uses by key: %s"), Depth * 2, TEXT(""), *FString::Join(Keys, TEXT(", ")));

		for (FMetasoundAssetBase* Reference : Asset->GetReferencedAssets()) {
			if (Reference != nullptr) ReportMetaSound(Reference->GetOwningAsset(), Seen, Depth + 1);
		}
	}

	/* Plays a MetaSound source into a buffer for a while and says how loud it came out, second by
	 * second. With bCooked, it is registered the way a cooked game registers it, which does not
	 * bring its nodes up to this engine's first. */
	int32 RenderMetaSound(UMetaSoundSource* Source, const float Seconds, const bool bCooked) {
		using namespace Metasound;
		using namespace Metasound::Frontend;

		FMetaSoundAssetRegistrationOptions Options;
		Options.bForceReregister = true;
		Options.bRegisterDependencies = true;
		Options.bAutoUpdate = !bCooked;
		Options.bAutoUpdateLogWarningOnDroppedConnection = true;
		Source->RegisterGraphWithFrontend(Options);

		const bool bHasGraph = FMetasoundFrontendRegistryContainer::Get()->GetGraph(Source->GetRegistryKey()).IsValid();
		UE_LOG(LogReflectMetaSound, Display, TEXT("registered %s (%s): runtime graph %s"), *Source->GetName(),
			bCooked ? TEXT("as a cooked game would, no auto-update") : TEXT("as the editor does"), bHasGraph ? TEXT("built") : TEXT("missing"));

		FSoundGeneratorInitParams Init;
		Init.AudioDeviceID = 0;
		Init.SampleRate = 48000.0f;
		Init.NumChannels = FMath::Max(1, Source->NumChannels);
		Init.NumFramesPerCallback = 512;
		Init.AudioMixerNumOutputFrames = 512;
		Init.InstanceID = 0x5EED;
		Init.GraphName = Source->GetName();

		TArray<FAudioParameter> Defaults;
		static_cast<USoundBase*>(Source)->GetAllDefaultParameters(Defaults);
		Source->InitParameters(Defaults, NAME_None);

		ISoundGeneratorPtr Generator = Source->CreateSoundGenerator(Init, MoveTemp(Defaults));
		if (!Generator.IsValid()) {
			UE_LOG(LogReflectMetaSound, Error, TEXT("no sound generator: nothing for it to play"));
			return 1;
		}

		Generator->OnBeginGenerate();
		const int32 Samples = Init.NumFramesPerCallback * Init.NumChannels;
		TArray<float> Buffer;
		Buffer.SetNumZeroed(Samples);

		/* The operator is built on a task and only taken up by a render, so renders are asked
		 * for (and give silence without moving on) until one has taken it */
		FThreadSafeBool bBuilt = false;
		FMetasoundGenerator* Built = static_cast<FMetasoundGenerator*>(Generator.Get());
		const FDelegateHandle Handle = Built->AddGraphSetCallback(FOnSetGraph::FDelegate::CreateLambda([&bBuilt]() { bBuilt = true; }));
		for (int32 Wait = 0; Wait < 1000 && !bBuilt; ++Wait) {
			FPlatformProcess::Sleep(0.01f);
			if (!bBuilt) Generator->GetNextBuffer(Buffer.GetData(), Samples, true);
		}
		Built->RemoveGraphSetCallback(Handle);
		UE_LOG(LogReflectMetaSound, Display, TEXT("operator %s"), bBuilt ? TEXT("built") : TEXT("never built (10 s)"));

		const int32 BlocksPerSecond = FMath::CeilToInt(Init.SampleRate / Init.NumFramesPerCallback);
		int32 Loud = 0;
		for (int32 Second = 0; Second < FMath::CeilToInt(Seconds); ++Second) {
			double Peak = 0.0, Squares = 0.0;
			int64 Counted = 0;
			for (int32 Block = 0; Block < BlocksPerSecond; ++Block) {
				FMemory::Memzero(Buffer.GetData(), Samples * sizeof(float));
				Generator->GetNextBuffer(Buffer.GetData(), Samples, true);
				for (const float Sample : Buffer) {
					Peak = FMath::Max(Peak, static_cast<double>(FMath::Abs(Sample)));
					Squares += Sample * Sample;
				}
				Counted += Samples;
				/* Decoding runs beside the render, the way a device would pace it */
				FPlatformProcess::Sleep(0.0005f);
			}
			const double Rms = FMath::Sqrt(Squares / FMath::Max<int64>(1, Counted));
			if (Peak > 1e-4) Loud++;
			UE_LOG(LogReflectMetaSound, Display, TEXT("  second %d: peak %.4f, rms %.4f%s"), Second, Peak, Rms, Generator->IsFinished() ? TEXT(" (finished)") : TEXT(""));
		}
		Generator->OnEndGenerate();
		Source->OnEndGenerate(Generator);

		UE_LOG(LogReflectMetaSound, Display, TEXT("%s: %d of %d second(s) had sound"), *Source->GetName(), Loud, FMath::CeilToInt(Seconds));
		return Loud > 0 ? 0 : 1;
	}
}

/* -metasoundcheck=/Mount/Path/MetaSound: says what a MetaSound already in the project holds and
 * what it uses, without importing or saving anything.
 *   -setupgraph   gives it, and what it uses, the editor graph an import now gives
 *   -rawinitgraph has the MetaSound editor set up its graph itself, the way opening it in the
 *                 editor does for one that has none
 *   -render=<s>   plays it for that many seconds and says how loud it was
 *   -cooked       registers it for -render the way a cooked game does */
bool RunMetaSoundCheck(const FString& Params, int32& OutResult) {
	FString Checked;
	if (!FParse::Value(*Params, TEXT("metasoundcheck="), Checked) || Checked.IsEmpty()) return false;
	if (!Checked.Contains(TEXT("."))) Checked += TEXT(".") + FPackageName::GetShortName(Checked);

	UObject* MetaSound = LoadObject<UObject>(nullptr, *Checked);
	if (MetaSound == nullptr || Metasound::IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(MetaSound) == nullptr) {
		UE_LOG(LogReflectMetaSound, Error, TEXT("there is no MetaSound at \"%s\""), *Checked);
		OutResult = 1;
		return true;
	}

	{
		TSet<const UObject*> Seen;
		ReportMetaSound(MetaSound, Seen, 0);
	}

	if (FParse::Param(*Params, TEXT("rawinitgraph"))) {
		UE_LOG(LogReflectMetaSound, Display, TEXT("--- the editor setting up its own graph"));
		UMetaSoundEditorSubsystem::GetChecked().InitEdGraph(*MetaSound);
		TSet<const UObject*> Seen;
		ReportMetaSound(MetaSound, Seen, 0);
	}

	if (FParse::Param(*Params, TEXT("setupgraph"))) {
		UE_LOG(LogReflectMetaSound, Display, TEXT("--- setting up editor graphs the way an import does"));
		TArray<UObject*> Repaired;
		EnsureMetaSoundEditorGraph(MetaSound, &Repaired);
		UE_LOG(LogReflectMetaSound, Display, TEXT("%d MetaSound(s) it uses were mended too"), Repaired.Num());
		TSet<const UObject*> Seen;
		ReportMetaSound(MetaSound, Seen, 0);
	}

	OutResult = 0;
	float Seconds = 0.0f;
	if (FParse::Value(*Params, TEXT("render="), Seconds) && Seconds > 0.0f) {
		UMetaSoundSource* Source = Cast<UMetaSoundSource>(MetaSound);
		if (Source == nullptr) {
			UE_LOG(LogReflectMetaSound, Error, TEXT("%s is not a MetaSound source, so there is nothing to play"), *MetaSound->GetPathName());
			OutResult = 1;
		} else {
			OutResult = RenderMetaSound(Source, Seconds, FParse::Param(*Params, TEXT("cooked")));
		}
	}

	return true;
}
#else
bool RunMetaSoundCheck(const FString& Params, int32& OutResult) {
	return false;
}
#endif
