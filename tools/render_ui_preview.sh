#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${TMPDIR:-/tmp}/dragon-touch-ui-preview"
output_png="${1:-$repo_dir/docs/assets/ui-preview.png}"
output_bmp="$build_dir/ui-preview.bmp"

test -d "$repo_dir/managed_components/lvgl__lvgl" || {
    echo "LVGL is missing; run an ESP-IDF dependency build first." >&2
    exit 1
}

cmake -S "$repo_dir/tools/ui_preview" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir"

if [ "${1:-}" = "--interactive" ]; then
    exec "$build_dir/dragon_touch_ui_preview" --interactive
fi

mkdir -p "$(dirname -- "$output_png")"
"$build_dir/dragon_touch_ui_preview" "$output_bmp"
magick "$output_bmp" "$output_png"
echo "$output_png"
