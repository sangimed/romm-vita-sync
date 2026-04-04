#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
DOCKER_IMAGE="${VITASDK_DOCKER_IMAGE:-gnuton/vitasdk-docker}"

print_help() {
  cat <<'EOF'
Usage:
  ./tools/docker-build-and-upload-vpk.sh [--docker-image IMAGE] [build-and-upload-vpk args...]

Examples:
  ./tools/docker-build-and-upload-vpk.sh
  ./tools/docker-build-and-upload-vpk.sh --ftp-host 192.168.1.20 --ftp-port 1337
  ./tools/docker-build-and-upload-vpk.sh --docker-image gnuton/vitasdk-docker:latest --ftp-host 192.168.1.20
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --docker-image)
      if [ "$#" -lt 2 ]; then
        echo "Missing value for --docker-image" >&2
        exit 1
      fi
      DOCKER_IMAGE="$2"
      shift 2
      ;;
    --help|-h)
      print_help
      exit 0
      ;;
    *)
      break
      ;;
  esac
done

if ! command -v docker >/dev/null 2>&1; then
  echo "Docker CLI is required but was not found in PATH." >&2
  exit 1
fi

if ! docker info >/dev/null 2>&1; then
  echo "Docker daemon is not reachable. Start Docker Desktop/Engine and retry." >&2
  exit 1
fi

echo "==> Docker build+upload wrapper"
echo "==> Image: $DOCKER_IMAGE"
echo "==> Container lifecycle: --rm (auto removed after script completion)"

docker run --rm \
  -v "$REPO_ROOT:/build/git" \
  "$DOCKER_IMAGE" \
  sh -lc 'set -eu; cd /build; ./git/tools/build-and-upload-vpk.sh "$@"' -- "$@"

echo "==> Done"
