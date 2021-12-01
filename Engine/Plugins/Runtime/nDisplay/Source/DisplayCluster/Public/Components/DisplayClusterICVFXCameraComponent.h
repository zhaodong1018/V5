// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "CineCameraActor.h"
#include "CineCameraComponent.h"

#include "DisplayClusterConfigurationTypes_ICVFX.h"
#include "DisplayClusterEditorPropertyReference.h"
#include "Render/Viewport/Containers/DisplayClusterViewport_CameraMotionBlur.h"

#include "DisplayClusterICVFXCameraComponent.generated.h"

struct FMinimalViewInfo;
class UCameraComponent;

/**
 * nDisplay in-camera VFX camera representation
 */
UCLASS(ClassGroup = (DisplayCluster), HideCategories = (AssetUserData, Collision, Cooking, ComponentReplication, Events, Physics, Sockets, Activation, Tags, ComponentTick), meta = (DisplayName="ICVFX Camera"))
class DISPLAYCLUSTER_API UDisplayClusterICVFXCameraComponent
	: public UCineCameraComponent
{
	GENERATED_BODY()

public:
	UDisplayClusterICVFXCameraComponent(const FObjectInitializer& ObjectInitializer)
	{ }

public:
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = NDisplay)
	FDisplayClusterConfigurationICVFX_CameraSettings CameraSettings;

#if WITH_EDITOR
	virtual bool GetEditorPreviewInfo(float DeltaTime, FMinimalViewInfo& ViewOut) override;
#endif

public:
	FDisplayClusterViewport_CameraMotionBlur GetMotionBlurParameters();


	bool IsICVFXEnabled() const
	{
		return CameraSettings.bEnable;
	}

	// Return unique camera name
	FString GetCameraUniqueId() const;

	const FDisplayClusterConfigurationICVFX_CameraSettings& GetCameraSettingsICVFX() const
	{
		return CameraSettings;
	}

	UCameraComponent* GetCameraComponent();
	void GetDesiredView(FMinimalViewInfo& DesiredView);

	
//////////////////////////////////////////////////////////////////////////////////////////////
// Details Panel Property Referencers
//////////////////////////////////////////////////////////////////////////////////////////////
#if WITH_EDITORONLY_DATA
private:
	friend class FDisplayClusterICVFXCameraComponentDetailsCustomization;
	
	UPROPERTY(EditAnywhere, Transient, Category = "In Camera VFX", meta = (PropertyPath = "CameraSettings.bEnable"))
	FDisplayClusterEditorPropertyReference IsEnabledRef;

	UPROPERTY(EditAnywhere, Transient, Category = "In Camera VFX", meta = (PropertyPath = "CameraSettings.HiddenICVFXViewports"))
	FDisplayClusterEditorPropertyReference HiddenICVFXViewportsRef;

	UPROPERTY(EditAnywhere, Transient, Category = "In Camera VFX", meta = (PropertyPath = "CameraSettings.ExternalCameraActor"))
	FDisplayClusterEditorPropertyReference ExternalCameraActorRef;

	UPROPERTY(EditAnywhere, Transient, Category = "In Camera VFX", meta = (PropertyPath = "CameraSettings.BufferRatio"))
	FDisplayClusterEditorPropertyReference BufferRatioRef;

	UPROPERTY(EditAnywhere, Transient, Category = "In Camera VFX", meta = (DisplayName = "Overscan", PropertyPath = "CameraSettings.CustomFrustum"))
	FDisplayClusterEditorPropertyReference CustomFrustumRef;

	UPROPERTY(EditAnywhere, Transient, Category = "In Camera VFX", meta = (PropertyPath = "CameraSettings.SoftEdge"))
	FDisplayClusterEditorPropertyReference SoftEdgeRef;

	UPROPERTY(EditAnywhere, Transient, Category = "In Camera VFX", meta = (PropertyPath = "CameraSettings.Border"))
	FDisplayClusterEditorPropertyReference BorderRef;

	UPROPERTY(EditAnywhere, Transient, Category = "In Camera VFX", meta = (PropertyPath = "CameraSettings.FrustumRotation"))
	FDisplayClusterEditorPropertyReference FrustumRotationRef;

	UPROPERTY(EditAnywhere, Transient, Category = "In Camera VFX", meta = (PropertyPath = "CameraSettings.FrustumOffset"))
	FDisplayClusterEditorPropertyReference FrustumOffsetRef;

	UPROPERTY(EditAnywhere, Transient, Category = "In Camera VFX", meta = (PropertyPath = "CameraSettings.RenderSettings.GenerateMips", EditCondition = "CameraSettings.bEnable"))
	FDisplayClusterEditorPropertyReference GenerateMipsRef;

	UPROPERTY(EditAnywhere, Transient, Category = "In Camera VFX", meta = (PropertyPath = "CameraSettings.CameraMotionBlur", EditCondition = "CameraSettings.bEnable"))
	FDisplayClusterEditorPropertyReference CameraMotionBlurRef;

	UPROPERTY(EditAnywhere, Transient, Category = "In Camera VFX", meta = (PropertyPath = "CameraSettings.CameraHideList", EditCondition = "CameraSettings.bEnable"))
	FDisplayClusterEditorPropertyReference CameraHideListRef;

	UPROPERTY(EditAnywhere, Transient, Category = Chromakey, meta = (PropertyPath = "CameraSettings.Chromakey.bEnable", EditCondition = "CameraSettings.bEnable"))
	FDisplayClusterEditorPropertyReference ChromaKeyEnabledRef;

	UPROPERTY(EditAnywhere, Transient, Category = Chromakey, meta = (PropertyPath = "CameraSettings.Chromakey.ChromakeyColor", EditCondition = "CameraSettings.bEnable && CameraSettings.Chromakey.bEnable"))
	FDisplayClusterEditorPropertyReference ChromakeyColorRef;

	UPROPERTY(EditAnywhere, Transient, Category = Chromakey, meta = (PropertyPath = "CameraSettings.Chromakey.ChromakeyMarkers", EditCondition = "CameraSettings.bEnable && CameraSettings.Chromakey.bEnable"))
	FDisplayClusterEditorPropertyReference ChromakeyMarkersRef;

	UPROPERTY(EditAnywhere, Transient, Category = Chromakey, meta = (PropertyPath = "CameraSettings.Chromakey.ChromakeyRenderTexture", EditCondition = "CameraSettings.bEnable && CameraSettings.Chromakey.bEnable"))
	FDisplayClusterEditorPropertyReference ChromakeyRenderTextureRef;

	UPROPERTY(EditAnywhere, Transient, Category = OCIO, meta = (PropertyPath = "CameraSettings.AllNodesOCIOConfiguration.bIsEnabled", DisplayName = "Enable Inner Frustum OCIO", ToolTip = "Enable the application of an OpenColorIO configuration to all nodes."))
	FDisplayClusterEditorPropertyReference OCIOConfigurationEnabledRef;

	UPROPERTY(EditAnywhere, Transient, Category = OCIO, meta = (PropertyPath = "CameraSettings.AllNodesOCIOConfiguration.OCIOConfiguration.ColorConfiguration", DisplayName = "All Nodes Color Configuration", ToolTip = "Apply this OpenColorIO configuration to all nodes.", EditCondition = "CameraSettings.AllNodesOCIOConfiguration.bIsEnabled"))
	FDisplayClusterEditorPropertyReference OCIOColorConfiguratonRef;

	UPROPERTY(EditAnywhere, Transient, Category = OCIO, meta = (PropertyPath = "CameraSettings.PerNodeOCIOProfiles", EditCondition = "CameraSettings.AllNodesOCIOConfiguration.bIsEnabled"))
	FDisplayClusterEditorPropertyReference PerNodeOCIOProfilesRef;

	UPROPERTY(EditAnywhere, Transient, Category = "Inner Frustum Color Grading", meta = (PropertyPath = "CameraSettings.AllNodesColorGrading", DisplayName = "All Nodes"))
	FDisplayClusterEditorPropertyReference AllNodesColorGradingRef;

	UPROPERTY(EditAnywhere, Transient, Category = "Inner Frustum Color Grading", meta = (PropertyPath = "CameraSettings.PerNodeColorGrading"))
	FDisplayClusterEditorPropertyReference PerNodeColorGradingRef;

	UPROPERTY(EditAnywhere, Transient, Category = "Texture Replacement", meta = (PropertyPath = "CameraSettings.RenderSettings.Replace.bAllowReplace", DisplayName = "Enable Inner Frustum Texture Replacement", ToolTip = "Set to True to replace the entire inner frustum with the specified texture.", EditCondition = "CameraSettings.bEnable"))
	FDisplayClusterEditorPropertyReference TextureReplacementEnabledRef;

	UPROPERTY(EditAnywhere, Transient, Category = "Texture Replacement", meta = (PropertyPath = "CameraSettings.RenderSettings.Replace.SourceTexture", EditCondition = "CameraSettings.bEnable && CameraSettings.RenderSettings.Replace.bAllowReplace"))
	FDisplayClusterEditorPropertyReference SourceTextureRef;

	UPROPERTY(EditAnywhere, Transient, Category = "Texture Replacement", meta = (PropertyPath = "CameraSettings.RenderSettings.Replace.bShouldUseTextureRegion", EditCondition = "CameraSettings.bEnable && CameraSettings.RenderSettings.Replace.bAllowReplace"))
	FDisplayClusterEditorPropertyReference ShouldUseTextureRegionRef;

	UPROPERTY(EditAnywhere, Transient, Category = "Texture Replacement", meta = (PropertyPath = "CameraSettings.RenderSettings.Replace.TextureRegion", EditCondition = "CameraSettings.bEnable && CameraSettings.RenderSettings.Replace.bAllowReplace && CameraSettings.RenderSettings.Replace.bShouldUseTextureRegion"))
	FDisplayClusterEditorPropertyReference TextureRegionRef;

	UPROPERTY(EditDefaultsOnly, Transient, Category = Configuration, meta = (PropertyPath = "CameraSettings.RenderSettings.RenderOrder", EditCondition = "CameraSettings.bEnable"))
	FDisplayClusterEditorPropertyReference RenderOrderRef;

	UPROPERTY(EditDefaultsOnly, Transient, Category = Configuration, meta = (PropertyPath = "CameraSettings.RenderSettings.CustomFrameSize", EditCondition = "CameraSettings.bEnable"))
	FDisplayClusterEditorPropertyReference CustomFrameSizeRef;

	UPROPERTY(EditDefaultsOnly, Transient, Category = Configuration, meta = (PropertyPath = "CameraSettings.RenderSettings.AdvancedRenderSettings.RenderTargetRatio", EditCondition = "CameraSettings.bEnable"))
	FDisplayClusterEditorPropertyReference RenderTargetRatioRef;

	UPROPERTY(EditDefaultsOnly, Transient, Category = Configuration, meta = (PropertyPath = "CameraSettings.RenderSettings.AdvancedRenderSettings.GPUIndex", EditCondition = "CameraSettings.bEnable"))
	FDisplayClusterEditorPropertyReference GPUIndexRef;

	UPROPERTY(EditDefaultsOnly, Transient, Category = Configuration, meta = (PropertyPath = "CameraSettings.RenderSettings.AdvancedRenderSettings.StereoGPUIndex", EditCondition = "CameraSettings.bEnable"))
	FDisplayClusterEditorPropertyReference StereoGPUIndexRef;

	UPROPERTY(EditDefaultsOnly, Transient, Category = Configuration, meta = (PropertyPath = "CameraSettings.RenderSettings.AdvancedRenderSettings.StereoMode", EditCondition = "CameraSettings.bEnable"))
	FDisplayClusterEditorPropertyReference StereoModeRef;

#endif // WITH_EDITORONLY_DATA
};
