#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${project_dir}/build}"
artifact_dir="${2:-${build_dir}/face-check}"
mkdir -p "$artifact_dir"
cmake --build "$build_dir" --target qzdesk_screen --parallel 4
cc -std=gnu99 -O2 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DQZDESK_SIMULATOR=1 \
    -DQZ_SCREEN_W=320 -DQZ_SCREEN_H=240 \
    -I"$project_dir/include" -I"$project_dir/third_party" -I"$project_dir/third_party/lvgl" \
    "$project_dir/tools/check_face.c" "$project_dir/app/ai_face.c" "$project_dir/app/scale.c" \
    -L"$build_dir/lib" -Wl,--start-group -llvgl -llvgl_thorvg -Wl,--end-group \
    -lstdc++ -lm -lpthread -lSDL2 -o "$artifact_dir/check_face"
for bits in 16 24 32; do
    for mode in motion reduced; do
        output="$artifact_dir/$bits-$mode"
        mkdir -p "$output"
        "$artifact_dir/check_face" "$output" "$mode" "$bits" > "$output/report.txt" 2>&1
        tail -n 1 "$output/report.txt"
        if rg -q '\[Error\]|\[Warn\]|FAIL' "$output/report.txt"; then
            cat "$output/report.txt"
            exit 1
        fi
    done
done
