# Colibri on PS4: OLMoE API package

This is an experimental fork. It builds a PS4 package with Colibri's OLMoE
engine and an in-process OpenAI-compatible API. Console inference is not yet
verified.

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
python .\ps4\package.py --toolchain $env:COLIBRI_PS4_TOOLCHAIN
```

The outputs are `build/ps4/olmoe.elf` and
`build/ps4/IV0000-COLB00001_00-COLIBRIAPI000001.pkg`.

The package expects model files under `/data/colibri-ps4/model` and an API key
at `/data/colibri_api_key.txt`. `ps4/deploy.py` uploads the package, model and
key to a GoldHEN FTP server. Then install the PKG with GoldHEN's Package
Installer and launch **Colibri PS4 API** from the home screen.

```powershell
python .\ps4\deploy.py --ip 192.168.1.253 --key-file C:\path\to\colibri-ps4-api-key.txt
```

Once the app is running, point an OpenAI client at `http://<ps4-ip>:8080/v1`:

```bash
curl http://<ps4-ip>:8080/v1/chat/completions \
  -H 'Authorization: Bearer <key>' -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"Hello"}],"max_tokens":64}'
```

## Port status

- Cross-compilation, linking, and PKG creation work with OpenOrbis.
- The installed app starts an in-process, single-request-at-a-time HTTP API:
  `GET /health`, `GET /v1/models`, and `POST /v1/chat/completions`.
- The API accepts text messages and non-streaming completions. Tools, images,
  and streaming are not implemented. It requires a bearer key in
  `/data/colibri_api_key.txt` before it opens port 8080.
- App launch, filesystem access and inference have not been verified on console.
- GPU inference: not implemented. The first runtime milestone is CPU-only.
- The existing PC OpenAI API is a Python gateway that starts the engine as a
  child process. The PS4 build instead calls OLMoE inference in-process; Garlic
  LLM informed its console socket setup.
