// Copyright Epic Games, Inc. All Rights Reserved.

#include "DerivedDataBuildOutput.h"

#include "Algo/BinarySearch.h"
#include "Algo/Find.h"
#include "Containers/StringView.h"
#include "Containers/UnrealString.h"
#include "DerivedDataBuildKey.h"
#include "DerivedDataBuildPrivate.h"
#include "DerivedDataCacheRecord.h"
#include "DerivedDataPayload.h"
#include "Misc/StringBuilder.h"
#include "Serialization/CompactBinary.h"
#include "Serialization/CompactBinaryWriter.h"

namespace UE::DerivedData::Private
{

static FUtf8StringView LexToString(EBuildOutputMessageLevel Level)
{
	switch (Level)
	{
	case EBuildOutputMessageLevel::Error: return "Error"_U8SV;
	case EBuildOutputMessageLevel::Warning: return "Warning"_U8SV;
	case EBuildOutputMessageLevel::Display: return "Display"_U8SV;
	default: return "Unknown"_U8SV;
	}
}

static bool TryLexFromString(EBuildOutputMessageLevel& OutLevel, FUtf8StringView String)
{
	if (String.Equals("Error"_U8SV, ESearchCase::CaseSensitive))
	{
		OutLevel = EBuildOutputMessageLevel::Error;
		return true;
	}
	if (String.Equals("Warning"_U8SV, ESearchCase::CaseSensitive))
	{
		OutLevel = EBuildOutputMessageLevel::Warning;
		return true;
	}
	if (String.Equals("Display"_U8SV, ESearchCase::CaseSensitive))
	{
		OutLevel = EBuildOutputMessageLevel::Display;
		return true;
	}
	return false;
}

static FUtf8StringView LexToString(EBuildOutputLogLevel Level)
{
	switch (Level)
	{
	case EBuildOutputLogLevel::Error: return "Error"_U8SV;
	case EBuildOutputLogLevel::Warning: return "Warning"_U8SV;
	default: return "Unknown"_U8SV;
	}
}

static bool TryLexFromString(EBuildOutputLogLevel& OutLevel, FUtf8StringView String)
{
	if (String.Equals("Error"_U8SV, ESearchCase::CaseSensitive))
	{
		OutLevel = EBuildOutputLogLevel::Error;
		return true;
	}
	if (String.Equals("Warning"_U8SV, ESearchCase::CaseSensitive))
	{
		OutLevel = EBuildOutputLogLevel::Warning;
		return true;
	}
	return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class FBuildOutputBuilderInternal final : public IBuildOutputBuilderInternal
{
public:
	explicit FBuildOutputBuilderInternal(FStringView InName, FStringView InFunction)
		: Name(InName)
		, Function(InFunction)
	{
		checkf(!Name.IsEmpty(), TEXT("A build output requires a non-empty name."));
		AssertValidBuildFunctionName(Function, Name);
		MessageWriter.BeginArray();
		LogWriter.BeginArray();
	}

	~FBuildOutputBuilderInternal() final = default;

	void SetMeta(FCbObject&& InMeta) final { Meta = MoveTemp(InMeta); Meta.MakeOwned(); }
	void AddPayload(const FPayload& Payload) final;
	void AddMessage(const FBuildOutputMessage& Message) final;
	void AddLog(const FBuildOutputLog& Log) final;
	bool HasError() const final { return bHasError; }
	FBuildOutput Build() final;

	FString Name;
	FString Function;
	FCbObject Meta;
	TArray<FPayload> Payloads;
	FCbWriter MessageWriter;
	FCbWriter LogWriter;
	bool bHasMessages = false;
	bool bHasLogs = false;
	bool bHasError = false;
};

class FBuildOutputInternal final : public IBuildOutputInternal
{
public:
	FBuildOutputInternal(FString&& Name, FString&& Function, FCbObject&& Meta, FCbObject&& Output, TArray<FPayload>&& Payloads);
	FBuildOutputInternal(FStringView Name, FStringView Function, const FCbObject& Output, bool& bOutIsValid);
	FBuildOutputInternal(FStringView Name, FStringView Function, const FCacheRecord& Output, bool& bOutIsValid);

	~FBuildOutputInternal() final = default;

	FStringView GetName() const final { return Name; }
	FStringView GetFunction() const final { return Function; }

	const FCbObject& GetMeta() const final { return Meta; }

	const FPayload& GetPayload(const FPayloadId& Id) const final;
	TConstArrayView<FPayload> GetPayloads() const final { return Payloads; }
	TConstArrayView<FBuildOutputMessage> GetMessages() const final { return Messages; }
	TConstArrayView<FBuildOutputLog> GetLogs() const final { return Logs; }
	bool HasLogs() const final { return !Logs.IsEmpty(); }
	bool HasError() const final;

	bool TryLoad();

	void Save(FCbWriter& Writer) const final;
	void Save(FCacheRecordBuilder& RecordBuilder) const final;

	inline void AddRef() const final
	{
		ReferenceCount.fetch_add(1, std::memory_order_relaxed);
	}

	inline void Release() const final
	{
		if (ReferenceCount.fetch_sub(1, std::memory_order_acq_rel) == 1)
		{
			delete this;
		}
	}

private:
	FString Name;
	FString Function;
	FCbObject Meta;
	FCbObject Output;
	TArray<FPayload> Payloads;
	TArray<FBuildOutputMessage> Messages;
	TArray<FBuildOutputLog> Logs;
	mutable std::atomic<uint32> ReferenceCount{0};
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FBuildOutputInternal::FBuildOutputInternal(
	FString&& InName,
	FString&& InFunction,
	FCbObject&& InMeta,
	FCbObject&& InOutput,
	TArray<FPayload>&& InPayloads)
	: Name(MoveTemp(InName))
	, Function(MoveTemp(InFunction))
	, Meta(MoveTemp(InMeta))
	, Output(MoveTemp(InOutput))
	, Payloads(MoveTemp(InPayloads))
{
	Meta.MakeOwned();
	Output.MakeOwned();
	TryLoad();
}

FBuildOutputInternal::FBuildOutputInternal(FStringView InName, FStringView InFunction, const FCbObject& InOutput, bool& bOutIsValid)
	: Name(InName)
	, Function(InFunction)
	, Output(InOutput)
{
	checkf(!Name.IsEmpty(), TEXT("A build output requires a non-empty name."));
	AssertValidBuildFunctionName(Function, Name);
	Output.MakeOwned();
	FCbField MetaField = InOutput["Meta"_ASV];
	bOutIsValid = TryLoad() && (!MetaField || MetaField.IsObject());
	Meta = MoveTemp(MetaField).AsObject();
}

FBuildOutputInternal::FBuildOutputInternal(FStringView InName, FStringView InFunction, const FCacheRecord& InOutput, bool& bOutIsValid)
	: Name(InName)
	, Function(InFunction)
	, Meta(InOutput.GetMeta())
	, Output(InOutput.GetValue())
	, Payloads(InOutput.GetAttachmentPayloads())
{
	checkf(!Name.IsEmpty(), TEXT("A build output requires a non-empty name."));
	AssertValidBuildFunctionName(Function, Name);
	bOutIsValid = TryLoad();
}

const FPayload& FBuildOutputInternal::GetPayload(const FPayloadId& Id) const
{
	const int32 Index = Algo::BinarySearchBy(Payloads, Id, &FPayload::GetId);
	return Payloads.IsValidIndex(Index) ? Payloads[Index] : FPayload::Null;
}

bool FBuildOutputInternal::HasError() const
{
	return Algo::FindBy(Messages, EBuildOutputMessageLevel::Error, &FBuildOutputMessage::Level) ||
		Algo::FindBy(Logs, EBuildOutputLogLevel::Error, &FBuildOutputLog::Level);
}

bool FBuildOutputInternal::TryLoad()
{
	const FCbObjectView OutputView = Output;

	if (Payloads.IsEmpty())
	{
		for (FCbFieldView Payload : OutputView["Payloads"_ASV])
		{
			const FPayloadId Id = Payload["Id"_ASV].AsObjectId();
			const FIoHash& RawHash = Payload["RawHash"_ASV].AsAttachment();
			const uint64 RawSize = Payload["RawSize"_ASV].AsUInt64(MAX_uint64);
			if (Id.IsNull() || RawHash.IsZero() || RawSize == MAX_uint64)
			{
				return false;
			}
			Payloads.Emplace(Id, RawHash, RawSize);
		}
	}

	if (FCbFieldView MessagesField = OutputView["Messages"_ASV])
	{
		if (!MessagesField.IsArray())
		{
			return false;
		}
		Messages.Reserve(MessagesField.AsArrayView().Num());
		for (FCbFieldView MessageField : MessagesField)
		{
			const FUtf8StringView LevelName = MessageField["Level"_ASV].AsString();
			const FUtf8StringView Message = MessageField["Message"_ASV].AsString();
			EBuildOutputMessageLevel Level;
			if (LevelName.IsEmpty() || Message.IsEmpty() || !TryLexFromString(Level, LevelName))
			{
				return false;
			}
			Messages.Add({Message, Level});
		}
	}

	if (FCbFieldView LogsField = OutputView["Logs"_ASV])
	{
		if (!LogsField.IsArray())
		{
			return false;
		}
		Logs.Reserve(LogsField.AsArrayView().Num());
		for (FCbFieldView LogField : LogsField)
		{
			const FUtf8StringView LevelName = LogField["Level"_ASV].AsString();
			const FUtf8StringView Category = LogField["Category"_ASV].AsString();
			const FUtf8StringView Message = LogField["Message"_ASV].AsString();
			EBuildOutputLogLevel Level;
			if (LevelName.IsEmpty() || Category.IsEmpty() || Message.IsEmpty() || !TryLexFromString(Level, LevelName))
			{
				return false;
			}
			Logs.Add({Category, Message, Level});
		}
	}

	return true;
}

void FBuildOutputInternal::Save(FCbWriter& Writer) const
{
	Writer.BeginObject();
	if (!Payloads.IsEmpty())
	{
		Writer.BeginArray("Payloads"_ASV);
		for (const FPayload& Payload : Payloads)
		{
			Writer.BeginObject();
			Writer.AddObjectId("Id"_ASV, Payload.GetId());
			Writer.AddBinaryAttachment("RawHash"_ASV, Payload.GetRawHash());
			Writer.AddInteger("RawSize"_ASV, Payload.GetRawSize());
			Writer.EndObject();
		}
		Writer.EndArray();
	}
	if (FCbField MessagesField = Output["Messages"_ASV])
	{
		Writer.AddField("Messages"_ASV, MessagesField);
	}
	if (FCbField LogsField = Output["Logs"_ASV])
	{
		Writer.AddField("Logs"_ASV, LogsField);
	}
	if (Meta)
	{
		Writer.AddObject("Meta"_ASV, Meta);
	}
	Writer.EndObject();
}

void FBuildOutputInternal::Save(FCacheRecordBuilder& RecordBuilder) const
{
	RecordBuilder.SetMeta(FCbObject(Meta));
	if (!Messages.IsEmpty() || !Logs.IsEmpty())
	{
		TCbWriter<1024> Writer;
		if (FCbField MessagesField = Output["Messages"_ASV])
		{
			Writer.AddField("Messages"_ASV, MessagesField);
		}
		if (FCbField LogsField = Output["Logs"_ASV])
		{
			Writer.AddField("Logs"_ASV, LogsField);
		}
		RecordBuilder.SetValue(Writer.Save().GetBuffer());
	}
	for (const FPayload& Payload : Payloads)
	{
		RecordBuilder.AddAttachment(Payload);
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void FBuildOutputBuilderInternal::AddPayload(const FPayload& Payload)
{
	checkf(Payload, TEXT("Null payload added in output for build of '%s' by %s."), *Name, *Function);
	const FPayloadId& Id = Payload.GetId();
	const int32 Index = Algo::LowerBoundBy(Payloads, Id, &FPayload::GetId);
	checkf(!(Payloads.IsValidIndex(Index) && Payloads[Index].GetId() == Id),
		TEXT("Duplicate ID %s used by payload for build of '%s' by %s."), *WriteToString<32>(Id), *Name, *Function);
	Payloads.Insert(Payload, Index);
}

void FBuildOutputBuilderInternal::AddMessage(const FBuildOutputMessage& Message)
{
	bHasError |= Message.Level == EBuildOutputMessageLevel::Error;
	bHasMessages = true;
	MessageWriter.BeginObject();
	MessageWriter.AddString("Level"_ASV, LexToString(Message.Level));
	MessageWriter.AddString("Message"_ASV, Message.Message);
	MessageWriter.EndObject();
}

void FBuildOutputBuilderInternal::AddLog(const FBuildOutputLog& Log)
{
	bHasError |= Log.Level == EBuildOutputLogLevel::Error;
	bHasLogs = true;
	LogWriter.BeginObject();
	LogWriter.AddString("Level"_ASV, LexToString(Log.Level));
	LogWriter.AddString("Category"_ASV, Log.Category);
	LogWriter.AddString("Message"_ASV, Log.Message);
	LogWriter.EndObject();
}

FBuildOutput FBuildOutputBuilderInternal::Build()
{
	if (bHasError)
	{
		Payloads.Empty();
	}
	MessageWriter.EndArray();
	LogWriter.EndArray();
	FCbObject Output;
	if (bHasMessages || bHasLogs)
	{
		TCbWriter<1024> Writer;
		Writer.BeginObject();
		if (bHasMessages)
		{
			Writer.AddArray("Messages"_ASV, MessageWriter.Save().AsArray());
			MessageWriter.Reset();
		}
		if (bHasLogs)
		{
			Writer.AddArray("Logs"_ASV, LogWriter.Save().AsArray());
			LogWriter.Reset();
		}
		Writer.EndObject();
		Output = Writer.Save().AsObject();
	}
	return CreateBuildOutput(new FBuildOutputInternal(MoveTemp(Name), MoveTemp(Function), MoveTemp(Meta), MoveTemp(Output), MoveTemp(Payloads)));
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FBuildOutput CreateBuildOutput(IBuildOutputInternal* Output)
{
	return FBuildOutput(Output);
}

FBuildOutputBuilder CreateBuildOutputBuilder(IBuildOutputBuilderInternal* OutputBuilder)
{
	return FBuildOutputBuilder(OutputBuilder);
}

FBuildOutputBuilder CreateBuildOutput(FStringView Name, FStringView Function)
{
	return CreateBuildOutputBuilder(new FBuildOutputBuilderInternal(Name, Function));
}

} // UE::DerivedData::Private

namespace UE::DerivedData
{

FOptionalBuildOutput FBuildOutput::Load(FStringView Name, FStringView Function, const FCbObject& Output)
{
	bool bIsValid = false;
	FOptionalBuildOutput Out = Private::CreateBuildOutput(
		new Private::FBuildOutputInternal(Name, Function, Output, bIsValid));
	if (!bIsValid)
	{
		Out.Reset();
	}
	return Out;
}

FOptionalBuildOutput FBuildOutput::Load(FStringView Name, FStringView Function, const FCacheRecord& Output)
{
	bool bIsValid = false;
	FOptionalBuildOutput Out = Private::CreateBuildOutput(
		new Private::FBuildOutputInternal(Name, Function, Output, bIsValid));
	if (!bIsValid)
	{
		Out.Reset();
	}
	return Out;
}

} // UE::DerivedData
