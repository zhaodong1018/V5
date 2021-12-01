// Copyright Epic Games, Inc. All Rights Reserved.

#include "DerivedDataBuildAction.h"

#include "Algo/AllOf.h"
#include "Containers/Map.h"
#include "Containers/StringConv.h"
#include "Containers/StringView.h"
#include "Containers/UnrealString.h"
#include "DerivedDataBuildKey.h"
#include "DerivedDataBuildPrivate.h"
#include "IO/IoHash.h"
#include "Misc/Guid.h"
#include "Misc/TVariant.h"
#include "Serialization/CompactBinary.h"
#include "Serialization/CompactBinaryWriter.h"
#include "Templates/Tuple.h"
#include <atomic>

namespace UE::DerivedData::Private
{

class FBuildActionBuilderInternal final : public IBuildActionBuilderInternal
{
public:
	inline FBuildActionBuilderInternal(
		FStringView InName,
		FStringView InFunction,
		const FGuid& InFunctionVersion,
		const FGuid& InBuildSystemVersion)
		: Name(InName)
		, Function(InFunction)
		, FunctionVersion(InFunctionVersion)
		, BuildSystemVersion(InBuildSystemVersion)
	{
		checkf(!Name.IsEmpty(), TEXT("A build action requires a non-empty name."));
		AssertValidBuildFunctionName(Function, Name);
	}

	~FBuildActionBuilderInternal() final = default;

	void AddConstant(FStringView Key, const FCbObject& Value) final
	{
		Add(Key, Value);
	}

	void AddInput(FStringView Key, const FIoHash& RawHash, uint64 RawSize) final
	{
		Add(Key, MakeTuple(RawHash, RawSize));
	}

	FBuildAction Build() final;

	using InputType = TVariant<FCbObject, TTuple<FIoHash, uint64>>;

	FString Name;
	FString Function;
	FGuid FunctionVersion;
	FGuid BuildSystemVersion;
	TMap<FString, InputType> Inputs;

private:
	template <typename ValueType>
	inline void Add(FStringView Key, const ValueType& Value)
	{
		const uint32 KeyHash = GetTypeHash(Key);
		checkf(!Key.IsEmpty(), TEXT("Empty key used in action for build of '%s' by %s."), *Name, *Function);
		checkf(!Inputs.ContainsByHash(KeyHash, Key), TEXT("Duplicate key '%.*s' used in action ")
			TEXT("for build of '%s' by %s."), Key.Len(), Key.GetData(), *Name, *Function);
		Inputs.EmplaceByHash(KeyHash, Key, InputType(TInPlaceType<ValueType>(), Value));
	}
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class FBuildActionInternal final : public IBuildActionInternal
{
public:
	explicit FBuildActionInternal(FBuildActionBuilderInternal&& ActionBuilder);
	explicit FBuildActionInternal(FStringView Name, FCbObject&& Action, bool& bOutIsValid);

	~FBuildActionInternal() final = default;

	const FBuildActionKey& GetKey() const final { return Key; }

	FStringView GetName() const final { return Name; }
	FStringView GetFunction() const final { return Function; }
	const FGuid& GetFunctionVersion() const final { return FunctionVersion; }
	const FGuid& GetBuildSystemVersion() const final { return BuildSystemVersion; }

	bool HasConstants() const final;
	bool HasInputs() const final;

	void IterateConstants(TFunctionRef<void (FStringView Key, FCbObject&& Value)> Visitor) const final;
	void IterateInputs(TFunctionRef<void (FStringView Key, const FIoHash& RawHash, uint64 RawSize)> Visitor) const final;

	void Save(FCbWriter& Writer) const final;

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
	FGuid FunctionVersion;
	FGuid BuildSystemVersion;
	FCbObject Action;
	FBuildActionKey Key;
	mutable std::atomic<uint32> ReferenceCount{0};
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FBuildActionInternal::FBuildActionInternal(FBuildActionBuilderInternal&& ActionBuilder)
	: Name(MoveTemp(ActionBuilder.Name))
	, Function(MoveTemp(ActionBuilder.Function))
	, FunctionVersion(MoveTemp(ActionBuilder.FunctionVersion))
	, BuildSystemVersion(MoveTemp(ActionBuilder.BuildSystemVersion))
{
	ActionBuilder.Inputs.KeySort(TLess<>());

	bool bHasConstants = false;
	bool bHasInputs = false;

	for (const TPair<FString, FBuildActionBuilderInternal::InputType>& Pair : ActionBuilder.Inputs)
	{
		if (Pair.Value.IsType<FCbObject>())
		{
			bHasConstants = true;
		}
		else if (Pair.Value.IsType<TTuple<FIoHash, uint64>>())
		{
			bHasInputs = true;
		}
	}

	TCbWriter<2048> Writer;
	Writer.BeginObject();
	Writer.AddString("Function"_ASV, Function);
	Writer.AddUuid("FunctionVersion"_ASV, FunctionVersion);
	Writer.AddUuid("BuildSystemVersion"_ASV, BuildSystemVersion);

	if (bHasConstants)
	{
		Writer.BeginObject("Constants"_ASV);
		for (const TPair<FString, FBuildActionBuilderInternal::InputType>& Pair : ActionBuilder.Inputs)
		{
			if (Pair.Value.IsType<FCbObject>())
			{
				Writer.AddObject(FTCHARToUTF8(Pair.Key), Pair.Value.Get<FCbObject>());
			}
		}
		Writer.EndObject();
	}

	if (bHasInputs)
	{
		Writer.BeginObject("Inputs"_ASV);
		for (const TPair<FString, FBuildActionBuilderInternal::InputType>& Pair : ActionBuilder.Inputs)
		{
			if (Pair.Value.IsType<TTuple<FIoHash, uint64>>())
			{
				const TTuple<FIoHash, uint64>& Input = Pair.Value.Get<TTuple<FIoHash, uint64>>();
				Writer.BeginObject(FTCHARToUTF8(Pair.Key));
				Writer.AddBinaryAttachment("RawHash"_ASV, Input.Get<FIoHash>());
				Writer.AddInteger("RawSize"_ASV, Input.Get<uint64>());
				Writer.EndObject();
			}
		}
		Writer.EndObject();
	}

	Writer.EndObject();
	Action = Writer.Save().AsObject();
	Key.Hash = Action.GetHash();
}

FBuildActionInternal::FBuildActionInternal(FStringView InName, FCbObject&& InAction, bool& bOutIsValid)
	: Name(InName)
	, Function(InAction.FindView("Function"_ASV).AsString())
	, FunctionVersion(InAction.FindView("FunctionVersion"_ASV).AsUuid())
	, BuildSystemVersion(InAction.FindView("BuildSystemVersion"_ASV).AsUuid())
	, Action(MoveTemp(InAction))
	, Key{Action.GetHash()}
{
	checkf(!Name.IsEmpty(), TEXT("A build action requires a non-empty name."));
	Action.MakeOwned();
	bOutIsValid = Action
		&& IsValidBuildFunctionName(Function)
		&& FunctionVersion.IsValid()
		&& BuildSystemVersion.IsValid()
		&& Algo::AllOf(Action.AsView()["Constants"_ASV],
			[](FCbFieldView Field) { return Field.GetName().Len() > 0 && Field.IsObject(); })
		&& Algo::AllOf(Action.AsView()["Inputs"_ASV], [](FCbFieldView Field)
			{
				return Field.GetName().Len() > 0 && Field.IsObject()
					&& Field["RawHash"_ASV].IsBinaryAttachment()
					&& Field["RawSize"_ASV].IsInteger();
			});
}

bool FBuildActionInternal::HasConstants() const
{
	return Action["Constants"_ASV].HasValue();
}

bool FBuildActionInternal::HasInputs() const
{
	return Action["Inputs"_ASV].HasValue();
}

void FBuildActionInternal::IterateConstants(TFunctionRef<void (FStringView Key, FCbObject&& Value)> Visitor) const
{
	for (FCbField Field : Action["Constants"_ASV])
	{
		Visitor(FUTF8ToTCHAR(Field.GetName()), Field.AsObject());
	}
}

void FBuildActionInternal::IterateInputs(TFunctionRef<void (FStringView Key, const FIoHash& RawHash, uint64 RawSize)> Visitor) const
{
	for (FCbFieldView Field : Action.AsView()["Inputs"_ASV])
	{
		Visitor(FUTF8ToTCHAR(Field.GetName()), Field["RawHash"_ASV].AsHash(), Field["RawSize"_ASV].AsUInt64());
	}
}

void FBuildActionInternal::Save(FCbWriter& Writer) const
{
	Writer.AddObject(Action);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FBuildAction FBuildActionBuilderInternal::Build()
{
	return CreateBuildAction(new FBuildActionInternal(MoveTemp(*this)));
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FBuildAction CreateBuildAction(IBuildActionInternal* Action)
{
	return FBuildAction(Action);
}

FBuildActionBuilder CreateBuildActionBuilder(IBuildActionBuilderInternal* ActionBuilder)
{
	return FBuildActionBuilder(ActionBuilder);
}

FBuildActionBuilder CreateBuildAction(FStringView Name, FStringView Function, const FGuid& FunctionVersion, const FGuid& BuildSystemVersion)
{
	return CreateBuildActionBuilder(new FBuildActionBuilderInternal(Name, Function, FunctionVersion, BuildSystemVersion));
}

} // UE::DerivedData::Private

namespace UE::DerivedData
{

FOptionalBuildAction FBuildAction::Load(FStringView Name, FCbObject&& Action)
{
	bool bIsValid = false;
	FOptionalBuildAction Out = Private::CreateBuildAction(
		new Private::FBuildActionInternal(Name, MoveTemp(Action), bIsValid));
	if (!bIsValid)
	{
		Out.Reset();
	}
	return Out;
}

} // UE::DerivedData
