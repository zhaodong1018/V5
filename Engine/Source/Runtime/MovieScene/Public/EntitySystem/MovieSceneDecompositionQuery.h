// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "EntitySystem/MovieSceneEntityIDs.h"
#include "Containers/ArrayView.h"
#include "Containers/Array.h"
#include "Templates/Tuple.h"
#include "Templates/TypeCompatibleBytes.h"
#include "Async/TaskGraphInterfaces.h"

#include "UObject/Interface.h"

#include "MovieSceneDecompositionQuery.generated.h"

namespace UE
{
namespace MovieScene
{

	/**
	 * Structure used to decompose the blended result of multiple components.
	 *
	 * Defines an object to query, and the entities that should have their pre-blended component values extracted
	 */
	struct FDecompositionQuery
	{
		/** Defines all the entities that should have their pre-component values extracted for recomposition */
		TArrayView<const FMovieSceneEntityID> Entities;

		/** Whether the entities above are source entities or runtime entities */
		bool bConvertFromSourceEntityIDs = true;

		/** The object that is being decomposed */
		UObject* Object = nullptr;
	};


	/** Used for decomposing how a final blended value was blended */
	struct FWeightedValue
	{
		double Value = 0.f;
		float Weight = 0.f;

		double WeightedValue() const
		{
			return Weight != 0.f ? Value / Weight : 0.f;
		}

		FWeightedValue Combine(FWeightedValue Other) const
		{
			return FWeightedValue{Value + Other.Value, Weight + Other.Weight};
		}

		FWeightedValue CombineWeighted(FWeightedValue Other) const
		{
			return FWeightedValue{Value + Other.Value * Other.Weight, Weight + Other.Weight};
		}
	};

	struct FDecomposedValue
	{
		struct FResult
		{
			FWeightedValue Absolute;
			double Additive = 0.f;
		};

		FResult Result;

		TArray<TTuple<FMovieSceneEntityID, FWeightedValue>> DecomposedAbsolutes;
		TArray<TTuple<FMovieSceneEntityID, FWeightedValue>> DecomposedAdditives;

		MOVIESCENE_API float Recompose(FMovieSceneEntityID EntityID, float CurrentValue, const float* InitialValue) const;
		MOVIESCENE_API double Recompose(FMovieSceneEntityID EntityID, double CurrentValue, const double* InitialValue) const;
		MOVIESCENE_API void Decompose(FMovieSceneEntityID EntityID, FWeightedValue& ThisValue, bool& bOutIsAdditive, FWeightedValue& Absolutes, FWeightedValue& Additives) const;
	};

	// Align results to cache lines so there's no contention between cores
	struct MS_ALIGN(PLATFORM_CACHE_LINE_SIZE) FAlignedDecomposedValue
	{
		FDecomposedValue Value;
	} GCC_ALIGN(PLATFORM_CACHE_LINE_SIZE);

	struct FValueDecompositionParams
	{
		FDecompositionQuery Query;
		uint16 DecomposeBlendChannel;
		FMovieSceneEntityID PropertyEntityID;
		FComponentTypeID ResultComponentType;
		FComponentTypeID PropertyTag;
	};

	template<typename PropertyType>
	struct TRecompositionResult
	{
		TRecompositionResult(const PropertyType& InCurrentValue, int32 Num)
		{
			while (--Num >= 0)
			{
				Values.Add(InCurrentValue);
			}
		}

		TArray<PropertyType, TInlineAllocator<1>> Values;
	};

} // namespace MovieScene
} // namespace UE


UINTERFACE()
class MOVIESCENE_API UMovieSceneValueDecomposer : public UInterface
{
public:
	GENERATED_BODY()
};

class IMovieSceneValueDecomposer
{
public:
	GENERATED_BODY()

	virtual FGraphEventRef DispatchDecomposeTask(const UE::MovieScene::FValueDecompositionParams& Params, UE::MovieScene::FAlignedDecomposedValue* Output) = 0;
};
