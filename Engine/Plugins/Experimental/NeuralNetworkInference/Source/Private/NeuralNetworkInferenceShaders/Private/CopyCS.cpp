// Copyright Epic Games, Inc. All Rights Reserved.

#include "CopyCS.h"



/* FCopyCS public functions
 *****************************************************************************/

void FCopyCS::ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& InParameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(InParameters, OutEnvironment);
	OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), THREADGROUP_SIZE_X);
}



const uint32 FCopyCS::THREADGROUP_SIZE_X = 128;



/* Shader implementation
 *****************************************************************************/

IMPLEMENT_SHADER_TYPE(, FCopyCS, TEXT("/Plugins/NeuralNetworkInference/Private/Copy.usf"), TEXT("CopyCS"), SF_Compute) // Path defined in NeuralNetworkInferenceShadersModule.cpp
