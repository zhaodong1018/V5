// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MassArchetypeTypes.h"
#include "InstancedStruct.h"
#include "MassEntityView.generated.h"


class UMassEntitySubsystem;
struct FMassArchetypeData;
struct FArchetypeHandle;
struct FMassArchetypeFragmentConfig;

/** 
 * The type representing a single entity in a single archetype. It's of a very transient nature so we guarantee it's 
 * validity only within the scope it has been created in. Don't store it. 
 */
USTRUCT()
struct MASSENTITY_API FMassEntityView
{
	GENERATED_BODY()

	FMassEntityView() = default;

	/** 
	 *  Resolves Entity against ArchetypeHandle. Note that this approach requires the caller to ensure that Entity
	 *  indeed belongs to ArchetypeHandle. If not the call will fail a check. As a remedy calling the 
	 *  UMassEntitySubsystem-flavored constructor is recommended since it will first find the appropriate archetype for
	 *  Entity. 
	 */
	FMassEntityView(const FArchetypeHandle& ArchetypeHandle, FMassEntityHandle Entity);

	/** 
	 *  Finds the archetype Entity belongs to and then resolves against it. The caller is responsible for ensuring
	 *  that the given Entity is in fact a valid ID tied to any of the archetypes 
	 */
	FMassEntityView(const UMassEntitySubsystem& EntitySubsystem, FMassEntityHandle Entity);

	FMassEntityHandle GetEntity() const	{ return Entity; }

	/** will fail a check if the viewed entity doesn't have the given fragment */	
	template<typename T>
	T& GetFragmentData() const
	{
		static_assert(!TIsDerivedFrom<T, FMassTag>::IsDerived,
			"Given struct doesn't represent a valid fragment type but a tag. Use HasTag instead.");
		static_assert(TIsDerivedFrom<T, FMassTag>::IsDerived || TIsDerivedFrom<T, FMassFragment>::IsDerived,
			"Given struct doesn't represent a valid fragment type. Make sure to inherit from FMassFragment or one of its child-types.");

		return *((T*)GetFragmentPtrChecked(*T::StaticStruct()));
	}
		
	/** if the viewed entity doesn't have the given fragment the function will return null */
	template<typename T>
	T* GetFragmentDataPtr() const
	{
		static_assert(!TIsDerivedFrom<T, FMassTag>::IsDerived,
			"Given struct doesn't represent a valid fragment type but a tag. Use HasTag instead.");
		static_assert(TIsDerivedFrom<T, FMassTag>::IsDerived || TIsDerivedFrom<T, FMassFragment>::IsDerived,
			"Given struct doesn't represent a valid fragment type. Make sure to inherit from FMassFragment or one of its child-types.");

		return (T*)GetFragmentPtr(*T::StaticStruct());
	}

	FStructView GetFragmentDataStruct(const UScriptStruct* FragmentType) const
	{
		check(FragmentType);
		return FStructView(FragmentType, static_cast<uint8*>(GetFragmentPtr(*FragmentType)));
	}

	template<typename T>
	bool HasTag() const
	{
		static_assert(TIsDerivedFrom<T, FMassTag>::IsDerived, "Given struct doesn't represent a valid tag type. Make sure to inherit from FMassTag or one of its child-types.");
		return HasTag(*T::StaticStruct());
	}

	bool IsSet() const { return Archetype != nullptr && EntityHandle.IsValid(); }
	bool operator==(const FMassEntityView& Other) const { return Archetype == Other.Archetype && EntityHandle == Other.EntityHandle; }

protected:
	void* GetFragmentPtr(const UScriptStruct& FragmentType) const;
	void* GetFragmentPtrChecked(const UScriptStruct& FragmentType) const;
	bool HasTag(const UScriptStruct& TagType) const;

private:
	FMassEntityHandle Entity;
	FInternalEntityHandle EntityHandle;
	FMassArchetypeData* Archetype = nullptr;
};
