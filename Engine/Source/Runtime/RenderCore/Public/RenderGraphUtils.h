// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "RenderGraphResources.h"
#include "RenderGraphBuilder.h"
#include "Shader.h"
#include "ShaderParameterStruct.h"
#include "ShaderParameterMacros.h"
#include "RHIGPUReadback.h"

class FGlobalShaderMap;

/** Returns whether the resource was produced by a prior pass. */
inline bool HasBeenProduced(FRDGParentResourceRef Resource)
{
	return Resource && Resource->HasBeenProduced();
}

/** Returns the texture if it was produced by a prior pass, or null otherwise. */
inline FRDGTextureRef GetIfProduced(FRDGTextureRef Texture, FRDGTextureRef FallbackTexture = nullptr)
{
	return HasBeenProduced(Texture) ? Texture : FallbackTexture;
}

/** Returns the buffer if has been produced by a prior pass, or null otherwise. */
inline FRDGBufferRef GetIfProduced(FRDGBufferRef Buffer, FRDGBufferRef FallbackBuffer = nullptr)
{
	return HasBeenProduced(Buffer) ? Buffer : FallbackBuffer;
}

/** Returns 'Load' if the texture has already been produced by a prior pass, or the requested initial action. */
inline ERenderTargetLoadAction GetLoadActionIfProduced(FRDGTextureRef Texture, ERenderTargetLoadAction ActionIfNotProduced)
{
	return HasBeenProduced(Texture) ? ERenderTargetLoadAction::ELoad : ActionIfNotProduced;
}

/** Returns a binding with the requested initial action, or a load action if the resource has been produced by a prior pass. */
inline FRenderTargetBinding GetLoadBindingIfProduced(FRDGTextureRef Texture, ERenderTargetLoadAction ActionIfNotProduced)
{
	return FRenderTargetBinding(Texture, GetLoadActionIfProduced(Texture, ActionIfNotProduced));
}

/** Returns the RHI texture from an RDG texture if it exists, or null otherwise. */
inline FRHITexture* TryGetRHI(FRDGTextureRef Texture)
{
	return Texture ? Texture->GetRHI() : nullptr;
}

/** Returns the pooled render target from an RDG texture if it exists, or null otherwise. */
UE_DEPRECATED(5.0, "Accessing the underlying pooled render target has been deprecated. Use TryGetRHI() instead.")
inline IPooledRenderTarget* TryGetPooledRenderTarget(FRDGTextureRef Texture)
{
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	return Texture ? Texture->GetPooledRenderTarget() : nullptr;
PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

inline FRenderTargetBindingSlots GetRenderTargetBindings(ERenderTargetLoadAction ColorLoadAction, TArrayView<FRDGTextureRef> ColorTextures)
{
	check(ColorTextures.Num() <= MaxSimultaneousRenderTargets);

	FRenderTargetBindingSlots BindingSlots;
	for (int32 Index = 0, Count = ColorTextures.Num(); Index < Count; ++Index)
	{
		check(ColorTextures[Index]);
		BindingSlots[Index] = FRenderTargetBinding(ColorTextures[Index], ColorLoadAction);
	}
	return BindingSlots;
}

struct FTextureRenderTargetBinding
{
	FRDGTextureRef Texture;
	int16 ArraySlice;
	ERenderTargetLoadAction OverrideLoadAction;

	FTextureRenderTargetBinding()
		: Texture(nullptr)
		, ArraySlice(-1)
		, OverrideLoadAction(ERenderTargetLoadAction::Num)
	{}

	FTextureRenderTargetBinding(FRDGTextureRef InTexture, ERenderTargetLoadAction InOverrideLoadAction)
		: Texture(InTexture)
		, ArraySlice(-1)
		, OverrideLoadAction(InOverrideLoadAction)
	{}

	FTextureRenderTargetBinding(FRDGTextureRef InTexture, int16 InArraySlice = -1, ERenderTargetLoadAction InOverrideLoadAction = ERenderTargetLoadAction::Num)
		: Texture(InTexture)
		, ArraySlice(InArraySlice)
		, OverrideLoadAction(InOverrideLoadAction)
	{}
};
inline FRenderTargetBindingSlots GetRenderTargetBindings(ERenderTargetLoadAction ColorLoadAction, TArrayView<FTextureRenderTargetBinding> ColorTextures)
{
	check(ColorTextures.Num() <= MaxSimultaneousRenderTargets);

	FRenderTargetBindingSlots BindingSlots;
	for (int32 Index = 0, Count = ColorTextures.Num(); Index < Count; ++Index)
	{
		check(ColorTextures[Index].Texture);
		BindingSlots[Index] = FRenderTargetBinding(ColorTextures[Index].Texture, ColorLoadAction, 0, ColorTextures[Index].ArraySlice);
		if (ColorTextures[Index].OverrideLoadAction != ERenderTargetLoadAction::Num)
		{
			BindingSlots[Index].SetLoadAction(ColorTextures[Index].OverrideLoadAction);
		}
	}
	return BindingSlots;
}

/**
 * Clears all render graph tracked resources that are not bound by a shader.
 * Excludes any resources on the ExcludeList from being cleared regardless of whether the 
 * shader binds them or not. This is needed for resources that are used outside of shader
 * bindings such as indirect arguments buffers.
 */
extern RENDERCORE_API void ClearUnusedGraphResourcesImpl(
	const FShaderParameterBindings& ShaderBindings,
	const FShaderParametersMetadata* ParametersMetadata,
	void* InoutParameters,
	std::initializer_list<FRDGResourceRef> ExcludeList);

/** Similar to the function above, but takes a list of shader bindings and only clears if none of the shaders contain the resource. */
extern RENDERCORE_API void ClearUnusedGraphResourcesImpl(
	TArrayView<const FShaderParameterBindings*> ShaderBindingsList,
	const FShaderParametersMetadata* ParametersMetadata,
	void* InoutParameters,
	std::initializer_list<FRDGResourceRef> ExcludeList);

template <typename TShaderClass>
void ClearUnusedGraphResources(
	const TShaderRef<TShaderClass>& Shader,
	const FShaderParametersMetadata* ParametersMetadata,
	typename TShaderClass::FParameters* InoutParameters,
	std::initializer_list<FRDGResourceRef> ExcludeList = {})
{
	// Verify the shader have all the parameters it needs. This is done before the
	// ClearUnusedGraphResourcesImpl() to not mislead user on why some resource are missing
	// when debugging a validation failure.
	ValidateShaderParameters(Shader, ParametersMetadata, InoutParameters);

	// Clear the resources the shader won't need.
	return ClearUnusedGraphResourcesImpl(Shader->Bindings, ParametersMetadata, InoutParameters, ExcludeList);
}

template <typename TShaderClass>
void ClearUnusedGraphResources(
	const TShaderRef<TShaderClass>& Shader,
	typename TShaderClass::FParameters* InoutParameters,
	std::initializer_list<FRDGResourceRef> ExcludeList = {})
{
	const FShaderParametersMetadata* ParametersMetadata = TShaderClass::FParameters::FTypeInfo::GetStructMetadata();
	return ClearUnusedGraphResources(Shader, ParametersMetadata, InoutParameters, MoveTemp(ExcludeList));
}

template <typename TShaderClassA, typename TShaderClassB, typename TPassParameterStruct>
void ClearUnusedGraphResources(
	const TShaderRef<TShaderClassA>& ShaderA,
	const TShaderRef<TShaderClassB>& ShaderB,
	TPassParameterStruct* InoutParameters,
	std::initializer_list<FRDGResourceRef> ExcludeList = {})
{
	static_assert(TIsSame<typename TShaderClassA::FParameters, TPassParameterStruct>::Value, "First shader FParameter type must match pass parameters.");
	static_assert(TIsSame<typename TShaderClassB::FParameters, TPassParameterStruct>::Value, "Second shader FParameter type must match pass parameters.");
	const FShaderParametersMetadata* ParametersMetadata = TPassParameterStruct::FTypeInfo::GetStructMetadata();

	// Verify the shader have all the parameters it needs. This is done before the
	// ClearUnusedGraphResourcesImpl() to not mislead user on why some resource are missing
	// when debugging a validation failure.
	ValidateShaderParameters(ShaderA, ParametersMetadata, InoutParameters);
	ValidateShaderParameters(ShaderB, ParametersMetadata, InoutParameters);

	// Clear the resources the shader won't need.
	const FShaderParameterBindings* ShaderBindings[] = { &ShaderA->Bindings, &ShaderB->Bindings };
	return ClearUnusedGraphResourcesImpl(ShaderBindings, ParametersMetadata, InoutParameters, ExcludeList);
}

/**
 * Register external texture with fallback if the resource is invalid.
 *
 * CAUTION: use this function very wisely. It may actually remove shader parameter validation
 * failure when a pass is actually trying to access a resource not yet or no longer available.
 */
RENDERCORE_API FRDGTextureRef RegisterExternalTextureWithFallback(
	FRDGBuilder& GraphBuilder,
	const TRefCountPtr<IPooledRenderTarget>& ExternalPooledTexture,
	const TRefCountPtr<IPooledRenderTarget>& FallbackPooledTexture,
	ERenderTargetTexture ExternalTexture = ERenderTargetTexture::ShaderResource,
	ERenderTargetTexture FallbackTexture = ERenderTargetTexture::ShaderResource);

/** Variants of RegisterExternalTexture which will returns null (rather than assert) if the external texture is null. */
inline FRDGTextureRef TryRegisterExternalTexture(
	FRDGBuilder& GraphBuilder,
	const TRefCountPtr<IPooledRenderTarget>& ExternalPooledTexture,
	ERenderTargetTexture RenderTargetTexture = ERenderTargetTexture::ShaderResource,
	ERDGTextureFlags Flags = ERDGTextureFlags::None)
{
	return ExternalPooledTexture ? GraphBuilder.RegisterExternalTexture(ExternalPooledTexture, RenderTargetTexture, Flags) : nullptr;
}

/** Variants of RegisterExternalBuffer which will return null (rather than assert) if the external buffer is null. */
inline FRDGBufferRef TryRegisterExternalBuffer(
	FRDGBuilder& GraphBuilder,
	const TRefCountPtr<FRDGPooledBuffer>& ExternalPooledBuffer,
	ERDGBufferFlags Flags = ERDGBufferFlags::None)
{
	return ExternalPooledBuffer ? GraphBuilder.RegisterExternalBuffer(ExternalPooledBuffer, Flags) : nullptr;
}

inline FRDGTextureRef RegisterExternalTexture(FRDGBuilder& GraphBuilder, FRHITexture* Texture, const TCHAR* NameIfUnregistered)
{
	if (FRDGTextureRef FoundTexture = GraphBuilder.FindExternalTexture(Texture))
	{
		return FoundTexture;
	}

	return GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Texture, NameIfUnregistered));
}

/** Simple pair of RDG textures used for MSAA. */
struct FRDGTextureMSAA
{
	FRDGTextureMSAA() = default;

	FRDGTextureMSAA(FRDGTextureRef InTarget, FRDGTextureRef InResolve)
		: Target(InTarget)
		, Resolve(InResolve)
	{}

	FRDGTextureMSAA(FRDGTextureRef InTexture)
		: Target(InTexture)
		, Resolve(InTexture)
	{}

	bool IsValid() const
	{
		return Target != nullptr && Resolve != nullptr;
	}

	bool IsSeparate() const
	{
		return Target != Resolve;
	}

	bool operator==(FRDGTextureMSAA Other) const
	{
		return Target == Other.Target && Resolve == Other.Resolve;
	}

	bool operator!=(FRDGTextureMSAA Other) const
	{
		return !(*this == Other);
	}

	FRDGTextureRef Target = nullptr;
	FRDGTextureRef Resolve = nullptr;
};

RENDERCORE_API FRDGTextureMSAA CreateTextureMSAA(
	FRDGBuilder& GraphBuilder,
	FRDGTextureDesc Desc,
	const TCHAR* Name,
	ETextureCreateFlags ResolveFlagsToAdd = TexCreate_None);

inline FRDGTextureMSAA RegisterExternalTextureMSAA(
	FRDGBuilder& GraphBuilder,
	const TRefCountPtr<IPooledRenderTarget>& ExternalPooledTexture)
{
	return FRDGTextureMSAA(
		GraphBuilder.RegisterExternalTexture(ExternalPooledTexture, ERenderTargetTexture::Targetable),
		GraphBuilder.RegisterExternalTexture(ExternalPooledTexture, ERenderTargetTexture::ShaderResource));
}

inline FRDGTextureMSAA TryRegisterExternalTextureMSAA(
	FRDGBuilder& GraphBuilder,
	const TRefCountPtr<IPooledRenderTarget>& ExternalPooledTexture)
{
	return FRDGTextureMSAA(
		TryRegisterExternalTexture(GraphBuilder, ExternalPooledTexture, ERenderTargetTexture::Targetable),
		TryRegisterExternalTexture(GraphBuilder, ExternalPooledTexture, ERenderTargetTexture::ShaderResource));
}

RENDERCORE_API FRDGTextureMSAA RegisterExternalTextureMSAAWithFallback(
	FRDGBuilder& GraphBuilder,
	const TRefCountPtr<IPooledRenderTarget>& ExternalPooledTexture,
	const TRefCountPtr<IPooledRenderTarget>& FallbackPooledTexture);

/** All utils for compute shaders.
 */
struct RENDERCORE_API FComputeShaderUtils
{
	/** Ideal size of group size 8x8 to occupy at least an entire wave on GCN, two warp on Nvidia. */
	static constexpr int32 kGolden2DGroupSize = 8;

	/** Compute the number of groups to dispatch. */
	static FIntVector GetGroupCount(const int32 ThreadCount, const int32 GroupSize)
	{
		return FIntVector(
			FMath::DivideAndRoundUp(ThreadCount, GroupSize),
			1,
			1);
	}
	static FIntVector GetGroupCount(const FIntPoint& ThreadCount, const FIntPoint& GroupSize)
	{
		return FIntVector(
			FMath::DivideAndRoundUp(ThreadCount.X, GroupSize.X),
			FMath::DivideAndRoundUp(ThreadCount.Y, GroupSize.Y),
			1);
	}
	static FIntVector GetGroupCount(const FIntPoint& ThreadCount, const int32 GroupSize)
	{
		return FIntVector(
			FMath::DivideAndRoundUp(ThreadCount.X, GroupSize),
			FMath::DivideAndRoundUp(ThreadCount.Y, GroupSize),
			1);
	}
	static FIntVector GetGroupCount(const FIntVector& ThreadCount, const FIntVector& GroupSize)
	{
		return FIntVector(
			FMath::DivideAndRoundUp(ThreadCount.X, GroupSize.X),
			FMath::DivideAndRoundUp(ThreadCount.Y, GroupSize.Y),
			FMath::DivideAndRoundUp(ThreadCount.Z, GroupSize.Z));
	}
	static FIntVector GetGroupCount(const FIntVector& ThreadCount, const int32 GroupSize)
	{
		return FIntVector(
			FMath::DivideAndRoundUp(ThreadCount.X, GroupSize),
			FMath::DivideAndRoundUp(ThreadCount.Y, GroupSize),
			FMath::DivideAndRoundUp(ThreadCount.Z, GroupSize));
	}



	/**
	 * Constant stride used when wrapping too large 1D dispatches using GetGroupCountWrapped, selected as 128 appears to be the lowest common denominator 
	 * for mobile (GLES 3.1). For PC (with ~64k groups / dimension) this yields ~8M groups (500M threads @ group size 64) before even wrapping into Z.
	 * NOTE: this value must match WRAPPED_GROUP_STRIDE in ComputeShaderUtils.ush
	 */
	static constexpr int32 WrappedGroupStride = 128;

	/**
	 * Wrapping number of groups to Y and Z dimension if X group count overflows GRHIMaxDispatchThreadGroupsPerDimension.
	 * Calculate the linear group index as (or use GetUnWrappedDispatchGroupId(GroupId) in ComputeShaderUtils.ush):
	 *  uint LinearGroupId = GroupId.X + (GroupId.Z * WrappedGroupStride + GroupId.Y) * WrappedGroupStride;
	 * Note that you must use an early out because LinearGroupId may be larger than the ideal due to wrapping.
	 */
	static FIntVector GetGroupCountWrapped(const int32 TargetGroupCount)
	{
		check(GRHIMaxDispatchThreadGroupsPerDimension.X >= WrappedGroupStride && GRHIMaxDispatchThreadGroupsPerDimension.Y >= WrappedGroupStride);

		FIntVector GroupCount(TargetGroupCount, 1, 1);

		if (GroupCount.X > GRHIMaxDispatchThreadGroupsPerDimension.X)
		{
			GroupCount.Y = FMath::DivideAndRoundUp(GroupCount.X, WrappedGroupStride);
			GroupCount.X = WrappedGroupStride;
		}
		if (GroupCount.Y > GRHIMaxDispatchThreadGroupsPerDimension.Y)
		{
			GroupCount.Z = FMath::DivideAndRoundUp(GroupCount.Y, WrappedGroupStride);
			GroupCount.Y = WrappedGroupStride;
		}

		check(TargetGroupCount <= GroupCount.X * GroupCount.Y * GroupCount.Z);

		return GroupCount;
	}

	/**
	 * Compute the number of groups to dispatch and allow wrapping to Y and Z dimension if X group count overflows. 
	 * Calculate the linear group index as (or use GetUnWrappedDispatchGroupId(GroupId) in ComputeShaderUtils.ush):
	 *  uint LinearGroupId = GroupId.X + (GroupId.Z * WrappedGroupStride + GroupId.Y) * WrappedGroupStride;
	 * Note that you must use an early out because LinearGroupId may be larger than the ideal due to wrapping.
	 */
	static FIntVector GetGroupCountWrapped(const int32 ThreadCount, const int32 GroupSize)
	{
		return GetGroupCountWrapped(FMath::DivideAndRoundUp(ThreadCount, GroupSize));
	}


	/** Dispatch a compute shader to rhi command list with its parameters. */
	template<typename TShaderClass>
	static void Dispatch(
		FRHIComputeCommandList& RHICmdList, 
		const TShaderRef<TShaderClass>& ComputeShader, 
		const FShaderParametersMetadata* ParametersMetadata,
		const typename TShaderClass::FParameters& Parameters,
		FIntVector GroupCount)
	{
		ValidateGroupCount(GroupCount);
		FRHIComputeShader* ShaderRHI = ComputeShader.GetComputeShader();
		RHICmdList.SetComputeShader(ShaderRHI);
		SetShaderParameters(RHICmdList, ComputeShader, ShaderRHI, ParametersMetadata, Parameters);
		RHICmdList.DispatchComputeShader(GroupCount.X, GroupCount.Y, GroupCount.Z);
		UnsetShaderUAVs(RHICmdList, ComputeShader, ShaderRHI);
	}

	template<typename TShaderClass>
	static void Dispatch(
		FRHIComputeCommandList& RHICmdList,
		const TShaderRef<TShaderClass>& ComputeShader,
		const typename TShaderClass::FParameters& Parameters,
		FIntVector GroupCount)
	{
		const FShaderParametersMetadata* ParametersMetadata = TShaderClass::FParameters::FTypeInfo::GetStructMetadata();
		Dispatch(RHICmdList, ComputeShader, ParametersMetadata, Parameters, GroupCount);
	}
	
	/** Indirect dispatch a compute shader to rhi command list with its parameters. */
	template<typename TShaderClass>
	static void DispatchIndirect(
		FRHIComputeCommandList& RHICmdList,
		const TShaderRef<TShaderClass>& ComputeShader,
		const typename TShaderClass::FParameters& Parameters,
		FRHIBuffer* IndirectArgsBuffer,
		uint32 IndirectArgOffset)
	{
		ValidateIndirectArgsBuffer(IndirectArgsBuffer->GetSize(), IndirectArgOffset);
		FRHIComputeShader* ShaderRHI = ComputeShader.GetComputeShader();
		RHICmdList.SetComputeShader(ShaderRHI);
		SetShaderParameters(RHICmdList, ComputeShader, ShaderRHI, Parameters);
		RHICmdList.DispatchIndirectComputeShader(IndirectArgsBuffer, IndirectArgOffset);
		UnsetShaderUAVs(RHICmdList, ComputeShader, ShaderRHI);
	}

	/** Dispatch a compute shader to rhi command list with its parameters and indirect args. */
	template<typename TShaderClass>
	static FORCEINLINE_DEBUGGABLE void DispatchIndirect(
		FRHIComputeCommandList& RHICmdList,
		const TShaderRef<TShaderClass>& ComputeShader,
		const typename TShaderClass::FParameters& Parameters,
		FRDGBufferRef IndirectArgsBuffer,
		uint32 IndirectArgOffset)
	{
		ValidateIndirectArgsBuffer(IndirectArgsBuffer, IndirectArgOffset);
		FRHIComputeShader* ShaderRHI = ComputeShader.GetComputeShader();
		RHICmdList.SetComputeShader(ShaderRHI);
		SetShaderParameters(RHICmdList, ComputeShader, ShaderRHI, Parameters);
		RHICmdList.DispatchIndirectComputeShader(IndirectArgsBuffer->GetIndirectRHICallBuffer(), IndirectArgOffset);
		UnsetShaderUAVs(RHICmdList, ComputeShader, ShaderRHI);
	}

	/** Dispatch a compute shader to render graph builder with its parameters. */
	template<typename TShaderClass>
	static void AddPass(
		FRDGBuilder& GraphBuilder,
		FRDGEventName&& PassName,
		ERDGPassFlags PassFlags,
		const TShaderRef<TShaderClass>& ComputeShader,
		const FShaderParametersMetadata* ParametersMetadata,
		typename TShaderClass::FParameters* Parameters,
		FIntVector GroupCount)
	{
		checkf(
			 EnumHasAnyFlags(PassFlags, ERDGPassFlags::Compute | ERDGPassFlags::AsyncCompute) &&
			!EnumHasAnyFlags(PassFlags, ERDGPassFlags::Copy | ERDGPassFlags::Raster), TEXT("AddPass only supports 'Compute' or 'AsyncCompute'."));

		ValidateGroupCount(GroupCount);
		ClearUnusedGraphResources(ComputeShader, ParametersMetadata, Parameters);

		GraphBuilder.AddPass(
			Forward<FRDGEventName>(PassName),
			ParametersMetadata,
			Parameters,
			PassFlags,
			[ParametersMetadata, Parameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
		{
			FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, ParametersMetadata, *Parameters, GroupCount);
		});
	}

	/** Dispatch a compute shader to render graph builder with its parameters. GroupCount is supplied through a callback.
	 *  This allows adding a dispatch with unknown GroupCount but the value must be ready before the pass is executed.
	 */
	template<typename TShaderClass>
	static void AddPass(
		FRDGBuilder& GraphBuilder,
		FRDGEventName&& PassName,
		ERDGPassFlags PassFlags,
		const TShaderRef<TShaderClass>& ComputeShader,
		const FShaderParametersMetadata* ParametersMetadata,
		typename TShaderClass::FParameters* Parameters,
		FRDGDispatchGroupCountCallback&& GroupCountCallback)
	{
		checkf(
			EnumHasAnyFlags(PassFlags, ERDGPassFlags::Compute | ERDGPassFlags::AsyncCompute) &&
			!EnumHasAnyFlags(PassFlags, ERDGPassFlags::Copy | ERDGPassFlags::Raster), TEXT("AddPass only supports 'Compute' or 'AsyncCompute'."));

		ClearUnusedGraphResources(ComputeShader, ParametersMetadata, Parameters);

		GraphBuilder.AddPass(
			Forward<FRDGEventName>(PassName),
			ParametersMetadata,
			Parameters,
			PassFlags,
			[ParametersMetadata, Parameters, ComputeShader, GroupCountCallback = MoveTemp(GroupCountCallback)](FRHIComputeCommandList& RHICmdList)
			{
				const FIntVector GroupCount = GroupCountCallback();
				if (GroupCount.X > 0 && GroupCount.Y > 0 && GroupCount.Z > 0)
				{
					ValidateGroupCount(GroupCount);
					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, ParametersMetadata, *Parameters, GroupCount);
				}
			});
	}

	template<typename TShaderClass>
	static void AddPass(
		FRDGBuilder& GraphBuilder,
		FRDGEventName&& PassName,
		ERDGPassFlags PassFlags,
		const TShaderRef<TShaderClass>& ComputeShader,
		typename TShaderClass::FParameters* Parameters,
		FIntVector GroupCount)
	{
		const FShaderParametersMetadata* ParametersMetadata = TShaderClass::FParameters::FTypeInfo::GetStructMetadata();
		AddPass(GraphBuilder, Forward<FRDGEventName>(PassName), PassFlags, ComputeShader, ParametersMetadata, Parameters, GroupCount);
	}

	template <typename TShaderClass>
	static FORCEINLINE void AddPass(
		FRDGBuilder& GraphBuilder,
		FRDGEventName&& PassName,
		const TShaderRef<TShaderClass>& ComputeShader,
		typename TShaderClass::FParameters* Parameters,
		FIntVector GroupCount)
	{
		const FShaderParametersMetadata* ParametersMetadata = TShaderClass::FParameters::FTypeInfo::GetStructMetadata();
		AddPass(GraphBuilder, Forward<FRDGEventName>(PassName), ERDGPassFlags::Compute, ComputeShader, ParametersMetadata, Parameters, GroupCount);
	}

	template <typename TShaderClass>
	static FORCEINLINE void AddPass(
		FRDGBuilder& GraphBuilder,
		FRDGEventName&& PassName,
		const TShaderRef<TShaderClass>& ComputeShader,
		typename TShaderClass::FParameters* Parameters,
		FRDGDispatchGroupCountCallback&& GroupCountCallback)
	{
		const FShaderParametersMetadata* ParametersMetadata = TShaderClass::FParameters::FTypeInfo::GetStructMetadata();
		AddPass(GraphBuilder, Forward<FRDGEventName>(PassName), ERDGPassFlags::Compute, ComputeShader, ParametersMetadata, Parameters, MoveTemp(GroupCountCallback));
	}

	/** Dispatch a compute shader to render graph builder with its parameters. */
	template<typename TShaderClass>
	static void AddPass(
		FRDGBuilder& GraphBuilder,
		FRDGEventName&& PassName,
		ERDGPassFlags PassFlags,
		const TShaderRef<TShaderClass>& ComputeShader,
		typename TShaderClass::FParameters* Parameters,
		FRDGBufferRef IndirectArgsBuffer,
		uint32 IndirectArgsOffset)
	{
		checkf(PassFlags == ERDGPassFlags::Compute || PassFlags == ERDGPassFlags::AsyncCompute, TEXT("AddPass only supports 'Compute' or 'AsyncCompute'."));
		checkf(IndirectArgsBuffer->Desc.Usage & BUF_DrawIndirect, TEXT("The buffer %s was not flagged for indirect draw parameters"), IndirectArgsBuffer->Name);

		ValidateIndirectArgsBuffer(IndirectArgsBuffer, IndirectArgsOffset);
		ClearUnusedGraphResources(ComputeShader, Parameters, { IndirectArgsBuffer });

		GraphBuilder.AddPass(
			Forward<FRDGEventName>(PassName),
			Parameters,
			PassFlags,
			[Parameters, ComputeShader, IndirectArgsBuffer, IndirectArgsOffset](FRHIComputeCommandList& RHICmdList)
		{			
			// Marks the indirect draw parameter as used by the pass manually, given it can't be bound directly by any of the shader,
			// meaning SetShaderParameters() won't be able to do it.
			IndirectArgsBuffer->MarkResourceAsUsed();

			FComputeShaderUtils::DispatchIndirect(RHICmdList, ComputeShader, *Parameters, IndirectArgsBuffer->GetIndirectRHICallBuffer(), IndirectArgsOffset);
		});
	}

	template<typename TShaderClass>
	static FORCEINLINE void AddPass(
		FRDGBuilder& GraphBuilder,
		FRDGEventName&& PassName,
		const TShaderRef<TShaderClass>& ComputeShader,
		typename TShaderClass::FParameters* Parameters,
		FRDGBufferRef IndirectArgsBuffer,
		uint32 IndirectArgsOffset)
	{
		AddPass(GraphBuilder, Forward<FRDGEventName>(PassName), ERDGPassFlags::Compute, ComputeShader, Parameters, IndirectArgsBuffer, IndirectArgsOffset);
	}

	static void ClearUAV(FRDGBuilder& GraphBuilder, FGlobalShaderMap* ShaderMap, FRDGBufferUAVRef UAV, uint32 ClearValue);
	static void ClearUAV(FRDGBuilder& GraphBuilder, FGlobalShaderMap* ShaderMap, FRDGBufferUAVRef UAV, FVector4f ClearValue);

	static inline void ValidateGroupCount(const FIntVector& GroupCount)
	{
		ensure(GroupCount.X <= GRHIMaxDispatchThreadGroupsPerDimension.X);
		ensure(GroupCount.Y <= GRHIMaxDispatchThreadGroupsPerDimension.Y);
		ensure(GroupCount.Z <= GRHIMaxDispatchThreadGroupsPerDimension.Z);
	}

	static inline void ValidateIndirectArgsBuffer(uint32 IndirectArgsBufferSize, uint32 IndirectArgOffset)
	{
		checkf((IndirectArgOffset % 4) == 0, TEXT("IndirectArgOffset for compute shader indirect dispatch needs to be a multiple of 4."));
		checkf(
			(IndirectArgOffset + sizeof(FRHIDispatchIndirectParameters)) <= IndirectArgsBufferSize,
			TEXT("Indirect parameters buffer for compute shader indirect dispatch at byte offset %d doesn't have anought room for FRHIDispatchIndirectParameters."),
			IndirectArgOffset);
	}

	static inline void ValidateIndirectArgsBuffer(FRDGBufferRef IndirectArgsBuffer, uint32 IndirectArgOffset)
	{
		checkf(IndirectArgsBuffer->Desc.UnderlyingType == FRDGBufferDesc::EUnderlyingType::VertexBuffer, TEXT("The buffer %s needs to be a vertex buffer to be used as an indirect dispatch parameters"), IndirectArgsBuffer->Name);
		checkf(IndirectArgsBuffer->Desc.Usage & BUF_DrawIndirect, TEXT("The buffer %s for indirect dispatch parameters was not flagged with BUF_DrawIndirect"), IndirectArgsBuffer->Name);
		ValidateIndirectArgsBuffer(IndirectArgsBuffer->Desc.GetTotalNumBytes(), IndirectArgOffset);
	}

	/**
	 * Create and set up an 1D indirect dispatch argument from some GPU-side integer in a buffer (InputCountBuffer).
	 * 	Sets up a group count as (InputCountBuffer[InputCountOffset] * Multiplier + Divisor - 1U) / Divisor;
	 *  Commonly use Divisor <=> number of threads per group.
	 */
	static FRDGBufferRef AddIndirectArgsSetupCsPass1D(FRDGBuilder& GraphBuilder, FRDGBufferRef& InputCountBuffer, const TCHAR* OutputBufferName, uint32 Divisor, uint32 InputCountOffset = 0U, uint32 Multiplier = 1U);
};

/** Adds a render graph pass to copy a region from one texture to another. Uses RHICopyTexture under the hood.
 *  Formats of the two textures must match. The output and output texture regions be within the respective extents.
 */
RENDERCORE_API void AddCopyTexturePass(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef InputTexture,
	FRDGTextureRef OutputTexture,
	const FRHICopyTextureInfo& CopyInfo);

/** Simpler variant of the above function for 2D textures.
 *  @param InputPosition The pixel position within the input texture of the top-left corner of the box.
 *  @param OutputPosition The pixel position within the output texture of the top-left corner of the box.
 *  @param Size The size in pixels of the region to copy from input to output. If zero, the full extent of
 *         the input texture is copied.
 */
inline void AddCopyTexturePass(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef InputTexture,
	FRDGTextureRef OutputTexture,
	FIntPoint InputPosition = FIntPoint::ZeroValue,
	FIntPoint OutputPosition = FIntPoint::ZeroValue,
	FIntPoint Size = FIntPoint::ZeroValue)
{
	FRHICopyTextureInfo CopyInfo;
	CopyInfo.SourcePosition.X = InputPosition.X;
	CopyInfo.SourcePosition.Y = InputPosition.Y;
	CopyInfo.DestPosition.X = OutputPosition.X;
	CopyInfo.DestPosition.Y = OutputPosition.Y;
	if (Size != FIntPoint::ZeroValue)
	{
		CopyInfo.Size = FIntVector(Size.X, Size.Y, 1);
	}
	AddCopyTexturePass(GraphBuilder, InputTexture, OutputTexture, CopyInfo);
}

/** Adds a render graph pass to resolve from one texture to another. Uses RHICopyToResolveTarget under the hood.
 *  The formats of the two textures don't need to match.
 */
RENDERCORE_API void AddCopyToResolveTargetPass(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef InputTexture,
	FRDGTextureRef OutputTexture,
	const FResolveParams& ResolveParams);

/** Adds a render graph pass to clear a texture or buffer UAV with a single typed value. */
RENDERCORE_API void AddClearUAVPass(FRDGBuilder& GraphBuilder, FRDGBufferUAVRef BufferUAV, uint32 Value);

RENDERCORE_API void AddClearUAVFloatPass(FRDGBuilder& GraphBuilder, FRDGBufferUAVRef BufferUAV, float Value);

RENDERCORE_API void AddClearUAVPass(FRDGBuilder& GraphBuilder, FRDGTextureUAVRef TextureUAV, float ClearValue);

RENDERCORE_API void AddClearUAVPass(FRDGBuilder& GraphBuilder, FRDGTextureUAVRef TextureUAV, uint32 ClearValue);

RENDERCORE_API void AddClearUAVPass(FRDGBuilder& GraphBuilder, FRDGTextureUAVRef TextureUAV, const FIntPoint& ClearValue);

RENDERCORE_API void AddClearUAVPass(FRDGBuilder& GraphBuilder, FRDGTextureUAVRef TextureUAV, const FVector2D& ClearValue);

RENDERCORE_API void AddClearUAVPass(FRDGBuilder& GraphBuilder, FRDGTextureUAVRef TextureUAV, const FVector& ClearValue);

RENDERCORE_API void AddClearUAVPass(FRDGBuilder& GraphBuilder, FRDGTextureUAVRef TextureUAV, const FUintVector4& ClearValues);

RENDERCORE_API void AddClearUAVPass(FRDGBuilder& GraphBuilder, FRDGTextureUAVRef TextureUAV, const FVector4& ClearValues);

RENDERCORE_API void AddClearUAVPass(FRDGBuilder& GraphBuilder, FRDGTextureUAVRef TextureUAV, const float(&ClearValues)[4]);

RENDERCORE_API void AddClearUAVPass(FRDGBuilder& GraphBuilder, FRDGTextureUAVRef TextureUAV, const uint32(&ClearValues)[4]);

RENDERCORE_API void AddClearUAVPass(FRDGBuilder& GraphBuilder, FRDGTextureUAVRef TextureUAV, const FLinearColor& ClearColor);

/** Clears parts of UAV specified by an array of screen rects. If no rects are specific, then it falls back to a standard UAV clear. */
RENDERCORE_API void AddClearUAVPass(FRDGBuilder& GraphBuilder, FRDGTextureUAVRef TextureUAV, const uint32(&ClearValues)[4], FRDGBufferSRVRef RectMinMaxBufferSRV, uint32 NumRects);

/** Adds a render graph pass to clear a render target to its clear value. */
RENDERCORE_API void AddClearRenderTargetPass(FRDGBuilder& GraphBuilder, FRDGTextureRef Texture);

/** Adds a render graph pass to clear a render target. Uses render pass clear actions if the clear color matches the fast clear color. */
RENDERCORE_API void AddClearRenderTargetPass(FRDGBuilder& GraphBuilder, FRDGTextureRef Texture, const FLinearColor& ClearColor);

/** Adds a render graph pass to clear a render target. Draws a quad to the requested viewport. */
RENDERCORE_API void AddClearRenderTargetPass(FRDGBuilder& GraphBuilder, FRDGTextureRef Texture, const FLinearColor& ClearColor, FIntRect Viewport);

/** Adds a render graph pass to clear a depth stencil target. Prefer to use clear actions if possible. */
RENDERCORE_API void AddClearDepthStencilPass(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef Texture,
	bool bClearDepth,
	float Depth,
	bool bClearStencil,
	uint8 Stencil);

/** Adds a render graph pass to clear a depth stencil target to its optimized clear value using a raster pass. */
RENDERCORE_API void AddClearDepthStencilPass(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef Texture,
	ERenderTargetLoadAction DepthLoadAction = ERenderTargetLoadAction::EClear,
	ERenderTargetLoadAction StencilLoadAction = ERenderTargetLoadAction::EClear);

/** Adds a render graph pass to clear the stencil portion of a depth / stencil target to its fast clear value. */
RENDERCORE_API void AddClearStencilPass(FRDGBuilder& GraphBuilder, FRDGTextureRef Texture);

/** Adds a render graph pass to resummarize the htile plane. */
RENDERCORE_API void AddResummarizeHTilePass(FRDGBuilder& GraphBuilder, FRDGTextureRef DepthTexture);

/** Adds a render graph pass to copy SrcBuffer content into DstBuffer. */
RENDERCORE_API void AddCopyBufferPass(FRDGBuilder& GraphBuilder, FRDGBufferRef DstBuffer, FRDGBufferRef SrcBuffer);

/** Adds a pass to readback contents of an RDG texture. */
RENDERCORE_API void AddEnqueueCopyPass(FRDGBuilder& GraphBuilder, FRHIGPUTextureReadback* Readback, FRDGTextureRef SourceTexture, FResolveRect Rect = FResolveRect());

/** Adds a pass to readback contents of an RDG buffer. */
RENDERCORE_API void AddEnqueueCopyPass(FRDGBuilder& GraphBuilder, FRHIGPUBufferReadback* Readback, FRDGBufferRef SourceBuffer, uint32 NumBytes);

UE_DEPRECATED(5.0, "Please use GraphBuilder.QueueBufferUpload to perform an upload.")
inline void AddBufferUploadPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferRef Buffer,
	const void* InitialData,
	uint64 InitialDataSize,
	ERDGInitialDataFlags InitialDataFlags = ERDGInitialDataFlags::None)
{
	GraphBuilder.QueueBufferUpload(Buffer, InitialData, InitialDataSize, InitialDataFlags);
}

/** Helper class to allocate data from a GraphBuilder in order to upload said data to an RDG resource.
*   Allocating from the GraphBuilder makes it so we don't have to copy the data before deferring the upload.
*/
template<typename InElementType>
struct FRDGUploadData : public TArrayView<InElementType, int32>
{
	FRDGUploadData() = delete;
	FRDGUploadData(FRDGBuilder& GraphBuilder, uint32 InCount)
		: TArrayView<InElementType, int32>(GraphBuilder.AllocPODArray<InElementType>(InCount), InCount)
	{
	}

	FORCEINLINE int32 GetTotalSize() const { return this->Num() * this->GetTypeSize(); }
};

/** Creates a structured buffer with initial data by creating an upload pass. */
RENDERCORE_API FRDGBufferRef CreateStructuredBuffer(
	FRDGBuilder& GraphBuilder,
	const TCHAR* Name,
	uint32 BytesPerElement,
	uint32 NumElements,
	const void* InitialData,
	uint64 InitialDataSize,
	ERDGInitialDataFlags InitialDataFlags = ERDGInitialDataFlags::None);

/** A variant where NumElements, InitialData, and InitialDataSize are supplied through callbacks. This allows creating a buffer with
 *  with information unknown at creation time. Though, data must be ready before the most recent RDG pass that references the buffer
 *  is executed.
 */
RENDERCORE_API FRDGBufferRef CreateStructuredBuffer(
	FRDGBuilder& GraphBuilder,
	const TCHAR* Name,
	uint32 BytesPerElement,
	FRDGBufferNumElementsCallback&& NumElementsCallback,
	FRDGBufferInitialDataCallback&& InitialDataCallback,
	FRDGBufferInitialDataSizeCallback&& InitialDataSizeCallback);

/**
 * Helper to create a structured buffer with initial data from a TArray.
 */
template <typename ElementType, typename AllocatorType>
FORCEINLINE FRDGBufferRef CreateStructuredBuffer(
	FRDGBuilder& GraphBuilder,
	const TCHAR* Name,
	const TArray<ElementType, AllocatorType>& InitialData,
	ERDGInitialDataFlags InitialDataFlags = ERDGInitialDataFlags::None)
{
	static const ElementType DummyElement = ElementType();
	if (InitialData.Num() == 0)
	{
		return CreateStructuredBuffer(GraphBuilder, Name, InitialData.GetTypeSize(), 1, &DummyElement, InitialData.GetTypeSize(), ERDGInitialDataFlags::NoCopy);
	}
	return CreateStructuredBuffer(GraphBuilder, Name, InitialData.GetTypeSize(), InitialData.Num(), InitialData.GetData(), InitialData.Num() * InitialData.GetTypeSize(), InitialDataFlags);
}

template <typename ElementType>
FORCEINLINE FRDGBufferRef CreateStructuredBuffer(
	FRDGBuilder& GraphBuilder,
	const TCHAR* Name,
	const FRDGUploadData<ElementType>& InitialData)
{
	static const ElementType DummyElement = ElementType();
	if (InitialData.Num() == 0)
	{
		return CreateStructuredBuffer(GraphBuilder, Name, InitialData.GetTypeSize(), 1, &DummyElement, InitialData.GetTypeSize(), ERDGInitialDataFlags::NoCopy);
	}
	return CreateStructuredBuffer(GraphBuilder, Name, InitialData.GetTypeSize(), InitialData.Num(), InitialData.GetData(), InitialData.GetTotalSize(), ERDGInitialDataFlags::NoCopy);
}

RENDERCORE_API FRDGBufferRef CreateUploadBuffer(
	FRDGBuilder& GraphBuilder,
	const TCHAR* Name,
	uint32 BytesPerElement,
	uint32 NumElements,
	const void* InitialData,
	uint64 InitialDataSize,
	ERDGInitialDataFlags InitialDataFlags = ERDGInitialDataFlags::None);

template <typename ElementType>
FORCEINLINE FRDGBufferRef CreateUploadBuffer(
	FRDGBuilder& GraphBuilder,
	const TCHAR* Name,
	uint32 BytesPerElement,
	uint32 NumElements,
	const FRDGUploadData<ElementType>& InitialData)
{
	return CreateUploadBuffer(GraphBuilder, Name, BytesPerElement, NumElements, InitialData.GetData(), InitialData.GetTotalSize(), ERDGInitialDataFlags::NoCopy);
}

template <typename ElementType>
FORCEINLINE FRDGBufferRef CreateUploadBuffer(
	FRDGBuilder& GraphBuilder,
	const TCHAR* Name,
	const FRDGUploadData<ElementType>& InitialData)
{
	return CreateUploadBuffer(GraphBuilder, Name, sizeof(ElementType), InitialData.Num(), InitialData);
}

/** Creates a vertex buffer with initial data by creating an upload pass. */
RENDERCORE_API FRDGBufferRef CreateVertexBuffer(
	FRDGBuilder& GraphBuilder,
	const TCHAR* Name,
	const FRDGBufferDesc& Desc,
	const void* InitialData,
	uint64 InitialDataSize,
	ERDGInitialDataFlags InitialDataFlags = ERDGInitialDataFlags::None);

/** Helper functions to add parameterless passes to the graph. */
template <typename ExecuteLambdaType>
FORCEINLINE void AddPass(FRDGBuilder& GraphBuilder, FRDGEventName&& Name, ExecuteLambdaType&& ExecuteLambda)
{
	GraphBuilder.AddPass(MoveTemp(Name), ERDGPassFlags::None, MoveTemp(ExecuteLambda));
}

template <typename ExecuteLambdaType>
UE_DEPRECATED(5.0, "AddPass without an RDG_EVENT_NAME is deprecated. Use the named version instead.")
FORCEINLINE void AddPass(FRDGBuilder& GraphBuilder, ExecuteLambdaType&& ExecuteLambda)
{
	AddPass(GraphBuilder, {}, MoveTemp(ExecuteLambda));
}

template <typename ExecuteLambdaType>
FORCEINLINE void AddPassIfDebug(FRDGBuilder& GraphBuilder, FRDGEventName&& Name, ExecuteLambdaType&& ExecuteLambda)
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	AddPass(GraphBuilder, MoveTemp(Name), MoveTemp(ExecuteLambda));
#endif
}

template <typename ExecuteLambdaType>
UE_DEPRECATED(5.0, "AddPassIfDebug without an RDG_EVENT_NAME is deprecated. Use the named version instead.")
FORCEINLINE void AddPassIfDebug(FRDGBuilder& GraphBuilder, ExecuteLambdaType&& ExecuteLambda)
{
	AddPassIfDebug(GraphBuilder, {}, MoveTemp(ExecuteLambda));
}

UE_DEPRECATED(5.0, "AddSetCurrentStatPass is deprecated. Use GraphBuilder.SetCommandListStat instead.")
FORCEINLINE void AddSetCurrentStatPass(FRDGBuilder& GraphBuilder, TStatId StatId)
{
	GraphBuilder.SetCommandListStat(StatId);
}

FORCEINLINE void AddDispatchToRHIThreadPass(FRDGBuilder& GraphBuilder)
{
	AddPass(GraphBuilder, RDG_EVENT_NAME("DispatchToRHI"), [](FRHICommandListImmediate& RHICmdList)
	{
		RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
	});
}

UE_DEPRECATED(5.0, "AddBeginUAVOverlapPass is deprecated.")
FORCEINLINE void AddBeginUAVOverlapPass(FRDGBuilder& GraphBuilder)
{
	AddPass(GraphBuilder, RDG_EVENT_NAME("BeginUAVOverlap"), [](FRHICommandList& RHICmdList)
	{
		RHICmdList.BeginUAVOverlap();
	});
}

UE_DEPRECATED(5.0, "AddEndUAVOverlapPass is deprecated.")
FORCEINLINE void AddEndUAVOverlapPass(FRDGBuilder& GraphBuilder)
{
	AddPass(GraphBuilder, RDG_EVENT_NAME("EndUAVOverlap"), [](FRHICommandList& RHICmdList)
	{
		RHICmdList.EndUAVOverlap();
	});
}

UE_DEPRECATED(5.0, "AddBeginUAVOverlapPass is deprecated.")
FORCEINLINE void AddBeginUAVOverlapPass(FRDGBuilder& GraphBuilder, FRHIUnorderedAccessView* UAV)
{
	AddPass(GraphBuilder, RDG_EVENT_NAME("BeginUAVOverlap"), [UAV](FRHICommandList& RHICmdList)
	{
		RHICmdList.BeginUAVOverlap(UAV);
	});
}

UE_DEPRECATED(5.0, "AddEndUAVOverlapPass is deprecated.")
FORCEINLINE void AddEndUAVOverlapPass(FRDGBuilder& GraphBuilder, FRHIUnorderedAccessView* UAV)
{
	AddPass(GraphBuilder, RDG_EVENT_NAME("EndUAVOverlap"), [UAV](FRHICommandList& RHICmdList)
	{
		RHICmdList.EndUAVOverlap(UAV);
	});
}

UE_DEPRECATED(5.0, "AddBeginUAVOverlapPass is deprecated.")
FORCEINLINE void AddBeginUAVOverlapPass(FRDGBuilder& GraphBuilder, TArrayView<FRHIUnorderedAccessView*> UAVs)
{
	uint32 AllocSize = UAVs.Num() * sizeof(FRHIUnorderedAccessView*);
	FRHIUnorderedAccessView** LocalUAVs = (FRHIUnorderedAccessView**)GraphBuilder.Alloc(AllocSize, alignof(FRHIUnorderedAccessView*));
	FMemory::Memcpy(LocalUAVs, UAVs.GetData(), AllocSize);
	TArrayView<FRHIUnorderedAccessView*> LocalView(LocalUAVs, UAVs.Num());
	AddPass(GraphBuilder, RDG_EVENT_NAME("BeginUAVOverlap"), [LocalView](FRHICommandList& RHICmdList)
	{
		RHICmdList.BeginUAVOverlap(LocalView);
	});
}

UE_DEPRECATED(5.0, "AddEndUAVOverlapPass is deprecated.")
FORCEINLINE void AddEndUAVOverlapPass(FRDGBuilder& GraphBuilder, TArrayView<FRHIUnorderedAccessView*> UAVs)
{
	uint32 AllocSize = UAVs.Num() * sizeof(FRHIUnorderedAccessView*);
	FRHIUnorderedAccessView** LocalUAVs = (FRHIUnorderedAccessView**)GraphBuilder.Alloc(AllocSize, alignof(FRHIUnorderedAccessView*));
	FMemory::Memcpy(LocalUAVs, UAVs.GetData(), AllocSize);
	TArrayView<FRHIUnorderedAccessView*> LocalView(LocalUAVs, UAVs.Num());
	AddPass(GraphBuilder, RDG_EVENT_NAME("EndUAVOverlap"), [LocalView](FRHICommandList& RHICmdList)
	{
		RHICmdList.EndUAVOverlap(LocalView);
	});
}

BEGIN_SHADER_PARAMETER_STRUCT(FReadbackTextureParameters, )
	RDG_TEXTURE_ACCESS(Texture, ERHIAccess::CopySrc)
END_SHADER_PARAMETER_STRUCT()

template <typename ExecuteLambdaType>
void AddReadbackTexturePass(FRDGBuilder& GraphBuilder, FRDGEventName&& Name, FRDGTextureRef Texture, ExecuteLambdaType&& ExecuteLambda)
{
	auto* PassParameters = GraphBuilder.AllocParameters<FReadbackTextureParameters>();
	PassParameters->Texture = Texture;
	GraphBuilder.AddPass(MoveTemp(Name), PassParameters, ERDGPassFlags::Readback, MoveTemp(ExecuteLambda));
}

BEGIN_SHADER_PARAMETER_STRUCT(FReadbackBufferParameters, )
	RDG_BUFFER_ACCESS(Buffer, ERHIAccess::CopySrc)
END_SHADER_PARAMETER_STRUCT()

template <typename ExecuteLambdaType>
void AddReadbackBufferPass(FRDGBuilder& GraphBuilder, FRDGEventName&& Name, FRDGBufferRef Buffer, ExecuteLambdaType&& ExecuteLambda)
{
	auto* PassParameters = GraphBuilder.AllocParameters<FReadbackBufferParameters>();
	PassParameters->Buffer = Buffer;
	GraphBuilder.AddPass(MoveTemp(Name), PassParameters, ERDGPassFlags::Readback, MoveTemp(ExecuteLambda));
}

/** Batches up RDG resource access finalizations and submits them all at once to RDG. */
class FRDGResourceAccessFinalizer
{
public:
	FRDGResourceAccessFinalizer() = default;
	~FRDGResourceAccessFinalizer()
	{
		checkf(IsEmpty(), TEXT("Finalize must be called before destruction."));
	}

	void Reserve(uint32 TextureCount, uint32 BufferCount)
	{
		Textures.Reserve(TextureCount);
		Buffers.Reserve(BufferCount);
	}

	void AddTexture(FRDGTextureRef Texture, ERHIAccess Access)
	{
		if (Texture)
		{
			checkf(IsValidAccess(Access) && Access != ERHIAccess::Unknown, TEXT("Attempted to finalize texture %s with an invalid access %s."), Texture->Name, *GetRHIAccessName(Access));
			Textures.Emplace(Texture, Access);
		}
	}

	void AddBuffer(FRDGBufferRef Buffer, ERHIAccess Access)
	{
		if (Buffer)
		{
			checkf(IsValidAccess(Access) && Access != ERHIAccess::Unknown, TEXT("Attempted to finalize buffer %s with an invalid access %s."), Buffer->Name, *GetRHIAccessName(Access));
			Buffers.Emplace(Buffer, Access);
		}
	}

	void Finalize(FRDGBuilder& GraphBuilder)
	{
		if (!IsEmpty())
		{
			GraphBuilder.FinalizeResourceAccess(MoveTemp(Textures), MoveTemp(Buffers));
		}
	}

	bool IsEmpty() const
	{
		return Textures.IsEmpty() && Buffers.IsEmpty();
	}

private:
	FRDGTextureAccessArray Textures;
	FRDGBufferAccessArray Buffers;
};

inline const TRefCountPtr<IPooledRenderTarget>& ConvertToFinalizedExternalTexture(
	FRDGBuilder& GraphBuilder,
	FRDGResourceAccessFinalizer& ResourceAccessFinalizer,
	FRDGTextureRef Texture,
	ERHIAccess AccessFinal = ERHIAccess::SRVMask)
{
	ResourceAccessFinalizer.AddTexture(Texture, AccessFinal);
	return GraphBuilder.ConvertToExternalTexture(Texture);
}

inline const TRefCountPtr<FRDGPooledBuffer>& ConvertToFinalizedExternalBuffer(
	FRDGBuilder& GraphBuilder,
	FRDGResourceAccessFinalizer& ResourceAccessFinalizer,
	FRDGBufferRef Buffer,
	ERHIAccess AccessFinal = ERHIAccess::SRVMask)
{
	ResourceAccessFinalizer.AddBuffer(Buffer, AccessFinal);
	return GraphBuilder.ConvertToExternalBuffer(Buffer);
}

inline const TRefCountPtr<IPooledRenderTarget>& ConvertToFinalizedExternalTexture(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef Texture,
	ERHIAccess AccessFinal = ERHIAccess::SRVMask)
{
	const TRefCountPtr<IPooledRenderTarget>& PooledTexture = GraphBuilder.ConvertToExternalTexture(Texture);
	GraphBuilder.FinalizeTextureAccess(Texture, AccessFinal);
	return PooledTexture;
}

inline const TRefCountPtr<FRDGPooledBuffer>& ConvertToFinalizedExternalBuffer(
	FRDGBuilder& GraphBuilder,
	FRDGBufferRef Buffer,
	ERHIAccess AccessFinal)
{
	const TRefCountPtr<FRDGPooledBuffer>& PooledBuffer = GraphBuilder.ConvertToExternalBuffer(Buffer);
	GraphBuilder.FinalizeBufferAccess(Buffer, AccessFinal);
	return PooledBuffer;
}

/** Scope used to wait for outstanding tasks when the scope destructor is called. Used for command list recording tasks. */
class FRDGWaitForTasksScope
{
public:
	FRDGWaitForTasksScope(FRDGBuilder& InGraphBuilder, bool InbCondition = true)
		: GraphBuilder(InGraphBuilder)
		, bCondition(InbCondition)
	{}

	RENDERCORE_API ~FRDGWaitForTasksScope();

private:
	FRDGBuilder& GraphBuilder;
	bool bCondition;
};

#define RDG_WAIT_FOR_TASKS_CONDITIONAL(GraphBuilder, bCondition) FRDGWaitForTasksScope PREPROCESSOR_JOIN(RDGWaitForTasksScope, __LINE__){ GraphBuilder, bCondition }
#define RDG_WAIT_FOR_TASKS(GraphBuilder) RDG_WAIT_FOR_TASKS_CONDITIONAL(GraphBuilder, true)

// Allocates an RDG pooled buffer instance. Attempts to reuse allocation if Out has a value. Returns true a new instance was allocated, or false if the existing allocation was reused.
RENDERCORE_API bool GetPooledFreeBuffer(
	FRHICommandList& RHICmdList,
	const FRDGBufferDesc& Desc,
	TRefCountPtr<FRDGPooledBuffer>& Out,
	const TCHAR* InDebugName);

//////////////////////////////////////////////////////////////////////////
//! Deprecated Functions

UE_DEPRECATED(5.0, "ConvertToExternalBuffer has been refactored to FRDGBuilder::ConvertToExternalBuffer.")
inline void ConvertToExternalBuffer(FRDGBuilder& GraphBuilder, FRDGBufferRef Buffer, TRefCountPtr<FRDGPooledBuffer>& OutPooledBuffer)
{
	OutPooledBuffer = GraphBuilder.ConvertToExternalBuffer(Buffer);
}

UE_DEPRECATED(5.0, "ConvertToExternalTexture has been refactored to FRDGBuilder::ConvertToExternalTexture.")
inline void ConvertToExternalTexture(FRDGBuilder& GraphBuilder, FRDGTextureRef Texture, TRefCountPtr<IPooledRenderTarget>& OutPooledRenderTarget)
{
	OutPooledRenderTarget = GraphBuilder.ConvertToExternalTexture(Texture);
}

UE_DEPRECATED(5.0, "ConvertToUntrackedExternalTexture has been refactored to ConvertToFinalizedExternalTexture.")
inline void ConvertToUntrackedExternalTexture(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef Texture,
	TRefCountPtr<IPooledRenderTarget>& OutPooledRenderTarget,
	ERHIAccess AccessFinal)
{
	OutPooledRenderTarget = ConvertToFinalizedExternalTexture(GraphBuilder, Texture, AccessFinal);
}

UE_DEPRECATED(5.0, "ConvertToUntrackedExternalTexture has been refactored to ConvertToFinalizedExternalBuffer.")
inline void ConvertToUntrackedExternalBuffer(
	FRDGBuilder& GraphBuilder,
	FRDGBufferRef Buffer,
	TRefCountPtr<FRDGPooledBuffer>& OutPooledBuffer,
	ERHIAccess AccessFinal)
{
	OutPooledBuffer = ConvertToFinalizedExternalBuffer(GraphBuilder, Buffer, AccessFinal);
}

//////////////////////////////////////////////////////////////////////////