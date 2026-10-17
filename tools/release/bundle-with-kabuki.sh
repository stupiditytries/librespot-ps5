#!/usr/bin/env bash
# Turns the draft release made by GitHub Actions into the download people use: one ZIP with
# the receiver and Kabuki. Kabuki's repository is private, so this runs with your own gh login.
#   bundle-with-kabuki.sh TAG [KABUKI_TAG]
set -euo pipefail
tag=${1:?Usage: bundle-with-kabuki.sh TAG [KABUKI_TAG]}
kabuki_tag=${2:-v0.2.0-rc.1}
kabuki_repo=stupiditytries/kabuki-ps5
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/librespot" "$work/kabuki" "$work/zip"

# The receiver from this repository's draft, checked against the checksums the build made.
gh release download "$tag" --dir "$work/librespot" \
    --pattern librespot-ps5.elf --pattern SHA256SUMS.txt --pattern BUILD-INFO.txt
(cd "$work/librespot" && sha256sum --check SHA256SUMS.txt)

# Kabuki, unchanged, checked against its own release's checksums.
gh release download "$kabuki_tag" --repo "$kabuki_repo" --dir "$work/kabuki" \
    --pattern kabuki-ps5.elf --pattern SHA256SUMS.txt
(cd "$work/kabuki" && sha256sum --check --ignore-missing SHA256SUMS.txt)

zip_name=librespot-ps5-$tag.zip
cp "$work/librespot/librespot-ps5.elf" "$work/kabuki/kabuki-ps5.elf" "$work/zip/"
python3 - "$work/zip" "$work/$zip_name" <<'PY'
import sys, zipfile, pathlib
source, target = pathlib.Path(sys.argv[1]), sys.argv[2]
with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as archive:
    for name in ('kabuki-ps5.elf', 'librespot-ps5.elf'):
        info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o644 << 16
        archive.writestr(info, (source / name).read_bytes())
PY
printf 'Kabuki: %s %s\n' "$kabuki_repo" "$kabuki_tag" >> "$work/librespot/BUILD-INFO.txt"
(cd "$work" && cp librespot/BUILD-INFO.txt . && cp zip/*.elf . &&
    sha256sum "$zip_name" librespot-ps5.elf kabuki-ps5.elf BUILD-INFO.txt > SHA256SUMS.txt && cat SHA256SUMS.txt)

gh release upload "$tag" --clobber "$work/$zip_name" "$work/SHA256SUMS.txt" "$work/BUILD-INFO.txt"
gh release delete-asset "$tag" librespot-ps5.elf --yes
echo "Bundled $zip_name into the draft of $tag"
