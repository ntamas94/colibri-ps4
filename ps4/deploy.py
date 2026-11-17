import argparse
import ftplib
import os
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "ps4"
MODEL = OUT / "model"
PKG = OUT / "IV0000-COLB00001_00-COLIBRIAPI000001.pkg"


def ensure_dir(ftp, name):
    try:
        ftp.mkd(name)
    except ftplib.error_perm as exc:
        if not str(exc).startswith("550"):
            raise
    ftp.cwd(name)


def main():
    ap = argparse.ArgumentParser(description="Upload the PKG, OLMoE model and API key to a PS4 GoldHEN FTP server.")
    ap.add_argument("--ip", default=os.environ.get("PS4_IP", ""))
    ap.add_argument("--port", type=int, default=int(os.environ.get("PS4_FTP_PORT", "2121")))
    ap.add_argument("--key-file", type=Path, required=True)
    args = ap.parse_args()
    if not args.ip:
        ap.error("pass --ip or set PS4_IP")
    for p in (PKG, MODEL / "config.json", MODEL / "gguf.json", MODEL / "tokenizer.json",
              MODEL / "tokenizer_config.json", MODEL / "generation_config.json",
              MODEL / "OLMoE-1B-7B-0125-Instruct-Q4_K_M.gguf", args.key_file):
        if not p.is_file():
            raise SystemExit(f"missing upload file: {p}")
    key = args.key_file.read_text(encoding="ascii").strip()
    if len(key) < 32 or any(c not in "0123456789abcdefABCDEF" for c in key):
        raise SystemExit("API key file must contain at least 32 hexadecimal characters")

    with ftplib.FTP() as ftp:
        ftp.connect(args.ip, args.port, timeout=120)
        ftp.login()
        ftp.cwd("/data")
        with args.key_file.open("rb") as f:
            ftp.storbinary("STOR colibri_api_key.txt", f, blocksize=1 << 20)
        ensure_dir(ftp, "pkg")
        with PKG.open("rb") as f:
            ftp.storbinary(f"STOR {PKG.name}", f, blocksize=1 << 20)
        ftp.cwd("/data")
        ensure_dir(ftp, "colibri-ps4")
        ensure_dir(ftp, "model")
        total = sum(p.stat().st_size for p in MODEL.iterdir() if p.is_file())
        done = 0
        for p in sorted(MODEL.iterdir(), key=lambda item: item.stat().st_size if item.is_file() else 0):
            if not p.is_file():
                continue
            last_report = [0]

            def progress(block):
                nonlocal done
                done += len(block)
                if done - last_report[0] >= 256 << 20 or done == total:
                    print(f"Model upload: {done / 1e9:.2f}/{total / 1e9:.2f} GB", flush=True)
                    last_report[0] = done

            with p.open("rb") as f:
                ftp.storbinary(f"STOR {p.name}", f, blocksize=1 << 20, callback=progress)
        print(f"Uploaded package and model to {args.ip}.")


if __name__ == "__main__":
    main()
