// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	AGXShaderParameterCache.cpp: AGX RHI Shader Parameter Cache Class.
=============================================================================*/


#include "AGXRHIPrivate.h"
#include "AGXShaderParameterCache.h"


//------------------------------------------------------------------------------

#pragma mark - AGX Shader Parameter Cache Class


FAGXShaderParameterCache::FAGXShaderParameterCache()
{
	for (int32 ArrayIndex = 0; ArrayIndex < CrossCompiler::PACKED_TYPEINDEX_MAX; ++ArrayIndex)
	{
		PackedGlobalUniforms[ArrayIndex] = nullptr;
		PackedGlobalUniformsSizes[ArrayIndex] = 0;
		PackedGlobalUniformDirty[ArrayIndex].LowVector = 0;
		PackedGlobalUniformDirty[ArrayIndex].HighVector = 0;
	}
}

void FAGXShaderParameterCache::ResizeGlobalUniforms(uint32 TypeIndex, uint32 UniformArraySize)
{
	if (!PackedGlobalUniforms[TypeIndex])
	{
		PackedGlobalUniforms[TypeIndex] = [[FAGXBufferData alloc] initWithSize:UniformArraySize];
	}
	else
	{
		PackedGlobalUniforms[TypeIndex]->Data = (uint8*)FMemory::Realloc(PackedGlobalUniforms[TypeIndex]->Data, UniformArraySize);
		PackedGlobalUniforms[TypeIndex]->Len = UniformArraySize;
	}
	PackedGlobalUniformsSizes[TypeIndex] = UniformArraySize;
	PackedGlobalUniformDirty[TypeIndex].LowVector = 0;
	PackedGlobalUniformDirty[TypeIndex].HighVector = 0;
}

FAGXShaderParameterCache::~FAGXShaderParameterCache()
{
	for (int32 ArrayIndex = 0; ArrayIndex < CrossCompiler::PACKED_TYPEINDEX_MAX; ++ArrayIndex)
	{
		[PackedGlobalUniforms[ArrayIndex] release];
	}
}

void FAGXShaderParameterCache::Reset()
{
	for (int32 ArrayIndex = 0; ArrayIndex < CrossCompiler::PACKED_TYPEINDEX_MAX; ++ArrayIndex)
	{
		PackedGlobalUniformDirty[ArrayIndex].LowVector = 0;
		PackedGlobalUniformDirty[ArrayIndex].HighVector = 0;
	}
}

void FAGXShaderParameterCache::MarkAllDirty()
{
	for (int32 ArrayIndex = 0; ArrayIndex < CrossCompiler::PACKED_TYPEINDEX_MAX; ++ArrayIndex)
	{
		PackedGlobalUniformDirty[ArrayIndex].LowVector = 0;
		PackedGlobalUniformDirty[ArrayIndex].HighVector = PackedGlobalUniformsSizes[ArrayIndex] / SizeOfFloat;
	}
}

void FAGXShaderParameterCache::Set(uint32 BufferIndexName, uint32 ByteOffset, uint32 NumBytes, const void* NewValues)
{
	if (NumBytes)
	{
		uint32 BufferIndex = CrossCompiler::PackedTypeNameToTypeIndex(BufferIndexName);
		check(BufferIndex < CrossCompiler::PACKED_TYPEINDEX_MAX);
		check(PackedGlobalUniforms[BufferIndex]);
		check(ByteOffset + NumBytes <= PackedGlobalUniformsSizes[BufferIndex]);
		PackedGlobalUniformDirty[BufferIndex].LowVector = FMath::Min(PackedGlobalUniformDirty[BufferIndex].LowVector, ByteOffset / SizeOfFloat);
		PackedGlobalUniformDirty[BufferIndex].HighVector = FMath::Max(PackedGlobalUniformDirty[BufferIndex].HighVector, (ByteOffset + NumBytes + SizeOfFloat - 1) / SizeOfFloat);
		FMemory::Memcpy(PackedGlobalUniforms[BufferIndex]->Data + ByteOffset, NewValues, NumBytes);
	}
}

void FAGXShaderParameterCache::CommitPackedGlobals(FAGXStateCache* Cache, FAGXCommandEncoder* Encoder, uint32 Frequency, const FMetalShaderBindings& Bindings)
{
	// copy the current uniform buffer into the ring buffer to submit
	for (int32 Index = 0; Index < Bindings.PackedGlobalArrays.Num(); ++Index)
	{
		int32 UniformBufferIndex = Bindings.PackedGlobalArrays[Index].TypeIndex;

		// is there any data that needs to be copied?
		if (PackedGlobalUniformDirty[Index].HighVector > 0)
		{
			uint32 TotalSize = Bindings.PackedGlobalArrays[Index].Size;
			uint32 SizeToUpload = PackedGlobalUniformDirty[Index].HighVector * SizeOfFloat;

			//@todo-rco: Temp workaround
			SizeToUpload = TotalSize;

			//@todo-rco: Temp workaround
			uint32 Size = FMath::Min(TotalSize, SizeToUpload);
			if (Size > AGXBufferPageSize)
			{
				uint8 const* Bytes = PackedGlobalUniforms[Index]->Data;
				ns::AutoReleased<FAGXBuffer> Buffer(Encoder->GetRingBuffer().NewBuffer(Size, 0));
				FMemory::Memcpy((uint8*)Buffer.GetContents(), Bytes, Size);
				Cache->SetShaderBuffer((EAGXShaderStages)Frequency, Buffer, nil, 0, Size, UniformBufferIndex, mtlpp::ResourceUsage::Read);
			}
			else
			{
				PackedGlobalUniforms[Index]->Len = Size;
				Cache->SetShaderBuffer((EAGXShaderStages)Frequency, nil, nil, 0, 0, UniformBufferIndex, mtlpp::ResourceUsage(0));
				Cache->SetShaderBuffer((EAGXShaderStages)Frequency, nil, PackedGlobalUniforms[Index], 0, Size, UniformBufferIndex, mtlpp::ResourceUsage::Read);
			}

			// mark as clean
			PackedGlobalUniformDirty[Index].HighVector = 0;
		}
	}
}
