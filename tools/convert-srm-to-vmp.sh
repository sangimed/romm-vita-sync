#!/usr/bin/env sh

set -eu

usage() {
    echo "Usage: $0 <input.srm> [output.vmp] <template.vmp> [--rebuild]" >&2
}

default_output_path() {
    source_path=$1
    case "$source_path" in
        *.[Ss][Rr][Mm])
            printf '%s\n' "${source_path%.*}.vmp"
            ;;
        *)
            printf '%s\n' "${source_path}.vmp"
            ;;
    esac
}

find_converter_binary() {
    for candidate in "$@"; do
        if [ -f "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

    return 1
}

is_absolute_path() {
    path_value=$1
    case "$path_value" in
        /*)
            return 0
            ;;
        [A-Za-z]:/*|[A-Za-z]:\\*)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

INPUT_PATH=""
OUTPUT_PATH=""
TEMPLATE_VMP_PATH=""
REBUILD=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --rebuild)
            REBUILD=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            if [ -z "$INPUT_PATH" ]; then
                INPUT_PATH=$1
            elif [ -z "$OUTPUT_PATH" ]; then
                OUTPUT_PATH=$1
            elif [ -z "$TEMPLATE_VMP_PATH" ]; then
                TEMPLATE_VMP_PATH=$1
            else
                usage
                exit 1
            fi
            ;;
    esac
    shift
done

if [ -z "$INPUT_PATH" ]; then
    usage
    exit 1
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd -P)

if [ ! -f "$INPUT_PATH" ]; then
    if [ -f "$PWD/$INPUT_PATH" ]; then
        INPUT_PATH=$PWD/$INPUT_PATH
    else
        echo "Input file not found: $INPUT_PATH" >&2
        exit 1
    fi
elif ! is_absolute_path "$INPUT_PATH"; then
    INPUT_PATH=$PWD/$INPUT_PATH
fi

if [ -z "$OUTPUT_PATH" ]; then
    OUTPUT_PATH=$(default_output_path "$INPUT_PATH")
elif ! is_absolute_path "$OUTPUT_PATH"; then
    OUTPUT_PATH=$PWD/$OUTPUT_PATH
fi

if [ -z "$TEMPLATE_VMP_PATH" ] && [ -n "${ROMM_VMP_TEMPLATE_PATH:-}" ]; then
    TEMPLATE_VMP_PATH=$ROMM_VMP_TEMPLATE_PATH
fi

if [ -z "$TEMPLATE_VMP_PATH" ] && [ -f "$SCRIPT_DIR/SCEVMC0.VMP" ]; then
    TEMPLATE_VMP_PATH=$SCRIPT_DIR/SCEVMC0.VMP
fi

if [ -z "$TEMPLATE_VMP_PATH" ] && [ -f "$REPO_ROOT/samples/vmp-templates/SCEVMC0.VMP" ]; then
    TEMPLATE_VMP_PATH=$REPO_ROOT/samples/vmp-templates/SCEVMC0.VMP
fi

if [ -z "$TEMPLATE_VMP_PATH" ]; then
    echo "Template VMP required. Pass an existing .VMP file as the third argument or set ROMM_VMP_TEMPLATE_PATH. A reference template is provided in samples/vmp-templates/ when checked out." >&2
    exit 1
fi

if ! is_absolute_path "$TEMPLATE_VMP_PATH"; then
    TEMPLATE_VMP_PATH=$PWD/$TEMPLATE_VMP_PATH
fi

if [ ! -f "$TEMPLATE_VMP_PATH" ]; then
    echo "Template VMP not found: $TEMPLATE_VMP_PATH" >&2
    exit 1
fi

BUILD_DIR=$REPO_ROOT/build-tools
RELEASE_BIN=$BUILD_DIR/Release/srm2vmp
ROOT_BIN=$BUILD_DIR/srm2vmp
RELWITHDEBINFO_BIN=$BUILD_DIR/RelWithDebInfo/srm2vmp
DEBUG_BIN=$BUILD_DIR/Debug/srm2vmp

CONVERTER_EXE=""
if [ "$REBUILD" -eq 0 ]; then
    CONVERTER_EXE=$(find_converter_binary \
        "$RELEASE_BIN" \
        "$ROOT_BIN" \
        "$RELWITHDEBINFO_BIN" \
        "$DEBUG_BIN" || true)
fi

if [ -z "$CONVERTER_EXE" ]; then
    if ! command -v cmake >/dev/null 2>&1; then
        echo "srm2vmp not found and cmake is unavailable. Build the tools target first." >&2
        exit 1
    fi

    cmake -S "$REPO_ROOT/tools" -B "$BUILD_DIR"
    cmake --build "$BUILD_DIR" --config Release

    CONVERTER_EXE=$(find_converter_binary \
        "$RELEASE_BIN" \
        "$ROOT_BIN" \
        "$RELWITHDEBINFO_BIN" \
        "$DEBUG_BIN" || true)

    if [ -z "$CONVERTER_EXE" ]; then
        echo "Build finished but srm2vmp was not found." >&2
        exit 1
    fi
fi

"$CONVERTER_EXE" "$INPUT_PATH" "$TEMPLATE_VMP_PATH" "$OUTPUT_PATH"
echo "VMP generated: $OUTPUT_PATH"
