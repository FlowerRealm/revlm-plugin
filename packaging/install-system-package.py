#!/usr/bin/env python3
"""Install one image-local .revlm-plugin under packages/<id>/<version>."""

from __future__ import annotations

import argparse
import json
import pathlib
import zipfile


def safe_component(value: str) -> bool:
    return bool(value) and all(char.isalnum() or char in "-_." for char in value)


def safe_member(name: str) -> pathlib.PurePosixPath:
    path = pathlib.PurePosixPath(name)
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        raise ValueError(f"unsafe archive member: {name}")
    return path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("archive", type=pathlib.Path)
    parser.add_argument("--root", type=pathlib.Path, required=True)
    args = parser.parse_args()

    with zipfile.ZipFile(args.archive) as archive:
        try:
            manifest = json.loads(archive.read("plugin.json"))
        except (KeyError, json.JSONDecodeError) as error:
            raise SystemExit(f"invalid plugin package manifest: {error}") from error
        plugin_id = manifest.get("id")
        version = manifest.get("version")
        if not isinstance(plugin_id, str) or not isinstance(version, str) or not safe_component(plugin_id) or not safe_component(version):
            raise SystemExit("plugin package has an unsafe id or version")

        destination = args.root / "packages" / plugin_id / version
        destination.mkdir(parents=True, exist_ok=False)
        for member in archive.infolist():
            if member.is_dir():
                continue
            relative = safe_member(member.filename)
            output = destination.joinpath(*relative.parts)
            output.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(member) as source, output.open("wb") as target:
                target.write(source.read())


if __name__ == "__main__":
    main()
