# Prepends a release entry to mod_list.json (OpenGOAL mod-source format).
# Adapted from OG-Fortnite-Building's update-mod-list.py for this repo's
# tag scheme: release tags are `sm64jak-vX.Y.Z` (distinct prefix because the
# fork inherits upstream jak-project tags), while the asset keeps the
# launcher-standard name `windows-vX.Y.Z.zip`. Windows-only.
import json
import os
from datetime import datetime, timezone

raw_version = os.environ["VERSION"]  # accepts sm64jak-v0.0.3, v0.0.3, or 0.0.3
supported_games = os.environ.get("SUPPORTED_GAMES", "jak1").split(",")
repo = os.environ.get("GITHUB_REPOSITORY", "Zedb0T/jak-project")

ver = raw_version
if ver.startswith("sm64jak-"):
    ver = ver[len("sm64jak-"):]
if ver.startswith("v"):
    ver = ver[1:]
tag = f"sm64jak-v{ver}"

mod_list_path = os.path.join(os.environ.get("GITHUB_WORKSPACE", "."), "mod_list.json")

with open(mod_list_path, "r", encoding="utf-8") as f:
    mod_list = json.load(f)

mod = mod_list["mods"]["sm64-jak"]

base_url = f"https://github.com/{repo}/releases/download/{tag}"

new_version = {
    "version": ver,
    "publishedDate": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "supportedGames": [g.strip() for g in supported_games],
    "assets": {
        "windows": f"{base_url}/windows-v{ver}.zip",
    },
    "assetDownloadCounts": {
        "windows": 0,
    },
}

# Deduplicate — don't add the same version twice
mod["versions"] = [v for v in mod["versions"] if v["version"] != ver]
mod["versions"].insert(0, new_version)

# Update top-level supportedGames from all versions
all_games = set()
for v in mod["versions"]:
    if v.get("supportedGames"):
        all_games.update(v["supportedGames"])
if all_games:
    mod["supportedGames"] = sorted(all_games)

mod_list["lastUpdated"] = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

with open(mod_list_path, "w", encoding="utf-8") as f:
    json.dump(mod_list, f, indent=2)
    f.write("\n")

print(f"Updated mod_list.json with version {ver} (tag {tag})")
