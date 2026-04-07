#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

DEFAULT_FTP_HOST="192.168.1.20"
DEFAULT_FTP_PORT="1337"
DEFAULT_FTP_REMOTE_DIR="ux0:/homebrews"
DEFAULT_CONFIGURATION="Release"
FTP_CONNECT_TIMEOUT_SECONDS="10"
FTP_UPLOAD_STALL_TIMEOUT_SECONDS="10"
FTP_UPLOAD_STALL_SPEED_BYTES="1"

FTP_HOST=""
FTP_PORT=""
FTP_REMOTE_DIR=""
CONFIGURATION="$DEFAULT_CONFIGURATION"
CONFIG_FILE="$SCRIPT_DIR/build-and-upload-vpk.local.env"
BUILD_DIR="$REPO_ROOT/build"
FALLBACK_BUILD_DIR="$REPO_ROOT/.build-wsl2"

while [ "$#" -gt 0 ]; do
  case "$1" in
    --ftp-host)
      FTP_HOST="$2"
      shift 2
      ;;
    --ftp-port)
      FTP_PORT="$2"
      shift 2
      ;;
    --ftp-remote-dir)
      FTP_REMOTE_DIR="$2"
      shift 2
      ;;
    --configuration)
      CONFIGURATION="$2"
      shift 2
      ;;
    --config-file)
      CONFIG_FILE="$2"
      shift 2
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

if [ -f "$CONFIG_FILE" ]; then
  echo "==> Config loaded: $CONFIG_FILE"
  while IFS= read -r raw_line || [ -n "$raw_line" ]; do
    line=$(printf '%s' "$raw_line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
    [ -z "$line" ] && continue
    case "$line" in
      \#*) continue ;;
    esac
    case "$line" in
      *=*) ;;
      *) continue ;;
    esac

    key=$(printf '%s' "${line%%=*}" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
    value=$(printf '%s' "${line#*=}" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')

    case "$value" in
      \"*\")
        value=${value#\"}
        value=${value%\"}
        ;;
      \'*\')
        value=${value#\'}
        value=${value%\'}
        ;;
    esac

    case "$key" in
      FTP_HOST)
        [ -n "$value" ] && FTP_HOST="$value"
        ;;
      FTP_PORT)
        [ -n "$value" ] && FTP_PORT="$value"
        ;;
      FTP_REMOTE_DIR)
        [ -n "$value" ] && FTP_REMOTE_DIR="$value"
        ;;
      *)
        ;;
    esac
  done < "$CONFIG_FILE"
else
  echo "==> Config not found: $CONFIG_FILE (defaults will be used when needed)"
fi

[ -z "$FTP_HOST" ] && FTP_HOST="$DEFAULT_FTP_HOST"
[ -z "$FTP_PORT" ] && FTP_PORT="$DEFAULT_FTP_PORT"
[ -z "$FTP_REMOTE_DIR" ] && FTP_REMOTE_DIR="$DEFAULT_FTP_REMOTE_DIR"

FTP_REMOTE_DIR=$(printf '%s' "$FTP_REMOTE_DIR" | sed -e 's:/*$::')

cmake_cache="$BUILD_DIR/CMakeCache.txt"
if [ -f "$cmake_cache" ]; then
  cached_build_dir=$(sed -n 's/^# For build in directory: //p' "$cmake_cache" | head -n 1)
  cached_source_dir=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cmake_cache" | head -n 1)

  if [ "$cached_build_dir" != "$BUILD_DIR" ] || [ "$cached_source_dir" != "$REPO_ROOT" ]; then
    echo "==> Stale CMake cache detected"
    echo "    - Cached build dir: ${cached_build_dir:-<unknown>}"
    echo "    - Cached source dir: ${cached_source_dir:-<unknown>}"
    echo "    - Expected build dir: $BUILD_DIR"
    echo "    - Expected source dir: $REPO_ROOT"
    echo "==> Removing $BUILD_DIR to regenerate CMake files for the current environment"

    if ! rm -rf "$BUILD_DIR" 2>/dev/null; then
      echo "==> Could not remove $BUILD_DIR (likely due to ownership mismatch)." >&2
      echo "==> Falling back to a user-owned build directory: $FALLBACK_BUILD_DIR"
      BUILD_DIR="$FALLBACK_BUILD_DIR"
    fi
  fi
fi

echo "==> Configure CMake"
cmake -S "$REPO_ROOT" -B "$BUILD_DIR"

echo "==> Build VPK ($CONFIGURATION)"
cmake --build "$BUILD_DIR" --config "$CONFIGURATION"

vpk_file=$(ls -1t "$BUILD_DIR"/*.vpk 2>/dev/null | head -n 1 || true)
if [ -z "$vpk_file" ]; then
  echo "No .vpk file found in $BUILD_DIR" >&2
  exit 1
fi

vpk_name=$(basename "$vpk_file")
remote_url="ftp://${FTP_HOST}:${FTP_PORT}/${FTP_REMOTE_DIR}/${vpk_name}"

echo "==> Upload: $vpk_file"
echo "==> Destination: $remote_url"
echo "==> FTP timeout policy"
echo "    - Connection timeout: ${FTP_CONNECT_TIMEOUT_SECONDS}s"
echo "    - Upload stall timeout: ${FTP_UPLOAD_STALL_TIMEOUT_SECONDS}s (speed < ${FTP_UPLOAD_STALL_SPEED_BYTES} B/s)"

curl \
  --ftp-method nocwd \
  --connect-timeout "$FTP_CONNECT_TIMEOUT_SECONDS" \
  --speed-time "$FTP_UPLOAD_STALL_TIMEOUT_SECONDS" \
  --speed-limit "$FTP_UPLOAD_STALL_SPEED_BYTES" \
  -T "$vpk_file" \
  "$remote_url" || {
    curl_exit=$?
    if [ "$curl_exit" -eq 28 ]; then
      echo "FTP timeout reached (connection or upload stalled for 10 seconds)." >&2
    fi
    exit "$curl_exit"
  }

echo "==> Completed"
