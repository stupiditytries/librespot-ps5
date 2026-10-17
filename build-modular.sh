#!/usr/bin/env bash
# One build pipeline: folder app -> embedded installer -> background receiver.
set -euo pipefail
repo=$(cd "$(dirname "$0")" && pwd)
if [[ -f "$repo/.local/build-env.sh" ]];then source "$repo/.local/build-env.sh";fi
bash "$repo/player/ps5/app/build.sh"
app=${PLAYER_WORK:-$repo/player/build/ps5}/dist/PPSA99730
export APP_BUNDLE="$repo/build/player-bundle"
python3 "$repo/connectd/app-install/bundle.py" "$app" "$APP_BUNDLE"
export REPO="$repo" TEMPLATE="${PS5_TEMPLATE:?Set PS5_TEMPLATE}"
bash "$repo/connectd/modular/build-receiver.sh"
