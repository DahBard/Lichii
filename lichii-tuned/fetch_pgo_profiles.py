"""Download the PGO profiles the owner recorded with a pgo-train build: the .zip and .profraw
files attached to this repository's releases whose tag starts with "pgo-".

usage: python3 lichii-tuned/fetch_pgo_profiles.py <output folder>

Needs GITHUB_TOKEN and GITHUB_REPOSITORY (both set in the workflow). It never fails the build:
without a token, a network error or no such release it says so and downloads nothing.
"""
import json
import os
import sys
import urllib.error
import urllib.request

AGENT = {"User-Agent": "lichii-tuned-ci"}


class NoRedirect(urllib.request.HTTPRedirectHandler):
    # The asset API redirects to signed storage, which must not get the token.
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def download(url, headers, target):
    opener = urllib.request.build_opener(NoRedirect)
    try:
        response = opener.open(urllib.request.Request(url, headers=headers), timeout=120)
    except urllib.error.HTTPError as error:
        if error.code not in (301, 302, 303, 307, 308):
            raise
        response = urllib.request.urlopen(
            urllib.request.Request(error.headers["Location"], headers=AGENT), timeout=600)
    with response, open(target, "wb") as out:
        while chunk := response.read(1 << 20):
            out.write(chunk)


def main():
    out_dir = sys.argv[1]
    token = os.environ.get("GITHUB_TOKEN", "")
    repo = os.environ.get("GITHUB_REPOSITORY", "")
    if not token or not repo:
        print("fetch_pgo_profiles: no GITHUB_TOKEN or GITHUB_REPOSITORY, nothing fetched")
        return
    api = {**AGENT, "Authorization": f"Bearer {token}",
           "Accept": "application/vnd.github+json", "X-GitHub-Api-Version": "2022-11-28"}
    request = urllib.request.Request(
        f"https://api.github.com/repos/{repo}/releases?per_page=100", headers=api)
    with urllib.request.urlopen(request, timeout=60) as response:
        releases = json.load(response)
    os.makedirs(out_dir, exist_ok=True)
    fetched = 0
    for release in releases:
        tag = release.get("tag_name", "")
        if release.get("draft") or not tag.startswith("pgo-"):
            continue
        for asset in release.get("assets", []):
            name = os.path.basename(asset.get("name", ""))
            if not name.endswith((".zip", ".profraw")):
                continue
            target = os.path.join(out_dir, f"{tag}-{name}")
            download(asset["url"], {**api, "Accept": "application/octet-stream"}, target)
            print(f"fetch_pgo_profiles: {tag}/{name}: {os.path.getsize(target)} bytes")
            fetched += 1
    print(f"fetch_pgo_profiles: {fetched} file(s) from pgo-* releases")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:  # noqa: BLE001 - a missing profile must not fail the build
        print(f"fetch_pgo_profiles: nothing fetched ({error})")
