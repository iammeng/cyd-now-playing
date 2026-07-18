"""Shared config + Spotify client setup for auth.py and app.py."""
import json
import os
import sys

BASE = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(BASE, "config.json")
CACHE_PATH = os.path.join(BASE, ".spotify_token_cache")

SCOPES = "user-read-currently-playing user-read-playback-state user-modify-playback-state"

CONFIG_TEMPLATE = {
    "client_id": "",
    "client_secret": "",
    "redirect_uri": "http://127.0.0.1:8888/callback",
    "port": 8080,
}


def load_config():
    if not os.path.exists(CONFIG_PATH):
        with open(CONFIG_PATH, "w") as f:
            json.dump(CONFIG_TEMPLATE, f, indent=2)
        sys.exit(
            f"Created {CONFIG_PATH}\n"
            "Fill in client_id / client_secret from https://developer.spotify.com/dashboard\n"
            "(app redirect URI must be exactly http://127.0.0.1:8888/callback), then rerun."
        )
    with open(CONFIG_PATH) as f:
        cfg = json.load(f)
    if not cfg.get("client_id") or not cfg.get("client_secret"):
        sys.exit(f"client_id / client_secret missing in {CONFIG_PATH}")
    return cfg


def make_auth_manager(cfg, open_browser):
    from spotipy.oauth2 import SpotifyOAuth

    return SpotifyOAuth(
        client_id=cfg["client_id"],
        client_secret=cfg["client_secret"],
        redirect_uri=cfg.get("redirect_uri", CONFIG_TEMPLATE["redirect_uri"]),
        scope=SCOPES,
        cache_path=CACHE_PATH,
        open_browser=open_browser,
    )
