#!/usr/bin/env python3
"""Build a format-v2 .revlm-plugin ZIP from precompiled modules."""

from __future__ import annotations

import argparse
import json
import pathlib
import shutil
import tempfile
import zipfile


ROOT = pathlib.Path(__file__).resolve().parents[1]


def add_tree(archive: zipfile.ZipFile, source: pathlib.Path, destination: pathlib.PurePosixPath) -> None:
    for path in sorted(source.rglob("*")):
        if not path.is_file():
            continue
        archive.write(path, (destination / path.relative_to(source)).as_posix())


def target_for(manifest: dict[str, object], os_name: str, arch: str) -> dict[str, object]:
    matches = [
        target
        for target in manifest.get("targets", [])
        if isinstance(target, dict) and target.get("os") == os_name and target.get("arch") == arch
    ]
    if len(matches) != 1 or not isinstance(matches[0].get("module"), str):
        raise SystemExit(f"manifest has no unique module for {os_name}-{arch}")
    return matches[0]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("plugin", help="directory name below plugins/")
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
    if not source.is_dir():
        raise SystemExit(f"plugin source directory does not exist: {source}")
    try:
        manifest = json.loads((source / "plugin.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit(f"invalid plugin manifest: {error}") from error
    if not isinstance(manifest, dict):
        raise SystemExit("plugin manifest must be an object")
    if args.system_target:
        if args.linux_amd64 or args.linux_arm64 or not args.module or not args.module.is_file():
            raise SystemExit("--system-target requires exactly --module <compiled shared library>")
    elif not args.linux_amd64 or not args.linux_arm64 or not args.linux_amd64.is_file() or not args.linux_arm64.is_file():
        raise SystemExit("both --linux-amd64 and --linux-arm64 must point to compiled shared libraries")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f"revlm-{args.plugin}-") as temporary:
        stage = pathlib.Path(temporary)
        if args.system_target:
            target_os, target_arch = args.system_target.split("-", 1)
            target = target_for(manifest, target_os, target_arch)
            manifest["targets"] = [target]
        (stage / "plugin.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        frontend = source / "frontend"
        if frontend.is_dir():
            shutil.copytree(frontend, stage / "frontend")
        migrations = source / "migrations"
        if migrations.exists():
            shutil.copytree(migrations, stage / "migrations")
        if args.system_target:
            module = stage / target["module"]
            module.parent.mkdir(parents=True)
            shutil.copy2(args.module, module)
        else:
            amd64_target = stage / target_for(manifest, "linux", "amd64")["module"]
            arm64_target = stage / target_for(manifest, "linux", "arm64")["module"]
            amd64_target.parent.mkdir(parents=True)
            arm64_target.parent.mkdir(parents=True)
            shutil.copy2(args.linux_amd64, amd64_target)
            shutil.copy2(args.linux_arm64, arm64_target)

        with zipfile.ZipFile(args.output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            add_tree(archive, stage, pathlib.PurePosixPath("."))


if __name__ == "__main__":
    main()
