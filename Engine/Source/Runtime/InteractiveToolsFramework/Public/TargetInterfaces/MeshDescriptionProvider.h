// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"

#include "MeshDescriptionProvider.generated.h"

struct FMeshDescription;

UINTERFACE()
class INTERACTIVETOOLSFRAMEWORK_API UMeshDescriptionProvider : public UInterface
{
	GENERATED_BODY()
};

class INTERACTIVETOOLSFRAMEWORK_API IMeshDescriptionProvider
{
	GENERATED_BODY()

public:
	/**
	 * Access the MeshDescriptionavailable through this Provider. Note that this MeshDescription may or may not 
	 * be owned by the provider and should not be modified directly. Use IMeshDescriptionCommitter for writes.
	 * @return pointer to MeshDescription 
	 */
	virtual const FMeshDescription* GetMeshDescription() = 0;

	/**
	 * Compute any auto-generated attributes on the input MeshDescription. This is required to support
	 * auto-computed Normals and Tangents on Static Mesh Assets, as those normals/tangents are computed 
	 * on-the-fly when generating rendering data, and will be zero in the MeshDescription returned by
	 * GetMeshDescription(). A no-op default implementation is provided.
	 */
	virtual void CalculateAutoGeneratedAttributes(FMeshDescription& MeshDescription)
	{
	}
};