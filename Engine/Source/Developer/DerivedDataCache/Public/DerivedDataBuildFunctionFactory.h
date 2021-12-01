// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "DerivedDataBuildFunction.h"
#include "Features/IModularFeatures.h"
#include "UObject/NameTypes.h"

namespace UE::DerivedData
{

/** DO NOT USE DIRECTLY. Base for the build function factory. Use TBuildFunctionFactory. */
class IBuildFunctionFactory : public IModularFeature
{
public:
	/** Returns the build function associated with this factory. */
	virtual const IBuildFunction& GetFunction() const = 0;

	/** Returns the name of the build function factory modular feature. */
	static FName GetFeatureName()
	{
		return FName("BuildFunctionFactory");
	}
};

/**
 * Factory that creates and registers a build function.
 *
 * A build function must be registered by a build function factory before it can execute a build.
 * Register a function in the source file that implements it or in the corresponding module.
 *
 * Examples:
 * static const TBuildFunctionFactory<FExampleFunction> ExampleFunctionFactory;
 * static const TBuildFunctionFactory<TExampleFunction<FType>> ExampleFunctionFactory(Name, Version);
 */
template <typename FunctionType>
class TBuildFunctionFactory final : public IBuildFunctionFactory
{
	static_assert(sizeof(FunctionType) == sizeof(IBuildFunction), "IBuildFunction must be pure and maintain no state.");

public:
	template <typename... ArgTypes>
	explicit TBuildFunctionFactory(ArgTypes&&... Args)
		: Function(Forward<ArgTypes>(Args)...)
	{
		IModularFeatures::Get().RegisterModularFeature(GetFeatureName(), this);
	}

	~TBuildFunctionFactory()
	{
		IModularFeatures::Get().UnregisterModularFeature(GetFeatureName(), this);
	}

	const IBuildFunction& GetFunction() const final
	{
		return Function;
	}

private:
	const FunctionType Function;
};

} // UE::DerivedData
