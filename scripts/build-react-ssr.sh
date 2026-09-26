#!/bin/sh
# Build the React SSR page sources (frontend/react-ssr/) into the resident
# node backend (bin/react-ssr-server) and the hydration client
# (www/js/react-ssr.js). The lume-side counterpart of agent-httpd's
# scripts/build-ssr.sh, minus the CGI entry: edit pages/About.tsx etc. under
# frontend/react-ssr/ and re-run `make react-ssr` (or this script) to see the
# change on :8085.
set -e
REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SRC="$REPO/frontend/react-ssr"
BIN_DIR="$REPO/bin"
SERVER_OUT="$BIN_DIR/react-ssr-server"
CLIENT_OUT="$REPO/www/js/react-ssr.js"
ESBUILD="$SRC/node_modules/.bin/esbuild"
TSC="$SRC/node_modules/.bin/tsc"
TAILWIND="$SRC/node_modules/.bin/tailwindcss"
TAILWIND_IN="$SRC/styles/main.css"
TAILWIND_OUT="$SRC/tailwind.css"

if [ ! -x "$ESBUILD" ] || [ ! -x "$TSC" ] || [ ! -x "$TAILWIND" ]; then
    echo "error: esbuild/tsc/tailwindcss missing. Run 'cd frontend/react-ssr && pnpm install' first" >&2
    exit 1
fi

mkdir -p "$BIN_DIR" "$(dirname "$CLIENT_OUT")"

# Type-check before bundling (esbuild does NOT type-check).
( cd "$SRC" && "$TSC" -p tsconfig.json )

# Tailwind v4: scans App.tsx for class names, emits the stylesheet that
# render.tsx inlines into <head>.
"$TAILWIND" -i "$TAILWIND_IN" -o "$TAILWIND_OUT" --minify >/dev/null

# Client bundle: browser platform, IIFE (loaded via <script>).
"$ESBUILD" "$SRC/client.tsx" \
    --bundle \
    --platform=browser \
    --format=iife \
    --jsx=automatic \
    --minify \
    --define:process.env.NODE_ENV='"production"' \
    --outfile="$CLIENT_OUT"

# Cache-busting hash embedded into the server bundle (SSR documents link
# /js/react-ssr.js?v=<hash>).
BUNDLE_HASH=$(
    { command -v sha256sum >/dev/null 2>&1 && sha256sum "$CLIENT_OUT" || shasum -a 256 "$CLIENT_OUT"; } | cut -c1-8
)

# Precompressed twin served with Accept-Encoding: gzip.
gzip -n -9 -c "$CLIENT_OUT" > "$CLIENT_OUT.gz"

# Resident server bundle: node platform, CJS. loader:.css=text inlines the
# compiled Tailwind; the banner makes it executable directly.
"$ESBUILD" "$SRC/server/main.tsx" \
    --bundle \
    --platform=node \
    --format=cjs \
    --jsx=automatic \
    --minify \
    --loader:.css=text \
    --define:process.env.NODE_ENV='"production"' \
    --define:BUNDLE_QUERY="\"$BUNDLE_HASH\"" \
    --banner:js='#!/usr/bin/env node' \
    --outfile="$SERVER_OUT"
chmod +x "$SERVER_OUT"

echo "Resident: $SERVER_OUT ($(wc -c < "$SERVER_OUT") bytes)"
echo "Client:   $CLIENT_OUT ($(wc -c < "$CLIENT_OUT") bytes)  hash=$BUNDLE_HASH"
echo "Tailwind: $TAILWIND_OUT ($(wc -c < "$TAILWIND_OUT") bytes)"
