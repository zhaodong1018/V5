// Copyright Epic Games, Inc. All Rights Reserved.
#include "MaterialHLSLGenerator.h"

#if WITH_EDITOR

#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionVolumetricAdvancedMaterialOutput.h"
#include "Materials/Material.h"
#include "ShaderCore.h"
#include "HLSLTree/HLSLTree.h"
#include "HLSLTree/HLSLTreeCommon.h"
#include "Containers/LazyPrintf.h"

static UE::Shader::EValueType GetShaderType(EMaterialValueType MaterialType)
{
	switch (MaterialType)
	{
	case MCT_Float1: return UE::Shader::EValueType::Float1;
	case MCT_Float2: return UE::Shader::EValueType::Float2;
	case MCT_Float3: return UE::Shader::EValueType::Float3;
	case MCT_Float4: return UE::Shader::EValueType::Float4;
	case MCT_Float: return UE::Shader::EValueType::Float1;
	case MCT_StaticBool: return UE::Shader::EValueType::Bool1;
	case MCT_MaterialAttributes: return UE::Shader::EValueType::Struct;
	case MCT_ShadingModel: return UE::Shader::EValueType::Int1;
	case MCT_LWCScalar: return UE::Shader::EValueType::Double1;
	case MCT_LWCVector2: return UE::Shader::EValueType::Double2;
	case MCT_LWCVector3: return UE::Shader::EValueType::Double3;
	case MCT_LWCVector4: return UE::Shader::EValueType::Double4;
	default:return UE::Shader::EValueType::Void;
	}
}

FMaterialHLSLGenerator::FMaterialHLSLGenerator(UMaterial* InTargetMaterial, const FMaterialCompileTargetParameters& InCompileTarget, UE::HLSLTree::FTree& InOutTree)
	: CompileTarget(InCompileTarget)
	, TargetMaterial(InTargetMaterial)
	, HLSLTree(&InOutTree)
	, bGeneratedResult(false)
{
	const EMaterialShadingModel DefaultShadingModel = InTargetMaterial->GetShadingModels().GetFirstShadingModel();

	FFunctionCallEntry* RootFunctionEntry = new(InOutTree.GetAllocator()) FFunctionCallEntry();
	FunctionCallStack.Add(RootFunctionEntry);

	TArray<UE::HLSLTree::FStructFieldInitializer, TInlineAllocator<MP_MAX>> MaterialAttributeFields;

	const TArray<FGuid>& OrderedVisibleAttributes = FMaterialAttributeDefinitionMap::GetOrderedVisibleAttributeList();
	for (const FGuid& AttributeID : OrderedVisibleAttributes)
	{
		const FString& PropertyName = FMaterialAttributeDefinitionMap::GetAttributeName(AttributeID);
		const EMaterialValueType PropertyType = FMaterialAttributeDefinitionMap::GetValueType(AttributeID);
		const UE::Shader::EValueType ValueType = GetShaderType(PropertyType);
		
		if (ValueType != UE::Shader::EValueType::Void &&
			ValueType != UE::Shader::EValueType::Struct)
		{
			MaterialAttributeFields.Emplace(PropertyName, ValueType);

			if (PropertyType == MCT_ShadingModel)
			{
				check(ValueType == UE::Shader::EValueType::Int1);
				MaterialAttributesDefaultValue.Component.Add((int32)DefaultShadingModel);
			}
			else
			{
				const UE::Shader::FValue DefaultValue = UE::Shader::Cast(FMaterialAttributeDefinitionMap::GetDefaultValue(AttributeID), ValueType);
				for (int32 i = 0; i < DefaultValue.NumComponents; ++i)
				{
					MaterialAttributesDefaultValue.Component.Add(DefaultValue.Component[i]);
				}
			}
		}
	}

	UE::HLSLTree::FStructTypeInitializer MaterialAttributesInitializer;
	MaterialAttributesInitializer.Name = TEXT("FMaterialAttributes");
	MaterialAttributesInitializer.Fields = MaterialAttributeFields;
	MaterialAttributesType = InOutTree.NewStructType(MaterialAttributesInitializer);

	MaterialAttributesDefaultValue.Type = MaterialAttributesType;
}

void FMaterialHLSLGenerator::AcquireErrors(FMaterial& OutMaterial)
{
	OutMaterial.CompileErrors = MoveTemp(CompileErrors);
	OutMaterial.ErrorExpressions = MoveTemp(ErrorExpressions);
}

bool FMaterialHLSLGenerator::Finalize()
{
	check(FunctionCallStack.Num() == 1);

	if (!bGeneratedResult)
	{
		Error(TEXT("Missing connection to material output"));
		return false;
	}

	if (!ResultExpression || !ResultStatement)
	{
		Error(TEXT("Failed to initialize result"));
		return false;
	}

	for (const auto& It : StatementMap)
	{
		const UMaterialExpression* Expression = It.Key;
		const FStatementEntry& Entry = It.Value;
		if (Entry.NumInputs != Expression->NumExecutionInputs)
		{
			Error(TEXT("Invalid number of input connections"));
			return false;
		}
	}

	if (JoinedScopeStack.Num() != 0)
	{
		Error(TEXT("Invalid control flow"));
		return false;
	}

	// Resolve values for any PHI nodes that were generated
	// Resolving a PHI may produce additional PHIs
	while(PHIExpressions.Num() > 0)
	{
		UE::HLSLTree::FExpressionLocalPHI* Expression = PHIExpressions.Pop(false);
		for (int32 i = 0; i < Expression->NumValues; ++i)
		{
			Expression->Values[i] = InternalAcquireLocalValue(*Expression->Scopes[i], Expression->LocalName);
			if (!Expression->Values[i])
			{
				Errorf(TEXT("Local %s is not assigned on all control paths"), *Expression->LocalName.ToString());
				return false;
			}
		}
	}

	return true;
}

EMaterialGenerateHLSLStatus FMaterialHLSLGenerator::Error(const FString& Message)
{
	UMaterialExpression* ExpressionToError = nullptr;
	FString ErrorString;

	if (ExpressionStack.Num() > 0)
	{
		UMaterialExpression* ErrorExpression = ExpressionStack.Last().Expression;
		check(ErrorExpression);

		if (ErrorExpression->GetClass() != UMaterialExpressionMaterialFunctionCall::StaticClass()
			&& ErrorExpression->GetClass() != UMaterialExpressionFunctionInput::StaticClass()
			&& ErrorExpression->GetClass() != UMaterialExpressionFunctionOutput::StaticClass())
		{
			// Add the expression currently being compiled to ErrorExpressions so we can draw it differently
			ExpressionToError = ErrorExpression;

			const int32 ChopCount = FCString::Strlen(TEXT("MaterialExpression"));
			const FString ErrorClassName = ErrorExpression->GetClass()->GetName();

			// Add the node type to the error message
			ErrorString += FString(TEXT("(Node ")) + ErrorClassName.Right(ErrorClassName.Len() - ChopCount) + TEXT(") ");
		}
	}

	ErrorString += Message;

	// Standard error handling, immediately append one-off errors and signal failure
	CompileErrors.AddUnique(ErrorString);

	if (ExpressionToError)
	{
		ErrorExpressions.Add(ExpressionToError);
		ExpressionToError->LastErrorText = Message;
	}

	return EMaterialGenerateHLSLStatus::Error;
}

static UE::HLSLTree::FExpression* CompileMaterialInput(FMaterialHLSLGenerator& Generator,
	UE::HLSLTree::FScope& Scope,
	EMaterialProperty InputProperty,
	UMaterial* Material,
	UE::HLSLTree::FExpression* AttributesExpression)
{
#if 0
	UE::HLSLTree::FExpression* Expression = nullptr;
	if (Material->IsPropertyActive(InputProperty))
	{
		FMaterialInputDescription InputDescription;
		if (Material->GetExpressionInputDescription(InputProperty, InputDescription))
		{
			if (InputDescription.bUseConstant)
			{
				UE::Shader::FValue DefaultValue = FMaterialAttributeDefinitionMap::GetDefaultValue(InputProperty);
				DefaultValue.NumComponents = UE::Shader::GetValueTypeDescription(InputDescription.Type).NumComponents;
				if (InputDescription.ConstantValue != DefaultValue)
				{
					Expression = Generator.NewConstant(InputDescription.ConstantValue);
				}
			}
			else
			{
				check(InputDescription.Input);
				if (InputProperty >= MP_CustomizedUVs0 && InputProperty <= MP_CustomizedUVs7)
				{
					const int32 TexCoordIndex = (int32)InputProperty - MP_CustomizedUVs0;
					if (TexCoordIndex < Material->NumCustomizedUVs)
					{
						Expression = InputDescription.Input->AcquireHLSLExpression(Generator, Scope);
					}
					if (!Expression)
					{
						Expression = Generator.NewTexCoord(Scope, TexCoordIndex);
					}
				}
				else
				{
					Expression = InputDescription.Input->AcquireHLSLExpression(Generator, Scope);
				}
			}
		}
	}

	if (Expression)
	{
		UE::HLSLTree::FExpressionSetMaterialAttribute* SetAttributeExpression = Generator.GetTree().NewExpression<UE::HLSLTree::FExpressionSetMaterialAttribute>(Scope);
		SetAttributeExpression->AttributeID = FMaterialAttributeDefinitionMap::GetID(InputProperty);
		SetAttributeExpression->AttributesExpression = AttributesExpression;
		SetAttributeExpression->ValueExpression = Expression;
		return SetAttributeExpression;
	}
#endif
	return AttributesExpression;
}

bool FMaterialHLSLGenerator::GenerateResult(UE::HLSLTree::FScope& Scope)
{
	bool bResult = false;
	if (bGeneratedResult)
	{
		Error(TEXT("Multiple connections to execution output"));
	}
	else
	{
		check(!ResultStatement);
		check(!ResultExpression);

		if (TargetMaterial)
		{
			UE::HLSLTree::FExpression* AttributesExpression = nullptr;
			if (TargetMaterial->bUseMaterialAttributes)
			{
				FMaterialInputDescription InputDescription;
				if (TargetMaterial->GetExpressionInputDescription(MP_MaterialAttributes, InputDescription))
				{
					check(InputDescription.Type == UE::Shader::EValueType::Struct);
					AttributesExpression = InputDescription.Input->AcquireHLSLExpression(*this, Scope);
				}
			}
			else
			{
				AttributesExpression = HLSLTree->NewExpression<UE::HLSLTree::FExpressionConstant>(MaterialAttributesDefaultValue);
				for (int32 PropertyIndex = 0; PropertyIndex < MP_MAX; ++PropertyIndex)
				{
					const EMaterialProperty Property = (EMaterialProperty)PropertyIndex;
					AttributesExpression = CompileMaterialInput(*this, Scope, Property, TargetMaterial, AttributesExpression);
				}
			}

			if (AttributesExpression)
			{
				UE::HLSLTree::FStatementReturn* ReturnStatement = HLSLTree->NewStatement<UE::HLSLTree::FStatementReturn>(Scope);
				ReturnStatement->Expression = AttributesExpression;
				ResultExpression = AttributesExpression;
				ResultStatement = ReturnStatement;
				bResult = true;
			}
		}
		else
		{
			check(false);
		}
		bGeneratedResult = true;
	}
	return bResult;
}

UE::HLSLTree::FScope* FMaterialHLSLGenerator::NewScope(UE::HLSLTree::FScope& Scope, EMaterialNewScopeFlag Flags)
{
	UE::HLSLTree::FScope* NewScope = HLSLTree->NewScope(Scope);
	if (!EnumHasAllFlags(Flags, EMaterialNewScopeFlag::NoPreviousScope))
	{
		NewScope->AddPreviousScope(Scope);
	}

	return NewScope;
}

UE::HLSLTree::FScope* FMaterialHLSLGenerator::NewOwnedScope(UE::HLSLTree::FStatement& Owner)
{
	UE::HLSLTree::FScope* NewScope = HLSLTree->NewOwnedScope(Owner);
	NewScope->AddPreviousScope(*Owner.ParentScope);
	return NewScope;
}

UE::HLSLTree::FScope* FMaterialHLSLGenerator::NewJoinedScope(UE::HLSLTree::FScope& Scope)
{
	UE::HLSLTree::FScope* NewScope = HLSLTree->NewScope(Scope);
	JoinedScopeStack.Add(NewScope);
	return NewScope;
}

UE::HLSLTree::FExpressionConstant* FMaterialHLSLGenerator::NewConstant(const UE::Shader::FValue& Value)
{
	UE::HLSLTree::FExpressionConstant* Expression = HLSLTree->NewExpression<UE::HLSLTree::FExpressionConstant>(Value);
	return Expression;
}

UE::HLSLTree::FExpressionExternalInput* FMaterialHLSLGenerator::NewTexCoord(int32 Index)
{
	UE::HLSLTree::FExpressionExternalInput* Expression = HLSLTree->NewExpression<UE::HLSLTree::FExpressionExternalInput>(UE::HLSLTree::MakeInputTexCoord(Index));
	return Expression;
}

UE::HLSLTree::FExpressionSwizzle* FMaterialHLSLGenerator::NewSwizzle(const UE::HLSLTree::FSwizzleParameters& Params, UE::HLSLTree::FExpression* Input)
{
	UE::HLSLTree::FExpressionSwizzle* Expression = HLSLTree->NewExpression<UE::HLSLTree::FExpressionSwizzle>(Params, Input);
	return Expression;
}

UE::HLSLTree::FTextureParameterDeclaration* FMaterialHLSLGenerator::AcquireTextureDeclaration(const UE::HLSLTree::FTextureDescription& Value)
{
	FString SamplerTypeError;
	if (!UMaterialExpressionTextureBase::VerifySamplerType(CompileTarget.FeatureLevel, CompileTarget.TargetPlatform, Value.Texture, Value.SamplerType, SamplerTypeError))
	{
		Errorf(TEXT("%s"), *SamplerTypeError);
		return nullptr;
	}

	UE::HLSLTree::FTextureParameterDeclaration*& Declaration = TextureDeclarationMap.FindOrAdd(Value);
	if (!Declaration)
	{
		Declaration = HLSLTree->NewTextureParameterDeclaration(FName(), Value);
	}
	return Declaration;
}

UE::HLSLTree::FTextureParameterDeclaration* FMaterialHLSLGenerator::AcquireTextureParameterDeclaration(const FName& Name, const UE::HLSLTree::FTextureDescription& DefaultValue)
{
	FString SamplerTypeError;
	if (!UMaterialExpressionTextureBase::VerifySamplerType(CompileTarget.FeatureLevel, CompileTarget.TargetPlatform, DefaultValue.Texture, DefaultValue.SamplerType, SamplerTypeError))
	{
		Errorf(TEXT("%s"), *SamplerTypeError);
		return nullptr;
	}

	UE::HLSLTree::FTextureParameterDeclaration*& Declaration = TextureParameterDeclarationMap.FindOrAdd(Name);
	if (!Declaration)
	{
		Declaration = HLSLTree->NewTextureParameterDeclaration(Name, DefaultValue);
	}
	return Declaration;
}

bool FMaterialHLSLGenerator::GenerateAssignLocal(UE::HLSLTree::FScope& Scope, const FName& LocalName, UE::HLSLTree::FExpression* Value)
{
	const FLocalKey Key(&Scope, LocalName);
	LocalMap.Add(Key, Value);
	return true;
}

UE::HLSLTree::FExpression* FMaterialHLSLGenerator::InternalAcquireLocalValue(UE::HLSLTree::FScope& Scope, const FName& LocalName)
{
	const FLocalKey Key(&Scope, LocalName);
	UE::HLSLTree::FExpression** FoundExpression = LocalMap.Find(Key);
	if (FoundExpression)
	{
		return *FoundExpression;
	}

	const TArrayView<UE::HLSLTree::FScope*> PreviousScopes = Scope.GetPreviousScopes();
	if (PreviousScopes.Num() > 1)
	{
		UE::HLSLTree::FExpressionLocalPHI* Expression = HLSLTree->NewExpression<UE::HLSLTree::FExpressionLocalPHI>();
		Expression->LocalName = LocalName;
		Expression->NumValues = PreviousScopes.Num();
		for (int32 i = 0; i < PreviousScopes.Num(); ++i)
		{
			Expression->Scopes[i] = PreviousScopes[i];
		}
		PHIExpressions.Add(Expression);
		LocalMap.Add(Key, Expression);
		return Expression;
	}

	if (PreviousScopes.Num() == 1)
	{
		return InternalAcquireLocalValue(*PreviousScopes[0], LocalName);
	}

	return nullptr;
}

UE::HLSLTree::FExpression* FMaterialHLSLGenerator::AcquireLocalValue(UE::HLSLTree::FScope& Scope, const FName& LocalName)
{
	return InternalAcquireLocalValue(Scope, LocalName);
}

UE::HLSLTree::FExpression* FMaterialHLSLGenerator::AcquireExpression(UE::HLSLTree::FScope& Scope, UMaterialExpression* MaterialExpression, int32 OutputIndex)
{
	FFunctionCallEntry* FunctionEntry = FunctionCallStack.Last();

	const FExpressionKey Key(MaterialExpression, OutputIndex);
	UE::HLSLTree::FExpression** PrevExpression = FunctionEntry->ExpressionMap.Find(Key);
	UE::HLSLTree::FExpression* Expression = nullptr;
	if (!PrevExpression)
	{
		// TODO - need to rethink this caching, won't work to cache expressions that depend on current value of local variables
		// May just remove caching at this level....need to rework function inputs in this case
		ExpressionStack.Add(Key);
		const EMaterialGenerateHLSLStatus Status = MaterialExpression->GenerateHLSLExpression(*this, Scope, OutputIndex, Expression);
		verify(ExpressionStack.Pop() == Key);
		FunctionEntry->ExpressionMap.Add(Key, Expression);
	}
	else
	{
		Expression = *PrevExpression;
	}
	return Expression;
}

UE::HLSLTree::FTextureParameterDeclaration* FMaterialHLSLGenerator::AcquireTextureDeclaration(UE::HLSLTree::FScope& Scope, UMaterialExpression* MaterialExpression, int32 OutputIndex)
{
	// No need to cache at this level, TextureDeclarations are cached at a lower level, as they're generated by UMaterialExpression
	UE::HLSLTree::FTextureParameterDeclaration* TextureDeclaration = nullptr;
	const EMaterialGenerateHLSLStatus Status = MaterialExpression->GenerateHLSLTexture(*this, Scope, OutputIndex, TextureDeclaration);
	return TextureDeclaration;
}

bool FMaterialHLSLGenerator::GenerateStatements(UE::HLSLTree::FScope& Scope, UMaterialExpression* MaterialExpression)
{
	FStatementEntry& Entry = StatementMap.FindOrAdd(MaterialExpression);
	check(Entry.NumInputs >= 0);
	check(Entry.NumInputs < MaterialExpression->NumExecutionInputs);
	if (Entry.NumInputs == MaxNumPreviousScopes)
	{
		Error(TEXT("Bad control flow"));
		return false;
	}

	Entry.PreviousScope[Entry.NumInputs++] = &Scope;

	bool bResult = true;
	if (Entry.NumInputs == MaterialExpression->NumExecutionInputs)
	{
		UE::HLSLTree::FScope* ScopeToUse = &Scope;
		if (MaterialExpression->NumExecutionInputs > 1u)
		{
			if (JoinedScopeStack.Num() == 0)
			{
				Error(TEXT("Bad control flow"));
				return false;
			}

			ScopeToUse = JoinedScopeStack.Pop(false);
			for (int32 i = 0; i < Entry.NumInputs; ++i)
			{
				ScopeToUse->AddPreviousScope(*Entry.PreviousScope[i]);
			}
		}

		const FExpressionKey Key(MaterialExpression);
		ExpressionStack.Add(Key);
		const EMaterialGenerateHLSLStatus Status = MaterialExpression->GenerateHLSLStatements(*this, *ScopeToUse);
		verify(ExpressionStack.Pop() == Key);
	}

	return bResult;
}

template<typename T>
TArrayView<T> CopyArrayView(FMemStackBase& Allocator, TArrayView<T> InView)
{
	const int32 Num = InView.Num();
	T* Result = new(Allocator) T[Num];
	for (int32 i = 0; i < Num; ++i)
	{
		Result[i] = InView[i];
	}
	return MakeArrayView(Result, Num);
}

UE::HLSLTree::FExpression* FMaterialHLSLGenerator::GenerateFunctionCall(UE::HLSLTree::FScope& Scope, UMaterialFunctionInterface* Function, TArrayView<const FFunctionExpressionInput> Inputs, int32 OutputIndex)
{
	if (!Function)
	{
		Error(TEXT("Missing material function"));
		return nullptr;
	}

	TArray<FFunctionExpressionInput> FunctionInputs;
	TArray<FFunctionExpressionOutput> FunctionOutputs;
	Function->GetInputsAndOutputs(FunctionInputs, FunctionOutputs);

	if (FunctionInputs.Num() != Inputs.Num())
	{
		Error(TEXT("Mismatched function inputs"));
		return nullptr;
	}

	const UMaterialExpressionFunctionOutput* ExpressionOutput = FunctionOutputs.IsValidIndex(OutputIndex) ? FunctionOutputs[OutputIndex].ExpressionOutput.Get() : nullptr;
	if (!ExpressionOutput)
	{
		Error(TEXT("Invalid function output"));
		return nullptr;
	}

	FSHA1 Hasher;
	Hasher.Update((uint8*)&Function, sizeof(UMaterialFunctionInterface*));

	TArray<UE::HLSLTree::FExpression*> InputExpressions;
	InputExpressions.Reserve(Inputs.Num());
	for (int32 InputIndex = 0; InputIndex < Inputs.Num(); ++InputIndex)
	{
		const FFunctionExpressionInput& Input = Inputs[InputIndex];
		UE::HLSLTree::FExpression* InputExpression = Input.Input.AcquireHLSLExpression(*this, Scope);

		check(InputExpression);
		InputExpressions.Add(InputExpression);
		Hasher.Update((uint8*)&InputExpression, sizeof(UE::HLSLTree::FExpression*));
	}

	Hasher.Final();

	FSHAHash Hash;
	Hasher.GetHash(Hash.Hash);

	FFunctionCallEntry** ExistingFunctionCall = FunctionCallMap.Find(Hash);
	FFunctionCallEntry* FunctionCall = ExistingFunctionCall ? *ExistingFunctionCall : nullptr;
	if (!FunctionCall)
	{
		FunctionCall = new(HLSLTree->GetAllocator()) FFunctionCallEntry();
		FunctionCall->Function = Function;
		// Inject the function inputs into the function scope
		for (int32 InputIndex = 0; InputIndex < Inputs.Num(); ++InputIndex)
		{
			const FFunctionExpressionInput& Input = FunctionInputs[InputIndex];
			UE::HLSLTree::FExpression* InputExpression = InputExpressions[InputIndex];
			const FExpressionKey ExpressionKey(Input.ExpressionInput, 0);
			FunctionCall->ExpressionMap.Add(ExpressionKey, InputExpression);
		}

		FunctionCallMap.Add(Hash, FunctionCall);
	}

	FunctionCallStack.Add(FunctionCall);
	UE::HLSLTree::FExpression* Result = ExpressionOutput->A.AcquireHLSLExpression(*this, Scope);
	verify(FunctionCallStack.Pop() == FunctionCall);

	return Result;
}

void FMaterialHLSLGenerator::InternalRegisterExpressionData(const FName& Type, const UMaterialExpression* MaterialExpression, void* Data)
{
	const FExpressionDataKey Key(Type, MaterialExpression);
	check(!ExpressionDataMap.Contains(Key));
	ExpressionDataMap.Add(Key, Data);
}

void* FMaterialHLSLGenerator::InternalFindExpressionData(const FName& Type, const UMaterialExpression* MaterialExpression)
{
	const FExpressionDataKey Key(Type, MaterialExpression);
	void** Result = ExpressionDataMap.Find(Key);
	return Result ? *Result : nullptr;
}

#endif // WITH_EDITOR
