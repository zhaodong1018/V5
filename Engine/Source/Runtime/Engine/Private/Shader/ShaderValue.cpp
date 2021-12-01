// Copyright Epic Games, Inc. All Rights Reserved.
#include "Shader/ShaderTypes.h"
#include "Misc/StringBuilder.h"
#include "Misc/LargeWorldRenderPosition.h"

namespace UE
{
namespace Shader
{

namespace Private
{
struct FCastFloat
{
	using FComponentType = float;
	inline float operator()(EValueComponentType Type, const FValueComponent& Component) const
	{
		switch (Type)
		{
		case EValueComponentType::Float: return Component.Float;
		case EValueComponentType::Double: return (float)Component.Double;
		case EValueComponentType::Int: return (float)Component.Int;
		case EValueComponentType::Bool: return (float)Component.Bool;
		default: return 0.0f;
		}
	}
};

struct FCastDouble
{
	using FComponentType = double;
	inline double operator()(EValueComponentType Type, const FValueComponent& Component) const
	{
		switch (Type)
		{
		case EValueComponentType::Float: return (double)Component.Float;
		case EValueComponentType::Double: return Component.Double;
		case EValueComponentType::Int: return (double)Component.Int;
		case EValueComponentType::Bool: return (double)Component.Bool;
		default: return 0.0f;
		}
	}
};

struct FCastInt
{
	using FComponentType = int32;
	inline int32 operator()(EValueComponentType Type, const FValueComponent& Component) const
	{
		switch (Type)
		{
		case EValueComponentType::Float: return (int32)Component.Float;
		case EValueComponentType::Double: return (int32)Component.Double;
		case EValueComponentType::Int: return Component.Int;
		case EValueComponentType::Bool: return Component.Bool ? 1 : 0;
		default: return false;
		}
	}
};

struct FCastBool
{
	using FComponentType = bool;
	inline bool operator()(EValueComponentType Type, const FValueComponent& Component) const
	{
		switch (Type)
		{
		case EValueComponentType::Float: return Component.Float != 0.0f;
		case EValueComponentType::Double: return Component.Double != 0.0;
		case EValueComponentType::Int: return Component.Int != 0;
		case EValueComponentType::Bool: return Component.AsBool();
		default: return false;
		}
	}
};

template<typename Operation, typename ResultType>
void AsType(const Operation& Op, const FValue& Value, ResultType& OutResult)
{
	using FComponentType = typename Operation::FComponentType;
	const EValueComponentType ComponentType = Value.ComponentType;
	const int32 NumComponents = Value.NumComponents;
	if (NumComponents == 1)
	{
		const FComponentType Component = Op(ComponentType, Value.Component[0]);
		for (int32 i = 0; i < 4; ++i)
		{
			OutResult[i] = Component;
		}
	}
	else
	{
		for (int32 i = 0; i < NumComponents; ++i)
		{
			OutResult[i] = Op(ComponentType, Value.Component[i]);
		}
		for (int32 i = NumComponents; i < 4; ++i)
		{
			OutResult[i] = (FComponentType)0;
		}
	}
}

template<typename Operation>
void Cast(const Operation& Op, const FValue& Value, FValue& OutResult)
{
	using FComponentType = typename Operation::FComponentType;
	const EValueComponentType ComponentType = Value.ComponentType;
	const int32 NumResultComponents = OutResult.NumComponents;
	const int32 NumCopyComponents = FMath::Min<int32>(Value.NumComponents, NumResultComponents);
	for (int32 i = 0; i < NumCopyComponents; ++i)
	{
		OutResult.Component[i] = Op(ComponentType, Value.Component[i]);
	}

	if (NumCopyComponents == 1 && NumResultComponents > 1)
	{
		for (int32 i = 1; i < NumResultComponents; ++i)
		{
			OutResult.Component[i] = OutResult.Component[0];
		}
	}
}

void FormatComponent_Double(double Value, int32 NumComponents, EValueStringFormat Format, FStringBuilderBase& OutResult)
{
	if (Format == EValueStringFormat::HLSL)
	{
		OutResult.Appendf(TEXT("%0.8f"), Value);
	}
	else
	{
		// Shorter format for more components
		switch (NumComponents)
		{
		case 4: OutResult.Appendf(TEXT("%.2g"), Value); break;
		case 3: OutResult.Appendf(TEXT("%.3g"), Value); break;
		case 2: OutResult.Appendf(TEXT("%.3g"), Value); break;
		default: OutResult.Appendf(TEXT("%.4g"), Value); break;
		}
	}
}

} // namespace Private

FValue FValue::FromMemoryImage(EValueType Type, const void* Data, uint32* OutSizeInBytes)
{
	FValue Result(Type);
	const uint8* Bytes = static_cast<const uint8*>(Data);
	const uint32 ComponentSizeInBytes = GetComponentTypeSizeInBytes(Result.ComponentType);
	if (ComponentSizeInBytes > 0u)
	{
		for (int32 i = 0u; i < Result.NumComponents; ++i)
		{
			FMemory::Memcpy(&Result.Component[i].Packed, Bytes, ComponentSizeInBytes);
			Bytes += ComponentSizeInBytes;
		}
	}
	if (OutSizeInBytes)
	{
		*OutSizeInBytes = (uint32)(Bytes - static_cast<const uint8*>(Data));
	}
	return Result;
}

FMemoryImageValue FValue::AsMemoryImage() const
{
	FMemoryImageValue Result;
	uint8* Bytes = Result.Bytes;
	const uint32 ComponentSizeInBytes = GetComponentTypeSizeInBytes(ComponentType);
	if (ComponentSizeInBytes > 0u)
	{
		for (int32 i = 0u; i < NumComponents; ++i)
		{
			FMemory::Memcpy(Bytes, &Component[i].Packed, ComponentSizeInBytes);
			Bytes += ComponentSizeInBytes;
		}
	}
	Result.Size = (uint32)(Bytes - Result.Bytes);
	check(Result.Size <= FMemoryImageValue::MaxSize);
	return Result;
}

FFloatValue FValue::AsFloat() const
{
	FFloatValue Result;
	Private::AsType(Private::FCastFloat(), *this, Result);
	return Result;
}

FDoubleValue FValue::AsDouble() const
{
	FDoubleValue Result;
	Private::AsType(Private::FCastDouble(), *this, Result);
	return Result;
}

FLinearColor FValue::AsLinearColor() const
{
	const FFloatValue Result = AsFloat();
	return FLinearColor(Result.Component[0], Result.Component[1], Result.Component[2], Result.Component[3]);
}

FVector4d FValue::AsVector4d() const
{
	const FDoubleValue Result = AsDouble();
	return FVector4d(Result.Component[0], Result.Component[1], Result.Component[2], Result.Component[3]);
}

FIntValue FValue::AsInt() const
{
	FIntValue Result;
	Private::AsType(Private::FCastInt(), *this, Result);
	return Result;
}

FBoolValue FValue::AsBool() const
{
	FBoolValue Result;
	Private::AsType(Private::FCastBool(), *this, Result);
	return Result;
}

float FValue::AsFloatScalar() const
{
	FFloatValue Result;
	Private::AsType(Private::FCastFloat(), *this, Result);
	return Result[0];
}

bool FValue::AsBoolScalar() const
{
	const FBoolValue Result = AsBool();
	for (int32 i = 0; i < NumComponents; ++i)
	{
		if (Result.Component[i])
		{
			return true;
		}
	}
	return false;
}

uint32 GetComponentTypeSizeInBytes(EValueComponentType Type)
{
	switch (Type)
	{
	case EValueComponentType::Void: return 0u;
	case EValueComponentType::Float: return sizeof(float);
	case EValueComponentType::Double: return sizeof(double);
	case EValueComponentType::Int: return sizeof(int32);
	case EValueComponentType::Bool: return 1u;
	default: checkNoEntry(); return 0u;
	}
}

const TCHAR* FValueComponent::ToString(EValueComponentType Type, FStringBuilderBase& OutString) const
{
	switch (Type)
	{
	case EValueComponentType::Int: OutString.Appendf(TEXT("%d"), Int); break;
	case EValueComponentType::Bool: OutString.Append(AsBool() ? TEXT("true") : TEXT("false")); break;
	case EValueComponentType::Float: OutString.Appendf(TEXT("%#.9gf"), Float); break;
	default: checkNoEntry(); break; // TODO - double
	}
	return OutString.ToString();
}

const TCHAR* FValue::ToString(EValueStringFormat Format, FStringBuilderBase& OutString) const
{
	if (Format == EValueStringFormat::HLSL && ComponentType == EValueComponentType::Double)
	{
		// Construct an HLSL LWC Vector
		TStringBuilder<256> TileValue;
		TStringBuilder<256> OffsetValue;
		for (int32 i = 0; i < NumComponents; ++i)
		{
			if (i > 0)
			{
				TileValue.Append(TEXT(", "));
				OffsetValue.Append(TEXT(", "));
			}

			const FLargeWorldRenderScalar Value(Component[i].Double);
			Private::FormatComponent_Double(Value.GetTileAsDouble(), NumComponents, Format, TileValue);
			Private::FormatComponent_Double(Value.GetOffsetAsDouble(), NumComponents, Format, OffsetValue);
		}

		if (NumComponents > 1)
		{
			OutString.Appendf(TEXT("MakeLWCVector%d(float%d(%s), float%d(%s))"), NumComponents, NumComponents, TileValue.ToString(), NumComponents, OffsetValue.ToString());
		}
		else
		{
			OutString.Appendf(TEXT("MakeLWCScalar(%s, %s)"), TileValue.ToString(), OffsetValue.ToString());
		}
	}
	else
	{
		if (Format == EValueStringFormat::HLSL)
		{
			const TCHAR* ComponentName = nullptr;
			switch (ComponentType)
			{
			case EValueComponentType::Int: ComponentName = TEXT("int"); break;
			case EValueComponentType::Bool: ComponentName = TEXT("bool"); break;
			case EValueComponentType::Float: ComponentName = TEXT("float"); break;
			default: checkNoEntry(); break; // Double is handled by above case
			}
			if (NumComponents > 1)
			{
				OutString.Appendf(TEXT("%s%d("), ComponentName, NumComponents);
			}
			else
			{
				OutString.Appendf(TEXT("%s("), ComponentName);
			}
		}

		for (int32 i = 0; i < NumComponents; ++i)
		{
			if (i > 0)
			{
				OutString.Append(TEXT(", "));
			}

			switch (ComponentType)
			{
			case EValueComponentType::Int: OutString.Appendf(TEXT("%d"), Component[i].Int); break;
			case EValueComponentType::Bool: OutString.Append(Component[i].Bool ? TEXT("true") : TEXT("false")); break;
			case EValueComponentType::Float: Private::FormatComponent_Double((double)Component[i].Float, NumComponents, Format, OutString); break;
			case EValueComponentType::Double: Private::FormatComponent_Double(Component[i].Double, NumComponents, Format, OutString); break;
			default: checkNoEntry(); break;
			}
		}

		if (Format == EValueStringFormat::HLSL)
		{
			OutString.Append(TEXT(")"));
		}
	}

	return OutString.ToString();
}

FValueTypeDescription GetValueTypeDescription(EValueType Type)
{
	switch (Type)
	{
	case EValueType::Void: return FValueTypeDescription(TEXT("void"), EValueComponentType::Void, 0);
	case EValueType::Float1: return FValueTypeDescription(TEXT("float"), EValueComponentType::Float, 1);
	case EValueType::Float2: return FValueTypeDescription(TEXT("float2"), EValueComponentType::Float, 2);
	case EValueType::Float3: return FValueTypeDescription(TEXT("float3"), EValueComponentType::Float, 3);
	case EValueType::Float4: return FValueTypeDescription(TEXT("float4"), EValueComponentType::Float, 4);
	case EValueType::Double1: return FValueTypeDescription(TEXT("FLWCScalar"), EValueComponentType::Double, 1);
	case EValueType::Double2: return FValueTypeDescription(TEXT("FLWCVector2"), EValueComponentType::Double, 2);
	case EValueType::Double3: return FValueTypeDescription(TEXT("FLWCVector3"), EValueComponentType::Double, 3);
	case EValueType::Double4: return FValueTypeDescription(TEXT("FLWCVector4"), EValueComponentType::Double, 4);
	case EValueType::Int1: return FValueTypeDescription(TEXT("int"), EValueComponentType::Int, 1);
	case EValueType::Int2: return FValueTypeDescription(TEXT("int2"), EValueComponentType::Int, 2);
	case EValueType::Int3: return FValueTypeDescription(TEXT("int3"), EValueComponentType::Int, 3);
	case EValueType::Int4: return FValueTypeDescription(TEXT("int4"), EValueComponentType::Int, 4);
	case EValueType::Bool1: return FValueTypeDescription(TEXT("bool"), EValueComponentType::Bool, 1);
	case EValueType::Bool2: return FValueTypeDescription(TEXT("bool2"), EValueComponentType::Bool, 2);
	case EValueType::Bool3: return FValueTypeDescription(TEXT("bool3"), EValueComponentType::Bool, 3);
	case EValueType::Bool4: return FValueTypeDescription(TEXT("bool4"), EValueComponentType::Bool, 4);
	case EValueType::Struct: return FValueTypeDescription(TEXT("struct"), EValueComponentType::Void, 0);
	default: checkNoEntry(); return FValueTypeDescription(TEXT("<INVALID>"), EValueComponentType::Void, 0);
	}
}

EValueType MakeValueType(EValueComponentType ComponentType, int32 NumComponents)
{
	if (NumComponents == 0)
	{
		return EValueType::Void;
	}

	switch (ComponentType)
	{
	case EValueComponentType::Float:
		switch (NumComponents)
		{
		case 1: return EValueType::Float1;
		case 2: return EValueType::Float2;
		case 3: return EValueType::Float3;
		case 4: return EValueType::Float4;
		default: break;
		}
	case EValueComponentType::Double:
		switch (NumComponents)
		{
		case 1: return EValueType::Double1;
		case 2: return EValueType::Double2;
		case 3: return EValueType::Double3;
		case 4: return EValueType::Double4;
		default: break;
		}
	case EValueComponentType::Int:
		switch (NumComponents)
		{
		case 1: return EValueType::Int1;
		case 2: return EValueType::Int2;
		case 3: return EValueType::Int3;
		case 4: return EValueType::Int4;
		default: break;
		}
	case EValueComponentType::Bool:
		switch (NumComponents)
		{
		case 1: return EValueType::Bool1;
		case 2: return EValueType::Bool2;
		case 3: return EValueType::Bool3;
		case 4: return EValueType::Bool4;
		default: break;
		}
	default:
		break;
	}

	checkNoEntry();
	return EValueType::Void;
}

EValueType MakeValueType(EValueType BaseType, int32 NumComponents)
{
	return MakeValueType(GetValueTypeDescription(BaseType).ComponentType, NumComponents);
}

EValueType MakeValueTypeWithRequestedNumComponents(EValueType BaseType, int8 RequestedNumComponents)
{
	const FValueTypeDescription TypeDesc = GetValueTypeDescription(BaseType);
	return MakeValueType(TypeDesc.ComponentType, FMath::Min(TypeDesc.NumComponents, RequestedNumComponents));
}

EValueType MakeArithmeticResultType(EValueType Lhs, EValueType Rhs, FString& OutErrorMessage)
{
	const FValueTypeDescription LhsDesc = GetValueTypeDescription(Lhs);
	const FValueTypeDescription RhsDesc = GetValueTypeDescription(Rhs);
	// Types with 0 components are non-arithmetic
	if (LhsDesc.NumComponents > 0 && RhsDesc.NumComponents > 0)
	{
		if (Lhs == Rhs)
		{
			return Lhs;
		}

		EValueComponentType ComponentType = EValueComponentType::Void;
		if (LhsDesc.ComponentType == RhsDesc.ComponentType)
		{
			ComponentType = LhsDesc.ComponentType;
		}
		else if (LhsDesc.ComponentType == EValueComponentType::Double || RhsDesc.ComponentType == EValueComponentType::Double)
		{
			ComponentType = EValueComponentType::Double;
		}
		else if (LhsDesc.ComponentType == EValueComponentType::Float || RhsDesc.ComponentType == EValueComponentType::Float)
		{
			ComponentType = EValueComponentType::Float;
		}
		else
		{
			ComponentType = EValueComponentType::Int;
		}

		if (ComponentType != EValueComponentType::Void)
		{
			if (LhsDesc.NumComponents == 1 || RhsDesc.NumComponents == 1)
			{
				// single component type is valid to combine with other type
				return MakeValueType(ComponentType, FMath::Max(LhsDesc.NumComponents, RhsDesc.NumComponents));
			}
			else if (LhsDesc.NumComponents == RhsDesc.NumComponents)
			{
				return MakeValueType(ComponentType, LhsDesc.NumComponents);
			}
		}

		OutErrorMessage = FString::Printf(TEXT("Arithmetic between types %s and %s are undefined"), LhsDesc.Name, RhsDesc.Name);
	}
	else
	{
		OutErrorMessage = FString::Printf(TEXT("Attempting to perform arithmetic on non-numeric types: %s %s"), LhsDesc.Name, RhsDesc.Name);
	}

	return EValueType::Void;
}

EValueType MakeComparisonResultType(EValueType Lhs, EValueType Rhs, FString& OutErrorMessage)
{
	const FValueTypeDescription LhsDesc = GetValueTypeDescription(Lhs);
	const FValueTypeDescription RhsDesc = GetValueTypeDescription(Rhs);

	if (Lhs == Rhs)
	{
		if (LhsDesc.NumComponents > 0)
		{
			return MakeValueType(EValueComponentType::Bool, LhsDesc.NumComponents);
		}
		else
		{
			OutErrorMessage = FString::Printf(TEXT("Attempting to perform comparison on non-numeric types: %s %s"), LhsDesc.Name, RhsDesc.Name);
		}
	}
	else
	{
		OutErrorMessage = FString::Printf(TEXT("Comparison between types %s and %s are undefined"), LhsDesc.Name, RhsDesc.Name);
	}

	return EValueType::Void;
}

namespace Private
{

/** Converts an arbitrary number into a safe divisor. i.e. FMath::Abs(Number) >= DELTA */
/**
 * FORCENOINLINE is required to discourage compiler from vectorizing the Div operation, which may tempt it into optimizing divide as A * rcp(B)
 * This will break shaders that are depending on exact divide results (see SubUV material function)
 * Technically this could still happen for a scalar divide, but it doesn't seem to occur in practice
 */
template<typename T>
FORCENOINLINE T GetSafeDivisor(T Number)
{
	if (FMath::Abs(Number) < (T)DELTA)
	{
		if (Number < 0)
		{
			return -(T)DELTA;
		}
		else
		{
			return (T)DELTA;
		}
	}
	else
	{
		return Number;
	}
}

template<>
FORCENOINLINE int32 GetSafeDivisor<int32>(int32 Number)
{
	return Number != 0 ? Number : 1;
}

struct FOpBase
{
	static constexpr bool SupportsDouble = true;
	static constexpr bool SupportsInt = true;
};

struct FOpBaseNoInt : public FOpBase
{
	static constexpr bool SupportsInt = false;
};

struct FOpAbs : public FOpBase { template<typename T> T operator()(T Value) const { return FMath::Abs(Value); } };
struct FOpSign : public FOpBase { template<typename T> T operator()(T Value) const { return FMath::Sign(Value); } };
struct FOpSaturate : public FOpBaseNoInt { template<typename T> T operator()(T Value) const { return FMath::Clamp(Value, (T)0, (T)1); } };
struct FOpFloor : public FOpBaseNoInt { template<typename T> T operator()(T Value) const { return FMath::FloorToFloat(Value); } };
struct FOpCeil : public FOpBaseNoInt { template<typename T> T operator()(T Value) const { return FMath::CeilToFloat(Value); } };
struct FOpRound : public FOpBaseNoInt { template<typename T> T operator()(T Value) const { return FMath::RoundToFloat(Value); } };
struct FOpTrunc : public FOpBaseNoInt { template<typename T> T operator()(T Value) const { return FMath::TruncToFloat(Value); } };
struct FOpFrac : public FOpBaseNoInt { template<typename T> T operator()(T Value) const { return FMath::Frac(Value); } };
struct FOpFractional : public FOpBaseNoInt { template<typename T> T operator()(T Value) const { return FMath::Fractional(Value); } };
struct FOpSqrt : public FOpBaseNoInt { template<typename T> T operator()(T Value) const { return FMath::Sqrt(Value); } };
struct FOpRcp : public FOpBaseNoInt { template<typename T> T operator()(T Value) const { return (T)1 / GetSafeDivisor(Value); } };
struct FOpLog2 : public FOpBaseNoInt { template<typename T> T operator()(T Value) const { return FMath::Log2(Value); } };
struct FOpLog10 : public FOpBaseNoInt
{
	template<typename T> T operator()(T Value) const
	{
		static const T LogToLog10 = (T)1.0 / FMath::Loge((T)10);
		return FMath::Loge(Value) * LogToLog10;
	}
};
struct FOpSin : public FOpBaseNoInt { template<typename T> T operator()(T Value) const { return FMath::Sin(Value); } };
struct FOpCos : public FOpBaseNoInt { template<typename T> T operator()(T Value) const { return FMath::Cos(Value); } };
struct FOpTan : public FOpBaseNoInt { template<typename T> T operator()(T Value) const { return FMath::Tan(Value); } };
struct FOpAsin : public FOpBaseNoInt { template<typename T> T operator()(T Value) const { return FMath::Asin(Value); } };
struct FOpAcos : public FOpBaseNoInt { template<typename T> T operator()(T Value) const { return FMath::Acos(Value); } };
struct FOpAtan : public FOpBaseNoInt { template<typename T> T operator()(T Value) const { return FMath::Atan(Value); } };
struct FOpAdd : public FOpBase { template<typename T> T operator()(T Lhs, T Rhs) const { return Lhs + Rhs; } };
struct FOpSub : public FOpBase { template<typename T> T operator()(T Lhs, T Rhs) const { return Lhs - Rhs; } };
struct FOpMul : public FOpBase { template<typename T> T operator()(T Lhs, T Rhs) const { return Lhs * Rhs; } };
struct FOpDiv : public FOpBase { template<typename T> T operator()(T Lhs, T Rhs) const { return Lhs / GetSafeDivisor(Rhs); } };
struct FOpMin : public FOpBase { template<typename T> T operator()(T Lhs, T Rhs) const { return FMath::Min(Lhs, Rhs); } };
struct FOpMax : public FOpBase { template<typename T> T operator()(T Lhs, T Rhs) const { return FMath::Max(Lhs, Rhs); } };
struct FOpFmod : public FOpBaseNoInt { template<typename T> T operator()(T Lhs, T Rhs) const { return FMath::Fmod(Lhs, Rhs); } };
struct FOpAtan2 : public FOpBaseNoInt { template<typename T> T operator()(T Lhs, T Rhs) const { return FMath::Atan2(Lhs, Rhs); } };

template<typename Operation>
inline FValue UnaryOp(const Operation& Op, const FValue& Value)
{
	const int8 NumComponents = Value.NumComponents;
	
	FValue Result;
	Result.NumComponents = NumComponents;

	if constexpr (Operation::SupportsDouble)
	{
		if (Value.ComponentType == EValueComponentType::Double)
		{
			Result.ComponentType = EValueComponentType::Double;
			const FDoubleValue Cast = Value.AsDouble();
			for (int32 i = 0; i < NumComponents; ++i)
			{
				Result.Component[i].Double = Op(Cast.Component[i]);
			}
			return Result;
		}
	}

	if constexpr (Operation::SupportsInt)
	{
		if (Value.ComponentType != EValueComponentType::Float)
		{
			Result.ComponentType = EValueComponentType::Int;
			const FIntValue Cast = Value.AsInt();
			for (int32 i = 0; i < NumComponents; ++i)
			{
				Result.Component[i].Int = Op(Cast.Component[i]);
			}
			return Result;
		}
	}

	Result.ComponentType = EValueComponentType::Float;
	const FFloatValue Cast = Value.AsFloat();
	for (int32 i = 0; i < NumComponents; ++i)
	{
		Result.Component[i].Float = Op(Cast.Component[i]);
	}
	return Result;
}

inline int8 GetNumComponentsResult(int8 Lhs, int8 Rhs)
{
	// operations between scalar and non-scalar will splat the scalar value
	// otherwise, operations should only be between types with same number of components
	return (Lhs == 1 || Rhs == 1) ? FMath::Max(Lhs, Rhs) : FMath::Min(Lhs, Rhs);
}

template<typename Operation>
inline FValue BinaryOp(const Operation& Op, const FValue& Lhs, const FValue& Rhs)
{
	const int8 NumComponents = GetNumComponentsResult(Lhs.NumComponents, Rhs.NumComponents);
	
	FValue Result;
	Result.NumComponents = NumComponents;

	if constexpr (Operation::SupportsDouble)
	{
		if (Lhs.ComponentType == EValueComponentType::Double || Rhs.ComponentType == EValueComponentType::Double)
		{
			Result.ComponentType = EValueComponentType::Double;
			const FDoubleValue LhsCast = Lhs.AsDouble();
			const FDoubleValue RhsCast = Rhs.AsDouble();
			for (int32 i = 0; i < NumComponents; ++i)
			{
				Result.Component[i].Double = Op(LhsCast.Component[i], RhsCast.Component[i]);
			}
			return Result;
		}
	}

	if constexpr (Operation::SupportsInt)
	{
		if (Lhs.ComponentType != EValueComponentType::Float && Rhs.ComponentType != EValueComponentType::Float)
		{
			Result.ComponentType = EValueComponentType::Int;
			const FIntValue LhsCast = Lhs.AsInt();
			const FIntValue RhsCast = Rhs.AsInt();
			for (int32 i = 0; i < NumComponents; ++i)
			{
				Result.Component[i].Int = Op(LhsCast.Component[i], RhsCast.Component[i]);
			}
			return Result;
		}
	}

	Result.ComponentType = EValueComponentType::Float;
	const FFloatValue LhsCast = Lhs.AsFloat();
	const FFloatValue RhsCast = Rhs.AsFloat();
	for (int32 i = 0; i < NumComponents; ++i)
	{
		Result.Component[i].Float = Op(LhsCast.Component[i], RhsCast.Component[i]);
	}
	return Result;
}

} // namespace Private

bool operator==(const FValue& Lhs, const FValue& Rhs)
{
	if (Lhs.ComponentType != Rhs.ComponentType || Lhs.NumComponents != Rhs.NumComponents)
	{
		return false;
	}
	for (int32 i = 0u; i < Lhs.NumComponents; ++i)
	{
		if (Lhs.Component[i].Packed != Rhs.Component[i].Packed)
		{
			return false;
		}
	}
	return true;
}

uint32 GetTypeHash(const FValue& Value)
{
	uint32 Result = ::GetTypeHash(Value.ComponentType);
	Result = HashCombine(Result, ::GetTypeHash(Value.NumComponents));
	for (int32 i = 0u; i < Value.NumComponents; ++i)
	{
		const FValueComponent& Component = Value.Component[i];
		uint32 ComponentHash = 0u;
		switch (Value.ComponentType)
		{
		case EValueComponentType::Float: ComponentHash = ::GetTypeHash(Component.Float); break;
		case EValueComponentType::Double: ComponentHash = ::GetTypeHash(Component.Double); break;
		case EValueComponentType::Int: ComponentHash = ::GetTypeHash(Component.Int); break;
		case EValueComponentType::Bool: ComponentHash = ::GetTypeHash(Component.Bool); break;
		default: checkNoEntry(); break;
		}
		Result = HashCombine(Result, ComponentHash);
	}
	return Result;
}

FValue Abs(const FValue& Value)
{
	return Private::UnaryOp(Private::FOpAbs(), Value);
}

FValue Saturate(const FValue& Value)
{
	return Private::UnaryOp(Private::FOpSaturate(), Value);
}

FValue Floor(const FValue& Value)
{
	return Private::UnaryOp(Private::FOpFloor(), Value);
}

FValue Ceil(const FValue& Value)
{
	return Private::UnaryOp(Private::FOpCeil(), Value);
}

FValue Round(const FValue& Value)
{
	return Private::UnaryOp(Private::FOpRound(), Value);
}

FValue Trunc(const FValue& Value)
{
	return Private::UnaryOp(Private::FOpTrunc(), Value);
}

FValue Sign(const FValue& Value)
{
	return Private::UnaryOp(Private::FOpSign(), Value);
}

FValue Frac(const FValue& Value)
{
	return Private::UnaryOp(Private::FOpFrac(), Value);
}

FValue Fractional(const FValue& Value)
{
	return Private::UnaryOp(Private::FOpFractional(), Value);
}

FValue Sqrt(const FValue& Value)
{
	return Private::UnaryOp(Private::FOpSqrt(), Value);
}

FValue Rcp(const FValue& Value)
{
	return Private::UnaryOp(Private::FOpRcp(), Value);
}

FValue Log2(const FValue& Value)
{
	return Private::UnaryOp(Private::FOpLog2(), Value);
}

FValue Log10(const FValue& Value)
{
	return Private::UnaryOp(Private::FOpLog10(), Value);
}

FValue Sin(const FValue& Value)
{
	return Private::UnaryOp(Private::FOpSin(), Value);
}

FValue Cos(const FValue& Value)
{
	return Private::UnaryOp(Private::FOpCos(), Value);
}

FValue Tan(const FValue& Value)
{
	return Private::UnaryOp(Private::FOpTan(), Value);
}

FValue Asin(const FValue& Value)
{
	return Private::UnaryOp(Private::FOpAsin(), Value);
}

FValue Acos(const FValue& Value)
{
	return Private::UnaryOp(Private::FOpAcos(), Value);
}

FValue Atan(const FValue& Value)
{
	return Private::UnaryOp(Private::FOpAtan(), Value);
}

FValue Add(const FValue& Lhs, const FValue& Rhs)
{
	return Private::BinaryOp(Private::FOpAdd(), Lhs, Rhs);
}

FValue Sub(const FValue& Lhs, const FValue& Rhs)
{
	return Private::BinaryOp(Private::FOpSub(), Lhs, Rhs);
}

FValue Mul(const FValue& Lhs, const FValue& Rhs)
{
	return Private::BinaryOp(Private::FOpMul(), Lhs, Rhs);
}

FValue Div(const FValue& Lhs, const FValue& Rhs)
{
	return Private::BinaryOp(Private::FOpDiv(), Lhs, Rhs);
}

FValue Min(const FValue& Lhs, const FValue& Rhs)
{
	return Private::BinaryOp(Private::FOpMin(), Lhs, Rhs);
}

FValue Max(const FValue& Lhs, const FValue& Rhs)
{
	return Private::BinaryOp(Private::FOpMax(), Lhs, Rhs);
}

FValue Fmod(const FValue& Lhs, const FValue& Rhs)
{
	return Private::BinaryOp(Private::FOpFmod(), Lhs, Rhs);
}

FValue Atan2(const FValue& Lhs, const FValue& Rhs)
{
	return Private::BinaryOp(Private::FOpAtan2(), Lhs, Rhs);
}

FValue Clamp(const FValue& Value, const FValue& Low, const FValue& High)
{
	return Min(Max(Value, Low), High);
}

FValue Dot(const FValue& Lhs, const FValue& Rhs)
{
	const int8 NumComponents = Private::GetNumComponentsResult(Lhs.NumComponents, Rhs.NumComponents);

	FValue Result;
	Result.NumComponents = 1;

	if (Lhs.ComponentType == EValueComponentType::Double || Rhs.ComponentType == EValueComponentType::Double)
	{
		Result.ComponentType = EValueComponentType::Double;
		const FDoubleValue LhsValue = Lhs.AsDouble();
		const FDoubleValue RhsValue = Rhs.AsDouble();
		double ComponentValue = 0.0;
		for (int32 i = 0; i < NumComponents; ++i)
		{
			ComponentValue += LhsValue.Component[i] * RhsValue.Component[i];
		}
		Result.Component[0].Double = ComponentValue;
	}
	else if (Lhs.ComponentType == EValueComponentType::Float || Rhs.ComponentType == EValueComponentType::Float)
	{
		Result.ComponentType = EValueComponentType::Float;
		const FFloatValue LhsValue = Lhs.AsFloat();
		const FFloatValue RhsValue = Rhs.AsFloat();
		float ComponentValue = 0.0f;
		for (int32 i = 0; i < NumComponents; ++i)
		{
			ComponentValue += LhsValue.Component[i] * RhsValue.Component[i];
		}
		Result.Component[0].Float = ComponentValue;
	}
	else
	{
		Result.ComponentType = EValueComponentType::Int;
		const FIntValue LhsValue = Lhs.AsInt();
		const FIntValue RhsValue = Rhs.AsInt();
		int32 ComponentValue = 0;
		for (int32 i = 0; i < NumComponents; ++i)
		{
			ComponentValue += LhsValue.Component[i] * RhsValue.Component[i];
		}
		Result.Component[0].Int = ComponentValue;
	}
	return Result;
}

FValue Cross(const FValue& Lhs, const FValue& Rhs)
{
	FValue Result;
	Result.NumComponents = 3;

	if (Lhs.ComponentType == EValueComponentType::Double || Rhs.ComponentType == EValueComponentType::Double)
	{
		Result.ComponentType = EValueComponentType::Double;
		const FDoubleValue LhsValue = Lhs.AsDouble();
		const FDoubleValue RhsValue = Rhs.AsDouble();

		Result.Component[0].Double = LhsValue.Component[1] * RhsValue.Component[2] - LhsValue.Component[2] * RhsValue.Component[1];
		Result.Component[1].Double = LhsValue.Component[2] * RhsValue.Component[0] - LhsValue.Component[0] * RhsValue.Component[2];
		Result.Component[2].Double = LhsValue.Component[0] * RhsValue.Component[1] - LhsValue.Component[1] * RhsValue.Component[0];
	}
	else if (Lhs.ComponentType == EValueComponentType::Float || Rhs.ComponentType == EValueComponentType::Float)
	{
		Result.ComponentType = EValueComponentType::Float;
		const FFloatValue LhsValue = Lhs.AsFloat();
		const FFloatValue RhsValue = Rhs.AsFloat();
		
		Result.Component[0].Float = LhsValue.Component[1] * RhsValue.Component[2] - LhsValue.Component[2] * RhsValue.Component[1];
		Result.Component[1].Float = LhsValue.Component[2] * RhsValue.Component[0] - LhsValue.Component[0] * RhsValue.Component[2];
		Result.Component[2].Float = LhsValue.Component[0] * RhsValue.Component[1] - LhsValue.Component[1] * RhsValue.Component[0];
	}
	else
	{
		Result.ComponentType = EValueComponentType::Int;
		const FIntValue LhsValue = Lhs.AsInt();
		const FIntValue RhsValue = Rhs.AsInt();

		Result.Component[0].Int = LhsValue.Component[1] * RhsValue.Component[2] - LhsValue.Component[2] * RhsValue.Component[1];
		Result.Component[1].Int = LhsValue.Component[2] * RhsValue.Component[0] - LhsValue.Component[0] * RhsValue.Component[2];
		Result.Component[2].Int = LhsValue.Component[0] * RhsValue.Component[1] - LhsValue.Component[1] * RhsValue.Component[0];
	}
	return Result;
}

FValue Append(const FValue& Lhs, const FValue& Rhs)
{
	FValue Result;
	int32 NumComponents = 0;
	if (Lhs.ComponentType == Rhs.ComponentType)
	{
		// If both values have the same component type, use as-is
		// (otherwise will need to convert)
		Result.ComponentType = Lhs.ComponentType;
		for (int32 i = 0; i < Lhs.NumComponents; ++i)
		{
			Result.Component[NumComponents++] = Lhs.Component[i];
		}
		for (int32 i = 0; i < Rhs.NumComponents && NumComponents < 4; ++i)
		{
			Result.Component[NumComponents++] = Rhs.Component[i];
		}
	}
	else if (Lhs.ComponentType == EValueComponentType::Double || Rhs.ComponentType == EValueComponentType::Double)
	{
		Result.ComponentType = EValueComponentType::Double;
		const FDoubleValue LhsValue = Lhs.AsDouble();
		const FDoubleValue RhsValue = Rhs.AsDouble();
		for (int32 i = 0; i < Lhs.NumComponents; ++i)
		{
			Result.Component[NumComponents++].Double = LhsValue.Component[i];
		}
		for (int32 i = 0; i < Rhs.NumComponents && NumComponents < 4; ++i)
		{
			Result.Component[NumComponents++].Double = RhsValue.Component[i];
		}
	}
	else if (Lhs.ComponentType == EValueComponentType::Float || Rhs.ComponentType == EValueComponentType::Float)
	{
		Result.ComponentType = EValueComponentType::Float;
		const FFloatValue LhsValue = Lhs.AsFloat();
		const FFloatValue RhsValue = Rhs.AsFloat();
		for (int32 i = 0; i < Lhs.NumComponents; ++i)
		{
			Result.Component[NumComponents++].Float = LhsValue.Component[i];
		}
		for (int32 i = 0; i < Rhs.NumComponents && NumComponents < 4; ++i)
		{
			Result.Component[NumComponents++].Float = RhsValue.Component[i];
		}
	}
	else
	{
		Result.ComponentType = EValueComponentType::Int;
		const FIntValue LhsValue = Lhs.AsInt();
		const FIntValue RhsValue = Rhs.AsInt();
		for (int32 i = 0; i < Lhs.NumComponents; ++i)
		{
			Result.Component[NumComponents++].Int = LhsValue.Component[i];
		}
		for (int32 i = 0; i < Rhs.NumComponents && NumComponents < 4; ++i)
		{
			Result.Component[NumComponents++].Int = RhsValue.Component[i];
		}
	}
	Result.NumComponents = NumComponents;
	return Result;
}

FValue Cast(const FValue& Value, EValueType Type)
{
	const EValueType SourceType = Value.GetType();
	if (Type == SourceType)
	{
		return Value;
	}

	FValue Result(Type);
	switch (Result.ComponentType)
	{
	case EValueComponentType::Float: Private::Cast(Private::FCastFloat(), Value, Result); break;
	case EValueComponentType::Double: Private::Cast(Private::FCastDouble(), Value, Result); break;
	case EValueComponentType::Int: Private::Cast(Private::FCastInt(), Value, Result); break;
	case EValueComponentType::Bool: Private::Cast(Private::FCastBool(), Value, Result); break;
	default: checkNoEntry(); break;
	}

	return Result;
}

} // namespace Shader
} // namespace UE
