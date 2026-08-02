#!/usr/bin/env python3
"""Build a deterministic V1 .revlm-plugin ZIP from two precompiled modules."""

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

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f"revlm-{args.plugin}-") as temporary:
        stage = pathlib.Path(temporary)
        manifest = json.loads((source / "plugin.json").read_text(encoding="utf-8"))
        if args.system_target:
            target_os, target_arch = args.system_target.split("-", 1)
            targets = [
                target
                for target in manifest["targets"]
                if target["os"] == target_os and target["arch"] == target_arch
            ]
            if len(targets) != 1:
                raise SystemExit(f"manifest has no unique target for {args.system_target}")
            manifest["targets"] = targets
        (stage / "plugin.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        shutil.copytree(source / "frontend", stage / "frontend")
        migrations = source / "migrations"
        if migrations.exists():
            shutil.copytree(migrations, stage / "migrations")
        if args.system_target:
            target = stage / "backend" / args.system_target / f"lib{args.plugin}.so"
            target.parent.mkdir(parents=True)
            shutil.copy2(args.module, target)
        else:
            amd64_target = stage / "backend" / "linux-amd64" / f"lib{args.plugin}.so"
            arm64_target = stage / "backend" / "linux-arm64" / f"lib{args.plugin}.so"
            amd64_target.parent.mkdir(parents=True)
            arm64_target.parent.mkdir(parents=True)
            shutil.copy2(args.linux_amd64, amd64_target)
            shutil.copy2(args.linux_arm64, arm64_target)

        with zipfile.ZipFile(args.output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            add_tree(archive, stage, pathlib.PurePosixPath("."))


if __name__ == "__main__":
    main()
