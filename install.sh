#!/usr/bin/env sh
# Lume — one-command installer.
#
#   curl -sSfL https://raw.githubusercontent.com/erishen/lume/main/install.sh | sh
#
# Downloads the prebuilt release tarball for your platform from GitHub
# Releases and installs it: the binary lands in ~/.local/bin/lume, and the
# web UI (www), examples and docs in ~/.local/share/lume so the bundled
# /chat /dsl demo pages resolve their docroot. Needs curl or wget.
#
# Overrides (set before piping, e.g. `... | LUME_VERSION=v0.1.0 sh`):
#   LUME_VERSION  version tag to install (default: latest)
#   LUME_PREFIX   install root (default: $HOME/.local; binary goes to $PREFIX/bin)
#   LUME_SHA256   expected sha256 of the binary, verified when set
#   LUME_REPO     GitHub repo to fetch from (default: erishen/lume)
set -eu

REPO="${LUME_REPO:-erishen/lume}"
VERSION="${LUME_VERSION:-latest}"
PREFIX="${LUME_PREFIX:-$HOME/.local}"
BINDIR="$PREFIX/bin"

# --- platform detection -----------------------------------------------------
case "$(uname -s)" in
  Linux)  os=linux ;;
  Darwin) os=darwin ;;
  *)
    echo "lume-install: unsupported OS: $(uname -s)" >&2
    exit 1
    ;;
esac

case "$(uname -m)" in
  x86_64|amd64)  arch=x64 ;;
  arm64|aarch64) arch=arm64 ;;
  *)
    echo "lume-install: unsupported architecture: $(uname -m)" >&2
    exit 1
    ;;
esac

asset="lume-$os-$arch.tar.gz"
if [ "$VERSION" = "latest" ]; then
  url="https://github.com/$REPO/releases/latest/download/$asset"
else
  url="https://github.com/$REPO/releases/download/$VERSION/$asset"
fi

# --- download ---------------------------------------------------------------
mkdir -p "$BINDIR"
tmp="$BINDIR/.lume.tmp.$$"
trap 'rm -f "$tmp"; rm -rf "$tmp.d" "$BINDIR/.lume.stage.$$"' EXIT INT TERM

echo "==> lume-install: downloading $asset"
if command -v curl >/dev/null 2>&1; then
  curl -sSfL "$url" -o "$tmp"
elif command -v wget >/dev/null 2>&1; then
  wget -qO "$tmp" "$url"
else
  echo "lume-install: need curl or wget" >&2
  exit 1
fi

# --- optional checksum ------------------------------------------------------
if [ -n "${LUME_SHA256:-}" ]; then
  if command -v sha256sum >/dev/null 2>&1; then
    actual=$(sha256sum "$tmp" | awk '{print $1}')
  elif command -v shasum >/dev/null 2>&1; then
    actual=$(shasum -a 256 "$tmp" | awk '{print $1}')
  else
    echo "lume-install: LUME_SHA256 set but no sha256sum/shasum found" >&2
    exit 1
  fi
  if [ "$actual" != "$LUME_SHA256" ]; then
    echo "lume-install: checksum mismatch" >&2
    echo "  expected: $LUME_SHA256" >&2
    echo "  actual:   $actual" >&2
    exit 1
  fi
  echo "==> lume-install: checksum ok"
fi

# --- install ----------------------------------------------------------------
if ! command -v tar >/dev/null 2>&1; then
  echo "lume-install: need tar to unpack the release" >&2
  exit 1
fi
stage="$BINDIR/.lume.stage.$$"
mkdir -p "$stage"
tar -xzf "$tmp" -C "$stage" || { echo "lume-install: corrupt download" >&2; exit 1; }
dir=$(find "$stage" -maxdepth 1 -mindepth 1 -type d | head -1)
[ -n "$dir" ] && [ -x "$dir/bin/lume" ] || {
  echo "lume-install: tarball layout unexpected" >&2
  exit 1
}

chmod +x "$dir/bin/lume"
mv "$dir/bin/lume" "$BINDIR/lume"
if [ -d "$dir/www" ] || [ -d "$dir/examples" ] || [ -d "$dir/docs" ]; then
  sharedir="$PREFIX/share/lume"
  mkdir -p "$sharedir"
  cp -R "$dir/www" "$dir/examples" "$dir/docs" "$dir/README.md" "$dir/CHANGELOG.md" \
    "$sharedir/" 2>/dev/null || true
  echo "==> lume-install: web UI + examples + docs in $sharedir"
  echo "    demo: cd $sharedir && $BINDIR/lume examples/sqlite-write.lume (then http://127.0.0.1:8084/chat)"
fi
rm -rf "$stage"
echo "==> lume-install: installed $BINDIR/lume"
echo "    run: $BINDIR/lume --help"

case ":$PATH:" in
  *":$BINDIR:"*) ;;
  *) echo "    add to PATH: export PATH=\"$BINDIR:\$PATH\"" ;;
esac
