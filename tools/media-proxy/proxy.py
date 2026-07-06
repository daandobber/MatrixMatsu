#!/usr/bin/env python3
"""Matrix homeserver reverse proxy for MatrixMatsu (Tanmatsu handheld client).

MatrixMatsu's software H.264 decoder only supports Baseline/Constrained
Baseline profile (a hardware/library limitation, not fixable client-side).
Most phone camera apps encode High Profile by default, so most real m.video
attachments would otherwise fail to decode on the device.

This proxy sits between MatrixMatsu and the real homeserver. It forwards
every request through unchanged, EXCEPT video downloads: those are fetched
from upstream, probed with ffprobe, and transcoded with ffmpeg to Baseline
profile + AAC audio (only when actually needed), then cached on disk keyed
by the mxc media id (Matrix media is immutable once uploaded, so the cache
never needs to be invalidated).

Only point MatrixMatsu's own "homeserver" field at this proxy. Every other
client (Element on a phone/PC, etc.) should keep talking to the real
homeserver directly -- they are unaffected either way since this proxy only
ever sees traffic that is explicitly routed through it.

Usage:
    pip install -r requirements.txt   # aiohttp
    # ffmpeg + ffprobe must be installed and on PATH
    python proxy.py https://matrix.example.org --port 8008

Then, on the Tanmatsu, log in with homeserver = http://<this-machine>:8008
"""
import argparse
import asyncio
import hashlib
import json
import logging
import re
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Optional, Tuple

from aiohttp import web, ClientSession, ClientTimeout

LOG = logging.getLogger("media-proxy")

DOWNLOAD_RE = re.compile(r"^/_matrix/client/v1/media/download/([^/]+)/([^/]+)$")

BASELINE_PROFILES = {"Baseline", "Constrained Baseline"}

# Headers that must never be blindly relayed as-is between hops (either
# because they describe framing we've already normalized away, or because
# they're connection-specific, not resource-specific).
HOP_BY_HOP = {"content-encoding", "transfer-encoding", "content-length", "connection", "host"}


class ProxyConfig:
    def __init__(self, upstream: str, cache_dir: Path, max_width: int):
        self.upstream = upstream.rstrip("/")
        self.cache_dir = cache_dir
        self.max_width = max_width


def cache_path(cfg: ProxyConfig, server: str, media_id: str) -> Path:
    key = hashlib.sha256(f"{server}/{media_id}".encode()).hexdigest()
    return cfg.cache_dir / f"{key}.mp4"


def filter_headers(headers) -> dict:
    return {k: v for k, v in headers.items() if k.lower() not in HOP_BY_HOP}


def probe_streams(path: Path) -> Tuple[bool, Optional[str], Optional[int], Optional[str]]:
    """Returns (has_video, video_profile, video_width, audio_codec)."""
    try:
        out = subprocess.run(
            [
                "ffprobe", "-v", "error", "-show_entries",
                "stream=codec_type,codec_name,profile,width",
                "-of", "json", str(path),
            ],
            capture_output=True, text=True, timeout=30, check=True,
        )
        data = json.loads(out.stdout)
    except Exception as exc:
        LOG.warning("ffprobe failed for %s: %s", path, exc)
        return False, None, None, None

    has_video = False
    video_profile = None
    video_width = None
    audio_codec = None
    for s in data.get("streams", []):
        if s.get("codec_type") == "video":
            has_video = True
            if video_profile is None:
                video_profile = s.get("profile")
                video_width = s.get("width")
        elif s.get("codec_type") == "audio" and audio_codec is None:
            audio_codec = s.get("codec_name")
    return has_video, video_profile, video_width, audio_codec


def needs_transcode(cfg: ProxyConfig, has_video: bool, video_profile, video_width, audio_codec) -> bool:
    if has_video:
        if video_profile not in BASELINE_PROFILES:
            return True
        if cfg.max_width and video_width and video_width > cfg.max_width:
            return True
    if audio_codec not in (None, "aac"):
        return True
    return False


def transcode_to_baseline(cfg: ProxyConfig, src: Path, dst: Path) -> bool:
    tmp = dst.with_suffix(".tmp.mp4")
    vf = []
    if cfg.max_width:
        vf = ["-vf", f"scale='min({cfg.max_width},iw)':-2"]
    cmd = [
        "ffmpeg", "-y", "-i", str(src),
        "-c:v", "libx264", "-profile:v", "baseline", "-level", "3.0", "-pix_fmt", "yuv420p",
        *vf,
        "-c:a", "aac", "-ac", "2",
        "-movflags", "+faststart",
        str(tmp),
    ]
    try:
        subprocess.run(cmd, capture_output=True, text=True, timeout=900, check=True)
        tmp.replace(dst)
        return True
    except subprocess.CalledProcessError as exc:
        LOG.error("ffmpeg failed for %s: %s", src, exc.stderr[-2000:])
    except Exception as exc:
        LOG.error("ffmpeg failed for %s: %s", src, exc)
    tmp.unlink(missing_ok=True)
    return False


async def handle_download(request: web.Request, cfg: ProxyConfig) -> web.StreamResponse:
    raw_path_qs = request.raw_path
    raw_path = raw_path_qs.split("?", 1)[0]
    m = DOWNLOAD_RE.match(raw_path)
    assert m is not None
    server, media_id = m.group(1), m.group(2)
    out_path = cache_path(cfg, server, media_id)

    if out_path.exists():
        LOG.info("cache hit: %s/%s", server, media_id)
        return web.FileResponse(out_path, headers={"Content-Type": "video/mp4"})

    upstream_url = cfg.upstream + raw_path_qs
    headers = {}
    if "Authorization" in request.headers:
        headers["Authorization"] = request.headers["Authorization"]

    async with ClientSession(timeout=ClientTimeout(total=120)) as session:
        async with session.get(upstream_url, headers=headers) as resp:
            body = await resp.read()
            if resp.status != 200:
                return web.Response(status=resp.status, body=body, headers=filter_headers(resp.headers))
            content_type = resp.headers.get("Content-Type", "")

    if not content_type.startswith("video/"):
        return web.Response(body=body, headers={"Content-Type": content_type or "application/octet-stream"})

    with tempfile.NamedTemporaryFile(suffix=".src", delete=False) as f:
        f.write(body)
        src_path = Path(f.name)

    try:
        has_video, video_profile, video_width, audio_codec = probe_streams(src_path)
        transcode = needs_transcode(cfg, has_video, video_profile, video_width, audio_codec)
        LOG.info(
            "%s/%s: video_profile=%s width=%s audio_codec=%s -> transcode=%s",
            server, media_id, video_profile, video_width, audio_codec, transcode,
        )
        cfg.cache_dir.mkdir(parents=True, exist_ok=True)
        if not transcode:
            shutil.copy(src_path, out_path)
        elif not transcode_to_baseline(cfg, src_path, out_path):
            # Transcode failed for some reason -- serve the original rather than nothing.
            return web.Response(body=body, headers={"Content-Type": content_type})
    finally:
        src_path.unlink(missing_ok=True)

    return web.FileResponse(out_path, headers={"Content-Type": "video/mp4"})


async def handle_passthrough(request: web.Request, cfg: ProxyConfig) -> web.StreamResponse:
    upstream_url = cfg.upstream + request.raw_path
    headers = filter_headers(request.headers)
    body = await request.read()

    async with ClientSession(timeout=ClientTimeout(total=90)) as session:
        async with session.request(request.method, upstream_url, headers=headers, data=body or None) as resp:
            resp_body = await resp.read()
            return web.Response(status=resp.status, body=resp_body, headers=filter_headers(resp.headers))


async def handle_all(request: web.Request) -> web.StreamResponse:
    cfg: ProxyConfig = request.app["cfg"]
    raw_path = request.raw_path.split("?", 1)[0]
    if request.method == "GET" and DOWNLOAD_RE.match(raw_path):
        try:
            return await handle_download(request, cfg)
        except Exception:
            LOG.exception("video handling failed for %s, falling back to plain passthrough", raw_path)
    return await handle_passthrough(request, cfg)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("upstream", help="Real homeserver base URL, e.g. https://matrix.example.org")
    parser.add_argument("--host", default="0.0.0.0", help="Address to listen on (default: all interfaces)")
    parser.add_argument("--port", type=int, default=8008)
    parser.add_argument("--cache-dir", default="./media-proxy-cache")
    parser.add_argument(
        "--max-width", type=int, default=240,
        help="Downscale video wider than this even if already Baseline profile (0 disables). "
             "On-device decode is the bottleneck, not the screen: a 384x384 Main-profile clip "
             "measured ~130ms/frame (~6fps) on real hardware, so this defaults well below the "
             "480px screen width to actually get closer to real-time playback, not just to fit "
             "the screen. Decode cost scales roughly with pixel count -- halving width+height "
             "cuts it to about a quarter.",
    )
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

    for tool in ("ffmpeg", "ffprobe"):
        if shutil.which(tool) is None:
            raise SystemExit(f"'{tool}' not found on PATH -- install ffmpeg first.")

    cfg = ProxyConfig(args.upstream, Path(args.cache_dir), args.max_width)
    app = web.Application(client_max_size=64 * 1024 * 1024)
    app["cfg"] = cfg
    app.router.add_route("*", "/{tail:.*}", handle_all)

    LOG.info("proxying %s -> upstream %s (cache: %s)", f"{args.host}:{args.port}", cfg.upstream, cfg.cache_dir)
    web.run_app(app, host=args.host, port=args.port)


if __name__ == "__main__":
    main()
