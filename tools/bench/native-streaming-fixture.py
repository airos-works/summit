#!/usr/bin/env python3
"""HLS and DASH fixtures for the native streaming player (#115), and a server.

Generates, with FFmpeg (libx264 + aac):
  vod/master.m3u8     fMP4, two video variants and a separate audio rendition
  single/master.m3u8  fMP4 byte ranges in one file per rendition (Reddit's form)
  mux/a.m3u8          a media playlist on its own, video and audio muxed
  mux/disc.m3u8       mux/a + EXT-X-DISCONTINUITY + another encoding (new init, timestamps from 500 s)
  mux/enc.m3u8        mux/a with AES-128 segments, IV from the media sequence number
  ts/plain.m3u8       MPEG-TS segments, muxed
  ts/aes.m3u8         MPEG-TS, AES-128 with an explicit IV
  ts/disc.m3u8        ts/plain + EXT-X-DISCONTINUITY + another encoding
  dashsb/manifest.mpd DASH SegmentBase: one fragmented MP4 per representation, global sidx
  dashwebm/manifest.mpd DASH SegmentBase WebM (VP9 + Opus, Cues), FFmpeg's webm_dash_manifest
  player.html         a page with a <video> for ?src= (and &type= for a typed <source>)

Serves the directory with byte ranges and CORS, and simulates live streams:
/live/<path>.m3u8 is <path>.m3u8 as a sliding window (--window segments)
that advances in real time and loops the source with EXT-X-DISCONTINUITY.
Every playlist under /live/ is live, so /live/vod/master.m3u8 is a live
multivariant presentation. /event/<path>.m3u8 is an EVENT playlist that grows
from the first request for it (--window segments at first) and ends after one
pass.
"""
import argparse
import http.server
import os
import pathlib
import re
import shutil
import subprocess
import time

FF = os.environ.get('FFMPEG', 'ffmpeg')
SRC = ['-f', 'lavfi', '-i', 'testsrc2=size=640x360:rate=30:duration=24',
       '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=24']
SRC2 = ['-f', 'lavfi', '-i', 'testsrc=size=480x270:rate=25:duration=12',
        '-f', 'lavfi', '-i', 'sine=frequency=880:sample_rate=44100:duration=12']
ENC = ['-pix_fmt', 'yuv420p', '-c:v', 'libx264', '-preset', 'veryfast', '-g', '60', '-keyint_min', '60', '-sc_threshold', '0',
       '-b:v', '600k', '-c:a', 'aac', '-b:a', '96k']
HLS = ['-f', 'hls', '-hls_time', '4', '-hls_playlist_type', 'vod']


def ffmpeg(*args):
    subprocess.run([FF, '-hide_banner', '-loglevel', 'error', '-y', *args], check=True)


def encrypt(source, target, key, iv):
    subprocess.run(['openssl', 'enc', '-aes-128-cbc', '-K', key.hex(), '-iv', iv.hex(),
                    '-in', str(source), '-out', str(target)], check=True)


def generate(root):
    if root.exists():
        shutil.rmtree(root)
    for sub in ('vod', 'single', 'mux', 'ts', 'dashsb', 'dashwebm'):
        (root / sub).mkdir(parents=True)
    os.chdir(root)
    ffmpeg(*SRC, '-filter_complex', '[0:v]split=2[a][b];[b]scale=320:180[c]', '-map', '[a]', '-map', '[c]', '-map', '1:a',
           '-c:v', 'libx264', '-preset', 'veryfast', '-g', '60', '-keyint_min', '60', '-sc_threshold', '0',
           '-b:v:0', '800k', '-b:v:1', '200k', '-c:a', 'aac', '-b:a', '96k', *HLS, '-hls_segment_type', 'fmp4',
           '-hls_segment_filename', 'vod/v%v_%03d.m4s', '-hls_fmp4_init_filename', 'init_%v.mp4',
           '-master_pl_name', 'master.m3u8',
           '-var_stream_map', 'v:0,agroup:aud v:1,agroup:aud a:0,agroup:aud,default:yes', 'vod/p_%v.m3u8')
    ffmpeg(*SRC, '-map', '0:v', '-map', '1:a', *ENC, *HLS, '-hls_segment_type', 'fmp4', '-hls_flags', 'single_file',
           '-hls_segment_filename', 'single/s%v.mp4', '-master_pl_name', 'master.m3u8',
           '-var_stream_map', 'v:0,agroup:aud a:0,agroup:aud,default:yes', 'single/p_%v.m3u8')
    ffmpeg(*SRC, *ENC, *HLS, '-hls_segment_type', 'fmp4', '-hls_segment_filename', 'mux/a%03d.m4s',
           '-hls_fmp4_init_filename', 'ainit.mp4', 'mux/a.m3u8')
    ffmpeg(*SRC2, *ENC, '-output_ts_offset', '500', *HLS, '-hls_segment_type', 'fmp4',
           '-hls_segment_filename', 'mux/b%03d.m4s', '-hls_fmp4_init_filename', 'binit.mp4', 'mux/b.m3u8')
    ffmpeg(*SRC, *ENC, *HLS, '-hls_segment_filename', 'ts/t%03d.ts', 'ts/plain.m3u8')
    ffmpeg(*SRC2, *ENC, '-output_ts_offset', '500', *HLS, '-hls_segment_filename', 'ts/d%03d.ts', 'ts/d.m3u8')
    key = os.urandom(16)
    (root / 'ts/key.bin').write_bytes(key)
    (root / 'ts/keyinfo').write_text(f'key.bin\n{root}/ts/key.bin\n{os.urandom(16).hex()}\n')
    ffmpeg(*SRC, *ENC, *HLS, '-hls_key_info_file', 'ts/keyinfo', '-hls_segment_filename', 'ts/k%03d.ts', 'ts/aes.m3u8')

    def body(path):
        return [line for line in path.read_text().splitlines()
                if line and not line.startswith(('#EXTM3U', '#EXT-X-VERSION', '#EXT-X-TARGETDURATION',
                                                  '#EXT-X-MEDIA-SEQUENCE', '#EXT-X-PLAYLIST-TYPE', '#EXT-X-ENDLIST'))]
    head = ['#EXTM3U', '#EXT-X-VERSION:7', '#EXT-X-TARGETDURATION:5', '#EXT-X-MEDIA-SEQUENCE:0', '#EXT-X-PLAYLIST-TYPE:VOD']
    (root / 'mux/disc.m3u8').write_text('\n'.join(head + body(root / 'mux/a.m3u8') + ['#EXT-X-DISCONTINUITY']
                                                  + body(root / 'mux/b.m3u8') + ['#EXT-X-ENDLIST', '']))
    (root / 'ts/disc.m3u8').write_text('\n'.join(head + body(root / 'ts/plain.m3u8') + ['#EXT-X-DISCONTINUITY']
                                                 + body(root / 'ts/d.m3u8') + ['#EXT-X-ENDLIST', '']))
    # The initialization section stays clear; every segment is encrypted
    # with the media sequence number as its IV.
    key = os.urandom(16)
    (root / 'mux/k.bin').write_bytes(key)
    lines, sequence = [], 0
    for line in body(root / 'mux/a.m3u8'):
        if line.startswith('#EXT-X-MAP'):
            lines += [line, '#EXT-X-KEY:METHOD=AES-128,URI="k.bin"']
        elif not line.startswith('#'):
            encrypt(root / 'mux' / line, root / 'mux' / ('e' + line), key, sequence.to_bytes(16, 'big'))
            lines.append('e' + line)
            sequence += 1
        else:
            lines.append(line)
    (root / 'mux/enc.m3u8').write_text('\n'.join(head + lines + ['#EXT-X-ENDLIST', '']))
    # DASH SegmentBase: fragmented MP4 with a global sidx after the moov.
    ffmpeg('-f', 'lavfi', '-i', 'testsrc2=size=640x360:rate=30:duration=24', *ENC[:-4], '-movflags',
           '+frag_keyframe+empty_moov+global_sidx+default_base_moof', 'dashsb/video.mp4')
    ffmpeg('-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=24', '-c:a', 'aac', '-b:a', '96k',
           '-frag_duration', '4000000', '-movflags', '+empty_moov+global_sidx+default_base_moof', 'dashsb/audio.mp4')

    def boxes(path):
        data, at, found = path.read_bytes(), 0, {}
        while at + 8 <= len(data):
            size, kind = int.from_bytes(data[at:at + 4], 'big'), data[at + 4:at + 8].decode('latin-1')
            found.setdefault(kind, (at, size))
            if kind == 'moof' or size < 8:
                break
            at += size
        return found
    video, audio = boxes(root / 'dashsb/video.mp4'), boxes(root / 'dashsb/audio.mp4')

    def index(found):
        return f'{found["sidx"][0]}-{found["sidx"][0] + found["sidx"][1] - 1}'
    (root / 'dashsb/manifest.mpd').write_text(f'''<?xml version="1.0"?>
<MPD xmlns="urn:mpeg:dash:schema:mpd:2011" type="static" mediaPresentationDuration="PT24S" minBufferTime="PT2S" profiles="urn:mpeg:dash:profile:isoff-on-demand:2011">
<Period id="0" start="PT0S">
<AdaptationSet mimeType="video/mp4" codecs="avc1.64001e"><Representation id="v" bandwidth="600000" width="640" height="360"><BaseURL>video.mp4</BaseURL><SegmentBase indexRange="{index(video)}"><Initialization range="0-{video["sidx"][0] - 1}"/></SegmentBase></Representation></AdaptationSet>
<AdaptationSet mimeType="audio/mp4" codecs="mp4a.40.2"><Representation id="a" bandwidth="96000"><BaseURL>audio.mp4</BaseURL><SegmentBase indexRange="{index(audio)}"/></Representation></AdaptationSet>
</Period></MPD>
''')
    ffmpeg('-f', 'lavfi', '-i', 'testsrc2=size=640x360:rate=30:duration=24', '-c:v', 'libvpx-vp9', '-deadline', 'realtime',
           '-cpu-used', '8', '-b:v', '600k', '-g', '60', '-keyint_min', '60', '-f', 'webm', '-dash', '1',
           '-cluster_time_limit', '4000', 'dashwebm/video.webm')
    ffmpeg('-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=24', '-c:a', 'libopus', '-b:a', '96k',
           '-f', 'webm', '-dash', '1', '-cluster_time_limit', '4000', 'dashwebm/audio.webm')
    ffmpeg('-f', 'webm_dash_manifest', '-i', 'dashwebm/video.webm', '-f', 'webm_dash_manifest', '-i', 'dashwebm/audio.webm',
           '-c', 'copy', '-map', '0', '-map', '1', '-f', 'webm_dash_manifest', '-adaptation_sets', 'id=0,streams=0 id=1,streams=1',
           'dashwebm/manifest.mpd')
    (root / 'player.html').write_text('''<!doctype html><meta charset=utf-8><title>HLS player</title>
<style>body{margin:0;background:#222;color:#eee;font:14px sans-serif}video{width:640px;height:360px;background:#000}</style>
<video id=v controls muted autoplay playsinline></video><pre id=log></pre>
<script>
const v = document.getElementById('v'), log = document.getElementById('log');
const src = new URLSearchParams(location.search).get('src');
const type = new URLSearchParams(location.search).get('type');
if (type) { const s = document.createElement('source'); s.src = src; s.type = type; v.append(s); } else v.src = src;
const events = [];
for (const e of ['loadedmetadata','playing','waiting','seeking','seeked','ended','error','durationchange'])
  v.addEventListener(e, () => events.push(e + '@' + v.currentTime.toFixed(2)));
window.state = () => { const q = v.getVideoPlaybackQuality(); const s = v.seekable;
  return { t: v.currentTime, d: v.duration, paused: v.paused, ended: v.ended, rs: v.readyState, w: v.videoWidth, h: v.videoHeight,
    err: v.error && v.error.code, frames: q.totalVideoFrames, dropped: q.droppedVideoFrames,
    seekable: s.length ? [s.start(0), s.end(s.length - 1)] : [], events: events.slice(-12) }; };
setInterval(() => log.textContent = JSON.stringify(state()), 500);
</script>
''')


SEGMENT = re.compile(r'^[^#]')


def live_playlist(source, now, window, event):
    lines = source.read_text().splitlines()
    header, segments, pending = [], [], []
    for line in lines:
        if line.startswith(('#EXTM3U', '#EXT-X-VERSION', '#EXT-X-TARGETDURATION', '#EXT-X-MAP', '#EXT-X-KEY')) and not segments and not pending:
            header.append(line)
        elif line.startswith(('#EXT-X-MEDIA-SEQUENCE', '#EXT-X-PLAYLIST-TYPE', '#EXT-X-ENDLIST')):
            continue
        elif SEGMENT.match(line):
            segments.append(pending + [line])
            pending = []
        else:
            pending.append(line)
    if not segments or '#EXT-X-STREAM-INF' in source.read_text():
        return source.read_text()
    durations = [float(next(l for l in s if l.startswith('#EXTINF:'))[8:].split(',')[0]) for s in segments]
    # A short tail segment would give the audio rendition one segment more
    # per loop than the video, and the loops' discontinuities would differ.
    if len(durations) > 1 and durations[-1] < 0.5:
        durations.pop()
        segments.pop()
    count = len(segments)
    # Segments that exist by now, the stream having started `window` segments in.
    produced, elapsed = window, now
    while elapsed >= durations[produced % count]:
        elapsed -= durations[produced % count]
        produced += 1
        if produced > 100000:
            break
    if event:
        produced = min(produced, count)
        first = 0
    else:
        first = produced - window
    out = [l for l in header] + [f'#EXT-X-MEDIA-SEQUENCE:{first}', f'#EXT-X-DISCONTINUITY-SEQUENCE:{first // count}']
    if event:
        out.insert(len(header), '#EXT-X-PLAYLIST-TYPE:EVENT')
    for g in range(first, produced):
        if g % count == 0 and g > first:
            out.append('#EXT-X-DISCONTINUITY')
        out += segments[g % count]
    if event and produced == count:
        out.append('#EXT-X-ENDLIST')
    return '\n'.join(out) + '\n'


class Handler(http.server.SimpleHTTPRequestHandler):
    start = time.monotonic()
    window = 4
    first_requests = {}

    def end_headers(self):
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()

    def guess_type(self, path):
        path = str(path)
        if path.endswith('.m3u8'):
            return 'application/vnd.apple.mpegurl'
        if path.endswith('.ts'):
            return 'video/mp2t'
        if path.endswith('.m4s'):
            return 'video/iso.segment'
        return super().guess_type(path)

    def do_GET(self):
        path = self.path.split('?')[0]
        for prefix, event in (('/live/', False), ('/event/', True)):
            if path.startswith(prefix):
                local = pathlib.Path(self.directory) / path[len(prefix):]
                if path.endswith('.m3u8') and local.is_file():
                    began = self.first_requests.setdefault(path, time.monotonic()) if event else self.start
                    text = live_playlist(local, time.monotonic() - began, self.window, event).encode()
                    self.send_response(200)
                    self.send_header('Content-Type', 'application/vnd.apple.mpegurl')
                    self.send_header('Content-Length', str(len(text)))
                    self.end_headers()
                    self.wfile.write(text)
                    return
                self.path = '/' + path[len(prefix):]
        header = self.headers.get('Range')
        local = pathlib.Path(self.translate_path(self.path))
        if header and local.is_file():
            match = re.fullmatch(r'bytes=(\d+)-(\d*)', header.strip())
            size = local.stat().st_size
            if match:
                first = int(match.group(1))
                last = min(int(match.group(2)) if match.group(2) else size - 1, size - 1)
                if first <= last:
                    with open(local, 'rb') as stream:
                        stream.seek(first)
                        data = stream.read(last - first + 1)
                    self.send_response(206)
                    self.send_header('Content-Type', self.guess_type(local))
                    self.send_header('Content-Range', f'bytes {first}-{last}/{size}')
                    self.send_header('Content-Length', str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                    return
        super().do_GET()

    def log_message(self, *args):
        pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--directory', type=pathlib.Path, default=pathlib.Path('.vm/hls-fixture'))
    parser.add_argument('--host', default='0.0.0.0')
    parser.add_argument('--port', type=int, default=8865)
    parser.add_argument('--window', type=int, default=4)
    parser.add_argument('--generate-only', action='store_true')
    parser.add_argument('--serve-only', action='store_true')
    args = parser.parse_args()
    root = args.directory.resolve()
    if not args.serve_only:
        generate(root)
    if args.generate_only:
        return
    Handler.window = args.window
    server = http.server.ThreadingHTTPServer((args.host, args.port),
                                             lambda *a, **k: Handler(*a, directory=str(root), **k))
    server.serve_forever()


if __name__ == '__main__':
    main()
