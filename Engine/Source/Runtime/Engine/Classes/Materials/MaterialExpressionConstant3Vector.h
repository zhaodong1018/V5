// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "Materials/MaterialExpression.h"
#include "MaterialExpressionConstant3Vector.generated.h"

UCLASS(collapsecategories, hidecategories=Object, MinimalAPI)
class UMaterialExpressionConstant3Vector
	: public UMaterialExpression
{
	GENERATED_UCLASS_BODY()

 	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=MaterialExpressionConstant3Vector, meta=(HideAlphaChannel))
	FLinearColor Constant;

public:

	//~ UMaterialExpression interface
#if WITH_EDITOR
	virtual int32 Compile(class FMaterialCompiler* Compiler, int32 OutputIndex) override;
	virtual void GetCaption(TArray<FString>& OutCaptions) const override;

	virtual FString GetDescription() const override;
	virtual uint32 GetOutputType(int32 OutputIndex) override {return MCT_Float3;}
	virtual EMaterialGenerateHLSLStatus GenerateHLSLExpression(FMaterialHLSLGenerator& Generator, UE::HLSLTree::FScope& Scope, int32 OutputIndex, UE::HLSLTree::FExpression*& OutExpression) override;
#endif // WITH_EDITOR
};
