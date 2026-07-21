#!/usr/bin/env python3
"""Backrooms 32X map editor — Flask backend.

Serves the canvas editor and reads/writes the repo's maps/*.map files through
the SHARED format module (tools/mapfmt.py), so what the editor saves is exactly
what the ROM codegen (tools/gen_maps.py) reads. The element palette is driven by
the same registry.json the build uses, so new elements show up automatically.

Run:  python3 app.py          (needs Flask; reads ../../maps + ../../registry.json)
  or: docker compose up       (mounts the repo at /repo)
"""
import json, os, re, sys

import config
sys.path.insert(0, config.TOOLS_DIR)
import mapfmt          # noqa: E402  (shared .map syntax — single source of truth)
import export_assets   # noqa: E402  (palette + textures from the ROM source)
import lint_maps       # noqa: E402  (the same gate CI runs — lint before submit)

import github_pr        # noqa: E402  (OAuth + fork/branch/PR — pipeline Phase 3)

from flask import Flask, jsonify, request, render_template, redirect, session

app = Flask(__name__)
# Signs the session cookie that carries a signed-in user's OWN GitHub token
# between requests (Phase 3). Unset -> OAuth routes disable themselves and the
# editor uses the zero-auth pre-filled-URL submit flow.
app.secret_key = config.SESSION_SECRET or None


def _safe(name):
    """Filename-safe map slug (no path traversal)."""
    return re.sub(r"[^a-zA-Z0-9_-]", "", name or "")[:32] or "untitled"


def _roles():
    try:
        with open(config.REGISTRY) as fh:
            return json.load(fh).get("roles", {})
    except Exception:
        return {}


def _iter_map_files():
    """Every maps/**/*.map across the role folders -> (folder, name, fullpath)."""
    out = []
    for root, _dirs, files in os.walk(config.MAPS_DIR):
        folder = "" if root == config.MAPS_DIR else os.path.basename(root)
        for f in files:
            if f.endswith(".map"):
                out.append((folder, f[:-4], os.path.join(root, f)))
    return out


def _find_map(name):
    """Resolve a map by slug across the role folders (for load)."""
    slug = _safe(name).lower()
    for _folder, n, path in _iter_map_files():
        if n.lower() == slug:
            return path
    return None


def _community_path(name):
    return os.path.join(config.MAPS_DIR, "community", _safe(name) + ".map")


def _list_maps():
    """[{name, title, role, folder, protected}] — the editor shows a lock on
    protected (core) maps and clones them on edit.

    `name` is the FILE stem (the /maps/<name> key); `title` is the map's
    `name:` header. They differ ("segapowerbase" vs "SEGA POWER BASE"), and
    story chains resolve by TITLE (gen_maps matches `next:` against the header
    name, uppercased) — so the editor's next-map picker needs the title, not
    the filename. Same parse as the role lookup, so it costs nothing extra."""
    roles = _roles()
    maps = []
    for folder, name, path in sorted(_iter_map_files(), key=lambda t: (t[0], t[1])):
        role, title = "community", ""
        try:
            m = mapfmt.parse(open(path).read())
            role = m.get("role", "community")
            title = m.get("name") or ""
        except Exception:
            pass
        maps.append({"name": name, "title": title, "role": role, "folder": folder,
                     "protected": bool(roles.get(role, {}).get("protected"))})
    return maps


@app.after_request
def _no_cache(resp):
    resp.headers["Cache-Control"] = "no-store"
    return resp


@app.route("/health")
def health():
    return jsonify({"status": "ok"})


@app.route("/")
def index():
    return render_template("index.html")


@app.route("/registry")
def registry():
    with open(config.REGISTRY) as fh:
        return jsonify(json.load(fh))


@app.route("/assets")
def assets():
    """The ROM's actual palette (+ base indices) so the preview renders in the
    game's real colors. Parsed live from raycast.c, so it tracks palette tweaks."""
    try:
        return jsonify(export_assets.build_assets(config.REPO_ROOT))
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@app.route("/config")
def client_config():
    """The client adapts its UI to this: on a hosted (read-only) instance the
    Save-to-disk button hides and Export (download) is the way out; when OAuth
    is configured the Submit button upgrades to the in-editor PR flow."""
    return jsonify({"readonly": config.READONLY,
                    "github_oauth": config.OAUTH_ENABLED,
                    "github_repo": config.GITHUB_REPO})


# ---------------- Phase 3: GitHub sign-in + in-editor PR ----------------

@app.route("/auth/login")
def auth_login():
    if not config.OAUTH_ENABLED:
        return jsonify({"error": "GitHub OAuth is not configured on this instance"}), 501
    state = os.urandom(16).hex()
    session["oauth_state"] = state
    redirect_uri = request.url_root.rstrip("/") + "/auth/callback"
    # fly terminates TLS ahead of us; the public callback must be https.
    if request.headers.get("X-Forwarded-Proto") == "https":
        redirect_uri = redirect_uri.replace("http://", "https://", 1)
    return redirect(github_pr.authorize_url(config.GITHUB_CLIENT_ID, redirect_uri, state))


@app.route("/auth/callback")
def auth_callback():
    if not config.OAUTH_ENABLED:
        return jsonify({"error": "OAuth not configured"}), 501
    if request.args.get("state") != session.pop("oauth_state", None):
        return jsonify({"error": "state mismatch — retry sign-in"}), 400
    code = request.args.get("code", "")
    try:
        token = github_pr.exchange_code(config.GITHUB_CLIENT_ID,
                                        config.GITHUB_CLIENT_SECRET, code)
        login = github_pr.whoami(token)
    except github_pr.GitHubError as e:
        return jsonify({"error": str(e)}), 502
    session["gh_token"] = token
    session["gh_login"] = login
    return redirect("/")


@app.route("/auth/user")
def auth_user():
    return jsonify({"login": session.get("gh_login") or None,
                    "oauth": config.OAUTH_ENABLED})


@app.route("/auth/logout", methods=["POST"])
def auth_logout():
    session.clear()
    return jsonify({"ok": True})


@app.route("/submit_pr", methods=["POST"])
def submit_pr():
    """Signed-in submit: lint, then fork/branch/commit/PR AS THE USER. The map
    arrives as a normal GitHub PR under their identity; CI takes it from there."""
    token, login = session.get("gh_token"), session.get("gh_login")
    if not (token and login):
        return jsonify({"error": "not signed in"}), 401
    model = request.get_json(force=True, silent=True)
    model, text, errs = _lint_submission(model)
    if errs:
        return jsonify({"ok": False, "errors": errs}), 400
    slug = _safe(model["name"]).lower()
    try:
        pr = github_pr.open_map_pr(
            token, login, config.GITHUB_REPO, config.GITHUB_BRANCH,
            "maps/community/%s.map" % slug, text,
            title="Community map: %s" % model["name"],
            body=("New community map **%s** by @%s, submitted from the "
                  "[hosted map editor](https://backrooms-32x-project.fly.dev/).\n\n"
                  "CI lints it and builds the ROM; once merged it ships in the "
                  "next `build-N` release automatically." % (model["name"], login)))
    except github_pr.GitHubError as e:
        return jsonify({"ok": False, "errors": [str(e)]}), 502
    return jsonify({"ok": True, "pr_url": pr["url"], "pr_number": pr["number"],
                    "existing": pr["existing"]})


@app.route("/maps")
def maps_list():
    return jsonify({"maps": _list_maps()})


@app.route("/maps/<name>")
def map_get(name):
    path = _find_map(name)
    if not path:
        return jsonify({"error": "not found"}), 404
    try:
        model = mapfmt.parse(open(path).read())
    except mapfmt.MapFormatError as e:
        return jsonify({"error": str(e)}), 400
    return jsonify({"model": model})


def _serialize_or_400(model):
    """Serialize via the shared format module with a round-trip guard.
    Returns (text, None) or (None, error_message)."""
    if not isinstance(model, dict):
        return None, "expected a JSON model"
    model["name"] = (model.get("name") or "untitled").upper()[:16]
    try:
        text = mapfmt.serialize(model)
        mapfmt.parse(text)            # what we emit must parse back cleanly
        return text, None
    except (mapfmt.MapFormatError, KeyError, TypeError) as e:
        return None, "serialize failed: %s" % e


@app.route("/export", methods=["POST"])
def map_export():
    """Stateless: serialize the posted model and hand it back as a .map file
    download. Never touches the filesystem — this is the save path on the hosted
    editor (session stays in the browser; the file lands on the user's disk)."""
    model = request.get_json(force=True, silent=True)
    text, err = _serialize_or_400(model)
    if err:
        return jsonify({"error": err}), 400
    name = _safe((model or {}).get("name")) or "untitled"
    resp = app.response_class(text, mimetype="text/plain")
    resp.headers["Content-Disposition"] = 'attachment; filename="%s.map"' % name.lower()
    resp.headers["Cache-Control"] = "no-store"
    return resp


@app.route("/parse", methods=["POST"])
def map_parse():
    """Stateless: parse raw .map text (a file the user picked off their disk)
    into a model via the shared parser. No filesystem write."""
    text = request.get_data(as_text=True)
    try:
        return jsonify({"model": mapfmt.parse(text)})
    except mapfmt.MapFormatError as e:
        return jsonify({"error": str(e)}), 400


@app.route("/maps/<name>", methods=["POST"])
def map_save(name):
    """Save-to-disk — LOCAL DEV ONLY (the hosted instance is read-only). Writes
    only into maps/community/; a protected (core) map is cloned, never
    overwritten, so canon is immutable here exactly as it is in CI/CODEOWNERS."""
    if config.READONLY:
        return jsonify({"error": "This hosted editor is read-only. Use Export to "
                                 "download the .map to your computer, then open a PR."}), 403
    model = request.get_json(force=True, silent=True)
    if not isinstance(model, dict):
        return jsonify({"error": "expected a JSON model"}), 400
    model["name"] = (model.get("name") or _safe(name)).upper()[:16]

    roles = _roles()
    role = (model.get("role") or "community").lower()
    cloned = False
    if roles.get(role, {}).get("protected"):     # clone-on-edit: fork to community
        model["role"] = "community"
        model["author"] = model.get("author") or os.environ.get("MAP_AUTHOR", "")
        cloned = True

    # Reject a name that collides with a DIFFERENT canonical map (clones must be
    # renamed) — keeps map names unique, which the lint also enforces.
    existing = _find_map(model["name"])
    if existing and os.path.basename(os.path.dirname(existing)) != "community":
        return jsonify({"error": "name %r is a protected map — choose a new name "
                                 "for your copy." % model["name"]}), 409

    text, err = _serialize_or_400(model)
    if err:
        return jsonify({"error": err}), 400
    out = _community_path(model["name"])
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w") as fh:
        fh.write(text)
    return jsonify({"ok": True, "name": _safe(model["name"]), "cloned": cloned,
                    "folder": "community", "text": text})


def _lint_submission(model):
    """Run the CI lint gate on a submission model (forced role=community).
    Returns (model, text, errors). Duplicate names against the repo's existing
    maps are rejected here so junk PRs never reach GitHub."""
    if not isinstance(model, dict):
        return None, None, ["expected a JSON model"]
    model = dict(model)
    model["name"] = (model.get("name") or "untitled").upper()[:16]
    model["role"] = "community"          # submissions are always community maps
    text, err = _serialize_or_400(model)
    if err:
        return model, None, [err]

    with open(config.REGISTRY) as fh:
        reg = json.load(fh)
    # Seed the duplicate-name check with the maps already in the repo — EXCEPT
    # a community map with this same name, which is the one being UPDATED (the
    # submission overwrites its file, it doesn't add a second copy). Without
    # this, every resubmission of an existing map failed as a duplicate of
    # itself. A same-named CORE map still collides: you can't shadow canon.
    this_name = (model.get("name") or "").upper()
    seen, errs = {}, []
    for folder, name, path in _iter_map_files():
        try:
            nm = (mapfmt.parse(open(path).read()).get("name") or name).upper()
        except Exception:
            nm = name.upper()
        if nm == this_name and (folder or "") == "community":
            continue                      # updating this map, not duplicating it
        seen[nm] = "maps/%s/%s.map" % (folder or ".", name)
    lint_maps.lint_model(mapfmt.parse(text), model["name"], "community",
                         reg, seen, errs)
    nxt = (model.get("next") or "").strip().upper()
    if nxt and nxt not in seen:
        errs.append("%s: next: %r is not a map in the game yet — submit the "
                    "later chapters of a story first (chains are built "
                    "tail-first)" % (model["name"], nxt))
    return model, text, errs


@app.route("/lint", methods=["POST"])
def map_lint():
    """The same gate CI runs, before the contributor leaves the editor."""
    model = request.get_json(force=True, silent=True)
    _model, _text, errs = _lint_submission(model)
    return jsonify({"ok": not errs, "errors": errs})


@app.route("/submit_url", methods=["POST"])
def submit_url():
    """Phase 1 of the community-PR pipeline: lint the model, then hand back a
    pre-filled github.com new-file URL. GitHub natively walks a signed-in
    contributor through fork -> commit -> pull request, so the PR is authored
    by THEIR identity with no tokens or secrets on this server. The client
    falls back to clipboard + a bare new-file URL if the encoded URL is too
    long for the browser/GitHub (~8KB)."""
    from urllib.parse import quote
    model = request.get_json(force=True, silent=True)
    model, text, errs = _lint_submission(model)
    if errs:
        return jsonify({"ok": False, "errors": errs}), 400
    slug = _safe(model["name"]).lower()
    filename = "maps/community/%s.map" % slug
    base = "https://github.com/%s/new/%s" % (config.GITHUB_REPO, config.GITHUB_BRANCH)
    url = "%s?filename=%s&value=%s" % (base, quote(filename, safe=""), quote(text, safe=""))
    return jsonify({"ok": True, "url": url, "bare_url": "%s?filename=%s" % (base, quote(filename, safe="")),
                    "filename": filename, "text": text, "url_len": len(url)})


@app.route("/new")
def map_new():
    try:
        with open(config.REGISTRY) as fh:
            md = json.load(fh)["limits"]["max_dim"]
        w = max(4, min(md, int(request.args.get("w", 16))))
        h = max(4, min(md, int(request.args.get("h", 16))))
    except ValueError:
        return jsonify({"error": "bad w/h"}), 400
    name = _safe(request.args.get("name", "untitled")).upper()[:16]
    return jsonify({"model": mapfmt.new_model(name, w, h)})


if __name__ == "__main__":
    print("map-editor: repo=%s  maps=%s" % (config.REPO_ROOT, config.MAPS_DIR))
    app.run(host=config.HOST, port=config.PORT, debug=config.DEBUG)
