// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BaseTools/SingleSelectionMeshEditingTool.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "MeshOpPreviewHelpers.h"
#include "Properties/MeshMaterialProperties.h"
#include "Properties/MeshUVChannelProperties.h"
#include "PropertySets/PolygroupLayersProperties.h"
#include "Drawing/UVLayoutPreview.h"

#include "ParameterizeMeshTool.generated.h"

// predeclarations
class UDynamicMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UParameterizeMeshToolProperties;
class UParameterizeMeshToolUVAtlasProperties;
class UParameterizeMeshToolXAtlasProperties;
class UParameterizeMeshToolPatchBuilderProperties;


UCLASS()
class MESHMODELINGTOOLSEDITORONLY_API UParameterizeMeshToolBuilder : public USingleSelectionMeshEditingToolBuilder
{
	GENERATED_BODY()
public:
	virtual USingleSelectionMeshEditingTool* CreateNewTool(const FToolBuilderState& SceneState) const override;
};


UCLASS()
class MESHMODELINGTOOLSEDITORONLY_API UParameterizeMeshToolPatchBuilderGroupLayerProperties : public UPolygroupLayersProperties
{
	GENERATED_BODY()
public:

	UPROPERTY(EditAnywhere, Category = PolygroupLayers)
	bool bConstrainToPolygroups = false;

};



/**
 * UParameterizeMeshTool automatically decomposes the input mesh into charts, solves for UVs,
 * and then packs the resulting charts
 */
UCLASS()
class MESHMODELINGTOOLSEDITORONLY_API UParameterizeMeshTool : public USingleSelectionMeshEditingTool, public UE::Geometry::IDynamicMeshOperatorFactory
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

	// IDynamicMeshOperatorFactory API
	virtual TUniquePtr<UE::Geometry::FDynamicMeshOperator> MakeNewOperator() override;

protected:
	UPROPERTY()
	TObjectPtr<UMeshUVChannelProperties> UVChannelProperties = nullptr;

	UPROPERTY()
	TObjectPtr<UParameterizeMeshToolProperties> Settings = nullptr;

	UPROPERTY()
	TObjectPtr<UParameterizeMeshToolUVAtlasProperties> UVAtlasProperties = nullptr;

	UPROPERTY()
	TObjectPtr<UParameterizeMeshToolXAtlasProperties> XAtlasProperties = nullptr;

	UPROPERTY()
	TObjectPtr<UParameterizeMeshToolPatchBuilderProperties> PatchBuilderProperties = nullptr;

	UPROPERTY()
	TObjectPtr<UParameterizeMeshToolPatchBuilderGroupLayerProperties> PolygroupLayerProperties = nullptr;

	UPROPERTY()
	TObjectPtr<UExistingMeshMaterialProperties> MaterialSettings = nullptr;

	UPROPERTY()
	bool bCreateUVLayoutViewOnSetup = true;

	UPROPERTY()
	TObjectPtr<UUVLayoutPreview> UVLayoutView = nullptr;

protected:
	UPROPERTY()
	TObjectPtr<UMeshOpPreviewWithBackgroundCompute> Preview = nullptr;

protected:
	TSharedPtr<UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe> InputMesh;

	void OnMethodTypeChanged();

	void OnPreviewMeshUpdated();
};
