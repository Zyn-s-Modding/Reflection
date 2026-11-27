/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/ObjectImporter.h"

#if REFLECTION_METASOUND_REGISTRY
#include "MetasoundFrontendSearchEngine.h"
#endif

#if REFLECTION_METASOUND_EDITOR_GRAPH
#include "Importers/Constructor/Asset.h"
#include "Importers/Types/Audio/MetaSoundEditorGraph.h"
#endif

#if ENGINE_UE5
namespace {
	const TSharedPtr<FJsonObject>* MetaSoundObject(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field) {
		const TSharedPtr<FJsonObject>* Found = nullptr;
		return Object.IsValid() && Object->TryGetObjectField(Field, Found) ? Found : nullptr;
	}

	TSharedPtr<FJsonObject> MetaSoundDefaultPage(const TArray<TSharedPtr<FJsonValue>>& Pages) {
		for (const TSharedPtr<FJsonValue>& Value : Pages) {
			if (!Value.IsValid() || Value->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject> Page = Value->AsObject();
			FString Written;
			FGuid Id;
			if (Page->TryGetStringField(TEXT("PageID"), Written) && FGuid::Parse(Written, Id) && !Id.IsValid()) return Page;
			if (Pages.Num() == 1) return Page;
		}
		return nullptr;
	}

	void RestoreMetaSoundDefaults(const TSharedPtr<FJsonObject>& Class) {
		const TSharedPtr<FJsonObject>* Interface = MetaSoundObject(Class, TEXT("Interface"));
		const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
		if (Interface == nullptr || !(*Interface)->TryGetArrayField(TEXT("Inputs"), Inputs)) return;

		for (const TSharedPtr<FJsonValue>& Value : *Inputs) {
			if (!Value.IsValid() || Value->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject> Input = Value->AsObject();
			const TArray<TSharedPtr<FJsonValue>>* Defaults = nullptr;
			if (Input->HasField(TEXT("DefaultLiteral")) || !Input->TryGetArrayField(TEXT("Defaults"), Defaults)) continue;

			if (const TSharedPtr<FJsonObject>* Literal = MetaSoundObject(MetaSoundDefaultPage(*Defaults), TEXT("Literal"))) {
				Input->SetObjectField(TEXT("DefaultLiteral"), *Literal);
			}
		}
	}

#if REFLECTION_METASOUND_REGISTRY
	template<typename ClassVertex>
	bool SupportsMetaSoundVertices(const TArray<FMetasoundFrontendVertex>& Vertices,
		const TSet<FGuid>& Used, const TArray<ClassVertex>& Available) {
		for (const FMetasoundFrontendVertex& Vertex : Vertices) {
			if (!Used.Contains(Vertex.VertexID)) continue;
			if (!Available.ContainsByPredicate([&Vertex](const ClassVertex& Pin) {
				return Pin.Name == Vertex.Name && Pin.TypeName == Vertex.TypeName;
			})) return false;
		}
		return true;
	}

	bool SupportsMetaSoundGraph(const FMetasoundFrontendGraph& Graph, const FGuid& ClassId,
		const FMetasoundFrontendClassInterface& Available) {
		for (const FMetasoundFrontendNode& Node : Graph.Nodes) {
			if (Node.ClassID != ClassId) continue;
			TSet<FGuid> Inputs, Outputs;
			for (const FMetasoundFrontendEdge& Edge : Graph.Edges) {
				if (Edge.ToNodeID == Node.GetID()) Inputs.Add(Edge.ToVertexID);
				if (Edge.FromNodeID == Node.GetID()) Outputs.Add(Edge.FromVertexID);
			}
			for (const FMetasoundFrontendVertexLiteral& Literal : Node.InputLiterals) Inputs.Add(Literal.VertexID);
			if (!SupportsMetaSoundVertices(Node.Interface.Inputs, Inputs, Available.Inputs)
				|| !SupportsMetaSoundVertices(Node.Interface.Outputs, Outputs, Available.Outputs)) return false;
		}
		return true;
	}

	void RepairMetaSoundDocument(FMetasoundFrontendDocument& Document) {
		using namespace Metasound::Frontend;
		Document.RootGraph.Metadata.SetType(EMetasoundFrontendClassType::Graph);

		// An interface registration includes editor metadata. Keep the graph's actual ports even
		// when the target editor has no registration for their source interface version.
		for (auto Interface = Document.Interfaces.CreateIterator(); Interface; ++Interface) {
			if (IInterfaceRegistry::Get().FindInterfaceRegistryEntry(GetInterfaceRegistryKey(*Interface)) == nullptr) {
				Interface.RemoveCurrent();
			}
		}

		for (FMetasoundFrontendClass& Dependency : Document.Dependencies) {
			FMetasoundFrontendClassMetadata& Metadata = Dependency.Metadata;
			if (Metadata.GetType() != EMetasoundFrontendClassType::External) continue;
			FGuid AssetClass;
			if (FGuid::Parse(Metadata.GetClassName().Name.ToString(), AssetClass)) continue;

			const FMetasoundFrontendVersionNumber Version = Metadata.GetVersion();
			TArray<FMetasoundFrontendClass> Classes = ISearchEngine::Get().FindClassesWithName(Metadata.GetClassName(), false);
			if (Classes.ContainsByPredicate([&Version](const FMetasoundFrontendClass& Found) { return Found.Metadata.GetVersion() == Version; })) continue;
			Classes.Sort([](const FMetasoundFrontendClass& A, const FMetasoundFrontendClass& B) {
				return B.Metadata.GetVersion() < A.Metadata.GetVersion();
			});
			for (const FMetasoundFrontendClass& Found : Classes) {
				if (Found.Metadata.GetVersion().Major != Version.Major || Found.Metadata.GetType() != Metadata.GetType()) continue;
				if (!SupportsMetaSoundGraph(Document.RootGraph.Graph, Dependency.ID, Found.Interface)
					|| Document.Subgraphs.ContainsByPredicate([&Dependency, &Found](const FMetasoundFrontendGraphClass& Graph) {
						return !SupportsMetaSoundGraph(Graph.Graph, Dependency.ID, Found.Interface);
					})) continue;

				Metadata.SetVersion(Found.Metadata.GetVersion());
				Dependency.Interface = Found.Interface;
				break;
			}
		}
	}
#endif

	class IMetaSoundImporter final : public IObjectImporter {
	public:
		bool Import() override {
			const UClass* Class = GetAssetClass();
			if (!ClassIsFormed(Class)) return false;
			const FStructProperty* DocumentProperty = FindFProperty<FStructProperty>(Class, TEXT("RootMetasoundDocument"));
			const TSharedPtr<FJsonObject>* Document = MetaSoundObject(GetAssetData(), TEXT("RootMetasoundDocument"));
			if (Document == nullptr) Document = MetaSoundObject(GetAssetData(), TEXT("RootMetaSoundDocument"));
			if (DocumentProperty == nullptr || Document == nullptr) return false;

			const FStructProperty* GraphClass = FindFProperty<FStructProperty>(DocumentProperty->Struct, TEXT("RootGraph"));
			const TSharedPtr<FJsonObject>* Root = MetaSoundObject(*Document, TEXT("RootGraph"));
			if (Root == nullptr || GraphClass == nullptr) return false;

			if (FindFProperty<FStructProperty>(GraphClass->Struct, TEXT("Graph")) != nullptr) {
				if (!(*Root)->HasField(TEXT("Graph"))) {
					const TArray<TSharedPtr<FJsonValue>>* Pages = nullptr;
					if (!(*Root)->TryGetArrayField(TEXT("PagedGraphs"), Pages)) return false;
					const TSharedPtr<FJsonObject> Page = MetaSoundDefaultPage(*Pages);
					if (!Page.IsValid()) return false;
					(*Root)->SetObjectField(TEXT("Graph"), Page);
					if (Pages->Num() > 1) FImportIssues::Report(EImportIssue::Data, TEXT("MetaSound used its default page"), GetAssetName());
				}

				RestoreMetaSoundDefaults(*Root);
				const TArray<TSharedPtr<FJsonValue>>* Dependencies = nullptr;
				if ((*Document)->TryGetArrayField(TEXT("Dependencies"), Dependencies)) {
					for (const TSharedPtr<FJsonValue>& Entry : *Dependencies) {
						if (Entry.IsValid() && Entry->Type == EJson::Object) RestoreMetaSoundDefaults(Entry->AsObject());
					}
				}
			}

			return IObjectImporter::Import();
		}

#if REFLECTION_METASOUND_REGISTRY
		void Repair(UObject* Asset) const override {
			const FStructProperty* Property = FindFProperty<FStructProperty>(Asset->GetClass(), TEXT("RootMetasoundDocument"));
			if (Property != nullptr && Property->Struct == FMetasoundFrontendDocument::StaticStruct()) {
				RepairMetaSoundDocument(*Property->ContainerPtrToValuePtr<FMetasoundFrontendDocument>(Asset));

#if REFLECTION_METASOUND_EDITOR_GRAPH
				/* Last, once the document is what it is going to be: the graph is drawn from it.
				 * Any MetaSound it uses that an earlier import left without a graph is mended and
				 * saved along with it, since opening this one opens those too. */
				TArray<UObject*> Repaired;
				EnsureMetaSoundEditorGraph(Asset, &Repaired);
				for (UObject* Used : Repaired) {
					UPackage* Package = Used->GetOutermost();
					if (FAssetUtilities::IsAllowedDestination(Package->GetName())) FAssetUtilities::QueueSave(Package);
				}
#endif
			}
		}
#endif
	};
}

REGISTER_IMPORTER(IMetaSoundImporter, (TArray<FString>{
	TEXT("MetaSoundSource"), TEXT("MetaSoundPatch")
}), TEXT("Sound Assets"));
#endif
