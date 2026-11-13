import sys
import os
import io
import wave
import array
import json
import warnings
import subprocess

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

try:
    import speech_recognition as sr
except ImportError:
    sr = None

# Win32 message constants matching host_tray.c
WM_COMMAND          = 0x0111
WM_APP_LOADER_STATE = 0x0400 + 2  # WM_USER + 2 (0=Hide, 1=Listen, 2=Search, 3=Load, 4=Deep Listen)
ID_TRAY_IDENTIFY    = 1001
CREATE_NO_WINDOW    = 0x08000000

HTTP_HEADERS = {"User-Agent": "SongFinder/1.0 (Windows; +https://github.com)"}


def notify_host_loader_state(state: int):
    """Updates the C bottom-right equalizer pill (0=Hide, 2=Searching, 4=Deep Listen)."""
    hwnd = ctypes.windll.user32.FindWindowW("SongFinderTrayClass", "SongFinderHost")
    if hwnd:
        ctypes.windll.user32.PostMessageW(hwnd, WM_APP_LOADER_STATE, state, 0)


def notify_host_hide_loader():
    notify_host_loader_state(0)


def notify_host_retry():
    """Tells the C host to start a fresh 4-second audio capture immediately."""
    hwnd = ctypes.windll.user32.FindWindowW("SongFinderTrayClass", "SongFinderHost")
    if hwnd:
        ctypes.windll.user32.PostMessageW(hwnd, WM_COMMAND, ID_TRAY_IDENTIFY, 0)


def fetch_json(url: str, timeout: float = 3.5) -> dict:
    """Helper for querying AnimeThemes, VGMdb, and Jikan JSON REST APIs."""
    try:
        req = urllib.request.Request(url, headers=HTTP_HEADERS)
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8", errors="ignore"))
    except Exception:
        return {}


def shift_wav_speed(wav_bytes: bytes, factor: float) -> bytes:
    """
    Backup 2 Helper: Resamples 16kHz 16-bit mono PCM in memory by `factor`
    (0.88x restores sped-up/Nightcore; 1.14x restores Slowed+Reverb) without ffmpeg.
    """
    try:
        with wave.open(io.BytesIO(wav_bytes), "rb") as wf:
            frames = wf.readframes(wf.getnframes())
        samples = array.array("h")
        samples.frombytes(frames)
        n = len(samples)
        if n == 0:
            return wav_bytes

        new_n = int(n / factor)
        shifted = array.array("h", (samples[min(int(i * factor), n - 1)] for i in range(new_n)))

        out_io = io.BytesIO()
        with wave.open(out_io, "wb") as wf_out:
            wf_out.setnchannels(1)
            wf_out.setsampwidth(2)
            wf_out.setframerate(16000)
            wf_out.writeframes(shifted.tobytes())
        return out_io.getvalue()
    except Exception:
        return wav_bytes


def extract_album_from_shazam(track_data: dict) -> str:
    """Extracts the album name from Shazam's nested metadata sections."""
    for section in track_data.get("sections", []):
        if section.get("type") == "SONG":
            for meta in section.get("metadata", []):
                if meta.get("title", "").lower() == "album":
                    return meta.get("text", "")
    return ""


def query_animethemes_api(query: str) -> dict:
    """Queries AnimeThemes API for anime OP/ED song title, artist, anime name, and cover art."""
    q = urllib.parse.quote_plus(query.strip())
    url = (
        f"https://api.animethemes.moe/search?q={q}"
        f"&fields[search]=songs&include[song]=animethemes.anime.images,artists"
    )
    data = fetch_json(url)
    songs = data.get("search", {}).get("songs", [])
    if not songs:
        return {}

    song = songs[0]
    song_title = song.get("title") or ""
    artists = song.get("artists", [])
    artist_name = artists[0].get("name", "") if artists else ""

    anime_label = ""
    cover_url = ""
    themes = song.get("animethemes", [])
    if themes:
        first_theme = themes[0]
        slug = first_theme.get("slug", "")
        anime = first_theme.get("anime", {})
        anime_name = anime.get("name", "")
        if anime_name:
            anime_label = f"{anime_name} ({slug})" if slug else anime_name
        for img in anime.get("images", []):
            if img.get("link"):
                cover_url = img["link"]
                break

    if not song_title and not anime_label:
        return {}

    return {
        "title": song_title,
        "artist": artist_name,
        "album": anime_label,
        "cover_url": cover_url
    }


def query_vgmdb_api(query: str) -> dict:
    """Queries VGMdb JSON mirror for Video Game / Anime OST album details and cover art."""
    q = urllib.parse.quote_plus(query.strip())
    search_data = fetch_json(f"https://vgmdb.info/search/albums?q={q}&format=json")
    albums = search_data.get("results", {}).get("albums", [])
    if not albums:
        return {}

    top_album = albums[0]
    titles = top_album.get("titles", {})
    album_name = titles.get("en") or titles.get("ja-latn") or top_album.get("name") or ""
    album_link = top_album.get("link", "")

    cover_url = ""
    artist_name = ""
    if album_link:
        detail = fetch_json(f"https://vgmdb.info/{album_link}?format=json", timeout=3.0)
        cover_url = detail.get("picture_full") or detail.get("picture_small") or ""
        composers = detail.get("composers", [])
        if composers and isinstance(composers[0], dict):
            names = composers[0].get("names", {})
            artist_name = names.get("en") or names.get("ja-latn") or ""

    if not album_name:
        return {}

    return {
        "album": album_name,
        "artist": artist_name,
        "cover_url": cover_url
    }


def query_jikan_api(query: str) -> dict:
    """Queries Jikan v4 (MyAnimeList) API for official anime title and high-res artwork."""
    q = urllib.parse.quote_plus(query.strip())
    data = fetch_json(f"https://api.jikan.moe/v4/anime?q={q}&limit=1")
    items = data.get("data", [])
    if not items:
        return {}

    top = items[0]
    anime_title = top.get("title_english") or top.get("title") or ""
    jpg_imgs = top.get("images", {}).get("jpg", {})
    cover_url = jpg_imgs.get("large_image_url") or jpg_imgs.get("image_url") or ""

    return {
        "album": anime_title,
        "cover_url": cover_url
    }


def enrich_with_anime_vg_databases(title: str, artist: str, album: str, cover_url: str) -> tuple:
    """
    Stage 5: Checks AnimeThemes, VGMdb, and Jikan when Album/Cover are missing
    or to attach the specific Anime OP/ED or Video Game OST source.
    """
    needs_album = (not album) or (album == "Single / Unknown Album")
    query = f"{title} {artist}".strip()

    # 1. Check AnimeThemes API for exact Anime OP/ED source
    at_hit = query_animethemes_api(title) or query_animethemes_api(query)
    if at_hit.get("album"):
        if needs_album:
            album = at_hit["album"]
        if not cover_url and at_hit.get("cover_url"):
            cover_url = at_hit["cover_url"]

    # 2. Check VGMdb for Video Game OST album match if still missing album or cover
    if (not album or album == "Single / Unknown Album") or not cover_url:
        vgm_hit = query_vgmdb_api(query)
        if vgm_hit.get("album") and (not album or album == "Single / Unknown Album"):
            album = vgm_hit["album"]
        if not cover_url and vgm_hit.get("cover_url"):
            cover_url = vgm_hit["cover_url"]

    # 3. Check Jikan API if cover art is still missing
    if not cover_url:
        jikan_hit = query_jikan_api(album if album else query)
        if jikan_hit.get("cover_url"):
            cover_url = jikan_hit["cover_url"]

    return album, cover_url


def resolve_youtube_links(artist: str, title: str) -> dict:
    """Finds the exact YouTube / YT Music videoId, fallback album name, and thumbnail."""
    query = f"{artist} - {title}".strip(" -")
    encoded_q = urllib.parse.quote_plus(query)
    result = {
        "yt_url": f"https://www.youtube.com/results?search_query={encoded_q}",
        "ytm_url": f"https://music.youtube.com/search?q={encoded_q}",
        "album": "",
        "cover_url": ""
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
            thumbs = top.get("thumbnails", [])
            if thumbs and isinstance(thumbs[-1], dict):
                result["cover_url"] = thumbs[-1].get("url", "")
    except Exception:
        pass

    return result


async def shazam_recognize_bytes(shazam: Shazam, wav_bytes: bytes) -> dict:
    """Queries Shazam with raw WAV bytes and returns the track dict if matched."""
    try:
        if hasattr(shazam, "recognize"):
            out = await shazam.recognize(wav_bytes)
        else:
            out = await shazam.recognize_song(wav_bytes)
        return out.get("track") or {}
    except Exception:
        return {}


async def shazam_with_speed_fallback(shazam: Shazam, wav_bytes: bytes) -> dict:
    """
    Runs primary Shazam scan first; if no match, runs Backup 2
    (0.88x Nightcore/sped-up fix and 1.14x Slowed+Reverb fix) in parallel.
    """
    track = await shazam_recognize_bytes(shazam, wav_bytes)
    if track:
        return track

    # Backup 2: Test speed/pitch-shifted buffers concurrently
    slower_bytes = shift_wav_speed(wav_bytes, 0.88)
    faster_bytes = shift_wav_speed(wav_bytes, 1.14)
    results = await asyncio.gather(
        shazam_recognize_bytes(shazam, slower_bytes),
        shazam_recognize_bytes(shazam, faster_bytes)
    )
    for res in results:
        if res:
            return res
    return {}


def transcribe_vocals_single_lang(wav_bytes: bytes, lang: str) -> str:
    """Transcribes sung vocals in a specific language using Google Web Speech."""
    if sr is None:
        return ""
    try:
        recognizer = sr.Recognizer()
        with sr.AudioFile(io.BytesIO(wav_bytes)) as source:
            audio_data = recognizer.record(source)
        text = recognizer.recognize_google(audio_data, language=lang)
        return text.strip() if isinstance(text, str) else ""
    except Exception:
        return ""


async def backup4_vocal_search(wav_bytes: bytes) -> dict:
    """
    Backup 4: Transcribes vocals in English (en-US) and Japanese (ja-JP) in parallel,
    then searches AnimeThemes, VGMdb, and YouTube Music.
    """
    en_text, ja_text = await asyncio.gather(
        asyncio.to_thread(transcribe_vocals_single_lang, wav_bytes, "en-US"),
        asyncio.to_thread(transcribe_vocals_single_lang, wav_bytes, "ja-JP")
    )

    candidates = []
    # Require at least 2 words for English or 4 characters for Japanese to filter out noise
    if en_text and len(en_text.split()) >= 2:
        candidates.append(en_text)
    if ja_text and len(ja_text) >= 4:
        candidates.append(ja_text)

    if not candidates:
        return {}

    ytm = None
    try:
        ytm = YTMusic()
    except Exception:
        pass

    for phrase in candidates:
        # 1. Check AnimeThemes API directly with transcribed phrase
        at_match = await asyncio.to_thread(query_animethemes_api, phrase)
        if at_match.get("title"):
            return {
                "title": at_match["title"],
                "artist": at_match.get("artist") or "Anime Theme Artist",
                "album": at_match.get("album") or "",
                "cover_url": at_match.get("cover_url") or ""
            }

        # 2. Search YouTube Music songs by transcribed lyrics
        if ytm is not None:
            try:
                hits = ytm.search(phrase, filter="songs", limit=1)
                if hits:
                    top = hits[0]
                    title = top.get("title", "")
                    artists = top.get("artists", [])
                    artist = artists[0].get("name", "Unknown Artist") if artists else "Unknown Artist"
                    album_info = top.get("album")
                    album = album_info.get("name", "") if isinstance(album_info, dict) else ""
                    thumbs = top.get("thumbnails", [])
                    cover_url = thumbs[-1].get("url", "") if thumbs else ""
                    if title:
                        return {
                            "title": title,
                            "artist": artist,
                            "album": album,
                            "cover_url": cover_url
                        }
            except Exception:
                pass

    return {}


def build_final_result(title: str, artist: str, album: str, cover_url: str) -> dict:
    """Combines YouTube Music link resolution with AnimeThemes/VGMdb/Jikan enrichment."""
    yt_data = resolve_youtube_links(artist, title)
    if not album and yt_data["album"]:
        album = yt_data["album"]
    if not cover_url and yt_data["cover_url"]:
        cover_url = yt_data["cover_url"]

    album, cover_url = enrich_with_anime_vg_databases(title, artist, album, cover_url)

    return {
        "found": True,
        "title": title,
        "artist": artist,
        "album": album or "Single / Unknown Album",
        "cover_url": cover_url,
        "yt_url": yt_data["yt_url"],
        "ytm_url": yt_data["ytm_url"]
    }


async def identify_audio(wav_path: str, host_exe_path: str) -> dict:
    """
    Executes the full recognition chain:
      1. Primary 4s Shazam scan
      2. Backup 2: Speed/Pitch-Shift Retry (0.88x & 1.14x)
      3. Backup 1: C Audio Engine 8s Deep Listen (--capture 8) + Shazam/Speed-Shift
      4. Backup 4: Dual-Language (en-US + ja-JP) Vocal Transcription -> AnimeThemes / YTMusic
      5. Stage 5: AnimeThemes / VGMdb / Jikan metadata & cover art enrichment
    """
    shazam = Shazam()
    with open(wav_path, "rb") as f:
        wav_4s_bytes = f.read()

    # Stages 1 & 2: Primary 4s Shazam + Speed/Pitch-Shift Retry
    track = await shazam_with_speed_fallback(shazam, wav_4s_bytes)
    if track:
        title = track.get("title", "Unknown Title")
        artist = track.get("subtitle", "Unknown Artist")
        images = track.get("images", {})
        cover_url = images.get("coverarthq") or images.get("coverart") or ""
        album = extract_album_from_shazam(track)
        return await asyncio.to_thread(build_final_result, title, artist, album, cover_url)

    # Stage 3 (Backup 1): Trigger C Audio Engine 8-Second Deep Listen
    active_wav_bytes = wav_4s_bytes
    if host_exe_path and os.path.exists(host_exe_path):
        deep_wav_path = os.path.join(os.path.dirname(os.path.abspath(wav_path)), "deep_temp.wav")
        try:
            notify_host_loader_state(4)  # Switch pill to purple "Deep Listen..."
            proc = await asyncio.to_thread(
                subprocess.run,
                [host_exe_path, "--capture", "8", deep_wav_path],
                creationflags=CREATE_NO_WINDOW
            )
            notify_host_loader_state(2)  # Switch pill back to blue "Searching..."

            if proc.returncode == 0 and os.path.exists(deep_wav_path):
                with open(deep_wav_path, "rb") as df:
                    active_wav_bytes = df.read()

                track = await shazam_with_speed_fallback(shazam, active_wav_bytes)
                if track:
                    title = track.get("title", "Unknown Title")
                    artist = track.get("subtitle", "Unknown Artist")
                    images = track.get("images", {})
                    cover_url = images.get("coverarthq") or images.get("coverart") or ""
                    album = extract_album_from_shazam(track)
                    return await asyncio.to_thread(build_final_result, title, artist, album, cover_url)
        except Exception:
            notify_host_loader_state(2)
        finally:
            if os.path.exists(deep_wav_path):
                try:
                    os.remove(deep_wav_path)
                except OSError:
                    pass

    # Stage 4 (Backup 4): Dual-Language (en-US / ja-JP) Vocal Transcription Fallback
    vocal_hit = await backup4_vocal_search(active_wav_bytes)
    if vocal_hit.get("title"):
        return await asyncio.to_thread(
            build_final_result,
            vocal_hit["title"],
            vocal_hit["artist"],
            vocal_hit.get("album", ""),
            vocal_hit.get("cover_url", "")
        )

    return {"found": False}


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
    host_exe_path = sys.argv[2] if len(sys.argv) > 2 else ""

    if not os.path.exists(wav_path):
        notify_host_hide_loader()
        sys.exit(1)

    try:
        data = asyncio.run(identify_audio(wav_path, host_exe_path))
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
