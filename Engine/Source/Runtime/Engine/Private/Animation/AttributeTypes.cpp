// Copyright Epic Games, Inc. All Rights Reserved.

#include "Animation/AttributeTypes.h"
#include "Animation/BuiltInAttributeTypes.h"
#include "Misc/DelayedAutoRegister.h"

namespace UE
{
	namespace Anim
	{		
		TArray<TWeakObjectPtr<const UScriptStruct>> AttributeTypes::RegisteredTypes;
		TArray<TUniquePtr<IAttributeBlendOperator>> AttributeTypes::Operators;
		TArray<TWeakObjectPtr<const UScriptStruct>> AttributeTypes::InterpolatableTypes;

		void AttributeTypes::Initialize()
		{
			static bool bInitialized = false;
			checkf(bInitialized == false, TEXT("Trying to initialize attribute type system multiple times"));
			
			bInitialized = true;
			RegisterType<FFloatAnimationAttribute>();
			RegisterType<FIntegerAnimationAttribute>();
			RegisterType<FStringAnimationAttribute>();
			RegisterType<FTransformAnimationAttribute>();
		}
		
		static FDelayedAutoRegisterHelper DelayedAttributeTypesInitializationHelper(EDelayedRegisterRunPhase::ObjectSystemReady, []()
		{
			UE::Anim::AttributeTypes::Initialize();
		});
	}
}

