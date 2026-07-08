#!/bin/bash
# Build the ACTUAL fly.io image (Dockerfile.fly, no repo volume mount) and
# smoke-test it locally. This is the gate before ANY `fly deploy`: the compose
# dev flow (test-docker-nocache.sh) mounts the whole repo, so it cannot catch
# a file missing from the image's explicit COPY list — this can.
#     ./test-fly-image.sh
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
PORT=5055
NAME=backrooms-flytest

cd "$ROOT"
echo "📦 Building Dockerfile.fly (the real deploy image)..."
docker build -q -f tools/map-editor/Dockerfile.fly -t backrooms-editor-flytest .

docker rm -f "$NAME" >/dev/null 2>&1 || true
docker run -d --name "$NAME" -p $PORT:5050 backrooms-editor-flytest >/dev/null
sleep 3

fail=0
if [ "$(docker logs "$NAME" 2>&1 | grep -c Traceback)" != "0" ]; then
    echo "❌ container boot: TRACEBACK in logs"; docker logs "$NAME" 2>&1 | tail -5; fail=1
else
    echo "✅ container boot: clean"
fi
check () {  # url, label, grep-pattern
    if curl -s --max-time 5 "http://localhost:$PORT$1" | grep -q "$3"; then
        echo "✅ $2"
    else
        echo "❌ $2"; fail=1
    fi
}
check /        "main page (Submit button)" "btn-submit"
check /config  "config"                    '"readonly"'
check /maps    "maps list"                 '"maps"'
check /auth/user "auth state"              '"oauth"'
if curl -s --max-time 10 -X POST -H 'Content-Type: application/json' \
     -d '{"name":"FLYSMOKE","w":8,"h":8,"grid":["########","#......#","#......#","#......#","#......#","#......#","#......#","########"],"spawn":{"x":4.5,"y":4.5,"facing":"N"},"crawls":[],"partitions":[],"decals":[],"lights":[],"options":{"place_outlets":0,"place_exit_door":0,"lobby_ceiling":0}}' \
     "http://localhost:$PORT/submit_url" | grep -q '"ok": *true'; then
    echo "✅ /submit_url (lint + URL build)"
else
    echo "❌ /submit_url"; fail=1
fi

docker rm -f "$NAME" >/dev/null
if [ "$fail" = "0" ]; then
    echo "🎉 fly image is green — safe to ./deploy-to-flyio.sh"
else
    echo "⚠️  DO NOT DEPLOY — fix the failures above first."
fi
exit $fail
