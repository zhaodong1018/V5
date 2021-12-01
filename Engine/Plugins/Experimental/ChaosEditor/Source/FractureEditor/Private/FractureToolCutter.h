// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "FractureTool.h"

#include "BaseGizmos/CombinedTransformGizmo.h"
#include "BaseGizmos/TransformProxy.h"
#include "Drawing/LineSetComponent.h"

#include "FractureToolCutter.generated.h"

class FFractureToolContext;

struct FNoiseSettings;

/** Settings specifically related to the one-time destructive fracturing of a mesh **/
UCLASS(config = EditorPerProjectUserSettings)
class UFractureCutterSettings : public UFractureToolSettings
{
public:

	GENERATED_BODY()

	UFractureCutterSettings(const FObjectInitializer& ObjInit)
		: Super(ObjInit)
		, RandomSeed(-1)
		, ChanceToFracture(1.0)
		, bGroupFracture(true)
		, bDrawSites(false)
		, bDrawDiagram(true)
		, Amplitude(0.0f)
		, Frequency(0.1f)
		, Lacunarity(2.0f)
		, Persistence(0.5f)
		, OctaveNumber(4)
		, SurfaceResolution(10)
	{}

	void TransferNoiseSettings(FNoiseSettings& NoiseSettingsOut);

	/** Random number generator seed for repeatability */
	UPROPERTY(EditAnywhere, Category = CommonFracture, meta = (DisplayName = "Random Seed", UIMin = "-1", UIMax = "1000", ClampMin = "-1"))
	int32 RandomSeed;

	/** Chance to shatter each mesh.  Useful when shattering multiple selected meshes.  */
	UPROPERTY(EditAnywhere, Category = CommonFracture, meta = (DisplayName = "Chance To Fracture Per Mesh", UIMin = "0.0", UIMax = "1.0", ClampMin = "0.0", ClampMax = "1.0"))
	float ChanceToFracture;

	/** Generate a fracture pattern across all selected meshes.  */
	UPROPERTY(EditAnywhere, Category = CommonFracture, meta = (EditCondition = "bGroupFractureToggleEnabled", HideEditConditionToggle, EditConditionHides, DisplayName = "Group Fracture"))
	bool bGroupFracture;

	// This flag allows tools to disable the above bGroupFracture option if/when it is not applicable
	UPROPERTY()
	bool bGroupFractureToggleEnabled = true;

	/** Draw points marking the centers of pieces to be cut out by the fracture pattern.  */
	UPROPERTY(EditAnywhere, Category = CommonFracture, meta = (EditCondition = "bDrawSitesToggleEnabled", HideEditConditionToggle, EditConditionHides, DisplayName = "Draw Sites"))
	bool bDrawSites;

	// This flag allows tools to disable the above bDrawSites option if/when it is not applicable
	UPROPERTY()
	bool bDrawSitesToggleEnabled = true;

	/** Draw the edges of the fracture pattern.  */
	UPROPERTY(EditAnywhere, Category = CommonFracture, meta = (DisplayName = "Draw Diagram"))
	bool bDrawDiagram;

	/** Amount of space to leave between cut pieces */
	UPROPERTY(EditAnywhere, Category = CommonFracture, meta = (UIMin = "0.0", ClampMin = "0.0"))
	float Grout = 0.0f;

	/** Size of the noise displacement in centimeters */
	UPROPERTY(EditAnywhere, Category = Noise, meta = (UIMin = "0.0"))
	float Amplitude;

	/** Period of the Perlin noise.  Smaller values will create noise faces that are smoother */
	UPROPERTY(EditAnywhere, Category = Noise)
	float Frequency;

	/** Lacunarity of the Perlin noise.  Controls how the frequency scales per octave. */
	UPROPERTY(EditAnywhere, Category = Noise, meta = (UIMin = "1.0", UIMax = "4.0"))
	float Lacunarity;

	/** Persistence of the Perlin noise.  Controls how the amplitude scales per octave. */
	UPROPERTY(EditAnywhere, Category = Noise, meta = (UIMin = "0.01", UIMax = "1.0"))
	float Persistence;

	/** Number of fractal layers of Perlin noise to apply.  Smaller values (1 or 2) will create noise that looks like gentle rolling hills, while larger values (> 4) will tend to look more like craggy mountains */
	UPROPERTY(EditAnywhere, Category = Noise, meta = (ClampMin = "1", UIMax = "8"))
	int32 OctaveNumber;

	/** Spacing between vertices on cut surfaces, where noise is added.  Larger spacing between vertices will create more efficient meshes with fewer triangles, but less resolution to see the shape of the added noise  */
	UPROPERTY(EditAnywhere, Category = Noise, meta = (DisplayName = "Point Spacing", UIMin = "1", ClampMin = "0.1"))
	float SurfaceResolution;

	/// Get the maximum distance a vertex could be moved by a combination of grout and noise
	float GetMaxVertexMovement()
	{
		float MaxDisp = Grout;
		float AmplitudeScaled = Amplitude;
		for (int32 OctaveIdx = 0; OctaveIdx < OctaveNumber; OctaveIdx++, AmplitudeScaled *= Persistence)
		{
			MaxDisp += FMath::Abs(AmplitudeScaled);
		}
		return MaxDisp;
	}
};

/** Settings related to the collision properties of the fractured mesh pieces */
UCLASS(config = EditorPerProjectUserSettings)
class UFractureCollisionSettings : public UFractureToolSettings
{
public:
	GENERATED_BODY()

	UFractureCollisionSettings(const FObjectInitializer& ObjInit)
	: Super(ObjInit) {}

	/** Target spacing between collision samples on the mesh surface. */
	UPROPERTY(EditAnywhere, Category = Collision, meta = (UIMin = "1", ClampMin = "0.1"))
	float PointSpacing = 50.0f;

	// TODO: add remeshing options here as well
};

UCLASS(Abstract, DisplayName = "Cutter Base", Category = "FractureTools")
class UFractureToolCutterBase : public UFractureInteractiveTool
{
public:
	GENERATED_BODY()

	UFractureToolCutterBase(const FObjectInitializer& ObjInit);

	/** This is the Text that will appear on the button to execute the fracture **/
	virtual FText GetApplyText() const override { return FText(NSLOCTEXT("Fracture", "ExecuteFracture", "Fracture")); }

	virtual bool CanExecute() const override;

	virtual TArray<FFractureToolContext> GetFractureToolContexts() const override;

	virtual void FractureContextChanged() override
	{
		UpdateDefaultRandomSeed();
	}

	FBox GetCombinedBounds(const TArray<FFractureToolContext>& Contexts) const;


	virtual void UpdateUseGizmo(bool bUseGizmo)
	{
		SetMandateGroupFracture(bUseGizmo);
	}

	void SetMandateGroupFracture(bool bMandateGroupFracture)
	{
		CutterSettings->bGroupFractureToggleEnabled = !bMandateGroupFracture;
		if (bMandateGroupFracture)
		{
			CutterSettings->bGroupFracture = true;
		}
	}

protected:
	UPROPERTY(EditAnywhere, Category = Slicing)
	TObjectPtr<UFractureCutterSettings> CutterSettings;

	UPROPERTY(EditAnywhere, Category = Collision)
	TObjectPtr<UFractureCollisionSettings> CollisionSettings;

	/// Manage which seed to use when no specific random seed is specified, to control when the seed changes
	/// (primarily to avoid the seed changing between creating the preview and doing the actual cut)
	void UpdateDefaultRandomSeed();
	int32 DefaultRandomSeed = 0;

};


UCLASS(Abstract, DisplayName = "Voronoi Base", Category = "FractureTools")
class UFractureToolVoronoiCutterBase : public UFractureToolCutterBase
{
public:
	GENERATED_BODY()

	UFractureToolVoronoiCutterBase(const FObjectInitializer& ObjInit);

	virtual void Render(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI) override;

	virtual void FractureContextChanged() override;
	virtual int32 ExecuteFracture(const FFractureToolContext& FractureContext) override;

protected:
	virtual void GenerateVoronoiSites(const FFractureToolContext& Context, TArray<FVector>& Sites) {}

	FBox GetVoronoiBounds(const FFractureToolContext& FractureContext, const TArray<FVector>& Sites) const;

	virtual void ClearVisualizations()
	{
		Super::ClearVisualizations();
		VoronoiSites.Empty();
		SitesMappings.Empty();
		ClearEdges();
	}

	void ClearEdges()
	{
		CellMember.Empty();
		EdgesMappings.Empty();
		for (ULineSetComponent* Lines : VoronoiLineSets)
		{
			Lines->DestroyComponent();
		}
		VoronoiLineSets.Empty();
	}

	virtual void UpdateVisualizations(TArray<FFractureToolContext>& FractureContexts);

private:
	UPROPERTY()
	TArray<TObjectPtr<ULineSetComponent>> VoronoiLineSets;

	TArray<int32> CellMember;
	TArray<FVector> VoronoiSites;

	TArray<FColor> Colors;

	FVisualizationMappings SitesMappings, EdgesMappings;

	void UpdateLineSetExplodedVectors();
};


/// This helps create a 3D transform gizmo that can be used to adjust fracture placement
// Note it is tailored to UFractureToolCutterBase, and expects Setup(), Shutdown()
// and ResetGizmo() to be called on tool setup, shutdown, and selection change respectively
UCLASS(config = EditorPerProjectUserSettings)
class UFractureTransformGizmoSettings : public UFractureToolSettings
{
public:
	GENERATED_BODY()

	UFractureTransformGizmoSettings(const FObjectInitializer & ObjInit);

	void Setup(UFractureToolCutterBase* Cutter, ETransformGizmoSubElements GizmoElements = ETransformGizmoSubElements::StandardTranslateRotate);
	void Shutdown();

	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

	void TransformChanged(UTransformProxy* Proxy, FTransform Transform);

	void ResetGizmo(bool bResetRotation = false);

	bool IsGizmoEnabled()
	{
		return bUseGizmo && TransformGizmo->IsVisible();
	}

	FTransform GetTransform()
	{
		return TransformProxy->GetTransform();
	}

	/** Use a 3D rigid transform gizmo to place the fracture pattern.  Only supports grouped fracture. */
	UPROPERTY(EditAnywhere, Category = PlacementControls, meta = (EditCondition = "bShowUseGizmoOption", EditConditionHides, HideEditConditionToggle))
	bool bUseGizmo = true;

	/** Recenter the gizmo to the center of the selection when selection changes */
	UPROPERTY(EditAnywhere, Category = PlacementControls, meta = (EditCondition = "bUseGizmo", EditConditionHides))
	bool bCenterOnSelection = true;

	UPROPERTY()
	bool bShowUseGizmoOption = true;

	UPROPERTY()
	TObjectPtr<UCombinedTransformGizmo> TransformGizmo = nullptr;

	UPROPERTY()
	TObjectPtr<UTransformProxy> TransformProxy = nullptr;

protected:

	TObjectPtr<UFractureToolCutterBase> AttachedCutter = nullptr;
};

