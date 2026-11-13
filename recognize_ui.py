import sys
import os
import io
import re
import wave
import array
import json
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

try:
    import speech_recognition as sr
except ImportError:
    sr = None

# Win32 message constants matching host_tray.c
WM_COMMAND          = 0x0111
WM_APP_LOADER_STATE = 0x0400 + 2  # WM_USER + 2 (0=Hide, 1=Listen, 2=Search, 3=Load, 4=Deep Listen)
ID_TRAY_IDENTIFY    = 1001

HTTP_HEADERS = {"User-Agent": "SongFinder/1.0 (Windows; +https://github.com)"}

# 5 supported languages for Backup 4 vocal recognition
VOCAL_LANGUAGES = ["en-US", "ja-JP", "pt-BR", "es-ES", "ko-KR"]


def notify_host_loader_state(state: int):
    """Updates the C bottom-right equalizer pill (0=Hide, 2=Searching, 4=Deep Listen)."""
    hwnd = ctypes.windll.user32.FindWindowW("SongFinderTrayClass", "SongFinderHost")
    if hwnd:
        ctypes.windll.user32.PostMessageW(hwnd, WM_APP_LOADER_STATE, state, 0)


def notify_host_hide_loader():
    notify_host_loader_state(0)


def notify_host_retry():
    """Tells the C host to start a fresh audio capture immediately."""
    hwnd = ctypes.windll.user32.FindWindowW("SongFinderTrayClass", "SongFinderHost")
    if hwnd:
        ctypes.windll.user32.PostMessageW(hwnd, WM_COMMAND, ID_TRAY_IDENTIFY, 0)


def fetch_json(url: str, timeout: float = 3.5) -> dict:
    """Helper for querying JSON REST APIs."""
    try:
        req = urllib.request.Request(url, headers=HTTP_HEADERS)
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8", errors="ignore"))
    except Exception:
        return {}


def normalize_for_match(text: str) -> str:
    """Normalizes track/artist/album strings (preserving Unicode) for catalog verification."""
    if not text:
        return ""
    cleaned = re.sub(r"\(.*?\)|\[.*?\]", " ", text.lower())
    cleaned = re.sub(r"[^\w\s]", " ", cleaned)
    return " ".join(cleaned.split())


def names_match(expected: str, candidate: str) -> bool:
    """Checks if a catalog result's title, artist, or album matches the expected value."""
    a = normalize_for_match(expected)
    b = normalize_for_match(candidate)
    if not a or not b:
        return False
    if a in b or b in a:
        return True
    # Also check primary artist token before feat./comma separators
    primary_a = normalize_for_match(re.split(r"[,&/]|feat\.?|ft\.?|cv\.?", expected, flags=re.IGNORECASE)[0])
    if primary_a and len(primary_a) >= 3 and (primary_a in b or b in primary_a):
        return True
    return False


def extract_youtube_video_id(url: str) -> str:
    """Extracts the 11-character YouTube videoId from a youtu.be or youtube.com URL."""
    if not url:
        return ""
    patterns = [
        r"(?:v=|youtu\.be/|/embed/|/v/)([A-Za-z0-9_-]{11})",
    ]
    for pat in patterns:
        m = re.search(pat, url)
        if m:
            return m.group(1)
    return ""


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


def extract_streaming_from_shazam(track_data: dict) -> dict:
    """
    Extracts direct Apple Music, Spotify, and YouTube links from Shazam's payload
    (Fix 1: checks hub actions/providers as well as the VIDEO section).
    """
    links = {"apple_url": "", "spotify_url": "", "yt_url": ""}
    hub = track_data.get("hub", {})
    if isinstance(hub, dict):
        # 1. Check hub actions and options for Apple Music URLs
        action_lists = [hub.get("actions", [])]
        for opt in hub.get("options", []):
            if isinstance(opt, dict):
                action_lists.append(opt.get("actions", []))

        for actions in action_lists:
            if not isinstance(actions, list):
                continue
            for act in actions:
                if not isinstance(act, dict):
                    continue
                uri = act.get("uri", "")
                if uri.startswith("https://music.apple.com") or uri.startswith("https://itunes.apple.com"):
                    links["apple_url"] = uri
                    break
            if links["apple_url"]:
                break

        # 2. Check hub providers for Spotify or YouTube links
        for prov in hub.get("providers", []):
            if not isinstance(prov, dict):
                continue
            p_type = prov.get("type", "").upper()
            p_cap = prov.get("caption", "").lower()
            for act in prov.get("actions", []):
                if not isinstance(act, dict):
                    continue
                uri = act.get("uri", "")
                if p_type == "SPOTIFY" or "spotify" in p_cap:
                    if uri.startswith("https://open.spotify.com/"):
                        links["spotify_url"] = uri
                        break
                    elif uri.startswith("spotify:track:"):
                        track_id = uri.split("spotify:track:")[-1].split("?")[0].strip()
                        if track_id:
                            links["spotify_url"] = f"https://open.spotify.com/track/{track_id}"
                            break
                    elif uri.startswith("spotify:search:") and not links["spotify_url"]:
                        raw_q = uri.split("spotify:search:")[-1].strip()
                        if raw_q:
                            links["spotify_url"] = f"https://open.spotify.com/search/{raw_q}"
                if "youtu.be/" in uri or "youtube.com/watch" in uri:
                    links["yt_url"] = uri

    # 3. Check Shazam's VIDEO section for direct YouTube URLs or Shazam video endpoints
    for section in track_data.get("sections", []):
        if not isinstance(section, dict):
            continue
        yt_field = section.get("youtubeurl")
        if isinstance(yt_field, str):
            if "youtu.be/" in yt_field or "youtube.com/watch" in yt_field:
                links["yt_url"] = yt_field
                break
            elif yt_field.startswith("https://"):
                vid_json = fetch_json(yt_field, timeout=2.0)
                for act in vid_json.get("actions", []):
                    uri = act.get("uri", "") if isinstance(act, dict) else ""
                    if "youtu.be/" in uri or "youtube.com/watch" in uri:
                        links["yt_url"] = uri
                        break
        elif isinstance(yt_field, dict):
            for act in yt_field.get("actions", []):
                uri = act.get("uri", "") if isinstance(act, dict) else ""
                if "youtu.be/" in uri or "youtube.com/watch" in uri:
                    links["yt_url"] = uri
                    break
        if links["yt_url"]:
            break

    return links


def resolve_streaming_and_songlink(
    artist: str,
    title: str,
    shazam_apple: str = "",
    shazam_spotify: str = "",
    shazam_yt: str = ""
) -> dict:
    """
    Verifies Apple Music & Spotify presence and uses Songlink's ISRC lookup (Fix 1)
    to fetch exact YouTube and YouTube Music links whenever available.
    """
    apple_url = shazam_apple
    spotify_url = shazam_spotify
    yt_url = shazam_yt
    ytm_url = ""

    query = f"{artist} {title}".strip()
    encoded_q = urllib.parse.quote_plus(query)

    # 1. Verify / Resolve Apple Music via Apple's keyless iTunes Search API
    if not apple_url:
        itunes_data = fetch_json(
            f"https://itunes.apple.com/search?term={encoded_q}&entity=song&limit=5",
            timeout=3.0
        )
        for item in itunes_data.get("results", []):
            if names_match(title, item.get("trackName", "")) and names_match(artist, item.get("artistName", "")):
                apple_url = item.get("trackViewUrl", "")
                if apple_url:
                    break

    # 2. Query Songlink using Apple Music URL to get exact ISRC-matched Spotify & YouTube links
    if apple_url and (not spotify_url or "/search/" in spotify_url or not yt_url):
        sl_url = f"https://api.song.link/v1-alpha.1/links?url={urllib.parse.quote_plus(apple_url)}"
        sl_data = fetch_json(sl_url, timeout=2.5)
        platforms = sl_data.get("linksByPlatform", {})
        if isinstance(platforms, dict):
            sp_link = platforms.get("spotify", {}).get("url", "")
            if sp_link and (not spotify_url or "/search/" in spotify_url):
                spotify_url = sp_link

            sl_yt = platforms.get("youtube", {}).get("url", "")
            if sl_yt and not yt_url:
                yt_url = sl_yt

            sl_ytm = platforms.get("youtubeMusic", {}).get("url", "")
            if sl_ytm:
                ytm_url = sl_ytm

    # 3. If Spotify is still unresolved, verify commercial streaming presence via Deezer API
    if not spotify_url:
        dz_data = fetch_json(f"https://api.deezer.com/search?q={encoded_q}&limit=5", timeout=2.5)
        for item in dz_data.get("data", []):
            dz_title = item.get("title", "")
            dz_artist = item.get("artist", {}).get("name", "") if isinstance(item.get("artist"), dict) else ""
            if names_match(title, dz_title) and names_match(artist, dz_artist):
                spotify_url = f"https://open.spotify.com/search/{urllib.parse.quote(query)}"
                break

    return {
        "apple_url": apple_url,
        "spotify_url": spotify_url,
        "yt_url": yt_url,
        "ytm_url": ytm_url
    }


def is_verified_youtube_hit(
    hit: dict,
    target_title: str,
    target_artist: str,
    target_album: str,
    is_video: bool
) -> bool:
    """
    Fix 2 Helper: Ensures a YouTube Music song or video result actually matches
    the identified track's Title and Artist/Album instead of a random popular song.
    """
    hit_title = hit.get("title", "")
    if not hit_title:
        return False

    hit_artists = " ".join(
        a.get("name", "") for a in hit.get("artists", []) if isinstance(a, dict)
    )
    album_obj = hit.get("album")
    hit_album = album_obj.get("name", "") if isinstance(album_obj, dict) else ""

    title_ok = names_match(target_title, hit_title)
    if not title_ok:
        return False

    known_artist = target_artist and target_artist not in ("Unknown Artist", "Anime Theme Artist")
    known_album = target_album and target_album != "Single / Unknown Album"

    if not is_video:
        # Official "songs" filter: require artist match or album match
        if known_artist and names_match(target_artist, hit_artists):
            return True
        if known_album and (names_match(target_album, hit_album) or names_match(target_album, hit_title)):
            return True
        return not known_artist and not known_album
    else:
        # Standard YouTube "videos" filter: artist or album often appears in the video title or channel name
        combined_meta = f"{hit_title} {hit_artists}"
        if known_artist and names_match(target_artist, combined_meta):
            return True
        if known_album and names_match(target_album, combined_meta):
            return True
        return not known_artist and not known_album


def resolve_youtube_links(
    artist: str,
    title: str,
    album: str = "",
    direct_yt_url: str = "",
    direct_ytm_url: str = ""
) -> dict:
    """
    Resolves YouTube and YT Music links using:
      - Fix 1: Direct videoId from Shazam or Songlink if available
      - Fix 2: Verified limit=5 search across 'songs' then 'videos'
      - Fix 3: Album/Anime/Game context search if artist+title is ambiguous
      - Fix 4: Safe search-results URL fallback if no verified videoId is found
    """
    base_query = f"{artist} - {title}".strip(" -")
    encoded_q = urllib.parse.quote_plus(base_query)

    # Fix 4: Safe default search URLs (used if no specific video passes verification)
    result = {
        "yt_url": f"https://www.youtube.com/results?search_query={encoded_q}",
        "ytm_url": f"https://music.youtube.com/search?q={encoded_q}",
        "album": "",
        "cover_url": ""
    }

    # Fix 1: Use direct videoId from Shazam or Songlink if already discovered
    direct_vid = extract_youtube_video_id(direct_yt_url) or extract_youtube_video_id(direct_ytm_url)
    if direct_vid:
        result["yt_url"] = f"https://www.youtube.com/watch?v={direct_vid}"
        result["ytm_url"] = f"https://music.youtube.com/watch?v={direct_vid}"

    try:
        ytm = YTMusic()
        verified_hit = None

        # Pass 1: Check top 5 licensed "songs"
        song_hits = ytm.search(base_query, filter="songs", limit=5) or []
        for hit in song_hits:
            if is_verified_youtube_hit(hit, title, artist, album, is_video=False):
                verified_hit = hit
                break

        # Pass 2: Check top 5 standard YouTube "videos" (catches OSTs, remixes, covers, uploads)
        if not verified_hit:
            video_hits = ytm.search(base_query, filter="videos", limit=5) or []
            for hit in video_hits:
                if is_verified_youtube_hit(hit, title, artist, album, is_video=True):
                    verified_hit = hit
                    break

        # Pass 3 (Fix 3): Disambiguate using enriched Anime / Game OST Album name
        if not verified_hit and album and album != "Single / Unknown Album":
            album_query = f"{title} {album}".strip()
            album_hits = ytm.search(album_query, filter="videos", limit=5) or []
            for hit in album_hits:
                if is_verified_youtube_hit(hit, title, artist, album, is_video=True):
                    verified_hit = hit
                    break

        if verified_hit:
            video_id = verified_hit.get("videoId")
            if video_id and not direct_vid:
                result["yt_url"] = f"https://www.youtube.com/watch?v={video_id}"
                result["ytm_url"] = f"https://music.youtube.com/watch?v={video_id}"
            album_info = verified_hit.get("album")
            if isinstance(album_info, dict) and album_info.get("name"):
                result["album"] = album_info["name"]
            thumbs = verified_hit.get("thumbnails", [])
            if thumbs and isinstance(thumbs[-1], dict):
                result["cover_url"] = thumbs[-1].get("url", "")
    except Exception:
        pass

    return result


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

    at_hit = query_animethemes_api(title) or query_animethemes_api(query)
    if at_hit.get("album"):
        if needs_album:
            album = at_hit["album"]
        if not cover_url and at_hit.get("cover_url"):
            cover_url = at_hit["cover_url"]

    if (not album or album == "Single / Unknown Album") or not cover_url:
        vgm_hit = query_vgmdb_api(query)
        if vgm_hit.get("album") and (not album or album == "Single / Unknown Album"):
            album = vgm_hit["album"]
        if not cover_url and vgm_hit.get("cover_url"):
            cover_url = vgm_hit["cover_url"]

    if not cover_url:
        jikan_hit = query_jikan_api(album if album else query)
        if jikan_hit.get("cover_url"):
            cover_url = jikan_hit["cover_url"]

    return album, cover_url


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


def transcribe_vocals_single_lang(wav_bytes: bytes, lang: str) -> tuple:
    """
    Transcribes sung vocals in `lang` with `show_all=True` and returns
    `(transcript, confidence, lang)` so the highest-confidence language wins.
    """
    if sr is None:
        return ("", 0.0, lang)
    try:
        recognizer = sr.Recognizer()
        with sr.AudioFile(io.BytesIO(wav_bytes)) as source:
            audio_data = recognizer.record(source)
        res = recognizer.recognize_google(audio_data, language=lang, show_all=True)
        if isinstance(res, dict) and res.get("alternative"):
            alts = res["alternative"]
            top = alts[0]
            text = (top.get("transcript") or "").strip()
            conf = float(top.get("confidence", 0.65))
            return (text, conf, lang)
        elif isinstance(res, str) and res.strip():
            return (res.strip(), 0.65, lang)
    except Exception:
        pass
    return ("", 0.0, lang)


async def backup4_vocal_search(wav_bytes: bytes) -> dict:
    """
    Backup 4: Transcribes vocals across English, Japanese, Portuguese, Spanish,
    and Korean concurrently, sorts by confidence, and searches AnimeThemes & YTMusic.
    """
    tasks = [
        asyncio.to_thread(transcribe_vocals_single_lang, wav_bytes, lang)
        for lang in VOCAL_LANGUAGES
    ]
    raw_results = await asyncio.gather(*tasks)

    candidates = []
    for text, conf, lang in raw_results:
        if not text:
            continue
        if lang in ("ja-JP", "ko-KR"):
            if len(text) >= 4:
                candidates.append((conf, lang, text))
        else:
            if len(text.split()) >= 2:
                candidates.append((conf, lang, text))

    if not candidates:
        return {}

    candidates.sort(key=lambda item: item[0], reverse=True)

    ytm = None
    try:
        ytm = YTMusic()
    except Exception:
        pass

    for _, lang, phrase in candidates:
        if lang in ("ja-JP", "en-US"):
            at_match = await asyncio.to_thread(query_animethemes_api, phrase)
            if at_match.get("title"):
                return {
                    "title": at_match["title"],
                    "artist": at_match.get("artist") or "Anime Theme Artist",
                    "album": at_match.get("album") or "",
                    "cover_url": at_match.get("cover_url") or ""
                }

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


def build_final_result(
    title: str,
    artist: str,
    album: str,
    cover_url: str,
    shazam_apple: str = "",
    shazam_spotify: str = "",
    shazam_yt: str = ""
) -> dict:
    """
    Fix 3 Order:
      1. Enriches Album/Anime/Game metadata first so we have context for disambiguation.
      2. Resolves Apple Music, Spotify, and Songlink ISRC YouTube links.
      3. Resolves & verifies YouTube / YT Music links using direct IDs or verified 5-hit search.
    """
    # 1. Enrich with AnimeThemes, VGMdb, and Jikan first
    album, cover_url = enrich_with_anime_vg_databases(title, artist, album, cover_url)

    # 2. Check Apple Music, Spotify, and Songlink cross-platform IDs
    stream_links = resolve_streaming_and_songlink(
        artist, title, shazam_apple, shazam_spotify, shazam_yt
    )

    # 3. Resolve verified YouTube / YT Music links with enriched album context
    yt_data = resolve_youtube_links(
        artist,
        title,
        album=album,
        direct_yt_url=stream_links["yt_url"],
        direct_ytm_url=stream_links["ytm_url"]
    )

    if (not album or album == "Single / Unknown Album") and yt_data["album"]:
        album = yt_data["album"]
    if not cover_url and yt_data["cover_url"]:
        cover_url = yt_data["cover_url"]

    return {
        "found": True,
        "title": title,
        "artist": artist,
        "album": album or "Single / Unknown Album",
        "cover_url": cover_url,
        "yt_url": yt_data["yt_url"],
        "ytm_url": yt_data["ytm_url"],
        "spotify_url": stream_links["spotify_url"],
        "apple_url": stream_links["apple_url"]
    }


async def wait_for_deep_wav(deep_wav_path: str, max_wait_sec: float = 6.5) -> bool:
    """Waits for the C host's simultaneous 0-10s recording to finish writing."""
    if not deep_wav_path:
        return False
    elapsed = 0.0
    step = 0.15
    while elapsed < max_wait_sec:
        if os.path.exists(deep_wav_path):
            return True
        await asyncio.sleep(step)
        elapsed += step
    return os.path.exists(deep_wav_path)


async def identify_audio(wav_path: str, deep_wav_path: str) -> dict:
    """
    Executes the pipelined recognition chain:
      1. Primary 5s Shazam scan (while C continues 5s->10s recording in background)
      2. Backup 2: Speed/Pitch-Shift Retry (0.88x & 1.14x) on the 5s clip
      3. Backup 1: Wait remaining ~2-3s for C's 10s cumulative clip + Shazam/Speed-Shift
      4. Backup 4: 5-Language (en-US, ja-JP, pt-BR, es-ES, ko-KR) Vocal Transcription
      5. Stage 5: Anime/VG Enrichment -> Songlink/Spotify/Apple -> Verified YouTube Resolution
    """
    shazam = Shazam()
    with open(wav_path, "rb") as f:
        wav_5s_bytes = f.read()

    # Stages 1 & 2: Primary 5s Shazam + Speed/Pitch-Shift Retry
    track = await shazam_with_speed_fallback(shazam, wav_5s_bytes)
    if track:
        title = track.get("title", "Unknown Title")
        artist = track.get("subtitle", "Unknown Artist")
        images = track.get("images", {})
        cover_url = images.get("coverarthq") or images.get("coverart") or ""
        album = extract_album_from_shazam(track)
        hub_links = extract_streaming_from_shazam(track)
        return await asyncio.to_thread(
            build_final_result,
            title, artist, album, cover_url,
            hub_links["apple_url"], hub_links["spotify_url"], hub_links["yt_url"]
        )

    # Stage 3 (Backup 1): Wait for C's simultaneous 0-10s recording to complete
    active_wav_bytes = wav_5s_bytes
    if deep_wav_path:
        notify_host_loader_state(4)  # Switch pill to purple "Deep Listen..."
        deep_ready = await wait_for_deep_wav(deep_wav_path)
        notify_host_loader_state(2)  # Switch pill back to blue "Searching..."

        if deep_ready:
            try:
                with open(deep_wav_path, "rb") as df:
                    active_wav_bytes = df.read()
                track = await shazam_with_speed_fallback(shazam, active_wav_bytes)
                if track:
                    title = track.get("title", "Unknown Title")
                    artist = track.get("subtitle", "Unknown Artist")
                    images = track.get("images", {})
                    cover_url = images.get("coverarthq") or images.get("coverart") or ""
                    album = extract_album_from_shazam(track)
                    hub_links = extract_streaming_from_shazam(track)
                    return await asyncio.to_thread(
                        build_final_result,
                        title, artist, album, cover_url,
                        hub_links["apple_url"], hub_links["spotify_url"], hub_links["yt_url"]
                    )
            except Exception:
                pass

    # Stage 4 (Backup 4): 5-Language Vocal Transcription Fallback
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
    """Renders a desktop card with album art, streaming links, and Try Again button."""
    root = tk.Tk()
    root.title("Song Identified" if data.get("found") else "No Match Found")
    root.configure(bg="#18181b")
    root.resizable(False, False)
    root.attributes("-topmost", True)

    win_w, win_h = 500, 230
    screen_w = root.winfo_screenwidth()
    screen_h = root.winfo_screenheight()
    root.geometry(f"{win_w}x{win_h}+{screen_w - win_w - 24}+{screen_h - win_h - 64}")

    def open_link_and_close(url: str):
        if url:
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
            pil_img = Image.open(BytesIO(img_bytes)).resize((145, 145), Image.Resampling.LANCZOS)
            cover_photo = ImageTk.PhotoImage(pil_img)
        except Exception:
            cover_photo = None

    notify_host_hide_loader()

    if cover_photo:
        img_label = tk.Label(container, image=cover_photo, bg="#27272a", bd=0)
        img_label.image = cover_photo
        img_label.pack(side="left", padx=(0, 14), anchor="n")

    info_frame = tk.Frame(container, bg="#18181b")
    info_frame.pack(side="left", fill="both", expand=True)

    tk.Label(
        info_frame, text=data["title"], fg="#ffffff", bg="#18181b",
        font=("Segoe UI", 12, "bold"), anchor="w", wraplength=305, justify="left"
    ).pack(fill="x")

    tk.Label(
        info_frame, text=data["artist"], fg="#a1a1aa", bg="#18181b",
        font=("Segoe UI", 10), anchor="w", wraplength=305, justify="left"
    ).pack(fill="x", pady=(2, 0))

    tk.Label(
        info_frame, text=f"Album: {data['album']}", fg="#71717a", bg="#18181b",
        font=("Segoe UI", 9), anchor="w", wraplength=305, justify="left"
    ).pack(fill="x", pady=(2, 8))

    btn_area = tk.Frame(info_frame, bg="#18181b")
    btn_area.pack(fill="x", side="bottom")

    # Row 1: Streaming Platform Buttons (YouTube, YT Music, Spotify, Apple Music)
    row_platforms = tk.Frame(btn_area, bg="#18181b")
    row_platforms.pack(fill="x", pady=(0, 6))

    tk.Button(
        row_platforms, text="YouTube", bg="#ef4444", fg="#ffffff",
        activebackground="#dc2626", activeforeground="#ffffff",
        font=("Segoe UI", 8, "bold"), relief="flat", padx=8, pady=4, cursor="hand2",
        command=lambda: open_link_and_close(data["yt_url"])
    ).pack(side="left", padx=(0, 5))

    tk.Button(
        row_platforms, text="YT Music", bg="#27272a", fg="#ffffff",
        activebackground="#3f3f46", activeforeground="#ffffff",
        font=("Segoe UI", 8, "bold"), relief="flat", padx=8, pady=4, cursor="hand2",
        command=lambda: open_link_and_close(data["ytm_url"])
    ).pack(side="left", padx=(0, 5))

    spotify_url = data.get("spotify_url", "")
    if spotify_url:
        tk.Button(
            row_platforms, text="Spotify", bg="#1db954", fg="#ffffff",
            activebackground="#16a34a", activeforeground="#ffffff",
            font=("Segoe UI", 8, "bold"), relief="flat", padx=8, pady=4, cursor="hand2",
            command=lambda: open_link_and_close(spotify_url)
        ).pack(side="left", padx=(0, 5))
    else:
        tk.Button(
            row_platforms, text="Spotify", bg="#27272a", fg="#52525b",
            disabledforeground="#52525b", font=("Segoe UI", 8, "bold"),
            relief="flat", padx=8, pady=4, state="disabled", cursor="arrow"
        ).pack(side="left", padx=(0, 5))

    apple_url = data.get("apple_url", "")
    if apple_url:
        tk.Button(
            row_platforms, text="Apple Music", bg="#fa243c", fg="#ffffff",
            activebackground="#e11d48", activeforeground="#ffffff",
            font=("Segoe UI", 8, "bold"), relief="flat", padx=8, pady=4, cursor="hand2",
            command=lambda: open_link_and_close(apple_url)
        ).pack(side="left")
    else:
        tk.Button(
            row_platforms, text="Apple Music", bg="#27272a", fg="#52525b",
            disabledforeground="#52525b", font=("Segoe UI", 8, "bold"),
            relief="flat", padx=8, pady=4, state="disabled", cursor="arrow"
        ).pack(side="left")

    # Row 2: Try Again Button
    row_actions = tk.Frame(btn_area, bg="#18181b")
    row_actions.pack(fill="x")

    tk.Button(
        row_actions, text="Try Again", bg="#3b82f6", fg="#ffffff",
        activebackground="#2563eb", activeforeground="#ffffff",
        font=("Segoe UI", 8, "bold"), relief="flat", padx=10, pady=4, cursor="hand2",
        command=retry_and_close
    ).pack(side="left")

    root.mainloop()


def main():
    wav_path = sys.argv[1] if len(sys.argv) > 1 else "temp.wav"
    deep_wav_path = sys.argv[2] if len(sys.argv) > 2 else ""

    if not os.path.exists(wav_path):
        notify_host_hide_loader()
        sys.exit(1)

    try:
        data = asyncio.run(identify_audio(wav_path, deep_wav_path))
    except Exception:
        data = {"found": False}
    finally:
        for p in (wav_path, deep_wav_path):
            if p and os.path.exists(p):
                try:
                    os.remove(p)
                except OSError:
                    pass

    show_result_window(data)


if __name__ == "__main__":
    main()
