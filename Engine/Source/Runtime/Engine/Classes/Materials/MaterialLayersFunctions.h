// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetData.h"
#include "MaterialLayersFunctions.generated.h"

#define LOCTEXT_NAMESPACE "MaterialLayersFunctions"

class FArchive;


UENUM()
enum EMaterialParameterAssociation
{
	LayerParameter,
	BlendParameter,
	GlobalParameter,
};

USTRUCT(BlueprintType)
struct ENGINE_API FMaterialParameterInfo
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=ParameterInfo)
	FName Name;

	/** Whether this is a global parameter, or part of a layer or blend */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=ParameterInfo)
	TEnumAsByte<EMaterialParameterAssociation> Association;

	/** Layer or blend index this parameter is part of. INDEX_NONE for global parameters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=ParameterInfo)
	int32 Index;

	FMaterialParameterInfo(const TCHAR* InName, EMaterialParameterAssociation InAssociation = EMaterialParameterAssociation::GlobalParameter, int32 InIndex = INDEX_NONE)
		: Name(InName)
		, Association(InAssociation)
		, Index(InIndex)
	{
	}
	FMaterialParameterInfo(FName InName = FName(), EMaterialParameterAssociation InAssociation = EMaterialParameterAssociation::GlobalParameter, int32 InIndex = INDEX_NONE)
	: Name(InName)
	, Association(InAssociation)
	, Index(InIndex)
	{
	}

	explicit FMaterialParameterInfo(const struct FMemoryImageMaterialParameterInfo& Rhs);

	FString ToString() const
	{
		return *Name.ToString() + FString::FromInt(Association) + FString::FromInt(Index);
	}

	friend FArchive& operator<<(FArchive& Ar, FMaterialParameterInfo& Ref)
	{
		Ar << Ref.Name << Ref.Association << Ref.Index;
		return Ar;
	}

	bool RemapLayerIndex(TArrayView<const int32> IndexRemap, FMaterialParameterInfo& OutResult) const;
};

struct FMemoryImageMaterialParameterInfo
{
	DECLARE_TYPE_LAYOUT(FMemoryImageMaterialParameterInfo, NonVirtual);
public:
	FMemoryImageMaterialParameterInfo(const TCHAR* InName, EMaterialParameterAssociation InAssociation = EMaterialParameterAssociation::GlobalParameter, int32 InIndex = INDEX_NONE)
		: Name(NameToScriptName(FName(InName)))
		, Index(InIndex)
		, Association(InAssociation)
	{}

	FMemoryImageMaterialParameterInfo(const FName& InName, EMaterialParameterAssociation InAssociation = EMaterialParameterAssociation::GlobalParameter, int32 InIndex = INDEX_NONE)
		: Name(NameToScriptName(InName))
		, Index(InIndex)
		, Association(InAssociation)
	{}

	FMemoryImageMaterialParameterInfo(const FScriptName& InName = FScriptName(), EMaterialParameterAssociation InAssociation = EMaterialParameterAssociation::GlobalParameter, int32 InIndex = INDEX_NONE)
		: Name(InName)
		, Index(InIndex)
		, Association(InAssociation)
	{}

	FMemoryImageMaterialParameterInfo(const FMaterialParameterInfo& Rhs)
		: Name(NameToScriptName(Rhs.Name))
		, Index(Rhs.Index)
		, Association(Rhs.Association)
	{}

	FMemoryImageMaterialParameterInfo(const FMemoryImageMaterialParameterInfo& Rhs) = default;

	FORCEINLINE FName GetName() const { return ScriptNameToName(Name); }

	friend FArchive& operator<<(FArchive& Ar, FMemoryImageMaterialParameterInfo& Ref)
	{
		FName RefName = ScriptNameToName(Ref.Name);
		Ar << RefName << Ref.Association << Ref.Index;
		Ref.Name = NameToScriptName(RefName);
		return Ar;
	}

	bool RemapLayerIndex(TArrayView<const int32> IndexRemap, FMemoryImageMaterialParameterInfo& OutResult) const;

	LAYOUT_FIELD(FScriptName, Name);
	LAYOUT_FIELD(int32, Index);
	LAYOUT_FIELD(TEnumAsByte<EMaterialParameterAssociation>, Association);
};

FORCEINLINE FMaterialParameterInfo::FMaterialParameterInfo(const struct FMemoryImageMaterialParameterInfo& Rhs)
	: Name(ScriptNameToName(Rhs.Name))
	, Association(Rhs.Association)
	, Index(Rhs.Index)
{
}

FORCEINLINE bool operator==(const FMaterialParameterInfo& Lhs, const FMaterialParameterInfo& Rhs)
{
	return Lhs.Name.IsEqual(Rhs.Name) && Lhs.Association == Rhs.Association && Lhs.Index == Rhs.Index;
}

FORCEINLINE bool operator!=(const FMaterialParameterInfo& Lhs, const FMaterialParameterInfo& Rhs)
{
	return !operator==(Lhs, Rhs);
}

FORCEINLINE bool operator==(const FMemoryImageMaterialParameterInfo& Lhs, const FMemoryImageMaterialParameterInfo& Rhs)
{
	return Lhs.Name == Rhs.Name && Lhs.Association == Rhs.Association && Lhs.Index == Rhs.Index;
}

FORCEINLINE bool operator!=(const FMemoryImageMaterialParameterInfo& Lhs, const FMemoryImageMaterialParameterInfo& Rhs)
{
	return !operator==(Lhs, Rhs);
}

FORCEINLINE bool operator==(const FMaterialParameterInfo& Lhs, const FMemoryImageMaterialParameterInfo& Rhs)
{
	return Lhs.Name == Rhs.Name && Lhs.Index == Rhs.Index && Lhs.Association == Rhs.Association;
}

FORCEINLINE bool operator!=(const FMaterialParameterInfo& Lhs, const FMemoryImageMaterialParameterInfo& Rhs)
{
	return !operator==(Lhs, Rhs);
}

FORCEINLINE bool operator==(const FMemoryImageMaterialParameterInfo& Lhs, const FMaterialParameterInfo& Rhs)
{
	return Lhs.Name == Rhs.Name && Lhs.Index == Rhs.Index && Lhs.Association == Rhs.Association;
}

FORCEINLINE bool operator!=(const FMemoryImageMaterialParameterInfo& Lhs, const FMaterialParameterInfo& Rhs)
{
	return !operator==(Lhs, Rhs);
}

FORCEINLINE uint32 GetTypeHash(const FMemoryImageMaterialParameterInfo& Value)
{
	return HashCombine(HashCombine(GetTypeHash(Value.Name), Value.Index), (uint32)Value.Association);
}

// Backwards compat
using FHashedMaterialParameterInfo = FMemoryImageMaterialParameterInfo;

UENUM()
enum class EMaterialLayerLinkState : uint8
{
	Uninitialized = 0u, // Saved with previous engine version
	LinkedToParent, // Layer should mirror changes from parent material
	UnlinkedFromParent, // Layer is based on parent material, but should not mirror changes
	NotFromParent, // Layer was created locally in this material, not in parent
};

USTRUCT()
struct ENGINE_API FMaterialLayersFunctions
{
	GENERATED_USTRUCT_BODY()

#if WITH_EDITOR
	/** Serializable ID structure for FMaterialLayersFunctions which allows us to deterministically recompile shaders*/
	struct ID
	{
		TArray<FGuid> LayerIDs;
		TArray<FGuid> BlendIDs;
		TArray<bool> LayerStates;

		bool operator==(const ID& Reference) const;
		inline bool operator!=(const ID& Reference) const { return !operator==(Reference); }

		void SerializeForDDC(FArchive& Ar);

		friend ID& operator<<(FArchive& Ar, ID& Ref)
		{
			Ref.SerializeForDDC(Ar);
			return Ref;
		}

		void UpdateHash(FSHA1& HashState) const;

		//TODO: Investigate whether this is really required given it is only used by FMaterialShaderMapId AND that one also uses UpdateHash
		void AppendKeyString(FString& KeyString) const;
	};
#endif // WITH_EDITOR

	static const FGuid BackgroundGuid;
		
	FMaterialLayersFunctions() = default;

	void Empty()
	{
		Layers.Empty();
		Blends.Empty();
#if WITH_EDITOR
		LayerStates.Empty();
		LayerNames.Empty();
		RestrictToLayerRelatives.Empty();
		RestrictToBlendRelatives.Empty();
		LayerGuids.Empty();
		LayerLinkStates.Empty();
#endif
	}

	UPROPERTY(EditAnywhere, Category=MaterialLayers)
	TArray<TObjectPtr<class UMaterialFunctionInterface>> Layers;

	UPROPERTY(EditAnywhere, Category=MaterialLayers)
	TArray<TObjectPtr<class UMaterialFunctionInterface>> Blends;

#if WITH_EDITORONLY_DATA
	UPROPERTY(EditAnywhere, Category = MaterialLayers)
	TArray<bool> LayerStates;

	UPROPERTY(EditAnywhere, Category = MaterialLayers)
	TArray<FText> LayerNames;

	UPROPERTY(EditAnywhere, Category = MaterialLayers)
	TArray<bool> RestrictToLayerRelatives;

	UPROPERTY(EditAnywhere, Category = MaterialLayers)
	TArray<bool> RestrictToBlendRelatives;

	/** Guid that identifies each layer in this stack */
	UPROPERTY(EditAnywhere, Category = MaterialLayers)
	TArray<FGuid> LayerGuids;

	/**
	 * State of each layer's link to parent material
	 */
	UPROPERTY(EditAnywhere, Category = MaterialLayers)
	TArray<EMaterialLayerLinkState> LayerLinkStates;

	/**
	 * List of Guids that exist in the parent material that have been explicitly deleted
	 * This is needed to distinguish these layers from newly added layers in the parent material
	 */
	UPROPERTY(EditAnywhere, Category = MaterialLayers)
	TArray<FGuid> DeletedParentLayerGuids;
#endif // WITH_EDITORONLY_DATA

	inline bool IsEmpty() const { return Layers.Num() == 0; }

	void AddDefaultBackgroundLayer()
	{
		// Default to a non-blended "background" layer
		Layers.AddDefaulted();
#if WITH_EDITORONLY_DATA
		LayerStates.Add(true);
		FText LayerName = FText(LOCTEXT("Background", "Background"));
		LayerNames.Add(LayerName);
		RestrictToLayerRelatives.Add(false);
		// Use a consistent Guid for the background layer
		// Default constructor assigning different guids will break FStructUtils::AttemptToFindUninitializedScriptStructMembers
		LayerGuids.Add(BackgroundGuid);
		LayerLinkStates.Add(EMaterialLayerLinkState::NotFromParent);
#endif // WITH_EDITORONLY_DATA
	}

	int32 AppendBlendedLayer();

	int32 AddLayerCopy(const FMaterialLayersFunctions& Source, int32 SourceLayerIndex, bool bVisible, EMaterialLayerLinkState LinkState);

	void InsertLayerCopy(const FMaterialLayersFunctions& Source, int32 SourceLayerIndex, EMaterialLayerLinkState LinkState, int32 LayerIndex);

	void RemoveBlendedLayerAt(int32 Index);

	void MoveBlendedLayer(int32 SrcLayerIndex, int32 DstLayerIndex);

#if WITH_EDITOR
	const ID GetID() const;

	/** Gets a string representation of the ID */
	FString GetStaticPermutationString() const;

	void UnlinkLayerFromParent(int32 Index);
	bool IsLayerLinkedToParent(int32 Index) const;
	void RelinkLayersToParent();
	bool HasAnyUnlinkedLayers() const;

	void ToggleBlendedLayerVisibility(int32 Index)
	{
		check(LayerStates.IsValidIndex(Index));
		LayerStates[Index] = !LayerStates[Index];
	}

	void SetBlendedLayerVisibility(int32 Index, bool InNewVisibility)
	{
		check(LayerStates.IsValidIndex(Index));
		LayerStates[Index] = InNewVisibility;
	}

	bool GetLayerVisibility(int32 Index) const
	{
		check(LayerStates.IsValidIndex(Index));
		return LayerStates[Index];
	}

	FText GetLayerName(int32 Counter) const
	{
		FText LayerName = FText::Format(LOCTEXT("LayerPrefix", "Layer {0}"), Counter);
		if (LayerNames.IsValidIndex(Counter))
		{
			LayerName = LayerNames[Counter];
		}
		return LayerName;
	}

	bool MatchesParent(const FMaterialLayersFunctions& Parent) const;

	void LinkAllLayersToParent();

	bool ResolveParent(const FMaterialLayersFunctions& Parent, TArray<int32>& OutRemapLayerIndices);

	void SerializeLegacy(FArchive& Ar);
#endif // WITH_EDITOR

	void PostSerialize(const FArchive& Ar);

	FORCEINLINE bool operator==(const FMaterialLayersFunctions& Other) const
	{
		if (Layers != Other.Layers || Blends != Other.Blends)
		{
			return false;
		}
#if WITH_EDITORONLY_DATA
		if (LayerStates != Other.LayerStates || LayerLinkStates != Other.LayerLinkStates || DeletedParentLayerGuids != Other.DeletedParentLayerGuids)
		{
			return false;
		}
#endif // WITH_EDITORONLY_DATA
		return true;
	}

	FORCEINLINE bool operator!=(const FMaterialLayersFunctions& Other) const
	{
		return !operator==(Other);
	}
};

template<>
struct TStructOpsTypeTraits<FMaterialLayersFunctions> : TStructOpsTypeTraitsBase2<FMaterialLayersFunctions>
{
	enum { WithPostSerialize = true };
};

#undef LOCTEXT_NAMESPACE