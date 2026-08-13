#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${TMPDIR:-/tmp}/dragon-touch-ui-preview"
mode=single
scenario=printing
page=home
output_png=

usage()
{
    echo "Usage: $0 [--interactive] [--scenario NAME] [--page NAME] [--all|--check] [output.png]" >&2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --interactive|--all|--check)
            mode=${1#--}
            ;;
        --scenario)
            [ "$#" -ge 2 ] || { usage; exit 2; }
            scenario=$2
            shift
            ;;
        --page)
            [ "$#" -ge 2 ] || { usage; exit 2; }
            page=$2
            shift
            ;;
        -*)
            usage
            exit 2
            ;;
        *)
            [ -z "$output_png" ] || { usage; exit 2; }
            output_png=$1
            ;;
    esac
    shift
done

test -d "$repo_dir/managed_components/lvgl__lvgl" || {
    echo "LVGL is missing; run an ESP-IDF dependency build first." >&2
    exit 1
}

cmake -S "$repo_dir/tools/ui_preview" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir"

if [ "$mode" = interactive ]; then
    exec "$build_dir/dragon_touch_ui_preview" --interactive --scenario "$scenario" --page "$page"
fi

render_scenario()
{
    render_name=$1
    render_output=$2
    render_page=${3:-home}
    render_bmp="$build_dir/ui-$render_name-$render_page.bmp"
    mkdir -p "$(dirname -- "$render_output")"
    render_attempt=1
    while [ "$render_attempt" -le 5 ]; do
        "$build_dir/dragon_touch_ui_preview" --scenario "$render_name" --page "$render_page" "$render_bmp"
        magick "$render_bmp" -strip "$render_output"
        render_colors=$(magick identify -format '%k' "$render_output")
        if [ "$render_colors" -gt 10 ]; then
            return 0
        fi
        echo "Retrying blank SDL capture: $render_name (attempt $render_attempt)" >&2
        render_attempt=$((render_attempt + 1))
    done
    echo "Could not capture a rendered SDL frame: $render_name" >&2
    return 1
}

asset_for_scenario()
{
    case "$1" in
        printing) echo "$repo_dir/docs/assets/ui-preview.png" ;;
        *) echo "$repo_dir/docs/assets/ui-$1.png" ;;
    esac
}

if [ "$mode" = all ] || [ "$mode" = check ]; then
    check_failed=0
    for render_name in printing disconnected idle paused fault; do
        expected=$(asset_for_scenario "$render_name")
        if [ "$mode" = all ]; then
            render_scenario "$render_name" "$expected"
            echo "$expected"
        else
            actual="$build_dir/check-$render_name.png"
            render_scenario "$render_name" "$actual"
            if [ ! -f "$expected" ]; then
                echo "Missing baseline: $expected" >&2
                check_failed=1
            elif ! magick compare -metric AE "$expected" "$actual" null: 2>/dev/null; then
                echo "Changed pixels: $render_name" >&2
                check_failed=1
            else
                echo "Matched: $render_name"
            fi
        fi
    done
    exit "$check_failed"
fi

if [ -z "$output_png" ]; then
    output_png=$(asset_for_scenario "$scenario")
fi
render_scenario "$scenario" "$output_png" "$page"
echo "$output_png"
