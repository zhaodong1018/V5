// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Layout/Visibility.h"
#include "IPropertyTypeCustomization.h"
#include "PropertyHandle.h"
#include "GameFramework/WorldSettings.h"
#include "OverrideResetToDefault.h"

class FDetailWidgetRow;

class FHierarchicalSimplificationCustomizations : public IPropertyTypeCustomization, public TOverrideResetToDefaultWithStaticUStruct<FHierarchicalSimplification>
{
public:
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	/** IPropertyTypeCustomization instance */
	virtual void CustomizeHeader(TSharedRef<IPropertyHandle> StructPropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& StructCustomizationUtils) override;
	virtual void CustomizeChildren( TSharedRef<IPropertyHandle> StructPropertyHandle, class IDetailChildrenBuilder& ChildBuilder, IPropertyTypeCustomizationUtils& StructCustomizationUtils ) override;

protected:
	EVisibility IsSimplifyMeshVisible() const;
	EVisibility IsProxyMeshSettingVisible() const;
	EVisibility IsMergeMeshSettingVisible() const;

	TSharedPtr< IPropertyHandle > SimplifyMeshPropertyHandle;
};
