/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Cloud/Prefetch.h"

#include <atomic>

#include "Async/Async.h"
#include "HAL/IConsoleManager.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Misc/Optional.h"

#include "Engine/Compatibility.h"
#include "Engine/Log.h"
#include "Importers/Types/Texture/TextureTypes.h"
#include "Modules/Cloud/Cloud.h"
#include "Modules/Cloud/Remote.h"

/* Named rather than anonymous: a unity build folds every anonymous namespace in a blob into one, and
 * names this plain would collide with whatever file lands beside this one */
namespace ReflectionCloudPrefetch {
	TAutoConsoleVariable<int32> CVarPrefetch(
		TEXT("Reflection.Cloud.Prefetch"),
		1,
		TEXT("Fetch Cloud exports ahead of an import reaching them. 0 asks for each one as it is reached, as before.")
	);

	TAutoConsoleVariable<int32> CVarConnections(
		TEXT("Reflection.Cloud.Prefetch.Connections"),
		6,
		TEXT("How many Cloud requests are fetched ahead at once.")
	);

	TAutoConsoleVariable<int32> CVarRunAhead(
		TEXT("Reflection.Cloud.Prefetch.RunAhead"),
		8,
		TEXT("How many of a run's own assets are held ahead of the one being imported.")
	);

	TAutoConsoleVariable<int32> CVarReferencesAhead(
		TEXT("Reflection.Cloud.Prefetch.ReferencesAhead"),
		24,
		TEXT("How many references are held ahead of an import reaching them.")
	);

	/* As long as a blocking request is given, so a wait taken from here ends where the request it
	 * stands in for would have */
	constexpr double WaitSeconds = 30.0;

	constexpr float PollSeconds = 0.001f;

	enum class EKind : uint8 { Raw, Binary };

	/* Indexes GAhead, so the values are spelled out */
	enum class EOrigin : uint8 { Run = 0, Reference = 1 };

	enum EStage : uint8 { Waiting, Answered, Failed };

	/* The half of an entry other threads write to.
	 *
	 * The HTTP thread finishes the request and a pool thread takes the answer apart, and neither of
	 * them touches anything but this. The game thread reads nothing in it until Stage says it is
	 * done, which is what hands the rest over. */
	struct FJob {
		std::atomic<bool> bClaimed { false };
		std::atomic<uint8> Stage { Waiting };
		FReflectionHttpResponse Response;
		TSharedPtr<FJsonObject> Raw;
	};

	using FJobPtr = TSharedPtr<FJob, ESPMode::ThreadSafe>;

	struct FEntry {
		EKind Kind = EKind::Raw;
		EOrigin Origin = EOrigin::Run;
		FString Key;

		/* What it is asked for as, and for the pixels, which encoding and which export */
		FString Path;
		FString ContentType;
		FString Named;

		bool bStarted = false;
		bool bFinished = false;
		bool bDropped = false;

		TOptional<FReflectionHttpRequest> Request;
		FJobPtr Job;
	};

	using FEntryPtr = TSharedPtr<FEntry>;

	/* Every entry not yet taken, by what it answers */
	TMap<FString, FEntryPtr> GEntries;

	/* References and pixel requests waiting to start, ahead of the run's own metadata. */
	TArray<FEntryPtr> GWaiting;

	/* Started and not yet seen to finish */
	TArray<FEntryPtr> GRunning;

	/* A run's paths in its order, one slot each, empty where nothing was queued for it */
	TArray<FEntryPtr> GRun;
	int32 GRunNext = 0;
	/* Start run requests in place so large folder queues only need one pass. */
	int32 GRunWaitingNext = 0;

	/* Separate limits for metadata and pixels so a full metadata window leaves room for pixels. */
	int32 GAhead[2][2] = {};

	int32 GScopes = 0;

	/* The reader pressed Cancel, so nothing new is started until the run is over */
	bool GCancelled = false;

	bool IsEnabled() {
		return IsInGameThread() && GScopes > 0 && !GCancelled && CVarPrefetch.GetValueOnAnyThread() != 0;
	}

	/* Everything Cloud reads of a path, which is as far as its first dot */
	FString PackageOf(const FString& Path) {
		int32 Dot = INDEX_NONE;

		return Path.FindChar(TEXT('.'), Dot) ? Path.Left(Dot) : Path;
	}

	/* Paths Cloud serves as files rather than as packages, extension and all */
	bool IsPackagePath(const FString& Path) {
		if (Path.IsEmpty()) return false;

		static const TCHAR* Files[] = {
			TEXT(".uplugin"), TEXT(".ini"), TEXT(".json"), TEXT(".txt"), TEXT(".xml"), TEXT(".csv"), TEXT(".uproject")
		};

		for (const TCHAR* Extension : Files) {
			if (Path.EndsWith(Extension, ESearchCase::IgnoreCase)) return false;
		}

		return true;
	}

	/* Keyed by the build as well, since the same path can be two different assets in two of them */
	FString RawKey(const FString& Path) {
		return Cloud::Build + TEXT("|raw|") + PackageOf(Path);
	}

	FString PixelKey(const FString& RawKey) {
		return RawKey + TEXT("|pixels");
	}

	/* Called once per request, from whichever thread the request finished on */
	void Finish(const FJobPtr& Job, const FReflectionHttpResponse& Response, const bool bSucceeded, const bool bDecode) {
		if (Job->bClaimed.exchange(true)) return;

		if (!bSucceeded || !Response.IsValid()) {
			Job->Stage.store(Failed);

			return;
		}

		if (!bDecode) {
			Job->Response = Response;
			Job->Stage.store(Answered);

			return;
		}

		Async(EAsyncExecution::ThreadPool, [Job, Response]() {
			Job->Raw = Cloud::DecodeResponse(Response);
			Job->Stage.store(Answered);
		});
	}

	void Start(const FEntryPtr& Entry) {
		const bool bDecode = Entry->Kind == EKind::Raw;
		const FReflectionHttpRequest Request = bDecode
			? Cloud::BuildRequest(Cloud::ExportURL, { { TEXT("path"), Entry->Path }, { TEXT("raw"), TEXT("true") } }, {})
			: Cloud::Export::BuildBinaryRequest(Entry->Path, Entry->ContentType, Entry->Named);

		if (bDecode) {
			Request->SetVerb(TEXT("GET"));
			Cloud::AcceptStructured(Request);
		}

		const FJobPtr Job = MakeShared<FJob, ESPMode::ThreadSafe>();

#if UE5_3_BEYOND
		/* Finished on the HTTP thread, so the answer is taken apart the moment it lands rather than
		 * the next time something on the game thread happens to tick the HTTP manager */
		Request->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnHttpThread);
#endif

		Request->OnProcessRequestComplete().BindLambda(
			[Job, bDecode](FHttpRequestPtr, FHttpResponsePtr Response, const bool bSucceeded) {
				Finish(Job, Response, bSucceeded, bDecode);
			}
		);

		Entry->Job = Job;
		Entry->Request = Request;
		Entry->bStarted = true;

		GAhead[static_cast<int32>(Entry->Origin)][static_cast<int32>(Entry->Kind)]++;
		GRunning.Add(Entry);

		if (!Request->ProcessRequest()) {
			Finish(Job, nullptr, false, false);
		}
	}

	void Drop(const FEntryPtr& Entry) {
		if (!Entry.IsValid() || Entry->bDropped) return;

		Entry->bDropped = true;

		GEntries.Remove(Entry->Key);

		if (!Entry->bStarted) {
			GWaiting.Remove(Entry);

			return;
		}

		GAhead[static_cast<int32>(Entry->Origin)][static_cast<int32>(Entry->Kind)]--;
		GRunning.RemoveSwap(Entry);

		if (!Entry->bFinished && Entry->Request.IsSet()) {
			Entry->Request.GetValue()->CancelRequest();
		}

		/* Whatever it decoded goes with it, not whenever the run ends */
		Entry->Job.Reset();
	}

	/* Which export FTextureImport::FromCloud takes out of an answer, picked the same way */
	TSharedPtr<FJsonObject> TextureExportOf(const TArray<TSharedPtr<FJsonValue>>& Exports, const FString& Path) {
		const auto ObjectAt = [&Exports](const int32 At) -> TSharedPtr<FJsonObject> {
			return Exports.IsValidIndex(At) && Exports[At].IsValid() && Exports[At]->Type == EJson::Object
				? Exports[At]->AsObject()
				: nullptr;
		};

		/* A reference names it after its last dot, a path typed in after its last slash */
		FString Leaf;

		if (!Path.Split(TEXT("."), nullptr, &Leaf, ESearchCase::CaseSensitive, ESearchDir::FromEnd)) {
			Path.Split(TEXT("/"), nullptr, &Leaf, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		}

		int32 Wanted = INDEX_NONE;

		if (Leaf.IsNumeric()) {
			const int32 At = FCString::Atoi(*Leaf);

			if (Exports.IsValidIndex(At)) Wanted = At;
		} else if (!Leaf.IsEmpty()) {
			for (int32 At = 0; At < Exports.Num() && Wanted == INDEX_NONE; ++At) {
				FString Named;

				if (const TSharedPtr<FJsonObject> One = ObjectAt(At); One.IsValid() && One->TryGetStringField(TEXT("Name"), Named) && Named == Leaf) {
					Wanted = At;
				}
			}
		}

		for (int32 At = 0; At < Exports.Num() && Wanted == INDEX_NONE; ++At) {
			FString Type;

			if (const TSharedPtr<FJsonObject> One = ObjectAt(At); One.IsValid() && One->TryGetStringField(TEXT("Type"), Type) && FTextureTypes::IsSupported(Type)) {
				Wanted = At;
			}
		}

		return ObjectAt(Wanted == INDEX_NONE ? 0 : Wanted);
	}

	/* A texture's pixels are a second request that can only be made once its export is in, since
	 * the export says which encoding to ask for. Made here the moment it is, rather than when the
	 * import reaches it: encoding pixels is the slowest thing Cloud does. */
	void QueuePixels(const FEntryPtr& Raw) {
		if (!IsEnabled() || !Raw->Job.IsValid() || !Raw->Job->Raw.IsValid()) return;

		const TArray<TSharedPtr<FJsonValue>>* Exports = nullptr;

		if (!Raw->Job->Raw->TryGetArrayField(TEXT("exports"), Exports) || Exports == nullptr || Exports->Num() == 0) return;

		const TSharedPtr<FJsonObject> Export = TextureExportOf(*Exports, Raw->Path);

		FString Type;

		if (!Export.IsValid() || !Export->TryGetStringField(TEXT("Type"), Type)) return;
		if (!FTextureTypes::IsSupported(Type) || !FTextureTypes::HasPixelPayload(Type)) return;

		FString Named;
		Export->TryGetStringField(TEXT("Name"), Named);

		const FString Key = PixelKey(Raw->Key);

		if (GEntries.Contains(Key)) return;

		const FEntryPtr Pixels = MakeShared<FEntry>();
		Pixels->Kind = EKind::Binary;
		Pixels->Origin = Raw->Origin;
		Pixels->Key = Key;
		Pixels->Path = Raw->Path;
		Pixels->ContentType = FTextureTypes::PixelContentType(Type, Export);
		Pixels->Named = Named;

		GEntries.Add(Key, Pixels);

		/* Wanted straight after the export it belongs to, so it goes first */
		GWaiting.Insert(Pixels, 0);
	}

	void Poll() {
		for (int32 Index = GRunning.Num() - 1; Index >= 0; --Index) {
			const FEntryPtr Entry = GRunning[Index];

			if (!Entry->Job.IsValid() || Entry->Job->Stage.load() == Waiting) continue;

			GRunning.RemoveAtSwap(Index);
			Entry->bFinished = true;
			Entry->Request.Reset();

			if (Entry->Kind == EKind::Raw && Entry->Job->Stage.load() == Answered) {
				QueuePixels(Entry);
			}
		}
	}

	void StartWhatCan() {
		if (!IsEnabled()) return;

		const int32 Connections = FMath::Max(1, CVarConnections.GetValueOnAnyThread());
		const int32 Limits[2] = {
			FMath::Max(1, CVarRunAhead.GetValueOnAnyThread()),
			FMath::Max(0, CVarReferencesAhead.GetValueOnAnyThread())
		};

		for (int32 Index = 0; Index < GWaiting.Num() && GRunning.Num() < Connections;) {
			const FEntryPtr Entry = GWaiting[Index];
			const int32 Origin = static_cast<int32>(Entry->Origin);

			if (GAhead[Origin][static_cast<int32>(Entry->Kind)] >= Limits[Origin]) {
				++Index;

				continue;
			}

			GWaiting.RemoveAt(Index);
			Start(Entry);
		}

		while (GRunWaitingNext < GRun.Num() && GRunning.Num() < Connections
			&& GAhead[static_cast<int32>(EOrigin::Run)][static_cast<int32>(EKind::Raw)] < Limits[0]) {
			const FEntryPtr& Entry = GRun[GRunWaitingNext++];
			if (Entry.IsValid() && !Entry->bStarted && !Entry->bDropped) {
				Start(Entry);
			}
		}
	}

	enum class EWaited : uint8 { Done, TimedOut, Cancelled, Dropped };

	/* Waits the way FRemoteUtilities::ExecuteRequestBlocking does: ticking HTTP, keeping the editor
	 * drawn and listening for Cancel, while everything else in flight carries on */
	EWaited WaitFor(const FEntryPtr& Entry) {
		/* Wanted now, so it does not wait its turn */
		if (!Entry->bStarted) {
			GWaiting.Remove(Entry);
			Start(Entry);
		}

		double Last = FPlatformTime::Seconds();
		const double Deadline = Last + WaitSeconds;

		for (;;) {
			Poll();

			if (Entry->bDropped) return EWaited::Dropped;
			if (Entry->bFinished) return EWaited::Done;

			StartWhatCan();

			const double Now = FPlatformTime::Seconds();

			FHttpModule::Get().GetHttpManager().Tick(static_cast<float>(Now - Last));
			Last = Now;

			if (!Entry->Job.IsValid() || Entry->Job->Stage.load() != Waiting) continue;

			if (FBlockingRequestScope::Pump()) return EWaited::Cancelled;
			if (Now >= Deadline) return EWaited::TimedOut;

			FPlatformProcess::Sleep(PollSeconds);
		}
	}

	/* Waits for Key's entry and takes it, false where there is none to take or the caller should
	 * ask for itself */
	bool Take(const FString& Key, FJobPtr& OutJob, bool& bOutAnswered) {
		const FEntryPtr Entry = GEntries.FindRef(Key);

		if (!Entry.IsValid()) return false;

		const EWaited Waited = WaitFor(Entry);
		const FJobPtr Job = Entry->Job;

		/* Reset by something the wait let run, which leaves it for the caller */
		if (Waited == EWaited::Dropped) return false;

		Drop(Entry);

		switch (Waited) {
			case EWaited::Cancelled:
				/* As the request itself would have been, and nothing else is started now */
				UE_LOG(LogReflection, Log, TEXT("Cloud prefetch cancelled"));
				GCancelled = true;
				FCloudPrefetch::Reset();

				bOutAnswered = false;

				return true;

			case EWaited::TimedOut:
				UE_LOG(LogReflection, Error, TEXT("HTTP request timed out after %.0f seconds: \"%s\""), WaitSeconds, *Entry->Path);

				bOutAnswered = false;

				return true;

			default:
				break;
		}

		/* Never reached the Cloud, which a request of the caller's own may well still do */
		if (!Job.IsValid() || Job->Stage.load() != Answered) return false;

		OutJob = Job;
		bOutAnswered = true;

		return true;
	}

	FEntryPtr QueueRaw(const FString& Path, const EOrigin Origin) {
		if (!IsPackagePath(Path)) return nullptr;

		const FString Key = RawKey(Path);

		if (GEntries.Contains(Key)) return nullptr;

		const FEntryPtr Entry = MakeShared<FEntry>();
		Entry->Kind = EKind::Raw;
		Entry->Origin = Origin;
		Entry->Key = Key;
		Entry->Path = Path;

		GEntries.Add(Key, Entry);

		return Entry;
	}
}

FCloudPrefetch::FScope::FScope() {
	using namespace ReflectionCloudPrefetch;

	if (!IsInGameThread()) return;

	bCounted = true;
	GScopes++;
}

FCloudPrefetch::FScope::~FScope() {
	using namespace ReflectionCloudPrefetch;

	if (!bCounted) return;

	if (--GScopes == 0) {
		Reset();

		GCancelled = false;
	}
}

void FCloudPrefetch::QueueRun(const TArray<FString>& Paths) {
	using namespace ReflectionCloudPrefetch;

	if (!IsEnabled()) return;

	GRun.Reserve(GRun.Num() + Paths.Num());
	for (const FString& Path : Paths) {
		GRun.Add(QueueRaw(Path, EOrigin::Run));
	}

	Tick();
}

void FCloudPrefetch::AdvanceRun() {
	using namespace ReflectionCloudPrefetch;

	if (!IsInGameThread() || !GRun.IsValidIndex(GRunNext)) return;

	/* Whatever the run did with this one it is done with it, so an answer it never took is let go
	 * rather than holding a place ahead of the ones it has yet to reach */
	const FEntryPtr Passed = GRun[GRunNext];

	GRun[GRunNext++].Reset();

	if (Passed.IsValid()) Drop(GEntries.FindRef(PixelKey(Passed->Key)));
	Drop(Passed);
	Tick();
}

TArray<FString> FCloudPrefetch::QueueReferences(const TArray<FString>& Paths) {
	using namespace ReflectionCloudPrefetch;

	TArray<FString> Queued;

	if (!IsEnabled()) return Queued;

	/* After the references already waiting, before any of the run's own */
	int32 InsertAt = 0;

	while (GWaiting.IsValidIndex(InsertAt) && GWaiting[InsertAt]->Origin == EOrigin::Reference) {
		InsertAt++;
	}

	for (const FString& Path : Paths) {
		if (const FEntryPtr Entry = QueueRaw(Path, EOrigin::Reference)) {
			GWaiting.Insert(Entry, InsertAt++);
			Queued.Add(Entry->Key);
		}
	}

	Tick();

	return Queued;
}

void FCloudPrefetch::Forget(const TArray<FString>& Keys) {
	using namespace ReflectionCloudPrefetch;

	if (!IsInGameThread()) return;

	for (const FString& Key : Keys) {
		Drop(GEntries.FindRef(PixelKey(Key)));
		Drop(GEntries.FindRef(Key));
	}

	Tick();
}

bool FCloudPrefetch::TakeRaw(const FString& Path, TSharedPtr<FJsonObject>& OutResponse) {
	using namespace ReflectionCloudPrefetch;

	if (!IsInGameThread() || GEntries.Num() == 0 || !IsPackagePath(Path)) return false;

	FJobPtr Job;
	bool bAnswered = false;

	if (!Take(RawKey(Path), Job, bAnswered)) return false;

	OutResponse = bAnswered ? Job->Raw : nullptr;

	Tick();

	return true;
}

bool FCloudPrefetch::TakeBinary(const FString& Path, const FString& ContentType, const FString& Named, TArray<uint8>& OutData, bool& bOutResult, FString* OutContentType) {
	using namespace ReflectionCloudPrefetch;

	if (!IsInGameThread() || GEntries.Num() == 0 || !IsPackagePath(Path)) return false;

	FJobPtr Job;
	bool bAnswered = false;

	const FString Key = PixelKey(RawKey(Path));
	const FEntryPtr Entry = GEntries.FindRef(Key);
	if (!Entry.IsValid() || Entry->ContentType != ContentType || Entry->Named != Named) return false;
	if (!Take(Key, Job, bAnswered)) return false;

	bOutResult = bAnswered && Cloud::Export::ReadBinaryResponse(Job->Response, OutData, OutContentType);

	Tick();

	return true;
}

void FCloudPrefetch::Tick() {
	using namespace ReflectionCloudPrefetch;

	if (!IsInGameThread() || (GRunning.Num() == 0 && GWaiting.Num() == 0 && GRunWaitingNext == GRun.Num())) return;

	Poll();
	StartWhatCan();
}

void FCloudPrefetch::Reset() {
	using namespace ReflectionCloudPrefetch;

	if (!IsInGameThread()) return;

	/* Copied out first, since cancelling can answer straight back into what is being walked */
	TArray<FEntryPtr> Running = GRunning;

	for (const FEntryPtr& Entry : Running) {
		if (Entry.IsValid() && !Entry->bFinished && Entry->Request.IsSet()) {
			Entry->bDropped = true;
			Entry->Request.GetValue()->CancelRequest();
		}
	}

	for (const TPair<FString, FEntryPtr>& Pair : GEntries) {
		Pair.Value->bDropped = true;
	}

	GEntries.Empty();
	GWaiting.Empty();
	GRunning.Empty();
	GRun.Empty();
	GRunNext = 0;
	GRunWaitingNext = 0;
	FMemory::Memzero(GAhead);
}
