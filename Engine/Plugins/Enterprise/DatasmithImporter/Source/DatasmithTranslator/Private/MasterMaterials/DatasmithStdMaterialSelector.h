// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "DatasmithDefinitions.h"
#include "MasterMaterials/DatasmithMasterMaterial.h"
#include "MasterMaterials/DatasmithMasterMaterialSelector.h"

#include "Templates/SharedPointer.h"

class IDatasmithMasterMaterialElement;

class FDatasmithStdMaterialSelector : public FDatasmithMasterMaterialSelector
{
public:
	FDatasmithStdMaterialSelector();
	virtual ~FDatasmithStdMaterialSelector() = default;

	virtual bool IsValid() const override;
	virtual const FDatasmithMasterMaterial& GetMasterMaterial( const TSharedPtr< IDatasmithMasterMaterialElement >& InDatasmithMaterial ) const override;
	virtual void FinalizeMaterialInstance(const TSharedPtr< IDatasmithMasterMaterialElement >& InDatasmithMaterial, UMaterialInstanceConstant* MaterialInstance) const override {}

private:
	FDatasmithMasterMaterial MasterMaterialOpaque;
	FDatasmithMasterMaterial MasterMaterialTranslucent;
};
