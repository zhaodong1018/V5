// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.IO;
using System.Threading.Tasks;
using Jupiter.Utils;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.Options;

namespace Jupiter.Common.Implementation
{
    public interface IBufferedPayload : IDisposable
    {
        Stream GetStream();
        long Length { get; }
    }

    /// <summary>
    /// Streaming request that is streamed into memory
    /// </summary>
    public class MemoryBufferedPayload : IBufferedPayload
    {
        private byte[] _buffer;

        public MemoryBufferedPayload(byte[] source)
        {
            _buffer = source;
        }

        public static async Task<MemoryBufferedPayload> Create(Stream s)
        {
            MemoryBufferedPayload payload = new MemoryBufferedPayload(await s.ToByteArray());

            return payload;
        }

        public void Dispose()
        {
            
        }

        public Stream GetStream()
        {
            return new MemoryStream(_buffer);
        }

        public long Length { get { return _buffer.LongLength; } }
    }

    /// <summary>
    /// A streaming request backed by a temporary file on disk
    /// </summary>
    public class FilesystemBufferedPayload : IBufferedPayload
    {
        private readonly FileInfo _tempFile;
        private long _length;

        private FilesystemBufferedPayload()
        {
            _tempFile = new FileInfo(Path.GetTempFileName());
        }

        public static async Task<FilesystemBufferedPayload> Create(Stream s)
        {
            FilesystemBufferedPayload payload = new FilesystemBufferedPayload();

            {
                await using FileStream fs = payload._tempFile.OpenWrite();
                await s.CopyToAsync(fs);
                payload._length = payload._tempFile.Length;
            }

            return payload;
        }

        public void Dispose()
        {
            if (_tempFile.Exists)
                _tempFile.Delete();
        }

        public Stream GetStream()
        {
            return _tempFile.OpenRead();
        }

        public long Length
        {
            get { return _length; }
        }
    }

    public class BufferedPayloadFactory
    {
        private readonly IOptionsMonitor<JupiterSettings> _jupiterSettings;

        public BufferedPayloadFactory(IOptionsMonitor<JupiterSettings> jupiterSettings)
        {
            _jupiterSettings = jupiterSettings;
        }

        public async Task<IBufferedPayload> CreateFromRequest(HttpRequest request)
        {
            long? contentLength = request.ContentLength;

            if (contentLength == null)
            {
                throw new Exception("Expected content-length on all requests");
            }

            
            // blob is small enough to fit into memory we just read it as is
            if (contentLength < _jupiterSettings.CurrentValue.MemoryBufferSize)
            {
                return await MemoryBufferedPayload.Create(request.Body);
            }

            return await FilesystemBufferedPayload.Create(request.Body);
        }
    }
}
