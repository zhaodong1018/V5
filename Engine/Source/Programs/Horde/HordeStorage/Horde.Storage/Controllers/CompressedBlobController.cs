﻿// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.ComponentModel.DataAnnotations;
using System.IO;
using System.Linq;
using System.Net.Mime;
using System.Threading.Tasks;
using Datadog.Trace;
using Horde.Storage.Implementation;
using Jupiter;
using Jupiter.Common.Implementation;
using Jupiter.Implementation;
using Jupiter.Utils;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Mvc;
using Serilog;

namespace Horde.Storage.Controllers
{
    [ApiController]
    [Route("api/v1/compressed-blobs")]
    public class CompressedBlobController : ControllerBase
    {
        private readonly IBlobService _storage;
        private readonly IContentIdStore _contentIdStore;
        private readonly IDiagnosticContext _diagnosticContext;
        private readonly IAuthorizationService _authorizationService;
        private readonly CompressedBufferUtils _compressedBufferUtils;
        private readonly BufferedPayloadFactory _bufferedPayloadFactory;

        private readonly ILogger _logger = Log.ForContext<CompressedBlobController>();

        public CompressedBlobController(IBlobService storage, IContentIdStore contentIdStore, IDiagnosticContext diagnosticContext, IAuthorizationService authorizationService, CompressedBufferUtils compressedBufferUtils, BufferedPayloadFactory bufferedPayloadFactory)
        {
            _storage = storage;
            _contentIdStore = contentIdStore;
            _diagnosticContext = diagnosticContext;
            _authorizationService = authorizationService;
            _compressedBufferUtils = compressedBufferUtils;
            _bufferedPayloadFactory = bufferedPayloadFactory;
        }


        [HttpGet("{ns}/{id}")]
        [Authorize("Storage.read")]
        [ProducesResponseType(type: typeof(byte[]), 200)]
        [ProducesResponseType(type: typeof(ValidationProblemDetails), 400)]
        [Produces(MediaTypeNames.Application.Json, CustomMediaTypeNames.UnrealCompactBinary, CustomMediaTypeNames.UnrealCompressedBuffer)]

        public async Task<IActionResult> Get(
            [Required] NamespaceId ns,
            [Required] BlobIdentifier id)
        {
            AuthorizationResult authorizationResult = await _authorizationService.AuthorizeAsync(User, ns, NamespaceAccessRequirement.Name);

            if (!authorizationResult.Succeeded)
            {
                return Forbid();
            }

            try
            {
                BlobContents blobContents = await GetImpl(ns, id);

                return File(blobContents.Stream, CustomMediaTypeNames.UnrealCompressedBuffer, enableRangeProcessing: true);
            }
            catch (BlobNotFoundException e)
            {
                return NotFound(new ValidationProblemDetails { Title = $"Object {e.Blob} not found" });
            }
            catch (ContentIdResolveException e)
            {
                return NotFound(new ValidationProblemDetails { Title = $"Content Id {e.ContentId} not found" });
            }
        }
        
        [HttpHead("{ns}/{id}")]
        [Authorize("Storage.read")]
        [ProducesDefaultResponseType]
        public async Task<IActionResult> Head(
            [Required] NamespaceId ns,
            [Required] BlobIdentifier id)
        {
            AuthorizationResult authorizationResult = await _authorizationService.AuthorizeAsync(User, ns, NamespaceAccessRequirement.Name);

            if (!authorizationResult.Succeeded)
            {
                return Forbid();
            }
            BlobIdentifier[]? chunks = await _contentIdStore.Resolve(ns, id);
            if (chunks == null)
            {
                return NotFound(new ValidationProblemDetails { Title = $"Content-id {id} not found"});
            }

            Task<bool>[] tasks = new Task<bool>[chunks.Length];
            for (int i = 0; i < chunks.Length; i++)
            {
                tasks[i] = _storage.Exists(ns, id);
            }

            await Task.WhenAll(tasks);

            bool exists = tasks.All(task => task.Result);

            if (!exists)
            {
                return NotFound(new ValidationProblemDetails { Title = $"Object {id} not found"});
            }

            return Ok();
        }

        [HttpPost("{ns}/exists")]
        [Authorize("Storage.read")]
        [ProducesDefaultResponseType]
        public async Task<IActionResult> ExistsMultiple(
            [Required] NamespaceId ns,
            [Required] [FromQuery] List<BlobIdentifier> id)
        {
            AuthorizationResult authorizationResult = await _authorizationService.AuthorizeAsync(User, ns, NamespaceAccessRequirement.Name);

            if (!authorizationResult.Succeeded)
            {
                return Forbid();
            }

            ConcurrentBag<BlobIdentifier> missingBlobs = new ConcurrentBag<BlobIdentifier>();
            ConcurrentBag<BlobIdentifier> invalidContentIds = new ConcurrentBag<BlobIdentifier>();

            IEnumerable<Task> tasks = id.Select(async blob =>
            {
                BlobIdentifier[]? chunks = await _contentIdStore.Resolve(ns, blob);

                if (chunks == null)
                {
                    invalidContentIds.Add(blob);
                    return;
                }

                foreach (BlobIdentifier chunk in chunks)
                {
                    if (!await _storage.Exists(ns, chunk))
                    {
                        missingBlobs.Add(blob);
                    }
                }
            });
            await Task.WhenAll(tasks);

            if (invalidContentIds.Count != 0)
                return NotFound(new ValidationProblemDetails { Title = $"Missing content ids {string.Join(" ,", invalidContentIds)}"});

            return Ok(new HeadMultipleResponse { Needs = missingBlobs.ToArray()});
        }

        private async Task<BlobContents> GetImpl(NamespaceId ns, BlobIdentifier contentId)
        {
            BlobIdentifier[]? chunks = await _contentIdStore.Resolve(ns, contentId);
            if (chunks == null)
            {
                throw new ContentIdResolveException(contentId);
            }

            using Scope _ = Tracer.Instance.StartActive("blob.combine");
            Task<BlobContents>[] tasks = new Task<BlobContents>[chunks.Length];
            for (int i = 0; i < chunks.Length; i++)
            {
                tasks[i] = _storage.GetObject(ns, chunks[i]);
            }

            MemoryStream ms = new MemoryStream();
            foreach (Task<BlobContents> task in tasks)
            {
                BlobContents blob = await task;
                await using Stream s = blob.Stream;
                await s.CopyToAsync(ms);
            }

            ms.Seek(0, SeekOrigin.Begin);

            return new BlobContents(ms, ms.Length);
        }

        [HttpPut("{ns}/{id}")]
        [Authorize("Storage.write")]
        [DisableRequestSizeLimit]
        [RequiredContentType(CustomMediaTypeNames.UnrealCompressedBuffer)]
        public async Task<IActionResult> Put(
            [Required] NamespaceId ns,
            [Required] BlobIdentifier id)
        {
            AuthorizationResult authorizationResult = await _authorizationService.AuthorizeAsync(User, ns, NamespaceAccessRequirement.Name);

            if (!authorizationResult.Succeeded)
            {
                return Forbid();
            }

            _diagnosticContext.Set("Content-Length", Request.ContentLength ?? -1);

            using IBufferedPayload payload = await _bufferedPayloadFactory.CreateFromRequest(Request);

            try
            {
                BlobIdentifier identifier = await PutImpl(ns, id, payload);

                return Ok(new
                {
                    Identifier = identifier.ToString()
                });
            }
            catch (HashMismatchException e)
            {
                return BadRequest(new ProblemDetails
                {
                    Title = $"Incorrect hash, got hash \"{e.SuppliedHash}\" but hash of content was determined to be \"{e.ContentHash}\""
                });
            }
        }

        private async Task<BlobIdentifier> PutImpl(NamespaceId ns, BlobIdentifier id, IBufferedPayload payload)
        {
            // decompress the content and generate a identifier from it to verify the identifier we got
            await using Stream decompressStream = payload.GetStream();
            // TODO: we should add a overload for decompress content that can work on streams, otherwise we are still limited to 2GB compressed blobs
            byte[] decompressedContent = _compressedBufferUtils.DecompressContent(await decompressStream.ToByteArray());

            MemoryStream decompressedStream = new MemoryStream(decompressedContent);
            BlobIdentifier identifierDecompressedPayload = await _storage.VerifyContentMatchesHash(decompressedStream, id);

            BlobIdentifier identifierCompressedPayload;
            {
                using Scope _ = Tracer.Instance.StartActive("web.hash");
                await using Stream hashStream = payload.GetStream();
                identifierCompressedPayload = await BlobIdentifier.FromStream(hashStream);
            }

            // commit the mapping from the decompressed hash to the compressed hash, we run this in parallel with the blob store submit
            // TODO: let users specify weight of the blob compared to previously submitted content ids
            int contentIdWeight = (int)payload.Length;
            Task contentIdStoreTask = _contentIdStore.Put(ns, identifierDecompressedPayload, identifierCompressedPayload, contentIdWeight);

            // we still commit the compressed buffer to the object store using the hash of the compressed content
            {
                await _storage.PutObjectKnownHash(ns, payload, identifierCompressedPayload);
            }

            await contentIdStoreTask;

            return identifierDecompressedPayload;
        }

        /*[HttpDelete("{ns}/{id}")]
        [Authorize("Storage.delete")]
        public async Task<IActionResult> Delete(
            [Required] string ns,
            [Required] BlobIdentifier id)
        {
            AuthorizationResult authorizationResult = await _authorizationService.AuthorizeAsync(User, ns, NamespaceAccessRequirement.Name);

            if (!authorizationResult.Succeeded)
            {
                return Forbid();
            }

            await DeleteImpl(ns, id);

            return Ok();
        }

        
        [HttpDelete("{ns}")]
        [Authorize("Admin")]
        public async Task<IActionResult> DeleteNamespace(
            [Required] string ns)
        {
            AuthorizationResult authorizationResult = await _authorizationService.AuthorizeAsync(User, ns, NamespaceAccessRequirement.Name);

            if (!authorizationResult.Succeeded)
            {
                return Forbid();
            }

            int deletedCount = await  _storage.DeleteNamespace(ns);

            return Ok( new { Deleted = deletedCount });
        }


        private async Task DeleteImpl(string ns, BlobIdentifier id)
        {
            await _storage.DeleteObject(ns, id);
        }*/
    }
}
