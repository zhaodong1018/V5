// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR

#include "Containers/Map.h"
#include "Misc/MemStack.h"
#include "Misc/EnumClassFlags.h"
#include "Templates/RefCounting.h"
#include "RHIDefinitions.h"
#include "HLSLTree/HLSLTree.h"

class FMaterial;
class FMaterialCompilationOutput;
struct FSharedShaderCompilerEnvironment;
struct FMaterialCompileTargetParameters;
class UMaterial;
class UMaterialFunctionInterface;
class UMaterialExpression;
class UMaterialExpressionFunctionInput;
class UMaterialExpressionFunctionOutput;
struct FFunctionExpressionInput;
class ITargetPlatform;
enum class EMaterialGenerateHLSLStatus : uint8;

namespace UE
{
namespace HLSLTree
{
struct FSwizzleParameters;
class FExpressionConstant;
class FExpressionExternalInput;
class FExpressionSwizzle;
class FExpressionLocalPHI;
}
}

enum class EMaterialNewScopeFlag : uint8
{
	None = 0u,
	NoPreviousScope = (1u << 0),
};
ENUM_CLASS_FLAGS(EMaterialNewScopeFlag);

template<typename T>
struct TMaterialHLSLGeneratorType;

#define DECLARE_MATERIAL_HLSLGENERATOR_DATA(T) \
	template<> struct TMaterialHLSLGeneratorType<T> { static const FName& GetTypeName() { static const FName Name(TEXT(#T)); return Name; } }

/**
 * MaterialHLSLGenerator is a bridge between a material, and HLSLTree.  It facilitates generating HLSL source code for a given material, using HLSLTree
 */
class FMaterialHLSLGenerator
{
public:
	FMaterialHLSLGenerator(UMaterial* InTargetMaterial, const FMaterialCompileTargetParameters& InCompilerTarget,
		UE::HLSLTree::FTree& InOutTree);

	const FMaterialCompileTargetParameters& GetCompileTarget() const { return CompileTarget; }

	bool Finalize();

	/** Retrieve the compile errors from the generator */
	void AcquireErrors(FMaterial& OutMaterial);

	EMaterialGenerateHLSLStatus Error(const FString& Message);

	template <typename FormatType, typename... ArgTypes>
	inline EMaterialGenerateHLSLStatus Errorf(const FormatType& Format, ArgTypes... Args)
	{
		return Error(FString::Printf(Format, Args...));
	}

	UE::HLSLTree::FTree& GetTree() const { return *HLSLTree; }

	UE::HLSLTree::FExpression* GetResultExpression() { return ResultExpression; }
	UE::HLSLTree::FStatement* GetResultStatement() { return ResultStatement; }

	bool GenerateResult(UE::HLSLTree::FScope& Scope);

	UE::HLSLTree::FScope* NewScope(UE::HLSLTree::FScope& Scope, EMaterialNewScopeFlag Flags = EMaterialNewScopeFlag::None);
	UE::HLSLTree::FScope* NewOwnedScope(UE::HLSLTree::FStatement& Owner);

	UE::HLSLTree::FScope* NewJoinedScope(UE::HLSLTree::FScope& Scope);

	UE::HLSLTree::FExpressionConstant* NewConstant(const UE::Shader::FValue& Value);
	UE::HLSLTree::FExpressionExternalInput* NewTexCoord(int32 Index);
	UE::HLSLTree::FExpressionSwizzle* NewSwizzle(const UE::HLSLTree::FSwizzleParameters& Params, UE::HLSLTree::FExpression* Input);

	/** Returns a declaration to access the given texture, with no parameter */
	UE::HLSLTree::FTextureParameterDeclaration* AcquireTextureDeclaration(const UE::HLSLTree::FTextureDescription& Value);

	/** Returns a declaration to access the given texture parameter */
	UE::HLSLTree::FTextureParameterDeclaration* AcquireTextureParameterDeclaration(const FName& Name, const UE::HLSLTree::FTextureDescription& DefaultValue);

	bool GenerateAssignLocal(UE::HLSLTree::FScope& Scope, const FName& LocalName, UE::HLSLTree::FExpression* Value);
	UE::HLSLTree::FExpression* AcquireLocalValue(UE::HLSLTree::FScope& Scope, const FName& LocalName);

	/**
	 * Returns the appropriate HLSLNode representing the given UMaterialExpression.
	 * The node will be created if it doesn't exist. Otherwise, the tree will be updated to ensure the node is visible in the given scope
	 * Note that a given UMaterialExpression may only support 1 of these node types, attempting to access an invalid node type will generate an error
	 */
	UE::HLSLTree::FExpression* AcquireExpression(UE::HLSLTree::FScope& Scope, UMaterialExpression* MaterialExpression, int32 OutputIndex);
	UE::HLSLTree::FTextureParameterDeclaration* AcquireTextureDeclaration(UE::HLSLTree::FScope& Scope, UMaterialExpression* MaterialExpression, int32 OutputIndex);
	bool GenerateStatements(UE::HLSLTree::FScope& Scope, UMaterialExpression* MaterialExpression);

	UE::HLSLTree::FExpression* GenerateFunctionCall(UE::HLSLTree::FScope& Scope, UMaterialFunctionInterface* Function, TArrayView<const FFunctionExpressionInput> Inputs, int32 OutputIndex);

	template<typename T, typename... ArgTypes>
	T* NewExpressionData(const UMaterialExpression* MaterialExpression, ArgTypes... Args)
	{
		T* Data = new T(Forward<ArgTypes>(Args)...);
		InternalRegisterExpressionData(TMaterialHLSLGeneratorType<T>::GetTypeName(), MaterialExpression, Data);
		return Data;
	}

	template<typename T>
	T* FindExpressionData(const UMaterialExpression* MaterialExpression)
	{
		return static_cast<T*>(InternalFindExpressionData(TMaterialHLSLGeneratorType<T>::GetTypeName(), MaterialExpression));
	}

	template<typename T>
	T* AcquireGlobalData()
	{
		T* Data = static_cast<T*>(InternalFindExpressionData(TMaterialHLSLGeneratorType<T>::GetTypeName(), nullptr));
		if (!Data)
		{
			Data = new T();
			InternalRegisterExpressionData(TMaterialHLSLGeneratorType<T>::GetTypeName(), nullptr, Data);
		}
		return Data;
	}

	const UE::HLSLTree::FStructType* GetMaterialAttributesType() const { return MaterialAttributesType; }
	const UE::HLSLTree::FConstantValue& GetMaterialAttributesDefaultValue() const { return MaterialAttributesDefaultValue; }

private:
	static constexpr int32 MaxNumPreviousScopes = UE::HLSLTree::MaxNumPreviousScopes;
	struct FExpressionKey
	{
		explicit FExpressionKey(UMaterialExpression* InExpression, int32 InOutputIndex = INDEX_NONE) : Expression(InExpression), OutputIndex(InOutputIndex) {}

		UMaterialExpression* Expression;
		int32 OutputIndex;

		friend inline uint32 GetTypeHash(const FExpressionKey& Value)
		{
			return HashCombine(GetTypeHash(Value.Expression), GetTypeHash(Value.OutputIndex));
		}

		friend inline bool operator==(const FExpressionKey& Lhs, const FExpressionKey& Rhs)
		{
			return Lhs.Expression == Rhs.Expression && Lhs.OutputIndex == Rhs.OutputIndex;
		}
	};

	struct FLocalKey
	{
		FLocalKey(UE::HLSLTree::FScope* InScope, const FName& InName) : Scope(InScope), Name(InName) {}

		UE::HLSLTree::FScope* Scope;
		FName Name;

		friend inline uint32 GetTypeHash(const FLocalKey& Value)
		{
			return HashCombine(GetTypeHash(Value.Scope), GetTypeHash(Value.Name));
		}

		friend inline bool operator==(const FLocalKey& Lhs, const FLocalKey& Rhs)
		{
			return Lhs.Scope == Rhs.Scope && Lhs.Name == Rhs.Name;
		}
	};

	struct FExpressionDataKey
	{
		FExpressionDataKey(const FName& InTypeName, const UMaterialExpression* InMaterialExpression) : MaterialExpression(InMaterialExpression), TypeName(InTypeName) {}

		const UMaterialExpression* MaterialExpression;
		FName TypeName;

		friend inline uint32 GetTypeHash(const FExpressionDataKey& Value)
		{
			return HashCombine(GetTypeHash(Value.MaterialExpression), GetTypeHash(Value.TypeName));
		}

		friend inline bool operator==(const FExpressionDataKey& Lhs, const FExpressionDataKey& Rhs)
		{
			return Lhs.MaterialExpression == Rhs.MaterialExpression && Lhs.TypeName == Rhs.TypeName;
		}
	};

	struct FFunctionCallEntry
	{
		UMaterialFunctionInterface* Function = nullptr;
		TMap<FExpressionKey, UE::HLSLTree::FExpression*> ExpressionMap;
	};

	struct FStatementEntry
	{
		UE::HLSLTree::FScope* PreviousScope[MaxNumPreviousScopes];
		int32 NumInputs = 0;
	};

	UE::HLSLTree::FExpression* InternalAcquireLocalValue(UE::HLSLTree::FScope& Scope, const FName& LocalName);

	void InternalRegisterExpressionData(const FName& Type, const UMaterialExpression* MaterialExpression, void* Data);
	void* InternalFindExpressionData(const FName& Type, const UMaterialExpression* MaterialExpression);

	const FMaterialCompileTargetParameters& CompileTarget;
	UMaterial* TargetMaterial;

	const UE::HLSLTree::FStructType* MaterialAttributesType;
	UE::HLSLTree::FConstantValue MaterialAttributesDefaultValue;

	UE::HLSLTree::FTree* HLSLTree;
	UE::HLSLTree::FExpression* ResultExpression = nullptr;
	UE::HLSLTree::FStatement* ResultStatement = nullptr;

	TArray<FExpressionKey> ExpressionStack;
	TArray<FFunctionCallEntry*> FunctionCallStack;
	TArray<UE::HLSLTree::FScope*> JoinedScopeStack;
	TArray<UE::HLSLTree::FExpressionLocalPHI*> PHIExpressions;
	TArray<FString> CompileErrors;
	TArray<UMaterialExpression*> ErrorExpressions;
	TMap<UE::HLSLTree::FTextureDescription, UE::HLSLTree::FTextureParameterDeclaration*> TextureDeclarationMap;
	TMap<FName, UE::HLSLTree::FTextureParameterDeclaration*> TextureParameterDeclarationMap;
	TMap<FSHAHash, FFunctionCallEntry*> FunctionCallMap;
	TMap<FLocalKey, UE::HLSLTree::FExpression*> LocalMap;
	TMap<UMaterialExpression*, FStatementEntry> StatementMap;
	TMap<FExpressionDataKey, void*> ExpressionDataMap;
	bool bGeneratedResult;
};

#endif // WITH_EDITOR
