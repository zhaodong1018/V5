// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "MLDeformerVizSettings.generated.h"

// Forward declarations.
class UAnimSequence;
class UComputeGraph;
class UGeometryCache;

UENUM()
enum class EMLDeformerVizMode : uint8
{
	TrainingData = 0,
	TestData
};

/**
 * The vizualization settings.
 */
UCLASS()
class MLDEFORMER_API UMLDeformerVizSettings : public UObject
{
	GENERATED_BODY()

public:
	// UObject overloads.
	virtual bool IsEditorOnly() const override { return true; }
	// ~End of UObject interface

#if WITH_EDITORONLY_DATA
	const UAnimSequence* GetTestAnimSequence() const { return TestAnimSequence; }
	UAnimSequence* GetTestAnimSequence() { return TestAnimSequence; }
	UGeometryCache* GetGroundTruth() const { return GroundTruth; }
	FVector GetMeshSpacingOffsetVector() const { return FVector(MeshSpacing, 0.0f, 0.0f); }
	float GetMeshSpacing() const { return MeshSpacing; }
	float GetLabelHeight() const { return LabelHeight; }
	float GetAnimPlaySpeed() const { return AnimPlaySpeed; }
	float GetVertexDeltaMultiplier() const { return VertexDeltaMultiplier; }
	int32 GetFrameNumber() const { return FrameNumber; }
	bool GetDrawLabels() const { return bDrawLabels; }
	float GetLabelScale() const { return LabelScale; }
	bool GetShowHeatMap() const { return bShowHeatMap; }
	bool GetDrawVertexDeltas() const { return bDrawDeltas; }
	bool GetDrawLinearSkinnedActor() const { return bDrawLinearSkinnedActor; }
	bool GetDrawMLDeformedActor() const { return bDrawMLDeformedActor; }
	bool GetDrawGroundTruthActor() const { return bDrawGroundTruthActor; }
	bool GetXRayDeltas() const { return bXRayDeltas; }
	UComputeGraph* GetDeformerGraph() const { return DeformerGraph; }
	EMLDeformerVizMode GetVisualizationMode() const { return VisualizationMode; }
	EMLDeformerVizMode GetTempVisualizationMode() const { return TempVisualizationMode; }
	
	void SetTempVisualizationMode(EMLDeformerVizMode Mode) { TempVisualizationMode = Mode; }
	void SetDeformerGraph(UComputeGraph* InDeformerGraph) { DeformerGraph = InDeformerGraph; }
#endif

public:
#if WITH_EDITORONLY_DATA
	/** The data to visualize. */
	UPROPERTY(EditAnywhere, Category = "Data");
	EMLDeformerVizMode VisualizationMode = EMLDeformerVizMode::TrainingData;
	EMLDeformerVizMode TempVisualizationMode = EMLDeformerVizMode::TrainingData;	// Workaround for a bug in UX where the combobox triggers a value changed when clicking it.

	/** The animation sequence to play on the skeletal mesh. */
	UPROPERTY(EditAnywhere, Category = "Testing")
	TObjectPtr<UAnimSequence> TestAnimSequence = nullptr;

	/** The deformer graph to use on the asset editor's deformed test actor. */
	UPROPERTY(EditAnywhere, Category = "Testing")
	TObjectPtr<UComputeGraph> DeformerGraph = nullptr;

	/** The geometry cache that represents the ground truth of the test anim sequence. */
	UPROPERTY(EditAnywhere, Category = "Testing")
	TObjectPtr<UGeometryCache> GroundTruth = nullptr;

	/** The scale factor of the ML deformer deltas being applied on top of the linear skinned results. */
	UPROPERTY(EditAnywhere, Category = "Testing", meta = (ClampMin = "0.0", ClampMax = "5.0"))
	float VertexDeltaMultiplier = 1.0f;

	/** The play speed factor of the test anim sequence. */
	UPROPERTY(EditAnywhere, Category = "Testing", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float AnimPlaySpeed = 1.0f;

	/** Show the heat map? This will visualize the active areas of the deformer. */
	UPROPERTY(EditAnywhere, Category = "Testing")
	bool bShowHeatMap = false;

	/** Draw the linear skinned actor? */
	UPROPERTY(EditAnywhere, Category = "Testing")
	bool bDrawLinearSkinnedActor = true;

	/** Draw the ML Deformed actor? */
	UPROPERTY(EditAnywhere, Category = "Testing", DisplayName = "Draw ML Deformed Actor")
	bool bDrawMLDeformedActor = true;

	/** Draw the ground truth actor? */
	UPROPERTY(EditAnywhere, Category = "Testing", meta = (EditCondition = "GroundTruth != nullptr"))
	bool bDrawGroundTruthActor = true;

	/** The frame number of the training data to visualize. */
	UPROPERTY(EditAnywhere, Category = "Training Meshes", meta = (ClampMin = "0"))
	uint32 FrameNumber = 0;

	/** Draw the vertex deltas? */
	UPROPERTY(EditAnywhere, Category = "Training Meshes")
	bool bDrawDeltas = true;

	/** Enable this to draw the deltas in xray mode? */
	UPROPERTY(EditAnywhere, Category = "Training Meshes", meta = (EditCondition = "bDrawDeltas"))
	bool bXRayDeltas = true;

	/** Draw the text labels above each actor? */
	UPROPERTY(EditAnywhere, Category = "Shared Settings")
	bool bDrawLabels = true;

	/** The height in units to draw the labels at. */
	UPROPERTY(EditAnywhere, Category = "Shared Settings", meta = (EditCondition = "bDrawLabels"))
	float LabelHeight = 300.0f;

	/** The scale of the label text. */
	UPROPERTY(EditAnywhere, Category = "Shared Settings", meta = (ClampMin = "0.001", EditCondition = "bDrawLabels"))
	float LabelScale = 1.0f;

	/** The spacing between meshes. */
	UPROPERTY(EditAnywhere, Category = "Shared Settings", meta = (ClampMin = "0"))
	float MeshSpacing = 300.0f;
#endif // WITH_EDITORONLY_DATA
};
