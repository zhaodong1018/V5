// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/MeshMerging.h"
#include "Layout/Visibility.h"
#include "IPropertyTypeCustomization.h"
#include "PropertyHandle.h"
#include "OverrideResetToDefault.h"

class FDetailWidgetRow;

class FMeshProxySettingsCustomizations : public IPropertyTypeCustomization, public TOverrideResetToDefaultWithStaticUStruct<FMeshProxySettings>
{

public:
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	/** IPropertyTypeCustomization instance */
	virtual void CustomizeHeader(TSharedRef<IPropertyHandle> StructPropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& StructCustomizationUtils) override;
	virtual void CustomizeChildren(TSharedRef<IPropertyHandle> StructPropertyHandle, class IDetailChildrenBuilder& ChildBuilder, IPropertyTypeCustomizationUtils& StructCustomizationUtils) override;

protected:
	EVisibility IsHardAngleThresholdVisible() const;
	EVisibility IsRecalculateNormalsVisible() const;
	EVisibility IsUseLandscapeCullingVisible() const;
	EVisibility IsUseLandscapeCullingPrecisionVisible() const;
	EVisibility IsMergeDistanceVisible() const;
	EVisibility IsUnresolvedGeometryColorVisible() const;
	EVisibility IsVoxelSizeVisible() const;
	EVisibility IsNormalCalcMethodVisible() const;
	EVisibility IsSearchDistanceVisible() const;

	EVisibility IsThirdPartySpecificVisible() const;
	EVisibility IsProxyLODSpecificVisible() const;
	bool UseNativeProxyLODTool() const;
};
