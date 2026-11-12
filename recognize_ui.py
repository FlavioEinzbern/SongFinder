import sys
import os
import warnings

# Silence pydub's top-level ffmpeg warning on clean Windows PCs
warnings.filterwarnings("ignore", category=RuntimeWarning)

import ctypes
import asyncio
import urllib.request
import urllib.parse
import webbrowser
from io import BytesIO
import tkinter as tk
from PIL import Image, ImageTk
from shazamio import Shazam
from ytmusicapi import YTMusic

# Win32 message constants matching host_tray.c
WM_COMMAND          = 0x0111
WM_APP_LOADER_STATE = 0x0400 + 2  # WM_USER + 2
ID_TRAY_IDENTIFY    = 1001


def notify_host_hide_loader():
    """Tells the C host to hide the bottom-right loading pill immediately."""
    hwnd = ctypes.windll.user32.FindWindowW("SongFinderTrayClass", "SongFinderHost")
    if hwnd:
        ctypes.windll.user32.PostMessageW(hwnd, WM_APP_LOADER_STATE, 0, 0)


def notify_host_retry():
    """Tells the C host to start a fresh 4-second audio capture immediately."""
    hwnd = ctypes.windll.user32.FindWindowW("SongFinderTrayClass", "SongFinderHost")
    if hwnd:
        ctypes.windll.user32.PostMessageW(hwnd, WM_COMMAND, ID_TRAY_IDENTIFY, 0)


def extract_album_from_shazam(track_data: dict) -> str:
    """Extracts the album name from Shazam's nested metadata sections."""
    for section in track_data.get("sections", []):
        if section.get("type") == "SONG":
            for meta in section.get("metadata", []):
                if meta.get("title", "").lower() == "album":
                    return meta.get("text", "")
    return ""


def resolve_youtube_links(artist: str, title: str) -> dict:
    """Finds the exact YouTube / YT Music videoId and fallback album name."""
    query = f"{artist} - {title}"
    encoded_q = urllib.parse.quote_plus(query)
    result = {
        "yt_url": f"https://www.youtube.com/results?search_query={encoded_q}",
        "ytm_url": f"https://music.youtube.com/search?q={encoded_q}",
        "album": ""
    }

    try:
        ytm = YTMusic()
        search_hits = ytm.search(query, filter="songs", limit=1)
        if not search_hits:
            search_hits = ytm.search(query, filter="videos", limit=1)

        if search_hits:
            top = search_hits[0]
            video_id = top.get("videoId")
            if video_id:
                result["yt_url"] = f"https://www.youtube.com/watch?v={video_id}"
                result["ytm_url"] = f"https://music.youtube.com/watch?v={video_id}"
            album_info = top.get("album")
            if isinstance(album_info, dict) and album_info.get("name"):
                result["album"] = album_info["name"]
    except Exception:
        pass

    return result


async def identify_audio(wav_path: str) -> dict:
    """Fingerprints the pre-formatted 16kHz Mono WAV bytes in memory without ffmpeg."""
    shazam = Shazam()
    with open(wav_path, "rb") as f:
        wav_bytes = f.read()

    if hasattr(shazam, "recognize"):
        out = await shazam.recognize(wav_bytes)
    else:
        out = await shazam.recognize_song(wav_bytes)

    track = out.get("track")
    if not track:
        return {"found": False}

    title = track.get("title", "Unknown Title")
    artist = track.get("subtitle", "Unknown Artist")
    images = track.get("images", {})
    cover_url = images.get("coverarthq") or images.get("coverart") or ""
    album = extract_album_from_shazam(track)

    yt_data = resolve_youtube_links(artist, title)
    if not album and yt_data["album"]:
        album = yt_data["album"]

    return {
        "found": True,
        "title": title,
        "artist": artist,
        "album": album or "Single / Unknown Album",
        "cover_url": cover_url,
        "yt_url": yt_data["yt_url"],
        "ytm_url": yt_data["ytm_url"]
    }


def show_result_window(data: dict):
    """Renders a desktop card with album art, YouTube links, and Try Again button."""
    root = tk.Tk()
    root.title("Song Identified" if data.get("found") else "No Match Found")
    root.configure(bg="#18181b")
    root.resizable(False, False)
    root.attributes("-topmost", True)

    win_w, win_h = 440, 210
    screen_w = root.winfo_screenwidth()
    screen_h = root.winfo_screenheight()
    root.geometry(f"{win_w}x{win_h}+{screen_w - win_w - 24}+{screen_h - win_h - 64}")

    def open_link_and_close(url: str):
        webbrowser.open(url)
        root.destroy()

    def retry_and_close():
        root.destroy()
        notify_host_retry()

    if not data.get("found"):
        notify_host_hide_loader()
        container = tk.Frame(root, bg="#18181b", padx=20, pady=20)
        container.pack(fill="both", expand=True)

        tk.Label(
            container,
            text="Could not identify the playing audio.\nMake sure the song is playing clearly.",
            fg="#e4e4e7", bg="#18181b", font=("Segoe UI", 10), justify="center"
        ).pack(expand=True, pady=(10, 14))

        btn_row = tk.Frame(container, bg="#18181b")
        btn_row.pack()

        tk.Button(
            btn_row, text="Try Again", bg="#3b82f6", fg="#ffffff",
            activebackground="#2563eb", activeforeground="#ffffff",
            font=("Segoe UI", 9, "bold"), relief="flat", padx=14, pady=6, cursor="hand2",
            command=retry_and_close
        ).pack(side="left", padx=6)

        tk.Button(
            btn_row, text="Close", bg="#27272a", fg="#e4e4e7",
            activebackground="#3f3f46", activeforeground="#ffffff",
            font=("Segoe UI", 9), relief="flat", padx=14, pady=6, cursor="hand2",
            command=root.destroy
        ).pack(side="left", padx=6)

        root.mainloop()
        return

    container = tk.Frame(root, bg="#18181b", padx=14, pady=14)
    container.pack(fill="both", expand=True)

    cover_photo = None
    if data.get("cover_url"):
        try:
            req = urllib.request.Request(data["cover_url"], headers={"User-Agent": "Mozilla/5.0"})
            with urllib.request.urlopen(req, timeout=4) as resp:
                img_bytes = resp.read()
            pil_img = Image.open(BytesIO(img_bytes)).resize((140, 140), Image.Resampling.LANCZOS)
            cover_photo = ImageTk.PhotoImage(pil_img)
        except Exception:
            cover_photo = None

    notify_host_hide_loader()

    if cover_photo:
        img_label = tk.Label(container, image=cover_photo, bg="#27272a", bd=0)
        img_label.image = cover_photo
        img_label.pack(side="left", padx=(0, 14))

    info_frame = tk.Frame(container, bg="#18181b")
    info_frame.pack(side="left", fill="both", expand=True)

    tk.Label(
        info_frame, text=data["title"], fg="#ffffff", bg="#18181b",
        font=("Segoe UI", 12, "bold"), anchor="w", wraplength=250, justify="left"
    ).pack(fill="x")

    tk.Label(
        info_frame, text=data["artist"], fg="#a1a1aa", bg="#18181b",
        font=("Segoe UI", 10), anchor="w", wraplength=250, justify="left"
    ).pack(fill="x", pady=(2, 0))

    tk.Label(
        info_frame, text=f"Album: {data['album']}", fg="#71717a", bg="#18181b",
        font=("Segoe UI", 9), anchor="w", wraplength=250, justify="left"
    ).pack(fill="x", pady=(2, 12))

    btn_frame = tk.Frame(info_frame, bg="#18181b")
    btn_frame.pack(fill="x", side="bottom")

    tk.Button(
        btn_frame, text="YouTube", bg="#ef4444", fg="#ffffff",
        activebackground="#dc2626", activeforeground="#ffffff",
        font=("Segoe UI", 9, "bold"), relief="flat", padx=9, pady=5, cursor="hand2",
        command=lambda: open_link_and_close(data["yt_url"])
    ).pack(side="left", padx=(0, 6))

    tk.Button(
        btn_frame, text="YT Music", bg="#27272a", fg="#ffffff",
        activebackground="#3f3f46", activeforeground="#ffffff",
        font=("Segoe UI", 9, "bold"), relief="flat", padx=9, pady=5, cursor="hand2",
        command=lambda: open_link_and_close(data["ytm_url"])
    ).pack(side="left", padx=(0, 6))

    tk.Button(
        btn_frame, text="Try Again", bg="#3b82f6", fg="#ffffff",
        activebackground="#2563eb", activeforeground="#ffffff",
        font=("Segoe UI", 9, "bold"), relief="flat", padx=9, pady=5, cursor="hand2",
        command=retry_and_close
    ).pack(side="left")

    root.mainloop()


def main():
    wav_path = sys.argv[1] if len(sys.argv) > 1 else "temp.wav"
    if not os.path.exists(wav_path):
        notify_host_hide_loader()
        sys.exit(1)

    try:
        data = asyncio.run(identify_audio(wav_path))
    except Exception:
        data = {"found": False}
    finally:
        if os.path.exists(wav_path):
            try:
                os.remove(wav_path)
            except OSError:
                pass

    show_result_window(data)


if __name__ == "__main__":
    main()
