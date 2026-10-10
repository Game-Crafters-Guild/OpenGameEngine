#!/usr/bin/env python3
"""Build the Deck runtime on macOS/Linux or Windows with WSL2 Docker integration."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


def promote(candidate, output):
    candidate, output = candidate.resolve(), output.resolve()
    if candidate == output or output.is_relative_to(candidate):
        raise ValueError("Candidate and output must be different, non-overlapping directories")
    names = ("steamdeck-player", "steamdeck-player.tar.gz")
    if not (candidate / names[0] / "Player").is_file() or not (candidate / names[1]).is_file():
        raise ValueError("Incomplete Deck candidate")
    output.mkdir(parents=True, exist_ok=True)
    backups, installed = [], []
    try:
        for name in names:
            target = output / name
            if target.exists():
                backup = candidate / (name + ".previous")
                if backup.exists():
                    raise ValueError("Candidate contains a previous incomplete promotion; recover it first")
                target.rename(backup)
                backups.append((backup, target))
            (candidate / name).rename(target)
            installed.append((target, candidate / name))
    except BaseException:
        for target, original in reversed(installed):
            target.rename(original)
        for backup, target in reversed(backups):
            backup.rename(target)
        raise
    for backup, _ in backups:
        if backup.is_dir():
            shutil.rmtree(backup)
        else:
            backup.unlink()


def wsl_path(path):
    return subprocess.check_output(["wsl.exe", "--exec", "wslpath", "-a", str(path.resolve())], text=True).strip()


def build(project, output, scene, pipeline, game_name, flags):
    repo = Path(__file__).resolve().parents[2]
    values = {"GE_STEAMDECK_PROJECT_ROOT": str(project.resolve()),
              "GE_STEAMDECK_HOST_DIST_DIR": str(output.resolve()),
              "GE_STEAMDECK_STARTUP_SCENE": scene,
              "GE_STEAMDECK_RENDER_PIPELINE": pipeline,
              "GE_STEAMDECK_GAME_NAME": game_name}
    if os.name == "nt":
        # WSL performs compilation and owns its Linux toolchain. Keep command
        # arguments separate: spaces/apostrophes in project paths are literal.
        linux_repo = wsl_path(repo)
        values["GE_STEAMDECK_PROJECT_ROOT"] = wsl_path(project)
        values["GE_STEAMDECK_HOST_DIST_DIR"] = wsl_path(output)
        command = ["wsl.exe", "--cd", linux_repo, "--exec", "env"]
        command += [f"{key}={value}" for key, value in values.items()]
        command += ["bash", linux_repo + "/Tools/Scripts/build-steamdeck-docker.sh", *flags]
        subprocess.run(command, check=True)
    else:
        subprocess.run(["bash", str(repo / "Tools/Scripts/build-steamdeck-docker.sh"), *flags],
                       env={**os.environ, **values}, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, default=Path.cwd())
    parser.add_argument("--output", type=Path, default=Path("Build/SteamDeck"))
    parser.add_argument("--scene", default="")
    parser.add_argument("--pipeline", default="RenderPipelines/ForwardPlus.rendergraph")
    parser.add_argument("--game-name", default="Open Engine Player")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--unity", action="store_true")
    parser.add_argument("--skip-image-build", action="store_true")
    parser.add_argument("--promote", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        if args.promote:
            promote(args.promote, args.output)
        else:
            flags = [flag for flag, enabled in (("--check", args.check), ("--unity", args.unity),
                                               ("--skip-image-build", args.skip_image_build)) if enabled]
            build(args.project, args.output, args.scene, args.pipeline, args.game_name, flags)
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Deck build: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
