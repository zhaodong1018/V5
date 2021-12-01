// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BaseTools/SingleSelectionMeshEditingTool.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "MeshOpPreviewHelpers.h"
#include "Properties/MeshMaterialProperties.h"
#include "Properties/MeshUVChannelProperties.h"
#include "Properties/RecomputeUVsProperties.h"
#include "PropertySets/PolygroupLayersProperties.h"
#include "Polygroups/PolygroupSet.h"
#include "Drawing/UVLayoutPreview.h"

#include "RecomputeUVsTool.generated.h"


// predeclarations
class UDynamicMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class RecomputeUVsOp;
class URecomputeUVsOpFactory;


/**
 *
 */
UCLASS()
class MESHMODELINGTOOLSEXP_API URecomputeUVsToolBuilder : public USingleSelectionMeshEditingToolBuilder
{
	GENERATED_BODY()

public:
	virtual USingleSelectionMeshEditingTool* CreateNewTool(const FToolBuilderState& SceneState) const override;
};


/**
 * URecomputeUVsTool Recomputes UVs based on existing segmentations of the mesh
 */
UCLASS()
class MESHMODELINGTOOLSEXP_API URecomputeUVsTool : public USingleSelectionMeshEditingTool
{
	GENERATED_BODY()

public:
	virtual void Setup() override;
	virtual void Shutdown(EToolShutdownType ShutdownType) override;

	virtual void Render(IToolsContextRenderAPI* RenderAPI) override;
	virtual void OnTick(float DeltaTime) override;

	virtual bool HasCancel() const override { return true; }
	virtual bool HasAccept() const override { return true; }
	virtual bool CanAccept() const override;

	virtual void OnPropertyModified(UObject* PropertySet, FProperty* Property) override;

protected:
	UPROPERTY()
	TObjectPtr<UMeshUVChannelProperties> UVChannelProperties = nullptr;

	UPROPERTY()
	TObjectPtr<URecomputeUVsToolProperties> Settings = nullptr;

	UPROPERTY()
	TObjectPtr<UPolygroupLayersProperties> PolygroupLayerProperties = nullptr;

	UPROPERTY()
	TObjectPtr<UExistingMeshMaterialProperties> MaterialSettings = nullptr;

	UPROPERTY()
	bool bCreateUVLayoutViewOnSetup = true;

	UPROPERTY()
	TObjectPtr<UUVLayoutPreview> UVLayoutView = nullptr;

	UPROPERTY()
	TObjectPtr<URecomputeUVsOpFactory> RecomputeUVsOpFactory;

protected:
	UPROPERTY()
	TObjectPtr<UMeshOpPreviewWithBackgroundCompute> Preview = nullptr;

protected:
	TSharedPtr<UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe> InputMesh;


	TSharedPtr<UE::Geometry::FPolygroupSet, ESPMode::ThreadSafe> ActiveGroupSet;
	void OnSelectedGroupLayerChanged();
	void UpdateActiveGroupLayer();
	int32 GetSelectedUVChannel() const;
	void OnPreviewMeshUpdated();
};
