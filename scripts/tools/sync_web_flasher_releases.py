#!/usr/bin/env python3
import argparse
import json
import os
import pathlib
import sys
import urllib.error
import urllib.request


DEFAULT_REPOSITORY = "M45Core/M45-Gamma-Firmware"
DEFAULT_BOARD_VERSION = "602"
NVS_START = 0x9000
NVS_SIZE = 0x6000


def die(message):
    print(f"error: {message}", file=sys.stderr)
    return 1


def request_headers(token=None, accept="application/vnd.github+json"):
    headers = {
        "Accept": accept,
        "User-Agent": "m45-web-flasher-release-sync",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    if token:
        headers["Authorization"] = f"Bearer {token}"
    return headers


def fetch_json(url, token=None):
    request = urllib.request.Request(url, headers=request_headers(token))
    with urllib.request.urlopen(request) as response:
        return json.loads(response.read().decode("utf-8"))


def download(url, destination, token=None):
    destination.parent.mkdir(parents=True, exist_ok=True)
    request = urllib.request.Request(
        url,
        headers=request_headers(token, accept="application/octet-stream"),
    )
    with urllib.request.urlopen(request) as response:
        destination.write_bytes(response.read())


def load_existing_manifest(site_dir):
    path = site_dir / "firmware-releases.json"
    if not path.exists():
        return {}
    return json.loads(path.read_text())


def local_releases(manifest):
    releases = []
    for release in manifest.get("releases", []):
        path = str(release.get("path", ""))
        if path and not path.startswith("firmware/"):
            releases.append(release)
    return releases


def release_pages(repository, token, max_releases):
    releases = []
    page = 1
    while len(releases) < max_releases:
        url = f"https://api.github.com/repos/{repository}/releases?per_page=100&page={page}"
        page_releases = fetch_json(url, token)
        if not page_releases:
            break
        releases.extend(page_releases)
        page += 1
    return releases[:max_releases]


def matching_asset(release, asset_prefix):
    expected_prefix = f"{asset_prefix}{release['tag_name']}"
    for asset in release.get("assets", []):
        name = asset.get("name", "")
        if name.startswith(expected_prefix) and name.endswith(".bin"):
            return asset
    return None


def sync_releases(site_dir, repository, board_version, max_releases, include_prereleases, token):
    manifest = load_existing_manifest(site_dir)
    asset_prefix = f"esp-miner-factory-{board_version}-"
    by_version = {}

    for release in local_releases(manifest):
        version = str(release.get("version", ""))
        if version:
            by_version[version] = release

    for release in release_pages(repository, token, max_releases):
        if release.get("draft"):
            continue
        if release.get("prerelease") and not include_prereleases:
            continue
        asset = matching_asset(release, asset_prefix)
        if not asset:
            continue

        tag = release["tag_name"]
        name = asset["name"]
        relative_path = pathlib.PurePosixPath("firmware", tag, name)
        destination = site_dir / relative_path
        if not destination.exists() or destination.stat().st_size != int(asset.get("size", 0)):
            download(asset["browser_download_url"], destination, token)

        by_version[tag] = {
            "version": tag,
            "name": release.get("name") or tag,
            "path": str(relative_path),
            "digest": asset.get("digest", ""),
            "size": asset.get("size", 0),
            "published_at": release.get("published_at", ""),
            "source": "github-release",
        }

    output = {
        "name": manifest.get("name", "M45 Gamma Firmware"),
        "repository": repository,
        "board_version": board_version,
        "asset_prefix": asset_prefix,
        "nvs": {
            "offset": NVS_START,
            "size": NVS_SIZE,
        },
        "releases": list(by_version.values()),
    }
    (site_dir / "firmware-releases.json").write_text(json.dumps(output, indent=2) + "\n")
    return len(output["releases"])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--site-dir", default="site")
    parser.add_argument("--repository", default=DEFAULT_REPOSITORY)
    parser.add_argument("--board-version", default=DEFAULT_BOARD_VERSION)
    parser.add_argument("--max-releases", type=int, default=100)
    parser.add_argument("--include-prereleases", action="store_true")
    args = parser.parse_args()

    site_dir = pathlib.Path(args.site_dir)
    if not site_dir.exists():
        return die(f"{site_dir} does not exist")

    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    try:
        count = sync_releases(
            site_dir,
            args.repository,
            args.board_version,
            args.max_releases,
            args.include_prereleases,
            token,
        )
    except (KeyError, OSError, urllib.error.URLError, json.JSONDecodeError) as err:
        return die(str(err))

    print(f"web flasher releases: {count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
