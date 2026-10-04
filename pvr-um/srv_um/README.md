# libsrv_um_cr

`libsrv_um_cr` is a clean-room implementation of the PowerVR services
user-mode library for the SGX540 `pvrsrvkm` module in `pvr-source/`. It
implements the services core: connection, device enumeration and
acquisition, device memory, sync operations, misc info, display-class and
buffer-class bridges, app hints, debug output and utilities. It exports the
91 functions in `exports.txt`. Every bridge call is one ioctl on
`/dev/pvrsrvkm` carrying a `PVRSRV_BRIDGE_PACKAGE`. `srv_um_abi.c` pins each
request code, input and output size, structure layout and `PVRSRV_ERROR`
value against the kernel headers in `pvr-source/`.

The SGX client layer, meaning the per-device connect check and the
dump-trace callbacks, is outside this library: the `apfnDevConnect` and
`apfnDumpTrace` tables stay empty.

## Provenance

The implementation was written from a functional specification, without
access to vendor binaries or their disassembly. The specification is
`spec/libsrv_um.md` in the private SGX540-Research repository at commit
`a9932cd83f2c46368deda676771b2599876790ca`, sha256
`a1619e4be3516741eb312b2ee75442a907c7b776f087d92315b4b817888d3ba0`.
The private repository also holds the record of where this library departs
from the specification, and the questions on-device traces settle.

## Build

- Soong: `libsrv_um_cr`, a vendor shared library. Its distinct name keeps
  it from replacing `libsrv_um.so` until a product selects it.
- NDK: `ndk-build.sh [all|build|tidy]` builds the library and the
  `srv_um_cr_probe` device program. It checks the export set against
  `exports.txt` and runs clang-tidy with warnings as errors; its header
  lists the environment it reads.

`srv_um_cr_probe` exercises the library against the running `pvrsrvkm`
and prints one line per step: connect, enumerate, acquire, a memory context
with its heaps, an allocation with CPU write and read-back, misc info, an
event wait, display- and buffer-class enumeration, and teardown.
