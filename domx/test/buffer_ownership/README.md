# DOMX buffer ownership regression

DOMX imports one ION reference for every accepted plane, including planes that
share an ION handle. The proxy transfers those references to its buffer slot
after remote acceptance. Failed local preparation and explicit remote rejection
release the acquired references and metadata. The MemPlugin allocator releases
its ION allocation when mapping or sharing fails.

The two-descriptor Tuna kernel sets `num_handles` to the highest successful
import index. A failed UV import can return a successful ioctl with one handle.
`RPC_RegisterBuffer` therefore validates the requested descriptors rather than
using the returned count to infer the requested buffer shape. `PROXY_UseBuffer`
passes -1 for an absent plane and preserves zero as a valid descriptor.

Run the regression from the repository root with an explicit build-output path:

```sh
PYTHON=$(command -v python3)
"$PYTHON" domx/test/buffer_ownership/run.py --output "$TEST_OUTPUT_DIR"
```

`TEST_OUTPUT_DIR` names a caller-owned build-output directory. The runner extracts
production function bodies and error macros, compiles their actual control flow
against deterministic API boundaries, and runs 34 DOMX cases plus 8 plugin cases.
The host needs a 32-bit C toolchain and UBSan runtime. Compiler warnings are
errors. The source digests and compiler command appear in the output. Android
translation-unit compilation separately validates the real headers and ABI.

The cases cover failed allocations, partial imports, kernel count truncation,
absent planes, descriptor zero, shared-plane references, metadata ownership,
remote rejection, unknown remote acceptance, cleanup failure, and successful
release. The matched-kernel regression produces 26 failing assertions before
the requested-plane and acknowledgement-boundary corrections, then passes.

## Ownership limits

A failed `RPC_UseBuffer` acknowledgement leaves remote acceptance unknown. The
proxy retains imported references and metadata on that path. A transport error
cannot authorize freeing memory that Ducati may own. The baseline already
retains those allocations; unconditional rollback introduces premature release
when the remote accepts a request but its acknowledgement fails. The regression
asserts retained references and metadata for unknown acceptance. The unknown
outcome remains a leak frontier, including errors that occur before submission
but lack an exposed transaction outcome. The RPC interface needs an explicit
submission outcome and a quiesced remote-session teardown before that frontier
can close safely. Retained metadata mappings and descriptors can survive until
process exit; the regression does not claim complete resource reclamation.

`PROXY_AllocateBuffer` releases its own primary allocation after UseBuffer
failure. The retained RPC import continues to hold the underlying ION buffer
when remote acceptance remains unknown. External callers retain responsibility
for their own dma-buf descriptors.

`TIMM_OSAL_ReadFromPipe` also requires signed read-result handling, EINTR retry,
and complete-message validation. The ownership patch leaves that separate
transport mechanism unchanged. The host doubles establish local ownership
behavior; product linking, Ducati execution, restart, and teardown need separate
integration and hardware evidence. The plugin mapping-failure guarantee depends
on libion closing its hidden map descriptor when `mmap` fails.
