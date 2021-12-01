// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "MetasoundBuilderInterface.h"
#include "MetasoundNode.h"
#include "MetasoundNodeInterface.h"
#include "MetasoundOperatorInterface.h"
#include "MetasoundDataReference.h"
#include "MetasoundExecutableOperator.h"
#include "MetasoundRouter.h"
#include "MetasoundVertex.h"

#include <type_traits>

#define LOCTEXT_NAMESPACE "MetasoundFrontend"


namespace Metasound
{
	template<typename TDataType>
	class TSendNode : public FNode
	{
	public:
		static const FVertexName& GetAddressInputName()
		{
			static const FVertexName InputName = TEXT("Address");
			return InputName;
		}

		static const FVertexName& GetSendInputName()
		{
			static const FVertexName& SendInput = GetMetasoundDataTypeName<TDataType>();
			return SendInput;
		}

		static FVertexInterface DeclareVertexInterface()
		{
			return FVertexInterface(
				FInputVertexInterface(
					TInputDataVertexModel<FSendAddress>(GetAddressInputName(), FText::GetEmpty()),
					TInputDataVertexModel<TDataType>(GetSendInputName(), FText::GetEmpty())
				),
				FOutputVertexInterface(
				)
			);
		}

		static const FNodeClassMetadata& GetNodeInfo()
		{
			auto InitNodeInfo = []() -> FNodeClassMetadata
			{
				const FVertexName& InputName = GetSendInputName();
				FNodeClassMetadata Info;

				Info.ClassName = { "Send", GetMetasoundDataTypeName<TDataType>(), FName() };
				Info.MajorVersion = 1;
				Info.MinorVersion = 0;
				Info.DisplayName = FText::Format(LOCTEXT("Metasound_SendNodeDisplayNameFormat", "Send {0}"), GetMetasoundDataTypeDisplayText<TDataType>());
				Info.Description = LOCTEXT("Metasound_SendNodeDescription", "Sends data from a send node with the same name.");
				Info.Author = PluginAuthor;
				Info.PromptIfMissing = PluginNodeMissingPrompt;
				Info.DefaultInterface = DeclareVertexInterface();
				Info.CategoryHierarchy = { LOCTEXT("Metasound_TransmissionNodeCategory", "Transmission") };
				Info.Keywords = { };

				return Info;
			};

			static const FNodeClassMetadata Info = InitNodeInfo();

			return Info;
		}


	private:
		class TSendOperator : public TExecutableOperator<TSendOperator>
		{
			public:

				TSendOperator(TDataReadReference<TDataType> InInputData, TDataReadReference<FSendAddress> InSendAddress, const FOperatorSettings& InOperatorSettings)
					: InputData(InInputData)
					, SendAddress(InSendAddress)
					, CachedSendAddress(*InSendAddress)
					, CachedSenderParams({InOperatorSettings, 0.0f})
					, Sender(nullptr)
				{
					Sender = CreateNewSender();
				}

				virtual ~TSendOperator() 
				{
					ResetSenderAndCleanupChannel();
				}

				virtual FDataReferenceCollection GetInputs() const override
				{
					FDataReferenceCollection Inputs;
					Inputs.AddDataReadReference<FSendAddress>(GetAddressInputName(), SendAddress);
					Inputs.AddDataReadReference<TDataType>(GetSendInputName(), TDataReadReference<TDataType>(InputData));
					return Inputs;
				}

				virtual FDataReferenceCollection GetOutputs() const override
				{
					return {};
				}

				void Execute()
				{
					if (*SendAddress != CachedSendAddress)
					{
						ResetSenderAndCleanupChannel();
						CachedSendAddress = *SendAddress;
						Sender = CreateNewSender();
						check(Sender.IsValid());
					}

					Sender->Push(*InputData);
				}

			private:

				TSenderPtr<TDataType> CreateNewSender() const
				{
					if (ensure(SendAddress->GetDataType().IsNone() || (GetMetasoundDataTypeName<TDataType>() == SendAddress->GetDataType())))
					{
						FSendAddress DataChannelKey(SendAddress->GetChannelName(), GetMetasoundDataTypeName<TDataType>(), SendAddress->GetInstanceID());
						return FDataTransmissionCenter::Get().RegisterNewSender<TDataType>(DataChannelKey, CachedSenderParams);
					}
					return TSenderPtr<TDataType>(nullptr);
				}

				void ResetSenderAndCleanupChannel()
				{
					Sender.Reset();
					FDataTransmissionCenter::Get().UnregisterDataChannelIfUnconnected(CachedSendAddress);
				}

				TDataReadReference<TDataType> InputData;
				TDataReadReference<FSendAddress> SendAddress;
				FSendAddress CachedSendAddress;
				FSenderInitParams CachedSenderParams;

				TSenderPtr<TDataType> Sender;
		};

		class FSendOperatorFactory : public IOperatorFactory
		{
			public:
				FSendOperatorFactory() = default;

				virtual TUniquePtr<IOperator> CreateOperator(const FCreateOperatorParams& InParams, FBuildErrorArray& OutErrors) override
				{
					if (InParams.InputDataReferences.ContainsDataReadReference<TDataType>(GetSendInputName()))
					{
						return MakeUnique<TSendOperator>(InParams.InputDataReferences.GetDataReadReference<TDataType>(GetSendInputName()),
							InParams.InputDataReferences.GetDataReadReferenceOrConstruct<FSendAddress>(GetAddressInputName()),
							InParams.OperatorSettings
						);
					}
					else
					{
						// No input hook up to send, so this node can no-op
						return MakeUnique<FNoOpOperator>();
					}
				}
		};

		public:

			TSendNode(const FNodeInitData& InInitData)
				: FNode(InInitData.InstanceName, InInitData.InstanceID, GetNodeInfo())
				, Interface(DeclareVertexInterface())
				, Factory(MakeOperatorFactoryRef<FSendOperatorFactory>())
			{
			}

			virtual ~TSendNode() = default;

			virtual const FVertexInterface& GetVertexInterface() const override
			{
				return Interface;
			}

			virtual bool SetVertexInterface(const FVertexInterface& InInterface) override
			{
				return Interface == InInterface;
			}

			virtual bool IsVertexInterfaceSupported(const FVertexInterface& InInterface) const override
			{
				return Interface == InInterface;
			}

			virtual FOperatorFactorySharedRef GetDefaultOperatorFactory() const override
			{
				return Factory;
			}

		private:
			FVertexInterface Interface;
			FOperatorFactorySharedRef Factory;
	};
}
#undef LOCTEXT_NAMESPACE
