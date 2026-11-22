/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/UI/Settings/BlacklistCustomization.h"

#include "Settings/Types/BlacklistSettings.h"
#include "Modules/UI/SupportedAssets/SSupportedAssetsPanel.h"
#include "Engine/Compatibility.h"

#include "AssetToolsModule.h"
#include "IAssetTypeActions.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "Engine/DataAsset.h"
#include "Styling/SlateIconFinder.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

#if UE5_2_BEYOND
#include "AssetDefinition.h"
#include "AssetDefinitionRegistry.h"
#endif

#define LOCTEXT_NAMESPACE "Reflection.Blacklist"

namespace {
	/* One row of the type list */
	struct FBlacklistTypeOption {
		FString Name;

		/* Where this engine has a class for it, for the icon and the class path */
		TWeakObjectPtr<UClass> Class;

		/* Something Reflection has an importer for, which is worth telling apart from the rest */
		bool bReflected = false;

		/* What was typed, offered as it is when nothing in the list is called that */
		bool bCustom = false;
	};

	using FBlacklistTypeOptionPtr = TSharedPtr<FBlacklistTypeOption>;

	/* A class the editor leaves behind while it recompiles or reloads something */
	bool IsLeftover(const FString& Name) {
		return Name.StartsWith(TEXT("SKEL_")) || Name.StartsWith(TEXT("REINST_")) || Name.StartsWith(TEXT("TRASH_"))
			|| Name.StartsWith(TEXT("HOTRELOADED_")) || Name.StartsWith(TEXT("PLACEHOLDER-"));
	}

	/* Every type worth listing: what Reflection imports, every kind of asset the editor knows of, and
	 * every data asset class, which is where most of a game's own types live. Gathered each time the
	 * list opens, since a plugin loaded since the last time brings classes of its own. */
	TArray<FBlacklistTypeOptionPtr> CollectTypes() {
		TArray<FBlacklistTypeOptionPtr> Options;
		TMap<FString, FBlacklistTypeOptionPtr> Seen;

		const auto Add = [&Options, &Seen](const FString& Name, UClass* Class, const bool bReflected) {
			if (Name.IsEmpty() || IsLeftover(Name)) return;

			if (FBlacklistTypeOptionPtr* Existing = Seen.Find(Name)) {
				(*Existing)->bReflected |= bReflected;

				if (!(*Existing)->Class.IsValid() && Class != nullptr) (*Existing)->Class = Class;

				return;
			}

			FBlacklistTypeOptionPtr Option = MakeShared<FBlacklistTypeOption>();
			Option->Name = Name;
			Option->Class = Class;
			Option->bReflected = bReflected;

			Seen.Add(Name, Option);
			Options.Add(Option);
		};

		for (const TSharedPtr<FSupportedAssetEntry>& Entry : SSupportedAssetsPanel::Collect()) {
			Add(Entry->Type, FindClassByType(Entry->Type), true);
		}

#if UE5_2_BEYOND
		if (const UAssetDefinitionRegistry* Registry = UAssetDefinitionRegistry::Get()) {
			for (const UAssetDefinition* Definition : Registry->GetAllAssetDefinitions()) {
				if (Definition == nullptr) continue;

				const TSoftClassPtr<UObject> AssetClass = Definition->GetAssetClass();

				if (AssetClass.IsNull()) continue;

				Add(AssetClass.ToSoftObjectPath().GetAssetName(), AssetClass.Get(), false);
			}
		}
#else
		{
			TArray<TWeakPtr<IAssetTypeActions>> Actions;
			FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get().GetAssetTypeActionsList(Actions);

			for (const TWeakPtr<IAssetTypeActions>& Action : Actions) {
				const TSharedPtr<IAssetTypeActions> Pinned = Action.Pin();

				if (!Pinned.IsValid()) continue;

				if (UClass* Class = Pinned->GetSupportedClass()) {
					Add(Class->GetName(), Class, false);
				}
			}
		}
#endif

		for (TObjectIterator<UClass> It; It; ++It) {
			UClass* Class = *It;

			if (!Class->IsChildOf(UDataAsset::StaticClass())) continue;
			if (!Class->HasAnyClassFlags(CLASS_Native)) continue;
			if (Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists)) continue;

			Add(Class->GetName(), Class, false);
		}

		Options.Sort([](const FBlacklistTypeOptionPtr& A, const FBlacklistTypeOptionPtr& B) {
			return A->Name.Compare(B->Name, ESearchCase::IgnoreCase) < 0;
		});

		return Options;
	}

	FString ReadString(const TSharedPtr<IPropertyHandle>& Handle) {
		FString Value;

		if (Handle.IsValid() && Handle->GetValue(Value) == FPropertyAccess::Success) {
			return Value;
		}

		return FString();
	}

	const FSlateBrush* FolderBrush() {
#if ENGINE_UE5
		return FAppStyle::Get().GetBrush("Icons.FolderClosed");
#else
		return FAppStyle::Get().GetBrush("ContentBrowser.AssetTreeFolderClosed");
#endif
	}
}

/* A dropdown of every asset type, with a search box, and whatever was typed on offer as well */
class SBlacklistTypePicker : public SCompoundWidget {
public:
	SLATE_BEGIN_ARGS(SBlacklistTypePicker) {}
		SLATE_ARGUMENT(TSharedPtr<IPropertyHandle>, Handle)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs) {
		Handle = InArgs._Handle;

		ChildSlot
		[
			SAssignNew(ComboButton, SComboButton)
			.ContentPadding(FMargin(2.0f, 2.0f))
			.OnGetMenuContent(this, &SBlacklistTypePicker::MakeMenu)
			.ToolTipText(LOCTEXT("TypeTooltip", "The type to keep out. Pick one from the list, or type a name it doesn't have and press Enter."))
			.ButtonContent()
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(SBox)
					.WidthOverride(16.0f)
					.HeightOverride(16.0f)
					[
						SNew(SImage)
						.Image(this, &SBlacklistTypePicker::GetCurrentIcon)
					]
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(IDetailLayoutBuilder::GetDetailFont())
					.Text(this, &SBlacklistTypePicker::GetCurrentText)
				]
			]
		];
	}

private:
	FText GetCurrentText() const {
		const FString Value = ReadString(Handle);

		return Value.IsEmpty() ? LOCTEXT("PickType", "Pick a type...") : FText::FromString(Value);
	}

	/* Looked up again only when the value changes, since this is asked every frame */
	const FSlateBrush* GetCurrentIcon() const {
		const FString Value = ReadString(Handle);

		if (Value != IconFor) {
			IconFor = Value;

			const UClass* Class = Value.IsEmpty() ? nullptr : FindClassByType(Value);

			Icon = Class != nullptr ? FSlateIconFinder::FindIconBrushForClass(Class) : nullptr;
		}

		return Icon;
	}

	FText GetSearchText() const { return SearchText; }

	TSharedRef<SWidget> MakeMenu() {
		All = CollectTypes();
		SearchText = FText::GetEmpty();

		TSharedRef<SWidget> Menu =
			SNew(SBox)
			.WidthOverride(360.0f)
			.HeightOverride(440.0f)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(4.0f)
				[
					SAssignNew(SearchBox, SSearchBox)
					.HintText(LOCTEXT("SearchTypes", "Search, or type a name and press Enter"))
					.OnTextChanged(this, &SBlacklistTypePicker::OnSearchChanged)
					.OnTextCommitted(this, &SBlacklistTypePicker::OnSearchCommitted)
				]

				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SAssignNew(ListView, SListView<FBlacklistTypeOptionPtr>)
					.ListItemsSource(&Filtered)
					.SelectionMode(ESelectionMode::Single)
					.OnGenerateRow(this, &SBlacklistTypePicker::GenerateRow)
					.OnMouseButtonClick(this, &SBlacklistTypePicker::Pick)
				]
			];

		Refilter();

		ComboButton->SetMenuContentWidgetToFocus(SearchBox);

		return Menu;
	}

	void OnSearchChanged(const FText& Text) {
		SearchText = Text;

		Refilter();
	}

	/* Enter takes the best match, which is what was typed where nothing else matches */
	void OnSearchCommitted(const FText& Text, const ETextCommit::Type CommitType) {
		if (CommitType != ETextCommit::OnEnter) return;

		SearchText = Text;
		Refilter();

		if (Filtered.Num() > 0) {
			Pick(Filtered[0]);
		}
	}

	void Refilter() {
		const FString Search = SearchText.ToString().TrimStartAndEnd();

		Filtered.Reset();

		bool bExact = false;

		for (const FBlacklistTypeOptionPtr& Option : All) {
			if (Search.IsEmpty() || Option->Name.Contains(Search, ESearchCase::IgnoreCase)) {
				Filtered.Add(Option);

				bExact |= Option->Name.Equals(Search, ESearchCase::IgnoreCase);
			}
		}

		/* What it is called, then what it starts with, then the rest, alphabetical within each */
		if (!Search.IsEmpty()) {
			const auto Rank = [&Search](const FBlacklistTypeOptionPtr& Option) {
				if (Option->Name.Equals(Search, ESearchCase::IgnoreCase)) return 0;
				if (Option->Name.StartsWith(Search, ESearchCase::IgnoreCase)) return 1;

				return 2;
			};

			Filtered.StableSort([&Rank](const FBlacklistTypeOptionPtr& A, const FBlacklistTypeOptionPtr& B) {
				return Rank(A) < Rank(B);
			});
		}

		/* A type the list doesn't have is still one the game can, so it goes on the end */
		if (!Search.IsEmpty() && !bExact) {
			FBlacklistTypeOptionPtr Custom = MakeShared<FBlacklistTypeOption>();
			Custom->Name = Search;
			Custom->bCustom = true;

			Filtered.Add(Custom);
		}

		if (ListView.IsValid()) {
			ListView->RequestListRefresh();
		}
	}

	TSharedRef<ITableRow> GenerateRow(FBlacklistTypeOptionPtr Option, const TSharedRef<STableViewBase>& Owner) {
		UClass* Class = Option->Class.Get();

		const FSlateBrush* RowIcon = Class != nullptr ? FSlateIconFinder::FindIconBrushForClass(Class) : nullptr;

		const FText Label = Option->bCustom
			? FText::Format(LOCTEXT("UseTyped", "Use \"{0}\""), FText::FromString(Option->Name))
			: FText::FromString(Option->Name);

		const FText Tooltip = Class != nullptr
			? FText::FromString(Class->GetPathName())
			: LOCTEXT("NoClass", "This engine has no class by this name, so Include Child Types can't reach past it");

		return SNew(STableRow<FBlacklistTypeOptionPtr>, Owner)
			.Padding(FMargin(4.0f, 2.0f))
			.ToolTipText(Tooltip)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SBox)
					.WidthOverride(16.0f)
					.HeightOverride(16.0f)
					[
						SNew(SImage)
						.Image(RowIcon)
					]
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Label)
					.HighlightText(this, &SBlacklistTypePicker::GetSearchText)
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(Option->bReflected ? LOCTEXT("Reflected", "Reflection") : FText::GetEmpty())
					.ToolTipText(LOCTEXT("ReflectedTooltip", "Reflection has an importer for this type"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			];
	}

	void Pick(FBlacklistTypeOptionPtr Option) {
		if (!Option.IsValid() || !Handle.IsValid()) return;

		Handle->SetValue(Option->Name);

		if (ComboButton.IsValid()) {
			ComboButton->SetIsOpen(false);
		}
	}

	TSharedPtr<IPropertyHandle> Handle;

	TSharedPtr<SComboButton> ComboButton;
	TSharedPtr<SSearchBox> SearchBox;
	TSharedPtr<SListView<FBlacklistTypeOptionPtr>> ListView;

	TArray<FBlacklistTypeOptionPtr> All;
	TArray<FBlacklistTypeOptionPtr> Filtered;

	FText SearchText;

	mutable FString IconFor;
	mutable const FSlateBrush* Icon = nullptr;
};

/* A path box that takes anything, and the Content Browser's folder picker beside it */
class SBlacklistFolderPicker : public SCompoundWidget {
public:
	SLATE_BEGIN_ARGS(SBlacklistFolderPicker) {}
		SLATE_ARGUMENT(TSharedPtr<IPropertyHandle>, Handle)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs) {
		Handle = InArgs._Handle;

		ChildSlot
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SEditableTextBox)
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.HintText(LOCTEXT("FolderHint", "/Game/Folder/ or any path from the game"))
				.ToolTipText(LOCTEXT("FolderTooltip", "Any folder, whether or not this project has it yet. The game's own spelling works too, like FortniteGame/Content/Athena/."))
				.Text(this, &SBlacklistFolderPicker::GetPathText)
				.OnTextCommitted(this, &SBlacklistFolderPicker::OnPathCommitted)
				.SelectAllTextWhenFocused(true)
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(2.0f, 0.0f, 0.0f, 0.0f)
			[
				SAssignNew(PickerButton, SComboButton)
				.ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("HoverHintOnly"))
				.HasDownArrow(false)
				.ContentPadding(FMargin(2.0f))
				.ToolTipText(LOCTEXT("PickFolder", "Pick a folder this project already has"))
				.OnGetMenuContent(this, &SBlacklistFolderPicker::MakePicker)
				.ButtonContent()
				[
					SNew(SImage)
					.Image(FolderBrush())
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
		];
	}

private:
	FText GetPathText() const {
		return FText::FromString(ReadString(Handle));
	}

	void OnPathCommitted(const FText& Text, const ETextCommit::Type CommitType) {
		if (!Handle.IsValid() || CommitType == ETextCommit::OnCleared) return;

		const FString Path = Text.ToString().TrimStartAndEnd();

		if (Path != ReadString(Handle)) {
			Handle->SetValue(Path);
		}
	}

	TSharedRef<SWidget> MakePicker() {
		FContentBrowserModule& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");

		FPathPickerConfig Config;

		/* Opens on what is already written, where that is somewhere the picker can show */
		FString Current = ReadString(Handle);

		while (Current.RemoveFromEnd(TEXT("/"))) {}

		if (Current.StartsWith(TEXT("/"))) {
			Config.DefaultPath = Current;
		}

		Config.bAllowContextMenu = false;
		Config.bAllowClassesFolder = false;
		Config.bFocusSearchBoxWhenOpened = true;
		Config.OnPathSelected = FOnPathSelected::CreateSP(this, &SBlacklistFolderPicker::OnFolderPicked);

		return SNew(SBox)
			.WidthOverride(320.0f)
			.HeightOverride(420.0f)
			[
				ContentBrowser.Get().CreatePathPicker(Config)
			];
	}

	void OnFolderPicked(const FString& Path) {
		if (Handle.IsValid() && !Path.IsEmpty()) {
			Handle->SetValue(Path.EndsWith(TEXT("/")) ? Path : Path + TEXT("/"));
		}

		if (PickerButton.IsValid()) {
			PickerButton->SetIsOpen(false);
		}
	}

	TSharedPtr<IPropertyHandle> Handle;
	TSharedPtr<SComboButton> PickerButton;
};

/* Customizations ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
TSharedRef<IPropertyTypeCustomization> FRBlacklistTypeCustomization::MakeInstance() {
	return MakeShareable(new FRBlacklistTypeCustomization());
}

void FRBlacklistTypeCustomization::CustomizeHeader(TSharedRef<IPropertyHandle> StructPropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& StructCustomizationUtils) {
	const TSharedPtr<IPropertyHandle> Type = StructPropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FRBlacklistType, Type));

	HeaderRow
	.NameContent()
	[
		StructPropertyHandle->CreatePropertyNameWidget()
	]
	.ValueContent()
	.MinDesiredWidth(250.0f)
	.MaxDesiredWidth(600.0f)
	[
		SNew(SBlacklistTypePicker)
		.Handle(Type)
	];
}

TSharedRef<IPropertyTypeCustomization> FRBlacklistFolderCustomization::MakeInstance() {
	return MakeShareable(new FRBlacklistFolderCustomization());
}

void FRBlacklistFolderCustomization::CustomizeHeader(TSharedRef<IPropertyHandle> StructPropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& StructCustomizationUtils) {
	const TSharedPtr<IPropertyHandle> Path = StructPropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FRBlacklistFolder, Path));

	HeaderRow
	.NameContent()
	[
		StructPropertyHandle->CreatePropertyNameWidget()
	]
	.ValueContent()
	.MinDesiredWidth(250.0f)
	.MaxDesiredWidth(600.0f)
	[
		SNew(SBlacklistFolderPicker)
		.Handle(Path)
	];
}

void FRBlacklistCustomization::Register() {
	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

	PropertyEditor.RegisterCustomPropertyTypeLayout(
		FRBlacklistType::StaticStruct()->GetFName(),
		FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FRBlacklistTypeCustomization::MakeInstance)
	);

	PropertyEditor.RegisterCustomPropertyTypeLayout(
		FRBlacklistFolder::StaticStruct()->GetFName(),
		FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FRBlacklistFolderCustomization::MakeInstance)
	);

	PropertyEditor.NotifyCustomizationModuleChanged();
}

void FRBlacklistCustomization::Unregister() {
	if (!FModuleManager::Get().IsModuleLoaded("PropertyEditor")) return;

	FPropertyEditorModule& PropertyEditor = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");

	PropertyEditor.UnregisterCustomPropertyTypeLayout(FRBlacklistType::StaticStruct()->GetFName());
	PropertyEditor.UnregisterCustomPropertyTypeLayout(FRBlacklistFolder::StaticStruct()->GetFName());
}

#undef LOCTEXT_NAMESPACE
