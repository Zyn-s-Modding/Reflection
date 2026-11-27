/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/ImportIssues.h"

#include "Engine/Log.h"
#include "Engine/Package.h"
#include "Modules/UI/Import/SImportIssuesPanel.h"
#include "Settings/Redirector.h"

#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"

#define LOCTEXT_NAMESPACE "Reflection.ImportIssues"

namespace {
	TArray<TSharedPtr<FImportIssueAsset>> GAssets;

	/* Innermost last: a reference imports from inside the asset that named it */
	TArray<TSharedPtr<FImportIssueAsset>> GStack;

	/* A reference that hadn't resolved yet, held back until the run is over */
	struct FPendingReference {
		FString AssetName;
		FString AssetPath;
		FString AssetType;

		FString Type;
		FString Name;
		FString Path;

		/* How the reference spelled it, where what it names is kept inside an asset */
		FString Within;
	};

	TArray<FPendingReference> GPending;

	/* Anything reported outside an asset's import lands here */
	const FString UnattributedName = TEXT("Import");

	/* Whether the reference is in the project now, at the path it named or the redirected one.
	 *
	 * And beside the file it named, since an asset out of a package holding several is written to a
	 * file of its own where they are being split apart. Asked only at the path the reference used,
	 * every one of those reads as missing however well it resolved. */
	bool ResolvesAt(const FString& Path) {
		if (LoadObjectByPath<UObject>(Path) != nullptr) {
			return true;
		}

		FString Redirected = Path;
		FRRedirects::Redirect(Redirected);

		return Redirected != Path && LoadObjectByPath<UObject>(Redirected) != nullptr;
	}

	bool ReferenceResolves(const FPendingReference& Pending) {
		/* Under the name it is kept under, which is the only one that finds it */
		if (!Pending.Within.IsEmpty() && ResolvesAt(Pending.Path + TEXT(".") + Pending.Within)) {
			return true;
		}

		if (ResolvesAt(Pending.Path + TEXT(".") + Pending.Name)) {
			return true;
		}

		/* A part of an asset is never a file of its own, so there is no second place to look */
		if (Pending.Name.Contains(TEXT(":"))) {
			return false;
		}

		FString Folder;

		if (!Pending.Path.Split(TEXT("/"), &Folder, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) {
			return false;
		}

		return ResolvesAt(Folder + TEXT("/") + Pending.Name + TEXT(".") + Pending.Name);
	}

	TSharedPtr<FImportIssueAsset> FindOrAddAsset(const FString& Name, const FString& Path, const FString& Type) {
		for (const TSharedPtr<FImportIssueAsset>& Asset : GAssets) {
			if (Asset->Name == Name && Asset->Path == Path) {
				return Asset;
			}
		}

		const TSharedPtr<FImportIssueAsset> Asset = MakeShared<FImportIssueAsset>();

		Asset->Name = Name;
		Asset->Path = Path;
		Asset->Type = Type;

		GAssets.Add(Asset);

		return Asset;
	}
}

FText GetImportIssueText(const EImportIssue Kind) {
	switch (Kind) {
		case EImportIssue::MissingAsset: return LOCTEXT("MissingAsset", "Missing Asset");
		case EImportIssue::MissingClass: return LOCTEXT("MissingClass", "Missing Class");
		case EImportIssue::Setting: return LOCTEXT("Setting", "Setting");
		case EImportIssue::Data: return LOCTEXT("Data", "Data");
		default: return LOCTEXT("Failed", "Failed");
	}
}

FLinearColor GetImportIssueColor(const EImportIssue Kind) {
	switch (Kind) {
		case EImportIssue::MissingAsset: return FLinearColor(1.0f, 0.55f, 0.15f);
		case EImportIssue::MissingClass: return FLinearColor(1.0f, 0.75f, 0.2f);
		case EImportIssue::Setting: return FLinearColor(0.45f, 0.7f, 1.0f);
		case EImportIssue::Data: return FLinearColor(0.8f, 0.8f, 0.8f);
		default: return FLinearColor(1.0f, 0.35f, 0.35f);
	}
}

void FImportIssues::Begin() {
	Clear();
}

void FImportIssues::Finish() {
	/* The stack outliving the run means a Push went unpopped, and the next run would inherit it */
	GStack.Reset();

	/* Only the ones still missing now were ever worth reporting */
	for (const FPendingReference& Pending : GPending) {
		if (ReferenceResolves(Pending)) {
			continue;
		}

		/* A class is compiled into the build, so one this engine hasn't got is missing rather than unfetched */
		const bool bNative = Pending.Path.StartsWith(TEXT("/Script/"));

		ReportFor(
			Pending.AssetName,
			Pending.AssetPath,
			Pending.AssetType,
			bNative ? EImportIssue::MissingClass : EImportIssue::MissingAsset,
			bNative
				? FString::Printf(TEXT("This engine has no %s"), *Pending.Name)
				: TEXT("Couldn't resolve ") + Pending.Name,
			bNative
				? FString::Printf(TEXT("%s is compiled into %s rather than stored as an asset, so nothing can fetch it."), *Pending.Name, *Pending.Path)
				: Pending.Type + TEXT(" at ") + Pending.Path
		);
	}

	GPending.Reset();

	if (GAssets.Num() == 0) {
		return;
	}

	UE_LOG(LogReflection, Warning, TEXT("Reflection finished with %d issue(s) across %d asset(s)."), NumIssues(), GAssets.Num());

	if (!IsRunningCommandlet()) {
		SImportIssuesPanel::Open();
	}
}

void FImportIssues::Push(const FString& Name, const FString& Path, const FString& Type) {
	const TSharedPtr<FImportIssueAsset> Asset = MakeShared<FImportIssueAsset>();

	Asset->Name = Name;
	Asset->Path = Path;
	Asset->Type = Type;

	/* Not added to the report yet: an asset that imports cleanly never gets a row */
	GStack.Push(Asset);
}

void FImportIssues::Pop() {
	if (GStack.Num() > 0) {
		GStack.Pop();
	}
}

void FImportIssues::Report(const EImportIssue Kind, const FString& Summary, const FString& Detail) {
	const TSharedPtr<FImportIssueAsset> Current = GStack.Num() > 0 ? GStack.Last() : nullptr;

	if (Current.IsValid()) {
		ReportFor(Current->Name, Current->Path, Current->Type, Kind, Summary, Detail);

		return;
	}

	ReportFor(UnattributedName, FString(), FString(), Kind, Summary, Detail);
}

void FImportIssues::ReportFor(const FString& Name, const FString& Path, const FString& Type, const EImportIssue Kind, const FString& Summary, const FString& Detail) {
	const TSharedPtr<FImportIssueAsset> Asset = FindOrAddAsset(Name, Path, Type);

	/* One node class missing is one line, however many nodes wanted it */
	for (const TSharedPtr<FImportIssue>& Existing : Asset->Issues) {
		if (Existing->Kind == Kind && Existing->Summary == Summary && Existing->Detail == Detail) {
			Existing->Count++;

			return;
		}
	}

	const TSharedPtr<FImportIssue> Issue = MakeShared<FImportIssue>();

	Issue->Kind = Kind;
	Issue->Summary = Summary;
	Issue->Detail = Detail;

	Asset->Issues.Add(Issue);
}

/* An asset's own exports come through here unresolved too, so nothing is reported until the end */
void FImportIssues::ReportUnresolvedReference(const FString& Type, const FString& Name, const FString& Path, const FString& Within) {
	if (Name.IsEmpty()) {
		return;
	}

	/* "Asset:Export" names something inside a package rather than a package */
	if (Name.Contains(TEXT(":"))) {
		return;
	}

	/* Engine content ships with the engine */
	if (Path.StartsWith(TEXT("/Engine/")) || Path.StartsWith(TEXT("Engine/"))) {
		return;
	}

	const TSharedPtr<FImportIssueAsset> Current = GStack.Num() > 0 ? GStack.Last() : nullptr;

	if (Current.IsValid() && !Path.IsEmpty()) {
		/* Same package, so it is one of this asset's own exports */
		if (Path == Current->Path) {
			return;
		}

		/* Same again, for a package the redirects moved out from under the reference */
		if (!Current->Name.IsEmpty() && Path.EndsWith(TEXT("/") + Current->Name)) {
			return;
		}
	}

	FPendingReference Pending;

	Pending.AssetName = Current.IsValid() ? Current->Name : UnattributedName;
	Pending.AssetPath = Current.IsValid() ? Current->Path : FString();
	Pending.AssetType = Current.IsValid() ? Current->Type : FString();

	Pending.Type = Type;
	Pending.Name = Name;
	Pending.Path = Path;
	Pending.Within = Within;

	/* Held until the run ends, where whether it resolved is a question with an answer */
	GPending.Add(Pending);
}

const TArray<TSharedPtr<FImportIssueAsset>>& FImportIssues::GetAssets() {
	return GAssets;
}

int32 FImportIssues::NumIssues() {
	int32 Count = 0;

	for (const TSharedPtr<FImportIssueAsset>& Asset : GAssets) {
		for (const TSharedPtr<FImportIssue>& Issue : Asset->Issues) {
			Count += Issue->Count;
		}
	}

	return Count;
}

void FImportIssues::Clear() {
	GAssets.Reset();
	GStack.Reset();
	GPending.Reset();
}

#undef LOCTEXT_NAMESPACE

namespace {
	TSet<FString>& HandledFields() {
		static TSet<FString> Fields;
		return Fields;
	}

	/* Built data the importer has the engine rebuild from the imported asset: reported apart, as
	 * rebuilt rather than left out, since nothing is lost */
	TMap<FString, FString>& RebuiltFields() {
		static TMap<FString, FString> Fields;
		return Fields;
	}

	struct FFieldAlias {
		FString From;
		/* Leaf of the source value -> this engine's value; "*" for anything else. Empty: as it is. */
		TMap<FString, TSharedPtr<FJsonValue>> Map;
	};

	TMap<FString, FFieldAlias>& FieldAliases() {
		static TMap<FString, FFieldAlias> Aliases;
		return Aliases;
	}

	/* Resources/ReflectionFields.json, read the first time a field is asked about */
	void ReadKnownFields() {
		static bool bRead = false;
		if (bRead) return;
		bRead = true;

		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Reflection"));
		const FString File = Plugin.IsValid() ? Plugin->GetBaseDir() / TEXT("Resources/ReflectionFields.json") : FString();
		FString Text;
		TSharedPtr<FJsonObject> Root;
		if (File.IsEmpty() || !FFileHelper::LoadFileToString(Text, *File) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid()) {
			UE_LOG(LogReflection, Warning, TEXT("Known source fields could not be read from \"%s\"; every field without a property here will be reported"), *File);
			return;
		}

		const TSharedPtr<FJsonObject>* Ignored = nullptr;
		if (Root->TryGetObjectField(TEXT("Ignored"), Ignored)) {
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Type : (*Ignored)->Values) {
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
				if (!Type.Value.IsValid() || !Type.Value->TryGetObject(Entry) || !(*Entry)->TryGetArrayField(TEXT("Fields"), Fields)) continue;
				for (const TSharedPtr<FJsonValue>& Field : *Fields) {
					FString Name;
					if (Field.IsValid() && Field->TryGetString(Name)) HandledFields().Add(Type.Key + TEXT(".") + Name);
				}
			}
		}

		const TSharedPtr<FJsonObject>* Rebuilt = nullptr;
		if (Root->TryGetObjectField(TEXT("Rebuilt"), Rebuilt)) {
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Type : (*Rebuilt)->Values) {
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
				if (!Type.Value.IsValid() || !Type.Value->TryGetObject(Entry) || !(*Entry)->TryGetArrayField(TEXT("Fields"), Fields)) continue;
				FString Why;
				(*Entry)->TryGetStringField(TEXT("By"), Why);
				for (const TSharedPtr<FJsonValue>& Field : *Fields) {
					FString Name;
					if (Field.IsValid() && Field->TryGetString(Name)) RebuiltFields().Add(Type.Key + TEXT(".") + Name, Why);
				}
			}
		}

		const TSharedPtr<FJsonObject>* Aliases = nullptr;
		if (Root->TryGetObjectField(TEXT("Aliases"), Aliases)) {
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Aliases)->Values) {
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				FFieldAlias Alias;
				if (!Pair.Value.IsValid() || !Pair.Value->TryGetObject(Entry) || !(*Entry)->TryGetStringField(TEXT("From"), Alias.From)) continue;
				const TSharedPtr<FJsonObject>* Map = nullptr;
				if ((*Entry)->TryGetObjectField(TEXT("Map"), Map)) Alias.Map = (*Map)->Values;
				FString Type;
				Pair.Key.Split(TEXT("."), &Type, nullptr);
				/* The field it is read from is placed, not left out */
				FString Field = Alias.From;
				Field.Split(TEXT("."), &Field, nullptr);
				HandledFields().Add(Type + TEXT(".") + (Field.IsEmpty() ? Alias.From : Field));
				FieldAliases().Add(Pair.Key, MoveTemp(Alias));
			}
		}
		UE_LOG(LogReflection, Display, TEXT("Read %d known source field(s), %d rebuilt field(s) and %d alias(es) from \"%s\""), HandledFields().Num(), RebuiltFields().Num(), FieldAliases().Num(), *File);
	}

	/* What rebuilds the field, where the data file says the engine does, for this type or one it derives from */
	const FString* RebuiltBy(const UStruct* Type, const FString& Field) {
		ReadKnownFields();
		for (const UStruct* Each = Type; Each != nullptr; Each = Each->GetSuperStruct()) {
			if (const FString* By = RebuiltFields().Find(Each->GetName() + TEXT(".") + Field)) return By;
		}
		return nullptr;
	}

	/* Known for this type, one it derives from, or every type */
	bool IsHandled(const UStruct* Type, const FString& Field) {
		ReadKnownFields();
		if (HandledFields().Contains(FString(TEXT("*.")) + Field)) return true;
		for (const UStruct* Each = Type; Each != nullptr; Each = Each->GetSuperStruct()) {
			if (HandledFields().Contains(Each->GetName() + TEXT(".") + Field)) return true;
		}
		return false;
	}
}

void FImportIssues::MarkFieldHandled(const FString& TypeName, const FString& Field) {
	HandledFields().Add(TypeName + TEXT(".") + Field);
}

TSharedPtr<FJsonValue> FImportIssues::AliasedValue(const UStruct* Type, const FString& Property, const FJsonObject& Fields) {
	/* What the user redirected comes ahead of what the plugin already knows */
	if (const TSharedPtr<FJsonValue> Redirected = FRRedirects::RedirectedProperty(Type, Property, Fields); Redirected.IsValid()) {
		return Redirected;
	}

	ReadKnownFields();
	for (const UStruct* Each = Type; Each != nullptr; Each = Each->GetSuperStruct()) {
		const FFieldAlias* Alias = FieldAliases().Find(Each->GetName() + TEXT(".") + Property);
		if (Alias == nullptr) continue;
		/* From may reach into the field: Constant.X is the X of the source's Constant */
		TArray<FString> Steps;
		Alias->From.ParseIntoArray(Steps, TEXT("."));
		TSharedPtr<FJsonValue> Value = Steps.IsEmpty() ? nullptr : Fields.TryGetField(Steps[0]);
		for (int32 Step = 1; Step < Steps.Num() && Value.IsValid(); ++Step) {
			const TSharedPtr<FJsonObject>* Inner = nullptr;
			Value = Value->TryGetObject(Inner) ? (*Inner)->TryGetField(Steps[Step]) : nullptr;
		}
		if (!Value.IsValid() || Alias->Map.IsEmpty()) return Value;

		FString Said;
		if (!Value->TryGetString(Said)) {
			if (Value->Type == EJson::Boolean) Said = Value->AsBool() ? TEXT("true") : TEXT("false");
			else if (Value->Type == EJson::Number) Said = FString::SanitizeFloat(Value->AsNumber(), 0);
		}
		FString Leaf;
		if (Said.Split(TEXT("::"), nullptr, &Leaf, ESearchCase::CaseSensitive, ESearchDir::FromEnd)) Said = Leaf;
		if (const TSharedPtr<FJsonValue>* Mapped = Alias->Map.Find(Said)) return *Mapped;
		if (const TSharedPtr<FJsonValue>* Otherwise = Alias->Map.Find(TEXT("*"))) return *Otherwise;
		return nullptr;
	}
	return nullptr;
}

void FImportIssues::ReportUnplacedFields(const UStruct* Type, const FJsonObject& Fields) {
	if (Type == nullptr) return;

	/* Core types (vectors, quaternions, transforms) are written with values the exporter works out
	 * alongside the real ones, and have nothing of their own to lose */
	if (Type->GetOutermost()->GetFName() == TEXT("/Script/CoreUObject")) return;

	TArray<FString> Left;
	TArray<FString> Rebuilt;
	TArray<FString> RebuiltBy_;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Fields.Values) {
		FString Key = Field.Key;

		/* Worked out by the exporter from the value beside it, not a field of the type */
		if (Key == TEXT("Hex")) continue;

		/* One element of a fixed-size array, which the serializer reads as Name[Index] */
		if (int32 Bracket; Key.EndsWith(TEXT("]")) && Key.FindChar(TEXT('['), Bracket)) Key.LeftInline(Bracket);

		if (Type->FindPropertyByName(FName(*Key)) != nullptr) continue;
		if (Type->FindPropertyByName(FName(*(Key + TEXT("_DEPRECATED")))) != nullptr) continue;
		if (IsHandled(Type, Key)) continue;
		if (FRRedirects::PlacesField(Type, Key)) continue;
		if (const FString* By = RebuiltBy(Type, Key)) {
			Rebuilt.AddUnique(Key);
			if (!By->IsEmpty()) RebuiltBy_.AddUnique(*By);
			continue;
		}

		Left.AddUnique(Key);
	}

	if (!Rebuilt.IsEmpty()) {
		Rebuilt.Sort();
		Report(EImportIssue::Data, TEXT("Built data was rebuilt by the engine from the imported asset, not copied"),
			FString::Printf(TEXT("%s: %s%s"), *Type->GetName(), *FString::Join(Rebuilt, TEXT(", ")),
				RebuiltBy_.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *FString::Join(RebuiltBy_, TEXT("; ")))));
	}

	if (Left.IsEmpty()) return;

	Left.Sort();
	Report(EImportIssue::Data, TEXT("Fields this engine has no property for were left out"),
		FString::Printf(TEXT("%s: %s"), *Type->GetName(), *FString::Join(Left, TEXT(", "))));
}
