// Copyright Epic Games, Inc. All Rights Reserved.

#include "MLDeformerVizSettingsDetails.h"
#include "MLDeformerVizSettings.h"
#include "MLDeformer.h"
#include "MLDeformerAsset.h"

#include "MLDeformerEditorData.h"
#include "ComputeFramework/ComputeGraph.h"

#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailsView.h"

#include "Widgets/SWidget.h"
#include "Layout/Margin.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SBoxPanel.h"
#include "EditorStyleSet.h"
#include "IDetailChildrenBuilder.h"
#include "IDetailGroup.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "PropertyCustomizationHelpers.h"
#include "AssetRegistry/AssetData.h"

#include "PropertyHandle.h"
#include "DetailLayoutBuilder.h"

#include "SWarningOrErrorBox.h"

#include "GeometryCache.h"
#include "Animation/AnimSequence.h"
#include "Engine/SkeletalMesh.h"

#define LOCTEXT_NAMESPACE "MLDeformerVizSettingsDetails"

TSharedRef<IDetailCustomization> FMLDeformerVizSettingsDetails::MakeInstance()
{
	return MakeShareable(new FMLDeformerVizSettingsDetails());
}

UMLDeformerAsset* FMLDeformerVizSettingsDetails::GetMLDeformerAsset() const
{
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailLayoutBuilder->GetObjectsBeingCustomized(Objects);
	UMLDeformerVizSettings* VizSettings = nullptr;
	UMLDeformerAsset* DeformerAsset = nullptr;
	if (Objects.Num() == 1)
	{
		VizSettings = Cast<UMLDeformerVizSettings>(Objects[0]);
		check(VizSettings);
		DeformerAsset = Cast<UMLDeformerAsset>(VizSettings->GetOuter());
		check(DeformerAsset);
		return DeformerAsset;
	}

	return nullptr;
}

void FMLDeformerVizSettingsDetails::CustomizeDetails(class IDetailLayoutBuilder& DetailBuilder)
{
	DetailLayoutBuilder = &DetailBuilder;

	UMLDeformerAsset* DeformerAsset = GetMLDeformerAsset();
	UMLDeformerVizSettings* VizSettings = DeformerAsset->GetVizSettings();

	const bool bShowTrainingData = VizSettings ? (VizSettings->GetVisualizationMode() == EMLDeformerVizMode::TrainingData) : true;
	const bool bShowTestData = VizSettings ? (VizSettings->GetVisualizationMode() == EMLDeformerVizMode::TestData) : true;

	IDetailCategoryBuilder& DataCategoryBuilder = DetailBuilder.EditCategory("Data Selection", FText::GetEmpty(), ECategoryPriority::Important);
	DataCategoryBuilder.AddProperty(GET_MEMBER_NAME_CHECKED(UMLDeformerVizSettings, VisualizationMode));

	// Shared settings.
	IDetailCategoryBuilder& SharedCategoryBuilder = DetailBuilder.EditCategory("Shared Settings", FText::GetEmpty(), ECategoryPriority::Important);
	SharedCategoryBuilder.AddProperty(GET_MEMBER_NAME_CHECKED(UMLDeformerVizSettings, bDrawLabels));
	SharedCategoryBuilder.AddProperty(GET_MEMBER_NAME_CHECKED(UMLDeformerVizSettings, LabelHeight));
	SharedCategoryBuilder.AddProperty(GET_MEMBER_NAME_CHECKED(UMLDeformerVizSettings, LabelScale));
	SharedCategoryBuilder.AddProperty(GET_MEMBER_NAME_CHECKED(UMLDeformerVizSettings, MeshSpacing));

	// Testing.
	IDetailCategoryBuilder& TestingCategoryBuilder = DetailBuilder.EditCategory("Testing", FText::GetEmpty(), ECategoryPriority::Important);
	TestingCategoryBuilder.SetCategoryVisibility(bShowTestData);
	
	IDetailPropertyRow& TestAnimRow = TestingCategoryBuilder.AddProperty(GET_MEMBER_NAME_CHECKED(UMLDeformerVizSettings, TestAnimSequence));
	TestAnimRow.CustomWidget()
	.NameContent()
	[
		TestAnimRow.GetPropertyHandle()->CreatePropertyNameWidget()
	]
	.ValueContent()
	[
		SNew(SObjectPropertyEntryBox)
		.PropertyHandle(TestAnimRow.GetPropertyHandle())
		.AllowedClass(UAnimSequence::StaticClass())
		.ObjectPath(VizSettings ? VizSettings->GetTestAnimSequence()->GetPathName() : FString())
		.ThumbnailPool(DetailBuilder.GetThumbnailPool())
		.OnShouldFilterAsset(
			this, 
			&FMLDeformerVizSettingsDetails::FilterAnimSequences, 
			DeformerAsset->GetSkeletalMesh() ? DeformerAsset->GetSkeletalMesh()->GetSkeleton() : nullptr
		)
	];

	if (VizSettings)
	{
		const FText AnimErrorText = DeformerAsset->GetIncompatibleSkeletonErrorText(DeformerAsset->GetSkeletalMesh(), VizSettings->GetTestAnimSequence());
		FDetailWidgetRow& AnimErrorRow = TestingCategoryBuilder.AddCustomRow(FText::FromString("AnimSkeletonMisMatchError"))
			.Visibility(!AnimErrorText.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed)
			.WholeRowContent()
			[
				SNew(SBox)
				.Padding(FMargin(0.0f, 4.0f))
				[
					SNew(SWarningOrErrorBox)
					.MessageStyle(EMessageStyle::Warning)
					.Message(AnimErrorText)
				]
			];
	}

	TestingCategoryBuilder.AddProperty(GET_MEMBER_NAME_CHECKED(UMLDeformerVizSettings, AnimPlaySpeed));

	FIsResetToDefaultVisible IsResetVisible = FIsResetToDefaultVisible::CreateSP(this, &FMLDeformerVizSettingsDetails::IsResetToDefaultDeformerGraphVisible);
	FResetToDefaultHandler ResetHandler = FResetToDefaultHandler::CreateSP(this, &FMLDeformerVizSettingsDetails::OnResetToDefaultDeformerGraph);
	FResetToDefaultOverride ResetOverride = FResetToDefaultOverride::Create(IsResetVisible, ResetHandler);
	TestingCategoryBuilder.AddProperty(GET_MEMBER_NAME_CHECKED(UMLDeformerVizSettings, DeformerGraph)).OverrideResetToDefault(ResetOverride);

	// Show a warning when no deformer graph has been selected.
	UObject* Graph = nullptr;
	TSharedRef<IPropertyHandle> DeformerGraphProperty = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UMLDeformerVizSettings, DeformerGraph));
	if (DeformerGraphProperty->GetValue(Graph) == FPropertyAccess::Result::Success)
	{
		FDetailWidgetRow& GraphErrorRow = TestingCategoryBuilder.AddCustomRow(FText::FromString("GraphError"))
			.Visibility((Graph == nullptr) ? EVisibility::Visible : EVisibility::Collapsed)
			.WholeRowContent()
			[
				SNew(SBox)
				.Padding(FMargin(0.0f, 4.0f))
				[
					SNew(SWarningOrErrorBox)
					.MessageStyle(EMessageStyle::Warning)
					.Message(FText::FromString("Please select a deformer graph.\nOtherwise only linear skinning is used."))
				]
			];
	}

	if (DeformerAsset)
	{
		FDetailWidgetRow& ErrorRow = TestingCategoryBuilder.AddCustomRow(FText::FromString("NoNeuralNetError"))
			.Visibility((DeformerAsset->GetInferenceNeuralNetwork() == nullptr && Graph != nullptr) ? EVisibility::Visible : EVisibility::Collapsed)
			.WholeRowContent()
			[
				SNew(SBox)
				.Padding(FMargin(0.0f, 4.0f))
				[
					SNew(SWarningOrErrorBox)
					.MessageStyle(EMessageStyle::Warning)
					.Message(FText::FromString("The selected deformer graph isn't used, because you didn't train the neural network yet.\n\nLinear skinning is used until then."))
				]
			];
	}

	TestingCategoryBuilder.AddProperty(GET_MEMBER_NAME_CHECKED(UMLDeformerVizSettings, GroundTruth));

	// Show an error when the test anim sequence duration doesn't match the one of the ground truth.
	if (VizSettings)
	{
		const FText AnimErrorText = DeformerAsset->GetAnimSequenceErrorText(VizSettings->GroundTruth, VizSettings->GetTestAnimSequence());
		FDetailWidgetRow& GroundTruthAnimErrorRow = TestingCategoryBuilder.AddCustomRow(FText::FromString("GroundTruthAnimMismatchError"))
			.Visibility(!AnimErrorText.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed)
			.WholeRowContent()
			[
				SNew(SBox)
				.Padding(FMargin(0.0f, 4.0f))
				[
					SNew(SWarningOrErrorBox)
					.MessageStyle(EMessageStyle::Warning)
					.Message(AnimErrorText)
				]
			];

		const FText GeomErrorText = DeformerAsset->GetGeomCacheErrorText(VizSettings->GetGroundTruth());
		FDetailWidgetRow& GroundTruthGeomErrorRow = TestingCategoryBuilder.AddCustomRow(FText::FromString("GroundTruthGeomMismatchError"))
			.Visibility(!GeomErrorText.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed)
			.WholeRowContent()
			[
				SNew(SBox)
				.Padding(FMargin(0.0f, 4.0f))
				[
					SNew(SWarningOrErrorBox)
					.MessageStyle(EMessageStyle::Warning)
					.Message(GeomErrorText)
				]
			];

		const FText VertexErrorText = DeformerAsset->GetVertexErrorText(DeformerAsset->SkeletalMesh, VizSettings->GetGroundTruth(), FText::FromString("Base Mesh"), FText::FromString("Ground Truth Mesh"));
		FDetailWidgetRow& GroundTruthVertexErrorRow = TestingCategoryBuilder.AddCustomRow(FText::FromString("GroundTruthVertexMismatchError"))
			.Visibility(!VertexErrorText.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed)
			.WholeRowContent()
			[
				SNew(SBox)
				.Padding(FMargin(0.0f, 4.0f))
				[
					SNew(SWarningOrErrorBox)
					.MessageStyle(EMessageStyle::Warning)
					.Message(VertexErrorText)
				]
			];
	}

	TestingCategoryBuilder.AddProperty(GET_MEMBER_NAME_CHECKED(UMLDeformerVizSettings, VertexDeltaMultiplier));
	TestingCategoryBuilder.AddProperty(GET_MEMBER_NAME_CHECKED(UMLDeformerVizSettings, bShowHeatMap));
	TestingCategoryBuilder.AddProperty(GET_MEMBER_NAME_CHECKED(UMLDeformerVizSettings, bDrawLinearSkinnedActor));
	TestingCategoryBuilder.AddProperty(GET_MEMBER_NAME_CHECKED(UMLDeformerVizSettings, bDrawMLDeformedActor));
	TestingCategoryBuilder.AddProperty(GET_MEMBER_NAME_CHECKED(UMLDeformerVizSettings, bDrawGroundTruthActor));

	// Training data.
	IDetailCategoryBuilder& TrainingMeshesCategoryBuilder = DetailBuilder.EditCategory("Training Meshes", FText::GetEmpty(), ECategoryPriority::Important);
	TrainingMeshesCategoryBuilder.SetCategoryVisibility(bShowTrainingData);
	TrainingMeshesCategoryBuilder.AddProperty(GET_MEMBER_NAME_CHECKED(UMLDeformerVizSettings, FrameNumber));
	TrainingMeshesCategoryBuilder.AddProperty(GET_MEMBER_NAME_CHECKED(UMLDeformerVizSettings, bDrawDeltas));
	TrainingMeshesCategoryBuilder.AddProperty(GET_MEMBER_NAME_CHECKED(UMLDeformerVizSettings, bXRayDeltas));
}

bool FMLDeformerVizSettingsDetails::FilterAnimSequences(const FAssetData& AssetData, USkeleton* Skeleton)
{
	if (Skeleton && Skeleton->IsCompatibleSkeletonByAssetData(AssetData))
	{
		return false;
	}

	return true;
}

void FMLDeformerVizSettingsDetails::OnResetToDefaultDeformerGraph(TSharedPtr<IPropertyHandle> PropertyHandle)
{
	UMLDeformerAsset* DeformerAsset = GetMLDeformerAsset();
	UMLDeformerVizSettings* VizSettings = DeformerAsset->GetVizSettings();

	UComputeGraph* DefaultGraph = FMLDeformerEditorData::LoadDefaultDeformerGraph();
	PropertyHandle->SetValue(DefaultGraph);
}

bool FMLDeformerVizSettingsDetails::IsResetToDefaultDeformerGraphVisible(TSharedPtr<IPropertyHandle> PropertyHandle)
{
	UObject* CurrentGraph = nullptr;
	PropertyHandle->GetValue(CurrentGraph);
	if (CurrentGraph == nullptr)
	{
		return true;
	}

	// Check if we already assigned the default asset.
	const FAssetData CurrentGraphAssetData(CurrentGraph);
	const FString CurrentPath = CurrentGraphAssetData.ObjectPath.ToString();
	const FString DefaultPath = FMLDeformerEditorData::GetDefaultDeformerGraphAssetPath();
	return (DefaultPath != CurrentPath);
}

#undef LOCTEXT_NAMESPACE
