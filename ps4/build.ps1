param(
    [string]$Toolchain = $env:COLIBRI_PS4_TOOLCHAIN
)

$ErrorActionPreference = "Stop"

if (-not $Toolchain) {
    throw "Set COLIBRI_PS4_TOOLCHAIN to a folder containing zig\zig.exe and OpenOrbis\PS4Toolchain."
}

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$toolchainRoot = (Resolve-Path $Toolchain).Path
$zig = Join-Path $toolchainRoot "zig\zig.exe"
$oo = Join-Path $toolchainRoot "OpenOrbis\PS4Toolchain"
$build = Join-Path $root "build\ps4"

foreach ($path in @($zig, (Join-Path $oo "include"), (Join-Path $oo "link.x"), (Join-Path $oo "lib\crt1.o"))) {
    if (-not (Test-Path $path)) { throw "Missing PS4 toolchain file: $path" }
}

New-Item -ItemType Directory -Force -Path $build | Out-Null
$object = Join-Path $build "colibri.o"
$elf = Join-Path $build "colibri.elf"

$compile = @(
    "cc", "-target", "x86_64-freebsd-none", "-march=btver2", "-fshort-wchar",
    "-O2", "-fPIC", "-funwind-tables", "-fno-stack-protector", "-nostdinc",
    "-isystem", (Join-Path $oo "include"), "-I", (Join-Path $root "c"),
    "-c", (Join-Path $root "c\colibri.c"), "-o", $object
)
& $zig @compile
if ($LASTEXITCODE -ne 0) { throw "PS4 compile failed with exit code $LASTEXITCODE" }

$link = @(
    "ld.lld", "-m", "elf_x86_64", "-pie", "--script", (Join-Path $oo "link.x"),
    "--eh-frame-hdr", $object, "-L", (Join-Path $oo "lib"), "-lc", "-lm",
    "-lpthread", "-lkernel", "-lSceNet", (Join-Path $oo "lib\crt1.o"), "-o", $elf
)
& $zig @link
if ($LASTEXITCODE -ne 0) { throw "PS4 link failed with exit code $LASTEXITCODE" }

Write-Output "Built PS4 ELF: $elf"
