// Copyright Epic Games, Inc. All Rights Reserved.

#include "MetasoundAssetBase.h"

#include "Algo/AnyOf.h"
#include "Algo/Transform.h"
#include "Containers/Set.h"
#include "HAL/FileManager.h"
#include "IAudioParameterTransmitter.h"
#include "Internationalization/Text.h"
#include "IStructSerializerBackend.h"
#include "Logging/LogMacros.h"
#include "MetasoundArchetype.h"
#include "MetasoundAssetManager.h"
#include "MetasoundFrontendArchetypeRegistry.h"
#include "MetasoundFrontendController.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendGraph.h"
#include "MetasoundFrontendInjectReceiveNodes.h"
#include "MetasoundFrontendRegistries.h"
#include "MetasoundFrontendSearchEngine.h"
#include "MetasoundFrontendTransform.h"
#include "MetasoundJsonBackend.h"
#include "MetasoundLog.h"
#include "MetasoundParameterTransmitter.h"
#include "MetasoundTrace.h"
#include "MetasoundVertex.h"
#include "StructSerializer.h"
#include "UObject/MetaData.h"

#define LOCTEXT_NAMESPACE "MetaSound"

namespace Metasound
{
	namespace AssetBasePrivate
	{
		void DepthFirstTraversal(const FMetasoundAssetBase& InInitAsset, TFunctionRef<TSet<const FMetasoundAssetBase*>(const FMetasoundAssetBase&)> InVisitFunction)
		{
			// Non recursive depth first traversal.
			TArray<const FMetasoundAssetBase*> Stack({ &InInitAsset });
			TSet<const FMetasoundAssetBase*> Visited;

			while (!Stack.IsEmpty())
			{
				const FMetasoundAssetBase* CurrentNode = Stack.Pop();
				if (!Visited.Contains(CurrentNode))
				{
					TArray<const FMetasoundAssetBase*> Children = InVisitFunction(*CurrentNode).Array();
					Stack.Append(Children);

					Visited.Add(CurrentNode);
				}
			}
		}
	} // namespace AssetBasePrivate
} // namespace Metasound

const FString FMetasoundAssetBase::FileExtension(TEXT(".metasound"));

void FMetasoundAssetBase::RegisterGraphWithFrontend(Metasound::Frontend::FMetaSoundAssetRegistrationOptions InRegistrationOptions)
{
	using namespace Metasound;
	using namespace Metasound::Frontend;

	METASOUND_TRACE_CPUPROFILER_EVENT_SCOPE(MetaSoundAssetBase::RegisterGraphWithFrontend);
	if (!InRegistrationOptions.bForceReregister)
	{
		if (IsRegistered())
		{
			return;
		}
	}

	// Triggers the existing runtime data to be out-of-date.
	CurrentCachedRuntimeDataChangeID = FGuid::NewGuid();

	if (InRegistrationOptions.bRegisterDependencies)
	{
		// Must be called in case register is called prior to asset scan being completed
		IMetaSoundAssetManager::GetChecked().AddAssetReferences(*this);
	}

	if (InRegistrationOptions.bRebuildReferencedAssetClassKeys)
	{
		TSet<FNodeRegistryKey> ReferencedKeys = IMetaSoundAssetManager::GetChecked().GetReferencedKeys(*this);
		SetReferencedAssetClassKeys(MoveTemp(ReferencedKeys));
	}

	if (InRegistrationOptions.bRegisterDependencies)
	{
		TArray<FMetasoundAssetBase*> References;
		ensureAlways(IMetaSoundAssetManager::GetChecked().TryLoadReferencedAssets(*this, References));

		GetReferencedAssetClassCache().Reset();
		for (FMetasoundAssetBase* Reference : References)
		{
			if (InRegistrationOptions.bForceReregister || !Reference->IsRegistered())
			{
				// TODO: Check for infinite recursion and error if so
				Reference->RegisterGraphWithFrontend(InRegistrationOptions);
			}

			if (UObject* RefAsset = Reference->GetOwningAsset())
			{
				GetReferencedAssetClassCache().Add(RefAsset);
			}
		}
	}

	// Auto update must be done after all referenced asset classes are registered
	if (InRegistrationOptions.bAutoUpdate)
	{
		const bool bWasAutoUpdated = AutoUpdate();
		if (bWasAutoUpdated)
		{
#if WITH_EDITORONLY_DATA
			SetSynchronizationRequired();
#endif // WITH_EDITORONLY_DATA
		}
	}

	// Registers node by copying document. Updates to document require re-registration.
	class FNodeRegistryEntry : public INodeRegistryEntry
	{
	public:
		FNodeRegistryEntry(const FString& InName, const FMetasoundFrontendDocument& InDocument, FName InAssetPath)
		: Name(InName)
		, Document(InDocument)
		{
			// Copy frontend class to preserve original document.
			FrontendClass = Document.RootGraph;
			FrontendClass.Metadata.SetType(EMetasoundFrontendClassType::External);
			ClassInfo = FNodeClassInfo(Document.RootGraph, InAssetPath);
		}

		virtual ~FNodeRegistryEntry() = default;

		virtual const FNodeClassInfo& GetClassInfo() const override
		{
			return ClassInfo;
		}

		virtual TUniquePtr<INode> CreateNode(const FNodeInitData&) const override
		{
			return FFrontendGraphBuilder().CreateGraph(Document);
		}

		virtual TUniquePtr<INode> CreateNode(FDefaultLiteralNodeConstructorParams&&) const override
		{
			return nullptr;
		}

		virtual TUniquePtr<INode> CreateNode(FDefaultNamedVertexNodeConstructorParams&&) const override
		{
			return nullptr;
		}

		virtual TUniquePtr<INode> CreateNode(FDefaultNamedVertexWithLiteralNodeConstructorParams&&) const override
		{
			return nullptr;
		}


		virtual const FMetasoundFrontendClass& GetFrontendClass() const override
		{
			return FrontendClass;
		}

		virtual TUniquePtr<INodeRegistryEntry> Clone() const override
		{
			return MakeUnique<FNodeRegistryEntry>(Name, Document, ClassInfo.AssetPath);
		}

		virtual bool IsNative() const override
		{
			return false;
		}

	private:
		
		FString Name;
		FMetasoundFrontendDocument Document;
		FMetasoundFrontendClass FrontendClass;
		FNodeClassInfo ClassInfo;
	};

	UnregisterGraphWithFrontend();

	FString AssetName;
	FString AssetPath;
	const UObject* OwningAsset = GetOwningAsset();
	if (ensure(OwningAsset))
	{
		AssetName = OwningAsset->GetName();
		AssetPath = OwningAsset->GetPathName();
	}

	FNodeClassInfo AssetClassInfo = GetAssetClassInfo();
	const FMetasoundFrontendDocument* Doc = GetDocument().Get();
	if (Doc)
	{
		RegistryKey = FMetasoundFrontendRegistryContainer::Get()->RegisterNode(MakeUnique<FNodeRegistryEntry>(AssetName, *Doc, AssetClassInfo.AssetPath));
	}

	if (NodeRegistryKey::IsValid(RegistryKey))
	{
#if WITH_EDITORONLY_DATA
		// Refresh Asset Registry Info if successfully registered with Frontend
		const FMetasoundFrontendGraphClass& DocumentClassGraph = GetDocumentHandle()->GetRootGraphClass();
		const FMetasoundFrontendClassMetadata& DocumentClassMetadata = DocumentClassGraph.Metadata;
		AssetClassInfo.AssetClassID = FGuid(DocumentClassMetadata.GetClassName().Name.ToString());
		FNodeClassName ClassName = DocumentClassMetadata.GetClassName().ToNodeClassName();
		FMetasoundFrontendClass GraphClass;
		ensure(ISearchEngine::Get().FindClassWithMajorVersion(ClassName, DocumentClassMetadata.GetVersion().Major, GraphClass));

		AssetClassInfo.Version = DocumentClassMetadata.GetVersion();

		AssetClassInfo.InputTypes.Reset();
		Algo::Transform(GraphClass.Interface.Inputs, AssetClassInfo.InputTypes, [] (const FMetasoundFrontendClassInput& Input) { return Input.TypeName; });

		AssetClassInfo.OutputTypes.Reset();
		Algo::Transform(GraphClass.Interface.Outputs, AssetClassInfo.OutputTypes, [](const FMetasoundFrontendClassOutput& Output) { return Output.TypeName; });

		SetRegistryAssetClassInfo(MoveTemp(AssetClassInfo));
#endif // WITH_EDITORONLY_DATA
	}
	else
	{
		FString ClassName;
		if (OwningAsset)
		{
			if (UClass* Class = OwningAsset->GetClass())
			{
				ClassName = Class->GetName();
			}
		}
		UE_LOG(LogMetaSound, Error, TEXT("Registration failed for MetaSound node class '%s' of UObject class '%s'"), *AssetName, *ClassName);
	}
}

void FMetasoundAssetBase::UnregisterGraphWithFrontend()
{
	using namespace Metasound::Frontend;
	METASOUND_TRACE_CPUPROFILER_EVENT_SCOPE(MetaSoundAssetBase::UnregisterGraphWithFrontend);

	if (!NodeRegistryKey::IsValid(RegistryKey))
	{
		return;
	}

	const UObject* OwningAsset = GetOwningAsset();
	if (!ensureAlways(OwningAsset))
	{
		return;
	}

	ensureAlways(FMetasoundFrontendRegistryContainer::Get()->UnregisterNode(RegistryKey));
	RegistryKey = FNodeRegistryKey();
}

void FMetasoundAssetBase::SetMetadata(FMetasoundFrontendClassMetadata& InMetadata)
{
	FMetasoundFrontendDocument& Doc = GetDocumentChecked();
	Doc.RootGraph.Metadata = InMetadata;

	if (Doc.RootGraph.Metadata.GetType() != EMetasoundFrontendClassType::Graph)
	{
		UE_LOG(LogMetaSound, Display, TEXT("Forcing class type to EMetasoundFrontendClassType::Graph on root graph metadata"));
		Doc.RootGraph.Metadata.SetType(EMetasoundFrontendClassType::Graph);
	}

	MarkMetasoundDocumentDirty();
}

bool FMetasoundAssetBase::GetDeclaredInterfaces(TArray<const Metasound::Frontend::IInterfaceRegistryEntry*>& OutInterfaces) const
{
	using namespace Metasound;
	using namespace Metasound::Frontend;

	if (const FMetasoundFrontendDocument* Document = GetDocument().Get())
	{
		bool bInterfacesFound = true;

		Algo::Transform(Document->InterfaceVersions, OutInterfaces, [&](const FMetasoundFrontendVersion& Version)
		{
			const FInterfaceRegistryKey InterfaceKey = GetInterfaceRegistryKey(Version);
			const IInterfaceRegistryEntry* RegistryEntry = IInterfaceRegistry::Get().FindInterfaceRegistryEntry(InterfaceKey);
			if (!RegistryEntry)
			{
				bInterfacesFound = false;
				UE_LOG(LogMetaSound, Warning, TEXT("No registered interface matching interface version on document [InterfaceVersion:%s]"), *Version.ToString());
			}

			return RegistryEntry;
		});
	
		return bInterfacesFound;
	}

	return false;
}

bool FMetasoundAssetBase::IsInterfaceDeclared(FName InName) const
{
	return Algo::AnyOf(GetDocumentChecked().InterfaceVersions, [&InName](const FMetasoundFrontendVersion& FrontendVersion)
	{
		return FrontendVersion.Name == InName;
	});
}

void FMetasoundAssetBase::SetDocument(const FMetasoundFrontendDocument& InDocument)
{
	FMetasoundFrontendDocument& Document = GetDocumentChecked();
	Document = InDocument;
	MarkMetasoundDocumentDirty();
}

void FMetasoundAssetBase::AddDefaultInterfaces()
{
	using namespace Metasound::Frontend;

	UObject* OwningAsset = GetOwningAsset();
	check(OwningAsset);

	UClass* AssetClass = OwningAsset->GetClass();
	check(AssetClass);

	FDocumentHandle DocumentHandle = GetDocumentHandle();

	TArray<FMetasoundFrontendInterface> InitInterfaces = ISearchEngine::Get().FindUClassDefaultInterfaces(AssetClass->GetFName());
	FModifyRootGraphInterfaces({ }, InitInterfaces).Transform(DocumentHandle);
}

bool FMetasoundAssetBase::AutoUpdate(bool bInMarkDirty)
{
	using namespace Metasound::Frontend;
	METASOUND_TRACE_CPUPROFILER_EVENT_SCOPE(MetaSoundAssetBase::AutoUpdate);

	const bool bUpdated = FAutoUpdateRootGraph().Transform(GetDocumentHandle());
	if (bUpdated && bInMarkDirty)
	{
		MarkMetasoundDocumentDirty();
	}

	return bUpdated;
}

bool FMetasoundAssetBase::VersionAsset()
{
	using namespace Metasound;
	using namespace Metasound::Frontend;
	METASOUND_TRACE_CPUPROFILER_EVENT_SCOPE(MetaSoundAssetBase::VersionAsset);

	FName AssetName;
	FString AssetPath;
	if (const UObject* OwningAsset = GetOwningAsset())
	{
		AssetName = FName(OwningAsset->GetName());
		AssetPath = OwningAsset->GetPathName();
	}

	FMetasoundFrontendDocument* Doc = GetDocument().Get();
	if (!ensure(Doc))
	{
		return false;
	}

	// Data migration for 5.0 Early Access data. ArchetypeVersion can be removed post 5.0 release.
	bool bDidEdit = false;
	if (Doc->ArchetypeVersion.IsValid())
	{
		Doc->InterfaceVersions.Add(Doc->ArchetypeVersion);
		Doc->ArchetypeVersion = FMetasoundFrontendVersion::GetInvalid();
		bDidEdit = true;
	}

	FDocumentHandle DocHandle = GetDocumentHandle();

	// Version Document Model
	{
		bDidEdit |= FVersionDocument(AssetName, AssetPath).Transform(DocHandle);
	}

	// Version Interfaces
	{
		bool bInterfaceUpdated = false;
		for (const FMetasoundFrontendVersion& Version : Doc->InterfaceVersions)
		{
			bInterfaceUpdated |= FUpdateRootGraphInterface(Version).Transform(DocHandle);
		}
		if (bInterfaceUpdated)
		{
			ConformObjectDataToInterfaces();
		}
		bDidEdit |= bInterfaceUpdated;
	}

	return bDidEdit;
}

#if WITH_EDITORONLY_DATA
bool FMetasoundAssetBase::GetSynchronizationPending() const
{
	return bSynchronizationRequired;
}

bool FMetasoundAssetBase::GetSynchronizationClearUpdateNotes() const
{
	return bSynchronizationClearUpdateNotes;
}

bool FMetasoundAssetBase::GetSynchronizationInterfacesUpdated() const
{
	return bSynchronizationInterfacesUpdated;
}

void FMetasoundAssetBase::SetSynchronizationRequired()
{
	bSynchronizationRequired = true;
}

void FMetasoundAssetBase::SetClearNodeNotesOnSynchronization()
{
	bSynchronizationClearUpdateNotes = true;
}

void FMetasoundAssetBase::SetInterfacesUpdatedOnSynchronization()
{
	bSynchronizationInterfacesUpdated = true;
}

void FMetasoundAssetBase::ResetSynchronizationState()
{
	bSynchronizationClearUpdateNotes = false;
	bSynchronizationInterfacesUpdated = false;
	bSynchronizationRequired = false;
}
#endif // WITH_EDITORONLY_DATA

TSharedPtr<Metasound::IGraph, ESPMode::ThreadSafe> FMetasoundAssetBase::BuildMetasoundDocument() const
{
	using namespace Metasound;
	using namespace Metasound::Frontend;

	METASOUND_TRACE_CPUPROFILER_EVENT_SCOPE(MetaSoundAssetBase::BuildMetasoundDocument);

	// Create graph which can spawn instances. TODO: cache graph.
	TUniquePtr<FFrontendGraph> FrontendGraph = FFrontendGraphBuilder::CreateGraph(GetDocumentChecked());
	if (FrontendGraph.IsValid())
	{
		const TArray<const FMetasoundFrontendClassInput*> TransmittableInputs = GetTransmittableClassInputs();

		TSet<FVertexName> TransmittableInputNames;
		Algo::Transform(TransmittableInputs, TransmittableInputNames, [](const FMetasoundFrontendClassInput* Input) { return Input->Name; });

		bool bSuccessfullyInjectedReceiveNodes = InjectReceiveNodes(*FrontendGraph, FMetaSoundParameterTransmitter::CreateSendAddressFromEnvironment, TransmittableInputNames);
		if (!bSuccessfullyInjectedReceiveNodes)
		{
			UE_LOG(LogMetaSound, Error, TEXT("Error while injecting async communication hooks. Instance communication may not function properly [Name:%s]."), *GetOwningAssetName());
		}
	}

	TSharedPtr<Metasound::IGraph, ESPMode::ThreadSafe> SharedGraph(FrontendGraph.Release());

	return SharedGraph;
}

bool FMetasoundAssetBase::IsRegistered() const
{
	using namespace Metasound::Frontend;

	if (!NodeRegistryKey::IsValid(RegistryKey))
	{
		return false;
	}

	return FMetasoundFrontendRegistryContainer::Get()->IsNodeRegistered(RegistryKey);
}

bool FMetasoundAssetBase::IsReferencedAsset(const FMetasoundAssetBase& InAsset) const
{
	using namespace Metasound::Frontend;

	bool bIsReferenced = false;
	Metasound::AssetBasePrivate::DepthFirstTraversal(*this, [&](const FMetasoundAssetBase& ChildAsset)
	{
		TSet<const FMetasoundAssetBase*> Children;
		if (&ChildAsset == &InAsset)
		{
			bIsReferenced = true;
			return Children;
		}

		TArray<FMetasoundAssetBase*> ChildRefs;
		ensureAlways(IMetaSoundAssetManager::GetChecked().TryLoadReferencedAssets(ChildAsset, ChildRefs));
		Algo::Transform(ChildRefs, Children, [](FMetasoundAssetBase* Child) { return Child; });
		return Children;

	});

	return bIsReferenced;
}

bool FMetasoundAssetBase::AddingReferenceCausesLoop(const FSoftObjectPath& InReferencePath) const
{
	using namespace Metasound::Frontend;

	const FMetasoundAssetBase* ReferenceAsset = IMetaSoundAssetManager::GetChecked().TryLoadAsset(InReferencePath);
	if (!ensureAlways(ReferenceAsset))
	{
		return false;
	}

	bool bCausesLoop = false;
	const FMetasoundAssetBase* Parent = this;
	Metasound::AssetBasePrivate::DepthFirstTraversal(*ReferenceAsset, [&](const FMetasoundAssetBase& ChildAsset)
	{
		TSet<const FMetasoundAssetBase*> Children;
		if (Parent == &ChildAsset)
		{
			bCausesLoop = true;
			return Children;
		}

		TArray<FMetasoundAssetBase*> ChildRefs;
		ensureAlways(IMetaSoundAssetManager::GetChecked().TryLoadReferencedAssets(ChildAsset, ChildRefs));
		Algo::Transform(ChildRefs, Children, [] (FMetasoundAssetBase* Child) { return Child; });
		return Children;
	});

	return bCausesLoop;
}

Metasound::FSendAddress FMetasoundAssetBase::CreateSendAddress(uint64 InInstanceID, const Metasound::FVertexName& InVertexName, const FName& InDataTypeName) const

{
	return Metasound::FSendAddress(InVertexName, InDataTypeName, InInstanceID);
}

void FMetasoundAssetBase::ConvertFromPreset()
{
	using namespace Metasound::Frontend;
	FGraphHandle GraphHandle = GetRootGraphHandle();
	FMetasoundFrontendGraphStyle Style = GraphHandle->GetGraphStyle();
	Style.bIsGraphEditable = true;
	GraphHandle->SetGraphStyle(Style);

	FMetasoundFrontendClassMetadata Metadata = GraphHandle->GetGraphMetadata();
	Metadata.SetAutoUpdateManagesInterface(false);
	GraphHandle->SetGraphMetadata(Metadata);
}

TArray<FMetasoundAssetBase::FSendInfoAndVertexName> FMetasoundAssetBase::GetSendInfos(uint64 InInstanceID) const
{
	using namespace Metasound;
	using namespace Metasound::Frontend;
	using FSendInfo = FMetaSoundParameterTransmitter::FSendInfo;

	check(IsInGameThread() || IsInAudioThread());

	const FRuntimeData& RuntimeData = GetRuntimeData();

	TArray<FSendInfoAndVertexName> SendInfos;

	for (const FMetasoundFrontendClassInput& Vertex : RuntimeData.TransmittableInputs)
	{
		FSendInfoAndVertexName Info;

		Info.SendInfo.Address = FMetaSoundParameterTransmitter::CreateSendAddressFromInstanceID(InInstanceID, Vertex.Name, Vertex.TypeName);
		Info.SendInfo.ParameterName = Vertex.Name;
		Info.SendInfo.TypeName = Vertex.TypeName;
		Info.VertexName = Vertex.Name;

		SendInfos.Add(Info);
		
	}

	return SendInfos;
}

Metasound::Frontend::FNodeHandle FMetasoundAssetBase::AddInputPinForSendAddress(const Metasound::FMetaSoundParameterTransmitter::FSendInfo& InSendInfo, Metasound::Frontend::FGraphHandle InGraph) const
{
	FMetasoundFrontendClassInput Description;
	FGuid VertexID = FGuid::NewGuid();

	Description.Name = InSendInfo.Address.GetChannelName();
	Description.TypeName = Metasound::GetMetasoundDataTypeName<Metasound::FSendAddress>();
	Description.Metadata.Description = FText::GetEmpty();
	Description.VertexID = VertexID;
	Description.DefaultLiteral.Set(InSendInfo.Address.GetChannelName().ToString());

	return InGraph->AddInputVertex(Description);
}

#if WITH_EDITORONLY_DATA
FText FMetasoundAssetBase::GetDisplayName(FString&& InTypeName) const
{
	using namespace Metasound::Frontend;

	FConstGraphHandle GraphHandle = GetRootGraphHandle();
	const bool bIsPreset = !GraphHandle->GetGraphStyle().bIsGraphEditable;

	if (!bIsPreset)
	{
		return FText::FromString(MoveTemp(InTypeName));
	}

	return FText::Format(LOCTEXT("PresetDisplayNameFormat", "{0} (Preset)"), FText::FromString(MoveTemp(InTypeName)));
}
#endif // WITH_EDITORONLY_DATA

bool FMetasoundAssetBase::MarkMetasoundDocumentDirty() const
{
	if (const UObject* OwningAsset = GetOwningAsset())
	{
		return ensure(OwningAsset->MarkPackageDirty());
	}
	return false;
}

Metasound::Frontend::FDocumentHandle FMetasoundAssetBase::GetDocumentHandle()
{
	return Metasound::Frontend::IDocumentController::CreateDocumentHandle(GetDocument());
}

Metasound::Frontend::FConstDocumentHandle FMetasoundAssetBase::GetDocumentHandle() const
{
	return Metasound::Frontend::IDocumentController::CreateDocumentHandle(GetDocument());
}

Metasound::Frontend::FGraphHandle FMetasoundAssetBase::GetRootGraphHandle()
{
	return GetDocumentHandle()->GetRootGraph();
}

Metasound::Frontend::FConstGraphHandle FMetasoundAssetBase::GetRootGraphHandle() const
{
	return GetDocumentHandle()->GetRootGraph();
}

bool FMetasoundAssetBase::ImportFromJSON(const FString& InJSON)
{
	METASOUND_TRACE_CPUPROFILER_EVENT_SCOPE(MetaSoundAssetBase::ImportFromJSON);

	FMetasoundFrontendDocument* Document = GetDocument().Get();
	if (ensure(nullptr != Document))
	{
		bool bSuccess = Metasound::Frontend::ImportJSONToMetasound(InJSON, *Document);

		if (bSuccess)
		{
			ensure(MarkMetasoundDocumentDirty());
		}

		return bSuccess;
	}
	return false;
}

bool FMetasoundAssetBase::ImportFromJSONAsset(const FString& InAbsolutePath)
{
	METASOUND_TRACE_CPUPROFILER_EVENT_SCOPE(MetaSoundAssetBase::ImportFromJSONAsset);

	Metasound::Frontend::FDocumentAccessPtr DocumentPtr = GetDocument();
	if (FMetasoundFrontendDocument* Document = DocumentPtr.Get())
	{
		bool bSuccess = Metasound::Frontend::ImportJSONAssetToMetasound(InAbsolutePath, *Document);

		if (bSuccess)
		{
			ensure(MarkMetasoundDocumentDirty());
		}

		return bSuccess;
	}
	return false;
}

FMetasoundFrontendDocument& FMetasoundAssetBase::GetDocumentChecked()
{
	FMetasoundFrontendDocument* Document = GetDocument().Get();
	check(nullptr != Document);
	return *Document;
}

const FMetasoundFrontendDocument& FMetasoundAssetBase::GetDocumentChecked() const
{
	const FMetasoundFrontendDocument* Document = GetDocument().Get();

	check(nullptr != Document);
	return *Document;
}

FString FMetasoundAssetBase::GetOwningAssetName() const
{
	if (const UObject* OwningAsset = GetOwningAsset())
	{
		return OwningAsset->GetName();
	}
	return FString();
}

TSharedPtr<const Metasound::IGraph, ESPMode::ThreadSafe> FMetasoundAssetBase::GetMetasoundCoreGraph() const
{
	return FMetasoundAssetBase::GetRuntimeData().Graph;
}

TArray<const FMetasoundFrontendClassInput*> FMetasoundAssetBase::GetTransmittableClassInputs() const
{
	using namespace Metasound;
	using namespace Metasound::Frontend;

	check(IsInGameThread() || IsInAudioThread());
	TArray<const FMetasoundFrontendClassInput*> Inputs;

	const FMetasoundFrontendDocument& Doc = GetDocumentChecked();
	auto GetInputName = [](const FMetasoundFrontendClassInput& InInput) { return InInput.Name; };

	// Do not transmit vertices defined in interface marked as non-transmittable
	TArray<const IInterfaceRegistryEntry*> Interfaces;
	TSet<FVertexName> NonTransmittableInputs;
	GetDeclaredInterfaces(Interfaces);
	for (const IInterfaceRegistryEntry* InterfaceEntry : Interfaces)
	{
		if (InterfaceEntry)
		{
			if (InterfaceEntry->GetRouterName() != Audio::IParameterTransmitter::RouterName)
			{
				const FMetasoundFrontendInterface& Interface = InterfaceEntry->GetInterface();
				Algo::Transform(Interface.Inputs, NonTransmittableInputs, GetInputName);
			}
		}
	}

	// Do not transmit vertices which are not transmittable. Async communication
	// is not supported without transmission.
	IDataTypeRegistry& Registry = IDataTypeRegistry::Get();
	auto IsTransmittable = [&Registry, &NonTransmittableInputs](const FMetasoundFrontendClassVertex& InVertex)
	{
		if (!NonTransmittableInputs.Contains(InVertex.Name))
		{
			FDataTypeRegistryInfo Info;
			if (Registry.GetDataTypeInfo(InVertex.TypeName, Info))
			{
				return Info.bIsTransmittable;
			}
		}

		return false;
	};
	Algo::TransformIf(Doc.RootGraph.Interface.Inputs, Inputs, IsTransmittable, [] (const FMetasoundFrontendClassInput& Input) { return &Input; });

	return Inputs;
}

const FMetasoundAssetBase::FRuntimeData& FMetasoundAssetBase::GetRuntimeData() const
{
	
	// Check if a ChangeID has been generated before.
	if (!CurrentCachedRuntimeDataChangeID.IsValid())
	{
		CurrentCachedRuntimeDataChangeID = FGuid::NewGuid();
	}

	// Check if CachedRuntimeData is out-of-date.
	if (CachedRuntimeData.ChangeID != CurrentCachedRuntimeDataChangeID)
	{
		// Update CachedRuntimeData.
		CachedRuntimeData.TransmittableInputs.Reset();
		TArray<const FMetasoundFrontendClassInput*> ClassInputs = GetTransmittableClassInputs();
		Algo::Transform(ClassInputs, CachedRuntimeData.TransmittableInputs, [] (const FMetasoundFrontendClassInput* Input) { return *Input; });

		CachedRuntimeData.Graph = BuildMetasoundDocument();
		CachedRuntimeData.ChangeID = CurrentCachedRuntimeDataChangeID;
	}

	return CachedRuntimeData;
}

#undef LOCTEXT_NAMESPACE // "MetaSound"
