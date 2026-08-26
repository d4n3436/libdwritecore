# Third party material

Everything here is proprietary Microsoft material vendored as input to the build.

The files here are not modified in place, and are instead read by tools to
source-generate files required to build the shim.

## windows-app-sdk/

The DWriteCore headers from the Windows App SDK: `dwrite.h`, `dwrite_1.h`,
`dwrite_2.h`, `dwrite_3.h`, `dwrite_core.h` and `DWriteExperimental.h`.

`tools/mirror_headers.py` mirrors these into `include/`.

## windows-sdk/

The subset of the Windows SDK the DWriteCore headers depend on, in the SDK's
own `shared/` and `um/` layout. `tools/mirror_sdk_headers.py` extracts only the
declarations that are actually needed.

## office-android/

`libdwritecore.so`, extracted from Microsoft Copilot
(`com.microsoft.office.officehubrow`) 16.0.20409.20036.
It is an Android/Bionic build of DWriteCore for x86-64, and it's the
implementation the shim forwards to, and the only binary any tool reads.

`sha256: 3da882f4f2697968816b90c1031d3dd212d7c93f464a2da14339715b6936563d`