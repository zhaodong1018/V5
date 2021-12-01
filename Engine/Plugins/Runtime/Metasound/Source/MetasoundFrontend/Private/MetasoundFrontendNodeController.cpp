// Copyright Epic Games, Inc. All Rights Reserved.

#include "MetasoundFrontendNodeController.h"

#include "Algo/Transform.h"
#include "HAL/IConsoleManager.h"
#include "MetasoundFrontendDocumentAccessPtr.h"
#include "MetasoundFrontendInputController.h"
#include "MetasoundFrontendInvalidController.h"
#include "MetasoundFrontendOutputController.h"
#include "MetasoundFrontendSearchEngine.h"
#include "MetasoundLog.h"
#include "MetasoundTrace.h"

#define LOCTEXT_NAMESPACE "MetasoundFrontendNodeController"

static int32 MetaSoundAutoUpdateNativeClassCVar = 1;
FAutoConsoleVariableRef CVarMetaSoundAutoUpdateNativeClass(
	TEXT("au.MetaSounds.AutoUpdate.NativeClasses"),
	MetaSoundAutoUpdateNativeClassCVar,
	TEXT("If true, node references to native class that share a version number will attempt to auto-update if the interface is different, which results in slower graph load times.\n")
	TEXT("0: Don't auto-update native classes, !0: Auto-update native classes (default)"),
	ECVF_Default);

namespace Metasound
{
	namespace Frontend
	{
		//
		// FBaseNodeController
		//
		FBaseNodeController::FBaseNodeController(const FBaseNodeController::FInitParams& InParams)
		: NodePtr(InParams.NodePtr)
		, ClassPtr(InParams.ClassPtr)
		, OwningGraph(InParams.OwningGraph)
		{
			if (FMetasoundFrontendNode* Node = NodePtr.Get())
			{
				if (const FMetasoundFrontendClass* Class = ClassPtr.Get())
				{
					if (Node->ClassID != Class->ID)
					{
						UE_LOG(LogMetaSound, Warning, TEXT("Changing node's class id from [ClassID:%s] to [ClassID:%s]"), *Node->ClassID.ToString(), *Class->ID.ToString());
						Node->ClassID = Class->ID;
					}
				}
			}
		}

		bool FBaseNodeController::IsValid() const
		{
			return OwningGraph->IsValid() && (nullptr != NodePtr.Get()) && (nullptr != ClassPtr.Get());
		}

		FGuid FBaseNodeController::GetOwningGraphClassID() const
		{
			return OwningGraph->GetClassID();
		}

		FGraphHandle FBaseNodeController::GetOwningGraph()
		{
			return OwningGraph;
		}

		FConstGraphHandle FBaseNodeController::GetOwningGraph() const
		{
			return OwningGraph;
		}

		FGuid FBaseNodeController::GetID() const
		{
			if (const FMetasoundFrontendNode* Node = NodePtr.Get())
			{
				return Node->GetID();
			}
			return Metasound::FrontendInvalidID;
		}

		FGuid FBaseNodeController::GetClassID() const
		{
			if (const FMetasoundFrontendClass* Class = ClassPtr.Get())
			{
				return Class->ID;
			}
			return Metasound::FrontendInvalidID;
		}

		const FMetasoundFrontendLiteral* FBaseNodeController::GetInputLiteral(const FGuid& InVertexID) const
		{
			if (const FMetasoundFrontendNode* Node = NodePtr.Get())
			{
				for (const FMetasoundFrontendVertexLiteral& VertexLiteral : Node->InputLiterals)
				{
					if (VertexLiteral.VertexID == InVertexID)
					{
						return &VertexLiteral.Value;
					}
				}
			}

			return nullptr;
		}

		void FBaseNodeController::SetInputLiteral(const FMetasoundFrontendVertexLiteral& InVertexLiteral)
		{
			if (FMetasoundFrontendNode* Node = NodePtr.Get())
			{
				auto IsInputVertex = [InVertexLiteral] (const FMetasoundFrontendVertex& Vertex)
				{
					return InVertexLiteral.VertexID == Vertex.VertexID;
				};

				FMetasoundFrontendNodeInterface& NodeInterface = Node->Interface;
				if (!ensure(NodeInterface.Inputs.ContainsByPredicate(IsInputVertex)))
				{
					return;
				}

				for (FMetasoundFrontendVertexLiteral& VertexLiteral : Node->InputLiterals)
				{
					if (VertexLiteral.VertexID == InVertexLiteral.VertexID)
					{
						if (ensure(VertexLiteral.Value.GetType() == InVertexLiteral.Value.GetType()))
						{
							VertexLiteral = InVertexLiteral;
						}
						return;
					}
				}

				Node->InputLiterals.Add(InVertexLiteral);
			}
		}

		bool FBaseNodeController::ClearInputLiteral(FGuid InVertexID)
		{
			if (FMetasoundFrontendNode* Node = NodePtr.Get())
			{
				auto IsInputVertex = [InVertexID](const FMetasoundFrontendVertexLiteral& VertexLiteral)
				{
					return InVertexID == VertexLiteral.VertexID;
				};

				return Node->InputLiterals.RemoveAllSwap(IsInputVertex, false) > 0;
			}

			return false;
		}

		const FMetasoundFrontendClassInterface& FBaseNodeController::GetClassInterface() const
		{
			if (const FMetasoundFrontendClass* Class = ClassPtr.Get())
			{
				return Class->Interface;
			}
			return Invalid::GetInvalidClassInterface();
		}

		const FMetasoundFrontendClassMetadata& FBaseNodeController::GetClassMetadata() const
		{
			if (const FMetasoundFrontendClass* Class = ClassPtr.Get())
			{
				return Class->Metadata;
			}
			return Invalid::GetInvalidClassMetadata();
		}

		const FMetasoundFrontendInterfaceStyle& FBaseNodeController::GetInputStyle() const
		{
			if (const FMetasoundFrontendClass* Class = ClassPtr.Get())
			{
				return Class->Interface.GetInputStyle();
			}

			return Invalid::GetInvalidInterfaceStyle();
		}

		const FMetasoundFrontendInterfaceStyle& FBaseNodeController::GetOutputStyle() const
		{
			if (const FMetasoundFrontendClass* Class = ClassPtr.Get())
			{
				return Class->Interface.GetOutputStyle();
			}

			return Invalid::GetInvalidInterfaceStyle();
		}

		const FMetasoundFrontendClassStyle& FBaseNodeController::GetClassStyle() const
		{
			if (const FMetasoundFrontendClass* Class = ClassPtr.Get())
			{
				return Class->Style;
			}

			static const FMetasoundFrontendClassStyle Invalid;
			return Invalid;
		}

		const FMetasoundFrontendNodeStyle& FBaseNodeController::GetNodeStyle() const
		{
			if (const FMetasoundFrontendNode* Node = NodePtr.Get())
			{
				return Node->Style;
			}

			static const FMetasoundFrontendNodeStyle Invalid;
			return Invalid;
		}

		void FBaseNodeController::SetNodeStyle(const FMetasoundFrontendNodeStyle& InStyle)
		{
			if (FMetasoundFrontendNode* Node = NodePtr.Get())
			{
				Node->Style = InStyle;
			}
		}

		const FText& FBaseNodeController::GetDescription() const
		{
			if (const FMetasoundFrontendClass* Class = ClassPtr.Get())
			{
				return Class->Metadata.GetDescription();
			}
			return Invalid::GetInvalidText();
		}

		const FVertexName& FBaseNodeController::GetNodeName() const
		{
			if (const FMetasoundFrontendNode* Node = NodePtr.Get())
			{
				return Node->Name;
			}
			return Invalid::GetInvalidName();
		}

		bool FBaseNodeController::CanAddInput(const FVertexName& InVertexName) const
		{
			// TODO: not yet supported
			return false;
		}

		FInputHandle FBaseNodeController::AddInput(const FVertexName& InVertexName, const FMetasoundFrontendLiteral* InDefault)
		{
			checkNoEntry();
			// TODO: not yet supported
			return IInputController::GetInvalidHandle();
		}

		bool FBaseNodeController::RemoveInput(FGuid InVertexID)
		{
			checkNoEntry();
			// TODO: not yet supported
			return false;
		}

		bool FBaseNodeController::CanAddOutput(const FVertexName& InVertexName) const
		{
			// TODO: not yet supported
			return false;
		}

		FInputHandle FBaseNodeController::AddOutput(const FVertexName& InVertexName, const FMetasoundFrontendLiteral* InDefault)
		{
			checkNoEntry();
			// TODO: not yet supported
			return IInputController::GetInvalidHandle();
		}

		bool FBaseNodeController::RemoveOutput(FGuid InVertexID)
		{
			checkNoEntry();
			// TODO: not yet supported
			return false;
		}

		TArray<FInputHandle> FBaseNodeController::GetInputs()
		{
			TArray<FInputHandle> Inputs;

			FNodeHandle ThisNode = this->AsShared();

			for (const FInputControllerParams& Params : GetInputControllerParams())
			{
				FInputHandle InputHandle = CreateInputController(Params.VertexID, Params.NodeVertexPtr, Params.ClassInputPtr, ThisNode);
				if (InputHandle->IsValid())
				{
					Inputs.Add(InputHandle);
				}
			}

			return Inputs;
		}

		int32 FBaseNodeController::GetNumInputs() const
		{
			if (const FMetasoundFrontendNode* Node = NodePtr.Get())
			{
				return Node->Interface.Inputs.Num();
			}

			return 0;
		}

		void FBaseNodeController::IterateInputs(TUniqueFunction<void(FInputHandle)> InFunction)
		{
			for (const FInputControllerParams& Params : GetInputControllerParams())
			{
				FInputHandle InputHandle = CreateInputController(Params.VertexID, Params.NodeVertexPtr, Params.ClassInputPtr, AsShared());
				if (InputHandle->IsValid())
				{
					InFunction(InputHandle);
				}
			}
		}

		TArray<FOutputHandle> FBaseNodeController::GetOutputs()
		{
			TArray<FOutputHandle> Outputs;

			FNodeHandle ThisNode = this->AsShared();

			for (const FOutputControllerParams& Params : GetOutputControllerParams())
			{
				FOutputHandle OutputHandle = CreateOutputController(Params.VertexID, Params.NodeVertexPtr, Params.ClassOutputPtr, ThisNode);
				if (OutputHandle->IsValid())
				{
					Outputs.Add(OutputHandle);
				}
			}

			return Outputs;
		}

		int32 FBaseNodeController::GetNumOutputs() const
		{
			if (const FMetasoundFrontendNode* Node = NodePtr.Get())
			{
				return Node->Interface.Outputs.Num();
			}

			return 0;
		}

		TArray<FConstInputHandle> FBaseNodeController::GetConstInputs() const
		{
			TArray<FConstInputHandle> Inputs;

			// If I had a nickle for every time C++ backed me into a corner, I would be sitting
			// on a tropical beach next to my mansion sipping strawberry daiquiris instead of 
			// trying to code using this guileful language. The const cast is generally safe here
			// because the FConstInputHandle only allows const access to the internal node controller. 
			// Ostensibly, there could have been a INodeController and IConstNodeController
			// which take different types in their constructor, but it starts to become
			// difficult to maintain. So instead of adding 500 lines of nearly duplicate 
			// code, a ConstCastSharedRef is used here. 
			FNodeHandle ThisNode = ConstCastSharedRef<INodeController>(this->AsShared());

			for (const FInputControllerParams& Params : GetInputControllerParams())
			{
				FConstInputHandle InputHandle = CreateInputController(Params.VertexID, Params.NodeVertexPtr, Params.ClassInputPtr, ThisNode);
				if (InputHandle->IsValid())
				{
					Inputs.Add(InputHandle);
				}
			}

			return Inputs;
		}

		void FBaseNodeController::IterateOutputs(TUniqueFunction<void(FOutputHandle)> InFunction)
		{
			for (const FOutputControllerParams& Params : GetOutputControllerParams())
			{
				FOutputHandle OutputHandle = CreateOutputController(Params.VertexID, Params.NodeVertexPtr, Params.ClassOutputPtr, AsShared());
				if (OutputHandle->IsValid())
				{
					InFunction(OutputHandle);
				}
			}
		}

		const FText& FBaseNodeController::GetDisplayTitle() const
		{
			if (const FMetasoundFrontendClass* Class = ClassPtr.Get())
			{
				return Class->Metadata.GetDisplayName();
			}

			return Invalid::GetInvalidText();
		}

		FText FBaseNodeController::GetDisplayName() const
		{
			if (const FMetasoundFrontendClass* Class = ClassPtr.Get())
			{
				return Class->Metadata.GetDisplayName();
			}

			return Invalid::GetInvalidText();
		}

		void FBaseNodeController::IterateConstInputs(TUniqueFunction<void(FConstInputHandle)> InFunction) const
		{
			// See early use of ConstCastSharedRef in this class for discussion.
			FNodeHandle ThisNode = ConstCastSharedRef<INodeController>(this->AsShared());

			for (const FInputControllerParams& Params : GetInputControllerParams())
			{
				FConstInputHandle InputHandle = CreateInputController(Params.VertexID, Params.NodeVertexPtr, Params.ClassInputPtr, ThisNode);
				if (InputHandle->IsValid())
				{
					InFunction(InputHandle);
				}
			}
		}

		TArray<FConstOutputHandle> FBaseNodeController::GetConstOutputs() const
		{
			TArray<FConstOutputHandle> Outputs;

			// See early use of ConstCastSharedRef in this class for discussion.
			FNodeHandle ThisNode = ConstCastSharedRef<INodeController>(this->AsShared());

			for (const FOutputControllerParams& Params : GetOutputControllerParams())
			{
				FConstOutputHandle OutputHandle = CreateOutputController(Params.VertexID, Params.NodeVertexPtr, Params.ClassOutputPtr, ThisNode);
				if (OutputHandle->IsValid())
				{
					Outputs.Add(OutputHandle);
				}
			}

			return Outputs;
		}

		void FBaseNodeController::IterateConstOutputs(TUniqueFunction<void(FConstOutputHandle)> InFunction) const
		{
			// See early use of ConstCastSharedRef in this class for discussion.
			FNodeHandle ThisNode = ConstCastSharedRef<INodeController>(this->AsShared());

			for (const FOutputControllerParams& Params : GetOutputControllerParams())
			{
				FConstOutputHandle OutputHandle = CreateOutputController(Params.VertexID, Params.NodeVertexPtr, Params.ClassOutputPtr, ThisNode);
				if (OutputHandle->IsValid())
				{
					InFunction(OutputHandle);
				}
			}
		}

		FInputHandle FBaseNodeController::GetInputWithVertexName(const FVertexName& InName)
		{
			FInputControllerParams Params;
			if (FindInputControllerParamsWithVertexName(InName, Params))
			{
				FNodeHandle ThisNode = this->AsShared();
				return CreateInputController(Params.VertexID, Params.NodeVertexPtr, Params.ClassInputPtr, ThisNode);
			}

			return IInputController::GetInvalidHandle();
		}

		FConstInputHandle FBaseNodeController::GetConstInputWithVertexName(const FVertexName& InName) const
		{
			FInputControllerParams Params;
			if (FindInputControllerParamsWithVertexName(InName, Params))
			{
				// See early use of ConstCastSharedRef in this class for discussion.
				FNodeHandle ThisNode = ConstCastSharedRef<INodeController>(this->AsShared());
				return CreateInputController(Params.VertexID, Params.NodeVertexPtr, Params.ClassInputPtr, ThisNode);
			}

			return IInputController::GetInvalidHandle();
		}

		FOutputHandle FBaseNodeController::GetOutputWithVertexName(const FVertexName& InName)
		{
			FOutputControllerParams Params;
			if (FindOutputControllerParamsWithVertexName(InName, Params))
			{
				FNodeHandle ThisNode = this->AsShared();
				return CreateOutputController(Params.VertexID, Params.NodeVertexPtr, Params.ClassOutputPtr, ThisNode);
			}

			return IOutputController::GetInvalidHandle();
		}

		FConstOutputHandle FBaseNodeController::GetConstOutputWithVertexName(const FVertexName& InName) const
		{
			FOutputControllerParams Params;
			if (FindOutputControllerParamsWithVertexName(InName, Params))
			{
				// See early use of ConstCastSharedRef in this class for discussion.
				FNodeHandle ThisNode = ConstCastSharedRef<INodeController>(this->AsShared());
				return CreateOutputController(Params.VertexID, Params.NodeVertexPtr, Params.ClassOutputPtr, ThisNode);
			}

			return IOutputController::GetInvalidHandle();
		}

		FInputHandle FBaseNodeController::GetInputWithID(FGuid InVertexID)
		{
			FInputControllerParams Params;

			if (FindInputControllerParamsWithID(InVertexID, Params))
			{
				FNodeHandle ThisNode = this->AsShared();
				return CreateInputController(Params.VertexID, Params.NodeVertexPtr, Params.ClassInputPtr, ThisNode);
			}

			return IInputController::GetInvalidHandle();
		}

		bool FBaseNodeController::IsInterfaceMember() const
		{
			return GetInterfaceVersion() != FMetasoundFrontendVersion::GetInvalid();
		}

		const FMetasoundFrontendVersion& FBaseNodeController::GetInterfaceVersion() const
		{
			return FMetasoundFrontendVersion::GetInvalid();
		}

		FConstInputHandle FBaseNodeController::GetInputWithID(FGuid InVertexID) const
		{
			FInputControllerParams Params;

			if (FindInputControllerParamsWithID(InVertexID, Params))
			{
				// See early use of ConstCastSharedRef in this class for discussion.
				FNodeHandle ThisNode = ConstCastSharedRef<INodeController>(this->AsShared());
				return CreateInputController(Params.VertexID, Params.NodeVertexPtr, Params.ClassInputPtr, ThisNode);
			}

			return IInputController::GetInvalidHandle();
		}

		FOutputHandle FBaseNodeController::GetOutputWithID(FGuid InVertexID)
		{
			FOutputControllerParams Params;

			if (FindOutputControllerParamsWithID(InVertexID, Params))
			{
				FNodeHandle ThisNode = this->AsShared();
				return CreateOutputController(Params.VertexID, Params.NodeVertexPtr, Params.ClassOutputPtr, ThisNode);
			}

			return IOutputController::GetInvalidHandle();
		}

		FConstOutputHandle FBaseNodeController::GetOutputWithID(FGuid InVertexID) const
		{
			FOutputControllerParams Params;

			if (FindOutputControllerParamsWithID(InVertexID, Params))
			{
				// See early use of ConstCastSharedRef in this class for discussion.
				FNodeHandle ThisNode = ConstCastSharedRef<INodeController>(this->AsShared());
				return CreateOutputController(Params.VertexID, Params.NodeVertexPtr, Params.ClassOutputPtr, ThisNode);
			}

			return IOutputController::GetInvalidHandle();
		}

		TArray<FBaseNodeController::FInputControllerParams> FBaseNodeController::GetInputControllerParams() const
		{
			TArray<FBaseNodeController::FInputControllerParams> Inputs;

			if (const FMetasoundFrontendNode* Node = NodePtr.Get())
			{
				for (const FMetasoundFrontendVertex& NodeInputVertex : Node->Interface.Inputs)
				{
					FConstVertexAccessPtr NodeVertexPtr = NodePtr.GetInputWithName(NodeInputVertex.Name);
					FConstClassInputAccessPtr ClassInputPtr = ClassPtr.GetInputWithName(NodeInputVertex.Name);

					Inputs.Add({NodeInputVertex.VertexID, NodeVertexPtr, ClassInputPtr});
				}
			}

			return Inputs;
		}

		TArray<FBaseNodeController::FOutputControllerParams> FBaseNodeController::GetOutputControllerParams() const
		{
			TArray<FBaseNodeController::FOutputControllerParams> Outputs;

			if (const FMetasoundFrontendNode* Node = NodePtr.Get())
			{
				for (const FMetasoundFrontendVertex& NodeOutputVertex : Node->Interface.Outputs)
				{
					const FVertexName& VertexName = NodeOutputVertex.Name;

					FConstVertexAccessPtr NodeVertexPtr = NodePtr.GetOutputWithName(VertexName);
					FConstClassOutputAccessPtr ClassOutputPtr = ClassPtr.GetOutputWithName(VertexName);

					Outputs.Add({NodeOutputVertex.VertexID, NodeVertexPtr, ClassOutputPtr});
				}
			}

			return Outputs;
		}

		bool FBaseNodeController::FindInputControllerParamsWithVertexName(const FVertexName& InName, FInputControllerParams& OutParams) const
		{
			FConstVertexAccessPtr NodeVertexPtr = NodePtr.GetInputWithName(InName);

			if (const FMetasoundFrontendVertex* Vertex = NodeVertexPtr.Get())
			{
				FConstClassInputAccessPtr ClassInputPtr = ClassPtr.GetInputWithName(InName);

				OutParams = FInputControllerParams{Vertex->VertexID, NodeVertexPtr, ClassInputPtr};
				return true;
			}

			return false;
		}

		bool FBaseNodeController::FindOutputControllerParamsWithVertexName(const FVertexName& InName, FOutputControllerParams& OutParams) const
		{
			FConstVertexAccessPtr NodeVertexPtr = NodePtr.GetOutputWithName(InName);

			if (const FMetasoundFrontendVertex* Vertex = NodeVertexPtr.Get())
			{
				FConstClassOutputAccessPtr ClassOutputPtr = ClassPtr.GetOutputWithName(InName);

				OutParams = FOutputControllerParams{Vertex->VertexID, NodeVertexPtr, ClassOutputPtr};
				return true;
			}

			return false;
		}

		bool FBaseNodeController::FindInputControllerParamsWithID(FGuid InVertexID, FInputControllerParams& OutParams) const
		{
			FConstVertexAccessPtr NodeVertexPtr = NodePtr.GetInputWithVertexID(InVertexID);

			if (const FMetasoundFrontendVertex* Vertex = NodeVertexPtr.Get())
			{
				FConstClassInputAccessPtr ClassInputPtr = ClassPtr.GetInputWithName(Vertex->Name);

				OutParams = FInputControllerParams{InVertexID, NodeVertexPtr, ClassInputPtr};
				return true;
			}

			return false;
		}

		bool FBaseNodeController::FindOutputControllerParamsWithID(FGuid InVertexID, FOutputControllerParams& OutParams) const
		{
			FConstVertexAccessPtr NodeVertexPtr = NodePtr.GetOutputWithVertexID(InVertexID);

			if (const FMetasoundFrontendVertex* Vertex = NodeVertexPtr.Get())
			{
				FConstClassOutputAccessPtr ClassOutputPtr = ClassPtr.GetOutputWithName(Vertex->Name);

				OutParams = FOutputControllerParams{InVertexID, NodeVertexPtr, ClassOutputPtr};
				return true;
			}

			return false;
		}

		FGraphHandle FBaseNodeController::AsGraph()
		{
			// TODO: consider adding support for external graph owned in another document.
			// Will require lookup support for external subgraphs..
			
			if (const FMetasoundFrontendClass* Class = ClassPtr.Get())
			{
				return GetOwningGraph()->GetOwningDocument()->GetSubgraphWithClassID(Class->ID);
			}

			return IGraphController::GetInvalidHandle();
		}

		FConstGraphHandle FBaseNodeController::AsGraph() const
		{
			// TODO: add support for graph owned in another asset.
			// Will require lookup support for external subgraphs.
			if (const FMetasoundFrontendClass* Class = ClassPtr.Get())
			{
				return GetOwningGraph()->GetOwningDocument()->GetSubgraphWithClassID(Class->ID);
			}

			return IGraphController::GetInvalidHandle();
		}

		FMetasoundFrontendVersionNumber FBaseNodeController::FindHighestMinorVersionInRegistry() const
		{
			const FMetasoundFrontendClassMetadata& Metadata = GetClassMetadata();
			const FMetasoundFrontendVersionNumber& CurrentVersion = Metadata.GetVersion();
			Metasound::FNodeClassName NodeClassName = Metadata.GetClassName().ToNodeClassName();

			FMetasoundFrontendClass ClassWithMajorVersion;
			if (ISearchEngine::Get().FindClassWithMajorVersion(NodeClassName, CurrentVersion.Major, ClassWithMajorVersion))
			{
				if (ClassWithMajorVersion.Metadata.GetVersion().Minor >= CurrentVersion.Minor)
				{
					return ClassWithMajorVersion.Metadata.GetVersion();
				}
			}

			return FMetasoundFrontendVersionNumber::GetInvalid();
		}

		FMetasoundFrontendVersionNumber FBaseNodeController::FindHighestVersionInRegistry() const
		{
			const FMetasoundFrontendClassMetadata& Metadata = GetClassMetadata();
			const FMetasoundFrontendVersionNumber& CurrentVersion = Metadata.GetVersion();
			Metasound::FNodeClassName NodeClassName = Metadata.GetClassName().ToNodeClassName();

			FMetasoundFrontendClass ClassWithHighestVersion;
			if (ISearchEngine::Get().FindClassWithHighestVersion(NodeClassName, ClassWithHighestVersion))
			{
				if (ClassWithHighestVersion.Metadata.GetVersion().Major >= CurrentVersion.Major)
				{
					return ClassWithHighestVersion.Metadata.GetVersion();
				}
			}

			return FMetasoundFrontendVersionNumber::GetInvalid();

		}

		FNodeHandle FBaseNodeController::ReplaceWithVersion(const FMetasoundFrontendVersionNumber& InNewVersion)
		{
			const FMetasoundFrontendClassMetadata& Metadata = GetClassMetadata();
			const TArray<FMetasoundFrontendClass> Versions = ISearchEngine::Get().FindClassesWithName(Metadata.GetClassName().ToNodeClassName(), false /* bInSortByVersion */);

			auto IsClassOfNewVersion = [InNewVersion](const FMetasoundFrontendClass& RegisteredClass)
			{
				return RegisteredClass.Metadata.GetVersion() == InNewVersion;
			};

			const FMetasoundFrontendClass* RegisteredClass = Versions.FindByPredicate(IsClassOfNewVersion);
			if (!ensure(RegisteredClass))
			{
				return this->AsShared();
			}

			FMetasoundFrontendNodeStyle Style = GetNodeStyle();

			using FConnectionKey = TPair<FVertexName, FName>;

			struct FInputConnectionInfo
			{
				FOutputHandle ConnectedOutput;
				FName DataType;
				FMetasoundFrontendLiteral DefaultValue;
				bool bLiteralSet = false;
			};

			// Cache input/output connections by name to try so they can be
			// hooked back up after swapping to the new class version.
			TMap<FConnectionKey, FInputConnectionInfo> InputConnections;
			IterateInputs([Connections = &InputConnections](FInputHandle InputHandle)
			{
				bool bLiteralSet = false;
				FMetasoundFrontendLiteral DefaultLiteral;
				if (const FMetasoundFrontendLiteral* Literal = InputHandle->GetLiteral())
				{
					// Array literals are not supported in UX, so don't pass along to referencing graph
					// TODO: Add UX in inspector to set literals (including arrays).
					if (!Literal->IsArray())
					{
						DefaultLiteral = *Literal;
						bLiteralSet = true;
					}
				}

				const FConnectionKey ConnectionKey(InputHandle->GetName(), InputHandle->GetDataType());
				Connections->Add(ConnectionKey, FInputConnectionInfo
				{
					InputHandle->GetConnectedOutput(),
					InputHandle->GetDataType(),
					MoveTemp(DefaultLiteral),
					bLiteralSet
				});
			});

			struct FOutputConnectionInfo
			{
				TArray<FInputHandle> ConnectedInputs;
				FName DataType;
			};

			TMap<FConnectionKey, FOutputConnectionInfo> OutputConnections;
			IterateOutputs([Connections = &OutputConnections](FOutputHandle OutputHandle)
			{
				const FConnectionKey ConnectionKey(OutputHandle->GetName(), OutputHandle->GetDataType());
				Connections->Add(ConnectionKey, FOutputConnectionInfo
				{
					OutputHandle->GetConnectedInputs(),
					OutputHandle->GetDataType(),
				});
			});

			const FGuid ReplacedNodeGuid = GetID();
			if (!ensureAlways(GetOwningGraph()->RemoveNode(*this)))
			{
				return this->AsShared();
			}

			// Make sure classes are up-to-date with registered versions of class.
			// Note that this may break other nodes in the graph that have stale
			// class API, but that's on the caller to fix-up or report invalid state.
			const FNodeRegistryKey RegistryKey = FMetasoundFrontendRegistryContainer::Get()->GetRegistryKey(RegisteredClass->Metadata);
			FDocumentHandle Document = GetOwningGraph()->GetOwningDocument();
			ensureAlways(Document->SynchronizeDependency(RegistryKey) != nullptr);

			FNodeHandle ReplacementNode = GetOwningGraph()->AddNode(RegisteredClass->Metadata, ReplacedNodeGuid);
			if (!ensureAlways(ReplacementNode->IsValid()))
			{
				return this->AsShared();
			}

			ReplacementNode->SetNodeStyle(Style);

			ReplacementNode->IterateInputs([Connections = &InputConnections](FInputHandle InputHandle)
			{
				const FConnectionKey ConnectionKey(InputHandle->GetName(), InputHandle->GetDataType());
				if (FInputConnectionInfo* ConnectionInfo = Connections->Find(ConnectionKey))
				{
					if (ConnectionInfo->bLiteralSet)
					{
						InputHandle->SetLiteral(ConnectionInfo->DefaultValue);
					}

					if (ConnectionInfo->ConnectedOutput->IsValid())
					{
						ensure(InputHandle->Connect(*ConnectionInfo->ConnectedOutput));
					}
				}
			});

			ReplacementNode->IterateOutputs([Connections = &OutputConnections](FOutputHandle OutputHandle)
			{
				const FConnectionKey ConnectionKey(OutputHandle->GetName(), OutputHandle->GetDataType());
				if (FOutputConnectionInfo* ConnectionInfo = Connections->Find(ConnectionKey))
				{
					for (FInputHandle InputHandle : ConnectionInfo->ConnectedInputs)
					{
						if (InputHandle->IsValid())
						{
							ensure(InputHandle->Connect(*OutputHandle));
						}
					}
				}
			});

			return ReplacementNode;
		}

		bool FBaseNodeController::DiffAgainstRegistryInterface(FClassInterfaceUpdates& OutInterfaceUpdates, bool bInUseHighestMinorVersion) const
		{
			METASOUND_TRACE_CPUPROFILER_EVENT_SCOPE(BaseNodeController::DiffAgainstRegistryInterface);

			OutInterfaceUpdates = FClassInterfaceUpdates();

			const FMetasoundFrontendClassMetadata& NodeClassMetadata = GetClassMetadata();
			const FMetasoundFrontendClassInterface& NodeClassInterface = GetClassInterface();

			Metasound::FNodeClassName NodeClassName = NodeClassMetadata.GetClassName().ToNodeClassName();

			if (bInUseHighestMinorVersion)
			{
				if (!ISearchEngine::Get().FindClassWithMajorVersion(NodeClassName, NodeClassMetadata.GetVersion().Major, OutInterfaceUpdates.RegistryClass))
				{
					Algo::Transform(NodeClassInterface.Inputs, OutInterfaceUpdates.RemovedInputs, [&](const FMetasoundFrontendClassInput& Input) { return &Input; });
					Algo::Transform(NodeClassInterface.Outputs, OutInterfaceUpdates.RemovedOutputs, [&](const FMetasoundFrontendClassOutput& Output) { return &Output; });
					return false;
				}
			}
			else
			{
				constexpr bool bSortByVersion = true;
				const TArray<FMetasoundFrontendClass> Classes = ISearchEngine::Get().FindClassesWithName(NodeClassName, bSortByVersion);
				const FMetasoundFrontendClass* ExactClass = Classes.FindByPredicate([CurrentVersion = &NodeClassMetadata.GetVersion()](const FMetasoundFrontendClass& AvailableClass)
				{
					return AvailableClass.Metadata.GetVersion() == *CurrentVersion;
				});

				if (!ExactClass)
				{
					Algo::Transform(NodeClassInterface.Inputs, OutInterfaceUpdates.RemovedInputs, [&](const FMetasoundFrontendClassInput& Input) { return &Input; });
					Algo::Transform(NodeClassInterface.Outputs, OutInterfaceUpdates.RemovedOutputs, [&](const FMetasoundFrontendClassOutput& Output) { return &Output; });
					return false;
				}
				OutInterfaceUpdates.RegistryClass = *ExactClass;
			}

			Algo::Transform(OutInterfaceUpdates.RegistryClass.Interface.Inputs, OutInterfaceUpdates.AddedInputs, [&](const FMetasoundFrontendClassInput& Input) { return &Input; });
			for (const FMetasoundFrontendClassInput& Input : NodeClassInterface.Inputs)
			{
				auto IsFunctionalEquivalent = [NodeClassInput = &Input](const FMetasoundFrontendClassInput* Iter)
				{
					return FMetasoundFrontendClassVertex::IsFunctionalEquivalent(*NodeClassInput, *Iter);
				};

				const int32 Index = OutInterfaceUpdates.AddedInputs.FindLastByPredicate(IsFunctionalEquivalent);
				if (Index == INDEX_NONE)
				{
					OutInterfaceUpdates.RemovedInputs.Add(&Input);
				}
				else
				{
					OutInterfaceUpdates.AddedInputs.RemoveAtSwap(Index, 1, false /* bAllowShrinking */);
				}
			}

			Algo::Transform(OutInterfaceUpdates.RegistryClass.Interface.Outputs, OutInterfaceUpdates.AddedOutputs, [&](const FMetasoundFrontendClassOutput& Output) { return &Output; });
			for (const FMetasoundFrontendClassOutput& Output : NodeClassInterface.Outputs)
			{
				auto IsFunctionalEquivalent = [NodeClassOutput = &Output](const FMetasoundFrontendClassOutput* Iter)
				{
					return FMetasoundFrontendClassVertex::IsFunctionalEquivalent(*NodeClassOutput, *Iter);
				};

				const int32 Index = OutInterfaceUpdates.AddedOutputs.FindLastByPredicate(IsFunctionalEquivalent);
				if (Index == INDEX_NONE)
				{
					OutInterfaceUpdates.RemovedOutputs.Add(&Output);
				}
				else
				{
					OutInterfaceUpdates.AddedOutputs.RemoveAtSwap(Index, 1, false /* bAllowShrinking */);
				}
			}

			return true;
		}

		bool FBaseNodeController::CanAutoUpdate(FClassInterfaceUpdates* OutInterfaceUpdates) const
		{
			METASOUND_TRACE_CPUPROFILER_EVENT_SCOPE(BaseNodeController::CanAutoUpdate);

			const FMetasoundFrontendClassMetadata& NodeClassMetadata = GetClassMetadata();
			if (IMetaSoundAssetManager* AssetManager = IMetaSoundAssetManager::Get())
			{
				if (!AssetManager->CanAutoUpdate(NodeClassMetadata.GetClassName()))
				{
					return false;
				}
			}

			FMetasoundFrontendClass RegistryClass;
			if (!ISearchEngine::Get().FindClassWithMajorVersion(
				NodeClassMetadata.GetClassName().ToNodeClassName(),
				NodeClassMetadata.GetVersion().Major,
				RegistryClass))
			{
				return false;
			}

			if (RegistryClass.Metadata.GetVersion() < NodeClassMetadata.GetVersion())
			{
				return false;
			}

			if (RegistryClass.Metadata.GetVersion() == NodeClassMetadata.GetVersion())
			{
				// TODO: Merge these paths.  Shouldn't use different logic to
				// define changes in native vs asset class definitions.
				const FNodeRegistryKey RegistryKey = NodeRegistryKey::CreateKey(RegistryClass.Metadata);
				const bool bIsClassNative = FMetasoundFrontendRegistryContainer::Get()->IsNodeNative(RegistryKey);
				if (bIsClassNative)
				{
					if (!MetaSoundAutoUpdateNativeClassCVar)
					{
						return false;
					}

					FClassInterfaceUpdates InterfaceUpdates;
					DiffAgainstRegistryInterface(InterfaceUpdates, true /* bUseHighestMinorVersion */);
					if (OutInterfaceUpdates)
					{
						*OutInterfaceUpdates = InterfaceUpdates;
					}

					if (!InterfaceUpdates.ContainsChanges())
					{
						return false;
					}
				}
				else
				{
					if (RegistryClass.Metadata.GetChangeID() == NodeClassMetadata.GetChangeID())
					{
						const FGuid& NodeClassInterfaceChangeID = GetClassInterface().GetChangeID();
						if (RegistryClass.Interface.GetChangeID() == NodeClassInterfaceChangeID)
						{
							return false;
						}
					}
				}
			}

			return true;
		}

		FDocumentAccess FBaseNodeController::ShareAccess()
		{
			FDocumentAccess Access;

			Access.Node = NodePtr;
			Access.ConstNode = NodePtr;
			Access.ConstClass = ClassPtr;

			return Access;
		}

		FConstDocumentAccess FBaseNodeController::ShareAccess() const
		{
			FConstDocumentAccess Access;

			Access.ConstNode = NodePtr;
			Access.ConstClass = ClassPtr;

			return Access;
		}


		// 
		// FNodeController
		//
		FNodeController::FNodeController(EPrivateToken InToken, const FNodeController::FInitParams& InParams)
		: FBaseNodeController({InParams.NodePtr, InParams.ClassPtr, InParams.OwningGraph})
		, GraphPtr(InParams.GraphPtr)
		{
		}

		FNodeHandle FNodeController::CreateNodeHandle(const FNodeController::FInitParams& InParams)
		{
			if (const FMetasoundFrontendNode* Node = InParams.NodePtr.Get())
			{
				if (const FMetasoundFrontendClass* Class = InParams.ClassPtr.Get())
				{
					// Cannot make a valid node handle if the node description and class description differ
					if (Node->ClassID == Class->ID)
					{
						return MakeShared<FNodeController>(EPrivateToken::Token, InParams);
					}
					else
					{
						UE_LOG(LogMetaSound, Warning, TEXT("Frontend Node [NodeID:%s, ClassID:%s] is not of expected class class [ClassID:%s]"), *Node->GetID().ToString(), *Node->ClassID.ToString(), *Class->ID.ToString());
					}
				}
			}
			return INodeController::GetInvalidHandle();
		}

		FConstNodeHandle FNodeController::CreateConstNodeHandle(const FNodeController::FInitParams& InParams)
		{
			if (const FMetasoundFrontendNode* Node = InParams.NodePtr.Get())
			{
				if (const FMetasoundFrontendClass* Class = InParams.ClassPtr.Get())
				{
					// Cannot make a valid node handle if the node description and class description differ
					if (Node->ClassID == Class->ID)
					{
						return MakeShared<const FNodeController>(EPrivateToken::Token, InParams);
					}
					else
					{
						UE_LOG(LogMetaSound, Warning, TEXT("Frontend Node [NodeID:%s, ClassID:%s] is not of expected class class [ClassID:%s]"), *Node->GetID().ToString(), *Node->ClassID.ToString(), *Class->ID.ToString());
					}
				}
			}
			return INodeController::GetInvalidHandle();
		}

		bool FNodeController::IsValid() const
		{
			return FBaseNodeController::IsValid() && (nullptr != GraphPtr.Get());
		}

		FInputHandle FNodeController::CreateInputController(FGuid InVertexID, FConstVertexAccessPtr InNodeVertexPtr, FConstClassInputAccessPtr InClassInputPtr, FNodeHandle InOwningNode) const
		{
			return MakeShared<FBaseInputController>(FBaseInputController::FInitParams{InVertexID, InNodeVertexPtr, InClassInputPtr, GraphPtr, InOwningNode});
		}

		FOutputHandle FNodeController::CreateOutputController(FGuid InVertexID, FConstVertexAccessPtr InNodeVertexPtr, FConstClassOutputAccessPtr InClassOutputPtr, FNodeHandle InOwningNode) const
		{
			return MakeShared<FBaseOutputController>(FBaseOutputController::FInitParams{InVertexID, InNodeVertexPtr, InClassOutputPtr, GraphPtr, InOwningNode});
		}

		FDocumentAccess FNodeController::ShareAccess() 
		{
			FDocumentAccess Access = FBaseNodeController::ShareAccess();

			Access.Graph = GraphPtr;
			Access.ConstGraph = GraphPtr;

			return Access;
		}

		FConstDocumentAccess FNodeController::ShareAccess() const
		{
			FConstDocumentAccess Access = FBaseNodeController::ShareAccess();

			Access.ConstGraph = GraphPtr;

			return Access;
		}


		//
		// FOutputNodeController
		//
		FOutputNodeController::FOutputNodeController(FOutputNodeController::EPrivateToken InToken, const FOutputNodeController::FInitParams& InParams)
		: FBaseNodeController({InParams.NodePtr, InParams.ClassPtr, InParams.OwningGraph})
		, GraphPtr(InParams.GraphPtr)
		, OwningGraphClassOutputPtr(InParams.OwningGraphClassOutputPtr)
		{
		}

		FNodeHandle FOutputNodeController::CreateOutputNodeHandle(const FOutputNodeController::FInitParams& InParams)
		{
			if (const FMetasoundFrontendNode* Node = InParams.NodePtr.Get())
			{
				if (const FMetasoundFrontendClass* Class = InParams.ClassPtr.Get())
				{
					if (EMetasoundFrontendClassType::Output == Class->Metadata.GetType())
					{
						if (Class->ID == Node->ClassID)
						{
							return MakeShared<FOutputNodeController>(EPrivateToken::Token, InParams);
						}
						else
						{
							UE_LOG(LogMetaSound, Warning, TEXT("Frontend Node [NodeID:%s, ClassID:%s] is not of expected class class [ClassID:%s]"), *Node->GetID().ToString(), *Node->ClassID.ToString(), *Class->ID.ToString());
						}
					}
					else
					{
						UE_LOG(LogMetaSound, Warning, TEXT("Frontend Class of incorrect class type [ClassID:%s] while creating output node.. Must be EMetasoundFrontendClassType::Output."), *Class->ID.ToString());
					}
				}
			}

			return INodeController::GetInvalidHandle();
		}

		const FText& FOutputNodeController::GetDescription() const
		{
			if (const FMetasoundFrontendClassOutput* OwningOutput = OwningGraphClassOutputPtr.Get())
			{
				return OwningOutput->Metadata.Description;
			}

			return Invalid::GetInvalidText();
		}

		FText FOutputNodeController::GetDisplayName() const
		{
			if (const FMetasoundFrontendClassOutput* OwningOutput = OwningGraphClassOutputPtr.Get())
			{
				return OwningOutput->Metadata.DisplayName;
			}

			return Invalid::GetInvalidText();
		}

		void FOutputNodeController::SetDescription(const FText& InDescription)
		{
			// TODO: can we remove the const cast by constructing output nodes with a non-const access to class outputs?
			if (FMetasoundFrontendClassOutput* ClassOutput = ConstCastAccessPtr<FClassOutputAccessPtr>(OwningGraphClassOutputPtr).Get())
			{
				ClassOutput->Metadata.Description = InDescription;
				OwningGraph->UpdateInterfaceChangeID();
			}
		}

		void FOutputNodeController::SetNodeName(const FVertexName& InName)
		{
			if (FMetasoundFrontendNode* Node = NodePtr.Get())
			{
				Node->Name = InName;

				for (FMetasoundFrontendVertex& Vertex : Node->Interface.Inputs)
				{
					Vertex.Name = InName;
				}

				for (FMetasoundFrontendVertex& Vertex : Node->Interface.Outputs)
				{
					Vertex.Name = InName;
				}
			}

			// TODO: can we remove the const cast by constructing output nodes with a non-const access to class outputs?
			if (FMetasoundFrontendClassOutput* ClassOutput = ConstCastAccessPtr<FClassOutputAccessPtr>(OwningGraphClassOutputPtr).Get())
			{
				ClassOutput->Name = InName;
				OwningGraph->UpdateInterfaceChangeID();
			}
		}

		void FOutputNodeController::SetDisplayName(const FText& InDisplayName)
		{
			// TODO: can we remove the const cast by constructing output nodes with a non-const access to class outputs?
			if (FMetasoundFrontendClassOutput* ClassOutput = ConstCastAccessPtr<FClassOutputAccessPtr>(OwningGraphClassOutputPtr).Get())
			{
				ClassOutput->Metadata.DisplayName = InDisplayName;
				OwningGraph->UpdateInterfaceChangeID();
			}
		}

		FConstNodeHandle FOutputNodeController::CreateConstOutputNodeHandle(const FOutputNodeController::FInitParams& InParams)
		{
			if (const FMetasoundFrontendNode* Node = InParams.NodePtr.Get())
			{
				if (const FMetasoundFrontendClass* Class = InParams.ClassPtr.Get())
				{
					if (EMetasoundFrontendClassType::Output == Class->Metadata.GetType())
					{
						if (Class->ID == Node->ClassID)
						{
							return MakeShared<const FOutputNodeController>(EPrivateToken::Token, InParams);
						}
						else
						{
							UE_LOG(LogMetaSound, Warning, TEXT("Frontend Node [NodeID:%s, ClassID:%s] is not of expected class class [ClassID:%s]"), *Node->GetID().ToString(), *Node->ClassID.ToString(), *Class->ID.ToString());
						}
					}
					else
					{
						UE_LOG(LogMetaSound, Warning, TEXT("Frontend Class of incorrect class type [ClassID:%s] while creating output node.. Must be EMetasoundFrontendClassType::Output."), *Class->ID.ToString());
					}
				}
			}

			return INodeController::GetInvalidHandle();
		}

		const FText& FOutputNodeController::GetDisplayTitle() const
		{
			static FText OutputDisplayTitle = LOCTEXT("OutputNode_Title", "Output");
			return OutputDisplayTitle;
		}

		const FMetasoundFrontendVersion& FOutputNodeController::GetInterfaceVersion() const
		{
			FConstDocumentHandle OwningDocument = OwningGraph->GetOwningDocument();
			FConstGraphHandle RootGraph = OwningDocument->GetRootGraph();

			// Test if this node exists on the document's root graph.
			const bool bIsNodeOnRootGraph = OwningGraph->IsValid() && (RootGraph->GetClassID() == OwningGraph->GetClassID());

			if (bIsNodeOnRootGraph)
			{
				// If the node is on the root graph, test if it is in the interfaces' required inputs or outputs.
				FMetasoundFrontendInterface Interface;

				for (const FMetasoundFrontendVersion& InterfaceVersion : OwningDocument->GetInterfaceVersions())
				{
					FInterfaceRegistryKey InterfaceKey = GetInterfaceRegistryKey(InterfaceVersion);
					bool bFoundInterface = IInterfaceRegistry::Get().FindInterface(InterfaceKey, Interface);
					if (bFoundInterface)
					{
						if (const FMetasoundFrontendNode* Node = NodePtr.Get())
						{
							if (ensure(Node->Interface.Outputs.Num() == 1))
							{
								const FVertexName& Name = Node->Name;
								const FName& DataType = Node->Interface.Outputs[0].TypeName;
								auto IsOutputWithSameNameAndType = [&Name, &DataType](const FMetasoundFrontendClassOutput& InOutput)
								{
									return InOutput.Name == Name && InOutput.TypeName == DataType;
								};

								if (Interface.Outputs.ContainsByPredicate(IsOutputWithSameNameAndType))
								{
									return InterfaceVersion;
								}
							}
						}
					}
					else
					{
						UE_LOG(LogMetaSound, Warning, TEXT("Document using unregistered interface [InterfaceVersion:%s]"), *InterfaceVersion.ToString());
					}
				}
			}

			return FMetasoundFrontendVersion::GetInvalid();
		}

		bool FOutputNodeController::IsValid() const
		{
			return OwningGraph->IsValid() && (nullptr != OwningGraphClassOutputPtr.Get()) && (nullptr != GraphPtr.Get());
		}

		FInputHandle FOutputNodeController::CreateInputController(FGuid InVertexID, FConstVertexAccessPtr InNodeVertexPtr, FConstClassInputAccessPtr InClassInputPtr, FNodeHandle InOwningNode) const
		{
			return MakeShared<FOutputNodeInputController>(FOutputNodeInputController::FInitParams{InVertexID, InNodeVertexPtr, InClassInputPtr, OwningGraphClassOutputPtr, GraphPtr, InOwningNode});
		}

		FOutputHandle FOutputNodeController::CreateOutputController(FGuid InVertexID, FConstVertexAccessPtr InNodeVertexPtr, FConstClassOutputAccessPtr InClassOutputPtr, FNodeHandle InOwningNode) const
		{
			return MakeShared<FOutputNodeOutputController>(FOutputNodeOutputController::FInitParams{InVertexID, InNodeVertexPtr, InClassOutputPtr, OwningGraphClassOutputPtr, GraphPtr, InOwningNode});
		}

		FDocumentAccess FOutputNodeController::ShareAccess()
		{
			FDocumentAccess Access = FBaseNodeController::ShareAccess();

			Access.Graph = GraphPtr;
			Access.ConstGraph = GraphPtr;
			Access.ConstClassOutput = OwningGraphClassOutputPtr;

			return Access;
		}

		FConstDocumentAccess FOutputNodeController::ShareAccess() const
		{
			FConstDocumentAccess Access = FBaseNodeController::ShareAccess();

			Access.ConstGraph = GraphPtr;
			Access.ConstClassOutput = OwningGraphClassOutputPtr;

			return Access;
		}


		//
		// FInputNodeController
		//
		FInputNodeController::FInputNodeController(EPrivateToken InToken, const FInputNodeController::FInitParams& InParams)
		: FBaseNodeController({InParams.NodePtr, InParams.ClassPtr, InParams.OwningGraph})
		, OwningGraphClassInputPtr(InParams.OwningGraphClassInputPtr)
		, GraphPtr(InParams.GraphPtr)
		{
		}

		FNodeHandle FInputNodeController::CreateInputNodeHandle(const FInputNodeController::FInitParams& InParams)
		{
			if (const FMetasoundFrontendNode* Node = InParams.NodePtr.Get())
			{
				if (const FMetasoundFrontendClass* Class = InParams.ClassPtr.Get())
				{
					if (EMetasoundFrontendClassType::Input == Class->Metadata.GetType())
					{
						if (Class->ID == Node->ClassID)
						{
							return MakeShared<FInputNodeController>(EPrivateToken::Token, InParams);
						}
						else
						{
							UE_LOG(LogMetaSound, Warning, TEXT("Frontend Node [NodeID:%s, ClassID:%s] is not of expected class class [ClassID:%s]"), *Node->GetID().ToString(), *Node->ClassID.ToString(), *Class->ID.ToString());
						}
					}
					else
					{
						UE_LOG(LogMetaSound, Warning, TEXT("Frontend Class of incorrect class type [ClassID:%s] while creating input node. Must be EMetasoundFrontendClassType::Input."), *Class->ID.ToString());
					}
				}
			}

			return INodeController::GetInvalidHandle();
		}

		FConstNodeHandle FInputNodeController::CreateConstInputNodeHandle(const FInputNodeController::FInitParams& InParams)
		{
			if (const FMetasoundFrontendNode* Node = InParams.NodePtr.Get())
			{
				if (const FMetasoundFrontendClass* Class = InParams.ClassPtr.Get())
				{
					if (EMetasoundFrontendClassType::Input == Class->Metadata.GetType())
					{
						if (Class->ID == Node->ClassID)
						{
							return MakeShared<const FInputNodeController>(EPrivateToken::Token, InParams);
						}
						else
						{
							UE_LOG(LogMetaSound, Warning, TEXT("Frontend Node [NodeID:%s, ClassID:%s] is not of expected class class [ClassID:%s]"), *Node->GetID().ToString(), *Node->ClassID.ToString(), *Class->ID.ToString());
						}
					}
					else
					{
						UE_LOG(LogMetaSound, Warning, TEXT("Frontend Class of incorrect class type [ClassID:%s] while creating input node. Must be EMetasoundFrontendClassType::Input."), *Class->ID.ToString());
					}
				}
			}

			return INodeController::GetInvalidHandle();
		}

		bool FInputNodeController::IsValid() const
		{
			return OwningGraph->IsValid() && (nullptr != OwningGraphClassInputPtr.Get()) && (nullptr != GraphPtr.Get());
		}

		FInputHandle FInputNodeController::CreateInputController(FGuid InVertexID, FConstVertexAccessPtr InNodeVertexPtr, FConstClassInputAccessPtr InClassInputPtr, FNodeHandle InOwningNode) const
		{
			return MakeShared<FInputNodeInputController>(FInputNodeInputController::FInitParams{InVertexID, InNodeVertexPtr, InClassInputPtr, OwningGraphClassInputPtr, GraphPtr, InOwningNode});
		}

		FOutputHandle FInputNodeController::CreateOutputController(FGuid InVertexID, FConstVertexAccessPtr InNodeVertexPtr, FConstClassOutputAccessPtr InClassOutputPtr, FNodeHandle InOwningNode) const
		{
			return MakeShared<FInputNodeOutputController>(FInputNodeOutputController::FInitParams{InVertexID, InNodeVertexPtr, InClassOutputPtr, OwningGraphClassInputPtr, GraphPtr, InOwningNode});
		}

		const FText& FInputNodeController::GetDescription() const
		{
			if (const FMetasoundFrontendClassInput* OwningInput = OwningGraphClassInputPtr.Get())
			{
				return OwningInput->Metadata.Description;
			}

			return Invalid::GetInvalidText();
		}

		FText FInputNodeController::GetDisplayName() const
		{
			if (const FMetasoundFrontendClassInput* OwningInput = OwningGraphClassInputPtr.Get())
			{
				return OwningInput->Metadata.DisplayName;
			}

			return Invalid::GetInvalidText();
		}

		const FText& FInputNodeController::GetDisplayTitle() const
		{
			static FText InputDisplayTitle = LOCTEXT("InputNode_Title", "Input");
			return InputDisplayTitle;
		}

		const FMetasoundFrontendVersion& FInputNodeController::GetInterfaceVersion() const
		{
			FConstDocumentHandle OwningDocument = OwningGraph->GetOwningDocument();
			FConstGraphHandle RootGraph = OwningDocument->GetRootGraph();

			// Test if this node exists on the document's root graph.
			const bool bIsNodeOnRootGraph = OwningGraph->IsValid() && (RootGraph->GetClassID() == OwningGraph->GetClassID());
			if (bIsNodeOnRootGraph)
			{
				for (const FMetasoundFrontendVersion& InterfaceVersion : OwningDocument->GetInterfaceVersions())
				{
					// If the node is on the root graph, test if it is in the interfaces required inputs.
					FMetasoundFrontendInterface Interface;
					FInterfaceRegistryKey InterfaceKey = GetInterfaceRegistryKey(InterfaceVersion);
					bool bFoundInterface = IInterfaceRegistry::Get().FindInterface(InterfaceKey, Interface);
					if (bFoundInterface)
					{
						if (const FMetasoundFrontendNode* Node = NodePtr.Get())
						{
							const FVertexName& Name = Node->Name;
							if (ensure(Node->Interface.Inputs.Num() == 1))
							{
								const FName& DataType = Node->Interface.Inputs[0].TypeName;
								auto IsInputWithSameNameAndType = [&Name, &DataType](const FMetasoundFrontendClassInput& InInput)
								{
									return InInput.Name == Name && InInput.TypeName == DataType;
								};

								if (Interface.Inputs.ContainsByPredicate(IsInputWithSameNameAndType))
								{
									return InterfaceVersion;
								}
							}
						}
					}
					else
					{
						UE_LOG(LogMetaSound, Warning, TEXT("Document using unregistered interface [InterfaceVersion:%s]"), *InterfaceVersion.ToString());
					}
				}
			}

			return FMetasoundFrontendVersion::GetInvalid();
		}

		void FInputNodeController::SetDescription(const FText& InDescription)
		{
			// TODO: can we remove these const casts by constructing FINputNodeController with non-const access to the class input?
			if (FMetasoundFrontendClassInput* ClassInput = ConstCastAccessPtr<FClassInputAccessPtr>(OwningGraphClassInputPtr).Get())
			{
				ClassInput->Metadata.Description = InDescription;
				OwningGraph->UpdateInterfaceChangeID();
			}
		}

		void FInputNodeController::SetNodeName(const FVertexName& InName)
		{
			if (FMetasoundFrontendNode* Node = NodePtr.Get())
			{
				Node->Name = InName;

				for (FMetasoundFrontendVertex& Vertex : Node->Interface.Inputs)
				{
					Vertex.Name = InName;
				}

				for (FMetasoundFrontendVertex& Vertex : Node->Interface.Outputs)
				{
					Vertex.Name = InName;
				}
			}

			if (FMetasoundFrontendClassInput* ClassInput = ConstCastAccessPtr<FClassInputAccessPtr>(OwningGraphClassInputPtr).Get())
			{
				ClassInput->Name = InName;
			}
		}

		void FInputNodeController::SetDisplayName(const FText& InDisplayName)
		{
			// TODO: can we remove these const casts by constructing FINputNodeController with non-const access to the class input?
			if (FMetasoundFrontendClassInput* ClassInput = ConstCastAccessPtr<FClassInputAccessPtr>(OwningGraphClassInputPtr).Get())
			{
				ClassInput->Metadata.DisplayName = InDisplayName;
				OwningGraph->UpdateInterfaceChangeID();
			}
		}

		FDocumentAccess FInputNodeController::ShareAccess()
		{
			FDocumentAccess Access = FBaseNodeController::ShareAccess();

			Access.Graph = GraphPtr;
			Access.ConstGraph = GraphPtr;
			Access.ConstClassInput = OwningGraphClassInputPtr;

			return Access;
		}

		FConstDocumentAccess FInputNodeController::ShareAccess() const
		{
			FConstDocumentAccess Access = FBaseNodeController::ShareAccess();

			Access.ConstGraph = GraphPtr;
			Access.ConstClassInput = OwningGraphClassInputPtr;

			return Access;
		}

		// 
		// FVariableNodeController
		//
		FVariableNodeController::FVariableNodeController(EPrivateToken InToken, const FVariableNodeController::FInitParams& InParams)
		: FNodeController(FNodeController::EPrivateToken::Token, InParams)
		{
		}

		FNodeHandle FVariableNodeController::CreateNodeHandle(const FVariableNodeController::FInitParams& InParams)
		{
			if (const FMetasoundFrontendNode* Node = InParams.NodePtr.Get())
			{
				if (const FMetasoundFrontendClass* Class = InParams.ClassPtr.Get())
				{
					// Cannot make a valid node handle if the node description and class description differ
					if (Node->ClassID == Class->ID)
					{
						EMetasoundFrontendClassType ClassType = Class->Metadata.GetType();

						// Class type must be one of the associated variable class types.
						if (ensure(IsSupportedClassType(ClassType)))
						{
							return MakeShared<FVariableNodeController>(EPrivateToken::Token, InParams);
						}
					}
					else
					{
						UE_LOG(LogMetaSound, Warning, TEXT("Frontend Node [NodeID:%s, ClassID:%s] is not of expected class class [ClassID:%s]"), *Node->GetID().ToString(), *Node->ClassID.ToString(), *Class->ID.ToString());
					}
				}
			}
			return INodeController::GetInvalidHandle();
		}

		FConstNodeHandle FVariableNodeController::CreateConstNodeHandle(const FVariableNodeController::FInitParams& InParams)
		{
			if (const FMetasoundFrontendNode* Node = InParams.NodePtr.Get())
			{
				if (const FMetasoundFrontendClass* Class = InParams.ClassPtr.Get())
				{
					// Cannot make a valid node handle if the node description and class description differ
					if (Node->ClassID == Class->ID)
					{
						EMetasoundFrontendClassType ClassType = Class->Metadata.GetType();

						// Class type must be one of the associated variable class types.
						if (ensure(IsSupportedClassType(ClassType)))
						{
							return MakeShared<const FVariableNodeController>(EPrivateToken::Token, InParams);
						}
					}
					else
					{
						UE_LOG(LogMetaSound, Warning, TEXT("Frontend Node [NodeID:%s, ClassID:%s] is not of expected class class [ClassID:%s]"), *Node->GetID().ToString(), *Node->ClassID.ToString(), *Class->ID.ToString());
					}
				}
			}
			return INodeController::GetInvalidHandle();
		}

		FInputHandle FVariableNodeController::CreateInputController(FGuid InVertexID, FConstVertexAccessPtr InNodeVertexPtr, FConstClassInputAccessPtr InClassInputPtr, FNodeHandle InOwningNode) const
		{
			if (const FMetasoundFrontendClassInput* ClassInput = InClassInputPtr.Get())
			{
				if (IsVariableDataType(ClassInput->TypeName))
				{
					FGraphAccessPtr SuperGraphPtr = ConstCastAccessPtr<FGraphAccessPtr>(Super::ShareAccess().ConstGraph);

					return MakeShared<FVariableInputController>(FVariableInputController::FInitParams{InVertexID, InNodeVertexPtr, InClassInputPtr, SuperGraphPtr, InOwningNode});
				}
			}
			return Super::CreateInputController(InVertexID, InNodeVertexPtr, InClassInputPtr, InOwningNode);
		}

		FOutputHandle FVariableNodeController::CreateOutputController(FGuid InVertexID, FConstVertexAccessPtr InNodeVertexPtr, FConstClassOutputAccessPtr InClassOutputPtr, FNodeHandle InOwningNode) const
		{
			if (const FMetasoundFrontendClassOutput* ClassOutput = InClassOutputPtr.Get())
			{
				if (IsVariableDataType(ClassOutput->TypeName))
				{
					FGraphAccessPtr SuperGraphPtr = ConstCastAccessPtr<FGraphAccessPtr>(Super::ShareAccess().ConstGraph);
					return MakeShared<FVariableOutputController>(FVariableOutputController::FInitParams{InVertexID, InNodeVertexPtr, InClassOutputPtr, SuperGraphPtr, InOwningNode});
				}
			}
			return Super::CreateOutputController(InVertexID, InNodeVertexPtr, InClassOutputPtr, InOwningNode);
		}

		bool FVariableNodeController::IsSupportedClassType(EMetasoundFrontendClassType InClassType)
		{
			const bool bIsVariableNode = (InClassType == EMetasoundFrontendClassType::Variable)
				|| (InClassType == EMetasoundFrontendClassType::VariableAccessor)
				|| (InClassType == EMetasoundFrontendClassType::VariableDeferredAccessor)
				|| (InClassType == EMetasoundFrontendClassType::VariableMutator);
			return bIsVariableNode;
		}

		bool FVariableNodeController::IsVariableDataType(const FName& InTypeName)
		{
			FDataTypeRegistryInfo DataTypeInfo;
			if (ensure(IDataTypeRegistry::Get().GetDataTypeInfo(InTypeName, DataTypeInfo)))
			{
				return DataTypeInfo.bIsVariable;
			}
			return false;
		}
	}
}
#undef LOCTEXT_NAMESPACE
