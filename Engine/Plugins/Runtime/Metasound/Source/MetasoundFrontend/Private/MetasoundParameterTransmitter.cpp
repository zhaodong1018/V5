// Copyright Epic Games, Inc. All Rights Reserved.

#include "MetasoundParameterTransmitter.h"

#include "IAudioGeneratorInterfaceRegistry.h"
#include "MetasoundLog.h"


namespace Metasound
{
	namespace Frontend
	{
		FLiteral ConvertParameterToLiteral(FAudioParameter&& InValue)
		{
			switch (InValue.ParamType)
			{
				case EAudioParameterType::Boolean:
				{
					return FLiteral(InValue.BoolParam);
				}

				case EAudioParameterType::BooleanArray:
				{
					return FLiteral(MoveTemp(InValue.ArrayBoolParam));
				}

				case EAudioParameterType::Float:
				{
					return FLiteral(InValue.FloatParam);
				}

				case EAudioParameterType::FloatArray:
				{
					return FLiteral(MoveTemp(InValue.ArrayFloatParam));
				}

				case EAudioParameterType::Integer:
				{
					return FLiteral(InValue.IntParam);
				}

				case EAudioParameterType::IntegerArray:
				{
					return FLiteral(MoveTemp(InValue.ArrayIntParam));
				}

				case EAudioParameterType::None:
				{
					return FLiteral();
				}

				case EAudioParameterType::NoneArray:
				{
					TArray<FLiteral::FNone> InitArray;
					InitArray.Init(FLiteral::FNone(), InValue.IntParam);
					return FLiteral(MoveTemp(InitArray));
				}

				case EAudioParameterType::Object:
				{
					if (InValue.ObjectProxies.IsEmpty())
					{
						return FLiteral();
					}

					return FLiteral(MoveTemp(InValue.ObjectProxies[0]));
				}

				case EAudioParameterType::ObjectArray:
				{
					return FLiteral(MoveTemp(InValue.ObjectProxies));
				}

				case EAudioParameterType::String:
				{
					return FLiteral(MoveTemp(InValue.StringParam));
				}

				case EAudioParameterType::StringArray:
				{
					return FLiteral(MoveTemp(InValue.ArrayStringParam));
				}

				default:
				{
					static_assert(static_cast<int32>(EAudioParameterType::COUNT) == 12, "Possible missing switch case coverage");
					checkNoEntry();
				}
			}

			return FLiteral();
		}

		FName ConvertParameterToDataType(EAudioParameterType InParameterType)
		{
			switch (InParameterType)
			{
				case EAudioParameterType::Boolean:
					return GetMetasoundDataTypeName<bool>();
				case EAudioParameterType::BooleanArray:
					return GetMetasoundDataTypeName<TArray<bool>>();
				case EAudioParameterType::Float:
					return GetMetasoundDataTypeName<float>();
				case EAudioParameterType::FloatArray:
					return GetMetasoundDataTypeName<TArray<float>>();
				case EAudioParameterType::Integer:
					return GetMetasoundDataTypeName<int32>();
				case EAudioParameterType::IntegerArray:
					return GetMetasoundDataTypeName<TArray<int32>>();
				case EAudioParameterType::String:
					return GetMetasoundDataTypeName<FString>();
				case EAudioParameterType::StringArray:
					return GetMetasoundDataTypeName<TArray<FString>>();

				case EAudioParameterType::Object:
				case EAudioParameterType::ObjectArray:
					// TODO: Add support for objects

				case EAudioParameterType::None:
				case EAudioParameterType::NoneArray:
				default:
					ensureAlwaysMsgf(false, TEXT("Failed to convert AudioParameterType to POD MetaSound DataType"));
					static_assert(static_cast<int32>(EAudioParameterType::COUNT) == 12, "Possible missing case coverage");
					return FName();
			}
		}
	}

	const FVertexName& FMetaSoundParameterTransmitter::GetInstanceIDEnvironmentVariableName()
	{
		static const FVertexName VarName = "TransmitterInstanceID";
		return VarName;
	}

	FSendAddress FMetaSoundParameterTransmitter::CreateSendAddressFromEnvironment(const FMetasoundEnvironment& InEnvironment, const FVertexName& InVertexName, const FName& InTypeName)
	{
		const FVertexName IDVarName = GetInstanceIDEnvironmentVariableName();
		uint64 InstanceID = -1;

		if (ensure(InEnvironment.Contains<uint64>(IDVarName)))
		{
			InstanceID = InEnvironment.GetValue<uint64>(IDVarName);
		}

		return CreateSendAddressFromInstanceID(InstanceID, InVertexName, InTypeName);
	}

	FSendAddress FMetaSoundParameterTransmitter::CreateSendAddressFromInstanceID(uint64 InInstanceID, const FVertexName& InVertexName, const FName& InTypeName)
	{
		return FSendAddress(InVertexName, InTypeName, InInstanceID);
	}

	FMetaSoundParameterTransmitter::FMetaSoundParameterTransmitter(const FMetaSoundParameterTransmitter::FInitParams& InInitParams)
	: SendInfos(InInitParams.Infos)
	, OperatorSettings(InInitParams.OperatorSettings)
	, InstanceID(InInitParams.InstanceID)
	{
	}

	bool FMetaSoundParameterTransmitter::Reset()
	{
		bool bSuccess = true;

		for (const FSendInfo& SendInfo : SendInfos)
		{
			bSuccess &= FDataTransmissionCenter::Get().UnregisterDataChannel(SendInfo.Address);
		}

		return bSuccess;
	}

	uint64 FMetaSoundParameterTransmitter::GetInstanceID() const
	{
		return InstanceID;
	}

	bool FMetaSoundParameterTransmitter::SetParameter(FAudioParameter&& InParameter)
	{
		const FName ParamName = InParameter.ParamName;
		return SetParameterWithLiteral(ParamName, Frontend::ConvertParameterToLiteral(MoveTemp(InParameter)));
	}

	bool FMetaSoundParameterTransmitter::SetParameter(FName InInterfaceName, FAudioParameter&& InParameter)
	{
		InParameter.ParamName = Audio::IGeneratorInterfaceRegistry::GetMemberFullName(InInterfaceName, InParameter.ParamName);
		const FName ParamName = InParameter.ParamName;
		return SetParameterWithLiteral(ParamName, Frontend::ConvertParameterToLiteral(MoveTemp(InParameter)));
	}

	bool FMetaSoundParameterTransmitter::SetParameterWithLiteral(FName InParameterName, const FLiteral& InLiteral)
	{
		if (ISender* Sender = FindSender(InParameterName))
		{
			return Sender->PushLiteral(InLiteral);
		}

		// If no sender exists for parameter name, attempt to add one.
		if (const FSendInfo* SendInfo = FindSendInfo(InParameterName))
		{
			if (ISender* Sender = AddSender(*SendInfo))
			{
				return Sender->PushLiteral(InLiteral);
			}
		}

		return false;
	}

	TUniquePtr<Audio::IParameterTransmitter> FMetaSoundParameterTransmitter::Clone() const
	{
		return MakeUnique<FMetaSoundParameterTransmitter>(FMetaSoundParameterTransmitter::FInitParams(OperatorSettings, InstanceID, SendInfos));
	}

	const FMetaSoundParameterTransmitter::FSendInfo* FMetaSoundParameterTransmitter::FindSendInfo(const FName& InParameterName) const
	{
		return SendInfos.FindByPredicate([&](const FSendInfo& Info) { return Info.ParameterName == InParameterName; });
	}

	ISender* FMetaSoundParameterTransmitter::FindSender(const FName& InParameterName)
	{
		if (TUniquePtr<ISender>* SenderPtrPtr = InputSends.Find(InParameterName))
		{
			return SenderPtrPtr->Get();
		}
		return nullptr;
	}

	ISender* FMetaSoundParameterTransmitter::AddSender(const FSendInfo& InInfo)
	{
		// TODO: likely want to remove this and opt for different protocols having different behaviors.
		const float DelayTimeInSeconds = 0.1f; // This not used for non-audio routing.
		const FSenderInitParams InitParams = { OperatorSettings, DelayTimeInSeconds };

		TUniquePtr<ISender> Sender = FDataTransmissionCenter::Get().RegisterNewSender(InInfo.Address, InitParams);
		if (ensureMsgf(Sender.IsValid(), TEXT("Failed to create sender [Address:%s]"), *InInfo.Address.ToString()))
		{
			ISender* Ptr = Sender.Get();
			InputSends.Add(InInfo.ParameterName, MoveTemp(Sender));
			return Ptr;
		}

		return nullptr;
	}
}
