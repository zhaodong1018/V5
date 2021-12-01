// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

inline void FRDGSubresourceState::Finalize()
{
	ensureMsgf(!EnumHasAnyFlags(GetPipelines(), ERHIPipeline::AsyncCompute), TEXT("Resource should not be on the async compute pipeline!"));

	const ERHIAccess LocalAccess = Access;
	*this = {};
	Access = LocalAccess;
}

inline void FRDGSubresourceState::SetPass(ERHIPipeline Pipeline, FRDGPassHandle PassHandle)
{
	FirstPass = {};
	LastPass = {};
	FirstPass[Pipeline] = PassHandle;
	LastPass[Pipeline] = PassHandle;
}

inline void FRDGSubresourceState::Validate()
{
#if RDG_ENABLE_DEBUG
	for (ERHIPipeline Pipeline : GetRHIPipelines())
	{
		checkf(FirstPass[Pipeline].IsValid() == LastPass[Pipeline].IsValid(), TEXT("Subresource state has unset first or last pass on '%s."), *GetRHIPipelineName(Pipeline));
	}
#endif
}

inline bool FRDGSubresourceState::IsUsedBy(ERHIPipeline Pipeline) const
{
	check(FirstPass[Pipeline].IsValid() == LastPass[Pipeline].IsValid());
	return FirstPass[Pipeline].IsValid();
}

inline FRDGPassHandle FRDGSubresourceState::GetLastPass() const
{
	return FRDGPassHandle::Max(LastPass[ERHIPipeline::Graphics], LastPass[ERHIPipeline::AsyncCompute]);
}

inline FRDGPassHandle FRDGSubresourceState::GetFirstPass() const
{
	return FRDGPassHandle::Min(FirstPass[ERHIPipeline::Graphics], FirstPass[ERHIPipeline::AsyncCompute]);
}

FORCEINLINE ERHIPipeline FRDGSubresourceState::GetPipelines() const
{
	ERHIPipeline Pipelines = ERHIPipeline::None;
	Pipelines |= FirstPass[ERHIPipeline::Graphics].IsValid()     ? ERHIPipeline::Graphics     : ERHIPipeline::None;
	Pipelines |= FirstPass[ERHIPipeline::AsyncCompute].IsValid() ? ERHIPipeline::AsyncCompute : ERHIPipeline::None;
	return Pipelines;
}

inline FPooledRenderTargetDesc Translate(const FRDGTextureDesc& InDesc)
{
	check(InDesc.IsValid());

	const ETextureCreateFlags ShaderResourceOnlyFlags = TexCreate_Transient | TexCreate_FastVRAM | TexCreate_ResolveTargetable | TexCreate_DepthStencilResolveTarget;
	const ETextureCreateFlags ShaderResourceFlags = TexCreate_ShaderResource;

	FPooledRenderTargetDesc OutDesc;
	OutDesc.ClearValue = InDesc.ClearValue;
	OutDesc.Flags = (InDesc.Flags & ShaderResourceOnlyFlags) | (InDesc.Flags & ShaderResourceFlags);
	OutDesc.TargetableFlags = (InDesc.Flags & ~ShaderResourceOnlyFlags);
	OutDesc.Format = InDesc.Format;
	OutDesc.UAVFormat = InDesc.UAVFormat;
	OutDesc.Extent.X = InDesc.Extent.X;
	OutDesc.Extent.Y = InDesc.Extent.Y;
	OutDesc.Depth = InDesc.Dimension == ETextureDimension::Texture3D ? InDesc.Depth : 0;
	OutDesc.ArraySize = InDesc.ArraySize;
	OutDesc.NumMips = InDesc.NumMips;
	OutDesc.NumSamples = InDesc.NumSamples;
	OutDesc.bIsArray = InDesc.IsTextureArray();
	OutDesc.bIsCubemap = InDesc.IsTextureCube();
	OutDesc.bForceSeparateTargetAndShaderResource = false;
	OutDesc.bForceSharedTargetAndShaderResource = InDesc.IsMultisample(); // Don't set this unless actually necessary to avoid creating separate pool buckets.
	OutDesc.AutoWritable = false;

	check(OutDesc.IsValid());
	return OutDesc;
}

inline FRHIBufferCreateInfo Translate(const FRDGBufferDesc& InDesc)
{
	FRHIBufferCreateInfo CreateInfo;
	CreateInfo.Size = InDesc.GetTotalNumBytes();
	if (InDesc.UnderlyingType == FRDGBufferDesc::EUnderlyingType::VertexBuffer)
	{
		CreateInfo.Stride = 0;
		CreateInfo.Usage = InDesc.Usage | BUF_VertexBuffer;
	}
	else if (InDesc.UnderlyingType == FRDGBufferDesc::EUnderlyingType::StructuredBuffer)
	{
		CreateInfo.Stride = InDesc.BytesPerElement;
		CreateInfo.Usage = InDesc.Usage | BUF_StructuredBuffer;
	}
	else
	{
		check(0);
	}

	return CreateInfo;
}

FRDGTextureDesc Translate(const FPooledRenderTargetDesc& InDesc, ERenderTargetTexture InTexture)
{
	check(InDesc.IsValid());

	FRDGTextureDesc OutDesc;
	OutDesc.ClearValue = InDesc.ClearValue;
	OutDesc.Format = InDesc.Format;
	OutDesc.UAVFormat = InDesc.UAVFormat;
	OutDesc.Extent = InDesc.Extent;
	OutDesc.ArraySize = InDesc.ArraySize;
	OutDesc.NumMips = InDesc.NumMips;

	if (InDesc.Depth > 0)
	{
		OutDesc.Depth = InDesc.Depth;
		OutDesc.Dimension = ETextureDimension::Texture3D;
	}
	else if (InDesc.bIsCubemap)
	{
		OutDesc.Dimension = InDesc.bIsArray ? ETextureDimension::TextureCubeArray : ETextureDimension::TextureCube;
	}
	else if (InDesc.bIsArray)
	{
		OutDesc.Dimension = ETextureDimension::Texture2DArray;
	}

	// Matches logic in RHIUtilities.h for compatibility.
	const ETextureCreateFlags TargetableFlags = ETextureCreateFlags(InDesc.TargetableFlags) | TexCreate_ShaderResource;
	const ETextureCreateFlags ShaderResourceFlags = ETextureCreateFlags(InDesc.Flags) | TexCreate_ShaderResource;

	OutDesc.Flags = TargetableFlags | ShaderResourceFlags;

	bool bUseSeparateTextures = InDesc.bForceSeparateTargetAndShaderResource;

	if (InDesc.NumSamples > 1 && !InDesc.bForceSharedTargetAndShaderResource)
	{
		bUseSeparateTextures = RHISupportsSeparateMSAAAndResolveTextures(GMaxRHIShaderPlatform);
	}

	if (bUseSeparateTextures)
	{
		if (InTexture == ERenderTargetTexture::Targetable)
		{
			OutDesc.NumSamples = InDesc.NumSamples;
			OutDesc.Flags = TargetableFlags;
		}
		else
		{
			OutDesc.Flags = ShaderResourceFlags;

			if (EnumHasAnyFlags(TargetableFlags, TexCreate_RenderTargetable))
			{
				OutDesc.Flags |= TexCreate_ResolveTargetable;
			}

			if (EnumHasAnyFlags(TargetableFlags, TexCreate_DepthStencilTargetable))
			{
				OutDesc.Flags |= TexCreate_DepthStencilResolveTarget;
			}
		}
	}

	check(OutDesc.IsValid());
	return OutDesc;
}

inline FRDGTextureSubresourceRange FRDGTextureSRV::GetSubresourceRange() const
{
	FRDGTextureSubresourceRange Range = GetParent()->GetSubresourceRange();
	Range.MipIndex = Desc.MipLevel;
	Range.PlaneSlice = GetResourceTransitionPlaneForMetadataAccess(Desc.MetaData);

	if (Desc.MetaData == ERDGTextureMetaDataAccess::None && Desc.Texture && Desc.Texture->Desc.Format == PF_DepthStencil)
	{
		// PF_X24_G8 is used to indicate that this is a view on the stencil plane. Otherwise, it is a view on the depth plane
		Range.PlaneSlice = Desc.Format == PF_X24_G8 ? FRHITransitionInfo::kStencilPlaneSlice : FRHITransitionInfo::kDepthPlaneSlice;
		Range.NumPlaneSlices = 1;
	}

	if (Desc.NumMipLevels != 0)
	{
		Range.NumMips = Desc.NumMipLevels;
	}

	if (Desc.NumArraySlices != 0)
	{
		Range.NumArraySlices = Desc.NumArraySlices;
	}

	if (Desc.MetaData != ERDGTextureMetaDataAccess::None)
	{
		Range.NumPlaneSlices = 1;
	}

	return Range;
}

inline FRDGTextureSubresourceRange FRDGTextureUAV::GetSubresourceRange() const
{
	FRDGTextureSubresourceRange Range = GetParent()->GetSubresourceRange();
	Range.MipIndex = Desc.MipLevel;
	Range.NumMips = 1;
	Range.PlaneSlice = GetResourceTransitionPlaneForMetadataAccess(Desc.MetaData);

	if (Desc.MetaData != ERDGTextureMetaDataAccess::None)
	{
		Range.NumPlaneSlices = 1;
	}

	return Range;
}

inline FRDGBufferSRVDesc::FRDGBufferSRVDesc(FRDGBufferRef InBuffer)
	: Buffer(InBuffer)
{
	if (EnumHasAnyFlags(Buffer->Desc.Usage, BUF_DrawIndirect))
	{
		BytesPerElement = 4;
		Format = PF_R32_UINT;
	}
	else if (EnumHasAnyFlags(Buffer->Desc.Usage, BUF_AccelerationStructure))
	{
		// nothing special here
	}
	else
	{
		checkf(Buffer->Desc.UnderlyingType != FRDGBufferDesc::EUnderlyingType::VertexBuffer, TEXT("VertexBuffer %s requires a type when creating a SRV."), Buffer->Name);
	}
}

inline FRDGBufferUAVDesc::FRDGBufferUAVDesc(FRDGBufferRef InBuffer)
	: Buffer(InBuffer)
{
	if (EnumHasAnyFlags(Buffer->Desc.Usage, BUF_DrawIndirect))
	{
		Format = PF_R32_UINT;
	}
	else
	{
		checkf(Buffer->Desc.UnderlyingType != FRDGBufferDesc::EUnderlyingType::VertexBuffer, TEXT("VertexBuffer %s requires a type when creating a UAV."), Buffer->Name);
	}
}