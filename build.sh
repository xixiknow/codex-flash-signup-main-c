#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

TARGET="${1:-build}"
JOBS="${JOBS:-}"

usage() {
  cat <<'EOF'
Usage: ./build.sh [target]

Targets:
  build    Build the web assets and C executable (default)
  rebuild  Clean first, then build
  web      Build only the frontend assets
  pack     Build frontend assets and pack them into C source
  run      Build and run with a generated development admin password
  clean    Remove build outputs
  help     Show this help

Environment overrides are passed through to Makefile:
  CC CFLAGS LDFLAGS CURL_CFLAGS CURL_LIBS SQLITE_CFLAGS SQLITE_LIBS JOBS
EOF
}

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

detect_jobs() {
  if [ -n "$JOBS" ]; then
    printf '%s\n' "$JOBS"
    return
  fi

  if command -v nproc >/dev/null 2>&1; then
    nproc
    return
  fi

  getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2\n'
}

preflight() {
  case "$TARGET" in
    clean|help|-h|--help)
      return
      ;;
  esac

  need_cmd make
  need_cmd gcc
  need_cmd npm
  need_cmd python3
}

make_build() {
  local jobs
  jobs="$(detect_jobs)"
  make -j "$jobs" app
}

preflight

case "$TARGET" in
  build|all|app)
    make_build
    ;;
  rebuild)
    make clean
    make_build
    ;;
  web)
    make web
    ;;
  pack)
    make pack
    ;;
  run)
    make run
    ;;
  clean)
    make clean
    ;;
  help|-h|--help)
    usage
    ;;
  *)
    usage >&2
    die "unknown target: $TARGET"
    ;;
esac
