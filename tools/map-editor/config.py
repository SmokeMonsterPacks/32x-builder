"""Env-var config for the Backrooms map editor (borrowed pattern from the
intellivision-overlay-editor)."""
import os

HOST = os.environ.get("MAP_HOST", "127.0.0.1")
PORT = int(os.environ.get("MAP_PORT", "5050"))
DEBUG = os.environ.get("MAP_DEBUG", "0") == "1"

# Repo root holds maps/, registry.json and tools/mapfmt.py. Defaults to three
# levels up from this file (tools/map-editor/config.py -> repo root); override
# with MAP_REPO_ROOT (the Docker compose sets it to the mounted /repo).
REPO_ROOT = os.environ.get(
    "MAP_REPO_ROOT",
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
MAPS_DIR = os.path.join(REPO_ROOT, "maps")
REGISTRY = os.path.join(REPO_ROOT, "registry.json")
TOOLS_DIR = os.path.join(REPO_ROOT, "tools")

# Hosted instances (fly.io) run READ-ONLY: the container filesystem is ephemeral
# (no volume), so sessions are isolated in the browser and the only way out is
# Export — a download to the user's local disk that they commit / PR from their
# own clone. Local dev (your own checkout) leaves this off and can write straight
# to maps/community/. Dockerfile.fly sets MAP_READONLY=1.
READONLY = os.environ.get("MAP_READONLY", "0") == "1"

# Community-PR pipeline: where "Submit to game" sends maps. The pre-filled
# github.com/new URL flow (and later the OAuth PR flow) target this repo/branch.
GITHUB_REPO = os.environ.get("MAP_GITHUB_REPO", "mholzinger/32x-builder")
GITHUB_BRANCH = os.environ.get("MAP_GITHUB_BRANCH", "main")

# Phase 3 — in-editor GitHub sign-in. Register a GitHub OAuth App (callback URL
# https://<host>/auth/callback) and set these as fly secrets; when unset the
# editor falls back to the zero-auth pre-filled-URL submit flow. The session
# secret signs the Flask cookie that carries the user's OAuth token between
# requests (it is the USER'S OWN token — signing prevents tampering; the only
# party who can read it is the user it belongs to).
GITHUB_CLIENT_ID = os.environ.get("GITHUB_CLIENT_ID", "")
GITHUB_CLIENT_SECRET = os.environ.get("GITHUB_CLIENT_SECRET", "")
SESSION_SECRET = os.environ.get("MAP_SESSION_SECRET", "")
OAUTH_ENABLED = bool(GITHUB_CLIENT_ID and GITHUB_CLIENT_SECRET and SESSION_SECRET)
