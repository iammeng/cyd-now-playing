"""One-time Spotify login. Opens a browser, stores the refresh token next to this file.

Run:  .venv/bin/python auth.py
"""
import spotipy

from spotify_common import load_config, make_auth_manager


def main():
    cfg = load_config()
    sp = spotipy.Spotify(auth_manager=make_auth_manager(cfg, open_browser=True))
    me = sp.me()
    print(f"Logged in as: {me.get('display_name')} ({me.get('id')})")
    print("Token cached. app.py will refresh it automatically from now on.")


if __name__ == "__main__":
    main()
