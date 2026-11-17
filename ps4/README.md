# Colibri on PS4: first build probe

This is an experimental fork. The first milestone cross-compiles and links the
existing CPU engine for the PS4's x86-64 FreeBSD target. It does not create a
fake PKG, add a PS4 UI, or claim that the executable has run on a console.

The probe intentionally leaves OpenMP and all GPU backends disabled. Colibri's
current CUDA, Vulkan, HIP, and Metal backends do not target the PS4 GPU.

## Build

Install Zig and the OpenOrbis PS4 Toolchain, with this layout under one folder:

```text
toolchain/
  zig/zig.exe
  OpenOrbis/PS4Toolchain/
```

Then point the script at that folder and build from PowerShell:

```powershell
$env:COLIBRI_PS4_TOOLCHAIN = "C:\path\to\toolchain"
.\ps4\build.ps1
```

The output is `build/ps4/colibri.elf`.

## Port status

- Cross-compilation and linking of `c/colibri.c`: works with OpenOrbis.
- PS4 package, app lifecycle, filesystem behavior, and model inference: not yet
  implemented or verified on console.
- GPU inference: not implemented. The first runtime milestone is CPU-only.
- The existing OpenAI API is a Python gateway that starts the engine as a child
  process; it cannot be reused directly in a PS4 ELF. A native API needs to call
  Colibri inference in-process. Garlic LLM is a reference for the PS4 network
  server, not a drop-in API implementation.
