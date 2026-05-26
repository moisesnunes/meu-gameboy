#!/usr/bin/env bash
set -u
shopt -s globstar nullglob

manifest="${GBA_COMPAT_MANIFEST:-tests/gba_compat/manifest.tsv}"
gba_root="${GBA_COMPAT_ROOT:-roms/game-boy-advance-test-roms}"
gba_bios="${GBA_COMPAT_BIOS:-}"
out_dir="${GBA_COMPAT_OUT:-tests/gba_compat/out}"
result_file="${GBA_COMPAT_RESULT:-$out_dir/compat.txt}"
summary_file="${GBA_COMPAT_SUMMARY:-$out_dir/compat_summary.txt}"
max_override="${GBA_COMPAT_MAX_CYCLES:-}"
tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/gba-compat.XXXXXX")"
trap 'rm -rf "$tmp_dir"' EXIT

mkdir -p "$out_dir"
result_base="$(basename "$result_file")"
summary_base="$(basename "$summary_file")"
find "$out_dir" -maxdepth 1 -type f ! -name "$result_base" ! -name "$summary_base" -delete
: > "$result_file"
: > "$summary_file"

{
    echo "meu-GBA compatibility summary"
    echo "manifest=$manifest"
    echo "gba_root=$gba_root"
    [[ -n "$gba_bios" ]] && echo "gba_bios=$gba_bios"
    echo "result=$result_file"
    echo
} >> "$summary_file"

if [[ ! -x ./gba_compat_test ]]; then
    make gba_compat_test >/dev/null || exit 2
fi

total=0
pass=0
fail=0
timeout=0
visual=0
unknown=0

run_one() {
    local rom="$1"
    local expect="$2"
    local max_cycles="$3"
    local expected_sha1="$4"
    local extra_args="$5"

    [[ -f "$rom" ]] || return 0

    if [[ -n "$max_override" ]]; then
        max_cycles="$max_override"
    fi

    local base
    base="$(basename "$rom")"
    base="${base%.*}"
    local ppm="$tmp_dir/$base.ppm"

    local cmd=(./gba_compat_test --expect "$expect" --max-cycles "$max_cycles")
    if [[ -n "$gba_bios" ]]; then
        cmd+=(--bios "$gba_bios")
    fi
    if [[ -n "$extra_args" ]]; then
        # shellcheck disable=SC2206
        local extra=($extra_args)
        cmd+=("${extra[@]}")
    fi
    if [[ "$expect" == "visual" ]]; then
        cmd+=(--ppm "$ppm")
    fi
    cmd+=("$rom")

    local output
    local err_file="$tmp_dir/$base.stderr"
    output="$("${cmd[@]}" 2>"$err_file")"
    local status="${output%%$'\t'*}"
    local final_status="$status"
    local sha1=""

    if [[ "$expect" == "visual" && -f "$ppm" ]]; then
        sha1="$(sha1sum "$ppm" | awk '{print $1}')"
        if [[ -n "$expected_sha1" ]]; then
            if [[ "$sha1" == "$expected_sha1" ]]; then
                final_status="PASS"
            else
                final_status="FAIL"
            fi
        fi
    fi

    total=$((total + 1))
    case "$final_status" in
        PASS)    pass=$((pass + 1)) ;;
        FAIL)    fail=$((fail + 1)) ;;
        TIMEOUT) timeout=$((timeout + 1)) ;;
        VISUAL)  visual=$((visual + 1)) ;;
        *)       unknown=$((unknown + 1)) ;;
    esac

    if [[ "$expect" == "visual" ]]; then
        printf '%s\t%s\tsha1=%s\t%s\n' "$final_status" "$rom" "$sha1" "$output" | tee -a "$result_file"
    else
        printf '%s\n' "$output" | tee -a "$result_file"
    fi

    {
        echo "[$final_status] $rom"
        echo "  expect=$expect max_cycles=$max_cycles"
        [[ -n "$extra_args" ]] && echo "  extra_args=$extra_args"
        if [[ "$expect" == "visual" ]]; then
            echo "  visual_sha1=${sha1:-none}"
            [[ -n "$expected_sha1" ]] && echo "  expected_sha1=$expected_sha1"
        fi
        echo "  output=$output"
        local stderr_text
        stderr_text="$(cat "$err_file")"
        if [[ -n "$stderr_text" ]]; then
            echo "  stderr:"
            sed 's/^/    /' "$err_file"
        fi
        echo
    } >> "$summary_file"
}

if [[ ! -f "$manifest" ]]; then
    echo "GBA compat manifest not found: $manifest" >&2
    exit 2
fi

while IFS=$'\t' read -r path expect max_cycles sha1 rest; do
    [[ -z "${path:-}" || "${path:0:1}" == "#" ]] && continue
    path="${path//\{gba_root\}/$gba_root}"
    expect="${expect:-auto}"
    max_cycles="${max_cycles:-500000000}"
    sha1="${sha1:-}"

    if [[ "$path" == glob:* ]]; then
        pattern="${path#glob:}"
        for rom in $pattern; do
            run_one "$rom" "$expect" "$max_cycles" "$sha1" "$rest"
        done
    else
        run_one "$path" "$expect" "$max_cycles" "$sha1" "$rest"
    fi
done < "$manifest"

summary="gba-compat TOTAL=$total PASS=$pass FAIL=$fail TIMEOUT=$timeout VISUAL=$visual UNKNOWN=$unknown result=$result_file"
echo "$summary" | tee -a "$result_file"

{
    echo "Totals"
    echo "  total=$total"
    echo "  pass=$pass"
    echo "  fail=$fail"
    echo "  timeout=$timeout"
    echo "  visual=$visual"
    echo "  unknown=$unknown"
    echo
    echo "$summary"
} >> "$summary_file"

if (( fail > 0 || timeout > 0 || unknown > 0 )); then
    exit 1
fi
exit 0
