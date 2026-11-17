import argparse
import os
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "ps4"
STAGE = OUT / "pkg"
TITLE = "Colibri PS4 API"
TITLE_ID = "COLB00001"
CONTENT_ID = "IV0000-COLB00001_00-COLIBRIAPI000001"
VERSION = "1.01"
OO_REL = Path("OpenOrbis") / "PS4Toolchain"


def run(args, cwd=None, env=None):
    subprocess.run([str(a) for a in args], cwd=cwd, env=env, check=True)


def fix_gp4_dirs(path):
    text = path.read_text(encoding="utf-8")
    start, end = text.index("<rootdir>"), text.index("</rootdir>") + len("</rootdir>")

    def tree(folder, depth):
        out = ""
        for sub in sorted(p for p in folder.iterdir() if p.is_dir()):
            inner = tree(sub, depth + 1)
            pad = "\t" * depth
            out += f'<dir targ_name="{sub.name}">\n{inner}{pad}</dir>\n' if inner else f'<dir targ_name="{sub.name}" />\n'
        return out

    text = text[:start] + "<rootdir>\n" + tree(STAGE, 2) + "\t</rootdir>" + text[end:]
    path.write_text(text, encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description="Package the Colibri PS4 API as an OpenOrbis PKG.")
    ap.add_argument("--toolchain", type=Path, default=Path(os.environ.get("COLIBRI_PS4_TOOLCHAIN", "")))
    args = ap.parse_args()
    toolchain = args.toolchain.resolve()
    oo = toolchain / OO_REL
    zig = toolchain / "zig" / "zig.exe"
    dotnet = toolchain / "dotnet" / "dotnet.exe"
    bin_dir = oo / "bin" / "windows"
    sample = oo / "samples" / "hello_world"
    elf = OUT / "olmoe.elf"
    required = [elf, zig, dotnet, oo / "link.x", bin_dir / "create-fself.exe", bin_dir / "create-gp4.exe",
                bin_dir / "PkgTool.Core.dll", sample / "sce_sys" / "about" / "right.sprx"]
    missing = [p for p in required if not p.is_file()]
    if missing:
        raise SystemExit("Missing build/package input: " + ", ".join(map(str, missing)))

    build_env = dict(os.environ, DOTNET_ROLL_FORWARD="Major", OO_PS4_TOOLCHAIN=str(oo),
                     DOTNET_SYSTEM_GLOBALIZATION_INVARIANT="1")
    stage = STAGE.resolve()
    if not stage.is_relative_to(OUT.resolve()):
        raise SystemExit("Refusing to clean a package directory outside build/ps4")
    shutil.rmtree(stage, ignore_errors=True)
    (stage / "sce_sys" / "about").mkdir(parents=True)
    (stage / "sce_module").mkdir()

    run([bin_dir / "create-fself.exe", f"-in={elf}", f"-out={OUT / 'olmoe.oelf'}",
         f"--eboot={stage / 'eboot.bin'}", "--paid", "0x3800000000000011"], env=build_env)
    shutil.copy(sample / "sce_sys" / "about" / "right.sprx", stage / "sce_sys" / "about")
    shutil.copy(sample / "sce_sys" / "icon0.png", stage / "sce_sys")
    for prx in (sample / "sce_module").glob("*.prx"):
        shutil.copy(prx, stage / "sce_module")

    sfo = "sce_sys/param.sfo"
    pkgtool = [dotnet, bin_dir / "PkgTool.Core.dll"]
    run([*pkgtool, "sfo_new", sfo], cwd=stage, env=build_env)
    entries = [
        ("APP_TYPE", "Integer", 4, "1"), ("APP_VER", "Utf8", 8, VERSION),
        ("ATTRIBUTE", "Integer", 4, "0"), ("CATEGORY", "Utf8", 4, "gd"),
        ("CONTENT_ID", "Utf8", 48, CONTENT_ID), ("DOWNLOAD_DATA_SIZE", "Integer", 4, "0"),
        ("SYSTEM_VER", "Integer", 4, "0"), ("TITLE", "Utf8", 128, TITLE),
        ("TITLE_ID", "Utf8", 12, TITLE_ID), ("VERSION", "Utf8", 8, VERSION),
    ]
    for key, typ, size, value in entries:
        run([*pkgtool, "sfo_setentry", sfo, key, "--type", typ, "--maxsize", str(size), "--value", value],
            cwd=stage, env=build_env)

    files = ["eboot.bin", sfo, "sce_sys/about/right.sprx", "sce_sys/icon0.png"]
    files.extend(f"sce_module/{p.name}" for p in sorted((stage / "sce_module").glob("*.prx")))
    gp4 = stage / "pkg.gp4"
    run([bin_dir / "create-gp4.exe", "-out", gp4.name, f"--content-id={CONTENT_ID}", "--files", " ".join(files)],
        cwd=stage, env=build_env)
    fix_gp4_dirs(gp4)
    run([*pkgtool, "pkg_build", gp4.name, str(OUT)], cwd=stage, env=build_env)
    pkg = OUT / f"{CONTENT_ID}.pkg"
    print(f"Built: {pkg} ({pkg.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
