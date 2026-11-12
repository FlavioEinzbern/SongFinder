import sys
import os
import asyncio
import urllib.request
import urllib.parse
import webbrowser
from io import BytesIO
import tkinter as tk
from PIL import Image, ImageTk
from shazamio import Shazam
from ytmusicapi import YTMusic


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
    """Runs Shazam acoustic fingerprinting on the recorded WAV file."""
    shazam = Shazam()
    if hasattr(shazam, "recognize"):
        out = await shazam.recognize(wav_path)
    else:
        out = await shazam.recognize_song(wav_path)

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
    """Renders a lightweight desktop card with the album cover and YouTube buttons."""
    root = tk.Tk()
    root.title("Song Identified" if data.get("found") else "No Match Found")
    root.configure(bg="#18181b")
    root.resizable(False, False)
    root.attributes("-topmost", True)

    # Position window near bottom-right of the screen
    win_w, win_h = 420, 210
    screen_w = root.winfo_screenwidth()
    screen_h = root.winfo_screenheight()
    root.geometry(f"{win_w}x{win_h}+{screen_w - win_w - 24}+{screen_h - win_h - 64}")

    if not data.get("found"):
        lbl = tk.Label(
            root,
            text="Could not identify the playing audio.\nMake sure music is playing clearly and try again.",
            fg="#e4e4e7", bg="#18181b", font=("Segoe UI", 10), justify="center"
        )
        lbl.pack(expand=True, padx=20, pady=20)
        root.after(5000, root.destroy)
        root.mainloop()
        return

    container = tk.Frame(root, bg="#18181b", padx=14, pady=14)
    container.pack(fill="both", expand=True)

    # Load Album Cover Art
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

    if cover_photo:
        img_label = tk.Label(container, image=cover_photo, bg="#27272a", bd=0)
        img_label.image = cover_photo
        img_label.pack(side="left", padx=(0, 14))

    info_frame = tk.Frame(container, bg="#18181b")
    info_frame.pack(side="left", fill="both", expand=True)

    tk.Label(
        info_frame, text=data["title"], fg="#ffffff", bg="#18181b",
        font=("Segoe UI", 12, "bold"), anchor="w", wraplength=230, justify="left"
    ).pack(fill="x")

    tk.Label(
        info_frame, text=data["artist"], fg="#a1a1aa", bg="#18181b",
        font=("Segoe UI", 10), anchor="w", wraplength=230, justify="left"
    ).pack(fill="x", pady=(2, 0))

    tk.Label(
        info_frame, text=f"Album: {data['album']}", fg="#71717a", bg="#18181b",
        font=("Segoe UI", 9), anchor="w", wraplength=230, justify="left"
    ).pack(fill="x", pady=(2, 12))

    btn_frame = tk.Frame(info_frame, bg="#18181b")
    btn_frame.pack(fill="x", side="bottom")

    tk.Button(
        btn_frame, text="YouTube", bg="#ef4444", fg="#ffffff",
        activebackground="#dc2626", activeforeground="#ffffff",
        font=("Segoe UI", 9, "bold"), relief="flat", padx=10, pady=5, cursor="hand2",
        command=lambda: webbrowser.open(data["yt_url"])
    ).pack(side="left", padx=(0, 8))

    tk.Button(
        btn_frame, text="YT Music", bg="#27272a", fg="#ffffff",
        activebackground="#3f3f46", activeforeground="#ffffff",
        font=("Segoe UI", 9, "bold"), relief="flat", padx=10, pady=5, cursor="hand2",
        command=lambda: webbrowser.open(data["ytm_url"])
    ).pack(side="left")

    root.mainloop()


def main():
    wav_path = sys.argv[1] if len(sys.argv) > 1 else "temp.wav"
    if not os.path.exists(wav_path):
        sys.exit(1)

    try:
        data = asyncio.run(identify_audio(wav_path))
    except Exception:
        data = {"found": False}
    finally:
        # Clean up the temporary WAV file immediately
        if os.path.exists(wav_path):
            try:
                os.remove(wav_path)
            except OSError:
                pass

    show_result_window(data)


if __name__ == "__main__":
    main()