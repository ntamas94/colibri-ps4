# Colibri on PS4: OLMoE engine probe

This is an experimental fork. The first milestone cross-compiles and links the
repository's smallest model engine, OLMoE, for the PS4's x86-64 FreeBSD target.
It does not create a PS4 package or claim that inference has run on a console.

The probe intentionally leaves OpenMP and all GPU backends disabled. Colibri's
current CUDA, Vulkan, HIP, and Metal backends do not target the PS4 GPU. OLMoE
is the smallest engine in this repository, but its Q4 model is still about 4 GB;
whether it fits alongside PS4 system memory use has not been established.

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

The output is `build/ps4/olmoe.elf`.

The ELF must be launched by a PS4 homebrew loader with `SNAP` pointing to an
OLMoE model folder and `PS4_API=1`. The model itself is not included in this
repository. Once running, point an OpenAI client at `http://<ps4-ip>:8080/v1`.
Create `/data/colibri_api_key.txt` first; the API refuses to listen without a
key. For example:

```bash
curl http://<ps4-ip>:8080/v1/chat/completions \
  -H 'Authorization: Bearer <key>' -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"Hello"}],"max_tokens":64}'
```

## Port status

- Cross-compilation and linking of `c/olmoe.c`: works with OpenOrbis.
- `PS4_API=1` enables an in-process, single-request-at-a-time HTTP API:
  `GET /health`, `GET /v1/models`, and `POST /v1/chat/completions`.
- The API accepts text messages and non-streaming completions. Tools, images,
  and streaming are not implemented. It requires a bearer key in
  `/data/colibri_api_key.txt` before it opens port 8080.
- PS4 package and app lifecycle are not implemented. The in-process model path
  and filesystem assumptions have not been verified on console.
- GPU inference: not implemented. The first runtime milestone is CPU-only.
- The existing PC OpenAI API is a Python gateway that starts the engine as a
  child process. The PS4 build instead calls OLMoE inference in-process; Garlic
  LLM informed its console socket setup.
