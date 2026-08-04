#!/usr/bin/env python3
"""Build a v1 (v3 architecture) .revlm-plugin ZIP from two precompiled modules.

The package format (docs/plugin-package-format.md) is:

    plugin.json            five fields: id/type/name/description/version
    backend/amd/libX.so    exactly one .so per platform dir (filename arbitrary)
    backend/arm/libX.so
    frontend/entry.js      required; may be an empty/no-op ESM

This script stages the plugin directory (the manifest + both platform modules
plus the frontend tree) and writes a deterministic ZIP. It does not build the
modules; pass the compiled shared libraries.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import tempfile
import zipfile


ROOT = pathlib.Path(__file__).resolve().parents[1]


def add_tree(archive: zipfile.ZipFile, source: pathlib.Path, destination: pathlib.PurePosixPath) -> None:
    for path in sorted(source.rglob("*")):
        if not path.is_file():
            continue
        archive.write(path, (destination / path.relative_to(source)).as_posix())


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("plugin", choices=("OpenAI", "Anthropic"))
    parser.add_argument("--linux-amd64", type=pathlib.Path)
    parser.add_argument("--linux-arm64", type=pathlib.Path)
    parser.add_argument(
        "--system-target",
        choices=("linux-amd64", "linux-arm64"),
        help="build an image-local system package containing only this platform's module",
    )
    parser.add_argument("--module", type=pathlib.Path, help="module used with --system-target")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()

    source = ROOT / "plugins" / args.plugin
    if args.system_target:
        if args.linux_amd64 or args.linux_arm64 or not args.module or not args.module.is_file():
            raise SystemExit("--system-target requires exactly --module <compiled shared library>")
    elif not args.linux_amd64 or not args.linux_arm64 or not args.linux_amd64.is_file() or not args.linux_arm64.is_file():
        raise SystemExit("both --linux-amd64 and --linux-arm64 must point to compiled shared libraries")

    # Validate the v1 manifest (five fields, no v2 legacy fields).
    manifest = json.loads((source / "plugin.json").read_text(encoding="utf-8"))
    for legacy in ("format_version", "core_abi", "requires", "targets", "load_order", "migrations"):
        if legacy in manifest:
            raise SystemExit(f"plugin.json must not contain {legacy} (v1 manifests have only five fields)")
    for field in ("id", "type", "name", "description", "version"):
        if field not in manifest:
            raise SystemExit(f"plugin.json is missing the {field} field")
    if manifest["type"] != "channel":
        raise SystemExit("unsupported plugin type; only \"channel\" is a valid v1 type")

    if not (source / "frontend" / "entry.js").is_file():
        raise SystemExit("plugin package is missing frontend/entry.js")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f"revlm-{args.plugin}-") as temporary:
        stage = pathlib.Path(temporary)
        (stage / "plugin.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        import shutil
        shutil.copytree(source / "frontend", stage / "frontend")

        # v3 directory convention: backend/<amd|arm>/, exactly one .so per dir.
        if args.system_target:
            arch = args.system_target.split("-", 1)[1]
            arch_dir = "amd" if arch == "amd64" else "arm"
            target = stage / "backend" / arch_dir / f"lib{args.plugin}.so"
            target.parent.mkdir(parents=True)
            shutil.copy2(args.module, target)
        else:
            for arch_dir, module in (("amd", args.linux_amd64), ("arm", args.linux_arm64)):
                target = stage / "backend" / arch_dir / f"lib{args.plugin}.so"
                target.parent.mkdir(parents=True)
                shutil.copy2(module, target)

        with zipfile.ZipFile(args.output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            add_tree(archive, stage, pathlib.PurePosixPath("."))


if __name__ == "__main__":
    main()
