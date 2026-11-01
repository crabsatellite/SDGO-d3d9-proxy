"""Run a native GameTest and retain bounded SFX-only clips from passed battles.

Developer command only. Captures the owned game window and that process's
audio, with no desktop input. Failed recordings are deleted by exact path.
"""
from __future__ import annotations
import argparse
from concurrent.futures import ThreadPoolExecutor
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import queue
import re
import shutil
import subprocess
import threading
import time
import xml.etree.ElementTree as ET

import game_test


def owned_window(pid: int) -> int | None:
    user = ctypes.WinDLL('user32', use_last_error=True)
    visit_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.IsWindowVisible.argtypes = [wintypes.HWND]
    user.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user.EnumWindows.argtypes = [visit_type, wintypes.LPARAM]
    found = []
    @visit_type
    def visit(hwnd, _):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        name = ctypes.create_unicode_buffer(128)
        user.GetClassNameW(hwnd, name, len(name))
        if owner.value == pid and user.IsWindowVisible(hwnd) and name.value == '_GONLINE_':
            found.append(int(hwnd))
        return True
    user.EnumWindows(visit, 0)
    return found[0] if len(found) == 1 else None


def validate_audio_policy(metadata: dict) -> None:
    if metadata.get('recording', {}).get('audio') != 'sfx_only':
        raise game_test.GameTestError('prepare a fresh run with --sfx-only before recording')
    sound = ET.parse(Path(metadata['client']['root'])/'config.xml').getroot().find('Sound')
    if (sound is None or sound.find('BGMVolume') is None or sound.find('EffectVolume') is None
            or sound.find('BGMVolume').get('Value') != '0'
            or sound.find('BGMVolume').get('use') != 'false'
            or sound.find('EffectVolume').get('use') != 'true'
            or int(sound.find('EffectVolume').get('Value', '0')) <= 0):
        raise game_test.GameTestError('copied client audio policy changed; recording refused')


def events_at(path: Path) -> list[dict]:
    if not path.exists():
        return []
    events = []
    for line in path.read_text(encoding='utf-8').splitlines():
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            # An incomplete trailing write is retried on the next poll.
            break
    return events


def capture(metadata: dict, event: dict, folder: Path, seconds: int, helper: Path) -> dict:
    scene = event['case'].replace('.', '-')
    base = folder/scene
    video = base.with_suffix('.video.mp4')
    audio = base.with_suffix('.audio.wav')
    pending = base.with_suffix('.pending.mp4')
    paths = [video, audio, pending]
    record = {'case': event['case'], 'status': 'discarded', 'reason': None,
              'audio': 'selected_process_tree_sfx_only', 'duration_limit_seconds': seconds}
    audio_process = None
    try:
        pid = event['pid']
        expected = Path(metadata['client']['game']).resolve()
        actual = game_test.process_image_path(pid)
        if actual is None or Path(actual).resolve() != expected:
            raise game_test.GameTestError('native event PID no longer owns this copied game')
        hwnd = owned_window(pid)
        if not hwnd:
            raise game_test.GameTestError('unique visible window for the owned game was not found')
        ffmpeg = shutil.which('ffmpeg')
        if not ffmpeg:
            raise game_test.GameTestError('ffmpeg is required for developer recording')
        with base.with_suffix('.audio.log').open('wb') as audio_log, base.with_suffix('.video.log').open('wb') as video_log:
            audio_process = subprocess.Popen([str(helper), str(pid), str(seconds+3), str(audio)],
                stdout=subprocess.PIPE, stderr=audio_log, creationflags=subprocess.CREATE_NO_WINDOW)
            ready_lines = queue.Queue()
            def read_ready():
                ready_lines.put(audio_process.stdout.readline())
            reader = threading.Thread(target=read_ready, daemon=True)
            reader.start()
            ready = json.loads(ready_lines.get(timeout=12))
            if ready.get('event') != 'audio_ready' or ready.get('pid') != pid:
                raise game_test.GameTestError('process audio capture did not become ready')
            video_started = time.perf_counter()
            subprocess.run([ffmpeg, '-hide_banner', '-loglevel', 'warning', '-nostdin',
                '-f', 'gdigrab', '-draw_mouse', '0', '-framerate', '30', '-i', 'hwnd='+str(hwnd),
                '-t', str(seconds), '-vf', 'scale=trunc(iw/2)*2:trunc(ih/2)*2',
                '-c:v', 'libx264', '-preset', 'veryfast', '-crf', '20',
                '-maxrate', '8M', '-bufsize', '16M', '-pix_fmt', 'yuv420p', '-fs', '40M', str(video)],
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=video_log,
                timeout=seconds+15, check=True, creationflags=subprocess.CREATE_NO_WINDOW)
            if audio_process.wait(timeout=15) != 0:
                raise game_test.GameTestError('selected process audio capture failed')
            audio_offset = max(0, video_started-ready['qpc']/ready['qpc_frequency'])
            subprocess.run([ffmpeg, '-hide_banner', '-loglevel', 'error', '-nostdin',
                '-i', str(video), '-ss', str(audio_offset), '-i', str(audio),
                '-map', '0:v:0', '-map', '1:a:0', '-c:v', 'copy', '-c:a', 'aac',
                '-b:a', '192k', '-shortest', str(pending)], check=True, timeout=30,
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=video_log,
                creationflags=subprocess.CREATE_NO_WINDOW)
        record.update(status='pending_validation', path=str(pending), pid=pid,
                      hwnd=hwnd, approximate_audio_offset_seconds=audio_offset)
    except (OSError, ValueError, queue.Empty, subprocess.SubprocessError, game_test.GameTestError) as error:
        record['reason'] = f'{type(error).__name__}: {error}'
        pending.unlink(missing_ok=True)
    finally:
        if audio_process is not None:
            if audio_process.poll() is None:
                audio_process.terminate()
                audio_process.wait(timeout=5)
            if audio_process.stdout:
                audio_process.stdout.close()
        for path in paths[:2]:
            path.unlink(missing_ok=True)
    return record


def finalize_clip(record: dict, passed: bool) -> None:
    if record['status'] != 'pending_validation':
        return
    pending = Path(record['path'])
    if not passed:
        pending.unlink(missing_ok=True)
        record.update(status='discarded', reason='native test run did not pass')
        record.pop('path', None)
        return
    ffmpeg = shutil.which('ffmpeg')
    try:
        check = subprocess.run([ffmpeg, '-hide_banner', '-i', str(pending),
            '-vf', 'blackdetect=d=2:pix_th=0.1', '-af', 'astats=metadata=0:reset=0',
            '-f', 'null', 'NUL'], stdin=subprocess.DEVNULL, capture_output=True,
            timeout=45, creationflags=subprocess.CREATE_NO_WINDOW)
    except (OSError, subprocess.SubprocessError) as error:
        pending.unlink(missing_ok=True)
        record.update(status='discarded', reason=f'recording validation failed: {type(error).__name__}')
        record.pop('path', None)
        return
    output = check.stderr.decode(errors='replace')
    rms = re.findall(r'RMS level dB:\s*(-?\w+(?:\.\d+)?)', output)
    levels = [float(value) for value in rms if value != '-inf']
    black = re.findall(r'black_start:0(?:\.0+)?\s+black_end:[\d.]+\s+black_duration:([\d.]+)', output)
    if (check.returncode or not levels or max(levels) < -60
            or any(float(duration) >= record['duration_limit_seconds']*.9 for duration in black)):
        pending.unlink(missing_ok=True)
        record.update(status='discarded', reason='silent, black or undecodable recording')
        record.pop('path', None)
        return
    final = pending.with_name(pending.name.replace('.pending.mp4', '.mp4'))
    pending.rename(final)
    record.update(status='retained_for_visual_review', path=str(final), bytes=final.stat().st_size)


def run_and_record(run_dir: Path, output: Path, helper: Path, seconds: int = 25, timeout: float = 1800) -> dict:
    if not 5 <= seconds <= 45:
        raise game_test.GameTestError('clip duration must be 5..45 seconds')
    metadata = game_test.validate_metadata(run_dir.resolve())
    validate_audio_policy(metadata)
    if not helper.is_file():
        raise game_test.GameTestError('build the game_test_audio developer target first')
    folder = output.resolve()/metadata['run_id']
    folder.mkdir(parents=True, exist_ok=False)
    records = []
    seen = set()
    events_path = Path(metadata['client']['events'])
    with ThreadPoolExecutor(max_workers=1) as executor:
        native = executor.submit(game_test.run_prepared, run_dir, timeout, 45)
        try:
            while not native.done():
                for event in events_at(events_path):
                    if (event.get('run_id') == metadata['run_id'] and event.get('event') == 'case_end'
                            and event.get('status') == 'pass' and event.get('case') not in seen
                            and re.fullmatch(r'pvp(?:\.mode[13679])?\.ai_spawn', event.get('case', ''))):
                        seen.add(event['case'])
                        print('Recording passed native battle:', event['case'], flush=True)
                        records.append(capture(metadata, event, folder, seconds, helper.resolve()))
                time.sleep(.2)
            report = native.result()
        finally:
            passed = native.done() and native.exception() is None and native.result()['status'] == 'pass'
            for record in records:
                finalize_clip(record, passed)
            receipt = {'run_id': metadata['run_id'], 'native_receipt': str(events_path),
                       'native_passed': passed, 'clips': records}
            game_test.atomic_write(folder/'recording.json', json.dumps(receipt, indent=2))
    usable = bool(records) and all(record['status'] == 'retained_for_visual_review' for record in records)
    return {'test_status': report['status'], 'recording_status': 'candidates_ready' if usable else 'failed',
            'recording_receipt': str(folder/'recording.json'), 'clips': records}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    bundled_helper = Path(__file__).with_name('game_test_audio.exe')
    parser.add_argument('--audio-helper', type=Path,
                        default=bundled_helper if bundled_helper.is_file() else Path('build/Release/game_test_audio.exe'))
    parser.add_argument('--seconds', type=int, default=25)
    parser.add_argument('--timeout', type=float, default=1800)
    args = parser.parse_args()
    try:
        result = run_and_record(args.run_dir, args.output, args.audio_helper, args.seconds, args.timeout)
        print(json.dumps(result, indent=2))
        raise SystemExit(0 if result['test_status'] == 'pass' and result['recording_status'] == 'candidates_ready' else 1)
    except (game_test.GameTestError, OSError, ValueError) as error:
        parser.exit(2, f'game_test_record: {error}\n')
